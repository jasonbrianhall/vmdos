// NVMe disks (blk.h, for disk.c; QEMU: -device nvme,drive=...,serial=...).
// Polled, no interrupts: per controller one admin queue pair and one I/O
// queue pair, one command at a time. Each active namespace with 512-byte
// LBAs (and no separate metadata) is a disk of its own. The firmware's
// controller state is dropped: the controller is reset (CC.EN 0) and
// enabled again with our own queues.
#include "hw.hpp"
#include "pci.hpp"
#include "blk.h"

namespace {

void io_delay(int n) { while (n--) inb(0x80); }   // ~1 us each

const int MAX_CTRL = 4, MAX_DISKS = 8, QD = 16;   // queue depth (entries), admin and I/O

struct Ctrl {
    volatile uint8_t* r;          // registers (BAR0)
    uint32_t dstrd;               // doorbell stride, bytes
    uint8_t* asq; uint8_t* acq;   // admin queues: 64-byte SQ entries, 16-byte CQ entries
    uint8_t* isq; uint8_t* icq;   // I/O queues
    uint16_t asq_tail, acq_head, isq_tail, icq_head;
    uint8_t aphase, iphase;
    uint16_t cid;
    uint32_t max_secs;            // MDTS, in 512-byte sectors (0: no limit)
    uint64_t* prp;                // one page: the PRP list of an I/O command
    char model[41];
};
struct Disk {
    Ctrl* c;
    uint32_t nsid;
    uint64_t sectors;
    char name[48];
};
Ctrl ctrls[MAX_CTRL];
Disk disks[MAX_DISKS];
int n_ctrl, n_disks;
bool started;

inline uint32_t rd(Ctrl& c, int o) { return *(volatile uint32_t*)(c.r + o); }
inline void wr(Ctrl& c, int o, uint32_t v) { *(volatile uint32_t*)(c.r + o) = v; }
enum { CAP = 0x00, VS = 0x08, INTMS = 0x0C, CC = 0x14, CSTS = 0x1C, AQA = 0x24, ASQ = 0x28, ACQ = 0x30 };

inline void db(Ctrl& c, int qid, bool cq, uint16_t v) { wr(c, 0x1000 + (2 * qid + (cq ? 1 : 0)) * c.dstrd, v); }

// One command on a queue pair (qid 0: admin), polled until its completion.
// sqe: the 16 command dwords (CID filled in here). 0: done, else the status
// (-1: no answer within ~10 s).
int submit(Ctrl& c, int qid, uint32_t* sqe)
{
    uint8_t* sq = qid ? c.isq : c.asq;
    uint8_t* cq = qid ? c.icq : c.acq;
    uint16_t& tail = qid ? c.isq_tail : c.asq_tail;
    uint16_t& head = qid ? c.icq_head : c.acq_head;
    uint8_t& phase = qid ? c.iphase : c.aphase;
    uint16_t cid = ++c.cid;
    sqe[0] = (sqe[0] & 0xFFFF) | (uint32_t)cid << 16;
    volatile uint32_t* e = (volatile uint32_t*)(sq + tail * 64);
    for (int i = 0; i < 16; i++) e[i] = sqe[i];
    tail = (uint16_t)((tail + 1) % QD);
    __asm__ volatile("" ::: "memory");
    db(c, qid, false, tail);
    // As with AHCI: watch for the completion continuously (a disk answers
    // in microseconds), reading the clock only now and then.
    uint32_t t0 = pit_clock();
    for (uint32_t i = 1;; i++) {
        volatile uint32_t* ce = (volatile uint32_t*)(cq + head * 16);
        uint32_t dw3 = ce[3];
        if (((dw3 >> 16) & 1) == phase) {
            head = (uint16_t)((head + 1) % QD);
            if (!head) phase ^= 1;
            db(c, qid, true, head);
            if ((dw3 & 0xFFFF) != cid) continue;      // (stale: shouldn't happen, one at a time)
            uint32_t st = (dw3 >> 17) & 0x7FFF;
            return (int)st;
        }
        __asm__ volatile("pause");
        if (!(i & 1023) && pit_clock() - t0 > 1193182u * 10) {
            kprintf("nvme: %s: command %02x on queue %d: no answer\n", c.model, sqe[0] & 0xFF, qid);
            return -1;
        }
    }
}

int admin(Ctrl& c, uint8_t op, uint32_t nsid, void* buf, uint32_t cdw10, uint32_t cdw11)
{
    uint32_t sqe[16] = {};
    sqe[0] = op;
    sqe[1] = nsid;
    sqe[6] = (uint32_t)(uintptr_t)buf;                 // PRP1: one page, identity-mapped
    sqe[10] = cdw10; sqe[11] = cdw11;
    int st = submit(c, 0, sqe);
    if (st > 0) kprintf("nvme: %s: admin command %02x failed, status %x\n", c.model, op, st);
    return st;
}

bool wait_rdy(Ctrl& c, uint32_t want, uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i++) {
        uint32_t s = rd(c, CSTS);
        if (s == 0xFFFFFFFF) return false;             // gone
        if ((s & 1) == want) return true;
        if (want && (s & 2)) return false;             // fatal status
        io_delay(1000);
    }
    return false;
}

void trim(char* dst, const uint8_t* src, int n)
{
    int e = n;
    while (e > 0 && (src[e - 1] == ' ' || !src[e - 1])) e--;
    for (int i = 0; i < e; i++) dst[i] = (src[i] >= ' ' && src[i] < 127) ? (char)src[i] : '?';
    dst[e] = 0;
}

void add_ns(Ctrl& c, uint32_t nsid, uint8_t* id, bool several)
{
    if (n_disks >= MAX_DISKS) return;
    if (admin(c, 0x06, nsid, id, 0, 0)) return;        // IDENTIFY namespace (CNS 0)
    uint64_t nsze = *(uint64_t*)id;
    if (!nsze) return;                                 // inactive
    uint8_t flbas = id[26];
    uint32_t lbaf = *(uint32_t*)(id + 128 + 4 * (flbas & 0xF));
    uint32_t ms = lbaf & 0xFFFF, lbads = (lbaf >> 16) & 0xFF;
    if (lbads != 9 || (ms && !(flbas & 0x10))) {
        kprintf("nvme: %s namespace %u: %u-byte blocks%s (only 512 here), skipped\n", c.model, nsid, 1u << lbads,
                ms ? " with metadata" : "");
        return;
    }
    if (ms) {                                          // extended LBAs: data and metadata interleaved
        kprintf("nvme: %s namespace %u: metadata in its blocks, skipped\n", c.model, nsid);
        return;
    }
    Disk& d = disks[n_disks++];
    d.c = &c; d.nsid = nsid; d.sectors = nsze;
    if (several) snprintf(d.name, sizeof d.name, "%s n%u", c.model, nsid);
    else snprintf(d.name, sizeof d.name, "%s", c.model);
    kprintf("nvme: disk %d: %s, %u MiB\n", n_disks - 1, d.name, (uint32_t)(nsze >> 11));
}

void probe(const PciDevice& pd)
{
    Ctrl& c = ctrls[n_ctrl];
    memset(&c, 0, sizeof c);
    uint32_t bar = pci_read(pd, 0x10);
    uint64_t base = bar & ~0xFull;
    if ((bar & 0x6) == 0x4) base |= (uint64_t)pci_read(pd, 0x14) << 32;
    if (!base || (bar & 1)) { kprintf("nvme %02x:%02x.%u: no memory BAR (%x), skipped\n", pd.bus, pd.dev, pd.fn, bar); return; }
    pci_write(pd, 0x04, pci_read(pd, 0x04) | 0x406);   // memory space, bus master, no INTx: polled
    c.r = (volatile uint8_t*)map_mmio64(base, 0x2000);   // registers, doorbells (stride <= 1 KiB)
    if (!c.r) {
        kprintf("nvme %02x:%02x.%u: registers at %x%08x not mappable, skipped\n", pd.bus, pd.dev, pd.fn,
                (uint32_t)(base >> 32), (uint32_t)base);
        return;
    }
    uint32_t cap_lo = rd(c, CAP), cap_hi = rd(c, CAP + 4);
    if (cap_lo == 0xFFFFFFFF) { kprintf("nvme %02x:%02x.%u: registers read all ones (powered down?), skipped\n", pd.bus, pd.dev, pd.fn); return; }
    c.dstrd = 4u << (cap_hi & 0xF);
    uint32_t to_ms = ((cap_lo >> 24) & 0xFF) * 500 + 500;
    uint32_t mpsmin = (cap_hi >> 16) & 0xF;
    if (!(cap_hi & (1u << 5))) { kprintf("nvme %02x:%02x.%u: no NVM command set\n", pd.bus, pd.dev, pd.fn); return; }
    if (mpsmin) { kprintf("nvme %02x:%02x.%u: no 4 KiB pages\n", pd.bus, pd.dev, pd.fn); return; }
    uint32_t mqes = (cap_lo & 0xFFFF) + 1;             // max entries per queue
    if (mqes < (uint32_t)QD) { kprintf("nvme %02x:%02x.%u: queues of %u entries only\n", pd.bus, pd.dev, pd.fn, mqes); return; }
    if (0x1000 + 4 * c.dstrd > 0x2000) {              // wide doorbell stride: map the rest
        c.r = (volatile uint8_t*)map_mmio64(base, 0x1000 + 4 * c.dstrd);
        if (!c.r) { kprintf("nvme %02x:%02x.%u: doorbells not mappable, skipped\n", pd.bus, pd.dev, pd.fn); return; }
    }

    // Reset: the firmware may have it running with queues of its own.
    if (rd(c, CC) & 1) {
        wr(c, CC, rd(c, CC) & ~1u);
        if (!wait_rdy(c, 0, to_ms)) { kprintf("nvme %02x:%02x.%u: won't reset\n", pd.bus, pd.dev, pd.fn); return; }
    } else if (!wait_rdy(c, 0, to_ms)) {
        kprintf("nvme %02x:%02x.%u: still busy after reset (CSTS %x), skipped\n", pd.bus, pd.dev, pd.fn, rd(c, CSTS));
        return;
    }

    uint8_t* mem = (uint8_t*)phys_try_alloc(6 * 4096);
    if (!mem) { kprintf("nvme %02x:%02x.%u: out of memory\n", pd.bus, pd.dev, pd.fn); return; }
    c.asq = mem; c.acq = mem + 4096; c.isq = mem + 2 * 4096; c.icq = mem + 3 * 4096;
    c.prp = (uint64_t*)(mem + 4 * 4096);
    uint8_t* id = mem + 5 * 4096;
    c.aphase = c.iphase = 1;

    wr(c, AQA, (uint32_t)(QD - 1) << 16 | (QD - 1));
    wr(c, ASQ, (uint32_t)(uintptr_t)c.asq); wr(c, ASQ + 4, 0);
    wr(c, ACQ, (uint32_t)(uintptr_t)c.acq); wr(c, ACQ + 4, 0);
    wr(c, INTMS, 0xFFFFFFFF);                          // polled
    wr(c, CC, 4u << 20 | 6u << 16 | 1);                // I/O CQ entries 16 B, SQ entries 64 B, 4 KiB pages, NVM, enable
    if (!wait_rdy(c, 1, to_ms)) {
        kprintf("nvme %02x:%02x.%u: won't start (CSTS %x)\n", pd.bus, pd.dev, pd.fn, rd(c, CSTS));
        phys_release(mem, 6 * 4096);
        return;
    }
    snprintf(c.model, sizeof c.model, "NVMe");
    if (admin(c, 0x06, 0, id, 1, 0)) {                // IDENTIFY controller
        kprintf("nvme %02x:%02x.%u: IDENTIFY failed, skipped\n", pd.bus, pd.dev, pd.fn);
        wr(c, CC, rd(c, CC) & ~1u);
        wait_rdy(c, 0, to_ms);
        phys_release(mem, 6 * 4096);
        return;
    }
    trim(c.model, id + 24, 40);
    if (!c.model[0]) snprintf(c.model, sizeof c.model, "NVMe");
    uint8_t mdts = id[77];
    c.max_secs = mdts ? (mdts >= 7 ? 0 : (4096u << mdts) / 512) : 0;   // >= 512 KiB: more than we ever send
    uint32_t nn = *(uint32_t*)(id + 516);

    // One I/O queue pair: completion queue 1, then submission queue 1 on it.
    if (admin(c, 0x05, 0, c.icq, (uint32_t)(QD - 1) << 16 | 1, 1) ||
        admin(c, 0x01, 0, c.isq, (uint32_t)(QD - 1) << 16 | 1, 1u << 16 | 1)) {
        kprintf("nvme: %s: no I/O queue\n", c.model);
        wr(c, CC, rd(c, CC) & ~1u);
        wait_rdy(c, 0, to_ms);
        phys_release(mem, 6 * 4096);
        return;
    }
    kprintf("nvme %02x:%02x.%u: %s, %u namespace%s\n", pd.bus, pd.dev, pd.fn, c.model, nn, nn == 1 ? "" : "s");
    n_ctrl++;

    // The active namespaces: their list (NVMe 1.1, CNS 2), else 1 .. NN.
    uint32_t vs = rd(c, VS);
    uint32_t list[64];
    int nl = 0;
    if (vs >= 0x10100 && admin(c, 0x06, 0, id, 2, 0) == 0) {
        for (int i = 0; i < 64 && ((uint32_t*)id)[i]; i++) list[nl++] = ((uint32_t*)id)[i];
    } else {
        for (uint32_t n = 1; n <= nn && nl < 64; n++) list[nl++] = n;
    }
    int before = n_disks;
    for (int i = 0; i < nl; i++) add_ns(c, list[i], id, nl > 1);
    if (n_disks == before) kprintf("nvme: %s: no namespace usable here\n", c.model);
}

int start(void)
{
    if (started) return n_disks;
    started = true;
    PciDevice pd;
    int found = 0;
    for (int k = 0; n_ctrl < MAX_CTRL && pci_find_class(1, 8, 2, &pd, k); k++, found++) probe(pd);
    if (!found) {
        // NVMe drives the PCI bus doesn't show: behind a RAID controller,
        // above all Intel VMD ("RST", "RAID On", "Intel Optane"), common on
        // laptops. Only the firmware setting gets them out.
        for (int k = 0; pci_find_class(1, 4, -1, &pd, k); k++) {
            uint32_t id = pci_read(pd, 0);
            kprintf("nvme: none on PCI; RAID controller %04x:%04x at %02x:%02x.%u%s may hide NVMe drives: "
                    "set the storage mode to AHCI (or NVMe) in the firmware setup\n", id & 0xFFFF, id >> 16,
                    pd.bus, pd.dev, pd.fn, (id & 0xFFFF) == 0x8086 ? " (Intel VMD/RST)" : "");
        }
        for (int k = 0; pci_find_class(1, 8, -1, &pd, k); k++)
            kprintf("nvme: %02x:%02x.%u: non-volatile memory controller, interface %02x (not NVMe), skipped\n",
                    pd.bus, pd.dev, pd.fn, (pci_read(pd, 0x08) >> 8) & 0xFF);
    }
    return n_disks;
}

uint64_t sectors(int i) { return i < n_disks ? disks[i].sectors : 0; }
const char* name(int i) { return i < n_disks ? disks[i].name : ""; }

// n sectors at buf (identity-mapped, dword aligned): PRP1 the first page
// (with buf's offset), PRP2 the second page or, past two, the PRP list.
int xfer(Disk& d, uint64_t lba, uint32_t n, uint32_t buf, int write)
{
    Ctrl& c = *d.c;
    uint32_t bytes = n * 512, first = 4096 - (buf & 0xFFF);
    uint32_t sqe[16] = {};
    sqe[0] = write ? 0x01 : 0x02;
    sqe[1] = d.nsid;
    sqe[6] = buf;
    if (bytes > first) {
        uint32_t p = (buf & ~0xFFFu) + 4096, left = bytes - first;
        if (left <= 4096) sqe[8] = p;
        else {
            for (int k = 0; left; k++, p += 4096) {
                c.prp[k] = p;
                left = left > 4096 ? left - 4096 : 0;
            }
            sqe[8] = (uint32_t)(uintptr_t)c.prp;
        }
    }
    sqe[10] = (uint32_t)lba; sqe[11] = (uint32_t)(lba >> 32);
    sqe[12] = n - 1;
    int st = submit(c, 1, sqe);
    if (st > 0) kprintf("nvme: %s: %s at %u failed, status %x\n", d.name, write ? "write" : "read", (uint32_t)lba, st);
    return st ? -1 : 0;
}

int rw(int i, uint64_t lba, uint32_t n, void* buf, int write)
{
    if (i >= n_disks || !n || n > 128 || lba + n > disks[i].sectors || ((uintptr_t)buf & 3)) return -1;
    Disk& d = disks[i];
    uint32_t b = (uint32_t)(uintptr_t)buf, max = d.c->max_secs ? d.c->max_secs : n;
    while (n) {
        uint32_t k = n < max ? n : max;
        if (xfer(d, lba, k, b, write)) return -1;
        lba += k; n -= k; b += k * 512;
    }
    return 0;
}

} // namespace

extern "C" const struct blk_driver blk_nvme = {
    "NVMe", "nvme=off", 1, 0, start, nullptr, sectors, name, nullptr, rw,
};
