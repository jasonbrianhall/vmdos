// virtio-balloon, the statistics queue only (QEMU: -device virtio-balloon-pci;
// Linux: virtio_balloon): how much memory vmdos has free, for the host to
// read (QMP: qom-set guest-stats-polling-interval, then qom-get guest-stats;
// virsh dommemstat). Legacy or virtio 1.0, the transport in virtio.cpp.
//
// No inflating: num_pages is left alone and "actual" stays 0, so the host
// never gets pages back. The protocol: we give the device one buffer of
// (tag, value) pairs; it keeps it until it wants fresh numbers, then hands it
// back (a used entry); we refill it and give it again. Polled from the timer.
// "balloon=off" skips it.
#include "virtio.hpp"

namespace {

const uint32_t F_STATS_VQ = 1u << 1;            // VIRTIO_BALLOON_F_STATS_VQ
const int Q_STATS = 2;                          // 0 inflate, 1 deflate, 2 stats
enum { S_MEMFREE = 4, S_MEMTOT = 5, S_AVAIL = 6 };   // VIRTIO_BALLOON_S_*

struct __attribute__((packed)) Stat { uint16_t tag; uint64_t val; };
const int NSTATS = 3;

VirtioDev v;
Virtq q;
volatile Stat* buf;
bool on;
uint32_t refills;

void fill()
{
    // RAM above 4 GiB counts as free: vmdos (32-bit) never touches it, so
    // it costs the host nothing (virt-manager shows used = balloon - free).
    uint64_t hi = phys_unreachable();
    buf[0].tag = S_MEMTOT;  buf[0].val = (uint64_t)phys_total() + hi;
    buf[1].tag = S_MEMFREE; buf[1].val = (uint64_t)phys_free() + hi;
    buf[2].tag = S_AVAIL;   buf[2].val = (uint64_t)phys_spare();   // what XMS, EMS and DPMI can still get
}

void give()
{
    volatile VirtqDesc* d = q.desc();
    d[0].addr = (uint32_t)(uintptr_t)buf; d[0].len = NSTATS * sizeof(Stat); d[0].flags = 0; d[0].next = 0;
    q.push(0);
    virtio_notify(v, Q_STATS, q);
}

bool probe(const PciDevice& pd)
{
    if (!virtio_open(v, pd)) return false;
    uint32_t got;
    if (!virtio_features(v, F_STATS_VQ, &got) || !(got & F_STATS_VQ)) {
        kprintf("virtio-balloon %02x:%02x.%u: no statistics queue\n", pd.bus, pd.dev, pd.fn);
        virtio_fail(v);
        return false;
    }
    if (!virtio_queue(v, Q_STATS, q, 4)) { kprintf("virtio-balloon: no queue %d\n", Q_STATS); virtio_fail(v); return false; }
    buf = (volatile Stat*)phys_alloc(4096);
    virtio_ready(v);
    fill();
    give();                                     // the first set, at once
    kprintf("virtio-balloon %02x:%02x.%u%s: memory statistics for the host\n", pd.bus, pd.dev, pd.fn,
            v.modern ? "" : " (legacy)");
    return true;
}

} // namespace

extern "C" void balloon_init(void)
{
    if (strstr(cmdline, "balloon=off")) return;
    for (int bus = 0; bus < 256 && !on; bus++)
        for (int dv = 0; dv < 32 && !on; dv++) {
            PciDevice pd{(uint8_t)bus, (uint8_t)dv, 0};
            uint32_t id0 = pci_read(pd, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            int fns = (pci_read(pd, 0x0C) >> 16) & 0x80 ? 8 : 1;
            for (int fn = 0; fn < fns && !on; fn++) {
                pd.fn = (uint8_t)fn;
                uint32_t id = pci_read(pd, 0);
                if ((id & 0xFFFF) == 0x1AF4 && ((id >> 16) == 0x1002 || (id >> 16) == 0x1045)) on = probe(pd);
            }
        }
}

// Each timer tick: the host has handed the buffer back, so it wants new numbers.
extern "C" void balloon_tick(void)
{
    if (!on || !q.has_used()) return;
    q.take();
    fill();
    give();
    refills++;
}
