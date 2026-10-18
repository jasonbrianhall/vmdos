// virtio over PCI (virtio.hpp): legacy and virtio 1.0 transports, split virtqueues.
#include "virtio.hpp"

namespace {
// legacy registers (I/O BAR 0); the device configuration follows at 14h
// (no MSI-X: we never turn it on)
enum { L_DEVF = 0x00, L_DRVF = 0x04, L_PFN = 0x08, L_QSIZE = 0x0C, L_QSEL = 0x0E, L_NOTIFY = 0x10,
       L_STATUS = 0x12, L_CONFIG = 0x14 };
// modern common configuration
enum { M_DFSEL = 0x00, M_DF = 0x04, M_GFSEL = 0x08, M_GF = 0x0C, M_STATUS = 0x14, M_QSEL = 0x16,
       M_QSIZE = 0x18, M_QMSIX = 0x1A, M_QENABLE = 0x1C, M_QNOFF = 0x1E, M_QDESC = 0x20, M_QDRV = 0x28,
       M_QDEV = 0x30 };
enum { S_ACK = 1, S_DRIVER = 2, S_DRIVER_OK = 4, S_FEATURES_OK = 8, S_FAILED = 0x80 };

inline void w16(volatile uint8_t* p, uint16_t v) { *(volatile uint16_t*)p = v; }
inline void w32(volatile uint8_t* p, uint32_t v) { *(volatile uint32_t*)p = v; }
inline uint16_t r16(volatile uint8_t* p) { return *(volatile uint16_t*)p; }
inline uint32_t r32(volatile uint8_t* p) { return *(volatile uint32_t*)p; }
inline void barrier() { __asm__ volatile("" ::: "memory"); }
inline void delay_us(uint32_t us) { while (us--) inb(0x80); }

uint8_t status(VirtioDev& v) { return v.modern ? v.common[M_STATUS] : inb(v.io + L_STATUS); }
void set_status(VirtioDev& v, uint8_t s)
{
    if (v.modern) v.common[M_STATUS] = s;
    else outb(v.io + L_STATUS, s);
}

volatile uint8_t* map_bar(const PciDevice& d, int b, uint32_t len)
{
    uint32_t bar = pci_read(d, 0x10 + 4 * b);
    if (bar & 1) return 0;
    uint64_t base = bar & ~0xFull;
    if ((bar & 6) == 4) base |= (uint64_t)pci_read(d, 0x14 + 4 * b) << 32;
    return base ? (volatile uint8_t*)map_mmio64(base, len) : 0;
}

bool find_caps(VirtioDev& v)
{
    int cap = pci_read(v.pci, 0x34) & 0xFC;
    for (int n = 0; cap && n < 48; n++) {
        uint32_t h = pci_read(v.pci, cap);
        if ((h & 0xFF) == 0x09) {                // vendor specific: a virtio structure
            int type = (h >> 24) & 0xFF, bar = pci_read(v.pci, cap + 4) & 0xFF;
            uint32_t off = pci_read(v.pci, cap + 8), len = pci_read(v.pci, cap + 12);
            if (bar < 6 && (type == 1 || type == 2 || type == 4)) {
                volatile uint8_t* m = map_bar(v.pci, bar, off + len);
                if (m) {
                    if (type == 1 && !v.common) v.common = m + off;
                    if (type == 2 && !v.notify) { v.notify = m + off; v.notify_mult = pci_read(v.pci, cap + 16); }
                    if (type == 4 && !v.dev) v.dev = m + off;
                }
            }
        }
        cap = (h >> 8) & 0xFC;
    }
    return v.common && v.notify;
}
} // namespace

void Virtq::push(uint16_t head)
{
    volatile uint16_t* a = avail();
    uint16_t idx = a[1];
    a[2 + idx % size] = head;
    barrier();
    a[1] = (uint16_t)(idx + 1);
}

VirtqUsed Virtq::take()
{
    barrier();
    volatile VirtqUsed& u = used_ring()[used_seen % size];
    VirtqUsed r = { u.id, u.len };
    used_seen++;
    return r;
}

bool virtio_open(VirtioDev& v, const PciDevice& d)
{
    memset(&v, 0, sizeof v);
    v.pci = d;
    pci_write(d, 0x04, pci_read(d, 0x04) | 0x07 | 0x400u);   // I/O, memory, bus master; INTx off (we poll)
    uint16_t id = pci_read(d, 0) >> 16;
    uint32_t bar0 = pci_read(d, 0x10);
    v.modern = id >= 0x1040 || !(bar0 & 1);
    if (v.modern && !find_caps(v)) { kprintf("virtio %02x:%02x.%u: no virtio 1.0 capabilities\n", d.bus, d.dev, d.fn); return false; }
    if (!v.modern) v.io = (uint16_t)(bar0 & 0xFFFC);
    set_status(v, 0);                           // reset
    for (int i = 0; i < 1000 && status(v); i++) delay_us(100);
    set_status(v, S_ACK);
    set_status(v, S_ACK | S_DRIVER);
    return true;
}

bool virtio_features(VirtioDev& v, uint32_t want, uint32_t* got)
{
    if (!v.modern) {
        *got = inl(v.io + L_DEVF) & want;
        outl(v.io + L_DRVF, *got);
        return true;
    }
    volatile uint8_t* m = v.common;
    w32(m + M_DFSEL, 0); uint32_t f0 = r32(m + M_DF);
    w32(m + M_DFSEL, 1); uint32_t f1 = r32(m + M_DF);
    if (!(f1 & 1)) return false;                // no VERSION_1
    *got = f0 & want;
    w32(m + M_GFSEL, 0); w32(m + M_GF, *got);
    w32(m + M_GFSEL, 1); w32(m + M_GF, 1);
    set_status(v, S_ACK | S_DRIVER | S_FEATURES_OK);
    return status(v) & S_FEATURES_OK;
}

bool virtio_queue(VirtioDev& v, int q, Virtq& vq, int max)
{
    uint16_t n;
    if (v.modern) {
        volatile uint8_t* m = v.common;
        w16(m + M_QSEL, (uint16_t)q);
        n = r16(m + M_QSIZE);
        if (!n) return false;
        while (n > max && n > 1) n >>= 1;      // modern: smaller is allowed (a power of 2)
        w16(m + M_QSIZE, n);
        vq.qoff = r16(m + M_QNOFF);
        w16(m + M_QMSIX, 0xFFFF);
    } else {
        outw(v.io + L_QSEL, (uint16_t)q);
        n = inw(v.io + L_QSIZE);                // legacy: the device's own size
        if (!n || n > 32768) return false;
        vq.qoff = 0;
    }
    vq.size = n;
    vq.used_seen = 0;
    vq.mem = (uint8_t*)phys_alloc(Virtq::used_off(n) + 6 + 8 * n);   // page-aligned, zeroed
    if (!vq.mem) return false;
    vq.avail()[0] = 1;                          // VIRTQ_AVAIL_F_NO_INTERRUPT
    uintptr_t base = (uintptr_t)vq.mem;
    if (v.modern) {
        volatile uint8_t* m = v.common;
        w32(m + M_QDESC, (uint32_t)base); w32(m + M_QDESC + 4, 0);
        w32(m + M_QDRV, (uint32_t)(base + 16 * n)); w32(m + M_QDRV + 4, 0);
        w32(m + M_QDEV, (uint32_t)(base + Virtq::used_off(n))); w32(m + M_QDEV + 4, 0);
        w16(m + M_QENABLE, 1);
    } else outl(v.io + L_PFN, (uint32_t)(base >> 12));
    return true;
}

void virtio_ready(VirtioDev& v) { set_status(v, status(v) | S_DRIVER_OK); }
void virtio_fail(VirtioDev& v) { set_status(v, S_FAILED); }

void virtio_notify(VirtioDev& v, int q, Virtq& vq)
{
    barrier();
    if (v.modern) *(volatile uint16_t*)(v.notify + (uint32_t)vq.qoff * v.notify_mult) = (uint16_t)q;
    else outw(v.io + L_NOTIFY, (uint16_t)q);
}

uint8_t virtio_cfg8(VirtioDev& v, int off) { return v.modern ? (v.dev ? v.dev[off] : 0) : inb(v.io + L_CONFIG + off); }
uint32_t virtio_cfg32(VirtioDev& v, int off)
{
    if (v.modern) return v.dev ? r32(v.dev + off) : 0;
    return inl(v.io + L_CONFIG + off);
}
void virtio_cfg8_write(VirtioDev& v, int off, uint8_t b)
{
    if (v.modern) { if (v.dev) v.dev[off] = b; }
    else outb(v.io + L_CONFIG + off, b);
}
