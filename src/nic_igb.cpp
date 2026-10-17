// Intel 82575/82576/82580/I350/I354/I210/I211, DH89xxCC (Linux: igb)
// One receive and one transmit queue (queue 0) with advanced descriptors,
// as Linux's igb sets them up (igb_reset_hw_82575/82580, igb_configure_rx/
// tx_ring), polled. Copper ports on the internal PHY, which keeps the boot
// code's settings (woken if powered down); SerDes/SGMII ports are tried as
// the NVM set them up. The flashless I210's PLL workaround is done too.
#include "net.hpp"

using namespace net;

namespace {
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
uint32_t phy_mask(Card& c) { static const uint32_t m[4] = { 2, 4, 0x20, 0x40 }; return m[c.g.port & 3]; }
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
    w32(c, MDIC, v | (uint32_t)reg << 16 | (uint32_t)c.g.phy_addr << 21 | (write ? 1u : 2u) << 26);
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
    w32(c, RAH, a[4] | a[5] << 8 | 0x80000000u | (c.g.type != T82575 ? 0x40000u : 0));   // valid, pool 0
}

bool init(Card& c)
{
    const Dev* dv = dev_of((uint32_t)c.device << 16 | c.vendor);
    c.g.type = dv->type;
    pci_d0(c.pci);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x06) & ~0x400u);   // memory, bus master
    c.mm = map_bar(c.pci, 0x20000);
    if (!c.mm) return false;
    snprintf(c.name, sizeof c.name, "%s", dv->name);
    if (r32(c, STATUS) == 0xFFFFFFFF) { kprintf("net: %s doesn't answer\n", c.name); return false; }
    c.g.port = (r32(c, STATUS) >> 2) & 3;       // the port

    // igb_reset_hw_82575 / _82580
    w32(c, CTRL, r32(c, CTRL) | 4);             // GIO master disable
    for (int i = 0; i < 800 && (r32(c, STATUS) & 0x80000); i++) udelay(100);
    if (c.g.type <= T82576) {                  // PCIe completion timeout
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
    if (c.g.type >= T82580) w32(c, STATUS, 0x00100000);   // DEV_RST_SET
    w32(c, IMC, 0xFFFFFFFF);
    w32(c, EIMC, 0xFFFFFFFF);
    r32(c, ICR);
    // the manageability configuration cycle
    uint32_t done = 0x40000u << c.g.port;
    int mr = c.g.type >= TI210 ? EEMNGCTL_I210 : EEMNGCTL;
    for (int i = 0; i < 100 && !(r32(c, mr) & done); i++) udelay(1000);

    w32(c, CTRL_EXT, r32(c, CTRL_EXT) | (1u << 28));   // DRV_LOAD: a driver has it
    uint32_t mdicnfg = c.g.type >= T82580 ? r32(c, MDICNFG) : 0;
    c.g.phy_addr = (mdicnfg & 0x80000000u) ? (mdicnfg >> 21) & 0x1F : 1;
    if (c.g.type >= TI210 && !(r32(c, EECD) & 0x80000)) pll_workaround(c);   // flashless

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
        dbg(1, "net: %s PHY %d: BMCR %04x\n", c.name, c.g.phy_addr, bmcr & 0xFFFF);
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
    if (c.g.type >= T82576) w32(c, VMOLR, (r32(c, VMOLR) & ~(0x08000000u | 0x00020000u)) | 0x01000000u | 0x08000000u);   // untagged, broadcast
    uint32_t pth = c.g.type == TI354 ? 12 : 8;
    w32(c, RXDCTL, pth | 8u << 8 | 1u << 16 | 0x02000000u);   // write back each descriptor (no interrupts to flush them)
    for (int i = 0; i < 100 && !(r32(c, RXDCTL) & 0x02000000u); i++) udelay(100);
    w32(c, RDT, NRX - 1);
    c.rx_cur = 0;

    w32(c, TXDCTL, 0);
    w32(c, TDLEN, NTX * 16);
    w32(c, TDBAL, (uint32_t)(uintptr_t)c.txd); w32(c, TDBAH, 0);
    w32(c, TDH, 0); w32(c, TDT, 0);
    w32(c, TXDCTL, (c.g.type == TI354 ? 20u : 8u) | 1u << 8 | 1u << 16 | 0x02000000u);
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
} // namespace

const NicDriver net::nic_igb = {
    KIND_IGB, [](uint32_t id, uint32_t) { return igb::match(id); },
    igb::init, igb::link, igb::set_mode, igb::set_mac, igb::poll, igb::send, nullptr,
};
