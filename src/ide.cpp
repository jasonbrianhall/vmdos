// IDE (parallel ATA, and SATA controllers in IDE mode) disks, for C: on a
// real disk (disk.c), and IDE CD/DVD drives (ATAPI, for cd.c). Polled PIO,
// no interrupts (nIEN): disks 512-byte sectors, LBA28 or LBA48; optical
// drives SCSI commands in ATA PACKET.
//
// Every PCI IDE controller (class 01h/01h), both channels: a channel in
// compatibility mode is at the ISA addresses (1F0h/3F6h, 170h/376h), one
// in native mode at its BARs. Each channel is reset, then master and slave
// are asked for IDENTIFY (DEVICE or PACKET DEVICE).
#include "hw.hpp"
#include "pci.hpp"

// Milliseconds by the PIT's channel 0 counter (reloaded once a millisecond):
// counted from its reloads, so it works with interrupts off too (early in
// boot the tick count doesn't move yet).
static uint16_t pit_count() { outb(0x43, 0x00); uint16_t c = inb(0x40); return c | (uint16_t)(inb(0x40) << 8); }
struct Clock {
    uint16_t prev = pit_count();
    int ms = 0;
    int now() { uint16_t c = pit_count(); if (c > prev) ms++; prev = c; return ms; }
};
static void sleep_ms(int ms) { Clock t; while (t.now() < ms) {} }

enum { R_DATA = 0, R_ERR = 1, R_FEAT = 1, R_COUNT = 2, R_LBA0 = 3, R_LBA1 = 4, R_LBA2 = 5, R_DEV = 6,
       R_STATUS = 7, R_CMD = 7 };
enum { ST_ERR = 0x01, ST_DRQ = 0x08, ST_DF = 0x20, ST_BSY = 0x80 };

struct Chan { uint16_t cmd, ctl; };
struct Drive {
    Chan* ch;
    int slave;
    bool lba48;
    uint64_t sectors;
    char model[41];
    uint8_t err;                 // error register of the last failed command
};
#define MAX_CHANS 8
#define MAX_DISKS 8
#define MAX_CDS 4
static Chan chans[MAX_CHANS];
static Drive disks[MAX_DISKS], cds[MAX_CDS];
static int n_chans, n_disks, n_cds;
static bool ide_done;

static inline uint8_t status(const Chan& c) { return inb(c.ctl); }          // alternate status: no side effects
static inline void delay400(const Chan& c) { for (int i = 0; i < 4; i++) inb(c.ctl); }

// Until BSY clears (and, with `drq`, DRQ or an error shows): the status, or
// -1 after `ms` milliseconds (by the PIT).
static int wait_ready(const Chan& c, int ms, bool drq) {
    Clock t;
    for (;;) {
        uint8_t s = status(c);
        if (s == 0xFF) return -1;                                // nothing on the bus
        if (!(s & ST_BSY) && (!drq || (s & (ST_DRQ | ST_ERR | ST_DF)))) return s;
        if (t.now() > ms) return -1;
    }
}

static bool select(Drive& d, uint8_t lba_bits) {
    const Chan& c = *d.ch;
    if (wait_ready(c, 1000, false) < 0) return false;
    outb(c.cmd + R_DEV, 0xA0 | 0x40 | (d.slave ? 0x10 : 0) | lba_bits);
    delay400(c);
    return wait_ready(c, 1000, false) >= 0;
}

static void read_words(uint16_t port, void* buf, uint32_t words) {
    __asm__ volatile("cld; rep insw" : "+D"(buf), "+c"(words) : "d"(port) : "memory");
}
static void write_words(uint16_t port, const void* buf, uint32_t words) {
    __asm__ volatile("cld; rep outsw" : "+S"(buf), "+c"(words) : "d"(port) : "memory");
}

// Sector commands: READ/WRITE SECTORS (EXT), count 1-128, PIO a sector at a time.
static bool rw(Drive& d, uint64_t lba, uint32_t count, void* buf, bool write) {
    const Chan& c = *d.ch;
    bool ext = d.lba48 && (lba + count > 0x0FFFFFFF);
    if (!select(d, ext ? 0 : (uint8_t)((lba >> 24) & 0x0F))) return false;
    if (ext) {
        outb(c.cmd + R_COUNT, (uint8_t)(count >> 8));
        outb(c.cmd + R_LBA0, (uint8_t)(lba >> 24));
        outb(c.cmd + R_LBA1, (uint8_t)(lba >> 32));
        outb(c.cmd + R_LBA2, (uint8_t)(lba >> 40));
    }
    outb(c.cmd + R_COUNT, (uint8_t)count);                       // 0 would mean 256 (LBA28) / 65536
    outb(c.cmd + R_LBA0, (uint8_t)lba);
    outb(c.cmd + R_LBA1, (uint8_t)(lba >> 8));
    outb(c.cmd + R_LBA2, (uint8_t)(lba >> 16));
    outb(c.cmd + R_CMD, write ? (ext ? 0x34 : 0x30) : (ext ? 0x24 : 0x20));
    delay400(c);
    uint8_t* p = (uint8_t*)buf;
    for (uint32_t i = 0; i < count; i++, p += 512) {
        int s = wait_ready(c, 10000, true);
        if (s < 0 || (s & (ST_ERR | ST_DF)) || !(s & ST_DRQ)) goto fail;
        if (write) write_words(c.cmd + R_DATA, p, 256);
        else read_words(c.cmd + R_DATA, p, 256);
        delay400(c);
    }
    if (write) {                                                 // the last sector written: wait for the drive
        int s = wait_ready(c, 10000, false);
        if (s < 0 || (s & (ST_ERR | ST_DF))) goto fail;
    }
    inb(c.cmd + R_STATUS);                                       // clears a pending interrupt
    return true;
fail:
    d.err = inb(c.cmd + R_ERR);
    kprintf("ide: %s of %u sectors at %u failed (status %x, error %x)\n", write ? "write" : "read", count,
            (uint32_t)lba, inb(c.cmd + R_STATUS), d.err);
    return false;
}

// ATA PACKET, PIO: the 12-byte CDB, then the drive hands over the data in
// pieces of the size it puts in LBA1/LBA2. True on success; d.err holds
// the error register (sense key in bits 7-4) on failure.
static bool packet(Drive& d, const uint8_t* cdb, void* buf, uint32_t bytes) {
    const Chan& c = *d.ch;
    d.err = 0;
    if (!select(d, 0)) return false;
    outb(c.cmd + R_FEAT, 0);                                     // PIO
    outb(c.cmd + R_LBA1, 0xFE);                                  // byte count limit 0xFFFE
    outb(c.cmd + R_LBA2, 0xFF);
    outb(c.cmd + R_CMD, 0xA0);
    delay400(c);
    int s = wait_ready(c, 1000, true);
    if (s < 0 || (s & ST_ERR) || !(s & ST_DRQ)) { d.err = inb(c.cmd + R_ERR); inb(c.cmd + R_STATUS); return false; }
    write_words(c.cmd + R_DATA, cdb, 6);
    delay400(c);
    uint8_t* p = (uint8_t*)buf;
    uint32_t got = 0;
    for (;;) {
        s = wait_ready(c, 10000, false);                         // a spinning-up drive takes its time
        if (s < 0) { kprintf("ide: CD command %02x timed out\n", cdb[0]); return false; }
        if (s & ST_ERR) { d.err = inb(c.cmd + R_ERR); inb(c.cmd + R_STATUS); return false; }
        if (!(s & ST_DRQ)) break;                                // done
        uint32_t n = inb(c.cmd + R_LBA1) | (uint32_t)inb(c.cmd + R_LBA2) << 8;
        uint32_t take = got + n <= bytes ? n : (bytes > got ? bytes - got : 0);
        if (take) read_words(c.cmd + R_DATA, p + got, (take + 1) / 2);
        for (uint32_t i = (take + 1) / 2 * 2; i < n; i += 2) inw(c.cmd + R_DATA);   // more than asked for
        got += take;
        delay400(c);
    }
    inb(c.cmd + R_STATUS);
    return true;
}

static void get_model(const uint16_t* w, char* m) {
    for (int i = 0; i < 20; i++) { m[i * 2] = (char)(w[27 + i] >> 8); m[i * 2 + 1] = (char)w[27 + i]; }
    int e = 40;
    while (e > 0 && m[e - 1] == ' ') e--;
    m[e] = 0;
}

// The signature the reset left in a device's LBA1/LBA2: 0000h ATA,
// EB14h ATAPI; -1 for no device. Read for both before any command: the
// task file registers are written to both devices at once.
static int signature(const Chan& c, int slave) {
    outb(c.cmd + R_DEV, 0xA0 | (slave ? 0x10 : 0));
    delay400(c);
    if (status(c) == 0xFF || wait_ready(c, 500, false) < 0) return -1;
    return inb(c.cmd + R_LBA1) | inb(c.cmd + R_LBA2) << 8;
}

// IDENTIFY (PACKET) DEVICE into id; true if a device answered.
static bool identify(Drive& d, uint16_t* id, bool atapi) {
    const Chan& c = *d.ch;
    outb(c.cmd + R_DEV, 0xA0 | (d.slave ? 0x10 : 0));
    delay400(c);
    if (wait_ready(c, 500, false) < 0) return false;
    outb(c.cmd + R_COUNT, 0); outb(c.cmd + R_LBA0, 0); outb(c.cmd + R_LBA1, 0); outb(c.cmd + R_LBA2, 0);
    outb(c.cmd + R_CMD, atapi ? 0xA1 : 0xEC);
    delay400(c);
    if (status(c) == 0) return false;                               // no device
    int s = wait_ready(c, 5000, true);
    if (s < 0 || (s & ST_ERR) || !(s & ST_DRQ)) return false;
    read_words(c.cmd + R_DATA, id, 256);
    inb(c.cmd + R_STATUS);
    return true;
}

static void chan_init(uint16_t cmd, uint16_t ctl) {
    if (n_chans == MAX_CHANS) return;
    for (int i = 0; i < n_chans; i++) if (chans[i].cmd == cmd) return;  // two functions, one legacy channel
    Chan& c = chans[n_chans];
    c.cmd = cmd; c.ctl = ctl;
    if (inb(c.ctl) == 0xFF && inb(c.cmd + R_STATUS) == 0xFF) return;   // floating bus: nothing there
    outb(c.ctl, 0x06);                                              // SRST, nIEN
    sleep_ms(1);
    outb(c.ctl, 0x02);                                              // nIEN: we poll
    sleep_ms(3);
    if (wait_ready(c, 3000, false) < 0 && status(c) == 0xFF) return;
    n_chans++;
    int sig[2] = { signature(c, 0), signature(c, 1) };
    static uint16_t id[256];
    for (int sl = 0; sl < 2; sl++) {
        if (sig[sl] != 0 && sig[sl] != 0xEB14) continue;          // none, or a SATA bridge / port multiplier
        Drive probe = {};
        probe.ch = &c; probe.slave = sl;
        bool atapi = sig[sl] == 0xEB14;
        if (!identify(probe, id, atapi)) continue;
        get_model(id, probe.model);
        const char* where = sl ? "slave" : "master";
        if (atapi) {
            if ((id[0] >> 8 & 0x1F) != 5) {                        // peripheral type: CD/DVD only
                kprintf("ide: %x %s: ATAPI device type %d (%s), skipped\n", cmd, where, id[0] >> 8 & 0x1F, probe.model);
                continue;
            }
            if (n_cds == MAX_CDS) continue;
            cds[n_cds] = probe;
            kprintf("ide: CD/DVD drive %d: %s (%x %s)\n", n_cds, probe.model, cmd, where);
            n_cds++;
            continue;
        }
        if (!(id[49] & (1 << 9))) { kprintf("ide: %x %s: %s: no LBA, skipped\n", cmd, where, probe.model); continue; }
        probe.lba48 = id[83] & (1 << 10);
        probe.sectors = probe.lba48 ? (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32
                                    : (uint64_t)id[60] | (uint64_t)id[61] << 16;
        if (!probe.sectors || n_disks == MAX_DISKS) continue;
        disks[n_disks] = probe;
        kprintf("ide: disk %d: %s, %u MiB (%x %s%s)\n", n_disks, probe.model, (uint32_t)(probe.sectors >> 11),
                cmd, where, probe.lba48 ? ", LBA48" : "");
        n_disks++;
    }
}

extern "C" int ide_init(void) {
    if (ide_done) return n_disks;
    ide_done = true;
    PciDevice pd;
    for (int k = 0; k < 8 && pci_find_class(1, 1, -1, &pd, k); k++) {
        uint32_t pi = (pci_read(pd, 0x08) >> 8) & 0xFF;
        pci_write(pd, 0x04, pci_read(pd, 0x04) | 0x01);           // I/O space
        for (int ch = 0; ch < 2; ch++) {
            uint16_t cmd = ch ? 0x170 : 0x1F0, ctl = ch ? 0x376 : 0x3F6;
            if (pi & (1 << (ch * 2))) {                           // native mode: the BARs
                uint32_t b0 = pci_read(pd, 0x10 + ch * 8), b1 = pci_read(pd, 0x14 + ch * 8);
                if (!(b0 & 1) || !(b1 & 1)) continue;
                cmd = (uint16_t)(b0 & ~3u);
                ctl = (uint16_t)((b1 & ~3u) + 2);
                if (!cmd || ctl == 2) continue;
            }
            chan_init(cmd, ctl);
        }
    }
    return n_disks;
}

extern "C" uint64_t ide_sectors(int d) { return d < n_disks ? disks[d].sectors : 0; }
extern "C" const char* ide_model(int d) { return d < n_disks ? disks[d].model : ""; }

// count <= 128 sectors.
extern "C" int ide_rw(int d, uint64_t lba, uint32_t count, void* buf, int write) {
    if (d >= n_disks || !count || count > 128 || lba + count > disks[d].sectors) return -1;
    if (!disks[d].lba48 && lba + count > 0x0FFFFFFF) return -1;
    return rw(disks[d], lba, count, buf, write) ? 0 : -1;
}

// CD/DVD drives, as ahci_cd_*: a SCSI command (12 bytes) with bytes of
// data in to buf; 0, the sense key of the failure (sense data read right
// away), or -1.
static uint8_t cd_asc;
extern "C" int ide_cd_count(void) { return n_cds; }
extern "C" const char* ide_cd_model(int i) { return i < n_cds ? cds[i].model : ""; }
extern "C" int ide_cd_asc(void) { return cd_asc; }
extern "C" int ide_cd_packet(int i, const uint8_t* cdb, void* buf, uint32_t bytes) {
    if (i >= n_cds) return -1;
    cd_asc = 0;
    if (packet(cds[i], cdb, buf, bytes)) return 0;
    int key = cds[i].err >> 4;
    static uint8_t sense[18];
    uint8_t rs[12] = { 0x03, 0, 0, 0, 18 };
    for (int k = 0; k < 18; k++) sense[k] = 0;
    if (packet(cds[i], rs, sense, 18) && (sense[0] & 0x7E) == 0x70) {
        key = sense[2] & 0xF;
        cd_asc = sense[12];
    }
    return key ? key : -1;
}
