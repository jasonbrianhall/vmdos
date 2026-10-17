// USB Ethernet adapters: usb.cpp speaks CDC-ECM to them; this is their card.
#include "net.hpp"

using namespace net;

extern "C" {
int usb_net_present(void);
const char* usb_net_name(void);
void usb_net_mac(uint8_t* m);
int usb_net_send(const uint8_t* f, unsigned len);
void usb_net_filter(int bits);
}

namespace {
namespace usbnet {
bool init(Card& c)
{
    if (!usb_net_present()) return false;
    usb_net_mac(c.mac);
    snprintf(c.name, sizeof c.name, "%s", usb_net_name());
    c.pci.bus = c.pci.dev = c.pci.fn = 0xFF;
    c.u.spoof = false;
    return true;
}
bool link(Card&) { return usb_net_present(); }
void set_mode(Card& c, int m)
{
    int bits = 0;                               // SET_ETHERNET_PACKET_FILTER
    if (m >= 2) bits |= 0x04;                   // directed
    if (m >= 3) bits |= 0x08;                   // broadcast
    if (m >= 4) bits |= 0x02;                   // all multicast
    if (m >= 6 || (c.u.spoof && m >= 2)) bits |= 0x01;   // promiscuous
    usb_net_filter(bits);
}
void set_mac(Card& c, const uint8_t* a)
{
    c.u.spoof = memcmp(a, c.mac, 6) != 0;
    set_mode(c, rx_mode);
}
bool send(Card&, const uint8_t* f, unsigned len) { return usb_net_send(f, len); }
} // namespace usbnet
} // namespace

const NicDriver net::nic_usb = {
    KIND_USB, nullptr,
    usbnet::init, usbnet::link, usbnet::set_mode, usbnet::set_mac, nullptr, usbnet::send, nullptr,
};
