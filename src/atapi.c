/* An IDE channel for the guest with ATAPI CD-ROM drives on it: the
   secondary channel's ports (170h-177h, 376h) and IRQ 15, the master
   being VMCD's drive 1 and the slave its drive 2 (an image drive, or a
   real drive on SATA/IDE/USB: cd.c's units). The PnP BIOS reports the
   channel (vpci.c), so Windows 9x finds a "Standard IDE/ESDI Hard Disk
   Controller", drives it with its own protected-mode driver (ESDI_506)
   and reads the discs with CDFS: Joliet's long file names, which DOS's
   CD layer (VMCD.SYS + SHSUCDX) never shows, autoplay, CD audio.

   PIO only (no bus master DMA): the guest's REP INSW is one exit for a
   whole transfer in VMHV. Commands finish at once (BSY is never seen);
   each data block and the end raise IRQ 15 unless nIEN. atapi=off: no
   channel; atapilog logs the commands and how they ended, and register accesses. */
#include "kernel.h"

int cd_unit_count(void);
u32 cd_unit_gen(int unit);
int cd_unit_present(int unit);
u32 cd_unit_sectors(int unit);
int cd_unit_toc(int unit, int *first, int *last, u32 *start, u8 *ctl);
int cd_unit_read(int unit, u32 lba, u32 n, u8 *buf);
int cd_unit_play(int unit, u32 start, u32 end);
void cd_unit_pause(int unit);
void cd_unit_resume(int unit);
void cd_unit_stop(int unit);
int cd_unit_audio(int unit, u32 *pos);

#define BASE 0x170
#define CTRL 0x376
#define IRQ  15

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DSC  0x10
#define ST_DRDY 0x40

enum { PH_IDLE, PH_PACKET, PH_IN, PH_OUT };

static int on = -1;                      /* -1: not decided yet */
static u8 feat, err, nsect, sect, cyl_lo, cyl_hi, dh, dctl, status;
static struct dev {
    int unit;                            /* cd.c's unit, -1: none */
    u32 gen;                             /* media changes seen */
    u8 key, asc, ascq;                   /* sense */
} dv[2];

/* The transfer in progress (one device at a time, as on a channel). */
static int phase;
static u8 cdb[12];
static int cdb_n;
static u8 *buf;                          /* 64 KiB + room */
static u32 buf_len, buf_pos;             /* what's in buf, what's been taken */
static u32 total;                        /* bytes left in the whole transfer */
static u32 chunk;                        /* bytes left in this DRQ block */
static u32 bcl;                          /* byte count limit (PACKET) */
static u32 rd_lba, rd_left;              /* READ: sectors still to fetch */
static int rd_unit;
static int ata_pio;                      /* a plain ATA PIO data-in (IDENTIFY PACKET DEVICE): no IRQ at the end */
#define BUFSZ 65536

static struct dev *cur(void) { return &dv[(dh >> 4) & 1]; }
static int alog;                         /* atapilog: commands and their results, the first 3000 */
#define ALOG(...) do { if (alog > 0) { alog--; kprintf(__VA_ARGS__); } } while (0)
static void irq(void) { if (!(dctl & 2)) vpic_raise(IRQ); }

static void signature(void)
{
    nsect = 1; sect = 1; cyl_lo = 0x14; cyl_hi = 0xEB;
}

static void decide(void)
{
    if (on >= 0) return;
    on = !strstr(cmdline, "atapi=off");
    if (!on) return;
    if (strstr(cmdline, "atapilog")) alog = 3000;
    buf = phys_try_alloc(BUFSZ + 4096);
    if (!buf) { on = 0; kprintf("atapi: no memory for the IDE channel: off\n"); return; }
    int n = cd_unit_count();
    dv[0].unit = n > 0 ? 0 : -1;
    dv[1].unit = n > 1 ? 1 : -1;
    kprintf("atapi: the guest touched the IDE channel at 170h: master %s, slave %s\n",
            n > 0 ? "CD drive 1" : "none", n > 1 ? "CD drive 2" : "none");   /* (once: the first access) */
    for (int i = 0; i < 2; i++) if (dv[i].unit >= 0) dv[i].gen = cd_unit_gen(dv[i].unit);
    signature();
    err = 1; status = 0; dh = 0xA0;
}

int atapi_on(void) { return !strstr(cmdline, "atapi=off"); }   /* (vpci.c's PnP BIOS node, before any access) */

/* ---------------- phases ---------------- */
static void done(void)                   /* command complete: status, IRQ */
{
    ALOG("atapi: %s: done\n", (dh & 0x10) ? "slave" : "master");
    phase = PH_IDLE;
    nsect = 3;                           /* I/O, C/D: status */
    status = ST_DRDY | ST_DSC;
    err = 0;
    irq();
}

static void fail(u8 key, u8 asc, u8 ascq)
{
    struct dev *d = cur();
    ALOG("atapi: %s: failed, sense %x/%02x/%02x\n", (dh & 0x10) ? "slave" : "master", key, asc, ascq);
    d->key = key; d->asc = asc; d->ascq = ascq;
    phase = PH_IDLE;
    nsect = 3;
    err = (u8)(key << 4 | 4);            /* sense key, ABRT */
    status = ST_DRDY | ST_DSC | ST_ERR;
    irq();
}

static void ok_sense(void) { struct dev *d = cur(); d->key = d->asc = d->ascq = 0; }

/* Fill buf with the next sectors of a READ. 0, or a sense key. */
static int refill(void)
{
    u32 n = rd_left > BUFSZ / 2048 ? BUFSZ / 2048 : rd_left;
    int e = cd_unit_read(rd_unit, rd_lba, n, buf);
    if (e) return e;
    rd_lba += n; rd_left -= n;
    buf_len = n * 2048; buf_pos = 0;
    return 0;
}

/* The next DRQ block of a data-in transfer (or the end). */
static void next_block(void)
{
    if (!total) { if (ata_pio) { phase = PH_IDLE; status = ST_DRDY | ST_DSC; ata_pio = 0; } else done(); return; }
    if (buf_pos >= buf_len) {
        int e = refill();
        if (e) { fail(e < 0 ? 4 : (u8)e, e == 2 ? 0x3A : e == 5 ? 0x21 : 0x11, 0); return; }
    }
    u32 c = total;
    if (c > bcl) c = bcl;
    if (c > buf_len - buf_pos) c = buf_len - buf_pos;
    chunk = c;
    phase = PH_IN;
    if (!ata_pio) { nsect = 2; cyl_lo = (u8)c; cyl_hi = (u8)(c >> 8); }   /* I/O: to the host */
    status = ST_DRDY | ST_DSC | ST_DRQ;
    irq();
}

/* Reply with len bytes already in buf (no more than the host allowed). */
static void reply(u32 len, u32 alloc)
{
    if (len > alloc) len = alloc;
    ok_sense();
    rd_left = 0; buf_len = len; buf_pos = 0; total = len;
    next_block();
}

/* ---------------- SCSI (MMC) commands ---------------- */
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static u32 be16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }
static void put32(u8 *p, u32 v) { p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v; }
static void put16(u8 *p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void addr(u8 *p, u32 lba, int msf)
{
    if (!msf) { put32(p, lba); return; }
    lba += 150;
    p[0] = 0; p[1] = (u8)(lba / 4500); p[2] = (u8)(lba / 75 % 60); p[3] = (u8)(lba % 75);
}

static u32 toc_start[101];
static u8 toc_ctl[100];

static void mode_page(u8 **p, int pg, int present)
{
    u8 *q = *p;
    (void)present;
    if (pg == 0x01) { u8 x[8] = { 0x01, 0x06, 0, 5 }; memcpy(q, x, 8); q += 8; }
    else if (pg == 0x0E) {
        u8 x[16] = { 0x0E, 0x0E, 0x04, 0, 0, 0, 0, 0x4B, 1, 0xFF, 2, 0xFF, 0, 0, 0, 0 };
        memcpy(q, x, 16); q += 16;
    } else if (pg == 0x2A) {
        u8 x[20] = { 0x2A, 0x12, 0x00, 0x00, 0x71, 0x03, 0x29, 0x03, 0x0B, 0x00, 0x01, 0x00, 0x02, 0x00, 0x0B, 0x00 };
        memcpy(q, x, 20); q += 20;
    }
    *p = q;
}

static void packet(void)
{
    struct dev *d = cur();
    int u = d->unit;
    u8 op = cdb[0];
    ALOG("atapi: %s: SCSI %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x (bcl %x)\n", (dh & 0x10) ? "slave" : "master",
         cdb[0], cdb[1], cdb[2], cdb[3], cdb[4], cdb[5], cdb[6], cdb[7], cdb[8], cdb[9], bcl);
    memset(buf, 0, 256);
    int media = op == 0x00 || op == 0x25 || op == 0x28 || op == 0xA8 || op == 0x43 || op == 0x45 ||
                op == 0x47 || op == 0xA5 || op == 0x2B || op == 0xBE;
    int present = media ? cd_unit_present(u) : 1;                  /* (a real drive asked first: its change counts now) */
    if (op != 0x12 && op != 0x03 && op != 0x4A && d->gen != cd_unit_gen(u)) {
        d->gen = cd_unit_gen(u);
        fail(6, 0x28, 0);                /* unit attention: the medium may have changed */
        return;
    }
    if (media && !present) {
        d->gen = cd_unit_gen(u);
        fail(2, 0x3A, 0);                /* not ready: no medium */
        return;
    }
    switch (op) {
    case 0x00:                                                     /* TEST UNIT READY */
    case 0x1B:                                                     /* START STOP UNIT */
    case 0x1E:                                                     /* PREVENT ALLOW MEDIUM REMOVAL */
    case 0x2B:                                                     /* SEEK */
    case 0xBB:                                                     /* SET CD SPEED */
    case 0x35:                                                     /* SYNCHRONIZE CACHE */
        ok_sense(); done(); return;
    case 0x03: {                                                   /* REQUEST SENSE */
        buf[0] = 0x70; buf[2] = d->key; buf[7] = 10; buf[12] = d->asc; buf[13] = d->ascq;
        u32 al = cdb[4];
        d->key = d->asc = d->ascq = 0;
        buf_len = al < 18 ? al : 18; buf_pos = 0; total = buf_len; rd_left = 0;
        next_block();
        return; }
    case 0x12:                                                     /* INQUIRY */
        buf[0] = 0x05; buf[1] = 0x80; buf[2] = 0x00; buf[3] = 0x21; buf[4] = 31;
        memcpy(buf + 8, "VMDOS   ", 8);
        memcpy(buf + 16, u == 0 ? "VIRTUAL CD-ROM 1" : "VIRTUAL CD-ROM 2", 16);
        memcpy(buf + 32, "1.00", 4);
        reply(36, cdb[4]);
        return;
    case 0x1A: case 0x5A: {                                        /* MODE SENSE (6), (10) */
        int ten = op == 0x5A, pg = cdb[2] & 0x3F, present = cd_unit_present(u);
        u32 alloc = ten ? be16(cdb + 7) : cdb[4], hl = ten ? 8 : 4;
        u8 *p = buf + hl;
        if (pg == 0x3F) { mode_page(&p, 0x01, present); mode_page(&p, 0x0E, present); mode_page(&p, 0x2A, present); }
        else if (pg == 0x01 || pg == 0x0E || pg == 0x2A) mode_page(&p, pg, present);
        else { fail(5, 0x24, 0); return; }                          /* invalid field in CDB */
        u32 len = (u32)(p - buf);
        if (ten) { put16(buf, len - 2); buf[2] = present ? 0x01 : 0x70; }
        else { buf[0] = (u8)(len - 1); buf[1] = present ? 0x01 : 0x70; }
        reply(len, alloc);
        return; }
    case 0x25:                                                     /* READ CAPACITY */
        put32(buf, cd_unit_sectors(u) - 1); put32(buf + 4, 2048);
        reply(8, 8);
        return;
    case 0x28: case 0xA8: {                                        /* READ (10), (12) */
        u32 lba = be32(cdb + 2), n = op == 0x28 ? be16(cdb + 7) : be32(cdb + 6);
        if (!n) { ok_sense(); done(); return; }
        if (lba + n > cd_unit_sectors(u) || lba + n < lba) { fail(5, 0x21, 0); return; }   /* LBA out of range */
        ok_sense();
        rd_unit = u; rd_lba = lba; rd_left = n;
        buf_len = buf_pos = 0; total = n * 2048;
        next_block();
        return; }
    case 0x43: {                                                   /* READ TOC */
        int msf = (cdb[1] >> 1) & 1, fmt = cdb[2] & 0x0F, first, last;
        if (!fmt && cdb[9] >> 6) fmt = cdb[9] >> 6;                /* the old place for it */
        u32 alloc = be16(cdb + 7);
        if (cd_unit_toc(u, &first, &last, toc_start, toc_ctl)) { fail(2, 0x3A, 0); return; }
        u8 *p = buf + 4;
        if (fmt == 0) {
            int t0 = cdb[6] ? cdb[6] : first;
            if (t0 > last && t0 != 0xAA) { fail(5, 0x24, 0); return; }
            for (int t = t0 == 0xAA ? last + 1 : t0; t <= last + 1; t++) {
                p[1] = t > last ? 0x14 : toc_ctl[t];
                p[2] = (u8)(t > last ? 0xAA : t);
                addr(p + 4, toc_start[t], msf);
                p += 8;
            }
            buf[2] = (u8)first; buf[3] = (u8)last;
        } else if (fmt == 1) {                                     /* session info: one session */
            p[1] = toc_ctl[first]; p[2] = (u8)first; addr(p + 4, toc_start[first], msf);
            p += 8;
            buf[2] = 1; buf[3] = 1;
        } else { fail(5, 0x24, 0); return; }
        u32 len = (u32)(p - buf);
        put16(buf, len - 2);
        reply(len, alloc);
        return; }
    case 0x42: {                                                   /* READ SUB-CHANNEL */
        int msf = (cdb[1] >> 1) & 1, subq = cdb[2] & 0x40, fmt = cdb[3];
        u32 alloc = be16(cdb + 7), pos;
        int st = cd_unit_audio(u, &pos);
        buf[1] = st == 1 ? 0x11 : st == 2 ? 0x12 : 0x15;
        u32 len = 4;
        if (subq && fmt == 1) {
            int first, last;
            u8 *p = buf + 4;
            p[0] = 1;
            if (!cd_unit_toc(u, &first, &last, toc_start, toc_ctl)) {
                int t = first;
                while (t < last && toc_start[t + 1] <= pos) t++;
                p[1] = toc_ctl[t]; p[2] = (u8)t; p[3] = 1;
                addr(p + 4, pos, msf);
                u32 rel = pos >= toc_start[t] ? pos - toc_start[t] : 0;
                if (msf) { p[8] = 0; p[9] = (u8)(rel / 4500); p[10] = (u8)(rel / 75 % 60); p[11] = (u8)(rel % 75); }
                else put32(p + 8, rel);
            }
            len += 12;
        } else if (subq && (fmt == 2 || fmt == 3)) { buf[4] = (u8)fmt; len += 20; }   /* no UPC / ISRC */
        put16(buf + 2, len - 4);
        reply(len, alloc);
        return; }
    case 0x45: case 0xA5: case 0x47: {                             /* PLAY AUDIO (10), (12), MSF */
        u32 s, e;
        if (op == 0x47) {
            s = (cdb[3] * 60 + cdb[4]) * 75 + cdb[5] - 150;
            e = (cdb[6] * 60 + cdb[7]) * 75 + cdb[8] - 150;
            if (cdb[3] == 0xFF && cdb[4] == 0xFF && cdb[5] == 0xFF) { u32 p; cd_unit_audio(u, &p); s = p; }
        } else {
            s = be32(cdb + 2);
            e = s + (op == 0x45 ? be16(cdb + 7) : be32(cdb + 6));
        }
        if (e <= s) { ok_sense(); done(); return; }
        if (cd_unit_play(u, s, e)) { fail(5, 0x64, 0); return; }  /* illegal mode for this track */
        ok_sense(); done();
        return; }
    case 0x4B:                                                     /* PAUSE / RESUME */
        if (cdb[8] & 1) cd_unit_resume(u); else cd_unit_pause(u);
        ok_sense(); done();
        return;
    case 0x4E:                                                     /* STOP PLAY / SCAN */
        cd_unit_stop(u);
        ok_sense(); done();
        return;
    case 0x4A:                                                     /* GET EVENT STATUS NOTIFICATION */
        put16(buf, 2); buf[2] = 0x80; buf[3] = 0x10;              /* no event available; media class */
        reply(4, be16(cdb + 7));
        return;
    case 0xBD:                                                     /* MECHANISM STATUS */
        put16(buf + 6, 0);
        reply(8, be16(cdb + 8));
        return;
    case 0x15: case 0x55: {                                        /* MODE SELECT (6), (10): taken, ignored */
        u32 n = op == 0x15 ? cdb[4] : be16(cdb + 7);
        if (!n) { ok_sense(); done(); return; }
        ok_sense();
        total = n; chunk = n > bcl ? bcl : n;
        phase = PH_OUT;
        nsect = 0; cyl_lo = (u8)chunk; cyl_hi = (u8)(chunk >> 8);
        status = ST_DRDY | ST_DSC | ST_DRQ;
        irq();
        return; }
    }
    fail(5, 0x20, 0);                                              /* invalid command operation code */
}

/* ---------------- ATA commands ---------------- */
static void id_string(u16 *w, int at, int words, const char *s)
{
    for (int i = 0; i < words * 2; i++) {
        char c = *s ? *s++ : ' ';
        u8 *b = (u8 *)&w[at + i / 2];
        b[(i & 1) ^ 1] = (u8)c;                                    /* the first character in the high byte */
    }
}

static void command(u8 c)
{
    struct dev *d = cur();
    ALOG("atapi: %s: ATA %02x (feat %02x, %s)\n", (dh & 0x10) ? "slave" : "master", c, feat, d->unit < 0 ? "no drive" : "drive");
    if (d->unit < 0) return;                                       /* no device there */
    err = 0;
    switch (c) {
    case 0xA0:                                                     /* PACKET */
        if (feat & 1) { err = 4; status = ST_DRDY | ST_DSC | ST_ERR; nsect = 3; irq(); return; }   /* DMA: none */
        bcl = (u32)cyl_hi << 8 | cyl_lo;
        if (!bcl || bcl > 0xFFFE) bcl = 0xFFFE;
        bcl &= ~1u;
        cdb_n = 0;
        phase = PH_PACKET;
        nsect = 1;                                                 /* C/D: the command packet */
        status = ST_DRDY | ST_DSC | ST_DRQ;                        /* (DRQ at once: no interrupt) */
        return;
    case 0xA1: {                                                   /* IDENTIFY PACKET DEVICE */
        u16 *w = (u16 *)buf;
        memset(buf, 0, 512);
        w[0] = 0x85C0;                                             /* ATAPI, CD-ROM, removable, 50 us DRQ, 12-byte packets */
        id_string(w, 10, 10, d->unit ? "VMDOSCD2" : "VMDOSCD1");
        id_string(w, 23, 4, "1.00");
        id_string(w, 27, 20, d->unit ? "VMDOS VIRTUAL CD-ROM 2" : "VMDOS VIRTUAL CD-ROM 1");
        w[49] = 0x0200;                                            /* LBA; no DMA */
        w[53] = 0x0002;                                            /* words 64-70 valid */
        w[64] = 0x0003;                                            /* PIO 3, 4 */
        w[65] = w[66] = w[67] = w[68] = 120;
        w[80] = 0x0010;                                            /* ATA/ATAPI-4 */
        w[82] = 0x4010;                                            /* PACKET */
        w[83] = 0x4000; w[84] = 0x4000; w[85] = 0x4010; w[87] = 0x4000;
        rd_left = 0; buf_len = 512; buf_pos = 0; total = 512; bcl = 512;
        ata_pio = 1;
        next_block();
        return; }
    case 0xEC:                                                     /* IDENTIFY DEVICE: not for ATAPI */
        signature();
        err = 4; status = ST_DRDY | ST_DSC | ST_ERR; irq();
        return;
    case 0x08:                                                     /* DEVICE RESET */
        phase = PH_IDLE; signature(); err = 1; status = 0;
        return;
    case 0x90:                                                     /* EXECUTE DEVICE DIAGNOSTIC */
        phase = PH_IDLE; signature(); err = 1; status = 0; irq();
        return;
    case 0xE5: nsect = 0xFF; /* fall through */                    /* CHECK POWER MODE: active */
    case 0xEF: case 0xE0: case 0xE1: case 0xE2: case 0xE3: case 0xE6: case 0xE7: case 0x91:
        phase = PH_IDLE; status = ST_DRDY | ST_DSC; irq();         /* SET FEATURES, power, flush: fine */
        return;
    }
    err = 4; status = ST_DRDY | ST_DSC | ST_ERR; irq();            /* aborted */
}

/* ---------------- the data port ---------------- */
static u32 data_in(int sz)
{
    if (phase != PH_IN) return 0xFFFFFFFFu >> (32 - 8 * sz);
    u32 v = 0;
    for (int i = 0; i < sz && chunk; i++) {
        v |= (u32)buf[buf_pos++] << (8 * i);
        chunk--; total--;
    }
    if (!chunk) next_block();
    return v;
}

static void data_out(u32 v, int sz)
{
    if (phase == PH_PACKET) {
        for (int i = 0; i < sz && cdb_n < 12; i++) cdb[cdb_n++] = (u8)(v >> (8 * i));
        if (cdb_n == 12) { phase = PH_IDLE; packet(); }
        return;
    }
    if (phase == PH_OUT) {
        u32 n = (u32)sz > chunk ? chunk : (u32)sz;
        chunk -= n; total -= n;
        if (!chunk) {
            if (total) { chunk = total > bcl ? bcl : total; cyl_lo = (u8)chunk; cyl_hi = (u8)(chunk >> 8); irq(); }
            else done();
        }
    }
}

/* ---------------- the ports ---------------- */
int atapi_port(u16 port, int size, int write, u32 *val)
{
    if (!((u32)(port - BASE) < 8u || port == CTRL)) return 0;
    decide();
    if (!on) return 0;
    struct dev *d = cur();
    int present = d->unit >= 0;
    if (alog > 0 && port != BASE) {                                 /* the registers too (not data), a run once */
        static u16 lp; static u32 lv; static int lw, rep;
        u32 v = write ? *val : 0;
        if (port == lp && write == lw && (!write || v == lv)) rep++;
        else {
            if (rep) ALOG("atapi:   (x%d)\n", rep + 1);
            rep = 0; lp = port; lw = write; lv = v;
            if (write) ALOG("atapi:   out %03x <- %02x\n", port, v & 0xFF);
            else ALOG("atapi:   in  %03x\n", port);
        }
    }
    if (port == BASE) {
        if (write) data_out(*val, size); else *val = data_in(size);
        return 1;
    }
    if (size > 1 && !write) {                                       /* a wider read of the registers: bytes */
        u32 v = 0;
        for (int i = 0; i < size; i++) { u32 b; atapi_port((u16)(port + i), 1, 0, &b); v |= (b & 0xFF) << (8 * i); }
        *val = v;
        return 1;
    }
    u8 v = (u8)*val;
    if (write) {
        switch (port) {
        case BASE + 1: feat = v; break;
        case BASE + 2: nsect = v; break;
        case BASE + 3: sect = v; break;
        case BASE + 4: cyl_lo = v; break;
        case BASE + 5: cyl_hi = v; break;
        case BASE + 6: dh = v | 0xA0; break;
        case BASE + 7: command(v); break;
        case CTRL:
            if ((dctl & 4) && !(v & 4)) ALOG("atapi: soft reset\n");
            if ((dctl & 4) && !(v & 4)) {                           /* SRST ends: both reset */
                phase = PH_IDLE; signature(); err = 1; status = 0; dh = 0xA0;
            }
            dctl = v;
            break;
        }
        return 1;
    }
    switch (port) {
    case BASE + 1: v = err; break;
    case BASE + 2: v = nsect; break;
    case BASE + 3: v = sect; break;
    case BASE + 4: v = cyl_lo; break;
    case BASE + 5: v = cyl_hi; break;
    case BASE + 6: v = dh; break;
    case BASE + 7: case CTRL:
        v = present ? (dctl & 4 ? 0x80 : status) : 0;
        break;
    }
    *val = v;
    return 1;
}
