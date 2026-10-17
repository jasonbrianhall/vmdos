// Broadcom Tigon3: BCM570x, 575x, 578x, 5906, 5717-5762 (Linux: tg3)
// What Linux's tg3 does to start one (tg3_get_invariants, tg3_chip_reset,
// tg3_reset_hw), for one transmit and one receive ring, polled, without
// TSO or jumbo frames, so no firmware download. The PHY is left as the
// boot code set it up, as for the Realtek cards: it is only woken if
// powered down, and the MAC is set to the speed and duplex it reports.
// Copper and MII-attached SerDes (5714S/5715S/5780S, 5717+ fiber); the
// 5700-5704's TBI fiber ports aren't driven. The APE (management
// processor of the 5717/5719/5720/5761/5762 family) is told a driver runs
// and gets its heartbeat; the PHY and the reset are taken under its locks.
#include "net.hpp"

using namespace net;

namespace {
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

uint32_t asic(Card& c) { return c.t.chiprev >> 12; }
bool has(Card& c, uint32_t f) { return (c.t.tflags & f) != 0; }

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
uint32_t ape_rd(Card& c, uint32_t r) { return *(volatile uint32_t*)(c.t.ape + r); }
void ape_wr(Card& c, uint32_t r, uint32_t v) { *(volatile uint32_t*)(c.t.ape + r) = v; }
// The driver's bit in a lock: its own for the PHY locks (0, 2, 3, 5), its
// PCI function's for GRC, MEM and GPIO.
uint32_t ape_bit(Card& c, int lock)
{
    if (lock == 0 || lock == 2 || lock == 3 || lock == 5) return APE_LOCK_DRIVER;
    return c.t.pci_fn ? 1u << c.t.pci_fn : APE_LOCK_DRIVER;
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
    ape_wr(c, APE_HOST_HB_COUNT, c.t.ape_hb++);
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
    ape_lock(c, c.t.ape_phy_lock);
    twf(c, MAC_MI_COM, (uint32_t)c.t.phy_addr << 21 | (uint32_t)reg << 16 | 0x08000000u | 0x20000000u);
    int v = -1;
    for (int i = 0; i < 5000; i++) {
        udelay(10);
        uint32_t f = tr(c, MAC_MI_COM);
        if (!(f & 0x20000000u)) { udelay(5); v = tr(c, MAC_MI_COM) & 0xFFFF; break; }
    }
    ape_unlock(c, c.t.ape_phy_lock);
    return v;
}
void phy_wr(Card& c, int reg, uint16_t val)
{
    ape_lock(c, c.t.ape_phy_lock);
    twf(c, MAC_MI_COM, (uint32_t)c.t.phy_addr << 21 | (uint32_t)reg << 16 | val | 0x04000000u | 0x20000000u);
    for (int i = 0; i < 5000; i++) { udelay(10); if (!(tr(c, MAC_MI_COM) & 0x20000000u)) break; }
    ape_unlock(c, c.t.ape_phy_lock);
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
    if (speed == c.t.speed && full == c.t.full) return;
    c.t.speed = speed; c.t.full = full;
    uint32_t m = c.t.mac_mode & ~0x0Eu;           // port mode, half duplex
    if (has(c, FTBI)) m |= 0x0C;
    else if (has(c, FMIISER)) m |= 0x08;
    else if (speed == 10 || speed == 100 || (!speed && has(c, FFET))) m |= 0x04;   // MII
    else m |= 0x08;                             // GMII
    if (speed && !full) m |= 0x02;
    c.t.mac_mode = m;
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
    c.t.tflags &= ~FFLUSH;                        // no read-back of the reset write
    if (asic(c) == A57780) tw(c, PCIE_LNKCTL, (tr(c, PCIE_LNKCTL) & ~0x08u) | 0x80);

    uint32_t val = 1;                           // CORECLK_RESET
    if (has(c, FPCIE)) {
        if (asic(c) != A5785 && !has(c, F57765P) && tr(c, PCIE_PHY_TSTCTL) == 0x60) tw(c, PCIE_PHY_TSTCTL, 0x20);
        if (c.t.chiprev != 0x4000) { tw(c, GRC_MISC_CFG, 1u << 29); val |= 1u << 29; }
    }
    if (asic(c) == A5906) {
        tw(c, VCPU_STATUS, tr(c, VCPU_STATUS) | 0x08000000u);            // DRV_RESET
        tw(c, GRC_VCPU_EXT_CTRL, tr(c, GRC_VCPU_EXT_CTRL) & ~0x00400000u);   // HALT_CPU
    }
    if (asic(c) == A5717) tw(c, CPMU_CLCK_ORIDE_ENABLE, tr(c, CPMU_CLCK_ORIDE_ENABLE) | (1u << 13));
    else if (asic(c) == A5719 || asic(c) == A5720) tw(c, CPMU_CLCK_ORIDE, 0x80000000u);
    if (has(c, F5705P) && !has(c, FCPMU)) val |= 0x04000000u;            // KEEP_GPHY_POWER
    tw(c, GRC_MISC_CFG, val);
    if (flush) c.t.tflags |= FFLUSH;
    udelay(120);
    pci_read(c.pci, 0x04);                      // the only read that works now
    udelay(120);

    if (has(c, FPCIE) && c.t.pcie_cap) {
        if (c.t.chiprev == 0x4000) {              // 5750 A0: link training
            udelay(500000);
            pci_write(c.pci, 0xC4, pci_read(c.pci, 0xC4) | (1u << 15));
        }
        uint32_t dc = pci_read(c.pci, c.t.pcie_cap + 8);   // device control / status
        uint16_t clear = 0x0010 | 0x0800;       // relaxed ordering, no snoop
        if (!has(c, FCPMU)) clear |= 0x00E0;    // max payload: 128
        dc = (dc & 0xFFFF & ~(uint32_t)clear) | 0x000F0000u;   // and clear the error status
        pci_write(c.pci, c.t.pcie_cap + 8, dc);
    }
    // tg3_restore_pci_state
    pci_write(c.pci, MISC_HOST_CTRL, c.t.misc_host);
    uint32_t ps = 0x20 | 0x40;                  // ROM_ENABLE, ROM_RETRY_ENABLE
    if (c.t.chiprev == 0x2000 && has(c, FPCIX)) ps |= 0x2000;   // RETRY_SAME_DMA
    if (has(c, FAPE)) ps |= 0x70000;
    pci_write(c.pci, PCISTATE, ps);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) & 0xFFFF0000u) | cmd);
    if (!has(c, FPCIE)) pci_write(c.pci, 0x0C, (pci_read(c.pci, 0x0C) & 0xFFFF0000u) | c.t.cls_lat);
    if (has(c, FPCIX) && c.t.pcix_cap) pci_write(c.pci, c.t.pcix_cap, pci_read(c.pci, c.t.pcix_cap) & ~(0x0002u << 16));   // ERO

    tw(c, MEMARB_MODE, (has(c, F5780C) ? tr(c, MEMARB_MODE) : 0) | 2);
    if (c.t.chiprev == 0x4003) tw(c, 0x5000, 0x400);   // 5750 A3

    // the boot code done (tg3_poll_fw)
    if (asic(c) == A5906) {
        int i = 0;
        for (; i < 200 && !(tr(c, VCPU_STATUS) & 0x04000000u); i++) udelay(100);
        if (i == 200) { kprintf("net: %s: no boot code (VCPU)\n", c.name); return false; }
    } else {
        int i = 0;
        for (; i < 100000 && mem_rd(c, SRAM_FW_MBOX) != ~FW_MAGIC; i++) udelay(10);
        if (i == 100000) kprintf("net: %s: no boot code answered (running anyway)\n", c.name);
        if (c.t.chiprev == 0x57785000) udelay(10000);
    }

    tw(c, GRC_MODE, c.t.grc_mode);
    if (c.t.chiprev == 0x3000) tw(c, 0xC4, tr(c, 0xC4) | (1u << 15));
    c.t.mac_mode = has(c, FTBI) ? 0x0Cu : has(c, FMIISER) ? 0x08u : 0;
    tw_wait(c, MAC_MODE, c.t.mac_mode, 40);
    ape_unlock(c, APE_LOCK_GRC);
    tw_wait(c, MAC_MI_MODE, c.t.mi_mode, 80);     // MDIO, no auto-polling
    if (has(c, FPCIE) && c.t.chiprev != 0x4000 && asic(c) != A5785 && !has(c, F57765P))
        tw(c, 0x7C00, tr(c, 0x7C00) | (1u << 25));
    if (asic(c) == A5717) tw(c, CPMU_CLCK_ORIDE_ENABLE, tr(c, CPMU_CLCK_ORIDE_ENABLE) & ~(1u << 13));
    else if (asic(c) == A5719 || asic(c) == A5720) tw(c, CPMU_CLCK_ORIDE, tr(c, CPMU_CLCK_ORIDE) & ~0x80000000u);
    if (asic(c) == A5762) tw(c, CPMU_CLCK_ORIDE_ENABLE, tr(c, CPMU_CLCK_ORIDE_ENABLE) | (1u << 13));

    c.t.tflags &= ~(FASF | FASFNEW);              // ASF as the boot code says now
    if (mem_rd(c, SRAM_DATA_SIG) == FW_MAGIC && (mem_rd(c, SRAM_DATA_CFG) & 0x80)) {
        c.t.tflags |= FASF;
        if (has(c, F5750P)) c.t.tflags |= FASFNEW;
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
    uint32_t rx = c.t.rx_mode_reg & ~0x100u;      // PROMISC
    for (int i = 0; i < 4; i++) tw(c, MAC_HASH0 + 4 * i, m >= 4 ? 0xFFFFFFFFu : 0);
    if (m >= 6) rx |= 0x100;
    if (m >= 2) rx |= 0x02; else rx &= ~0x02u;  // ENABLE
    c.t.rx_mode_reg = rx;
    tw_wait(c, MAC_RX_MODE, rx, 10);
}

void bdinfo(Card& c, uint32_t at, void* ring, uint32_t maxlen_flags, uint32_t nic)
{
    mem_wr(c, at + 0, 0);
    mem_wr(c, at + 4, (uint32_t)(uintptr_t)ring);
    mem_wr(c, at + 8, maxlen_flags);
    if (!has(c, F5705P)) mem_wr(c, at + 12, nic);
}

bool init(Card& c)
{
    pci_d0(c.pci);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x06) & ~0x410u);   // memory, bus master; no MWI, no INTx
    c.mm = map_bar_n(c.pci, 0, 0x10000);
    if (!c.mm) return false;

    // ---- what chip (tg3_detect_asic_rev) ----
    uint32_t misc = pci_read(c.pci, MISC_HOST_CTRL);
    c.t.chiprev = misc >> 16;
    c.t.tflags = 0;
    if (asic(c) == A_PRODID) {
        c.t.tflags |= FCPMU;
        uint16_t d = c.device;
        int reg = PRODID_ASICREV;
        if (d == 0x1655 || d == 0x1665 || d == 0x1656 || d == 0x1657 || d == 0x165F || d == 0x1683 ||
            d == 0x1642 || d == 0x1687 || d == 0x1643 || d == 0x16F3 || d == 0x1641) reg = GEN2_PRODID_ASICREV;
        else if (d == 0x16B1 || d == 0x16B5 || d == 0x16B0 || d == 0x16B4 || d == 0x16B2 || d == 0x16B6 ||
                 d == 0x1682 || d == 0x1686 || d == 0x16B7 || d == 0x16B3) reg = GEN15_PRODID_ASICREV;
        c.t.chiprev = pci_read(c.pci, reg);
    }
    if (c.t.chiprev == 0x5000) c.t.chiprev = 0x6000;            // 5752 A0
    if (c.t.chiprev == 0x05717200) c.t.chiprev = 0x05720000;    // 5717 C0 is a 5720 A0
    uint32_t a = asic(c);
    if (a == A5717 || a == A5719 || a == A5720) c.t.tflags |= F5717P;
    if (a == A57765 || a == A57766) c.t.tflags |= F57765C;
    if (has(c, F57765C) || has(c, F5717P) || a == A5762) c.t.tflags |= F57765P;
    if (a == A5755 || a == A5787 || a == A5784 || a == A5761 || a == A5785 || a == A57780 || has(c, F57765P))
        c.t.tflags |= F5755P;
    if (a == A5780 || a == A5714) c.t.tflags |= F5780C;
    if (a == A5750 || a == A5752 || a == A5906 || has(c, F5755P) || has(c, F5780C)) c.t.tflags |= F5750P;
    if (a == A5705 || has(c, F5750P)) c.t.tflags |= F5705P;
    if (a == A5717 || a == A5719 || a == A5720 || a == A5762) c.t.tflags |= FLRG;
    bool jumbo_bdflag = has(c, F57765P) && c.t.chiprev != 0x05719000;
    if (!has(c, F5705P) || has(c, F5780C) || jumbo_bdflag) c.t.tflags |= FJUMBO;
    if (a == A5906) c.t.tflags |= FFET;
    if (a != A5700 && a != A5701) c.t.tflags |= FNVRAM;      // (those two have a serial EEPROM)

    static const char* const names[] = { "5701", "5703", "5704", "5705", "5750", "?", "5752", "5700",
                                         "5780", "5714", "5755", "5787", "5906" };
    const char* nm = a < 13 ? names[a] : a == A57765 ? "57765" : a == A57766 ? "57766" : a == A57780 ? "57780" : 0;
    if (nm) snprintf(c.name, sizeof c.name, "BCM%s", nm);
    else snprintf(c.name, sizeof c.name, "BCM%x", a);

    c.t.misc_host = (misc & 0xFFFF0000u) | 0x02 | 0x08 | 0x80 | 0x10;   // MASK_PCI_INT, WORD_SWAP, INDIR_ACCESS, PCISTATE_RW
    pci_write(c.pci, MISC_HOST_CTRL, c.t.misc_host);

    // ---- the bus ----
    c.t.pcie_cap = (uint8_t)pci_cap(c.pci, 0x10);
    c.t.pcix_cap = 0;
    uint32_t ps = pci_read(c.pci, PCISTATE);
    if (c.t.pcie_cap) c.t.tflags |= FPCIE;
    else if (a == A5785) c.t.tflags |= FPCIE;
    else if (!has(c, F5705P) || has(c, F5780C)) {
        c.t.pcix_cap = (uint8_t)pci_cap(c.pci, 0x07);
        if (c.t.pcix_cap && !(ps & 0x04)) c.t.tflags |= FPCIX;          // not CONV_PCI_MODE
    }
    if (ps & 0x80) c.t.tflags |= FPCIHI;          // BUS_SPEED_HIGH
    c.t.cls_lat = (uint16_t)pci_read(c.pci, 0x0C);
    if (a == A5703 && (c.t.cls_lat >> 8) < 64) {
        c.t.cls_lat = (c.t.cls_lat & 0xFF) | 64 << 8;
        pci_write(c.pci, 0x0C, (pci_read(c.pci, 0x0C) & 0xFFFF0000u) | c.t.cls_lat);
    }
    if ((c.t.chiprev >> 8) == 0x71) {             // 5700 BX
        c.t.tflags |= FTXDBUG;
        if (has(c, FPCIX)) c.t.tflags |= FINDWR;
    }
    if (c.t.chiprev == 0x2000 && !(ps & 0x2000)) pci_write(c.pci, PCISTATE, ps | 0x2000);
    if (!has(c, FINDWR) && (a == A5701 || (has(c, FPCIE) && c.t.chiprev == 0x4000))) c.t.tflags |= FFLUSH;
    if (has(c, FINDWR) || (has(c, FPCIX) && (a == A5700 || a == A5701))) c.t.tflags |= FSRAMCFG;

    tw(c, MEMARB_MODE, tr(c, MEMARB_MODE) | 2);

    c.t.pci_fn = c.pci.fn & 3;
    if (a == A5704 || has(c, F5780C)) {
        if (has(c, FPCIX) && c.t.pcix_cap) c.t.pci_fn = pci_read(c.pci, c.t.pcix_cap + 4) & 7;
    } else if (has(c, F5717P)) {
        uint32_t v = mem_rd(c, SRAM_CPMU_STATUS);
        if ((v & 0xFFFF) != 0x362C) v = tr(c, CPMU_STATUS);
        c.t.pci_fn = a == A5717 ? ((v & 0x20000000u) ? 1 : 0) : (v & 0xC0000000u) >> 30;
    }

    // ---- what the boot code says (tg3_get_eeprom_hw_cfg) ----
    c.t.led = 0x800;                              // LED_CTRL_MODE_PHY_1
    if (a != A5906 && mem_rd(c, SRAM_DATA_SIG) == FW_MAGIC) {
        uint32_t cfg = mem_rd(c, SRAM_DATA_CFG), ver = mem_rd(c, SRAM_DATA_VER) >> 16, cfg2 = 0;
        if (a != A5700 && a != A5701 && a != A5703 && ver > 0 && ver < 0x100) cfg2 = mem_rd(c, SRAM_DATA_CFG_2);
        if ((cfg & 0x30) == 0x20) c.t.tflags |= has(c, F5705P) ? FMIISER : FTBI;   // fiber
        uint32_t led = has(c, F5750P) ? cfg2 & (0x0C | 0x18000) : cfg & 0x0C;
        switch (led) {
        case 0x08: c.t.led = 0x1000; break;       // PHY_2
        case 0x00: c.t.led = (a == A5700 || a == A5701) ? 0x800 : 0; break;   // MAC
        case 0x8000:                            // shared
            c.t.led = 0x4000;
            if (c.t.chiprev != 0x4000 && c.t.chiprev != 0x4001) c.t.led |= 0x1800;
            if (has(c, F5717P) || a == A5762) c.t.led |= 0x80000000u | 0x7FF80000u;
            break;
        case 0x10000: c.t.led = 0x2000; break;    // Shasta MAC
        case 0x18000: c.t.led = 0x8000 | (c.t.chiprev != 0x4000 ? 0x1800 : 0); break;   // combo
        default: c.t.led = 0x800;
        }
        if ((c.t.chiprev >> 8) == 0x57840) c.t.led = 0x800;
        if (cfg & 0x80) { c.t.tflags |= FASF; if (has(c, F5750P)) c.t.tflags |= FASFNEW; }
        if ((cfg & 0x00200000u) && has(c, F5750P)) c.t.tflags |= FAPE;
    }
    if (has(c, FAPE)) {
        c.t.ape = map_bar_n(c.pci, 2, 0x10000);
        if (!c.t.ape) c.t.tflags &= ~FAPE;
        else {
            pci_write(c.pci, PCISTATE, pci_read(c.pci, PCISTATE) | 0x70000);
            ape_lock_init(c);
        }
    }
    if (has(c, FTBI)) {
        kprintf("net: %s: a fiber (TBI) port, which vmdos doesn't drive\n", c.name);
        return false;
    }
    if (c.t.chiprev == 0x0000) kprintf("net: %s: a 5701 A0, which wants a firmware fix vmdos doesn't have\n", c.name);
    if (a == A5785 || a == A57780) dbg(1, "net: %s: external PHY, left as the boot code set it\n", c.name);

    c.t.mi_mode = has(c, FCPMU) ? 0x8000u : 0xC0000u;
    c.t.coalesce = ((c.t.chiprev >> 8) == 0x70 || (c.t.chiprev >> 8) == 0x71) ? 0 : 0x100;   // 32BYTE
    c.t.grc_mode = 0x20 | 0x10 | 0x04;           // WSWAP_DATA, BSWAP_DATA, WSWAP_NONFRM_DATA
    if (a == A5717 || a == A5762 || c.t.chiprev == 0x05719000 || c.t.chiprev == 0x05720000) {
        c.t.coalesce |= 0x04;                     // ATTN
        c.t.grc_mode |= 0x10000000u;              // IRQ_ON_FLOW_ATTN
    }
    c.t.ape_phy_lock = APE_LOCK_PHY0;
    if (has(c, F5717P)) {
        c.t.phy_addr = c.t.pci_fn + 1;
        bool ser = c.t.chiprev != 0x05717000 ? (tr(c, SG_DIG_STATUS) & 0x100) != 0 : (tr(c, CPMU_PHY_STRAP) & 0x20) != 0;
        if (ser) { c.t.phy_addr += 7; c.t.tflags |= FMIISER; }
    } else c.t.phy_addr = 1;
    static const uint8_t phy_locks[4] = { 0, 2, 3, 5 };     // PHY0..PHY3
    if (has(c, FAPE)) c.t.ape_phy_lock = phy_locks[c.t.pci_fn & 3];
    tw_wait(c, MAC_MI_MODE, c.t.mi_mode, 80);

    uint32_t g = tr(c, GRC_MODE);
    g &= (a == A5720 || a == A5762) ? (0x40u | 0x80u | 0x8000u | 0x40000u | 0x10000u) : 0x10000u;
    tw(c, GRC_MODE, g | c.t.grc_mode);

    // tg3_switch_clocks
    c.t.clock_ctrl = 0;
    if (!has(c, FCPMU) && !has(c, F5780C)) {
        uint32_t cc = tr(c, CLOCK_CTRL), keep = cc & (0x200000u | 0x400000u | 0x1F);
        c.t.clock_ctrl = keep;
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
        (c.t.chiprev == 0x0000 || c.t.chiprev == 0x0100 || c.t.chiprev == 0x0102 || c.t.chiprev == 0x0105)) {
        volatile uint32_t* s = (volatile uint32_t*)(c.mm + 0x8000 + SRAM_STATS_BLK);   // the 5701's PCI-X target bug
        s[0] = 0; s[1] = 0; s[1] = 0xFFFFFFFFu;
        if (s[0] != 0) c.t.tflags |= FINDWR | FSRAMCFG;
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
    c.t.dma_rwctrl = dma;
    tw(c, DMA_RW_CTRL, dma);

    // ---- rings ----
    c.t.std_size = has(c, FLRG) ? 2048 : 512;
    c.t.ret_size = has(c, FLRG) ? 4096 : (has(c, FJUMBO) && !has(c, F5780C)) ? 1024 : 512;
    c.rxd = (uint8_t*)phys_alloc(c.t.std_size * sizeof(BD));
    uint8_t* ret = (uint8_t*)phys_alloc(c.t.ret_size * sizeof(BD));
    c.txd = (uint8_t*)phys_alloc(TX_RING * sizeof(TD));
    c.rxb = (uint8_t*)phys_alloc(NRX * BUF);
    c.txb = (uint8_t*)phys_alloc(NTXB * BUF);
    c.t.stblk = (uint8_t*)phys_alloc(4096);       // status block, then (5700-5704) the statistics block
    c.t.retd = ret;
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
        if (c.t.chiprev == 0x57785000) {
            tw(c, GRC_MODE, (gm & ~0xA0400000u) | 0x00400000u);   // PL
            tw(c, PCIE_TLDLPL_PORT + 0x14, tr(c, PCIE_TLDLPL_PORT + 0x14) | 0x80000000u);
            tw(c, GRC_MODE, gm);
        }
        if ((c.t.chiprev >> 8) != 0x577850) {     // not 57765 AX: transmit hang fix
            tw(c, CPMU_PADRNG_CTL, tr(c, CPMU_PADRNG_CTL) | 0x40000);
            tw(c, GRC_MODE, (gm & ~0xA0400000u) | 0x20000000u);   // DL
            tw(c, PCIE_TLDLPL_PORT + 0x0C, (tr(c, PCIE_TLDLPL_PORT + 0x0C) & ~0xFFu) | 0x2C);
            tw(c, GRC_MODE, gm);
        }
        tw(c, CPMU_LSPD_10MB_CLK, (tr(c, CPMU_LSPD_10MB_CLK) & ~0x1F0000u) | 0x130000u);
    }
    if (c.t.chiprev == 0x05717000 && has(c, FPCIE) && c.t.pcie_cap && !(pci_read(c.pci, c.t.pcie_cap + 0x10) & 0x100)) {
        uint32_t gm = tr(c, GRC_MODE);           // L1PLLPD_EN (5717 A0 without CLKREQ)
        tw(c, GRC_MODE, (gm & ~0xA0400000u) | 0x00400000u);
        tw(c, PCIE_TLDLPL_PORT + 0x04, tr(c, PCIE_TLDLPL_PORT + 0x04) | 0x1000);
        tw(c, GRC_MODE, gm);
    }
    if (!has(c, FCPMU)) {
        if (!has(c, FPCIE)) c.t.clock_ctrl |= 0x80000000u;   // DELAY_PCI_GRANT
        twf(c, CLOCK_CTRL, c.t.clock_ctrl);
    }
    if (c.t.chiprev == 0x2000 && has(c, FPCIX)) tw(c, PCISTATE, tr(c, PCISTATE) | 0x2000);
    if (has(c, FAPE)) tw(c, PCISTATE, tr(c, PCISTATE) | 0x70000);
    if ((c.t.chiprev >> 8) == 0x21) tw(c, MSI_DATA, tr(c, MSI_DATA) | (1u << 26) | (1u << 28) | (1u << 29));   // 5704 BX

    memset(c.t.stblk, 0, 4096);
    if (has(c, F57765P)) {
        uint32_t v = tr(c, DMA_RW_CTRL) & ~1u;  // DIS_CACHE_ALIGNMENT
        if (c.t.chiprev == 0x57785000) v &= ~0x380u;
        if (!has(c, F57765C) && a != A5717 && a != A5762) v |= 0x80;   // TAGGED_STAT_WA
        tw(c, DMA_RW_CTRL, v | c.t.dma_rwctrl);
    } else if (a != A5784 && a != A5761) tw(c, DMA_RW_CTRL, c.t.dma_rwctrl);

    c.t.grc_mode = (c.t.grc_mode & ~(0x20000u | 0x20000000u | 0x100000u | 0x800000u)) | 0x20000u | 0x100000u;   // HOST_SENDBDS, NO_TX_PHDR_CSUM
    tw(c, GRC_MODE, c.t.grc_mode | 0x04000000u | 0x10000u);     // IRQ_ON_MAC_ATTN, HOST_STACKUP
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
    if (a == A5717 || a == A5762 || c.t.chiprev == 0x05719000 || c.t.chiprev == 0x05720000) bm |= 0x10;
    tw(c, BUFMGR_MODE, bm);
    {
        int i = 0;
        for (; i < 2000 && !(tr(c, BUFMGR_MODE) & 2); i++) udelay(10);
        if (i == 2000) { kprintf("net: %s: buffer manager didn't start\n", c.name); return false; }
    }
    if (c.t.chiprev == 0xC001) tw(c, ISO_PKT_TX, (tr(c, ISO_PKT_TX) & ~3u) | 2);

    // tg3_setup_rxbd_thresholds
    uint32_t bdcache = 128;
    if (has(c, F5750P) && !has(c, F5780C) && a != A5750 && a != A5752 && !has(c, F57765P))
        bdcache = (a == A5755 || a == A5787) ? 64 : 32;
    uint32_t max_post = (a == A5750 || a == A5752 || a == A5755) ? 8 : c.t.std_size;
    uint32_t nic_rep = bdcache / 2 < max_post ? bdcache / 2 : max_post, host_rep = NRX / 8 ? NRX / 8 : 1;
    tw(c, RCVBDI_STD_THRESH, nic_rep < host_rep ? nic_rep : host_rep);
    if (has(c, F57765P)) tw(c, STD_REPLENISH_LWM, bdcache);

    tw(c, RCVDBDI_STD_BD + 0, 0);
    tw(c, RCVDBDI_STD_BD + 4, (uint32_t)(uintptr_t)c.rxd);
    if (!has(c, F5717P)) tw(c, RCVDBDI_STD_BD + 12, SRAM_RX_BUFFER_DESC);
    if (!has(c, F5705P)) tw(c, RCVDBDI_MINI_BD + 8, 2);       // DISABLED
    uint32_t v;
    if (c.t.chiprev == 0x05719000 || (has(c, FJUMBO) && !has(c, F5780C))) {
        tw(c, RCVDBDI_JUMBO_BD + 8, 2);         // no jumbo ring
        v = has(c, F57765P) ? (uint32_t)c.t.std_size << 16 | RX_DMA << 2 : (uint32_t)RX_DMA << 16;
    } else v = 512u << 16;
    tw(c, RCVDBDI_STD_BD + 8, v);
    c.t.std_prod = NRX;
    mbox(c, MB_RCV_STD_PROD, c.t.std_prod);
    mbox(c, MB_RCV_JUMBO_PROD, 0);

    // tg3_rings_reset
    uint32_t lim = !has(c, F5705P) ? 16 : has(c, F5717P) ? 4 : (has(c, F57765C) || a == A5762) ? 2 : 1;
    for (uint32_t i = 1; i < lim; i++) mem_wr(c, SRAM_SEND_RCB + 16 * i + 8, 2);
    lim = has(c, F5717P) ? 17 : !has(c, F5705P) ? 16 : (a == A5755 || a == A5762 || has(c, F57765C)) ? 4 : 1;
    for (uint32_t i = 1; i < lim; i++) mem_wr(c, SRAM_RCV_RET_RCB + 16 * i + 8, 2);
    mbox(c, MB_INT0, 1);
    r32(c, a == A5906 ? MB_INT0 + GRCMBOX_BASE : MB_INT0);
    c.t.tx_prod = 0; c.t.tx_cons = 0; c.t.ret_cons = 0;
    tx_mbox(c, MB_SNDHOST_PROD0, 0);
    mbox(c, MB_RCVRET_CON0, 0);
    if (!has(c, F5705P)) for (int i = 0; i < 16; i++) tx_mbox(c, MB_SNDNIC_PROD0 + 8 * i, 0);
    tw(c, HOSTCC_STATUS_BLK_HOST + 0, 0);
    tw(c, HOSTCC_STATUS_BLK_HOST + 4, (uint32_t)(uintptr_t)c.t.stblk);
    bdinfo(c, SRAM_SEND_RCB, c.txd, (uint32_t)TX_RING << 16, SRAM_TX_BUFFER_DESC);
    bdinfo(c, SRAM_RCV_RET_RCB, ret, (uint32_t)c.t.ret_size << 16, 0);

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
    if (a == A5705 && c.t.chiprev != 0x3000 && !has(c, FPCIHI)) rdmac |= 0x30000;   // FIFO_LONG_BURST
    if (has(c, FPCIE)) rdmac |= 0x30000;
    if (a == A57766) rdmac |= 0x800000;         // JMB_2K_MMRR
    if (a == A5720 || a == A5762) rdmac |= tr(c, RDMAC_MODE) & 0x20000000u;
    if (a == A5761 || a == A5784 || a == A5785 || a == A57780 || has(c, F57765P)) {
        uint32_t reg = a == A5762 ? RDMA_RSRVCTRL2 : RDMA_RSRVCTRL, x = tr(c, reg);
        if (c.t.chiprev == 0x05719000 || a == A5762)
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
        tw(c, HOSTCC_STATS_BLK_HOST + 4, (uint32_t)(uintptr_t)(c.t.stblk + 0x100));   // 0x800 bytes after the status block
        tw(c, HOSTCC_STATS_BLK_NIC, SRAM_STATS_BLK);
        tw(c, HOSTCC_STATUS_BLK_NIC, SRAM_STATUS_BLK);
        for (uint32_t o = SRAM_STATS_BLK; o < SRAM_STATUS_BLK + 0x50; o += 4) { mem_wr(c, o, 0); udelay(40); }
    }
    tw(c, HOSTCC_MODE, 2 | c.t.coalesce);
    tw(c, RCVCC_MODE, 2 | 4);
    tw(c, RCVLPC_MODE, 2);
    if (!has(c, F5705P)) tw(c, RCVLSC_MODE, 2 | 4);
    if (has(c, FMIISER)) tw_wait(c, MAC_RX_MODE, 1, 10);   // RX_MODE_RESET

    c.t.mac_mode |= 0x4000 | 0x800 | 0x200000 | 0x400000 | 0x800000;   // TX/RX statistics, TDE, RDE, FHDE
    if (has(c, FAPE)) c.t.mac_mode |= 0x10000000u | 0x08000000u;
    if (!has(c, F5705P) && a != A5700) c.t.mac_mode |= 0x400;          // LINK_POLARITY
    tw_wait(c, MAC_MODE, c.t.mac_mode | 0x1000 | 0x8000, 40);         // and clear the statistics
    // GPIOs as the boot code set them (LOMs and NICs alike); interrupt on attention, auto SEEPROM
    tw_wait(c, GRC_LOCAL_CTRL, (tr(c, GRC_LOCAL_CTRL) & 0x0001FFF0u) | 0x08 | 0x01000000u, 100);
    if (!has(c, F5705P)) tw_wait(c, DMAC_MODE, 2, 40);

    v = 0x002 | 0x004 | 0x008 | 0x010 | 0x020 | 0x040 | 0x080 | 0x100 | 0x200;
    if (a == A5705 && c.t.chiprev != 0x3000 && !has(c, FPCIHI)) v |= 0x400;   // RX_ACCEL
    if (has(c, F5755P)) v |= 0x20000000u;       // STATUS_TAG_FIX
    if (a == A5785) v |= 0xC0000000u;
    tw_wait(c, WDMAC_MODE, v, 40);
    if (has(c, FPCIX) && c.t.pcix_cap && (a == A5703 || a == A5704)) {
        uint32_t x = pci_read(c.pci, c.t.pcix_cap);
        uint32_t cmdx = x >> 16;
        cmdx &= a == A5703 ? ~0x000Cu : ~(0x000Cu | 0x0070u);
        cmdx |= 0x0008;                         // max read 2K
        pci_write(c.pci, c.t.pcix_cap, (x & 0xFFFF) | cmdx << 16);
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

    c.t.rx_mode_reg = 2;                          // RX enable
    if (has(c, F5755P)) c.t.rx_mode_reg |= 0x01000000u;
    if (a == A5762) c.t.rx_mode_reg |= 0x02000000u;
    if (!has(c, FASF)) c.t.rx_mode_reg |= 0x400;  // KEEP_VLAN_TAG: frames as they came
    tw_wait(c, MAC_RX_MODE, c.t.rx_mode_reg, 10);
    tw(c, MAC_LED_CTRL, c.t.led);
    tw(c, MAC_MI_STAT, 1);                      // LNKSTAT_ATTN_ENAB
    tw_wait(c, MAC_RX_MODE, c.t.rx_mode_reg, 10);
    twf(c, MAC_LOW_WMARK, has(c, F57765C) ? 1 : 2);

    // the PHY: woken if the boot code left it powered down or isolated
    int bmcr = phy_rd(c, 0);
    if (bmcr >= 0 && (bmcr & 0x0C00)) {
        phy_wr(c, 0, (uint16_t)((bmcr & ~0x0C00) | 0x1200));   // autonegotiate, restart
        kprintf("net: %s: PHY was powered down, woken\n", c.name);
    }
    c.t.speed = -1;
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
    c.t.hb_tick = ticks;
    dbg(1, "net: %s: chip %x, %s%s%s, PHY %d, MAC_MODE %x\n", c.name, c.t.chiprev,
        has(c, FPCIE) ? "PCIe" : has(c, FPCIX) ? "PCI-X" : "PCI", has(c, FAPE) ? ", APE" : "",
        has(c, FASF) ? ", ASF" : "", c.t.phy_addr, c.t.mac_mode);
    return true;
}

bool link(Card& c)
{
    if (has(c, FAPE) && ticks - c.t.hb_tick >= 1000) {   // the APE's heartbeat
        c.t.hb_tick = ticks;
        ape_wr(c, APE_HOST_HB_COUNT, c.t.ape_hb++);
    }
    link_update(c);
    return c.t.speed > 0;
}

void poll(Card& c)
{
    volatile Status* s = (volatile Status*)c.t.stblk;
    volatile BD* ret = (volatile BD*)c.t.retd;
    volatile BD* std = (volatile BD*)c.rxd;
    uint16_t prod = s->idx[0].rx_prod;
    BARRIER();
    int n = 0;
    while (c.t.ret_cons != prod && room() && n++ < 64) {
        volatile BD& d = ret[c.t.ret_cons];
        uint32_t opaque = d.opaque, idx = opaque & 0xFFFF;
        unsigned len = (d.idx_len & 0xFFFF);
        bool bad = (d.err_vlan & 0x01EF0000u) || len < 18;      // RXD_ERR_MASK (an odd nibble is fine)
        if ((opaque & 0x00070000u) != 0x00010000u || idx >= (uint32_t)NRX) bad = true, idx = 0xFFFF;
        if (!bad) enqueue(c.rxb + idx * BUF, len - 4);   // less the FCS
        else st.rx_err++;
        if (idx != 0xFFFF) {                    // the buffer back on the standard ring
            volatile BD& p = std[c.t.std_prod % c.t.std_size];
            p.addr_hi = 0;
            p.addr_lo = (uint32_t)(uintptr_t)(c.rxb + idx * BUF);
            p.idx_len = RX_DMA;
            p.type_flags = 0x0004;
            p.opaque = 0x00010000u | idx;
            c.t.std_prod = (uint16_t)((c.t.std_prod + 1) % c.t.std_size);
        }
        c.t.ret_cons = (uint16_t)((c.t.ret_cons + 1) % c.t.ret_size);
    }
    if (n) {
        BARRIER();
        mbox(c, MB_RCVRET_CON0, c.t.ret_cons);
        mbox(c, MB_RCV_STD_PROD, c.t.std_prod);
    }
}

bool send(Card& c, const uint8_t* f, unsigned len)
{
    volatile Status* s = (volatile Status*)c.t.stblk;
    for (int i = 0; i < 2000; i++) {            // a free buffer: the card is less than NTXB behind
        c.t.tx_cons = s->idx[0].tx_cons;
        if ((uint16_t)((c.t.tx_prod - c.t.tx_cons) % TX_RING) < NTXB) break;
        udelay(5);
    }
    if ((uint16_t)((c.t.tx_prod - c.t.tx_cons) % TX_RING) >= NTXB) return false;
    uint8_t* b = c.txb + (c.t.tx_prod % NTXB) * BUF;
    memcpy(b, f, len);
    if (len < 60) { memset(b + len, 0, 60 - len); len = 60; }
    volatile TD& d = ((volatile TD*)c.txd)[c.t.tx_prod];
    d.addr_hi = 0;
    d.addr_lo = (uint32_t)(uintptr_t)b;
    d.len_flags = len << 16 | 0x0004;           // END
    d.vlan = 0;
    BARRIER();
    c.t.tx_prod = (uint16_t)((c.t.tx_prod + 1) % TX_RING);
    tx_mbox(c, MB_SNDHOST_PROD0, c.t.tx_prod);
    return true;
}
} // namespace tg3
} // namespace

const NicDriver net::nic_tg3 = {
    KIND_TG3, [](uint32_t id, uint32_t) { return tg3::match(id); },
    tg3::init, tg3::link, tg3::set_mode, tg3::set_mac, tg3::poll, tg3::send, nullptr,
};
