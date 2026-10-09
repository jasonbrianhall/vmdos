// AHCI (SATA) disks, for C: on a real disk (disk.c), and SATA CD/DVD drives
// (ATAPI, for cd.c). Polled, one command slot per port; disks: 512-byte
// sectors, LBA48 DMA; optical drives: SCSI commands in ATA PACKET, DMA. The firmware's controller
// state is taken over: each port is stopped and given our own command
// list, FIS area and command table, then restarted.
#include "hw.hpp"
#include "pci.hpp"

static void io_delay(int n) { while (n--) inb(0x80); }   // ~1 us each

#define MAX_DISKS 8
struct Port {
    volatile uint8_t* r;         // port registers
    uint8_t* mem;                // one page: command list, received FIS, command table
    uint64_t sectors;
    char model[41];
    uint8_t err;                 // error register of the last failed command
};
#define MAX_CDS 4
static Port disks[MAX_DISKS], cds[MAX_CDS];
static int n_disks, n_cds;
static bool ahci_done;
static volatile uint8_t* abar;

static inline uint32_t rd(volatile uint8_t* b, int o) { return *(volatile uint32_t*)(b + o); }
static inline void wr(volatile uint8_t* b, int o, uint32_t v) { *(volatile uint32_t*)(b + o) = v; }

enum { PxCLB = 0x00, PxCLBU = 0x04, PxFB = 0x08, PxFBU = 0x0C, PxIS = 0x10, PxIE = 0x14, PxCMD = 0x18,
       PxTFD = 0x20, PxSIG = 0x24, PxSSTS = 0x28, PxSERR = 0x30, PxCI = 0x38 };
enum { CMD_ST = 1, CMD_SUD = 2, CMD_POD = 4, CMD_FRE = 0x10, CMD_FR = 0x4000, CMD_CR = 0x8000 };

static bool wait_clear(volatile uint8_t* p, int reg, uint32_t bits, int ms) {
    for (int i = 0; i < ms * 10; i++) { if (!(rd(p, reg) & bits)) return true; io_delay(100); }
    return false;
}

static bool port_stop(volatile uint8_t* p) {
    wr(p, PxCMD, rd(p, PxCMD) & ~CMD_ST);
    if (!wait_clear(p, PxCMD, CMD_CR, 500)) return false;
    wr(p, PxCMD, rd(p, PxCMD) & ~CMD_FRE);
    return wait_clear(p, PxCMD, CMD_FR, 500);
}

// One command in slot 0; buf is identity-mapped (physical = linear).
// cdb: a 12-byte SCSI command to send with ATA PACKET (cmd 0xA0), else null.
// quiet: don't log a failure (an ATAPI "not ready" is routine).
static bool issue(Port& d, uint8_t cmd, uint64_t lba, uint32_t count, void* buf, uint32_t bytes, bool write,
                  const uint8_t* cdb = nullptr, bool quiet = false) {
    volatile uint8_t* p = d.r;
    uint32_t* hdr = (uint32_t*)d.mem;
    uint8_t* tbl = d.mem + 0x500;
    for (int i = 0; i < 0x80 + 16; i++) tbl[i] = 0;
    hdr[0] = 5 | (write ? 1u << 6 : 0) | (cdb ? 1u << 5 : 0) | (bytes ? 1u << 16 : 0);   // FIS length 5 dwords, ATAPI, PRDT entries
    hdr[1] = 0;
    hdr[2] = (uint32_t)(uintptr_t)tbl;
    hdr[3] = 0;
    uint8_t* f = tbl;
    f[0] = 0x27; f[1] = 0x80; f[2] = cmd;                   // H2D register FIS, command
    f[4] = (uint8_t)lba; f[5] = (uint8_t)(lba >> 8); f[6] = (uint8_t)(lba >> 16);
    f[7] = 0x40;                                              // LBA mode
    f[8] = (uint8_t)(lba >> 24); f[9] = (uint8_t)(lba >> 32); f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)count; f[13] = (uint8_t)(count >> 8);
    if (cdb) {                                                // PACKET: DMA, byte count limit, the CDB in ACMD
        f[3] = bytes ? 1 : 0;                                 // DMA
        f[4] = f[8] = f[9] = f[10] = f[12] = f[13] = 0;
        f[5] = 0xFE; f[6] = 0xFF;
        f[7] = 0;
        for (int i = 0; i < 12; i++) tbl[0x40 + i] = cdb[i];
    }
    if (bytes) {
        uint32_t* prd = (uint32_t*)(tbl + 0x80);
        prd[0] = (uint32_t)(uintptr_t)buf; prd[1] = 0; prd[2] = 0; prd[3] = bytes - 1;
    }
    if (!wait_clear(p, PxTFD, 0x88, 1000)) return false;     // BSY / DRQ
    wr(p, PxIS, 0xFFFFFFFF);
    wr(p, PxCI, 1);
    for (int i = 0; i < 100000; i++) {                       // ~10 s
        uint32_t is = rd(p, PxIS);
        if (is & (1u << 30)) break;                           // task file error
        if (!(rd(p, PxCI) & 1)) return !(rd(p, PxTFD) & 1);
        io_delay(100);
    }
    if (!quiet) kprintf("ahci: command %02x at %u failed (TFD %x IS %x)\n", cdb ? cdb[0] : cmd, (uint32_t)lba, rd(p, PxTFD), rd(p, PxIS));
    d.err = (uint8_t)(rd(p, PxTFD) >> 8);                     // ATAPI: sense key in bits 7-4
    port_stop(p);                                             // recover: restart the port
    wr(p, PxSERR, 0xFFFFFFFF); wr(p, PxIS, 0xFFFFFFFF);
    wr(p, PxCMD, rd(p, PxCMD) | CMD_FRE);
    wr(p, PxCMD, rd(p, PxCMD) | CMD_ST);
    return false;
}

static void port_init(volatile uint8_t* p, bool staggered) {
    if ((rd(p, PxSSTS) & 0xF) != 3) return;                  // no device / no link
    uint32_t sig = rd(p, PxSIG);
    if (sig != 0x00000101 && sig != 0xFFFFFFFF && sig != 0xEB140101) {   // port multiplier, enclosure ...
        kprintf("ahci: port %d: device signature %x, skipped\n", (int)((p - abar - 0x100) / 0x80), sig);
        return;
    }
    if (n_cds == MAX_CDS && n_disks == MAX_DISKS) return;
    if (!port_stop(p)) { kprintf("ahci: port won't stop\n"); return; }
    uint8_t* mem = (uint8_t*)phys_try_alloc(4096);
    uint8_t* id = (uint8_t*)phys_try_alloc(4096);
    if (!mem || !id) return;
    wr(p, PxCLB, (uint32_t)(uintptr_t)mem); wr(p, PxCLBU, 0);
    wr(p, PxFB, (uint32_t)(uintptr_t)mem + 0x400); wr(p, PxFBU, 0);
    wr(p, PxSERR, 0xFFFFFFFF); wr(p, PxIS, 0xFFFFFFFF); wr(p, PxIE, 0);
    uint32_t c = rd(p, PxCMD) | CMD_POD | (staggered ? CMD_SUD : 0);
    wr(p, PxCMD, c | CMD_FRE);
    if (sig == 0xFFFFFFFF) {                                  // never started: wait for the device's first FIS
        for (int i = 0; i < 1000 && rd(p, PxSIG) == 0xFFFFFFFF; i++) io_delay(1000);
        sig = rd(p, PxSIG);
        if (sig != 0x00000101 && sig != 0xEB140101) {
            kprintf("ahci: port %d: device signature %x after start, skipped\n", (int)((p - abar - 0x100) / 0x80), sig);
            port_stop(p);
            return;
        }
    }
    bool atapi = sig == 0xEB140101;                           // CD/DVD drive
    if (atapi ? n_cds == MAX_CDS : n_disks == MAX_DISKS) { port_stop(p); return; }
    Port& d = atapi ? cds[n_cds] : disks[n_disks];
    d.r = p;
    d.mem = mem;
    wr(p, PxCMD, rd(p, PxCMD) | CMD_ST);
    if (!issue(d, atapi ? 0xA1 : 0xEC, 0, 0, id, 512, false)) { kprintf("ahci: IDENTIFY%s failed\n", atapi ? " PACKET" : ""); return; }
    uint16_t* w = (uint16_t*)id;
    if (atapi) {
        for (int i = 0; i < 20; i++) { d.model[i * 2] = (char)(w[27 + i] >> 8); d.model[i * 2 + 1] = (char)w[27 + i]; }
        int e = 40;
        while (e > 0 && d.model[e - 1] == ' ') e--;
        d.model[e] = 0;
        kprintf("ahci: CD/DVD drive %d: %s\n", n_cds, d.model);
        n_cds++;
        return;
    }
    d.sectors = (w[83] & (1 << 10)) ? (uint64_t)w[100] | (uint64_t)w[101] << 16 | (uint64_t)w[102] << 32
                                    : (uint64_t)w[60] | (uint64_t)w[61] << 16;
    for (int i = 0; i < 20; i++) { d.model[i * 2] = (char)(w[27 + i] >> 8); d.model[i * 2 + 1] = (char)w[27 + i]; }
    int e = 40;
    while (e > 0 && d.model[e - 1] == ' ') e--;
    d.model[e] = 0;
    kprintf("ahci: disk %d: %s, %u MiB\n", n_disks, d.model, (uint32_t)(d.sectors >> 11));
    n_disks++;
}

extern "C" int ahci_init(void) {
    if (ahci_done) return n_disks;
    ahci_done = true;
    PciDevice pd;
    for (int k = 0; k < 4 && pci_find_class(1, 6, 1, &pd, k); k++) {
        uint32_t bar = pci_read(pd, 0x24) & ~0xFu;
        if (!bar) continue;
        pci_write(pd, 0x04, pci_read(pd, 0x04) | 0x06);      // memory space, bus master
        abar = (volatile uint8_t*)map_mmio64(bar, 0x1100);
        if (!abar) continue;
        if ((rd(abar, 0x24) & 1)) {                          // BIOS/OS handoff
            wr(abar, 0x28, rd(abar, 0x28) | 2);
            for (int i = 0; i < 250 && (rd(abar, 0x28) & 1); i++) io_delay(1000);
        }
        wr(abar, 0x04, rd(abar, 0x04) | 0x80000000u);       // AHCI mode
        wr(abar, 0x04, rd(abar, 0x04) & ~2u);                // no interrupts: polled
        uint32_t cap = rd(abar, 0x00), pi = rd(abar, 0x0C);
        for (int i = 0; i < 32; i++)
            if (pi & (1u << i)) port_init(abar + 0x100 + i * 0x80, cap & (1u << 27));
    }
    return n_disks;
}

extern "C" uint64_t ahci_sectors(int d) { return d < n_disks ? disks[d].sectors : 0; }
extern "C" const char* ahci_model(int d) { return d < n_disks ? disks[d].model : ""; }

// count <= 128 sectors; buf identity-mapped, word aligned.
extern "C" int ahci_rw(int d, uint64_t lba, uint32_t count, void* buf, int write) {
    if (d >= n_disks || !count || lba + count > disks[d].sectors) return -1;
    return issue(disks[d], write ? 0x35 : 0x25, lba, count, buf, count * 512, write) ? 0 : -1;
}

// CD/DVD drives. A SCSI command (cdb, 12 bytes) with bytes of data in to
// buf (identity-mapped, word aligned; 0 for none). 0, or the sense key of
// the failure (2 not ready, 6 unit attention: disc changed, ...), or -1.
// A failure is always followed by REQUEST SENSE: the drive keeps its sense
// data (and may not take the next command right) until it's read.
static uint8_t cd_asc;
extern "C" int ahci_cd_count(void) { return n_cds; }
extern "C" const char* ahci_cd_model(int i) { return i < n_cds ? cds[i].model : ""; }
extern "C" int ahci_cd_asc(void) { return cd_asc; }          // additional sense code of the last failure
extern "C" int ahci_cd_packet(int i, const uint8_t* cdb, void* buf, uint32_t bytes) {
    if (i >= n_cds) return -1;
    cd_asc = 0;
    cds[i].err = 0;
    if (issue(cds[i], 0xA0, 0, 0, buf, bytes, false, cdb, true)) return 0;
    int key = cds[i].err >> 4;
    uint8_t rs[12] = { 0x03, 0, 0, 0, 18 };
    uint8_t* sense = cds[i].mem + 0x800;                      // spare room in the port's page
    for (int k = 0; k < 18; k++) sense[k] = 0;
    if (issue(cds[i], 0xA0, 0, 0, sense, 18, false, rs, true) && (sense[0] & 0x7E) == 0x70) {
        key = sense[2] & 0xF;
        cd_asc = sense[12];
    }
    return key ? key : -1;
}
