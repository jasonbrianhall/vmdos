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
// or e1000e), Intel 82575/82576/82580/I350/I354/I210/I211 "igb" (-nic model=igb), Realtek RTL8139 (-nic model=rtl8139), Realtek RTL8111/8168,
// RTL8125 (2.5G), RTL8126 (5G), RTL8127 (10G), RTL810x (10/100) and the PCI
// RTL8169/8110 (every chip Linux's r8169 drives) on real PCs, Broadcom Tigon3
// (BCM570x/575x/578x/5906/5717-5762 NetXtreme and NetLink, Linux's tg3). Kernel options: net=off (none), nic=N,
// netlog (every call and frame in the log).
//
// INT 2Fh AX=564Eh ('N'), BL = function; BX = 564Dh ('VM') on return:
//   0  query: AX = 1 card / 0 none, CL = link up, CH = card kind
//      (1 e1000, 2 RTL8139, 3 RTL8168/8125, 4 USB, 5 Broadcom, 6 igb), DX = frames waiting,
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

enum { KIND_NONE, KIND_E1000, KIND_RTL8139, KIND_R8169, KIND_USB, KIND_TG3, KIND_IGB };
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
    uint8_t ver;                                // Linux's r8169 mac_version
    bool evl_up;
    // e1000: the ICH/PCH family (82566-82579, I217-I219): NVM in the
    // chipset's flash, a PHY on its own chip, the ME sharing the MAC
    bool pch;
    // USB (CDC-ECM): its address can't be changed, so while DOS's differs
    // the adapter takes everything and vmdos filters (spoof)
    bool spoof;
    // tg3
    volatile uint8_t* ape;                      // the APE's registers (BAR 2)
    uint32_t chiprev, tflags, misc_host, grc_mode, mac_mode, mi_mode, led, coalesce, clock_ctrl;
    uint32_t dma_rwctrl, rx_mode_reg, ape_hb, hb_tick;
    uint8_t pci_fn, phy_addr, ape_phy_lock, pcie_cap, pcix_cap;
    uint16_t cls_lat, std_prod, ret_cons, tx_prod, tx_cons;
    int std_size, ret_size, speed;
    bool full;
    uint8_t *stblk, *retd;                      // status block, receive return ring
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
    CTRL = 0x0, STATUS = 0x8, EERD = 0x14, CTRL_EXT = 0x18, MDIC = 0x20, ICR = 0xC0, IMC = 0xD8,
    EXTCNF_CTRL = 0xF00, PHY_CTRL = 0xF10, FWSM = 0x5B54,
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
    0x150C,
};
struct Pch { uint16_t id; const char* name; };
const Pch pchs[] = {                            // the chipset's own MAC (Linux: e1000e ich8lan)
    { 0x1049, "82566MM" }, { 0x104A, "82566DM" }, { 0x104B, "82566DC" }, { 0x104C, "82562V" },
    { 0x104D, "82566MC" }, { 0x10C4, "82562GT" }, { 0x10C5, "82562G" },
    { 0x10BD, "82566DM-2" }, { 0x294C, "82566DC-2" }, { 0x10BF, "82567LF" }, { 0x10C0, "82562V-2" },
    { 0x10C2, "82562G-2" }, { 0x10C3, "82562GT-2" }, { 0x10CB, "82567V" }, { 0x10CC, "82567LM-2" },
    { 0x10CD, "82567LF-2" }, { 0x10CE, "82567V-2" }, { 0x10E5, "82567LM-4" }, { 0x10F5, "82567LM" },
    { 0x10DE, "82567LM-3" }, { 0x10DF, "82567LF-3" }, { 0x1501, "82567V-3" },
    { 0x10EA, "82577LM" }, { 0x10EB, "82577LC" }, { 0x10EF, "82578DM" }, { 0x10F0, "82578DC" },
    { 0x1502, "82579LM" }, { 0x1503, "82579V" },
    { 0x153A, "I217-LM" }, { 0x153B, "I217-V" }, { 0x155A, "I218-LM" }, { 0x1559, "I218-V" },
    { 0x15A0, "I218-LM" }, { 0x15A1, "I218-V" }, { 0x15A2, "I218-LM" }, { 0x15A3, "I218-V" },
    { 0x156F, "I219-LM" }, { 0x1570, "I219-V" }, { 0x15B7, "I219-LM" }, { 0x15B8, "I219-V" },
    { 0x15B9, "I219-LM" }, { 0x15BB, "I219-LM" }, { 0x15BC, "I219-V" }, { 0x15BD, "I219-LM" },
    { 0x15BE, "I219-V" }, { 0x15D6, "I219-V" }, { 0x15D7, "I219-LM" }, { 0x15D8, "I219-V" },
    { 0x15E3, "I219-LM" }, { 0x15E1, "I219-LM" }, { 0x15E2, "I219-V" }, { 0x15DF, "I219-LM" },
    { 0x15E0, "I219-V" }, { 0x0D4C, "I219-LM" }, { 0x0D4D, "I219-V" }, { 0x0D4E, "I219-LM" },
    { 0x0D4F, "I219-V" }, { 0x0D53, "I219-LM" }, { 0x0D55, "I219-V" },
};

const Pch* pch_of(uint32_t id)
{
    if ((id & 0xFFFF) != 0x8086) return 0;
    for (const Pch& p : pchs) if (p.id == id >> 16) return &p;
    return 0;
}

bool match(uint32_t id)
{
    if ((id & 0xFFFF) != 0x8086) return false;
    for (uint16_t d : ids) if (d == id >> 16) return true;
    return pch_of(id) != 0;
}

// ICH/PCH: the software flag, which the ME honours, around a reset or PHY access
bool swflag(Card& c)
{
    for (int i = 0; i < 1000 && (r32(c, EXTCNF_CTRL) & 0x20); i++) udelay(1000);
    w32(c, EXTCNF_CTRL, r32(c, EXTCNF_CTRL) | 0x20);
    for (int i = 0; i < 1000; i++) { if (r32(c, EXTCNF_CTRL) & 0x20) return true; udelay(1000); }
    return false;
}
void swflag_off(Card& c) { w32(c, EXTCNF_CTRL, r32(c, EXTCNF_CTRL) & ~0x20u); }

int mdic(Card& c, int phy, int reg, bool write, uint16_t v)
{
    w32(c, MDIC, v | (uint32_t)reg << 16 | (uint32_t)phy << 21 | (write ? 1u : 2u) << 26);
    for (int i = 0; i < 2000; i++) {
        udelay(50);
        uint32_t m = r32(c, MDIC);
        if (m & (1u << 28)) return m & (1u << 30) ? -1 : (int)(m & 0xFFFF);
    }
    return -1;
}

// The PHY (address 2 on a PCH, 1 on an ICH): powered up and negotiating.
// Firmware that didn't use the LAN may have left it powered down.
void pch_phy(Card& c)
{
    if (!swflag(c)) { dbg(1, "net: %s: PHY busy (the ME has it)\n", c.name); return; }
    int addr = 2, id = mdic(c, 2, 2, false, 0);
    if (id <= 0 || id == 0xFFFF) { addr = 1; id = mdic(c, 1, 2, false, 0); }
    int bmcr = mdic(c, addr, 0, false, 0), bmsr = mdic(c, addr, 1, false, 0);
    dbg(1, "net: %s PHY %d id %04x, BMCR %04x, BMSR %04x, PHY_CTRL %x, FWSM %x\n", c.name, addr, id & 0xFFFF,
        bmcr & 0xFFFF, bmsr & 0xFFFF, r32(c, PHY_CTRL), r32(c, FWSM));
    if (bmcr >= 0 && (bmcr & 0x0C00)) {         // power down / isolate
        mdic(c, addr, 0, true, (uint16_t)((bmcr & ~0x0C00) | 0x1200));   // autonegotiate, restart
        kprintf("net: %s: PHY was powered down, woken\n", c.name);
    }
    swflag_off(c);
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
    const Pch* pch = pch_of((uint32_t)c.device << 16 | c.vendor);
    c.pch = pch != 0;
    if (c.pch) pci_d0(c.pci);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x06) & ~0x400u);   // memory, bus master
    c.mm = map_bar(c.pci, 0x20000);
    if (!c.mm) return false;
    if (pch) snprintf(c.name, sizeof c.name, "%s", pch->name);
    else snprintf(c.name, sizeof c.name, "Intel");

    w32(c, IMC, 0xFFFFFFFF);
    if (c.pch) {                                // as e1000e: stop DMA, take the flag, reset
        w32(c, CTRL, r32(c, CTRL) | (1u << 2));                 // GIO master disable
        for (int i = 0; i < 800 && (r32(c, STATUS) & (1u << 19)); i++) udelay(100);
        if (!swflag(c)) dbg(1, "net: %s: no software flag, resetting anyway\n", c.name);
    }
    w32(c, CTRL, r32(c, CTRL) | (1u << 26));    // RST (frees the flag)
    udelay(20000);
    for (int i = 0; i < 1000 && (r32(c, CTRL) & (1u << 26)); i++) udelay(100);
    if (c.pch) {                                // the NVM's settings loaded
        for (int i = 0; i < 150 && !(r32(c, STATUS) & (1u << 9)); i++) udelay(100);
        w32(c, STATUS, r32(c, STATUS) & ~(1u << 9));
        w32(c, CTRL_EXT, r32(c, CTRL_EXT) | (1u << 28));        // DRV_LOAD: a driver has it (to the ME)
        if (r32(c, PHY_CTRL) & 0x40) w32(c, PHY_CTRL, r32(c, PHY_CTRL) & ~0x40u);   // GbE disable
    }
    w32(c, IMC, 0xFFFFFFFF);
    r32(c, ICR);
    uint32_t ctl = r32(c, CTRL);
    ctl |= (1u << 6) | (1u << 5);               // SLU, ASDE
    ctl &= ~((1u << 3) | (1u << 7) | (1u << 30) | (1u << 31));   // LRST, ILOS, VME, PHY_RST
    w32(c, CTRL, ctl);

    if (c.pch) pch_phy(c);

    uint32_t lo = r32(c, RAL), hi = r32(c, RAH);
    if (!c.pch && (!(hi & 0x80000000u) || (!lo && !(hi & 0xFFFF)))) {
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
    if (c.pch && !(hi & 0x80000000u)) kprintf("net: %s: no address from the NVM\n", c.name);
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

// ---------------- Intel 82575/82576/82580/I350/I354/I210/I211, DH89xxCC (Linux: igb) ----------------
// One receive and one transmit queue (queue 0) with advanced descriptors,
// as Linux's igb sets them up (igb_reset_hw_82575/82580, igb_configure_rx/
// tx_ring), polled. Copper ports on the internal PHY, which keeps the boot
// code's settings (woken if powered down); SerDes/SGMII ports are tried as
// the NVM set them up. The flashless I210's PLL workaround is done too.
namespace igb {
const int NRX = 32, NTX = 16, BUF = 2048;
enum {
    CTRL = 0x0, STATUS = 0x8, EECD = 0x10, CTRL_EXT = 0x18, MDIC = 0x20, ICR = 0xC0, IMC = 0xD8,
    RCTL = 0x100, TCTL = 0x400, TCTL_EXT = 0x404, MDICNFG = 0xE04, EEMNGCTL = 0x1010, EIMC = 0x1528,
    RDBAL = 0x2800, RDBAH = 0x2804, RDLEN = 0x2808, SRRCTL = 0x280C, RDH = 0x2810, RDT = 0x2818, RXDCTL = 0x2828,
    TDBAL = 0x3800, TDBAH = 0x3804, TDLEN = 0x3808, TDH = 0x3810, TDT = 0x3818, TXDCTL = 0x3828,
    MTA = 0x5200, RAL = 0x5400, RAH = 0x5404, VMOLR = 0x5AD0, WUC = 0x5800, GCR = 0x5B00, SWSM = 0x5B50,
    SW_FW_SYNC = 0x5B5C, EEARBC_I210 = 0x12024, EEMNGCTL_I210 = 0x12030, INVM_DATA = 0x12120,
};
enum { T82575, T82576, T82580, TI350, TI354, TI210, TI211 };

struct Dev { uint16_t id; uint8_t type; const char* name; };
const Dev devs[] = {
    { 0x10A7, T82575, "82575EB" }, { 0x10A9, T82575, "82575EB" }, { 0x10D6, T82575, "82575GB" },
    { 0x10C9, T82576, "82576" }, { 0x10E6, T82576, "82576" }, { 0x10E7, T82576, "82576" }, { 0x10E8, T82576, "82576" },
    { 0x1526, T82576, "82576" }, { 0x150A, T82576, "82576NS" }, { 0x1518, T82576, "82576NS" }, { 0x150D, T82576, "82576" },
    { 0x150E, T82580, "82580" }, { 0x150F, T82580, "82580" }, { 0x1510, T82580, "82580" }, { 0x1511, T82580, "82580" },
    { 0x1516, T82580, "82580" }, { 0x1527, T82580, "82580" },
    { 0x0438, T82580, "DH89xxCC" }, { 0x043A, T82580, "DH89xxCC" }, { 0x043C, T82580, "DH89xxCC" }, { 0x0440, T82580, "DH89xxCC" },
    { 0x1521, TI350, "I350" }, { 0x1522, TI350, "I350" }, { 0x1523, TI350, "I350" }, { 0x1524, TI350, "I350" },
    { 0x1F40, TI354, "I354" }, { 0x1F41, TI354, "I354" }, { 0x1F45, TI354, "I354" },
    { 0x1533, TI210, "I210" }, { 0x1536, TI210, "I210" }, { 0x1537, TI210, "I210" }, { 0x1538, TI210, "I210" },
    { 0x157B, TI210, "I210" }, { 0x157C, TI210, "I210" },
    { 0x1539, TI211, "I211" },
};

const Dev* dev_of(uint32_t id)
{
    if ((id & 0xFFFF) != 0x8086) return 0;
    for (const Dev& d : devs) if (d.id == id >> 16) return &d;
    return 0;
}
bool match(uint32_t id) { return dev_of(id) != 0; }

// The software/firmware semaphore (SWSM, then SW_FW_SYNC) around PHY
// access: the manageability firmware shares the PHY.
bool hw_sema(Card& c)
{
    int i = 0;
    for (; i < 2000 && (r32(c, SWSM) & 1); i++) udelay(50);   // SMBI
    if (i == 2000) { w32(c, SWSM, r32(c, SWSM) & ~3u); }      // stuck: cleared once, as igb does on the I210
    for (i = 0; i < 2000; i++) {
        w32(c, SWSM, r32(c, SWSM) | 2);         // SWESMBI
        if (r32(c, SWSM) & 2) return true;
        udelay(50);
    }
    w32(c, SWSM, r32(c, SWSM) & ~3u);
    return false;
}
void hw_sema_put(Card& c) { w32(c, SWSM, r32(c, SWSM) & ~3u); }
uint32_t phy_mask(Card& c) { static const uint32_t m[4] = { 2, 4, 0x20, 0x40 }; return m[c.pci_fn & 3]; }
bool phy_get(Card& c)
{
    uint32_t m = phy_mask(c);
    for (int i = 0; i < 200; i++) {
        if (!hw_sema(c)) return false;
        uint32_t s = r32(c, SW_FW_SYNC);
        if (!(s & (m | m << 16))) { w32(c, SW_FW_SYNC, s | m); hw_sema_put(c); return true; }
        hw_sema_put(c);
        udelay(5000);
    }
    return false;
}
void phy_put(Card& c)
{
    for (int i = 0; i < 2000 && !hw_sema(c); i++) udelay(50);
    w32(c, SW_FW_SYNC, r32(c, SW_FW_SYNC) & ~phy_mask(c));
    hw_sema_put(c);
}
int mdic(Card& c, int reg, bool write, uint16_t v)
{
    w32(c, MDIC, v | (uint32_t)reg << 16 | (uint32_t)c.phy_addr << 21 | (write ? 1u : 2u) << 26);
    for (int i = 0; i < 1920; i++) {
        udelay(50);
        uint32_t m = r32(c, MDIC);
        if (m & (1u << 28)) return m & (1u << 30) ? -1 : (int)(m & 0xFFFF);
    }
    return -1;
}
int phy_rd(Card& c, int reg) { bool g = phy_get(c); int v = mdic(c, reg, false, 0); if (g) phy_put(c); return v; }
void phy_wr(Card& c, int reg, uint16_t v) { bool g = phy_get(c); mdic(c, reg, true, v); if (g) phy_put(c); }

// igb_pll_workaround_i210: a flashless I210's PHY PLL may not lock at power-up
void pll_workaround(Card& c)
{
    uint32_t wuc = r32(c, WUC), mdicnfg = r32(c, MDICNFG);
    w32(c, MDICNFG, mdicnfg & ~0x80000000u);    // the internal PHY
    uint16_t al = 0x202F;                       // E1000_INVM_DEFAULT_AL, unless the iNVM has the word
    for (int i = 0; i < 64; i++) {
        uint32_t d = r32(c, INVM_DATA + 4 * i);
        int type = d & 7;
        if (type == 0) break;
        if (type == 2) i += 1;
        if (type == 4) i += 8;
        if (type == 1 && ((d >> 9) & 0x7F) == 0x0A) { al = (uint16_t)(d >> 16); break; }
    }
    uint16_t tmp = al | 0x0010;                 // PLL_WO_VAL
    phy_wr(c, 0x16, 0xFC);                      // PLL frequency page
    bool ok = false;
    for (int i = 0; i < 5; i++) {
        int w = phy_rd(c, 0x0E);
        if (w >= 0 && (w & 0xFF) != 0xFF) { ok = true; break; }
        w32(c, CTRL, r32(c, CTRL) | 0x80000000u);                     // PHY reset
        w32(c, CTRL_EXT, r32(c, CTRL_EXT) | 0x00100000u | 0x00040000u);   // PHYPDEN, SDLPE
        w32(c, WUC, 0);
        w32(c, EEARBC_I210, 0x0A << 4 | (uint32_t)tmp << 16);
        int pm = 0x44;                          // PMCSR through D3 and back
        uint32_t v = pci_read(c.pci, pm & ~3);
        pci_write(c.pci, pm & ~3, v | 3);
        udelay(1500);
        pci_write(c.pci, pm & ~3, v & ~3u);
        w32(c, EEARBC_I210, 0x0A << 4 | (uint32_t)al << 16);
        w32(c, WUC, wuc);
    }
    phy_wr(c, 0x16, 0);
    w32(c, MDICNFG, mdicnfg);
    if (!ok) kprintf("net: %s: the PHY's PLL didn't lock\n", c.name);
}

void set_mode(Card& c, int m)
{
    uint32_t r = r32(c, RCTL) & ~((1u << 1) | (1u << 3) | (1u << 4) | (1u << 15) | (3u << 12) | (3u << 6));
    r |= 1u << 26;                              // SECRC
    if (m >= 2) r |= 1u << 1;                   // EN
    if (m >= 3) r |= 1u << 15;                  // BAM
    if (m >= 4) r |= 1u << 4;                   // MPE
    if (m >= 6) r |= 1u << 3;                   // UPE
    w32(c, RCTL, r);
}

void set_mac(Card& c, const uint8_t* a)
{
    w32(c, RAL, a[0] | a[1] << 8 | a[2] << 16 | (uint32_t)a[3] << 24);
    w32(c, RAH, a[4] | a[5] << 8 | 0x80000000u | (c.chiprev != T82575 ? 0x40000u : 0));   // valid, pool 0
}

bool init(Card& c)
{
    const Dev* dv = dev_of((uint32_t)c.device << 16 | c.vendor);
    c.chiprev = dv->type;
    pci_d0(c.pci);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x06) & ~0x400u);   // memory, bus master
    c.mm = map_bar(c.pci, 0x20000);
    if (!c.mm) return false;
    snprintf(c.name, sizeof c.name, "%s", dv->name);
    if (r32(c, STATUS) == 0xFFFFFFFF) { kprintf("net: %s doesn't answer\n", c.name); return false; }
    c.pci_fn = (r32(c, STATUS) >> 2) & 3;       // the port

    // igb_reset_hw_82575 / _82580
    w32(c, CTRL, r32(c, CTRL) | 4);             // GIO master disable
    for (int i = 0; i < 800 && (r32(c, STATUS) & 0x80000); i++) udelay(100);
    if (c.chiprev <= T82576) {                  // PCIe completion timeout
        uint32_t gcr = r32(c, GCR);
        if (!(gcr & 0xF000)) {
            if (!(gcr & 0x40000)) gcr |= 0x1000;    // 10 ms
            else {
                int cap = 0, p = pci_read(c.pci, 0x34) & 0xFC;
                for (int n = 0; p && n < 48; n++) { uint32_t h = pci_read(c.pci, p); if ((h & 0xFF) == 0x10) { cap = p; break; } p = (h >> 8) & 0xFC; }
                if (cap) pci_write(c.pci, cap + 0x28, pci_read(c.pci, cap + 0x28) | 5);   // 16 ms
            }
        }
        w32(c, GCR, gcr & ~0x10000u);           // no resend
    }
    w32(c, IMC, 0xFFFFFFFF);
    w32(c, RCTL, 0);
    w32(c, TCTL, 1u << 3);                      // PSP
    r32(c, STATUS);
    udelay(10000);
    w32(c, CTRL, r32(c, CTRL) | (1u << 26));    // RST
    r32(c, STATUS);
    udelay(1000);
    for (int i = 0; i < 10 && !(r32(c, EECD) & 0x200); i++) udelay(1000);   // NVM auto-read done
    if (c.chiprev >= T82580) w32(c, STATUS, 0x00100000);   // DEV_RST_SET
    w32(c, IMC, 0xFFFFFFFF);
    w32(c, EIMC, 0xFFFFFFFF);
    r32(c, ICR);
    // the manageability configuration cycle
    uint32_t done = 0x40000u << c.pci_fn;
    int mr = c.chiprev >= TI210 ? EEMNGCTL_I210 : EEMNGCTL;
    for (int i = 0; i < 100 && !(r32(c, mr) & done); i++) udelay(1000);

    w32(c, CTRL_EXT, r32(c, CTRL_EXT) | (1u << 28));   // DRV_LOAD: a driver has it
    uint32_t mdicnfg = c.chiprev >= T82580 ? r32(c, MDICNFG) : 0;
    c.phy_addr = (mdicnfg & 0x80000000u) ? (mdicnfg >> 21) & 0x1F : 1;
    if (c.chiprev >= TI210 && !(r32(c, EECD) & 0x80000)) pll_workaround(c);   // flashless

    uint32_t mode = (r32(c, CTRL_EXT) >> 22) & 3;
    if (mode) dbg(1, "net: %s: %s port, left as the NVM set it up\n", c.name, mode == 2 ? "SGMII" : "SerDes");
    uint32_t ctl = r32(c, CTRL);
    ctl |= 1u << 6;                             // SLU
    ctl &= ~((1u << 11) | (1u << 12) | (1u << 3) | (1u << 7) | (1u << 31));   // FRCSPD, FRCDPLX, LRST, ILOS, PHY_RST
    w32(c, CTRL, ctl);
    if (!mode) {                                // copper: the PHY woken if powered down
        int bmcr = phy_rd(c, 0);
        if (bmcr >= 0 && (bmcr & 0x0C00)) {
            phy_wr(c, 0, (uint16_t)((bmcr & ~0x0C00) | 0x1200));
            kprintf("net: %s: PHY was powered down, woken\n", c.name);
        } else if (bmcr >= 0 && !(r32(c, STATUS) & 2)) {
            phy_wr(c, 0, (uint16_t)(bmcr | 0x1200));   // no link yet: negotiate (as igb_setup_copper_link)
        }
        dbg(1, "net: %s PHY %d: BMCR %04x\n", c.name, c.phy_addr, bmcr & 0xFFFF);
    }

    uint32_t lo = r32(c, RAL), hi = r32(c, RAH);
    for (int i = 0; i < 4; i++) c.mac[i] = (uint8_t)(lo >> (8 * i));
    c.mac[4] = (uint8_t)hi; c.mac[5] = (uint8_t)(hi >> 8);
    if (!(hi & 0x80000000u) || (c.mac[0] & 1)) kprintf("net: %s: no address from the NVM\n", c.name);
    set_mac(c, c.mac);
    for (int i = 0; i < 128; i++) w32(c, MTA + 4 * i, 0);

    // rings: queue 0, advanced descriptors (16 bytes each, like the legacy ones)
    c.rxd = (uint8_t*)phys_alloc(NRX * 16);
    c.txd = (uint8_t*)phys_alloc(NTX * 16);
    c.rxb = (uint8_t*)phys_alloc(NRX * BUF);
    c.txb = (uint8_t*)phys_alloc(NTX * BUF);
    volatile uint32_t* rx = (volatile uint32_t*)c.rxd;
    volatile uint32_t* tx = (volatile uint32_t*)c.txd;
    for (int i = 0; i < NRX; i++) { rx[4 * i] = (uint32_t)(uintptr_t)(c.rxb + i * BUF); rx[4 * i + 1] = 0; rx[4 * i + 2] = 0; rx[4 * i + 3] = 0; }
    for (int i = 0; i < NTX; i++) { tx[4 * i] = (uint32_t)(uintptr_t)(c.txb + i * BUF); tx[4 * i + 1] = 0; tx[4 * i + 2] = 0; tx[4 * i + 3] = 1; }   // DD: free

    set_mode(c, 1);
    w32(c, RXDCTL, 0);
    w32(c, RDBAL, (uint32_t)(uintptr_t)c.rxd); w32(c, RDBAH, 0);
    w32(c, RDLEN, NRX * 16);
    w32(c, RDH, 0); w32(c, RDT, 0);
    w32(c, SRRCTL, (256u << 2) | (BUF >> 10) | 0x02000000u);   // header size, 2K buffers, advanced one-buffer
    if (c.chiprev >= T82576) w32(c, VMOLR, (r32(c, VMOLR) & ~(0x08000000u | 0x00020000u)) | 0x01000000u | 0x08000000u);   // untagged, broadcast
    uint32_t pth = c.chiprev == TI354 ? 12 : 8;
    w32(c, RXDCTL, pth | 8u << 8 | 1u << 16 | 0x02000000u);   // write back each descriptor (no interrupts to flush them)
    for (int i = 0; i < 100 && !(r32(c, RXDCTL) & 0x02000000u); i++) udelay(100);
    w32(c, RDT, NRX - 1);
    c.rx_cur = 0;

    w32(c, TXDCTL, 0);
    w32(c, TDLEN, NTX * 16);
    w32(c, TDBAL, (uint32_t)(uintptr_t)c.txd); w32(c, TDBAH, 0);
    w32(c, TDH, 0); w32(c, TDT, 0);
    w32(c, TXDCTL, (c.chiprev == TI354 ? 20u : 8u) | 1u << 8 | 1u << 16 | 0x02000000u);
    for (int i = 0; i < 100 && !(r32(c, TXDCTL) & 0x02000000u); i++) udelay(100);
    c.tx_cur = 0;
    uint32_t tctl = r32(c, TCTL) & ~((0xFFu << 4) | (0x3FFu << 12));
    tctl |= (1u << 3) | (1u << 24) | (15u << 4) | (63u << 12) | (1u << 1);   // PSP, RTLC, CT, COLD, EN
    w32(c, TCTL, tctl);
    return true;
}

bool link(Card& c) { return r32(c, STATUS) & 2; }

void poll(Card& c)
{
    volatile uint32_t* rx = (volatile uint32_t*)c.rxd;
    for (int n = 0; n < NRX; n++) {
        volatile uint32_t* d = rx + 4 * c.rx_cur;
        uint32_t st_err = d[2];
        if (!(st_err & 1) || !room()) break;    // DD
        BARRIER();
        unsigned len = d[3] & 0xFFFF;
        if ((st_err & 2) && !(st_err & 0x80000000u) && len >= 14) enqueue(c.rxb + c.rx_cur * BUF, len);   // EOP, no RXE
        else st.rx_err++;
        d[0] = (uint32_t)(uintptr_t)(c.rxb + c.rx_cur * BUF);   // back to the read format
        d[1] = 0; d[2] = 0; d[3] = 0;
        BARRIER();
        w32(c, RDT, c.rx_cur);
        c.rx_cur = (c.rx_cur + 1) % NRX;
    }
}

bool send(Card& c, const uint8_t* f, unsigned len)
{
    volatile uint32_t* d = (volatile uint32_t*)c.txd + 4 * c.tx_cur;
    for (int i = 0; i < 2000 && !(d[3] & 1); i++) udelay(5);
    if (!(d[3] & 1)) return false;
    memcpy(c.txb + c.tx_cur * BUF, f, len);
    if (len < 60) { memset(c.txb + c.tx_cur * BUF + len, 0, 60 - len); len = 60; }
    d[0] = (uint32_t)(uintptr_t)(c.txb + c.tx_cur * BUF);
    d[1] = 0;
    d[2] = 0x00300000u | 0x20000000u | 0x02000000u | 0x01000000u | 0x08000000u | len;   // data, advanced, IFCS, EOP, RS
    d[3] = len << 14;                           // PAYLEN (and DD clear)
    BARRIER();
    c.tx_cur = (c.tx_cur + 1) % NTX;
    w32(c, TDT, c.tx_cur);
    return true;
}
} // namespace igb

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
    { 0x6C9, "RTL8127A", 80 }, { 0x64A, "RTL8126A", 70 }, { 0x649, "RTL8126A", 70 },   // 10G, 5G
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

const Chip masked8169[] = {                     // the PCI RTL8169/8110: XID & 0xFC8
    { 0x980, "RTL8169sc", 6 }, { 0x180, "RTL8169sc", 5 }, { 0x100, "RTL8169sb", 4 },
    { 0x040, "RTL8110s", 3 }, { 0x008, "RTL8169s", 2 },
};

// Linux r8169's PCI IDs (sub: the subsystem ID, vendor in the low word)
bool match(uint32_t id, uint32_t sub)
{
    uint16_t v = id & 0xFFFF, d = id >> 16;
    if (v == 0x10EC)
        return d == 0x8168 || d == 0x8161 || d == 0x8162 || d == 0x8167 || d == 0x8169 || d == 0x8129 ||
               d == 0x8136 ||                   // RTL810x Fast Ethernet
               d == 0x8125 || d == 0x8126 || d == 0x8127 || d == 0x3000 || d == 0x5000 ||
               d == 0x2502 || d == 0x2600 || d == 0x0E10;   // Killer E2600/E3000 and others
    return id == 0x816810FF ||                  // NCube
           id == 0x43001186 || id == 0x43021186 ||   // D-Link DGE-528T and others
           id == 0xC1071259 ||                  // Allied Telesis
           id == 0x011616EC ||                  // USRobotics
           (id == 0x10321737 && (sub >> 16) == 0x0024) ||   // Linksys EG1032 v3
           (id == 0x81680001 && (sub >> 16) == 0x2410);
}

void unlock(Card& c) { w8(c, Cfg9346, 0xC0); }
void lock(Card& c) { w8(c, Cfg9346, 0x00); }

uint16_t mac_ocp_read(Card& c, uint32_t reg) { w32(c, OCPDR, reg << 15); return (uint16_t)r32(c, OCPDR); }
void mac_ocp_write(Card& c, uint32_t reg, uint16_t v) { w32(c, OCPDR, 0x80000000u | reg << 15 | v); }
void mac_ocp_modify(Card& c, uint32_t reg, uint16_t clear, uint16_t set)
{
    mac_ocp_write(c, reg, (uint16_t)((mac_ocp_read(c, reg) & ~clear) | set));
}

// The 8168dp (r8169 versions 28, 31) shares the MDIO with its management
// firmware: taken (0xD0 bit 17 clear) around each access.
void dp_mdio(Card& c, bool take)
{
    if (c.ver == 28 || c.ver == 31) w32(c, 0xD0, take ? r32(c, 0xD0) & ~0x00020000u : r32(c, 0xD0) | 0x00020000u);
}

int phy_read(Card& c, int reg)
{
    if (c.ver == 28 || c.ver == 31) {
        if (reg == 3) return 0xC912;            // the chip reports a wrong PHY ID
        dp_mdio(c, true);
        w32(c, PHYAR, (uint32_t)(reg & 0x1F) << 16);
        int v = -1;
        for (int i = 0; i < 100; i++) { udelay(20); uint32_t x = r32(c, PHYAR); if (x & 0x80000000u) { udelay(20); v = x & 0xFFFF; break; } }
        dp_mdio(c, false);
        return v;
    }
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
    if (c.ver == 28 || c.ver == 31) {
        dp_mdio(c, true);
        w32(c, PHYAR, 0x80000000u | (uint32_t)(reg & 0x1F) << 16 | v);
        for (int i = 0; i < 100 && (r32(c, PHYAR) & 0x80000000u); i++) udelay(20);
        udelay(20);
        dp_mdio(c, false);
        return;
    }
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

// rtl8169s/sb/scd/sce_hw_phy_config (Linux r8169_phy_config.c)
void phy_8169(Card& c)
{
    static const uint16_t s[][2] = {            // 8169s, 8110s (versions 2, 3)
        { 0x1f, 0x0001 }, { 0x06, 0x006e }, { 0x08, 0x0708 }, { 0x15, 0x4000 }, { 0x18, 0x65c7 },
        { 0x1f, 0x0001 }, { 0x03, 0x00a1 }, { 0x02, 0x0008 }, { 0x01, 0x0120 }, { 0x00, 0x1000 },
        { 0x04, 0x0800 }, { 0x04, 0x0000 }, { 0x03, 0xff41 }, { 0x02, 0xdf60 }, { 0x01, 0x0140 },
        { 0x00, 0x0077 }, { 0x04, 0x7800 }, { 0x04, 0x7000 }, { 0x03, 0x802f }, { 0x02, 0x4f02 },
        { 0x01, 0x0409 }, { 0x00, 0xf0f9 }, { 0x04, 0x9800 }, { 0x04, 0x9000 }, { 0x03, 0xdf01 },
        { 0x02, 0xdf20 }, { 0x01, 0xff95 }, { 0x00, 0xba00 }, { 0x04, 0xa800 }, { 0x04, 0xa000 },
        { 0x03, 0xff41 }, { 0x02, 0xdf20 }, { 0x01, 0x0140 }, { 0x00, 0x00bb }, { 0x04, 0xb800 },
        { 0x04, 0xb000 }, { 0x03, 0xdf41 }, { 0x02, 0xdc60 }, { 0x01, 0x6340 }, { 0x00, 0x007d },
        { 0x04, 0xd800 }, { 0x04, 0xd000 }, { 0x03, 0xdf01 }, { 0x02, 0xdf20 }, { 0x01, 0x100a },
        { 0x00, 0xa0ff }, { 0x04, 0xf800 }, { 0x04, 0xf000 }, { 0x1f, 0x0000 }, { 0x0b, 0x0000 },
        { 0x00, 0x9200 },
    };
    static const uint16_t sc[][2] = {           // the start the 8169sc's two share (versions 5, 6)
        { 0x1f, 0x0001 }, { 0x04, 0x0000 }, { 0x03, 0x00a1 }, { 0x02, 0x0008 }, { 0x01, 0x0120 },
        { 0x00, 0x1000 }, { 0x04, 0x0800 }, { 0x04, 0x9000 }, { 0x03, 0x802f }, { 0x02, 0x4f02 },
        { 0x01, 0x0409 }, { 0x00, 0xf099 }, { 0x04, 0x9800 }, { 0x04, 0xa000 }, { 0x03, 0xdf01 },
        { 0x02, 0xdf20 }, { 0x01, 0xff95 }, { 0x00, 0xba00 }, { 0x04, 0xa800 }, { 0x04, 0xf000 },
        { 0x03, 0xdf01 }, { 0x02, 0xdf20 }, { 0x01, 0x101a }, { 0x00, 0xa0ff }, { 0x04, 0xf800 },
        { 0x04, 0x0000 }, { 0x1f, 0x0000 },
    };
    static const uint16_t scd[][2] = {          // version 5
        { 0x1f, 0x0001 }, { 0x10, 0xf41b }, { 0x14, 0xfb54 }, { 0x18, 0xf5c7 }, { 0x1f, 0x0000 },
        { 0x1f, 0x0001 }, { 0x17, 0x0cc0 }, { 0x1f, 0x0000 },
    };
    static const uint16_t sce[][2] = {          // version 6
        { 0x1f, 0x0001 }, { 0x0b, 0x8480 }, { 0x1f, 0x0000 },
        { 0x1f, 0x0001 }, { 0x18, 0x67c7 }, { 0x04, 0x2000 }, { 0x03, 0x002f }, { 0x02, 0x4360 },
        { 0x01, 0x0109 }, { 0x00, 0x3022 }, { 0x04, 0x2800 }, { 0x1f, 0x0000 },
        { 0x1f, 0x0001 }, { 0x17, 0x0cc0 }, { 0x1f, 0x0000 },
    };
    switch (c.ver) {
    case 2: case 3: for (const auto& w : s) phy_write(c, w[0], w[1]); break;
    case 4: phy_write(c, 0x1f, 2); phy_write(c, 0x01, 0x90d0); phy_write(c, 0x1f, 0); break;
    case 5: for (const auto& w : sc) phy_write(c, w[0], w[1]); for (const auto& w : scd) phy_write(c, w[0], w[1]); break;
    case 6: for (const auto& w : sc) phy_write(c, w[0], w[1]); for (const auto& w : sce) phy_write(c, w[0], w[1]); break;
    }
}

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
    if (!chip) for (const Chip& k : masked8169) if (k.xid == (c.xid & 0xFC8)) { chip = &k; break; }
    static const Chip ext9151 = { 0x7C8, "RTL9151AS", 64 };
    if (!chip && (c.xid & 0x7CF) == 0x7C8 && !(r32(c, 0x60B0) & 0x7FFFFFFF)) chip = &ext9151;   // TX_CONFIG_V2
    if (!chip) {
        kprintf("net: Realtek %04x, chip XID %03x vmdos doesn't know\n", c.device, c.xid);
        return false;
    }
    snprintf(c.name, sizeof c.name, "%s", chip->name);
    if (c.device == 0x8136 && chip->ver == 42) snprintf(c.name, sizeof c.name, "RTL8106eus");   // gigabit MAC, 100M PHY
    if (c.device == 0x8136 && chip->ver == 46) snprintf(c.name, sizeof c.name, "RTL8107e");
    c.ver = chip->ver;
    c.is8125 = chip->ver >= 61;
    c.ocp_phy = chip->ver >= 40;
    c.gen_g = chip->ver >= 40 && chip->ver <= 52;
    c.evl_up = chip->ver >= 34 && chip->ver != 39 && chip->ver <= 52;
    dbg(1, "net: %s (XID %03x, r8169 version %u)\n", chip->name, c.xid, chip->ver);

    irq_off(c);
    // stop it (it may still run from the firmware's network boot), then reset
    if (chip->ver >= 40) { w32(c, MISC, r32(c, MISC) | (1u << 19)); udelay(2000); }   // RXDV gate
    else if (chip->ver > 6) { w8(c, ChipCmd, r8(c, ChipCmd) | 0x80); udelay(100); }   // StopReq (not on the 8169)
    w8(c, ChipCmd, 0x10);                       // CmdReset
    for (int i = 0; i < 1000 && (r8(c, ChipCmd) & 0x10); i++) udelay(100);
    if (r8(c, ChipCmd) & 0x10) { kprintf("net: %s didn't come out of reset\n", c.name); return false; }

    uint32_t m0 = r32(c, MAC0), m4 = r32(c, MAC4);
    for (int i = 0; i < 4; i++) c.mac[i] = (uint8_t)(m0 >> (8 * i));
    c.mac[4] = (uint8_t)m4; c.mac[5] = (uint8_t)(m4 >> 8);

    // Which PHY steps (rtlphy= on the command line, letters): a ALDPS off,
    // p the PHY patch, r a PHY soft reset at the end. Default: all of them,
    // as Linux does.
    bool fe = chip->ver == 29 || chip->ver == 30 || chip->ver == 37 || chip->ver == 39;   // the 8105e/8402/8106e
    const char* steps = "apr";
    if (const char* o = strstr(cmdline, "rtlphy=")) steps = o + 7;
    auto step = [&](char k) { for (const char* q = steps; *q && *q != ' '; q++) if (*q == k) return true; return false; };
    if (debug_level >= 1) dump_phy(c, "found");

    // the PHY: wake it if the firmware left it powered down
    int bmcr = phy_read(c, 0);
    dbg(1, "net: %s PHY BMCR %04x, PHYstatus %02x\n", c.name, bmcr & 0xFFFF, r8(c, PHYstatus));
    if (bmcr >= 0 && (bmcr & 0x0800)) {
        phy_write(c, 0, (uint16_t)((bmcr & ~0x0800) | 0x1200));   // power up, autonegotiate
        kprintf("net: %s PHY was powered down: woken, negotiating\n", c.name);
    }
    if (fe && step('a')) {
        phy_write(c, 0x1F, 0);                  // PHY page 0
        phy_write(c, 0x18, 0x0310);             // ALDPS (link-down power saving) off, as Linux does
        udelay(100000);
    }
    if (chip->ver == 39 && step('p')) {         // rtl8106e_hw_phy_config: the PHY patch, then its settings
        for (const auto& w : rtl8106e_1_fw) phy_write(c, w[0], w[1]);
        static const uint16_t after[][2] = { { 0x1F, 4 }, { 0x10, 0xC07F }, { 0x19, 0x7030 }, { 0x1F, 0 } };
        for (const auto& w : after) phy_write(c, w[0], w[1]);
        for (int i = 0; i < 60 && (phy_read(c, 0) & 0x8000); i++) udelay(10000);
        dbg(1, "net: %s: PHY patch (%u writes) applied\n", c.name, (unsigned)(sizeof rtl8106e_1_fw / sizeof rtl8106e_1_fw[0]));
    }
    if (chip->ver <= 6 && step('p')) phy_8169(c);   // the PCI 8169's PHY settings (r8169_phy_config.c)
    if (chip->ver <= 6) {                       // rtl8169_init_phy
        pci_write(c.pci, 0x0C, (pci_read(c.pci, 0x0C) & 0xFFFF0000u) | 0x4008);   // latency 64, cache line 32
        w8(c, 0x82, 1);                         // undocumented, as Linux
        if (chip->ver == 5 && pci_read(c.pci, 0x2C) == 0xE0001458) {   // Gigabyte
            phy_write(c, 0x1F, 1); phy_write(c, 0x10, 0xF01B); phy_write(c, 0x1F, 0);
        }
    }
    if (chip->ver == 80 && (mac_ocp_read(c, 0xD006) & 0xFF) == 7)
        kprintf("net: %s: an SFP+ port, which vmdos doesn't set up (copper only)\n", c.name);

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
        int end = (chip->ver == 63 || chip->ver == 70) ? 0xA80 : 0xB00;
        for (int i = 0xA00; i < end; i += 4) w32(c, i, 0);   // interrupt coalescing off
        if (chip->ver == 63 || chip->ver == 70 || chip->ver == 80) w16(c, IntCfg1_8125, 0);
        if (chip->ver == 70 || chip->ver == 80) w8(c, 0xD8, r8(c, 0xD8) & ~0x02);
        w32(c, RSS_CTRL_8125, 0);
        w16(c, Q_NUM_CTRL_8125, 0);
        mac_ocp_modify(c, 0xEB58, 0x0001, 0);   // the old descriptor format
    } else {
        w8(c, MaxTxPacketSize, c.evl_up ? 0x27 : 0x3F);   // on the 8169: EarlyTxThres, no early transmit
        w16(c, IntrMitigate, 0);
        if (chip->ver <= 6) {                   // rtl_hw_start_8169
            w16(c, CPlusCmd, r16(c, CPlusCmd) | 0x0008 | (chip->ver <= 3 ? 0x4000 : 0));   // PCIMulRW, EnAnaPLL
            if (chip->ver >= 5)                 // rtl8169_set_magic_reg
                w32(c, 0x7C, (chip->ver == 5 ? 0x000FFF00u : 0x00FFFF00u) | ((r8(c, 0x53) & 1) ? 0xFF : 0));
        }
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
    if (step('r')) {                            // PHY soft reset, negotiate afresh (genphy_soft_reset)
        int b = phy_read(c, 0);
        phy_write(c, 0, (uint16_t)((b & ~0x0800) | 0x8000 | ((b & 0x1000) ? 0x0200 : 0)));
    }
    if (debug_level >= 1) {
        char sv[8] = { 0 };
        for (int i = 0; i < 7 && steps[i] && steps[i] != ' '; i++) sv[i] = steps[i];
        kprintf("net: %s PHY steps \"%s\"\n", c.name, sv);
    }
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

// ---------------- Broadcom Tigon3: BCM570x, 575x, 578x, 5906, 5717-5762 (Linux: tg3) ----------------
// What Linux's tg3 does to start one (tg3_get_invariants, tg3_chip_reset,
// tg3_reset_hw), for one transmit and one receive ring, polled, without
// TSO or jumbo frames, so no firmware download. The PHY is left as the
// boot code set it up, as for the Realtek cards: it is only woken if
// powered down, and the MAC is set to the speed and duplex it reports.
// Copper and MII-attached SerDes (5714S/5715S/5780S, 5717+ fiber); the
// 5700-5704's TBI fiber ports aren't driven. The APE (management
// processor of the 5717/5719/5720/5761/5762 family) is told a driver runs
// and gets its heartbeat; the PHY and the reset are taken under its locks.
namespace tg3 {
enum : uint32_t {
    // PCI configuration space (mirrored at the start of BAR 0)
    MISC_HOST_CTRL = 0x68, DMA_RW_CTRL = 0x6C, PCISTATE = 0x70, CLOCK_CTRL = 0x74,
    REG_BASE_ADDR = 0x78, MEM_WIN_BASE_ADDR = 0x7C, REG_DATA = 0x80, MEM_WIN_DATA = 0x84,
    MSI_DATA = 0x64, DEV_STATUS_CTRL = 0xB4, DUAL_MAC_CTRL = 0xB8,
    PRODID_ASICREV = 0xBC, GEN2_PRODID_ASICREV = 0xF4, GEN15_PRODID_ASICREV = 0xFC,
    // mailboxes
    MB_INT0 = 0x204, MB_RCVRET_CON0 = 0x284, MB_RCV_STD_PROD = 0x26C, MB_RCV_JUMBO_PROD = 0x274,
    MB_SNDHOST_PROD0 = 0x304, MB_SNDNIC_PROD0 = 0x384, GRCMBOX_BASE = 0x5600,
    // MAC
    MAC_MODE = 0x400, MAC_STATUS = 0x404, MAC_EVENT = 0x408, MAC_LED_CTRL = 0x40C, MAC_ADDR0_HI = 0x410,
    MAC_TX_BACKOFF = 0x438, MAC_RX_MTU = 0x43C, MAC_MI_COM = 0x44C, MAC_MI_STAT = 0x450, MAC_MI_MODE = 0x454,
    MAC_TX_MODE = 0x45C, MAC_TX_LENGTHS = 0x464, MAC_RX_MODE = 0x468, MAC_HASH0 = 0x470, MAC_RCV_RULE0 = 0x480,
    MAC_RCV_RULE_CFG = 0x500, MAC_LOW_WMARK = 0x504, MAC_EXTADDR0_HI = 0x530, SG_DIG_STATUS = 0x5B4,
    SNDDATAI_MODE = 0xC00, SNDDATAI_STATSCTRL = 0xC08, SNDDATAI_STATSENAB = 0xC0C, ISO_PKT_TX = 0xC20,
    SNDDATAC_MODE = 0x1000, SNDBDS_MODE = 0x1400, SNDBDI_MODE = 0x1800, SNDBDC_MODE = 0x1C00,
    RCVLPC_MODE = 0x2000, RCVLPC_CONFIG = 0x2010, RCVLPC_STATSCTRL = 0x2014, RCVLPC_STATS_ENABLE = 0x2018,
    RCVDBDI_MODE = 0x2400, RCVDBDI_JUMBO_BD = 0x2440, RCVDBDI_STD_BD = 0x2450, RCVDBDI_MINI_BD = 0x2460,
    RCVDCC_MODE = 0x2800, RCVBDI_MODE = 0x2C00, RCVBDI_STD_THRESH = 0x2C18, STD_REPLENISH_LWM = 0x2D00,
    RCVCC_MODE = 0x3000, RCVLSC_MODE = 0x3400, MBFREE_MODE = 0x3800,
    HOSTCC_MODE = 0x3C00, HOSTCC_RXCOL_TICKS = 0x3C08, HOSTCC_TXCOL_TICKS = 0x3C0C, HOSTCC_RXMAX_FRAMES = 0x3C10,
    HOSTCC_TXMAX_FRAMES = 0x3C14, HOSTCC_RXCOAL_TICK_INT = 0x3C18, HOSTCC_TXCOAL_TICK_INT = 0x3C1C,
    HOSTCC_RXCOAL_MAXF_INT = 0x3C20, HOSTCC_TXCOAL_MAXF_INT = 0x3C24, HOSTCC_STAT_COAL_TICKS = 0x3C28,
    HOSTCC_STATS_BLK_HOST = 0x3C30, HOSTCC_STATUS_BLK_HOST = 0x3C38, HOSTCC_STATS_BLK_NIC = 0x3C40,
    HOSTCC_STATUS_BLK_NIC = 0x3C44,
    MEMARB_MODE = 0x4000, BUFMGR_MODE = 0x4400, BUFMGR_MB_POOL_ADDR = 0x4408, BUFMGR_MB_POOL_SIZE = 0x440C,
    BUFMGR_MB_RDMA_LOW = 0x4410, BUFMGR_MB_MACRX_LOW = 0x4414, BUFMGR_MB_HIGH = 0x4418,
    BUFMGR_DMA_DESC_POOL_ADDR = 0x442C, BUFMGR_DMA_DESC_POOL_SIZE = 0x4430, BUFMGR_DMA_LOW = 0x4434,
    BUFMGR_DMA_HIGH = 0x4438,
    RDMAC_MODE = 0x4800, RDMA_RSRVCTRL2 = 0x4890, LSO_RD_DMA_CRPTEN2 = 0x48A0, RDMA_RSRVCTRL = 0x4900,
    LSO_RD_DMA_CRPTEN = 0x4910, RDMA_LENGTH = 0x4BE0, WDMAC_MODE = 0x4C00,
    VCPU_STATUS = 0x5100, CPMU_LSPD_10MB_CLK = 0x3604, CPMU_CLCK_ORIDE = 0x3624, CPMU_CLCK_ORIDE_ENABLE = 0x3628,
    CPMU_STATUS = 0x362C, CPMU_PHY_STRAP = 0x3664, CPMU_PADRNG_CTL = 0x3668,
    DMAC_MODE = 0x6400, GRC_MODE = 0x6800, GRC_MISC_CFG = 0x6804, GRC_LOCAL_CTRL = 0x6808,
    GRC_RX_CPU_EVENT = 0x6810, GRC_VCPU_EXT_CTRL = 0x6890, GRC_FASTBOOT_PC = 0x6894,
    NVRAM_SWARB = 0x7020, PCIE_TLDLPL_PORT = 0x7C00, PCIE_TRANSACTION_CFG = 0x7C04, PCIE_LNKCTL = 0x7D54,
    PCIE_PHY_TSTCTL = 0x7E2C,
    // NIC SRAM (through the memory window)
    SRAM_SEND_RCB = 0x100, SRAM_RCV_RET_RCB = 0x200, SRAM_STATS_BLK = 0x300, SRAM_STATUS_BLK = 0xB00,
    SRAM_FW_MBOX = 0xB50, SRAM_DATA_SIG = 0xB54, SRAM_DATA_CFG = 0xB58, SRAM_DATA_VER = 0xB5C,
    SRAM_FW_CMD_MBOX = 0xB78, SRAM_DRV_STATE_MBOX = 0xC04, SRAM_MAC_HI_MBOX = 0xC14, SRAM_MAC_LO_MBOX = 0xC18,
    SRAM_DATA_CFG_2 = 0xD38, SRAM_CPMU_STATUS = 0xE00,
    SRAM_TX_BUFFER_DESC = 0x4000, SRAM_RX_BUFFER_DESC = 0x6000, SRAM_MBUF_POOL = 0x8000,
    SRAM_DMA_DESC_POOL = 0x2000,
    FW_MAGIC = 0x4B657654,
    // APE (BAR 2)
    APE_EVENT = 0x000C, APE_LOCK_REQ = 0x002C, APE_LOCK_GRANT = 0x004C, APE_SEG_SIG = 0x4000,
    APE_FW_STATUS = 0x400C, APE_HOST_SEG_SIG = 0x4200, APE_HOST_SEG_LEN = 0x4204, APE_HOST_INIT_COUNT = 0x4208,
    APE_HOST_DRIVER_ID = 0x420C, APE_HOST_BEHAVIOR = 0x4210, APE_HOST_HB_INT = 0x4214,
    APE_HOST_HB_COUNT = 0x4218, APE_HOST_DRVR_STATE = 0x421C, APE_EVENT_STATUS = 0x4300,
    APE_PER_LOCK_REQ = 0x8400, APE_PER_LOCK_GRANT = 0x8420,
    APE_LOCK_PHY0 = 0, APE_LOCK_GRC = 1, APE_LOCK_MEM = 4, APE_LOCK_GPIO = 7, APE_LOCK_DRIVER = 0x1000,
};
// chip_rev >> 12
enum : uint32_t {
    A5701 = 0x00, A5703 = 0x01, A5704 = 0x02, A5705 = 0x03, A5750 = 0x04, A5752 = 0x06, A5700 = 0x07,
    A5780 = 0x08, A5714 = 0x09, A5755 = 0x0A, A5787 = 0x0B, A5906 = 0x0C, A_PRODID = 0x0F,
    A5717 = 0x5717, A5719 = 0x5719, A5720 = 0x5720, A5761 = 0x5761, A5762 = 0x5762, A5784 = 0x5784,
    A5785 = 0x5785, A57765 = 0x57785, A57766 = 0x57766, A57780 = 0x57780,
};
enum : uint32_t {                               // Card::tflags
    F5705P = 1 << 0, F5750P = 1 << 1, F5755P = 1 << 2, F5780C = 1 << 3, F57765P = 1 << 4, F57765C = 1 << 5,
    F5717P = 1 << 6, FPCIE = 1 << 7, FPCIX = 1 << 8, FCPMU = 1 << 9, FAPE = 1 << 10, FASF = 1 << 11,
    FASFNEW = 1 << 12, FLRG = 1 << 13, FJUMBO = 1 << 14, FFET = 1 << 15, FMIISER = 1 << 16, FTBI = 1 << 17,
    FFLUSH = 1 << 18, FINDWR = 1 << 19, FSRAMCFG = 1 << 20, FTXDBUG = 1 << 21, FNVRAM = 1 << 22,
    FPCIHI = 1 << 23, FUSBPHY = 1 << 24,
};
const int NRX = 64, NTXB = 16, BUF = 2048, RX_DMA = 1536, TX_RING = 512;
struct BD { uint32_t addr_hi, addr_lo, idx_len, type_flags, csum, err_vlan, std_opaque, opaque; };   // receive
struct TD { uint32_t addr_hi, addr_lo, len_flags, vlan; };
struct Status { uint32_t status, tag; uint16_t rx_jumbo_cons, rx_cons, rx_mini_cons, res;
                struct { uint16_t rx_prod, tx_cons; } idx[16]; };

const uint16_t bcm_ids[] = {
    0x1644, 0x1645, 0x1646, 0x1647, 0x1648, 0x164D, 0x1653, 0x1654, 0x165D, 0x165E, 0x16A6, 0x16A7,
    0x16A8, 0x16C6, 0x16C7, 0x1696, 0x169C, 0x169D, 0x170D, 0x170E, 0x1649, 0x166E, 0x1659, 0x165A,
    0x1676, 0x1677, 0x167D, 0x167E, 0x1600, 0x1601, 0x16F7, 0x16FD, 0x16FE, 0x167A, 0x1672, 0x167B,
    0x1673, 0x1674, 0x169A, 0x169B, 0x1693, 0x167F, 0x1668, 0x1669, 0x1678, 0x1679, 0x166A, 0x166B,
    0x16DD, 0x1712, 0x1713, 0x1698, 0x1684, 0x165B, 0x1681, 0x1680, 0x1688, 0x1689, 0x1699, 0x16A0,
    0x1692, 0x1690, 0x1694, 0x1691, 0x1655, 0x1665, 0x1656, 0x16B1, 0x16B5, 0x16B0, 0x16B4, 0x16B2,
    0x16B6, 0x1657, 0x165F, 0x1682, 0x1686, 0x1687, 0x1643, 0x16F3, 0x1642, 0x1683, 0x1641, 0x16B7,
    0x16B3,
};
const uint32_t other_ids[] = {                  // device << 16 | vendor
    0x44001148, 0x45001148,                     // SysKonnect SK-9Dxx, SK-9Mxx
    0x03E8173B, 0x03E9173B, 0x03EA173B, 0x03EB173B,   // Altima AC1000, AC1001, AC9100, AC1003
    0x1645106B,                                 // Apple
    0x11A210CF,                                 // Fujitsu (BCM5703)
};

bool match(uint32_t id)
{
    if ((id & 0xFFFF) == 0x14E4) { for (uint16_t d : bcm_ids) if (d == id >> 16) return true; return false; }
    for (uint32_t o : other_ids) if (o == id) return true;
    return false;
}

uint32_t asic(Card& c) { return c.chiprev >> 12; }
bool has(Card& c, uint32_t f) { return (c.tflags & f) != 0; }

uint32_t tr(Card& c, uint32_t r) { return r32(c, r); }
void tw(Card& c, uint32_t r, uint32_t v)
{
    if (has(c, FINDWR)) { pci_write(c.pci, REG_BASE_ADDR, r); pci_write(c.pci, REG_DATA, v); return; }
    w32(c, r, v);
    if (has(c, FFLUSH)) r32(c, r);
}
void twf(Card& c, uint32_t r, uint32_t v) { tw(c, r, v); if (!has(c, FINDWR)) r32(c, r); }
void tw_wait(Card& c, uint32_t r, uint32_t v, uint32_t us) { twf(c, r, v); udelay(us); }

void mbox(Card& c, uint32_t r, uint32_t v)      // a mailbox register
{
    if (asic(c) == A5906) r += GRCMBOX_BASE;
    w32(c, r, v);
}
void tx_mbox(Card& c, uint32_t r, uint32_t v)
{
    mbox(c, r, v);
    if (has(c, FTXDBUG)) mbox(c, r, v);         // 5700 BX: written twice
    r32(c, asic(c) == A5906 ? r + GRCMBOX_BASE : r);   // posted: push it out
}

uint32_t mem_rd(Card& c, uint32_t off)          // NIC SRAM
{
    if (asic(c) == A5906 && off >= SRAM_STATS_BLK && off < SRAM_TX_BUFFER_DESC) return 0;
    uint32_t v;
    if (has(c, FSRAMCFG)) {
        pci_write(c.pci, MEM_WIN_BASE_ADDR, off); v = pci_read(c.pci, MEM_WIN_DATA);
        pci_write(c.pci, MEM_WIN_BASE_ADDR, 0);
    } else {
        twf(c, MEM_WIN_BASE_ADDR, off); v = tr(c, MEM_WIN_DATA); twf(c, MEM_WIN_BASE_ADDR, 0);
    }
    return v;
}
void mem_wr(Card& c, uint32_t off, uint32_t v)
{
    if (asic(c) == A5906 && off >= SRAM_STATS_BLK && off < SRAM_TX_BUFFER_DESC) return;
    if (has(c, FSRAMCFG)) {
        pci_write(c.pci, MEM_WIN_BASE_ADDR, off); pci_write(c.pci, MEM_WIN_DATA, v);
        pci_write(c.pci, MEM_WIN_BASE_ADDR, 0);
    } else {
        twf(c, MEM_WIN_BASE_ADDR, off); twf(c, MEM_WIN_DATA, v); twf(c, MEM_WIN_BASE_ADDR, 0);
    }
}

// ---- APE ----
uint32_t ape_rd(Card& c, uint32_t r) { return *(volatile uint32_t*)(c.ape + r); }
void ape_wr(Card& c, uint32_t r, uint32_t v) { *(volatile uint32_t*)(c.ape + r) = v; }
// The driver's bit in a lock: its own for the PHY locks (0, 2, 3, 5), its
// PCI function's for GRC, MEM and GPIO.
uint32_t ape_bit(Card& c, int lock)
{
    if (lock == 0 || lock == 2 || lock == 3 || lock == 5) return APE_LOCK_DRIVER;
    return c.pci_fn ? 1u << c.pci_fn : APE_LOCK_DRIVER;
}
bool ape_lock(Card& c, int lock)
{
    if (!has(c, FAPE)) return true;
    if (lock == APE_LOCK_GPIO && asic(c) == A5761) return true;
    bool old = asic(c) == A5761;
    uint32_t req = old ? APE_LOCK_REQ : APE_PER_LOCK_REQ, gnt = old ? APE_LOCK_GRANT : APE_PER_LOCK_GRANT;
    uint32_t bit = ape_bit(c, lock);
    ape_wr(c, req + 4 * lock, bit);
    for (int i = 0; i < 100; i++) { if (ape_rd(c, gnt + 4 * lock) == bit) return true; udelay(10); }
    ape_wr(c, gnt + 4 * lock, bit);             // give the request back
    return false;
}
void ape_unlock(Card& c, int lock)
{
    if (!has(c, FAPE)) return;
    if (lock == APE_LOCK_GPIO && asic(c) == A5761) return;
    ape_wr(c, (asic(c) == A5761 ? APE_LOCK_GRANT : APE_PER_LOCK_GRANT) + 4 * lock, ape_bit(c, lock));
}
void ape_lock_init(Card& c)
{
    uint32_t base = asic(c) == A5761 ? APE_LOCK_GRANT : APE_PER_LOCK_GRANT;
    for (int i = 0; i <= (int)APE_LOCK_GPIO; i++) ape_wr(c, base + 4 * i, ape_bit(c, i));
}
void ape_driver_start(Card& c)
{
    if (!has(c, FAPE)) return;
    ape_wr(c, APE_HOST_HB_COUNT, c.ape_hb++);
    ape_wr(c, APE_HOST_SEG_SIG, 0x484F5354);
    ape_wr(c, APE_HOST_SEG_LEN, 0x20);
    ape_wr(c, APE_HOST_INIT_COUNT, ape_rd(c, APE_HOST_INIT_COUNT) + 1);
    ape_wr(c, APE_HOST_DRIVER_ID, 0xF0000000u | 3 << 16 | 137 << 8);   // as Linux's tg3 3.137
    ape_wr(c, APE_HOST_BEHAVIOR, 1);            // NO_PHYLOCK
    ape_wr(c, APE_HOST_DRVR_STATE, 1);          // START
    if (ape_rd(c, APE_SEG_SIG) != 0x41504521 || !(ape_rd(c, APE_FW_STATUS) & 0x100)) return;
    for (int i = 0; i < 2000; i++) {            // the APE done with its last event
        if (!ape_lock(c, APE_LOCK_MEM)) { udelay(10); continue; }
        if (!(ape_rd(c, APE_EVENT_STATUS) & 0x80000000u)) {
            ape_wr(c, APE_EVENT_STATUS, 0x00010000u | 0x10 | 0x500 | 0x80000000u);   // START, driver, state change
            ape_unlock(c, APE_LOCK_MEM);
            ape_wr(c, APE_EVENT, 1);
            return;
        }
        ape_unlock(c, APE_LOCK_MEM);
        udelay(10);
    }
}

// ---- PHY ----
int phy_rd(Card& c, int reg)
{
    ape_lock(c, c.ape_phy_lock);
    twf(c, MAC_MI_COM, (uint32_t)c.phy_addr << 21 | (uint32_t)reg << 16 | 0x08000000u | 0x20000000u);
    int v = -1;
    for (int i = 0; i < 5000; i++) {
        udelay(10);
        uint32_t f = tr(c, MAC_MI_COM);
        if (!(f & 0x20000000u)) { udelay(5); v = tr(c, MAC_MI_COM) & 0xFFFF; break; }
    }
    ape_unlock(c, c.ape_phy_lock);
    return v;
}
void phy_wr(Card& c, int reg, uint16_t val)
{
    ape_lock(c, c.ape_phy_lock);
    twf(c, MAC_MI_COM, (uint32_t)c.phy_addr << 21 | (uint32_t)reg << 16 | val | 0x04000000u | 0x20000000u);
    for (int i = 0; i < 5000; i++) { udelay(10); if (!(tr(c, MAC_MI_COM) & 0x20000000u)) break; }
    ape_unlock(c, c.ape_phy_lock);
}

// Link, speed and duplex from the PHY's standard registers; the MAC's port
// mode follows them (GMII at 1000, MII at 10/100), as tg3_setup_copper_phy.
// speed: 0 down, 10, 100, 1000.
void link_update(Card& c)
{
    int speed = 0; bool full = true;
    phy_rd(c, 1);
    int bmsr = phy_rd(c, 1);                    // latched: the second read is now
    if (bmsr >= 0 && (bmsr & 4)) {
        int bmcr = phy_rd(c, 0);
        if (has(c, FMIISER)) speed = 1000;
        else if (bmcr >= 0 && !(bmcr & 0x1000)) {   // autonegotiation off: forced
            speed = (bmcr & 0x40) ? 1000 : (bmcr & 0x2000) ? 100 : 10;
            full = (bmcr & 0x100) != 0;
        } else {
            int g = 0;
            if (!has(c, FFET)) {
                int ctl = phy_rd(c, 9), stat = phy_rd(c, 10);
                if (ctl >= 0 && stat >= 0) g = ctl & (stat >> 2) & 0x300;
            }
            int both = phy_rd(c, 4) & phy_rd(c, 5);
            if (g) { speed = 1000; full = (g & 0x200) != 0; }
            else if (both & 0x100) speed = 100;
            else if (both & 0x080) { speed = 100; full = false; }
            else if (both & 0x040) speed = 10;
            else if (both & 0x020) { speed = 10; full = false; }
            else {                              // not resolved (yet): Broadcom's own summary
                int aux = phy_rd(c, 0x19);
                switch ((aux >> 8) & 7) {
                case 1: speed = 10; full = false; break;
                case 2: speed = 10; break;
                case 3: speed = 100; full = false; break;
                case 5: speed = 100; break;
                case 6: speed = 1000; full = false; break;
                case 7: speed = 1000; break;
                default: speed = 100;               // up, but unknown: 10/100 is MII either way
                }
            }
        }
    }
    if (speed == c.speed && full == c.full) return;
    c.speed = speed; c.full = full;
    uint32_t m = c.mac_mode & ~0x0Eu;           // port mode, half duplex
    if (has(c, FTBI)) m |= 0x0C;
    else if (has(c, FMIISER)) m |= 0x08;
    else if (speed == 10 || speed == 100 || (!speed && has(c, FFET))) m |= 0x04;   // MII
    else m |= 0x08;                             // GMII
    if (speed && !full) m |= 0x02;
    c.mac_mode = m;
    tw_wait(c, MAC_MODE, m, 40);
    if (asic(c) != A5700) tw_wait(c, MAC_EVENT, 0x1000, 40);   // LNKSTATE_CHANGED
    if (speed) dbg(1, "net: %s: %d Mbit/s, %s duplex\n", c.name, speed, full ? "full" : "half");
}

// ---- chip reset (tg3_chip_reset) ----
bool chip_reset(Card& c)
{
    bool nvlock = false;
    if (has(c, FNVRAM)) {
        tw(c, NVRAM_SWARB, 0x02);               // REQ_SET1
        for (int i = 0; i < 8000; i++) { if (tr(c, NVRAM_SWARB) & 0x200) { nvlock = true; break; } udelay(20); }
        if (!nvlock) tw(c, NVRAM_SWARB, 0x20);  // REQ_CLR1
    }
    ape_lock(c, APE_LOCK_GRC);
    uint32_t cmd = pci_read(c.pci, 0x04) & 0xFFFF;

    if (asic(c) == A5752 || has(c, F5755P)) tw(c, GRC_FASTBOOT_PC, 0);
    bool flush = has(c, FFLUSH);
    c.tflags &= ~FFLUSH;                        // no read-back of the reset write
    if (asic(c) == A57780) tw(c, PCIE_LNKCTL, (tr(c, PCIE_LNKCTL) & ~0x08u) | 0x80);

    uint32_t val = 1;                           // CORECLK_RESET
    if (has(c, FPCIE)) {
        if (asic(c) != A5785 && !has(c, F57765P) && tr(c, PCIE_PHY_TSTCTL) == 0x60) tw(c, PCIE_PHY_TSTCTL, 0x20);
        if (c.chiprev != 0x4000) { tw(c, GRC_MISC_CFG, 1u << 29); val |= 1u << 29; }
    }
    if (asic(c) == A5906) {
        tw(c, VCPU_STATUS, tr(c, VCPU_STATUS) | 0x08000000u);            // DRV_RESET
        tw(c, GRC_VCPU_EXT_CTRL, tr(c, GRC_VCPU_EXT_CTRL) & ~0x00400000u);   // HALT_CPU
    }
    if (asic(c) == A5717) tw(c, CPMU_CLCK_ORIDE_ENABLE, tr(c, CPMU_CLCK_ORIDE_ENABLE) | (1u << 13));
    else if (asic(c) == A5719 || asic(c) == A5720) tw(c, CPMU_CLCK_ORIDE, 0x80000000u);
    if (has(c, F5705P) && !has(c, FCPMU)) val |= 0x04000000u;            // KEEP_GPHY_POWER
    tw(c, GRC_MISC_CFG, val);
    if (flush) c.tflags |= FFLUSH;
    udelay(120);
    pci_read(c.pci, 0x04);                      // the only read that works now
    udelay(120);

    if (has(c, FPCIE) && c.pcie_cap) {
        if (c.chiprev == 0x4000) {              // 5750 A0: link training
            udelay(500000);
            pci_write(c.pci, 0xC4, pci_read(c.pci, 0xC4) | (1u << 15));
        }
        uint32_t dc = pci_read(c.pci, c.pcie_cap + 8);   // device control / status
        uint16_t clear = 0x0010 | 0x0800;       // relaxed ordering, no snoop
        if (!has(c, FCPMU)) clear |= 0x00E0;    // max payload: 128
        dc = (dc & 0xFFFF & ~(uint32_t)clear) | 0x000F0000u;   // and clear the error status
        pci_write(c.pci, c.pcie_cap + 8, dc);
    }
    // tg3_restore_pci_state
    pci_write(c.pci, MISC_HOST_CTRL, c.misc_host);
    uint32_t ps = 0x20 | 0x40;                  // ROM_ENABLE, ROM_RETRY_ENABLE
    if (c.chiprev == 0x2000 && has(c, FPCIX)) ps |= 0x2000;   // RETRY_SAME_DMA
    if (has(c, FAPE)) ps |= 0x70000;
    pci_write(c.pci, PCISTATE, ps);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) & 0xFFFF0000u) | cmd);
    if (!has(c, FPCIE)) pci_write(c.pci, 0x0C, (pci_read(c.pci, 0x0C) & 0xFFFF0000u) | c.cls_lat);
    if (has(c, FPCIX) && c.pcix_cap) pci_write(c.pci, c.pcix_cap, pci_read(c.pci, c.pcix_cap) & ~(0x0002u << 16));   // ERO

    tw(c, MEMARB_MODE, (has(c, F5780C) ? tr(c, MEMARB_MODE) : 0) | 2);
    if (c.chiprev == 0x4003) tw(c, 0x5000, 0x400);   // 5750 A3

    // the boot code done (tg3_poll_fw)
    if (asic(c) == A5906) {
        int i = 0;
        for (; i < 200 && !(tr(c, VCPU_STATUS) & 0x04000000u); i++) udelay(100);
        if (i == 200) { kprintf("net: %s: no boot code (VCPU)\n", c.name); return false; }
    } else {
        int i = 0;
        for (; i < 100000 && mem_rd(c, SRAM_FW_MBOX) != ~FW_MAGIC; i++) udelay(10);
        if (i == 100000) kprintf("net: %s: no boot code answered (running anyway)\n", c.name);
        if (c.chiprev == 0x57785000) udelay(10000);
    }

    tw(c, GRC_MODE, c.grc_mode);
    if (c.chiprev == 0x3000) tw(c, 0xC4, tr(c, 0xC4) | (1u << 15));
    c.mac_mode = has(c, FTBI) ? 0x0Cu : has(c, FMIISER) ? 0x08u : 0;
    tw_wait(c, MAC_MODE, c.mac_mode, 40);
    ape_unlock(c, APE_LOCK_GRC);
    tw_wait(c, MAC_MI_MODE, c.mi_mode, 80);     // MDIO, no auto-polling
    if (has(c, FPCIE) && c.chiprev != 0x4000 && asic(c) != A5785 && !has(c, F57765P))
        tw(c, 0x7C00, tr(c, 0x7C00) | (1u << 25));
    if (asic(c) == A5717) tw(c, CPMU_CLCK_ORIDE_ENABLE, tr(c, CPMU_CLCK_ORIDE_ENABLE) & ~(1u << 13));
    else if (asic(c) == A5719 || asic(c) == A5720) tw(c, CPMU_CLCK_ORIDE, tr(c, CPMU_CLCK_ORIDE) & ~0x80000000u);
    if (asic(c) == A5762) tw(c, CPMU_CLCK_ORIDE_ENABLE, tr(c, CPMU_CLCK_ORIDE_ENABLE) | (1u << 13));

    c.tflags &= ~(FASF | FASFNEW);              // ASF as the boot code says now
    if (mem_rd(c, SRAM_DATA_SIG) == FW_MAGIC && (mem_rd(c, SRAM_DATA_CFG) & 0x80)) {
        c.tflags |= FASF;
        if (has(c, F5750P)) c.tflags |= FASFNEW;
    }
    return true;
}

void set_mac(Card& c, const uint8_t* a)
{
    uint32_t hi = a[0] << 8 | a[1], lo = a[2] << 24 | a[3] << 16 | a[4] << 8 | a[5];
    for (int i = 0; i < 4; i++) { tw(c, MAC_ADDR0_HI + 8 * i, hi); tw(c, MAC_ADDR0_HI + 4 + 8 * i, lo); }
    if (asic(c) == A5703 || asic(c) == A5704)
        for (int i = 0; i < 12; i++) { tw(c, MAC_EXTADDR0_HI + 8 * i, hi); tw(c, MAC_EXTADDR0_HI + 4 + 8 * i, lo); }
    tw(c, MAC_TX_BACKOFF, (a[0] + a[1] + a[2] + a[3] + a[4] + a[5]) & 0x3FF);
}

void set_mode(Card& c, int m)
{
    uint32_t rx = c.rx_mode_reg & ~0x100u;      // PROMISC
    for (int i = 0; i < 4; i++) tw(c, MAC_HASH0 + 4 * i, m >= 4 ? 0xFFFFFFFFu : 0);
    if (m >= 6) rx |= 0x100;
    if (m >= 2) rx |= 0x02; else rx &= ~0x02u;  // ENABLE
    c.rx_mode_reg = rx;
    tw_wait(c, MAC_RX_MODE, rx, 10);
}

void bdinfo(Card& c, uint32_t at, void* ring, uint32_t maxlen_flags, uint32_t nic)
{
    mem_wr(c, at + 0, 0);
    mem_wr(c, at + 4, (uint32_t)(uintptr_t)ring);
    mem_wr(c, at + 8, maxlen_flags);
    if (!has(c, F5705P)) mem_wr(c, at + 12, nic);
}

int pci_cap(const PciDevice& d, int id)
{
    if (!(pci_read(d, 0x04) & (1u << 20))) return 0;
    int cap = pci_read(d, 0x34) & 0xFC;
    for (int n = 0; cap && n < 48; n++) {
        uint32_t h = pci_read(d, cap);
        if ((h & 0xFF) == (uint32_t)id) return cap;
        cap = (h >> 8) & 0xFC;
    }
    return 0;
}

volatile uint8_t* map_bar_n(const PciDevice& d, int b, uint32_t len)
{
    uint32_t bar = pci_read(d, 0x10 + 4 * b);
    if (bar & 1) return 0;
    uint64_t base = bar & ~0xFull;
    if ((bar & 6) == 4) base |= (uint64_t)pci_read(d, 0x14 + 4 * b) << 32;
    return base ? (volatile uint8_t*)map_mmio64(base, len) : 0;
}

bool init(Card& c)
{
    pci_d0(c.pci);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x06) & ~0x410u);   // memory, bus master; no MWI, no INTx
    c.mm = map_bar_n(c.pci, 0, 0x10000);
    if (!c.mm) return false;

    // ---- what chip (tg3_detect_asic_rev) ----
    uint32_t misc = pci_read(c.pci, MISC_HOST_CTRL);
    c.chiprev = misc >> 16;
    c.tflags = 0;
    if (asic(c) == A_PRODID) {
        c.tflags |= FCPMU;
        uint16_t d = c.device;
        int reg = PRODID_ASICREV;
        if (d == 0x1655 || d == 0x1665 || d == 0x1656 || d == 0x1657 || d == 0x165F || d == 0x1683 ||
            d == 0x1642 || d == 0x1687 || d == 0x1643 || d == 0x16F3 || d == 0x1641) reg = GEN2_PRODID_ASICREV;
        else if (d == 0x16B1 || d == 0x16B5 || d == 0x16B0 || d == 0x16B4 || d == 0x16B2 || d == 0x16B6 ||
                 d == 0x1682 || d == 0x1686 || d == 0x16B7 || d == 0x16B3) reg = GEN15_PRODID_ASICREV;
        c.chiprev = pci_read(c.pci, reg);
    }
    if (c.chiprev == 0x5000) c.chiprev = 0x6000;            // 5752 A0
    if (c.chiprev == 0x05717200) c.chiprev = 0x05720000;    // 5717 C0 is a 5720 A0
    uint32_t a = asic(c);
    if (a == A5717 || a == A5719 || a == A5720) c.tflags |= F5717P;
    if (a == A57765 || a == A57766) c.tflags |= F57765C;
    if (has(c, F57765C) || has(c, F5717P) || a == A5762) c.tflags |= F57765P;
    if (a == A5755 || a == A5787 || a == A5784 || a == A5761 || a == A5785 || a == A57780 || has(c, F57765P))
        c.tflags |= F5755P;
    if (a == A5780 || a == A5714) c.tflags |= F5780C;
    if (a == A5750 || a == A5752 || a == A5906 || has(c, F5755P) || has(c, F5780C)) c.tflags |= F5750P;
    if (a == A5705 || has(c, F5750P)) c.tflags |= F5705P;
    if (a == A5717 || a == A5719 || a == A5720 || a == A5762) c.tflags |= FLRG;
    bool jumbo_bdflag = has(c, F57765P) && c.chiprev != 0x05719000;
    if (!has(c, F5705P) || has(c, F5780C) || jumbo_bdflag) c.tflags |= FJUMBO;
    if (a == A5906) c.tflags |= FFET;
    if (a != A5700 && a != A5701) c.tflags |= FNVRAM;      // (those two have a serial EEPROM)

    static const char* const names[] = { "5701", "5703", "5704", "5705", "5750", "?", "5752", "5700",
                                         "5780", "5714", "5755", "5787", "5906" };
    const char* nm = a < 13 ? names[a] : a == A57765 ? "57765" : a == A57766 ? "57766" : a == A57780 ? "57780" : 0;
    if (nm) snprintf(c.name, sizeof c.name, "BCM%s", nm);
    else snprintf(c.name, sizeof c.name, "BCM%x", a);

    c.misc_host = (misc & 0xFFFF0000u) | 0x02 | 0x08 | 0x80 | 0x10;   // MASK_PCI_INT, WORD_SWAP, INDIR_ACCESS, PCISTATE_RW
    pci_write(c.pci, MISC_HOST_CTRL, c.misc_host);

    // ---- the bus ----
    c.pcie_cap = (uint8_t)pci_cap(c.pci, 0x10);
    c.pcix_cap = 0;
    uint32_t ps = pci_read(c.pci, PCISTATE);
    if (c.pcie_cap) c.tflags |= FPCIE;
    else if (a == A5785) c.tflags |= FPCIE;
    else if (!has(c, F5705P) || has(c, F5780C)) {
        c.pcix_cap = (uint8_t)pci_cap(c.pci, 0x07);
        if (c.pcix_cap && !(ps & 0x04)) c.tflags |= FPCIX;          // not CONV_PCI_MODE
    }
    if (ps & 0x80) c.tflags |= FPCIHI;          // BUS_SPEED_HIGH
    c.cls_lat = (uint16_t)pci_read(c.pci, 0x0C);
    if (a == A5703 && (c.cls_lat >> 8) < 64) {
        c.cls_lat = (c.cls_lat & 0xFF) | 64 << 8;
        pci_write(c.pci, 0x0C, (pci_read(c.pci, 0x0C) & 0xFFFF0000u) | c.cls_lat);
    }
    if ((c.chiprev >> 8) == 0x71) {             // 5700 BX
        c.tflags |= FTXDBUG;
        if (has(c, FPCIX)) c.tflags |= FINDWR;
    }
    if (c.chiprev == 0x2000 && !(ps & 0x2000)) pci_write(c.pci, PCISTATE, ps | 0x2000);
    if (!has(c, FINDWR) && (a == A5701 || (has(c, FPCIE) && c.chiprev == 0x4000))) c.tflags |= FFLUSH;
    if (has(c, FINDWR) || (has(c, FPCIX) && (a == A5700 || a == A5701))) c.tflags |= FSRAMCFG;

    tw(c, MEMARB_MODE, tr(c, MEMARB_MODE) | 2);

    c.pci_fn = c.pci.fn & 3;
    if (a == A5704 || has(c, F5780C)) {
        if (has(c, FPCIX) && c.pcix_cap) c.pci_fn = pci_read(c.pci, c.pcix_cap + 4) & 7;
    } else if (has(c, F5717P)) {
        uint32_t v = mem_rd(c, SRAM_CPMU_STATUS);
        if ((v & 0xFFFF) != 0x362C) v = tr(c, CPMU_STATUS);
        c.pci_fn = a == A5717 ? ((v & 0x20000000u) ? 1 : 0) : (v & 0xC0000000u) >> 30;
    }

    // ---- what the boot code says (tg3_get_eeprom_hw_cfg) ----
    c.led = 0x800;                              // LED_CTRL_MODE_PHY_1
    if (a != A5906 && mem_rd(c, SRAM_DATA_SIG) == FW_MAGIC) {
        uint32_t cfg = mem_rd(c, SRAM_DATA_CFG), ver = mem_rd(c, SRAM_DATA_VER) >> 16, cfg2 = 0;
        if (a != A5700 && a != A5701 && a != A5703 && ver > 0 && ver < 0x100) cfg2 = mem_rd(c, SRAM_DATA_CFG_2);
        if ((cfg & 0x30) == 0x20) c.tflags |= has(c, F5705P) ? FMIISER : FTBI;   // fiber
        uint32_t led = has(c, F5750P) ? cfg2 & (0x0C | 0x18000) : cfg & 0x0C;
        switch (led) {
        case 0x08: c.led = 0x1000; break;       // PHY_2
        case 0x00: c.led = (a == A5700 || a == A5701) ? 0x800 : 0; break;   // MAC
        case 0x8000:                            // shared
            c.led = 0x4000;
            if (c.chiprev != 0x4000 && c.chiprev != 0x4001) c.led |= 0x1800;
            if (has(c, F5717P) || a == A5762) c.led |= 0x80000000u | 0x7FF80000u;
            break;
        case 0x10000: c.led = 0x2000; break;    // Shasta MAC
        case 0x18000: c.led = 0x8000 | (c.chiprev != 0x4000 ? 0x1800 : 0); break;   // combo
        default: c.led = 0x800;
        }
        if ((c.chiprev >> 8) == 0x57840) c.led = 0x800;
        if (cfg & 0x80) { c.tflags |= FASF; if (has(c, F5750P)) c.tflags |= FASFNEW; }
        if ((cfg & 0x00200000u) && has(c, F5750P)) c.tflags |= FAPE;
    }
    if (has(c, FAPE)) {
        c.ape = map_bar_n(c.pci, 2, 0x10000);
        if (!c.ape) c.tflags &= ~FAPE;
        else {
            pci_write(c.pci, PCISTATE, pci_read(c.pci, PCISTATE) | 0x70000);
            ape_lock_init(c);
        }
    }
    if (has(c, FTBI)) {
        kprintf("net: %s: a fiber (TBI) port, which vmdos doesn't drive\n", c.name);
        return false;
    }
    if (c.chiprev == 0x0000) kprintf("net: %s: a 5701 A0, which wants a firmware fix vmdos doesn't have\n", c.name);
    if (a == A5785 || a == A57780) dbg(1, "net: %s: external PHY, left as the boot code set it\n", c.name);

    c.mi_mode = has(c, FCPMU) ? 0x8000u : 0xC0000u;
    c.coalesce = ((c.chiprev >> 8) == 0x70 || (c.chiprev >> 8) == 0x71) ? 0 : 0x100;   // 32BYTE
    c.grc_mode = 0x20 | 0x10 | 0x04;           // WSWAP_DATA, BSWAP_DATA, WSWAP_NONFRM_DATA
    if (a == A5717 || a == A5762 || c.chiprev == 0x05719000 || c.chiprev == 0x05720000) {
        c.coalesce |= 0x04;                     // ATTN
        c.grc_mode |= 0x10000000u;              // IRQ_ON_FLOW_ATTN
    }
    c.ape_phy_lock = APE_LOCK_PHY0;
    if (has(c, F5717P)) {
        c.phy_addr = c.pci_fn + 1;
        bool ser = c.chiprev != 0x05717000 ? (tr(c, SG_DIG_STATUS) & 0x100) != 0 : (tr(c, CPMU_PHY_STRAP) & 0x20) != 0;
        if (ser) { c.phy_addr += 7; c.tflags |= FMIISER; }
    } else c.phy_addr = 1;
    static const uint8_t phy_locks[4] = { 0, 2, 3, 5 };     // PHY0..PHY3
    if (has(c, FAPE)) c.ape_phy_lock = phy_locks[c.pci_fn & 3];
    tw_wait(c, MAC_MI_MODE, c.mi_mode, 80);

    uint32_t g = tr(c, GRC_MODE);
    g &= (a == A5720 || a == A5762) ? (0x40u | 0x80u | 0x8000u | 0x40000u | 0x10000u) : 0x10000u;
    tw(c, GRC_MODE, g | c.grc_mode);

    // tg3_switch_clocks
    c.clock_ctrl = 0;
    if (!has(c, FCPMU) && !has(c, F5780C)) {
        uint32_t cc = tr(c, CLOCK_CTRL), keep = cc & (0x200000u | 0x400000u | 0x1F);
        c.clock_ctrl = keep;
        if (has(c, F5705P)) { if (cc & 0x100000u) tw_wait(c, CLOCK_CTRL, keep | 0x100000u, 40); }
        else if (cc & 0x40000u) {
            tw_wait(c, CLOCK_CTRL, keep | 0x40000u | 0x1000u, 40);
            tw_wait(c, CLOCK_CTRL, keep | 0x1000u, 40);
        }
        tw_wait(c, CLOCK_CTRL, keep, 40);
    }
    tw(c, MEM_WIN_BASE_ADDR, 0);
    tw(c, REG_BASE_ADDR, 0);
    if (!(pci_read(c.pci, PCISTATE) & 0x04) && !has(c, FINDWR) &&
        (c.chiprev == 0x0000 || c.chiprev == 0x0100 || c.chiprev == 0x0102 || c.chiprev == 0x0105)) {
        volatile uint32_t* s = (volatile uint32_t*)(c.mm + 0x8000 + SRAM_STATS_BLK);   // the 5701's PCI-X target bug
        s[0] = 0; s[1] = 0; s[1] = 0xFFFFFFFFu;
        if (s[0] != 0) c.tflags |= FINDWR | FSRAMCFG;
    }

    // DMA settings (tg3_test_dma's choice, without the test)
    uint32_t dma = 7u << 28 | 6u << 24;         // PCI write / read commands
    if (!has(c, F57765P)) {
        if (has(c, FPCIE)) dma |= 0x00180000;
        else if (!has(c, FPCIX)) dma |= (a == A5705 || a == A5750) ? 0x003F0000 : 0x003F000F;
        else if (a == A5703 || a == A5704) {
            uint32_t cc = tr(c, CLOCK_CTRL) & 0x1F;
            if (cc == 6 || cc == 7) dma |= 0x4000;   // ONE_DMA
            dma |= (a == A5703 ? 4u : 7u) << 19 | 3u << 16 | 1u << 23;
        } else if (a == A5780) dma |= 0x00144000;
        else if (a == A5714) dma |= 0x00148000;
        else dma |= 0x001B000F;
        if (a == A5703 || a == A5704) dma &= 0xFFFFFFF0u;
        if (a == A5700 || a == A5701) dma |= 0x00400000 | 0x00800000;   // USE_MEM_READ_MULT, ASSERT_ALL_BE
    }
    c.dma_rwctrl = dma;
    tw(c, DMA_RW_CTRL, dma);

    // ---- rings ----
    c.std_size = has(c, FLRG) ? 2048 : 512;
    c.ret_size = has(c, FLRG) ? 4096 : (has(c, FJUMBO) && !has(c, F5780C)) ? 1024 : 512;
    c.rxd = (uint8_t*)phys_alloc(c.std_size * sizeof(BD));
    uint8_t* ret = (uint8_t*)phys_alloc(c.ret_size * sizeof(BD));
    c.txd = (uint8_t*)phys_alloc(TX_RING * sizeof(TD));
    c.rxb = (uint8_t*)phys_alloc(NRX * BUF);
    c.txb = (uint8_t*)phys_alloc(NTXB * BUF);
    c.stblk = (uint8_t*)phys_alloc(4096);       // status block, then (5700-5704) the statistics block
    c.retd = ret;
    volatile BD* std = (volatile BD*)c.rxd;
    for (int i = 0; i < NRX; i++) {
        std[i].addr_hi = 0;
        std[i].addr_lo = (uint32_t)(uintptr_t)(c.rxb + i * BUF);
        std[i].idx_len = RX_DMA;
        std[i].type_flags = 0x0004;             // END
        std[i].opaque = 0x00010000u | i;        // standard ring, buffer i
    }

    // ---- start it (tg3_reset_hw) ----
    mbox(c, MB_INT0, 1);                        // interrupts off
    if (has(c, FASF) && !has(c, FAPE)) {        // tg3_stop_fw: the ASF firmware paused
        for (int i = 0; i < 250 && (tr(c, GRC_RX_CPU_EVENT) & 0x4000); i++) udelay(10);
        mem_wr(c, SRAM_FW_CMD_MBOX, 2);         // PAUSE_FW
        twf(c, GRC_RX_CPU_EVENT, tr(c, GRC_RX_CPU_EVENT) | 0x4000);
        for (int i = 0; i < 250 && (tr(c, GRC_RX_CPU_EVENT) & 0x4000); i++) udelay(10);
    }
    mem_wr(c, SRAM_FW_MBOX, FW_MAGIC);          // tg3_write_sig_pre_reset
    if (has(c, FASFNEW)) mem_wr(c, SRAM_DRV_STATE_MBOX, 1);   // START
    if (!chip_reset(c)) return false;
    if (has(c, FASF)) mem_wr(c, SRAM_DRV_STATE_MBOX, 1);      // tg3_write_sig_legacy

    if (has(c, F57765C)) {
        uint32_t gm = tr(c, GRC_MODE);
        if (c.chiprev == 0x57785000) {
            tw(c, GRC_MODE, (gm & ~0xA0400000u) | 0x00400000u);   // PL
            tw(c, PCIE_TLDLPL_PORT + 0x14, tr(c, PCIE_TLDLPL_PORT + 0x14) | 0x80000000u);
            tw(c, GRC_MODE, gm);
        }
        if ((c.chiprev >> 8) != 0x577850) {     // not 57765 AX: transmit hang fix
            tw(c, CPMU_PADRNG_CTL, tr(c, CPMU_PADRNG_CTL) | 0x40000);
            tw(c, GRC_MODE, (gm & ~0xA0400000u) | 0x20000000u);   // DL
            tw(c, PCIE_TLDLPL_PORT + 0x0C, (tr(c, PCIE_TLDLPL_PORT + 0x0C) & ~0xFFu) | 0x2C);
            tw(c, GRC_MODE, gm);
        }
        tw(c, CPMU_LSPD_10MB_CLK, (tr(c, CPMU_LSPD_10MB_CLK) & ~0x1F0000u) | 0x130000u);
    }
    if (c.chiprev == 0x05717000 && has(c, FPCIE) && c.pcie_cap && !(pci_read(c.pci, c.pcie_cap + 0x10) & 0x100)) {
        uint32_t gm = tr(c, GRC_MODE);           // L1PLLPD_EN (5717 A0 without CLKREQ)
        tw(c, GRC_MODE, (gm & ~0xA0400000u) | 0x00400000u);
        tw(c, PCIE_TLDLPL_PORT + 0x04, tr(c, PCIE_TLDLPL_PORT + 0x04) | 0x1000);
        tw(c, GRC_MODE, gm);
    }
    if (!has(c, FCPMU)) {
        if (!has(c, FPCIE)) c.clock_ctrl |= 0x80000000u;   // DELAY_PCI_GRANT
        twf(c, CLOCK_CTRL, c.clock_ctrl);
    }
    if (c.chiprev == 0x2000 && has(c, FPCIX)) tw(c, PCISTATE, tr(c, PCISTATE) | 0x2000);
    if (has(c, FAPE)) tw(c, PCISTATE, tr(c, PCISTATE) | 0x70000);
    if ((c.chiprev >> 8) == 0x21) tw(c, MSI_DATA, tr(c, MSI_DATA) | (1u << 26) | (1u << 28) | (1u << 29));   // 5704 BX

    memset(c.stblk, 0, 4096);
    if (has(c, F57765P)) {
        uint32_t v = tr(c, DMA_RW_CTRL) & ~1u;  // DIS_CACHE_ALIGNMENT
        if (c.chiprev == 0x57785000) v &= ~0x380u;
        if (!has(c, F57765C) && a != A5717 && a != A5762) v |= 0x80;   // TAGGED_STAT_WA
        tw(c, DMA_RW_CTRL, v | c.dma_rwctrl);
    } else if (a != A5784 && a != A5761) tw(c, DMA_RW_CTRL, c.dma_rwctrl);

    c.grc_mode = (c.grc_mode & ~(0x20000u | 0x20000000u | 0x100000u | 0x800000u)) | 0x20000u | 0x100000u;   // HOST_SENDBDS, NO_TX_PHDR_CSUM
    tw(c, GRC_MODE, c.grc_mode | 0x04000000u | 0x10000u);     // IRQ_ON_MAC_ATTN, HOST_STACKUP
    uint32_t sub = pci_read(c.pci, 0x2C);
    if (sub == 0x07F01028) tw(c, DEV_STATUS_CTRL, (tr(c, DEV_STATUS_CTRL) & ~0x7000u) | 0x4000);   // Dell 5762: MRRS 2048
    tw(c, GRC_MISC_CFG, (tr(c, GRC_MISC_CFG) & ~0xFFu) | 65 << 1);   // timer prescaler: 66 MHz

    if (!has(c, F5750P) && a != A5705) {        // 5700-5704: the mbuf and descriptor pools
        tw(c, BUFMGR_MB_POOL_ADDR, SRAM_MBUF_POOL);
        tw(c, BUFMGR_MB_POOL_SIZE, a == A5704 ? 0x10000 : 0x18000);
        tw(c, BUFMGR_DMA_DESC_POOL_ADDR, SRAM_DMA_DESC_POOL);
        tw(c, BUFMGR_DMA_DESC_POOL_SIZE, 0x2000);
    }
    uint32_t rdma_low = 0x50, macrx_low = 0x20, high = 0x60;
    if (has(c, F57765P)) { rdma_low = 0; macrx_low = 0x2A; high = 0xA0; }
    else if (has(c, F5705P)) { rdma_low = 0; macrx_low = a == A5906 ? 4 : 0x10; high = a == A5906 ? 0x10 : 0x60; }
    tw(c, BUFMGR_MB_RDMA_LOW, rdma_low);
    tw(c, BUFMGR_MB_MACRX_LOW, macrx_low);
    tw(c, BUFMGR_MB_HIGH, high);
    tw(c, BUFMGR_DMA_LOW, 5);
    tw(c, BUFMGR_DMA_HIGH, 10);
    uint32_t bm = 2 | 4;                        // ENABLE, ATTN_ENABLE
    if (a == A5719) bm |= 0x80000000u;
    if (a == A5717 || a == A5762 || c.chiprev == 0x05719000 || c.chiprev == 0x05720000) bm |= 0x10;
    tw(c, BUFMGR_MODE, bm);
    {
        int i = 0;
        for (; i < 2000 && !(tr(c, BUFMGR_MODE) & 2); i++) udelay(10);
        if (i == 2000) { kprintf("net: %s: buffer manager didn't start\n", c.name); return false; }
    }
    if (c.chiprev == 0xC001) tw(c, ISO_PKT_TX, (tr(c, ISO_PKT_TX) & ~3u) | 2);

    // tg3_setup_rxbd_thresholds
    uint32_t bdcache = 128;
    if (has(c, F5750P) && !has(c, F5780C) && a != A5750 && a != A5752 && !has(c, F57765P))
        bdcache = (a == A5755 || a == A5787) ? 64 : 32;
    uint32_t max_post = (a == A5750 || a == A5752 || a == A5755) ? 8 : c.std_size;
    uint32_t nic_rep = bdcache / 2 < max_post ? bdcache / 2 : max_post, host_rep = NRX / 8 ? NRX / 8 : 1;
    tw(c, RCVBDI_STD_THRESH, nic_rep < host_rep ? nic_rep : host_rep);
    if (has(c, F57765P)) tw(c, STD_REPLENISH_LWM, bdcache);

    tw(c, RCVDBDI_STD_BD + 0, 0);
    tw(c, RCVDBDI_STD_BD + 4, (uint32_t)(uintptr_t)c.rxd);
    if (!has(c, F5717P)) tw(c, RCVDBDI_STD_BD + 12, SRAM_RX_BUFFER_DESC);
    if (!has(c, F5705P)) tw(c, RCVDBDI_MINI_BD + 8, 2);       // DISABLED
    uint32_t v;
    if (c.chiprev == 0x05719000 || (has(c, FJUMBO) && !has(c, F5780C))) {
        tw(c, RCVDBDI_JUMBO_BD + 8, 2);         // no jumbo ring
        v = has(c, F57765P) ? (uint32_t)c.std_size << 16 | RX_DMA << 2 : (uint32_t)RX_DMA << 16;
    } else v = 512u << 16;
    tw(c, RCVDBDI_STD_BD + 8, v);
    c.std_prod = NRX;
    mbox(c, MB_RCV_STD_PROD, c.std_prod);
    mbox(c, MB_RCV_JUMBO_PROD, 0);

    // tg3_rings_reset
    uint32_t lim = !has(c, F5705P) ? 16 : has(c, F5717P) ? 4 : (has(c, F57765C) || a == A5762) ? 2 : 1;
    for (uint32_t i = 1; i < lim; i++) mem_wr(c, SRAM_SEND_RCB + 16 * i + 8, 2);
    lim = has(c, F5717P) ? 17 : !has(c, F5705P) ? 16 : (a == A5755 || a == A5762 || has(c, F57765C)) ? 4 : 1;
    for (uint32_t i = 1; i < lim; i++) mem_wr(c, SRAM_RCV_RET_RCB + 16 * i + 8, 2);
    mbox(c, MB_INT0, 1);
    r32(c, a == A5906 ? MB_INT0 + GRCMBOX_BASE : MB_INT0);
    c.tx_prod = 0; c.tx_cons = 0; c.ret_cons = 0;
    tx_mbox(c, MB_SNDHOST_PROD0, 0);
    mbox(c, MB_RCVRET_CON0, 0);
    if (!has(c, F5705P)) for (int i = 0; i < 16; i++) tx_mbox(c, MB_SNDNIC_PROD0 + 8 * i, 0);
    tw(c, HOSTCC_STATUS_BLK_HOST + 0, 0);
    tw(c, HOSTCC_STATUS_BLK_HOST + 4, (uint32_t)(uintptr_t)c.stblk);
    bdinfo(c, SRAM_SEND_RCB, c.txd, (uint32_t)TX_RING << 16, SRAM_TX_BUFFER_DESC);
    bdinfo(c, SRAM_RCV_RET_RCB, ret, (uint32_t)c.ret_size << 16, 0);

    // the address: the boot code's mailbox, else the MAC's own register
    uint32_t hi = mem_rd(c, SRAM_MAC_HI_MBOX), lo;
    if ((hi >> 16) == 0x484B && (lo = mem_rd(c, SRAM_MAC_LO_MBOX), (hi & 0xFFFF) | lo)) {
        c.mac[0] = (uint8_t)(hi >> 8); c.mac[1] = (uint8_t)hi;
        c.mac[2] = (uint8_t)(lo >> 24); c.mac[3] = (uint8_t)(lo >> 16); c.mac[4] = (uint8_t)(lo >> 8); c.mac[5] = (uint8_t)lo;
    } else {
        hi = tr(c, MAC_ADDR0_HI); lo = tr(c, MAC_ADDR0_HI + 4);
        c.mac[0] = (uint8_t)(hi >> 8); c.mac[1] = (uint8_t)hi;
        c.mac[2] = (uint8_t)(lo >> 24); c.mac[3] = (uint8_t)(lo >> 16); c.mac[4] = (uint8_t)(lo >> 8); c.mac[5] = (uint8_t)lo;
    }
    if (c.mac[0] & 1 || !(c.mac[0] | c.mac[1] | c.mac[2] | c.mac[3] | c.mac[4] | c.mac[5]))
        kprintf("net: %s: no valid address from the boot code\n", c.name);
    set_mac(c, c.mac);

    tw(c, MAC_RX_MTU, 1500 + 14 + 4 + 4);
    v = 2u << 12 | 6u << 8 | 32;                // IPG_CRS, IPG, slot time
    if (a == A5720 || a == A5762) v |= tr(c, MAC_TX_LENGTHS) & 0xFFFF0000u;
    tw(c, MAC_TX_LENGTHS, v);
    tw(c, MAC_RCV_RULE_CFG, 8);                 // DEFAULT_CLASS
    tw(c, RCVLPC_CONFIG, 0x181);

    uint32_t rdmac = 0x002 | 0x004 | 0x008 | 0x010 | 0x020 | 0x040 | 0x080 | 0x100 | 0x200;
    if (a == A5717) rdmac |= 0x01000000u;
    if (a == A5784 || a == A5785 || a == A57780) rdmac |= 0x800 | 0x1000 | 0x2000;
    if (a == A5705 && c.chiprev != 0x3000 && !has(c, FPCIHI)) rdmac |= 0x30000;   // FIFO_LONG_BURST
    if (has(c, FPCIE)) rdmac |= 0x30000;
    if (a == A57766) rdmac |= 0x800000;         // JMB_2K_MMRR
    if (a == A5720 || a == A5762) rdmac |= tr(c, RDMAC_MODE) & 0x20000000u;
    if (a == A5761 || a == A5784 || a == A5785 || a == A57780 || has(c, F57765P)) {
        uint32_t reg = a == A5762 ? RDMA_RSRVCTRL2 : RDMA_RSRVCTRL, x = tr(c, reg);
        if (c.chiprev == 0x05719000 || a == A5762)
            x = (x & ~(0xFFE00000u | 0xFF0u | 0xFF000u)) | 0x28000000u | 0xC00u | 0xC0000u;
        tw(c, reg, x | 4);                      // FIFO_OFLW_FIX
    }
    if (a == A5719 || a == A5720 || a == A5762) {
        uint32_t reg = a == A5762 ? LSO_RD_DMA_CRPTEN2 : LSO_RD_DMA_CRPTEN;
        tw(c, reg, tr(c, reg) | 0x30000 | 0xC0000);
    }
    if (has(c, F5750P)) tw(c, RCVLPC_STATS_ENABLE, tr(c, RCVLPC_STATS_ENABLE) & ~0x40000u);
    else tw(c, RCVLPC_STATS_ENABLE, 0xFFFFFF);
    tw(c, RCVLPC_STATSCTRL, 1);
    tw(c, SNDDATAI_STATSENAB, 0xFFFFFF);
    tw(c, SNDDATAI_STATSCTRL, 1 | 2);

    tw(c, HOSTCC_MODE, 0);
    for (int i = 0; i < 2000 && (tr(c, HOSTCC_MODE) & 2); i++) udelay(10);
    tw(c, HOSTCC_RXCOL_TICKS, 20);              // the status block soon after a frame
    tw(c, HOSTCC_RXMAX_FRAMES, 1);
    tw(c, HOSTCC_RXCOAL_MAXF_INT, 1);
    tw(c, HOSTCC_TXCOL_TICKS, 72);
    tw(c, HOSTCC_TXMAX_FRAMES, 1);
    tw(c, HOSTCC_TXCOAL_MAXF_INT, 1);
    if (!has(c, F5705P)) {
        tw(c, HOSTCC_RXCOAL_TICK_INT, 0x19);
        tw(c, HOSTCC_TXCOAL_TICK_INT, 0x19);
        tw(c, HOSTCC_STAT_COAL_TICKS, 1000000);
        tw(c, HOSTCC_STATS_BLK_HOST + 0, 0);
        tw(c, HOSTCC_STATS_BLK_HOST + 4, (uint32_t)(uintptr_t)(c.stblk + 0x100));   // 0x800 bytes after the status block
        tw(c, HOSTCC_STATS_BLK_NIC, SRAM_STATS_BLK);
        tw(c, HOSTCC_STATUS_BLK_NIC, SRAM_STATUS_BLK);
        for (uint32_t o = SRAM_STATS_BLK; o < SRAM_STATUS_BLK + 0x50; o += 4) { mem_wr(c, o, 0); udelay(40); }
    }
    tw(c, HOSTCC_MODE, 2 | c.coalesce);
    tw(c, RCVCC_MODE, 2 | 4);
    tw(c, RCVLPC_MODE, 2);
    if (!has(c, F5705P)) tw(c, RCVLSC_MODE, 2 | 4);
    if (has(c, FMIISER)) tw_wait(c, MAC_RX_MODE, 1, 10);   // RX_MODE_RESET

    c.mac_mode |= 0x4000 | 0x800 | 0x200000 | 0x400000 | 0x800000;   // TX/RX statistics, TDE, RDE, FHDE
    if (has(c, FAPE)) c.mac_mode |= 0x10000000u | 0x08000000u;
    if (!has(c, F5705P) && a != A5700) c.mac_mode |= 0x400;          // LINK_POLARITY
    tw_wait(c, MAC_MODE, c.mac_mode | 0x1000 | 0x8000, 40);         // and clear the statistics
    // GPIOs as the boot code set them (LOMs and NICs alike); interrupt on attention, auto SEEPROM
    tw_wait(c, GRC_LOCAL_CTRL, (tr(c, GRC_LOCAL_CTRL) & 0x0001FFF0u) | 0x08 | 0x01000000u, 100);
    if (!has(c, F5705P)) tw_wait(c, DMAC_MODE, 2, 40);

    v = 0x002 | 0x004 | 0x008 | 0x010 | 0x020 | 0x040 | 0x080 | 0x100 | 0x200;
    if (a == A5705 && c.chiprev != 0x3000 && !has(c, FPCIHI)) v |= 0x400;   // RX_ACCEL
    if (has(c, F5755P)) v |= 0x20000000u;       // STATUS_TAG_FIX
    if (a == A5785) v |= 0xC0000000u;
    tw_wait(c, WDMAC_MODE, v, 40);
    if (has(c, FPCIX) && c.pcix_cap && (a == A5703 || a == A5704)) {
        uint32_t x = pci_read(c.pci, c.pcix_cap);
        uint32_t cmdx = x >> 16;
        cmdx &= a == A5703 ? ~0x000Cu : ~(0x000Cu | 0x0070u);
        cmdx |= 0x0008;                         // max read 2K
        pci_write(c.pci, c.pcix_cap, (x & 0xFFFF) | cmdx << 16);
    }
    tw_wait(c, RDMAC_MODE, rdmac, 40);
    if (a == A5719 || a == A5720) {
        uint32_t maxmtu = has(c, FJUMBO) ? 9000 : 1500;
        for (int i = 0; i < 4; i++)
            if (tr(c, RDMA_LENGTH + 4 * i) > maxmtu) {
                tw(c, LSO_RD_DMA_CRPTEN, tr(c, LSO_RD_DMA_CRPTEN) | (a == A5719 ? 0x02000000u : 0x00200000u));
                break;
            }
    }
    tw(c, RCVDCC_MODE, 2 | 4);
    if (!has(c, F5705P)) tw(c, MBFREE_MODE, 2);
    tw(c, SNDDATAC_MODE, a == A5761 ? 2 | 0x10 : 2);
    tw(c, SNDBDC_MODE, 2 | 4);
    tw(c, RCVBDI_MODE, 2 | 4);
    tw(c, RCVDBDI_MODE, 2 | 0x10 | (has(c, FLRG) ? 0x10000u : 0));
    tw(c, SNDDATAI_MODE, 2);
    tw(c, SNDBDI_MODE, 2 | 4);
    tw(c, SNDBDS_MODE, 2 | 4);

    v = 2;                                      // TX enable
    if (has(c, F5755P) || a == A5906) v |= 0x100;   // MBUF_LOCKUP_FIX
    if (a == A5720 || a == A5762) v |= tr(c, MAC_TX_MODE) & (0x400000u | 0x800000u);
    tw_wait(c, MAC_TX_MODE, v, 100);

    c.rx_mode_reg = 2;                          // RX enable
    if (has(c, F5755P)) c.rx_mode_reg |= 0x01000000u;
    if (a == A5762) c.rx_mode_reg |= 0x02000000u;
    if (!has(c, FASF)) c.rx_mode_reg |= 0x400;  // KEEP_VLAN_TAG: frames as they came
    tw_wait(c, MAC_RX_MODE, c.rx_mode_reg, 10);
    tw(c, MAC_LED_CTRL, c.led);
    tw(c, MAC_MI_STAT, 1);                      // LNKSTAT_ATTN_ENAB
    tw_wait(c, MAC_RX_MODE, c.rx_mode_reg, 10);
    twf(c, MAC_LOW_WMARK, has(c, F57765C) ? 1 : 2);

    // the PHY: woken if the boot code left it powered down or isolated
    int bmcr = phy_rd(c, 0);
    if (bmcr >= 0 && (bmcr & 0x0C00)) {
        phy_wr(c, 0, (uint16_t)((bmcr & ~0x0C00) | 0x1200));   // autonegotiate, restart
        kprintf("net: %s: PHY was powered down, woken\n", c.name);
    }
    c.speed = -1;
    link_update(c);

    set_mode(c, 1);
    tw(c, MAC_RCV_RULE0, 0xC2000000u & 0x7FFFFFFFu); tw(c, MAC_RCV_RULE0 + 4, 0xFFFFFFFFu & 0x7FFFFFFFu);
    tw(c, MAC_RCV_RULE0 + 8, 0x86000004u & 0x7FFFFFFFu); tw(c, MAC_RCV_RULE0 + 12, 0xFFFFFFFFu & 0x7FFFFFFFu);
    int limit = has(c, F5705P) && !has(c, F5780C) ? 8 : 16;
    if (has(c, FASF)) limit -= 4;
    for (int i = 4; i < limit; i++) { tw(c, MAC_RCV_RULE0 + 8 * i, 0); tw(c, MAC_RCV_RULE0 + 8 * i + 4, 0); }
    if (has(c, FAPE)) ape_wr(c, APE_HOST_HB_INT, 5000);
    if (has(c, FASFNEW)) mem_wr(c, SRAM_DRV_STATE_MBOX, 0x80000001u);   // START_DONE
    ape_driver_start(c);
    c.hb_tick = ticks;
    dbg(1, "net: %s: chip %x, %s%s%s, PHY %d, MAC_MODE %x\n", c.name, c.chiprev,
        has(c, FPCIE) ? "PCIe" : has(c, FPCIX) ? "PCI-X" : "PCI", has(c, FAPE) ? ", APE" : "",
        has(c, FASF) ? ", ASF" : "", c.phy_addr, c.mac_mode);
    return true;
}

bool link(Card& c)
{
    if (has(c, FAPE) && ticks - c.hb_tick >= 1000) {   // the APE's heartbeat
        c.hb_tick = ticks;
        ape_wr(c, APE_HOST_HB_COUNT, c.ape_hb++);
    }
    link_update(c);
    return c.speed > 0;
}

void poll(Card& c)
{
    volatile Status* s = (volatile Status*)c.stblk;
    volatile BD* ret = (volatile BD*)c.retd;
    volatile BD* std = (volatile BD*)c.rxd;
    uint16_t prod = s->idx[0].rx_prod;
    BARRIER();
    int n = 0;
    while (c.ret_cons != prod && room() && n++ < 64) {
        volatile BD& d = ret[c.ret_cons];
        uint32_t opaque = d.opaque, idx = opaque & 0xFFFF;
        unsigned len = (d.idx_len & 0xFFFF);
        bool bad = (d.err_vlan & 0x01EF0000u) || len < 18;      // RXD_ERR_MASK (an odd nibble is fine)
        if ((opaque & 0x00070000u) != 0x00010000u || idx >= (uint32_t)NRX) bad = true, idx = 0xFFFF;
        if (!bad) enqueue(c.rxb + idx * BUF, len - 4);   // less the FCS
        else st.rx_err++;
        if (idx != 0xFFFF) {                    // the buffer back on the standard ring
            volatile BD& p = std[c.std_prod % c.std_size];
            p.addr_hi = 0;
            p.addr_lo = (uint32_t)(uintptr_t)(c.rxb + idx * BUF);
            p.idx_len = RX_DMA;
            p.type_flags = 0x0004;
            p.opaque = 0x00010000u | idx;
            c.std_prod = (uint16_t)((c.std_prod + 1) % c.std_size);
        }
        c.ret_cons = (uint16_t)((c.ret_cons + 1) % c.ret_size);
    }
    if (n) {
        BARRIER();
        mbox(c, MB_RCVRET_CON0, c.ret_cons);
        mbox(c, MB_RCV_STD_PROD, c.std_prod);
    }
}

bool send(Card& c, const uint8_t* f, unsigned len)
{
    volatile Status* s = (volatile Status*)c.stblk;
    for (int i = 0; i < 2000; i++) {            // a free buffer: the card is less than NTXB behind
        c.tx_cons = s->idx[0].tx_cons;
        if ((uint16_t)((c.tx_prod - c.tx_cons) % TX_RING) < NTXB) break;
        udelay(5);
    }
    if ((uint16_t)((c.tx_prod - c.tx_cons) % TX_RING) >= NTXB) return false;
    uint8_t* b = c.txb + (c.tx_prod % NTXB) * BUF;
    memcpy(b, f, len);
    if (len < 60) { memset(b + len, 0, 60 - len); len = 60; }
    volatile TD& d = ((volatile TD*)c.txd)[c.tx_prod];
    d.addr_hi = 0;
    d.addr_lo = (uint32_t)(uintptr_t)b;
    d.len_flags = len << 16 | 0x0004;           // END
    d.vlan = 0;
    BARRIER();
    c.tx_prod = (uint16_t)((c.tx_prod + 1) % TX_RING);
    tx_mbox(c, MB_SNDHOST_PROD0, c.tx_prod);
    return true;
}
} // namespace tg3

// ---------------- USB Ethernet adapters (usb.cpp: CDC-ECM) ----------------
} // namespace
extern "C" {
int usb_net_present(void);
uint32_t usb_net_gen(void);
const char* usb_net_name(void);
void usb_net_mac(uint8_t* m);
int usb_net_send(const uint8_t* f, unsigned len);
void usb_net_filter(int bits);
}
namespace {
namespace usbnet {
bool init(Card& c)
{
    if (!usb_net_present()) return false;
    usb_net_mac(c.mac);
    snprintf(c.name, sizeof c.name, "%s", usb_net_name());
    c.pci.bus = c.pci.dev = c.pci.fn = 0xFF;
    c.spoof = false;
    return true;
}
bool link(Card&) { return usb_net_present(); }
void set_mode(Card& c, int m)
{
    int bits = 0;                               // SET_ETHERNET_PACKET_FILTER
    if (m >= 2) bits |= 0x04;                   // directed
    if (m >= 3) bits |= 0x08;                   // broadcast
    if (m >= 4) bits |= 0x02;                   // all multicast
    if (m >= 6 || (c.spoof && m >= 2)) bits |= 0x01;   // promiscuous
    usb_net_filter(bits);
}
void set_mac(Card& c, const uint8_t* a)
{
    c.spoof = memcmp(a, c.mac, 6) != 0;
    set_mode(c, rx_mode);
}
bool send(Card&, const uint8_t* f, unsigned len) { return usb_net_send(f, len); }
} // namespace usbnet

// ---------------- the cards ----------------
bool card_link(Card& c)
{
    if (!c.running) return false;
    switch (c.kind) {
    case KIND_E1000: return e1k::link(c);
    case KIND_RTL8139: return rtl::link(c);
    case KIND_R8169: return r8169::link(c);
    case KIND_TG3: return tg3::link(c);
    case KIND_IGB: return igb::link(c);
    case KIND_USB: return usbnet::link(c);
    }
    return false;
}

void card_mode(Card& c, int m)                  // 1: receiving nothing
{
    switch (c.kind) {
    case KIND_E1000: e1k::set_mode(c, m); break;
    case KIND_RTL8139: rtl::set_mode(c, m); break;
    case KIND_R8169: r8169::set_mode(c, m); break;
    case KIND_TG3: tg3::set_mode(c, m); break;
    case KIND_IGB: igb::set_mode(c, m); break;
    case KIND_USB: usbnet::set_mode(c, m); break;
    }
}

void card_mac(Card& c, const uint8_t* a)
{
    switch (c.kind) {
    case KIND_E1000: e1k::set_mac(c, a); break;
    case KIND_RTL8139: rtl::set_mac(c, a); break;
    case KIND_R8169: r8169::set_mac(c, a); break;
    case KIND_TG3: tg3::set_mac(c, a); break;
    case KIND_IGB: igb::set_mac(c, a); break;
    case KIND_USB: usbnet::set_mac(c, a); break;
    }
}

bool card_start(Card& c)
{
    switch (c.kind) {
    case KIND_E1000: c.running = e1k::init(c); break;
    case KIND_RTL8139: c.running = rtl::init(c); break;
    case KIND_R8169: c.running = r8169::init(c); break;
    case KIND_TG3: c.running = tg3::init(c); break;
    case KIND_IGB: c.running = igb::init(c); break;
    case KIND_USB: c.running = usbnet::init(c); break;
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
    case KIND_TG3: tg3::poll(c); break;
    case KIND_IGB: igb::poll(c); break;
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
    case KIND_TG3: return tg3::send(c, f, len);
    case KIND_IGB: return igb::send(c, f, len);
    case KIND_USB: return usbnet::send(c, f, len);
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
    if (c.kind == KIND_USB) kprintf("net: using card %d, %s (%s), link %s", i + 1, c.name, why, card_link(c) ? "up" : "down");
    else kprintf("net: using card %d, %s at %02x:%02x.%u (%s), link %s", i + 1, c.name, c.pci.bus, c.pci.dev, c.pci.fn,
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
static void usb_check(bool pick);
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
        int k = e1k::match(id) ? KIND_E1000 : id == 0x813910EC ? KIND_RTL8139 : r8169::match(id, pci_read(d, 0x2C)) ? KIND_R8169 :
                tg3::match(id) ? KIND_TG3 : igb::match(id) ? KIND_IGB : KIND_NONE;
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
    qbuf = (uint8_t*)phys_alloc(QLEN * FRAME_MAX);
    net_ready = 1;                              // a USB adapter may come later
    usb_check(false);
    if (!ncards) { kprintf("net: no Ethernet card vmdos can drive (a USB one is taken when plugged in)\n"); return; }

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
// A card for the USB adapter when one is plugged in (the same entry when it
// comes back); auto: used if nothing has a link.
static void usb_check(bool pick)
{
    static uint32_t gen;
    uint32_t g = usb_net_gen();
    if (g == gen) return;
    gen = g;
    int u = -1;
    for (int i = 0; i < ncards; i++) if (cards[i].kind == KIND_USB) u = i;
    if (usb_net_present()) {
        if (u < 0) {
            if (ncards == MAXCARDS) return;
            u = ncards++;
            cards[u].kind = KIND_USB;
        }
        Card& c = cards[u];
        if (!card_start(c)) return;
        kprintf("net: card %d: %s", u + 1, c.name);
        log_mac(", MAC ", c.mac);
        kprintf("\n");
        if (!pick) card_mode(c, 1);             // at boot: chosen with the others
        else if (active == u) use_card(u, "plugged in again");
        else if (active < 0 || (auto_pick && !card_link(cards[active]))) use_card(u, "auto: plugged in");
        else card_mode(c, 1);
    } else if (u >= 0 && cards[u].running) {
        cards[u].running = false;
        kprintf("net: card %d (%s) unplugged\n", u + 1, cards[u].name);
        if (active == u && auto_pick)
            for (int i = 0; i < ncards; i++)
                if (i != u && card_link(cards[i])) { use_card(i, "auto: the USB adapter went"); break; }
    }
}

// usb.cpp: a frame from the USB adapter (from usb_poll, the timer tick).
extern "C" void net_usb_rx(const uint8_t* f, unsigned len)
{
    if (active < 0 || cards[active].kind != KIND_USB || !qbuf) return;
    if (cards[active].spoof && rx_mode < 6 && !(f[0] & 1) && memcmp(f, cur_mac(), 6)) return;   // not ours
    if (!room()) { st.dropped++; return; }
    enqueue(f, len);
}

extern "C" void net_tick(void)
{
    usb_check(true);
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
