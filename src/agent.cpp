// The QEMU guest agent (qemu-ga's protocol) on a virtio-serial port named
// org.qemu.guest_agent.0 (libvirt: <channel type='unix'><target type='virtio'
// name='org.qemu.guest_agent.0'/></channel>; virt-manager adds it by default).
// What it answers: guest-sync(-delimited), guest-ping, guest-info,
// guest-shutdown (powerdown, halt, reboot), guest-get-time,
// guest-get-osinfo, guest-get-host-name ("hostname=" on the command line),
// guest-network-get-interfaces (DOS's address as net.cpp sees it go by).
// Anything else: CommandNotFound. "agent=off" skips it.
//
// The clipboard (clip.c) on the SPICE agent's port, com.redhat.spice.0
// (virt-manager's SPICE channel, or QEMU's own: -chardev
// qemu-vdagent,id=vda,clipboard=on -device virtserialport,chardev=vda,
// name=com.redhat.spice.0): spice-vdagent's protocol, just its clipboard
// (CLIPBOARD_BY_DEMAND, UTF-8 text). The host's copy is fetched when it
// grabs the clipboard, for Ctrl+Shift+V; a copy in vmdos grabs it back.
//
// virtio-serial (virtio-console with MULTIPORT), legacy or virtio 1.0, the
// transport in virtio.cpp: queues 2/3 carry control messages, port p's data
// is on queues 0/1 (p = 0) or 2p+2/2p+3. The device names its ports
// (PORT_ADD, then PORT_NAME once we say PORT_READY); we open the agent's
// (PORT_OPEN) and talk JSON on it, one object per request, a line per reply.
// Polled from the timer.
#include "virtio.hpp"
#include "version.h"

extern "C" int net_guest_addr(uint8_t* mac, uint8_t* ip, int* prefix);

namespace {

int strcmp(const char* a, const char* b) { while (*a && *a == *b) a++, b++; return (uint8_t)*a - (uint8_t)*b; }

const uint32_t F_MULTIPORT = 1u << 1;
enum { DEVICE_READY = 0, PORT_ADD = 1, PORT_REMOVE = 2, PORT_READY = 3, CONSOLE_PORT = 4, RESIZE = 5,
       PORT_OPEN = 6, PORT_NAME = 7 };
const char AGENT_NAME[] = "org.qemu.guest_agent.0";
const char SPICE_NAME[] = "com.redhat.spice.0";
const int MAXPORTS = 8;                         // ports 0-7 (libvirt puts the agent at 1 or 2)
const int NQ = 2 * MAXPORTS + 2;
const int NBUF = 4, BUFSZ = 1024;               // receive buffers per queue we read
const int NTX = 4, TXSZ = 4096;                 // send slots per queue we write

struct Ring {
    Virtq q;
    bool up;
    uint8_t* buf;                               // NBUF x BUFSZ (receive) or NTX x TXSZ (send)
    bool busy[NTX];
};

VirtioDev v;
Ring rings[NQ];
int nports, agent = -1, qnum_rx, qnum_tx;
int spice = -1, sq_rx, sq_tx;
bool on, host_open, spice_open;

int rxq(int p) { return p ? 2 * p + 2 : 0; }

void post_rx(int qn)
{
    Ring& r = rings[qn];
    if (!r.buf) r.buf = (uint8_t*)phys_alloc(NBUF * BUFSZ);
    volatile VirtqDesc* d = r.q.desc();
    for (int i = 0; i < NBUF; i++) {
        d[i].addr = (uint32_t)(uintptr_t)(r.buf + i * BUFSZ); d[i].len = BUFSZ; d[i].flags = VQ_WRITE; d[i].next = 0;
        r.q.push((uint16_t)i);
    }
    virtio_notify(v, qn, r.q);
}

void repost(int qn, uint16_t i)
{
    rings[qn].q.push(i);
    virtio_notify(v, qn, rings[qn].q);
}

void reclaim(int qn)
{
    Ring& r = rings[qn];
    while (r.q.has_used()) { VirtqUsed u = r.q.take(); if (u.id < NTX) r.busy[u.id] = false; }
}

bool send(int qn, const void* data, int len)
{
    Ring& r = rings[qn];
    if (!r.up || len <= 0 || len > TXSZ) return false;
    if (!r.buf) r.buf = (uint8_t*)phys_alloc(NTX * TXSZ);
    reclaim(qn);
    for (int i = 0; i < NTX; i++) {
        if (r.busy[i]) continue;
        memcpy(r.buf + i * TXSZ, data, len);
        volatile VirtqDesc* d = r.q.desc();
        d[i].addr = (uint32_t)(uintptr_t)(r.buf + i * TXSZ); d[i].len = (uint32_t)len; d[i].flags = 0; d[i].next = 0;
        r.busy[i] = true;
        r.q.push((uint16_t)i);
        virtio_notify(v, qn, r.q);
        return true;
    }
    return false;                               // all slots still with the host: dropped
}

void control(uint32_t id, uint16_t event, uint16_t value)
{
    struct { uint32_t id; uint16_t event, value; } m = { id, event, value };
    send(3, &m, sizeof m);
}

// ---------------- the replies ----------------
char out[TXSZ];
int outn;

void put(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(out + outn, sizeof out - outn, fmt, ap);
    va_end(ap);
    if (w > 0) outn += w;
    if (outn > (int)sizeof out - 1) outn = sizeof out - 1;
}

const char* dec(int64_t v)                      // kprintf's vsnprintf has no %lld
{
    static char b[2][24];
    static int k;
    char* e = b[k ^= 1] + 23;
    *e = 0;
    uint64_t u = v < 0 ? -(uint64_t)v : (uint64_t)v;
    do { *--e = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) *--e = '-';
    return e;
}

void flush_reply()
{
    if (outn && agent >= 0) send(qnum_tx, out, outn);
    outn = 0;
}

// ---------------- just enough JSON ----------------
// The value after "key": in [p, e), or 0 (the first such key at any depth).
const char* jfind(const char* p, const char* e, const char* key)
{
    int kl = (int)strlen(key);
    for (; p + kl + 2 < e; p++) {
        if (*p != '"' || memcmp(p + 1, key, kl) || p[kl + 1] != '"') continue;
        const char* q = p + kl + 2;
        while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
        if (q < e && *q == ':') {
            q++;
            while (q < e && (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n')) q++;
            return q;
        }
    }
    return 0;
}

int jstr(const char* p, const char* e, char* s, int max)      // a string value (no escapes kept)
{
    if (!p || p >= e || *p != '"') return 0;
    int n = 0;
    for (p++; p < e && *p != '"' && n < max - 1; p++) {
        if (*p == '\\' && p + 1 < e) p++;
        s[n++] = *p;
    }
    s[n] = 0;
    return 1;
}

bool jnum(const char* p, const char* e, int64_t* v)
{
    if (!p || p >= e) return false;
    bool neg = *p == '-';
    if (neg) p++;
    if (p >= e || *p < '0' || *p > '9') return false;
    int64_t n = 0;
    while (p < e && *p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    *v = neg ? -n : n;
    return true;
}

// ---------------- the commands ----------------
uint8_t cmos(uint8_t r) { outb(0x70, r); return inb(0x71); }

int64_t rtc_unix()                              // the RTC (taken as UTC), seconds since 1970
{
    for (int i = 0; i < 10000 && (cmos(0x0A) & 0x80); i++) kdelay_us(10);   // not mid-update
    uint8_t sec = cmos(0), min = cmos(2), hr = cmos(4), day = cmos(7), mon = cmos(8), yr = cmos(9), cen = cmos(0x32);
    uint8_t b = cmos(0x0B);
    bool pm = hr & 0x80;
    hr &= 0x7F;
    if (!(b & 4)) {
        auto bin = [](uint8_t x) { return (uint8_t)((x >> 4) * 10 + (x & 15)); };
        sec = bin(sec); min = bin(min); hr = bin(hr); day = bin(day); mon = bin(mon); yr = bin(yr); cen = bin(cen);
    }
    if (!(b & 2)) { hr %= 12; if (pm) hr += 12; }
    int y = (cen >= 19 && cen <= 21 ? cen : 20) * 100 + yr;
    // days from 1970-01-01 (H. Hinnant's days_from_civil)
    int yy = y - (mon <= 2), era = yy / 400, yoe = yy - era * 400;
    int doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int64_t days = (int64_t)era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + hr * 3600 + min * 60 + sec;
}

const char* const commands[] = {
    "guest-sync", "guest-sync-delimited", "guest-ping", "guest-info", "guest-shutdown", "guest-get-time",
    "guest-get-osinfo", "guest-get-host-name", "guest-network-get-interfaces",
};

void host_name(char* h, int max)
{
    const char* c = strstr(cmdline, "hostname=");
    int n = 0;
    if (c) for (c += 9; *c && *c != ' ' && n < max - 1; c++) h[n++] = *c;
    if (!n) for (const char* d = "vmdos"; *d && n < max - 1; d++) h[n++] = *d;
    h[n] = 0;
}

void request(const char* p, const char* e)
{
    char cmd[48];
    if (!jstr(jfind(p, e, "execute"), e, cmd, sizeof cmd)) {
        put("{\"error\":{\"class\":\"GenericError\",\"desc\":\"no execute\"}}\n");
        return;
    }
    const char* args = jfind(p, e, "arguments");
    const char* ae = args ? e : p;
    if (!args) args = p;

    if (!strcmp(cmd, "guest-sync") || !strcmp(cmd, "guest-sync-delimited")) {
        int64_t id = 0;
        jnum(jfind(args, ae, "id"), ae, &id);
        if (cmd[10]) put("\xff");               // -delimited: a 0xFF first, to resync the host's parser
        put("{\"return\":%s}\n", dec(id));
    } else if (!strcmp(cmd, "guest-ping")) {
        put("{\"return\":{}}\n");
    } else if (!strcmp(cmd, "guest-info")) {
        put("{\"return\":{\"version\":\"vmdos %s\",\"supported_commands\":[", VMDOS_VERSION);
        for (unsigned i = 0; i < sizeof commands / sizeof *commands; i++)
            put("%s{\"enabled\":true,\"name\":\"%s\",\"success-response\":%s}", i ? "," : "", commands[i],
                strcmp(commands[i], "guest-shutdown") ? "true" : "false");
        put("]}}\n");
    } else if (!strcmp(cmd, "guest-shutdown")) {
        char mode[16] = "powerdown";
        jstr(jfind(args, ae, "mode"), ae, mode, sizeof mode);
        bool rb = !strcmp(mode, "reboot");
        kprintf("agent: guest-shutdown (%s)\n", mode);
        video_osd(rb ? "Host asked: restarting" : "Host asked: turning off");
        power_req = rb ? 2 : 1;                 // v86.c, when the disks are quiet; no reply (as qemu-ga)
    } else if (!strcmp(cmd, "guest-get-time")) {
        put("{\"return\":%s000000000}\n", dec(rtc_unix()));
    } else if (!strcmp(cmd, "guest-get-osinfo")) {
        put("{\"return\":{\"id\":\"dos\",\"name\":\"DOS\",\"pretty-name\":\"DOS (vmdos %s)\",\"kernel-release\":\"%s\","
            "\"kernel-version\":\"vmdos\",\"machine\":\"i686\"}}\n", VMDOS_VERSION, VMDOS_VERSION);
    } else if (!strcmp(cmd, "guest-get-host-name")) {
        char h[64];
        host_name(h, sizeof h);
        put("{\"return\":{\"host-name\":\"%s\"}}\n", h);
    } else if (!strcmp(cmd, "guest-network-get-interfaces")) {
        uint8_t mac[6], ip[4];
        int prefix = 0;
        put("{\"return\":[");
        if (net_guest_addr(mac, ip, &prefix)) {
            put("{\"name\":\"eth0\",\"hardware-address\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"ip-addresses\":[",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            if (ip[0] | ip[1] | ip[2] | ip[3])
                put("{\"ip-address-type\":\"ipv4\",\"ip-address\":\"%u.%u.%u.%u\",\"prefix\":%d}", ip[0], ip[1], ip[2], ip[3], prefix);
            put("]}");
        }
        put("]}\n");
    } else {
        put("{\"error\":{\"class\":\"CommandNotFound\",\"desc\":\"The command %s has not been found\"}}\n", cmd);
    }
}

// The byte stream: whole JSON objects (by brace depth, strings minded);
// anything between them (newlines, the host's 0xFF resync) is skipped.
char req[2048];
int reqn, depth;
bool in_str, esc, overflow;

void feed(const uint8_t* b, int n)
{
    for (int i = 0; i < n; i++) {
        char c = (char)b[i];
        if (!depth) {
            if (c != '{') continue;
            reqn = 0; in_str = esc = overflow = false;
        }
        if (reqn < (int)sizeof req) req[reqn++] = c; else overflow = true;
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') in_str = true;
        else if (c == '{' || c == '[') depth++;
        else if ((c == '}' || c == ']') && --depth == 0) {
            if (overflow) put("{\"error\":{\"class\":\"GenericError\",\"desc\":\"request too long\"}}\n");
            else request(req, req + reqn);
            flush_reply();
        }
    }
}

// ---------------- the SPICE agent: the clipboard ----------------
enum { VD_MSG_CLIPBOARD = 4, VD_MSG_ANNOUNCE = 6, VD_MSG_GRAB = 7, VD_MSG_REQUEST = 8, VD_MSG_RELEASE = 9 };
enum { CAP_CLIPBOARD_BY_DEMAND = 5, CAP_CLIPBOARD_SELECTION = 6, CAP_LINEEND_LF = 8 };
const uint32_t VD_UTF8 = 1;
const int VD_CHUNK = 2048;                      // the most data in one chunk
const uint32_t VD_HDR = 20;                     // protocol, type, opaque (8), size

// Sent as a byte stream: chunks (port, size) each carrying part of a message.
uint8_t* vout;                                  // what's still to go to the host
uint32_t vout_n;
const uint32_t VOUT_MAX = 72 * 1024;
bool host_selection;                            // both sides said CLIPBOARD_SELECTION: a 4-byte prefix

void vd_put(const void* p, uint32_t n)
{
    if (!vout) vout = (uint8_t*)phys_alloc(VOUT_MAX);
    if (vout_n + n > VOUT_MAX) return;
    memcpy(vout + vout_n, p, n);
    vout_n += n;
}

// A message: the header and the data, cut into chunks.
void vd_msg(uint32_t type, const uint8_t* pre, uint32_t pren, const uint8_t* data, uint32_t n)
{
    uint8_t h[VD_HDR] = {};
    uint32_t total = pren + n;
    *(uint32_t*)h = 1;                          // VD_AGENT_PROTOCOL
    *(uint32_t*)(h + 4) = type;
    *(uint32_t*)(h + 16) = total;
    uint32_t all = VD_HDR + total, done = 0;
    if (vout_n + all + (all / VD_CHUNK + 1) * 8 > VOUT_MAX) { kprintf("clip: too much to send at once\n"); return; }
    while (done < all) {
        uint32_t c = all - done < (uint32_t)VD_CHUNK ? all - done : VD_CHUNK;
        uint32_t ch[2] = { 1, c };              // VDP_CLIENT_PORT
        vd_put(ch, 8);
        for (uint32_t i = 0; i < c; i++, done++) {   // byte i of header, prefix, data
            uint32_t k = done;
            uint8_t b = k < VD_HDR ? h[k] : k < VD_HDR + pren ? pre[k - VD_HDR] : data[k - VD_HDR - pren];
            vd_put(&b, 1);
        }
    }
}

void vd_clip_msg(uint32_t type, const uint8_t* data, uint32_t n)
{
    uint8_t sel[4] = {};                        // selection 0: the clipboard
    vd_msg(type, sel, host_selection ? 4 : 0, data, n);
}

void vd_announce(bool request)
{
    uint32_t a[2] = { request ? 1u : 0u, 1u << CAP_CLIPBOARD_BY_DEMAND | 1u << CAP_LINEEND_LF };
    vd_msg(VD_MSG_ANNOUNCE, nullptr, 0, (const uint8_t*)a, sizeof a);
}

void vd_pump()
{
    if (spice < 0 || !vout_n) return;
    reclaim(sq_tx);
    uint32_t off = 0;
    while (off < vout_n) {
        uint32_t c = vout_n - off < (uint32_t)TXSZ ? vout_n - off : TXSZ;
        if (!send(sq_tx, vout + off, (int)c)) break;   // the host still has every slot
        off += c;
    }
    memmove(vout, vout + off, vout_n - off);
    vout_n -= off;
}

void vd_message(uint32_t type, const uint8_t* d, uint32_t n)
{
    dbg(2, "clip: SPICE agent message %u, %u bytes\n", type, n);
    if (type == VD_MSG_ANNOUNCE) {
        if (n < 8) return;
        uint32_t req = *(const uint32_t*)d, caps = *(const uint32_t*)(d + 4);
        host_selection = false;                 // (we don't offer it: no prefix either way)
        dbg(1, "clip: the host's SPICE agent caps %x%s\n", caps, req ? ", asks ours" : "");
        if (req) vd_announce(false);
        return;
    }
    if (type != VD_MSG_GRAB && type != VD_MSG_CLIPBOARD && type != VD_MSG_REQUEST && type != VD_MSG_RELEASE) return;
    if (host_selection) { if (n < 4 || d[0] != 0) return; d += 4; n -= 4; }
    if (type == VD_MSG_GRAB) {                  // the host copied: fetch its text, if it has some
        for (uint32_t i = 0; i + 4 <= n; i += 4)
            if (*(const uint32_t*)(d + i) == VD_UTF8) { vd_clip_msg(VD_MSG_REQUEST, (const uint8_t*)&VD_UTF8, 4); return; }
    } else if (type == VD_MSG_CLIPBOARD) {
        if (n >= 4 && *(const uint32_t*)d == VD_UTF8) clip_from_host(d + 4, n - 4);
    } else if (type == VD_MSG_REQUEST) {        // the host pastes what vmdos copied
        static uint8_t* t;
        if (!t) t = (uint8_t*)phys_alloc(65536 * 3 + 4);
        *(uint32_t*)t = VD_UTF8;
        uint32_t len = clip_to_utf8(t + 4, 65536 * 3);
        uint32_t room = VOUT_MAX / 2;           // what fits in the send buffer with its chunk headers
        if (len + 4 > room) len = room - 4;
        vd_clip_msg(VD_MSG_CLIPBOARD, t, len + 4);
    }
}

// The byte stream: chunk headers, and the messages they carry.
uint32_t vin_hdr_n, vin_chunk_left;
uint8_t vin_hdr[8];
uint8_t* vmsg;
uint32_t vmsg_n, vmsg_want;
const uint32_t VMSG_MAX = 192 * 1024 + VD_HDR + 8;

void vd_feed(const uint8_t* b, int n)
{
    if (!vmsg) vmsg = (uint8_t*)phys_alloc(VMSG_MAX);
    for (int i = 0; i < n; ) {
        if (!vin_chunk_left) {                  // a chunk header
            vin_hdr[vin_hdr_n++] = b[i++];
            if (vin_hdr_n == 8) { vin_chunk_left = *(uint32_t*)(vin_hdr + 4); vin_hdr_n = 0; }
            continue;
        }
        uint8_t c = b[i++];
        vin_chunk_left--;
        if (vmsg_n < VMSG_MAX) vmsg[vmsg_n] = c;
        vmsg_n++;
        if (vmsg_n == VD_HDR) vmsg_want = VD_HDR + *(uint32_t*)(vmsg + 16);
        if (vmsg_n >= VD_HDR && vmsg_n == vmsg_want) {
            if (vmsg_want <= VMSG_MAX) vd_message(*(uint32_t*)(vmsg + 4), vmsg + VD_HDR, vmsg_want - VD_HDR);
            else kprintf("clip: a SPICE agent message too big (%u bytes) skipped\n", vmsg_want);
            vmsg_n = 0; vmsg_want = 0;
        }
    }
}

// ---------------- the device ----------------
void control_msg(const uint8_t* m, int len)
{
    if (len < 8) return;
    uint32_t id = *(const uint32_t*)m;
    uint16_t ev = *(const uint16_t*)(m + 4), val = *(const uint16_t*)(m + 6);
    switch (ev) {
    case PORT_ADD:
        control(id, PORT_READY, id < (uint32_t)nports);
        break;
    case PORT_NAME: {
        int n = len - 8, k = (int)sizeof AGENT_NAME - 1;   // QEMU sends a NUL after the name
        bool match = (n == k || (n > k && !m[8 + k])) && !memcmp(m + 8, AGENT_NAME, k);
        if (match && agent < 0 && id < (uint32_t)nports) {
            agent = (int)id;
            qnum_rx = rxq(agent);
            qnum_tx = qnum_rx + 1;
            post_rx(qnum_rx);
            control(id, PORT_OPEN, 1);
            kprintf("agent: QEMU guest agent on virtio-serial port %u\n", id);
        }
        int ks = (int)sizeof SPICE_NAME - 1;
        bool sm = (n == ks || (n > ks && !m[8 + ks])) && !memcmp(m + 8, SPICE_NAME, ks);
        if (sm && spice < 0 && id < (uint32_t)nports && !strstr(cmdline, "clipboard=off")) {
            spice = (int)id;
            sq_rx = rxq(spice);
            sq_tx = sq_rx + 1;
            post_rx(sq_rx);
            control(id, PORT_OPEN, 1);
            kprintf("agent: SPICE agent (the clipboard) on virtio-serial port %u\n", id);
        }
        break; }
    case PORT_OPEN:
        if ((int)id == agent) { host_open = val; depth = 0; }
        if ((int)id == spice) {
            spice_open = val;
            vin_hdr_n = vin_chunk_left = vmsg_n = vmsg_want = 0;
            vout_n = 0;
            if (val) vd_announce(true);
        }
        break;
    default:
        break;
    }
}

bool probe(const PciDevice& pd)
{
    if (!virtio_open(v, pd)) return false;
    uint32_t got;
    if (!virtio_features(v, F_MULTIPORT, &got) || !(got & F_MULTIPORT)) {
        kprintf("virtio-serial %02x:%02x.%u: no multiport: no agent\n", pd.bus, pd.dev, pd.fn);
        virtio_fail(v);
        return false;
    }
    uint32_t max = virtio_cfg32(v, 4);
    nports = max < (uint32_t)MAXPORTS ? (int)max : MAXPORTS;
    for (int p = 0; p < nports; p++)
        for (int k = 0; k < 2; k++) rings[rxq(p) + k].up = virtio_queue(v, rxq(p) + k, rings[rxq(p) + k].q, 8);
    rings[2].up = virtio_queue(v, 2, rings[2].q, 8);
    rings[3].up = virtio_queue(v, 3, rings[3].q, 8);
    if (!rings[2].up || !rings[3].up) { kprintf("virtio-serial: no control queues\n"); virtio_fail(v); return false; }
    virtio_ready(v);
    post_rx(2);
    control(0, DEVICE_READY, 1);
    kprintf("virtio-serial %02x:%02x.%u%s: %u ports, looking for %s\n", pd.bus, pd.dev, pd.fn,
            v.modern ? "" : " (legacy)", max, AGENT_NAME);
    return true;
}

} // namespace

extern "C" void agent_init(void)
{
    if (strstr(cmdline, "agent=off")) return;
    PciDevice pd;                               // the first virtio-serial (libvirt makes one)
    if (pci_find_id(0x1AF4, 0x1003, &pd) || pci_find_id(0x1AF4, 0x1043, &pd)) on = probe(pd);
}

extern "C" void agent_tick(void)
{
    if (!on) return;
    reclaim(3);
    Ring& c = rings[2];
    while (c.q.has_used()) {
        VirtqUsed u = c.q.take();
        if (u.id < NBUF) { control_msg(c.buf + u.id * BUFSZ, (int)u.len); repost(2, (uint16_t)u.id); }
    }
    if (spice >= 0) {
        Ring& s = rings[sq_rx];
        while (s.q.has_used()) {
            VirtqUsed u = s.q.take();
            if (u.id < NBUF) { vd_feed(s.buf + u.id * BUFSZ, (int)u.len); repost(sq_rx, (uint16_t)u.id); }
        }
        if (clip_grab_req && spice_open) { clip_grab_req = 0; vd_clip_msg(VD_MSG_GRAB, (const uint8_t*)&VD_UTF8, 4); dbg(2, "clip: grab sent to the host\n"); }
        vd_pump();
    }
    if (agent < 0) return;
    reclaim(qnum_tx);
    Ring& r = rings[qnum_rx];
    while (r.q.has_used()) {
        VirtqUsed u = r.q.take();
        if (u.id < NBUF) { feed(r.buf + u.id * BUFSZ, (int)u.len); repost(qnum_rx, (uint16_t)u.id); }
    }
}
