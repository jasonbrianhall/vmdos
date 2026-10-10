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
   one FAT volume per drive, and nothing outside it can be reached. */
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
    u8 bpb[53];                           /* from its boot sector: DOS's BPB, FAT32 fields too */
} units[HD_MAX];
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

/* VMHD's list: the drives, then each disk and its partitions, numbered
   for "VMHD D: n":
     SATA disk 1: Samsung SSD 860
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
    if (!n_parts) put(&o, "No partitions found on the other disks.\r\n");
    for (int i = 0; i < n_parts; i++) {
        const struct part *p = &parts[i];
        if (!i || parts[i - 1].dev != p->dev) {
            snprintf(b, sizeof b, "%s disk %d: %s\r\n", disk_dev_kind(p->dev), disk_dev_index(p->dev), disk_dev_name(p->dev));
            put(&o, b);
        }
        char k[32], where[16], fs[24];
        kind(k, sizeof k, p);
        if (p->num) snprintf(where, sizeof where, "partition %d", p->num); else snprintf(where, sizeof where, "whole disk");
        if (p->fat) snprintf(fs, sizeof fs, "FAT%d %s", p->fat, p->label);
        else snprintf(fs, sizeof fs, "%s", is_ext(p->type) ? "" : "no FAT");
        u32 mib = p->size >> 11;
        snprintf(b, sizeof b, "%5d  %-13s%-17s%6u %s  %-18s", i + 1, where, k, mib >= 10240 ? mib >> 10 : mib,
                 mib >= 10240 ? "GiB" : "MiB", fs);
        int e = (int)strlen(b);
        while (e && b[e - 1] == ' ') b[--e] = 0;
        put(&o, b);
        int u = unit_of(p);
        if (is_c(p)) put(&o, "  = C:");
        else if (u >= 0) { snprintf(b, sizeof b, "  = %c:", 'A' + first_drive + u); put(&o, b); }
        put(&o, "\r\n");
    }
    wr8(o, '$');
}

/* Put partition k (0-based, of the last scan) in unit u. 0, or an error:
   2 no such partition, 3 it's C:, 4 in another drive, 5 no FAT, 6 not a
   DOS partition (confirm), 7 disk error. */
static int attach(int u, int k, int ro, int confirmed, u32 warn)
{
    if (k < 0 || k >= n_parts) return 2;
    struct part *p = &parts[k];
    if (is_c(p)) return 3;
    int v = unit_of(p);
    if (v >= 0 && v != u) return 4;
    if (!p->fat) return 5;
    if (!dos_part(p) && !confirmed) {
        char d[120];
        describe(d, sizeof d, p);
        put(&warn, d);
        wr8(warn, '$');
        return 6;
    }
    if (disk_dev_rw(p->dev, p->start, 1, sec, 0)) return 7;
    struct unit *t = &units[u];
    t->p = *p;
    memcpy(t->bpb, sec + 11, 53);
    if (p->fat != 32) memset(t->bpb + 25, 0, 28);                  /* no FAT32 fields */
    t->ro = (u8)ro;
    t->in = 1;
    t->changed = 1;
    char d[120];
    describe(d, sizeof d, p);
    kprintf("hd: %c: is %s, FAT%d%s\n", 'A' + first_drive + u, d, p->fat, ro ? ", read-only" : "");
    return 0;
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
        u32 b = LIN(DX(r), SI(r) + u * 64);
        for (int i = 0; i < 53; i++) wr8(b + i, t->bpb[i]);
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
        wr16(rh + 18, (u16)n);
        break; }
    default: st = ST_ERR(3);
    }
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
