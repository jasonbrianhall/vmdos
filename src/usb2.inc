// EHCI (USB 2), UHCI and OHCI (USB 1.1) host controllers, under the same device
// code as xHCI (included from usb.cpp): keyboards, mice, gamepads, hubs and
// mass storage work the same on any of them, and USB audio (at the end).
//
// Older PCs (and QEMU/libvirt's default USB) have an EHCI controller for
// high-speed devices with UHCI "companion" controllers for the low- and
// full-speed ones (keyboards, mice, gamepads): EHCI hands those ports
// over. Later boards without companions put a hub in between, whose
// transaction translator EHCI reaches with split transactions.
//
// Everything is polled. Control and bulk transfers run one at a time and
// are waited for; each interrupt endpoint (reports, hub status) has a queue
// head with one transfer descriptor that usb_poll checks and re-arms.

#define PID_SETUP 0x2D
#define PID_IN    0x69
#define PID_OUT   0xE1

static uint8_t alloc_addr() {
    for (int a = 1; a < 128; a++) if (!H->addr_used[a]) { H->addr_used[a] = 1; return (uint8_t)a; }
    return 0;
}

// The setup packet in the controller's own buffer.
static void put_setup(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len) {
    volatile uint8_t* s = H->sbuf;
    s[0] = type; s[1] = req; s[2] = (uint8_t)value; s[3] = (uint8_t)(value >> 8);
    s[4] = (uint8_t)index; s[5] = (uint8_t)(index >> 8); s[6] = (uint8_t)len; s[7] = (uint8_t)(len >> 8);
}

// Packets a transfer took, for the data toggle: all of len, or up to a short one.
static int packets(uint32_t len, uint32_t got, int mps, bool short_pkt) {
    if (short_pkt) return (int)(got / (uint32_t)mps) + 1;
    return len ? (int)((len + mps - 1) / (uint32_t)mps) : 1;
}

// ================================================================ UHCI
#define UHCI_TDS 1100                               // a 64 KiB bulk transfer at 64 bytes a packet, and some
static const uint32_t U_ACTIVE = 1u << 23, U_STALL = 1u << 22, U_DBE = 1u << 21, U_BABBLE = 1u << 20,
    U_CRC = 1u << 18, U_BSE = 1u << 17, U_ERRS = U_STALL | U_DBE | U_BABBLE | U_CRC | U_BSE,
    U_LS = 1u << 26, U_IOC = 1u << 24, U_SPD = 1u << 29, U_CERR3 = 3u << 27;
static inline uint16_t uin(int r) { return inw((uint16_t)(H->io + r)); }
static inline void uout(int r, uint16_t v) { outw((uint16_t)(H->io + r), v); }
static inline uint16_t uport(int p) { return uin(0x10 + 2 * (p - 1)); }
static inline void uport_w(int p, uint16_t v) { uout(0x10 + 2 * (p - 1), v); }      // CSC, PEC: write 1 to clear
static volatile uint32_t* u_td(int i) { return H->tds + i * 8; }               // 32 bytes a TD
static uint32_t u_token(int pid, int addr, int ep, int tog, int len) {
    return (uint32_t)pid | (uint32_t)addr << 8 | (uint32_t)ep << 15 | (uint32_t)tog << 19 |
           (uint32_t)((len ? len - 1 : 0x7FF) & 0x7FF) << 21;
}
static int u_actlen(uint32_t st) { return (int)((st + 1) & 0x7FF); }

// TD i: one packet. The chain is depth first; the last one ends it.
static void u_fill(int i, bool last, uint32_t status, uint32_t token, uint64_t buf) {
    volatile uint32_t* t = u_td(i);
    t[0] = last ? 1 : (uint32_t)phys(u_td(i + 1)) | 4;
    t[3] = (uint32_t)buf;
    t[2] = token;
    barrier();
    t[1] = status;
}

// Run TDs 0..n-1 on the control/bulk queue head. status_td: for a control
// transfer, where a short IN packet continues (-1: a short packet ends the
// transfer). 1 done, 13 short, 6 stall, -1 error or timeout; *got bytes of
// data (TDs data_from..data_to).
static int u_run(int n, int status_td, int data_from, int data_to, int timeout_ms, uint32_t* got, int* done_tds) {
    volatile uint32_t* q = H->qh_ctl;
    barrier();
    q[1] = (uint32_t)phys(u_td(0));
    int i = 0, rc = 1;
    uint32_t bytes = 0;
    for (int t = 0; i < n; ) {
        uint32_t st = u_td(i)[1];
        if (st & U_ACTIVE) {
            if (++t > timeout_ms * 100) { rc = -1; break; }
            io_delay(10);
            continue;
        }
        if (st & U_ERRS) { rc = (st & U_STALL) && !(st & (U_CRC | U_BABBLE | U_DBE)) ? 6 : -1; break; }
        uint32_t tok = u_td(i)[2];
        int want = (int)((tok >> 21) + 1) & 0x7FF, len = u_actlen(st);
        if (i >= data_from && i <= data_to) bytes += (uint32_t)len;
        i++;
        if ((tok & 0xFF) == PID_IN && len < want && i - 1 >= data_from && i - 1 <= data_to) {   // short packet
            if (status_td < 0) { rc = 13; break; }
            i = status_td;                                     // on to the status stage
            q[1] = (uint32_t)phys(u_td(i));
        }
    }
    q[1] = 1;                                                  // the queue is empty again
    if (got) *got = bytes;
    if (done_tds) *done_tds = i;
    return rc;
}

static bool u_control(Keyboard& k, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, volatile void* buf) {
    bool in = type & 0x80;
    uint32_t ls = k.speed == 2 ? U_LS : 0, st = U_ACTIVE | U_CERR3 | ls;
    put_setup(type, req, value, index, len);
    int n = 0, mps = k.mps0 ? k.mps0 : 8;
    u_fill(n++, false, st, u_token(PID_SETUP, k.addr, 0, 0, 8), phys(H->sbuf));
    int data_from = n, tog = 1;
    for (uint32_t off = 0; off < len; off += (uint32_t)mps) {
        if (n >= UHCI_TDS - 1) return false;
        int l = len - off < (uint32_t)mps ? (int)(len - off) : mps;
        u_fill(n++, false, st | (in ? U_SPD : 0), u_token(in ? PID_IN : PID_OUT, k.addr, 0, tog, l), phys(buf) + off);
        tog ^= 1;
    }
    int data_to = n - 1, status_td = n;
    u_fill(n++, true, st | U_IOC, u_token(len && in ? PID_OUT : PID_IN, k.addr, 0, 1, 0), 0);
    int rc = u_run(n, status_td, data_from, data_to, 500, nullptr, nullptr);
    ctl_cc = rc;
    return rc == 1 || rc == 13;
}

static int u_bulk(Keyboard& k, bool in, volatile void* buf, uint32_t len, uint32_t* got) {
    int ep = (in ? k.ep_in : k.ep_out) & 0xF, mps = k.mps ? k.mps : 64;
    uint8_t& tog = in ? k.tog_in : k.tog_out;
    int n = len ? (int)((len + mps - 1) / (uint32_t)mps) : 1;
    if (n > UHCI_TDS) return -1;
    uint32_t st = U_ACTIVE | U_CERR3 | (k.speed == 2 ? U_LS : 0) | (in ? U_SPD : 0);
    int t = tog;
    for (int i = 0; i < n; i++) {
        uint32_t off = (uint32_t)i * mps;
        int l = len - off < (uint32_t)mps ? (int)(len - off) : mps;
        u_fill(i, i == n - 1, st, u_token(in ? PID_IN : PID_OUT, k.addr, ep, t, l), phys(buf) + off);
        t ^= 1;
    }
    uint32_t bytes = 0;
    int done = 0;
    int rc = u_run(n, -1, 0, n - 1, 5000, &bytes, &done);
    if (rc == 1 || rc == 13) tog = (uint8_t)(tog ^ (done & 1));
    if (got) *got = bytes;
    return rc;
}

// The interrupt queue heads hang between qh_int (every frame) and qh_ctl.
static void u_intr_arm(Keyboard& k) {
    volatile uint32_t* t = k.itd;
    t[0] = 1;
    t[3] = (uint32_t)phys(k.reports);
    t[2] = u_token(PID_IN, k.addr, k.dci, k.itog, k.ilen);
    barrier();
    t[1] = U_ACTIVE | U_CERR3 | (k.speed == 2 ? U_LS : 0);
    barrier();
    k.iqh[1] = (uint32_t)phys(t);
}

static bool u_init(const PciDevice& d) {
    uint32_t bar = pci_read(d, 0x20);
    if (!(bar & 1) || !(bar & 0xFFE0)) { printf("USB: UHCI I/O BAR not set\n"); return false; }
    H->io = (uint16_t)(bar & 0xFFE0);
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x05);       // I/O space, bus master
    uint32_t leg = pci_read(d, 0xC0);                    // legacy support: no BIOS keyboard emulation, no SMIs
    pci_write(d, 0xC0, (leg & 0xFFFF0000u) | 0x8F00);
    uout(0, 0);                                         // stop
    for (int i = 0; i < 20 && !(uin(2) & 0x20); i++) delay_ms(1);
    uout(0, 4); delay_ms(50); uout(0, 0); delay_ms(10); // global reset
    uout(0, 2);                                         // host controller reset
    for (int i = 0; i < 100 && (uin(0) & 2); i++) delay_ms(1);
    uout(4, 0);                                         // no interrupts: polled
    H->frames = (volatile uint32_t*)dma_alloc(4096, 4096);
    H->qh_int = (volatile uint32_t*)dma_alloc(32, 32);
    H->qh_ctl = (volatile uint32_t*)dma_alloc(32, 32);
    H->tds = (volatile uint32_t*)dma_alloc(UHCI_TDS * 32, 32);
    H->sbuf = (volatile uint8_t*)dma_alloc(64, 16);
    if (!H->frames || !H->qh_int || !H->qh_ctl || !H->tds || !H->sbuf) { printf("USB: out of memory\n"); return false; }
    H->qh_ctl[0] = 1; H->qh_ctl[1] = 1;
    H->qh_int[0] = (uint32_t)phys(H->qh_ctl) | 2; H->qh_int[1] = 1;
    for (int i = 0; i < 1024; i++) H->frames[i] = (uint32_t)phys(H->qh_int) | 2;
    outl((uint16_t)(H->io + 8), (uint32_t)phys(H->frames));
    uout(6, 0);
    outb((uint16_t)(H->io + 0x0C), 64);                 // start of frame: 1 ms
    uout(2, 0xFFFF);                                    // clear the status bits
    uout(0, 0x80 | 0x40 | 1);                           // 64-byte packets, configured, run
    H->num_ports = 2;                                   // more where bit 7 reads 1, as Linux checks
    for (int p = 3; p <= 8; p++) { uint16_t v = uport(p); if (v == 0xFFFF || !(v & 0x80)) break; H->num_ports = p; }
    return true;
}

// Reset root port p and enable it; false: nothing (left) there.
static bool u_reset_port(int p) {
    uint16_t v = uport(p);
    if (!(v & 1)) return false;
    uport_w(p, (uint16_t)((v & ~0x000A) | 0x200));      // reset
    delay_ms(50);
    uport_w(p, (uint16_t)(uport(p) & ~0x020A));
    delay_ms(1);
    for (int i = 0; i < 10; i++) {
        v = uport(p);
        if (!(v & 1)) return false;
        if (v & 0x000A) { uport_w(p, v); continue; }    // ack connect / enable changes
        if (v & 4) break;
        uport_w(p, (uint16_t)((v & ~0x000A) | 4));      // enable
        delay_ms(10);
    }
    delay_ms(10);                                       // reset recovery
    return uport(p) & 4;
}

static void hc2_forget_port(int p) {
    for (int i = 0; i < MAX_KBD; i++)
        if (kbds[i].active && kbds[i].hc == H && kbds[i].port == p && kbds[i].parent < 0) forget(i);
}

static void u_root_poll(bool first) {
    for (int p = 1; p <= H->num_ports; p++) {
        uint16_t v = uport(p);
        if (!(v & 2) && !(first && (v & 1))) continue;   // no connect change (or the first look)
        uport_w(p, (uint16_t)((v & ~0x000A) | (v & 0x000A)));   // ack
        usb_activity = true;
        hc2_forget_port(p);
        if (!(v & 1)) continue;
        delay_ms(100);                                  // connect debounce
        if (!u_reset_port(p)) continue;
        int speed = (uport(p) & 0x100) ? 2 : 1;         // low / full
        printf("USB: UHCI port %d: device connected (%s speed)\n", p, speed_name(speed));
        setup_device(p, -1, 0, speed);
    }
}

// ================================================================ EHCI
#define EHCI_QTDS 16
static const uint32_t E_ACTIVE = 0x80, E_HALTED = 0x40, E_DBE = 0x20, E_BABBLE = 0x10, E_XACT = 0x08;
static inline uint32_t erd(int o) { return rd(H->eop, (uint32_t)o); }
static inline void ewr(int o, uint32_t v) { wr(H->eop, (uint32_t)o, v); }
static inline uint32_t eport(int p) { return erd(0x44 + 4 * (p - 1)); }
static inline void eport_w(int p, uint32_t v) { ewr(0x44 + 4 * (p - 1), v); }      // CSC, PEC, OCC: write 1 to clear
#define E_WC 0x2Au
static volatile uint32_t* e_qtd(int i) { return H->tds + i * 16; }              // 64 bytes a qTD (64-bit fields too)

static void e_fill(int i, int next, int alt, int pid, int tog, uint64_t buf, uint32_t len, bool ioc) {
    volatile uint32_t* q = e_qtd(i);
    q[0] = next < 0 ? 1 : (uint32_t)phys(e_qtd(next));
    q[1] = alt < 0 ? 1 : (uint32_t)phys(e_qtd(alt));
    q[3] = (uint32_t)buf;
    for (int j = 1; j < 5; j++) q[3 + j] = (uint32_t)((buf & ~0xFFFull) + 0x1000ull * j);
    for (int j = 0; j < 5; j++) q[8 + j] = 0;           // upper halves (64-bit controllers)
    barrier();
    q[2] = E_ACTIVE | (uint32_t)pid << 8 | 3u << 10 | (ioc ? 1u << 15 : 0) | len << 16 | (uint32_t)tog << 31;
}

// Endpoint characteristics and capabilities of k's endpoint ep.
static uint32_t e_chars(const Keyboard& k, int ep, int mps, bool control, bool dtc) {
    uint32_t eps = k.speed == 3 ? 2 : k.speed == 2 ? 1 : 0;
    return (uint32_t)k.addr | (uint32_t)ep << 8 | eps << 12 | (dtc ? 1u << 14 : 0) | (uint32_t)mps << 16 |
           (control && k.speed != 3 ? 1u << 27 : 0);
}
static uint32_t e_caps(const Keyboard& k, bool periodic) {
    uint32_t c = 1u << 30;                              // one transaction a microframe
    if (k.speed != 3) c |= (uint32_t)k.tt_slot << 16 | (uint32_t)k.tt_port << 23;   // through a hub's translator
    if (periodic) c |= k.speed == 3 ? 0x01 : 0x01 | 0x1C << 8;   // every frame; splits complete in uframes 2-4
    return c;
}

// Run qTDs 0..n-1 on the async schedule's transfer queue head, with the
// schedule switched on only for it. Data qTDs are data_from..data_to (the
// bytes each asked for kept in its spare word 15); a short one goes on to
// its alternate (a control transfer's status stage) or ends the transfer.
// tog: the data toggle to start the queue head with (DTC 0). 1 done, 13
// short, 6 stall, -1 error or timeout.
static int e_run(uint32_t chars, uint32_t caps, int n, int data_from, int data_to, int tog, int timeout_ms,
                 uint32_t* got, bool* short_pkt) {
    volatile uint32_t* x = H->xqh;
    x[1] = chars; x[2] = caps; x[3] = 0;
    x[4] = (uint32_t)phys(e_qtd(0)); x[5] = 1;
    for (int j = 7; j < 17; j++) x[j] = 0;
    barrier();
    x[6] = (uint32_t)tog << 31;
    ewr(0x18, (uint32_t)phys(H->aqh));
    ewr(0, erd(0) | 0x20);                              // async schedule on
    for (int i = 0; i < 100 && !(erd(4) & 0x8000); i++) io_delay(10);
    int rc = 1, t = 0;
    uint32_t bytes = 0;
    bool shortp = false;
    for (int i = 0; i < n; ) {
        uint32_t tok = e_qtd(i)[2];
        if (tok & E_ACTIVE) {
            if (++t > timeout_ms * 100) { rc = -1; break; }
            io_delay(10);
            continue;
        }
        if (tok & E_HALTED) { rc = (tok & (E_BABBLE | E_XACT | E_DBE)) ? -1 : 6; break; }
        if (i >= data_from && i <= data_to) {
            uint32_t left = (tok >> 16) & 0x7FFF;
            bytes += e_qtd(i)[15] - left;
            if (left) {
                shortp = true;
                if (e_qtd(i)[1] & 1) { rc = 13; break; }   // no alternate: the transfer ends here
                i = n - 1;                                  // on to the status stage
                continue;
            }
        }
        i++;
    }
    ewr(0, erd(0) & ~0x20u);                            // async schedule off
    for (int i = 0; i < 100 && (erd(4) & 0x8000); i++) io_delay(10);
    if (got) *got = bytes;
    if (short_pkt) *short_pkt = shortp;
    return rc;
}

static bool e_control(Keyboard& k, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, volatile void* buf) {
    bool in = type & 0x80;
    put_setup(type, req, value, index, len);
    int status = len ? 2 : 1;
    e_fill(0, 1, -1, 2, 0, phys(H->sbuf), 8, false);    // SETUP, DATA0
    if (len) {
        e_fill(1, 2, in ? 2 : -1, in ? 1 : 0, 1, phys(buf), len, false);   // DATA1 on
        e_qtd(1)[15] = len;
    }
    e_fill(status, -1, -1, len && in ? 0 : 1, 1, 0, 0, true);   // STATUS, the other way
    int rc = e_run(e_chars(k, 0, k.mps0 ? k.mps0 : 64, true, true), e_caps(k, false), status + 1,
                   1, len ? 1 : 0, 0, 500, nullptr, nullptr);
    ctl_cc = rc;                                        // for messages: 1 done, 13 short, 6 stall, -1 error / timeout
    return rc == 1 || rc == 13;
}

static int e_bulk(Keyboard& k, bool in, volatile void* buf, uint32_t len, uint32_t* got) {
    int ep = (in ? k.ep_in : k.ep_out) & 0xF, mps = k.mps ? k.mps : 512;
    uint8_t& tog = in ? k.tog_in : k.tog_out;
    int n = len ? (int)((len + 16383) / 16384) : 1;     // qTDs of 16 KiB
    if (n > EHCI_QTDS) return -1;
    for (int i = 0; i < n; i++) {
        uint32_t off = (uint32_t)i * 16384, l = len ? (len - off < 16384 ? len - off : 16384) : 0;
        e_fill(i, i + 1 < n ? i + 1 : -1, -1, in ? 1 : 0, 0, phys(buf) + off, l, i == n - 1);
        e_qtd(i)[15] = l;
    }
    uint32_t bytes = 0;
    bool shortp = false;
    int rc = e_run(e_chars(k, ep, mps, false, false), e_caps(k, false), n, 0, n - 1, tog, 5000, &bytes, &shortp);
    if (rc == 1 || rc == 13) tog = (uint8_t)(tog ^ (packets(len, bytes, mps, shortp) & 1));
    if (got) *got = bytes;
    return rc;
}

// Interrupt queue heads hang off the periodic head (polled every frame).
static void e_intr_arm(Keyboard& k) {
    volatile uint32_t* t = k.itd;
    t[0] = 1; t[1] = 1;
    uint64_t b = phys(k.reports);
    t[3] = (uint32_t)b;
    for (int j = 1; j < 5; j++) t[3 + j] = (uint32_t)((b & ~0xFFFull) + 0x1000ull * j);
    for (int j = 0; j < 5; j++) t[8 + j] = 0;
    barrier();
    t[2] = E_ACTIVE | 1u << 8 | 3u << 10 | (uint32_t)k.ilen << 16;
    barrier();
    k.iqh[4] = (uint32_t)phys(t);                       // the idle queue head picks it up
}

static bool e_init(const PciDevice& d) {
    uint32_t bar = pci_read(d, 0x10);
    uint64_t base = bar & 0xFFFFFFF0;
    if ((bar & 0x6) == 0x4) base |= (uint64_t)pci_read(d, 0x14) << 32;
    if (!base) { printf("USB: EHCI BAR not set\n"); return false; }
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x06);      // memory, bus master
    volatile uint8_t* cap = (volatile uint8_t*)map_mmio64(base, 0x1000);
    if (!cap) { printf("USB: can't map the EHCI registers\n"); return false; }
    H->eop = cap + cap[0];
    uint32_t hcs = rd(cap, 4), hcc = rd(cap, 8);
    H->num_ports = (int)(hcs & 0xF);
    H->ncomp = (int)((hcs >> 12) & 0xF);
    // BIOS handoff (USB legacy support, in PCI configuration space).
    for (int eecp = (hcc >> 8) & 0xFF, g = 0; eecp >= 0x40 && g < 16; g++) {
        uint32_t v = pci_read(d, eecp);
        if ((v & 0xFF) == 1) {
            pci_write(d, eecp, v | 1u << 24);
            for (int i = 0; i < 1000 && (pci_read(d, eecp) & 1u << 16); i++) delay_ms(1);
            if (pci_read(d, eecp) & 1u << 16) printf("USB: EHCI: BIOS won't let go, taking it anyway\n");
            pci_write(d, eecp + 4, 0);                  // no SMIs
            break;
        }
        eecp = (v >> 8) & 0xFF;
    }
    ewr(0, erd(0) & ~1u);                               // stop
    for (int i = 0; i < 20 && !(erd(4) & 0x1000); i++) delay_ms(1);
    ewr(0, 2);                                          // reset
    for (int i = 0; i < 250 && (erd(0) & 2); i++) delay_ms(1);
    if (erd(0) & 2) { printf("USB: EHCI reset timed out\n"); return false; }
    if (hcc & 1) ewr(0x10, 0);                          // 64-bit structures: all below 4 GiB
    ewr(8, 0);                                          // no interrupts: polled
    H->frames = (volatile uint32_t*)dma_alloc(4096, 4096);
    H->qh_int = (volatile uint32_t*)dma_alloc(128, 32); // periodic head
    H->aqh = (volatile uint32_t*)dma_alloc(128, 32);    // async head
    H->xqh = (volatile uint32_t*)dma_alloc(128, 32);    // control / bulk transfers
    H->tds = (volatile uint32_t*)dma_alloc(EHCI_QTDS * 64, 32);
    H->sbuf = (volatile uint8_t*)dma_alloc(64, 32);
    if (!H->frames || !H->qh_int || !H->aqh || !H->xqh || !H->tds || !H->sbuf) { printf("USB: out of memory\n"); return false; }
    volatile uint32_t* p = H->qh_int;
    p[0] = 1; p[1] = 2u << 12; p[2] = 1u << 30; p[4] = 1; p[5] = 1; p[6] = E_HALTED;    // S-mask 0: never runs
    for (int i = 0; i < 1024; i++) H->frames[i] = (uint32_t)phys(p) | 2;
    volatile uint32_t* a = H->aqh;
    a[0] = (uint32_t)phys(H->xqh) | 2; a[1] = 1u << 15 | 2u << 12; a[2] = 1u << 30; a[4] = 1; a[5] = 1; a[6] = E_HALTED;
    volatile uint32_t* x = H->xqh;
    x[0] = (uint32_t)phys(a) | 2; x[4] = 1; x[5] = 1; x[6] = E_HALTED;
    ewr(0x14, (uint32_t)phys(H->frames));
    ewr(0x18, (uint32_t)phys(a));
    ewr(0, 8u << 16 | 0x10 | 1);                        // run, periodic schedule on, 1024 frames
    for (int i = 0; i < 20 && (erd(4) & 0x1000); i++) delay_ms(1);
    ewr(0x40, 1);                                       // all ports to EHCI (companions get what we hand over)
    delay_ms(5);
    if (hcs & 0x10) for (int q = 1; q <= H->num_ports; q++) eport_w(q, (eport(q) & ~E_WC) | 0x1000);   // power
    delay_ms(100);
    return true;
}

static void e_root_poll(bool first) {
    for (int p = 1; p <= H->num_ports; p++) {
        uint32_t v = eport(p);
        if (v & 0x2000) continue;                       // a companion's now
        if (!(v & 2) && !(first && (v & 1))) continue;
        eport_w(p, (v & ~E_WC) | (v & E_WC));           // ack
        usb_activity = true;
        hc2_forget_port(p);
        if (!(v & 1)) continue;
        delay_ms(100);                                  // connect debounce
        v = eport(p);
        if (!(v & 1)) continue;
        bool handover = ((v >> 10) & 3) == 1;           // K state: low speed
        if (!handover) {
            eport_w(p, (v & ~E_WC & ~4u) | 0x100);      // reset
            delay_ms(50);
            eport_w(p, eport(p) & ~E_WC & ~0x100u);
            for (int i = 0; i < 20 && (eport(p) & 0x100); i++) delay_ms(1);
            delay_ms(10);
            v = eport(p);
            if (!(v & 1)) continue;
            handover = !(v & 4);                        // not enabled: full speed
        }
        if (handover) {
            if (!H->ncomp) { printf("USB: EHCI port %d: a %s-speed device, but no companion controller for it\n", p,
                                    ((v >> 10) & 3) == 1 ? "low" : "full"); continue; }
            eport_w(p, (eport(p) & ~E_WC) | 0x2000);    // port owner: the companion (UHCI/OHCI)
            dbg(1, "USB: EHCI port %d: low/full speed, handed to the companion controller\n", p);
            continue;
        }
        printf("USB: EHCI port %d: device connected (high speed)\n", p);
        setup_device(p, -1, 0, 3);
    }
}

// ================================================================ OHCI
// The other USB 1.1 controller: AMD, ATI, NVIDIA, VIA, SiS and ALi chipsets
// use it where Intel used UHCI (as EHCI's companion, or on its own). Lists
// of endpoint descriptors (EDs), each with a queue of transfer descriptors
// (TDs) running from its head to a dummy tail TD the controller never
// touches. Control and bulk transfers run one at a time on their own ED,
// with that list switched on only while they run (as e_run does); interrupt
// EDs hang off a head ED that every slot of the HCCA's interrupt table
// points at, and an isochronous ED (audio) after them all.
#define OHCI_TDS 80                                 // 16-byte general TDs, the last one the dummy tail
#define O_TD_NONE 0xFu                              // condition code: not accessed yet
static inline uint32_t ord(int o) { return rd(H->oreg, (uint32_t)o); }
static inline void owr(int o, uint32_t v) { wr(H->oreg, (uint32_t)o, v); }
static inline uint32_t oport(int p) { return ord(0x54 + 4 * (p - 1)); }
static inline void oport_w(int p, uint32_t v) { owr(0x54 + 4 * (p - 1), v); }
static volatile uint32_t* o_td(int i) { return H->tds + i * 4; }
static volatile uint32_t* o_tail() { return o_td(OHCI_TDS - 1); }
static volatile uint32_t* o_anchor() { return H->qh_int + 8; }   // the periodic list's last (skipped) ED

// ED dword 0 for k's endpoint ep.
static uint32_t o_ed0(const Keyboard& k, int ep, int mps) {
    return (uint32_t)k.addr | (uint32_t)ep << 7 | (k.speed == 2 ? 1u << 13 : 0) | (uint32_t)mps << 16;   // direction from the TDs
}

// TD i: pid 0 SETUP, 1 OUT, 2 IN; tog: 0 from the ED's carry, 2 DATA0, 3 DATA1.
static void o_fill(int i, volatile uint32_t* next, int pid, int tog, bool rounding, uint64_t buf, uint32_t len) {
    volatile uint32_t* t = o_td(i);
    t[1] = len ? (uint32_t)buf : 0;
    t[2] = (uint32_t)phys(next);
    t[3] = len ? (uint32_t)buf + len - 1 : 0;
    t[0] = O_TD_NONE << 28 | (uint32_t)tog << 24 | 7u << 21 | (uint32_t)pid << 19 | (rounding ? 1u << 18 : 0);
}

// Wait for a new frame: what the controller had of a list it was told to drop is let go.
static void o_next_frame() {
    uint32_t f = ord(0x3C) & 0xFFFF;
    for (int i = 0; i < 300 && (ord(0x3C) & 0xFFFF) == f; i++) io_delay(10);
}

// Run TDs 0..n-1 on ED ed (control list if ctl, else bulk), toggle carry
// *carry in and out. Data TDs data_from..data_to (asked-for length in
// lens[]). 1 done, 13 short, 6 stall, -1 error or timeout.
static int o_run(volatile uint32_t* ed, bool ctl, uint32_t ed0, int n, int data_from, int data_to,
                 const uint32_t* lens, uint8_t* carry, int timeout_ms, uint32_t* got) {
    volatile uint32_t* tail = o_tail();
    ed[0] = ed0;
    ed[1] = (uint32_t)phys(tail);
    ed[3] = 0;
    barrier();
    ed[2] = (uint32_t)phys(o_td(0)) | (carry && *carry ? 2u : 0);
    uint32_t list = ctl ? 1u << 4 : 1u << 5;           // CLE / BLE
    owr(ctl ? 0x24 : 0x2C, 0);                          // current ED
    owr(ctl ? 0x20 : 0x28, (uint32_t)phys(ed));         // head ED
    owr(0x04, ord(0x04) | list);
    owr(0x08, ctl ? 2 : 4);                             // CLF / BLF: there's work
    int t = 0;
    while ((ed[2] & ~0xFu) != (uint32_t)phys(tail) && !(ed[2] & 1)) {
        if (++t > timeout_ms * 100) break;
        io_delay(10);
    }
    owr(0x04, ord(0x04) & ~list);
    o_next_frame();
    int rc = 1;
    uint32_t bytes = 0;
    bool timed_out = (ed[2] & ~0xFu) != (uint32_t)phys(tail) && !(ed[2] & 1);
    for (int i = 0; i < n; i++) {
        volatile uint32_t* td = o_td(i);
        uint32_t cc = td[0] >> 28;
        if (cc == O_TD_NONE || cc == 0xE) { rc = timed_out ? -1 : rc; break; }   // not reached (after a short one)
        if (i >= data_from && i <= data_to) {
            uint32_t left = td[1] ? td[3] - td[1] + 1 : 0;
            bytes += lens[i] - left;
            if (left || cc == 9) {                      // short (data underrun with no rounding)
                rc = 13;
                if (cc == 9 || i == data_to) break;
                continue;
            }
        }
        if (cc == 9) continue;
        if (cc) { rc = cc == 4 ? 6 : -1; break; }
    }
    if (timed_out && rc == 1) rc = -1;
    if (carry) *carry = (uint8_t)((ed[2] >> 1) & 1);
    ed[2] = (uint32_t)phys(tail) | (ed[2] & 2);         // empty, not halted
    if (got) *got = bytes;
    return rc;
}

static bool o_control(Keyboard& k, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, volatile void* buf) {
    bool in = type & 0x80;
    put_setup(type, req, value, index, len);
    uint32_t lens[OHCI_TDS] = {};
    int n = 0;
    o_fill(n++, o_td(1), 0, 2, false, phys(H->sbuf), 8);                       // SETUP, DATA0
    int data_from = n;
    for (uint32_t off = 0; off < len; off += 4096) {
        if (n >= OHCI_TDS - 2) return false;
        uint32_t l = len - off < 4096 ? len - off : 4096;
        lens[n] = l;
        o_fill(n, o_td(n + 1), in ? 2 : 1, 0, true, phys(buf) + off, l);         // DATA1 on (the carry)
        n++;
    }
    int data_to = n - 1;
    o_fill(n, o_tail(), len && in ? 1 : 2, 3, false, 0, 0);                      // STATUS, DATA1
    n++;
    uint8_t carry = 0;
    int rc = o_run(H->qh_ctl, true, o_ed0(k, 0, k.mps0 ? k.mps0 : 8), n, data_from, data_to, lens, &carry, 500, nullptr);
    ctl_cc = rc;
    return rc == 1 || rc == 13;
}

static int o_bulk(Keyboard& k, bool in, volatile void* buf, uint32_t len, uint32_t* got) {
    int ep = (in ? k.ep_in : k.ep_out) & 0xF, mps = k.mps ? k.mps : 64;
    uint8_t& tog = in ? k.tog_in : k.tog_out;
    int n = len ? (int)((len + 4095) / 4096) : 1;
    if (n > OHCI_TDS - 1) return -1;
    uint32_t lens[OHCI_TDS] = {};
    for (int i = 0; i < n; i++) {
        uint32_t off = (uint32_t)i * 4096, l = len ? (len - off < 4096 ? len - off : 4096) : 0;
        lens[i] = l;
        // Short packets: an error (the ED halts, so no more IN tokens go out)
        // except in the last TD, where it simply ends the transfer.
        o_fill(i, i + 1 < n ? o_td(i + 1) : o_tail(), in ? 2 : 1, 0, i == n - 1, phys(buf) + off, l);
    }
    uint32_t bytes = 0;
    int rc = o_run(H->xqh, false, o_ed0(k, ep, mps), n, 0, n - 1, lens, &tog, 5000, &bytes);
    if (got) *got = bytes;
    return rc;
}

// k's interrupt ED (k.iqh): one TD (k.itd), the dummy tail after it.
static void o_intr_arm(Keyboard& k) {
    volatile uint32_t* t = k.itd;
    volatile uint32_t* tail = k.itd + 8;
    uint64_t b = phys(k.reports);
    t[1] = (uint32_t)b;
    t[2] = (uint32_t)phys(tail);
    t[3] = (uint32_t)b + k.ilen - 1;
    t[0] = O_TD_NONE << 28 | 7u << 21 | 2u << 19 | 1u << 18;   // IN, toggle from the ED, short is fine
    barrier();
    k.iqh[2] = (uint32_t)phys(t) | (k.iqh[2] & 2);
}

static void o_unlink(Keyboard& k) {
    if (!k.linked) return;
    k.iqh[0] |= 1u << 14;                               // skip
    volatile uint32_t* prev = H->qh_int;
    uint32_t me = (uint32_t)phys(k.iqh);
    for (int g = 0; g < MAX_KBD + 2 && prev[3]; g++) {
        if (prev[3] == me) { prev[3] = k.iqh[3]; break; }
        prev = (volatile uint32_t*)(uintptr_t)prev[3];
    }
    k.linked = false;
    delay_ms(2);                                        // the controller may still be on it
}

static bool o_intr(Keyboard& k) {
    volatile uint32_t* q = k.iqh;
    for (int j = 0; j < 4; j++) q[j] = 0;
    q[0] = o_ed0(k, k.dci, k.mps) | 2u << 11;           // IN
    q[1] = (uint32_t)phys(k.itd + 8);
    q[2] = (uint32_t)phys(k.itd + 8);
    o_intr_arm(k);
    q[3] = H->qh_int[3];                                // in after the head (so before any isochronous ED)
    barrier();
    H->qh_int[3] = (uint32_t)phys(q);
    k.linked = true;
    return true;
}

static bool o_init(const PciDevice& d) {
    uint32_t bar = pci_read(d, 0x10);
    uint64_t base = bar & 0xFFFFF000;
    if (!base) { printf("USB: OHCI BAR not set\n"); return false; }
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x06);       // memory, bus master
    H->oreg = (volatile uint8_t*)map_mmio64(base, 0x1000);
    if (!H->oreg) { printf("USB: can't map the OHCI registers\n"); return false; }
    // BIOS handoff: the firmware's SMM driver lets go when asked (as Linux does).
    if (ord(0x04) & 1u << 8) {
        owr(0x10, 1u << 30);                            // ownership change interrupt on
        owr(0x08, 1u << 3);                             // ownership change request
        for (int i = 0; i < 500 && (ord(0x04) & 1u << 8); i++) delay_ms(1);
        if (ord(0x04) & 1u << 8) printf("USB: OHCI: BIOS won't let go, taking it anyway\n");
    }
    owr(0x14, 0xC000007F);                              // no interrupts: polled
    owr(0x04, 0);                                       // reset state
    delay_ms(50);                                       // the bus sees the reset
    uint32_t fi = ord(0x34) & 0x3FFF;
    if (fi < 11000 || fi > 13000) fi = 11999;
    owr(0x08, 1);                                       // host controller reset (lands in suspend)
    for (int i = 0; i < 100 && (ord(0x08) & 1); i++) io_delay(10);
    if (ord(0x08) & 1) { printf("USB: OHCI reset timed out\n"); return false; }
    H->frames = (volatile uint32_t*)dma_alloc(256, 256);   // HCCA
    H->qh_int = (volatile uint32_t*)dma_alloc(64, 32);     // periodic head ED, then the anchor ED
    H->qh_ctl = (volatile uint32_t*)dma_alloc(16, 16);
    H->xqh = (volatile uint32_t*)dma_alloc(16, 16);
    H->tds = (volatile uint32_t*)dma_alloc(OHCI_TDS * 16, 16);
    H->sbuf = (volatile uint8_t*)dma_alloc(64, 16);
    if (!H->frames || !H->qh_int || !H->qh_ctl || !H->xqh || !H->tds || !H->sbuf) { printf("USB: out of memory\n"); return false; }
    for (int i = 0; i < 64; i++) H->frames[i] = 0;
    volatile uint32_t* hd = H->qh_int;
    volatile uint32_t* an = o_anchor();
    an[0] = 1u << 14; an[1] = an[2] = an[3] = 0;        // skipped; isochronous EDs hang off it
    hd[0] = 1u << 14; hd[1] = hd[2] = 0; hd[3] = (uint32_t)phys(an);
    for (int i = 0; i < 32; i++) H->frames[i] = (uint32_t)phys(hd);
    volatile uint32_t* eds[2] = { H->qh_ctl, H->xqh };  // control, bulk: skipped, empty
    for (volatile uint32_t* e : eds) { e[0] = 1u << 14; e[1] = e[2] = (uint32_t)phys(o_tail()); e[3] = 0; }
    // Within 2 ms of the reset: frame timing, schedule, operational.
    owr(0x34, ((ord(0x34) & 0x80000000u) ^ 0x80000000u) | (uint32_t)((6 * (fi - 210)) / 7) << 16 | fi);
    owr(0x40, fi * 9 / 10);                             // periodic start
    owr(0x44, 0x628);                                   // low-speed threshold
    owr(0x18, (uint32_t)phys(H->frames));
    owr(0x20, 0); owr(0x24, 0); owr(0x28, 0); owr(0x2C, 0);
    owr(0x0C, 0xFFFFFFFF);                              // clear the status bits
    owr(0x04, 2u << 6 | 1u << 3 | 1u << 2 | 3);         // operational, periodic + isochronous lists, 4:1 control:bulk
    uint32_t ra = ord(0x48);
    H->num_ports = (int)(ra & 0xFF);
    if (H->num_ports > 15) H->num_ports = 15;
    owr(0x50, 1u << 16);                                // global power on
    for (int p = 1; p <= H->num_ports; p++) oport_w(p, 1u << 8);   // and each port's
    int potpgt = (int)(ra >> 24) * 2;
    delay_ms(potpgt > 100 ? potpgt : 100);
    return true;
}

static bool o_reset_port(int p) {
    if (!(oport(p) & 1)) return false;
    oport_w(p, 1u << 4);                                // reset (the controller times it)
    for (int i = 0; i < 100 && !(oport(p) & 1u << 20); i++) delay_ms(1);
    oport_w(p, 1u << 20 | 1u << 17 | 1u << 16);         // ack reset, enable, connect changes
    delay_ms(10);                                       // reset recovery
    uint32_t v = oport(p);
    if (!(v & 1)) return false;
    if (!(v & 2)) { oport_w(p, 2); delay_ms(5); }       // set enable
    return oport(p) & 2;
}

static void o_root_poll(bool first) {
    for (int p = 1; p <= H->num_ports; p++) {
        uint32_t v = oport(p);
        if (!(v & 1u << 16) && !(first && (v & 1))) continue;   // no connect change (or the first look)
        oport_w(p, 1u << 16);                           // ack
        usb_activity = true;
        hc2_forget_port(p);
        if (!(v & 1)) continue;
        delay_ms(100);                                  // connect debounce
        if (!o_reset_port(p)) continue;
        int speed = (oport(p) & 1u << 9) ? 2 : 1;       // low / full
        printf("USB: OHCI port %d: device connected (%s speed)\n", p, speed_name(speed));
        setup_device(p, -1, 0, speed);
    }
}

// ================================================================ shared
static bool hc2_control(Keyboard& k, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, volatile void* buf) {
    return H->type == HC_EHCI ? e_control(k, type, req, value, index, len, buf) :
           H->type == HC_OHCI ? o_control(k, type, req, value, index, len, buf) : u_control(k, type, req, value, index, len, buf);
}
static const char* hc2_name() { return H->type == HC_EHCI ? "EHCI" : H->type == HC_OHCI ? "OHCI" : "UHCI"; }
static void hc2_root_poll(bool first) {
    if (H->type == HC_EHCI) e_root_poll(first); else if (H->type == HC_OHCI) o_root_poll(first); else u_root_poll(first);
}

// Address 0 -> k.slot: the default pipe's packet size first.
static bool hc2_address(Keyboard& k) {
    static volatile uint8_t d8[8] __attribute__((aligned(64)));
    k.addr = 0;
    k.mps0 = k.speed == 3 ? 64 : 8;
    if (!hc2_control(k, 0x80, 6, 0x0100, 0, 8, d8)) { printf("USB: %s: no answer at address 0\n", k.where); return false; }
    if (d8[7] == 8 || d8[7] == 16 || d8[7] == 32 || d8[7] == 64) k.mps0 = d8[7];
    if (!hc2_control(k, 0x00, 5, k.slot, 0, 0, nullptr)) { printf("USB: %s: SET_ADDRESS failed\n", k.where); return false; }
    k.addr = (uint8_t)k.slot;
    delay_ms(10);
    return true;
}

static void hc2_unlink(Keyboard& k) {
    if (!k.linked) return;
    if (H->type == HC_OHCI) { o_unlink(k); return; }
    volatile uint32_t* prev = H->qh_int;
    uint32_t me = (uint32_t)phys(k.iqh);
    for (int g = 0; g < MAX_KBD + 2 && !(prev[0] & 1); g++) {
        volatile uint32_t* nxt = (volatile uint32_t*)(uintptr_t)(prev[0] & ~0x1Fu);
        if ((prev[0] & ~0x1Fu) == me) { prev[0] = k.iqh[0]; break; }
        prev = nxt;
    }
    k.linked = false;
    delay_ms(2);                                         // the controller may still be on it
}

static bool hc2_intr(Keyboard& k, int ep_addr, int ep_mps, int ep_interval) {
    (void)ep_interval;
    k.dci = ep_addr & 0xF;
    k.mps = ep_mps;
    k.ilen = ep_mps < 1 ? 8 : ep_mps > REPORT_MAX ? REPORT_MAX : ep_mps;
    k.itog = 0;
    hc2_unlink(k);
    if (H->type == HC_OHCI) return o_intr(k);
    volatile uint32_t* q = k.iqh;
    for (int j = 0; j < 32; j++) q[j] = 0;
    if (H->type == HC_EHCI) {
        q[1] = e_chars(k, k.dci, ep_mps, false, false);
        q[2] = e_caps(k, true);
        q[4] = 1; q[5] = 1;
        e_intr_arm(k);
    } else {
        q[1] = 1;
        u_intr_arm(k);
    }
    barrier();
    q[0] = H->qh_int[0];                                 // in at the head of the interrupt list
    barrier();
    H->qh_int[0] = (uint32_t)phys(q) | 2;
    k.linked = true;
    return true;
}

static void hc2_release(Keyboard& k) {
    hc2_unlink(k);
    if (k.slot > 0 && k.slot < 128) H->addr_used[k.slot] = 0;
}

static int hc2_bulk(Keyboard& k, bool in, volatile void* buf, uint32_t len, uint32_t* got) {
    return H->type == HC_EHCI ? e_bulk(k, in, buf, len, got) : H->type == HC_OHCI ? o_bulk(k, in, buf, len, got) :
           u_bulk(k, in, buf, len, got);
}

// A halted bulk endpoint: CLEAR_FEATURE(HALT) and its toggle back to DATA0.
static void hc2_clear_halt(Keyboard& k, bool in) {
    hc2_control(k, 0x02, 1, 0, in ? k.ep_in : k.ep_out, 0, nullptr);
    (in ? k.tog_in : k.tog_out) = 0;
}

// Reports and hub status that came in; root ports that changed.
static void hc2_poll() {
    // Disconnects first: an unplugged device's transfers fail too, and it
    // should be forgotten (address back, "unplugged") rather than stopped.
    hc2_root_poll(false);
    for (int i = 0; i < MAX_KBD; i++) {
        Keyboard& k = kbds[i];
        if (!k.active || k.hc != H || !k.linked) continue;
        int got;
        bool err;
        if (H->type == HC_EHCI) {
            uint32_t tok = k.itd[2];
            if (tok & E_ACTIVE) continue;
            err = tok & E_HALTED;
            got = k.ilen - (int)((tok >> 16) & 0x7FFF);
        } else if (H->type == HC_OHCI) {
            uint32_t cc = k.itd[0] >> 28;
            if (cc == O_TD_NONE) continue;
            err = cc != 0 && cc != 9;
            got = k.itd[1] ? (int)(k.itd[1] - (uint32_t)phys(k.reports)) : k.ilen;
        } else {
            uint32_t st = k.itd[1];
            if (st & U_ACTIVE) continue;
            err = st & U_ERRS;
            got = u_actlen(st);
        }
        if (err) {                                     // goes quiet; unplugging it forgets it
            if (k.kind != HUB) release_all(k);
            hc2_unlink(k);
            dbg(1, "USB: %s on %s: transfer error, stopped\n", k.kind == HUB ? "hub" : k.kind == JOY ? "gamepad" : k.mouse ? "mouse" : "keyboard", k.where);
            continue;
        }
        const volatile uint8_t* r = k.reports;
        if (k.kind == HUB) k.dirty |= (uint32_t)(r[0] | r[1] << 8 | r[2] << 16) & ~1u;
        else if (k.kind == JOY) pad_report(k, r, got);
        else if (got > 0) handle_report(k, r);
        k.itog ^= 1;
        if (H->type == HC_EHCI) e_intr_arm(k); else if (H->type == HC_OHCI) o_intr_arm(k); else u_intr_arm(k);
    }
}

// ================================================================ audio
// USB audio output (usb.cpp's ua) on EHCI and UHCI: isochronous, one
// descriptor a frame. UA2_SLOTS of them are linked into the periodic frame
// list once, when the device is set up: the one for frame f at every frame
// list entry f % UA2_SLOTS, ahead of the interrupt queue heads. The pump
// then only fills in and activates the descriptors for the next few frames
// (an inactive one is passed over), and the controller's frame counter
// says how far it has played, so nothing it does touches the schedule
// itself and it can run from the timer interrupt.
//   UHCI: an isochronous TD, for full-speed devices on a companion port.
//   EHCI, full speed (behind a hub's transaction translator, such as the
//     hub built into the chipset on later boards): a split transaction
//     isochronous TD (siTD), the packet sent in start-splits of 188 bytes.
//   EHCI, high speed: an isochronous TD (iTD), a packet every 1, 2, 4 or 8
//     microframes.
//   OHCI: no frame list. An isochronous ED after the interrupt EDs, whose
//     TDs (one a frame, each naming its frame) are queued at its tail; the
//     ring of UA2_SLOTS is used in order. Only one TD leaves the queue a
//     frame, so a late one (its frame gone) would hold up all those behind
//     it: after a stall the queue is emptied first (the ED skipped for a
//     frame, then its head moved to its tail), and the stream restarted.
#define UA2_SLOTS 32                                   // frames; divides the 1024-entry frame list
#define UA2_LEAD  3                                    // frames ahead of the controller, at least (EHCI may cache one)
#define UA2_QUEUE 12                                   // frames kept queued (the pump runs at 240 Hz)
#define UA2_BUF   4096                                 // bytes of packet buffer a slot
enum { UA2_UHCI, UA2_SITD, UA2_ITD, UA2_OHCI };

static volatile uint32_t* ua2_desc(int slot) { return ua.iso + slot * 32; }   // 128 bytes each
static volatile uint8_t* ua2_buf(int slot) { return ua.pkt + slot * UA2_BUF; }
static volatile uint32_t* ua2_oed(void) { return ua.iso + UA2_SLOTS * 32; }   // OHCI: the ED, after the TDs

// The controller's frame counter (11 bits; OHCI's 16), extended.
static uint32_t ua2_mask(Hc* h) { return h->type == HC_OHCI ? 0xFFFF : 0x7FF; }
static uint32_t ua2_hw(Hc* h) {
    return h->type == HC_EHCI ? (rd(h->eop, 0x0C) >> 3) & 0x7FF :
           h->type == HC_OHCI ? rd(h->oreg, 0x3C) & 0xFFFF : inw((uint16_t)(h->io + 6)) & 0x7FF;
}
static uint32_t ua2_now(void) {
    uint32_t c = ua2_hw(ua.iso_hc);
    ua.hw_ext += (c - ua.hw_last) & ua2_mask(ua.iso_hc);
    ua.hw_last = c;
    return ua.hw_ext;
}

// Take the descriptors out of the frame list (the device went, or the
// stream moves to another controller).
static void ua2_unlink(void) {
    Hc* h = ua.iso_hc;
    if (!h) return;
    if (h->type == HC_OHCI) {
        ua2_oed()[0] |= 1u << 14;                      // skip
        h->qh_int[8 + 3] = 0;                          // off the anchor ED
    } else {
        uint32_t back = (uint32_t)phys(h->qh_int) | 2;
        for (int i = 0; i < 1024; i++) h->frames[i] = back;
    }
    for (int s = 0; s < UA2_SLOTS; s++) {              // nothing left active for a frame list that may come back
        volatile uint32_t* d = ua2_desc(s);
        if (ua.iso_kind == UA2_UHCI || ua.iso_kind == UA2_OHCI) d[1] = 0;
        else if (ua.iso_kind == UA2_SITD) d[3] = 0;
        else for (int u = 0; u < 8; u++) d[1 + u] = 0;
    }
    ua.iso_hc = nullptr;
    delay_ms(2);                                       // the controller may be on one now
}

// Device k (on H) plays through endpoint a.ep: link its descriptors in.
static bool ua2_link(Keyboard& k, const UaAlt& a) {
    if (!ua.iso) {
        ua.iso = (volatile uint32_t*)dma_alloc(UA2_SLOTS * 128 + 16, 64);
        if (!ua.iso) { printf("USB: %s: out of memory for audio\n", k.where); return false; }
    }
    ua2_unlink();
    int kind = H->type == HC_UHCI ? UA2_UHCI : H->type == HC_OHCI ? UA2_OHCI : k.speed == 3 ? UA2_ITD : UA2_SITD;
    if (kind == UA2_SITD && !k.tt_slot) {
        printf("USB: %s: audio: a full-speed device with no transaction translator\n", k.where);
        return false;
    }
    ua.iso_kind = kind;
    ua.addr = k.addr; ua.ep = a.ep & 0xF;
    ua.hub = k.tt_slot; ua.hub_port = k.tt_port;
    ua.hs_iv = kind == UA2_ITD ? 1 << (a.interval - 1) : 8;
    ua.iso_hc = H;
    if (kind == UA2_OHCI) {
        for (int s = 0; s < UA2_SLOTS; s++) { volatile uint32_t* d = ua2_desc(s); for (int j = 0; j < 32; j++) d[j] = 0; }
        volatile uint32_t* ed = ua2_oed();
        ed[0] = (uint32_t)ua.addr | (uint32_t)ua.ep << 7 | 1u << 11 | 1u << 15 | (uint32_t)(a.mps & 0x3FF) << 16;   // OUT, isochronous
        ed[1] = ed[2] = (uint32_t)phys(ua2_desc(0));   // empty: the dummy TD
        ed[3] = 0;
        ua.o_tail = 0;
        ua.o_flush = 0;
        barrier();
        H->qh_int[8 + 3] = (uint32_t)phys(ed);         // after the anchor ED: the periodic list's end
    } else {
        uint32_t next = (uint32_t)phys(H->qh_int) | 2; // on to the interrupt queue heads
        for (int s = 0; s < UA2_SLOTS; s++) {
            volatile uint32_t* d = ua2_desc(s);
            for (int j = 0; j < 32; j++) d[j] = 0;     // inactive (64-bit fields zero too)
            d[0] = next;
            if (kind == UA2_SITD) d[6] = 1;            // back pointer: none
        }
        barrier();
        uint32_t type = kind == UA2_SITD ? 2u << 1 : 0;   // EHCI: iTD 0, siTD 2; UHCI: a TD
        for (int i = 0; i < 1024; i++) H->frames[i] = (uint32_t)phys(ua2_desc(i % UA2_SLOTS)) | type;
    }
    ua.hw_last = ua.hw_ext = ua2_hw(H);
    ua.f_base = 0;
    return true;
}

// Frames played: every TD whose frame the controller has moved past.
static void ua2_done(void) {
    uint32_t now = ua2_now();
    uint32_t upto = now - ua.f_base;                   // the seq playing now
    if ((int32_t)(upto - ua.seq_enq) > 0) upto = ua.seq_enq;
    if ((int32_t)(upto - ua.seq_done) > 0) ua.seq_done = upto;
}

// usb_audio_start: the next TD goes out UA2_LEAD frames from now.
static void ua2_start(void) {
    ua.f_base = ua2_now() + UA2_LEAD - ua.seq_enq;
    ua.seq_done = ua.seq_enq;
    if (ua.iso_kind == UA2_OHCI) ua.o_flush = 1;      // anything left from before: out of the way
}

// Frame f's packet, from 1 ms (48 frames) of audio.cpp's ring.
static void ua2_fill(uint32_t f, uint32_t src) {
    int slot = ua.iso_kind == UA2_OHCI ? ua.o_tail : (int)(f % UA2_SLOTS);
    volatile uint32_t* d = ua2_desc(slot);
    volatile uint8_t* p = ua2_buf(slot);
    uint64_t b = phys(p);
    int fb = ua.ch * ua.bytes;
    if (ua.iso_kind != UA2_ITD) {                      // one packet
        ua.facc += ua.rate * 1000;
        uint32_t n = ua.facc / 1000000;
        ua.facc %= 1000000;
        int cap = ua.pkt_max < 1023 ? ua.pkt_max : 1023;
        if ((int)n * fb > cap) n = (uint32_t)(cap / fb);
        if (!n && ua.iso_kind == UA2_OHCI) n = 1;
        int len = n ? ua_render(p, src, 48, n) : 0;
        if (ua.iso_kind == UA2_OHCI) {
            // Fill in the dummy tail TD, then a fresh dummy after it.
            int nx = (slot + 1) % UA2_SLOTS;
            volatile uint32_t* t = ua2_desc(nx);
            for (int j = 0; j < 8; j++) t[j] = 0;
            d[1] = (uint32_t)b & ~0xFFFu;              // buffer page 0
            d[2] = (uint32_t)phys(t);
            d[3] = (uint32_t)b + (uint32_t)len - 1;    // buffer end
            d[4] = 0xE000u | ((uint32_t)b & 0xFFF);    // packet 0: not accessed, its offset
            d[0] = O_TD_NONE << 28 | 7u << 21 | (f & 0xFFFF);   // one packet (FC 0), in frame f
            barrier();
            ua2_oed()[1] = (uint32_t)phys(t);          // the new tail: the controller may have it now
            ua.o_tail = nx;
        } else if (ua.iso_kind == UA2_UHCI) {
            d[3] = (uint32_t)b;
            d[2] = u_token(PID_OUT, ua.addr, ua.ep, 0, len);
            barrier();
            d[1] = U_ACTIVE | 1u << 25;                // isochronous
        } else {
            // Start-splits of up to 188 bytes in microframes 1, 2, ... (the
            // interrupt endpoints' splits start in 0); no complete-splits.
            int tcount = len ? (len + 187) / 188 : 1;
            d[1] = (uint32_t)ua.hub_port << 24 | (uint32_t)ua.hub << 16 | (uint32_t)ua.ep << 8 | ua.addr;   // OUT
            d[2] = (uint32_t)((1 << tcount) - 1) << 1;  // S-mask; C-mask 0
            d[4] = (uint32_t)b;
            d[5] = (uint32_t)((b & ~0xFFFull) + 0x1000) | (tcount > 1 ? 1u << 3 : 0) | (uint32_t)tcount;   // TP: begin / all
            barrier();
            d[3] = (uint32_t)len << 16 | 0x80;         // active
        }
        return;
    }
    // iTD: a packet every hs_iv microframes, each its share of the 1 ms.
    int per_tr = 8 / ua.hs_iv;
    int cap = ua.pkt_max;
    if (cap > UA2_BUF / per_tr) cap = UA2_BUF / per_tr;
    uint32_t n[8] = {}, total = 0;
    for (int t = 0; t < per_tr; t++) {
        ua.facc += ua.rate * 125 * (uint32_t)ua.hs_iv;
        n[t] = ua.facc / 1000000;
        ua.facc %= 1000000;
        if ((int)n[t] * fb > cap) n[t] = (uint32_t)(cap / fb);
        total += n[t];
    }
    if (total) ua_render(p, src, 48, total);
    uint64_t p0 = b & ~0xFFFull;
    d[9] = (uint32_t)p0 | (uint32_t)ua.ep << 8 | ua.addr;
    d[10] = (uint32_t)(p0 + 0x1000) | (uint32_t)ua.pkt_max;   // OUT
    d[11] = (uint32_t)(p0 + 0x2000) | 1;               // one transaction a microframe
    for (int j = 3; j < 7; j++) d[9 + j] = (uint32_t)(p0 + 0x1000ull * j);
    uint64_t a = b;
    for (int u = 0; u < 8; u++) d[1 + u] = 0;
    barrier();
    for (int t = 0; t < per_tr; t++) {
        uint32_t len = n[t] * (uint32_t)fb;
        uint32_t pg = (uint32_t)((a >> 12) - (p0 >> 12));
        d[1 + t * ua.hs_iv] = 1u << 31 | len << 16 | pg << 12 | (uint32_t)(a & 0xFFF);
        a += len;
    }
}

static void ua2_pump(void) {
    ua2_done();
    uint32_t now = ua.hw_ext;
    if (ua.iso_kind == UA2_OHCI && ua.o_flush) {      // OHCI: emptying the ED's queue
        volatile uint32_t* ed = ua2_oed();
        if (ua.o_flush == 1) { ed[0] |= 1u << 14; ua.o_flush_at = now; ua.o_flush = 2; return; }   // skip it
        if ((int32_t)(now - ua.o_flush_at) < 2) return;   // a whole frame without it
        ed[2] = ed[1];                                 // head = tail: empty (and not halted)
        barrier();
        ed[0] &= ~(1u << 14);
        ua.o_flush = 0;
        ua.f_base = now + UA2_LEAD - ua.seq_enq;
        ua.seq_done = ua.seq_enq;
    }
    // Fallen behind (the pump didn't run for a while): skip ahead, a gap.
    if ((int32_t)(ua.f_base + ua.seq_enq - (now + UA2_LEAD)) < 0) {
        if (ua.seq_enq != ua.seq_start) ua.underruns++;
        if (ua.iso_kind == UA2_OHCI) { ua.o_flush = 1; return; }
        ua.f_base = now + UA2_LEAD - ua.seq_enq;
        ua.seq_done = ua.seq_enq;
    }
    while ((int32_t)(ua.seq_enq - ua.seq_done) < UA2_QUEUE) {
        uint32_t f = ua.f_base + ua.seq_enq;
        if ((int32_t)(f - now) >= UA2_SLOTS - 2) break;   // that slot may not have played yet
        ua2_fill(f, ua.src_pos);
        ua.src_pos = (ua.src_pos + 48) % ua.src_frames;
        ua.seq_enq++;
    }
}
