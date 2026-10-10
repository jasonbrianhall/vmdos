// EHCI (USB 2) and UHCI (USB 1.1) host controllers, under the same device
// code as xHCI (included from usb.cpp): keyboards, mice, gamepads, hubs and
// mass storage work the same on any of them.
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

// ================================================================ shared
static bool hc2_control(Keyboard& k, uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len, volatile void* buf) {
    return H->type == HC_EHCI ? e_control(k, type, req, value, index, len, buf) : u_control(k, type, req, value, index, len, buf);
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
    return H->type == HC_EHCI ? e_bulk(k, in, buf, len, got) : u_bulk(k, in, buf, len, got);
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
    if (H->type == HC_EHCI) e_root_poll(false); else u_root_poll(false);
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
        if (H->type == HC_EHCI) e_intr_arm(k); else u_intr_arm(k);
    }
}
