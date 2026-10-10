/* CD-ROM drives. VMCD.SYS, a CD-ROM device driver in DOS, hands every
   request header here (INT 2Fh AX=5644h); SHSUCDX (or MSCDEX) gives each
   drive a letter.

   Image drives come first. Each holds one image at a time:
   - an ISO file on C:, read straight from the disk (no RAM copy). VMCD.COM
     puts one in a drive at any time ("VMCD D: C:\ISOS\WAR2.ISO"); cd=PATH
     on the kernel command line fills drive 1 at boot;
   - a CUE sheet on C: with its BIN file(s) next to it, the same way: data
     and CD audio tracks (MODE1/2048, MODE1/2352, MODE2/2352, MODE2/2336,
     AUDIO; one BIN or one per track);
   - an ISO loaded as a boot module after dos.img (GRUB "module", QEMU
     -initrd "dos.img,game.iso", vmdos.efi with C: as RAM disk).
   Changing the image is reported to DOS as a media change. There is
   always at least one image drive (cdrives=N, 1-4, for more).

   Real CD/DVD drives on SATA (AHCI, ahci.cpp), IDE (ide.cpp) and USB (usb.cpp) come after
   the image drives, one unit each: the disc in the drive, read with SCSI
   commands; eject and close work, a disc swap is a media change.
   cdphys=off leaves them out.

   CD audio (MSCDEX PLAY AUDIO / STOP / RESUME, Q-channel, audio status,
   volume) plays through the sound card, mixed in with the Sound Blaster:
   from a CUE image's audio tracks, or read digitally from a real drive
   (READ CD). A drive that can't read audio digitally is told to play it
   itself (PLAY AUDIO MSF: its own audio out, e.g. the headphone jack). */
#include "kernel.h"

int ahci_init(void);
int ahci_cd_count(void);
const char *ahci_cd_model(int i);
int ahci_cd_packet(int i, const u8 *cdb, void *buf, u32 bytes);
int ahci_cd_asc(void);                   /* ASC of the last failure (sense already read) */
int ide_init(void);
int ide_cd_count(void);
const char *ide_cd_model(int i);
int ide_cd_packet(int i, const u8 *cdb, void *buf, u32 bytes);
int ide_cd_asc(void);
int usb_cd_count(void);
void usb_settle(int max_ms);
const char *usb_cd_model(int i);
int usb_cd_packet(int i, const u8 *cdb, void *buf, u32 bytes);
int usb_cd_asc(void);

#define MAX_IMAGES 16
#define MAX_UNITS 4                      /* image drives */
#define MAX_PHYS 4                       /* real drives, after them */
#define MAX_TRACKS 99
#define RAW 2352                         /* bytes in a raw CD frame (1/75 s of audio) */
#define XBUF 65536
#define XFRAMES (XBUF / RAW)             /* raw frames per read: 27 */

/* A disc's table of contents. ctl: ADR << 4 | CONTROL, as READ TOC gives
   it (CONTROL bit 2: data track). */
struct toc {
    int first, last;
    u32 start[MAX_TRACKS + 2];           /* LBA of track t at [t]; lead-out at [last + 1] */
    u8 ctl[MAX_TRACKS + 1];
};

/* ---------------- CUE / BIN images ---------------- */
struct bfile { struct extent *ext; int n_ext; u32 size; };
struct ctrack {
    u8 num, file, audio, hdr;            /* hdr: bytes before the 2048 of user data */
    u16 bsize;                           /* 2048, 2336 or 2352 bytes a sector in the file */
    u32 lba;                             /* absolute LBA of INDEX 01 */
    u32 off;                             /* its byte offset in the file */
};
struct cue {
    int n_files, n_trk;
    struct bfile f[MAX_TRACKS];
    struct ctrack t[MAX_TRACKS];
    u32 leadout;
};

static struct {
    u8 *data;                            /* RAM image, or */
    struct extent *ext; int n_ext;       /* the ISO file's runs of disk sectors, or */
    struct cue *cue;                     /* a CUE sheet's tracks */
    u32 sectors;
    struct toc toc;
    char name[80];
} img[MAX_IMAGES];
static int n_img, n_units = -1;
static int unit_img[MAX_UNITS] = { -1, -1, -1, -1 };
static u8 changed[MAX_UNITS + MAX_PHYS];
static int n_mod;                        /* boot-module images, first in img[] */
static u8 *xbuf;                         /* 64 KiB, identity-mapped: DMA and raw reads */

static void set_name(int i, const char *name)
{
    int j = 0;
    for (; name[j] && name[j] != ' ' && j < 79; j++) img[i].name[j] = name[j];
    img[i].name[j] = 0;
}

static void toc_one_data_track(struct toc *t, u32 sectors)
{
    t->first = t->last = 1;
    t->start[1] = 0; t->start[2] = sectors; t->ctl[1] = 0x14;
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
    toc_one_data_track(&img[n_img].toc, img[n_img].sectors);
    set_name(n_img, name);
    kprintf("cd: image %d: %s, %u MiB\n", n_img + 1, img[n_img].name, size >> 20);
    n_img++; n_mod = n_img;
}

/* count 512-byte sectors from sector s of a file (its runs on C:). */
static int ext_read(struct extent *ext, int n_ext, u32 s, u32 count, u8 *buf)
{
    u32 base = 0;
    for (int e = 0; e < n_ext && count; e++) {
        struct extent *x = &ext[e];
        if (s < base + x->count) {
            u32 off = s - base, k = x->count - off;
            if (k > count) k = count;
            if (disk_read(x->lba + off, k, buf)) return -1;
            buf += k * 512; s += k; count -= k;
        }
        base += x->count;
    }
    return count ? -1 : 0;
}

/* len bytes at byte off of a file; past its end reads as zeros. */
static int file_read(struct bfile *f, u32 off, u32 len, u8 *out)
{
    static u8 sec[16 * 512];
    if (off >= f->size) { memset(out, 0, len); return 0; }
    if (off + len > f->size) { u32 in = f->size - off; memset(out + in, 0, len - in); len = in; }
    while (len) {
        u32 s = off / 512, w = off % 512, k = (w + len + 511) / 512;
        if (k > 16) k = 16;
        if (ext_read(f->ext, f->n_ext, s, k, sec)) return -1;
        u32 c = k * 512 - w;
        if (c > len) c = len;
        memcpy(out, sec + w, c);
        out += c; off += c; len -= c;
    }
    return 0;
}

/* The track LBA l is in (the last one starting at or before it). */
static struct ctrack *cue_track(struct cue *c, u32 l)
{
    int i = 0;
    while (i + 1 < c->n_trk && c->t[i + 1].lba <= l) i++;
    return &c->t[i];
}

/* n frames from LBA l, raw (2352 bytes each) or cooked (2048 of user data).
   Raw reads of data tracks give zeros (silence); cooked reads of audio
   tracks fail. */
static int cue_read(struct cue *c, u32 l, u32 n, u8 *out, int raw)
{
    while (n) {
        struct ctrack *t = cue_track(c, l);
        u32 k = n;
        if (t + 1 < c->t + c->n_trk && l + k > t[1].lba) k = t[1].lba - l;
        u32 sz = raw ? RAW : 2048;
        if (l < t->lba) {                                   /* before the first track's INDEX 01 */
            u32 gap = t->lba - l;
            if (k > gap) k = gap;
            memset(out, 0, k * sz);
        } else if (raw && !t->audio) {
            memset(out, 0, k * sz);
        } else if (!raw && t->audio) {
            return -1;
        } else {
            u32 at = t->off + (l - t->lba) * t->bsize;
            if (t->bsize == sz && (raw || !t->hdr)) {          /* the sectors as they are in the file */
                if (file_read(&c->f[t->file], at, k * sz, out)) return -1;
            } else {                                        /* user data out of each raw sector */
                if (k > XBUF / t->bsize) k = XBUF / t->bsize;
                static u8 tmp[XBUF];
                if (file_read(&c->f[t->file], at, k * t->bsize, tmp)) return -1;
                for (u32 i = 0; i < k; i++) memcpy(out + i * 2048, tmp + i * t->bsize + t->hdr, 2048);
            }
        }
        out += k * sz; l += k; n -= k;
    }
    return 0;
}

/* Read n CD sectors (2048 bytes) from image im into buf. */
static int img_read(int im, u32 start, u32 n, u8 *buf)
{
    if (img[im].data) { memcpy(buf, img[im].data + start * 2048, n * 2048); return 0; }
    if (img[im].cue) return cue_read(img[im].cue, start, n, buf, 0);
    return ext_read(img[im].ext, img[im].n_ext, start * 4, n * 4, buf);
}

static int msf_frames(const char *p, u32 *f)                /* "mm:ss:ff" */
{
    u32 v[3] = { 0, 0, 0 };
    for (int i = 0; i < 3; i++) {
        if (*p < '0' || *p > '9') return -1;
        while (*p >= '0' && *p <= '9') v[i] = v[i] * 10 + (u32)(*p++ - '0');
        if (i < 2 && *p++ != ':') return -1;
    }
    *f = (v[0] * 60 + v[1]) * 75 + v[2];
    return 0;
}

static int word_is(const char *p, const char *w)
{
    while (*w) if ((*p++ | 32) != (*w++ | 32)) return 0;
    return *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || !*p;
}

static const char *skip_ws(const char *p) { while (*p == ' ' || *p == '\t') p++; return p; }
static const char *next_word(const char *p) { while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++; return skip_ws(p); }

/* Look a file on C: up and get its runs. 0, or an error as mount_file's. */
static int open_file(struct fatvol *v, const char *path, struct bfile *f)
{
    u32 clus;
    if (fat_lookup(v, path, &clus, &f->size)) return 2;
    int n = fat_extents(v, clus, f->size, 0, 1 << 20);
    if (n <= 0) return 7;
    f->ext = phys_try_alloc((u32)n * sizeof *f->ext);
    if (!f->ext) return 6;
    fat_extents(v, clus, f->size, f->ext, n);
    f->n_ext = n;
    return 0;
}

/* Parse the CUE sheet at path; its BIN files are looked up next to it (a
   long BIN name, which C:'s 8.3 lookup can't find, falls back to the
   sheet's own name with .BIN when there is only one). 0, or an error as
   mount_file's (8: not a CUE sheet vmdos can use). */
static int cue_open(struct fatvol *v, const char *path, struct bfile *sheet, struct cue **out)
{
    if (sheet->size > 32768) return 8;
    static char text[32768 + 1];
    if (file_read(sheet, 0, sheet->size, (u8 *)text)) return 7;
    text[sheet->size] = 0;
    struct cue *c = phys_try_alloc(sizeof *c);
    if (!c) return 6;
    memset(c, 0, sizeof *c);
    char dir[80];
    int dl = 0;
    for (int i = 0; path[i] && i < 79; i++) if (path[i] == '\\' || path[i] == '/') dl = i + 1;
    memcpy(dir, path, (u32)dl);
    u32 base = 0, pregap = 0, prev_f = 0;           /* the current file's first LBA; PREGAP frames so far;
                                                       the latest INDEX 01 in the file (frames) */
    int last_file_trk = -1;                         /* the current file's latest track */
    struct ctrack *t = 0;
    for (const char *p = text; *p; ) {
        const char *line = skip_ws(p);
        while (*p && *p != '\n') p++;
        if (*p) p++;
        if (word_is(line, "FILE")) {
            if (c->n_files == MAX_TRACKS) return 8;
            if (c->n_files) {                               /* the next file starts where the last one ended */
                struct ctrack *lt = last_file_trk >= 0 ? &c->t[last_file_trk] : 0;
                base += c->f[c->n_files - 1].size / (lt ? lt->bsize : RAW);
            }
            const char *q = next_word(line), *e;
            char name[80];
            if (*q == '"') { q++; for (e = q; *e && *e != '"' && *e != '\n'; e++) ; }
            else for (e = q; *e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n'; e++) ;
            if (word_is(skip_ws(*e == '"' ? e + 1 : e), "WAVE")) { kprintf("cd: %s: WAVE files aren't supported\n", path); return 8; }
            int n = dl;
            memcpy(name, dir, (u32)dl);
            for (const char *s = q; s < e && n < 79; s++) name[n++] = *s == '/' ? '\\' : *s;
            name[n] = 0;
            struct bfile *f = &c->f[c->n_files];
            int r = open_file(v, name, f);
            if (r == 2 && c->n_files == 0) {                /* long name: try SHEET.BIN */
                int m = 0, dot = -1;
                for (; path[m] && m < 75; m++) { name[m] = path[m]; if (path[m] == '.') dot = m; if (path[m] == '\\') dot = -1; }
                if (dot < 0) dot = m;
                memcpy(name + dot, ".BIN", 5);
                r = open_file(v, name, f);
            }
            if (r) { kprintf("cd: %s: can't find %s\n", path, name); return r; }
            c->n_files++;
            last_file_trk = -1;
        } else if (word_is(line, "TRACK")) {
            if (!c->n_files || c->n_trk == MAX_TRACKS) return 8;
            const char *q = next_word(line);
            u32 num = 0;
            while (*q >= '0' && *q <= '9') num = num * 10 + (u32)(*q++ - '0');
            q = next_word(q);
            t = &c->t[c->n_trk];
            t->num = (u8)num; t->file = (u8)(c->n_files - 1);
            if (word_is(q, "AUDIO")) { t->audio = 1; t->bsize = RAW; }
            else if (word_is(q, "MODE1/2048")) t->bsize = 2048;
            else if (word_is(q, "MODE1/2352")) { t->bsize = RAW; t->hdr = 16; }
            else if (word_is(q, "MODE2/2352")) { t->bsize = RAW; t->hdr = 24; }
            else if (word_is(q, "MODE2/2336")) { t->bsize = 2336; t->hdr = 8; }
            else { kprintf("cd: %s: track %u: unsupported mode\n", path, num); return 8; }
            if (num < 1 || num > MAX_TRACKS || (c->n_trk && num != c->t[c->n_trk - 1].num + 1u)) return 8;
            t->lba = 0xFFFFFFFFu;                           /* set by INDEX 01 */
            c->n_trk++;
        } else if (word_is(line, "PREGAP") && t) {
            u32 f;
            if (msf_frames(next_word(line), &f)) return 8;
            pregap += f;
        } else if (word_is(line, "INDEX") && t) {
            const char *q = next_word(line);
            u32 ix = 0, f;
            while (*q >= '0' && *q <= '9') ix = ix * 10 + (u32)(*q++ - '0');
            if (ix != 1) continue;
            if (msf_frames(next_word(q), &f)) return 8;
            t->lba = base + f + pregap;
            if (last_file_trk < 0) t->off = f * t->bsize;  /* first track in this file */
            else t->off = c->t[last_file_trk].off + (f - prev_f) * c->t[last_file_trk].bsize;
            prev_f = f;
            last_file_trk = (int)(t - c->t);
        }
    }
    if (!c->n_trk) return 8;
    for (int i = 0; i < c->n_trk; i++) if (c->t[i].lba == 0xFFFFFFFFu || (i && c->t[i].lba < c->t[i - 1].lba)) return 8;
    struct ctrack *lt = &c->t[c->n_trk - 1];
    c->leadout = lt->lba + (c->f[lt->file].size > lt->off ? (c->f[lt->file].size - lt->off) / lt->bsize : 0);
    *out = c;
    return 0;
}

/* Put the ISO file or CUE sheet at path (on C:) in drive u. 0, or 2 not
   found, 3 not an ISO image, 4 not on C:, 6 out of memory, 7 disk error,
   8 a CUE sheet vmdos can't use. */
static int mount_file(int u, const char *path)
{
    if (path[0] && path[1] == ':' && (path[0] | 32) != 'c') return 4;
    static struct fatvol v;
    if (disk_volume(&v)) return 7;
    struct bfile f;
    int r = open_file(&v, path, &f);
    if (r) return r;
    int slot = -1;
    for (int i = n_mod; i < n_img; i++) {                 /* a file slot no drive uses */
        int used = 0;
        for (int k = 0; k < MAX_UNITS; k++) if (unit_img[k] == i) used = 1;
        if (!used && slot < 0) slot = i;
    }
    if (slot < 0) { if (n_img == MAX_IMAGES) return 6; slot = n_img; }
    int pl = 0;
    while (path[pl]) pl++;
    int is_cue = pl > 4 && path[pl - 4] == '.' && (path[pl - 3] | 32) == 'c' && (path[pl - 2] | 32) == 'u' && (path[pl - 1] | 32) == 'e';
    img[slot].data = 0; img[slot].ext = 0; img[slot].n_ext = 0; img[slot].cue = 0;
    if (is_cue) {
        struct cue *c;
        if ((r = cue_open(&v, path, &f, &c))) return r;
        img[slot].cue = c;
        img[slot].sectors = c->leadout;
        struct toc *t = &img[slot].toc;
        t->first = c->t[0].num; t->last = c->t[c->n_trk - 1].num;
        for (int i = 0; i < c->n_trk; i++) { t->start[c->t[i].num] = c->t[i].lba; t->ctl[c->t[i].num] = c->t[i].audio ? 0x10 : 0x14; }
        t->start[t->last + 1] = c->leadout;
    } else {
        if (f.size < 0x8800) return 3;
        img[slot].ext = f.ext; img[slot].n_ext = f.n_ext;
        img[slot].sectors = f.size / 2048;
        toc_one_data_track(&img[slot].toc, img[slot].sectors);
    }
    if (img[slot].toc.ctl[img[slot].toc.first] & 4) {
        static u8 pvd[2048];
        if (img_read(slot, img[slot].toc.start[img[slot].toc.first] + 16, 1, pvd)) return 7;
        if (memcmp(pvd + 1, "CD001", 5)) return 3;
    }
    set_name(slot, path);
    if (slot == n_img) n_img++;
    unit_img[u] = slot;
    changed[u] = 1;
    struct toc *t = &img[slot].toc;
    int audio = 0;
    for (int i = t->first; i <= t->last; i++) audio += !(t->ctl[i] & 4);
    kprintf("cd: drive %d now holds %s (%u MiB, tracks %d-%d, %d audio)\n", u + 1, img[slot].name,
            img[slot].sectors >> 9, t->first, t->last, audio);
    return 0;
}

/* ---------------- real drives ---------------- */
static int n_phys;
static struct {
    int usb, ide, idx;                   /* usb_cd_*, ide_cd_* or ahci_cd_* drive idx */
    int present;                         /* a readable disc is in */
    u32 sectors;
    struct toc toc;
} ph[MAX_PHYS];

static int pkt(int d, const u8 *cdb, void *buf, u32 bytes)
{
    return ph[d].usb ? usb_cd_packet(ph[d].idx, cdb, buf, bytes) : ph[d].ide ? ide_cd_packet(ph[d].idx, cdb, buf, bytes)
                     : ahci_cd_packet(ph[d].idx, cdb, buf, bytes);
}
static int pkt_asc(int d) { return ph[d].usb ? usb_cd_asc() : ph[d].ide ? ide_cd_asc() : ahci_cd_asc(); }
static const char *pmodel(int d)
{
    return ph[d].usb ? usb_cd_model(ph[d].idx) : ph[d].ide ? ide_cd_model(ph[d].idx) : ahci_cd_model(ph[d].idx);
}

static int packet(int d, u8 c0, u32 lba, u16 len, u8 b9, void *buf, u32 bytes)
{
    u8 cdb[12] = { c0 };
    cdb[2] = (u8)(lba >> 24); cdb[3] = (u8)(lba >> 16); cdb[4] = (u8)(lba >> 8); cdb[5] = (u8)lba;
    cdb[7] = (u8)(len >> 8); cdb[8] = (u8)len; cdb[9] = b9;
    return pkt(d, cdb, buf, bytes);
}

static void audio_unit_gone(int unit);

/* Is a disc in drive d? On a change (or the first look) re-read its size
   and table of contents and flag the media change for DOS. */
static void phys_poll(int d, int unit)
{
    /* TEST UNIT READY. Unit attention (6) means a disc change: ask again.
       Not ready (2): "becoming ready" (ASC 04h, a disc just went in, still
       spinning up) is waited for, up to 10 s; "no medium" (3Ah) is asked
       once more, as drives (and QEMU) report it once around a change.
       The packet functions have already read the sense data. */
    int k = -1, waited = 0, empty = 0;
    for (int tries = 0; tries < 8; tries++) {
        k = packet(d, 0x00, 0, 0, 0, 0, 0);
        if (k == 6) { changed[unit] = 1; ph[d].present = 0; continue; }
        if (k != 2) break;
        int asc = pkt_asc(d);
        if (asc == 0x04 && waited < 10000) {
            for (int i = 0; i < 250000; i++) inb(0x80);                 /* ~250 ms */
            waited += 250; tries--; continue;
        }
        if (asc != 0x3A || ++empty > 1) break;
    }
    if (k) {
        if (ph[d].present) { changed[unit] = 1; audio_unit_gone(unit); }
        ph[d].present = 0;
        return;
    }
    if (ph[d].present) return;
    audio_unit_gone(unit);
    if (packet(d, 0x25, 0, 0, 0, xbuf, 8)) return;                   /* READ CAPACITY(10) */
    u32 last = (u32)xbuf[0] << 24 | xbuf[1] << 16 | xbuf[2] << 8 | xbuf[3];
    struct toc *t = &ph[d].toc;
    ph[d].sectors = last + 1;
    toc_one_data_track(t, ph[d].sectors);
    /* READ TOC, format 0, LBA addresses */
    u8 cdb[12] = { 0x43, 0, 0, 0, 0, 0, 1, 0x03, 0x24, 0 };
    if (!pkt(d, cdb, xbuf, 804) && xbuf[2] >= 1 && xbuf[3] >= xbuf[2] && xbuf[3] <= MAX_TRACKS) {
        int n = ((xbuf[0] << 8 | xbuf[1]) - 2) / 8;
        t->first = xbuf[2]; t->last = xbuf[3];
        for (int i = 0; i < n && i < MAX_TRACKS + 1; i++) {
            u8 *e = xbuf + 4 + i * 8;
            u32 a = (u32)e[4] << 24 | e[5] << 16 | e[6] << 8 | e[7];
            if (e[2] == 0xAA) t->start[t->last + 1] = a;
            else if (e[2] >= 1 && e[2] <= MAX_TRACKS) { t->start[e[2]] = a; t->ctl[e[2]] = e[1]; }
        }
    }
    ph[d].present = 1;
    changed[unit] = 1;
    int audio = 0;
    for (int i = t->first; i <= t->last; i++) audio += !(t->ctl[i] & 4);
    kprintf("cd: drive %d (%s): disc in, %u KiB, tracks %d-%d, %d audio\n", unit + 1, pmodel(d),
            ph[d].sectors * 2, t->first, t->last, audio);
}

/* n sectors from start: cooked (2048 bytes) or raw (2352, READ CD,
   audio). 0, or a sense key, or -1. */
static int phys_read(int d, u32 start, u32 n, u8 *buf, int raw)
{
    u32 sz = raw ? RAW : 2048, most = XBUF / sz;
    while (n) {
        u32 k = n > most ? most : n;
        int e;
        if (raw) {                                                      /* READ CD: CD-DA, user data */
            u8 cdb[12] = { 0xBE, 0x04, (u8)(start >> 24), (u8)(start >> 16), (u8)(start >> 8), (u8)start,
                           (u8)(k >> 16), (u8)(k >> 8), (u8)k, 0x10, 0, 0 };
            e = pkt(d, cdb, xbuf, k * sz);
        } else e = packet(d, 0x28, start, (u16)k, 0, xbuf, k * sz);       /* READ(10) */
        if (e) return e;
        if (buf != xbuf) memcpy(buf, xbuf, k * sz);
        buf += k * sz; start += k; n -= k;
    }
    return 0;
}

static void units_init(void)
{
    if (n_units >= 0) return;
    xbuf = phys_try_alloc(XBUF);
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
    if (xbuf && !strstr(cmdline, "cdphys=off")) {
        if (!strstr(cmdline, "ahci=off")) {
            ahci_init();                                /* already done when C: is on a disk */
            for (int i = 0; i < ahci_cd_count() && n_phys < MAX_PHYS; i++) { ph[n_phys].usb = 0; ph[n_phys].idx = i; n_phys++; }
        }
        if (!strstr(cmdline, "ide=off")) {
            ide_init();                                 /* already done when C: is on a disk */
            for (int i = 0; i < ide_cd_count() && n_phys < MAX_PHYS; i++) { ph[n_phys].ide = 1; ph[n_phys].idx = i; n_phys++; }
        }
        usb_settle(4000);                               /* drives still connecting */
        for (int i = 0; i < usb_cd_count() && n_phys < MAX_PHYS; i++) { ph[n_phys].usb = 1; ph[n_phys].idx = i; n_phys++; }
        for (int d = 0; d < n_phys; d++)
            kprintf("cd: drive %d is the real %s drive %s\n", n_units + d + 1,
                    ph[d].usb ? "USB" : ph[d].ide ? "IDE" : "SATA", pmodel(d));
    }
}

/* The real drive behind unit u, or -1 for an image drive. */
static int phys_of(int unit) { return unit >= n_units ? unit - n_units : -1; }

/* The disc in a unit: its TOC, or 0 for none. */
static struct toc *unit_toc(int unit)
{
    int d = phys_of(unit);
    if (d >= 0) return ph[d].present ? &ph[d].toc : 0;
    return unit_img[unit] >= 0 ? &img[unit_img[unit]].toc : 0;
}

/* n raw frames (audio) from a unit. 0, or a sense key (5: the drive can't
   read audio), or -1. */
static int unit_raw(int unit, u32 lba, u32 n, u8 *buf)
{
    int d = phys_of(unit);
    if (d >= 0) {
        if (!ph[d].present) return 2;
        struct toc *t = &ph[d].toc;
        while (n) {                                                     /* data tracks: silence, not READ CD */
            int tr = t->first;
            while (tr < t->last && t->start[tr + 1] <= lba) tr++;
            u32 k = n, next = t->start[tr + 1];
            if (next > lba && k > next - lba) k = next - lba;
            if (t->ctl[tr] & 4) memset(buf, 0, k * RAW);
            else { int e = phys_read(d, lba, k, buf, 1); if (e) return e; }
            buf += k * RAW; lba += k; n -= k;
        }
        return 0;
    }
    int im = unit_img[unit];
    if (im < 0) return 2;
    if (img[im].cue) return cue_read(img[im].cue, lba, n, buf, 1) ? -1 : 0;
    memset(buf, 0, n * RAW);                                            /* an ISO: all data, no sound */
    return 0;
}

/* ---------------- CD audio ---------------- */
#define RING 16384                       /* 44.1 kHz stereo frames (~0.37 s) */
#define SPF 588                          /* samples (stereo frames) in a CD frame */
static u32 ring[RING];
static volatile u32 wr, rd;              /* free-running positions in ring */
static u32 frac;                         /* 16.16 position between rd and rd + 1 */
static struct {
    int unit;                            /* -1: none */
    volatile int state;                  /* 0 stopped, 1 playing, 2 paused */
    int analog;                          /* the drive plays it itself */
    u32 start, end;                      /* the play: [start, end) */
    u32 fill;                            /* next frame to read into the ring */
    u32 base, rbase;                     /* frame `base` is at ring position rbase */
    u32 pos;                             /* analog: where the drive is */
    u32 polled;                          /* analog: ticks at the last READ SUB-CHANNEL */
} pl = { .unit = -1 };
static u8 chan[8] = { 0, 0xFF, 1, 0xFF, 2, 0, 3, 0 };   /* IOCTL 4 / IOCTL out 3: input, volume per output */

static void ring_reset(u32 at)
{
    wr = rd = 0; frac = 0;
    pl.fill = pl.base = at; pl.rbase = 0;
}

/* Read frames into the ring. Returns 0, or a sense key / -1 on failure. */
static int fill_ring(u32 most)
{
    u32 space = RING - (wr - rd), k = space / SPF;
    if (k > most) k = most;
    if (k > XFRAMES) k = XFRAMES;
    if (k > pl.end - pl.fill) k = pl.end - pl.fill;
    if (!k) return 0;
    int e = unit_raw(pl.unit, pl.fill, k, xbuf);
    if (e) return e;
    const u32 *s = (const u32 *)xbuf;
    for (u32 i = 0; i < k * SPF; i++) ring[(wr + i) & (RING - 1)] = s[i];
    wr += k * SPF;
    pl.fill += k;
    return 0;
}

/* Where the play is now (LBA). */
static u32 play_pos(void)
{
    if (pl.analog) return pl.pos;
    u32 p = pl.base + (rd - pl.rbase) / SPF;
    return p > pl.end ? pl.end : p;
}

static void analog_cmd(u8 op, u32 a, u32 b)
{
    int d = phys_of(pl.unit);
    u8 cdb[12] = { op };
    if (op == 0x47) {                                                   /* PLAY AUDIO MSF */
        a += 150; b += 150;
        cdb[3] = (u8)(a / 4500); cdb[4] = (u8)(a / 75 % 60); cdb[5] = (u8)(a % 75);
        cdb[6] = (u8)(b / 4500); cdb[7] = (u8)(b / 75 % 60); cdb[8] = (u8)(b % 75);
    } else if (op == 0x4B) cdb[8] = (u8)a;                              /* PAUSE / RESUME */
    pkt(d, cdb, 0, 0);
}

/* Analog play: ask the drive where it is (at most every 200 ms). */
static void analog_poll(void)
{
    if (!pl.analog || pl.state != 1 || ticks - pl.polled < 200) return;
    pl.polled = ticks;
    u8 cdb[12] = { 0x42, 0, 0x40, 1, 0, 0, 0, 0, 16, 0 };               /* READ SUB-CHANNEL: position */
    if (pkt(phys_of(pl.unit), cdb, xbuf, 16)) return;
    pl.pos = (u32)xbuf[8] << 24 | xbuf[9] << 16 | xbuf[10] << 8 | xbuf[11];
    if (xbuf[1] == 0x13 || xbuf[1] == 0x14 || xbuf[1] == 0x15) { pl.state = 0; pl.pos = pl.end; }
}

static int playing(int unit) { analog_poll(); return pl.unit == unit && pl.state == 1; }

static void stop_all(void)
{
    if (pl.analog && pl.state == 1) analog_cmd(0x4B, 0, 0);
    pl.state = 0;
}

void cdaudio_stop(void) { stop_all(); pl.unit = -1; }

static void audio_unit_gone(int unit) { if (pl.unit == unit) { stop_all(); pl.unit = -1; } }

/* Start playing [start, end) on a unit. ST_DONE-style status. */
#define ST_DONE 0x0100
#define ST_BUSY 0x0200
#define ST_ERR(e) (0x8100 | (e))          /* 2 not ready, 3 unknown command, 8 sector not found, F invalid disk change */
static u16 play(int unit, u32 start, u32 end)
{
    stop_all();
    pl.unit = unit;
    pl.start = start; pl.end = end; pl.analog = 0;
    ring_reset(start);
    if (start >= end) return ST_DONE;
    int e = fill_ring(XFRAMES);                                         /* spins the drive up, here, not in an IRQ */
    if (e == 5 && phys_of(unit) >= 0) {                                 /* no digital audio: the drive plays it */
        pl.analog = 1; pl.pos = start; pl.polled = ticks;
        analog_cmd(0x47, start, end);
        kprintf("cd: drive %d can't read audio digitally; playing it through the drive's own output\n", unit + 1);
    } else if (e) {
        pl.unit = -1;
        return ST_ERR(e == 2 ? 2 : 0xB);
    }
    pl.state = 1;
    dbg(1, "cd: play %u-%u on drive %d\n", start, end, unit + 1);
    return ST_DONE | ST_BUSY;
}

/* Timer interrupt, every few ticks: keep the ring topped up. Monitor code
   runs with interrupts off, so this never cuts into a DOS request. */
void cdaudio_pump(void)
{
    static int in;
    int disk_busy(void);
    if (in || disk_busy() || pl.state != 1 || pl.analog || pl.unit < 0) return;
    if (RING - (wr - rd) < 4 * SPF && pl.fill < pl.end) return;          /* full enough */
    in = 1;
    int e = fill_ring(XFRAMES);
    if (e > 0) { kprintf("cd: audio read failed (sense key %d): stopped\n", e); pl.state = 0; }
    in = 0;
}

/* Mix n 48 kHz stereo frames of CD audio into buf (or, buf 0, just let the
   play run on: no sound card). From sound_tick. */
void cdaudio_mix(int16_t *buf, int n)
{
    if (pl.state != 1 || pl.analog) return;
    int vl = chan[1], vr = chan[3];
    int il = chan[0] & 1, ir = chan[2] & 1;
    for (int i = 0; i < n; i++) {
        if (wr - rd < 2) {
            if (pl.fill >= pl.end) { rd = wr; pl.state = 0; }         /* played to the end */
            return;
        }
        if (buf) {
            u32 a = ring[rd & (RING - 1)], b = ring[(rd + 1) & (RING - 1)];
            int f = (int)(frac >> 1);                                   /* 0..32767 */
            int ch[2];
            for (int c = 0; c < 2; c++) {
                int sa = (int16_t)(c ? a >> 16 : a), sb = (int16_t)(c ? b >> 16 : b);
                ch[c] = sa + (((sb - sa) * f) >> 15);
            }
            int l = buf[i * 2] + ch[il] * vl / 255, r = buf[i * 2 + 1] + ch[ir] * vr / 255;
            buf[i * 2] = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            buf[i * 2 + 1] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        frac += 60211;                                                  /* 44100 / 48000 in 16.16 */
        while (frac >= 65536) { frac -= 65536; rd++; }
    }
}

static u16 stop_audio(int unit)
{
    if (pl.unit != unit) return ST_DONE;
    if (pl.state == 1) {                                                /* pause */
        if (pl.analog) { analog_poll(); pl.polled = 0; analog_cmd(0x4B, 0, 0); }
        else { u32 p = play_pos(); ring_reset(p); }
        pl.state = 2;
    } else if (pl.state == 2) {                                         /* stop: forget the resume point */
        if (pl.analog) analog_cmd(0x4E, 0, 0);
        pl.state = 0;
    }
    return ST_DONE;
}

static u16 resume_audio(int unit)
{
    if (pl.unit != unit || pl.state != 2) return ST_ERR(0xC);
    if (pl.analog) { analog_cmd(0x4B, 1, 0); pl.state = 1; return ST_DONE | ST_BUSY; }
    pl.state = 1;
    int e = fill_ring(XFRAMES);
    if (e > 0) { pl.state = 0; return ST_ERR(e == 2 ? 2 : 0xB); }
    return ST_DONE | ST_BUSY;
}

/* ---------------- MSCDEX device-driver requests ---------------- */
static u32 msf(u32 lba) { lba += 150; return (lba / 4500) << 16 | (lba / 75 % 60) << 8 | (lba % 75); }
static u32 msf_rel(u32 n) { return (n / 4500) << 16 | (n / 75 % 60) << 8 | (n % 75); }
static u32 from_rb(u32 a) { return ((a >> 16 & 0xFF) * 60 + (a >> 8 & 0xFF)) * 75 + (a & 0xFF) - 150; }

static u16 ioctl_in(int unit, u32 cb, u16 drv_cs)
{
    int d = phys_of(unit);
    if (d >= 0) phys_poll(d, unit);
    struct toc *t = unit_toc(unit);
    u32 sectors = !t ? 0 : d >= 0 ? ph[d].sectors : img[unit_img[unit]].sectors;
    int tr;
    switch (rd8(cb)) {
    case 0: wr16(cb + 1, 0); wr16(cb + 3, drv_cs); return ST_DONE;     /* device header address */
    case 1: {                                                           /* location of head */
        u32 p = pl.unit == unit && pl.state ? play_pos() : 0;
        wr32(cb + 2, rd8(cb + 1) ? msf(p) : p);
        return ST_DONE; }
    case 4: for (int i = 0; i < 8; i++) wr8(cb + 1 + i, chan[i]); return ST_DONE;   /* audio channel info */
    case 5: wr8(cb + 1, 0); return ST_DONE;                             /* drive bytes */
    case 6:                                                             /* status: unlocked, HSG+RB, audio */
        wr32(cb + 1, 0x00000312u | (t ? 0 : d >= 0 ? 0x801 : 0x800));
        return ST_DONE;
    case 7: wr16(cb + 2, 2048); return ST_DONE;                         /* sector size */
    case 8: if (!t) return ST_ERR(2); wr32(cb + 1, sectors); return ST_DONE;   /* volume size */
    case 9:                                                             /* media changed */
        wr8(cb + 1, changed[unit] ? 0xFF : 1);
        changed[unit] = 0;
        return ST_DONE;
    case 10:                                                            /* audio disc info */
        if (!t) return ST_ERR(2);
        wr8(cb + 1, (u8)t->first); wr8(cb + 2, (u8)t->last); wr32(cb + 3, msf(t->start[t->last + 1]));
        return ST_DONE;
    case 11:                                                            /* audio track info */
        if (!t) return ST_ERR(2);
        tr = rd8(cb + 1);
        if (tr < t->first || tr > t->last) return ST_ERR(8);
        wr32(cb + 2, msf(t->start[tr]));
        wr8(cb + 6, (u8)(t->ctl[tr] << 4 | t->ctl[tr] >> 4));          /* CONTROL in the high nibble */
        return ST_DONE;
    case 12: {                                                          /* audio Q-channel info */
        if (!t) return ST_ERR(2);
        u32 p = pl.unit == unit && pl.state ? play_pos() : 0;
        for (tr = t->first; tr < t->last && t->start[tr + 1] <= p; tr++) ;
        wr8(cb + 1, (u8)(t->ctl[tr] << 4 | t->ctl[tr] >> 4));
        wr8(cb + 2, (u8)tr); wr8(cb + 3, 1);
        u32 rel = msf_rel(p >= t->start[tr] ? p - t->start[tr] : 0), ab = msf(p);
        wr8(cb + 4, (u8)(rel >> 16)); wr8(cb + 5, (u8)(rel >> 8)); wr8(cb + 6, (u8)rel); wr8(cb + 7, 0);
        wr8(cb + 8, (u8)(ab >> 16)); wr8(cb + 9, (u8)(ab >> 8)); wr8(cb + 10, (u8)ab);
        return ST_DONE; }
    case 15: {                                                          /* audio status */
        int mine = pl.unit == unit;
        playing(unit);
        wr16(cb + 1, mine && pl.state == 2 ? 1 : 0);
        wr32(cb + 3, mine ? msf(pl.state == 2 ? play_pos() : pl.start) : 0);
        wr32(cb + 7, mine ? msf(pl.end) : 0);
        return ST_DONE; }
    }
    return ST_ERR(3);
}

/* IOCTL output: 0 eject, 2 reset, 3 audio channel control, 5 close the
   tray; the rest accepted. */
static u16 ioctl_out(int unit, u32 cb)
{
    int d = phys_of(unit);
    switch (rd8(cb)) {
    case 0: case 5:
        if (d < 0) return ST_DONE;
        audio_unit_gone(unit);
        {
            u8 cdb[12] = { 0x1B, 0, 0, 0, rd8(cb) ? 3 : 2 };            /* START STOP UNIT, LoEj */
            pkt(d, cdb, 0, 0);
        }
        ph[d].present = 0; changed[unit] = 1;
        return ST_DONE;
    case 2:
        audio_unit_gone(unit);
        if (d >= 0) { ph[d].present = 0; changed[unit] = 1; }
        return ST_DONE;
    case 3:
        for (int i = 0; i < 8; i++) chan[i] = rd8(cb + 1 + i);
        return ST_DONE;
    }
    return ST_DONE;
}

static u16 read_long(int unit, u32 rh)
{
    u32 buf = LIN(rd16(rh + 16), rd16(rh + 14)), n = rd16(rh + 18), start = rd32(rh + 20);
    if (rd8(rh + 13) == 1) start = from_rb(start);                      /* Red Book M:S:F */
    if (rd8(rh + 24) != 0) return ST_ERR(3);                            /* cooked only */
    if (buf + n * 2048 > GUEST_TOP) return ST_ERR(0xC);
    int d = phys_of(unit);
    if (d >= 0) {
        if (!ph[d].present) phys_poll(d, unit);
        if (!ph[d].present) return ST_ERR(2);
        if (start + n > ph[d].sectors || start + n < start) return ST_ERR(8);
        int e = phys_read(d, start, n, gptr(buf), 0);
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

/* PLAY AUDIO: rh+13 addressing mode, rh+14 start, rh+18 sectors. */
static u16 play_request(int unit, u32 rh)
{
    struct toc *t = unit_toc(unit);
    int d = phys_of(unit);
    if (d >= 0 && !t) { phys_poll(d, unit); t = unit_toc(unit); }
    if (!t) return ST_ERR(2);
    u32 start = rd32(rh + 14), n = rd32(rh + 18), lead = t->start[t->last + 1];
    if (rd8(rh + 13) == 1) start = from_rb(start);
    if (start >= lead) return ST_ERR(8);
    if (n > lead - start) n = lead - start;
    return play(unit, start, start + n);
}

static void put(u32 *o, const char *s) { for (; *s; s++) wr8((*o)++, (u8)*s); }

/* INT 2Fh AX=5644h. BX=0: number of drives. BX=1: ES:DI = request header,
   DX = driver segment. BX=2: VMCD.COM: CX=0 list (ES:DI <- text), else
   put image DL (1..n, 0 = empty) in drive CL (1..). BX=3: put the ISO
   file or CUE sheet DS:SI (full DOS path, from INT 21h AH=60h) in drive
   CL; AX=0 or an error (see mount_file). */
void cd_api(struct regs *r)
{
    units_init();
    switch (BX(r)) {
    case 0: AX(r) = (u16)(n_units + n_phys); return;
    case 1: {
        u32 rh = LIN(r->v86_es, DI(r));
        int unit = rd8(rh + 1), cmd = rd8(rh + 2);
        u32 cb = LIN(rd16(rh + 16), rd16(rh + 14));
        u16 st;
        if (unit >= n_units + n_phys) st = ST_ERR(1);
        else switch (cmd) {
        case 3: st = ioctl_in(unit, cb, DX(r)); break;
        case 12: st = ioctl_out(unit, cb); break;
        case 128: st = read_long(unit, rh); break;
        case 132: st = play_request(unit, rh); break;
        case 133: st = stop_audio(unit); break;
        case 136: st = resume_audio(unit); break;
        case 13: case 14: case 130: case 131: st = ST_DONE; break;    /* open, close, prefetch, seek */
        default: st = ST_ERR(3);
        }
        if (unit < n_units + n_phys && playing(unit)) st |= ST_BUSY;
        wr16(rh + 3, st);
        return; }
    case 2: {
        if (CX(r) == 0) {                                               /* list into ES:DI ($-terminated) */
            u32 o = LIN(r->v86_es, DI(r));
            /* DX='LE': DS:SI holds the drives' letters (MSCDEX 150Dh, 0 = A:): name them by letter */
            char nm[MAX_UNITS + MAX_PHYS][12];
            for (int u = 0; u < n_units + n_phys; u++) {
                u8 l = DX(r) == 0x4C45 ? rd8(LIN(r->v86_ds, SI(r)) + u) : 0xFF;
                if (l < 26) { nm[u][0] = (char)('A' + l); nm[u][1] = ':'; nm[u][2] = 0; }
                else snprintf(nm[u], sizeof nm[u], "drive %d", u + 1);
            }
            put(&o, "CD images:\r\n");
            for (int i = 0; i < n_img; i++) {
                int used = 0;
                for (int u = 0; u < n_units; u++) if (unit_img[u] == i) used = 1;
                if (i >= n_mod && !used) continue;                      /* a file no drive holds any more */
                char num[4] = { ' ', (char)(i < 9 ? '1' + i : 'A' + i - 9), ' ', 0 };
                put(&o, num);
                put(&o, img[i].name);
                for (int u = 0; u < n_units; u++)
                    if (unit_img[u] == i) { put(&o, "  (in "); put(&o, nm[u]); put(&o, ")"); }
                put(&o, "\r\n");
            }
            for (int u = 0; u < n_units; u++)
                if (unit_img[u] < 0) { put(&o, " "); put(&o, nm[u]); put(&o, " empty\r\n"); }
            for (int d = 0; d < n_phys; d++) {
                put(&o, " "); put(&o, nm[n_units + d]);
                put(&o, ph[d].usb ? " real USB drive " : ph[d].ide ? " real IDE drive " : " real SATA drive ");
                put(&o, pmodel(d)); put(&o, "\r\n");
            }
            wr8(o, '$');
            AX(r) = 0;
            return;
        }
        int u = CL(r) - 1, i = DL(r) - 1;
        if (u < 0 || u >= n_units || i < -1 || i >= n_mod) { AX(r) = 1; return; }
        audio_unit_gone(u);
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
        audio_unit_gone(u);
        AX(r) = (u16)mount_file(u, path);
        return; }
    }
    AX(r) = 0xFFFF;
}
