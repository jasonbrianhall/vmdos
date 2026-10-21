// TCP for VMDOSNET's 9P over the network (fs9p.cpp): a small TCP client run
// by vmdos itself on the PC's network card, as IPXNET runs UDP. VMDOSNET.EXE
// does the setup with the DOS TCP/IP stack (net/: the addresses from DHCP,
// DNS, the next hop's MAC, IPv4 or IPv6) and hands that over; nothing stays
// resident in DOS.
//
// One connection per host share. Polled: fs9p sends a request, then pumps
// (tcp9p_pump) until the answers are in, with the card polled from here.
// Segments come in order or are dropped (a duplicate ACK asks again);
// what we sent is kept until acknowledged and sent again after a timeout
// or three duplicate ACKs. Frames for us are taken out of the card's
// stream before DOS's packet driver queue (net.cpp), so DOS's own TCP/IP
// can run with the same address at the same time; ARP and IPv6 neighbor
// solicitations for our addresses are answered here, since nothing in DOS
// may be listening.
#include "net.hpp"
#include "tcp9p.hpp"

namespace {

const int NCONN = 4, RXQ = 64;
const u32 TXBUF = 512 * 1024, RXBUF = 320 * 1024;
enum { CLOSED, SYN_SENT, OPEN, DEAD };
enum { F_FIN = 1, F_SYN = 2, F_RST = 4, F_PSH = 8, F_ACK = 16 };

struct Conn {
    int state;
    Tcp9pCfg c;
    u32 iss, snd_una, snd_nxt, rcv_nxt, tx_base, tx_len, rx_len;
    u32 progress, rto;
    u16 peer_win, mss, dupacks;
    bool need_ack;
    u8 *tx, *rx;
};
Conn conns[NCONN];
int nactive;
u8* rxq;                                       // frames for us, taken from the card's stream
u16 rxq_len[RXQ];
int rxq_head, rxq_n;
u32 dropped;
int logging = -1;

bool log_on()
{
    if (logging < 0) logging = strstr(cmdline, "tcplog") != 0;
    return logging;
}
#define tlog(...) do { if (log_on()) kprintf(__VA_ARGS__); } while (0)

inline bool zero(const u8* p, int n) { while (n--) if (*p++) return false; return true; }
inline u32 g32(const u8* p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline void p32(u8* p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
inline int sdiff(u32 a, u32 b) { return (int)(a - b); }

u16 csum_add(u32 s, const u8* p, unsigned n)
{
    while (n > 1) { s += (p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) s += p[0] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (u16)s;
}

void send_frame(u8* f, unsigned len)
{
    if (len < 60) { memset(f + len, 0, 60 - len); len = 60; }
    net_wire_send(f, len);
}

// A segment: flags, len bytes from the send buffer at seq (SYN: our MSS option).
void segment(Conn& k, u8 flags, u32 seq, const u8* data, unsigned len)
{
    static u8 f[net::FRAME_MAX];
    const Tcp9pCfg& c = k.c;
    memcpy(f, c.hop_mac, 6);
    memcpy(f + 6, net_cur_mac(), 6);
    unsigned opt = (flags & F_SYN) ? 4 : 0, th = 20 + opt;
    u8 *ip = f + 14, *t;
    u32 ps;                                    // pseudo-header sum
    if (c.family == 4) {
        f[12] = 0x08; f[13] = 0x00;
        static u16 ident;
        unsigned tot = 20 + th + len;
        ip[0] = 0x45; ip[1] = 0; ip[2] = (u8)(tot >> 8); ip[3] = (u8)tot;
        ident++; ip[4] = (u8)(ident >> 8); ip[5] = (u8)ident;
        ip[6] = 0x40; ip[7] = 0;               // don't fragment
        ip[8] = 64; ip[9] = 6; ip[10] = ip[11] = 0;
        memcpy(ip + 12, c.my4, 4);
        memcpy(ip + 16, c.server, 4);
        u16 s = ~csum_add(0, ip, 20);
        ip[10] = (u8)(s >> 8); ip[11] = (u8)s;
        t = ip + 20;
        ps = csum_add(0, ip + 12, 8) + 6 + th + len;
    } else {
        f[12] = 0x86; f[13] = 0xDD;
        ip[0] = 0x60; ip[1] = ip[2] = ip[3] = 0;
        ip[4] = (u8)((th + len) >> 8); ip[5] = (u8)(th + len);
        ip[6] = 6; ip[7] = 64;
        bool ll = c.server[0] == 0xFE && (c.server[1] & 0xC0) == 0x80;
        memcpy(ip + 8, ll || zero(c.my6, 16) ? c.my6ll : c.my6, 16);
        memcpy(ip + 24, c.server, 16);
        t = ip + 40;
        ps = csum_add(0, ip + 8, 32) + 6 + th + len;
    }
    t[0] = (u8)(c.local_port >> 8); t[1] = (u8)c.local_port;
    t[2] = (u8)(c.port >> 8); t[3] = (u8)c.port;
    p32(t + 4, seq);
    p32(t + 8, (flags & F_ACK) ? k.rcv_nxt : 0);
    t[12] = (u8)((th / 4) << 4);
    t[13] = flags;
    u32 room = RXBUF - k.rx_len;
    u16 win = room > 65535 ? 65535 : (u16)room;
    t[14] = (u8)(win >> 8); t[15] = (u8)win;
    t[16] = t[17] = t[18] = t[19] = 0;
    if (opt) {                                 // MSS
        u16 mss = c.family == 4 ? 1460 : 1440;
        t[20] = 2; t[21] = 4; t[22] = (u8)(mss >> 8); t[23] = (u8)mss;
    }
    if (len) memcpy(t + th, data, len);
    u16 s = ~csum_add(ps, t, th + len);
    t[16] = (u8)(s >> 8); t[17] = (u8)s;
    send_frame(f, (unsigned)(t - f) + th + len);
    if (flags & F_ACK) k.need_ack = false;
}

// Who has the next hop (IPv4: ARP; IPv6: a neighbor solicitation)?
void ask_hop(Conn& k)
{
    static u8 o[net::FRAME_MAX];
    const u8* me = net_cur_mac();
    if (k.c.family == 4) {
        if (zero(k.c.hop4, 4)) return;
        memset(o, 0xFF, 6); memcpy(o + 6, me, 6);
        o[12] = 0x08; o[13] = 0x06; o[14] = 0; o[15] = 1; o[16] = 0x08; o[17] = 0; o[18] = 6; o[19] = 4;
        o[20] = 0; o[21] = 1;
        memcpy(o + 22, me, 6); memcpy(o + 28, k.c.my4, 4);
        memset(o + 32, 0, 6); memcpy(o + 38, k.c.hop4, 4);
        send_frame(o, 42);
    } else {                                   // a neighbor solicitation, to its solicited-node group
        if (zero(k.c.hop6, 16)) return;
        const u8* t = k.c.hop6;
        o[0] = 0x33; o[1] = 0x33; o[2] = 0xFF; o[3] = t[13]; o[4] = t[14]; o[5] = t[15];
        memcpy(o + 6, me, 6);
        o[12] = 0x86; o[13] = 0xDD;
        u8* ip = o + 14;
        ip[0] = 0x60; ip[1] = ip[2] = ip[3] = 0;
        ip[4] = 0; ip[5] = 32; ip[6] = 58; ip[7] = 255;
        bool ll = t[0] == 0xFE && (t[1] & 0xC0) == 0x80;
        memcpy(ip + 8, ll || zero(k.c.my6, 16) ? k.c.my6ll : k.c.my6, 16);
        static const u8 snm[13] = { 0xFF, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0xFF };
        memcpy(ip + 24, snm, 13); memcpy(ip + 37, t + 13, 3);
        u8* m = ip + 40;
        memset(m, 0, 32);
        m[0] = 135;
        memcpy(m + 8, t, 16);
        m[24] = 1; m[25] = 1; memcpy(m + 26, me, 6);
        u32 s = csum_add(0, ip + 8, 32) + 58 + 32;
        u16 c = ~csum_add(s, m, 32);
        m[2] = (u8)(c >> 8); m[3] = (u8)c;
        send_frame(o, 14 + 40 + 32);
    }
}

// ARP and IPv6 neighbor solicitations for our addresses; ARP answers from the next hop.
void neighbors(const u8* f, unsigned len)
{
    const u8* me = net_cur_mac();
    u16 t = (u16)((f[12] << 8) | f[13]);
    static u8 o[net::FRAME_MAX];
    if (t == 0x0806 && len >= 42) {
        for (auto& k : conns)                  // the next hop moved?
            if (k.state != CLOSED && k.c.family == 4 && !memcmp(f + 28, k.c.hop4, 4) && memcmp(k.c.hop_mac, f + 22, 6)) {
                memcpy(k.c.hop_mac, f + 22, 6);
                tlog("tcp: the next hop is now %02x:%02x:%02x:%02x:%02x:%02x\n", f[22], f[23], f[24], f[25], f[26], f[27]);
            }
        if (f[20] != 0 || f[21] != 1) return;
        const u8* my = 0;
        for (auto& k : conns) if (k.state != CLOSED && k.c.family == 4 && !memcmp(f + 38, k.c.my4, 4)) my = k.c.my4;
        if (!my) return;
        memcpy(o, f + 6, 6); memcpy(o + 6, me, 6);
        memcpy(o + 12, f + 12, 8);
        o[20] = 0; o[21] = 2;
        memcpy(o + 22, me, 6); memcpy(o + 28, my, 4);
        memcpy(o + 32, f + 22, 10);
        send_frame(o, 42);
        return;
    }
    if (t == 0x86DD && len >= 14 + 40 + 24 && f[20] == 58 && f[21] == 255) {
        const u8* target = f + 62;
        if (f[54] == 136) {                    // an advertisement: the next hop's MAC?
            for (auto& k : conns)
                if (k.state != CLOSED && k.c.family == 6 && len >= 14 + 40 + 32 && f[86] == 2 &&
                    !memcmp(f + 22, k.c.hop6, 16))
                    memcpy(k.c.hop_mac, f + 88, 6);
            return;
        }
        if (f[54] != 135 || zero(f + 22, 16)) return;
        bool mine = false;
        for (auto& k : conns)
            if (k.state != CLOSED && k.c.family == 6 &&
                ((!zero(k.c.my6, 16) && !memcmp(target, k.c.my6, 16)) || !memcmp(target, k.c.my6ll, 16))) mine = true;
        if (!mine) return;
        memcpy(o, f + 6, 6); memcpy(o + 6, me, 6);
        o[12] = 0x86; o[13] = 0xDD;
        u8* ip = o + 14;
        ip[0] = 0x60; ip[1] = ip[2] = ip[3] = 0;
        ip[4] = 0; ip[5] = 32; ip[6] = 58; ip[7] = 255;
        memcpy(ip + 8, target, 16);
        memcpy(ip + 24, f + 22, 16);
        u8* m = ip + 40;
        memset(m, 0, 32);
        m[0] = 136; m[4] = 0x60;               // solicited, override
        memcpy(m + 8, target, 16);
        m[24] = 2; m[25] = 1; memcpy(m + 26, me, 6);
        u32 s = csum_add(0, ip + 8, 32) + 58 + 32;
        u16 c = ~csum_add(s, m, 32);
        m[2] = (u8)(c >> 8); m[3] = (u8)c;
        send_frame(o, 14 + 40 + 32);
    }
}

// The TCP segment in a frame for one of our connections: its index, or -1.
int ours(const u8* f, unsigned len, const u8** tcp, unsigned* tlen)
{
    u16 t = (u16)((f[12] << 8) | f[13]);
    const u8 *ip = f + 14, *src, *dst;
    int family;
    if (t == 0x0800) {
        if (len < 14 + 40 || (ip[0] >> 4) != 4 || ip[9] != 6 || (ip[6] & 0x3F) || ip[7]) return -1;
        unsigned ihl = (ip[0] & 15) * 4u, tot = (unsigned)((ip[2] << 8) | ip[3]);
        if (tot > len - 14 || tot < ihl + 20) return -1;
        family = 4; src = ip + 12; dst = ip + 16; *tcp = ip + ihl; *tlen = tot - ihl;
    } else if (t == 0x86DD) {
        if (len < 14 + 60 || ip[6] != 6) return -1;
        unsigned pl = (unsigned)((ip[4] << 8) | ip[5]);
        if (pl > len - 54 || pl < 20) return -1;
        family = 6; src = ip + 8; dst = ip + 24; *tcp = ip + 40; *tlen = pl;
    } else return -1;
    u16 sport = (u16)(((*tcp)[0] << 8) | (*tcp)[1]), dport = (u16)(((*tcp)[2] << 8) | (*tcp)[3]);
    for (int i = 0; i < NCONN; i++) {
        Conn& k = conns[i];
        if (k.state == CLOSED || k.c.family != family || k.c.port != sport || k.c.local_port != dport) continue;
        if (memcmp(src, k.c.server, family == 4 ? 4 : 16)) continue;
        if (family == 4 && memcmp(dst, k.c.my4, 4)) continue;
        return i;
    }
    return -1;
}

void segment_in(Conn& k, const u8* t, unsigned tl)
{
    unsigned doff = (t[12] >> 4) * 4u;
    if (doff < 20 || doff > tl) return;
    u8 fl = t[13];
    u32 seq = g32(t + 4), ack = g32(t + 8);
    const u8* data = t + doff;
    unsigned n = tl - doff;
    if (log_on() && (fl & (F_FIN | F_RST | F_SYN))) tlog("tcp: in flags %02x seq %u ack %u len %u (state %d)\n", fl, seq - k.rcv_nxt, ack - k.iss, n, k.state);
    if (fl & F_RST) {
        if (k.state == SYN_SENT && !((fl & F_ACK) && ack == k.iss + 1)) return;
        tlog("tcp: the server reset the connection\n");
        k.state = DEAD;
        return;
    }
    if (k.state == SYN_SENT) {
        if ((fl & (F_SYN | F_ACK)) != (F_SYN | F_ACK) || ack != k.iss + 1) return;
        k.rcv_nxt = seq + 1;
        k.snd_una = k.snd_nxt = k.tx_base = ack;
        k.mss = 536;
        for (unsigned i = 20; i + 1 < doff; ) {            // options: the MSS
            u8 o = t[i];
            if (o == 0) break;
            if (o == 1) { i++; continue; }
            if (t[i + 1] < 2) break;
            if (o == 2 && t[i + 1] == 4) k.mss = (u16)((t[i + 2] << 8) | t[i + 3]);
            i += t[i + 1];
        }
        u16 mymss = k.c.family == 4 ? 1460 : 1440;
        if (k.mss > mymss) k.mss = mymss;
        k.peer_win = (u16)((t[14] << 8) | t[15]);
        k.state = OPEN;
        k.progress = ticks;
        k.rto = 300;
        segment(k, F_ACK, k.snd_nxt, 0, 0);
        tlog("tcp: connected, mss %u\n", k.mss);
        return;
    }
    if (k.state != OPEN) return;
    if (fl & F_ACK) {
        if (sdiff(ack, k.snd_una) > 0 && sdiff(ack, k.snd_nxt) <= 0) {
            u32 done = ack - k.snd_una;
            k.snd_una = ack;
            k.progress = ticks;
            k.rto = 300;
            k.dupacks = 0;
            if (k.snd_una == k.tx_base + k.tx_len) { k.tx_base = k.snd_una; k.tx_len = 0; }   // all of it
            (void)done;
        } else if (ack == k.snd_una && !n && k.snd_una != k.snd_nxt && ++k.dupacks == 3) {
            k.snd_nxt = k.snd_una;             // fast retransmit
            k.dupacks = 0;
        }
        k.peer_win = (u16)((t[14] << 8) | t[15]);
    }
    if (n) {
        if (seq == k.rcv_nxt && k.rx_len + n <= RXBUF) {
            memcpy(k.rx + k.rx_len, data, n);
            k.rx_len += n;
            k.rcv_nxt += n;
        }
        k.need_ack = true;                     // (out of order: a duplicate ACK)
    }
    if ((fl & F_FIN) && seq + n == k.rcv_nxt) {
        k.rcv_nxt++;
        segment(k, F_ACK, k.snd_nxt, 0, 0);
        tlog("tcp: the server closed the connection\n");
        k.state = DEAD;
    }
}

void process()
{
    while (rxq_n) {
        const u8* f = rxq + rxq_head * net::FRAME_MAX;
        unsigned len = rxq_len[rxq_head];
        const u8* t;
        unsigned tl;
        int i = ours(f, len, &t, &tl);
        if (i >= 0) segment_in(conns[i], t, tl);
        else neighbors(f, len);
        rxq_head = (rxq_head + 1) % RXQ;
        rxq_n--;
    }
}

// Send what the window allows; again what wasn't acknowledged in time.
void output(Conn& k)
{
    if (k.state == SYN_SENT) {
        if ((u32)(ticks - k.progress) >= k.rto) {
            ask_hop(k);
            segment(k, F_SYN, k.iss, 0, 0);
            k.progress = ticks;
            k.rto = k.rto * 2 > 4000 ? 4000 : k.rto * 2;
        }
        return;
    }
    if (k.state != OPEN) return;
    if (k.snd_una != k.snd_nxt && (u32)(ticks - k.progress) >= k.rto) {
        tlog("tcp: nothing acknowledged in %u ms: again from %u\n", k.rto, k.snd_una - k.iss);
        k.snd_nxt = k.snd_una;
        k.progress = ticks;
        k.rto = k.rto * 2 > 4000 ? 4000 : k.rto * 2;
        if (k.rto >= 2000) ask_hop(k);
    }
    u32 end = k.tx_base + k.tx_len;
    for (int burst = 0; burst < 48 && sdiff(end, k.snd_nxt) > 0; burst++) {
        u32 inflight = k.snd_nxt - k.snd_una, win = k.peer_win;
        if (!win && !inflight && (u32)(ticks - k.progress) >= k.rto) win = 1;   // a window probe
        if (inflight >= win) break;
        u32 n = end - k.snd_nxt;
        if (n > k.mss) n = k.mss;
        if (n > win - inflight) n = win - inflight;
        if (inflight == 0) k.progress = ticks;
        segment(k, F_ACK | F_PSH, k.snd_nxt, k.tx + (k.snd_nxt - k.tx_base), n);
        k.snd_nxt += n;
    }
    if (k.need_ack) segment(k, F_ACK, k.snd_nxt, 0, 0);
}

} // namespace

// net.cpp: a frame from the wire. True: it was ours (not for DOS).
bool tcp9p_rx(const u8* f, unsigned len)
{
    const u8* t;
    unsigned tl;
    bool mine = ours(f, len, &t, &tl) >= 0;
    u16 ty = (u16)((f[12] << 8) | f[13]);
    if (!mine && ty != 0x0806 && !(ty == 0x86DD && len > 54 && f[20] == 58)) return false;
    if (rxq_n == RXQ) { dropped++; return mine; }
    int i = (rxq_head + rxq_n) % RXQ;
    memcpy(rxq + i * net::FRAME_MAX, f, len);
    rxq_len[i] = (u16)len;
    rxq_n++;
    return mine;                               // ARP and ND: DOS sees them too
}
bool tcp9p_active() { return nactive > 0; }
void tcp9p_tick()                              // net.cpp, the timer: neighbors, late segments
{
    if (!nactive) return;
    process();
    for (auto& k : conns) if (k.state == OPEN && k.need_ack) segment(k, F_ACK, k.snd_nxt, 0, 0);
}

// A connection to cfg: its number, or -1 no memory, -2 no answer, -3 refused
int tcp9p_open(const Tcp9pCfg& cfg)
{
    if (!rxq) rxq = (u8*)phys_try_alloc(RXQ * net::FRAME_MAX);
    if (!rxq) return -1;
    int i = 0;
    while (i < NCONN && conns[i].state != CLOSED) i++;
    if (i == NCONN) return -1;
    Conn& k = conns[i];
    u8 *tx = k.tx, *rx = k.rx;
    memset(&k, 0, sizeof k);
    k.tx = tx ? tx : (u8*)phys_try_alloc(TXBUF);
    k.rx = rx ? rx : (u8*)phys_try_alloc(RXBUF);
    if (!k.tx || !k.rx) return -1;
    k.c = cfg;
    k.iss = (u32)tsc_read() ^ (u32)(tsc_read() >> 17) ^ ticks * 2654435761u;
    k.state = SYN_SENT;
    k.rto = 500;
    k.progress = ticks - k.rto;                // the SYN now
    nactive++;
    net_keep_rx(true);
    u32 start = ticks;
    while (k.state == SYN_SENT && (u32)(ticks - start) < 8000) tcp9p_pump(i);
    if (k.state == OPEN) return i;
    int e = k.state == DEAD ? -3 : -2;
    tcp9p_close(i);
    return e;
}

void tcp9p_close(int i)
{
    if (i < 0 || i >= NCONN || conns[i].state == CLOSED) return;
    Conn& k = conns[i];
    if (k.state == OPEN) segment(k, F_RST | F_ACK, k.snd_nxt, 0, 0);
    k.state = CLOSED;
    if (!--nactive) net_keep_rx(false);
}

// Queue bytes to send: false when the buffer is full or the connection gone.
bool tcp9p_send(int i, const u8* p, u32 n)
{
    Conn& k = conns[i];
    if (k.state != OPEN) return false;
    if (k.tx_len + n > TXBUF) {                // drop what's acknowledged
        u32 acked = k.snd_una - k.tx_base;
        memmove(k.tx, k.tx + acked, k.tx_len - acked);
        k.tx_base += acked; k.tx_len -= acked;
        if (k.tx_len + n > TXBUF) return false;
    }
    memcpy(k.tx + k.tx_len, p, n);
    k.tx_len += n;
    return true;
}

// Poll the card, take what came, send what can go. False: the connection is gone.
bool tcp9p_pump(int i)
{
    net_poll();
    process();
    output(conns[i]);
    tick_poll();
    return conns[i].state == OPEN || conns[i].state == SYN_SENT;
}

u8* tcp9p_data(int i, u32* have) { *have = conns[i].rx_len; return conns[i].rx; }
void tcp9p_consume(int i, u32 n)
{
    Conn& k = conns[i];
    if (n > k.rx_len) n = k.rx_len;
    u32 before = RXBUF - k.rx_len;
    memmove(k.rx, k.rx + n, k.rx_len - n);
    k.rx_len -= n;
    if (before < 8192 && RXBUF - k.rx_len >= 8192 && k.state == OPEN) segment(k, F_ACK, k.snd_nxt, 0, 0);   // the window opens again
}
