/* CD-ROM drives. VMCD.SYS, a CD-ROM device driver in DOS, hands every
   request header here (INT 2Fh AX=5644h); SHSUCDX (or MSCDEX) gives each
   drive a letter. A drive holds one image at a time:
   - an ISO file on C:, read straight from the disk (no RAM copy). VMCD.COM
     puts one in a drive at any time ("VMCD D: C:\ISOS\WAR2.ISO"); cd=PATH
     on the kernel command line fills drive 1 at boot;
   - an ISO loaded as a boot module after dos.img (GRUB "module", QEMU
     -initrd "dos.img,game.iso", vmdos.efi with C: as RAM disk).
   Changing the image is reported to DOS as a media change. There is
   always at least one image drive (cdrives=N, 1-4, for more).
   Real CD/DVD drives on SATA (AHCI, ahci.cpp) come after the image drives,
   one unit each: the disc in the drive, read with SCSI commands; eject
   and close work, a disc swap is a media change. cdphys=off leaves them
   out. */
#include "kernel.h"

int ahci_init(void);
int ahci_cd_count(void);
const char *ahci_cd_model(int i);
int ahci_cd_packet(int i, const u8 *cdb, void *buf, u32 bytes);
int ahci_cd_asc(void);                   /* ASC of the last failure (sense already read) */

#define MAX_IMAGES 16
#define MAX_UNITS 4                      /* image drives */
#define MAX_PHYS 4                       /* real drives, after them */
#define MAX_TRACKS 99
static struct {
    u8 *data;                            /* RAM image, or */
    struct extent *ext; int n_ext;       /* the file's runs of disk sectors */
    u32 sectors;
    char name[80];
} img[MAX_IMAGES];
static int n_img, n_units = -1;
static int unit_img[MAX_UNITS] = { -1, -1, -1, -1 };
static u8 changed[MAX_UNITS + MAX_PHYS];
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

/* ---------------- real drives ---------------- */
static int n_phys;
static struct {
    int present;                         /* a readable disc is in */
    u32 sectors;
    int first, last;                     /* tracks */
    u32 start[MAX_TRACKS + 2];           /* LBA of track t at [t]; lead-out at [last + 1] */
    u8 ctl[MAX_TRACKS + 1];              /* ADR/control byte of track t */
} ph[MAX_PHYS];
static u8 *pbuf;                         /* identity-mapped DMA buffer */
#define PBUF_SECS 32

static int packet(int d, u8 c0, u32 lba, u16 len, u8 b9, void *buf, u32 bytes)
{
    u8 cdb[12] = { c0 };
    cdb[2] = (u8)(lba >> 24); cdb[3] = (u8)(lba >> 16); cdb[4] = (u8)(lba >> 8); cdb[5] = (u8)lba;
    cdb[7] = (u8)(len >> 8); cdb[8] = (u8)len; cdb[9] = b9;
    return ahci_cd_packet(d, cdb, buf, bytes);
}

/* Is a disc in drive d? On a change (or the first look) re-read its size
   and table of contents and flag the media change for DOS. */
static void phys_poll(int d, int unit)
{
    /* TEST UNIT READY. Unit attention (6) means a disc change: ask again.
       Not ready (2): "becoming ready" (ASC 04h, a disc just went in, still
       spinning up) is waited for, up to 10 s; "no medium" (3Ah) is asked
       once more, as drives (and QEMU) report it once around a change.
       ahci_cd_packet has already read the sense data after a failure. */
    int k = -1, waited = 0, empty = 0;
    for (int tries = 0; tries < 8; tries++) {
        k = packet(d, 0x00, 0, 0, 0, 0, 0);
        if (k == 6) { changed[unit] = 1; ph[d].present = 0; continue; }
        if (k != 2) break;
        int asc = ahci_cd_asc();
        if (asc == 0x04 && waited < 10000) {
            for (int i = 0; i < 250000; i++) inb(0x80);                 /* ~250 ms */
            waited += 250; tries--; continue;
        }
        if (asc != 0x3A || ++empty > 1) break;
    }
    if (k) { if (ph[d].present) changed[unit] = 1; ph[d].present = 0; return; }
    if (ph[d].present) return;
    if (packet(d, 0x25, 0, 0, 0, pbuf, 8)) return;                   /* READ CAPACITY(10) */
    u32 last = (u32)pbuf[0] << 24 | pbuf[1] << 16 | pbuf[2] << 8 | pbuf[3];
    ph[d].sectors = last + 1;
    ph[d].first = ph[d].last = 1;
    ph[d].start[1] = 0; ph[d].start[2] = ph[d].sectors; ph[d].ctl[1] = 0x14;
    /* READ TOC, format 0, LBA addresses; byte 1 of the CDB: MSF bit off */
    u8 cdb[12] = { 0x43, 0, 0, 0, 0, 0, 1, 0x03, 0x24, 0 };
    if (!ahci_cd_packet(d, cdb, pbuf, 804) && pbuf[2] >= 1 && pbuf[3] >= pbuf[2] && pbuf[3] <= MAX_TRACKS) {
        int n = ((pbuf[0] << 8 | pbuf[1]) - 2) / 8;
        ph[d].first = pbuf[2]; ph[d].last = pbuf[3];
        for (int i = 0; i < n && i < MAX_TRACKS + 1; i++) {
            u8 *t = pbuf + 4 + i * 8;
            u32 a = (u32)t[4] << 24 | t[5] << 16 | t[6] << 8 | t[7];
            if (t[2] == 0xAA) ph[d].start[ph[d].last + 1] = a;
            else if (t[2] >= 1 && t[2] <= MAX_TRACKS) { ph[d].start[t[2]] = a; ph[d].ctl[t[2]] = t[1]; }
        }
    }
    ph[d].present = 1;
    changed[unit] = 1;
    kprintf("cd: drive %d (%s): disc in, %u KiB, tracks %d-%d\n", unit + 1, ahci_cd_model(d),
            ph[d].sectors * 2, ph[d].first, ph[d].last);
}

static int phys_read(int d, u32 start, u32 n, u8 *buf)
{
    while (n) {
        u32 k = n > PBUF_SECS ? PBUF_SECS : n;
        int e = packet(d, 0x28, start, (u16)k, 0, pbuf, k * 2048);    /* READ(10) */
        if (e) return e;
        memcpy(buf, pbuf, k * 2048);
        buf += k * 2048; start += k; n -= k;
    }
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
    o = strstr(cmdline, "cdphys=off");
    if (!o && !strstr(cmdline, "ahci=off") && (pbuf = phys_try_alloc(PBUF_SECS * 2048))) {
        ahci_init();                                    /* already done when C: is on a disk */
        n_phys = ahci_cd_count();
        if (n_phys > MAX_PHYS) n_phys = MAX_PHYS;
        for (int d = 0; d < n_phys; d++)
            kprintf("cd: drive %d is the real drive %s\n", n_units + d + 1, ahci_cd_model(d));
    }
}

/* The real drive behind unit u, or -1 for an image drive. */
static int phys_of(int unit) { return unit >= n_units ? unit - n_units : -1; }

/* ---------------- MSCDEX device-driver requests ---------------- */
#define ST_DONE 0x0100
#define ST_ERR(e) (0x8100 | (e))          /* 2 not ready, 3 unknown command, 8 sector not found, F invalid disk change */

static u32 msf(u32 lba) { lba += 150; return (lba / 4500) << 16 | (lba / 75 % 60) << 8 | (lba % 75); }

static u16 phys_ioctl_in(int unit, int d, u32 cb, u16 drv_cs)
{
    phys_poll(d, unit);
    int t, in = ph[d].present;
    switch (rd8(cb)) {
    case 0: wr16(cb + 1, 0); wr16(cb + 3, drv_cs); return ST_DONE;
    case 1: wr32(cb + 2, 0); return ST_DONE;
    case 4: for (int i = 0; i < 4; i++) { wr8(cb + 1 + i * 2, (u8)i); wr8(cb + 2 + i * 2, 0xFF); } return ST_DONE;
    case 5: wr8(cb + 1, 0); return ST_DONE;
    case 6: wr32(cb + 1, 0x00000202u | (in ? 0 : 0x801)); return ST_DONE;   /* no disc: door open too */
    case 7: wr16(cb + 2, 2048); return ST_DONE;
    case 8: if (!in) return ST_ERR(2); wr32(cb + 1, ph[d].sectors); return ST_DONE;
    case 9: wr8(cb + 1, changed[unit] ? 0xFF : 1); changed[unit] = 0; return ST_DONE;
    case 10:
        if (!in) return ST_ERR(2);
        wr8(cb + 1, (u8)ph[d].first); wr8(cb + 2, (u8)ph[d].last);
        wr32(cb + 3, msf(ph[d].start[ph[d].last + 1]));
        return ST_DONE;
    case 11:
        t = rd8(cb + 1);
        if (!in) return ST_ERR(2);
        if (t < ph[d].first || t > ph[d].last) return ST_ERR(8);
        wr32(cb + 2, msf(ph[d].start[t]));
        wr8(cb + 6, (u8)(ph[d].ctl[t] << 4 | ph[d].ctl[t] >> 4));  /* control in the high nibble */
        return ST_DONE;
    case 12: for (int i = 1; i <= 10; i++) wr8(cb + i, 0); return ST_DONE;
    case 15: wr16(cb + 1, 0); wr32(cb + 3, 0); wr32(cb + 7, 0); return ST_DONE;
    }
    return ST_ERR(3);
}

/* IOCTL output: 0 eject, 2 reset, 5 close the tray; the rest accepted. */
static u16 phys_ioctl_out(int unit, int d, u32 cb)
{
    switch (rd8(cb)) {
    case 0: case 5: {
        u8 cdb[12] = { 0x1B, 0, 0, 0, rd8(cb) ? 3 : 2 };              /* START STOP UNIT, LoEj */
        ahci_cd_packet(d, cdb, 0, 0);
        ph[d].present = 0; changed[unit] = 1;
        return ST_DONE; }
    case 2: ph[d].present = 0; changed[unit] = 1; return ST_DONE;
    }
    return ST_DONE;
}

static u16 ioctl_in(int unit, u32 cb, u16 drv_cs)
{
    if (phys_of(unit) >= 0) return phys_ioctl_in(unit, phys_of(unit), cb, drv_cs);
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
    u32 buf = LIN(rd16(rh + 16), rd16(rh + 14)), n = rd16(rh + 18), start = rd32(rh + 20);
    if (rd8(rh + 13) == 1)                                              /* Red Book M:S:F */
        start = ((start >> 16 & 0xFF) * 60 + (start >> 8 & 0xFF)) * 75 + (start & 0xFF) - 150;
    if (rd8(rh + 24) != 0) return ST_ERR(3);                            /* cooked only */
    if (buf + n * 2048 > GUEST_TOP) return ST_ERR(0xC);
    int d = phys_of(unit);
    if (d >= 0) {
        if (!ph[d].present) phys_poll(d, unit);
        if (!ph[d].present) return ST_ERR(2);
        if (start + n > ph[d].sectors || start + n < start) return ST_ERR(8);
        int e = phys_read(d, start, n, gptr(buf));
        if (e == 6 || e == 2) {                                         /* disc changed or taken out */
            ph[d].present = 0;
            phys_poll(d, unit);
            changed[unit] = 1;
            return ST_ERR(ph[d].present ? 0xF : 2);                     /* invalid disk change / not ready */
        }
        return e ? ST_ERR(0xB) : ST_DONE;
    }
    int im = unit_img[unit];
    if (im < 0) return ST_ERR(2);
    if (start + n > img[im].sectors || start + n < start) return ST_ERR(8);
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
    case 0: AX(r) = (u16)(n_units + n_phys); return;
    case 1: {
        u32 rh = LIN(r->v86_es, DI(r));
        int unit = rd8(rh + 1), cmd = rd8(rh + 2);
        u16 st;
        if (unit >= n_units + n_phys) st = ST_ERR(1);
        else switch (cmd) {
        case 3: st = ioctl_in(unit, LIN(rd16(rh + 16), rd16(rh + 14)), DX(r)); break;
        case 12:
            st = phys_of(unit) >= 0 ? phys_ioctl_out(unit, phys_of(unit), LIN(rd16(rh + 16), rd16(rh + 14))) : ST_DONE;
            break;
        case 13: case 14: case 130: case 132: case 133: st = ST_DONE; break;   /* ioctl out, open, close, seek, stop, resume */
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
            for (int d = 0; d < n_phys; d++) {
                char t[24] = " drive 1: real drive ";
                t[7] = (char)('1' + n_units + d);
                put(&o, t); put(&o, ahci_cd_model(d)); put(&o, "\r\n");
            }
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
