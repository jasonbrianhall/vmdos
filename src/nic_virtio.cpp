// virtio-net (QEMU: -nic model=virtio-net-pci; Linux: virtio_net,
// virtio_pci_legacy / virtio_pci_modern). Two split virtqueues, polled:
// queue 0 receives, queue 1 sends. Either face of the device: legacy (the
// transitional 1AF4:1000, its registers in I/O BAR 0) or modern (virtio 1.0,
// 1AF4:1041 or a transitional one with disable-legacy=on: registers found
// through vendor capabilities in a memory BAR).
//
// Without the control queue, QEMU's device takes every frame (promiscuous)
// and a modern device's address can't be changed: so vmdos filters by the
// address DOS uses and the receive mode, as for the USB adapter.
#include "net.hpp"

using namespace net;

namespace {
namespace vio {

const int NBUF = 64;                            // buffers per queue (the rings may be bigger)
const int BUFSZ = 2048;

// legacy registers (I/O)
enum { L_DEVF = 0x00, L_DRVF = 0x04, L_PFN = 0x08, L_QSIZE = 0x0C, L_QSEL = 0x0E, L_NOTIFY = 0x10,
       L_STATUS = 0x12, L_ISR = 0x13, L_CONFIG = 0x14 };
// modern common configuration (MMIO)
enum { M_DFSEL = 0x00, M_DF = 0x04, M_GFSEL = 0x08, M_GF = 0x0C, M_STATUS = 0x14, M_QSEL = 0x16,
       M_QSIZE = 0x18, M_QMSIX = 0x1A, M_QENABLE = 0x1C, M_QNOFF = 0x1E, M_QDESC = 0x20, M_QDRV = 0x28,
       M_QDEV = 0x30 };
enum { S_ACK = 1, S_DRIVER = 2, S_DRIVER_OK = 4, S_FEATURES_OK = 8, S_FAILED = 0x80 };
const uint32_t F_MAC = 1u << 5, F_STATUS = 1u << 16;   // VIRTIO_NET_F_*; VERSION_1 is bit 32

struct Desc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct Used { uint32_t id, len; };

bool match(uint32_t id, uint32_t)
{
    if ((id & 0xFFFF) != 0x1AF4) return false;
    uint16_t d = id >> 16;
    return d == 0x1000 || d == 0x1041;
}

// A queue's three parts. Legacy: one block, the used ring on the next 4 KiB
// boundary (the device works the layout out from the size). Modern: the same.
volatile Desc* desc(uint8_t* r) { return (volatile Desc*)r; }
volatile uint16_t* avail(Card& c, int q) { return (volatile uint16_t*)(c.v.ring[q] + 16 * c.v.qsz[q]); }
uint32_t used_off(int n) { return (16 * n + 6 + 2 * n + 4095) & ~4095u; }
volatile uint16_t* used_hdr(Card& c, int q) { return (volatile uint16_t*)(c.v.ring[q] + used_off(c.v.qsz[q])); }
volatile Used* used_ring(Card& c, int q) { return (volatile Used*)(c.v.ring[q] + used_off(c.v.qsz[q]) + 4); }
uint32_t ring_bytes(int n) { return used_off(n) + 6 + 8 * n; }

uint8_t status_get(Card& c) { return c.v.modern ? c.v.common[M_STATUS] : inb(c.io + L_STATUS); }
void status_set(Card& c, uint8_t s)
{
    if (c.v.modern) c.v.common[M_STATUS] = s;
    else outb(c.io + L_STATUS, s);
}
uint8_t cfg8(Card& c, int off) { return c.v.modern ? c.v.dev[off] : inb(c.io + L_CONFIG + off); }

void notify(Card& c, int q)
{
    BARRIER();
    if (c.v.modern) *(volatile uint16_t*)(c.v.notify + (uint32_t)c.v.qoff[q] * c.v.notify_mult) = (uint16_t)q;
    else outw(c.io + L_NOTIFY, (uint16_t)q);
}

// Modern: the vendor capabilities (common, notify, device configuration).
bool find_caps(Card& c)
{
    int cap = pci_read(c.pci, 0x34) & 0xFC;
    for (int n = 0; cap && n < 48; n++) {
        uint32_t h = pci_read(c.pci, cap);
        if ((h & 0xFF) == 0x09) {
            int type = (h >> 24) & 0xFF;
            int bar = pci_read(c.pci, cap + 4) & 0xFF;
            uint32_t off = pci_read(c.pci, cap + 8), len = pci_read(c.pci, cap + 12);
            if (bar < 6 && (type == 1 || type == 2 || type == 4)) {
                volatile uint8_t* m = map_bar_n(c.pci, bar, off + len);
                if (m) {
                    if (type == 1 && !c.v.common) c.v.common = m + off;
                    if (type == 2 && !c.v.notify) { c.v.notify = m + off; c.v.notify_mult = pci_read(c.pci, cap + 16); }
                    if (type == 4 && !c.v.dev) c.v.dev = m + off;
                }
            }
        }
        cap = (h >> 8) & 0xFC;
    }
    return c.v.common && c.v.notify;
}

bool setup_queue(Card& c, int q)
{
    uint16_t n;
    if (c.v.modern) {
        volatile uint8_t* m = c.v.common;
        *(volatile uint16_t*)(m + M_QSEL) = (uint16_t)q;
        n = *(volatile uint16_t*)(m + M_QSIZE);
        if (!n) return false;
        if (n > 256) n = 256;                   // modern: we may make it smaller (a power of 2)
        *(volatile uint16_t*)(m + M_QSIZE) = n;
        c.v.qoff[q] = *(volatile uint16_t*)(m + M_QNOFF);
        *(volatile uint16_t*)(m + M_QMSIX) = 0xFFFF;   // no MSI-X vector
    } else {
        outw(c.io + L_QSEL, (uint16_t)q);
        n = inw(c.io + L_QSIZE);                // legacy: the device's size, no choice
        if (!n || n > 32768) return false;
    }
    c.v.qsz[q] = n;
    c.v.ring[q] = (uint8_t*)phys_alloc(ring_bytes(n));   // page-aligned, zeroed
    if (!c.v.ring[q]) return false;
    avail(c, q)[0] = 1;                         // VIRTQ_AVAIL_F_NO_INTERRUPT: we poll
    uintptr_t base = (uintptr_t)c.v.ring[q];
    if (c.v.modern) {
        volatile uint8_t* m = c.v.common;
        *(volatile uint32_t*)(m + M_QDESC) = (uint32_t)base; *(volatile uint32_t*)(m + M_QDESC + 4) = 0;
        *(volatile uint32_t*)(m + M_QDRV) = (uint32_t)(base + 16 * n); *(volatile uint32_t*)(m + M_QDRV + 4) = 0;
        *(volatile uint32_t*)(m + M_QDEV) = (uint32_t)(base + used_off(n)); *(volatile uint32_t*)(m + M_QDEV + 4) = 0;
        *(volatile uint16_t*)(m + M_QENABLE) = 1;
    } else outl(c.io + L_PFN, (uint32_t)(base >> 12));
    return true;
}

// Receive buffer b (descriptor b) to the device.
void rx_give(Card& c, int b)
{
    volatile uint16_t* a = avail(c, 0);
    uint16_t idx = a[1];
    a[2 + idx % c.v.qsz[0]] = (uint16_t)b;
    BARRIER();
    a[1] = (uint16_t)(idx + 1);
}

bool init(Card& c)
{
    memset(&c.v, 0, sizeof c.v);
    pci_write(c.pci, 0x04, pci_read(c.pci, 0x04) | 0x07 | 0x400u);   // I/O, memory, bus master; INTx off
    c.v.modern = c.device == 0x1041;
    uint32_t bar0 = pci_read(c.pci, 0x10);
    if (!c.v.modern && !(bar0 & 1)) c.v.modern = true;    // transitional, legacy face turned off
    if (c.v.modern && !find_caps(c)) { kprintf("net: virtio: no virtio 1.0 capabilities\n"); return false; }
    if (!c.v.modern) c.io = (uint16_t)(bar0 & 0xFFFC);
    snprintf(c.name, sizeof c.name, "virtio%s", c.v.modern ? "" : " legacy");

    status_set(c, 0);                           // reset
    for (int i = 0; i < 1000 && status_get(c); i++) udelay(100);
    status_set(c, S_ACK);
    status_set(c, S_ACK | S_DRIVER);
    uint32_t want = F_MAC | F_STATUS, got;
    if (c.v.modern) {
        volatile uint8_t* m = c.v.common;
        *(volatile uint32_t*)(m + M_DFSEL) = 0; uint32_t f0 = *(volatile uint32_t*)(m + M_DF);
        *(volatile uint32_t*)(m + M_DFSEL) = 1; uint32_t f1 = *(volatile uint32_t*)(m + M_DF);
        if (!(f1 & 1)) { kprintf("net: virtio: no VERSION_1\n"); status_set(c, S_FAILED); return false; }
        got = f0 & want;
        *(volatile uint32_t*)(m + M_GFSEL) = 0; *(volatile uint32_t*)(m + M_GF) = got;
        *(volatile uint32_t*)(m + M_GFSEL) = 1; *(volatile uint32_t*)(m + M_GF) = 1;   // VERSION_1
        status_set(c, S_ACK | S_DRIVER | S_FEATURES_OK);
        if (!(status_get(c) & S_FEATURES_OK)) { kprintf("net: virtio: features refused\n"); status_set(c, S_FAILED); return false; }
        c.v.hdr = 12;                           // virtio_net_hdr with num_buffers (always, in 1.0)
    } else {
        got = inl(c.io + L_DEVF) & want;
        outl(c.io + L_DRVF, got);
        c.v.hdr = 10;                           // virtio_net_hdr (no MRG_RXBUF)
    }
    c.v.status = got & F_STATUS;

    if (got & F_MAC) for (int i = 0; i < 6; i++) c.mac[i] = cfg8(c, i);
    else {                                      // none given: a local one
        static const uint8_t m[6] = { 0x02, 0x56, 0x4D, 0x44, 0x4F, 0x53 };
        memcpy(c.mac, m, 6);
    }
    memcpy(c.v.addr, c.mac, 6);
    c.v.mode = 3;

    if (!setup_queue(c, 0) || !setup_queue(c, 1)) {
        kprintf("net: virtio: queues not set up\n");
        status_set(c, S_FAILED);
        return false;
    }
    c.rxb = (uint8_t*)phys_alloc(NBUF * BUFSZ);
    c.txb = (uint8_t*)phys_alloc(NBUF * BUFSZ);
    int nrx = NBUF < c.v.qsz[0] ? NBUF : c.v.qsz[0];
    for (int b = 0; b < nrx; b++) {
        volatile Desc& d = desc(c.v.ring[0])[b];
        d.addr = (uint32_t)(uintptr_t)(c.rxb + b * BUFSZ);
        d.len = BUFSZ;
        d.flags = 2;                            // VIRTQ_DESC_F_WRITE
        rx_give(c, b);
    }
    c.v.rx_used = c.v.tx_avail = c.v.tx_used = 0;
    c.v.tx_busy = 0;
    status_set(c, S_ACK | S_DRIVER | S_FEATURES_OK | S_DRIVER_OK);
    notify(c, 0);
    return true;
}

bool link(Card& c) { return !c.v.status || (cfg8(c, 6) & 1); }   // VIRTIO_NET_S_LINK_UP

void set_mode(Card& c, int m) { c.v.mode = m; }

void set_mac(Card& c, const uint8_t* a)
{
    memcpy(c.v.addr, a, 6);
    if (!c.v.modern)                            // legacy: the configuration's address is writable
        for (int i = 0; i < 6; i++) outb(c.io + L_CONFIG + i, a[i]);
}

bool wanted(Card& c, const uint8_t* f)
{
    int m = c.v.mode;
    if (m <= 1) return false;
    if (m >= 6) return true;
    if (!(f[0] & 1)) return !memcmp(f, c.v.addr, 6);
    bool bcast = (f[0] & f[1] & f[2] & f[3] & f[4] & f[5]) == 0xFF;
    return bcast ? m >= 3 : m >= 4;
}

void reclaim_tx(Card& c)
{
    volatile uint16_t* uh = used_hdr(c, 1);
    while (c.v.tx_used != uh[1]) {
        BARRIER();
        uint32_t id = used_ring(c, 1)[c.v.tx_used % c.v.qsz[1]].id;
        if (id < 32) c.v.tx_busy &= ~(1u << id);
        c.v.tx_used++;
    }
}

void poll(Card& c)
{
    volatile uint16_t* uh = used_hdr(c, 0);
    bool gave = false;
    for (int n = 0; n < NBUF && c.v.rx_used != uh[1]; n++) {
        if (!room()) break;                     // leave them in the ring
        BARRIER();
        volatile Used& u = used_ring(c, 0)[c.v.rx_used % c.v.qsz[0]];
        uint32_t id = u.id, len = u.len;
        if (id < (uint32_t)NBUF && len > c.v.hdr) {
            const uint8_t* f = c.rxb + id * BUFSZ + c.v.hdr;
            if (wanted(c, f)) enqueue(f, len - c.v.hdr);
        } else st.rx_err++;
        c.v.rx_used++;
        if (id < (uint32_t)NBUF) { rx_give(c, (int)id); gave = true; }
    }
    if (gave) notify(c, 0);
    reclaim_tx(c);
}

bool send(Card& c, const uint8_t* f, unsigned len)
{
    int nslots = NBUF < c.v.qsz[1] ? NBUF : c.v.qsz[1];
    if (nslots > 32) nslots = 32;               // tx_busy's bits
    int slot = -1;
    for (int tries = 0; tries < 2000 && slot < 0; tries++) {
        reclaim_tx(c);
        for (int s = 0; s < nslots; s++) if (!(c.v.tx_busy & (1u << s))) { slot = s; break; }
        if (slot < 0) udelay(5);
    }
    if (slot < 0) return false;
    uint8_t* b = c.txb + slot * BUFSZ;
    memset(b, 0, c.v.hdr);                      // no checksum offload, no GSO
    memcpy(b + c.v.hdr, f, len);
    volatile Desc& d = desc(c.v.ring[1])[slot];
    d.addr = (uint32_t)(uintptr_t)b;
    d.len = c.v.hdr + len;
    d.flags = 0;
    c.v.tx_busy |= 1u << slot;
    volatile uint16_t* a = avail(c, 1);
    uint16_t idx = a[1];
    a[2 + idx % c.v.qsz[1]] = (uint16_t)slot;
    BARRIER();
    a[1] = (uint16_t)(idx + 1);
    notify(c, 1);
    return true;
}

} // namespace vio
} // namespace

const NicDriver net::nic_virtio = {
    KIND_VIRTIO, vio::match, vio::init, vio::link, vio::set_mode, vio::set_mac, vio::poll, vio::send, nullptr,
};
