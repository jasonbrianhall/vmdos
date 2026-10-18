// virtio over PCI, what the virtio drivers share (nic_virtio.cpp, blk_virtio.cpp):
// finding the device's registers (legacy: I/O BAR 0 of a transitional
// device; modern, virtio 1.0: through its vendor capabilities), the status
// and feature handshake, and split virtqueues, polled (interrupts suppressed).
#pragma once
#include "pci.hpp"
#include "hw.hpp"

struct VirtioDev {
    PciDevice pci;
    bool modern;
    uint16_t io;                                // legacy: the I/O ports
    volatile uint8_t *common, *notify, *dev;    // modern: the capabilities' registers
    uint32_t notify_mult;
};

struct VirtqDesc { uint64_t addr; uint32_t len; uint16_t flags, next; };
struct VirtqUsed { uint32_t id, len; };
enum { VQ_NEXT = 1, VQ_WRITE = 2 };             // descriptor flags

// A split virtqueue: descriptors, the available ring, the used ring on the
// next 4 KiB boundary (the layout a legacy device works out from the size).
struct Virtq {
    uint8_t* mem;
    uint16_t size, qoff, used_seen;             // qoff: modern notify offset; used_seen: entries taken
    volatile VirtqDesc* desc() { return (volatile VirtqDesc*)mem; }
    volatile uint16_t* avail() { return (volatile uint16_t*)(mem + 16 * size); }   // flags, idx, ring[]
    volatile uint16_t* used() { return (volatile uint16_t*)(mem + used_off(size)); } // flags, idx
    volatile VirtqUsed* used_ring() { return (volatile VirtqUsed*)(mem + used_off(size) + 4); }
    static uint32_t used_off(int n) { return (16 * n + 6 + 2 * n + 4095) & ~4095u; }
    void push(uint16_t head);                   // a descriptor chain to the device
    bool has_used() { return used_seen != used()[1]; }
    VirtqUsed take();                           // the next used entry (has_used() first)
};

// The device at d: registers found, reset, ACKNOWLEDGE | DRIVER. A
// transitional device (1000h-103Fh) with its I/O BAR is driven as legacy; a
// modern-only one (1040h + type), or a transitional one with its legacy face
// off (QEMU: disable-legacy=on), as virtio 1.0.
bool virtio_open(VirtioDev& v, const PciDevice& d);
// Features (the low 32 bits): want & offered into got. Modern: VERSION_1 too,
// and FEATURES_OK. False if the device refuses.
bool virtio_features(VirtioDev& v, uint32_t want, uint32_t* got);
// Queue q set up with at most max entries (legacy: the device's size).
bool virtio_queue(VirtioDev& v, int q, Virtq& vq, int max);
void virtio_ready(VirtioDev& v);                // DRIVER_OK
void virtio_fail(VirtioDev& v);                 // FAILED
void virtio_notify(VirtioDev& v, int q, Virtq& vq);
uint8_t virtio_cfg8(VirtioDev& v, int off);     // the device-specific configuration
uint32_t virtio_cfg32(VirtioDev& v, int off);
void virtio_cfg8_write(VirtioDev& v, int off, uint8_t b);
