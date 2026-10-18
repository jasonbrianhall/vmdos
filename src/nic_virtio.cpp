// virtio-net (QEMU: -nic model=virtio-net-pci; Linux: virtio_net). Two split
// virtqueues, polled: queue 0 receives, queue 1 sends. Legacy or virtio 1.0,
// the transport in virtio.cpp.
//
// Without the control queue, QEMU's device takes every frame (promiscuous)
// and a modern device's address can't be changed: so vmdos filters by the
// address DOS uses and the receive mode, as for the USB adapter.
#include "net.hpp"

using namespace net;

namespace {
namespace vio {

const int NBUF = 32;                            // buffers per queue (the rings may be bigger)
const int BUFSZ = 2048;
const uint32_t F_MAC = 1u << 5, F_STATUS = 1u << 16;   // VIRTIO_NET_F_*

bool match(uint32_t id, uint32_t)
{
    if ((id & 0xFFFF) != 0x1AF4) return false;
    uint16_t d = id >> 16;
    return d == 0x1000 || d == 0x1041;
}

bool init(Card& c)
{
    memset(&c.v, 0, sizeof c.v);
    VirtioDev& v = c.v.d;
    if (!virtio_open(v, c.pci)) return false;
    snprintf(c.name, sizeof c.name, "virtio%s", v.modern ? "" : " legacy");
    uint32_t got;
    if (!virtio_features(v, F_MAC | F_STATUS, &got)) {
        kprintf("net: virtio: features refused\n");
        virtio_fail(v);
        return false;
    }
    c.v.status = got & F_STATUS;
    c.v.hdr = v.modern ? 12 : 10;               // virtio_net_hdr; 1.0 always has num_buffers
    if (got & F_MAC) for (int i = 0; i < 6; i++) c.mac[i] = virtio_cfg8(v, i);
    else {                                      // none given: a local one
        static const uint8_t m[6] = { 0x02, 0x56, 0x4D, 0x44, 0x4F, 0x53 };
        memcpy(c.mac, m, 6);
    }
    memcpy(c.v.addr, c.mac, 6);
    c.v.mode = 3;

    if (!virtio_queue(v, 0, c.v.rxq, 256) || !virtio_queue(v, 1, c.v.txq, 256)) {
        kprintf("net: virtio: queues not set up\n");
        virtio_fail(v);
        return false;
    }
    c.rxb = (uint8_t*)phys_alloc(NBUF * BUFSZ);
    c.txb = (uint8_t*)phys_alloc(NBUF * BUFSZ);
    int nrx = NBUF < c.v.rxq.size ? NBUF : c.v.rxq.size;
    for (int b = 0; b < nrx; b++) {
        volatile VirtqDesc& d = c.v.rxq.desc()[b];
        d.addr = (uint32_t)(uintptr_t)(c.rxb + b * BUFSZ);
        d.len = BUFSZ;
        d.flags = VQ_WRITE;
        c.v.rxq.push((uint16_t)b);
    }
    virtio_ready(v);
    virtio_notify(v, 0, c.v.rxq);
    return true;
}

bool link(Card& c) { return !c.v.status || (virtio_cfg8(c.v.d, 6) & 1); }   // VIRTIO_NET_S_LINK_UP

void set_mode(Card& c, int m) { c.v.mode = m; }

void set_mac(Card& c, const uint8_t* a)
{
    memcpy(c.v.addr, a, 6);
    if (!c.v.d.modern)                          // legacy: the configuration's address is writable
        for (int i = 0; i < 6; i++) virtio_cfg8_write(c.v.d, i, a[i]);
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
    while (c.v.txq.has_used()) {
        uint32_t id = c.v.txq.take().id;
        if (id < 32) c.v.tx_busy &= ~(1u << id);
    }
}

void poll(Card& c)
{
    Virtq& q = c.v.rxq;
    bool gave = false;
    for (int n = 0; n < NBUF && q.has_used(); n++) {
        if (!room()) break;                     // leave them in the ring
        VirtqUsed u = q.take();
        if (u.id < (uint32_t)NBUF && u.len > c.v.hdr) {
            const uint8_t* f = c.rxb + u.id * BUFSZ + c.v.hdr;
            if (wanted(c, f)) enqueue(f, u.len - c.v.hdr);
        } else st.rx_err++;
        if (u.id < (uint32_t)NBUF) { q.push((uint16_t)u.id); gave = true; }
    }
    if (gave) virtio_notify(c.v.d, 0, q);
    reclaim_tx(c);
}

bool send(Card& c, const uint8_t* f, unsigned len)
{
    int nslots = NBUF < c.v.txq.size ? NBUF : c.v.txq.size;
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
    volatile VirtqDesc& d = c.v.txq.desc()[slot];
    d.addr = (uint32_t)(uintptr_t)b;
    d.len = c.v.hdr + len;
    d.flags = 0;
    c.v.tx_busy |= 1u << slot;
    c.v.txq.push((uint16_t)slot);
    virtio_notify(c.v.d, 1, c.v.txq);
    return true;
}

} // namespace vio
} // namespace

const NicDriver net::nic_virtio = {
    KIND_VIRTIO, vio::match, vio::init, vio::link, vio::set_mode, vio::set_mac, vio::poll, vio::send, nullptr,
};
