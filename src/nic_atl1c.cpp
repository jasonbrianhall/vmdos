// Atheros L1C/L2C (AR8131/AR8132), AR8152 (L2C-B, B2), AR8151 (L1D, L1D 2.0)
// and the Killer variant of the last (Linux: atl1c). Started as Linux's atl1c
// does at probe and open (atl1c_reset_pcie, atl1c_phy_reset, atl1c_reset_mac,
// atl1c_phy_init, atl1c_configure, atl1c_start_mac on link up), one receive
// and one transmit ring, polled. As atl1c, the PHY is always reset and set to
// autonegotiate (these chips leave no mark of what the BIOS did).
#include "net.hpp"

using namespace net;

namespace {
namespace atl1c {
const int NRX = 64, NTX = 64, BUF = 2048, RXBUF = 1536;   // RXBUF: >= AT_RX_BUF_SIZE (1522)
enum { L1C, L2C, L2C_B, L2C_B2, L1D, L1D_2, MT };         // Linux's nic_type
enum {
    LINK_CTRL = 0x68, IND_ACC_ADDR = 0x80, IND_ACC_DATA = 0x84,   // PCI configuration space
    TWSI_CTRL = 0x0218, PCIE_PHYMISC = 0x1000, PCIE_PHYMISC2 = 0x1004, TWSI_DEBUG = 0x1108, DMA_DBG = 0x1114,
    OTP_CTRL = 0x12F0, PM_CTRL = 0x12F8, LTSSM_ID_CTRL = 0x12FC, MASTER = 0x1400, IRQ_MODRT = 0x1408,
    GPHY_CTRL = 0x140C, IDLE_STATUS = 0x1410, MDIO = 0x1414, SERDES = 0x1424, LPI_CTRL = 0x1440,
    MDIO_EXTN = 0x1448, MAC_CTRL = 0x1480, STAD0 = 0x1488, STAD1 = 0x148C, HASH0 = 0x1490, HASH1 = 0x1494,
    MTU = 0x149C, WOL = 0x14A0, SRAM_RFD0_INFO = 0x1500, SRAM_TRD_ADDR = 0x1518, SRAM_RXF_ADDR = 0x1520,
    SRAM_RXF_LEN = 0x1524, SRAM_TXF_ADDR = 0x1528, SRAM_TXF_LEN = 0x152C, LOAD_PTR = 0x1534,
    RX_BASE_HI = 0x1540, TX_BASE_HI = 0x1544, RFD0_ADDR = 0x1550, RFD_RING_SZ = 0x1560, RX_BUF_SZ = 0x1564,
    RRD0_ADDR = 0x1568, RRD_RING_SZ = 0x1578, TPD_PRI1_ADDR = 0x157C, TPD_PRI0_ADDR = 0x1580,
    TPD_RING_SZ = 0x1584, TXQ_CTRL = 0x1590, TSO_THRESH = 0x1594, TXF_WATER_MARK = 0x1598, RXQ_CTRL = 0x15A0,
    RXD_DMA_CTRL = 0x15AC, DMA_CTRL = 0x15C0, SMB_TIMER = 0x15C4, RFD0_PIDX = 0x15E0, TPD_PRI0_PIDX = 0x15F2,
    TPD_PRI0_CIDX = 0x15F6, ISR = 0x1600, IMR = 0x1604, INT_RETRIG = 0x1608, CLK_GATING = 0x1814,
    MT_MAGIC = 0x1F00, MT_MODE = 0x1F04, MT_SPEED = 0x1F08,
};
const uint32_t SOFT_RST = 1u << 0, WAKEN_25M = 1u << 5, OOB_DIS = 1u << 6, SA_TIMER_EN = 1u << 7,
               TX_ITIMER_EN = 1u << 10, RX_ITIMER_EN = 1u << 11, CLK_SEL_DIS = 1u << 12, INT_RDCLR = 1u << 14,
               OTP_SEL = 1u << 31;                                       // MASTER
const uint32_t MC_TX_EN = 1u << 0, MC_RX_EN = 1u << 1, MC_TX_FLOW = 1u << 2, MC_RX_FLOW = 1u << 3,
               MC_DUPLX = 1u << 5, MC_ADD_CRC = 1u << 6, MC_PAD = 1u << 7, MC_RMV_VLAN = 1u << 14,
               MC_PROMIS = 1u << 15, MC_MC_ALL = 1u << 25, MC_BC_EN = 1u << 26, MC_SINGLE_PAUSE = 1u << 28,
               MC_HASH_CRC32 = 1u << 29, MC_SPEED_SW = 1u << 30;        // MAC_CTRL

const char* kind_name(int t, bool mt)
{
    static const char* const n[] = { "AR8131", "AR8132", "AR8152 v1.1", "AR8152 v2.0", "AR8151", "AR8151 v2.0" };
    return mt ? "Killer (AR8151)" : n[t];
}
bool l2cb(Card& c) { return c.x.type == L2C_B || c.x.type == L2C_B2; }
bool newer(Card& c) { return c.x.type == L2C_B || c.x.type == L1D || c.x.type == L2C_B2 || c.x.type == L1D_2; }

// ---- MDIO: the standard registers, the extended (MMD) ones, the debug port ----
bool mdio_idle(Card& c)
{
    for (int i = 0; i < 120; i++) { if (!(r32(c, MDIO) & ((1u << 27) | (1u << 23)))) return true; udelay(10); }
    return false;
}
uint32_t clk(Card& c)                           // the slow clock while hibernating (no link), L2C-B2/L1D-2 only
{
    return ((c.x.type == L2C_B2 || c.x.type == L1D_2) && !c.x.speed ? 7u : 0u) << 24;
}
int phy_core_rd(Card& c, bool ext, int dev, int reg)
{
    uint32_t v;
    if (ext) {
        w32(c, MDIO_EXTN, (uint32_t)dev << 16 | reg);
        v = (1u << 22) | clk(c) | (1u << 23) | (1u << 30) | (1u << 21);   // preamble, start, ext, read
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
    } else v = (1u << 22) | clk(c) | d | (uint32_t)reg << 16 | (1u << 23);
    w32(c, MDIO, v);
    mdio_idle(c);
}
int phy_rd(Card& c, int reg) { return phy_core_rd(c, false, 0, reg); }
void phy_wr(Card& c, int reg, uint16_t d) { phy_core_wr(c, false, 0, reg, d); }
void ext_wr(Card& c, int dev, int reg, uint16_t d) { phy_core_wr(c, true, dev, reg, d); }
int ext_rd(Card& c, int dev, int reg) { return phy_core_rd(c, true, dev, reg); }
int dbg_rd(Card& c, int reg) { phy_wr(c, 0x1D, (uint16_t)reg); return phy_rd(c, 0x1E); }
void dbg_wr(Card& c, int reg, uint16_t d) { phy_wr(c, 0x1D, (uint16_t)reg); phy_wr(c, 0x1E, d); }

bool match(uint32_t id, uint32_t)
{
    if ((id & 0xFFFF) != 0x1969) return false;
    uint16_t d = id >> 16;
    return d == 0x1063 || d == 0x1062 || d == 0x2060 || d == 0x2062 || d == 0x1073 || d == 0x1083;
}

// atl1c_patch_assign's table: boards that want the link threshold patch
bool lnkpatch(Card& c, uint8_t rev)
{
    static const struct { uint16_t did; uint8_t rev; uint16_t sv, sd; } plats[] = {
        { 0x2060, 0xC1, 0x1019, 0x8152 }, { 0x2060, 0xC1, 0x1019, 0x2060 }, { 0x2060, 0xC1, 0x1019, 0xE000 },
        { 0x2062, 0xC0, 0x1019, 0x8152 }, { 0x2062, 0xC0, 0x1019, 0x2062 }, { 0x2062, 0xC0, 0x1458, 0xE000 },
        { 0x2062, 0xC1, 0x1019, 0x8152 }, { 0x2062, 0xC1, 0x1019, 0x2062 }, { 0x2062, 0xC1, 0x1458, 0xE000 },
        { 0x2062, 0xC1, 0x1565, 0x2802 }, { 0x2062, 0xC1, 0x1565, 0x2801 }, { 0x1073, 0xC0, 0x1019, 0x8151 },
        { 0x1073, 0xC0, 0x1019, 0x1073 }, { 0x1073, 0xC0, 0x1458, 0xE000 }, { 0x1083, 0xC0, 0x1458, 0xE000 },
        { 0x1083, 0xC0, 0x1019, 0x8151 }, { 0x1083, 0xC0, 0x1019, 0x1083 }, { 0x1083, 0xC0, 0x1462, 0x7680 },
        { 0x1083, 0xC0, 0x1565, 0x2803 },
    };
    uint32_t sub = pci_read(c.pci, 0x2C);
    for (const auto& p : plats)
        if (p.did == c.device && p.rev == rev && p.sv == (sub & 0xFFFF) && p.sd == sub >> 16) return true;
    return false;
}

bool wait_idle(Card& c, uint32_t bits)          // atl1c_wait_until_idle
{
    for (int i = 0; i < 10; i++) { if (!(r32(c, IDLE_STATUS) & bits)) return true; udelay(1000); }
    return false;
}

void set_aspm(Card& c, int speed)               // atl1c_set_aspm (L0s and L1 allowed, as Linux)
{
    uint32_t p = r32(c, PM_CTRL);
    p &= ~((1u << 3) | (1u << 12) | (1u << 30));                // ASPM_L1, ASPM_L0S, MAC_ASPM_CHK
    bool fast = speed == 1000 || speed == 100;
    if (c.x.type == L2C_B2 || c.x.type == L1D_2) {
        p &= ~(1u << 19);                                       // TXL1_AFTER_L0S
        p = (p & ~(7u << 16)) | (fast ? 4u : 1u) << 16;         // L1 entry 16 us
    } else {
        uint32_t t = c.x.type == L2C_B ? 7 : 0xF;
        if (!fast) t = 1;
        p = (p & ~(0xFu << 16)) | t << 16;
    }
    if (speed) p |= (1u << 12) | (1u << 30);
    p |= (1u << 3) | (1u << 30);
    if (newer(c)) {
        p = (p & ~(0xFu << 20)) | 0xFu << 20;                   // PM request timer
        p |= (1u << 15) | (1u << 6) | (1u << 13);               // RCVR_WT, SERDES_PD_EX_L1, CLK_SWH_L1
        p &= ~((1u << 4) | (1u << 5) | (1u << 7) | (1u << 29) | (1u << 31));
        if (!speed || c.x.type == L2C_B) p &= ~(1u << 12);
    } else {                                    // L1C, L2C
        p &= ~(0xFu << 16);
        if (speed) {
            p |= (1u << 4) | (1u << 5) | (1u << 7);
            p &= ~((1u << 6) | (1u << 13) | (1u << 12) | (1u << 3));
        } else {
            p |= 1u << 13;
            p &= ~((1u << 4) | (1u << 5) | (1u << 7) | (1u << 12));
        }
    }
    w32(c, PM_CTRL, p);
}

void reset_pcie(Card& c)                        // atl1c_reset_pcie, atl1c_pcie_patch, atl1c_disable_l0s_l1
{
    w32(c, WOL, 0);
    w32(c, LTSSM_ID_CTRL, r32(c, LTSSM_ID_CTRL) & ~0x1000u);   // LTSSM_ID_EN_WRO
    uint32_t m = r32(c, MASTER) & ~CLK_SEL_DIS;
    w32(c, MASTER, m);
    if (c.x.type == L1C || c.x.type == L2C) w32(c, PCIE_PHYMISC, r32(c, PCIE_PHYMISC) | 4);   // FORCE_RCV_DET
    else if (!(m & WAKEN_25M)) w32(c, MASTER, m | WAKEN_25M);
    if (c.x.type == L2C_B && c.x.rev == 0xC0) {
        w32(c, PCIE_PHYMISC2, (r32(c, PCIE_PHYMISC2) & ~(0xFu << 16)) | 3u << 16 | 3u << 18);   // CDR BW, L0S threshold
        pci_write(c.pci, LINK_CTRL, pci_read(c.pci, LINK_CTRL) | 0x80);                         // EXT_SYNC
    }
    if (c.x.type == L2C_B || c.x.type == L1D) {
        w32(c, PM_CTRL, r32(c, PM_CTRL) | (1u << 28));         // L0S_BUFSRX_EN
        w32(c, DMA_DBG, r32(c, DMA_DBG) & ~1u);                 // VENDOR_MSG
    }
    // atl1c_disable_l0s_l1: atl1c_set_aspm(SPEED_0) with neither allowed
    uint32_t p = r32(c, PM_CTRL) & ~((1u << 3) | (1u << 12) | (1u << 30));
    if (c.x.type == L2C_B2 || c.x.type == L1D_2) p = (p & ~((1u << 19) | (7u << 16))) | 1u << 16;
    else p = (p & ~(0xFu << 16)) | 1u << 16;
    if (newer(c)) {
        p = (p & ~(0xFu << 20)) | 0xFu << 20;
        p |= (1u << 15) | (1u << 6) | (1u << 13);
        p &= ~((1u << 4) | (1u << 5) | (1u << 7) | (1u << 29) | (1u << 31) | (1u << 12));
    } else {
        p &= ~(0xFu << 16);
        p |= 1u << 13;
        p &= ~((1u << 4) | (1u << 5) | (1u << 7) | (1u << 12));
    }
    w32(c, PM_CTRL, p);
    udelay(5000);
}

bool stop_mac(Card& c)                          // atl1c_stop_mac
{
    w32(c, RXQ_CTRL, r32(c, RXQ_CTRL) & ~(1u << 31));
    w32(c, TXQ_CTRL, r32(c, TXQ_CTRL) & ~(1u << 5));
    wait_idle(c, (1u << 2) | (1u << 3));
    w32(c, MAC_CTRL, r32(c, MAC_CTRL) & ~(MC_TX_EN | MC_RX_EN));
    return wait_idle(c, 3);
}

bool reset_mac(Card& c)                         // atl1c_reset_mac
{
    stop_mac(c);
    uint32_t m = r32(c, MASTER) | OOB_DIS;
    w32(c, MASTER, m | SOFT_RST);
    r32(c, MASTER);
    udelay(10000);
    if (!wait_idle(c, 0xF)) return false;
    w32(c, MASTER, m);
    w32(c, MAC_CTRL, r32(c, MAC_CTRL) | MC_SPEED_SW);
    uint32_t s = r32(c, SERDES);
    if (c.x.type == L2C_B) w32(c, SERDES, s & ~((1u << 18) | (1u << 17)));
    else if (c.x.type == L2C_B2 || c.x.type == L1D_2) w32(c, SERDES, s | (1u << 18) | (1u << 17));
    return true;
}

void phy_reset(Card& c)                         // atl1c_phy_reset (hibernation allowed, as Linux)
{
    uint32_t v = r32(c, GPHY_CTRL);
    v &= ~(1u | (1u << 7) | (1u << 5) | (1u << 14) | (1u << 2) | (1u << 17) | (1u << 13));   // EXT_RESET, IDDQ, GATE_25M, PWDOWN_HW, CLS
    v |= (1u << 12) | (1u << 10) | (1u << 11);  // SEL_ANA_RST, HIB_EN, HIB_PULSE
    w32(c, GPHY_CTRL, v);
    r32(c, GPHY_CTRL);
    udelay(10);
    w32(c, GPHY_CTRL, v | 1);
    r32(c, GPHY_CTRL);
    udelay(800);
    if (c.x.type == L2C_B) dbg_wr(c, 0x0A, (uint16_t)(dbg_rd(c, 0x0A) & ~0x2000));   // CFGLPSPD_RSTCNT_CLK125SW
    if (l2cb(c)) {
        dbg_wr(c, 0x3E, (uint16_t)(dbg_rd(c, 0x3E) | 0x8000));                         // CABLE1TH_DET_EN
        dbg_wr(c, 0x3B, (uint16_t)(dbg_rd(c, 0x3B) & ~0x8));                           // VOLT_CTRL_SWLOWEST
    }
    dbg_wr(c, 0x29, c.x.type == L1D || c.x.type == L1D_2 ? 0x129D : 0x36DD);          // LEGCYPS
    dbg_wr(c, 0x04, 0x88BB);                                                           // SYSMODCTRL
    if (c.x.type == L1D || c.x.type == L1D_2 || c.x.type == L2C_B2) {
        w32(c, LPI_CTRL, r32(c, LPI_CTRL) & ~1u);   // no EEE
        ext_wr(c, 7, 0x3C, 0);
        ext_wr(c, 3, 0x8003, 0x4D19);
    }
    dbg_wr(c, 0x00, 0x02EF);
    dbg_wr(c, 0x05, 0x2C46);
    dbg_wr(c, 0x12, 0x4C04);
    dbg_wr(c, 0x36, 0xE12C | 0x80);
    phy_wr(c, 0x12, 0x0400 | 0x0800);           // PHY interrupt mask: link up/down
}

void phy_init(Card& c)                          // atl1c_phy_init: autonegotiate everything
{
    phy_wr(c, 4, 0x01 | 0x20 | 0x40 | 0x80 | 0x100 | 0x400 | 0x800);   // CSMA, 10/100 half+full, pause, asym
    phy_wr(c, 9, c.x.giga ? 0x300 : 0);         // 1000 half+full
    phy_wr(c, 0, 0x8000 | 0x1000 | 0x0200);     // reset, autonegotiate, restart
}

void post_phy_link(Card& c)                     // atl1c_post_phy_linkchg
{
    bool adj = newer(c) && c.x.lnk_patch;
    if (c.x.speed) {
        if (c.x.type == L1D_2) dbg_wr(c, 0x15, (ext_rd(c, 3, 0x8006) & 0x1FF) > 0x50 ? 0xB210 : 0x3220);
        if (adj && c.x.speed == 100) { dbg_wr(c, 0x18, 0x05EA); dbg_wr(c, 0x04, 0x4FBB); }
    } else if (adj) { dbg_wr(c, 0x04, 0x88BB); dbg_wr(c, 0x18, 0x02EA); }
}

void set_mode(Card& c, int m)
{
    w32(c, HASH0, m >= 4 ? 0xFFFFFFFF : 0);
    w32(c, HASH1, m >= 4 ? 0xFFFFFFFF : 0);
    c.x.mac_ctrl &= ~(MC_MC_ALL | MC_PROMIS | MC_BC_EN);
    if (m >= 3) c.x.mac_ctrl |= MC_BC_EN;
    if (m >= 4) c.x.mac_ctrl |= MC_MC_ALL;
    if (m >= 6) c.x.mac_ctrl |= MC_PROMIS;
    w32(c, MAC_CTRL, (r32(c, MAC_CTRL) & ~(MC_MC_ALL | MC_PROMIS | MC_BC_EN)) | (c.x.mac_ctrl & (MC_MC_ALL | MC_PROMIS | MC_BC_EN)));
}

void set_mac(Card& c, const uint8_t* a)          // as alx: STAD0 = bytes 2-5, STAD1 = bytes 0-1
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
bool get_mac(Card& c)                           // atl1c_get_permanent_address: the register, else load the EEPROM/OTP
{
    if (read_mac(c)) return true;
    uint32_t otp = r32(c, OTP_CTRL);
    bool old = c.x.type == L1C || c.x.type == L2C, raise = false;
    if ((r32(c, TWSI_DEBUG) & (1u << 29)) || (r32(c, MASTER) & OTP_SEL)) {
        if (old && !(otp & 2)) { otp |= 2; w32(c, OTP_CTRL, otp); udelay(1000); }
        if (l2cb(c)) {
            dbg_wr(c, 0x00, (uint16_t)(dbg_rd(c, 0x00) & ~0x80));
            dbg_wr(c, 0x3B, (uint16_t)(dbg_rd(c, 0x3B) | 0x8));
            udelay(20);
            raise = true;
        }
        w32(c, TWSI_CTRL, r32(c, TWSI_CTRL) | (1u << 11));   // SW_LDSTART
        int i = 0;
        for (; i < 100; i++) { udelay(10000); if (!(r32(c, TWSI_CTRL) & (1u << 11))) break; }
        if (i == 100) return false;
    }
    if (old) { w32(c, OTP_CTRL, otp & ~2u); udelay(1000); }
    if (raise) {
        dbg_wr(c, 0x00, (uint16_t)(dbg_rd(c, 0x00) | 0x80));
        dbg_wr(c, 0x3B, (uint16_t)(dbg_rd(c, 0x3B) & ~0x8));
        udelay(20);
    }
    return read_mac(c);
}

// atl1c_configure: the MAC (atl1c_configure_mac and what it calls), the rings
void configure(Card& c)
{
    if (c.x.type == MT) w32(c, MT_MODE, r32(c, MT_MODE) & ~1u);   // one queue
    set_mac(c, c.mac);
    uint32_t m = r32(c, MASTER) & ~(TX_ITIMER_EN | RX_ITIMER_EN | INT_RDCLR);
    w32(c, ISR, 0xFFFFFFFF);
    w32(c, WOL, 0);
    w32(c, CLK_GATING, 0);
    w32(c, INT_RETRIG, 50000);

    // atl1c_configure_des_ring
    c.x.rx_idx = c.x.rrd_idx = c.x.tx_prod = c.x.tx_cons = 0;
    memset(c.x.rrd, 0, NRX * 16);
    w32(c, TX_BASE_HI, 0);
    w32(c, TPD_PRI0_ADDR, (uint32_t)(uintptr_t)c.txd);
    w32(c, TPD_PRI1_ADDR, (uint32_t)(uintptr_t)c.txd);   // Linux always sets up two; the second stays empty
    w32(c, TPD_RING_SZ, NTX);
    w32(c, RX_BASE_HI, 0);
    w32(c, RFD0_ADDR, (uint32_t)(uintptr_t)c.rxd);
    w32(c, RFD_RING_SZ, NRX);
    w32(c, RX_BUF_SZ, RXBUF);
    w32(c, RRD0_ADDR, (uint32_t)(uintptr_t)c.x.rrd);
    w32(c, RRD_RING_SZ, NRX);
    if (c.x.type == L2C_B) {
        w32(c, SRAM_RXF_LEN, 0x02A0); w32(c, SRAM_TXF_LEN, 0x0100); w32(c, SRAM_RXF_ADDR, 0x029F0000);
        w32(c, SRAM_RFD0_INFO, 0x02BF02A0); w32(c, SRAM_TXF_ADDR, 0x03BF02C0); w32(c, SRAM_TRD_ADDR, 0x03DF03C0);
        w32(c, TXF_WATER_MARK, 0); w32(c, RXD_DMA_CTRL, 0);
    }
    w32(c, LOAD_PTR, 1);

    w32(c, IRQ_MODRT, 1000 | 200u << 16);       // tx, rx moderation timers (interrupts stay masked: polled)
    m |= TX_ITIMER_EN | RX_ITIMER_EN | SA_TIMER_EN;
    w32(c, MASTER, m);
    w32(c, SMB_TIMER, 200000);
    w32(c, MTU, 1500 + 14 + 4 + 4);

    // atl1c_configure_tx
    w32(c, TSO_THRESH, (7 * 1024) >> 3);
    uint32_t dmar = 3;                          // 1024-byte read requests, capped by the PCIe maximum
    int cap = pci_cap(c.pci, 0x10);
    if (cap) {
        uint32_t dc = pci_read(c.pci, cap + 8);
        uint32_t rq = (128u << ((dc >> 12) & 7)) >> 8;   // Linux: pcie_get_readrq() >> 8
        if (rq < dmar) dmar = rq;
        if (dmar < 2) { pci_write(c.pci, cap + 8, (dc & 0xFFFF & ~0x7000u) | 2u << 12); dmar = 2; }   // 512
    }
    uint32_t txq = 5 | (1u << 6) | (1u << 7) | (1u << 4);   // TPD burst, enhanced mode, LSO 802.3, IP options
    txq |= (l2cb(c) ? 0x40u : 0x200u) << 16;     // TXF burst
    w32(c, TXQ_CTRL, txq);

    // atl1c_configure_rx
    uint32_t rxq = 8u << 20;                    // RFD burst
    if (c.x.type != L1D_2 && (c.device & 1)) rxq |= 3;   // ASPM throughput limit 100M
    w32(c, RXQ_CTRL, rxq);

    // atl1c_configure_dma
    w32(c, DMA_CTRL, 4 | (1u << 10) | dmar << 4 | 4u << 16 | 15u << 11);

    set_mode(c, rx_mode);
    c.x.rfd_prod = NRX - 1;                     // all buffers but one posted
    w32(c, RFD0_PIDX, c.x.rfd_prod);
}

void start_mac(Card& c)                         // atl1c_start_mac
{
    w32(c, TXQ_CTRL, r32(c, TXQ_CTRL) | (1u << 5));
    w32(c, RXQ_CTRL, r32(c, RXQ_CTRL) | (1u << 31));
    uint32_t m = r32(c, MAC_CTRL) & ~(MC_MC_ALL | MC_PROMIS | MC_BC_EN | MC_RMV_VLAN);
    m |= MC_TX_EN | MC_TX_FLOW | MC_RX_EN | MC_RX_FLOW | MC_ADD_CRC | MC_PAD | MC_SINGLE_PAUSE | MC_HASH_CRC32;
    m |= c.x.mac_ctrl & (MC_MC_ALL | MC_PROMIS | MC_BC_EN);   // the receive mode (Linux: always broadcast)
    if (c.x.full) m |= MC_DUPLX; else m &= ~MC_DUPLX;
    m = (m & ~(3u << 20)) | (c.x.speed == 1000 ? 2u : 1u) << 20;
    m = (m & ~(0xFu << 10)) | 7u << 10;         // preamble
    w32(c, MAC_CTRL, m);
}

bool init(Card& c)
{
    pci_d0(c.pci);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x07) & ~0x400u);   // I/O, memory, bus master
    c.mm = map_bar(c.pci, 0x4000);
    if (!c.mm) return false;
    c.x.rev = (uint8_t)pci_read(c.pci, 0x08);
    switch (c.device) {
    case 0x1062: c.x.type = L2C; break;
    case 0x2060: c.x.type = L2C_B; break;
    case 0x2062: c.x.type = L2C_B2; break;
    case 0x1073: c.x.type = L1D; break;
    case 0x1083: c.x.type = r32(c, MT_MAGIC) == 0xAABB1234 ? MT : L1D_2; break;
    default: c.x.type = L1C;
    }
    bool mt = c.x.type == MT;
    snprintf(c.name, sizeof c.name, "%s", kind_name(mt ? (int)L1D_2 : (int)c.x.type, mt));
    c.x.giga = c.x.type == L1C || c.x.type == L1D || c.x.type == L1D_2;
    c.x.lnk_patch = lnkpatch(c, c.x.rev);
    c.x.speed = 0;
    c.x.mac_ctrl = 0;
    w32(c, IMR, 0);                             // atl1c_irq_disable
    w32(c, ISR, 1u << 31);
    if (c.device == 0x2062 && c.x.rev == 0xC1) {   // atl1c_patch_assign: L2C-B2 v2.1
        pci_write(c.pci, IND_ACC_ADDR, 0x21C);
        uint32_t misc = pci_read(c.pci, IND_ACC_DATA) & ~0x100u;
        pci_write(c.pci, IND_ACC_ADDR, 0x21C);
        pci_write(c.pci, IND_ACC_DATA, misc);
    }

    reset_pcie(c);
    phy_reset(c);
    if (!reset_mac(c)) { kprintf("net: %s: MAC reset failed\n", c.name); return false; }
    if (!mt) phy_init(c);
    dbg(1, "net: %s rev %02X: PHY reset, negotiating\n", c.name, c.x.rev);
    if (!get_mac(c)) kprintf("net: %s: no valid address\n", c.name);

    c.rxd = (uint8_t*)phys_alloc(NRX * 8);
    c.x.rrd = (uint8_t*)phys_alloc(NRX * 16);
    c.txd = (uint8_t*)phys_alloc(NTX * 16);
    c.rxb = (uint8_t*)phys_alloc(NRX * BUF);
    c.txb = (uint8_t*)phys_alloc(NTX * BUF);
    volatile uint32_t* rfd = (volatile uint32_t*)c.rxd;
    for (int i = 0; i < NRX; i++) { rfd[2 * i] = (uint32_t)(uintptr_t)(c.rxb + i * BUF); rfd[2 * i + 1] = 0; }
    memset(c.txd, 0, NTX * 16);
    configure(c);
    return true;
}

// atl1c_check_link_status
bool link(Card& c)
{
    int speed = 0;
    bool full = true;
    if (c.x.type == MT) speed = (int)r32(c, MT_SPEED);
    else {
        phy_rd(c, 0x13);                        // clear the PHY's interrupt status
        phy_rd(c, 1);
        int bmsr = phy_rd(c, 1);
        if (bmsr >= 0 && (bmsr & 4)) {
            int p = phy_rd(c, 0x11);            // PHY specific status
            if (p < 0 || !(p & 0x0800)) return c.x.speed != 0;   // not resolved yet: as before
            switch (p & 0xC000) { case 0x8000: speed = 1000; break; case 0x4000: speed = 100; break; case 0: speed = 10; }
            full = (p & 0x2000) != 0;
        }
    }
    if (speed == c.x.speed && (!speed || full == c.x.full)) return speed != 0;
    if (speed) {
        c.x.speed = speed; c.x.full = full;
        dbg(1, "net: %s: %d Mbit/s, %s duplex\n", c.name, speed, full ? "full" : "half");
        set_aspm(c, speed);
        post_phy_link(c);
        start_mac(c);
    } else {                                    // down: the MAC reset, everything set up again
        c.x.speed = 0;
        reset_mac(c);
        set_aspm(c, 0);
        post_phy_link(c);
        configure(c);
    }
    return speed != 0;
}

void poll(Card& c)                              // atl1c_clean_rx_irq
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
        if ((w3 & ((1u << 20) | (1u << 30))) || len < 18) st.rx_err++;   // RX_ERR_SUM, 802.3 length error
        else enqueue(c.rxb + c.x.rx_idx * BUF, len - 4);   // less the FCS
        c.x.rx_idx = (uint16_t)((c.x.rx_idx + 1) % NRX);
        c.x.rrd_idx = (uint16_t)((c.x.rrd_idx + 1) % NRX);
        c.x.rfd_prod = (uint16_t)((c.x.rfd_prod + 1) % NRX);   // that buffer posted again
        n++;
    }
    if (n) { BARRIER(); w32(c, RFD0_PIDX, c.x.rfd_prod); }
}

bool send(Card& c, const uint8_t* f, unsigned len)   // atl1c_xmit_frame: one TPD, no offloads
{
    for (int i = 0; i < 2000; i++) {
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
} // namespace atl1c
} // namespace

const NicDriver net::nic_atl1c = {
    KIND_ATL1C, atl1c::match,
    atl1c::init, atl1c::link, atl1c::set_mode, atl1c::set_mac, atl1c::poll, atl1c::send, nullptr,
};
