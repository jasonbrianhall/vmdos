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
enum { HC_XHCI = 0, HC_EHCI = 1, HC_UHCI = 2 };       // EHCI / UHCI: usb2.inc
struct Hc {
    int type;
    // EHCI / UHCI
    uint16_t io;                           // UHCI I/O ports
    volatile uint8_t* eop;                 // EHCI operational registers
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
enum Kind { KBD, MOUSE, HUB, MSD, JOY };

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
    uint32_t power_good_ms;
    char where[24];                        // "port 3" / "port 3.2" for messages
    // mass storage
    int dci_out, ep_in, ep_out, iface;
    int subclass;                          // 2, 5 (ATAPI), 4 (UFI): commands padded to 12 bytes
    Ring bout;                             // bulk OUT (bulk IN uses intr)
    uint32_t tag, bsize;
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
static bool hc2_intr(Keyboard& k, int ep_addr, int ep_mps, int ep_interval);
static int hc2_bulk(Keyboard& k, bool in, volatile void* buf, uint32_t len, uint32_t* got);
static void hc2_clear_halt(Keyboard& k, bool in);
static bool hc2_address(Keyboard& k);
static void hc2_release(Keyboard& k);
static void hc2_poll();
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
    for (auto& k : kbds) {
        if (!k.active || k.hc != H || k.slot != slot || k.dci != ep || k.kind == MSD) continue;
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

// A control transfer on endpoint 0. Returns true on success.
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
    if (!wait_event(TRB_TRANSFER_EVENT, k.slot, 1, &e)) return false;
    int cc = e.d2 >> 24;
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
#define MSD_MAX 4
static int msd_dev[MSD_MAX], n_msd;
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
    if (cc != 1) { if (cc == 6) bot_reset(k); return -1; }
    if (len) {
        cc = bulk(k, in, msd_buf, len, nullptr);
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
    if (n_msd == MSD_MAX) return false;
    k.kind = MSD;
    if (!configure_bulk(k)) return false;
    k.active = true;
    msd_buf[0] = 0;
    uint8_t inq[6] = { 0x12, 0, 0, 0, 36, 0 };
    int ri = -1;
    for (int t = 0; t < 3 && ri != 0; t++) {                // a drive just plugged in may not answer at first
        msd_buf[0] = 0;
        ri = scsi(k, inq, 6, true, 36);
        if (ri) { request_sense(k); delay_ms(100); }
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
        else { request_sense(k); delay_ms(100); }
    }
    uint8_t rc[10] = { 0x25 };
    int r = -1;
    for (int i = 0; i < 3 && r != 0; i++) { r = scsi(k, rc, 10, true, 8); if (r) request_sense(k); }
    if (r != 0) { printf("USB: %s: %s: no capacity (no medium? ready: %d)\n", k.where, k.model, ready); k.active = false; return false; }
    uint32_t last = (uint32_t)msd_buf[0] << 24 | msd_buf[1] << 16 | msd_buf[2] << 8 | msd_buf[3];
    k.bsize = (uint32_t)msd_buf[4] << 24 | msd_buf[5] << 16 | msd_buf[6] << 8 | msd_buf[7];
    k.blocks = (uint64_t)last + 1;
    printf("USB: disk on %s: %s, %u MiB, %u-byte blocks (slot %d, %s speed)\n", k.where, k.model,
           (uint32_t)(k.blocks * k.bsize >> 20), k.bsize, k.slot, speed_name(k.speed));
    if (k.bsize != 512) { printf("USB: %s: only 512-byte blocks are supported\n", k.where); k.active = false; return false; }
    msd_dev[n_msd++] = (int)(&k - kbds);
    return true;
}

extern "C" int usb_msd_count(void) { return n_msd; }
extern "C" uint64_t usb_msd_sectors(int i) {
    if (i >= n_msd) return 0;
    Keyboard& k = kbds[msd_dev[i]];
    return k.active && k.kind == MSD ? k.blocks : 0;
}
extern "C" const char* usb_msd_name(int i) { return i < n_msd ? kbds[msd_dev[i]].model : ""; }

// count <= 128 sectors; buf anywhere (copied through the DMA buffer).
extern "C" int usb_msd_rw(int i, uint64_t lba, uint32_t count, void* buf, int write) {
    if (i >= n_msd || !count || count > 128) return -1;
    Keyboard& k = kbds[msd_dev[i]];
    if (!k.active || k.kind != MSD || lba + count > k.blocks || lba > 0xFFFFFFFFull) return -1;
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

// The rest of setting up a device that has slot k.slot: its memory, an
// address, and a keyboard, mouse or hub on top. False: not driven.
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
        int cc = command((uint32_t)ic, (uint32_t)(ic >> 32), 0,
                         TRB_ADDRESS_DEVICE << 10 | (uint32_t)k.slot << 24, nullptr);
        if (cc != 1) {
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
            addr_failed = true;
            return false;
        }
        delay_ms(10);
    }

    static volatile uint8_t desc[256] __attribute__((aligned(64)));
    if (!control(k, 0x80, 6, 0x0100, 0, 8, desc)) { printf("USB: %s: no descriptor\n", k.where); return false; }
    int mps = desc[7];
    bool hub = desc[4] == 9;                            // device class: hub
    if (H->type == HC_XHCI && mps && mps != mps0 && k.speed < 4) {   // fix EP0 max packet size
        memset((void*)ctx(k.in_ctx, 0), 0, H->csz);
        ctx(k.in_ctx, 0)[1] = 0x2;
        ep0[1] = (ep0[1] & 0xFFFF) | (uint32_t)mps << 16;
        command((uint32_t)ic, (uint32_t)(ic >> 32), 0, TRB_EVALUATE_CTX << 10 | (uint32_t)k.slot << 24, nullptr);
    }
    uint16_t vid = 0, pid = 0;
    if (control(k, 0x80, 6, 0x0100, 0, 18, desc)) { vid = desc[8] | desc[9] << 8; pid = desc[10] | desc[11] << 8; }
    if (!control(k, 0x80, 6, 0x0200, 0, 9, desc)) return false;
    int total = desc[2] | desc[3] << 8;
    if (total > (int)sizeof(desc)) total = sizeof(desc);
    if (!control(k, 0x80, 6, 0x0200, 0, total, desc)) return false;
    int config = desc[5];

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
    k.active = false;
    if (H->type == HC_XHCI) command(0, 0, 0, 10 << 10 | (uint32_t)k.slot << 24, nullptr);   // disable slot
    else hc2_release(k);
    printf("USB: %s on %s unplugged\n", k.kind == HUB ? "hub" : k.kind == MSD ? (k.bsize == 2048 ? "CD/DVD drive" : "disk") :
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
    if (!connected || existing >= 0) return;

    // Reset the port, wait for it to finish, and see how fast the device is.
    // SuperSpeed ports too (see reset_port); a link stuck in SS.Inactive or
    // Compliance gets a warm (BH) reset.
    int pls = (status >> 5) & 0xF;
    bool warm = ss && (pls == 6 || pls == 10);
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
    setup_device(hb.port, h, port, speed);
}

static void setup_port(int port) {
    for (auto& k : kbds) if (k.active && k.hc == H && k.port == port && k.parent < 0) return;
    if (!reset_port(port)) return;
    int speed = (portsc(port) >> 10) & 0xF;             // 1 FS, 2 LS, 3 HS, 4+ SS
    addr_failed = false;
    setup_device(port, -1, 0, speed);
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
    // EHCI (USB 2), then its UHCI companions, which get the low- and
    // full-speed devices EHCI hands over. (OHCI companions: not yet.)
    static const struct { int progif, type; const char* name; } older[] = { { 0x20, HC_EHCI, "EHCI" }, { 0x00, HC_UHCI, "UHCI" } };
    for (auto& o : older)
        for (int i = 0; num_hc < MAX_HC && pci_find_class(0x0C, 0x03, o.progif, &d, i); i++) {
            H = &hcs[num_hc];
            memset((void*)H, 0, sizeof *H);
            H->type = o.type;
            uint32_t vd = pci_read(d, 0);
            printf("USB: %s %d at PCI %02x:%02x.%x (%04x:%04x)\n", o.name, num_hc, d.bus, d.dev, d.fn, vd & 0xFFFF, vd >> 16);
            if (!(o.type == HC_EHCI ? e_init(d) : u_init(d))) continue;
            num_hc++;
        }
    for (int c = 0; c < num_hc; c++) {                  // their devices: EHCI's first (it hands some over)
        H = &hcs[c];
        if (H->type == HC_XHCI) continue;
        if (H->type == HC_EHCI) e_root_poll(true); else u_root_poll(true);
    }
    for (int c = 0; c < num_hc; c++) {
        H = &hcs[c];
        if (H->type == HC_XHCI) continue;
        int n = 0, nd = 0;
        for (auto& k : kbds) { n += k.active && k.hc == H && (k.kind == KBD || k.kind == MOUSE || k.kind == JOY); nd += k.active && k.hc == H && k.kind == MSD; }
        printf("USB: %s %d with %d ports, %d keyboard/mouse/gamepad device%s, %d disk%s\n", H->type == HC_EHCI ? "EHCI" : "UHCI",
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
