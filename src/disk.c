/* Drive C: (BIOS disk 80h). Two backends:
   - a FAT partition on a real disk (SATA/AHCI, ahci.cpp, or a USB stick,
     usb.cpp's mass storage): the default when one
     holds KERNEL.SYS, so a vmdos ESP prepared by "make esp" (or by hand)
     is C: and changes are kept. DOS sees a virtual disk: sector 0 is a
     made-up MBR with that one partition (GPT disks too), the partition's
     sectors are the real ones, everything else reads as zeros and can't
     be written, so other partitions are safe.
   - the RAM disk (dos.img as a boot module): with c=ram, or when no disk
     partition qualifies. Changes are lost at power-off.
   vmdos.efi passes esp=LBA and espsig= (the partition it was started from),
   which is preferred over others. */
#include "kernel.h"

int ahci_init(void);
u64 ahci_sectors(int d);
const char *ahci_model(int d);
int ahci_rw(int d, u64 lba, u32 count, void *buf, int write);
int ide_init(void);
u64 ide_sectors(int d);
const char *ide_model(int d);
int ide_rw(int d, u64 lba, u32 count, void *buf, int write);
int usb_msd_count(void);
u64 usb_msd_sectors(int i);
const char *usb_msd_name(int i);
int usb_msd_rw(int i, u64 lba, u32 count, void *buf, int write);
void usb_kick_ports(void);

/* The real disks: AHCI (SATA) disks, IDE disks and USB mass storage,
   512-byte sectors. */
#define MAX_DEVS 16
static struct { int usb, ide, idx; u64 sectors; const char *name; } dev[MAX_DEVS];
static int n_dev;

u32 disk_sectors;                     /* size DOS sees */
static int on_dev, cdev;              /* C: is on dev[cdev] */
static int usb_seen;                  /* USB disks taken into dev[] */
static const char *dev_kind(int d) { return dev[d].usb ? "USB" : dev[d].ide ? "IDE" : "SATA"; }
static u32 pstart, psize;
static u8 vmbr[512];
static u8 *bounce;                    /* 64 KiB, identity-mapped, for AHCI DMA */
#define BOUNCE_SECS 128

/* SATA and IDE transfers run with interrupts on (once they are set up):
   a long read (IDE PIO under a VM above all) would otherwise hold off the
   timer, and with it the sound, for its whole length: a gap you hear, and
   lost ticks. The timer leaves the disks (and the CD) alone meanwhile.
   USB stays as it was: usb_tick runs from the timer too. */
static volatile int dev_busy;
int disk_busy(void) { return dev_busy; }

static int dev_rw(int d, u32 lba, u32 n, void *buf, int write)
{
    if (dev[d].usb) return usb_msd_rw(dev[d].idx, lba, n, buf, write);
    u32 fl;
    __asm__ volatile("pushfl; popl %0" : "=r"(fl));
    dev_busy = 1;
    if (usb_ready) sti();
    int e;
    if (dev[d].ide) e = ide_rw(dev[d].idx, lba, n, buf, write) ? -1 : 0;    /* PIO: any buffer */
    else {
        if (write) memcpy(bounce, buf, n * 512);
        e = ahci_rw(dev[d].idx, lba, n, bounce, write) ? -1 : 0;
        if (!e && !write) memcpy(buf, bounce, n * 512);
    }
    if (!(fl & EFL_IF)) __asm__ volatile("cli" ::: "memory");
    dev_busy = 0;
    return e;
}

/* Raw reads from dev[probe_disk] while probing. */
static int probe_disk;
static int probe_rd(u32 lba, u32 n, void *buf)
{
    for (u32 i = 0; i < n; i += BOUNCE_SECS) {
        u32 k = n - i < BOUNCE_SECS ? n - i : BOUNCE_SECS;
        if (dev_rw(probe_disk, lba + i, k, (u8 *)buf + i * 512, 0)) return -1;
    }
    return 0;
}

/* ---- the cache, for C: on a real disk ----
   DOS asks for one sector at a time (a file read in small pieces, every
   FAT and directory sector), and each request was a disk command of its
   own. Reads now load 64 sectors (32 KiB, aligned to the partition start)
   at once and serve the rest from memory; writes go into the cached copy
   at once and, when sequential, are gathered into one run (up to 64 KiB)
   that goes to the disk as one command: when the next write isn't
   contiguous, when the run is full, 50 ms after it started (timer tick),
   on INT 13h reset, before a restart and when vmdos stops. diskcache=MB
   sizes the read cache (default 8, 0: off). */
#define LINE 64                                   /* sectors a cache line */
#define RUN_MAX BOUNCE_SECS                       /* sectors a gathered write */
static struct cline { u32 lba, n, used; } *lines; /* lba ~0: empty */
static u8 *cdata;
static u32 n_lines, stamp;
static u8 *run;                                   /* pending write run */
static u32 run_lba, run_n, run_since;
static int flushing;

static int dev_io(u32 lba, u32 n, void *buf, int write)  /* any length, through the bounce buffer */
{
    u8 *b = buf;
    while (n) {
        u32 k = n < BOUNCE_SECS ? n : BOUNCE_SECS;
        if (dev_rw(cdev, lba, k, b, write)) return -1;
        lba += k; n -= k; b += k * 512;
    }
    return 0;
}

static void cache_init(void)
{
    u32 mb = 8;
    const char *o = strstr(cmdline, "diskcache=");
    if (o) { mb = 0; for (o += 10; *o >= '0' && *o <= '9'; o++) mb = mb * 10 + (u32)(*o - '0'); }
    if (mb > 64) mb = 64;
    if (mb && mb * 2 > phys_free() >> 20) mb = phys_free() >> 21;    /* never more than half the free RAM */
    run = phys_try_alloc(RUN_MAX * 512);
    n_lines = mb * 32;                                              /* 32 KiB lines */
    if (n_lines) {
        lines = phys_try_alloc(n_lines * sizeof *lines);
        cdata = phys_try_alloc(n_lines * LINE * 512);
        if (!lines || !cdata) n_lines = 0;
        for (u32 i = 0; i < n_lines; i++) lines[i].lba = ~0u;
    }
    kprintf("disk: cache %u MiB%s\n", n_lines / 32, run ? ", writes gathered" : "");
}

/* Put the pending run on the disk. */
void disk_flush(void)
{
    if (!run_n || flushing) return;
    flushing = 1;
    if (dev_io(run_lba, run_n, run, 1)) kprintf("disk: write of %u sectors at %u failed\n", run_n, run_lba);
    run_n = 0;
    flushing = 0;
}

/* Timer tick: a run doesn't wait more than 50 ms. */
void disk_tick(void) { if (!dev_busy && run_n && ticks - run_since > 50) disk_flush(); }

/* Sectors of [lba, lba + n) that are in the pending run: copied over buf. */
static void overlay_run(u32 lba, u32 n, u8 *buf)
{
    if (!run_n || lba >= run_lba + run_n || lba + n <= run_lba) return;
    u32 a = lba > run_lba ? lba : run_lba, b = lba + n < run_lba + run_n ? lba + n : run_lba + run_n;
    memcpy(buf + (a - lba) * 512, run + (a - run_lba) * 512, (b - a) * 512);
}

static u8 *line_data(u32 i) { return cdata + i * LINE * 512; }

static int line_get(u32 base, u32 *out)          /* the line holding base, loaded if need be */
{
    u32 victim = 0, oldest = ~0u;
    for (u32 i = 0; i < n_lines; i++) {
        if (lines[i].lba == base) { lines[i].used = ++stamp; *out = i; return 0; }
        if (lines[i].used < oldest) { oldest = lines[i].used; victim = i; }
    }
    u32 n = disk_sectors - base < LINE ? disk_sectors - base : LINE;
    lines[victim].lba = ~0u;
    if (dev_io(base, n, line_data(victim), 0)) return -1;
    overlay_run(base, n, line_data(victim));     /* the disk doesn't have those yet */
    lines[victim].lba = base; lines[victim].n = n; lines[victim].used = ++stamp;
    *out = victim;
    return 0;
}

/* The partition's sectors, through the cache. */
static int part_read(u32 lba, u32 n, u8 *d)
{
    if (!n_lines || n >= LINE) {                  /* big reads straight from the disk (they'd only evict) */
        if (dev_io(lba, n, d, 0)) return -1;
        overlay_run(lba, n, d);
        for (u32 i = 0; i < n_lines; i++)         /* cached lines may be newer than the disk */
            if (lines[i].lba != ~0u && lines[i].lba < lba + n && lines[i].lba + lines[i].n > lba) {
                u32 a = lines[i].lba > lba ? lines[i].lba : lba;
                u32 b = lines[i].lba + lines[i].n < lba + n ? lines[i].lba + lines[i].n : lba + n;
                memcpy(d + (a - lba) * 512, line_data(i) + (a - lines[i].lba) * 512, (b - a) * 512);
            }
        return 0;
    }
    while (n) {
        u32 base = pstart + (lba - pstart) / LINE * LINE, off = lba - base, k = LINE - off, li;
        if (k > n) k = n;
        if (line_get(base, &li)) return -1;
        memcpy(d, line_data(li) + off * 512, k * 512);
        lba += k; n -= k; d += k * 512;
    }
    return 0;
}

static int part_write(u32 lba, u32 n, const u8 *s)
{
    for (u32 i = 0; i < n_lines; i++)              /* cached copies first */
        if (lines[i].lba != ~0u && lines[i].lba < lba + n && lines[i].lba + lines[i].n > lba) {
            u32 a = lines[i].lba > lba ? lines[i].lba : lba;
            u32 b = lines[i].lba + lines[i].n < lba + n ? lines[i].lba + lines[i].n : lba + n;
            memcpy(line_data(i) + (a - lines[i].lba) * 512, s + (a - lba) * 512, (b - a) * 512);
        }
    if (!run) return dev_io(lba, n, (void *)s, 1);
    if (run_n && lba >= run_lba && lba + n <= run_lba + run_n) {    /* rewrites sectors of the run */
        memcpy(run + (lba - run_lba) * 512, s, n * 512);
        return 0;
    }
    if (run_n && lba == run_lba + run_n && run_n + n <= RUN_MAX) {  /* carries it on */
        memcpy(run + run_n * 512, s, n * 512);
        run_n += n;
        return 0;
    }
    disk_flush();
    if (n >= RUN_MAX) return dev_io(lba, n, (void *)s, 1);
    memcpy(run, s, n * 512);
    run_lba = lba; run_n = n; run_since = ticks;
    return 0;
}

/* ---- I/O as DOS sees the disk: 0 or a BIOS status (4 sector not found, 3 write protected, 0x20 controller) ---- */
int disk_read(u32 lba, u32 n, void *buf)
{
    if (lba >= disk_sectors || n > disk_sectors - lba) return 0x04;
    if (!on_dev) { memcpy(buf, disk_image + lba * 512, n * 512); return 0; }
    u8 *d = buf;
    if (lba < pstart) {                                        /* MBR + gap before the partition */
        u32 k = pstart - lba < n ? pstart - lba : n;
        for (u32 i = 0; i < k; i++) {
            if (lba + i == 0) memcpy(d + i * 512, vmbr, 512);
            else memset(d + i * 512, 0, 512);
        }
        lba += k; n -= k; d += k * 512;
    }
    if (n && part_read(lba, n, d)) return 0x20;
    return 0;
}

int disk_write(u32 lba, u32 n, const void *buf)
{
    if (lba >= disk_sectors || n > disk_sectors - lba) return 0x04;
    if (!on_dev) { memcpy(disk_image + lba * 512, buf, n * 512); return 0; }
    if (lba < pstart) return 0x03;                             /* only the partition is writable */
    return part_write(lba, n, buf) ? 0x20 : 0;
}

/* The FAT partition DOS boots from, through the same view (for cd.c / bios.c). */
static int view_rd(u32 lba, u32 n, void *buf) { return disk_read(lba, n, buf) ? -1 : 0; }
int disk_volume(struct fatvol *v)
{
    u8 m[512];
    if (disk_read(0, 1, m)) return -1;
    u32 base = 0;
    if (memcmp(m + 0x52, "FAT32", 5) && memcmp(m + 0x36, "FAT", 3)) {
        int best = -1;
        for (int i = 0; i < 4; i++) {
            u8 *e = m + 446 + i * 16, t = e[4];
            if (t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C || t == 0x0E || t == 0xEF)
                if (best < 0 || e[0] == 0x80) { best = i; base = *(u32 *)(e + 8); }
        }
        if (best < 0) return -1;
    }
    return fat_mount(v, view_rd, base);
}

/* ---- choosing the partition ----
   Each FAT partition holding KERNEL.SYS scores: the one vmdos.efi started
   from (esp=LBA and espsig=, the disk signature or GPT partition GUID from
   its device path) far above the rest; without a hint (GRUB), a USB stick
   before an internal disk. */
static u32 hint;
static u8 hint_sig[16];
static int hint_sig_len;              /* 0 none, 4 MBR disk signature, 16 GPT partition GUID */

static int candidate(int d, u64 start, u64 size, const u8 *sig, int sig_len)
{
    if (!size || start + size > dev[d].sectors || start + size > 0xFFFFFFFFull) return 0;
    static struct fatvol v;
    probe_disk = d;
    if (fat_mount(&v, probe_rd, (u32)start)) return 0;
    u32 c, sz;
    if (fat_lookup(&v, "\\KERNEL.SYS", &c, &sz)) return 0;
    int q = dev[d].usb ? 3 : 2;
    if (hint && hint == start) {
        q = 6;
        if (hint_sig_len && sig_len == hint_sig_len && !memcmp(sig, hint_sig, (size_t)sig_len)) q = 8;
        else if (hint_sig_len) q = 4;                               /* same place, another disk */
    }
    return q;
}

static void use(int d, u32 start, u32 size)
{
    static struct fatvol v;
    probe_disk = d;
    fat_mount(&v, probe_rd, start);
    on_dev = 1; cdev = d; pstart = start; psize = size;
    disk_sectors = start + size;
    memset(vmbr, 0, 512);
    vmbr[0] = 0xCD; vmbr[1] = 0x18;                             /* INT 18h */
    u8 *e = vmbr + 446;
    e[0] = 0x80;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF; e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    e[4] = v.type == 32 ? 0x0C : v.type == 16 ? 0x0E : 0x01;
    *(u32 *)(e + 8) = start; *(u32 *)(e + 12) = size;
    vmbr[510] = 0x55; vmbr[511] = 0xAA;
    kprintf("disk: C: is FAT%d partition at LBA %u (%u MiB) on %s disk %d (%s)\n",
            v.type, start, size >> 11, dev_kind(d), dev[d].idx, dev[d].name);
    cache_init();
}

static int scan(int d, u32 *bs, u32 *bz)
{
    static u8 s0[512], s1[512], ent[512];
    probe_disk = d;
    if (probe_rd(0, 1, s0) || s0[510] != 0x55 || s0[511] != 0xAA) return 0;
    int best = 0;
    #define TRY(st, sz, sg, sl) do { int q = candidate(d, st, sz, sg, sl); if (q > best) { best = q; *bs = (u32)(st); *bz = (u32)(sz); } } while (0)
    if (s0[446 + 4] == 0xEE && !probe_rd(1, 1, s1) && !memcmp(s1, "EFI PART", 8)) {
        u64 el = *(u64 *)(s1 + 72);
        u32 ne = *(u32 *)(s1 + 80), es = *(u32 *)(s1 + 84);
        if (es < 128 || es > 512 || ne > 256) ne = 0;
        for (u32 i = 0; i < ne && best < 8; i++) {
            u32 off = i * es;
            if (off % 512 == 0 && probe_rd((u32)(el + off / 512), 1, ent)) break;
            u8 *g = ent + off % 512;
            static const u8 zero[16];
            if (!memcmp(g, zero, 16)) continue;
            u64 first = *(u64 *)(g + 32), last = *(u64 *)(g + 40);
            if (last >= first) TRY(first, last - first + 1, g + 16, 16);
        }
    } else if (!memcmp(s0 + 0x36, "FAT", 3) || !memcmp(s0 + 0x52, "FAT32", 5)) {
        TRY(0, dev[d].sectors, s0, 0);                          /* superfloppy: no partition table */
    } else {
        for (int i = 0; i < 4 && best < 8; i++) {
            u8 *e = s0 + 446 + i * 16;
            if (e[4] && e[4] != 0x05 && e[4] != 0x0F) TRY(*(u32 *)(e + 8), *(u32 *)(e + 12), s0 + 440, 4);
        }
    }
    return best;
}

/* Busy-wait ms milliseconds by the PIT (channel 0 counts down once per ms;
   this runs before interrupts are on, so the tick counter doesn't move). */
static u16 pit_count(void) { outb(0x43, 0x00); u16 c = inb(0x40); return c | (u16)(inb(0x40) << 8); }
static void sleep_ms(int ms)
{
    u16 prev = pit_count();
    while (ms > 0) {
        u16 c = pit_count();
        if (c > prev) ms--;                                     /* reloaded: a millisecond went by */
        prev = c;
    }
}

static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1; }

void disk_init(void)
{
    const char *h = strstr(cmdline, "esp=");
    if (h) for (h += 4; *h >= '0' && *h <= '9'; h++) hint = hint * 10 + (u32)(*h - '0');
    h = strstr(cmdline, "espsig=");                             /* hex bytes in disk order */
    if (h) {
        h += 7;
        while (hint_sig_len < 16 && hexval(h[0]) >= 0 && hexval(h[1]) >= 0) {
            hint_sig[hint_sig_len++] = (u8)(hexval(h[0]) << 4 | hexval(h[1]));
            h += 2;
        }
        if (hint_sig_len != 4 && hint_sig_len != 16) hint_sig_len = 0;
    }
    int want_ram = strstr(cmdline, "c=ram") != 0;
    if (!want_ram) {
        bounce = phys_try_alloc(BOUNCE_SECS * 512);
        int na = (bounce && !strstr(cmdline, "ahci=off")) ? ahci_init() : 0;
        for (int i = 0; i < na && n_dev < MAX_DEVS; i++) {
            dev[n_dev].usb = 0; dev[n_dev].idx = i; dev[n_dev].sectors = ahci_sectors(i); dev[n_dev].name = ahci_model(i);
            n_dev++;
        }
        int ni = !strstr(cmdline, "ide=off") ? ide_init() : 0;
        for (int i = 0; i < ni && n_dev < MAX_DEVS; i++) {
            dev[n_dev].ide = 1; dev[n_dev].idx = i; dev[n_dev].sectors = ide_sectors(i); dev[n_dev].name = ide_model(i);
            n_dev++;
        }
        /* USB sticks show up a little after the controller reset (USB 3 link
           training, slow sticks): keep polling for a while, longer when
           vmdos.efi says it came from a USB device (bootdev=usb), until the
           partition it started from (or, without a hint, any) turns up. */
        int boot_usb = strstr(cmdline, "bootdev=usb") != 0;
        int wait_ms = boot_usb ? 10000 : usb_msd_count() || hint ? 2000 : 1000;
        int bd = -1, bq = 0, waited = 0;
        u32 bs = 0, bz = 0;
        for (int d = 0; d < n_dev; d++) {
            u32 s = 0, z = 0;
            int q = scan(d, &s, &z);
            if (q > bq) { bq = q; bd = d; bs = s; bz = z; }
        }
        for (;;) {
            while (usb_seen < usb_msd_count() && n_dev < MAX_DEVS) {
                int i = usb_seen++, d = n_dev++;
                dev[d].usb = 1; dev[d].idx = i; dev[d].sectors = usb_msd_sectors(i); dev[d].name = usb_msd_name(i);
                u32 s = 0, z = 0;
                int q = scan(d, &s, &z);
                kprintf("disk: USB disk %d (%s, %u MiB): %s\n", i, dev[d].name, (u32)(dev[d].sectors >> 11),
                        q >= 8 ? "the boot partition" : q ? "has KERNEL.SYS" : "no FAT partition with KERNEL.SYS");
                if (q > bq) { bq = q; bd = d; bs = s; bz = z; }
            }
            int good = hint ? bq >= (hint_sig_len ? 8 : 6) : bq > 0;
            if (good && !(boot_usb && bd >= 0 && !dev[bd].usb && waited < wait_ms)) break;
            if (waited >= wait_ms) break;
            if (waited == 0) kprintf("disk: waiting for USB disks (up to %d s)\n", wait_ms / 1000);
            usb_tick();
            if (waited % 1000 == 500) usb_kick_ports();             /* stuck USB 3 links: warm reset */
            sleep_ms(20);
            waited += 20;
        }
        if (bd >= 0) { use(bd, bs, bz); return; }
        if (n_dev) kprintf("disk: no FAT partition with KERNEL.SYS on the %d disk%s found\n", n_dev, n_dev == 1 ? "" : "s");
    }
    if (!disk_image)
        panic("No C: drive: no FAT partition with KERNEL.SYS on a SATA (AHCI), IDE or USB disk, and no RAM disk "
              "(build with RAMDISK=1: dos.img next to vmdos.efi, GRUB module, QEMU -initrd).");
    disk_sectors = disk_size / 512;
    kprintf("disk: C: is the RAM disk (dos.img, %u MiB)%s\n", disk_size >> 20,
            want_ram ? "" : "; changes are lost at power-off");
}

/* ---- the other disks, for VMHD (hd.c) ----
   Any disk by its dev[] index; USB disks plugged in since boot are taken
   in when the list is asked for. */
int disk_dev_count(void)
{
    while (usb_seen < usb_msd_count() && n_dev < MAX_DEVS) {
        int i = usb_seen++, d = n_dev++;
        dev[d].usb = 1; dev[d].idx = i; dev[d].sectors = usb_msd_sectors(i); dev[d].name = usb_msd_name(i);
    }
    return n_dev;
}
u64 disk_dev_sectors(int d) { return dev[d].usb ? usb_msd_sectors(dev[d].idx) : dev[d].sectors; }   /* 0: unplugged */
const char *disk_dev_name(int d) { return dev[d].name; }
const char *disk_dev_kind(int d) { return dev_kind(d); }
int disk_dev_index(int d) { return dev[d].idx; }
int disk_dev_rw(int d, u32 lba, u32 n, void *buf, int write)      /* any length */
{
    u8 *b = buf;
    while (n) {
        u32 k = n < BOUNCE_SECS ? n : BOUNCE_SECS;
        if (dev_rw(d, lba, k, b, write)) return -1;
        lba += k; n -= k; b += k * 512;
    }
    return 0;
}
/* The disk and partition C: is on; -1 if C: isn't on a real disk. */
int disk_c_dev(u32 *start, u32 *size) { *start = pstart; *size = psize; return on_dev ? cdev : -1; }

const char *disk_kind(void) { return on_dev ? (dev[cdev].usb ? "USB disk" : dev[cdev].ide ? "IDE disk" : "SATA disk") : "RAM disk"; }
