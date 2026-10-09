/* Floppy drives A: and B:, backed by disk image files on C: (there is no
   floppy controller; nothing here touches one). VMFD.COM puts an image in
   a drive at any time ("VMFD A: C:\DISKS\INSTALL1.IMG", INT 2Fh AX=5646h);
   fda=PATH / fdb=PATH on the kernel command line fill them at boot.
   The BIOS reports two drives with change lines, so DOS sees a new image
   as a disk change, and an empty drive as "not ready".

   The image is read and written in place on C: (it never changes size).
   Before a write the file is looked up again: if it has been replaced,
   resized or moved since it went in, the write is refused (write-
   protected) rather than landing in clusters that may belong to something
   else now. An
   image put in read-only (VMFD /R) refuses writes too.

   Sizes: 160K, 180K, 320K, 360K, 720K, 1.2M, 1.44M, 1.68M/1.72M (DMF),
   2.88M; anything else is taken from the boot sector's BPB. */
#include "kernel.h"

#define BIOS_SEG 0xF000                  /* as bios.c: the BIOS image, its diskette table pointer at +202h */
#define BIOS_LIN 0xF0000u
#define FD_DRIVES 2
static void set_cf(struct regs *r, int c) { if (c) r->eflags |= EFL_CF; else r->eflags &= ~EFL_CF; }
static struct fd {
    struct extent *ext; int n_ext;
    u32 sectors, size, clus;
    u8 cyls, heads, spt, type;           /* type: INT 13h AH=08h BL (1 360K, 2 1.2M, 3 720K, 4 1.44M, 6 2.88M) */
    u8 in, ro, changed;
    char path[80];
} fd[FD_DRIVES] = { { .changed = 1 }, { .changed = 1 } };
static u8 fd_status;

static const struct { u32 kb; u8 cyls, heads, spt, type; } shapes[] = {
    { 160, 40, 1, 8, 1 }, { 180, 40, 1, 9, 1 }, { 320, 40, 2, 8, 1 }, { 360, 40, 2, 9, 1 },
    { 720, 80, 2, 9, 3 }, { 1200, 80, 2, 15, 2 }, { 1440, 80, 2, 18, 4 },
    { 1680, 80, 2, 21, 4 }, { 1722, 82, 2, 21, 4 }, { 2880, 80, 2, 36, 6 },
};

/* count sectors from sector s of drive f's image (runs on C:). */
static int img_io(struct fd *f, u32 s, u32 count, u8 *buf, int write)
{
    u32 base = 0;
    for (int e = 0; e < f->n_ext && count; e++) {
        struct extent *x = &f->ext[e];
        if (s < base + x->count) {
            u32 off = s - base, k = x->count - off;
            if (k > count) k = count;
            int st = write ? disk_write(x->lba + off, k, buf) : disk_read(x->lba + off, k, buf);
            if (st) return st;
            buf += k * 512; s += k; count -= k;
        }
        base += x->count;
    }
    return count ? 0x04 : 0;
}

/* Put the image at path (on C:) in drive d (0 A:, 1 B:). 0, or 2 not
   found, 3 not a floppy image, 4 not on C:, 6 out of memory, 7 disk error. */
static int fd_mount(int d, const char *path, int ro)
{
    if (path[0] && path[1] == ':' && (path[0] | 32) != 'c') return 4;
    static struct fatvol v;
    if (disk_volume(&v)) return 7;
    u32 clus, size;
    if (fat_lookup(&v, path, &clus, &size)) return 2;
    if (size < 512 || size % 512 || size > 4u << 20) return 3;
    int n = fat_extents(&v, clus, size, 0, 1 << 20);
    if (n <= 0) return 7;
    struct extent *ext = phys_try_alloc((u32)n * sizeof *ext);
    if (!ext) return 6;
    fat_extents(&v, clus, size, ext, n);
    struct fd t = { .ext = ext, .n_ext = n, .sectors = size / 512, .size = size, .clus = clus, .ro = (u8)ro };
    for (unsigned i = 0; i < sizeof shapes / sizeof shapes[0]; i++)
        if (shapes[i].kb * 1024 == size) {
            t.cyls = shapes[i].cyls; t.heads = shapes[i].heads; t.spt = shapes[i].spt; t.type = shapes[i].type;
        }
    /* The boot sector's BPB, when it has a sane one, says how DOS will
       address the disk: its geometry wins over the one the size suggests. */
    static u8 bs[512];
    if (img_io(&t, 0, 1, bs, 0)) return 7;
    u32 spt = bs[0x18] | bs[0x19] << 8, heads = bs[0x1A] | bs[0x1B] << 8;
    u32 bps = bs[0x0B] | bs[0x0C] << 8, total = bs[0x13] | bs[0x14] << 8;
    int bpb = bps == 512 && spt >= 8 && spt <= 63 && heads >= 1 && heads <= 2 && bs[0x15] >= 0xF0;
    if (bpb && (spt != t.spt || heads != t.heads)) {
        u32 cyls = (t.sectors + spt * heads - 1) / (spt * heads);
        if (cyls <= 255) {
            if (t.spt) kprintf("floppy: %s: %u KiB, but its boot sector says %u heads, %u sectors a track: going by that\n",
                               path, size >> 10, heads, spt);
            t.cyls = (u8)cyls; t.heads = (u8)heads; t.spt = (u8)spt;
            if (!t.type) t.type = 4;
        }
    }
    if (!t.spt) return 3;                                 /* odd size and no BPB to go by */
    if (!bpb) kprintf("floppy: %s: no DOS boot sector (a game's own boot disk?): DOS may not read it\n", path);
    else if (total && total != t.sectors)
        kprintf("floppy: %s: its boot sector says %u sectors, the file has %u\n", path, total, t.sectors);
    int j = 0;
    for (; path[j] && j < 79; j++) t.path[j] = path[j];
    t.path[j] = 0;
    t.in = 1;
    t.changed = 1;
    fd[d] = t;
    kprintf("floppy: %c: now holds %s (%u KiB, %u/%u/%u%s)\n", 'A' + d, t.path, size >> 10,
            t.cyls, t.heads, t.spt, ro ? ", read-only" : "");
    return 0;
}

static void fd_eject(int d)
{
    if (fd[d].in) kprintf("floppy: %c: empty\n", 'A' + d);
    fd[d].in = 0;
    fd[d].changed = 1;
}

/* fda= / fdb= on the kernel command line. */
static void fd_boot(void)
{
    static int done;
    if (done) return;
    done = 1;
    for (int d = 0; d < FD_DRIVES; d++) {
        char opt[5] = "fda=";
        opt[2] = (char)('a' + d);
        const char *o = strstr(cmdline, opt);
        if (!o || (o != cmdline && o[-1] != ' ')) continue;
        char path[80] = "C:";
        int j = 2;
        for (o += 4; *o && *o != ' ' && j < 79; o++) path[j++] = *o == '/' ? '\\' : *o;
        path[j] = 0;
        if (path[3] == ':') memmove(path, path + 2, (u32)j - 1);      /* fda=C:\... */
        int e = fd_mount(d, path, 0);
        if (e) kprintf("floppy: %s: can't use it (error %d)\n", path, e);
    }
}

/* Writes only go to the file the image was when it went in: the same
   name, size and runs of sectors on C:. */
static int still_there(struct fd *f)
{
    static struct fatvol v;
    static struct extent now[1024];
    u32 clus, size;
    if (disk_volume(&v) || fat_lookup(&v, f->path, &clus, &size)) return 0;
    if (clus != f->clus || size != f->size) return 0;
    if (f->n_ext > 1024 || fat_extents(&v, clus, size, 0, 1 << 20) != f->n_ext) return 0;
    fat_extents(&v, clus, size, now, f->n_ext);
    for (int i = 0; i < f->n_ext; i++)
        if (now[i].lba != f->ext[i].lba || now[i].count != f->ext[i].count) return 0;
    return 1;
}

/* INT 13h for DL < 80h. */
int fd_int13(struct regs *r)
{
    fd_boot();
    int d = DL(r), st = 0;
    u8 fn = AH(r);
    if (d >= FD_DRIVES) { AH(r) = 0x01; set_cf(r, 1); return 0; }
    struct fd *f = &fd[d];
    switch (fn) {
    case 0x00: break;                                                  /* reset */
    case 0x01: AH(r) = fd_status; set_cf(r, fd_status != 0); return 0;
    case 0x02: case 0x03: case 0x04: {                                  /* read, write, verify */
        if (!f->in) { st = 0x80; AL(r) = 0; break; }                   /* no disk: time-out (not ready) */
        u32 cyl = CH(r) | ((u32)(CL(r) & 0xC0) << 2), sec = CL(r) & 63, head = DH(r), n = AL(r);
        u32 lba = (cyl * f->heads + head) * f->spt + sec - 1, buf = LIN(r->v86_es, BX(r));
        if (!sec || sec > f->spt || head >= f->heads || cyl >= f->cyls || lba + n > f->sectors) {
            static int said;
            if (said++ < 8)
                kprintf("floppy: %c: %s of %u at C/H/S %u/%u/%u: outside the image (%u/%u/%u, %u sectors)\n",
                        'A' + d, fn == 3 ? "write" : "read", n, cyl, head, sec, f->cyls, f->heads, f->spt, f->sectors);
            st = 0x04; AL(r) = 0; break;
        }
        if (fn == 0x04) break;
        if (buf + n * 512 > GUEST_TOP) { st = 0x09; AL(r) = 0; break; }
        if (fn == 0x03) {
            if (f->ro) { st = 0x03; AL(r) = 0; break; }
            if (!still_there(f)) {
                kprintf("floppy: %c: %s was replaced or resized on C:; writes refused\n", 'A' + d, f->path);
                f->ro = 1; st = 0x03; AL(r) = 0; break;
            }
        }
        st = img_io(f, lba, n, gptr(buf), fn == 0x03);
        if (st) { st = st == 0x04 ? 0x04 : 0x20; AL(r) = 0; }
        break; }
    case 0x05: {                                                        /* format a track: fill it */
        if (!f->in) { st = 0x80; break; }
        if (f->ro || !still_there(f)) { st = 0x03; break; }
        u32 cyl = CH(r), head = DH(r);
        if (head >= f->heads || cyl >= f->cyls) { st = 0x04; break; }
        static u8 fill[63 * 512];
        memset(fill, 0xF6, sizeof fill);
        st = img_io(f, (cyl * f->heads + head) * f->spt, f->spt, fill, 1) ? 0x20 : 0;
        break; }
    case 0x08: {                                                        /* drive parameters */
        u8 cyls = f->in ? f->cyls : 80, heads = f->in ? f->heads : 2, spt = f->in ? f->spt : 18;
        BL(r) = f->in ? f->type : 4;
        CH(r) = (u8)(cyls - 1);
        CL(r) = spt;
        DH(r) = (u8)(heads - 1);
        DL(r) = FD_DRIVES;
        r->v86_es = BIOS_SEG;
        DI(r) = rd16(BIOS_LIN + 0x202);                                 /* the diskette parameter table */
        AX(r) = 0;
        set_cf(r, 0);
        fd_status = 0;
        return 0; }
    case 0x15: AH(r) = 0x02; set_cf(r, 0); return 0;                    /* a floppy drive with change line */
    case 0x16:                                                          /* disk changed? */
        if (!f->in || f->changed) { f->changed = !f->in; st = 0x06; }
        break;
    case 0x17: case 0x18:                                               /* set DASD / media type */
        if (fn == 0x18) { r->v86_es = BIOS_SEG; DI(r) = rd16(BIOS_LIN + 0x202); }
        if (!f->in && fn == 0x18) { st = 0x80; break; }
        break;
    default:
        dbg(1, "INT 13h AH=%02x DL=%02x unsupported\n", fn, d);
        st = 0x01;
    }
    fd_status = (u8)st;
    wr8(BDA + 0x41, (u8)st);
    AH(r) = (u8)st;
    set_cf(r, st != 0);
    return 0;
}

static void put(u32 *o, const char *s) { for (; *s; s++) wr8((*o)++, (u8)*s); }

/* INT 2Fh AX=5646h (VMFD.COM). BX=0: list into ES:DI ($-terminated).
   BX=1: put the image DS:SI (full DOS path) in drive CL (0 A:, 1 B:),
   DL bit 0 = read-only; AX = 0 or an error (see fd_mount). BX=2: empty
   drive CL. */
void fd_api(struct regs *r)
{
    fd_boot();
    int d = CL(r);
    switch (BX(r)) {
    case 0: {
        u32 o = LIN(r->v86_es, DI(r));
        for (int i = 0; i < FD_DRIVES; i++) {
            char t[8] = " A: ";
            t[1] = (char)('A' + i);
            put(&o, t);
            if (fd[i].in) {
                char sz[24];
                snprintf(sz, sizeof sz, "  (%uK%s)", fd[i].size >> 10, fd[i].ro ? ", read-only" : "");
                put(&o, fd[i].path); put(&o, sz);
            } else put(&o, "empty");
            put(&o, "\r\n");
        }
        wr8(o, '$');
        AX(r) = 0;
        return; }
    case 1: {
        if (d >= FD_DRIVES) { AX(r) = 5; return; }
        char path[80];
        u32 a = LIN(r->v86_ds, SI(r));
        int j = 0;
        for (; j < 79 && rd8(a + j); j++) path[j] = (char)rd8(a + j);
        path[j] = 0;
        AX(r) = (u16)fd_mount(d, path, DL(r) & 1);
        return; }
    case 2:
        if (d >= FD_DRIVES) { AX(r) = 5; return; }
        fd_eject(d);
        AX(r) = 0;
        return;
    }
    AX(r) = 0xFFFF;
}
