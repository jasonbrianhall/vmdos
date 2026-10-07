/* CD-ROM images: ISO 9660 files loaded as boot modules after dos.img
   (GRUB "module", QEMU -initrd "dos.img,game.iso", vmdos.efi takes the
   *.ISO files next to it). VMCD.SYS, a CD-ROM device driver in DOS, hands
   every request header here (INT 2Fh AX=5644h); SHSUCDX (or MSCDEX) gives
   each drive a letter. VMCD.COM lists the images and changes the one in a
   drive (multi-disc games), which the driver reports as a media change. */
#include "kernel.h"

#define MAX_IMAGES 8
#define MAX_UNITS 4
static struct { u8 *data; u32 sectors; char name[32]; } img[MAX_IMAGES];
static int n_img, n_units;
static int unit_img[MAX_UNITS];          /* image in each drive, -1 = empty */
static u8 changed[MAX_UNITS];

void cd_add(u8 *data, u32 size, const char *name)
{
    if (n_img >= MAX_IMAGES) return;
    if (size < 0x8800 || memcmp(data + 0x8001, "CD001", 5)) {
        kprintf("cd: %s is not an ISO 9660 image\n", name);
        return;
    }
    img[n_img].data = data;
    img[n_img].sectors = size / 2048;
    int j = 0;
    for (const char *p = name; *p; p++) if (*p == '/' || *p == '\\') name = p + 1;
    for (; name[j] && name[j] != ' ' && j < 31; j++) img[n_img].name[j] = name[j];
    img[n_img].name[j] = 0;
    kprintf("cd: image %d: %s, %u MiB\n", n_img + 1, img[n_img].name, size >> 20);
    if (n_units < MAX_UNITS) { unit_img[n_units] = n_img; n_units++; }
    n_img++;
}

/* ---------------- MSCDEX device-driver requests ---------------- */
#define ST_DONE 0x0100
#define ST_ERR(e) (0x8100 | (e))          /* 2 not ready, 3 unknown command, 8 sector not found */

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
    memcpy(gptr(buf), img[im].data + start * 2048, n * 2048);
    return ST_DONE;
}

/* INT 2Fh AX=5644h. BX=0: number of drives. BX=1: ES:DI = request header,
   DX = driver segment. BX=2: VMCD.COM: CX=0 list (ES:DI <- text), else
   put image DL (1..n, 0 = empty) in drive CL (1..). */
void cd_api(struct regs *r)
{
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
            const char *hdr = "CD images:\r\n";
            for (const char *p = hdr; *p; p++) wr8(o++, (u8)*p);
            for (int i = 0; i < n_img; i++) {
                wr8(o++, ' '); wr8(o++, (u8)('1' + i)); wr8(o++, ' ');
                for (const char *p = img[i].name; *p; p++) wr8(o++, (u8)*p);
                for (int u = 0; u < n_units; u++)
                    if (unit_img[u] == i) { const char *t = "  (in drive "; for (const char *p = t; *p; p++) wr8(o++, (u8)*p);
                                            wr8(o++, (u8)('1' + u)); wr8(o++, ')'); }
                wr8(o++, '\r'); wr8(o++, '\n');
            }
            if (!n_img) { const char *t = " none: add ISO files as boot modules\r\n"; for (const char *p = t; *p; p++) wr8(o++, (u8)*p); }
            wr8(o, '$');
            AX(r) = 0;
            return;
        }
        int u = CL(r) - 1, i = DL(r) - 1;
        if (u < 0 || u >= n_units || i < -1 || i >= n_img) { AX(r) = 1; return; }
        unit_img[u] = i;
        changed[u] = 1;
        kprintf("cd: drive %d now holds %s\n", u + 1, i >= 0 ? img[i].name : "nothing");
        AX(r) = 0;
        return; }
    }
    AX(r) = 0xFFFF;
}
