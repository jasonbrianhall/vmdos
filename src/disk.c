/* Drive C: (BIOS disk 80h). Two backends:
   - a FAT partition on a real disk (AHCI, ahci.cpp): the default when one
     holds KERNEL.SYS, so a vmdos ESP prepared by "make esp" (or by hand)
     is C: and changes are kept. DOS sees a virtual disk: sector 0 is a
     made-up MBR with that one partition (GPT disks too), the partition's
     sectors are the real ones, everything else reads as zeros and can't
     be written, so other partitions are safe.
   - the RAM disk (dos.img as a boot module): with c=ram, or when no disk
     partition qualifies. Changes are lost at power-off.
   vmdos.efi passes esp=LBA (the partition it was started from), which is
   preferred over others. */
#include "kernel.h"

int ahci_init(void);
u64 ahci_sectors(int d);
const char *ahci_model(int d);
int ahci_rw(int d, u64 lba, u32 count, void *buf, int write);

u32 disk_sectors;                     /* size DOS sees */
static int on_ahci, adisk;
static u32 pstart, psize;
static u8 vmbr[512];
static u8 *bounce;                    /* 64 KiB, identity-mapped, for DMA */
#define BOUNCE_SECS 128

/* Raw reads from AHCI disk 'adisk' while probing. */
static int probe_disk;
static int probe_rd(u32 lba, u32 n, void *buf)
{
    for (u32 i = 0; i < n; i += BOUNCE_SECS) {
        u32 k = n - i < BOUNCE_SECS ? n - i : BOUNCE_SECS;
        if (ahci_rw(probe_disk, lba + i, k, bounce, 0)) return -1;
        memcpy((u8 *)buf + i * 512, bounce, k * 512);
    }
    return 0;
}

/* ---- I/O as DOS sees the disk: 0 or a BIOS status (4 sector not found, 3 write protected, 0x20 controller) ---- */
int disk_read(u32 lba, u32 n, void *buf)
{
    if (lba >= disk_sectors || n > disk_sectors - lba) return 0x04;
    if (!on_ahci) { memcpy(buf, disk_image + lba * 512, n * 512); return 0; }
    u8 *d = buf;
    while (n) {
        u32 k;
        if (lba < pstart) {                                    /* MBR + gap before the partition */
            k = pstart - lba < n ? pstart - lba : n;
            for (u32 i = 0; i < k; i++) {
                if (lba + i == 0) memcpy(d + i * 512, vmbr, 512);
                else memset(d + i * 512, 0, 512);
            }
        } else {
            k = n < BOUNCE_SECS ? n : BOUNCE_SECS;
            if (ahci_rw(adisk, lba, k, bounce, 0)) return 0x20;
            memcpy(d, bounce, k * 512);
        }
        lba += k; n -= k; d += k * 512;
    }
    return 0;
}

int disk_write(u32 lba, u32 n, const void *buf)
{
    if (lba >= disk_sectors || n > disk_sectors - lba) return 0x04;
    if (!on_ahci) { memcpy(disk_image + lba * 512, buf, n * 512); return 0; }
    if (lba < pstart) return 0x03;                             /* only the partition is writable */
    const u8 *s = buf;
    while (n) {
        u32 k = n < BOUNCE_SECS ? n : BOUNCE_SECS;
        memcpy(bounce, s, k * 512);
        if (ahci_rw(adisk, lba, k, bounce, 1)) return 0x20;
        lba += k; n -= k; s += k * 512;
    }
    return 0;
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

/* ---- choosing the partition ---- */
static int candidate(int d, u64 start, u64 size, u32 hint)
{
    if (!size || start + size > ahci_sectors(d) || start + size > 0xFFFFFFFFull) return 0;
    static struct fatvol v;
    probe_disk = d;
    if (fat_mount(&v, probe_rd, (u32)start)) return 0;
    u32 c, sz;
    if (fat_lookup(&v, "\\KERNEL.SYS", &c, &sz)) return 0;
    if (hint && hint != start) return 1;                        /* usable, but not the one vmdos.efi came from */
    return 2;
}

static void use(int d, u32 start, u32 size)
{
    static struct fatvol v;
    probe_disk = d;
    fat_mount(&v, probe_rd, start);
    on_ahci = 1; adisk = d; pstart = start; psize = size;
    disk_sectors = start + size;
    memset(vmbr, 0, 512);
    vmbr[0] = 0xCD; vmbr[1] = 0x18;                             /* INT 18h */
    u8 *e = vmbr + 446;
    e[0] = 0x80;
    e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF; e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
    e[4] = v.type == 32 ? 0x0C : v.type == 16 ? 0x0E : 0x01;
    *(u32 *)(e + 8) = start; *(u32 *)(e + 12) = size;
    vmbr[510] = 0x55; vmbr[511] = 0xAA;
    kprintf("disk: C: is FAT%d partition at LBA %u (%u MiB) on AHCI disk %d (%s)\n",
            v.type, start, size >> 11, d, ahci_model(d));
}

static int scan(int d, u32 hint, u32 *bs, u32 *bz)
{
    static u8 s0[512], s1[512], ent[512];
    probe_disk = d;
    if (probe_rd(0, 1, s0) || s0[510] != 0x55 || s0[511] != 0xAA) return 0;
    int best = 0;
    #define TRY(st, sz) do { int q = candidate(d, st, sz, hint); if (q > best) { best = q; *bs = (u32)(st); *bz = (u32)(sz); } } while (0)
    if (s0[446 + 4] == 0xEE && !probe_rd(1, 1, s1) && !memcmp(s1, "EFI PART", 8)) {
        u64 el = *(u64 *)(s1 + 72);
        u32 ne = *(u32 *)(s1 + 80), es = *(u32 *)(s1 + 84);
        if (es < 128 || es > 512 || ne > 256) ne = 0;
        for (u32 i = 0; i < ne && best < 2; i++) {
            u32 off = i * es;
            if (off % 512 == 0 && probe_rd((u32)(el + off / 512), 1, ent)) break;
            u8 *g = ent + off % 512;
            static const u8 zero[16];
            if (!memcmp(g, zero, 16)) continue;
            u64 first = *(u64 *)(g + 32), last = *(u64 *)(g + 40);
            if (last >= first) TRY(first, last - first + 1);
        }
    } else if (!memcmp(s0 + 0x36, "FAT", 3) || !memcmp(s0 + 0x52, "FAT32", 5)) {
        TRY(0, ahci_sectors(d));                                /* superfloppy: no partition table */
    } else {
        for (int i = 0; i < 4 && best < 2; i++) {
            u8 *e = s0 + 446 + i * 16;
            if (e[4] && e[4] != 0x05 && e[4] != 0x0F) TRY(*(u32 *)(e + 8), *(u32 *)(e + 12));
        }
    }
    return best;
}

void disk_init(void)
{
    u32 hint = 0;
    const char *h = strstr(cmdline, "esp=");
    if (h) for (h += 4; *h >= '0' && *h <= '9'; h++) hint = hint * 10 + (u32)(*h - '0');
    int want_ram = strstr(cmdline, "c=ram") != 0;
    if (!want_ram && !strstr(cmdline, "ahci=off")) {
        bounce = phys_try_alloc(BOUNCE_SECS * 512);
        int n = bounce ? ahci_init() : 0;
        int bd = -1, bq = 0;
        u32 bs = 0, bz = 0;
        for (int d = 0; d < n && bq < 2; d++) {
            u32 s = 0, z = 0;
            int q = scan(d, hint, &s, &z);
            if (q > bq) { bq = q; bd = d; bs = s; bz = z; }
        }
        if (bd >= 0) { use(bd, bs, bz); return; }
        if (n) kprintf("disk: no FAT partition with KERNEL.SYS on the AHCI disks\n");
    }
    if (!disk_image)
        panic("No C: drive: no FAT partition with KERNEL.SYS on an AHCI (SATA) disk, and no RAM disk "
              "(build with RAMDISK=1: dos.img next to vmdos.efi, GRUB module, QEMU -initrd).");
    disk_sectors = disk_size / 512;
    kprintf("disk: C: is the RAM disk (dos.img, %u MiB)%s\n", disk_size >> 20,
            want_ram ? "" : "; changes are lost at power-off");
}

const char *disk_kind(void) { return on_ahci ? "disk" : "RAM disk"; }
