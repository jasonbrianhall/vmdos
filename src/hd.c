/* VMHD: FAT partitions on the other disks (SATA, IDE, USB) as DOS drives,
   only when asked for. VMHD.SYS (a block device driver) reserves a few
   empty drive letters at boot (2, or DEVICE=...\VMHD.SYS n, up to 8);
   VMHD.COM puts a partition in one ("VMHD D: 3") or takes it out again.
   Nothing is put in a drive by itself, so a disk vmdos finds (another
   OS's) is never touched unless chosen.

   A partition goes in only if it holds a FAT file system, isn't C: and
   isn't in another drive already. One whose partition type isn't a DOS
   one (an EFI system partition, a hidden or Linux-typed partition holding
   FAT, a GPT partition other than "basic data") needs a confirmation:
   VMHD.COM asks.

   Every request of VMHD.SYS comes here (INT 2Fh AX=5647h BX=1), as
   VMCD.SYS's go to cd.c. Sectors are relative to the partition: DOS sees
   one FAT volume per drive, and nothing outside it can be reached.

   The BIOS shows every other disk as hard disk 81h, 82h ..., as on any
   PC, so FDISK works on them as usual; but only once the DOS kernel has
   set up its drives (VMHD.SYS loading, or the first program run), so DOS
   gives none of their partitions a letter by itself. A new
   partition (a DOS type, no file system yet) goes in a drive after a Y,
   and FORMAT works on it: VMHD.SYS answers generic IOCTL (device
   parameters with the BPB FORMAT should make, access flags, locks), and a
   write to the boot sector makes DOS read the new BPB. */
#include "kernel.h"

#define HD_MAX 8
#define MAX_PARTS 48
#define ST_DONE 0x0100
#define ST_ERR(e) (0x8100 | (e))          /* 0 write protected, 2 not ready, 3 unknown command, 8 sector not found, C general */

struct part {
    int dev, num;                         /* disk (disk_dev_*), partition number on it (0: whole disk) */
    u32 start, size;
    u8 type;                              /* MBR type; 0 for GPT (see gpt) or a whole disk */
    u8 gpt;                               /* 1 basic data, 2 EFI system, 3 other */
    u8 fat;                               /* 12 / 16 / 32, 0: no FAT file system */
    char label[12];
};
static struct part parts[MAX_PARTS];
static int n_parts;

static struct unit {
    int in;                               /* a partition is in */
    struct part p;
    u8 ro, changed;
} units[HD_MAX];
static int bios_on;                       /* the other disks are BIOS disks 81h ... (the DOS kernel is up) */
static int bios_num(int d);
static int n_units, first_drive;          /* first_drive: 0 = A: */

static u8 sec[512], sec2[512], ents[512];

static int probe_dev;
static int probe_rd(u32 lba, u32 n, void *buf) { return disk_dev_rw(probe_dev, lba, n, buf, 0); }

/* A FAT boot sector at start: its type (12/16/32) and label, or 0. */
static int fat_at(int d, u32 start, u32 size, char *label)
{
    static struct fatvol v;
    probe_dev = d;
    if (!size || fat_mount(&v, probe_rd, start)) return 0;
    if (v.total > size) return 0;                                   /* bigger than its partition */
    if (disk_dev_rw(d, start, 1, sec2, 0)) return 0;
    int at = v.type == 32 ? 71 : 43;
    if (sec2[at - 5] == 0x29) {                                     /* extended boot signature: a label */
        int n = 11;
        memcpy(label, sec2 + at, 11);
        while (n && label[n - 1] == ' ') n--;
        label[n] = 0;
        if (!memcmp(label, "NO NAME", 8)) label[0] = 0;
    }
    return v.type;
}

/* The BPB FORMAT should give partition p (as MS-DOS's FORMAT would):
   FAT32 for a FAT32 type or 2 GiB and up, FAT12 up to 16 MiB, else FAT16;
   the cluster size by size. 53 bytes, from offset 11 of a boot sector. */
static void default_bpb(const struct part *p, u8 *b)
{
    u32 n = p->size;
    int t = p->type == 0x01 || n <= 32680 ? 12 : (p->type == 0x0B || p->type == 0x0C || n >= 4194304) && n >= 66600 ? 32 : 16;
    u32 spc, res = t == 32 ? 32 : 1, root = t == 32 ? 0 : 512, rds = (root * 32 + 511) / 512, fsz;
    if (t == 32) spc = n <= 532480 ? 1 : n <= 16777216 ? 8 : n <= 33554432 ? 16 : n <= 67108864 ? 32 : 64;
    else if (t == 16) spc = n <= 262144 ? 4 : n <= 524288 ? 8 : n <= 1048576 ? 16 : n <= 2097152 ? 32 : 64;
    else for (spc = 1; spc < 64 && (n - res - rds) / spc > 4084; spc *= 2) ;
    if (t == 12) {
        u32 cl = (n - res - rds) / spc;
        fsz = ((cl + 2) * 3 / 2 + 511) / 512;
    } else {
        u32 t1 = n - (res + rds), t2 = 256 * spc + 2;
        if (t == 32) t2 /= 2;
        fsz = (t1 + t2 - 1) / t2;
    }
    memset(b, 0, 53);
    b[0] = 0x00; b[1] = 0x02; b[2] = (u8)spc;
    b[3] = (u8)res; b[4] = (u8)(res >> 8); b[5] = 2;
    b[6] = (u8)root; b[7] = (u8)(root >> 8);
    if (n < 65536) { b[8] = (u8)n; b[9] = (u8)(n >> 8); } else *(u32 *)(b + 21) = n;
    b[10] = 0xF8;
    if (t != 32) { b[11] = (u8)fsz; b[12] = (u8)(fsz >> 8); }
    b[13] = 63; b[15] = 255;
    *(u32 *)(b + 17) = p->start;
    if (t == 32) {
        *(u32 *)(b + 25) = fsz;
        b[33] = 2;                                                  /* root directory: cluster 2 */
        b[37] = 1; b[39] = 6;                                       /* FS info sector, backup boot sector */
    }
}

/* What DOS is to use for p: the BPB of the FAT file system on it, or
   (none yet: FORMAT comes next) the one FORMAT would make. */
static void current_bpb(const struct part *p, u8 *b)
{
    char label[12];
    int t = fat_at(p->dev, p->start, p->size, label);
    if (!t || disk_dev_rw(p->dev, p->start, 1, sec, 0)) { default_bpb(p, b); return; }
    memcpy(b, sec + 11, 53);
    if (t != 32) memset(b + 25, 0, 28);                             /* no FAT32 fields */
}

static void add(int d, int num, u32 start, u32 size, u8 type, u8 gpt)
{
    if (n_parts == MAX_PARTS || !size) return;
    struct part *p = &parts[n_parts++];
    memset(p, 0, sizeof *p);
    p->dev = d; p->num = num; p->start = start; p->size = size; p->type = type; p->gpt = gpt;
    if (type == 0x05 || type == 0x0F || type == 0x85) return;       /* extended: its logical ones come next */
    p->fat = (u8)fat_at(d, start, size, p->label);
}

static int dos_type(u8 t) { return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C || t == 0x0E; }
static int is_ext(u8 t) { return t == 0x05 || t == 0x0F || t == 0x85; }

static const u8 guid_basic[16] = { 0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44, 0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7 };
static const u8 guid_esp[16]   = { 0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11, 0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B };

/* Every partition of every disk: MBR (with logical partitions) or GPT; a
   disk with FAT at sector 0 and no partition table is one "whole disk". */
static void scan(void)
{
    n_parts = 0;
    int nd = disk_dev_count();
    for (int d = 0; d < nd; d++) {
        u64 total = disk_dev_sectors(d);
        if (!total || disk_dev_rw(d, 0, 1, sec, 0) || sec[510] != 0x55 || sec[511] != 0xAA) continue;
        if (sec[446 + 4] == 0xEE && !disk_dev_rw(d, 1, 1, sec2, 0) && !memcmp(sec2, "EFI PART", 8)) {
            u64 el = *(u64 *)(sec2 + 72);
            u32 ne = *(u32 *)(sec2 + 80), es = *(u32 *)(sec2 + 84);
            if (es < 128 || es > 512 || ne > 256) continue;
            for (u32 i = 0; i < ne; i++) {
                u32 off = i * es;
                if (off % 512 == 0 && disk_dev_rw(d, (u32)(el + off / 512), 1, ents, 0)) break;
                u8 *g = ents + off % 512;
                static const u8 zero[16];
                if (!memcmp(g, zero, 16)) continue;
                u64 first = *(u64 *)(g + 32), last = *(u64 *)(g + 40);
                if (last < first || last >= total || last > 0xFFFFFFFFull) continue;
                add(d, (int)i + 1, (u32)first, (u32)(last - first + 1), 0,
                    !memcmp(g, guid_basic, 16) ? 1 : !memcmp(g, guid_esp, 16) ? 2 : 3);
            }
            continue;
        }
        if ((!memcmp(sec + 0x36, "FAT", 3) || !memcmp(sec + 0x52, "FAT32", 5)) && total <= 0xFFFFFFFFull) {
            add(d, 0, 0, (u32)total, 0x06, 0);                      /* a superfloppy: the whole disk */
            if (parts[n_parts - 1].fat) continue;
            n_parts--;                                              /* not FAT after all: read it as an MBR */
        }
        u8 mbr[64];
        memcpy(mbr, sec + 446, 64);
        u32 ext_base = 0;
        for (int i = 0; i < 4; i++) {
            u8 *e = mbr + i * 16;
            u32 st = *(u32 *)(e + 8), sz = *(u32 *)(e + 12);
            if (!e[4] || (u64)st + sz > total) continue;
            add(d, i + 1, st, sz, e[4], 0);
            if (is_ext(e[4]) && !ext_base) ext_base = st;
        }
        /* Logical partitions: the chain of extended boot records, numbered 5 on (as Linux does). */
        u32 ebr = ext_base;
        for (int num = 5; ebr && num < 5 + 32; num++) {
            if (disk_dev_rw(d, ebr, 1, sec2, 0) || sec2[510] != 0x55 || sec2[511] != 0xAA) break;
            u8 *e = sec2 + 446;
            u32 st = ebr + *(u32 *)(e + 8), sz = *(u32 *)(e + 12);
            u32 next = *(u32 *)(e + 16 + 8);
            u8 nt = e[16 + 4];
            if (e[4] && (u64)st + sz <= total) add(d, num, st, sz, e[4], 0);
            ebr = is_ext(nt) && next ? ext_base + next : 0;
        }
    }
}

static int is_c(const struct part *p)
{
    u32 cs, cz;
    int cd = disk_c_dev(&cs, &cz);
    return cd == p->dev && cs == p->start;
}
static int unit_of(const struct part *p)
{
    for (int u = 0; u < n_units; u++)
        if (units[u].in && units[u].p.dev == p->dev && units[u].p.start == p->start) return u;
    return -1;
}
static int dos_part(const struct part *p) { return p->gpt ? p->gpt == 1 : dos_type(p->type); }

static void put(u32 *o, const char *s) { for (; *s; s++) wr8((*o)++, (u8)*s); }

/* "partition 2 (EFI system)": for VMHD's warning and the log. */
static void kind(char *b, int n, const struct part *p)
{
    if (p->gpt) snprintf(b, n, "%s", p->gpt == 1 ? "basic data" : p->gpt == 2 ? "EFI system" : "non-DOS GPT type");
    else if (!p->num) snprintf(b, n, "no partition table");
    else snprintf(b, n, "type %02x%s", p->type, dos_type(p->type) ? "" : p->type == 0x83 ? " Linux" :
                  p->type == 0x07 ? " NTFS/exFAT" : p->type == 0xEF ? " EFI" : is_ext(p->type) ? " extended" : "");
}
static void describe(char *b, int n, const struct part *p)
{
    char k[32];
    kind(k, sizeof k, p);
    if (p->num) snprintf(b, n, "%s disk %d, partition %d (%s)", disk_dev_kind(p->dev), disk_dev_index(p->dev), p->num, k);
    else snprintf(b, n, "%s disk %d, whole disk (%s)", disk_dev_kind(p->dev), disk_dev_index(p->dev), k);
}

/* VMHD's list: the drives, then each disk (numbered, for VMHD /FDISK) and
   its partitions (numbered, for "VMHD D: n"):
     Disk 2: SATA, Samsung SSD 860, 476 GiB
        2  partition 1  type 06           40 MiB  FAT16 DATA16      = D: */
static void list(u32 o)
{
    char b[160];
    put(&o, "Drives:");
    for (int u = 0; u < n_units; u++) {
        snprintf(b, sizeof b, "  %c: %s", 'A' + first_drive + u, units[u].in ? "" : "empty");
        put(&o, b);
        if (units[u].in) {
            int k = -1;
            for (int i = 0; i < n_parts; i++)
                if (parts[i].dev == units[u].p.dev && parts[i].start == units[u].p.start) k = i;
            if (k >= 0) snprintf(b, sizeof b, "%d%s", k + 1, units[u].ro ? " /R" : "");
            else snprintf(b, sizeof b, "(disk gone)");
            put(&o, b);
        }
    }
    put(&o, "\r\n");
    int nd = disk_dev_count();
    for (int d = 0; d < nd; d++) {
        u32 mib = (u32)(disk_dev_sectors(d) >> 11);
        snprintf(b, sizeof b, "Disk %d: %s, %s, %u %s", d + 1, disk_dev_kind(d), disk_dev_name(d),
                 mib >= 10240 ? mib >> 10 : mib, mib >= 10240 ? "GiB" : "MiB");
        put(&o, b);
        int bd = bios_num(d);
        if (bd) { snprintf(b, sizeof b, "  = BIOS disk %02xh", bd); put(&o, b); }
        int any = 0;
        for (int i = 0; i < n_parts; i++) any |= parts[i].dev == d;
        put(&o, any ? "\r\n" : !mib ? "  (unplugged)\r\n" : "  (no partitions)\r\n");
        for (int i = 0; i < n_parts; i++) {
            const struct part *p = &parts[i];
            if (p->dev != d) continue;
            char k[32], where[16], fs[24];
            kind(k, sizeof k, p);
            if (p->num) snprintf(where, sizeof where, "partition %d", p->num); else snprintf(where, sizeof where, "whole disk");
            if (p->fat) snprintf(fs, sizeof fs, "FAT%d %s", p->fat, p->label);
            else snprintf(fs, sizeof fs, "%s", is_ext(p->type) ? "" : dos_part(p) ? "not formatted" : "no FAT");
            u32 pm = p->size >> 11;
            snprintf(b, sizeof b, "%5d  %-13s%-17s%6u %s  %-18s", i + 1, where, k, pm >= 10240 ? pm >> 10 : pm,
                     pm >= 10240 ? "GiB" : "MiB", fs);
            int e = (int)strlen(b);
            while (e && b[e - 1] == ' ') b[--e] = 0;
            put(&o, b);
            int u = unit_of(p);
            if (is_c(p)) put(&o, "  = C:");
            else if (u >= 0) { snprintf(b, sizeof b, "  = %c:", 'A' + first_drive + u); put(&o, b); }
            put(&o, "\r\n");
        }
    }
    wr8(o, '$');
}

/* ---- BIOS disks 81h ..., for FDISK ----
   Every disk but C:'s, in disk order, whole, through INT 13h (bios.c sends
   DL=81h on here): CHS (255 heads, 63 sectors) and the LBA extensions. */
static void set_cf(struct regs *r, int c) { if (c) r->eflags |= EFL_CF; else r->eflags &= ~EFL_CF; }
#define BIOS_MAX 15

/* Disk d's BIOS number (81h ...), 0 for none (C:'s disk, or before DOS is up). */
static int bios_num(int d)
{
    if (!bios_on) return 0;
    u32 cs, cz;
    int cd = disk_c_dev(&cs, &cz), n = 0x81;
    for (int i = 0; i < d; i++) if (i != cd) n++;
    return d == cd || n > 0x80 + BIOS_MAX ? 0 : n;
}
/* The disk behind BIOS number dl, -1 for none. */
static int bios_disk(int dl)
{
    if (!bios_on) return -1;
    int nd = disk_dev_count();
    for (int d = 0; d < nd; d++) if (bios_num(d) == dl) return d;
    return -1;
}
int hd_bios_disks(void)
{
    if (!bios_on) return 1;
    u32 cs, cz;
    int n = disk_dev_count() - (disk_c_dev(&cs, &cz) >= 0);
    return 1 + (n > BIOS_MAX ? BIOS_MAX : n);
}
/* The DOS kernel has its drives: from now on the BIOS shows the other disks. */
void hd_dos_up(void)
{
    if (bios_on) return;
    bios_on = 1;
    wr8(BDA + 0x75, (u8)hd_bios_disks());
    if (hd_bios_disks() > 1) kprintf("hd: %d more BIOS hard disk%s (81h on), for FDISK\n", hd_bios_disks() - 1,
                                     hd_bios_disks() > 2 ? "s" : "");
}

static int bios_rw(int dev, u32 lba, u32 n, u32 buf, int write)
{
    u64 total = disk_dev_sectors(dev);
    if (!n) return 0;
    if (lba >= total || n > total - lba) return 0x04;
    if (buf + n * 512 > GUEST_TOP) return 0x09;
    return disk_dev_rw(dev, lba, n, gptr(buf), write) ? (write ? 0x03 : 0x04) : 0;
}

void hd_int13(struct regs *r)
{
    static u8 status;
    int st = 0;
    u8 fn = AH(r);
    int dev = bios_disk(DL(r));
    u64 total = dev >= 0 ? disk_dev_sectors(dev) : 0;
    u32 cyls = (total > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)total) / (255 * 63);   /* no 64-bit division here */
    if (cyls > 1024) cyls = 1024;
    if (!total) { AH(r) = 0x80; set_cf(r, 1); return; }             /* gone: time out */
    switch (fn) {
    case 0x00: case 0x0D: case 0x04: case 0x0C: case 0x10: case 0x11: case 0x47: break;
    case 0x01: AH(r) = status; set_cf(r, status != 0); return;
    case 0x02: case 0x03: {
        u32 cyl = CH(r) | ((u32)(CL(r) & 0xC0) << 2), sc = CL(r) & 63, head = DH(r);
        if (!sc) { st = 0x04; break; }
        st = bios_rw(dev, (cyl * 255 + head) * 63 + sc - 1, AL(r), LIN(r->v86_es, BX(r)), fn == 3);
        if (st) AL(r) = 0;
        break; }
    case 0x08: {
        u32 mc = cyls ? cyls - 1 : 0;
        CH(r) = (u8)mc;
        CL(r) = (u8)(((mc >> 2) & 0xC0) | 63);
        DH(r) = 254;
        DL(r) = (u8)hd_bios_disks();
        break; }
    case 0x15:
        AH(r) = 3;
        CX(r) = (u16)(total > 0xFFFFFFFFull ? 0xFFFF : total >> 16);
        DX(r) = (u16)(total > 0xFFFFFFFFull ? 0xFFFF : total);
        set_cf(r, 0);
        return;
    case 0x41:
        if (BX(r) != 0x55AA) { st = 1; break; }
        BX(r) = 0xAA55; AH(r) = 0x21; CX(r) = 1;
        set_cf(r, 0);
        return;
    case 0x42: case 0x43: case 0x44: {
        u32 p = LIN(r->v86_ds, SI(r));
        u16 count = rd16(p + 2);
        u32 buf = LIN(rd16(p + 6), rd16(p + 4));
        if (rd32(p + 12)) { st = 0x04; wr16(p + 2, 0); break; }
        if (fn != 0x44) st = bios_rw(dev, rd32(p + 8), count, buf, fn == 0x43);
        if (st) wr16(p + 2, 0);
        break; }
    case 0x48: {
        u32 p = LIN(r->v86_ds, SI(r));
        if (rd16(p) < 26) { st = 1; break; }
        wr16(p, 26); wr16(p + 2, 2);
        wr32(p + 4, cyls); wr32(p + 8, 255); wr32(p + 12, 63);
        wr32(p + 16, (u32)total); wr32(p + 20, (u32)(total >> 32)); wr16(p + 24, 512);
        break; }
    default: st = 1;
    }
    status = (u8)st;
    AH(r) = (u8)st;
    set_cf(r, st != 0);
}

/* Put partition k (0-based, of the last scan) in unit u. 0, or an error:
   2 no such partition, 3 it's C:, 4 in another drive, 5 no FAT and not
   a DOS partition, 6 confirm first (the warning in warn), 7 disk error. */
static int attach(int u, int k, int ro, int confirmed, u32 warn)
{
    if (k < 0 || k >= n_parts) return 2;
    struct part *p = &parts[k];
    if (is_c(p)) return 3;
    int v = unit_of(p);
    if (v >= 0 && v != u) return 4;
    if (!p->fat && (!dos_part(p) || is_ext(p->type))) return 5;
    if ((!p->fat || !dos_part(p)) && !confirmed) {
        char d[120];
        describe(d, sizeof d, p);
        put(&warn, d);
        put(&warn, !p->fat ? ": no file system yet.\r\nFORMAT can make one on it (anything on it now is lost)."
                           : ": not a DOS partition.\r\nIt holds a FAT file system, but its partition type says it belongs\r\n"
                             "to something else (another OS, the firmware). Writing to it from DOS\r\ncould damage it.");
        wr8(warn, '$');
        return 6;
    }
    if (disk_dev_rw(p->dev, p->start, 1, sec, 0)) return 7;
    struct unit *t = &units[u];
    t->p = *p;
    t->ro = (u8)ro;
    t->in = 1;
    t->changed = 1;
    char d[120];
    describe(d, sizeof d, p);
    if (p->fat) kprintf("hd: %c: is %s, FAT%d%s\n", 'A' + first_drive + u, d, p->fat, ro ? ", read-only" : "");
    else kprintf("hd: %c: is %s, not formatted%s\n", 'A' + first_drive + u, d, ro ? ", read-only" : "");
    return 0;
}

/* Generic IOCTL (INT 21h AX=440Dh), category 08h or 48h (FAT32), function
   fn, parameter block at pb. */
static u16 genioctl(struct unit *t, int cat, int fn, u32 pb)
{
    if (cat != 0x08 && cat != 0x48) return ST_ERR(3);
    switch (fn) {
    case 0x60: {                                                    /* get device parameters */
        if (!t->in) return ST_ERR(2);
        u8 b[53];
        if (rd8(pb) & 1) current_bpb(&t->p, b); else default_bpb(&t->p, b);
        wr8(pb + 1, 5);                                             /* fixed disk */
        wr16(pb + 2, 1);                                            /* not removable */
        u32 cyl = t->p.size / (255 * 63);
        wr16(pb + 4, (u16)(cyl > 0xFFFF ? 0xFFFF : cyl));
        wr8(pb + 6, 0);
        int fat32 = b[11] == 0 && b[12] == 0;
        if (fat32 && cat == 0x08) b[6] = b[7] = 0;                  /* FAT32: FORMAT asks again, as 4860h */
        for (int i = 0; i < 25; i++) wr8(pb + 7 + i, b[i]);
        if (cat == 0x48) for (int i = 25; i < 53; i++) wr8(pb + 7 + i, b[i]);
        return ST_DONE; }
    case 0x66: {                                                    /* get media ID: serial, label, FS type */
        if (!t->in || disk_dev_rw(t->p.dev, t->p.start, 1, sec, 0)) return ST_ERR(2);
        int o = sec[0x42] == 0x29 ? 0x43 : sec[0x26] == 0x29 ? 0x27 : 0;
        if (!o) return ST_ERR(3);
        wr16(pb, 0);
        for (int i = 0; i < 23; i++) wr8(pb + 2 + i, sec[o + i]);
        return ST_DONE; }
    case 0x67: wr8(pb + 1, 1); return ST_DONE;                      /* access flag: on */
    case 0x40: case 0x47: case 0x4A: case 0x4B: case 0x6A: case 0x6B:   /* set parameters, access flag, (un)lock */
        return ST_DONE;
    }
    return ST_ERR(3);
}

/* A request from VMHD.SYS: ES:DI request header, DX:SI its BPB slots (64 bytes a unit). */
static void request(struct regs *r)
{
    u32 rh = LIN(r->v86_es, DI(r));
    int u = rd8(rh + 1), cmd = rd8(rh + 2);
    u16 st = ST_DONE;
    struct unit *t = u < n_units ? &units[u] : 0;
    if (!t) st = ST_ERR(1);
    else switch (cmd) {
    case 1:                                                         /* media check */
        if (!t->in) { st = ST_ERR(2); break; }
        wr8(rh + 14, t->changed ? 0xFF : 1);
        break;
    case 2: {                                                       /* build BPB */
        if (!t->in) { st = ST_ERR(2); break; }
        u8 bpb[53];
        current_bpb(&t->p, bpb);
        u32 b = LIN(DX(r), SI(r) + u * 64);
        for (int i = 0; i < 53; i++) wr8(b + i, bpb[i]);
        wr16(rh + 18, (u16)(SI(r) + u * 64));
        wr16(rh + 20, DX(r));
        t->changed = 0;
        break; }
    case 4: case 8: case 9: {                                       /* input, output, output with verify */
        u32 n = rd16(rh + 18), s = rd16(rh + 20);
        if (s == 0xFFFF) s = rd32(rh + 26);
        u32 buf = LIN(rd16(rh + 16), rd16(rh + 14));
        int write = cmd != 4;
        wr16(rh + 18, 0);
        if (!t->in) { st = ST_ERR(2); break; }
        if (write && t->ro) { st = ST_ERR(0); break; }
        if (s >= t->p.size || n > t->p.size - s) { st = ST_ERR(8); break; }
        if (buf + n * 512 > GUEST_TOP) { st = ST_ERR(0xC); break; }
        if (disk_dev_rw(t->p.dev, t->p.start + s, n, gptr(buf), write)) { st = ST_ERR(write ? 0xA : 0xB); break; }
        if (write && s == 0) t->changed = 1;                        /* a new boot sector (FORMAT): DOS reads the BPB again */
        wr16(rh + 18, (u16)n);
        break; }
    case 19:                                                        /* generic IOCTL (FORMAT) */
        st = genioctl(t, rd8(rh + 13), rd8(rh + 14), LIN(rd16(rh + 21), rd16(rh + 19)));
        break;
    case 23: wr8(rh + 1, 0); break;                                 /* get logical device: one letter a drive */
    case 24: break;
    default: st = ST_ERR(3);
    }
    if (st & 0x8000) dbg(1, "hd: %c: request %d (%02x) failed: %x\n", 'A' + first_drive + u, cmd,
                         cmd == 19 ? rd8(rh + 14) : 0, st & 0xFF);
    wr16(rh + 3, st);
}

/* INT 2Fh AX=5647h. BX=0 (VMHD.SYS init): CX units wanted, DL its first
   drive (0 = A:); AX = units given. BX=1: a request (see request). BX=2
   (VMHD.COM): list into ES:DI ($-terminated); AX = number of units, DL
   the first drive. BX=3: put partition DL (1-based, as listed) in drive
   CL (0 = A:), DH bit 0 read-only, bit 1 confirmed; AX = 0 or an error
   (see attach; 1: not a VMHD drive), a warning in ES:DI for 6. BX=4:
   take drive CL's partition out; AX = 0 or 1. */
void hd_api(struct regs *r)
{
    switch (BX(r)) {
    case 0:
        n_units = CX(r) < 1 ? 1 : CX(r) > HD_MAX ? HD_MAX : CX(r);
        first_drive = DL(r);
        hd_dos_up();                                                /* CONFIG.SYS: the kernel has its drives */
        kprintf("hd: VMHD drives %c: to %c:, empty\n", 'A' + first_drive, 'A' + first_drive + n_units - 1);
        AX(r) = (u16)n_units;
        return;
    case 1:
        request(r);
        return;
    case 2:
        scan();
        list(LIN(r->v86_es, DI(r)));
        AX(r) = (u16)n_units;
        DL(r) = (u8)first_drive;
        return;
    case 3: {
        int u = CL(r) - first_drive;
        if (u < 0 || u >= n_units) { AX(r) = 1; return; }
        scan();
        AX(r) = (u16)attach(u, DL(r) - 1, DH(r) & 1, DH(r) & 2, LIN(r->v86_es, DI(r)));
        return; }
    case 4: {
        int u = CL(r) - first_drive;
        if (u < 0 || u >= n_units) { AX(r) = 1; return; }
        if (units[u].in) kprintf("hd: %c: emptied\n", 'A' + first_drive + u);
        units[u].in = 0;
        units[u].changed = 1;
        AX(r) = 0;
        return; }
    }
    AX(r) = 0xFFFF;
}
