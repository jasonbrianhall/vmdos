// Realtek RTL8139 (QEMU: -nic model=rtl8139).
#include "net.hpp"

using namespace net;

namespace {
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
    c.r39.rx_off = 0;
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
    c.r39.rx_off = 0;
    c.tx_cur = 0;
    snprintf(c.name, sizeof c.name, "RTL8139");
    return true;
}

bool link(Card& c) { return !(inb(c.io + MSR) & 4); }   // LINKB: 1 = link down

void poll(Card& c)
{
    for (int n = 0; n < 64 && room() && !(inb(c.io + CR) & 1); n++) {   // BUFE clear: a frame is there
        uint16_t status = *(volatile uint16_t*)(c.rxb + c.r39.rx_off);
        uint16_t len = *(volatile uint16_t*)(c.rxb + c.r39.rx_off + 2);
        if (status == 0 && len == 0xFFF0) break;          // early receive, not there yet
        if (!(status & 1) || len < 18 || len > 1518 + 4) { st.rx_err++; rx_reset(c); break; }
        enqueue(c.rxb + c.r39.rx_off + 4, len - 4);
        c.r39.rx_off = (c.r39.rx_off + len + 4 + 3) & ~3u;
        c.r39.rx_off %= RXLEN;
        outw(c.io + CAPR, (uint16_t)(c.r39.rx_off - 16));
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
} // namespace

const NicDriver net::nic_rtl8139 = {
    KIND_RTL8139, [](uint32_t id, uint32_t) { return id == 0x813910EC; },
    rtl::init, rtl::link, rtl::set_mode, rtl::set_mac, rtl::poll, rtl::send, nullptr,
};
