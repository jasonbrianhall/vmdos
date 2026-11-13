// Hyper-V VMBus (vmbus.hpp): the hypervisor interface (hypercall page,
// SynIC), the VMBus control protocol (version, offers, GPADLs, opening
// channels) and the channels' ring buffers. After Linux's drivers/hv and
// include/linux/hyperv.h; the Hyper-V TLFS for the MSRs and hypercalls.
//
// What vmdos needs only: one CPU (the boot CPU's VP) takes the messages, on
// SINT 2 as Linux does. That SINT is unmasked with vector 3Fh (cpu.c's IDT:
// a bare IRET) and auto-EOI, so if Hyper-V does interrupt us nothing
// happens; the message page is polled while we wait for an answer.
// Kernel options: vmbus=off (don't look), vmbuslog (every control message).
#include "vmbus.hpp"

namespace {

const uint32_t MSR_GUEST_OS_ID = 0x40000000, MSR_HYPERCALL = 0x40000001, MSR_VP_INDEX = 0x40000002,
               MSR_SCONTROL = 0x40000080, MSR_SIEFP = 0x40000082, MSR_SIMP = 0x40000083, MSR_EOM = 0x40000084,
               MSR_SINT0 = 0x40000090;
const int MSG_SINT = 2;                         // VMBUS_MESSAGE_SINT
const int SINT_VEC = 0x3F;                      // cpu.c: IRET
const uint64_t HVCALL_POST_MESSAGE = 0x5C, HVCALL_SIGNAL_EVENT = 0x5D, HV_FAST = 1u << 16;
enum { HV_OK = 0, HV_INSUFFICIENT_MEMORY = 0x0B, HV_INVALID_CONNECTION_ID = 0x12, HV_INSUFFICIENT_BUFFERS = 0x13 };

// channel message types (enum vmbus_channel_message_type)
enum {
    CH_OFFER = 1, CH_RESCIND = 2, CH_REQUESTOFFERS = 3, CH_ALLOFFERS = 4, CH_OPEN = 5, CH_OPEN_RESULT = 6,
    CH_GPADL_HEADER = 8, CH_GPADL_BODY = 9, CH_GPADL_CREATED = 10, CH_INITIATE = 14, CH_VERSION_RESPONSE = 15,
    CH_UNLOAD = 16, CH_UNLOAD_RESPONSE = 17,
};
inline uint32_t VER(int maj, int min) { return (uint32_t)maj << 16 | (uint32_t)min; }
const uint32_t versions[] = { VER(6, 0), VER(5, 3), VER(5, 2), VER(5, 1), VER(5, 0), VER(4, 1), VER(4, 0),
                              VER(3, 0), VER(2, 4), VER(1, 1), VER(0, 13) };
const uint32_t V_WS2008 = VER(0, 13), V_WIN10_V5 = VER(5, 0);

const int MAXCH = 32;
VmbusChannel chans[MAXCH];
int nch;
bool tried, up, log_on;
uint8_t* hc_page;                               // the hypercall page (Hyper-V's code)
uint8_t* hc_in;                                 // HvPostMessage's input
volatile uint8_t* simp;                         // the SynIC message page
uint8_t *siefp, *int_page, *mon1, *mon2;
uint32_t msg_conn_id, version, vp;
uint32_t next_gpadl = 0xE1E10;

inline uint32_t pa(const void* p) { return (uint32_t)(uintptr_t)p; }   // identity-mapped
inline void mb() { __asm__ volatile("lock; addl $0,(%%esp)" ::: "memory"); }
inline uint32_t rd32(const volatile void* p) { return *(const volatile uint32_t*)p; }
inline uint32_t get32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline uint16_t get16(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
inline void put32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
inline void put16(uint8_t* p, uint16_t v) { memcpy(p, &v, 2); }
inline void put64(uint8_t* p, uint64_t v) { memcpy(p, &v, 8); }

#ifndef VMBUS_SIM
uint64_t rdmsr(uint32_t m) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return (uint64_t)hi << 32 | lo; }
void wrmsr(uint32_t m, uint64_t v) { __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)) : "memory"); }
void cpuid(uint32_t leaf, uint32_t* r)
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

// 32-bit calling convention: EDX:EAX control, EBX:ECX input GPA, EDI:ESI
// output GPA; the result in EDX:EAX. A fast call: EBX:ECX is the input itself.
uint64_t hypercall(uint64_t control, uint32_t in, uint32_t out)
{
    uint64_t st;
    uint32_t in_lo = in;
    __asm__ volatile("call *%[hc]"
                     : "=A"(st), "+c"(in_lo)
                     : "A"(control), "b"(0), "D"(0), "S"(out), [hc] "m"(hc_page)
                     : "cc", "memory");
    return st;
}
#else                                           // tests/hvsim: a simulated Hyper-V host
} // namespace
uint64_t sim_rdmsr(uint32_t m);
void sim_wrmsr(uint32_t m, uint64_t v);
void sim_cpuid(uint32_t leaf, uint32_t* r);
uint64_t sim_hypercall(uint64_t control, uint32_t in, uint32_t out);
namespace {
inline uint64_t rdmsr(uint32_t m) { return sim_rdmsr(m); }
inline void wrmsr(uint32_t m, uint64_t v) { sim_wrmsr(m, v); }
inline void cpuid(uint32_t leaf, uint32_t* r) { sim_cpuid(leaf, r); }
inline uint64_t hypercall(uint64_t control, uint32_t in, uint32_t out) { return sim_hypercall(control, in, out); }
#endif

// A message to the host (HvPostMessage): 0 or the hypervisor's status.
int post(const void* msg, uint32_t len)
{
    if (len > 240) return -1;
    for (int tries = 0; tries < 1000; tries++) {
        memset(hc_in, 0, 256);
        put32(hc_in, msg_conn_id);
        put32(hc_in + 8, 1);                    // message type (Linux: always 1)
        put32(hc_in + 12, len);
        memcpy(hc_in + 16, msg, len);
        int st = (int)(hypercall(HVCALL_POST_MESSAGE, pa(hc_in), 0) & 0xFFFF);
        if (st != HV_INSUFFICIENT_MEMORY && st != HV_INSUFFICIENT_BUFFERS) return st;
        kdelay_us(100);                         // the host's queue is full: again in a moment
    }
    return HV_INSUFFICIENT_BUFFERS;
}

void handle(const uint8_t* m, uint32_t len)
{
    uint32_t type = get32(m);
    if (log_on) kprintf("vmbus: message %u (%u bytes)\n", type, len);
    if (type != CH_OFFER) return;
    uint32_t relid = get32(m + 184);
    for (int i = 0; i < nch; i++) if (chans[i].relid == relid) return;
    if (nch == MAXCH) return;
    VmbusChannel& c = chans[nch++];
    memset(&c, 0, sizeof c);
    memcpy(c.type, m + 8, 16);
    memcpy(c.inst, m + 24, 16);
    memcpy(c.user_def, m + 60, 120);
    c.sub_index = get16(m + 180);
    c.relid = relid;
    if (version > V_WS2008) {                   // win7 and later
        c.dedicated = m[190] & 1;
        c.conn_id = get32(m + 192);
    } else c.conn_id = 2;                        // VMBUS_EVENT_CONNECTION_ID
    if (log_on) {
        const uint8_t* g = c.type;
        kprintf("vmbus: offer relid %u, %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", relid,
                g[3], g[2], g[1], g[0], g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    }
}

// The next message from the host into m (240 bytes), taken off the message page.
bool take(uint8_t* m, uint32_t* len)
{
    volatile uint8_t* slot = simp + MSG_SINT * 256;
    if (!rd32(slot)) return false;              // HVMSG_NONE: empty
    uint32_t n = slot[4];
    if (n > 240) n = 240;
    memset(m, 0, 240);
    for (uint32_t i = 0; i < n; i++) m[i] = slot[16 + i];
    *len = n;
    *(volatile uint32_t*)slot = 0;
    mb();
    if (slot[5] & 1) wrmsr(MSR_EOM, 0);         // more queued: deliver the next
    return true;
}

// Messages until one of type want for which ok() says yes (offers are kept
// on the way); false after ms milliseconds.
bool wait_msg(uint32_t want, uint32_t ms, uint8_t* m, bool (*ok)(const uint8_t*, uint32_t), uint32_t arg)
{
    uint32_t len;
    for (uint32_t t = 0; t < ms * 50; t++) {
        while (take(m, &len)) {
            handle(m, len);
            if (get32(m) == want && (!ok || ok(m, arg))) return true;
        }
        kdelay_us(20);
    }
    return false;
}
bool gpadl_is(const uint8_t* m, uint32_t h) { return get32(m + 12) == h; }
bool relid_is(const uint8_t* m, uint32_t r) { return get32(m + 8) == r; }

int hexval(char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1; }
// "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" as guid_t has it in memory (the first three fields little-endian).
void parse_guid(const char* s, uint8_t* g)
{
    static const uint8_t order[16] = { 3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15 };
    uint8_t raw[16];
    int n = 0;
    for (; *s && n < 32; s++) {
        int v = hexval(*s);
        if (v < 0) continue;
        if (n & 1) raw[n / 2] |= (uint8_t)v; else raw[n / 2] = (uint8_t)(v << 4);
        n++;
    }
    for (int i = 0; i < 16; i++) g[i] = raw[order[i]];
}

uint32_t to_ring(uint8_t* ring, uint32_t size, uint32_t pos, const void* src, uint32_t len)
{
    const uint8_t* s = (const uint8_t*)src;
    while (len) {
        uint32_t k = size - pos < len ? size - pos : len;
        if (s) { memcpy(ring + pos, s, k); s += k; } else memset(ring + pos, 0, k);
        pos = (pos + k) % size;
        len -= k;
    }
    return pos;
}
uint32_t from_ring(const uint8_t* ring, uint32_t size, uint32_t pos, void* dst, uint32_t len)
{
    uint8_t* d = (uint8_t*)dst;
    while (len) {
        uint32_t k = size - pos < len ? size - pos : len;
        memcpy(d, ring + pos, k);
        d += k;
        pos = (pos + k) % size;
        len -= k;
    }
    return pos;
}
// Bytes the writer may still write (one is always left free: full != empty).
inline uint32_t avail_write(uint32_t size, uint32_t read, uint32_t write)
{
    return write >= read ? size - (write - read) : read - write;
}

bool connect()
{
    uint32_t r[4];
    cpuid(1, r);
    if (!(r[2] & (1u << 31))) return false;     // no hypervisor
    cpuid(0x40000000, r);
    if (r[1] != 0x7263694D || r[2] != 0x666F736F || r[3] != 0x76482074) return false;   // "Microsoft Hv"
    if (r[0] < 0x40000005) return false;
    cpuid(0x40000001, r);
    if (r[0] != 0x31237648) return false;       // "Hv#1"
    uint32_t ver[4], feat[4];
    cpuid(0x40000002, ver);
    cpuid(0x40000003, feat);
    kprintf("vmbus: Hyper-V %u.%u build %u\n", ver[1] >> 16, ver[1] & 0xFFFF, ver[0]);
    // SynIC registers (bit 2) and the hypercall MSRs (bit 5); PostMessage, SignalEvent
    if (!(feat[0] & (1u << 2)) || !(feat[0] & (1u << 5)) || !(feat[1] & (1u << 4)) || !(feat[1] & (1u << 5))) {
        kprintf("vmbus: the hypervisor lacks SynIC or messages (features %x %x)\n", feat[0], feat[1]);
        return false;
    }

    // Who we are (an open-source OS, Linux's vendor ID), then the hypercall page.
    if (!rdmsr(MSR_GUEST_OS_ID)) wrmsr(MSR_GUEST_OS_ID, 0x8100ull << 48 | (uint64_t)0x060100 << 16);
    hc_page = (uint8_t*)phys_alloc(4096);
    uint64_t h = rdmsr(MSR_HYPERCALL);
    if (h & 2) { kprintf("vmbus: the hypercall page is locked\n"); return false; }
    wrmsr(MSR_HYPERCALL, (h & 0xFFE) | pa(hc_page) | 1);
    if (!(rdmsr(MSR_HYPERCALL) & 1)) { kprintf("vmbus: no hypercall page\n"); return false; }
    vp = feat[0] & (1u << 6) ? (uint32_t)rdmsr(MSR_VP_INDEX) : 0;   // AccessVpIndex

    hc_in = (uint8_t*)phys_alloc(4096);
    simp = (volatile uint8_t*)phys_alloc(4096);
    siefp = (uint8_t*)phys_alloc(4096);
    int_page = (uint8_t*)phys_alloc(4096);
    mon1 = (uint8_t*)phys_alloc(4096);
    mon2 = (uint8_t*)phys_alloc(4096);
    wrmsr(MSR_SIMP, (rdmsr(MSR_SIMP) & 0xFFE) | pa((const void*)simp) | 1);
    wrmsr(MSR_SIEFP, (rdmsr(MSR_SIEFP) & 0xFFE) | pa(siefp) | 1);
    uint64_t s = rdmsr(MSR_SINT0 + MSG_SINT);
    s = (s & ~0x100FFull) | SINT_VEC | 1u << 17; // unmasked, auto-EOI
    wrmsr(MSR_SINT0 + MSG_SINT, s);
    wrmsr(MSR_SCONTROL, rdmsr(MSR_SCONTROL) | 1);

    // The newest protocol the host knows (connection.c vmbus_connect).
    uint8_t m[240];
    version = 0;
    for (uint32_t v : versions) {
        memset(m, 0, 40);
        put32(m, CH_INITIATE);
        put32(m + 8, v);
        put32(m + 12, vp);
        if (v >= V_WIN10_V5) { m[16] = MSG_SINT; msg_conn_id = 4; }   // VMBUS_MESSAGE_CONNECTION_ID_4
        else { put64(m + 16, pa(int_page)); msg_conn_id = 1; }
        put64(m + 24, pa(mon1));
        put64(m + 32, pa(mon2));
        int st = post(m, 40);
        if (st) {
            if (v >= V_WIN10_V5 && st == HV_INVALID_CONNECTION_ID) continue;   // an older host
            kprintf("vmbus: can't post (status %x): no VMBus\n", st);
            return false;
        }
        if (!wait_msg(CH_VERSION_RESPONSE, 1000, m, 0, 0)) { kprintf("vmbus: no answer from the host\n"); return false; }
        if (!m[8]) continue;                    // not that one
        version = v;
        if (v >= V_WIN10_V5) msg_conn_id = get32(m + 12);
        break;
    }
    if (!version) { kprintf("vmbus: no protocol version in common\n"); return false; }

    memset(m, 0, 8);
    put32(m, CH_REQUESTOFFERS);
    if (post(m, 8)) return false;
    if (!wait_msg(CH_ALLOFFERS, 2000, m, 0, 0)) kprintf("vmbus: not all offers came\n");
    kprintf("vmbus: protocol %u.%u, %d channels\n", version >> 16, version & 0xFFFF, nch);
    return true;
}

} // namespace

bool vmbus_start()
{
    if (tried) return up;
    tried = true;
    if (strstr(cmdline, "vmbus=off")) return false;
    log_on = strstr(cmdline, "vmbuslog") != 0;
    up = connect();
    return up;
}

bool vmbus_up() { return up; }

VmbusChannel* vmbus_find(const char* guid, int nth)
{
    if (!up) return 0;
    uint8_t g[16];
    parse_guid(guid, g);
    for (int i = 0; i < nch; i++)
        if (!memcmp(chans[i].type, g, 16) && nth-- == 0) return &chans[i];
    return 0;
}

bool vmbus_gpadl(VmbusChannel& c, const void* buf, uint32_t bytes, uint32_t* handle)
{
    uint32_t pages = (bytes + 4095) >> 12, first = pa(buf) >> 12;
    if (pa(buf) & 4095) return false;
    uint32_t hd = next_gpadl++;
    uint8_t m[240];
    memset(m, 0, sizeof m);
    put32(m, CH_GPADL_HEADER);
    put32(m + 8, c.relid);
    put32(m + 12, hd);
    put16(m + 16, (uint16_t)(8 + 8 * pages));   // range_buflen: the whole range
    put16(m + 18, 1);                           // one range
    put32(m + 20, pages << 12);
    put32(m + 24, 0);
    uint32_t k = pages < 26 ? pages : 26, done = 0;
    for (uint32_t i = 0; i < k; i++) put64(m + 28 + 8 * i, first + i);
    if (post(m, 28 + 8 * k)) return false;
    for (done = k; done < pages; done += k) {   // the rest, 28 PFNs a message
        k = pages - done < 28 ? pages - done : 28;
        memset(m, 0, sizeof m);
        put32(m, CH_GPADL_BODY);
        put32(m + 12, hd);
        for (uint32_t i = 0; i < k; i++) put64(m + 16 + 8 * i, first + done + i);
        if (post(m, 16 + 8 * k)) return false;
    }
    if (!wait_msg(CH_GPADL_CREATED, 5000, m, gpadl_is, hd)) { kprintf("vmbus: relid %u: no GPADL\n", c.relid); return false; }
    if (get32(m + 16)) { kprintf("vmbus: relid %u: GPADL refused (%x)\n", c.relid, get32(m + 16)); return false; }
    *handle = hd;
    return true;
}

bool vmbus_open(VmbusChannel& c, uint32_t out_pages, uint32_t in_pages, const void* userdata, uint32_t ulen)
{
    if (c.open) return true;
    uint8_t* ring = (uint8_t*)phys_alloc((out_pages + in_pages) * 4096);
    c.out_hdr = (volatile uint32_t*)ring;
    c.out_data = ring + 4096;
    c.out_size = (out_pages - 1) * 4096;
    c.in_hdr = (volatile uint32_t*)(ring + out_pages * 4096);
    c.in_data = ring + (out_pages + 1) * 4096;
    c.in_size = (in_pages - 1) * 4096;
    c.out_hdr[16] = 1;                          // feature_bits: feat_pending_send_sz
    c.in_hdr[16] = 1;
    c.in_hdr[2] = 1;                            // interrupt_mask: we poll, no signals wanted
    if (!vmbus_gpadl(c, ring, (out_pages + in_pages) * 4096, &c.gpadl)) return false;

    uint8_t m[240];
    memset(m, 0, sizeof m);
    put32(m, CH_OPEN);
    put32(m + 8, c.relid);
    put32(m + 12, c.relid);                     // openid
    put32(m + 16, c.gpadl);
    put32(m + 20, vp);
    put32(m + 24, out_pages);                   // where the inbound ring starts, in pages
    if (ulen > 120) ulen = 120;
    if (ulen) memcpy(m + 28, userdata, ulen);
    if (post(m, 148)) return false;
    if (!wait_msg(CH_OPEN_RESULT, 5000, m, relid_is, c.relid)) { kprintf("vmbus: relid %u: no answer to open\n", c.relid); return false; }
    if (get32(m + 16)) { kprintf("vmbus: relid %u: open refused (%x)\n", c.relid, get32(m + 16)); return false; }
    c.open = true;
    return true;
}

void vmbus_signal(VmbusChannel& c)
{
    if (!c.dedicated) {                         // the shared page's second half: guest to host
        uint32_t* bits = (uint32_t*)(int_page + 2048);
        __asm__ volatile("lock; btsl %1,%0" : "+m"(*bits) : "r"(c.relid) : "memory");
    }
    hypercall(HVCALL_SIGNAL_EVENT | HV_FAST, c.conn_id, 0);
}

bool vmbus_send(VmbusChannel& c, uint16_t type, uint16_t flags, uint64_t tid,
                const void* extra, uint32_t elen, const void* data, uint32_t dlen)
{
    if (!c.open) return false;
    uint32_t len = 16 + elen + dlen, aligned = (len + 7) & ~7u;
    uint32_t read = c.out_hdr[1], write = c.out_hdr[0];
    if (read >= c.out_size || write >= c.out_size) return false;
    if (avail_write(c.out_size, read, write) <= aligned + 8) return false;   // full
    VmPacketDesc d = { type, (uint16_t)((16 + elen) >> 3), (uint16_t)(aligned >> 3), flags, tid };
    uint32_t p = to_ring(c.out_data, c.out_size, write, &d, 16);
    if (elen) p = to_ring(c.out_data, c.out_size, p, extra, elen);
    if (dlen) p = to_ring(c.out_data, c.out_size, p, data, dlen);
    p = to_ring(c.out_data, c.out_size, p, 0, aligned - len);
    uint64_t prev = (uint64_t)write << 32;
    p = to_ring(c.out_data, c.out_size, p, &prev, 8);
    mb();
    c.out_hdr[0] = p;
    mb();
    // hv_signal_on_write: only when the host may have gone to sleep on an empty ring
    if (!c.out_hdr[2] && c.out_hdr[1] == write) vmbus_signal(c);
    return true;
}

bool vmbus_send_buffer(VmbusChannel& c, const void* data, uint32_t dlen, uint32_t addr, uint32_t len, uint64_t tid)
{
    uint32_t off = addr & 4095, n = (off + len + 4095) >> 12;
    if (n > 17) return false;
    uint8_t e[16 + 8 * 17];
    put32(e, 0);                                // reserved
    put32(e + 4, 1);                            // one range
    put32(e + 8, len);
    put32(e + 12, off);
    for (uint32_t i = 0; i < n; i++) put64(e + 16 + 8 * i, (addr >> 12) + i);
    return vmbus_send(c, VM_PKT_DATA_USING_GPA_DIRECT, VMBUS_COMPLETION_REQUESTED, tid, e, 16 + 8 * n, data, dlen);
}

bool vmbus_send_pagebuf(VmbusChannel& c, const void* data, uint32_t dlen, uint32_t addr, uint32_t len, uint64_t tid)
{
    uint8_t e[8 + 16 * 3];
    uint32_t k = 0;
    while (len && k < 3) {
        uint32_t off = addr & 4095, piece = 4096 - off < len ? 4096 - off : len;
        put32(e + 8 + 16 * k, piece);
        put32(e + 12 + 16 * k, off);
        put64(e + 16 + 16 * k, addr >> 12);
        addr += piece; len -= piece; k++;
    }
    if (len) return false;
    put32(e, 0);
    put32(e + 4, k);
    return vmbus_send(c, VM_PKT_DATA_USING_GPA_DIRECT, VMBUS_COMPLETION_REQUESTED, tid, e, 8 + 16 * k, data, dlen);
}

bool vmbus_pending(VmbusChannel& c) { return c.open && c.in_hdr[0] != c.in_hdr[1]; }

uint32_t vmbus_recv(VmbusChannel& c, void* buf, uint32_t room)
{
    if (!c.open) return 0;
    uint32_t write = c.in_hdr[0], read = c.in_hdr[1], size = c.in_size;
    if (write == read || write >= size || read >= size) return 0;
    mb();
    uint32_t avail = write > read ? write - read : size - read + write;
    VmPacketDesc d;
    from_ring(c.in_data, size, read, &d, 16);
    uint32_t len = (uint32_t)d.len8 << 3;
    if (len < 16 || len + 8 > avail) {          // broken: drop what's there
        kprintf("vmbus: relid %u: bad packet (%u bytes, %u in the ring)\n", c.relid, len, avail);
        c.in_hdr[1] = write;
        return 0;
    }
    from_ring(c.in_data, size, read, buf, len < room ? len : room);
    uint32_t nread = (read + len + 8) % size;
    mb();
    c.in_hdr[1] = nread;
    mb();
    // The host waits for room (pending_send_sz): tell it once there is (hv_pkt_iter_close).
    uint32_t pend = c.in_hdr[3];
    if (pend) {
        uint32_t w = c.in_hdr[0];
        uint32_t before = avail_write(size, read, w), now = avail_write(size, nread, w);
        if (before <= pend && now > pend) vmbus_signal(c);
    }
    return len;
}

void vmbus_unload()
{
    if (!up) return;
    uint8_t m[240];
    memset(m, 0, 8);
    put32(m, CH_UNLOAD);
    if (!post(m, 8)) wait_msg(CH_UNLOAD_RESPONSE, 200, m, 0, 0);
    up = false;
}

extern "C" void vmbus_shutdown(void) { vmbus_unload(); }
