// Qualcomm Atheros AR8161/AR8162/AR8171/AR8172, Killer E2200/E2400/E2500
// (Linux: alx). Started as Linux's alx does (alx_reset_pcie, alx_reset_mac,
// alx_configure, alx_start_mac on link up), one receive and one transmit
// ring, polled. As alx, a PHY the BIOS already set up (its "inited" mark in
// the PHY's debug register and the driver register agree) is left alone;
// otherwise it gets alx_reset_phy and is set to autonegotiate.
#include "net.hpp"

using namespace net;

namespace {
namespace alx {
const int NRX = 64, NTX = 64, BUF = 2048, RXBUF = 1544;   // RXBUF: ALX_MAX_FRAME_LEN(1500)
enum {
    UE_SVRT = 0x010C, PDLL_TRNS1 = 0x1104, PMCTRL = 0x12F8, MASTER = 0x1400, IRQ_MODU_TIMER = 0x1408,
    PHY_CTRL = 0x140C, MAC_STS = 0x1410, MDIO = 0x1414, SERDES = 0x1424, LPI_CTRL = 0x1440, MDIO_EXTN = 0x1448,
    IDLE_DECISN_TIMER = 0x1474, MAC_CTRL = 0x1480, STAD0 = 0x1488, STAD1 = 0x148C, HASH_TBL0 = 0x1490,
    HASH_TBL1 = 0x1494, MTU = 0x149C, WOL0 = 0x14A0, SRAM5 = 0x1524, SRAM9 = 0x1534, RX_BASE_ADDR_HI = 0x1540,
    TX_BASE_ADDR_HI = 0x1544, RFD_ADDR_LO = 0x1550, RFD_RING_SZ = 0x1560, RFD_BUF_SZ = 0x1564,
    RRD_ADDR_LO = 0x1568, RRD_RING_SZ = 0x1578, TPD_PRI0_ADDR_LO = 0x1580, TPD_RING_SZ = 0x1584,
    TXQ0 = 0x1590, TXQ1 = 0x1594, RXQ0 = 0x15A0, RXQ2 = 0x15A8, DMA = 0x15C0, SMB_TIMER = 0x15C4,
    TINT_TPD_THRSHLD = 0x15C8, TINT_TIMER = 0x15CC, RFD_PIDX = 0x15E0, TPD_PRI0_PIDX = 0x15F2,
    TPD_PRI0_CIDX = 0x15F6, ISR = 0x1600, IMR = 0x1604, INT_RETRIG = 0x1608, DRV = 0x1804, CLK_GATE = 0x1814,
    WRR = 0x1938, HQTPD = 0x193C, MISC = 0x19C0, MSIC2 = 0x19C8, MISC3 = 0x19CC, MSIX_MASK = 0x0090,
    SLD = 0x0218, EFLD = 0x0204,
};
const uint32_t PHY_CFG_MINE = 0xB8;             // ethadv_to_hw_cfg(autoneg, 10/100 half+full, 1000 full) >> 21

bool rev_a(Card& c) { return c.x.rev <= 1; }

// ---- MDIO: the standard registers, the extended (MMD) ones, the debug port ----
bool mdio_idle(Card& c)
{
    for (int i = 0; i < 120; i++) { if (!(r32(c, MDIO) & (1u << 27))) return true; udelay(10); }
    return false;
}
uint32_t clk(Card& c) { return (c.x.speed ? 0u : 7u) << 24; }   // slow clock while there's no link (hibernation)
int phy_core_rd(Card& c, bool ext, int dev, int reg)
{
    uint32_t v;
    if (ext) {
        w32(c, MDIO_EXTN, (uint32_t)dev << 16 | reg);
        v = (1u << 22) | (1u << 23) | (1u << 30) | (1u << 21) | clk(c);   // preamble, start, ext, read
    } else v = (1u << 22) | clk(c) | (uint32_t)reg << 16 | (1u << 23) | (1u << 21);
    w32(c, MDIO, v);
    if (!mdio_idle(c)) return -1;
    return r32(c, MDIO) & 0xFFFF;
}
void phy_core_wr(Card& c, bool ext, int dev, int reg, uint16_t d)
{
    uint32_t v;
    if (ext) {
        w32(c, MDIO_EXTN, (uint32_t)dev << 16 | reg);
        v = (1u << 22) | clk(c) | d | (1u << 23) | (1u << 30);
    } else v = (1u << 22) | clk(c) | (uint32_t)reg << 16 | d | (1u << 23);
    w32(c, MDIO, v);
    mdio_idle(c);
}
int phy_rd(Card& c, int reg) { return phy_core_rd(c, false, 0, reg); }
void phy_wr(Card& c, int reg, uint16_t d) { phy_core_wr(c, false, 0, reg, d); }
int ext_rd(Card& c, int dev, int reg) { return phy_core_rd(c, true, dev, reg); }
void ext_wr(Card& c, int dev, int reg, uint16_t d) { phy_core_wr(c, true, dev, reg, d); }
int dbg_rd(Card& c, int reg) { phy_wr(c, 0x1D, (uint16_t)reg); return phy_rd(c, 0x1E); }
void dbg_wr(Card& c, int reg, uint16_t d) { phy_wr(c, 0x1D, (uint16_t)reg); phy_wr(c, 0x1E, d); }

bool match(uint32_t id, uint32_t)
{
    if ((id & 0xFFFF) != 0x1969) return false;
    uint16_t d = id >> 16;
    return d == 0x1091 || d == 0xE091 || d == 0xE0A1 || d == 0xE0B1 || d == 0x1090 || d == 0x10A1 || d == 0x10A0;
}

void enable_aspm(Card& c, bool l0s, bool l1)     // alx_enable_aspm
{
    uint32_t p = r32(c, PMCTRL);
    p = (p & ~(0xFu << 24)) | 0xCu << 24;       // LCKDET_TIMER
    p |= (1u << 15) | (1u << 13) | (1u << 6);   // RCVR_WT_1US, L1_CLKSW_EN, L1_SRDSRX_PWD
    p = (p & ~(0xFu << 20)) | 0xFu << 20;       // L1REQ_TO
    p = (p & ~(0x7u << 16)) | 4u << 16;         // L1_TIMER 16 us
    p &= ~((1u << 4) | (1u << 5) | (1u << 7) | (1u << 29) | (1u << 31) | (1u << 12) | (1u << 3) | (1u << 30) |
           (1u << 19) | (1u << 11));
    if (rev_a(c) && c.x.cr) p |= (1u << 4) | (1u << 5);   // L1_SRDS_EN, L1_SRDSPLL_EN
    if (l0s) p |= (1u << 12) | (1u << 30);
    if (l1) p |= (1u << 3) | (1u << 30);
    w32(c, PMCTRL, p);
}

void reset_osc(Card& c)
{
    w32(c, MISC3, (r32(c, MISC3) & ~2u) | 1);   // 25M not by software, not internal
    uint32_t v = r32(c, MISC);
    if (c.x.rev >= 2) {                         // B0 and later
        v = (v & ~(7u << 21)) | 7u << 21;       // PSW_OCP default
        v &= ~8u;                               // INTNLOSC_OPEN: 0 -> 1 latches the oscillator
        w32(c, MISC, v);
        w32(c, MISC, v | 8);
        uint32_t v2 = r32(c, MSIC2) & ~1u;
        w32(c, MSIC2, v2);
        w32(c, MSIC2, v2 | 1);                  // calibrate
    } else {
        v &= ~8u;
        if (rev_a(c)) v &= ~(1u << 12);         // ISO_EN
        w32(c, MISC, v | 8);
        w32(c, MISC, v);
    }
    udelay(20);
}

bool reset_mac(Card& c)                         // alx_reset_mac
{
    bool a_cr = rev_a(c) && c.x.cr;
    uint32_t pm = 0;
    w32(c, MSIX_MASK, 0xFFFFFFFF);
    w32(c, IMR, 0);
    w32(c, ISR, 1u << 31);
    w32(c, RXQ0, r32(c, RXQ0) & ~(1u << 31));
    w32(c, TXQ0, r32(c, TXQ0) & ~(1u << 5));
    udelay(40);
    c.x.mac_ctrl &= ~3u;
    w32(c, MAC_CTRL, c.x.mac_ctrl);
    for (int i = 0; i < 50 && (r32(c, MAC_STS) & 0xF); i++) udelay(10);
    w32(c, RFD_PIDX, 1);                        // the MAC reset workaround
    if (a_cr) {
        pm = r32(c, PMCTRL);
        if (pm & ((1u << 3) | (1u << 12))) w32(c, PMCTRL, pm & ~((1u << 3) | (1u << 12)));
    }
    w32(c, MASTER, r32(c, MASTER) | 1 | (1u << 6));   // DMA_MAC_RST, OOB_DIS
    udelay(10);
    int i = 0;
    uint32_t v = 0;
    for (; i < 50; i++) { if (!r32(c, RFD_PIDX)) break; udelay(10); }
    for (; i < 50; i++) { v = r32(c, MASTER); if (!(v & 1)) break; udelay(10); }
    if (i == 50) return false;
    udelay(10);
    if (a_cr) {
        w32(c, MASTER, v | (1u << 12));         // PCLKSEL_SRDS
        if (pm & ((1u << 3) | (1u << 12))) w32(c, PMCTRL, pm);
    }
    reset_osc(c);
    w32(c, MISC3, (r32(c, MISC3) & ~2u) | 1);
    v = r32(c, MISC) & ~8u;
    if (rev_a(c)) v &= ~(1u << 12);
    w32(c, MISC, v);
    udelay(20);
    w32(c, MAC_CTRL, c.x.mac_ctrl);
    w32(c, SERDES, r32(c, SERDES) | (1u << 17) | (1u << 18));   // MAC/PHY clock slow-down
    return true;
}

void reset_phy(Card& c)                         // alx_reset_phy
{
    uint32_t v = r32(c, PHY_CTRL);
    v &= ~(1u | (1u << 7) | (1u << 5) | (1u << 14) | (1u << 2) | (1u << 17) | (1u << 13));   // DSPRST_OUT, IDDQ, GATE_25M, POWER_DOWN, CLS
    v |= (1u << 12) | (1u << 11) | (1u << 10);  // RST_ANALOG, HIB_PULSE, HIB_EN
    w32(c, PHY_CTRL, v);
    udelay(10);
    w32(c, PHY_CTRL, v | 1);
    udelay(800);
    dbg_wr(c, 0x29, 0x129D);                    // power saving, hibernation
    dbg_wr(c, 0x04, 0xBB8B);
    ext_wr(c, 3, 0x8062, 0x3);
    w32(c, LPI_CTRL, r32(c, LPI_CTRL) & ~1u);   // no EEE
    ext_wr(c, 7, 0x3C, 0);
    dbg_wr(c, 0x12, 0x4C04);
    dbg_wr(c, 0x05, 0x2C46);
    dbg_wr(c, 0x36, 0xE12C);
    dbg_wr(c, 0x00, 0x02EF);
    int p = dbg_rd(c, 0x3D);
    dbg_wr(c, 0x3D, (uint16_t)(p & ~0x0080));
    ext_wr(c, 7, 0x8027, 0x8A05);               // the RTL8139C 120 m issue
    ext_wr(c, 7, 0x8023, 0);
    if (c.x.lnk_patch) {
        ext_wr(c, 3, 0x8003, (uint16_t)(ext_rd(c, 3, 0x8003) | 0x8000));
        dbg_wr(c, 0x3D, (uint16_t)(dbg_rd(c, 0x3D) | 0x8000));
        ext_wr(c, 3, 0x8005, (uint16_t)(ext_rd(c, 3, 0x8005) | 0x4000));
    }
    phy_wr(c, 0x12, 0x0400 | 0x0800);           // PHY interrupt mask: link up/down
}

void setup_autoneg(Card& c)                     // alx_setup_speed_duplex: autonegotiate everything
{
    phy_wr(c, 0x1D, 0);
    uint32_t drv = r32(c, DRV) & ~(0xFFu << 21);
    phy_wr(c, 4, 0x01 | 0x20 | 0x40 | 0x80 | 0x100 | 0x400);   // CSMA, 10/100 half+full, pause
    phy_wr(c, 9, c.x.giga ? 0x200 : 0);         // 1000 full
    phy_wr(c, 0, 0x8000 | 0x1000 | 0x0200);     // reset, autonegotiate, restart
    phy_wr(c, 0x1D, 0x3F);                      // "inited"
    w32(c, DRV, drv | PHY_CFG_MINE << 21);
}

bool phy_configured(Card& c)
{
    if (!(r32(c, PHY_CTRL) & 1)) return false;  // PHY in reset
    uint32_t cfg = (r32(c, DRV) >> 21) & 0xFF;
    if (!cfg) return false;
    return phy_rd(c, 0x1D) == 0x3F && cfg == PHY_CFG_MINE;
}

void post_phy_link(Card& c)                     // alx_post_phy_link: B0 and A only
{
    if (c.x.rev != 2 && !rev_a(c)) return;
    bool adj = c.x.rev == 2;
    if (c.x.speed) {
        int len = ext_rd(c, 3, 0x8006) & 0xFF, agc = (dbg_rd(c, 0x23) >> 8) & 0x3F;
        bool lng = (c.x.speed == 1000 && (len > 116 || (len == 0 && agc > 40))) ||
                   (c.x.speed == 100 && (len > 152 || (len == 0 && agc > 44)));
        dbg_wr(c, 0x15, lng ? 0x3210 : 0x3220);
        int afe = ext_rd(c, 7, 0x801A);
        ext_wr(c, 7, 0x801A, (uint16_t)(lng ? afe | 0x40 : afe & ~0x40));
        if (adj && c.x.lnk_patch) {
            if (c.x.speed == 100) dbg_wr(c, 0x18, 0x05EA);
            else if (c.x.speed == 1000) dbg_wr(c, 0x1C, (uint16_t)((dbg_rd(c, 0x1C) & ~(0x7F << 2)) | 0x54 << 2));
        }
    } else {
        ext_wr(c, 7, 0x801A, (uint16_t)(ext_rd(c, 7, 0x801A) & ~0x40));
        if (adj && c.x.lnk_patch) {
            dbg_wr(c, 0x18, 0x02EA);
            dbg_wr(c, 0x1C, (uint16_t)((dbg_rd(c, 0x1C) & ~(0x7F << 2)) | 0x2E << 2));
        }
    }
}

void set_mode(Card& c, int m)
{
    w32(c, HASH_TBL0, m >= 4 ? 0xFFFFFFFF : 0);
    w32(c, HASH_TBL1, m >= 4 ? 0xFFFFFFFF : 0);
    c.x.mac_ctrl &= ~((1u << 25) | (1u << 15) | (1u << 26));   // MULTIALL, PROMISC, BRD
    if (m >= 3) c.x.mac_ctrl |= 1u << 26;
    if (m >= 4) c.x.mac_ctrl |= 1u << 25;
    if (m >= 6) c.x.mac_ctrl |= 1u << 15;
    w32(c, MAC_CTRL, c.x.mac_ctrl);
}

void set_mac(Card& c, const uint8_t* a)          // STAD0 = 6AF600DC, STAD1 = 000B for 00-0B-6A-F6-00-DC
{
    w32(c, STAD0, (uint32_t)a[2] << 24 | a[3] << 16 | a[4] << 8 | a[5]);
    w32(c, STAD1, a[0] << 8 | a[1]);
}

bool read_mac(Card& c)
{
    uint32_t m0 = r32(c, STAD0), m1 = r32(c, STAD1);
    c.mac[0] = (uint8_t)(m1 >> 8); c.mac[1] = (uint8_t)m1;
    c.mac[2] = (uint8_t)(m0 >> 24); c.mac[3] = (uint8_t)(m0 >> 16); c.mac[4] = (uint8_t)(m0 >> 8); c.mac[5] = (uint8_t)m0;
    return !(c.mac[0] & 1) && (c.mac[0] | c.mac[1] | c.mac[2] | c.mac[3] | c.mac[4] | c.mac[5]);
}
bool wait_clear(Card& c, int reg, uint32_t bits)
{
    for (int i = 0; i < 100; i++) { if (!(r32(c, reg) & bits)) return true; udelay(1000); }
    return false;
}
bool get_mac(Card& c)                           // alx_get_perm_macaddr: the register, the eFuse, the flash/EEPROM
{
    if (read_mac(c)) return true;
    if (wait_clear(c, SLD, (1u << 12) | (1u << 11))) {
        w32(c, SLD, r32(c, SLD) | (1u << 11));
        if (wait_clear(c, SLD, 1u << 11) && read_mac(c)) return true;
    }
    if (r32(c, EFLD) & ((1u << 10) | (1u << 9)))
        if (wait_clear(c, EFLD, (1u << 5) | 1)) {
            w32(c, EFLD, r32(c, EFLD) | 1);
            if (wait_clear(c, EFLD, 1) && read_mac(c)) return true;
        }
    return false;
}

// alx_configure_basic, alx_disable_rss, the receive mode, the rings
void configure(Card& c)
{
    set_mac(c, c.mac);
    w32(c, CLK_GATE, 0x3F);
    if (c.x.rev >= 2) w32(c, IDLE_DECISN_TIMER, 0x400);
    w32(c, SMB_TIMER, 400 * 500);
    w32(c, MASTER, r32(c, MASTER) | (1u << 11) | (1u << 10) | (1u << 7));   // IRQMOD2, IRQMOD1, SYSALVTIMER
    w32(c, IRQ_MODU_TIMER, 200 >> 1);
    w32(c, INT_RETRIG, 20000);
    w32(c, TINT_TPD_THRSHLD, NTX / 3);
    w32(c, TINT_TIMER, 200);
    uint32_t raw_mtu = 1500 + 14 + 4 + 4;
    w32(c, MTU, raw_mtu);
    w32(c, TXQ1, ((raw_mtu + 7) >> 3) | (1u << 11));   // ERRLGPKT_DROP_EN
    int cap = pci_cap(c.pci, 0x10);
    uint32_t max_payload = 2;
    if (cap) {
        uint32_t dc = pci_read(c.pci, cap + 8);
        max_payload = (128u << ((dc >> 12) & 7)) >> 8;
        if (max_payload < 2) pci_write(c.pci, cap + 8, (dc & 0xFFFF & ~0x7000u) | 2u << 12);   // read request 512
    }
    w32(c, TXQ0, 5 | (1u << 6) | (1u << 7) | (1u << 4) | 0x200u << 16);   // TPD burst, enhanced mode, LSO 802.3, IP options, TXF burst
    w32(c, HQTPD, 5 | 5 << 4 | 5 << 8 | (1u << 31));
    uint32_t len = (r32(c, SRAM5) & 0xFFF) << 3, xoff = 1536 >> 3, xon;
    xon = len > 8 * 1024 ? (len - 3212) >> 3 : (len - 1536) >> 3;
    w32(c, RXQ2, xoff << 16 | xon);
    uint32_t rq = 8u << 20 | 0x100u << 8 | 0x3C | (1u << 7);   // RFD prefetch, RSS off, IPv6 parse
    if (c.x.giga) rq |= 3;                      // ASPM threshold 100M
    w32(c, RXQ0, rq);
    uint32_t chn = c.x.rev >= 2 ? 4 : 2;
    w32(c, DMA, 4 | (1u << 10) | max_payload << 4 | 4u << 16 | 15u << 11 | (chn - 1) << 26);
    w32(c, WRR, 3u << 29 | 4 | 4 << 8 | 4 << 16 | 4u << 24);
    set_mode(c, rx_mode);
    w32(c, MAC_CTRL, c.x.mac_ctrl);

    // the rings (alx_init_ring_ptrs), all buffers but one posted
    c.x.rx_idx = c.x.rrd_idx = c.x.tx_prod = c.x.tx_cons = 0;
    memset(c.x.rrd, 0, NRX * 16);
    w32(c, TPD_PRI0_ADDR_LO, (uint32_t)(uintptr_t)c.txd);
    w32(c, RRD_ADDR_LO, (uint32_t)(uintptr_t)c.x.rrd);
    w32(c, RFD_ADDR_LO, (uint32_t)(uintptr_t)c.rxd);
    w32(c, TX_BASE_ADDR_HI, 0);
    w32(c, TPD_RING_SZ, NTX);
    w32(c, RX_BASE_ADDR_HI, 0);
    w32(c, RRD_RING_SZ, NRX);
    w32(c, RFD_RING_SZ, NRX);
    w32(c, RFD_BUF_SZ, RXBUF);
    w32(c, SRAM9, 1);                           // load the pointers
    c.x.rfd_prod = NRX - 1;
    w16(c, RFD_PIDX, c.x.rfd_prod);
    w32(c, ISR, ~(1u << 31));                   // clear old interrupts (they stay masked: polled)
}

void start_mac(Card& c)                         // alx_start_mac
{
    w32(c, RXQ0, r32(c, RXQ0) | (1u << 31));
    w32(c, TXQ0, r32(c, TXQ0) | (1u << 5));
    uint32_t m = c.x.mac_ctrl;
    if (c.x.full) m |= 1u << 5; else m &= ~(1u << 5);
    m = (m & ~(3u << 20)) | (c.x.speed == 1000 ? 2u : 1u) << 20;
    m |= 3;                                     // TX, RX
    c.x.mac_ctrl = m;
    w32(c, MAC_CTRL, m);
}

bool init(Card& c)
{
    pci_d0(c.pci);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x07) & ~0x400u);   // I/O, memory, bus master
    c.mm = map_bar(c.pci, 0x4000);
    if (!c.mm) return false;
    uint8_t revid = (uint8_t)pci_read(c.pci, 0x08);
    c.x.rev = revid >> 3;
    c.x.cr = revid & 1;
    c.x.giga = c.device & 1;
    c.x.lnk_patch = c.device == 0x1091 && pci_read(c.pci, 0x2C) == 0x00911969 && revid == 0;
    static const struct { uint16_t id; const char* name; } names[] = {
        { 0x1091, "AR8161" }, { 0xE091, "Killer E2200" }, { 0xE0A1, "Killer E2400" }, { 0xE0B1, "Killer E2500" },
        { 0x1090, "AR8162" }, { 0x10A1, "AR8171" }, { 0x10A0, "AR8172" },
    };
    for (const auto& n : names) if (n.id == c.device) snprintf(c.name, sizeof c.name, "%s", n.name);
    if (c.x.rev > 3) { kprintf("net: %s: chip revision %u vmdos doesn't know\n", c.name, c.x.rev); return false; }
    c.x.speed = 0;
    c.x.mac_ctrl = (1u << 30) | (1u << 29) | (1u << 26) | (1u << 7) | (1u << 6) | (1u << 3) | (1u << 2) | 7u << 10;
    // WOLSPED_SWEN, MHASH_ALG_HI5B, BRD_EN, PCRCE, CRCE, RX/TX flow control, preamble 7

    // alx_reset_pcie
    w32(c, WOL0, 0);
    w32(c, PDLL_TRNS1, r32(c, PDLL_TRNS1) & ~(1u << 11));
    w32(c, UE_SVRT, r32(c, UE_SVRT) & ~((1u << 4) | (1u << 13)));
    uint32_t v = r32(c, MASTER);
    if (rev_a(c) && c.x.cr) {
        if (!(v & (1u << 5)) || !(v & (1u << 12))) w32(c, MASTER, v | (1u << 12) | (1u << 5));
    } else if (!(v & (1u << 5)) || (v & (1u << 12))) w32(c, MASTER, (v & ~(1u << 12)) | (1u << 5));
    enable_aspm(c, true, true);
    udelay(10);

    bool configured = phy_configured(c);
    if (!configured) reset_phy(c);
    if (!reset_mac(c)) { kprintf("net: %s: MAC reset failed\n", c.name); return false; }
    if (!configured) setup_autoneg(c);
    dbg(1, "net: %s rev %u%s: PHY %s\n", c.name, c.x.rev, c.x.cr ? " (CR)" : "",
        configured ? "as the BIOS set it" : "reset, negotiating");
    if (!get_mac(c)) kprintf("net: %s: no valid address\n", c.name);

    c.rxd = (uint8_t*)phys_alloc(NRX * 8);
    c.x.rrd = (uint8_t*)phys_alloc(NRX * 16);
    c.txd = (uint8_t*)phys_alloc(NTX * 16);
    c.rxb = (uint8_t*)phys_alloc(NRX * BUF);
    c.txb = (uint8_t*)phys_alloc(NTX * BUF);
    volatile uint32_t* rfd = (volatile uint32_t*)c.rxd;
    for (int i = 0; i < NRX; i++) { rfd[2 * i] = (uint32_t)(uintptr_t)(c.rxb + i * BUF); rfd[2 * i + 1] = 0; }
    configure(c);
    return true;
}

// alx_read_phy_link, and what alx_check_link does on a change
bool link(Card& c)
{
    phy_rd(c, 0x13);                            // clear the PHY's interrupt status
    phy_rd(c, 1);
    int bmsr = phy_rd(c, 1), speed = 0;
    bool full = false;
    if (bmsr >= 0 && (bmsr & 4)) {
        int p = phy_rd(c, 0x11);                // PHY specific status
        if (p >= 0 && (p & 0x0800)) {
            switch (p & 0xC000) { case 0x8000: speed = 1000; break; case 0x4000: speed = 100; break; case 0: speed = 10; }
            full = (p & 0x2000) != 0;
        }
    }
    if (speed == c.x.speed) return speed != 0;
    if (speed) {
        c.x.speed = speed; c.x.full = full;
        dbg(1, "net: %s: %d Mbit/s, %s duplex\n", c.name, speed, full ? "full" : "half");
        post_phy_link(c);
        enable_aspm(c, true, true);
        start_mac(c);
    } else {                                    // down: the MAC reset, everything set up again
        c.x.speed = 0;
        reset_mac(c);
        configure(c);
        enable_aspm(c, false, true);
        post_phy_link(c);
    }
    return speed != 0;
}

void poll(Card& c)
{
    volatile uint32_t* rrd = (volatile uint32_t*)c.x.rrd;
    int n = 0;
    while (n < NRX && room()) {
        volatile uint32_t* d = rrd + 4 * c.x.rrd_idx;
        uint32_t w3 = d[3];
        if (!(w3 & (1u << 31))) break;          // UPDATED
        BARRIER();
        uint32_t w0 = d[0];
        d[3] = 0;
        if (((w0 >> 20) & 0xFFF) != c.x.rx_idx || ((w0 >> 16) & 0xF) != 1) {   // out of step: start again
            st.rx_err++;
            kprintf("net: %s: receive ring out of step, reset\n", c.name);
            reset_mac(c); configure(c);
            if (c.x.speed) start_mac(c);
            return;
        }
        unsigned len = w3 & 0x3FFF;
        if ((w3 & ((1u << 20) | (1u << 30))) || len < 18) st.rx_err++;   // ERR_RES, ERR_LEN
        else enqueue(c.rxb + c.x.rx_idx * BUF, len - 4);   // less the FCS
        c.x.rx_idx = (uint16_t)((c.x.rx_idx + 1) % NRX);
        c.x.rrd_idx = (uint16_t)((c.x.rrd_idx + 1) % NRX);
        c.x.rfd_prod = (uint16_t)((c.x.rfd_prod + 1) % NRX);   // that buffer posted again
        n++;
    }
    if (n) { BARRIER(); w16(c, RFD_PIDX, c.x.rfd_prod); }
}

bool send(Card& c, const uint8_t* f, unsigned len)
{
    for (int i = 0; i < 2000; i++) {            // room: the card behind by less than the ring
        c.x.tx_cons = r16(c, TPD_PRI0_CIDX);
        if ((uint16_t)((c.x.tx_prod + 1) % NTX) != c.x.tx_cons) break;
        udelay(5);
    }
    if ((uint16_t)((c.x.tx_prod + 1) % NTX) == c.x.tx_cons) return false;
    uint8_t* b = c.txb + c.x.tx_prod * BUF;
    memcpy(b, f, len);
    if (len < 60) { memset(b + len, 0, 60 - len); len = 60; }
    volatile uint32_t* d = (volatile uint32_t*)c.txd + 4 * c.x.tx_prod;
    d[0] = len;                                 // length, no VLAN tag
    d[1] = 1u << 31;                            // EOP
    d[2] = (uint32_t)(uintptr_t)b;
    d[3] = 0;
    BARRIER();
    c.x.tx_prod = (uint16_t)((c.x.tx_prod + 1) % NTX);
    w16(c, TPD_PRI0_PIDX, c.x.tx_prod);
    return true;
}
} // namespace alx
} // namespace

const NicDriver net::nic_alx = {
    KIND_ALX, alx::match,
    alx::init, alx::link, alx::set_mode, alx::set_mac, alx::poll, alx::send, nullptr,
};
