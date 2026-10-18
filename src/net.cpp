// Network: the PC's own Ethernet cards, for DOS's packet driver (VMPKT.COM).
//
// vmdos drives the cards itself, polled from the timer tick (their own
// interrupts stay masked). Every card it can drive is started at boot; one
// of them is in use at a time. Received frames wait in a queue here; while
// it holds any, the IRQ VMPKT asked for is raised in the virtual PIC, and
// VMPKT's handler takes them out one at a time (INT 2Fh AX=564Eh BL=3) and
// hands them to its clients. Sending is one call (BL=2), straight to the
// card. DOS never sees the cards themselves (VMHV's PCI reads FFh anyway).
//
// Which card: nic=N on the kernel command line (1 = the first in PCI
// order), or VMNIC.COM later. Otherwise "auto": the first card, and while
// it has no link another one that has. Once DOS has asked for the card's
// address (VMPKT does, as it loads), that address stays: a card switched to
// later is given it, so VMPKT, VMIPX and their clients carry on unchanged.
//
// Cards: Intel 8254x/8257x/I21x "e1000" (QEMU's default, -nic model=e1000
// or e1000e), Intel 82575/82576/82580/I350/I354/I210/I211 "igb" (-nic model=igb),
// Intel I225/I226 "igc" (2.5G), Realtek RTL8139 (-nic model=rtl8139), Realtek RTL8111/8168,
// RTL8125 (2.5G), RTL8126 (5G), RTL8127 (10G), RTL810x (10/100) and the PCI
// RTL8169/8110 (every chip Linux's r8169 drives) on real PCs, Broadcom Tigon3
// (BCM570x/575x/578x/5906/5717-5762 NetXtreme and NetLink, Linux's tg3), Atheros/Killer AR8161/8162/8171/8172
// and E2200/E2400/E2500 (alx), Atheros AR8131/8132/8151/8152 (atl1c), virtio-net (QEMU,
// -nic model=virtio-net-pci). Kernel options: net=off (none), nic=N,
// netlog (every call and frame in the log).
//
// INT 2Fh AX=564Eh ('N'), BL = function; BX = 564Dh ('VM') on return:
//   0  query: AX = 1 card / 0 none, CL = link up, CH = card kind
//      (1 e1000, 2 RTL8139, 3 RTL8168/8125, 4 USB, 5 Broadcom, 6 igb, 7 igc, 8 alx, 9 atl1c, 10 virtio), DX = frames waiting,
//      ES:DI <- the MAC (6 bytes). From then on that MAC stays.
//   1  CL = IRQ to raise while frames wait (FFh: none)
//   2  send DS:SI, CX bytes (14..1514): AX = 0 sent, 1 failed
//   3  receive: the next frame into ES:DI (CX = room), CX = its length
//      (0: none waiting; a frame longer than the room is cut), DX = still waiting
//   4  receive mode CL (packet driver numbering: 1 off, 2 own address,
//      3 + broadcast, 4/5 + multicast, 6 everything): AX = 0, 1 not supported
//   5  statistics into ES:DI: 7 dwords: frames in, frames out, bytes in,
//      bytes out, receive errors, send errors, frames dropped (queue full)
//   6  the cards into ES:DI, 32 bytes each (at most 8): AX = how many,
//      CX = the one in use (0-based, FFFFh none), DX bit 0 = auto.
//      Each: kind, state (0 off/failed, 1 running), link, bus, device,
//      function, vendor (word), device ID (word), its own MAC (6), the
//      address it sends with (6), name (10 bytes at 22, NUL-terminated).
//   7  use card CL (0-based; FFh: auto): AX = 0, 1 no such card, 2 it didn't start
#include "net.hpp"

extern "C" {
int usb_net_present(void);
uint32_t usb_net_gen(void);
}

// ---- what the drivers share (net.hpp) ----
namespace {
uint8_t* qbuf;                                  // the receive queue
uint16_t qlen_of[net::QLEN];
int q_head, q_count;
}

namespace net {
Stats st;
int rx_mode = 3;
bool log_on;

void enqueue(const uint8_t* f, unsigned len)
{
    if (len < 14 || len > FRAME_MAX) { st.rx_err++; return; }
    if (rx_mode == 1) return;
    if (q_count == QLEN) { st.dropped++; return; }
    int i = (q_head + q_count) % QLEN;
    memcpy(qbuf + i * FRAME_MAX, f, len);
    qlen_of[i] = (uint16_t)len;
    q_count++;
    st.rx++; st.rx_bytes += len;
    if (log_on) kprintf("net: in %u bytes, type %02x%02x, from %02x:%02x:%02x:%02x:%02x:%02x\n", len, f[12], f[13],
                        f[6], f[7], f[8], f[9], f[10], f[11]);
}

// Room for another frame? While the queue is full, frames stay in the
// card's own ring (more buffering; under QEMU the sender waits too).
bool room() { return q_count < QLEN || rx_mode == 1; }

// PCI power state D0 (full power), as pci_enable_device does: the firmware
// may have left the card in D3. Returns the state it was in.
int pci_d0(const PciDevice& d)
{
    if (!(pci_read(d, 0x04) & (1u << 20))) return -1;   // no capability list
    int cap = pci_read(d, 0x34) & 0xFC;
    for (int n = 0; cap && n < 48; n++) {
        uint32_t h = pci_read(d, cap);
        if ((h & 0xFF) == 0x01) {               // power management
            uint32_t pmcsr = pci_read(d, cap + 4);
            int st = pmcsr & 3;
            if (st) {
                pci_write(d, cap + 4, (pmcsr & ~3u) | 0x8000);   // D0 (and clear PME status)
                udelay(10000);
            }
            return st;
        }
        cap = (h >> 8) & 0xFC;
    }
    return -1;
}

// The card's first memory BAR, mapped (0: none).
volatile uint8_t* map_bar(const PciDevice& d, uint32_t len)
{
    for (int b = 0; b < 6; b++) {
        uint32_t bar = pci_read(d, 0x10 + 4 * b);
        if (bar & 1) continue;                  // I/O
        uint64_t base = bar & ~0xFull;
        bool is64 = (bar & 6) == 4;
        if (is64) base |= (uint64_t)pci_read(d, 0x14 + 4 * b) << 32;
        if (base) return (volatile uint8_t*)map_mmio64(base, len);
        if (is64) b++;
    }
    return 0;
}

int pci_cap(const PciDevice& d, int id)
{
    if (!(pci_read(d, 0x04) & (1u << 20))) return 0;
    int cap = pci_read(d, 0x34) & 0xFC;
    for (int n = 0; cap && n < 48; n++) {
        uint32_t h = pci_read(d, cap);
        if ((h & 0xFF) == (uint32_t)id) return cap;
        cap = (h >> 8) & 0xFC;
    }
    return 0;
}

volatile uint8_t* map_bar_n(const PciDevice& d, int b, uint32_t len)
{
    uint32_t bar = pci_read(d, 0x10 + 4 * b);
    if (bar & 1) return 0;
    uint64_t base = bar & ~0xFull;
    if ((bar & 6) == 4) base |= (uint64_t)pci_read(d, 0x14 + 4 * b) << 32;
    return base ? (volatile uint8_t*)map_mmio64(base, len) : 0;
}
} // namespace net

using namespace net;

namespace {
int irq_line = -1;

Card cards[MAXCARDS];
int ncards;
int active = -1;
bool auto_pick = true;
bool mac_fixed;                                 // DOS has the address
uint8_t dos_mac[6];

// ---------------- the cards ----------------
// Probed in this order: the first whose match() claims the PCI ID drives it.
const NicDriver* const drivers[] = { &nic_e1000, &nic_rtl8139, &nic_r8169, &nic_tg3, &nic_igb, &nic_igc, &nic_alx, &nic_atl1c, &nic_virtio };

// Asking the driver can mean several PHY reads over MDIO (Broadcom: about a
// millisecond), and the packet driver asks with every poll: at most every 100 ms.
bool card_link(Card& c)
{
    if (!c.running) return false;
    if (!c.link_known || ticks - c.link_tick >= 100) {
        c.link_up = c.drv->link(c);
        c.link_tick = ticks;
        c.link_known = true;
    }
    return c.link_up;
}
void card_mode(Card& c, int m) { c.drv->set_mode(c, m); }   // 1: receiving nothing
void card_mac(Card& c, const uint8_t* a) { c.drv->set_mac(c, a); }
bool card_start(Card& c) { c.kind = c.drv->kind; c.link_known = false; return c.running = c.drv->init(c); }

void poll_card()
{
    if (active < 0) return;
    Card& c = cards[active];
    if (c.drv->poll) c.drv->poll(c);
}

bool send_frame(const uint8_t* f, unsigned len)
{
    if (active < 0) return false;
    Card& c = cards[active];
    return c.drv->send(c, f, len);
}

const uint8_t* cur_mac() { return mac_fixed ? dos_mac : active >= 0 ? cards[active].mac : dos_mac; }

void log_mac(const char* what, const uint8_t* m)
{
    kprintf("%s%02x:%02x:%02x:%02x:%02x:%02x", what, m[0], m[1], m[2], m[3], m[4], m[5]);
}

// Make card i the one in use. 0, 1 no such card, 2 it didn't start.
int use_card(int i, const char* why)
{
    if (i < 0 || i >= ncards) return 1;
    Card& c = cards[i];
    if (!c.running) return 2;
    if (active >= 0 && active != i) card_mode(cards[active], 1);
    active = i;
    q_head = q_count = 0;
    card_mac(c, cur_mac());                     // its own, or the one DOS has
    card_mode(c, rx_mode);
    if (c.kind == KIND_USB) kprintf("net: using card %d, %s (%s), link %s", i + 1, c.name, why, card_link(c) ? "up" : "down");
    else kprintf("net: using card %d, %s at %02x:%02x.%u (%s), link %s", i + 1, c.name, c.pci.bus, c.pci.dev, c.pci.fn,
                 why, card_link(c) ? "up" : "down");
    log_mac(", address ", cur_mac());
    kprintf("\n");
    return 0;
}

// Auto: while the card in use has had no link for 3 s, change to one that has.
// Every link change in the log.
void link_watch()
{
    static uint32_t last;
    static uint8_t was[MAXCARDS];
    if (ticks - last < 250) return;
    last = ticks;
    for (int i = 0; i < ncards; i++) {
        if (!cards[i].running) continue;
        uint8_t now = card_link(cards[i]) ? 2 : 1;
        if (was[i] && now != was[i]) kprintf("net: card %d (%s): link %s\n", i + 1, cards[i].name, now == 2 ? "up" : "down");
        was[i] = now;
        // debug=1: what the PHY says while there's no link, every 2 s for the first 30 s (Realtek)
        static int dumps[MAXCARDS];
        if (debug_level >= 1 && now == 1 && cards[i].drv->debug && dumps[i] < 15 && (ticks / 250) % 8 == 0) {
            dumps[i]++;
            cards[i].drv->debug(cards[i], "no link");
        }
    }
}

void auto_check()
{
    static uint32_t down_since, last;
    if (!auto_pick || ncards < 2 || ticks - last < 500) return;
    last = ticks;
    if (active >= 0 && card_link(cards[active])) { down_since = 0; return; }
    if (!down_since) { down_since = ticks | 1; return; }
    if (ticks - down_since < 3000) return;
    for (int i = 0; i < ncards; i++)
        if (i != active && card_link(cards[i])) { use_card(i, "auto: it has a link"); down_since = 0; return; }
}

} // namespace

extern "C" int net_ready;
int net_ready;
extern "C" int ne2k_running(void);
extern "C" void ne2k_tick(void);
extern "C" void ne2k_init(void);

// For the emulated NE2000 (ne2k.c).
extern "C" int net_peek_len(void) { if (!q_count) poll_card(); return q_count ? qlen_of[q_head] : 0; }
extern "C" int net_take(uint8_t* buf, int max)
{
    if (!q_count) return 0;
    int len = qlen_of[q_head];
    memcpy(buf, qbuf + q_head * FRAME_MAX, len < max ? len : max);
    q_head = (q_head + 1) % QLEN;
    q_count--;
    return len;
}
extern "C" int net_send(const uint8_t* f, unsigned len)
{
    bool ok = len >= 14 && len <= 1514 && send_frame(f, len);
    if (ok) { st.tx++; st.tx_bytes += len; } else st.tx_err++;
    if (log_on) kprintf("net: NE2000 out %u bytes, type %02x%02x%s\n", len, f[12], f[13], ok ? "" : " FAILED");
    return ok;
}
extern "C" int net_dos_mac(uint8_t* mac, int fix)          // the address DOS sees (fix: from now on)
{
    if (active < 0) return 0;
    if (fix && !mac_fixed) { memcpy(dos_mac, cards[active].mac, 6); mac_fixed = true; }
    memcpy(mac, cur_mac(), 6);
    return 1;
}
extern "C" void net_rx_mode(int m)
{
    if (m == rx_mode) return;
    rx_mode = m;
    if (active >= 0) card_mode(cards[active], m);
}

static void net_init_cards(void);
static void usb_check(bool pick);
extern "C" void net_init(void)
{
    net_init_cards();
    ne2k_init();
}

static void net_init_cards(void)
{
    if (strstr(cmdline, "net=off")) return;
    log_on = strstr(cmdline, "netlog") != 0;
    PciDevice d;
    for (int i = 0; ncards < MAXCARDS && pci_find_class(0x02, 0x00, -1, &d, i); i++) {
        uint32_t id = pci_read(d, 0), sub = pci_read(d, 0x2C);
        const NicDriver* drv = 0;
        for (const NicDriver* k : drivers) if (k->match(id, sub)) { drv = k; break; }
        kprintf("net: PCI %02x:%02x.%u %04x:%04x%s\n", d.bus, d.dev, d.fn, id & 0xFFFF, id >> 16,
                drv ? "" : " (not supported)");
        if (!drv) continue;
        Card& c = cards[ncards++];
        c.drv = drv; c.pci = d; c.vendor = id & 0xFFFF; c.device = id >> 16;
        if (card_start(c)) {
            kprintf("net: card %d: %s", ncards, c.name);
            log_mac(", MAC ", c.mac);
            kprintf(", link %s\n", card_link(c) ? "up" : "down");
        } else kprintf("net: card %d (%04x:%04x) didn't start\n", ncards, c.vendor, c.device);
    }
    qbuf = (uint8_t*)phys_alloc(QLEN * FRAME_MAX);
    net_ready = 1;                              // a USB adapter may come later
    usb_check(false);
    if (!ncards) { kprintf("net: no Ethernet card vmdos can drive (a USB one is taken when plugged in)\n"); return; }

    int want = -1;
    if (const char* p = strstr(cmdline, "nic=")) { want = p[4] - '1'; auto_pick = false; }
    if (want >= 0 && use_card(want, "nic=") == 0) return;
    if (want >= 0) kprintf("net: nic=%d: no such card running\n", want + 1);
    auto_pick = true;
    int pick = -1;
    for (int i = 0; i < ncards && pick < 0; i++) if (card_link(cards[i])) pick = i;   // one with a link
    for (int i = 0; i < ncards && pick < 0; i++) if (cards[i].running) pick = i;
    if (pick >= 0) use_card(pick, "auto");
}

// Every timer tick.
// A card for the USB adapter when one is plugged in (the same entry when it
// comes back); auto: used if nothing has a link.
static void usb_check(bool pick)
{
    static uint32_t gen;
    uint32_t g = usb_net_gen();
    if (g == gen) return;
    gen = g;
    int u = -1;
    for (int i = 0; i < ncards; i++) if (cards[i].kind == KIND_USB) u = i;
    if (usb_net_present()) {
        if (u < 0) {
            if (ncards == MAXCARDS) return;
            u = ncards++;
            cards[u].drv = &nic_usb;
            cards[u].kind = KIND_USB;
        }
        Card& c = cards[u];
        if (!card_start(c)) return;
        kprintf("net: card %d: %s", u + 1, c.name);
        log_mac(", MAC ", c.mac);
        kprintf("\n");
        if (!pick) card_mode(c, 1);             // at boot: chosen with the others
        else if (active == u) use_card(u, "plugged in again");
        else if (active < 0 || (auto_pick && !card_link(cards[active]))) use_card(u, "auto: plugged in");
        else card_mode(c, 1);
    } else if (u >= 0 && cards[u].running) {
        cards[u].running = false;
        kprintf("net: card %d (%s) unplugged\n", u + 1, cards[u].name);
        if (active == u && auto_pick)
            for (int i = 0; i < ncards; i++)
                if (i != u && card_link(cards[i])) { use_card(i, "auto: the USB adapter went"); break; }
    }
}

// usb.cpp: a frame from the USB adapter (from usb_poll, the timer tick).
extern "C" void net_usb_rx(const uint8_t* f, unsigned len)
{
    if (active < 0 || cards[active].kind != KIND_USB || !qbuf) return;
    if (cards[active].u.spoof && rx_mode < 6 && !(f[0] & 1) && memcmp(f, cur_mac(), 6)) return;   // not ours
    if (!room()) { st.dropped++; return; }
    enqueue(f, len);
}

extern "C" void net_tick(void)
{
    usb_check(true);
    poll_card();
    link_watch();
    auto_check();
    if (ne2k_running()) ne2k_tick();
    if (q_count && irq_line >= 0) vpic_raise(irq_line);
}

extern "C" void net_api(struct regs* r)
{
    int fn = BL(r);
    BX(r) = 0x564D;
    switch (fn) {
    case 0: {
        AX(r) = active >= 0 ? 1 : 0;
        CL(r) = active >= 0 && card_link(cards[active]);
        CH(r) = active >= 0 ? (uint8_t)cards[active].kind : 0;
        poll_card();
        DX(r) = (uint16_t)q_count;
        if (active >= 0) {
            if (!mac_fixed) { memcpy(dos_mac, cards[active].mac, 6); mac_fixed = true; }
            memcpy(gptr(LIN(r->v86_es, DI(r))), dos_mac, 6);
        }
        return; }
    case 1:
        irq_line = CL(r) < 16 ? CL(r) : -1;
        if (log_on) kprintf("net: IRQ %d\n", irq_line);
        return;
    case 2: {
        unsigned len = CX(r);
        const uint8_t* f = gptr(LIN(r->v86_ds, SI(r)));
        bool ok = len >= 14 && len <= 1514 && send_frame(f, len);
        if (ok) { st.tx++; st.tx_bytes += len; } else st.tx_err++;
        if (log_on) kprintf("net: out %u bytes, type %02x%02x, to %02x:%02x:%02x:%02x:%02x:%02x%s\n", len, f[12], f[13],
                            f[0], f[1], f[2], f[3], f[4], f[5], ok ? "" : " FAILED");
        AX(r) = ok ? 0 : 1;
        return; }
    case 3: {
        if (!q_count) poll_card();
        if (!q_count) { CX(r) = 0; DX(r) = 0; return; }
        unsigned len = qlen_of[q_head], room = CX(r);
        memcpy(gptr(LIN(r->v86_es, DI(r))), qbuf + q_head * FRAME_MAX, len < room ? len : room);
        q_head = (q_head + 1) % QLEN;
        q_count--;
        CX(r) = (uint16_t)len;
        DX(r) = (uint16_t)q_count;
        return; }
    case 4: {
        int m = CL(r);
        if (m < 1 || m > 6) { AX(r) = 1; return; }
        rx_mode = m;
        if (active >= 0) card_mode(cards[active], m);
        if (m == 1) q_count = 0;
        AX(r) = 0;
        return; }
    case 5:
        memcpy(gptr(LIN(r->v86_es, DI(r))), &st, sizeof st);
        AX(r) = 0;
        return;
    case 6: {                                   // the cards, 32 bytes each
        uint8_t* o = gptr(LIN(r->v86_es, DI(r)));
        int n = ncards < 8 ? ncards : 8;
        for (int i = 0; i < n; i++, o += 32) {
            Card& c = cards[i];
            memset(o, 0, 32);
            o[0] = (uint8_t)c.kind;
            o[1] = c.running;
            o[2] = card_link(c);
            o[3] = c.pci.bus; o[4] = c.pci.dev; o[5] = c.pci.fn;
            o[6] = (uint8_t)c.vendor; o[7] = (uint8_t)(c.vendor >> 8);
            o[8] = (uint8_t)c.device; o[9] = (uint8_t)(c.device >> 8);
            memcpy(o + 10, c.mac, 6);
            memcpy(o + 16, i == active ? cur_mac() : c.mac, 6);
            for (int k = 0; k < 9 && c.name[k]; k++) o[22 + k] = (uint8_t)c.name[k];   // 9 chars + NUL
        }
        AX(r) = (uint16_t)ncards;
        CX(r) = (uint16_t)(active >= 0 ? active : 0xFFFF);
        DX(r) = auto_pick ? 1 : 0;
        return; }
    case 7: {
        if (CL(r) == 0xFF) { auto_pick = true; AX(r) = 0; kprintf("net: auto\n"); return; }
        int e = use_card(CL(r), "VMNIC");
        if (!e) auto_pick = false;
        AX(r) = (uint16_t)e;
        return; }
    }
    AX(r) = 0xFFFF;
}

