// virtio-blk disks (blk.h; QEMU: -drive if=virtio, or -device virtio-blk-pci;
// Linux: virtio_blk). Legacy or virtio 1.0, the transport in virtio.cpp.
// One request at a time on queue 0, polled: a header (type, sector), the
// data, a status byte, as a chain of three descriptors.
#include "virtio.hpp"
#include "blk.h"

namespace {

const int MAX_DISKS = 8;
const uint32_t F_RO = 1u << 5, F_BLK_SIZE = 1u << 6;   // VIRTIO_BLK_F_*
enum { T_IN = 0, T_OUT = 1, T_GET_ID = 8 };

struct Req { uint32_t type, reserved; uint64_t sector; };
struct Disk {
    VirtioDev v;
    Virtq q;
    volatile uint8_t* req;                      // the header at 0, the status byte at 16, the ID at 64
    uint64_t sectors;
    bool ro;
    char name[24];
};
Disk disks[MAX_DISKS];
int n_disks;
bool started;

inline void delay_us(uint32_t us) { while (us--) inb(0x80); }

// One request: data (identity-mapped) of len bytes in (device -> memory) or out. 0: done.
int request(Disk& d, uint32_t type, uint64_t sector, void* data, uint32_t len, bool in)
{
    volatile Req* h = (volatile Req*)d.req;
    h->type = type; h->reserved = 0; h->sector = sector;
    d.req[16] = 0xFF;
    volatile VirtqDesc* ds = d.q.desc();
    ds[0].addr = (uint32_t)(uintptr_t)d.req; ds[0].len = sizeof(Req); ds[0].flags = VQ_NEXT; ds[0].next = 1;
    ds[1].addr = (uint32_t)(uintptr_t)data; ds[1].len = len; ds[1].flags = VQ_NEXT | (in ? VQ_WRITE : 0); ds[1].next = 2;
    ds[2].addr = (uint32_t)(uintptr_t)(d.req + 16); ds[2].len = 1; ds[2].flags = VQ_WRITE; ds[2].next = 0;
    d.q.push(0);
    virtio_notify(d.v, 0, d.q);
    for (uint32_t t = 0; t < 5000000 && !d.q.has_used(); t++) delay_us(1);   // up to ~5 s
    if (!d.q.has_used()) { kprintf("virtio-blk: %s: no answer\n", d.name); return -1; }
    d.q.take();
    return d.req[16];                           // 0 OK, 1 IOERR, 2 UNSUPP
}

void probe(const PciDevice& pd)
{
    if (n_disks >= MAX_DISKS) return;
    Disk& d = disks[n_disks];
    memset(&d, 0, sizeof d);
    if (!virtio_open(d.v, pd)) return;
    uint32_t got;
    if (!virtio_features(d.v, F_RO | F_BLK_SIZE, &got)) { kprintf("virtio-blk: features refused\n"); virtio_fail(d.v); return; }
    if ((got & F_BLK_SIZE) && virtio_cfg32(d.v, 20) != 512) {
        kprintf("virtio-blk %02x:%02x.%u: %u-byte blocks (only 512 here)\n", pd.bus, pd.dev, pd.fn, virtio_cfg32(d.v, 20));
        virtio_fail(d.v);
        return;
    }
    d.ro = got & F_RO;
    d.sectors = virtio_cfg32(d.v, 0) | (uint64_t)virtio_cfg32(d.v, 4) << 32;
    if (!virtio_queue(d.v, 0, d.q, 16)) { kprintf("virtio-blk: no request queue\n"); virtio_fail(d.v); return; }
    d.req = (volatile uint8_t*)phys_alloc(4096);
    virtio_ready(d.v);
    snprintf(d.name, sizeof d.name, "virtio-blk");                 // (no serial)
    if (request(d, T_GET_ID, 0, (void*)(d.req + 64), 20, true) == 0 && d.req[64]) {   // its serial (-drive serial=)
        int k = 0;
        for (; k < 20 && d.req[64 + k] >= ' ' && d.req[64 + k] < 127; k++) d.name[k] = (char)d.req[64 + k];
        d.name[k] = 0;
    }
    kprintf("virtio-blk %02x:%02x.%u%s: %s, %u MiB%s\n", pd.bus, pd.dev, pd.fn, d.v.modern ? "" : " (legacy)", d.name,
            (uint32_t)(d.sectors >> 11), d.ro ? ", read-only" : "");
    n_disks++;
}

int start(void)
{
    if (started) return n_disks;
    started = true;
    for (int bus = 0; bus < 256; bus++)
        for (int dv = 0; dv < 32; dv++) {
            PciDevice pd{(uint8_t)bus, (uint8_t)dv, 0};
            uint32_t id0 = pci_read(pd, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            int fns = (pci_read(pd, 0x0C) >> 16) & 0x80 ? 8 : 1;
            for (int fn = 0; fn < fns; fn++) {
                pd.fn = (uint8_t)fn;
                uint32_t id = pci_read(pd, 0);
                if ((id & 0xFFFF) == 0x1AF4 && ((id >> 16) == 0x1001 || (id >> 16) == 0x1042)) probe(pd);
            }
        }
    return n_disks;
}

uint64_t sectors(int i) { return i < n_disks ? disks[i].sectors : 0; }
const char* name(int i) { return i < n_disks ? disks[i].name : ""; }

int rw(int i, uint64_t lba, uint32_t n, void* buf, int write)
{
    if (i >= n_disks || !n || n > 128 || lba + n > disks[i].sectors) return -1;
    Disk& d = disks[i];
    if (write && d.ro) return -1;
    return request(d, write ? T_OUT : T_IN, lba, buf, n * 512, !write) ? -1 : 0;
}

} // namespace

extern "C" const struct blk_driver blk_virtio = {
    "virtio", "virtio=off", 1, 0, start, nullptr, sectors, name, nullptr, rw,
};
