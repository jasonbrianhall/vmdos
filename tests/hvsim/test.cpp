// The guest side: vmdos's own drivers, as disk.c and net.cpp call them,
// against the simulated host (host.cpp). Built with kernel.h, not libc's
// headers (they'd clash); the C library is linked underneath.
#include "../../src/net.hpp"
#include "../../src/vmbus.hpp"
extern "C" {
#include "../../src/blk.h"

void exit(int);
}

void sim_config(bool old);
bool sim_unloaded();
uint32_t sim_version();
int sim_scsi_splits();
const uint8_t* sim_disk(int lun, uint64_t lba);
uint32_t sim_filter();
int sim_rx_outstanding();
void sim_link(bool up);
extern int net_frames_sent, net_acks, net_ctl;

#define T(c) do { if (!(c)) { printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

// net.cpp's part: the receive queue
namespace net {
Stats st;
bool log_on;
static uint8_t q[64][1536];
static unsigned qlen[64];
static int qn;
static bool full;                               // pretend DOS isn't taking frames
static int dropped;
void enqueue(const uint8_t* f, unsigned len)    // (net.cpp drops what doesn't fit)
{
    if (full || qn == 64) { dropped++; return; }
    memcpy(q[qn], f, len); qlen[qn++] = len;
}
bool room() { return !full && qn < 64; }
}

static uint8_t pattern(uint32_t i, uint32_t k) { return (uint8_t)(i * 7 + k * 13 + (k >> 8)); }

int main(int argc, char** argv)
{
    bool old = argc > 1 && argv[1][0] == 'o';
    sim_config(old);
    printf("== %s host ==\n", old ? "old (VMBus 4.0, VSTOR win7, NVSP 2)" : "new (VMBus 5.2, VSTOR win8.1, NVSP 5)");

    // ---- disks ----
    T(blk_storvsc.start() == 2);
    T(blk_storvsc.start() == 2);                // once
    T(blk_storvsc.sectors(0) == (16u << 20) / 512);
    T(blk_storvsc.sectors(1) == 3ull << 31);
    printf("disk 0: %s, disk 1: %s\n", blk_storvsc.name(0), blk_storvsc.name(1));
    T(sim_version() == (old ? 4u << 16 : (5u << 16 | 2)));

    // 128 sectors (64 KiB, disk.c's most) from a buffer that isn't page-aligned: 17 pages
    uint8_t* raw = (uint8_t*)phys_alloc(80 * 1024);
    uint8_t* buf = raw + 512 + 8;
    for (int k = 0; k < 65536; k++) buf[k] = pattern(1, k);
    T(blk_storvsc.rw(0, 1000, 128, buf, 1) == 0);
    for (int k = 0; k < 65536; k++) T(sim_disk(0, 1000 + k / 512)[k % 512] == pattern(1, k));
    memset(buf, 0, 65536);
    T(blk_storvsc.rw(0, 1000, 128, buf, 0) == 0);
    for (int k = 0; k < 65536; k++) T(buf[k] == pattern(1, k));
    T(sim_scsi_splits() >= 4);                  // 32 KiB a command on this host
    // the last sectors, and past the end
    T(blk_storvsc.rw(0, 32768 - 3, 3, buf, 0) == 0);
    T(blk_storvsc.rw(0, 32768 - 3, 4, buf, 0) == -1);
    // beyond 2 TiB: READ/WRITE (16)
    uint64_t far = (3ull << 31) - 100;
    for (int k = 0; k < 4096; k++) buf[k] = pattern(2, k);
    T(blk_storvsc.rw(1, far, 8, buf, 1) == 0);
    memset(buf, 0, 4096);
    T(blk_storvsc.rw(1, far, 8, buf, 0) == 0);
    for (int k = 0; k < 4096; k++) T(buf[k] == pattern(2, k));
    // many small ones: the rings wrap around many times
    for (uint32_t i = 0; i < 3000; i++) {
        uint32_t n = 1 + i % 9;
        for (uint32_t k = 0; k < n * 512; k++) buf[k] = pattern(i, k);
        T(blk_storvsc.rw(0, (i * 37) % 30000, n, buf, 1) == 0);
        memset(buf, 0, n * 512);
        T(blk_storvsc.rw(0, (i * 37) % 30000, n, buf, 0) == 0);
        for (uint32_t k = 0; k < n * 512; k++) T(buf[k] == pattern(i, k));
    }
    printf("disks: OK\n");

    // ---- network ----
    using namespace net;
    static Card c;
    T(netvsc_card(0, c));
    T(!netvsc_card(1, *new Card()));            // only one adapter
    c.drv = &nic_netvsc;
    T(c.drv->init(c));
    static const uint8_t mac[6] = { 0x00, 0x15, 0x5D, 0x01, 0x02, 0x03 };
    T(!memcmp(c.mac, mac, 6));
    T(c.drv->link(c));
    T(sim_filter() == (1 | 8));                 // directed + broadcast
    printf("net: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", c.name, c.mac[0], c.mac[1], c.mac[2], c.mac[3], c.mac[4], c.mac[5]);

    // frames of every size out, each echoed back by the host (to our address)
    static uint8_t f[1514];
    for (uint32_t i = 0; i < 4000; i++) {
        unsigned len = 60 + (i * 97) % (1514 - 60 + 1);
        for (unsigned k = 0; k < len; k++) f[k] = pattern(i, k);
        f[0] = 0xFF;                            // to somebody else; the echo comes to us
        T(c.drv->send(c, f, len));
        for (int t = 0; t < 50 && !qn; t++) { c.drv->poll(c); kdelay_us(10); }
        T(qn == 1);
        T(qlen[0] == len && !memcmp(q[0], mac, 6) && !memcmp(q[0] + 6, f + 6, len - 6));
        qn = 0;
    }
    T(net_frames_sent == 4000);
    // DOS not taking any: frames wait in the ring, none lost or acked early, then all come
    full = true;
    for (uint32_t i = 0; i < 12; i++) { f[0] = 0; T(c.drv->send(c, f, 100 + i)); c.drv->poll(c); }
    T(qn == 0 && dropped == 0 && sim_rx_outstanding() > 0);
    full = false;
    for (int t = 0; t < 200 && qn < 12; t++) { c.drv->poll(c); kdelay_us(10); }
    T(qn == 12);
    for (int i = 0; i < 12; i++) T(qlen[i] == 100u + i);
    qn = 0;
    T(sim_rx_outstanding() == 0);
    // ... and DOS still sending meanwhile: the send completions behind the
    // frames are taken (the frames dropped), a mode change still answered
    full = true;
    for (uint32_t i = 0; i < 100; i++) { f[0] = 0; T(c.drv->send(c, f, 100 + i)); c.drv->poll(c); }
    c.drv->set_mode(c, 4);
    T(sim_filter() == (1 | 4 | 8));
    full = false;
    for (int t = 0; t < 200; t++) { c.drv->poll(c); kdelay_us(10); }
    T(dropped > 0 && qn + dropped == 100);
    qn = 0;
    T(sim_rx_outstanding() == 0);
    // receive modes; another address (promiscuous at the host, vmdos filters)
    c.drv->set_mode(c, 6);
    T(sim_filter() == 0x20);
    c.drv->set_mode(c, 1);
    T(sim_filter() == 0);
    c.drv->set_mode(c, 3);
    static const uint8_t other[6] = { 0x02, 0, 0, 0, 0, 9 };
    c.drv->set_mac(c, other);
    T(sim_filter() == 0x20);
    c.drv->set_mac(c, mac);
    T(sim_filter() == (1 | 8));
    // link indications
    sim_link(false);
    c.drv->poll(c);
    T(!c.drv->link(c));
    sim_link(true);
    c.drv->poll(c);
    T(c.drv->link(c));
    printf("net: OK (%d frames each way, %d receive acks, %d control messages)\n", net_frames_sent, net_acks, net_ctl);

    vmbus_unload();
    T(sim_unloaded());
    printf("PASS\n");
    return 0;
}
