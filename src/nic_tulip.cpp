// DEC (Digital) 21140 "Tulip" Fast Ethernet: Hyper-V Gen 1's "Legacy Network
// Adapter", and real PCI cards (DE500 and many others). Linux:
// drivers/net/ethernet/dec/tulip (tulip_core.c, eeprom.c, media.c, timer.c).
//
// The CSRs through the I/O BAR, 8 bytes apart. Two descriptor rings in
// ring mode (16-byte descriptors, the last with "end of ring"), polled.
// The station address comes from the serial SROM (93C46/66 on CSR9), at
// byte 20 in DEC's SROM format or 0 on early boards. The address filter
// is a 192-byte "setup frame" sent down the transmit ring: 16 perfect
// addresses, our own plus broadcast (the 21140 needs broadcast in it);
// multicast and promiscuous are CSR6 bits, and vmdos filters by receive
// mode itself as for virtio.
//
// Media: the SROM's 21140 media table (Linux's tulip_parse_eeprom): an
// MII PHY's leaf (its GPIO init and reset sequences on CSR12) or the
// non-MII leaves (a CSR12 GPIO value, CSR6 port bits, a link-beat GPIO
// bit), else an MII PHY if one answers, else 100BASE-TX. Hyper-V's
// emulation doesn't care; real cards do.
//
// tulip=21143 also drives a 21143 (1011:0019) this way: QEMU's -nic
// model=tulip, which is one. (A real 21143's SIA media isn't set up.)
#include "net.hpp"

using namespace net;

namespace {
namespace tl {

enum { CSR0 = 0x00, CSR1 = 0x08, CSR2 = 0x10, CSR3 = 0x18, CSR4 = 0x20, CSR5 = 0x28, CSR6 = 0x30, CSR7 = 0x38,
       CSR8 = 0x40, CSR9 = 0x48, CSR12 = 0x60 };
const uint32_t OWN = 0x80000000u, END_RING = 0x02000000u, FS_LS = 0x60000000u, SETUP = 0x08000000u;
const uint32_t RX_ON = 0x0002, TX_ON = 0x2000, PASS_MCAST = 0x0080, PROMISC = 0x0040, FULL_DUPLEX = 0x0200;
const int NRX = 32, NTX = 16, BUF = 1536;
const uint32_t CSR0_VAL = 0x01A08000;           // Linux's x86 setting: MRL, MRM, 32-longword cache alignment, burst 16 (as tulip_core's csr0)

inline uint32_t rd(Card& c, int r) { return inl((uint16_t)(c.io + r)); }
inline void wr(Card& c, int r, uint32_t v) { outl((uint16_t)(c.io + r), v); }

bool match(uint32_t id, uint32_t)
{
    if ((id & 0xFFFF) != 0x1011) return false;
    uint16_t d = id >> 16;
    return d == 0x0009 || (d == 0x0019 && strstr(cmdline, "tulip=21143"));
}

// ---- the serial SROM (eeprom.c tulip_read_eeprom) ----
enum { EE_SHIFT_CLK = 0x02, EE_CS = 0x01, EE_DATA_WRITE = 0x04, EE_DATA_READ = 0x08, EE_ENB = 0x4800 | EE_CS, EE_READ_CMD = 6 };

// The whole shifted-in value, as Linux returns it: the probe for the address
// size looks at a bit beyond the 16 data bits.
uint32_t ee_read(Card& c, int loc, int addr_len)
{
    if (loc > (1 << addr_len) - 1) return 0;
    uint32_t cmd = (uint32_t)loc | (EE_READ_CMD << addr_len), v = 0;
    wr(c, CSR9, EE_ENB & ~EE_CS);
    wr(c, CSR9, EE_ENB);
    for (int i = 4 + addr_len; i >= 0; i--) {
        uint32_t d = cmd & (1u << i) ? EE_DATA_WRITE : 0;
        wr(c, CSR9, EE_ENB | d); rd(c, CSR9); udelay(1);
        wr(c, CSR9, EE_ENB | d | EE_SHIFT_CLK); rd(c, CSR9); udelay(1);
        v = v << 1 | ((rd(c, CSR9) & EE_DATA_READ) ? 1 : 0);
    }
    wr(c, CSR9, EE_ENB); rd(c, CSR9); udelay(1);
    for (int i = 16; i > 0; i--) {
        wr(c, CSR9, EE_ENB | EE_SHIFT_CLK); rd(c, CSR9); udelay(1);
        v = v << 1 | ((rd(c, CSR9) & EE_DATA_READ) ? 1 : 0);
        wr(c, CSR9, EE_ENB); rd(c, CSR9); udelay(1);
    }
    wr(c, CSR9, EE_ENB & ~EE_CS);
    return v;
}

// ---- the MII PHY (media.c tulip_mdio_read / write) ----
enum { MDIO_SHIFT_CLK = 0x10000, MDIO_DATA_WRITE1 = 0x20000, MDIO_ENB_IN = 0x40000, MDIO_DATA_READ = 0x80000 };

uint16_t mdio_read(Card& c, int phy, int reg)
{
    uint32_t cmd = (0xF6u << 10) | ((uint32_t)(phy & 0x1F) << 5) | (uint32_t)reg, v = 0;
    for (int i = 32; i >= 0; i--) {             // 32 ones: sync
        wr(c, CSR9, MDIO_DATA_WRITE1); rd(c, CSR9);
        wr(c, CSR9, MDIO_DATA_WRITE1 | MDIO_SHIFT_CLK); rd(c, CSR9);
    }
    for (int i = 15; i >= 0; i--) {
        uint32_t d = cmd & (1u << i) ? MDIO_DATA_WRITE1 : 0;
        wr(c, CSR9, d); rd(c, CSR9);
        wr(c, CSR9, d | MDIO_SHIFT_CLK); rd(c, CSR9);
    }
    for (int i = 19; i > 0; i--) {
        wr(c, CSR9, MDIO_ENB_IN); rd(c, CSR9);
        v = v << 1 | ((rd(c, CSR9) & MDIO_DATA_READ) ? 1 : 0);
        wr(c, CSR9, MDIO_ENB_IN | MDIO_SHIFT_CLK); rd(c, CSR9);
    }
    return (uint16_t)(v >> 1);
}

// ---- CSR6 changes: the receiver and transmitter stopped around them (tulip_restart_rxtx) ----
void stop_rxtx(Card& c)
{
    uint32_t v = rd(c, CSR6);
    if (!(v & (RX_ON | TX_ON))) return;
    wr(c, CSR6, v & ~(RX_ON | TX_ON));
    for (int i = 0; i < 1300 && (rd(c, CSR5) & 0x007E0000); i++) udelay(10);   // CSR5 TS | RS: both stopped
}

uint32_t filter_bits(Card& c)
{
    int m = c.tu.mode;
    if (m >= 6 || memcmp(c.tu.addr, c.mac, 6) != 0) return PROMISC | PASS_MCAST;   // (another address: see set_mac)
    return m >= 4 ? PASS_MCAST : 0;
}

void apply_csr6(Card& c)
{
    stop_rxtx(c);
    wr(c, CSR6, c.tu.csr6 | (c.tu.fd ? FULL_DUPLEX : 0) | filter_bits(c) | RX_ON | TX_ON);
    wr(c, CSR2, 0);                             // receive poll demand
}

// The setup frame onto the transmit ring (set_rx_mode): perfect filtering,
// 16 addresses each as three 16-bit words in the low halves of 32-bit words.
bool send_setup(Card& c)
{
    uint8_t* f = c.tu.setup;
    memset(f, 0xFF, 192);                       // broadcast in every slot (the 21140 wants it there)
    for (int w = 0; w < 3; w++) {               // our address in the last
        f[180 + 4 * w] = c.tu.addr[2 * w];
        f[181 + 4 * w] = c.tu.addr[2 * w + 1];
    }
    for (int pass = 0; pass < 2; pass++) {      // a dummy first unless at entry 0 (Linux: a chip erratum)
        volatile uint32_t* d = (volatile uint32_t*)(c.txd + 16 * c.tx_cur);
        for (int t = 0; t < 10000 && (d[0] & OWN); t++) udelay(10);
        if (d[0] & OWN) return false;
        uint32_t ring = c.tx_cur == NTX - 1 ? END_RING : 0;
        bool dummy = pass == 0 && c.tx_cur != 0;
        d[1] = dummy ? ring : (SETUP | 192 | ring);
        d[2] = dummy ? 0 : (uint32_t)(uintptr_t)f;
        d[3] = 0;
        d[0] = OWN;
        c.tx_cur = (c.tx_cur + 1) % NTX;
        if (!dummy) break;
    }
    wr(c, CSR1, 0);                             // transmit poll demand
    return true;
}

// ---- media (eeprom.c tulip_parse_eeprom, media.c tulip_select_media for the 21140) ----
void media(Card& c, const uint8_t* ee, int ee_len)
{
    c.tu.mii = false;
    c.tu.fd = false;
    c.tu.sense = 0x7F;
    // MII PHY? (tulip_find_mii: 1..31, then 0)
    int phy = -1;
    for (int k = 1; k <= 32 && phy < 0; k++) {
        uint16_t bmsr = mdio_read(c, k & 31, 1);
        if (bmsr != 0xFFFF && bmsr != 0) phy = k & 31;
    }
    // the media table: 2-byte default media, CSR12 direction, count, leaves
    const uint8_t* leaf_mii = 0;
    const uint8_t* best = 0;
    int best_rank = -1;
    uint8_t dir = 0;
    bool newfmt = false;                        // old format (bytes 0-7 = 16-23): just an address
    for (int i = 0; i < 8; i++) if (ee[i] != ee[16 + i]) newfmt = true;
    if (newfmt && ee[27] && ee[27] + 4 < ee_len) {
        const uint8_t* p = ee + ee[27];
        uint16_t def = (uint16_t)(p[0] | p[1] << 8);
        p += 2;
        dir = *p++;
        int count = *p++;
        for (int i = 0; i < count && p + 4 <= ee + ee_len; i++) {
            int type, med;
            const uint8_t* data;
            if (!(p[0] & 0x80)) { type = 0; med = p[0] & 0x3F; data = p; p += 4; }   // compact
            else { type = p[1]; data = p + 2; med = type & 1 ? 11 : p[2] & 0x3F; p += (p[0] & 0x3F) + 1; }
            if (type == 1) { if (!leaf_mii) leaf_mii = data; continue; }
            if (type != 0) continue;
            // the default if the SROM names one, else 100TX-FD, 100TX, 10T-FD, 10T
            static const int8_t rank[16] = { 1, -1, -1, 3, 2, 4, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
            int r = !(def & 0x0800) && med == (def & 0x3F) ? 10 : med < 16 ? rank[med] : -1;
            if (r > best_rank) { best_rank = r; best = data; }
        }
    }
    if (leaf_mii || (!best && phy >= 0)) {      // MII: its GPIO reset and init sequences (type 1 leaf)
        c.tu.mii = true;
        c.tu.phy = (uint8_t)(phy >= 0 ? phy : leaf_mii[0]);
        if (leaf_mii) {
            int init_len = leaf_mii[1], reset_len = leaf_mii[2 + init_len];
            wr(c, CSR12, dir | 0x100);
            for (int i = 0; i < reset_len; i++) wr(c, CSR12, leaf_mii[3 + init_len + i]);
            udelay(500);
            for (int i = 0; i < init_len; i++) wr(c, CSR12, leaf_mii[2 + i]);
        }
        c.tu.csr6 = 0x020E0000;
    } else if (best) {                          // non-MII: GPIO value, port bits, link bit
        wr(c, CSR12, dir | 0x100);
        wr(c, CSR12, best[1]);
        c.tu.csr6 = 0x02000000 | ((uint32_t)(best[2] & 0x71) << 18);
        c.tu.fd = (best[0] & 0x3F) == 4 || (best[0] & 0x3F) == 5;
        if (!(best[3] & 0x80)) c.tu.sense = (int8_t)best[2];
    } else c.tu.csr6 = 0x03860000;              // nothing known: 100BASE-TX, symbol mode (Linux's default)
}

// MII: link, and the duplex autonegotiation settled on (tulip_check_duplex)
bool mii_link(Card& c)
{
    mdio_read(c, c.tu.phy, 1);                  // the link bit latches low: read twice
    uint16_t bmsr = mdio_read(c, c.tu.phy, 1);
    if (bmsr == 0xFFFF || !(bmsr & 4)) return false;
    uint16_t lpa = mdio_read(c, c.tu.phy, 5), adv = mdio_read(c, c.tu.phy, 4);
    uint16_t neg = lpa & adv;
    bool fd = (neg & 0x0100) || (neg & 0x01C0) == 0x0040;
    if (lpa != 0xFFFF && fd != c.tu.fd) { c.tu.fd = fd; apply_csr6(c); }
    return true;
}

bool init(Card& c)
{
    uint32_t bar = pci_read(c.pci, 0x10);
    if (!(bar & 1)) return false;
    c.io = (uint16_t)(bar & 0xFFFC);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x05) & ~0x400u);   // I/O, bus master, INTx on (we don't take it)
    pci_d0(c.pci);

    // The SROM: 64 or 256 words (the size from what an 8-bit address read gives back)
    static uint8_t ee[256];
    int abits = (ee_read(c, 0xFF, 8) & 0x40000) ? 8 : 6;   // 93C66 or 93C46 (Linux's test)
    int words = 1 << abits;
    if (words > 128) words = 128;
    for (int i = 0; i < words; i++) { uint16_t w = (uint16_t)ee_read(c, i, abits); ee[2 * i] = (uint8_t)w; ee[2 * i + 1] = (uint8_t)(w >> 8); }
    int sa = 0;
    for (int i = 0; i < 8; i++) if (ee[i] != ee[16 + i]) sa = 20;   // DEC's format; early boards: at 0
    if (ee[0] == 0xFF && ee[1] == 0xFF && ee[2] == 0) sa = 2;        // Matrox
    memcpy(c.mac, ee + sa, 6);
    if ((c.mac[0] == 0xA0 || c.mac[0] == 0xC0 || c.mac[0] == 0x02) && c.mac[1] == 0)   // Lite-On: byte-swapped
        for (int i = 0; i < 6; i += 2) { uint8_t t = c.mac[i]; c.mac[i] = c.mac[i + 1]; c.mac[i + 1] = t; }
    bool blank = true;
    for (int i = 0; i < 6; i++) if (c.mac[i] != 0xFF && c.mac[i] != 0) blank = false;
    if (blank) { kprintf("net: DEC 21140: no address in the SROM\n"); return false; }
    memcpy(c.tu.addr, c.mac, 6);
    c.tu.mode = 3;

    if (!c.rxd) {
        c.rxd = (uint8_t*)phys_alloc(16 * NRX + 16 * NTX);
        c.txd = c.rxd + 16 * NRX;
        c.rxb = (uint8_t*)phys_alloc(NRX * BUF);
        c.txb = (uint8_t*)phys_alloc(NTX * BUF);
        c.tu.setup = (uint8_t*)phys_alloc(192);
    }

    // Reset (tulip_up): bit 0 for at least 50 PCI cycles, then the bus mode
    wr(c, CSR6, 0);
    wr(c, CSR0, 1);
    pci_read(c.pci, 0x04);
    udelay(100);
    wr(c, CSR0, CSR0_VAL);
    pci_read(c.pci, 0x04);
    udelay(100);
    wr(c, CSR7, 0);                             // no interrupts: polled
    wr(c, CSR5, 0xFFFFFFFF);

    for (int i = 0; i < NRX; i++) {
        volatile uint32_t* d = (volatile uint32_t*)(c.rxd + 16 * i);
        d[1] = BUF | (i == NRX - 1 ? END_RING : 0);
        d[2] = (uint32_t)(uintptr_t)(c.rxb + i * BUF);
        d[3] = 0;
        d[0] = OWN;
    }
    for (int i = 0; i < NTX; i++) {
        volatile uint32_t* d = (volatile uint32_t*)(c.txd + 16 * i);
        d[0] = 0;
        d[1] = i == NTX - 1 ? END_RING : 0;
        d[2] = d[3] = 0;
    }
    c.rx_cur = c.tx_cur = 0;
    wr(c, CSR3, (uint32_t)(uintptr_t)c.rxd);
    wr(c, CSR4, (uint32_t)(uintptr_t)c.txd);

    media(c, ee, words * 2);
    if (c.device == 0x0019) c.tu.sense = 0x7F;  // a 21143's CSR12 is the SIA's: no GPIO link bit
    c.tu.last_rx = 0;
    if (!send_setup(c)) return false;
    wr(c, CSR6, c.tu.csr6 | (c.tu.fd ? FULL_DUPLEX : 0) | TX_ON);   // the setup frame goes first
    volatile uint32_t* sd = (volatile uint32_t*)(c.txd + 16 * ((c.tx_cur + NTX - 1) % NTX));
    for (int t = 0; t < 10000 && (sd[0] & OWN); t++) udelay(10);
    if (sd[0] & OWN) kprintf("net: DEC 21140: the setup frame wasn't taken\n");
    apply_csr6(c);
    snprintf(c.name, sizeof c.name, c.device == 0x0019 ? "DEC 21143" : "DEC 21140");
    if (log_on) kprintf("net: DEC 21140: SROM address at %d, %s, CSR6 %x\n", sa, c.tu.mii ? "MII" : "GPIO media", c.tu.csr6);
    return true;
}

bool link(Card& c)
{
    if (c.tu.mii) return mii_link(c);
    if (c.tu.sense == 0x7F) return true;        // no way to tell (boards with no link bit)
    if (c.tu.last_rx && ticks - c.tu.last_rx < 3000) return true;   // frames coming in: up (emulations that don't drive the bit)
    uint32_t csr12 = rd(c, CSR12);
    int8_t b = c.tu.sense;                      // timer.c: bit (b >> 1) & 7, set means up when b >= 0
    return (b < 0) != ((csr12 & (1u << ((b >> 1) & 7))) != 0);
}

void set_mode(Card& c, int m)
{
    uint32_t before = filter_bits(c);
    c.tu.mode = m;
    if (filter_bits(c) != before) apply_csr6(c);
}

// Hyper-V drops frames from other addresses unless its MAC address
// spoofing is on; a real card sends them. Either way the filter follows.
void set_mac(Card& c, const uint8_t* a)
{
    memcpy(c.tu.addr, a, 6);
    send_setup(c);
    apply_csr6(c);
}

bool wanted(Card& c, const uint8_t* f)
{
    int m = c.tu.mode;
    if (m <= 1) return false;
    if (m >= 6) return true;
    if (!(f[0] & 1)) return !memcmp(f, c.tu.addr, 6);
    bool bcast = (f[0] & f[1] & f[2] & f[3] & f[4] & f[5]) == 0xFF;
    return bcast ? m >= 3 : m >= 4;
}

void poll(Card& c)
{
    bool gave = false;
    for (int n = 0; n < NRX && room(); n++) {
        volatile uint32_t* d = (volatile uint32_t*)(c.rxd + 16 * c.rx_cur);
        uint32_t s = d[0];
        if (s & OWN) break;
        uint32_t len = (s >> 16) & 0x3FFF;
        c.tu.last_rx = ticks | 1;
        // a whole frame in one buffer (first and last), no error summary; less the CRC
        if ((s & 0x0300) != 0x0300 || (s & 0x8000) || len < 18 || len > BUF) st.rx_err++;
        else {
            const uint8_t* f = c.rxb + c.rx_cur * BUF;
            if (wanted(c, f)) enqueue(f, len - 4);
        }
        d[0] = OWN;
        gave = true;
        c.rx_cur = (c.rx_cur + 1) % NRX;
    }
    if (gave) wr(c, CSR2, 0);                   // receive poll demand (it may have run out)
    if (rd(c, CSR5) & 0x2000) {                 // fatal bus error: start over
        kprintf("net: DEC 21140: bus error, restarting\n");
        init(c);
    }
}

bool send(Card& c, const uint8_t* f, unsigned len)
{
    if (len > BUF) return false;
    volatile uint32_t* d = (volatile uint32_t*)(c.txd + 16 * c.tx_cur);
    for (int t = 0; t < 2000 && (d[0] & OWN); t++) udelay(5);
    if (d[0] & OWN) return false;
    uint8_t* b = c.txb + c.tx_cur * BUF;
    memcpy(b, f, len);
    if (len < 60) { memset(b + len, 0, 60 - len); len = 60; }
    d[1] = FS_LS | len | (c.tx_cur == NTX - 1 ? END_RING : 0);
    d[2] = (uint32_t)(uintptr_t)b;
    d[3] = 0;
    d[0] = OWN;
    c.tx_cur = (c.tx_cur + 1) % NTX;
    wr(c, CSR1, 0);                             // transmit poll demand
    return true;
}

} // namespace tl
} // namespace

const NicDriver net::nic_tulip = {
    KIND_TULIP, tl::match, tl::init, tl::link, tl::set_mode, tl::set_mac, tl::poll, tl::send, nullptr,
};
