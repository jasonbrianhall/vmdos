// Intel I225/I226 (2.5 GbE) and I220/I221 (Linux: igc).
// One receive and one transmit queue (queue 0) with advanced descriptors,
// as Linux's igc sets them up (igc_reset_hw_base, igc_configure_rx/tx_ring),
// polled. The GPY PHY keeps the boot code's settings: woken if powered
// down, autonegotiation restarted if there's no link yet.
#include "net.hpp"

using namespace net;

namespace {
namespace igc {
const int NRX = 32, NTX = 16, BUF = 2048;
enum {
    CTRL = 0x0, STATUS = 0x8, EECD = 0x10, CTRL_EXT = 0x18, MDIC = 0x20, RCTL = 0x100, TCTL = 0x400,
    ICR = 0x1500, IMC = 0x150C, EIMC = 0x1528, MTA = 0x5200, RAL = 0x5400, RAH = 0x5404, UTA = 0xA000,
    SWSM = 0x5B50, SW_FW_SYNC = 0x5B5C,
    RDBAL = 0xC000, RDBAH = 0xC004, RDLEN = 0xC008, SRRCTL = 0xC00C, RDH = 0xC010, RDT = 0xC018, RXDCTL = 0xC028,
    TDBAL = 0xE000, TDBAH = 0xE004, TDLEN = 0xE008, TDH = 0xE010, TDT = 0xE018, TXDCTL = 0xE028,
};

struct Dev { uint16_t id; const char* name; };
const Dev devs[] = {
    { 0x15F2, "I225-LM" }, { 0x15F3, "I225-V" }, { 0x15F8, "I225-I" }, { 0x15F7, "I220-V" },
    { 0x3100, "I225-K" }, { 0x3101, "I225-K" }, { 0x3102, "I226-K" }, { 0x5502, "I225-LMvP" },
    { 0x5503, "I226-LMvP" }, { 0x0D9F, "I225-IT" }, { 0x125B, "I226-LM" }, { 0x125C, "I226-V" },
    { 0x125D, "I226-IT" }, { 0x125E, "I221-V" }, { 0x125F, "I226" }, { 0x15FD, "I225" },   // the last two: blank NVM
};

const Dev* dev_of(uint32_t id)
{
    if ((id & 0xFFFF) != 0x8086) return 0;
    for (const Dev& d : devs) if (d.id == id >> 16) return &d;
    return 0;
}

// The SW/FW semaphore around PHY access (igc_acquire_swfw_sync_i225, PHY0)
bool hw_sema(Card& c)
{
    int i = 0;
    for (; i < 2000 && (r32(c, SWSM) & 1); i++) udelay(500);  // SMBI
    if (i == 2000) w32(c, SWSM, r32(c, SWSM) & ~3u);          // stuck: cleared once
    for (i = 0; i < 2000; i++) {
        w32(c, SWSM, r32(c, SWSM) | 2);         // SWESMBI
        if (r32(c, SWSM) & 2) return true;
        udelay(50);
    }
    w32(c, SWSM, r32(c, SWSM) & ~3u);
    return false;
}
void hw_sema_put(Card& c) { w32(c, SWSM, r32(c, SWSM) & ~3u); }
bool phy_get(Card& c)
{
    for (int i = 0; i < 200; i++) {
        if (!hw_sema(c)) return false;
        uint32_t s = r32(c, SW_FW_SYNC);
        if (!(s & (2 | 2 << 16))) { w32(c, SW_FW_SYNC, s | 2); hw_sema_put(c); return true; }
        hw_sema_put(c);
        udelay(5000);
    }
    return false;
}
void phy_put(Card& c)
{
    for (int i = 0; i < 2000 && !hw_sema(c); i++) udelay(50);
    w32(c, SW_FW_SYNC, r32(c, SW_FW_SYNC) & ~2u);
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

bool match(uint32_t id, uint32_t) { return dev_of(id) != 0; }

void set_mode(Card& c, int m)
{
    uint32_t r = r32(c, RCTL) & ~((1u << 1) | (1u << 3) | (1u << 4) | (1u << 15) | (3u << 12) | (3u << 6) | (1u << 2));
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
    r32(c, STATUS);
    w32(c, RAH, a[4] | a[5] << 8 | 0x80000000u);   // valid
    r32(c, STATUS);
}

bool init(Card& c)
{
    const Dev* dv = dev_of((uint32_t)c.device << 16 | c.vendor);
    pci_d0(c.pci);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x06) & ~0x400u);   // memory, bus master
    c.mm = map_bar(c.pci, 0x20000);
    if (!c.mm) return false;
    snprintf(c.name, sizeof c.name, "%s", dv->name);
    if (r32(c, STATUS) == 0xFFFFFFFF) { kprintf("net: %s doesn't answer\n", c.name); return false; }
    c.g.phy_addr = 0;                           // as igc (its phy.addr is never set)

    // igc_reset_hw_base
    w32(c, CTRL, r32(c, CTRL) | 4);             // GIO master disable
    for (int i = 0; i < 800 && (r32(c, STATUS) & 0x80000); i++) udelay(100);
    w32(c, IMC, 0xFFFFFFFF);
    w32(c, RCTL, 0);
    w32(c, TCTL, 1u << 3);                      // PSP
    r32(c, STATUS);
    udelay(10000);
    w32(c, CTRL, r32(c, CTRL) | (1u << 26));    // RST
    r32(c, STATUS);
    udelay(1000);
    for (int i = 0; i < 10 && !(r32(c, EECD) & 0x200); i++) udelay(1000);   // NVM auto-read done
    w32(c, IMC, 0xFFFFFFFF);
    w32(c, EIMC, 0xFFFFFFFF);
    r32(c, ICR);

    w32(c, CTRL_EXT, r32(c, CTRL_EXT) | (1u << 28));   // DRV_LOAD: a driver has it
    uint32_t ctl = r32(c, CTRL);
    ctl |= 1u << 6;                             // SLU
    ctl &= ~((1u << 11) | (1u << 12));          // FRCSPD, FRCDPX
    w32(c, CTRL, ctl);
    int bmcr = phy_rd(c, 0);                    // the PHY woken if powered down
    if (bmcr >= 0 && (bmcr & 0x0C00)) {
        phy_wr(c, 0, (uint16_t)((bmcr & ~0x0C00) | 0x1200));
        kprintf("net: %s: PHY was powered down, woken\n", c.name);
    } else if (bmcr >= 0 && !(r32(c, STATUS) & 2)) {
        phy_wr(c, 0, (uint16_t)(bmcr | 0x1200));   // no link yet: negotiate
    }
    dbg(1, "net: %s PHY: BMCR %04x, STATUS %08x\n", c.name, bmcr & 0xFFFF, r32(c, STATUS));

    uint32_t lo = r32(c, RAL), hi = r32(c, RAH);
    for (int i = 0; i < 4; i++) c.mac[i] = (uint8_t)(lo >> (8 * i));
    c.mac[4] = (uint8_t)hi; c.mac[5] = (uint8_t)(hi >> 8);
    if (!(hi & 0x80000000u) || (c.mac[0] & 1)) kprintf("net: %s: no address from the NVM\n", c.name);
    set_mac(c, c.mac);
    for (int i = 0; i < 128; i++) w32(c, MTA + 4 * i, 0);
    for (int i = 0; i < 128; i++) w32(c, UTA + 4 * i, 0);

    // rings: queue 0, advanced descriptors
    c.rxd = (uint8_t*)phys_alloc(NRX * 16);
    c.txd = (uint8_t*)phys_alloc(NTX * 16);
    c.rxb = (uint8_t*)phys_alloc(NRX * BUF);
    c.txb = (uint8_t*)phys_alloc(NTX * BUF);
    volatile uint32_t* rx = (volatile uint32_t*)c.rxd;
    volatile uint32_t* tx = (volatile uint32_t*)c.txd;
    for (int i = 0; i < NRX; i++) { rx[4 * i] = (uint32_t)(uintptr_t)(c.rxb + i * BUF); rx[4 * i + 1] = rx[4 * i + 2] = rx[4 * i + 3] = 0; }
    for (int i = 0; i < NTX; i++) { tx[4 * i] = (uint32_t)(uintptr_t)(c.txb + i * BUF); tx[4 * i + 1] = tx[4 * i + 2] = 0; tx[4 * i + 3] = 1; }   // DD: free

    set_mode(c, 1);
    w32(c, RXDCTL, 0);
    w32(c, RDBAL, (uint32_t)(uintptr_t)c.rxd); w32(c, RDBAH, 0);
    w32(c, RDLEN, NRX * 16);
    w32(c, RDH, 0); w32(c, RDT, 0);
    uint32_t srr = r32(c, SRRCTL) & ~(0x7Fu | 0x3F00u | 0x0E000000u);
    w32(c, SRRCTL, srr | (256 >> 6) << 8 | (BUF >> 10) | 0x02000000u);   // header size, 2K buffers, advanced one-buffer
    w32(c, RXDCTL, 8 | 8u << 8 | 1u << 16 | 0x02000000u);   // write back each descriptor (nothing interrupt-driven)
    for (int i = 0; i < 100 && !(r32(c, RXDCTL) & 0x02000000u); i++) udelay(100);
    w32(c, RDT, NRX - 1);
    c.rx_cur = 0;

    w32(c, TXDCTL, 0);
    r32(c, STATUS);
    w32(c, TDLEN, NTX * 16);
    w32(c, TDBAL, (uint32_t)(uintptr_t)c.txd); w32(c, TDBAH, 0);
    w32(c, TDH, 0); w32(c, TDT, 0);
    w32(c, TXDCTL, 8 | 1u << 8 | 1u << 16 | 0x02000000u);
    for (int i = 0; i < 100 && !(r32(c, TXDCTL) & 0x02000000u); i++) udelay(100);
    c.tx_cur = 0;
    uint32_t tctl = r32(c, TCTL) & ~0xFF0u;
    tctl |= (1u << 3) | (1u << 24) | (15u << 4) | (1u << 1);   // PSP, RTLC, CT, EN
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
} // namespace igc
} // namespace

const NicDriver net::nic_igc = {
    KIND_IGC, igc::match,
    igc::init, igc::link, igc::set_mode, igc::set_mac, igc::poll, igc::send, nullptr,
};
