/* CD-ROM drives. VMCD.SYS, a CD-ROM device driver in DOS, hands every
   request header here (INT 2Fh AX=5644h); SHSUCDX (or MSCDEX) gives each
   drive a letter. A drive holds one image at a time:
   - an ISO file on C:, read straight from the disk (no RAM copy). VMCD.COM
     puts one in a drive at any time ("VMCD D: C:\ISOS\WAR2.ISO"); cd=PATH
     on the kernel command line fills drive 1 at boot;
   - an ISO loaded as a boot module after dos.img (GRUB "module", QEMU
     -initrd "dos.img,game.iso", vmdos.efi with C: as RAM disk).
   Changing the image is reported to DOS as a media change. There is
   always at least one drive (cdrives=N, 1-4, for more). */
#include "kernel.h"

#define MAX_IMAGES 16
#define MAX_UNITS 4
static struct {
    u8 *data;                            /* RAM image, or */
    struct extent *ext; int n_ext;       /* the file's runs of disk sectors */
    u32 sectors;
    char name[80];
} img[MAX_IMAGES];
static int n_img, n_units = -1;
static int unit_img[MAX_UNITS] = { -1, -1, -1, -1 };
static u8 changed[MAX_UNITS];
static int n_mod;                        /* boot-module images, first in img[] */

static void set_name(int i, const char *name)
{
    int j = 0;
    for (; name[j] && name[j] != ' ' && j < 79; j++) img[i].name[j] = name[j];
    img[i].name[j] = 0;
}

void cd_add(u8 *data, u32 size, const char *name)
{
    if (n_img >= MAX_IMAGES) return;
    if (size < 0x8800 || memcmp(data + 0x8001, "CD001", 5)) {
        kprintf("cd: %s is not an ISO 9660 image\n", name);
        return;
    }
    for (const char *p = name; *p; p++) if (*p == '/' || *p == '\\') name = p + 1;
    img[n_img].data = data;
    img[n_img].sectors = size / 2048;
    set_name(n_img, name);
    kprintf("cd: image %d: %s, %u MiB\n", n_img + 1, img[n_img].name, size >> 20);
    n_img++; n_mod = n_img;
}

/* Read n CD sectors from image im into buf. */
static int img_read(int im, u32 start, u32 n, u8 *buf)
{
    if (img[im].data) { memcpy(buf, img[im].data + start * 2048, n * 2048); return 0; }
    u32 s = start * 4, left = n * 4, base = 0;           /* in 512-byte disk sectors */
    for (int e = 0; e < img[im].n_ext && left; e++) {
        struct extent *x = &img[im].ext[e];
        if (s < base + x->count) {
            u32 off = s - base, k = x->count - off;
            if (k > left) k = left;
            if (disk_read(x->lba + off, k, buf)) return -1;
            buf += k * 512; s += k; left -= k;
        }
        base += x->count;
    }
    return left ? -1 : 0;
}

/* Put the ISO file at path (on C:) in drive u. 0, or 2 not found, 3 not an
   ISO image, 4 not on C:, 6 out of memory, 7 disk error. */
static int mount_file(int u, const char *path)
{
    if (path[0] && path[1] == ':' && (path[0] | 32) != 'c') return 4;
    static struct fatvol v;
    if (disk_volume(&v)) return 7;
    u32 clus, size;
    if (fat_lookup(&v, path, &clus, &size)) return 2;
    if (size < 0x8800) return 3;
    int slot = -1;
    for (int i = n_mod; i < n_img; i++) {                 /* a file slot no drive uses */
        int used = 0;
        for (int k = 0; k < MAX_UNITS; k++) if (unit_img[k] == i) used = 1;
        if (!used && slot < 0) slot = i;
    }
    if (slot < 0) { if (n_img == MAX_IMAGES) return 6; slot = n_img; }
    int n = fat_extents(&v, clus, size, 0, 1 << 20);
    if (n <= 0) return 7;
    struct extent *ext = phys_try_alloc((u32)n * sizeof *ext);
    if (!ext) return 6;
    fat_extents(&v, clus, size, ext, n);
    img[slot].data = 0; img[slot].ext = ext; img[slot].n_ext = n;
    img[slot].sectors = size / 2048;
    static u8 pvd[2048];
    if (img_read(slot, 16, 1, pvd)) return 7;
    if (memcmp(pvd + 1, "CD001", 5)) return 3;
    set_name(slot, path);
    if (slot == n_img) n_img++;
    unit_img[u] = slot;
    changed[u] = 1;
    kprintf("cd: drive %d now holds %s (%u MiB, %d run%s on C:)\n", u + 1, img[slot].name, size >> 20, n, n == 1 ? "" : "s");
    return 0;
}

static void units_init(void)
{
    if (n_units >= 0) return;
    n_units = n_img < 1 ? 1 : n_img > MAX_UNITS ? MAX_UNITS : n_img;
    const char *o = strstr(cmdline, "cdrives=");
    if (o && o[8] >= '1' && o[8] <= '4') n_units = o[8] - '0';
    for (int u = 0; u < n_units && u < n_img; u++) unit_img[u] = u;
    o = strstr(cmdline, "cd=");
    if (o && (o == cmdline || o[-1] == ' ')) {
        char path[80];
        int j = 0;
        for (o += 3; *o && *o != ' ' && j < 79; o++) path[j++] = *o == '/' ? '\\' : *o;
        path[j] = 0;
        int e = mount_file(0, path);
        if (e) kprintf("cd: cd=%s: can't use it (error %d)\n", path, e);
        changed[0] = 0;
    }
}

/* ---------------- MSCDEX device-driver requests ---------------- */
#define ST_DONE 0x0100
#define ST_ERR(e) (0x8100 | (e))          /* 2 not ready, 3 unknown command, 8 sector not found, F invalid disk change */

static u32 msf(u32 lba) { lba += 150; return (lba / 4500) << 16 | (lba / 75 % 60) << 8 | (lba % 75); }

static u16 ioctl_in(int unit, u32 cb, u16 drv_cs)
{
    int im = unit_img[unit];
    u32 sectors = im >= 0 ? img[im].sectors : 0;
    switch (rd8(cb)) {
    case 0: wr16(cb + 1, 0); wr16(cb + 3, drv_cs); return ST_DONE;     /* device header address */
    case 1: wr32(cb + 2, 0); return ST_DONE;                            /* location of head */
    case 4: for (int i = 0; i < 4; i++) { wr8(cb + 1 + i * 2, (u8)i); wr8(cb + 2 + i * 2, 0xFF); } return ST_DONE;
    case 5: wr8(cb + 1, 0); return ST_DONE;                             /* drive bytes */
    case 6: wr32(cb + 1, 0x00000202u | (im < 0 ? 0x800 : 0)); return ST_DONE;   /* status: unlocked, HSG+RB */
    case 7: wr16(cb + 2, 2048); return ST_DONE;                         /* sector size */
    case 8: wr32(cb + 1, sectors); return ST_DONE;                      /* volume size */
    case 9:                                                             /* media changed */
        wr8(cb + 1, changed[unit] ? 0xFF : 1);
        changed[unit] = 0;
        return ST_DONE;
    case 10: wr8(cb + 1, 1); wr8(cb + 2, 1); wr32(cb + 3, msf(sectors)); return ST_DONE;   /* disc: track 1 */
    case 11: wr32(cb + 2, msf(0)); wr8(cb + 6, 0x40); return ST_DONE;   /* track 1: data */
    case 12: for (int i = 1; i <= 10; i++) wr8(cb + i, 0); return ST_DONE;
    case 15: wr16(cb + 1, 0); wr32(cb + 3, 0); wr32(cb + 7, 0); return ST_DONE;   /* audio status */
    }
    return ST_ERR(3);
}

static u16 read_long(int unit, u32 rh)
{
    int im = unit_img[unit];
    if (im < 0) return ST_ERR(2);
    u32 buf = LIN(rd16(rh + 16), rd16(rh + 14)), n = rd16(rh + 18), start = rd32(rh + 20);
    if (rd8(rh + 13) == 1)                                              /* Red Book M:S:F */
        start = ((start >> 16 & 0xFF) * 60 + (start >> 8 & 0xFF)) * 75 + (start & 0xFF) - 150;
    if (rd8(rh + 24) != 0) return ST_ERR(3);                            /* cooked only */
    if (start + n > img[im].sectors || start + n < start) return ST_ERR(8);
    if (buf + n * 2048 > GUEST_TOP) return ST_ERR(0xC);
    if (img_read(im, start, n, gptr(buf))) return ST_ERR(0xB);          /* read fault */
    return ST_DONE;
}

static void put(u32 *o, const char *s) { for (; *s; s++) wr8((*o)++, (u8)*s); }

/* INT 2Fh AX=5644h. BX=0: number of drives. BX=1: ES:DI = request header,
   DX = driver segment. BX=2: VMCD.COM: CX=0 list (ES:DI <- text), else
   put image DL (1..n, 0 = empty) in drive CL (1..). BX=3: put the ISO
   file DS:SI (full DOS path, from INT 21h AH=60h) in drive CL; AX=0 or
   an error (see mount_file). */
void cd_api(struct regs *r)
{
    units_init();
    switch (BX(r)) {
    case 0: AX(r) = (u16)n_units; return;
    case 1: {
        u32 rh = LIN(r->v86_es, DI(r));
        int unit = rd8(rh + 1), cmd = rd8(rh + 2);
        u16 st;
        if (unit >= n_units) st = ST_ERR(1);
        else switch (cmd) {
        case 3: st = ioctl_in(unit, LIN(rd16(rh + 16), rd16(rh + 14)), DX(r)); break;
        case 12: case 13: case 14: case 130: case 132: case 133: st = ST_DONE; break;   /* ioctl out, open, close, seek, stop, resume */
        case 128: st = read_long(unit, rh); break;
        default: st = ST_ERR(3);
        }
        wr16(rh + 3, st);
        return; }
    case 2: {
        if (CX(r) == 0) {                                               /* list into ES:DI ($-terminated) */
            u32 o = LIN(r->v86_es, DI(r));
            put(&o, "CD images:\r\n");
            for (int i = 0; i < n_img; i++) {
                int used = 0;
                for (int u = 0; u < n_units; u++) if (unit_img[u] == i) used = 1;
                if (i >= n_mod && !used) continue;                      /* a file no drive holds any more */
                char num[4] = { ' ', (char)(i < 9 ? '1' + i : 'A' + i - 9), ' ', 0 };
                put(&o, num);
                put(&o, img[i].name);
                for (int u = 0; u < n_units; u++)
                    if (unit_img[u] == i) { char t[16] = "  (in drive 1)"; t[12] = (char)('1' + u); put(&o, t); }
                put(&o, "\r\n");
            }
            for (int u = 0; u < n_units; u++)
                if (unit_img[u] < 0) { char t[24] = " drive 1: empty\r\n"; t[7] = (char)('1' + u); put(&o, t); }
            wr8(o, '$');
            AX(r) = 0;
            return;
        }
        int u = CL(r) - 1, i = DL(r) - 1;
        if (u < 0 || u >= n_units || i < -1 || i >= n_mod) { AX(r) = 1; return; }
        unit_img[u] = i;
        changed[u] = 1;
        kprintf("cd: drive %d now holds %s\n", u + 1, i >= 0 ? img[i].name : "nothing");
        AX(r) = 0;
        return; }
    case 3: {
        int u = CL(r) - 1;
        if (u < 0 || u >= n_units) { AX(r) = 5; return; }
        char path[80];
        u32 a = LIN(r->v86_ds, SI(r));
        int j = 0;
        for (; j < 79 && rd8(a + j); j++) path[j] = (char)rd8(a + j);
        path[j] = 0;
        AX(r) = (u16)mount_file(u, path);
        return; }
    }
    AX(r) = 0xFFFF;
}
