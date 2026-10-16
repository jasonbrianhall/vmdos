// Network: the PC's own Ethernet cards, for DOS's packet driver (VMPKT.COM).
//
// vmdos drives the cards itself, polled from the timer tick (their own
// interrupts stay masked). Every card it can drive is started at boot; one
// of them is in use at a time. Received frames wait in a queue here; while
// it holds any, the IRQ VMPKT asked for is raised in the virtual PIC, and
// VMPKT's handler takes them out one at a time (INT 2Fh AX=564Eh BL=3) and
// hands them to its clients. Sending is one call (BL=2), straight to the
// card. DOS never sees the cards themselves (VMHV's PCI reads FFh anyway).
//
// Which card: nic=N on the kernel command line (1 = the first in PCI
// order), or VMNIC.COM later. Otherwise "auto": the first card, and while
// it has no link another one that has. Once DOS has asked for the card's
// address (VMPKT does, as it loads), that address stays: a card switched to
// later is given it, so VMPKT, VMIPX and their clients carry on unchanged.
//
// Cards: Intel 8254x/8257x/I21x "e1000" (QEMU's default, -nic model=e1000
// or e1000e), Realtek RTL8139 (-nic model=rtl8139), Realtek RTL8111/8168,
// RTL8125 (2.5G) and RTL810x (10/100) on real PCs. Kernel options: net=off (none), nic=N,
// netlog (every call and frame in the log).
//
// INT 2Fh AX=564Eh ('N'), BL = function; BX = 564Dh ('VM') on return:
//   0  query: AX = 1 card / 0 none, CL = link up, CH = card kind
//      (1 e1000, 2 RTL8139, 3 RTL8168/8125), DX = frames waiting,
//      ES:DI <- the MAC (6 bytes). From then on that MAC stays.
//   1  CL = IRQ to raise while frames wait (FFh: none)
//   2  send DS:SI, CX bytes (14..1514): AX = 0 sent, 1 failed
//   3  receive: the next frame into ES:DI (CX = room), CX = its length
//      (0: none waiting; a frame longer than the room is cut), DX = still waiting
//   4  receive mode CL (packet driver numbering: 1 off, 2 own address,
//      3 + broadcast, 4/5 + multicast, 6 everything): AX = 0, 1 not supported
//   5  statistics into ES:DI: 7 dwords: frames in, frames out, bytes in,
//      bytes out, receive errors, send errors, frames dropped (queue full)
//   6  the cards into ES:DI, 32 bytes each (at most 8): AX = how many,
//      CX = the one in use (0-based, FFFFh none), DX bit 0 = auto.
//      Each: kind, state (0 off/failed, 1 running), link, bus, device,
//      function, vendor (word), device ID (word), its own MAC (6), the
//      address it sends with (6), name (10 bytes at 22, NUL-terminated).
//   7  use card CL (0-based; FFh: auto): AX = 0, 1 no such card, 2 it didn't start
#include "pci.hpp"
#include "hw.hpp"
#include "../third_party/rtl_nic/rtl8106e-1.h"

namespace {

enum { KIND_NONE, KIND_E1000, KIND_RTL8139, KIND_R8169 };
const int QLEN = 64, FRAME_MAX = 1536, MAXCARDS = 8;

bool log_on;
int irq_line = -1;
int rx_mode = 3;

struct Stats { uint32_t rx, tx, rx_bytes, tx_bytes, rx_err, tx_err, dropped; } st;

#define BARRIER() __asm__ volatile("" ::: "memory")

void udelay(uint32_t us) { while (us--) inb(0x80); }      // ~1 us each (works with interrupts off)

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

// Room for another frame? While the queue is full, frames stay in the
// card's own ring (more buffering; under QEMU the sender waits too).
bool room() { return q_count < QLEN || rx_mode == 1; }

struct Card {
    int kind;
    PciDevice pci;
    uint16_t vendor, device;
    bool running;
    uint8_t mac[6];                             // its own
    char name[20];
    // e1000 / r8169: MMIO; RTL8139: I/O
    volatile uint8_t* mm;
    uint16_t io;
    uint8_t *rxd, *txd, *rxb, *txb;
    int rx_cur, tx_cur;
    uint32_t rx_off;                            // RTL8139
    // r8169
    uint16_t xid;
    bool is8125, ocp_phy, gen_g;                // 8125 family; PHY over OCP (8168g+); 8168g..8117
    bool evl_up;
};
Card cards[MAXCARDS];
int ncards;
int active = -1;
bool auto_pick = true;
bool mac_fixed;                                 // DOS has the address
uint8_t dos_mac[6];

uint32_t r32(Card& c, int r) { return *(volatile uint32_t*)(c.mm + r); }
uint16_t r16(Card& c, int r) { return *(volatile uint16_t*)(c.mm + r); }
uint8_t r8(Card& c, int r) { return c.mm[r]; }
void w32(Card& c, int r, uint32_t v) { *(volatile uint32_t*)(c.mm + r) = v; }
void w16(Card& c, int r, uint16_t v) { *(volatile uint16_t*)(c.mm + r) = v; }
void w8(Card& c, int r, uint8_t v) { c.mm[r] = v; }

// PCI power state D0 (full power), as pci_enable_device does: the firmware
// may have left the card in D3. Returns the state it was in.
int pci_d0(const PciDevice& d)
{
    if (!(pci_read(d, 0x04) & (1u << 20))) return -1;   // no capability list
    int cap = pci_read(d, 0x34) & 0xFC;
    for (int n = 0; cap && n < 48; n++) {
        uint32_t h = pci_read(d, cap);
        if ((h & 0xFF) == 0x01) {               // power management
            uint32_t pmcsr = pci_read(d, cap + 4);
            int st = pmcsr & 3;
            if (st) {
                pci_write(d, cap + 4, (pmcsr & ~3u) | 0x8000);   // D0 (and clear PME status)
                udelay(10000);
            }
            return st;
        }
        cap = (h >> 8) & 0xFC;
    }
    return -1;
}

// The card's first memory BAR, mapped (0: none).
volatile uint8_t* map_bar(const PciDevice& d, uint32_t len)
{
    for (int b = 0; b < 6; b++) {
        uint32_t bar = pci_read(d, 0x10 + 4 * b);
        if (bar & 1) continue;                  // I/O
        uint64_t base = bar & ~0xFull;
        bool is64 = (bar & 6) == 4;
        if (is64) base |= (uint64_t)pci_read(d, 0x14 + 4 * b) << 32;
        if (base) return (volatile uint8_t*)map_mmio64(base, len);
        if (is64) b++;
    }
    return 0;
}

// ---------------- Intel e1000 (legacy descriptors) ----------------
namespace e1k {
const int NRX = 32, NTX = 16;
struct RxD { uint64_t addr; uint16_t len, csum; uint8_t status, errors; uint16_t special; };
struct TxD { uint64_t addr; uint16_t len; uint8_t cso, cmd, status, css; uint16_t special; };
enum {
    CTRL = 0x0, STATUS = 0x8, EERD = 0x14, ICR = 0xC0, IMC = 0xD8,
    RCTL = 0x100, TCTL = 0x400, TIPG = 0x410,
    RDBAL = 0x2800, RDBAH = 0x2804, RDLEN = 0x2808, RDH = 0x2810, RDT = 0x2818,
    TDBAL = 0x3800, TDBAH = 0x3804, TDLEN = 0x3808, TDH = 0x3810, TDT = 0x3818,
    MTA = 0x5200, RAL = 0x5400, RAH = 0x5404,
};

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

uint16_t eeprom(Card& c, int word)
{
    // 82540/82545-style EERD (done bit 4, address at 8), else 82541+ (done bit 1, address at 2)
    w32(c, EERD, 1 | (uint32_t)word << 8);
    for (int i = 0; i < 10000; i++) { uint32_t v = r32(c, EERD); if (v & 0x10) return v >> 16; udelay(1); }
    w32(c, EERD, 1 | (uint32_t)word << 2);
    for (int i = 0; i < 10000; i++) { uint32_t v = r32(c, EERD); if (v & 2) return v >> 16; udelay(1); }
    return 0xFFFF;
}

void set_mode(Card& c, int m)
{
    uint32_t r = r32(c, RCTL) & ~((1u << 1) | (1u << 3) | (1u << 4) | (1u << 15));
    if (m >= 2) r |= 1u << 1;                   // EN
    if (m >= 3) r |= 1u << 15;                  // BAM
    if (m >= 4) r |= 1u << 4;                   // MPE
    if (m >= 6) r |= 1u << 3;                   // UPE
    w32(c, RCTL, r);
}

void set_mac(Card& c, const uint8_t* a)
{
    w32(c, RAL, a[0] | a[1] << 8 | a[2] << 16 | (uint32_t)a[3] << 24);
    w32(c, RAH, a[4] | a[5] << 8 | 0x80000000u);
}

bool init(Card& c)
{
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x06) & ~0x400u);   // memory, bus master
    c.mm = map_bar(c.pci, 0x20000);
    if (!c.mm) return false;

    w32(c, IMC, 0xFFFFFFFF);
    w32(c, CTRL, r32(c, CTRL) | (1u << 26));    // RST
    udelay(20000);
    for (int i = 0; i < 1000 && (r32(c, CTRL) & (1u << 26)); i++) udelay(100);
    w32(c, IMC, 0xFFFFFFFF);
    r32(c, ICR);
    uint32_t ctl = r32(c, CTRL);
    ctl |= (1u << 6) | (1u << 5);               // SLU, ASDE
    ctl &= ~((1u << 3) | (1u << 7) | (1u << 30) | (1u << 31));   // LRST, ILOS, VME, PHY_RST
    w32(c, CTRL, ctl);

    uint32_t lo = r32(c, RAL), hi = r32(c, RAH);
    if (!(hi & 0x80000000u) || (!lo && !(hi & 0xFFFF))) {
        uint16_t a = eeprom(c, 0), b = eeprom(c, 1), d = eeprom(c, 2);
        lo = a | (uint32_t)b << 16; hi = d;
        w32(c, RAL, lo); w32(c, RAH, hi | 0x80000000u);
    }
    for (int i = 0; i < 4; i++) c.mac[i] = (uint8_t)(lo >> (8 * i));
    c.mac[4] = (uint8_t)hi; c.mac[5] = (uint8_t)(hi >> 8);
    for (int i = 0; i < 128; i++) w32(c, MTA + 4 * i, 0);

    c.rxd = (uint8_t*)phys_alloc(NRX * sizeof(RxD));
    c.txd = (uint8_t*)phys_alloc(NTX * sizeof(TxD));
    c.rxb = (uint8_t*)phys_alloc(NRX * 2048);
    c.txb = (uint8_t*)phys_alloc(NTX * 2048);
    volatile RxD* rx = (volatile RxD*)c.rxd;
    volatile TxD* tx = (volatile TxD*)c.txd;
    for (int i = 0; i < NRX; i++) { rx[i].addr = (uint32_t)(uintptr_t)(c.rxb + i * 2048); rx[i].status = 0; }
    for (int i = 0; i < NTX; i++) { tx[i].addr = (uint32_t)(uintptr_t)(c.txb + i * 2048); tx[i].status = 1; }   // DD: free

    w32(c, RDBAL, (uint32_t)(uintptr_t)c.rxd); w32(c, RDBAH, 0);
    w32(c, RDLEN, NRX * sizeof(RxD));
    w32(c, RDH, 0); w32(c, RDT, NRX - 1);
    c.rx_cur = 0;
    w32(c, RCTL, 1u << 26);                     // SECRC; 2048-byte buffers; off until set_mode
    w32(c, TDBAL, (uint32_t)(uintptr_t)c.txd); w32(c, TDBAH, 0);
    w32(c, TDLEN, NTX * sizeof(TxD));
    w32(c, TDH, 0); w32(c, TDT, 0);
    c.tx_cur = 0;
    w32(c, TIPG, 0x0060200A);
    w32(c, TCTL, (1u << 1) | (1u << 3) | (0x0Fu << 4) | (0x40u << 12) | (1u << 24));   // EN, PSP, CT, COLD, RTLC
    snprintf(c.name, sizeof c.name, "Intel");
    return true;
}

bool link(Card& c) { return r32(c, STATUS) & 2; }

void poll(Card& c)
{
    volatile RxD* rx = (volatile RxD*)c.rxd;
    for (int n = 0; n < NRX; n++) {
        volatile RxD& d = rx[c.rx_cur];
        if (!(d.status & 1) || !room()) break;  // DD
        BARRIER();
        if ((d.status & 2) && !d.errors) enqueue(c.rxb + c.rx_cur * 2048, d.len);
        else st.rx_err++;
        d.status = 0;
        BARRIER();
        w32(c, RDT, c.rx_cur);
        c.rx_cur = (c.rx_cur + 1) % NRX;
    }
}

bool send(Card& c, const uint8_t* f, unsigned len)
{
    volatile TxD& d = ((volatile TxD*)c.txd)[c.tx_cur];
    for (int i = 0; i < 2000 && !(d.status & 1); i++) udelay(5);
    if (!(d.status & 1)) return false;
    memcpy(c.txb + c.tx_cur * 2048, f, len);
    d.len = (uint16_t)len;
    d.cso = 0; d.css = 0; d.special = 0;
    d.cmd = 0x01 | 0x02 | 0x08;                 // EOP, IFCS, RS
    d.status = 0;
    BARRIER();
    c.tx_cur = (c.tx_cur + 1) % NTX;
    w32(c, TDT, c.tx_cur);
    return true;
}
} // namespace e1k

// ---------------- Realtek RTL8139 ----------------
namespace rtl {
const uint32_t RXLEN = 8192;
enum { IDR = 0x00, MAR = 0x08, TSD = 0x10, TSAD = 0x20, RBSTART = 0x30, CR = 0x37, CAPR = 0x38,
       IMR = 0x3C, ISR = 0x3E, TCR = 0x40, RCR = 0x44, CFG9346 = 0x50, CONFIG1 = 0x52, MSR = 0x58 };

uint32_t rcr_for(int m)
{
    uint32_t r = (1u << 7) | (7u << 8) | (7u << 13);   // WRAP, MXDMA unlimited, RXFTH none
    if (m >= 2) r |= 1u << 1;                   // APM
    if (m >= 3) r |= 1u << 3;                   // AB
    if (m >= 4) r |= 1u << 2;                   // AM
    if (m >= 6) r |= 1u << 0;                   // AAP
    return r;
}

void set_mode(Card& c, int m)
{
    for (int i = 0; i < 8; i++) outb(c.io + MAR + i, m >= 4 ? 0xFF : 0);
    outl(c.io + RCR, rcr_for(m));
}

void set_mac(Card& c, const uint8_t* a)
{
    outb(c.io + CFG9346, 0xC0);
    outl(c.io + IDR, a[0] | a[1] << 8 | a[2] << 16 | (uint32_t)a[3] << 24);
    outl(c.io + IDR + 4, a[4] | a[5] << 8);
    outb(c.io + CFG9346, 0);
}

void rx_reset(Card& c)
{
    outb(c.io + CR, 0x04);                      // TE only: receiver off
    c.rx_off = 0;
    outl(c.io + RBSTART, (uint32_t)(uintptr_t)c.rxb);
    outb(c.io + CR, 0x0C);
    outl(c.io + RCR, rcr_for(rx_mode));
    outw(c.io + CAPR, (uint16_t)(0 - 16));
}

bool init(Card& c)
{
    uint32_t bar = pci_read(c.pci, 0x10);
    if (!(bar & 1)) return false;
    c.io = (uint16_t)(bar & 0xFFFC);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x05) & ~0x400u);   // I/O, bus master
    outb(c.io + CONFIG1, 0);
    outb(c.io + CR, 0x10);
    for (int i = 0; i < 1000 && (inb(c.io + CR) & 0x10); i++) udelay(100);
    for (int i = 0; i < 6; i++) c.mac[i] = inb(c.io + IDR + i);
    c.rxb = (uint8_t*)phys_alloc(RXLEN + 16 + 2048);
    c.txb = (uint8_t*)phys_alloc(4 * 2048);
    outw(c.io + IMR, 0);
    outw(c.io + ISR, 0xFFFF);
    outl(c.io + RBSTART, (uint32_t)(uintptr_t)c.rxb);
    outb(c.io + CR, 0x0C);                      // RE, TE
    outl(c.io + TCR, (6u << 8) | (3u << 24));   // MXDMA 1024, normal IFG
    set_mode(c, 1);
    outw(c.io + CAPR, (uint16_t)(0 - 16));
    c.rx_off = 0;
    c.tx_cur = 0;
    snprintf(c.name, sizeof c.name, "RTL8139");
    return true;
}

bool link(Card& c) { return !(inb(c.io + MSR) & 4); }   // LINKB: 1 = link down

void poll(Card& c)
{
    for (int n = 0; n < 64 && room() && !(inb(c.io + CR) & 1); n++) {   // BUFE clear: a frame is there
        uint16_t status = *(volatile uint16_t*)(c.rxb + c.rx_off);
        uint16_t len = *(volatile uint16_t*)(c.rxb + c.rx_off + 2);
        if (status == 0 && len == 0xFFF0) break;          // early receive, not there yet
        if (!(status & 1) || len < 18 || len > 1518 + 4) { st.rx_err++; rx_reset(c); break; }
        enqueue(c.rxb + c.rx_off + 4, len - 4);
        c.rx_off = (c.rx_off + len + 4 + 3) & ~3u;
        c.rx_off %= RXLEN;
        outw(c.io + CAPR, (uint16_t)(c.rx_off - 16));
    }
    uint16_t isr = inw(c.io + ISR);
    if (isr) {
        outw(c.io + ISR, isr);
        if (isr & 0x10) rx_reset(c);            // RXOVW
    }
}

bool send(Card& c, const uint8_t* f, unsigned len)
{
    uint32_t tsd = c.io + TSD + 4 * c.tx_cur;
    for (int i = 0; i < 2000 && !(inl(tsd) & (1u << 13)); i++) udelay(5);   // OWN: the card is done with it
    if (!(inl(tsd) & (1u << 13))) return false;
    uint8_t* b = c.txb + c.tx_cur * 2048;
    memcpy(b, f, len);
    if (len < 60) { memset(b + len, 0, 60 - len); len = 60; }
    outl(c.io + TSAD + 4 * c.tx_cur, (uint32_t)(uintptr_t)b);
    outl(tsd, len);                             // clears OWN: sends
    c.tx_cur = (c.tx_cur + 1) & 3;
    return true;
}
} // namespace rtl

// ---------------- Realtek RTL8111/8168, RTL8125 (Linux: r8169) ----------------
// The descriptor-ring design these share. vmdos does what Linux's r8169
// does to start one, less the per-chip PHY and PCIe tuning: it relies on
// the firmware (BIOS/UEFI) having powered and set up the PHY, as it
// normally has, and only wakes the PHY if it was left powered down.
namespace r8169 {
const int NRX = 32, NTX = 16, BUF = 2048;
struct Desc { uint32_t opts1, opts2; uint64_t addr; };
enum {
    MAC0 = 0x00, MAC4 = 0x04, MAR0 = 0x08, TxDescLo = 0x20, TxDescHi = 0x24,
    ChipCmd = 0x37, TxPoll = 0x38, IntrMask = 0x3C, IntrStatus = 0x3E,
    IntrMask8125 = 0x38, IntrStatus8125 = 0x3C, IntCfg0_8125 = 0x34, IntCfg1_8125 = 0x7A,
    TxConfig = 0x40, RxConfig = 0x44, Cfg9346 = 0x50, PHYAR = 0x60, PHYstatus = 0x6C,
    TxPoll8125 = 0x90, OCPDR = 0xB0, GPHY_OCP = 0xB8, RxMaxSize = 0xDA, CPlusCmd = 0xE0,
    IntrMitigate = 0xE2, RxDescLo = 0xE4, RxDescHi = 0xE8, MaxTxPacketSize = 0xEC, MISC = 0xF0,
    RSS_CTRL_8125 = 0x4500, Q_NUM_CTRL_8125 = 0x4800,
};
const uint32_t OWN = 1u << 31, EOR = 1u << 30, FS = 1u << 29, LS = 1u << 28, RES = 1u << 21;

struct Chip { uint16_t xid; const char* name; uint8_t ver; };
// XID ((TxConfig >> 20) & 0xFCF) -> Linux's mac_version; 0x7C8-masked ones
// are matched with that mask too.
const Chip chips[] = {
    { 0x689, "RTL8125D", 64 }, { 0x688, "RTL8125D", 64 }, { 0x68A, "RTL8125K", 64 }, { 0x68B, "RTL9151A", 64 },
    { 0x708, "RTL8125CP", 65 }, { 0x681, "RTL8125BP", 66 },
    { 0x641, "RTL8125B", 63 }, { 0x609, "RTL8125A", 61 },
    { 0x54B, "RTL8117", 52 }, { 0x54A, "RTL8117", 52 }, { 0x502, "RTL8168ep", 51 },
    { 0x541, "RTL8168h", 46 }, { 0x6C0, "RTL8168M", 46 },
    { 0x5C8, "RTL8411b", 44 }, { 0x509, "RTL8168gu", 42 }, { 0x4C0, "RTL8168g", 40 },
    { 0x481, "RTL8168f", 36 }, { 0x480, "RTL8168f", 35 }, { 0x2C1, "RTL8168e", 32 },
    { 0x281, "RTL8168d", 25 }, { 0x28A, "RTL8168dp", 28 }, { 0x28B, "RTL8168dp", 31 },
    { 0x3C9, "RTL8168cp", 23 }, { 0x3C8, "RTL8168cp", 18 }, { 0x3C2, "RTL8168c", 20 }, { 0x3C0, "RTL8168c", 19 },
    { 0x384, "RTL8168b", 17 }, { 0x380, "RTL8168b", 17 }, { 0x300, "RTL8168b", 11 },
    { 0x3C3, "RTL8168c", 21 },
    // the 8101 family: Fast Ethernet (10EC:8136)
    { 0x409, "RTL8105e", 29 }, { 0x349, "RTL8102e", 8 }, { 0x249, "RTL8102e", 8 },
    { 0x348, "RTL8102e", 7 }, { 0x248, "RTL8102e", 7 }, { 0x240, "RTL8401", 14 },
};
const Chip masked[] = {                         // matched on XID & 0x7C8
    { 0x488, "RTL8411", 38 }, { 0x2C8, "RTL8168evl", 34 }, { 0x2C0, "RTL8168e", 33 },
    { 0x280, "RTL8168d", 26 }, { 0x3C8, "RTL8168cp", 24 }, { 0x3C0, "RTL8168c", 22 },
    { 0x380, "RTL8168b", 17 },
    { 0x448, "RTL8106e", 39 }, { 0x440, "RTL8402", 37 }, { 0x408, "RTL8105e", 30 },
    { 0x348, "RTL8103e", 9 }, { 0x248, "RTL8103e", 9 }, { 0x340, "RTL8101e", 10 },
};

bool match(uint32_t id)
{
    if ((id & 0xFFFF) != 0x10EC) return false;
    uint16_t d = id >> 16;
    return d == 0x8168 || d == 0x8161 || d == 0x8162 || d == 0x8125 || d == 0x3000 || d == 0x5000 ||
           d == 0x8136;                           // RTL810x Fast Ethernet
}

void unlock(Card& c) { w8(c, Cfg9346, 0xC0); }
void lock(Card& c) { w8(c, Cfg9346, 0x00); }

uint16_t mac_ocp_read(Card& c, uint32_t reg) { w32(c, OCPDR, reg << 15); return (uint16_t)r32(c, OCPDR); }
void mac_ocp_write(Card& c, uint32_t reg, uint16_t v) { w32(c, OCPDR, 0x80000000u | reg << 15 | v); }
void mac_ocp_modify(Card& c, uint32_t reg, uint16_t clear, uint16_t set)
{
    mac_ocp_write(c, reg, (uint16_t)((mac_ocp_read(c, reg) & ~clear) | set));
}

int phy_read(Card& c, int reg)
{
    if (c.ocp_phy) {
        w32(c, GPHY_OCP, (uint32_t)(0xA400 + reg * 2) << 15);
        for (int i = 0; i < 100; i++) { udelay(10); uint32_t v = r32(c, GPHY_OCP); if (v & 0x80000000u) return v & 0xFFFF; }
        return -1;
    }
    w32(c, PHYAR, (uint32_t)(reg & 0x1F) << 16);
    for (int i = 0; i < 100; i++) {
        udelay(20);
        uint32_t v = r32(c, PHYAR);
        if (v & 0x80000000u) { udelay(20); return v & 0xFFFF; }
    }
    return -1;
}

void phy_write(Card& c, int reg, uint16_t v)
{
    if (c.ocp_phy) {
        w32(c, GPHY_OCP, 0x80000000u | (uint32_t)(0xA400 + reg * 2) << 15 | v);
        for (int i = 0; i < 100 && (r32(c, GPHY_OCP) & 0x80000000u); i++) udelay(10);
        return;
    }
    w32(c, PHYAR, 0x80000000u | (uint32_t)(reg & 0x1F) << 16 | v);
    for (int i = 0; i < 100 && (r32(c, PHYAR) & 0x80000000u); i++) udelay(20);
    udelay(20);
}

uint32_t accept_bits(int m)
{
    uint32_t a = 0;
    if (m >= 2) a |= 0x02;                      // AcceptMyPhys
    if (m >= 3) a |= 0x08;                      // AcceptBroadcast
    if (m >= 4) a |= 0x04;                      // AcceptMulticast
    if (m >= 6) a |= 0x01;                      // AcceptAllPhys
    return a;
}

void set_mode(Card& c, int m)
{
    w32(c, MAR0 + 4, m >= 4 ? 0xFFFFFFFF : 0);
    w32(c, MAR0, m >= 4 ? 0xFFFFFFFF : 0);
    w32(c, RxConfig, (r32(c, RxConfig) & ~0x3Fu) | accept_bits(m));
}

void set_mac(Card& c, const uint8_t* a)
{
    unlock(c);
    w32(c, MAC4, a[4] | a[5] << 8);
    r32(c, MAC4);
    w32(c, MAC0, a[0] | a[1] << 8 | a[2] << 16 | (uint32_t)a[3] << 24);
    r32(c, MAC0);
    lock(c);
}

void irq_off(Card& c)
{
    if (c.is8125) { w32(c, IntrMask8125, 0); w32(c, IntrStatus8125, 0xFFFFFFFF); }
    else { w16(c, IntrMask, 0); w16(c, IntrStatus, 0xFFFF); }
}

void dump_phy(Card& c, const char* why);

void eri_write(Card& c, int addr, uint32_t mask, uint32_t v)   // ERI (ExGMAC) register write
{
    w32(c, 0x70, v);                            // ERIDR
    w32(c, 0x74, 0x80000000u | mask << 12 | (uint32_t)addr);   // ERIAR: write, EXGMAC
    for (int i = 0; i < 100 && (r32(c, 0x74) & 0x80000000u); i++) udelay(10);
}

bool init(Card& c)
{
    int was = pci_d0(c.pci);
    if (was > 0) kprintf("net: %02x:%02x.%u was in PCI power state D%d: now D0\n", c.pci.bus, c.pci.dev, c.pci.fn, was);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x06) & ~0x400u);   // memory, bus master
    c.mm = map_bar(c.pci, 0x10000);
    if (!c.mm) return false;
    uint32_t txc = r32(c, TxConfig);
    if (txc == 0xFFFFFFFF) { kprintf("net: %02x:%02x.%u doesn't answer\n", c.pci.bus, c.pci.dev, c.pci.fn); return false; }
    c.xid = (txc >> 20) & 0xFCF;
    const Chip* chip = 0;
    for (const Chip& k : chips) if (k.xid == c.xid) { chip = &k; break; }
    if (!chip) for (const Chip& k : masked) if (k.xid == (c.xid & 0x7C8)) { chip = &k; break; }
    if (!chip) {
        kprintf("net: Realtek %04x, chip XID %03x vmdos doesn't know\n", c.device, c.xid);
        return false;
    }
    snprintf(c.name, sizeof c.name, "%s", chip->name);
    c.is8125 = chip->ver >= 61;
    c.ocp_phy = chip->ver >= 40;
    c.gen_g = chip->ver >= 40 && chip->ver <= 52;
    c.evl_up = chip->ver >= 34 && chip->ver != 39 && chip->ver <= 52;
    dbg(1, "net: %s (XID %03x, r8169 version %u)\n", chip->name, c.xid, chip->ver);

    irq_off(c);
    // stop it (it may still run from the firmware's network boot), then reset
    if (chip->ver >= 40) { w32(c, MISC, r32(c, MISC) | (1u << 19)); udelay(2000); }   // RXDV gate
    else { w8(c, ChipCmd, r8(c, ChipCmd) | 0x80); udelay(100); }              // StopReq
    w8(c, ChipCmd, 0x10);                       // CmdReset
    for (int i = 0; i < 1000 && (r8(c, ChipCmd) & 0x10); i++) udelay(100);
    if (r8(c, ChipCmd) & 0x10) { kprintf("net: %s didn't come out of reset\n", c.name); return false; }

    uint32_t m0 = r32(c, MAC0), m4 = r32(c, MAC4);
    for (int i = 0; i < 4; i++) c.mac[i] = (uint8_t)(m0 >> (8 * i));
    c.mac[4] = (uint8_t)m4; c.mac[5] = (uint8_t)(m4 >> 8);

    // the PHY: wake it if the firmware left it powered down
    int bmcr = phy_read(c, 0);
    dbg(1, "net: %s PHY BMCR %04x, PHYstatus %02x\n", c.name, bmcr & 0xFFFF, r8(c, PHYstatus));
    if (bmcr >= 0 && (bmcr & 0x0800)) {
        phy_write(c, 0, (uint16_t)((bmcr & ~0x0800) | 0x1200));   // power up, autonegotiate
        kprintf("net: %s PHY was powered down: woken, negotiating\n", c.name);
    }
    bool fe = chip->ver == 29 || chip->ver == 30 || chip->ver == 37 || chip->ver == 39;   // the 8105e/8402/8106e
    if (fe) {
        phy_write(c, 0x1F, 0);                  // PHY page 0
        phy_write(c, 0x18, 0x0310);             // ALDPS (link-down power saving) off, as Linux does
        udelay(100000);
    }
    if (chip->ver == 39) {                      // rtl8106e_hw_phy_config: the PHY patch, then its settings
        for (const auto& w : rtl8106e_1_fw) phy_write(c, w[0], w[1]);
        static const uint16_t after[][2] = { { 0x1F, 4 }, { 0x10, 0xC07F }, { 0x19, 0x7030 }, { 0x1F, 0 } };
        for (const auto& w : after) phy_write(c, w[0], w[1]);
        for (int i = 0; i < 60 && (phy_read(c, 0) & 0x8000); i++) udelay(10000);
        dbg(1, "net: %s: PHY patch (%u writes) applied\n", c.name, (unsigned)(sizeof rtl8106e_1_fw / sizeof rtl8106e_1_fw[0]));
    }

    // rings
    c.rxd = (uint8_t*)phys_alloc(NRX * sizeof(Desc));
    c.txd = (uint8_t*)phys_alloc(NTX * sizeof(Desc));
    c.rxb = (uint8_t*)phys_alloc(NRX * BUF);
    c.txb = (uint8_t*)phys_alloc(NTX * BUF);
    volatile Desc* rx = (volatile Desc*)c.rxd;
    volatile Desc* tx = (volatile Desc*)c.txd;
    for (int i = 0; i < NRX; i++) {
        rx[i].addr = (uint32_t)(uintptr_t)(c.rxb + i * BUF);
        rx[i].opts2 = 0;
        rx[i].opts1 = OWN | (i == NRX - 1 ? EOR : 0) | BUF;
    }
    for (int i = 0; i < NTX; i++) {
        tx[i].addr = (uint32_t)(uintptr_t)(c.txb + i * BUF);
        tx[i].opts2 = 0;
        tx[i].opts1 = i == NTX - 1 ? EOR : 0;
    }
    c.rx_cur = c.tx_cur = 0;

    // rtl_hw_start, the parts every chip shares
    unlock(c);
    w16(c, CPlusCmd, r16(c, CPlusCmd) & ~0x0063u);   // no VLAN / checksum offload, no interrupt timer
    if (c.is8125) {
        w8(c, IntCfg0_8125, 0);
        for (int i = 0xA00; i < 0xA80; i += 4) w32(c, i, 0);   // interrupt coalescing off
        if (chip->ver == 63) w16(c, IntCfg1_8125, 0);
        w32(c, RSS_CTRL_8125, 0);
        w16(c, Q_NUM_CTRL_8125, 0);
        mac_ocp_modify(c, 0xEB58, 0x0001, 0);   // the old descriptor format
    } else {
        w8(c, MaxTxPacketSize, c.evl_up ? 0x27 : 0x3F);
        w16(c, IntrMitigate, 0);
        if (fe) {
            w32(c, MISC, r32(c, MISC) | 0x002800);   // FuncEvent: leave ASPM while Rx/Tx are busy
            w8(c, 0xD3, r8(c, 0xD3) | 0x0C);         // MCU: EN_NDP, EN_OOB_RESET
        }
        if (chip->ver == 39) {
            w32(c, MISC, (r32(c, MISC) | (1u << 23)) & ~(1u << 16));   // DISABLE_LAN_EN, no early tally
            w8(c, 0xD0, r8(c, 0xD0) & ~0x40);        // DLLPR: PFM off
            eri_write(c, 0x1D0, 3, 0);
            eri_write(c, 0x1B0, 3, 0);               // EEE off
        }
    }
    // the PLL stays up (Linux: rtl_set_d3_pll_down; older ones powered it up here for the 8106e)
    if (!c.is8125 && chip->ver >= 25 && chip->ver != 28 && chip->ver != 31 && chip->ver != 38)
        w8(c, 0x6F, r8(c, 0x6F) | 0xC0);           // PMCH: D3hot / D3cold no PLL down
    if (chip->ver >= 40) w32(c, MISC, r32(c, MISC) & ~(1u << 19));   // RXDV gate off
    w16(c, RxMaxSize, BUF);
    w32(c, TxDescHi, 0); w32(c, TxDescLo, (uint32_t)(uintptr_t)c.txd);
    w32(c, RxDescHi, 0); w32(c, RxDescLo, (uint32_t)(uintptr_t)c.rxd);
    lock(c);
    r8(c, ChipCmd);
    w8(c, ChipCmd, 0x0C);                       // TxEnb, RxEnb

    uint32_t rxc;
    if (c.is8125) rxc = (8u << 27) | (7u << 8) | (chip->ver >= 63 ? (1u << 11) : 0);
    else if (c.gen_g) rxc = (1u << 15) | (1u << 14) | (7u << 8) | (1u << 11);
    else if ((chip->ver >= 18 && chip->ver <= 24) || (chip->ver >= 34 && chip->ver <= 36) || chip->ver == 38)
        rxc = (1u << 15) | (1u << 14) | (7u << 8);   // RX128_INT_EN, RX_MULTI_EN
    else if (chip->ver <= 17 && (chip->ver < 7 || chip->ver > 9)) rxc = (7u << 13) | (7u << 8);   // FIFO threshold, DMA burst
    else rxc = (1u << 15) | (7u << 8);
    w32(c, RxConfig, rxc);                      // accepting nothing yet
    w32(c, TxConfig, (7u << 8) | (3u << 24) | (c.evl_up ? (1u << 7) : 0));
    irq_off(c);
    phy_write(c, 0x1F, 0);
    phy_write(c, 0, 0x9200);                    // PHY soft reset, autonegotiate afresh (genphy_soft_reset)
    if (debug_level >= 1) dump_phy(c, "started");
    return true;
}

// The PHY's own word on it (BMSR bit 2, latched: read twice), as Linux's
// phylib; the MAC's PHYstatus copy too.
bool link(Card& c)
{
    if (r8(c, PHYstatus) & 2) return true;
    phy_write(c, 0x1F, 0);
    phy_read(c, 1);
    int bmsr = phy_read(c, 1);
    return bmsr >= 0 && bmsr != 0xFFFF && (bmsr & 4);
}

void dump_phy(Card& c, const char* why)
{
    phy_write(c, 0x1F, 0);
    int bmcr = phy_read(c, 0), bmsr1 = phy_read(c, 1), bmsr = phy_read(c, 1);
    int id1 = phy_read(c, 2), id2 = phy_read(c, 3), adv = phy_read(c, 4), lpa = phy_read(c, 5);
    kprintf("net: %s %s: PHY id %04x:%04x BMCR %04x BMSR %04x/%04x ADV %04x LPA %04x, PHYstatus %02x, ChipCmd %02x\n",
            c.name, why, id1 & 0xFFFF, id2 & 0xFFFF, bmcr & 0xFFFF, bmsr1 & 0xFFFF, bmsr & 0xFFFF, adv & 0xFFFF, lpa & 0xFFFF,
            r8(c, PHYstatus), r8(c, ChipCmd));
    kprintf("net: %s %s: Config0-5 %02x %02x %02x %02x %02x %02x PMCH %02x MISC %08x MCU %02x DLLPR %02x PHY 18h %04x\n",
            c.name, why, r8(c, 0x51), r8(c, 0x52), r8(c, 0x53), r8(c, 0x54), r8(c, 0x55), r8(c, 0x56), r8(c, 0x6F),
            r32(c, MISC), r8(c, 0xD3), r8(c, 0xD0), phy_read(c, 0x18) & 0xFFFF);
}

void poll(Card& c)
{
    volatile Desc* rx = (volatile Desc*)c.rxd;
    for (int n = 0; n < NRX; n++) {
        volatile Desc& d = rx[c.rx_cur];
        uint32_t s = d.opts1;
        if ((s & OWN) || !room()) break;
        BARRIER();
        if ((s & RES) || (s & (FS | LS)) != (FS | LS)) st.rx_err++;
        else {
            unsigned len = s & 0x3FFF;
            if (len > 4) enqueue(c.rxb + c.rx_cur * BUF, len - 4);   // less the CRC
        }
        d.opts2 = 0;
        BARRIER();
        d.opts1 = OWN | (c.rx_cur == NRX - 1 ? EOR : 0) | BUF;
        c.rx_cur = (c.rx_cur + 1) % NRX;
    }
    if (c.is8125) { uint32_t s = r32(c, IntrStatus8125); if (s) w32(c, IntrStatus8125, s); }
    else { uint16_t s = r16(c, IntrStatus); if (s) w16(c, IntrStatus, s); }
}

bool send(Card& c, const uint8_t* f, unsigned len)
{
    volatile Desc& d = ((volatile Desc*)c.txd)[c.tx_cur];
    for (int i = 0; i < 2000 && (d.opts1 & OWN); i++) udelay(5);
    if (d.opts1 & OWN) return false;
    uint8_t* b = c.txb + c.tx_cur * BUF;
    memcpy(b, f, len);
    if (len < 60) { memset(b + len, 0, 60 - len); len = 60; }
    d.opts2 = 0;
    BARRIER();
    d.opts1 = OWN | FS | LS | (c.tx_cur == NTX - 1 ? EOR : 0) | len;
    BARRIER();
    if (c.is8125) w16(c, TxPoll8125, 1);
    else w8(c, TxPoll, 0x40);                   // NPQ
    c.tx_cur = (c.tx_cur + 1) % NTX;
    return true;
}
} // namespace r8169

// ---------------- the cards ----------------
bool card_link(Card& c)
{
    if (!c.running) return false;
    switch (c.kind) {
    case KIND_E1000: return e1k::link(c);
    case KIND_RTL8139: return rtl::link(c);
    case KIND_R8169: return r8169::link(c);
    }
    return false;
}

void card_mode(Card& c, int m)                  // 1: receiving nothing
{
    switch (c.kind) {
    case KIND_E1000: e1k::set_mode(c, m); break;
    case KIND_RTL8139: rtl::set_mode(c, m); break;
    case KIND_R8169: r8169::set_mode(c, m); break;
    }
}

void card_mac(Card& c, const uint8_t* a)
{
    switch (c.kind) {
    case KIND_E1000: e1k::set_mac(c, a); break;
    case KIND_RTL8139: rtl::set_mac(c, a); break;
    case KIND_R8169: r8169::set_mac(c, a); break;
    }
}

bool card_start(Card& c)
{
    switch (c.kind) {
    case KIND_E1000: c.running = e1k::init(c); break;
    case KIND_RTL8139: c.running = rtl::init(c); break;
    case KIND_R8169: c.running = r8169::init(c); break;
    }
    return c.running;
}

void poll_card()
{
    if (active < 0) return;
    Card& c = cards[active];
    switch (c.kind) {
    case KIND_E1000: e1k::poll(c); break;
    case KIND_RTL8139: rtl::poll(c); break;
    case KIND_R8169: r8169::poll(c); break;
    }
}

bool send_frame(const uint8_t* f, unsigned len)
{
    if (active < 0) return false;
    Card& c = cards[active];
    switch (c.kind) {
    case KIND_E1000: return e1k::send(c, f, len);
    case KIND_RTL8139: return rtl::send(c, f, len);
    case KIND_R8169: return r8169::send(c, f, len);
    }
    return false;
}

const uint8_t* cur_mac() { return mac_fixed ? dos_mac : active >= 0 ? cards[active].mac : dos_mac; }

void log_mac(const char* what, const uint8_t* m)
{
    kprintf("%s%02x:%02x:%02x:%02x:%02x:%02x", what, m[0], m[1], m[2], m[3], m[4], m[5]);
}

// Make card i the one in use. 0, 1 no such card, 2 it didn't start.
int use_card(int i, const char* why)
{
    if (i < 0 || i >= ncards) return 1;
    Card& c = cards[i];
    if (!c.running) return 2;
    if (active >= 0 && active != i) card_mode(cards[active], 1);
    active = i;
    q_head = q_count = 0;
    card_mac(c, cur_mac());                     // its own, or the one DOS has
    card_mode(c, rx_mode);
    kprintf("net: using card %d, %s at %02x:%02x.%u (%s), link %s", i + 1, c.name, c.pci.bus, c.pci.dev, c.pci.fn,
            why, card_link(c) ? "up" : "down");
    log_mac(", address ", cur_mac());
    kprintf("\n");
    return 0;
}

// Auto: while the card in use has had no link for 3 s, change to one that has.
// Every link change in the log.
void link_watch()
{
    static uint32_t last;
    static uint8_t was[MAXCARDS];
    if (ticks - last < 250) return;
    last = ticks;
    for (int i = 0; i < ncards; i++) {
        if (!cards[i].running) continue;
        uint8_t now = card_link(cards[i]) ? 2 : 1;
        if (was[i] && now != was[i]) kprintf("net: card %d (%s): link %s\n", i + 1, cards[i].name, now == 2 ? "up" : "down");
        was[i] = now;
        // debug=1: what a Realtek PHY says while there's no link, every 2 s for the first 30 s
        static int dumps[MAXCARDS];
        if (debug_level >= 1 && now == 1 && cards[i].kind == KIND_R8169 && dumps[i] < 15 && (ticks / 250) % 8 == 0) {
            dumps[i]++;
            r8169::dump_phy(cards[i], "no link");
        }
    }
}

void auto_check()
{
    static uint32_t down_since, last;
    if (!auto_pick || ncards < 2 || ticks - last < 500) return;
    last = ticks;
    if (active >= 0 && card_link(cards[active])) { down_since = 0; return; }
    if (!down_since) { down_since = ticks | 1; return; }
    if (ticks - down_since < 3000) return;
    for (int i = 0; i < ncards; i++)
        if (i != active && card_link(cards[i])) { use_card(i, "auto: it has a link"); down_since = 0; return; }
}

} // namespace

extern "C" int net_ready;
int net_ready;
extern "C" int ne2k_running(void);
extern "C" void ne2k_tick(void);
extern "C" void ne2k_init(void);

// For the emulated NE2000 (ne2k.c).
extern "C" int net_peek_len(void) { if (!q_count) poll_card(); return q_count ? qlen_of[q_head] : 0; }
extern "C" int net_take(uint8_t* buf, int max)
{
    if (!q_count) return 0;
    int len = qlen_of[q_head];
    memcpy(buf, qbuf + q_head * FRAME_MAX, len < max ? len : max);
    q_head = (q_head + 1) % QLEN;
    q_count--;
    return len;
}
extern "C" int net_send(const uint8_t* f, unsigned len)
{
    bool ok = len >= 14 && len <= 1514 && send_frame(f, len);
    if (ok) { st.tx++; st.tx_bytes += len; } else st.tx_err++;
    if (log_on) kprintf("net: NE2000 out %u bytes, type %02x%02x%s\n", len, f[12], f[13], ok ? "" : " FAILED");
    return ok;
}
extern "C" int net_dos_mac(uint8_t* mac, int fix)          // the address DOS sees (fix: from now on)
{
    if (active < 0) return 0;
    if (fix && !mac_fixed) { memcpy(dos_mac, cards[active].mac, 6); mac_fixed = true; }
    memcpy(mac, cur_mac(), 6);
    return 1;
}
extern "C" void net_rx_mode(int m)
{
    if (m == rx_mode) return;
    rx_mode = m;
    if (active >= 0) card_mode(cards[active], m);
}

static void net_init_cards(void);
extern "C" void net_init(void)
{
    net_init_cards();
    ne2k_init();
}

static void net_init_cards(void)
{
    if (strstr(cmdline, "net=off")) return;
    log_on = strstr(cmdline, "netlog") != 0;
    PciDevice d;
    for (int i = 0; ncards < MAXCARDS && pci_find_class(0x02, 0x00, -1, &d, i); i++) {
        uint32_t id = pci_read(d, 0);
        int k = e1k::match(id) ? KIND_E1000 : id == 0x813910EC ? KIND_RTL8139 : r8169::match(id) ? KIND_R8169 : KIND_NONE;
        kprintf("net: PCI %02x:%02x.%u %04x:%04x%s\n", d.bus, d.dev, d.fn, id & 0xFFFF, id >> 16,
                k ? "" : " (not supported)");
        if (!k) continue;
        Card& c = cards[ncards++];
        c.kind = k; c.pci = d; c.vendor = id & 0xFFFF; c.device = id >> 16;
        if (card_start(c)) {
            kprintf("net: card %d: %s", ncards, c.name);
            log_mac(", MAC ", c.mac);
            kprintf(", link %s\n", card_link(c) ? "up" : "down");
        } else kprintf("net: card %d (%04x:%04x) didn't start\n", ncards, c.vendor, c.device);
    }
    if (!ncards) { kprintf("net: no Ethernet card vmdos can drive\n"); return; }
    qbuf = (uint8_t*)phys_alloc(QLEN * FRAME_MAX);
    net_ready = 1;

    int want = -1;
    if (const char* p = strstr(cmdline, "nic=")) { want = p[4] - '1'; auto_pick = false; }
    if (want >= 0 && use_card(want, "nic=") == 0) return;
    if (want >= 0) kprintf("net: nic=%d: no such card running\n", want + 1);
    auto_pick = true;
    int pick = -1;
    for (int i = 0; i < ncards && pick < 0; i++) if (card_link(cards[i])) pick = i;   // one with a link
    for (int i = 0; i < ncards && pick < 0; i++) if (cards[i].running) pick = i;
    if (pick >= 0) use_card(pick, "auto");
}

// Every timer tick.
extern "C" void net_tick(void)
{
    poll_card();
    link_watch();
    auto_check();
    if (ne2k_running()) ne2k_tick();
    if (q_count && irq_line >= 0) vpic_raise(irq_line);
}

extern "C" void net_api(struct regs* r)
{
    int fn = BL(r);
    BX(r) = 0x564D;
    switch (fn) {
    case 0: {
        AX(r) = active >= 0 ? 1 : 0;
        CL(r) = active >= 0 && card_link(cards[active]);
        CH(r) = active >= 0 ? (uint8_t)cards[active].kind : 0;
        poll_card();
        DX(r) = (uint16_t)q_count;
        if (active >= 0) {
            if (!mac_fixed) { memcpy(dos_mac, cards[active].mac, 6); mac_fixed = true; }
            memcpy(gptr(LIN(r->v86_es, DI(r))), dos_mac, 6);
        }
        return; }
    case 1:
        irq_line = CL(r) < 16 ? CL(r) : -1;
        if (log_on) kprintf("net: IRQ %d\n", irq_line);
        return;
    case 2: {
        unsigned len = CX(r);
        const uint8_t* f = gptr(LIN(r->v86_ds, SI(r)));
        bool ok = len >= 14 && len <= 1514 && send_frame(f, len);
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
        if (active >= 0) card_mode(cards[active], m);
        if (m == 1) q_count = 0;
        AX(r) = 0;
        return; }
    case 5:
        memcpy(gptr(LIN(r->v86_es, DI(r))), &st, sizeof st);
        AX(r) = 0;
        return;
    case 6: {                                   // the cards, 32 bytes each
        uint8_t* o = gptr(LIN(r->v86_es, DI(r)));
        int n = ncards < 8 ? ncards : 8;
        for (int i = 0; i < n; i++, o += 32) {
            Card& c = cards[i];
            memset(o, 0, 32);
            o[0] = (uint8_t)c.kind;
            o[1] = c.running;
            o[2] = card_link(c);
            o[3] = c.pci.bus; o[4] = c.pci.dev; o[5] = c.pci.fn;
            o[6] = (uint8_t)c.vendor; o[7] = (uint8_t)(c.vendor >> 8);
            o[8] = (uint8_t)c.device; o[9] = (uint8_t)(c.device >> 8);
            memcpy(o + 10, c.mac, 6);
            memcpy(o + 16, i == active ? cur_mac() : c.mac, 6);
            for (int k = 0; k < 9 && c.name[k]; k++) o[22 + k] = (uint8_t)c.name[k];   // 9 chars + NUL
        }
        AX(r) = (uint16_t)ncards;
        CX(r) = (uint16_t)(active >= 0 ? active : 0xFFFF);
        DX(r) = auto_pick ? 1 : 0;
        return; }
    case 7: {
        if (CL(r) == 0xFF) { auto_pick = true; AX(r) = 0; kprintf("net: auto\n"); return; }
        int e = use_card(CL(r), "VMNIC");
        if (!e) auto_pick = false;
        AX(r) = (uint16_t)e;
        return; }
    }
    AX(r) = 0xFFFF;
}
