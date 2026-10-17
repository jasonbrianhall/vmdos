// Intel 8254x/8257x "e1000" and e1000e, and the ICH/PCH MACs (82566-82579,
// I217-I219), with legacy descriptors.
#include "net.hpp"

using namespace net;

namespace {
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
    c.e.pch = pch != 0;
    if (c.e.pch) pci_d0(c.pci);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x06) & ~0x400u);   // memory, bus master
    c.mm = map_bar(c.pci, 0x20000);
    if (!c.mm) return false;
    if (pch) snprintf(c.name, sizeof c.name, "%s", pch->name);
    else snprintf(c.name, sizeof c.name, "Intel");

    w32(c, IMC, 0xFFFFFFFF);
    if (c.e.pch) {                                // as e1000e: stop DMA, take the flag, reset
        w32(c, CTRL, r32(c, CTRL) | (1u << 2));                 // GIO master disable
        for (int i = 0; i < 800 && (r32(c, STATUS) & (1u << 19)); i++) udelay(100);
        if (!swflag(c)) dbg(1, "net: %s: no software flag, resetting anyway\n", c.name);
    }
    w32(c, CTRL, r32(c, CTRL) | (1u << 26));    // RST (frees the flag)
    udelay(20000);
    for (int i = 0; i < 1000 && (r32(c, CTRL) & (1u << 26)); i++) udelay(100);
    if (c.e.pch) {                                // the NVM's settings loaded
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

    if (c.e.pch) pch_phy(c);

    uint32_t lo = r32(c, RAL), hi = r32(c, RAH);
    if (!c.e.pch && (!(hi & 0x80000000u) || (!lo && !(hi & 0xFFFF)))) {
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
    if (c.e.pch && !(hi & 0x80000000u)) kprintf("net: %s: no address from the NVM\n", c.name);
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
} // namespace

const NicDriver net::nic_e1000 = {
    KIND_E1000, [](uint32_t id, uint32_t) { return e1k::match(id); },
    e1k::init, e1k::link, e1k::set_mode, e1k::set_mac, e1k::poll, e1k::send, nullptr,
};
