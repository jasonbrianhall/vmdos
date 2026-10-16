// Network: the PC's own Ethernet card, for DOS's packet driver (VMPKT.COM).
//
// vmdos drives the card itself, polled from the timer tick (the card's
// own interrupt stays masked). Received frames wait in a queue here; while
// it holds any, the IRQ VMPKT asked for is raised in the virtual PIC, and
// VMPKT's handler takes them out one at a time (INT 2Fh AX=564Eh BL=3) and
// hands them to its clients. Sending is one call (BL=2), straight to the
// card. DOS never sees the card itself (VMHV's PCI reads FFh anyway).
//
// Cards: Intel 8254x/8257x "e1000" (QEMU's default, -nic model=e1000 or
// e1000e, and many real Intel NICs) and Realtek RTL8139 (-nic model=rtl8139).
// Kernel options: net=off (no card), nic=N (the Nth card found, from 0),
// netlog (every call and frame in the log).
//
// INT 2Fh AX=564Eh ('N'), BL = function; BX = 564Dh ('VM') on return:
//   0  query: AX = 1 card / 0 none, CL = link up, CH = card kind
//      (1 e1000, 2 RTL8139), DX = frames waiting, ES:DI <- the MAC (6 bytes)
//   1  CL = IRQ to raise while frames wait (FFh: none)
//   2  send DS:SI, CX bytes (14..1514): AX = 0 sent, 1 failed
//   3  receive: the next frame into ES:DI (CX = room), CX = its length
//      (0: none waiting; a frame longer than the room is cut), DX = still waiting
//   4  receive mode CL (packet driver numbering: 1 off, 2 own address,
//      3 + broadcast, 4/5 + multicast, 6 everything): AX = 0, 1 not supported
//   5  statistics into ES:DI: 7 dwords: frames in, frames out, bytes in,
//      bytes out, receive errors, send errors, frames dropped (queue full)
#include "pci.hpp"
#include "hw.hpp"

namespace {

enum { KIND_NONE, KIND_E1000, KIND_RTL8139 };
const int QLEN = 64, FRAME_MAX = 1536;

int kind;
uint8_t mac[6];
int irq_line = -1;
int rx_mode = 3;
bool log_on;

struct Stats { uint32_t rx, tx, rx_bytes, tx_bytes, rx_err, tx_err, dropped; } st;

// ---- the receive queue ----
uint8_t* qbuf;
uint16_t qlen_of[QLEN];
int q_head, q_count;

void enqueue(const uint8_t* f, unsigned len)
{
    if (len < 14 || len > FRAME_MAX) { st.rx_err++; return; }
    if (rx_mode == 1) return;
    if (q_count == QLEN) { st.dropped++; return; }
    int i = (q_head + q_count) % QLEN;
    memcpy(qbuf + i * FRAME_MAX, f, len);
    qlen_of[i] = (uint16_t)len;
    q_count++;
    st.rx++; st.rx_bytes += len;
    if (log_on) kprintf("net: in %u bytes, type %02x%02x, from %02x:%02x:%02x:%02x:%02x:%02x\n", len, f[12], f[13],
                        f[6], f[7], f[8], f[9], f[10], f[11]);
}

void udelay(uint32_t us) { while (us--) inb(0x80); }      // ~1 us each (works with interrupts off)

#define BARRIER() __asm__ volatile("" ::: "memory")

// ---------------- Intel e1000 (legacy descriptors) ----------------
namespace e1k {
volatile uint8_t* mm;
const int NRX = 32, NTX = 16;
struct RxD { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; };
struct TxD { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; };
volatile RxD* rx;
volatile TxD* tx;
uint8_t *rxb, *txb;
int rx_cur, tx_cur;

enum {
    CTRL = 0x0, STATUS = 0x8, EECD = 0x10, EERD = 0x14, ICR = 0xC0, IMC = 0xD8,
    RCTL = 0x100, TCTL = 0x400, TIPG = 0x410,
    RDBAL = 0x2800, RDBAH = 0x2804, RDLEN = 0x2808, RDH = 0x2810, RDT = 0x2818,
    TDBAL = 0x3800, TDBAH = 0x3804, TDLEN = 0x3808, TDH = 0x3810, TDT = 0x3818,
    MTA = 0x5200, RAL = 0x5400, RAH = 0x5404,
};
uint32_t rd(int r) { return *(volatile uint32_t*)(mm + r); }
void wr(int r, uint32_t v) { *(volatile uint32_t*)(mm + r) = v; }

// Known 8254x/8257x/I21x device IDs (all speak the legacy descriptors).
const uint16_t ids[] = {
    0x1000, 0x1001, 0x1004, 0x1008, 0x1009, 0x100C, 0x100D, 0x100E, 0x100F, 0x1010, 0x1011, 0x1012,
    0x1013, 0x1014, 0x1015, 0x1016, 0x1017, 0x1018, 0x1019, 0x101A, 0x101D, 0x101E, 0x1026, 0x1027,
    0x1028, 0x1075, 0x1076, 0x1077, 0x1078, 0x1079, 0x107A, 0x107B, 0x107C, 0x108A, 0x1099, 0x10B5,
    0x105E, 0x105F, 0x1060, 0x107D, 0x107E, 0x107F, 0x10B9, 0x108B, 0x108C, 0x109A, 0x10D3, 0x10F6,
    0x150C, 0x153A, 0x153B, 0x155A, 0x1559, 0x15A0, 0x15A1, 0x15A2, 0x15A3,
    0x156F, 0x1570, 0x15B7, 0x15B8, 0x15B9, 0x15BB, 0x15BC, 0x15BD, 0x15BE, 0x15D6, 0x15D7, 0x15D8,
    0x15E3, 0x15E1, 0x15E2, 0x15DF, 0x15E0, 0x0D4C, 0x0D4D, 0x0D4E, 0x0D4F, 0x0D53, 0x0D55,
};

bool match(uint32_t id)
{
    if ((id & 0xFFFF) != 0x8086) return false;
    for (uint16_t d : ids) if (d == id >> 16) return true;
    return false;
}

uint16_t eeprom(int word)
{
    // 82540/82545-style EERD (done bit 4, address at 8), else 82541+ (done bit 1, address at 2)
    wr(EERD, 1 | (uint32_t)word << 8);
    for (int i = 0; i < 10000; i++) { uint32_t v = rd(EERD); if (v & 0x10) return v >> 16; udelay(1); }
    wr(EERD, 1 | (uint32_t)word << 2);
    for (int i = 0; i < 10000; i++) { uint32_t v = rd(EERD); if (v & 2) return v >> 16; udelay(1); }
    return 0xFFFF;
}

void set_mode(int m)
{
    uint32_t r = rd(RCTL) & ~((1u << 3) | (1u << 4) | (1u << 15));
    if (m >= 3) r |= 1u << 15;                 // BAM
    if (m >= 4) r |= 1u << 4;                  // MPE
    if (m >= 6) r |= 1u << 3;                  // UPE
    wr(RCTL, r);
}

bool init(const PciDevice& d)
{
    uint32_t bar = pci_read(d, 0x10);
    uint64_t base = bar & ~0xFull;
    if ((bar & 6) == 4) base |= (uint64_t)pci_read(d, 0x14) << 32;
    if (bar & 1 || !base) return false;
    pci_write(d, 0x04, (pci_read(d, 0x04) | 0x06) & ~0x400u);   // memory, bus master, INTx on (masked in the card)
    mm = (volatile uint8_t*)map_mmio64(base, 0x20000);
    if (!mm) return false;

    wr(IMC, 0xFFFFFFFF);
    wr(CTRL, rd(CTRL) | (1u << 26));            // RST
    udelay(20000);
    for (int i = 0; i < 1000 && (rd(CTRL) & (1u << 26)); i++) udelay(100);
    wr(IMC, 0xFFFFFFFF);
    rd(ICR);
    uint32_t c = rd(CTRL);
    c |= (1u << 6) | (1u << 5);                 // SLU, ASDE
    c &= ~((1u << 3) | (1u << 7) | (1u << 30) | (1u << 31));   // LRST, ILOS, VME, PHY_RST
    wr(CTRL, c);

    uint32_t lo = rd(RAL), hi = rd(RAH);
    if (!(hi & 0x80000000u) || (!lo && !(hi & 0xFFFF))) {
        uint16_t w0 = eeprom(0), w1 = eeprom(1), w2 = eeprom(2);
        lo = w0 | (uint32_t)w1 << 16; hi = w2;
        wr(RAL, lo); wr(RAH, hi | 0x80000000u);
    }
    for (int i = 0; i < 4; i++) mac[i] = (uint8_t)(lo >> (8 * i));
    mac[4] = (uint8_t)hi; mac[5] = (uint8_t)(hi >> 8);
    for (int i = 0; i < 128; i++) wr(MTA + 4 * i, 0);

    rx = (volatile RxD*)phys_alloc(NRX * sizeof(RxD));
    tx = (volatile TxD*)phys_alloc(NTX * sizeof(TxD));
    rxb = (uint8_t*)phys_alloc(NRX * 2048);
    txb = (uint8_t*)phys_alloc(NTX * 2048);
    for (int i = 0; i < NRX; i++) { rx[i].addr = (uint32_t)(uintptr_t)(rxb + i * 2048); rx[i].status = 0; }
    for (int i = 0; i < NTX; i++) { tx[i].addr = (uint32_t)(uintptr_t)(txb + i * 2048); tx[i].status = 1; }   // DD: free

    wr(RDBAL, (uint32_t)(uintptr_t)rx); wr(RDBAH, 0);
    wr(RDLEN, NRX * sizeof(RxD));
    wr(RDH, 0); wr(RDT, NRX - 1);
    rx_cur = 0;
    wr(RCTL, (1u << 1) | (1u << 15) | (1u << 26));   // EN, BAM, SECRC; 2048-byte buffers

    wr(TDBAL, (uint32_t)(uintptr_t)tx); wr(TDBAH, 0);
    wr(TDLEN, NTX * sizeof(TxD));
    wr(TDH, 0); wr(TDT, 0);
    tx_cur = 0;
    wr(TIPG, 0x0060200A);
    wr(TCTL, (1u << 1) | (1u << 3) | (0x0Fu << 4) | (0x40u << 12) | (1u << 24));   // EN, PSP, CT, COLD, RTLC
    return true;
}

bool link() { return rd(STATUS) & 2; }

void poll()
{
    for (int n = 0; n < NRX; n++) {
        volatile RxD& d = rx[rx_cur];
        if (!(d.status & 1)) break;             // DD
        BARRIER();
        if ((d.status & 2) && !d.errors) enqueue(rxb + rx_cur * 2048, d.len);
        else st.rx_err++;
        d.status = 0;
        BARRIER();
        wr(RDT, rx_cur);
        rx_cur = (rx_cur + 1) % NRX;
    }
}

bool send(const uint8_t* f, unsigned len)
{
    volatile TxD& d = tx[tx_cur];
    for (int i = 0; i < 2000 && !(d.status & 1); i++) udelay(5);
    if (!(d.status & 1)) return false;
    memcpy(txb + tx_cur * 2048, f, len);
    d.len = (uint16_t)len;
    d.cso = 0; d.css = 0; d.special = 0;
    d.cmd = 0x01 | 0x02 | 0x08;                 // EOP, IFCS, RS
    d.status = 0;
    BARRIER();
    tx_cur = (tx_cur + 1) % NTX;
    wr(TDT, tx_cur);
    return true;
}
} // namespace e1k

// ---------------- Realtek RTL8139 ----------------
namespace rtl {
uint16_t io;
uint8_t* rxb;
uint8_t* txb;
int tx_cur;
uint32_t rx_off;
const uint32_t RXLEN = 8192;

enum { IDR = 0x00, MAR = 0x08, TSD = 0x10, TSAD = 0x20, RBSTART = 0x30, CR = 0x37, CAPR = 0x38,
       IMR = 0x3C, ISR = 0x3E, TCR = 0x40, RCR = 0x44, CONFIG1 = 0x52, MSR = 0x58 };

uint32_t rcr_for(int m)
{
    uint32_t r = (1u << 7) | (7u << 8) | (7u << 13);   // WRAP, MXDMA unlimited, RXFTH none
    if (m >= 2) r |= 1u << 1;                   // APM
    if (m >= 3) r |= 1u << 3;                   // AB
    if (m >= 4) r |= 1u << 2;                   // AM
    if (m >= 6) r |= 1u << 0;                   // AAP
    return r;
}

void set_mode(int m)
{
    for (int i = 0; i < 8; i++) outb(io + MAR + i, m >= 4 ? 0xFF : 0);
    outl(io + RCR, rcr_for(m));
}

void rx_reset()
{
    outb(io + CR, 0x04);                        // TE only: receiver off
    rx_off = 0;
    outl(io + RBSTART, (uint32_t)(uintptr_t)rxb);
    outb(io + CR, 0x0C);
    outl(io + RCR, rcr_for(rx_mode));
    outw(io + CAPR, (uint16_t)(0 - 16));
}

bool init(const PciDevice& d)
{
    uint32_t bar = pci_read(d, 0x10);
    if (!(bar & 1)) return false;
    io = (uint16_t)(bar & 0xFFFC);
    pci_write(d, 0x04, (pci_read(d, 0x04) | 0x05) & ~0x400u);   // I/O, bus master
    outb(io + CONFIG1, 0);
    outb(io + CR, 0x10);
    for (int i = 0; i < 1000 && (inb(io + CR) & 0x10); i++) udelay(100);
    for (int i = 0; i < 6; i++) mac[i] = inb(io + IDR + i);
    rxb = (uint8_t*)phys_alloc(RXLEN + 16 + 2048);
    txb = (uint8_t*)phys_alloc(4 * 2048);
    outw(io + IMR, 0);
    outw(io + ISR, 0xFFFF);
    outl(io + RBSTART, (uint32_t)(uintptr_t)rxb);
    outb(io + CR, 0x0C);                        // RE, TE
    outl(io + TCR, (6u << 8) | (3u << 24));     // MXDMA 1024, normal IFG
    set_mode(rx_mode);
    outw(io + CAPR, (uint16_t)(0 - 16));
    rx_off = 0;
    tx_cur = 0;
    return true;
}

bool link() { return !(inb(io + MSR) & 4); }    // LINKB: 1 = link down

void poll()
{
    for (int n = 0; n < 64 && !(inb(io + CR) & 1); n++) {   // BUFE clear: a frame is there
        uint16_t status = *(volatile uint16_t*)(rxb + rx_off);
        uint16_t len = *(volatile uint16_t*)(rxb + rx_off + 2);
        if (status == 0 && len == 0xFFF0) break;          // early receive, not there yet
        if (!(status & 1) || len < 18 || len > 1518 + 4) { st.rx_err++; rx_reset(); break; }
        enqueue(rxb + rx_off + 4, len - 4);
        rx_off = (rx_off + len + 4 + 3) & ~3u;
        rx_off %= RXLEN;
        outw(io + CAPR, (uint16_t)(rx_off - 16));
    }
    uint16_t isr = inw(io + ISR);
    if (isr) {
        outw(io + ISR, isr);
        if (isr & 0x10) rx_reset();             // RXOVW
    }
}

bool send(const uint8_t* f, unsigned len)
{
    uint32_t tsd = io + TSD + 4 * tx_cur;
    for (int i = 0; i < 2000 && !(inl(tsd) & (1u << 13)); i++) udelay(5);   // OWN: the card is done with it
    if (!(inl(tsd) & (1u << 13))) return false;
    uint8_t* b = txb + tx_cur * 2048;
    memcpy(b, f, len);
    if (len < 60) { memset(b + len, 0, 60 - len); len = 60; }
    outl(io + TSAD + 4 * tx_cur, (uint32_t)(uintptr_t)b);
    outl(tsd, len);                             // clears OWN: sends
    tx_cur = (tx_cur + 1) & 3;
    return true;
}
} // namespace rtl

void poll_card()
{
    if (kind == KIND_E1000) e1k::poll();
    else if (kind == KIND_RTL8139) rtl::poll();
}

bool link_up()
{
    return kind == KIND_E1000 ? e1k::link() : kind == KIND_RTL8139 ? rtl::link() : false;
}

const char* kind_name() { return kind == KIND_E1000 ? "Intel e1000" : kind == KIND_RTL8139 ? "RTL8139" : "none"; }

// The nth Ethernet card (class 02h/00h) vmdos can drive.
bool find(int want, PciDevice* out, int* k)
{
    PciDevice d;
    for (int i = 0, seen = 0; pci_find_class(0x02, 0x00, -1, &d, i); i++) {
        uint32_t id = pci_read(d, 0);
        int kk = e1k::match(id) ? KIND_E1000 : id == 0x813910EC ? KIND_RTL8139 : KIND_NONE;
        kprintf("net: PCI %02x:%02x.%u %04x:%04x%s\n", d.bus, d.dev, d.fn, id & 0xFFFF, id >> 16,
                kk ? "" : " (not supported)");
        if (kk && seen++ == want) { *out = d; *k = kk; return true; }
    }
    return false;
}

} // namespace

extern "C" int net_ready;
int net_ready;

extern "C" void net_init(void)
{
    if (strstr(cmdline, "net=off")) return;
    log_on = strstr(cmdline, "netlog") != 0;
    int want = 0;
    if (const char* p = strstr(cmdline, "nic=")) want = p[4] - '0';
    PciDevice d;
    int k;
    if (!find(want, &d, &k)) { kprintf("net: no Ethernet card vmdos can drive\n"); return; }
    qbuf = (uint8_t*)phys_alloc(QLEN * FRAME_MAX);
    bool ok = k == KIND_E1000 ? e1k::init(d) : rtl::init(d);
    if (!ok) { kprintf("net: %s didn't start\n", k == KIND_E1000 ? "e1000" : "RTL8139"); return; }
    kind = k;
    kprintf("net: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x, link %s\n", kind_name(),
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], link_up() ? "up" : "down");
    net_ready = 1;
}

// Every timer tick.
extern "C" void net_tick(void)
{
    poll_card();
    if (q_count && irq_line >= 0) vpic_raise(irq_line);
}

extern "C" void net_api(struct regs* r)
{
    int fn = BL(r);
    BX(r) = 0x564D;
    switch (fn) {
    case 0: {
        AX(r) = kind ? 1 : 0;
        CL(r) = link_up();
        CH(r) = (uint8_t)kind;
        poll_card();
        DX(r) = (uint16_t)q_count;
        if (kind) memcpy(gptr(LIN(r->v86_es, DI(r))), mac, 6);
        return; }
    case 1:
        irq_line = CL(r) < 16 ? CL(r) : -1;
        if (log_on) kprintf("net: IRQ %d\n", irq_line);
        return;
    case 2: {
        unsigned len = CX(r);
        const uint8_t* f = gptr(LIN(r->v86_ds, SI(r)));
        bool ok = kind && len >= 14 && len <= 1514 &&
                  (kind == KIND_E1000 ? e1k::send(f, len) : rtl::send(f, len));
        if (ok) { st.tx++; st.tx_bytes += len; } else st.tx_err++;
        if (log_on) kprintf("net: out %u bytes, type %02x%02x, to %02x:%02x:%02x:%02x:%02x:%02x%s\n", len, f[12], f[13],
                            f[0], f[1], f[2], f[3], f[4], f[5], ok ? "" : " FAILED");
        AX(r) = ok ? 0 : 1;
        return; }
    case 3: {
        if (!q_count) poll_card();
        if (!q_count) { CX(r) = 0; DX(r) = 0; return; }
        unsigned len = qlen_of[q_head], room = CX(r);
        memcpy(gptr(LIN(r->v86_es, DI(r))), qbuf + q_head * FRAME_MAX, len < room ? len : room);
        q_head = (q_head + 1) % QLEN;
        q_count--;
        CX(r) = (uint16_t)len;
        DX(r) = (uint16_t)q_count;
        return; }
    case 4: {
        int m = CL(r);
        if (m < 1 || m > 6) { AX(r) = 1; return; }
        rx_mode = m;
        if (kind == KIND_E1000) e1k::set_mode(m);
        else if (kind == KIND_RTL8139) rtl::set_mode(m);
        if (m == 1) q_count = 0;
        AX(r) = 0;
        return; }
    case 5:
        memcpy(gptr(LIN(r->v86_es, DI(r))), &st, sizeof st);
        AX(r) = 0;
        return;
    }
    AX(r) = 0xFFFF;
}
