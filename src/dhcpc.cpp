// DHCP lease renewal, run by vmdos itself underneath DOS, so DOS's IPv4
// address stays leased with nothing resident in DOS. DHCP.EXE gets the
// lease as usual, then hands it here (INT 2Fh AX=564Eh BL=11) and exits;
// DHCP -status asks how it's doing (BL=12).
//
// RFC 2131 section 4.4.5: at T1 (half the lease unless the server said)
// a DHCPREQUEST goes straight to the server that gave the lease
// (RENEWING); no answer by T2 (7/8 of it) and it is broadcast for any
// server (REBINDING); retries at half the time left, at least 60 s apart.
// An ACK starts the lease over. If the lease runs out with nobody
// answering, vmdos keeps asking for the same address (INIT-REBOOT, every
// 60 s) so it comes back when the server does. A NAK, or an ACK for
// another address, means DOS's address is gone: vmdos stops and logs it
// (DHCP -status shows it), since only running DHCP again gives DOS's
// TCP/IP stack its new settings.
//
// The answers are vmdos's (its own transaction ID) and go no further; DOS
// never sees them. dhcplog on the kernel command line logs each step.
#include "net.hpp"

namespace {

// INT 2Fh AX=564Eh BL=11: ES:DI -> this (mode 0: stop)
struct __attribute__((packed)) Lease {
    uint8_t  mode;                  // 0 stop, 1 keep renewing
    uint8_t  ip[4];                 // DOS's address (yiaddr)
    uint8_t  server[4];             // the server identifier (option 54)
    uint8_t  server_mac[6];         // where its ACK came from (it, or the relay on our side)
    uint32_t lease, t1, t2;         // seconds from now (t1, t2 0: the defaults)
    uint32_t want;                  // the lease to ask for (option 51; 0: none)
    char     host[33];              // option 12 (NUL-terminated; empty: none)
};

// BL=12: ES:DI <- this
struct __attribute__((packed)) Status {
    uint8_t  state;                 // State below
    uint8_t  ip[4], server[4];
    uint32_t lease;                 // the lease, seconds
    uint32_t age;                   // seconds since the last ACK
    uint32_t next;                  // seconds to the next request
    uint32_t renewals, sent;        // ACKs since DHCP handed it over; requests sent
    uint8_t  why;                   // STOPPED: 1 NAK, 2 another address
};

enum State : uint8_t { OFF, BOUND, RENEWING, REBINDING, EXPIRED, STOPPED };

Lease cfg;
State state = OFF;
uint32_t now_s, sec_mark;           // seconds since vmdos started counting (from ticks)
uint32_t bound_at;                  // now_s at the last ACK
uint32_t t1_at, t2_at, end_at;      // now_s of T1, T2, the lease's end
uint32_t next_at;                   // the next request
uint32_t try_start;                 // now_s when this round of requests began (the secs field)
uint32_t xid;
uint32_t renewals, sent;
uint8_t why;
int logging = -1;

bool log_on()
{
    if (logging < 0) logging = strstr(cmdline, "dhcplog") != 0;
    return logging;
}
#define dlog(...) do { if (log_on()) kprintf(__VA_ARGS__); } while (0)

const char* state_name(State s)
{
    static const char* n[] = { "off", "bound", "renewing", "rebinding", "expired", "stopped" };
    return n[s];
}

inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

uint16_t csum(const uint8_t* p, unsigned n)
{
    uint32_t s = 0;
    while (n > 1) { s += (p[0] << 8) | p[1]; p += 2; n -= 2; }
    if (n) s += p[0] << 8;
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

// Times from a lease of `lease` seconds starting now (t1, t2 from the server, 0: defaults).
void start_lease(uint32_t lease, uint32_t t1, uint32_t t2)
{
    cfg.lease = lease;
    bound_at = now_s;
    if (!t1 || t1 >= lease) t1 = lease / 2;
    if (!t2 || t2 >= lease || t2 < t1) t2 = lease - lease / 8;
    if (t2 < t1) t2 = t1;
    t1_at = now_s + t1; t2_at = now_s + t2; end_at = now_s + lease;
    next_at = t1_at;
    state = BOUND;
    dlog("dhcp: %u.%u.%u.%u leased for %u s; renewing in %u s, rebinding in %u s\n",
         cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3], lease, t1, t2);
}

// When to try again: half the time left to `until`, at least 60 s (and not past it).
void retry_by(uint32_t until)
{
    uint32_t left = until > now_s ? until - now_s : 0;
    uint32_t wait = left / 2;
    if (wait < 60) wait = 60;
    if (wait > left && left) wait = left;
    next_at = now_s + wait;
}

void send_request()
{
    static uint8_t f[14 + 20 + 8 + 300];
    memset(f, 0, sizeof f);
    const uint8_t* me = net_cur_mac();
    bool unicast = state == RENEWING;
    bool reboot = state == EXPIRED;             // INIT-REBOOT: no address of our own yet
    // Ethernet
    if (unicast) memcpy(f, cfg.server_mac, 6); else memset(f, 0xFF, 6);
    memcpy(f + 6, me, 6);
    f[12] = 0x08; f[13] = 0x00;
    // BOOTP / DHCP
    uint8_t* b = f + 14 + 20 + 8;
    b[0] = 1; b[1] = 1; b[2] = 6;               // request, Ethernet, 6-byte address
    put32(b + 4, xid);
    uint32_t secs = now_s - try_start;
    b[8] = (uint8_t)((secs > 0xFFFF ? 0xFFFF : secs) >> 8); b[9] = (uint8_t)(secs > 0xFFFF ? 0xFF : secs);
    if (!reboot) memcpy(b + 12, cfg.ip, 4);     // ciaddr
    memcpy(b + 28, me, 6);                      // chaddr
    uint8_t* o = b + 236;
    o[0] = 99; o[1] = 130; o[2] = 83; o[3] = 99;
    o += 4;
    *o++ = 53; *o++ = 1; *o++ = 3;              // DHCPREQUEST
    if (reboot) { *o++ = 50; *o++ = 4; memcpy(o, cfg.ip, 4); o += 4; }   // the address we had
    unsigned hl = strlen(cfg.host);
    if (hl) { *o++ = 12; *o++ = (uint8_t)hl; memcpy(o, cfg.host, hl); o += hl; }
    if (cfg.want) { *o++ = 51; *o++ = 4; put32(o, cfg.want); o += 4; }
    *o++ = 55; *o++ = 6; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51; *o++ = 58; *o++ = 59;
    *o++ = 255;
    unsigned blen = 300;                        // BOOTP's minimum; the rest is padding
    // UDP 68 -> 67 (no checksum: optional over IPv4)
    uint8_t* u = f + 14 + 20;
    u[0] = 0; u[1] = 68; u[2] = 0; u[3] = 67;
    u[4] = (uint8_t)((8 + blen) >> 8); u[5] = (uint8_t)(8 + blen);
    // IPv4
    uint8_t* ip = f + 14;
    unsigned tot = 20 + 8 + blen;
    ip[0] = 0x45; ip[2] = (uint8_t)(tot >> 8); ip[3] = (uint8_t)tot;
    static uint16_t ident;
    ident++; ip[4] = ident >> 8; ip[5] = (uint8_t)ident;
    ip[8] = 64; ip[9] = 17;
    if (!reboot) memcpy(ip + 12, cfg.ip, 4);
    if (unicast) memcpy(ip + 16, cfg.server, 4); else memset(ip + 16, 0xFF, 4);
    uint16_t c = csum(ip, 20);
    ip[10] = c >> 8; ip[11] = (uint8_t)c;
    net_wire_send(f, sizeof f);
    sent++;
    dlog("dhcp: %s: DHCPREQUEST for %u.%u.%u.%u%s\n", state_name(state),
         cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3], unicast ? " to the server" : " (broadcast)");
}

void new_round()
{
    xid = pit_clock() ^ (ticks << 12) ^ 0x564D0000u;
    try_start = now_s;
}

void stop(uint8_t reason, const char* msg)
{
    state = STOPPED;
    why = reason;
    kprintf("dhcp: %s: %u.%u.%u.%u is no longer DOS's; run DHCP again\n", msg,
            cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3]);
}

} // namespace

bool dhcpc_active() { return state != OFF && state != STOPPED; }

// A frame from the wire: true if it was an answer to one of our requests.
bool dhcpc_rx(const uint8_t* f, unsigned len)
{
    if (len < 14 + 20 + 8 + 240 || f[12] != 0x08 || f[13] != 0x00 || (f[14] >> 4) != 4 || f[23] != 17) return false;
    unsigned ihl = (f[14] & 15) * 4, u = 14 + ihl, b = u + 8;
    if ((f[20] & 0x3F) || f[21]) return false;  // a fragment
    if (b + 240 > len || f[u] != 0 || f[u + 1] != 67 || f[u + 2] != 0 || f[u + 3] != 68) return false;
    if (f[b] != 2 || be32(f + b + 4) != xid || memcmp(f + b + 28, net_cur_mac(), 6)) return false;
    if (memcmp(f + b + 236, "\x63\x82\x53\x63", 4)) return false;
    int type = 0;
    uint32_t lease = 0, t1 = 0, t2 = 0;
    const uint8_t* sid = 0;
    for (unsigned o = b + 240; o < len && f[o] != 255;) {
        if (f[o] == 0) { o++; continue; }
        if (o + 2 > len) break;
        unsigned n = f[o + 1];
        if (o + 2 + n > len) break;
        const uint8_t* v = f + o + 2;
        if (f[o] == 53 && n >= 1) type = v[0];
        else if (f[o] == 51 && n == 4) lease = be32(v);
        else if (f[o] == 58 && n == 4) t1 = be32(v);
        else if (f[o] == 59 && n == 4) t2 = be32(v);
        else if (f[o] == 54 && n == 4) sid = v;
        o += 2 + n;
    }
    if (type == 6) {                            // NAK
        stop(1, "the DHCP server refused to renew the lease");
        return true;
    }
    if (type != 5) return true;                 // ours, but nothing to do with it
    if (memcmp(f + b + 16, cfg.ip, 4)) {        // yiaddr
        stop(2, "the DHCP server gave another address");
        return true;
    }
    if (sid) memcpy(cfg.server, sid, 4);
    memcpy(cfg.server_mac, f + 6, 6);           // the server, or the relay on our side
    if (!lease) lease = cfg.lease;
    renewals++;
    xid ^= 0x5A5A5A5Au;                         // late copies of this answer are no longer ours
    start_lease(lease, t1, t2);
    return true;
}

// Every timer tick.
void dhcpc_tick()
{
    if (!dhcpc_active()) return;
    uint32_t el = ticks - sec_mark;
    if (el >= 1000) { now_s += el / 1000; sec_mark += el / 1000 * 1000; }
    if ((int32_t)(now_s - next_at) < 0) return;
    State was = state;
    if (state == BOUND || state == RENEWING) {
        if ((int32_t)(now_s - t2_at) >= 0) state = REBINDING;
        else state = RENEWING;
    }
    if (state == REBINDING && (int32_t)(now_s - end_at) >= 0) state = EXPIRED;
    if (state != was) {
        new_round();
        if (state == EXPIRED) kprintf("dhcp: the lease on %u.%u.%u.%u ran out with no answer; still asking for it\n",
                                      cfg.ip[0], cfg.ip[1], cfg.ip[2], cfg.ip[3]);
    }
    send_request();
    if (state == RENEWING) retry_by(t2_at);
    else if (state == REBINDING) retry_by(end_at);
    else next_at = now_s + 60;
}

// INT 2Fh AX=564Eh BL=11 (hand over a lease / stop: AX = 0, 1 nothing to
// renew), BL=12 (status).
void dhcpc_api(struct regs* r, int fn)
{
    uint8_t* p = gptr(LIN(r->v86_es, DI(r)));
    if (fn == 11) {
        state = OFF;
        memcpy(&cfg, p, sizeof cfg);
        cfg.host[sizeof cfg.host - 1] = 0;
        if (cfg.mode != 1 || !cfg.lease || cfg.lease == 0xFFFFFFFFu || !(cfg.ip[0] | cfg.ip[1] | cfg.ip[2] | cfg.ip[3])) {
            if (cfg.mode == 1) dlog("dhcp: nothing to renew\n");
            else dlog("dhcp: stopped\n");
            AX(r) = cfg.mode == 1;
            return;
        }
        sec_mark = ticks;
        renewals = sent = 0;
        why = 0;
        new_round();
        start_lease(cfg.lease, cfg.t1, cfg.t2);
        AX(r) = 0;
        return;
    }
    Status* s = (Status*)p;
    memset(s, 0, sizeof *s);
    s->state = state;
    if (state != OFF) {
        memcpy(s->ip, cfg.ip, 4);
        memcpy(s->server, cfg.server, 4);
        s->lease = cfg.lease;
        s->age = now_s - bound_at;
        s->next = dhcpc_active() && (int32_t)(next_at - now_s) > 0 ? next_at - now_s : 0;
        s->renewals = renewals;
        s->sent = sent;
        s->why = why;
    }
    AX(r) = 0;
}
