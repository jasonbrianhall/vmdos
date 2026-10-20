// What net.cpp (the receive queue, the card in use, the INT 2Fh interface)
// shares with the drivers, one per nic_*.cpp. A driver is a NicDriver: it
// claims PCI IDs, starts its card, polls it (from the timer tick) and sends
// for it; received frames go to enqueue().
#pragma once
#include "pci.hpp"
#include "hw.hpp"
#include "virtio.hpp"

namespace net {

// card kinds, as INT 2Fh AX=564Eh BL=0 (CH) and BL=6 report them
enum { KIND_NONE, KIND_E1000, KIND_RTL8139, KIND_R8169, KIND_USB, KIND_TG3, KIND_IGB, KIND_IGC, KIND_ALX, KIND_ATL1C, KIND_VIRTIO };
const int QLEN = 64, FRAME_MAX = 1536, MAXCARDS = 8;

struct Stats { uint32_t rx, tx, rx_bytes, tx_bytes, rx_err, tx_err, dropped; };
extern Stats st;
extern int rx_mode;                             // packet driver numbering: 1 off .. 6 everything
extern bool log_on;                             // netlog

#define BARRIER() __asm__ volatile("" ::: "memory")
inline void udelay(uint32_t us) { kdelay_us(us); }           // TSC-timed (works with interrupts off)

void enqueue(const uint8_t* f, unsigned len);   // a received frame (less its CRC)
bool room();                                    // room for another? (else leave them in the card's ring)

struct NicDriver;

struct Card {
    const NicDriver* drv;
    int kind;
    PciDevice pci;
    uint16_t vendor, device;
    bool running;
    uint8_t mac[6];                             // its own
    char name[20];
    volatile uint8_t* mm;                       // MMIO (BAR)
    uint16_t io;                                // I/O ports (RTL8139)
    uint8_t *rxd, *txd, *rxb, *txb;             // descriptor rings, buffers
    int rx_cur, tx_cur;
    uint32_t link_tick;                         // card_link: when the driver was last asked
    bool link_up, link_known;                   // and what it said
    union {                                     // each driver's own
        struct {                                // e1000: the ICH/PCH family (82566-82579, I217-I219): NVM in the
            bool pch;                           // chipset's flash, a PHY on its own chip, the ME sharing the MAC
            bool lv, was_up;                    // 82579 (K1 workaround on each link up); link at the last look
            uint32_t dbg_tick, dbg_n, sent;     // debug=1: the counters now and then
            uint32_t gprc, gptc, mpc, rnbc, crc;
        } e;
        struct { uint8_t type, port, phy_addr; } g;   // igb, igc
        struct { uint32_t rx_off; } r39;        // RTL8139
        struct {                                // r8169
            uint16_t xid;
            bool is8125, ocp_phy, gen_g;        // 8125 family; PHY over OCP (8168g+); 8168g..8117
            bool evl_up;
            uint8_t ver;                        // Linux's r8169 mac_version
        } r;
        struct {                                // tg3
            volatile uint8_t* ape;              // the APE's registers (BAR 2)
            uint32_t chiprev, tflags, misc_host, grc_mode, mac_mode, mi_mode, led, coalesce, clock_ctrl;
            uint32_t dma_rwctrl, rx_mode_reg, ape_hb, hb_tick;
            uint8_t pci_fn, phy_addr, ape_phy_lock, pcie_cap, pcix_cap;
            uint16_t cls_lat, std_prod, ret_cons, tx_prod, tx_cons;
            int std_size, ret_size, speed;
            bool full;
            uint8_t *stblk, *retd;              // status block, receive return ring
        } t;
        struct {                                // alx, atl1c (Atheros / Killer)
            uint32_t mac_ctrl;
            uint8_t rev, type;
            bool cr, giga, lnk_patch, full;
            int speed;                          // 0: no link
            uint16_t rfd_prod, rx_idx, rrd_idx, tx_prod, tx_cons;
            uint8_t* rrd;                       // receive return ring
            uint32_t dma_rd, dma_wr;            // atl1c: DMA burst sizes (as the register wants them)
        } x;
        struct {                                // virtio-net (QEMU): transport in virtio.cpp
            VirtioDev d;
            Virtq rxq, txq;                     // queue 0 receives, 1 sends
            bool status;                        // link status in the configuration
            uint16_t hdr;                       // the header before each frame
            uint32_t tx_busy;                   // transmit slots the device hasn't given back (bit n)
            uint8_t addr[6];                    // the address we answer to (we filter)
            int mode;
        } v;
        struct {                                // USB (CDC-ECM): its address can't be changed, so while
            bool spoof;                         // DOS's differs the adapter takes everything and vmdos filters
        } u;
    };
};

struct NicDriver {
    int kind;
    bool (*match)(uint32_t id, uint32_t sub);   // PCI vendor/device, subsystem (0: not a PCI card)
    bool (*init)(Card&);
    bool (*link)(Card&);
    void (*set_mode)(Card&, int m);             // 1: receiving nothing
    void (*set_mac)(Card&, const uint8_t* a);
    void (*poll)(Card&);                        // 0: frames come by themselves (USB)
    bool (*send)(Card&, const uint8_t* f, unsigned len);
    void (*debug)(Card&, const char* why);      // debug=1: what it says while there's no link (0: nothing)
};
extern const NicDriver nic_e1000, nic_igb, nic_igc, nic_rtl8139, nic_r8169, nic_tg3, nic_alx, nic_atl1c, nic_virtio, nic_usb;

inline uint32_t r32(Card& c, int r) { return *(volatile uint32_t*)(c.mm + r); }
inline uint16_t r16(Card& c, int r) { return *(volatile uint16_t*)(c.mm + r); }
inline uint8_t r8(Card& c, int r) { return c.mm[r]; }
inline void w32(Card& c, int r, uint32_t v) { *(volatile uint32_t*)(c.mm + r) = v; }
inline void w16(Card& c, int r, uint16_t v) { *(volatile uint16_t*)(c.mm + r) = v; }
inline void w8(Card& c, int r, uint8_t v) { c.mm[r] = v; }

int pci_d0(const PciDevice& d);                 // PCI power state D0; returns the state it was in
int pci_cap(const PciDevice& d, int id);        // a capability's offset (0: none)
volatile uint8_t* map_bar(const PciDevice& d, uint32_t len);          // the first memory BAR, mapped
volatile uint8_t* map_bar_n(const PciDevice& d, int b, uint32_t len); // BAR b

} // namespace net

// IPXNET (ipxnet.cpp): DOSBox's IPX over UDP, under DOS. net.cpp calls it
// with every frame from the wire and from DOS while it runs.
bool ipxnet_active();
bool ipxnet_rx(const uint8_t* f, unsigned len);    // true: it was IPXNET's, not for DOS
bool ipxnet_tx(const uint8_t* f, unsigned len);    // false: it needn't go on the wire
void ipxnet_tick();
void ipxnet_api(struct regs* r, int fn);           // INT 2Fh AX=564Eh BL=8, 9
// ... and these from net.cpp
const uint8_t* net_cur_mac();                       // the address DOS uses
bool net_wire_send(const uint8_t* f, unsigned len);
void net_dos_inject(const uint8_t* f, unsigned len);
void net_promisc(bool on);                          // the card takes everything (DOS still gets its own)
