// xHCI USB keyboards, mice and mass storage (polled).
//
// Mass storage: bulk-only transport sticks and card readers (SCSI READ(10)
// / WRITE(10), 512-byte blocks), for C: on the USB stick vmdos booted from
// (disk.c, through usb_msd_*).
//
// Takes every xHCI controller from the BIOS, resets it, enumerates keyboards and
// mice on the root ports and behind hubs (hubs in hubs too, and ones
// plugged in later), and turns keyboards' 8-byte boot reports into the same
// PS/2 set-1 scancodes the rest of the kernel already handles. A USB 3 hub
// is two: its USB 2 half (keyboards, mice, USB 2 sticks) and its
// SuperSpeed half (USB 3 sticks and drives). Gamepads and joysticks (any
// HID one with X/Y or a hat switch; the DragonRise SNES clones by their
// own layout) become the game port's joysticks (joy.c).
// From baremetaldoom (same author); here the scancodes go to the DOS guest's
// virtual keyboard controller.
#include "hw.hpp"
#include "pci.hpp"
#include "usb.hpp"

static void kbd_push(uint8_t b) { vkbd_real_scancode(b); }
static void mouse_push(int dx, int dy, int buttons, int wheel) { (void)wheel; mouse_input(dx, dy, buttons); }

// Delays count port 80h reads, ~1 us each on most PCs but several times
// faster on some (AMD chipsets): usb_init measures them against the PIT
// (channel 0, reloaded once a millisecond) and scales.
static uint32_t io_per_ms = 1000;
static void io_delay(int us) { for (uint32_t n = (uint32_t)us * io_per_ms / 1000; n; n--) inb(0x80); }
static void delay_ms(int ms) { while (ms--) io_delay(1000); }
static uint16_t pit_count() { outb(0x43, 0x00); uint16_t c = inb(0x40); return c | (uint16_t)(inb(0x40) << 8); }
static void calibrate_delay() {
    uint16_t prev = pit_count();
    int ms = -1;
    uint32_t n = 0;
    while (ms < 20 && n < 100000000u) {                 // 20 whole milliseconds, counted from a reload
        uint16_t c = pit_count();
        if (c > prev) ms++;
        prev = c;
        if (ms >= 0) { inb(0x80); n++; }
    }
    if (ms == 20 && n / 20 >= 100) io_per_ms = n / 20;
}
#define barrier() __asm__ volatile("" ::: "memory")

// ---------------------------------------------------------------- memory
// All controller-visible structures come from one zeroed, identity-mapped pool.
// When it runs out (several controllers, each with scratchpad pages), more
// comes from the kernel heap, which is identity-mapped too.
static uint8_t pool0[2 << 20] __attribute__((aligned(4096)));
static uint8_t* pool = pool0;
static size_t pool_size = sizeof(pool0), pool_used;
static void* dma_alloc(size_t size, size_t align) {
    uintptr_t at = ((uintptr_t)pool + pool_used + align - 1) & ~(uintptr_t)(align - 1);
    if (at + size > (uintptr_t)pool + pool_size) {
        size_t chunk = size + align > (1u << 20) ? size + align : 1u << 20;
        uint8_t* more = (uint8_t*)phys_try_alloc((uint32_t)chunk);
        if (!more) return nullptr;
        pool = more; pool_size = chunk; pool_used = 0;
        at = ((uintptr_t)pool + align - 1) & ~(uintptr_t)(align - 1);
    }
    pool_used = at + size - (uintptr_t)pool;
    void* p = (void*)at;
    memset(p, 0, size);
    return p;
}
static inline uint64_t phys(const volatile void* p) { return (uint64_t)(uintptr_t)p; }

// ---------------------------------------------------------------- registers
static inline uint32_t rd(volatile uint8_t* b, uint32_t o) { return *(volatile uint32_t*)(b + o); }
static inline void wr(volatile uint8_t* b, uint32_t o, uint32_t v) { *(volatile uint32_t*)(b + o) = v; }
static inline void wr64(volatile uint8_t* b, uint32_t o, uint64_t v) { wr(b, o, (uint32_t)v); wr(b, o + 4, (uint32_t)(v >> 32)); }

// Bits that must be written back unchanged when touching PORTSC; everything
// else (the write-1-to-clear change bits, PED) is written as 0.
static const uint32_t PORT_KEEP = (1u << 0) | (1u << 3) | (0xFu << 10) | (1u << 30) |
                                  (0xFu << 5) | (1u << 9) | (3u << 14) | (7u << 25);
enum {
    PORT_CCS = 1 << 0, PORT_PED = 1 << 1, PORT_PR = 1 << 4, PORT_PP = 1 << 9,
    PORT_CSC = 1 << 17, PORT_PRC = 1 << 21, PORT_CHANGES = 0x7F << 17,
};

// ---------------------------------------------------------------- rings
struct Trb { uint32_t d0, d1, d2, d3; };
enum {
    TRB_NORMAL = 1, TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4, TRB_LINK = 6,
    TRB_ENABLE_SLOT = 9, TRB_ADDRESS_DEVICE = 11, TRB_CONFIGURE_EP = 12, TRB_EVALUATE_CTX = 13,
    TRB_TRANSFER_EVENT = 32, TRB_CMD_COMPLETION = 33, TRB_PORT_STATUS = 34,
};
#define RING_TRBS 64

struct Ring {
    volatile Trb* trb;
    int enq;
    uint32_t cycle;
};

static bool ring_init(Ring& r) {
    r.trb = (volatile Trb*)dma_alloc(RING_TRBS * sizeof(Trb), 64);
    if (!r.trb) return false;
    r.enq = 0;
    r.cycle = 1;
    volatile Trb& link = r.trb[RING_TRBS - 1];     // last TRB links back, toggling cycle
    link.d0 = (uint32_t)phys(r.trb);
    link.d1 = (uint32_t)(phys(r.trb) >> 32);
    link.d3 = TRB_LINK << 10 | (1 << 1);
    return true;
}

/* A ring in memory that's already allocated (reused across plug-ins). */
static void ring_attach(Ring& r, volatile Trb* mem) {
    r.trb = mem;
    memset((void*)mem, 0, RING_TRBS * sizeof(Trb));
    r.enq = 0;
    r.cycle = 1;
    volatile Trb& link = r.trb[RING_TRBS - 1];
    link.d0 = (uint32_t)phys(r.trb);
    link.d1 = (uint32_t)(phys(r.trb) >> 32);
    link.d3 = TRB_LINK << 10 | (1 << 1);
}

static uint64_t ring_push(Ring& r, uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3) {
    volatile Trb& t = r.trb[r.enq];
    uint64_t addr = phys(&t);
    t.d0 = d0; t.d1 = d1; t.d2 = d2;
    barrier();
    t.d3 = (d3 & ~1u) | r.cycle;
    if (++r.enq == RING_TRBS - 1) {
        volatile Trb& link = r.trb[RING_TRBS - 1];
        link.d3 = (link.d3 & ~1u) | r.cycle;       // hand the link TRB over too
        r.enq = 0;
        r.cycle ^= 1;
    }
    return addr;
}

#define EVT_TRBS 256

// One xHCI controller. A PC can have several (the chipset's plus add-in
// cards, or separate USB 2/USB 3 controllers on some boards); each gets its
// own registers, command and event rings and device slots. H is the one
// being worked on: set it before touching a controller or its devices.
enum { HC_XHCI = 0, HC_EHCI = 1, HC_UHCI = 2, HC_OHCI = 3 };   // EHCI / UHCI / OHCI: usb2.inc
struct Hc {
    int type;
    PciDevice pci;                         // where it is (LSUSB)
    uint16_t vendor, device;
    // EHCI / UHCI
    uint16_t io;                           // UHCI I/O ports
    volatile uint8_t* eop;                 // EHCI operational registers
    volatile uint8_t* oreg;                // OHCI registers (frames: its HCCA; qh_int: periodic head ED,
                                           // qh_ctl / xqh: control / bulk EDs; tds: general TDs)
    volatile uint32_t *frames, *qh_int, *qh_ctl, *aqh, *xqh, *tds;
    volatile uint8_t* sbuf;                // setup packets
    uint8_t addr_used[128];
    int ncomp;                             // EHCI: companion controllers
    // xHCI
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t* db;
    Ring cmd_ring;
    volatile Trb* evt;
    int evt_deq;
    uint32_t evt_cycle;
    size_t csz;                            // context size: 32 or 64 bytes
    volatile uint64_t* dcbaa;
    int max_slots, num_ports;
    bool port_dirty[256];
};
#define MAX_HC 8
static Hc hcs[MAX_HC];
static int num_hc;
static Hc* H;

static inline uint32_t portsc(int port) { return rd(H->op, 0x400 + 0x10 * (port - 1)); }
static inline void set_portsc(int port, uint32_t v) { wr(H->op, 0x400 + 0x10 * (port - 1), v); }

static bool next_event(Trb* out) {
    volatile Trb& e = H->evt[H->evt_deq];
    if ((e.d3 & 1) != H->evt_cycle) return false;
    barrier();
    out->d0 = e.d0; out->d1 = e.d1; out->d2 = e.d2; out->d3 = e.d3;
    if (++H->evt_deq == EVT_TRBS) { H->evt_deq = 0; H->evt_cycle ^= 1; }
    wr64(H->rt, 0x38, phys(&H->evt[H->evt_deq]) | (1 << 3));  // ERDP, clear busy
    return true;
}

// ---------------------------------------------------------------- devices

// Everything plugged in that we drive: keyboards, mice and the hubs they
// hang off. Where a device is: the root port its tree starts at, the route
// string down through the hubs (4 bits a tier), and, for a low/full-speed
// device behind a high-speed hub, that hub's slot and port (its transaction
// translator).
enum Kind { KBD, MOUSE, HUB, MSD, JOY, AUDIO, NET };

// Where a gamepad's report has its X, Y, hat switch and buttons, from its
// HID report descriptor. bit < 0: not there.
struct HidField { int bit, size; int32_t lmin, lmax; };
struct PadLayout {
    int report_id;                         // 0: reports have no ID byte
    HidField x, y, hat;
    int btn_bit, btn_count;                // buttons 1..n, one bit each from btn_bit
    bool dragonrise;                       // 0079:0011 SNES clones: fixed layout
};
struct Keyboard {
    bool active;
    Hc* hc;                                // the controller it's on
    Kind kind;
    bool mouse;                            // kind == MOUSE (kept for the report code)
    int port;                              // root port
    uint32_t route;
    int depth;                             // hub tier below the root (0: on a root port)
    int parent;                            // index of the hub it's on, -1: a root port
    int hub_port;                          // its port on that hub
    int tt_slot, tt_port;                  // transaction translator, 0: none
    int slot, speed, dci, mps;
    volatile uint8_t* out_ctx;
    volatile uint8_t* in_ctx;
    Ring ep0, intr;
    volatile uint8_t* reports;             // one 8-byte buffer per ring slot
    uint8_t prev[8];
    int rsize;                             // bytes a report buffer (8; a gamepad's endpoint size)
    // gamepads
    int pad;                               // joystick A (0) or B (1)
    PadLayout lay;
    // hubs
    int nports, ttt;
    uint32_t dirty;                        // ports with a change waiting (bit n = port n)
    uint32_t given_up;                     // ports whose device failed setup too often (bit n = port n)
    uint8_t port_fails[32];                // failed setups in a row, by port
    uint32_t power_good_ms;
    char where[24];                        // "port 3" / "port 3.2" for messages
    // mass storage
    int dci_out, ep_in, ep_out, iface;
    int subclass;                          // 2, 5 (ATAPI), 4 (UFI): commands padded to 12 bytes
    Ring bout;                             // bulk OUT (bulk IN uses intr)
    uint32_t tag, bsize;
    int timeouts;                          // mass storage: bulk transfers that never completed
    uint64_t blocks;
    char model[28];
    // EHCI / UHCI: the bus address (k.slot once set), the default pipe's
    // packet size, bulk toggles, the interrupt endpoint's queue head and TD
    uint8_t addr;
    int mps0, ilen;
    uint8_t itog, tog_in, tog_out;
    bool linked;
    volatile uint32_t *iqh, *itd;
};
#define MAX_KBD 32
static Keyboard kbds[MAX_KBD];

// Every device seen on a port (LSUSB), driven or not: what its descriptors
// said, and the kbds entry it was set up in. Dropped when its port goes
// empty (or its hub does); a device set up again on the same port replaces it.
#define MAX_SEEN 48
static struct Seen {
    bool used, have_ids, have_iface;
    Hc* hc;
    char where[24];
    int speed, ki;
    uint16_t vid, pid, bcd_usb;
    uint8_t dclass, dsub, dproto, iclass, isub, iproto, nconf, nifaces;
} seen[MAX_SEEN];
static Seen* seen_cur;                          // the device being set up

static bool where_under(const char* w, const char* top) {   // w is top, or behind it ("port 1.4" under "port 1")
    size_t n = strlen(top);
    return !strncmp(w, top, n) && (w[n] == 0 || w[n] == '.');
}
static void seen_drop(Hc* hc, const char* where) {
    for (auto& e : seen) if (e.used && e.hc == hc && where_under(e.where, where)) e.used = false;
}
static Seen* seen_begin(Hc* hc, const char* where, int speed, int ki) {
    seen_drop(hc, where);
    for (auto& e : seen)
        if (!e.used) {
            memset(&e, 0, sizeof e);
            e.used = true; e.hc = hc; e.speed = speed; e.ki = ki;
            snprintf(e.where, sizeof e.where, "%s", where);
            return &e;
        }
    return nullptr;
}
/* Each table entry's controller-visible memory, allocated the first time
 * the entry is used and reused by whatever is plugged in next: the pool is
 * never freed, so allocating per plug-in ran it dry after a few hundred. */
static struct {
    volatile uint8_t *out_ctx, *in_ctx, *reports;
    volatile Trb *ep0, *intr, *bout;
    volatile uint32_t *iqh, *itd;
} devmem[MAX_KBD];

// EHCI / UHCI (usb2.inc).
static bool hc2_control(Keyboard& k, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, volatile void* buf);
static bool hc2_intr(Keyboard& k, int ep_addr, int ep_mps, int ep_interval, int ilen = 0);
static int hc2_bulk(Keyboard& k, bool in, volatile void* buf, uint32_t len, uint32_t* got);
static void hc2_clear_halt(Keyboard& k, bool in);
static bool hc2_address(Keyboard& k);
static void hc2_release(Keyboard& k);
static void hc2_poll();
static void hc2_root_poll(bool first);
static const char* hc2_name();
static uint8_t alloc_addr();

static inline volatile uint32_t* ctx(volatile uint8_t* base, int idx) {
    return (volatile uint32_t*)(base + idx * H->csz);
}

// ---------------------------------------------------------------- key reports
// HID usage -> PS/2 set-1 scancode (0x100 flag = E0-prefixed): letters,
// digits, punctuation, Enter/Esc/Backspace/Tab/Space, F1-F12, arrows, the
// editing keys and the keypad.
static const struct { uint8_t usage; uint16_t code; } keymap[] = {
    {0x04, 0x1E}, {0x05, 0x30}, {0x06, 0x2E}, {0x07, 0x20}, {0x08, 0x12}, {0x09, 0x21},
    {0x0A, 0x22}, {0x0B, 0x23}, {0x0C, 0x17}, {0x0D, 0x24}, {0x0E, 0x25}, {0x0F, 0x26},
    {0x10, 0x32}, {0x11, 0x31}, {0x12, 0x18}, {0x13, 0x19}, {0x14, 0x10}, {0x15, 0x13},
    {0x16, 0x1F}, {0x17, 0x14}, {0x18, 0x16}, {0x19, 0x2F}, {0x1A, 0x11}, {0x1B, 0x2D},
    {0x1C, 0x15}, {0x1D, 0x2C}, {0x1E, 0x2}, {0x1F, 0x3}, {0x20, 0x4}, {0x21, 0x5},
    {0x22, 0x6}, {0x23, 0x7}, {0x24, 0x8}, {0x25, 0x9}, {0x26, 0xA}, {0x27, 0xB},
    {0x28, 0x1C}, {0x29, 0x1}, {0x2A, 0xE}, {0x2B, 0xF}, {0x2C, 0x39}, {0x2D, 0xC},
    {0x2E, 0xD}, {0x2F, 0x1A}, {0x30, 0x1B}, {0x31, 0x2B}, {0x33, 0x27}, {0x34, 0x28},
    {0x35, 0x29}, {0x36, 0x33}, {0x37, 0x34}, {0x38, 0x35}, {0x3A, 0x3B}, {0x3B, 0x3C},
    {0x3C, 0x3D}, {0x3D, 0x3E}, {0x3E, 0x3F}, {0x3F, 0x40}, {0x40, 0x41}, {0x41, 0x42},
    {0x42, 0x43}, {0x43, 0x44}, {0x44, 0x57}, {0x45, 0x58}, {0x4F, 0x14D}, {0x50, 0x14B},
    {0x51, 0x150}, {0x52, 0x148}, {0x58, 0x11C},
    // Caps Lock; Insert, Home, Page Up, Delete, End, Page Down
    {0x39, 0x3A}, {0x49, 0x152}, {0x4A, 0x147}, {0x4B, 0x149}, {0x4C, 0x153}, {0x4D, 0x14F}, {0x4E, 0x151},
    // keypad: Num Lock, / * - +, 1-9, 0, .
    {0x53, 0x45}, {0x54, 0x135}, {0x55, 0x37}, {0x56, 0x4A}, {0x57, 0x4E},
    {0x59, 0x4F}, {0x5A, 0x50}, {0x5B, 0x51}, {0x5C, 0x4B}, {0x5D, 0x4C}, {0x5E, 0x4D},
    {0x5F, 0x47}, {0x60, 0x48}, {0x61, 0x49}, {0x62, 0x52}, {0x63, 0x53},
    {0x64, 0x56},                                       // the key left of Z on ISO boards
};

static void emit(uint16_t code, bool down) {
    if (code & 0x100) kbd_push(0xE0);
    kbd_push((uint8_t)(code & 0x7F) | (down ? 0 : 0x80));
}
static void emit_usage(uint8_t usage, bool down) {
    for (auto& k : keymap) if (k.usage == usage) { emit(k.code, down); return; }
}

static void handle_report(Keyboard& k, const volatile uint8_t* r) {
    if (k.mouse) {                                         // buttons, dx, dy (boot protocol)
        // Most mice add the wheel as a 4th byte even in the boot protocol;
        // a 3-byte report leaves it 0 (queue_report clears the buffer).
        mouse_push((int8_t)r[1], (int8_t)r[2], r[0] & 7, (int8_t)r[3]);
        return;
    }
    uint8_t cur[8];
    for (int i = 0; i < 8; i++) cur[i] = r[i];
    if (cur[2] == 1) return;                               // rollover error: ignore
    uint8_t mchg = cur[0] ^ k.prev[0];
    if (mchg & 0x02) emit(0x2A, cur[0] & 0x02);            // left shift
    if (mchg & 0x20) emit(0x36, cur[0] & 0x20);            // right shift
    if (mchg & 0x01) emit(0x1D, cur[0] & 0x01);            // left ctrl
    if (mchg & 0x10) emit(0x11D, cur[0] & 0x10);           // right ctrl
    if (mchg & 0x04) emit(0x38, cur[0] & 0x04);            // left alt
    if (mchg & 0x40) emit(0x138, cur[0] & 0x40);           // right alt
    for (int i = 2; i < 8; i++) {                          // releases
        uint8_t u = k.prev[i];
        if (!u) continue;
        bool still = false;
        for (int j = 2; j < 8; j++) still |= cur[j] == u;
        if (!still) emit_usage(u, false);
    }
    for (int i = 2; i < 8; i++) {                          // presses
        uint8_t u = cur[i];
        if (!u) continue;
        bool was = false;
        for (int j = 2; j < 8; j++) was |= k.prev[j] == u;
        if (!was) emit_usage(u, true);
    }
    memcpy(k.prev, cur, 8);
}

#define REPORT_MAX 64                       // a report buffer at most (a full-speed interrupt packet)

// ---------------------------------------------------------------- gamepads
static int32_t hid_bits(const volatile uint8_t* r, int len, int bit, int size, bool sign) {
    uint32_t v = 0;
    for (int i = 0; i < size; i++) {
        int b = bit + i;
        if ((b >> 3) < len && (r[b >> 3] >> (b & 7)) & 1) v |= 1u << i;
    }
    if (sign && size < 32 && (v >> (size - 1)) & 1) v |= ~0u << size;
    return (int32_t)v;
}
// A field's value as 0..255.
static int hid_axis(const volatile uint8_t* r, int len, const HidField& f) {
    if (f.bit < 0 || f.lmax <= f.lmin) return 128;
    int32_t v = hid_bits(r, len, f.bit, f.size, f.lmin < 0);
    if (v < f.lmin) v = f.lmin;
    if (v > f.lmax) v = f.lmax;
    uint32_t off = (uint32_t)(v - f.lmin), span = (uint32_t)(f.lmax - f.lmin);
    while (span > 0xFFFFFF) { span >>= 8; off >>= 8; }       // 32-bit math (no 64-bit division here)
    return span ? (int)(off * 255 / span) : 128;
}

// The parts of a HID report descriptor a gamepad needs: the first input
// X, Y, hat switch and buttons (and their report ID). True if it is a
// joystick or gamepad with something to steer by.
static bool hid_parse(const volatile uint8_t* d, int n, PadLayout& L) {
    L.report_id = 0; L.x.bit = L.y.bit = L.hat.bit = -1; L.btn_bit = -1; L.btn_count = 0;
    int page = 0, size = 0, count = 0, rid = 0, ids_used = 0;
    int32_t lmin = 0, lmax = 0;
    int usages[16], nu = 0, umin = 0, umax = -1;
    bool app = false;
    int bitpos[16] = { 0 };                 // the next bit in each report ID's report
    for (int i = 0; i < n; ) {
        uint8_t h = d[i];
        if (h == 0xFE) { if (i + 2 >= n) break; i += 3 + d[i + 1]; continue; }   // long item
        int sz = (h & 3) == 3 ? 4 : h & 3;
        if (i + 1 + sz > n) break;
        uint32_t u = 0;
        for (int j = 0; j < sz; j++) u |= (uint32_t)d[i + 1 + j] << (8 * j);
        int32_t sv = sz == 1 ? (int8_t)u : sz == 2 ? (int16_t)u : (int32_t)u;
        int tag = h & 0xFC;
        i += 1 + sz;
        switch (tag) {
        case 0x04: page = (int)u; break;                                         // usage page
        case 0x14: lmin = sv; break;
        case 0x24: lmax = sv; break;
        case 0x74: size = (int)u; break;
        case 0x94: count = (int)u; break;
        case 0x84: rid = (int)u & 15; ids_used = 1; break;                       // report ID
        case 0x08: if (nu < 16) usages[nu++] = (int)(u & 0xFFFF) | ((u >> 16) ? (int)(u >> 16) << 16 : page << 16); break;
        case 0x18: umin = (int)u; break;
        case 0x28: umax = (int)u; break;
        case 0xA0:                                                               // collection
            if (nu && (usages[0] == (1 << 16 | 4) || usages[0] == (1 << 16 | 5))) app = true;   // joystick, gamepad
            nu = 0; umin = 0; umax = -1;
            break;
        case 0x80: {                                                             // input
            int32_t mx = lmax;
            if (mx < lmin && size < 32) mx = (int32_t)((uint32_t)lmax & ((1u << size) - 1));   // 0..255 written as -1
            for (int k = 0; k < count; k++) {
                int bit = bitpos[rid] + k * size;
                if (u & 1) continue;                                             // constant: padding
                int usage = k < nu ? usages[k] : nu ? usages[nu - 1] : umax >= umin ? (page << 16 | (umin + k)) : 0;
                if (!(u & 2)) usage = 0;                                         // an array: not ours
                HidField f = { bit, size, lmin, mx };
                bool take = !L.report_id || L.report_id == rid;
                if (usage == (1 << 16 | 0x30) && L.x.bit < 0 && take) { L.x = f; L.report_id = rid; }
                else if (usage == (1 << 16 | 0x31) && L.y.bit < 0 && take) { L.y = f; L.report_id = rid; }
                else if (usage == (1 << 16 | 0x39) && L.hat.bit < 0 && take) { L.hat = f; L.report_id = rid; }
                else if (page == 9 && size == 1 && take && umax >= umin) {
                    if (L.btn_bit < 0) { L.btn_bit = bit; L.report_id = rid; }
                    if (bit == L.btn_bit + L.btn_count) L.btn_count++;
                }
            }
            bitpos[rid] += size * count;
            nu = 0; umin = 0; umax = -1;
            break; }
        case 0x90: case 0xB0: case 0xC0:                                         // output, feature, end
            if (tag != 0xC0) bitpos[rid] += 0;                                    // (not in input reports)
            nu = 0; umin = 0; umax = -1;
            break;
        }
    }
    if (!ids_used) L.report_id = 0;
    else if (!L.report_id) L.report_id = -1;                                     // IDs, but ours had none?
    return app && ((L.x.bit >= 0 && L.y.bit >= 0) || L.hat.bit >= 0);
}

// The buttons beyond the game port's four (L, R, Select, Start) do nothing,
// unless joykeys=L,R,SELECT,START on the kernel command line makes them
// press keys (set-1 scancodes in hex, 1xx for E0-prefixed ones, 0 for
// none), e.g. joykeys=39,2A,01,1C for Space, Left Shift, Esc, Enter.
static uint16_t padkey[4];
static void pad_keys_option(const char* cmdline) {
    const char* o = cmdline ? strstr(cmdline, "joykeys=") : nullptr;
    if (!o) return;
    o += 8;
    for (int i = 0; i < 4; i++) {
        uint16_t v = 0;
        for (; (*o >= '0' && *o <= '9') || ((*o | 32) >= 'a' && (*o | 32) <= 'f'); o++)
            v = (uint16_t)(v * 16 + (*o <= '9' ? *o - '0' : (*o | 32) - 'a' + 10));
        padkey[i] = v & 0x17F;
        if (*o != ',') break;
        o++;
    }
}
static void pad_keys(Keyboard& k, int keys) {           // keys: bit 0 L, 1 R, 2 Select, 3 Start
    int chg = keys ^ k.prev[0];
    for (int i = 0; i < 4; i++)
        if ((chg >> i & 1) && padkey[i]) emit(padkey[i], keys >> i & 1);
    k.prev[0] = (uint8_t)keys;
}

static void pad_report(Keyboard& k, const volatile uint8_t* r, int len) {
    const PadLayout& L = k.lay;
    int x, y, b = 0, keys = 0;
    if (L.dragonrise) {
        // 01 7F 7F xx yy bb cc 00: D-pad X in byte 3, Y in byte 4; byte 5
        // bits 4-7 X A B Y, byte 6 bits 0-1 L R, 4-5 Select Start.
        if (len < 7) return;
        x = r[3]; y = r[4];
        uint8_t b5 = r[5];
        if (b5 & 0x40) b |= 1;                                    // B: button 1
        if (b5 & 0x20) b |= 2;                                    // A: button 2
        if (b5 & 0x80) b |= 4;                                    // Y: button 3
        if (b5 & 0x10) b |= 8;                                    // X: button 4
        uint8_t b6 = r[6];
        keys = (b6 & 1) | (b6 & 2) | (b6 & 0x10 ? 4 : 0) | (b6 & 0x20 ? 8 : 0);   // L R Select Start
    } else {
        int off = 0;
        if (L.report_id > 0) { if (len < 1 || r[0] != L.report_id) return; off = 1; }
        const volatile uint8_t* p = r + off;
        int n = len - off;
        x = hid_axis(p, n, L.x); y = hid_axis(p, n, L.y);
        if (L.hat.bit >= 0) {                                     // a hat switch steers too
            int32_t h = hid_bits(p, n, L.hat.bit, L.hat.size, false) - L.hat.lmin;
            if (h >= 0 && h < 8) {                                // 0 up, clockwise; else centred
                static const int8_t hx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, hy[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
                if (hx[h]) x = hx[h] < 0 ? 0 : 255;
                if (hy[h]) y = hy[h] < 0 ? 0 : 255;
            }
        }
        for (int i = 0; i < 4 && i < L.btn_count; i++)
            if (hid_bits(p, n, L.btn_bit + i, 1, false)) b |= 1 << i;
        // Most pads: buttons 5, 6 the shoulder ones, 9 Select, 10 Start.
        static const int extra[4] = { 4, 5, 8, 9 };
        for (int i = 0; i < 4; i++)
            if (extra[i] < L.btn_count && hid_bits(p, n, L.btn_bit + extra[i], 1, false)) keys |= 1 << i;
    }
    joy_set(k.pad, 1, x, y, b);
    pad_keys(k, keys);
}

static void queue_report(Keyboard& k) {
    if (k.hc->type != HC_XHCI) return;                  // EHCI / UHCI: armed by usb2.inc
    int idx = k.intr.enq;
    for (int i = 0; i < k.rsize; i++) k.reports[idx * REPORT_MAX + i] = 0;
    uint64_t buf = phys(k.reports + idx * REPORT_MAX);
    ring_push(k.intr, (uint32_t)buf, (uint32_t)(buf >> 32), k.rsize,
              TRB_NORMAL << 10 | (1 << 5) | (1 << 2));     // IOC, ISP
}

static void release_all(Keyboard& k) {
    if (k.kind == JOY) { joy_set(k.pad, 0, 128, 128, 0); pad_keys(k, 0); return; }
    if (k.mouse) { mouse_push(0, 0, 0, 0); return; }
    uint8_t empty[8] = {0};
    handle_report(k, empty);
}

static bool ua_dispatch(int slot, int ep, const Trb& e);   // the audio stream's (below)
static bool un_dispatch(int slot, int ep, const Trb& e);   // USB Ethernet's receive ring (below)
// Events that arrive while we're waiting for something else.
static void dispatch(const Trb& e) {
    int type = (e.d3 >> 10) & 0x3F;
    if (type == TRB_PORT_STATUS) {
        int port = (e.d0 >> 24) & 0xFF;
        if (port >= 1 && port <= H->num_ports) H->port_dirty[port] = true;
        return;
    }
    if (type != TRB_TRANSFER_EVENT) return;
    int slot = e.d3 >> 24, ep = (e.d3 >> 16) & 0x1F, cc = e.d2 >> 24;
    if (ua_dispatch(slot, ep, e)) return;
    if (un_dispatch(slot, ep, e)) return;
    for (auto& k : kbds) {
        if (!k.active || k.hc != H || k.slot != slot || k.dci != ep || k.kind == MSD || k.kind == AUDIO || k.kind == NET) continue;
        uint64_t trb = (uint64_t)e.d1 << 32 | e.d0;
        int idx = (int)((trb - phys(k.intr.trb)) / sizeof(Trb));
        if (idx >= 0 && idx < RING_TRBS && (cc == 1 || cc == 13)) {
            const volatile uint8_t* r = k.reports + idx * REPORT_MAX;
            if (k.kind == HUB) k.dirty |= (uint32_t)(r[0] | r[1] << 8 | r[2] << 16) & ~1u;   // bit 0: the hub itself
            else if (k.kind == JOY) pad_report(k, r, k.rsize - (int)(e.d2 & 0xFFFFFF));
            else handle_report(k, r);
        }
        if (cc == 1 || cc == 13) queue_report(k);
        else {
            if (k.kind != HUB) release_all(k);
            k.active = false;
            printf("USB: %s on %s stopped (code %d)\n", k.kind == HUB ? "hub" : k.kind == JOY ? "gamepad" : k.mouse ? "mouse" : "keyboard", k.where, cc);
        }
        H->db[slot] = k.dci;
    }
}

// Wait for an event of 'type' (and, for transfers, matching slot/endpoint).
static bool wait_event(int type, int slot, int ep, Trb* out, int ms = 500) {
    for (int t = 0; t < ms * 20; t++) {
        Trb e;
        while (next_event(&e)) {
            int et = (e.d3 >> 10) & 0x3F;
            bool match = et == type;
            if (match && type == TRB_TRANSFER_EVENT)
                match = (int)(e.d3 >> 24) == slot && (int)((e.d3 >> 16) & 0x1F) == ep;
            if (match) { *out = e; return true; }
            dispatch(e);
        }
        io_delay(50);
    }
    return false;
}

// A command and its completion (1 = success), -1 if none came. The
// controller may take seconds (Address Device retrying a silent device;
// Linux allows 5), and a command that never finishes is aborted so the
// ring moves on. Completions are matched by TRB, so a late one for an
// earlier command isn't taken for this one's.
static int command(uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3, Trb* ev) {
    uint64_t trb = ring_push(H->cmd_ring, d0, d1, d2, d3);
    H->db[0] = 0;
    Trb e;
    for (;;) {
        if (!wait_event(TRB_CMD_COMPLETION, 0, 0, &e, 5000)) {
            wr(H->op, 0x18, 1u << 2);                   // CRCR.CA: abort it
            for (int i = 0; i < 5000 && (rd(H->op, 0x18) & (1u << 3)); i++) delay_ms(1);
            return -1;
        }
        if ((e.d0 & ~15u) == (uint32_t)trb && e.d1 == (uint32_t)(trb >> 32)) break;
    }
    if (ev) *ev = e;
    return e.d2 >> 24;
}

// A control transfer on endpoint 0. Returns true on success; ctl_cc: its
// completion code (-1: none came).
static int ctl_cc;
static bool control(Keyboard& k, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                    uint16_t len, volatile void* buf) {
    if (H->type != HC_XHCI) return hc2_control(k, type, req, value, index, len, buf);
    bool in = type & 0x80;
    uint32_t trt = len ? (in ? 3 : 2) : 0;
    ring_push(k.ep0, type | req << 8 | (uint32_t)value << 16, index | (uint32_t)len << 16, 8,
              TRB_SETUP << 10 | (1 << 6) | trt << 16);
    if (len) {
        uint64_t p = phys(buf);
        ring_push(k.ep0, (uint32_t)p, (uint32_t)(p >> 32), len, TRB_DATA << 10 | (in ? 1 << 16 : 0));
    }
    ring_push(k.ep0, 0, 0, 0, TRB_STATUS << 10 | (1 << 5) | ((len && in) ? 0 : 1 << 16));
    H->db[k.slot] = 1;
    Trb e;
    if (!wait_event(TRB_TRANSFER_EVENT, k.slot, 1, &e)) { ctl_cc = -1; return false; }
    int cc = e.d2 >> 24;
    ctl_cc = cc;
    return cc == 1 || cc == 13;
}

// ---------------------------------------------------------------- enumeration
// Every device gets a reset before it's addressed, USB 3 ones too (as
// Linux does): a USB 3 link can stay up through the controller reset, with
// the device still in whatever state the firmware left it.
static bool reset_port(int port) {
    uint32_t sc = portsc(port);
    if (!(sc & PORT_CCS)) return false;
    set_portsc(port, (sc & PORT_KEEP) | PORT_PR);
    for (int i = 0; i < 500 && !(portsc(port) & PORT_PRC); i++) delay_ms(1);
    for (int i = 0; i < 100 && (portsc(port) & PORT_PR); i++) delay_ms(1);
    sc = portsc(port);
    set_portsc(port, (sc & PORT_KEEP) | (sc & PORT_CHANGES));   // ack change bits
    delay_ms(100);                                              // reset recovery – some sticks need more
    return portsc(port) & PORT_PED;
}

static const char* speed_name(int s) { return s == 2 ? "low" : s == 1 ? "full" : s == 3 ? "high" : "super"; }

// The slot context's words 0-2: speed, route, root port, transaction
// translator and, for a hub, its port count. `entries` is the last
// endpoint context in use.
static void fill_slot(const Keyboard& k, int entries) {
    volatile uint32_t* sl = ctx(k.in_ctx, 1);
    sl[0] = (k.route & 0xFFFFF) | (uint32_t)k.speed << 20 | (uint32_t)entries << 27 |
            (k.kind == HUB ? 1u << 26 : 0);
    sl[1] = (uint32_t)k.port << 16 | (k.kind == HUB ? (uint32_t)k.nports << 24 : 0);
    sl[2] = (uint32_t)k.tt_slot | (uint32_t)k.tt_port << 8 | (k.kind == HUB ? (uint32_t)k.ttt << 16 : 0);
    sl[3] = 0;
}

// Interrupt IN endpoint `ep_addr` as the device's report/status ring.
static bool configure_intr(Keyboard& k, int ep_addr, int ep_mps, int ep_interval) {
    if (H->type != HC_XHCI) return hc2_intr(k, ep_addr, ep_mps, ep_interval);
    k.dci = (ep_addr & 0xF) * 2 + 1;
    k.mps = ep_mps;
    int interval;
    if (k.speed == 1 || k.speed == 2) {                 // bInterval in ms -> 2^n x 125 us
        interval = 3;
        while (interval < 10 && (1 << (interval + 1)) <= ep_interval * 8) interval++;
    } else {
        interval = ep_interval ? ep_interval - 1 : 0;
        if (interval > 15) interval = 15;
    }
    memset((void*)k.in_ctx, 0, 33 * H->csz);
    ctx(k.in_ctx, 0)[1] = 1 | 1u << k.dci;
    fill_slot(k, k.dci);
    volatile uint32_t* ep = ctx(k.in_ctx, 1 + k.dci);
    ep[0] = (uint32_t)interval << 16;
    ep[1] = 3 << 1 | 7 << 3 | (uint32_t)ep_mps << 16;  // CErr 3, interrupt IN
    uint64_t ri = phys(k.intr.trb) | 1;
    ep[2] = (uint32_t)ri; ep[3] = (uint32_t)(ri >> 32);
    ep[4] = 8 | (uint32_t)ep_mps << 16;
    uint64_t ic = phys(k.in_ctx);
    int cc = command((uint32_t)ic, (uint32_t)(ic >> 32), 0, TRB_CONFIGURE_EP << 10 | (uint32_t)k.slot << 24, nullptr);
    if (cc != 1) { printf("USB: %s: configure failed (%d)\n", k.where, cc); return false; }
    return true;
}

static void hub_port_change(int h, int port);
static void forget(int i);

// A device that's just been reset and enabled, wherever it is: give it a
// slot and an address, and set it up as a keyboard, mouse or hub.
static bool setup_slot(Keyboard& k, int ki);
static bool addr_failed;                               // the last setup_slot's Address Device failed
static int addr_cc;                                    // and its completion code (-1: no answer at all)
static bool msd_stuck;                                 // the last setup_slot's mass-storage commands never completed
static void release_slot(int slot) {
    if (H->type != HC_XHCI) return;
    H->dcbaa[slot] = 0;
    command(0, 0, 0, 10 << 10 | (uint32_t)slot << 24, nullptr);   // Disable Slot
}

static void setup_device(int root_port, int parent, int hub_port, int speed) {
    for (auto& k : kbds)                               // already there?
        if (k.active && k.hc == H && k.port == root_port && k.parent == parent && (parent < 0 || k.hub_port == hub_port)) return;
    int ki = -1;
    for (int i = 0; i < MAX_KBD; i++) if (!kbds[i].active) { ki = i; break; }
    if (ki < 0) { printf("USB: too many devices\n"); return; }
    Keyboard& k = kbds[ki];
    memset((void*)&k, 0, sizeof(k));
    k.hc = H;
    k.port = root_port;
    k.parent = parent;
    k.hub_port = hub_port;
    k.speed = speed;
    if (parent < 0) {
        snprintf(k.where, sizeof k.where, "port %d", root_port);
    } else {
        const Keyboard& h = kbds[parent];
        k.depth = h.depth + 1;
        k.route = h.route | (uint32_t)(hub_port > 15 ? 15 : hub_port) << (4 * h.depth);
        snprintf(k.where, sizeof k.where, "%s.%d", h.where, hub_port);
        if (speed == 1 || speed == 2) {                 // low/full speed: through a translator
            if (h.speed == 3) { k.tt_slot = h.slot; k.tt_port = hub_port; }
            else { k.tt_slot = h.tt_slot; k.tt_port = h.tt_port; }
        }
    }

    seen_cur = seen_begin(H, k.where, speed, ki);

    if (H->type != HC_XHCI) {                          // EHCI / UHCI: a bus address
        k.slot = alloc_addr();
        if (!k.slot) { printf("USB: %s: no free USB address\n", k.where); return; }
        if (!setup_slot(k, ki)) hc2_release(k);
        return;
    }
    Trb ev;
    if (command(0, 0, 0, TRB_ENABLE_SLOT << 10, &ev) != 1) { printf("USB: %s: no slot\n", k.where); return; }
    k.slot = ev.d3 >> 24;
    if (k.slot < 1 || k.slot > H->max_slots) return;
    delay_ms(10);   // let the controller finish enabling the slot
    // Anything that doesn't end up driven gives its slot back: kept, a USB
    // stick plugged in a few dozen times used them all up.
    if (!setup_slot(k, ki)) release_slot(k.slot);
}


// ---------------------------------------------------------------- mass storage
// Bulk-only transport: a 31-byte command block wrapper (CBW) on bulk OUT, the
// data on bulk IN or OUT, a 13-byte status wrapper (CSW) on bulk IN.
static volatile uint8_t* msd_buf;                       // 64 KiB, 64 KiB aligned (a TRB can't cross 64 KiB)
static volatile uint8_t* msd_cbw;
// USB disks, by the index disk.c and hd.c know them by. A disk plugged
// in takes the slot of one that went (the same model and size first), so
// it shows up as that entry again instead of a new one beside a dead one;
// new slots are only made when none is free. -1: unplugged.
#define MSD_MAX 4
static int msd_dev[MSD_MAX], n_msd;
static struct { char model[28]; uint64_t blocks; uint32_t gen; } msd_last[MSD_MAX];   // what was in each slot; gen: plug count
static bool msd_is(int i) {                                 // slot i still holds a plugged-in disk
    if (i < 0 || i >= n_msd || msd_dev[i] < 0) return false;
    Keyboard& k = kbds[msd_dev[i]];
    return k.active && k.kind == MSD && k.bsize == 512;
}
static volatile int usb_busy;                          // a disk transfer is running: usb_poll keeps off
static int in_poll;                                    // usb_poll running (the stop screen polls too)

static bool configure_bulk(Keyboard& k) {
    if (H->type != HC_XHCI) { k.tog_in = k.tog_out = 0; return true; }
    k.dci = (k.ep_in & 0xF) * 2 + 1;
    k.dci_out = (k.ep_out & 0xF) * 2;
    int last = k.dci > k.dci_out ? k.dci : k.dci_out;
    memset((void*)k.in_ctx, 0, 33 * H->csz);
    ctx(k.in_ctx, 0)[1] = 1 | 1u << k.dci | 1u << k.dci_out;
    fill_slot(k, last);
    for (int w = 0; w < 2; w++) {
        int dci = w ? k.dci : k.dci_out;
        Ring& r = w ? k.intr : k.bout;
        volatile uint32_t* ep = ctx(k.in_ctx, 1 + dci);
        ep[0] = 0;
        ep[1] = 3 << 1 | (uint32_t)(w ? 6 : 2) << 3 | (uint32_t)k.mps << 16;   // CErr 3, bulk IN / OUT
        uint64_t ri = phys(r.trb) | 1;
        ep[2] = (uint32_t)ri; ep[3] = (uint32_t)(ri >> 32);
        ep[4] = 1024;                                   // average TRB length
    }
    uint64_t ic = phys(k.in_ctx);
    int cc = command((uint32_t)ic, (uint32_t)(ic >> 32), 0, TRB_CONFIGURE_EP << 10 | (uint32_t)k.slot << 24, nullptr);
    if (cc != 1) { printf("USB: %s: configure failed (%d)\n", k.where, cc); return false; }
    return true;
}

// One bulk transfer; the completion code (1 ok, 13 short, 6 stall, -1 timeout).
static int bulk(Keyboard& k, bool in, volatile void* buf, uint32_t len, uint32_t* got) {
    if (H->type != HC_XHCI) return hc2_bulk(k, in, buf, len, got);
    Ring& r = in ? k.intr : k.bout;
    int dci = in ? k.dci : k.dci_out;
    uint64_t p = phys(buf);
    uint64_t trb = ring_push(r, (uint32_t)p, (uint32_t)(p >> 32), len, TRB_NORMAL << 10 | (1 << 5) | (in ? 1 << 2 : 0));
    H->db[k.slot] = dci;
    Trb e;
    // The event for this TRB: some controllers (AMD, Renesas) send a second
    // one for a short packet; taking that as the next transfer's would put
    // every transfer after it out of step.
    for (int tries = 0; ; tries++) {
        if (tries == 8 || !wait_event(TRB_TRANSFER_EVENT, k.slot, dci, &e, 5000)) {
            printf("USB: %s: bulk %s of %u bytes: no completion\n", k.where, in ? "IN" : "OUT", len);
            return -1;
        }
        if ((e.d0 & ~15u) == (uint32_t)trb) break;
        dbg(1, "USB: %s: stray event for TRB %x (code %u)\n", k.where, e.d0, e.d2 >> 24);
    }
    int cc = e.d2 >> 24;
    if (got) *got = len - (e.d2 & 0xFFFFFF);
    return cc;
}

// A halted bulk endpoint: Reset Endpoint, move its ring on, CLEAR_FEATURE(HALT).
static void clear_halt(Keyboard& k, bool in) {
    if (H->type != HC_XHCI) { hc2_clear_halt(k, in); return; }
    Ring& r = in ? k.intr : k.bout;
    int dci = in ? k.dci : k.dci_out;
    command(0, 0, 0, 14u << 10 | (uint32_t)dci << 16 | (uint32_t)k.slot << 24, nullptr);       // Reset Endpoint
    uint64_t dq = phys(&r.trb[r.enq]) | r.cycle;
    command((uint32_t)dq, (uint32_t)(dq >> 32), 0, 16u << 10 | (uint32_t)dci << 16 | (uint32_t)k.slot << 24, nullptr);
    control(k, 0x02, 1, 0, in ? k.ep_in : k.ep_out, 0, nullptr);
}

static void bot_reset(Keyboard& k) {
    control(k, 0x21, 0xFF, 0, k.iface, 0, nullptr);       // Bulk-Only Mass Storage Reset
    clear_halt(k, true);
    clear_halt(k, false);
}

// A SCSI command: 0 done, 1 failed (check condition), -1 transport error.
static int scsi(Keyboard& k, const uint8_t* cdb, int cdb_len, bool in, uint32_t len) {
    volatile uint8_t* w = msd_cbw;
    for (int i = 0; i < 31; i++) w[i] = 0;
    uint32_t tag = ++k.tag;
    w[0] = 'U'; w[1] = 'S'; w[2] = 'B'; w[3] = 'C';
    w[4] = (uint8_t)tag; w[5] = (uint8_t)(tag >> 8); w[6] = (uint8_t)(tag >> 16); w[7] = (uint8_t)(tag >> 24);
    w[8] = (uint8_t)len; w[9] = (uint8_t)(len >> 8); w[10] = (uint8_t)(len >> 16); w[11] = (uint8_t)(len >> 24);
    w[12] = in ? 0x80 : 0;
    // ATAPI and UFI devices take 12-byte commands only (Linux pads them too):
    // a USB CD drive behind such a bridge fails a 6-byte INQUIRY.
    w[14] = (uint8_t)(k.subclass >= 2 && k.subclass <= 5 ? 12 : cdb_len);
    for (int i = 0; i < cdb_len; i++) w[15 + i] = cdb[i];
    int cc = bulk(k, false, w, 31, nullptr);
    if (cc < 0) { k.timeouts++; return -1; }
    if (cc != 1) { if (cc == 6) bot_reset(k); return -1; }
    if (len) {
        cc = bulk(k, in, msd_buf, len, nullptr);
        if (cc < 0) { k.timeouts++; return -1; }
        if (cc == 6) clear_halt(k, in);
        else if (cc != 1 && cc != 13) return -1;
    }
    volatile uint8_t* csw = msd_cbw + 64;
    for (int tries = 0; tries < 2; tries++) {
        for (int i = 0; i < 13; i++) csw[i] = 0;
        cc = bulk(k, true, csw, 13, nullptr);
        if (cc == 6) { clear_halt(k, true); continue; }
        break;
    }
    if (cc < 0) { k.timeouts++; return -1; }
    if (cc != 1 && cc != 13) { bot_reset(k); return -1; }
    uint32_t t = csw[4] | csw[5] << 8 | csw[6] << 16 | (uint32_t)csw[7] << 24;
    if (csw[0] != 'U' || csw[1] != 'S' || csw[2] != 'B' || csw[3] != 'S' || t != tag || csw[12] == 2) { bot_reset(k); return -1; }
    return csw[12] ? 1 : 0;
}

static void request_sense(Keyboard& k) {
    uint8_t cdb[6] = { 0x03, 0, 0, 0, 18, 0 };
    scsi(k, cdb, 6, true, 18);
}

// USB CD/DVD drives (SCSI peripheral type 5): no medium needed to attach;
// cd.c sends them SCSI commands through usb_cd_packet. A drive plugged
// back in takes the slot of the one that went, so its DOS drive letter
// works again.
#define UCD_MAX 4
static int ucd_dev[UCD_MAX], n_ucd;
static uint8_t ucd_asc;
static bool ucd_is(int i) {                                 // slot i still holds a plugged-in drive
    Keyboard& k = kbds[ucd_dev[i]];
    return k.active && k.kind == MSD && k.bsize == 2048;
}
static bool ucd_setup(Keyboard& k) {
    int slot = -1;
    for (int i = 0; i < n_ucd; i++) if (!ucd_is(i)) { slot = i; break; }
    if (slot < 0) { if (n_ucd == UCD_MAX) { k.active = false; return false; } slot = n_ucd++; }
    ucd_dev[slot] = (int)(&k - kbds);
    k.bsize = 2048;
    printf("USB: CD/DVD drive on %s: %s (slot %d, %s speed)\n", k.where, k.model, k.slot, speed_name(k.speed));
    return true;
}

static bool msd_setup(Keyboard& k) {
    if (!msd_buf) {
        msd_buf = (volatile uint8_t*)dma_alloc(65536, 65536);
        msd_cbw = (volatile uint8_t*)dma_alloc(128, 64);
        if (!msd_buf || !msd_cbw) { printf("USB: out of memory for mass storage\n"); return false; }
    }
    bool room = n_msd < MSD_MAX;
    for (int i = 0; i < n_msd && !room; i++) room = !msd_is(i);
    if (!room) { printf("USB: %s: too many USB disks\n", k.where); return false; }
    k.kind = MSD;
    if (!configure_bulk(k)) return false;
    k.active = true;
    msd_buf[0] = 0;
    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    int ri = -1;
    k.timeouts = 0;
    for (int t = 0; t < 3 && ri != 0; t++) {                // a drive just plugged in may not answer at first
        msd_buf[0] = 0;
        ri = scsi(k, inq, 6, true, 36);
        if (k.timeouts) break;
        if (ri) { request_sense(k); delay_ms(100); }
    }
    // A transfer that never completed: the device (or its link) isn't
    // working, and every later command would wait out the same timeout.
    // Give up on it rather than retry for minutes; a SuperSpeed one gets
    // another try at USB 2 (setup_port), where many sticks do work.
    if (k.timeouts) {
        printf("USB: %s: no answer to mass-storage commands%s\n", k.where,
               k.speed >= 4 && k.parent < 0 ? "" : ": giving up on it");
        msd_stuck = true;
        k.active = false;
        return false;
    }
    if (ri) printf("USB: %s: no INQUIRY answer, taken for a disk\n", k.where);
    if (ri == 0) {
        int n = 0;
        for (int i = 8; i < 32 && n < 27; i++) {
            char c = (char)msd_buf[i];
            if (c < 32 || c > 126) c = ' ';
            if (c == ' ' && (n == 0 || k.model[n - 1] == ' ')) continue;
            k.model[n++] = c;
        }
        while (n && k.model[n - 1] == ' ') n--;
        k.model[n] = 0;
    }
    if ((msd_buf[0] & 0x1F) == 5) return ucd_setup(k);       // CD/DVD drive: cd.c's, through usb_cd_*
    if (msd_buf[0] & 0x1F) { printf("USB: %s: storage device type %d, not a disk\n", k.where, msd_buf[0] & 0x1F); k.active = false; return false; }
    bool ready = false;
    for (int i = 0; i < 50 && !ready; i++) {            // up to ~5 s to spin up / settle
        uint8_t tur[6] = { 0 };
        int r = scsi(k, tur, 6, false, 0);
        if (r == 0) ready = true;
        else if (k.timeouts) break;
        else { request_sense(k); delay_ms(100); }
    }
    uint8_t rc[10] = { 0x25 };
    int r = -1;
    for (int i = 0; i < 3 && r != 0 && !k.timeouts; i++) { r = scsi(k, rc, 10, true, 8); if (r && !k.timeouts) request_sense(k); }
    if (r != 0) { printf("USB: %s: %s: no capacity (no medium? ready: %d)\n", k.where, k.model, ready); k.active = false; return false; }
    uint32_t last = (uint32_t)msd_buf[0] << 24 | msd_buf[1] << 16 | msd_buf[2] << 8 | msd_buf[3];
    k.bsize = (uint32_t)msd_buf[4] << 24 | msd_buf[5] << 16 | msd_buf[6] << 8 | msd_buf[7];
    k.blocks = (uint64_t)last + 1;
    printf("USB: disk on %s: %s, %u MiB, %u-byte blocks (slot %d, %s speed)\n", k.where, k.model,
           (uint32_t)(k.blocks * k.bsize >> 20), k.bsize, k.slot, speed_name(k.speed));
    if (k.bsize != 512) { printf("USB: %s: only 512-byte blocks are supported\n", k.where); k.active = false; return false; }
    int slot = -1;
    for (int i = 0; i < n_msd && slot < 0; i++)         // this disk, back again
        if (!msd_is(i) && msd_last[i].blocks == k.blocks && !memcmp(msd_last[i].model, k.model, sizeof k.model)) slot = i;
    for (int i = 0; i < n_msd && slot < 0; i++)         // another unplugged one's slot
        if (!msd_is(i)) slot = i;
    if (slot < 0 && n_msd < MSD_MAX) slot = n_msd++;    // a new one
    if (slot < 0) { k.active = false; return false; }
    msd_dev[slot] = (int)(&k - kbds);
    memcpy(msd_last[slot].model, k.model, sizeof msd_last[slot].model);
    msd_last[slot].blocks = k.blocks;
    msd_last[slot].gen++;
    printf("USB: %s is USB disk %d\n", k.where, slot);
    return true;
}

extern "C" int usb_msd_count(void) { return n_msd; }
extern "C" uint64_t usb_msd_sectors(int i) { return msd_is(i) ? kbds[msd_dev[i]].blocks : 0; }
extern "C" uint32_t usb_msd_gen(int i) { return i >= 0 && i < n_msd ? msd_last[i].gen : 0; }
extern "C" const char* usb_msd_name(int i) { return i >= 0 && i < n_msd ? msd_last[i].model : ""; }

// count <= 128 sectors; buf anywhere (copied through the DMA buffer).
extern "C" int usb_msd_rw(int i, uint64_t lba, uint32_t count, void* buf, int write) {
    if (!msd_is(i) || !count || count > 128) return -1;
    Keyboard& k = kbds[msd_dev[i]];
    if (lba + count > k.blocks || lba > 0xFFFFFFFFull) return -1;
    Hc* saved = H;
    usb_busy = 1;
    H = k.hc;
    uint32_t bytes = count * 512;
    if (write) memcpy((void*)msd_buf, buf, bytes);
    uint8_t cdb[10] = { (uint8_t)(write ? 0x2A : 0x28), 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                        (uint8_t)(lba >> 8), (uint8_t)lba, 0, (uint8_t)(count >> 8), (uint8_t)count, 0 };
    int r = -1;
    for (int t = 0; t < 3 && r != 0; t++) {
        r = scsi(k, cdb, 10, !write, bytes);
        if (r != 0) {
            request_sense(k);
            if (write) memcpy((void*)msd_buf, buf, bytes);  // the sense data went through the buffer
        }
    }
    if (r == 0 && !write) memcpy(buf, (const void*)msd_buf, bytes);
    if (r != 0) printf("USB: %s: %s of %u sectors at %u failed\n", k.where, write ? "write" : "read", count, (uint32_t)lba);
    H = saved;
    usb_busy = 0;
    return r == 0 ? 0 : -1;
}

// ---------------------------------------------------------------- audio
// USB Audio Class 1 and 2 output (USB headphones, headsets, DACs, speaker
// dongles), xHCI only: one isochronous OUT stream, 16-bit stereo at 48 kHz
// when the device takes it (else 44.1 kHz, 24/32-bit, mono: converted on
// the way). audio.cpp writes its usual 48 kHz ring; usb_audio_pump (from
// the timer interrupt, like the other cards' refills) copies the next few
// milliseconds of it into packets, one transfer descriptor (TD) per service
// interval. Completions (every couple of ms) are what moves the play
// position on. The device is set up (interface, rate, unmuted) when it's
// plugged in; starting and stopping only start and stop the packets, so
// nothing here sends a control transfer from the interrupt.
#define UA_TRBS 512                                    // the stream's ring, link TRB included
static struct {
    int dev = -1;                                      // kbds index, -1: none plugged in
    uint32_t gen;                                      // plug count, for audio.cpp to notice
    volatile Trb* trb;                                 // own ring (longer than RING_TRBS)
    volatile uint8_t* pkt;                             // a packet buffer per ring slot
    int pkt_max;                                       // bytes a packet buffer
    int enq; uint32_t cycle;
    int dci;
    uint32_t td_us;                                    // one TD: a service interval
    uint32_t rate; int ch, bytes;                      // what the device plays
    uint32_t seq_of[UA_TRBS];                          // TD number in each ring slot
    volatile uint32_t seq_done, seq_enq;
    volatile bool running;
    const int16_t* src; uint32_t src_frames;           // audio.cpp's ring (48 kHz stereo)
    uint32_t seq_start, facc;
    uint32_t src_pos;                                  // ring frame of the next TD
    uint32_t underruns;
    uint32_t ev_dry, ev_missed, ev_err, last_cc;       // xHCI's events for the stream: ring underrun, missed service, other errors
    char name[64];
    // EHCI / UHCI (usb2.inc): one isochronous descriptor per frame, in a
    // ring of UA2_SLOTS linked into the periodic schedule once.
    Hc* iso_hc;                                        // the controller they're linked into, null: none
    volatile uint32_t* iso;                            // UA2_SLOTS descriptors, 128 bytes each
    int iso_kind;                                      // UA2_UHCI / UA2_SITD / UA2_ITD
    int addr, ep, hub, hub_port, hs_iv;                // hs_iv: microframes between packets (iTD)
    uint32_t hw_last, hw_ext;                          // the controller's frame counter, extended to 32 bits
    uint32_t f_base;                                   // TD seq is played in frame f_base + seq
    int o_tail;                                        // OHCI: the ring slot of the ED's dummy tail TD
    volatile int o_flush; uint32_t o_flush_at;         // OHCI: emptying its queue (1: to start, 2: skipping since o_flush_at)
} ua;

static bool ua_is(void) { return ua.dev >= 0 && kbds[ua.dev].active && kbds[ua.dev].kind == AUDIO; }
// EHCI / UHCI streams (usb2.inc).
struct UaAlt;
static bool ua2_link(Keyboard& k, const UaAlt& a);
static void ua2_unlink(void);
static void ua2_start(void);
static void ua2_done(void);
static void ua2_pump(void);

// A class request on endpoint 0 that the device may stall: on xHCI a STALL
// halts the default endpoint too, so it's reset and moved on.
static bool ctl_try(Keyboard& k, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, volatile void* buf) {
    if (control(k, type, req, value, index, len, buf)) return true;
    if (H->type == HC_XHCI && ctl_cc == 6) {
        command(0, 0, 0, 14u << 10 | 1u << 16 | (uint32_t)k.slot << 24, nullptr);            // Reset Endpoint 0
        uint64_t dq = phys(&k.ep0.trb[k.ep0.enq]) | k.ep0.cycle;
        command((uint32_t)dq, (uint32_t)(dq >> 32), 0, 16u << 10 | 1u << 16 | (uint32_t)k.slot << 24, nullptr);
    }
    return false;
}

// An audio streaming alternate setting with an isochronous OUT endpoint.
struct UaAlt {
    int iface, alt, ep, mps, interval;
    int ch, bytes, bits;
    uint32_t rates[8]; int nrates;                     // UAC1: discrete rates; nrates < 0: range rates[0]..rates[1]
    bool fmt_pcm;
};

static bool ua_rate_ok(const UaAlt& a, uint32_t r) {
    if (a.nrates < 0) return r >= a.rates[0] && r <= a.rates[1];
    for (int i = 0; i < a.nrates; i++) if (a.rates[i] == r) return true;
    return a.nrates == 0;                              // UAC2: the clock decides
}
static int ua_score(const UaAlt& a) {
    if (!a.fmt_pcm || a.ch < 1 || a.bytes < 2 || a.bytes > 4) return -1;
    int s = 0;
    if (ua_rate_ok(a, 48000)) s += 8; else if (ua_rate_ok(a, 44100)) s += 4; else return -1;
    if (a.ch == 2) s += 4; else if (a.ch == 1) s += 2;
    if (a.bytes == 2) s += 2;
    return s;
}

// Device k's descriptors (config descriptor `d`, `total` bytes): if it plays
// audio, set it up as the USB audio output. True: driven (as AUDIO).
static bool ua_setup(Keyboard& k, const volatile uint8_t* d, int total, int config, uint16_t vid, uint16_t pid) {
    int uac = 0, ac_iface = -1;
    int fu[8], nfu = 0, clk[4], nclk = 0;
    UaAlt best{}; int best_score = -1;
    UaAlt cur{}; bool in_as = false, cur_valid = false;
    auto finish = [&]() {
        if (in_as && cur_valid && cur.ep) {
            int s = ua_score(cur);
            if (s > best_score) { best = cur; best_score = s; }
        }
    };
    for (int i = 0; i + 1 < total && d[i] >= 2; i += d[i]) {
        uint8_t type = d[i + 1];
        if (type == 4 && i + 8 < total) {
            finish();
            in_as = false; cur_valid = false;
            if (d[i + 5] == 1 && d[i + 6] == 1) {          // audio control
                if (ac_iface < 0) { ac_iface = d[i + 2]; uac = d[i + 7] == 0x20 ? 2 : 1; }
            } else if (d[i + 5] == 1 && d[i + 6] == 2 && d[i + 2] != 0xFF) {   // audio streaming
                in_as = d[i + 3] != 0;                         // alt 0: no bandwidth
                cur = UaAlt{};
                cur.iface = d[i + 2]; cur.alt = d[i + 3];
            }
        } else if (type == 0x24 && ac_iface >= 0 && !in_as && i + 3 < total) {    // AC class-specific
            int sub = d[i + 2];
            if (sub == 0x06 && nfu < 8) fu[nfu++] = d[i + 3];                     // feature unit
            if (uac == 2 && sub == 0x0A && nclk < 4) clk[nclk++] = d[i + 3];       // clock source
        } else if (type == 0x24 && in_as && i + 3 < total) {                      // AS class-specific
            int sub = d[i + 2];
            if (sub == 1) {                                                       // AS_GENERAL
                if (uac == 2 && i + 10 < total) {
                    cur.fmt_pcm = d[i + 5] == 1 && (d[i + 6] & 1);                 // type I, PCM
                    cur.ch = d[i + 10];
                } else if (i + 6 < total) cur.fmt_pcm = (d[i + 5] | d[i + 6] << 8) == 1;   // PCM
            } else if (sub == 2 && i + 6 < total && d[i + 3] == 1) {              // FORMAT_TYPE I
                if (uac == 2) { cur.bytes = d[i + 4]; cur.bits = d[i + 5]; cur.nrates = 0; }
                else {
                    cur.ch = d[i + 4]; cur.bytes = d[i + 5]; cur.bits = d[i + 6];
                    int nf = d[i + 7];
                    if (nf == 0 && i + 13 < total) {
                        cur.nrates = -1;
                        cur.rates[0] = d[i + 8] | d[i + 9] << 8 | d[i + 10] << 16;
                        cur.rates[1] = d[i + 11] | d[i + 12] << 8 | d[i + 13] << 16;
                    } else
                        for (int f = 0; f < nf && f < 8 && i + 10 + f * 3 < total; f++)
                            cur.rates[cur.nrates++] = d[i + 8 + f * 3] | d[i + 9 + f * 3] << 8 | d[i + 10 + f * 3] << 16;
                }
                cur_valid = true;
            }
        } else if (type == 5 && in_as && i + 6 < total) {
            int a = d[i + 2], attr = d[i + 3];
            if ((attr & 3) == 1 && !(a & 0x80) && !cur.ep) {                      // isochronous OUT (not feedback)
                cur.ep = a;
                cur.mps = (d[i + 4] | d[i + 5] << 8) & 0x7FF;
                cur.interval = d[i + 6] ? d[i + 6] : 1;
            }
        }
    }
    finish();
    if (ac_iface < 0) return false;                    // not an audio device
    if (best_score < 0) {
        printf("USB: audio device %04x:%04x on %s: no output it can play here (only a microphone?)\n", vid, pid, k.where);
        return false;
    }
    if (H->type != HC_XHCI && best.interval != 1 && k.speed != 3) {
        printf("USB: audio device %04x:%04x on %s: a packet every %d frames isn't supported\n", vid, pid, k.where,
               1 << (best.interval - 1));
        return false;
    }
    if (H->type != HC_XHCI && k.speed == 3 && best.interval > 4) {
        printf("USB: audio device %04x:%04x on %s: a packet every %d microframes isn't supported\n", vid, pid, k.where,
               1 << (best.interval - 1));
        return false;
    }
    if (k.speed >= 4) { printf("USB: audio device on %s: SuperSpeed audio isn't supported\n", k.where); return false; }
    if (ua_is()) { printf("USB: audio device on %s: one is plugged in already\n", k.where); return false; }
    if (!ua.trb) {
        ua.trb = (volatile Trb*)dma_alloc(UA_TRBS * sizeof(Trb), 64);
        ua.pkt = (volatile uint8_t*)dma_alloc(UA_TRBS * 1024, 64);
        if (!ua.trb || !ua.pkt) { printf("USB: %s: out of memory for audio\n", k.where); ua.trb = nullptr; return false; }
    }
    if (best.mps > 1024) best.mps = 1024;

    if (!control(k, 0x00, 9, config, 0, 0, nullptr)) {                          // SET_CONFIGURATION
        printf("USB: %s: audio: SET_CONFIGURATION %d failed (cc=%d)\n", k.where, config, ctl_cc);
        return false;
    }
    if (!control(k, 0x01, 11, best.alt, best.iface, 0, nullptr)) {               // SET_INTERFACE
        printf("USB: %s: audio: SET_INTERFACE %d alt %d failed\n", k.where, best.iface, best.alt);
        return false;
    }
    static volatile uint8_t buf[8] __attribute__((aligned(64)));
    uint32_t rate = ua_rate_ok(best, 48000) ? 48000 : 44100;
    if (uac == 1) {
        // Sampling frequency control on the endpoint (if it has one).
        buf[0] = (uint8_t)rate; buf[1] = (uint8_t)(rate >> 8); buf[2] = (uint8_t)(rate >> 16);
        ctl_try(k, 0x22, 0x01, 0x0100, best.ep, 3, buf);
        if (best.nrates == 1) rate = best.rates[0];
    } else {
        // Every clock source to 48 kHz (else 44.1), then what it says it runs at.
        for (int c = 0; c < nclk; c++) {
            static const uint32_t wants[2] = {48000, 44100};
            for (uint32_t want : wants) {
                buf[0] = (uint8_t)want; buf[1] = (uint8_t)(want >> 8); buf[2] = (uint8_t)(want >> 16); buf[3] = 0;
                if (ctl_try(k, 0x21, 0x01, 0x0100, (uint16_t)(clk[c] << 8 | ac_iface), 4, buf)) break;
            }
            buf[0] = buf[1] = buf[2] = buf[3] = 0;
            if (ctl_try(k, 0xA1, 0x01, 0x0100, (uint16_t)(clk[c] << 8 | ac_iface), 4, buf)) {
                uint32_t got = buf[0] | buf[1] << 8 | buf[2] << 16 | (uint32_t)buf[3] << 24;
                if (got >= 8000 && got <= 192000) rate = got;
            }
        }
    }
    // Unmuted, at 0 dB (the volume setting is applied to the samples).
    for (int f = 0; f < nfu; f++)
        for (int c = 0; c <= 2; c++) {
            buf[0] = 0;
            ctl_try(k, 0x21, 0x01, (uint16_t)(1 << 8 | c), (uint16_t)(fu[f] << 8 | ac_iface), 1, buf);     // mute off
            buf[0] = 0; buf[1] = 0;
            ctl_try(k, 0x21, 0x01, (uint16_t)(2 << 8 | c), (uint16_t)(fu[f] << 8 | ac_iface), 2, buf);     // volume 0 dB
        }

    // The isochronous OUT endpoint. Interval: 2^(bInterval-1) frames (full
    // speed) or microframes (high speed), in xHCI's 125 us units.
    int interval = k.speed == 3 ? best.interval - 1 : best.interval - 1 + 3;
    if (interval < 0) interval = 0;
    if (interval > 15) interval = 15;
    int dci = (best.ep & 0xF) * 2;
    ua.running = false;
    if (H->type != HC_XHCI) {                          // EHCI / UHCI: a descriptor a frame
        if (!ua2_link(k, best)) return false;
        interval = 3;                                  // 1 ms
        dci = best.ep & 0xF;
    } else {
        memset((void*)k.in_ctx, 0, 33 * H->csz);
        ctx(k.in_ctx, 0)[1] = 1 | 1u << dci;
        fill_slot(k, dci);
        volatile uint32_t* ep = ctx(k.in_ctx, 1 + dci);
        ep[0] = (uint32_t)interval << 16;
        ep[1] = 1u << 3 | (uint32_t)best.mps << 16;              // isochronous OUT, CErr 0
        for (int i = 0; i < UA_TRBS; i++) ua.trb[i].d0 = ua.trb[i].d1 = ua.trb[i].d2 = ua.trb[i].d3 = 0;
        volatile Trb& link = ua.trb[UA_TRBS - 1];
        link.d0 = (uint32_t)phys(ua.trb); link.d1 = (uint32_t)(phys(ua.trb) >> 32);
        link.d3 = TRB_LINK << 10 | (1 << 1);
        uint64_t ri = phys(ua.trb) | 1;
        ep[2] = (uint32_t)ri; ep[3] = (uint32_t)(ri >> 32);
        ep[4] = (uint32_t)best.mps | (uint32_t)best.mps << 16;   // average TRB length, max ESIT payload
        uint64_t ic = phys(k.in_ctx);
        int cc = command((uint32_t)ic, (uint32_t)(ic >> 32), 0, TRB_CONFIGURE_EP << 10 | (uint32_t)k.slot << 24, nullptr);
        if (cc != 1) { printf("USB: %s: audio: configure endpoint failed (%d)\n", k.where, cc); return false; }
    }

    ua.enq = 0; ua.cycle = 1;
    ua.dci = dci;
    ua.td_us = 125u << interval;
    ua.pkt_max = best.mps;
    ua.rate = rate; ua.ch = best.ch; ua.bytes = best.bytes;
    ua.seq_done = ua.seq_enq = 0;
    ua.facc = 0;
    k.kind = AUDIO;
    k.dci = dci;
    k.active = true;
    ua.dev = (int)(&k - kbds);
    snprintf(ua.name, sizeof ua.name, "USB audio %04x:%04x on %s", vid, pid, k.where);
    ua.gen++;
    printf("USB: audio output on %s: %04x:%04x, UAC%d, %u Hz, %d-bit, %d channel%s, %u us packets (slot %d, %s speed)\n",
           k.where, vid, pid, uac, rate, best.bits ? best.bits : best.bytes * 8, best.ch, best.ch == 1 ? "" : "s",
           ua.td_us, k.slot, speed_name(k.speed));
    return true;
}

// A transfer event on the stream's endpoint (from dispatch): every TD up to
// the one it names has been played (or skipped).
static bool ua_dispatch(int slot, int ep, const Trb& e) {
    if (!ua_is() || kbds[ua.dev].hc != H || kbds[ua.dev].slot != slot || ep != ua.dci) return false;
    int cc = e.d2 >> 24;
    uint64_t trb = (uint64_t)e.d1 << 32 | e.d0;
    int idx = (int)((trb - phys(ua.trb)) / sizeof(Trb));
    if (cc == 14 || cc == 15) { ua.ev_dry++; ua.seq_done = ua.seq_enq; return true; }   // ring under/overrun: all of it went
    if (cc == 23) ua.ev_missed++;                       // missed service: the controller was late for it
    else if (cc != 1 && cc != 13) { ua.ev_err++; ua.last_cc = (uint32_t)cc; }
    if (idx >= 0 && idx < UA_TRBS - 1 && (int32_t)(ua.seq_of[idx] + 1 - ua.seq_done) > 0) ua.seq_done = ua.seq_of[idx] + 1;
    return true;
}

// audio.cpp's side. All of these may run from the timer interrupt: they
// touch only the stream's own ring and doorbell, never H or the event ring.
extern "C" int usb_audio_present(void) { return ua_is(); }
extern "C" uint32_t usb_audio_gen(void) { return ua.gen; }
extern "C" const char* usb_audio_name(void) { return ua.name; }
extern "C" int usb_audio_start(const int16_t* ring, uint32_t frames) {
    if (!ua_is()) return 0;
    ua.running = false;
    ua.src = ring; ua.src_frames = frames;
    ua.seq_start = ua.seq_enq;
    ua.src_pos = 0;
    if (kbds[ua.dev].hc->type != HC_XHCI) ua2_start();
    ua.running = true;
    printf("USB: audio: playing through %s\n", ua.name);
    return 1;
}
extern "C" void usb_audio_stop(void) { ua.running = false; }
// Where audio.cpp must stay ahead of: how far the pump has taken samples
// from its ring (into packets the device plays over the next few tens of
// ms), not how far the device has played. Reported as the device's play
// position, the queue (and how late the transfer events are read) went
// unaccounted: audio.cpp kept its 50 ms lead on a position that was up to
// 40 ms behind the pump, which then caught up with what it hadn't written
// yet, or set off a resync, many times a second on real hardware.
extern "C" uint32_t usb_audio_pos(void) {
    if (!ua.running) return 0;
    return ua.src_pos;
}
extern "C" uint32_t usb_audio_underruns(void) { return ua.underruns; }
// For the gap log: TDs played (seq_done), the pump finding the queue empty,
// the controller's ring underruns, missed services, other errors (last code).
extern "C" void usb_audio_stats(uint32_t* s) {
    s[0] = ua.seq_done; s[1] = ua.underruns; s[2] = ua.ev_dry; s[3] = ua.ev_missed; s[4] = ua.ev_err; s[5] = ua.last_cc;
    s[6] = ua.td_us;
}

// n device frames into p from `per` of audio.cpp's ring frames starting at
// src (resampled, to the device's channels and sample size). Bytes written.
static int ua_render(volatile uint8_t* p, uint32_t src, uint32_t per, uint32_t n) {
    int o = 0;
    for (uint32_t j = 0; j < n; j++) {
        // per ring frames stretched to n: linear, in 1/256ths.
        uint32_t pos = j * per * 256 / n;
        uint32_t i0 = (src + (pos >> 8)) % ua.src_frames, i1 = (i0 + 1) % ua.src_frames, f = pos & 255;
        int l = (ua.src[i0 * 2] * (int)(256 - f) + ua.src[i1 * 2] * (int)f) >> 8;
        int r = (ua.src[i0 * 2 + 1] * (int)(256 - f) + ua.src[i1 * 2 + 1] * (int)f) >> 8;
        for (int c = 0; c < ua.ch; c++) {
            int v = ua.ch == 1 ? (l + r) / 2 : c == 0 ? l : c == 1 ? r : 0;
            if (ua.bytes == 2) { p[o++] = (uint8_t)v; p[o++] = (uint8_t)(v >> 8); }
            else if (ua.bytes == 3) { p[o++] = 0; p[o++] = (uint8_t)v; p[o++] = (uint8_t)(v >> 8); }
            else { p[o++] = 0; p[o++] = 0; p[o++] = (uint8_t)v; p[o++] = (uint8_t)(v >> 8); }
        }
    }
    return o;
}

// Keep about 30 ms of packets queued. What has played is known only from
// the transfer events, and those are read every 8 ms (usb_tick), while
// this runs every 4 ms, often just before them: it can be working from
// a count 12 ms old. With only 10 ms queued it believed the ring full
// while the device was about to run dry, and real hardware (unlike QEMU's
// device, which buffers on its own) went silent for a moment every few
// milliseconds: sound that kept cutting out.
#define UA_QUEUE_US 30000
extern "C" void usb_audio_pump(void) {
    if (!ua.running || !ua_is() || !ua.src) return;
    Keyboard& k = kbds[ua.dev];
    if (k.hc->type != HC_XHCI) { ua2_pump(); return; }
    uint32_t per = 48 * ua.td_us / 1000;               // ring frames a TD
    uint32_t q = UA_QUEUE_US / ua.td_us; if (q < 2) q = 2; if (q > UA_TRBS - 8) q = UA_TRBS - 8;
    uint32_t ioc_every = 2000 / ua.td_us; if (ioc_every < 1) ioc_every = 1;
    if ((int32_t)(ua.seq_enq - ua.seq_done) <= 0 && ua.seq_enq != ua.seq_start) ua.underruns++;   // ran dry
    bool any = false;
    while ((int32_t)(ua.seq_enq - ua.seq_done) < (int32_t)q) {
        uint32_t seq = ua.seq_enq;
        uint32_t src = ua.src_pos;
        ua.src_pos = (ua.src_pos + per) % ua.src_frames;
        // Device frames this TD: rate x interval, the remainder carried over.
        ua.facc += ua.rate * ua.td_us;
        uint32_t n = ua.facc / 1000000;
        ua.facc %= 1000000;
        int fb = ua.ch * ua.bytes;
        if ((int)n * fb > ua.pkt_max) n = (uint32_t)(ua.pkt_max / fb);
        volatile uint8_t* p = ua.pkt + ua.enq * 1024;
        int o = ua_render(p, src, per, n);
        volatile Trb& t = ua.trb[ua.enq];
        uint64_t a = phys(p);
        t.d0 = (uint32_t)a; t.d1 = (uint32_t)(a >> 32); t.d2 = (uint32_t)o;
        ua.seq_of[ua.enq] = seq;
        barrier();
        // Isoch TRB, start as soon as possible (after the one before),
        // interrupt on completion every couple of ms.
        t.d3 = 5u << 10 | (seq % ioc_every == ioc_every - 1 ? 1u << 5 : 0) | 1u << 31 | ua.cycle;
        if (++ua.enq == UA_TRBS - 1) {
            volatile Trb& link = ua.trb[UA_TRBS - 1];
            link.d3 = (link.d3 & ~1u) | ua.cycle;
            ua.enq = 0;
            ua.cycle ^= 1;
        }
        ua.seq_enq = seq + 1;
        any = true;
    }
    if (any) k.hc->db[k.slot] = ua.dci;
}

// The rest of setting up a device that has slot k.slot: its memory, an
// address, and a keyboard, mouse or hub on top. False: not driven.
// ---------------------------------------------------------------- USB Ethernet (CDC-ECM)
// The standard USB Ethernet class: a communications interface (class 02h,
// subclass 06h) whose Ethernet descriptor names the MAC address string, and
// a data interface (0Ah) whose alternate setting 1 has a bulk IN and a bulk
// OUT endpoint; one frame per transfer. Adapters with a vendor protocol of
// their own (RTL8153...) often offer ECM as another configuration, and
// QEMU's usb-net has RNDIS first: every configuration is looked through.
// Received frames go to net.cpp (net_usb_rx) as their transfers complete;
// UNET_RX transfers wait on the bulk IN endpoint all the time (xHCI); on
// EHCI, UHCI and OHCI the endpoint is polled from the periodic schedule
// (usb2.inc's un2_*).
extern "C" void net_usb_rx(const uint8_t* f, unsigned len);
#define UNET_RX 16
#define UNET_BUF 2048
static struct {
    int dev = -1;
    int ctl_iface, data_iface;
    uint8_t mac[6];
    char name[24];
    volatile uint8_t *rxb, *txb;
    int buf_of[RING_TRBS];
    uint32_t gen, errors;
    // The communications interface's notification endpoint (interrupt IN):
    // NETWORK_CONNECTION says whether a cable is in (xHCI only; elsewhere,
    // and until the adapter says, the link is taken as up).
    int st_ep, st_mps, st_int, st_dci;
    Ring st;
    volatile Trb* st_mem;
    volatile uint8_t* st_buf;
    int link;                                       // -1 not heard yet, 0 down, 1 up
    uint32_t link_since;                            // ticks when the notifications started
} un;
extern "C" volatile uint32_t ticks;

static bool un_is() { return un.dev >= 0 && kbds[un.dev].active && kbds[un.dev].kind == NET; }
static bool un2_start(Keyboard& k);                    // EHCI / UHCI / OHCI (usb2.inc)
static const char* hc_type_name(int t);

static void un_queue(Keyboard& k, int b)
{
    un.buf_of[k.intr.enq] = b;
    uint64_t p = phys(un.rxb + b * UNET_BUF);
    ring_push(k.intr, (uint32_t)p, (uint32_t)(p >> 32), UNET_BUF, TRB_NORMAL << 10 | (1 << 5) | (1 << 2));   // IOC, ISP
}

static void un_st_queue()
{
    uint64_t p = phys(un.st_buf);
    ring_push(un.st, (uint32_t)p, (uint32_t)(p >> 32), 64, TRB_NORMAL << 10 | (1 << 5) | (1 << 2));   // IOC, ISP
}

// A notification: NETWORK_CONNECTION (00h, wValue 1 = connected); others
// (CONNECTION_SPEED_CHANGE) ignored. Then wait for the next.
static void un_st_event(Keyboard& k, const Trb& e)
{
    int cc = e.d2 >> 24;
    if (cc == 1 || cc == 13) {
        uint32_t got = 64 - (e.d2 & 0xFFFFFF);
        const volatile uint8_t* n = un.st_buf;
        if (got >= 8 && n[0] == 0xA1 && n[1] == 0x00) {
            int up = (n[2] | n[3] << 8) != 0;
            if (up != un.link) printf("USB: Ethernet on %s: cable %s\n", k.where, up ? "connected" : "unplugged");
            un.link = up;
        }
    } else if (cc == 6) {                               // stalled: reset the endpoint, move on
        command(0, 0, 0, 14u << 10 | (uint32_t)un.st_dci << 16 | (uint32_t)k.slot << 24, nullptr);
        uint64_t dq = phys(&un.st.trb[un.st.enq]) | un.st.cycle;
        command((uint32_t)dq, (uint32_t)(dq >> 32), 0, 16u << 10 | (uint32_t)un.st_dci << 16 | (uint32_t)k.slot << 24, nullptr);
        control(k, 0x02, 1, 0, (uint16_t)un.st_ep, 0, nullptr);
    } else return;                                      // stopped / other: leave it
    un_st_queue();
    H->db[k.slot] = (uint32_t)un.st_dci;
}

// Add the notification endpoint to the device's configured endpoints (xHCI).
static void un_st_start(Keyboard& k)
{
    un.link = -1;
    un.link_since = ticks;
    un.st_dci = 0;
    if (H->type != HC_XHCI || !un.st_ep) return;
    if (!un.st_mem) {
        un.st_mem = (volatile Trb*)dma_alloc(RING_TRBS * sizeof(Trb), 64);
        un.st_buf = (volatile uint8_t*)dma_alloc(64, 64);
        if (!un.st_mem || !un.st_buf) { un.st_mem = nullptr; return; }
    }
    ring_attach(un.st, un.st_mem);
    int dci = (un.st_ep & 0xF) * 2 + 1;
    int interval;
    if (k.speed == 1 || k.speed == 2) {                 // bInterval in ms -> 2^n x 125 us
        interval = 3;
        while (interval < 10 && (1 << (interval + 1)) <= un.st_int * 8) interval++;
    } else {
        interval = un.st_int ? un.st_int - 1 : 0;
        if (interval > 15) interval = 15;
    }
    int last = dci;
    if (k.dci > last) last = k.dci;
    if (k.dci_out > last) last = k.dci_out;
    memset((void*)k.in_ctx, 0, 33 * H->csz);
    ctx(k.in_ctx, 0)[1] = 1 | 1u << dci;
    fill_slot(k, last);
    volatile uint32_t* ep = ctx(k.in_ctx, 1 + dci);
    ep[0] = (uint32_t)interval << 16;
    ep[1] = 3 << 1 | 7 << 3 | (uint32_t)un.st_mps << 16;   // CErr 3, interrupt IN
    uint64_t ri = phys(un.st.trb) | 1;
    ep[2] = (uint32_t)ri; ep[3] = (uint32_t)(ri >> 32);
    ep[4] = 16 | (uint32_t)un.st_mps << 16;
    uint64_t ic = phys(k.in_ctx);
    int cc = command((uint32_t)ic, (uint32_t)(ic >> 32), 0, TRB_CONFIGURE_EP << 10 | (uint32_t)k.slot << 24, nullptr);
    if (cc != 1) { printf("USB: %s: Ethernet notifications not set up (%d)\n", k.where, cc); return; }
    un.st_dci = dci;
    un_st_queue();
    H->db[k.slot] = (uint32_t)dci;
}

static bool un_dispatch(int slot, int ep, const Trb& e)
{
    if (!un_is()) return false;
    Keyboard& k = kbds[un.dev];
    if (k.hc == H && k.slot == slot && un.st_dci && ep == un.st_dci) { un_st_event(k, e); return true; }
    if (k.hc != H || k.slot != slot || k.dci != ep) return false;
    int cc = e.d2 >> 24;
    uint64_t trb = (uint64_t)e.d1 << 32 | e.d0;
    int idx = (int)((trb - phys(k.intr.trb)) / sizeof(Trb));
    if (idx < 0 || idx >= RING_TRBS - 1) return true;
    int b = un.buf_of[idx];
    if (cc == 1 || cc == 13) {
        uint32_t len = UNET_BUF - (e.d2 & 0xFFFFFF);
        if (len >= 14) net_usb_rx((const uint8_t*)(un.rxb + b * UNET_BUF), len);
        un_queue(k, b);
        H->db[k.slot] = k.dci;
        return true;
    }
    if (++un.errors <= 10) printf("USB: Ethernet on %s: receive error (code %d)\n", k.where, cc);
    if (cc == 6) {                                      // stalled: clear it, start again
        clear_halt(k, true);
        for (int i = 0; i < UNET_RX; i++) un_queue(k, i);
    } else un_queue(k, b);
    H->db[k.slot] = k.dci;
    return true;
}

// Look through the device's configurations for CDC-ECM; set it up if found.
static bool un_probe(Keyboard& k, int ki, volatile uint8_t* desc, int total, int nconf, uint16_t vid, uint16_t pid)
{
    if (nconf < 1) nconf = 1;
    for (int ci = 0; ci < nconf && ci < 4; ci++) {
        if (ci > 0) {                                   // the next configuration
            if (!control(k, 0x80, 6, (uint16_t)(0x0200 | ci), 0, 9, desc)) break;
            total = desc[2] | desc[3] << 8;
            if (total > 1024) total = 1024;
            if (!control(k, 0x80, 6, (uint16_t)(0x0200 | ci), 0, (uint16_t)total, desc)) break;
        }
        int config = desc[5];
        int ctl = -1, data = -1, alt = 0, mac_idx = 0, ep_in = 0, ep_out = 0, mps = 512;
        int cur = -1, cur_alt = 0, cur_class = -1, cur_sub = -1, in_ep = 0, out_ep = 0, cur_mps = 512;
        int st_ep = 0, st_mps = 8, st_int = 8;
        for (int i = 0; i + 1 < total && desc[i] >= 2; i += desc[i]) {
            uint8_t t = desc[i + 1];
            if (t == 4) {
                if (cur_class == 0x0A && in_ep && out_ep && data < 0) { data = cur; alt = cur_alt; ep_in = in_ep; ep_out = out_ep; mps = cur_mps; }
                cur = desc[i + 2]; cur_alt = desc[i + 3]; cur_class = desc[i + 5]; cur_sub = desc[i + 6];
                in_ep = out_ep = 0;
                if (cur_class == 2 && cur_sub == 6 && ctl < 0) ctl = cur;
            } else if (t == 0x24 && cur == ctl && cur_class == 2 && i + 3 < total && desc[i + 2] == 0x0F) {
                mac_idx = desc[i + 3];                  // Ethernet networking functional descriptor
            } else if (t == 5 && cur == ctl && cur_class == 2 && (desc[i + 3] & 3) == 3 && (desc[i + 2] & 0x80) && !st_ep) {
                st_ep = desc[i + 2];                    // notifications
                st_mps = (desc[i + 4] | desc[i + 5] << 8) & 0x7FF;
                st_int = desc[i + 6] ? desc[i + 6] : 1;
            } else if (t == 5 && cur_class == 0x0A && (desc[i + 3] & 3) == 2) {
                if (desc[i + 2] & 0x80) in_ep = desc[i + 2]; else out_ep = desc[i + 2];
                cur_mps = (desc[i + 4] | desc[i + 5] << 8) & 0x7FF;
            }
        }
        if (cur_class == 0x0A && in_ep && out_ep && data < 0) { data = cur; alt = cur_alt; ep_in = in_ep; ep_out = out_ep; mps = cur_mps; }
        if (ctl < 0 || data < 0 || !mac_idx) continue;

        if (!control(k, 0x00, 9, (uint16_t)config, 0, 0, nullptr)) { printf("USB: %s: SET_CONFIGURATION %d failed\n", k.where, config); return false; }
        static volatile uint8_t str[64] __attribute__((aligned(64)));
        if (!control(k, 0x80, 6, (uint16_t)(0x0300 | mac_idx), 0x0409, sizeof str, str) || str[0] < 26) {
            printf("USB: %s: Ethernet adapter %04x:%04x: no MAC address string\n", k.where, vid, pid);
            return false;
        }
        for (int j = 0; j < 6; j++) {                   // 12 hex digits, UTF-16
            int v = 0;
            for (int n = 0; n < 2; n++) {
                int ch = str[2 + 2 * (2 * j + n)] | 0x20;
                v = v * 16 + (ch >= 'a' ? ch - 'a' + 10 : ch - '0');
            }
            un.mac[j] = (uint8_t)v;
        }
        if (!un.rxb) {
            un.rxb = (volatile uint8_t*)dma_alloc(UNET_RX * UNET_BUF, 4096);
            un.txb = (volatile uint8_t*)dma_alloc(UNET_BUF, 4096);
            if (!un.rxb || !un.txb) { printf("USB: out of memory for the Ethernet adapter\n"); un.rxb = nullptr; return false; }
        }
        // The data interface to alternate 0 (no endpoints: the function resets
        // its data path, as the CDC spec has it), then to the one with them.
        if (alt) {
            control(k, 0x01, 11, 0, (uint16_t)data, 0, nullptr);
            control(k, 0x01, 11, (uint16_t)alt, (uint16_t)data, 0, nullptr);
        }
        k.iface = data; k.ep_in = ep_in; k.ep_out = ep_out; k.mps = mps;
        if (!configure_bulk(k)) return false;
        un.ctl_iface = ctl; un.data_iface = data;
        un.st_ep = st_ep; un.st_mps = st_mps ? st_mps : 8; un.st_int = st_int;
        un_st_start(k);
        control(k, 0x21, 0x43, 0x0C, (uint16_t)ctl, 0, nullptr);   // SET_ETHERNET_PACKET_FILTER: directed, broadcast
        k.kind = NET;
        if (H->type == HC_XHCI) {
            for (int i = 0; i < UNET_RX; i++) un_queue(k, i);
            H->db[k.slot] = k.dci;
        } else if (!un2_start(k)) {                     // EHCI / UHCI / OHCI: polled from the periodic schedule
            printf("USB: %s: Ethernet adapter %04x:%04x: receive not set up\n", k.where, vid, pid);
            k.kind = KBD;
            return false;
        }
        k.active = true;
        un.dev = ki;
        un.gen++;
        un.errors = 0;
        snprintf(un.name, sizeof un.name, "USB %04x:%04x", vid, pid);
        printf("USB: Ethernet adapter %04x:%04x on %s (CDC-ECM, configuration %d, %s, %s speed), MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
               vid, pid, k.where, config, hc_type_name(H->type), speed_name(k.speed), un.mac[0], un.mac[1], un.mac[2], un.mac[3], un.mac[4], un.mac[5]);
        return true;
    }
    return false;
}

// net.cpp's side (from the kernel, interrupts off: never inside usb_poll).
extern "C" int usb_net_present(void) { return un_is(); }
// Cable in: what the adapter last said; up if it has no notifications, or
// hasn't said anything 3 s after they started (some only report changes).
extern "C" int usb_net_link(void)
{
    if (!un_is()) return 0;
    if (un.link >= 0) return un.link;
    return !un.st_dci || ticks - un.link_since > 3000;
}
extern "C" uint32_t usb_net_gen(void) { return un.gen; }
extern "C" const char* usb_net_name(void) { return un.name; }
extern "C" void usb_net_mac(uint8_t* m) { memcpy(m, un.mac, 6); }
extern "C" int usb_net_send(const uint8_t* f, unsigned len)
{
    if (!un_is() || usb_busy || in_poll || len > UNET_BUF - 1) return 0;
    Keyboard& k = kbds[un.dev];
    H = k.hc;
    memcpy((void*)un.txb, f, len);
    if (len % (unsigned)k.mps == 0) un.txb[len++] = 0;  // no zero-length packet needed: a byte of padding
    in_poll = 1;                                        // usb_tick stays out while we wait
    uint32_t got = 0;
    int cc = bulk(k, false, un.txb, len, &got);
    in_poll = 0;
    if (cc == 6) clear_halt(k, false);
    return cc == 1;
}
extern "C" void usb_net_filter(int bits)                // SET_ETHERNET_PACKET_FILTER
{
    if (!un_is() || usb_busy || in_poll) return;
    Keyboard& k = kbds[un.dev];
    H = k.hc;
    in_poll = 1;
    control(k, 0x21, 0x43, (uint16_t)bits, (uint16_t)un.ctl_iface, 0, nullptr);
    in_poll = 0;
}

static bool setup_slot(Keyboard& k, int ki) {
    auto& m = devmem[ki];
    if (!m.out_ctx) {                                   // sized for 64-byte contexts, whichever controller
        m.out_ctx = (volatile uint8_t*)dma_alloc(32 * 64, 64);
        m.in_ctx  = (volatile uint8_t*)dma_alloc(33 * 64, 64);
        m.reports = (volatile uint8_t*)dma_alloc(RING_TRBS * REPORT_MAX, 64);
        m.ep0     = (volatile Trb*)dma_alloc(RING_TRBS * sizeof(Trb), 64);
        m.intr    = (volatile Trb*)dma_alloc(RING_TRBS * sizeof(Trb), 64);
        m.bout    = (volatile Trb*)dma_alloc(RING_TRBS * sizeof(Trb), 64);
        m.iqh     = (volatile uint32_t*)dma_alloc(128, 64);
        m.itd     = (volatile uint32_t*)dma_alloc(64, 64);
        if (!m.out_ctx || !m.in_ctx || !m.reports || !m.ep0 || !m.intr || !m.bout || !m.iqh || !m.itd) {
            printf("USB: %s: out of memory\n", k.where);
            m.out_ctx = nullptr;
            return false;
        }
    }
    k.out_ctx = m.out_ctx; k.in_ctx = m.in_ctx; k.reports = m.reports;
    k.iqh = m.iqh; k.itd = m.itd;
    memset((void*)k.out_ctx, 0, 32 * 64);
    memset((void*)k.in_ctx, 0, 33 * 64);
    memset((void*)k.reports, 0, RING_TRBS * REPORT_MAX);
    k.rsize = 8;
    ring_attach(k.ep0, m.ep0);
    ring_attach(k.intr, m.intr);
    ring_attach(k.bout, m.bout);
    int mps0 = k.speed == 2 || k.speed == 1 ? 8 : k.speed == 3 ? 64 : 512;
    volatile uint32_t* ep0 = ctx(k.in_ctx, 2);
    uint64_t ic = phys(k.in_ctx);
    if (H->type != HC_XHCI) {                           // EHCI / UHCI: SET_ADDRESS
        if (!hc2_address(k)) { addr_failed = true; return false; }
    } else {
        H->dcbaa[k.slot] = phys(k.out_ctx);

        // Address Device: slot context + endpoint 0.
        ctx(k.in_ctx, 0)[1] = 0x3;                          // add slot + EP0
        fill_slot(k, 1);
        ep0[1] = 3 << 1 | 4 << 3 | (uint32_t)mps0 << 16;    // CErr 3, control, max packet
        uint64_t r0 = phys(k.ep0.trb) | 1;
        ep0[2] = (uint32_t)r0; ep0[3] = (uint32_t)(r0 >> 32);
        ep0[4] = 8;

        // First try the normal Address Device.
        // If it times out, try the BSR sequence that some broken sticks need:
        //   1. Address Device with BSR=1 (no SET_ADDRESS on the wire)
        //   2. short delay
        //   3. Address Device with BSR=0 (real SET_ADDRESS)
        int cc;
        if (k.tt_slot) {
            // Low/full speed behind a USB 2 hub's translator: as Linux and
            // Windows do, the slot first goes to the default state without
            // a SET_ADDRESS on the wire (BSR), the device descriptor's first
            // 8 bytes are read at address 0, and only then SET_ADDRESS.
            // Some keyboards and mice don't take a SET_ADDRESS that comes
            // first; done in one go, the controller waited on them for good.
            cc = command((uint32_t)ic, (uint32_t)(ic >> 32), 0,
                         TRB_ADDRESS_DEVICE << 10 | (1 << 9) | (uint32_t)k.slot << 24, nullptr);
            if (cc == 1) {
                static volatile uint8_t d8[8] __attribute__((aligned(64)));
                d8[7] = 0;
                bool got = control(k, 0x80, 6, 0x0100, 0, 8, d8);
                if (!got) printf("USB: %s: no descriptor at address 0 through the hub's translator (cc=%d)\n", k.where, ctl_cc);
                else if (d8[7] == 8 || d8[7] == 16 || d8[7] == 32 || d8[7] == 64)
                    ep0[1] = (ep0[1] & 0xFFFF) | (uint32_t)d8[7] << 16;
                // SET_ADDRESS: EP0 goes on from where its ring is now.
                uint64_t rq = phys(&k.ep0.trb[k.ep0.enq]) | k.ep0.cycle;
                ep0[2] = (uint32_t)rq; ep0[3] = (uint32_t)(rq >> 32);
                delay_ms(10);
                cc = command((uint32_t)ic, (uint32_t)(ic >> 32), 0,
                             TRB_ADDRESS_DEVICE << 10 | (uint32_t)k.slot << 24, nullptr);
            } else printf("USB: %s: Address Device (BSR) failed (cc=%d)\n", k.where, cc);
        } else
            cc = command((uint32_t)ic, (uint32_t)(ic >> 32), 0,
                         TRB_ADDRESS_DEVICE << 10 | (uint32_t)k.slot << 24, nullptr);
        if (cc != 1 && !k.tt_slot) {
            printf("USB: %s: Address Device failed (cc=%d), trying BSR sequence\n",
                   k.where, cc);
            // BSR = 1
            cc = command((uint32_t)ic, (uint32_t)(ic >> 32), 0,
                         TRB_ADDRESS_DEVICE << 10 | (1 << 9) | (uint32_t)k.slot << 24, nullptr);
            if (cc == 1) {
                delay_ms(20);
                // BSR = 0 (real SET_ADDRESS)
                cc = command((uint32_t)ic, (uint32_t)(ic >> 32), 0,
                             TRB_ADDRESS_DEVICE << 10 | (uint32_t)k.slot << 24, nullptr);
            }
        }
        if (cc != 1) {
            printf("USB: %s: address failed (cc=%d, speed=%s)\n",
                   k.where, cc, speed_name(k.speed));
            addr_cc = cc;
            // What the controller was given, and (behind a hub) what it
            // holds for that hub: route, root port, translator, hub bits.
            volatile uint32_t* sl = ctx(k.in_ctx, 1);
            printf("USB: %s: slot %d: route %x, root port %u, TT hub slot %u port %u (in: %08x %08x %08x)\n",
                   k.where, k.slot, sl[0] & 0xFFFFF, (sl[1] >> 16) & 0xFF, sl[2] & 0xFF, (sl[2] >> 8) & 0xFF,
                   sl[0], sl[1], sl[2]);
            if (k.parent >= 0) {
                const Keyboard& h = kbds[k.parent];
                volatile uint32_t* hs = ctx(h.out_ctx, 0);
                printf("USB: %s: its hub (slot %d, %s speed): hub bit %u, MTT %u, %u ports, TTT %u, state %u (out: %08x %08x %08x %08x)\n",
                       k.where, h.slot, speed_name(h.speed), (hs[0] >> 26) & 1, (hs[0] >> 25) & 1, hs[1] >> 24,
                       (hs[2] >> 16) & 3, hs[3] >> 27, hs[0], hs[1], hs[2], hs[3]);
            }
            addr_failed = true;
            return false;
        }
        delay_ms(10);
    }

    // Room for a headset's configuration (a microphone and several formats
    // run past 256 bytes, and what didn't fit was never seen).
    static volatile uint8_t desc[1024] __attribute__((aligned(64)));
    if (!control(k, 0x80, 6, 0x0100, 0, 8, desc)) { printf("USB: %s: no descriptor\n", k.where); return false; }
    int mps = desc[7];
    bool hub = desc[4] == 9;                            // device class: hub
    if (H->type == HC_XHCI && mps && mps != mps0 && k.speed < 4) {   // fix EP0 max packet size
        memset((void*)ctx(k.in_ctx, 0), 0, H->csz);
        ctx(k.in_ctx, 0)[1] = 0x2;
        ep0[1] = (ep0[1] & 0xFFFF) | (uint32_t)mps << 16;
        int ecc = command((uint32_t)ic, (uint32_t)(ic >> 32), 0, TRB_EVALUATE_CTX << 10 | (uint32_t)k.slot << 24, nullptr);
        if (ecc != 1) printf("USB: %s: Evaluate Context (EP0 packets of %d) failed (cc=%d)\n", k.where, mps, ecc);
    }
    uint16_t vid = 0, pid = 0;
    int nconf = 1;
    if (control(k, 0x80, 6, 0x0100, 0, 18, desc)) {
        vid = desc[8] | desc[9] << 8; pid = desc[10] | desc[11] << 8; nconf = desc[17];
        if (Seen* e = seen_cur) {
            e->have_ids = true; e->vid = vid; e->pid = pid; e->bcd_usb = (uint16_t)(desc[2] | desc[3] << 8);
            e->dclass = desc[4]; e->dsub = desc[5]; e->dproto = desc[6]; e->nconf = desc[17];
        }
    }
    if (!control(k, 0x80, 6, 0x0200, 0, 9, desc)) {
        printf("USB: %s: %04x:%04x: no configuration descriptor (cc=%d)\n", k.where, vid, pid, ctl_cc);
        return false;
    }
    int total = desc[2] | desc[3] << 8;
    if (total > (int)sizeof(desc)) {
        printf("USB: %s: %04x:%04x: configuration of %d bytes, only the first %d read\n", k.where, vid, pid, total, (int)sizeof(desc));
        total = sizeof(desc);
    }
    if (!control(k, 0x80, 6, 0x0200, 0, total, desc)) {
        printf("USB: %s: %04x:%04x: configuration (%d bytes) not read (cc=%d)\n", k.where, vid, pid, total, ctl_cc);
        return false;
    }
    int config = desc[5];
    if (Seen* e = seen_cur) {                           // the first interface's class (LSUSB)
        e->nifaces = desc[4];
        for (int i = 0; i + 1 < total && desc[i] >= 2; i += desc[i])
            if (desc[i + 1] == 4 && i + 7 < total) {
                e->have_iface = true; e->iclass = desc[i + 5]; e->isub = desc[i + 6]; e->iproto = desc[i + 7];
                break;
            }
    }

    // A boot keyboard or mouse interface, or a hub's, and its interrupt IN endpoint.
    int iface = -1, ep_addr = 0, ep_mps = 8, ep_interval = 10;
    bool in_iface = false, is_mouse = false;
    int ms_iface = -1, ms_in = 0, ms_out = 0, ms_mps = 512, ms_sub = 6;
    bool in_ms = false;
    // Any other HID interface (a gamepad?): its report descriptor's length, interrupt IN endpoint.
    int hid_iface = -1, hid_len = 0, hid_ep = 0, hid_mps = 8, hid_interval = 10;
    bool in_hid = false;
    for (int i = 0; i + 1 < total && desc[i] >= 2; i += desc[i]) {
        uint8_t type = desc[i + 1];
        if (type == 4) {
            in_hid = !hub && hid_iface < 0 && desc[i + 5] == 3 && !(desc[i + 6] == 1 && (desc[i + 7] == 1 || desc[i + 7] == 2));
            if (in_hid) hid_iface = desc[i + 2];
        } else if (type == 0x21 && in_hid && i + 8 < total) {
            hid_len = desc[i + 7] | desc[i + 8] << 8;                   // the first class descriptor: the report descriptor
        } else if (type == 5 && in_hid && !hid_ep && (desc[i + 2] & 0x80) && (desc[i + 3] & 3) == 3) {
            hid_ep = desc[i + 2];
            hid_mps = (desc[i + 4] | desc[i + 5] << 8) & 0x7FF;
            hid_interval = desc[i + 6];
        }
        if (type == 4) {                                // mass storage, bulk-only transport
            in_ms = !hub && ms_iface < 0 && desc[i + 5] == 8 && desc[i + 7] == 0x50;
            if (in_ms) { ms_iface = desc[i + 2]; ms_sub = desc[i + 6]; }
        }
        if (type == 5 && in_ms && (desc[i + 3] & 3) == 2) {
            int a = desc[i + 2], mp = (desc[i + 4] | desc[i + 5] << 8) & 0x7FF;
            if (a & 0x80) { if (!ms_in) ms_in = a; } else if (!ms_out) ms_out = a;
            ms_mps = mp;
        }
        if (type == 4) {
            if (hub) in_iface = iface < 0 && desc[i + 5] == 9;
            // HID boot interface: protocol 1 = keyboard, 2 = mouse.
            else in_iface = iface < 0 && desc[i + 5] == 3 && desc[i + 6] == 1 && (desc[i + 7] == 1 || desc[i + 7] == 2);
            if (in_iface) { iface = desc[i + 2]; is_mouse = !hub && desc[i + 7] == 2; }
        } else if (type == 5 && in_iface && !ep_addr && (desc[i + 2] & 0x80) && (desc[i + 3] & 3) == 3) {
            ep_addr = desc[i + 2];
            ep_mps = (desc[i + 4] | desc[i + 5] << 8) & 0x7FF;
            ep_interval = desc[i + 6];
        }
    }
    if (ms_iface < 0 && !hub) {                         // speakers / headphones (a headset's buttons are left alone)
        bool audio = false;
        for (int i = 0; i + 1 < total && desc[i] >= 2; i += desc[i])
            if (desc[i + 1] == 4 && desc[i + 5] == 1) audio = true;
        if (audio) return ua_setup(k, desc, total, config, (uint16_t)vid, (uint16_t)pid);
    }
    if (ms_iface >= 0 && ms_in && ms_out) {
        if (!control(k, 0x00, 9, config, 0, 0, nullptr)) return false;      // SET_CONFIGURATION
        k.iface = ms_iface; k.subclass = ms_sub; k.ep_in = ms_in; k.ep_out = ms_out; k.mps = ms_mps;
        return msd_setup(k);
    }
    if ((iface < 0 || !ep_addr) && hid_iface >= 0 && hid_ep && hid_len) {   // a gamepad?
        if (!control(k, 0x00, 9, config, 0, 0, nullptr)) return false;      // SET_CONFIGURATION
        static volatile uint8_t rd[512] __attribute__((aligned(64)));
        int n = hid_len > (int)sizeof(rd) ? (int)sizeof(rd) : hid_len;
        if (!control(k, 0x81, 6, 0x2200, hid_iface, n, rd)) { printf("USB: %s: no HID report descriptor\n", k.where); return false; }
        bool pad = hid_parse(rd, n, k.lay);
        k.lay.dragonrise = vid == 0x0079 && pid == 0x0011;
        if (!pad && !k.lay.dragonrise) {
            printf("USB: %s: HID device %04x:%04x, not a keyboard, mouse or gamepad\n", k.where, vid, pid);
            return false;
        }
        int slot = -1;
        for (int s2 = 0; s2 < 2 && slot < 0; s2++) {
            bool used = false;
            for (auto& o : kbds) used |= o.active && o.kind == JOY && o.pad == s2;
            if (!used) slot = s2;
        }
        if (slot < 0) { printf("USB: %s: a third gamepad (the game port has two)\n", k.where); return false; }
        control(k, 0x21, 0x0A, 0, hid_iface, 0, nullptr);                  // SET_IDLE 0: reports on changes (may stall)
        k.rsize = hid_mps < 8 ? 8 : hid_mps > REPORT_MAX ? REPORT_MAX : hid_mps;
        if (!configure_intr(k, hid_ep, hid_mps, hid_interval)) return false;
        k.kind = JOY;
        k.pad = slot;
        k.active = true;
        for (int i = 0; i < 8; i++) queue_report(k);
        if (H->type == HC_XHCI) H->db[k.slot] = k.dci;
        joy_set(slot, 1, 128, 128, 0);
        printf("USB: gamepad %04x:%04x on %s: joystick %c (%s, slot %d, %s speed)\n", vid, pid, k.where, 'A' + slot,
               k.lay.dragonrise ? "SNES layout" : k.lay.hat.bit >= 0 && k.lay.x.bit < 0 ? "hat" : "X/Y", k.slot, speed_name(k.speed));
        return true;
    }
    if ((iface < 0 || !ep_addr) && !hub && un_probe(k, ki, desc, total, nconf, vid, pid)) return true;   // USB Ethernet?
    if (iface < 0 || !ep_addr) {
        int c0 = -1, c1 = 0, c2 = 0;                    // the first interface's class / subclass / protocol
        for (int i = 0; i + 1 < total && desc[i] >= 2; i += desc[i])
            if (desc[i + 1] == 4) { c0 = desc[i + 5]; c1 = desc[i + 6]; c2 = desc[i + 7]; break; }
        printf("USB: %s: not a keyboard, mouse, hub or mass storage (class %02x/%02x/%02x)\n", k.where, c0 & 0xFF, c1, c2);
        return false;
    }
    if (hub && k.depth >= 5) { printf("USB: %s: hubs nested too deep\n", k.where); return false; }

    if (!control(k, 0x00, 9, config, 0, 0, nullptr)) return false;          // SET_CONFIGURATION

    if (hub) {
        // Hub descriptor: port count, characteristics (TT think time), power-on delay.
        // A USB 3 hub's SuperSpeed half has its own descriptor (type 2Ah) and
        // must be told its depth; USB 3 sticks behind it show up only there.
        bool ss = k.speed >= 4;
        if (!control(k, 0xA0, 6, ss ? 0x2A00 : 0x2900, 0, ss ? 12 : 9, desc)) { printf("USB: %s: no hub descriptor\n", k.where); return false; }
        if (ss && !control(k, 0x20, 12, k.depth, 0, 0, nullptr)) { printf("USB: %s: SET_HUB_DEPTH failed\n", k.where); return false; }
        k.kind = HUB;
        k.nports = desc[2] > 31 ? 31 : desc[2];
        k.ttt = ss ? 0 : (desc[3] >> 5) & 3;
        k.power_good_ms = desc[5] * 2u;
        if (!configure_intr(k, ep_addr, ep_mps, ep_interval)) return false;
        k.active = true;
        for (int i = 0; i < 8; i++) queue_report(k);
        if (H->type == HC_XHCI) H->db[k.slot] = k.dci;
        printf("USB: hub on %s (%d ports, %s speed)\n", k.where, k.nports, speed_name(k.speed));
        for (int p = 1; p <= k.nports; p++) control(k, 0x23, 3, 8, p, 0, nullptr);   // SET_FEATURE PORT_POWER
        delay_ms((int)k.power_good_ms + 100);           // power good, then connect debounce
        for (int p = 1; p <= k.nports; p++) hub_port_change(ki, p);
        return true;
    }

    control(k, 0x21, 0x0B, 0, iface, 0, nullptr);                      // SET_PROTOCOL boot
    control(k, 0x21, 0x0A, 0, iface, 0, nullptr);                      // SET_IDLE (may stall)
    if (!configure_intr(k, ep_addr, ep_mps, ep_interval)) return false;

    k.kind = is_mouse ? MOUSE : KBD;
    k.mouse = is_mouse;
    if (is_mouse) mouse_usb_attached();
    k.active = true;
    for (int i = 0; i < 8; i++) queue_report(k);
    if (H->type == HC_XHCI) H->db[k.slot] = k.dci;
    printf("USB: %s on %s (slot %d, %s speed)\n", k.mouse ? "mouse" : "keyboard", k.where, k.slot, speed_name(k.speed));
    return true;
}

// Drop device i and everything plugged into it (if it's a hub).
static void forget(int i) {
    Keyboard& k = kbds[i];
    if (!k.active) return;
    H = k.hc;
    for (int j = 0; j < MAX_KBD; j++)
        if (kbds[j].active && kbds[j].parent == i) forget(j);
    if (k.kind == KBD || k.kind == MOUSE || k.kind == JOY) release_all(k);
    for (int j = 0; j < n_msd; j++) if (msd_dev[j] == i) msd_dev[j] = -1;     // its slot waits for it to come back
    if (k.kind == NET && un.dev == i) { un.dev = -1; un.gen++; }
    if (k.kind == AUDIO && ua.dev == i) {
        ua.running = false; ua.dev = -1; ua.gen++;
        if (H->type != HC_XHCI) ua2_unlink();
    }
    k.active = false;
    seen_drop(k.hc, k.where);
    if (H->type == HC_XHCI) command(0, 0, 0, 10 << 10 | (uint32_t)k.slot << 24, nullptr);   // disable slot
    else hc2_release(k);
    printf("USB: %s on %s unplugged\n", k.kind == HUB ? "hub" : k.kind == NET ? "Ethernet adapter" : k.kind == AUDIO ? "audio device" : k.kind == MSD ? (k.bsize == 2048 ? "CD/DVD drive" : "disk") :
           k.kind == JOY ? "gamepad" : k.mouse ? "mouse" : "keyboard", k.where);
}

// Port `port` of hub h: something plugged in or out (or the first look).
static void hub_port_change(int h, int port) {
    Keyboard& hb = kbds[h];
    H = hb.hc;
    static volatile uint8_t st[4] __attribute__((aligned(64)));
    if (!hb.active || !control(hb, 0xA3, 0, 0, port, 4, st)) return;            // GET_STATUS
    uint16_t status = st[0] | st[1] << 8, change = st[2] | st[3] << 8;
    // C_PORT_* features by change bit. A USB 3 hub's SuperSpeed ports have
    // no enable/suspend changes (bits 1, 2) but warm reset, link state and
    // config error ones (bits 5-7).
    bool ss = hb.speed >= 4;
    static const uint8_t clear2[8] = {16, 17, 18, 19, 20, 0, 0, 0};
    static const uint8_t clear3[8] = {16, 0, 0, 19, 20, 29, 25, 26};
    for (int b = 0; b < 8; b++) {
        uint8_t f = ss ? clear3[b] : clear2[b];
        if (f && (change & (1 << b))) control(hb, 0x23, 1, f, port, 0, nullptr);
    }
    int existing = -1;
    for (int j = 0; j < MAX_KBD; j++)
        if (kbds[j].active && kbds[j].parent == h && kbds[j].hub_port == port) existing = j;
    bool connected = status & 1;
    if (existing >= 0 && (!connected || (change & 1))) { forget(existing); existing = -1; }
    uint32_t bit = port < 32 ? 1u << port : 0;
    if (!connected && port < 32) { hb.given_up &= ~bit; hb.port_fails[port] = 0; }   // unplugged: a fresh start
    if (!connected) {                                   // whatever was there (driven or not) is gone
        char w[32];
        snprintf(w, sizeof w, "%s.%d", hb.where, port);
        seen_drop(hb.hc, w);
    }
    if (!connected || existing >= 0 || (hb.given_up & bit)) return;
    // A device that fails its setup again and again (an internal one that
    // never answers, and keeps changing its port's state) would be reset and
    // enumerated on every change, each time holding up the poll (and with it
    // sound) for a fraction of a second: three tries, then it's left alone
    // until it's unplugged.
    struct Tally {
        Keyboard& hb; int h, port; uint32_t bit;
        ~Tally() {
            if (port >= 32 || !hb.active) return;
            bool there = false;
            for (int j = 0; j < MAX_KBD; j++) there |= kbds[j].active && kbds[j].parent == h && kbds[j].hub_port == port;
            if (there) { hb.port_fails[port] = 0; return; }
            if (++hb.port_fails[port] >= 3) {
                hb.given_up |= bit;
                printf("USB: %s.%d: failed 3 times, left alone until it's unplugged\n", hb.where, port);
            }
        }
    } tally{hb, h, port, bit};

    // Reset the port, wait for it to finish, and see how fast the device is.
    // SuperSpeed ports too (see reset_port); a link stuck in SS.Inactive or
    // Compliance gets a warm (BH) reset.
    // A device that never answers Address Device (cc=-1) gets one more
    // try after a warm (BH) reset on a USB 3 hub, or another port reset on
    // a USB 2 one (warm reset is USB 3 only).
    int pls = (status >> 5) & 0xF;
    bool warm = ss && (pls == 6 || pls == 10);
    for (int attempt = 0; attempt < 2; attempt++) {
        H = hb.hc;
        if (!control(hb, 0x23, 3, warm ? 28 : 4, port, 0, nullptr)) return;  // SET_FEATURE (BH_)PORT_RESET
        bool done = false;
        for (int i = 0; i < 50 && !done; i++) {
            delay_ms(10);
            if (!control(hb, 0xA3, 0, 0, port, 4, st)) return;
            done = (st[2] | st[3] << 8) & (warm ? 3 << 4 : 1 << 4);           // C_(BH_)PORT_RESET
        }
        control(hb, 0x23, 1, 20, port, 0, nullptr);                             // clear C_PORT_RESET
        if (warm) control(hb, 0x23, 1, 29, port, 0, nullptr);                   // and C_BH_PORT_RESET
        status = st[0] | st[1] << 8;
        if (!done || !(status & 2)) { printf("USB: %s.%d: port didn't enable\n", hb.where, port); return; }
        delay_ms(10);                                                           // reset recovery
        int speed = ss ? 4                                                          // SuperSpeed
                  : (status & (1 << 9)) ? 2 : (status & (1 << 10)) ? 3 : 1;        // low / high / full
        addr_failed = false;
        addr_cc = 0;
        setup_device(hb.port, h, port, speed);
        if (!addr_failed || addr_cc != -1) return;
        if (attempt) { printf("USB: %s.%d: still no address after the %s\n", hb.where, port, warm ? "warm reset" : "port reset"); return; }
        warm = ss;
        if (warm) printf("USB: %s.%d: no answer to Address Device (cc=-1): doing a WARM RESET, then trying again\n", hb.where, port);
        else printf("USB: %s.%d: no answer to Address Device (cc=-1); warm reset is USB 3 only, "
                    "doing a port reset and trying again\n", hb.where, port);
    }
}

static void setup_port(int port) {
    for (auto& k : kbds) if (k.active && k.hc == H && k.port == port && k.parent < 0) return;
    if (!reset_port(port)) return;
    int speed = (portsc(port) >> 10) & 0xF;             // 1 FS, 2 LS, 3 HS, 4+ SS
    addr_failed = msd_stuck = false;
    addr_cc = 0;
    setup_device(port, -1, 0, speed);
    bool no_addr = addr_failed && addr_cc == -1;          // Address Device never completed
    if (no_addr && speed < 4) {
        // A USB 2 port has no warm reset (that's USB 3 only): the nearest
        // thing is another port reset, then one more try.
        printf("USB: port %d: no answer to Address Device (cc=-1); warm reset is USB 3 only, "
               "doing a port reset and trying again\n", port);
        addr_failed = false;
        addr_cc = 0;
        if (reset_port(port)) setup_device(port, -1, 0, (portsc(port) >> 10) & 0xF);
        if (addr_failed) printf("USB: port %d: still no address after the port reset\n", port);
        return;
    }
    if (speed >= 4 && ((msd_stuck && !addr_failed) || no_addr)) {
        // A USB 3 device that never answered Address Device (cc=-1), or a
        // stick that took its address but whose transfers never complete:
        // typically the one the firmware just booted from, still in the
        // state the firmware left it (plugged in again later, it works).
        // A warm reset, which a hot reset is not, starts it over; if it's
        // still stuck, it gets the USB 2 try below.
        uint32_t sc = portsc(port);
        if (no_addr) printf("USB: port %d: no answer to Address Device (cc=-1): doing a WARM RESET, then trying again\n", port);
        else printf("USB: port %d: mass-storage commands never completed: doing a WARM RESET, then trying again\n", port);
        addr_failed = false;
        addr_cc = 0;
        set_portsc(port, (sc & PORT_KEEP) | (1u << 31));          // WPR
        for (int i = 0; i < 500 && !(portsc(port) & ((1u << 19) | PORT_PRC)); i++) delay_ms(1);
        for (int i = 0; i < 100 && (portsc(port) & PORT_PR); i++) delay_ms(1);
        sc = portsc(port);
        set_portsc(port, (sc & PORT_KEEP) | (sc & PORT_CHANGES));
        delay_ms(100);
        if ((portsc(port) & (PORT_CCS | PORT_PED)) == (PORT_CCS | PORT_PED)) {
            speed = (portsc(port) >> 10) & 0xF;
            msd_stuck = false;
            setup_device(port, -1, 0, speed);
            if (addr_failed) printf("USB: port %d: still no address after the warm reset (cc=%d)\n", port, addr_cc);
        } else printf("USB: port %d: not enabled after the warm reset (PORTSC=%08x)\n", port, portsc(port));
        if (msd_stuck || !(portsc(port) & PORT_PED)) addr_failed = true;     // on to USB 2
    }
    if (!addr_failed || speed < 4) return;

    // SuperSpeed Address Device failed (or timed out).  Many sticks never
    // answer on the SS link but work fine at USB 2.  Force the link into
    // SS.Disabled so the device drops to the USB-2 companion of this port.
    // Then issue a normal port reset so a fresh Connect Status Change is
    // guaranteed to appear (some controllers stay quiet otherwise).

    uint32_t sc = portsc(port);
    printf("USB: port %d: SS address failed, forcing USB-2 (PORTSC=%08x)\n",
           port, sc);

    // 1. Put the SuperSpeed link into Disabled
    set_portsc(port, (sc & PORT_KEEP & ~(0xFu << 5)) | (4u << 5) | (1u << 16)); // PLS=Disabled, LWS
    delay_ms(50);                           // give the device time to drop SS

    // 2. Clear any change bits that may have appeared
    sc = portsc(port);
    set_portsc(port, (sc & PORT_KEEP) | (sc & PORT_CHANGES));

    // 3. Force a fresh USB-2 enumeration with a normal port reset
    sc = portsc(port);
    set_portsc(port, (sc & PORT_KEEP) | PORT_PR);
    for (int i = 0; i < 500 && !(portsc(port) & PORT_PRC); i++)
        delay_ms(1);
    for (int i = 0; i < 100 && (portsc(port) & PORT_PR); i++)
        delay_ms(1);

    sc = portsc(port);
    set_portsc(port, (sc & PORT_KEEP) | (sc & PORT_CHANGES));   // ack
    delay_ms(20);                                               // reset recovery

    if (portsc(port) & PORT_CCS) {
        int new_speed = (portsc(port) >> 10) & 0xF;
        printf("USB: port %d: device reappeared at %s speed after SS fallback\n",
               port, speed_name(new_speed));
        // Do **not** call setup_device here – let the normal Port Status
        // Change path in usb_poll / setup_port pick it up.  That keeps the
        // slot bookkeeping clean.
        H->port_dirty[port] = true;         // make sure usb_poll sees it
    } else {
        printf("USB: port %d: nothing after forced USB-2 fallback (PORTSC=%08x)\n",
               port, portsc(port));
    }
}

static bool usb_activity;                              // usb_poll handled a connect / disconnect
#include "usb2.inc"

// ---------------------------------------------------------------- init
static bool bios_handoff() {
    uint32_t xecp = (rd(H->cap, 0x10) >> 16) * 4;
    for (int guard = 0; xecp && guard < 64; guard++) {
        uint32_t v = rd(H->cap, xecp);
        if ((v & 0xFF) == 1) {                          // USB legacy support
            wr(H->cap, xecp, v | (1u << 24));              // OS owned
            for (int i = 0; i < 1000 && (rd(H->cap, xecp) & (1u << 16)); i++) delay_ms(1);
            if (rd(H->cap, xecp) & (1u << 16)) { printf("USB: BIOS won't release the controller\n"); return false; }
            wr(H->cap, xecp + 4, 0xE0000000);              // disable SMIs, clear their status
            return true;
        }
        uint32_t next = (v >> 8) & 0xFF;
        if (!next) break;
        xecp += next * 4;
    }
    return true;
}

static bool init_controller(const PciDevice& d) {
    uint32_t bar = pci_read(d, 0x10);
    uint64_t base = bar & 0xFFFFFFF0;
    if ((bar & 0x6) == 0x4) base |= (uint64_t)pci_read(d, 0x14) << 32;
    if (!base) { printf("USB: xHCI BAR not set\n"); return false; }
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x06);      // memory + bus master

    H->cap = (volatile uint8_t*)map_mmio64(base, 0x1000);
    if (!H->cap) { printf("USB: can't map the xHCI registers\n"); return false; }
    {                                                   // map all of them
        uint32_t ext = 0x10000, db = rd(H->cap, 0x14) & ~0x3u, rt = rd(H->cap, 0x18) & ~0x1Fu;
        if (db + 0x400 > ext) ext = db + 0x400;
        if (rt + 0x1000 > ext) ext = rt + 0x1000;
        H->cap = (volatile uint8_t*)map_mmio64(base, ext);
        if (!H->cap) { printf("USB: can't map the xHCI registers\n"); return false; }
    }
    H->op = H->cap + (rd(H->cap, 0x00) & 0xFF);
    H->rt = H->cap + (rd(H->cap, 0x18) & ~0x1Fu);
    H->db = (volatile uint32_t*)(H->cap + (rd(H->cap, 0x14) & ~0x3u));
    uint32_t hcs1 = rd(H->cap, 0x04), hcs2 = rd(H->cap, 0x08), hcc1 = rd(H->cap, 0x10);
    H->max_slots = hcs1 & 0xFF;
    H->num_ports = hcs1 >> 24;
    H->csz = (hcc1 & (1 << 2)) ? 64 : 32;
    if (H->max_slots > 32) H->max_slots = 32;

    if (!bios_handoff()) return false;

    // Intel 7/8/9-series chipsets start with the USB ports routed to their
    // EHCI controllers: switch the USB 3 (SuperSpeed) and USB 2 ports over to
    // xHCI, as Linux does (usb_enable_intel_xhci_ports).
    uint32_t id = pci_read(d, 0);
    if ((id & 0xFFFF) == 0x8086) {
        uint16_t dev = id >> 16;
        static const uint16_t routed[] = { 0x1E31, 0x8C31, 0x9C31, 0x8CB1, 0x9CB1, 0x9D2F, 0x0F35, 0x22B5 };
        for (uint16_t r : routed) if (dev == r) {
            pci_write(d, 0xD8, pci_read(d, 0xDC));      // USB3_PSSEN = USB3PRM
            pci_write(d, 0xD0, pci_read(d, 0xD4));      // XUSB2PR = XUSB2PRM
            printf("USB: Intel port routing switched to xHCI (ports %x / %x)\n", pci_read(d, 0xD8), pci_read(d, 0xD0));
        }
    }

    // Halt and reset.
    wr(H->op, 0x00, rd(H->op, 0x00) & ~1u);
    for (int i = 0; i < 100 && !(rd(H->op, 0x04) & 1); i++) delay_ms(1);
    wr(H->op, 0x00, 1 << 1);
    for (int i = 0; i < 1000 && (rd(H->op, 0x00) & (1 << 1)); i++) delay_ms(1);
    for (int i = 0; i < 1000 && (rd(H->op, 0x04) & (1 << 11)); i++) delay_ms(1);
    if (rd(H->op, 0x00) & (1 << 1)) { printf("USB: controller reset timed out\n"); return false; }

    // Device context array, with scratchpad buffers if the controller wants them.
    H->dcbaa = (volatile uint64_t*)dma_alloc((H->max_slots + 1) * 8, 64);
    int scratch = ((hcs2 >> 27) & 0x1F) | (((hcs2 >> 21) & 0x1F) << 5);
    if (scratch) {
        volatile uint64_t* arr = (volatile uint64_t*)dma_alloc(scratch * 8, 64);
        for (int i = 0; i < scratch; i++) {
            void* page = dma_alloc(4096, 4096);
            if (!arr || !page) { printf("USB: out of memory for scratchpad\n"); return false; }
            arr[i] = phys(page);
        }
        H->dcbaa[0] = phys(arr);
    }
    if (!H->dcbaa || !ring_init(H->cmd_ring)) return false;

    // One event ring segment.
    H->evt = (volatile Trb*)dma_alloc(EVT_TRBS * sizeof(Trb), 64);
    volatile uint64_t* erst = (volatile uint64_t*)dma_alloc(16, 64);
    if (!H->evt || !erst) return false;
    erst[0] = phys(H->evt);
    erst[1] = EVT_TRBS;

    wr(H->op, 0x38, H->max_slots);                            // CONFIG: slots enabled
    wr64(H->op, 0x30, phys(H->dcbaa));
    wr64(H->op, 0x18, phys(H->cmd_ring.trb) | 1);             // CRCR, cycle 1
    wr(H->rt, 0x28, 1);                                    // ERSTSZ
    wr64(H->rt, 0x38, phys(H->evt));                          // ERDP
    wr64(H->rt, 0x30, phys(erst));                         // ERSTBA
    wr(H->rt, 0x20, 0);                                    // IMAN: no interrupts, we poll
    wr(H->op, 0x00, 1);                                    // run
    for (int i = 0; i < 100 && (rd(H->op, 0x04) & 1); i++) delay_ms(1);

    for (int p = 1; p <= H->num_ports; p++) {              // make sure ports are powered
        uint32_t sc = portsc(p);
        if (!(sc & PORT_PP)) set_portsc(p, (sc & PORT_KEEP) | PORT_PP);
    }
    delay_ms(100);                                      // let devices connect
    return true;
}

// USB 3 ports left in SS.Inactive or Compliance (it happens after the
// controller reset, on real PCs) never report their device again until a
// warm reset, as Linux does. Each such port of controller H gets one.
static void warm_reset_stuck() {
    for (int p = 1; p <= H->num_ports; p++) {
        uint32_t sc = portsc(p);
        int pls = (sc >> 5) & 0xF;
        if (pls != 6 && pls != 10) continue;
        printf("USB: port %d: link %s, warm reset\n", p, pls == 6 ? "inactive" : "in compliance mode");
        set_portsc(p, (sc & PORT_KEEP) | (1u << 31));          // WPR
        for (int i = 0; i < 300 && !(portsc(p) & ((1u << 19) | PORT_PRC)); i++) delay_ms(1);
        sc = portsc(p);
        set_portsc(p, (sc & PORT_KEEP) | (sc & PORT_CHANGES));
        delay_ms(20);
        if (portsc(p) & PORT_CCS) {
            printf("USB: port %d: device connected (%s speed)\n", p, speed_name((portsc(p) >> 10) & 0xF));
            setup_port(p);
        } else printf("USB: port %d: nothing after the warm reset (link state %d)\n", p, (portsc(p) >> 5) & 0xF);
    }
}

// From disk.c while it waits for the boot stick: stuck USB 3 ports on every controller.
extern "C" void usb_kick_ports(void) {
    if (usb_busy || in_poll) return;
    in_poll = 1;
    for (int c = 0; c < num_hc; c++) { H = &hcs[c]; if (H->type == HC_XHCI) warm_reset_stuck(); }
    in_poll = 0;
}

bool usb_init(const char* cmdline) {
    for (const char* p = cmdline; p && *p; p++)
        if (strncmp(p, "usb=off", 7) == 0) { printf("USB: disabled\n"); return false; }
    calibrate_delay();
    pad_keys_option(cmdline);
    dbg(1, "USB: %u port 80h reads a millisecond\n", io_per_ms);
    // Every xHCI controller, not just the first: keyboards and mice may be
    // on an add-in card, or on the second of a board's two controllers.
    PciDevice d;
    for (int i = 0; num_hc < MAX_HC && pci_find_class(0x0C, 0x03, 0x30, &d, i); i++) {
        H = &hcs[num_hc];
        memset((void*)H, 0, sizeof *H);
        H->evt_cycle = 1;
        uint32_t vd = pci_read(d, 0);
        H->pci = d; H->vendor = (uint16_t)vd; H->device = (uint16_t)(vd >> 16);
        printf("USB: xHCI %d at PCI %02x:%02x.%x (%04x:%04x)\n", num_hc, d.bus, d.dev, d.fn, vd & 0xFFFF, vd >> 16);
        if (!init_controller(d)) continue;              // try the others
        num_hc++;
        for (int p = 1; p <= H->num_ports; p++)
            if (portsc(p) & PORT_CCS) {
                printf("USB: port %d: device connected (%s speed)\n", p, speed_name((portsc(p) >> 10) & 0xF));
                setup_port(p);
            }
        warm_reset_stuck();
        int n = 0;
        int nd = 0;
        for (auto& k : kbds) { n += k.active && k.hc == H && (k.kind == KBD || k.kind == MOUSE); nd += k.active && k.hc == H && k.kind == MSD; }
        printf("USB: xHCI %d with %d ports, %d keyboard/mouse device%s, %d disk%s\n", num_hc - 1, H->num_ports, n, n == 1 ? "" : "s",
               nd, nd == 1 ? "" : "s");
    }
    // EHCI (USB 2), then its UHCI or OHCI companions, which get the low- and
    // full-speed devices EHCI hands over (or, with no EHCI, everything).
    // ehci=off: leave them to the firmware, as before vmdos drove them (its
    // legacy USB support may then still type for keyboards on them).
    bool older_off = cmdline && strstr(cmdline, "ehci=off");
    if (older_off) printf("USB: EHCI, UHCI and OHCI left to the firmware (ehci=off)\n");
    static const struct { int progif, type; const char* name; } older[] = {
        { 0x20, HC_EHCI, "EHCI" }, { 0x00, HC_UHCI, "UHCI" }, { 0x10, HC_OHCI, "OHCI" } };
    for (auto& o : older)
        if (!older_off)
        for (int i = 0; num_hc < MAX_HC && pci_find_class(0x0C, 0x03, o.progif, &d, i); i++) {
            H = &hcs[num_hc];
            memset((void*)H, 0, sizeof *H);
            H->type = o.type;
            uint32_t vd = pci_read(d, 0);
            H->pci = d; H->vendor = (uint16_t)vd; H->device = (uint16_t)(vd >> 16);
            printf("USB: %s %d at PCI %02x:%02x.%x (%04x:%04x)\n", o.name, num_hc, d.bus, d.dev, d.fn, vd & 0xFFFF, vd >> 16);
            if (!(o.type == HC_EHCI ? e_init(d) : o.type == HC_UHCI ? u_init(d) : o_init(d))) continue;
            num_hc++;
        }
    for (int c = 0; c < num_hc; c++) {                  // their devices: EHCI's first (it hands some over)
        H = &hcs[c];
        if (H->type == HC_XHCI) continue;
        hc2_root_poll(true);
    }
    for (int c = 0; c < num_hc; c++) {
        H = &hcs[c];
        if (H->type == HC_XHCI) continue;
        int n = 0, nd = 0;
        for (auto& k : kbds) { n += k.active && k.hc == H && (k.kind == KBD || k.kind == MOUSE || k.kind == JOY); nd += k.active && k.hc == H && k.kind == MSD; }
        printf("USB: %s %d with %d ports, %d keyboard/mouse/gamepad device%s, %d disk%s\n", hc2_name(),
               c, H->num_ports, n, n == 1 ? "" : "s", nd, nd == 1 ? "" : "s");
    }
    if (!num_hc) { printf("USB: no USB controller\n"); return false; }
    return true;
}

void usb_poll() {
    if (usb_busy || in_poll) return;                    // a disk transfer is waiting on the event ring / re-entered
    in_poll = 1;
    for (int c = 0; c < num_hc; c++) {
        H = &hcs[c];
        if (H->type != HC_XHCI) { hc2_poll(); continue; }
        Trb e;
        for (int i = 0; i < 64 && next_event(&e); i++) dispatch(e);
        for (int p = 1; p <= H->num_ports; p++) {
            if (!H->port_dirty[p]) continue;
            H->port_dirty[p] = false;
            usb_activity = true;
            uint32_t sc = portsc(p);
            set_portsc(p, (sc & PORT_KEEP) | (sc & PORT_CHANGES));   // ack
            if (sc & PORT_CCS) {
                if (sc & PORT_CSC || !(sc & PORT_PED)) {
                    printf("USB: port %d: device connected (%s speed)\n", p, speed_name((sc >> 10) & 0xF));
                    setup_port(p);
                }
            } else {
                for (int i = 0; i < MAX_KBD; i++)
                    if (kbds[i].active && kbds[i].hc == H && kbds[i].port == p && kbds[i].parent < 0) forget(i);
                char w[16];
                snprintf(w, sizeof w, "port %d", p);
                seen_drop(H, w);
            }
        }
    }
    for (int h = 0; h < MAX_KBD; h++) {                 // hubs' ports (hub_port_change picks the controller)
        while (kbds[h].active && kbds[h].kind == HUB && kbds[h].dirty) {
            uint32_t d = kbds[h].dirty;
            kbds[h].dirty = 0;
            usb_activity = true;
            for (int p = 1; p <= kbds[h].nports; p++)
                if (d & (1u << p)) hub_port_change(h, p);
        }
    }
    in_poll = 0;
}

// Before cd.c counts the USB CD/DVD drives (once, as VMCD.SYS loads): let
// devices still connecting finish (a slow drive, or one coming back at USB 2
// speed), until nothing has happened for half a second, up to max_ms.
extern "C" void usb_settle(int max_ms) {
    if (!num_hc) return;
    int quiet = 0;
    for (int t = 0; t < max_ms && quiet < 500; t += 10) {
        usb_activity = false;
        usb_poll();
        quiet = usb_activity ? 0 : quiet + 10;
        delay_ms(10);
    }
}

// C entry points for the kernel.
extern "C" void usb_start(const char* cmdline) { usb_init(cmdline); }
extern "C" void usb_tick(void) { usb_poll(); }
extern "C" int usb_controllers(void) { int n = 0; for (int c = 0; c < num_hc; c++) n += hcs[c].type == HC_XHCI; return n; }
extern "C" int usb_quiet(void) { return !usb_busy && !in_poll; }       // no USB transfer under way

// CD/DVD drives. A SCSI command (cdb; its length from the opcode group)
// with bytes (<= 64 KiB) of data in to buf. 0, or the sense key of the
// failure (sense data is read right away), or -1 (transport error, or the
// USB stack busy: try again later). An unplugged drive reads as empty.
extern "C" int usb_cd_count(void) { return n_ucd; }
extern "C" const char* usb_cd_model(int i) { return i < n_ucd && ucd_is(i) ? kbds[ucd_dev[i]].model : "(unplugged)"; }
extern "C" int usb_cd_asc(void) { return ucd_asc; }
extern "C" int usb_cd_packet(int i, const uint8_t* cdb, void* buf, uint32_t bytes) {
    ucd_asc = 0;
    if (i >= n_ucd || bytes > 65536) return -1;
    if (!ucd_is(i)) { ucd_asc = 0x3A; return 2; }              // gone: "medium not present"
    Keyboard& k = kbds[ucd_dev[i]];
    if (usb_busy) return -1;
    Hc* saved = H;
    usb_busy = 1;
    H = k.hc;
    int len = cdb[0] < 0x20 ? 6 : cdb[0] < 0x60 ? 10 : 12;
    int r = scsi(k, cdb, len, true, bytes), key = 0;
    if (r == 0 && bytes) memcpy(buf, (const void*)msd_buf, bytes);
    if (r != 0) {
        uint8_t rs[6] = { 0x03, 0, 0, 0, 18, 0 };
        msd_buf[0] = 0;
        if (scsi(k, rs, 6, true, 18) == 0 && (msd_buf[0] & 0x7E) == 0x70) { key = msd_buf[2] & 0xF; ucd_asc = msd_buf[12]; }
    }
    H = saved;
    usb_busy = 0;
    return r == 0 ? 0 : key ? key : -1;
}

// ---------------------------------------------------------------- LSUSB
// The controllers and every device seen on them, as text (CR LF lines) for
// LSUSB.COM (INT 2Fh AX=5649h, sysinfo.cpp). verbose: descriptor classes too.
static const char* usb_class_name(int c) {
    switch (c) {
    case 0x00: return "(per interface)";
    case 0x01: return "audio";
    case 0x02: return "communications";
    case 0x03: return "HID";
    case 0x05: return "physical";
    case 0x06: return "imaging";
    case 0x07: return "printer";
    case 0x08: return "mass storage";
    case 0x09: return "hub";
    case 0x0A: return "CDC data";
    case 0x0B: return "smart card";
    case 0x0D: return "content security";
    case 0x0E: return "video";
    case 0x0F: return "personal healthcare";
    case 0x10: return "audio/video";
    case 0x11: return "billboard";
    case 0xDC: return "diagnostic";
    case 0xE0: return "wireless (Bluetooth?)";
    case 0xEF: return "miscellaneous";
    case 0xFE: return "application specific";
    case 0xFF: return "vendor specific";
    }
    return "unknown class";
}

namespace {
struct Out {
    char* p; int max, n;
    void indent(int n) { while (n-- > 0) add(" "); }
    void add(const char* fmt, ...) {
        if (n >= max - 1) return;
        va_list ap;
        va_start(ap, fmt);
        int w = vsnprintf(p + n, (size_t)(max - n), fmt, ap);
        va_end(ap);
        n += w < 0 ? 0 : w;
        if (n > max - 1) n = max - 1;
    }
};
}

static const char* hc_type_name(int t) { return t == HC_XHCI ? "xHCI" : t == HC_EHCI ? "EHCI" : t == HC_OHCI ? "OHCI" : "UHCI"; }

extern "C" int usb_list(char* buf, int max, int verbose)
{
    Out o{buf, max, 0};
    if (!num_hc) { o.add("No USB controller driven by vmdos (usb=off, or none found)\r\n"); return o.n; }
    for (int c = 0; c < num_hc; c++) {
        Hc& h = hcs[c];
        int nd = 0;
        for (auto& e : seen) nd += e.used && e.hc == &h;
        o.add("Bus %03d: %s at PCI %02x:%02x.%x [%04x:%04x], %d port%s, %d device%s\r\n", c + 1, hc_type_name(h.type),
              h.pci.bus, h.pci.dev, h.pci.fn, h.vendor, h.device, h.num_ports, h.num_ports == 1 ? "" : "s", nd, nd == 1 ? "" : "s");
        // in port order: a hub's devices right after it
        bool done[MAX_SEEN] = {};
        for (;;) {
            int best = -1;
            for (int i = 0; i < MAX_SEEN; i++) {
                const Seen& e = seen[i];
                if (!e.used || e.hc != &h || done[i]) continue;
                if (best < 0) { best = i; continue; }
                // compare "port a.b.c" numerically, tier by tier
                const char *x = e.where + 5, *y = seen[best].where + 5;
                int cmp = 0;
                while (!cmp) {
                    int a = 0, b = 0;
                    while (*x >= '0' && *x <= '9') a = a * 10 + (*x++ - '0');
                    while (*y >= '0' && *y <= '9') b = b * 10 + (*y++ - '0');
                    if (a != b) { cmp = a < b ? -1 : 1; break; }
                    if (!*x || !*y) { cmp = !*x && *y ? -1 : *x && !*y ? 1 : 0; break; }
                    x++; y++;
                }
                if (cmp < 0) best = i;
            }
            if (best < 0) break;
            done[best] = true;
            const Seen& e = seen[best];
            int depth = 0;
            for (const char* q = e.where; *q; q++) depth += *q == '.';
            o.indent(2 + depth * 2);
            o.add("%c%s: ", e.where[0] - 32, e.where + 1);   // "Port 3.2"
            if (e.have_ids) o.add("ID %04x:%04x", e.vid, e.pid);
            else o.add("ID ????:????");
            o.add(", %s speed", speed_name(e.speed));
            const Keyboard& k = kbds[e.ki];
            bool driven = k.active && k.hc == e.hc && !strncmp(k.where, e.where, sizeof e.where);
            if (driven) {
                switch (k.kind) {
                case KBD: o.add(", keyboard"); break;
                case MOUSE: o.add(", mouse"); break;
                case HUB: o.add(", hub, %d ports", k.nports); break;
                case MSD: o.add(", %s: %s", k.bsize == 2048 ? "CD/DVD drive" : "disk", k.model); break;
                case JOY: o.add(", gamepad: joystick %c", 'A' + k.pad); break;
                case AUDIO: o.add(", audio (vmdos's sound output)"); break;
                case NET: o.add(", Ethernet adapter"); break;
                }
            } else {
                int cl = e.have_ids && e.dclass ? e.dclass : e.have_iface ? e.iclass : -1;
                if (cl >= 0) o.add(", %s", usb_class_name(cl));
                o.add(e.have_ids ? ", not used by vmdos" : ", didn't answer (no descriptor)");
            }
            o.add("\r\n");
            if (verbose && e.have_ids) {                // "USB 2.00, class 00/00/00, interface 03/01/01 (HID), slot 3"
                o.indent(6 + depth * 2);
                o.add("USB %x.%02x, class %02x/%02x/%02x", e.bcd_usb >> 8, e.bcd_usb & 0xFF, e.dclass, e.dsub, e.dproto);
                if (e.have_iface) o.add(", interface %02x/%02x/%02x (%s)", e.iclass, e.isub, e.iproto, usb_class_name(e.iclass));
                if (driven) o.add(", %s %d%s", e.hc->type == HC_XHCI ? "slot" : "address", k.slot, k.tt_slot ? " via TT" : "");
                o.add("\r\n");
            }
        }
    }
    return o.n;
}
