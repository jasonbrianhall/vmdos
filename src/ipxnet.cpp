// IPXNET: IPX over UDP, DOSBox's protocol, run by vmdos itself underneath
// DOS, so the PC plays (VMIPX and the game run as usual) while vmdos
// carries its IPX to and from DOSBox players. VMIPXBOX.EXE sets it up
// (INT 2Fh AX=564Eh BL=8) and goes; it can also tell how it's doing (BL=9).
//
// DOSBox's protocol: each IPX packet (30-byte header + data) is one UDP
// datagram. A packet to socket 2 with destination node 0 registers a
// client; the server answers with the client's node in the destination
// (its IPv4 address and UDP port: 6 bytes), source network 1. The server
// forwards by destination node; a destination host of FFFFFFFF is broadcast.
//
// SERVER: vmdos is the IPXNET server (DOSBox: IPXNET CONNECT <this PC>).
//   DOSBox clients keep their IP:port nodes; this PC and the LAN's IPX
//   machines keep their MACs. Nothing is rewritten.
// CONNECT: vmdos joins an IPXNET server (DOSBox's IPXNET STARTSERVER).
//   This PC and each LAN machine that talks to the other side get a UDP
//   port and a registration of their own; node addresses in the headers
//   are translated (MAC <-> that registration's node).
//
// Both: the LAN's IPX machines are bridged too, so the card listens to
// everything while IPXNET runs (what isn't for DOS is filtered out here),
// and frames for DOS players' nodes are carried over. vmdos answers ARP
// and IPv6 neighbor solicitations for the addresses it was given, since
// nothing in DOS may be listening. IPv4 or IPv6 (DOSBox: IPv4 only).
//
// Frames vmdos sends from here wait in a small queue, sent from the timer
// tick and after each DOS call, never from inside a driver's receive loop.
#include "net.hpp"

namespace {

const int IPX_HLEN = 30, MAX_IPX = 1500, OUTQ = 24, MAX_CLIENTS = 16, MAX_PEERS = 12, MAX_REMOTE = 32;
const uint32_t KEEPALIVE_MS = 20000, REG_RETRY_MS = 1000;

// INT 2Fh AX=564Eh BL=8: ES:DI -> this (mode 0: stop)
struct __attribute__((packed)) Config {
    uint8_t  mode;                  // 0 off, 1 connect, 2 server
    uint8_t  family;                // connect: 4 or 6 (the server's)
    uint16_t port;                  // connect: the server's; server: ours
    uint8_t  server[16];            // connect: its address
    uint8_t  hop_mac[6];            // connect: the next hop to it (router or itself)
    uint8_t  my4[4];                // our IPv4 address (0: none)
    uint8_t  my6[16];               // our IPv6 address (0: none)
    uint8_t  my6ll[16];             // our link-local address
    uint16_t local_port;            // connect: our first UDP port
};

// BL=9: ES:DI <- this
struct __attribute__((packed)) Entry {
    uint8_t  used, family;
    uint16_t port;
    uint8_t  node[6];               // its IPX node (connect: the registration's)
    uint8_t  mac[6];                // connect: the local machine
    uint8_t  addr[16];              // server: the client's address
};
struct __attribute__((packed)) Status {
    uint8_t  mode, registered, entries, frame;
    uint16_t port;
    uint8_t  node[6];               // connect: this PC's node on the server
    uint32_t udp_in, udp_out, ipx_in, ipx_out, dropped;
    Entry    e[16];
};

struct __attribute__((packed)) IpxHdr {
    uint16_t checksum, length;      // big endian
    uint8_t  tc, type;
    uint8_t  dnet[4], dnode[6];
    uint16_t dsock;
    uint8_t  snet[4], snode[6];
    uint16_t ssock;
};

struct Client {                     // server: a DOSBox client
    bool     used;
    uint8_t  family;
    uint8_t  addr[16];
    uint16_t port;
    uint8_t  node[6];
    uint8_t  mac[6];                // where its frames come from (router or itself)
    uint32_t last;
};

struct Peer {                       // connect: this PC (slot 0) or a LAN machine
    bool     used, registered, dos;
    uint8_t  mac[6], node[6];
    uint16_t port;
    uint32_t last, last_reg, last_keep;
};

Config cfg;
bool active;
Client clients[MAX_CLIENTS];
Peer peers[MAX_PEERS];
uint8_t remote[MAX_REMOTE][6];      // nodes seen on the other side
int remote_n, remote_next;
int frame_type;                     // LAN framing: 0 Ethernet II, 1 802.3, 2 802.2, 3 SNAP
uint32_t udp_in, udp_out, ipx_in, ipx_out, dropped;

uint8_t* outq;                      // OUTQ frames of FRAME_MAX
uint16_t outlen[OUTQ];
bool out_dos[OUTQ];                 // to DOS instead of the wire
int out_n;

const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

inline uint16_t be16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
inline bool is_bcast(const uint8_t* node) { return node[0] == 0xFF && node[1] == 0xFF && node[2] == 0xFF && node[3] == 0xFF; }
inline bool zero(const uint8_t* p, int n) { while (n--) if (*p++) return false; return true; }

uint8_t* out_slot(bool dos)
{
    if (!outq || out_n == OUTQ) { dropped++; return 0; }
    out_dos[out_n] = dos;
    return outq + out_n * net::FRAME_MAX;
}
void out_commit(unsigned len)
{
    if (len < 60) { memset(outq + out_n * net::FRAME_MAX + len, 0, 60 - len); len = 60; }
    outlen[out_n++] = (uint16_t)len;
}

uint16_t csum_add(uint32_t s, const uint8_t* p, unsigned n)
{
    while (n > 1) { s += (p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) s += p[0] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)s;
}

// UDP to family/addr via mac, from our address and sport.
void send_udp(uint8_t family, const uint8_t* addr, const uint8_t* mac, uint16_t sport, uint16_t dport,
              const uint8_t* data, unsigned len)
{
    if (len > MAX_IPX) { dropped++; return; }
    if (family == 4 && zero(cfg.my4, 4)) { dropped++; return; }
    uint8_t* f = out_slot(false);
    if (!f) return;
    memcpy(f, mac, 6);
    memcpy(f + 6, net_cur_mac(), 6);
    uint8_t* u;
    if (family == 4) {
        f[12] = 0x08; f[13] = 0x00;
        uint8_t* ip = f + 14;
        static uint16_t ident;
        unsigned tot = 20 + 8 + len;
        ip[0] = 0x45; ip[1] = 0; ip[2] = tot >> 8; ip[3] = tot;
        ident++; ip[4] = ident >> 8; ip[5] = ident;
        ip[6] = 0x40; ip[7] = 0;                // don't fragment
        ip[8] = 64; ip[9] = 17; ip[10] = ip[11] = 0;
        memcpy(ip + 12, cfg.my4, 4);
        memcpy(ip + 16, addr, 4);
        uint16_t c = ~csum_add(0, ip, 20);
        ip[10] = c >> 8; ip[11] = c;
        u = ip + 20;
    } else {
        f[12] = 0x86; f[13] = 0xDD;
        uint8_t* ip = f + 14;
        ip[0] = 0x60; ip[1] = ip[2] = ip[3] = 0;
        ip[4] = (8 + len) >> 8; ip[5] = 8 + len;
        ip[6] = 17; ip[7] = 64;
        bool ll = addr[0] == 0xFE && (addr[1] & 0xC0) == 0x80;
        memcpy(ip + 8, ll || zero(cfg.my6, 16) ? cfg.my6ll : cfg.my6, 16);
        memcpy(ip + 24, addr, 16);
        u = ip + 40;
    }
    u[0] = sport >> 8; u[1] = sport; u[2] = dport >> 8; u[3] = dport;
    u[4] = (8 + len) >> 8; u[5] = 8 + len; u[6] = u[7] = 0;
    memcpy(u + 8, data, len);
    if (family == 6) {                          // mandatory over IPv6
        uint8_t* ip = f + 14;
        uint32_t s = csum_add(0, ip + 8, 32);
        s += 17 + 8 + len;
        uint16_t c = ~csum_add(s, u, 8 + len);
        if (!c) c = 0xFFFF;
        u[6] = c >> 8; u[7] = c;
    }
    out_commit((family == 4 ? 14 + 20 : 14 + 40) + 8 + len);
    udp_out++;
}

// An IPX packet onto the LAN (dmac) or to DOS.
void send_ipx(const uint8_t* ipx, unsigned len, const uint8_t* dmac, bool to_dos)
{
    if (len > MAX_IPX) { dropped++; return; }
    uint8_t* f = out_slot(to_dos);
    if (!f) return;
    memcpy(f, dmac, 6);
    // To DOS: from the IPX source node, as if it were on the LAN.
    memcpy(f + 6, to_dos ? ((const IpxHdr*)ipx)->snode : net_cur_mac(), 6);
    unsigned h = 14;
    int ft = to_dos ? 0 : frame_type;
    switch (ft) {
    case 0: f[12] = 0x81; f[13] = 0x37; break;
    case 1: f[12] = len >> 8; f[13] = len; break;
    case 2: f[12] = (len + 3) >> 8; f[13] = len + 3; f[14] = 0xE0; f[15] = 0xE0; f[16] = 0x03; h = 17; break;
    case 3: f[12] = (len + 8) >> 8; f[13] = len + 8;
            f[14] = 0xAA; f[15] = 0xAA; f[16] = 0x03; f[17] = f[18] = f[19] = 0; f[20] = 0x81; f[21] = 0x37; h = 22; break;
    }
    memcpy(f + h, ipx, len);
    out_commit(h + len);
    ipx_out++;
}

// The IPX packet in an Ethernet frame: its start and length (0: not IPX).
unsigned ipx_of(const uint8_t* f, unsigned len, const uint8_t** ipx, int* ft)
{
    if (len < 14 + IPX_HLEN) return 0;
    unsigned off;
    uint16_t t = (f[12] << 8) | f[13];
    if (t == 0x8137) { off = 14; *ft = 0; }
    else if (t > 1500) return 0;
    else if (f[14] == 0xFF && f[15] == 0xFF) { off = 14; *ft = 1; }
    else if (f[14] == 0xE0 && f[15] == 0xE0 && f[16] == 0x03) { off = 17; *ft = 2; }
    else if (f[14] == 0xAA && f[15] == 0xAA && f[16] == 0x03 && f[20] == 0x81 && f[21] == 0x37) { off = 22; *ft = 3; }
    else return 0;
    if (len < off + IPX_HLEN) return 0;
    *ipx = f + off;
    unsigned l = be16(((const IpxHdr*)*ipx)->length);
    if (l < (unsigned)IPX_HLEN || l > len - off) return 0;
    return l;
}

bool is_remote(const uint8_t* node)
{
    for (int i = 0; i < remote_n; i++) if (!memcmp(remote[i], node, 6)) return true;
    return false;
}
void note_remote(const uint8_t* node)
{
    if (is_bcast(node) || is_remote(node)) return;
    memcpy(remote[remote_next], node, 6);
    remote_next = (remote_next + 1) % MAX_REMOTE;
    if (remote_n < MAX_REMOTE) remote_n++;
}

void log_node(const char* what, const uint8_t* n)
{
    kprintf("%s%02x%02x%02x%02x%02x%02x", what, n[0], n[1], n[2], n[3], n[4], n[5]);
}

// ---------------- SERVER ----------------

Client* client_by_node(const uint8_t* node)
{
    for (auto& c : clients) if (c.used && !memcmp(c.node, node, 6)) return &c;
    return 0;
}
Client* client_by_addr(uint8_t family, const uint8_t* addr, uint16_t port)
{
    for (auto& c : clients)
        if (c.used && c.family == family && c.port == port && !memcmp(c.addr, addr, family == 4 ? 4 : 16)) return &c;
    return 0;
}
void to_client(Client* c, const uint8_t* ipx, unsigned len)
{
    send_udp(c->family, c->addr, c->mac, cfg.port, c->port, ipx, len);
}

void server_udp(uint8_t family, const uint8_t* src, const uint8_t* smac, uint16_t sport, uint8_t* ipx, unsigned len)
{
    IpxHdr* h = (IpxHdr*)ipx;
    if (be16(h->dsock) == 2 && zero(h->dnode, 4)) {          // registration
        Client* c = client_by_addr(family, src, sport);
        if (!c) {
            c = &clients[0];
            for (auto& k : clients) {
                if (!k.used) { c = &k; break; }
                if (k.last < c->last) c = &k;
            }
            memset(c, 0, sizeof *c);
            c->used = true;
            c->family = family;
            memcpy(c->addr, src, family == 4 ? 4 : 16);
            c->port = sport;
            if (family == 4) memcpy(c->node, src, 4);
            else {                              // a made-up, locally administered one
                uint8_t a = 0, b = 0;
                for (int i = 0; i < 16; i += 2) { a ^= src[i]; b ^= src[i + 1]; }
                c->node[0] = 0x02; c->node[1] = 0; c->node[2] = a; c->node[3] = b;
            }
            c->node[4] = sport >> 8; c->node[5] = sport;
            log_node("ipxnet: client connected, node ", c->node);
            kprintf(", port %u\n", sport);
        }
        memcpy(c->mac, smac, 6);
        c->last = ticks;
        IpxHdr a;
        memset(&a, 0, sizeof a);
        a.checksum = 0xFFFF;
        a.length = be16(IPX_HLEN);
        memcpy(a.dnode, c->node, 6);
        a.dsock = be16(2);
        a.snet[3] = 1;
        memcpy(a.snode, cfg.my4, 4);
        a.snode[4] = cfg.port >> 8; a.snode[5] = cfg.port;
        a.ssock = be16(2);
        to_client(c, (uint8_t*)&a, IPX_HLEN);
        return;
    }
    Client* from = client_by_addr(family, src, sport);
    if (!from) { dropped++; return; }
    memcpy(from->mac, smac, 6);
    from->last = ticks;
    if (is_bcast(h->dnode)) {
        for (auto& c : clients) if (c.used && &c != from) to_client(&c, ipx, len);
        if (be16(h->dsock) != 2) {              // IPXNET's own pings stay with DOSBox
            send_ipx(ipx, len, bcast, false);
            send_ipx(ipx, len, bcast, true);
        }
        return;
    }
    if (Client* to = client_by_node(h->dnode)) { to_client(to, ipx, len); return; }
    if (!memcmp(h->dnode, net_cur_mac(), 6)) { send_ipx(ipx, len, h->dnode, true); return; }
    send_ipx(ipx, len, h->dnode, false);        // a LAN machine: its node is its MAC
}

// IPX from DOS or the LAN. Returns false if it needn't go on the wire (DOS).
bool server_ipx(uint8_t* ipx, unsigned len)
{
    IpxHdr* h = (IpxHdr*)ipx;
    if (is_bcast(h->dnode)) {
        for (auto& c : clients) if (c.used) to_client(&c, ipx, len);
        return true;
    }
    if (Client* to = client_by_node(h->dnode)) { to_client(to, ipx, len); return false; }
    return true;
}

// ---------------- CONNECT ----------------

void peer_send(Peer* p, const uint8_t* ipx, unsigned len)
{
    send_udp(cfg.family, cfg.server, cfg.hop_mac, p->port, cfg.port, ipx, len);
}
void register_peer(Peer* p)
{
    IpxHdr h;
    memset(&h, 0, sizeof h);
    h.checksum = 0xFFFF;
    h.length = be16(IPX_HLEN);
    h.dsock = be16(2);
    h.ssock = be16(2);
    peer_send(p, (uint8_t*)&h, IPX_HLEN);
    p->last_reg = ticks;
}
void keepalive(Peer* p)                         // keeps NAT open: to our own node, back to us
{
    IpxHdr h;
    memset(&h, 0, sizeof h);
    h.checksum = 0xFFFF;
    h.length = be16(IPX_HLEN);
    memcpy(h.dnode, p->node, 6); h.dsock = be16(2);
    memcpy(h.snode, p->node, 6); h.ssock = be16(2);
    peer_send(p, (uint8_t*)&h, IPX_HLEN);
    p->last_keep = ticks;
}
Peer* peer_by_port(uint16_t port)
{
    for (auto& p : peers) if (p.used && p.port == port) return &p;
    return 0;
}
Peer* peer_by_mac(const uint8_t* mac)
{
    for (auto& p : peers) if (p.used && !memcmp(p.mac, mac, 6)) return &p;
    return 0;
}
bool our_node(const uint8_t* node)
{
    for (auto& p : peers) if (p.used && p.registered && !memcmp(p.node, node, 6)) return true;
    return false;
}
Peer* new_peer(const uint8_t* mac)
{
    int s = -1;
    for (int i = 1; i < MAX_PEERS; i++) if (!peers[i].used) { s = i; break; }
    if (s < 0) {
        s = 1;
        for (int i = 2; i < MAX_PEERS; i++) if (peers[i].last < peers[s].last) s = i;
    }
    Peer* p = &peers[s];
    memset(p, 0, sizeof *p);
    p->used = true;
    memcpy(p->mac, mac, 6);
    p->port = cfg.local_port + s;
    p->last = ticks;
    register_peer(p);
    return p;
}

void connect_udp(uint16_t dport, uint8_t* ipx, unsigned len)
{
    Peer* p = peer_by_port(dport);
    if (!p) { dropped++; return; }
    IpxHdr* h = (IpxHdr*)ipx;
    if (be16(h->dsock) == 2 && be16(h->ssock) == 2 && !is_bcast(h->dnode) && h->snet[3] == 1 && len == (unsigned)IPX_HLEN) {
        bool first = !p->registered;            // the registration's answer
        memcpy(p->node, h->dnode, 6);
        p->registered = true;
        p->last_keep = ticks;
        if (first) {
            log_node(p->dos ? "ipxnet: connected, this PC is node " : "ipxnet: a LAN machine is node ", p->node);
            if (!p->dos) log_node(", MAC ", p->mac);
            kprintf("\n");
        }
        return;
    }
    if (!p->registered) { dropped++; return; }
    if (be16(h->dsock) == 2 && !memcmp(h->snode, p->node, 6)) return;   // our keepalive back
    if (is_bcast(h->dnode)) {
        if (be16(h->dsock) == 2) {              // IPXNET PING: answer, as DOSBox does
            IpxHdr r;
            memset(&r, 0, sizeof r);
            r.checksum = 0xFFFF; r.length = be16(IPX_HLEN);
            memcpy(r.dnode, h->snode, 6); r.dsock = be16(2);
            memcpy(r.snode, p->node, 6); r.ssock = be16(2);
            peer_send(p, (uint8_t*)&r, IPX_HLEN);
            return;
        }
        // Each of our registrations gets every broadcast: pass it on once.
        if (p != &peers[0] || our_node(h->snode)) return;
        note_remote(h->snode);
        send_ipx(ipx, len, bcast, true);
        send_ipx(ipx, len, bcast, false);
        return;
    }
    if (memcmp(h->dnode, p->node, 6)) { dropped++; return; }
    note_remote(h->snode);
    memcpy(h->dnode, p->mac, 6);
    send_ipx(ipx, len, p->mac, p->dos);
}

// IPX from this PC (dos) or a LAN machine (smac). Returns false if it
// needn't go on the wire.
bool connect_ipx(uint8_t* ipx, unsigned len, const uint8_t* smac, bool dos)
{
    IpxHdr* h = (IpxHdr*)ipx;
    bool b = is_bcast(h->dnode);
    if (!b && !is_remote(h->dnode)) return true;       // for the LAN (or nobody)
    Peer* p = dos ? &peers[0] : peer_by_mac(smac);
    if (!p) p = new_peer(smac);
    p->last = ticks;
    if (!p->registered) { dropped++; return true; }     // games resend
    // A copy: DOS's frame still goes onto the wire unchanged.
    static uint8_t buf[MAX_IPX];
    memcpy(buf, ipx, len);
    IpxHdr* c = (IpxHdr*)buf;
    if (!memcmp(c->snode, smac, 6)) memcpy(c->snode, p->node, 6);
    peer_send(p, buf, len);
    return b;
}

// ---------------- the frames ----------------

// ARP and IPv6 neighbor solicitations for our addresses.
void answer_neighbors(const uint8_t* f, unsigned len)
{
    const uint8_t* me = net_cur_mac();
    uint16_t t = (f[12] << 8) | f[13];
    if (t == 0x0806 && len >= 42 && f[20] == 0 && f[21] == 1 && !zero(cfg.my4, 4) && !memcmp(f + 38, cfg.my4, 4)) {
        uint8_t* o = out_slot(false);
        if (!o) return;
        memcpy(o, f + 6, 6); memcpy(o + 6, me, 6);
        memcpy(o + 12, f + 12, 8);              // type, htype, ptype, sizes
        o[20] = 0; o[21] = 2;
        memcpy(o + 22, me, 6); memcpy(o + 28, cfg.my4, 4);
        memcpy(o + 32, f + 22, 10);             // their MAC and IP
        out_commit(42);
        return;
    }
    if (t == 0x86DD && len >= 14 + 40 + 24 && f[20] == 58 && f[21] == 255 && f[54] == 135) {
        const uint8_t* target = f + 62;
        bool mine = (!zero(cfg.my6, 16) && !memcmp(target, cfg.my6, 16)) || (!zero(cfg.my6ll, 16) && !memcmp(target, cfg.my6ll, 16));
        if (!mine || zero(f + 22, 16)) return;  // not ours; or someone's DAD (leave that to DOS)
        uint8_t* o = out_slot(false);
        if (!o) return;
        memcpy(o, f + 6, 6); memcpy(o + 6, me, 6);
        o[12] = 0x86; o[13] = 0xDD;
        uint8_t* ip = o + 14;
        ip[0] = 0x60; ip[1] = ip[2] = ip[3] = 0;
        ip[4] = 0; ip[5] = 32; ip[6] = 58; ip[7] = 255;
        memcpy(ip + 8, target, 16);
        memcpy(ip + 24, f + 22, 16);
        uint8_t* m = ip + 40;
        memset(m, 0, 32);
        m[0] = 136; m[4] = 0x60;                // solicited, override
        memcpy(m + 8, target, 16);
        m[24] = 2; m[25] = 1; memcpy(m + 26, me, 6);
        uint32_t s = csum_add(0, ip + 8, 32);
        s += 58 + 32;
        uint16_t c = ~csum_add(s, m, 32);
        m[2] = c >> 8; m[3] = c;
        out_commit(14 + 40 + 32);
    }
}

// A UDP datagram for us on one of our ports? Handles it and returns true.
bool our_udp(const uint8_t* f, unsigned len)
{
    uint16_t t = (f[12] << 8) | f[13];
    uint8_t family;
    const uint8_t* src;
    const uint8_t* u;
    unsigned ulen;
    if (t == 0x0800) {
        const uint8_t* ip = f + 14;
        if (len < 14 + 20 + 8 || (ip[0] >> 4) != 4 || ip[9] != 17 || zero(cfg.my4, 4) || memcmp(ip + 16, cfg.my4, 4)) return false;
        if ((ip[6] & 0x3F) || ip[7]) return false;     // a fragment
        unsigned ihl = (ip[0] & 15) * 4, tot = (ip[2] << 8) | ip[3];
        if (tot > len - 14 || tot < ihl + 8) return false;
        family = 4; src = ip + 12; u = ip + ihl; ulen = tot - ihl;
    } else if (t == 0x86DD) {
        const uint8_t* ip = f + 14;
        if (len < 14 + 40 + 8 || ip[6] != 17) return false;
        if (!(!zero(cfg.my6, 16) && !memcmp(ip + 24, cfg.my6, 16)) && memcmp(ip + 24, cfg.my6ll, 16)) return false;
        unsigned pl = (ip[4] << 8) | ip[5];
        if (pl > len - 54 || pl < 8) return false;
        family = 6; src = ip + 8; u = ip + 40; ulen = pl;
    } else return false;
    uint16_t sport = (u[0] << 8) | u[1], dport = (u[2] << 8) | u[3];
    unsigned l = (u[4] << 8) | u[5];
    if (l < 8 || l > ulen) return false;
    static uint8_t buf[MAX_IPX];
    unsigned n = l - 8;
    if (cfg.mode == 2) {
        if (dport != cfg.port) return false;
    } else {
        if (dport < cfg.local_port || dport >= cfg.local_port + MAX_PEERS) return false;
        if (family != cfg.family || sport != cfg.port || memcmp(src, cfg.server, family == 4 ? 4 : 16)) return false;
    }
    udp_in++;
    if (n < (unsigned)IPX_HLEN || n > MAX_IPX) { dropped++; return true; }
    memcpy(buf, u + 8, n);
    unsigned il = be16(((IpxHdr*)buf)->length);
    if (il < (unsigned)IPX_HLEN || il > n) { dropped++; return true; }
    if (cfg.mode == 2) server_udp(family, src, f + 6, sport, buf, il);
    else connect_udp(dport, buf, il);
    return true;
}

void flush()
{
    for (int i = 0; i < out_n; i++) {
        uint8_t* f = outq + i * net::FRAME_MAX;
        if (out_dos[i]) net_dos_inject(f, outlen[i]);
        else net_wire_send(f, outlen[i]);
    }
    out_n = 0;
}

void stop()
{
    if (!active) return;
    active = false;
    out_n = 0;
    net_promisc(false);
    kprintf("ipxnet: stopped\n");
}

} // namespace

bool ipxnet_active() { return active; }

// A frame from the wire. True: it was ours (not for DOS).
bool ipxnet_rx(const uint8_t* f, unsigned len)
{
    answer_neighbors(f, len);
    if (our_udp(f, len)) return true;
    const uint8_t* ipx;
    int ft;
    unsigned l = ipx_of(f, len, &ipx, &ft);
    if (l && memcmp(f + 6, net_cur_mac(), 6)) {
        frame_type = ft;
        ipx_in++;
        static uint8_t buf[MAX_IPX];
        memcpy(buf, ipx, l);
        if (cfg.mode == 2) server_ipx(buf, l);
        else connect_ipx(buf, l, f + 6, false);
    }
    return false;
}

// A frame from DOS. False: it needn't go on the wire.
bool ipxnet_tx(const uint8_t* f, unsigned len)
{
    const uint8_t* ipx;
    int ft;
    unsigned l = ipx_of(f, len, &ipx, &ft);
    if (!l) return true;
    frame_type = ft;
    ipx_in++;
    static uint8_t buf[MAX_IPX];
    memcpy(buf, ipx, l);
    bool wire = cfg.mode == 2 ? server_ipx(buf, l) : connect_ipx(buf, l, net_cur_mac(), true);
    flush();
    return wire;
}

// Every timer tick: registrations, keepalives, the queue.
void ipxnet_tick()
{
    if (!active) return;
    if (cfg.mode == 1) {
        for (auto& p : peers) {
            if (!p.used) continue;
            if (!p.registered) { if (ticks - p.last_reg >= REG_RETRY_MS) register_peer(&p); }
            else if (ticks - p.last_keep >= KEEPALIVE_MS) keepalive(&p);
        }
    }
    flush();
}

// INT 2Fh AX=564Eh BL=8 (set up / stop) and BL=9 (status).
void ipxnet_api(struct regs* r, int fn)
{
    uint8_t* p = gptr(LIN(r->v86_es, DI(r)));
    if (fn == 8) {
        stop();
        memcpy(&cfg, p, sizeof cfg);
        if (cfg.mode != 1 && cfg.mode != 2) { AX(r) = 0; return; }
        if (!outq) outq = (uint8_t*)phys_alloc(OUTQ * net::FRAME_MAX);
        memset(clients, 0, sizeof clients);
        memset(peers, 0, sizeof peers);
        remote_n = remote_next = 0;
        frame_type = 0;
        udp_in = udp_out = ipx_in = ipx_out = dropped = 0;
        active = true;
        net_promisc(true);
        if (cfg.mode == 1) {
            Peer* d = &peers[0];
            d->used = d->dos = true;
            memcpy(d->mac, net_cur_mac(), 6);
            d->port = cfg.local_port;
            d->last = ticks;
            register_peer(d);
            kprintf("ipxnet: connecting to port %u (IPv%u)\n", cfg.port, cfg.family);
        } else kprintf("ipxnet: server on UDP port %u\n", cfg.port);
        flush();
        AX(r) = 0;
        return;
    }
    Status* s = (Status*)p;
    memset(s, 0, sizeof *s);
    s->mode = active ? cfg.mode : 0;
    s->port = cfg.port;
    s->frame = (uint8_t)frame_type;
    s->udp_in = udp_in; s->udp_out = udp_out; s->ipx_in = ipx_in; s->ipx_out = ipx_out; s->dropped = dropped;
    if (active && cfg.mode == 1) {
        s->registered = peers[0].registered;
        memcpy(s->node, peers[0].node, 6);
        int n = 0;
        for (int i = 1; i < MAX_PEERS && n < 16; i++) {
            if (!peers[i].used) continue;
            Entry& e = s->e[n++];
            e.used = 1; e.family = cfg.family; e.port = peers[i].port;
            memcpy(e.node, peers[i].node, 6); memcpy(e.mac, peers[i].mac, 6);
        }
        s->entries = (uint8_t)n;
    } else if (active) {
        int n = 0;
        for (auto& c : clients) {
            if (!c.used || n == 16) continue;
            Entry& e = s->e[n++];
            e.used = 1; e.family = c.family; e.port = c.port;
            memcpy(e.node, c.node, 6); memcpy(e.addr, c.addr, 16);
        }
        s->entries = (uint8_t)n;
    }
    AX(r) = 0;
}
