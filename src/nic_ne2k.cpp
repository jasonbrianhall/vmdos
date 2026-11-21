// PCI NE2000s: the Realtek RTL8029 (PCem's and 86Box's "Realtek RTL8029AS",
// QEMU's -nic model=ne2k_pci) and the other DP8390 clones Linux's ne2k-pci
// drives (Winbond 89C940, Compex, Holtek, VIA, Surecom, KTI, NetVin).
//
// No DMA: the 8390 keeps frames in the card's own 16 KB, and they come and go
// through the data port (remote DMA, 16-bit). Pages 40h-45h hold the frame
// being sent, 46h-7Fh are the receive ring. No link status: always up.
#include "net.hpp"

using namespace net;

namespace {
namespace ne {
enum { CR = 0x00, PSTART = 0x01, PSTOP = 0x02, BNRY = 0x03, TPSR = 0x04, TBCR0 = 0x05, TBCR1 = 0x06,
       ISR = 0x07, RSAR0 = 0x08, RSAR1 = 0x09, RBCR0 = 0x0A, RBCR1 = 0x0B, RCR = 0x0C, TCR = 0x0D,
       DCR = 0x0E, IMR = 0x0F, DATA = 0x10, RESET = 0x1F };
enum { PAR = 0x01, CURR = 0x07, MAR = 0x08 };                  // page 1
enum { CR_STP = 0x01, CR_STA = 0x02, CR_TXP = 0x04, CR_RREAD = 0x08, CR_RWRITE = 0x10, CR_NODMA = 0x20,
       CR_PAGE1 = 0x40 };
enum { ISR_OVW = 0x10, ISR_RDC = 0x40, ISR_RST = 0x80 };
const uint8_t TX_PAGE = 0x40, RX_START = 0x46, RX_STOP = 0x80;

uint8_t rcr_for(int m)
{
    if (m <= 1) return 0x20;                    // MON: checks frames, keeps none
    uint8_t r = 0;                              // its own address always
    if (m >= 3) r |= 0x04;                      // AB
    if (m >= 4) r |= 0x08;                      // AM
    if (m >= 6) r |= 0x10;                      // PRO
    return r;
}

void page(Card& c, int p) { outb(c.io + CR, (p ? CR_PAGE1 : 0) | CR_NODMA | CR_STA); }

void wait_rdc(Card& c)
{
    for (int i = 0; i < 2000 && !(inb(c.io + ISR) & ISR_RDC); i++) udelay(1);
    outb(c.io + ISR, ISR_RDC);
}

// Remote DMA: card memory at addr <-> buf, len bytes (rounded up to a word; buf has room).
void rd(Card& c, uint16_t addr, uint8_t* buf, unsigned len)
{
    unsigned n = (len + 1) & ~1u;
    outb(c.io + CR, CR_NODMA | CR_STA);
    outb(c.io + ISR, ISR_RDC);
    outb(c.io + RBCR0, n & 0xFF); outb(c.io + RBCR1, n >> 8);
    outb(c.io + RSAR0, addr & 0xFF); outb(c.io + RSAR1, addr >> 8);
    outb(c.io + CR, CR_RREAD | CR_STA);
    for (unsigned i = 0; i < n; i += 2) { uint16_t w = inw(c.io + DATA); buf[i] = (uint8_t)w; buf[i + 1] = w >> 8; }
    wait_rdc(c);
}

void wr(Card& c, uint16_t addr, const uint8_t* buf, unsigned len)
{
    unsigned n = (len + 1) & ~1u;
    outb(c.io + CR, CR_NODMA | CR_STA);
    outb(c.io + ISR, ISR_RDC);
    outb(c.io + RBCR0, n & 0xFF); outb(c.io + RBCR1, n >> 8);
    outb(c.io + RSAR0, addr & 0xFF); outb(c.io + RSAR1, addr >> 8);
    outb(c.io + CR, CR_RWRITE | CR_STA);
    for (unsigned i = 0; i < n; i += 2) outw(c.io + DATA, buf[i] | buf[i + 1] << 8);
    wait_rdc(c);
}

void set_mode(Card& c, int m)
{
    page(c, 1);
    for (int i = 0; i < 8; i++) outb(c.io + MAR + i, m >= 4 ? 0xFF : 0);
    page(c, 0);
    outb(c.io + RCR, rcr_for(m));
}

void set_mac(Card& c, const uint8_t* a)
{
    page(c, 1);
    for (int i = 0; i < 6; i++) outb(c.io + PAR + i, a[i]);
    page(c, 0);
}

// The receive ring, empty: BNRY one behind CURR. With the card stopped, as after an overflow.
void ring_reset(Card& c)
{
    outb(c.io + CR, CR_NODMA | CR_STP);
    for (int i = 0; i < 100 && !(inb(c.io + ISR) & ISR_RST); i++) udelay(20);   // the frame on the wire ends
    outb(c.io + RBCR0, 0); outb(c.io + RBCR1, 0);
    outb(c.io + TCR, 0x02);                     // loopback while it's stopped
    outb(c.io + PSTART, RX_START);
    outb(c.io + PSTOP, RX_STOP);
    outb(c.io + BNRY, RX_START);
    outb(c.io + CR, CR_PAGE1 | CR_NODMA | CR_STP);
    outb(c.io + CURR, RX_START + 1);
    outb(c.io + CR, CR_NODMA | CR_STA);
    outb(c.io + ISR, 0xFF);
    outb(c.io + TCR, 0);
    c.rx_cur = RX_START + 1;
}

bool init(Card& c)
{
    uint32_t bar = pci_read(c.pci, 0x10);
    if (!(bar & 1)) return false;
    c.io = (uint16_t)(bar & 0xFFFC);
    pci_d0(c.pci);
    pci_write(c.pci, 0x04, (pci_read(c.pci, 0x04) | 0x01) & ~0x400u);   // I/O; its interrupt stays off
    outb(c.io + RESET, inb(c.io + RESET));
    for (int i = 0; i < 1000 && !(inb(c.io + ISR) & ISR_RST); i++) udelay(10);
    outb(c.io + CR, CR_NODMA | CR_STP);
    if (inb(c.io + CR) == 0xFF) return false;  // nothing there
    outb(c.io + DCR, 0x49);                     // 16-bit, normal, FIFO 8 bytes
    outb(c.io + RBCR0, 0); outb(c.io + RBCR1, 0);
    outb(c.io + IMR, 0);
    outb(c.io + ISR, 0xFF);
    outb(c.io + RCR, 0x20);
    outb(c.io + TCR, 0x02);
    outb(c.io + CR, CR_NODMA | CR_STA);         // remote DMA needs it running
    uint8_t prom[32];
    rd(c, 0, prom, 32);
    bool doubled = true;                        // a 16-bit card's PROM: each byte twice
    for (int i = 0; i < 12; i += 2) if (prom[i] != prom[i + 1]) doubled = false;
    for (int i = 0; i < 6; i++) c.mac[i] = doubled ? prom[2 * i] : prom[i];
    outb(c.io + CR, CR_NODMA | CR_STP);
    outb(c.io + TPSR, TX_PAGE);
    set_mac(c, c.mac);
    ring_reset(c);
    set_mode(c, 1);
    snprintf(c.name, sizeof c.name, c.vendor == 0x10EC ? "RTL8029" : "NE2000 PCI");
    return true;
}

bool link(Card&) { return true; }               // an 8390 can't tell

uint8_t speed(Card&) { return link_info(10, -1); }   // 10BASE-T/2/5, whichever

void poll(Card& c)
{
    uint8_t isr = inb(c.io + ISR);
    if (isr & ISR_OVW) { st.rx_err++; ring_reset(c); return; }
    static uint8_t buf[FRAME_MAX + 2];
    for (int n = 0; n < 64 && room(); n++) {
        page(c, 1);
        uint8_t curr = inb(c.io + CURR);
        page(c, 0);
        if (c.rx_cur == curr) break;
        uint8_t h[4];
        rd(c, c.rx_cur << 8, h, 4);
        unsigned len = (h[2] | h[3] << 8) - 4, next = h[1];
        if (next < RX_START || next >= RX_STOP || len < 14 || len > FRAME_MAX) { st.rx_err++; ring_reset(c); return; }
        if (h[0] & 1) {                         // received intact
            unsigned at = (c.rx_cur << 8) + 4, first = (RX_STOP << 8) - at;
            if (len <= first) rd(c, at, buf, len);
            else {                              // runs past the end of the ring
                rd(c, at, buf, first);
                rd(c, RX_START << 8, buf + first, len - first);
            }
            enqueue(buf, len);
        }
        c.rx_cur = next;
        outb(c.io + BNRY, next == RX_START ? RX_STOP - 1 : next - 1);
    }
    isr = inb(c.io + ISR) & 0x3F;
    if (isr) outb(c.io + ISR, isr);
}

bool send(Card& c, const uint8_t* f, unsigned len)
{
    for (int i = 0; i < 2000 && (inb(c.io + CR) & CR_TXP); i++) udelay(5);   // the last one gone
    if (inb(c.io + CR) & CR_TXP) return false;
    static uint8_t b[FRAME_MAX + 2];
    memcpy(b, f, len);
    if (len < 60) { memset(b + len, 0, 60 - len); len = 60; }
    wr(c, TX_PAGE << 8, b, len);
    outb(c.io + TPSR, TX_PAGE);
    outb(c.io + TBCR0, len & 0xFF); outb(c.io + TBCR1, len >> 8);
    outb(c.io + CR, CR_NODMA | CR_TXP | CR_STA);
    return true;
}

bool match(uint32_t id, uint32_t)
{
    switch (id) {
    case 0x802910EC:                            // Realtek RTL8029
    case 0x09401050: case 0x5A5A1050:           // Winbond 89C940, 89C940F
    case 0x140111F6:                            // Compex RL2000
    case 0x005812C3: case 0x559812C3:           // Holtek HT80232, HT80229
    case 0x09261106:                            // VIA 82C926
    case 0x0E3410BD:                            // Surecom NE34
    case 0x30008E2E:                            // KTI ET32P2
    case 0x50004A14:                            // NetVin NV5000SC
        return true;
    }
    return false;
}
} // namespace ne
} // namespace

const NicDriver net::nic_ne2k = {
    KIND_NE2000, ne::match,
    ne::init, ne::link, ne::set_mode, ne::set_mac, ne::poll, ne::send, nullptr, ne::speed,
};
