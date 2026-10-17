// Realtek RTL8111/8168, RTL8125 (Linux: r8169)
// The descriptor-ring design these share. vmdos does what Linux's r8169
// does to start one, less the per-chip PHY and PCIe tuning: it relies on
// the firmware (BIOS/UEFI) having powered and set up the PHY, as it
// normally has, and only wakes the PHY if it was left powered down.
#include "net.hpp"
#include "../third_party/rtl_nic/rtl8106e-1.h"

using namespace net;

namespace {
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
    if (c.r.ver == 28 || c.r.ver == 31) w32(c, 0xD0, take ? r32(c, 0xD0) & ~0x00020000u : r32(c, 0xD0) | 0x00020000u);
}

int phy_read(Card& c, int reg)
{
    if (c.r.ver == 28 || c.r.ver == 31) {
        if (reg == 3) return 0xC912;            // the chip reports a wrong PHY ID
        dp_mdio(c, true);
        w32(c, PHYAR, (uint32_t)(reg & 0x1F) << 16);
        int v = -1;
        for (int i = 0; i < 100; i++) { udelay(20); uint32_t x = r32(c, PHYAR); if (x & 0x80000000u) { udelay(20); v = x & 0xFFFF; break; } }
        dp_mdio(c, false);
        return v;
    }
    if (c.r.ocp_phy) {
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
    if (c.r.ver == 28 || c.r.ver == 31) {
        dp_mdio(c, true);
        w32(c, PHYAR, 0x80000000u | (uint32_t)(reg & 0x1F) << 16 | v);
        for (int i = 0; i < 100 && (r32(c, PHYAR) & 0x80000000u); i++) udelay(20);
        udelay(20);
        dp_mdio(c, false);
        return;
    }
    if (c.r.ocp_phy) {
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
    if (c.r.is8125) { w32(c, IntrMask8125, 0); w32(c, IntrStatus8125, 0xFFFFFFFF); }
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
    switch (c.r.ver) {
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
    c.r.xid = (txc >> 20) & 0xFCF;
    const Chip* chip = 0;
    for (const Chip& k : chips) if (k.xid == c.r.xid) { chip = &k; break; }
    if (!chip) for (const Chip& k : masked) if (k.xid == (c.r.xid & 0x7C8)) { chip = &k; break; }
    if (!chip) for (const Chip& k : masked8169) if (k.xid == (c.r.xid & 0xFC8)) { chip = &k; break; }
    static const Chip ext9151 = { 0x7C8, "RTL9151AS", 64 };
    if (!chip && (c.r.xid & 0x7CF) == 0x7C8 && !(r32(c, 0x60B0) & 0x7FFFFFFF)) chip = &ext9151;   // TX_CONFIG_V2
    if (!chip) {
        kprintf("net: Realtek %04x, chip XID %03x vmdos doesn't know\n", c.device, c.r.xid);
        return false;
    }
    snprintf(c.name, sizeof c.name, "%s", chip->name);
    if (c.device == 0x8136 && chip->ver == 42) snprintf(c.name, sizeof c.name, "RTL8106eus");   // gigabit MAC, 100M PHY
    if (c.device == 0x8136 && chip->ver == 46) snprintf(c.name, sizeof c.name, "RTL8107e");
    c.r.ver = chip->ver;
    c.r.is8125 = chip->ver >= 61;
    c.r.ocp_phy = chip->ver >= 40;
    c.r.gen_g = chip->ver >= 40 && chip->ver <= 52;
    c.r.evl_up = chip->ver >= 34 && chip->ver != 39 && chip->ver <= 52;
    dbg(1, "net: %s (XID %03x, r8169 version %u)\n", chip->name, c.r.xid, chip->ver);

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
    if (c.r.is8125) {
        w8(c, IntCfg0_8125, 0);
        int end = (chip->ver == 63 || chip->ver == 70) ? 0xA80 : 0xB00;
        for (int i = 0xA00; i < end; i += 4) w32(c, i, 0);   // interrupt coalescing off
        if (chip->ver == 63 || chip->ver == 70 || chip->ver == 80) w16(c, IntCfg1_8125, 0);
        if (chip->ver == 70 || chip->ver == 80) w8(c, 0xD8, r8(c, 0xD8) & ~0x02);
        w32(c, RSS_CTRL_8125, 0);
        w16(c, Q_NUM_CTRL_8125, 0);
        mac_ocp_modify(c, 0xEB58, 0x0001, 0);   // the old descriptor format
    } else {
        w8(c, MaxTxPacketSize, c.r.evl_up ? 0x27 : 0x3F);   // on the 8169: EarlyTxThres, no early transmit
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
    if (!c.r.is8125 && chip->ver >= 25 && chip->ver != 28 && chip->ver != 31 && chip->ver != 38)
        w8(c, 0x6F, r8(c, 0x6F) | 0xC0);           // PMCH: D3hot / D3cold no PLL down
    if (chip->ver >= 40) w32(c, MISC, r32(c, MISC) & ~(1u << 19));   // RXDV gate off
    w16(c, RxMaxSize, BUF);
    w32(c, TxDescHi, 0); w32(c, TxDescLo, (uint32_t)(uintptr_t)c.txd);
    w32(c, RxDescHi, 0); w32(c, RxDescLo, (uint32_t)(uintptr_t)c.rxd);
    lock(c);
    r8(c, ChipCmd);
    w8(c, ChipCmd, 0x0C);                       // TxEnb, RxEnb

    uint32_t rxc;
    if (c.r.is8125) rxc = (8u << 27) | (7u << 8) | (chip->ver >= 63 ? (1u << 11) : 0);
    else if (c.r.gen_g) rxc = (1u << 15) | (1u << 14) | (7u << 8) | (1u << 11);
    else if ((chip->ver >= 18 && chip->ver <= 24) || (chip->ver >= 34 && chip->ver <= 36) || chip->ver == 38)
        rxc = (1u << 15) | (1u << 14) | (7u << 8);   // RX128_INT_EN, RX_MULTI_EN
    else if (chip->ver <= 17 && (chip->ver < 7 || chip->ver > 9)) rxc = (7u << 13) | (7u << 8);   // FIFO threshold, DMA burst
    else rxc = (1u << 15) | (7u << 8);
    w32(c, RxConfig, rxc);                      // accepting nothing yet
    w32(c, TxConfig, (7u << 8) | (3u << 24) | (c.r.evl_up ? (1u << 7) : 0));
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
    if (c.r.is8125) { uint32_t s = r32(c, IntrStatus8125); if (s) w32(c, IntrStatus8125, s); }
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
    if (c.r.is8125) w16(c, TxPoll8125, 1);
    else w8(c, TxPoll, 0x40);                   // NPQ
    c.tx_cur = (c.tx_cur + 1) % NTX;
    return true;
}
} // namespace r8169
} // namespace

const NicDriver net::nic_r8169 = {
    KIND_R8169, r8169::match,
    r8169::init, r8169::link, r8169::set_mode, r8169::set_mac, r8169::poll, r8169::send, r8169::dump_phy,
};
