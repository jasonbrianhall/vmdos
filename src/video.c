/* The guest's display: VGA text (B800h), CGA graphics (modes 4-6, B800h) and
   mode 13h (A000h) kept in guest memory, drawn onto the boot framebuffer
   (GRUB / GOP / Bochs VBE), plus the VGA registers and INT 10h.
   With a helper CPU (smp.c) it redraws while DOS runs on the boot CPU:
   video_lock keeps the redraw and the entry points that change the video
   state (mode sets, INT 10h, register writes, page mappings) apart. The
   palette and video memory are written without it, as on the real card. */
#include "kernel.h"
#include "font437.h"

const u8 *vga_font16 = font8x16, *vga_font8 = font8x8;

/* ---------------- framebuffer ---------------- */
static u8 *fb;
static u32 fb_pitch, fb_w, fb_h, fb_bpp;
static u8 rpos = 16, rsz = 8, gpos = 8, gsz = 8, bpos = 0, bsz = 8;
static int text_fallback;                 /* no framebuffer: copy text to real VGA memory */

void video_framebuffer(u64 addr, u32 pitch, u32 w, u32 h, u32 bpp,
                       u8 rp, u8 rs, u8 gp, u8 gs, u8 bp, u8 bs)
{
    if (bpp != 32 && bpp != 24 && bpp != 16 && bpp != 15) {
        kprintf("framebuffer: %u bpp unsupported\n", bpp);
        return;
    }
    fb = (u8 *)(uintptr_t)addr; fb_pitch = pitch; fb_w = w; fb_h = h; fb_bpp = bpp;
    rpos = rp; rsz = rs; gpos = gp; gsz = gs; bpos = bp; bsz = bs;
    kprintf("framebuffer %ux%ux%u at %p pitch %u, rgb %u:%u %u:%u %u:%u\n", w, h, bpp, (u32)addr, pitch, rp, rs, gp, gs, bp, bs);
}

static u32 pack(u8 r6, u8 g6, u8 b6)        /* 6-bit DAC values to a pixel */
{
    u32 r = (r6 << 2 | r6 >> 4), g = (g6 << 2 | g6 >> 4), b = (b6 << 2 | b6 >> 4);
    return (r >> (8 - rsz)) << rpos | (g >> (8 - gsz)) << gpos | (b >> (8 - bsz)) << bpos;
}

/* Every few rows of a redraw: count timer ticks the redraw would lose
   (it runs with interrupts off; a full screen can take several ms). */
static inline void row_poll(void)
{
    static u32 rows;
    if ((++rows & 7) == 0) tick_poll();
}

static void put_row(const u32 *px, u32 n, u32 x, u32 y)
{
    row_poll();
    if (y >= fb_h || x >= fb_w) return;
    if (x + n > fb_w) n = fb_w - x;
    u8 *d = fb + y * fb_pitch;
    if (fb_bpp == 32) memcpy(d + x * 4, px, n * 4);
    else if (fb_bpp == 24) { d += x * 3; for (u32 i = 0; i < n; i++, d += 3) { d[0] = px[i]; d[1] = px[i] >> 8; d[2] = px[i] >> 16; } }
    else { u16 *p = (u16 *)(d + x * 2); for (u32 i = 0; i < n; i++) p[i] = (u16)px[i]; }
}

static void clear_fb(void)
{
    if (!fb) return;
    for (u32 y = 0; y < fb_h; y++) memset(fb + y * fb_pitch, 0, fb_w * ((fb_bpp + 7) / 8));
}

/* ---------------- Bochs / QEMU VBE (when the loader gave no framebuffer) ---------------- */
static u32 pci_read(u32 bus, u32 dev, u32 fn, u32 off)
{
    outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fn << 8 | (off & 0xFC));
    return inl(0xCFC);
}

static int bochs_vbe(void)
{
    outw(0x1CE, 0);
    u16 id = inw(0x1CF);
    if (id < 0xB0C0 || id > 0xB0C5) return 0;
    u32 lfb = 0;
    for (u32 dev = 0; dev < 32 && !lfb; dev++) {
        u32 v = pci_read(0, dev, 0, 0);
        if (v == 0x11111234) lfb = pci_read(0, dev, 0, 0x10) & ~0xFu;
    }
    if (!lfb) return 0;
    const u16 W = 640, H = 480;
    static const u16 regs[][2] = { { 4, 0 }, { 1, 640 }, { 2, 480 }, { 3, 32 }, { 4, 0x41 } };
    for (unsigned i = 0; i < 5; i++) { outw(0x1CE, regs[i][0]); outw(0x1CF, regs[i][1]); }
    map_fb(lfb, W * H * 4);
    video_framebuffer(lfb, W * 4, W, H, 32, 16, 8, 8, 8, 0, 8);
    return 1;
}

/* ---------------- VGA state ---------------- */
int video_mode = 3;
u8 vga_dac[256][3];
u8 crtc[64];                                        /* 30h-3Fh: the ET4000's extended registers */
int palette_dirty = 1;
static u8 crtc_idx, seq_idx, gc_idx, atc_idx, atc_flip, misc_out = 0x67;
static u8 seq[8], gc[16], atc[32];                  /* ATC 16h-1Fh: ET4000 extensions (not drawn) */
static u8 atc_pas = 0x20;            /* attribute index bit 5: palette address source (screen on) */
static u8 dac_widx, dac_wc, dac_ridx, dac_rc, dac_mask = 0xFF;
/* DAC width: 6 bits a primary as on a VGA, or 8 once a program asks for it
   through VESA (4F08h), as most SVGA cards allow; a mode set goes back to
   6. vga_dac holds the values at the width they were written in. */
static int dac8;
static u8 dac_in(u8 v) { return dac8 ? v : v & 63; }
static u32 pal[256];
/* A DAC change redraws only the rows that show a changed color: pal_hit
   marks the indices whose color moved since the last refresh. */
static u8 pal_hit[256];
static int pal_changed;

/* The columns [*x0, *x1) of a source row that differ from its shadow, or
   (pal8: a row of DAC indices) show a changed palette entry. None: x0 == x1. */
static void row_span(const u8 *a, const u8 *sh, u32 n, int pal8, u32 *x0, u32 *x1)
{
    u32 i = 0, j = n;
    while (i < n && a[i] == sh[i] && !(pal8 && pal_hit[sh[i]])) i++;
    if (i == n) { *x0 = *x1 = 0; return; }
    while (j > i + 1 && a[j - 1] == sh[j - 1] && !(pal8 && pal_hit[sh[j - 1]])) j--;
    *x0 = i; *x1 = j;
}

/* Widen [*x0, *x1) to take in the mouse pointer drawn at column p. */
static void span_ptr(u32 *x0, u32 *x1, int p, u32 n)
{
    u32 a = p < 0 ? 0 : (u32)p, b = p + 10 < 0 ? 0 : (u32)(p + 10);   /* the arrow: 10 wide */
    if (b > n) b = n;
    if (a >= b) return;
    if (*x0 >= *x1) { *x0 = a; *x1 = b; return; }
    if (a < *x0) *x0 = a;
    if (b > *x1) *x1 = b;
}

/* VESA BIOS state (see the VBE section) */
static int vbe_on;
static u8 et_seg;                                    /* ET4000 segment select (3CDh) */
static int et_split;                                 /* its read segment != its write segment: writes trapped */
static int et_on;                                    /* an ET4000 mode: line pitch and display start come from the CRTC */
static u32 vbe_w, vbe_h, vbe_bpp, vbe_pitch, vbe_start;
static int vbe_crtc_follow;                 /* a VBE 3 mode set with CRTC timings: the display-end registers set the size */
static u32 vbe_mode_w, vbe_mode_h;          /* the VESA mode's own size */
#define GFX_MAXW 800                        /* the largest VGA-drawn graphics screen: VESA 102h */
#define GFX_MAXH 600
static u16 planar_vbe;                      /* VESA 102h (800x600x16, planar) while set: its number, else 0 */
static int vbe_interlace;                   /* VBE 3 timings interlaced: the CRTC counts one field, half the lines */
static int vbe_flip_pending;                /* 4F07h BL=02h: a display start scheduled, its retrace not yet past */
static u32 vbe_flip_at;                     /* when (PIT ticks) */
/* Vertical retrace as 3DAh shows it, in PIT ticks: period and how long the
   retrace bit stays on. 70 Hz for the BIOS modes; after a VBE 3 mode set
   with a CRTCInfoBlock, the refresh its pixel clock and totals give. */
#define VR_PERIOD_VGA 17045u
#define VR_LEN_VGA    1100u
static u32 vr_period = VR_PERIOD_VGA, vr_len = VR_LEN_VGA;
static int full_redraw = 1, last_layout_mode = -1;

static const u8 ega_atc[16] = { 0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F };
static const u8 cga16[16][3] = {
    { 0, 0, 0 }, { 0, 0, 42 }, { 0, 42, 0 }, { 0, 42, 42 }, { 42, 0, 0 }, { 42, 0, 42 }, { 42, 21, 0 }, { 42, 42, 42 },
    { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 }, { 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 } };

/* kind 0: the EGA 64-color palette (400-line text, modes 10h, 12h);
   1: the 256-color default (13h); 2: the CGA-compatible palette of the
   200-line modes (4, 5, 6, 0Dh, 0Eh), where attribute bit 4 is intensity. */
static void default_palette(int kind)
{
    int graphics256 = kind == 1;
    memset(vga_dac, 0, sizeof vga_dac);
    if (kind == 2) {
        for (int i = 0; i < 64; i++) {
            int in = (i & 0x10) ? 21 : 0;
            vga_dac[i][0] = ((i >> 2) & 1) * 42 + in;
            vga_dac[i][1] = (i & 0x17) == 6 ? 21 : ((i >> 1) & 1) * 42 + in;
            vga_dac[i][2] = (i & 1) * 42 + in;
        }
        for (int i = 0; i < 16; i++) atc[i] = (u8)((i & 7) | (i & 8 ? 0x10 : 0));
    } else if (!graphics256) {
        for (int i = 0; i < 64; i++) {
            vga_dac[i][0] = ((i >> 2) & 1) * 42 + ((i >> 5) & 1) * 21;
            vga_dac[i][1] = ((i >> 1) & 1) * 42 + ((i >> 4) & 1) * 21;
            vga_dac[i][2] = (i & 1) * 42 + ((i >> 3) & 1) * 21;
        }
        memcpy(atc, ega_atc, 16);
    } else {
        static const u8 grey[16] = { 0, 5, 8, 11, 14, 17, 20, 24, 28, 32, 36, 40, 45, 50, 56, 63 };
        memcpy(vga_dac, cga16, sizeof cga16);
        for (int i = 0; i < 16; i++) vga_dac[16 + i][0] = vga_dac[16 + i][1] = vga_dac[16 + i][2] = grey[i];
        static const u8 hi[3] = { 63, 28, 16 };
        int n = 32;
        for (int v = 0; v < 3; v++)
            for (int s = 0; s < 3; s++) {
                int mx = hi[v], mn = s == 0 ? 0 : s == 1 ? mx / 2 : mx * 45 / 63;
                for (int h = 0; h < 24; h++, n++) {
                    int seg = h / 4, t = h % 4, up = mn + (mx - mn) * t / 4, dn = mx - (mx - mn) * t / 4;
                    int r, g, b;
                    switch (seg) {
                    case 0: r = up; g = mn; b = mx; break;
                    case 1: r = mx; g = mn; b = dn; break;
                    case 2: r = mx; g = up; b = mn; break;
                    case 3: r = dn; g = mx; b = mn; break;
                    case 4: r = mn; g = mx; b = up; break;
                    default: r = mn; g = dn; b = mx; break;
                    }
                    vga_dac[n][0] = r; vga_dac[n][1] = g; vga_dac[n][2] = b;
                }
            }
        for (int i = 0; i < 16; i++) atc[i] = i;
    }
    atc[0x10] = graphics256 ? 0x41 : 0x0C;   /* mode control: blink on in text */
    atc[0x11] = 0; atc[0x12] = 0x0F; atc[0x13] = 0; atc[0x14] = 0;
    palette_dirty = 1;
}

/* ---------------- modes ---------------- */
static int text_cols = 80, text_rows = 25;
static u32 text_base = 0xB8000;

static u8 *planes;                 /* 4 x 64 KiB: unchained 256-color and 16-color modes */
static int is_text(int m) { return m <= 3 || m == 7; }
static int is_cga(int m) { return m >= 4 && m <= 6; }
static int is_ega(int m) { return m == 0x0D || m == 0x0E || m == 0x10 || m == 0x12; }   /* 16-color planar */
static void planar_check(void);
static void ega_check(void);
static void modex_check(void);
int video_et_split(u32 lin);
static void et_crtc(void);
static void vbe_crtc_size(void);
static void et_remap(void);
u8 video_et_read(u32 lin);
void video_et_write(u32 lin, u8 v);
static int vga_logging(u16 *cs, u32 *eip);
static int vgalog = -1;
static u32 vdd_code_at;                   /* vgalog: where the VGA VxD's check ran, dumped when Windows gives up */

/* Graphics mode size in pixels. */
/* 256-colour (mode 13h based) screens as the CRTC describes them, so
   tweaked modes come out right: Nesticle's 256x256, Mode X's 320x240,
   360x240 ... Width from the horizontal display end (4 pixels a character
   clock), height from the vertical display end over the scan lines per
   row (max scan line, double scan), bytes a line from the offset register.
   A half-programmed CRTC (mid-tweak) reads as plain 320x200. */
static void m13_geom(int *w, int *h)
{
    int ww = (crtc[0x01] + 1) * 4;
    int lines = (crtc[0x12] | (crtc[0x07] & 0x02) << 7 | (crtc[0x07] & 0x40) << 3) + 1;
    int per = ((crtc[0x09] & 0x1F) + 1) * ((crtc[0x09] & 0x80) ? 2 : 1);
    int hh = lines / per;
    if (ww < 128 || ww > 640 || hh < 100 || hh > 480 || !crtc[0x13]) { ww = 320; hh = 200; }
    *w = ww; *h = hh;
}
/* 16-colour planar screens (0Dh, 0Eh, 10h, 12h) as the CRTC describes them
   too: games that program the registers themselves (640x480 from mode 0Dh
   or from text mode, 640x400, 320x240 ...) come out at the size they set.
   Width: 8 pixels a character clock (the 320-wide modes halve the dot
   clock, so each is twice as wide on the monitor, but 40 clocks are still
   320 of them); height as for mode 13h. Unprogrammed or mid-tweak: the
   BIOS mode's size. */
static int scan_per_row(void) { return ((crtc[0x09] & 0x1F) + 1) * ((crtc[0x09] & 0x80) ? 2 : 1); }
static void ega_geom(int *w, int *h)
{
    int ww = (crtc[0x01] + 1) * 8;
    int lines = (crtc[0x12] | (crtc[0x07] & 0x02) << 7 | (crtc[0x07] & 0x40) << 3) + 1;
    int hh = lines / scan_per_row();
    if (!crtc[0x01] || ww < 160 || ww > GFX_MAXW || hh < 100 || hh > GFX_MAXH || !crtc[0x13]) {
        ww = video_mode == 0x0D ? 320 : 640;
        hh = video_mode == 0x10 ? 350 : video_mode == 0x12 ? 480 : 200;
    }
    *w = ww; *h = hh;
}
static int gfx_w(void)
{
    int w, h;
    if (video_mode == 0x13) { m13_geom(&w, &h); return w; }
    if (is_ega(video_mode)) { ega_geom(&w, &h); return w; }
    return video_mode == 6 ? 640 : 320;
}
static int gfx_h(void)
{
    int w, h;
    if (video_mode == 0x13) { m13_geom(&w, &h); return h; }
    if (is_ega(video_mode)) { ega_geom(&w, &h); return h; }
    return 200;
}
static int native_height(void);
int video_gfx_height(void)
{
    if (vga_native) return native_height();
    return vbe_on ? (int)vbe_h : is_text(video_mode) ? 200 : gfx_h();
}

/* CGA color select (port 3D9h, INT 10h AH=0Bh): background (or the
   640x200 foreground), palette and intensity, as attribute-palette entries. */
static u8 cga_sel;
static void cga_color_select(u8 v)
{
    cga_sel = v;
    wr8(BDA + 0x66, v);
#define CGA_ATTR(c) ((u8)(((c) & 7) | ((c) & 8 ? 0x10 : 0)))
    if (video_mode == 6) { atc[0] = 0; atc[1] = CGA_ATTR(v & 15); }
    else if (is_cga(video_mode)) {
        static const u8 cols[3][3] = { { 2, 4, 6 }, { 3, 5, 7 }, { 3, 4, 7 } };   /* palette 0, 1, mode 5 */
        int set = video_mode == 5 ? 2 : (v & 0x20) ? 1 : 0, hi = (v & 0x10) ? 8 : 0;
        atc[0] = CGA_ATTR(v & 15);
        for (int i = 0; i < 3; i++) atc[1 + i] = CGA_ATTR(cols[set][i] + hi);
    }
    palette_dirty = 1;
}

static void sync_cursor_crtc(void)
{
    u8 page = rd8(BDA + 0x62);
    u16 pos = rd16(BDA + 0x50 + page * 2);
    u16 off = (u16)(rd16(BDA + 0x4E) / 2 + (pos >> 8) * text_cols + (pos & 0xFF));
    crtc[0x0E] = off >> 8;
    crtc[0x0F] = (u8)off;
}

static void vbe_off(void);
static int vbe_setting;                    /* vbe_set is setting up the VGA state under a VESA mode */
static int reg_mode;                       /* graphics turned on through the registers, not the BIOS */
static void video_set_mode_(int mode, int clear);
void video_set_mode(int mode, int clear) { rlock_take(&video_lock); video_set_mode_(mode, clear); rlock_drop(&video_lock); }
static void video_set_mode_(int mode, int clear)
{
    mode &= 0x7F;
    if (v3_on || vga_native) return;              /* voodoo3, video=vbe: the card's own BIOS sets its modes */
    vbe_off();
    vr_period = VR_PERIOD_VGA; vr_len = VR_LEN_VGA;   /* a BIOS mode's own timing: 70 Hz */
    vbe_interlace = 0;
    planar_vbe = 0;
    video_mode = mode;
    text_cols = (mode == 0 || mode == 1 || mode == 4 || mode == 5 || mode == 0x0D || mode == 0x13) ? 40 : 80;
    text_rows = 25;
    text_base = mode == 7 ? 0xB0000 : 0xB8000;
    wr8(BDA + 0x49, (u8)mode);
    wr16(BDA + 0x4A, (u16)text_cols);
    wr16(BDA + 0x4C, is_text(mode) ? (text_cols == 40 ? 0x800 : 0x1000) : (mode == 0x13 ? 0xFA00 : 0x4000));
    if (mode == 0x12) text_rows = 30;
    else if (mode == 0x10) text_rows = 25;
    wr16(BDA + 0x4E, 0);
    for (int i = 0; i < 8; i++) wr16(BDA + 0x50 + i * 2, 0);
    wr16(BDA + 0x60, 0x0607);
    wr8(BDA + 0x62, 0);
    wr16(BDA + 0x63, mode == 7 ? 0x3B4 : 0x3D4);
    wr8(BDA + 0x65, mode == 0x13 ? 0x0E : 0x29);
    wr8(BDA + 0x66, 0x30);
    wr8(BDA + 0x84, (u8)(text_rows - 1));
    wr16(BDA + 0x85, mode == 0x10 ? 14 : mode == 0x12 || is_text(mode) ? 16 : 8);
    /* INT 43h: the graphics font for this mode's character height, as a VGA
       BIOS sets it (F000:D000 8x14, A000 8x16, B000 8x8). Programs draw text
       themselves from it with the height in 40:85h (SimCity, mode 10h). */
    if (!is_text(mode)) {
        u16 h = rd16(BDA + 0x85);
        wr16(0x43 * 4, h == 14 ? 0xD000 : h == 16 ? 0xA000 : 0xB000);
        wr16(0x43 * 4 + 2, 0xF000);
    }
    wr8(BDA + 0x87, (rd8(BDA + 0x87) & 0x7F) | (clear ? 0 : 0x80) | 0x60);
    wr8(BDA + 0x88, 0x09);
    wr8(BDA + 0x89, 0x11);
    /* The CRTC, the graphics controller's mode register and the misc output
       as IBM's VGA BIOS programs them (its parameter tables): software reads
       them back (Windows 3.1's display VxD takes the text screen's geometry
       from them), and the renderer takes widths, heights and pitches from
       them (ega_geom, m13_geom, ega_row). */
    static const u8 crtc_tab[][25] = {
        /* 0: text, 80 columns */ { 0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,0x00,0x4F,0x0D,0x0E,0,0,0,0,0x9C,0x8E,0x8F,0x28,0x1F,0x96,0xB9,0xA3,0xFF },
        /* 1: text, 40 columns */ { 0x2D,0x27,0x28,0x90,0x2B,0xA0,0xBF,0x1F,0x00,0x4F,0x0D,0x0E,0,0,0,0,0x9C,0x8E,0x8F,0x14,0x1F,0x96,0xB9,0xA3,0xFF },
        /* 2: CGA 4, 5 */          { 0x2D,0x27,0x28,0x90,0x2B,0x80,0xBF,0x1F,0x00,0xC1,0x00,0x00,0,0,0,0,0x9C,0x8E,0x8F,0x14,0x00,0x96,0xB9,0xA2,0xFF },
        /* 3: CGA 6 */             { 0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0xC1,0x00,0x00,0,0,0,0,0x9C,0x8E,0x8F,0x28,0x00,0x96,0xB9,0xC2,0xFF },
        /* 4: 0Dh */               { 0x2D,0x27,0x28,0x90,0x2B,0x80,0xBF,0x1F,0x00,0xC0,0x00,0x00,0,0,0,0,0x9C,0x8E,0x8F,0x14,0x00,0x96,0xB9,0xE3,0xFF },
        /* 5: 0Eh */               { 0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0xC0,0x00,0x00,0,0,0,0,0x9C,0x8E,0x8F,0x28,0x00,0x96,0xB9,0xE3,0xFF },
        /* 6: 10h */               { 0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0x40,0x00,0x00,0,0,0,0,0x83,0x85,0x5D,0x28,0x0F,0x63,0xBA,0xE3,0xFF },
        /* 7: 12h */               { 0x5F,0x4F,0x50,0x82,0x54,0x80,0x0B,0x3E,0x00,0x40,0x00,0x00,0,0,0,0,0xEA,0x8C,0xDF,0x28,0x00,0xE7,0x04,0xE3,0xFF },
        /* 8: 13h */               { 0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0x41,0x00,0x00,0,0,0,0,0x9C,0x8E,0x8F,0x28,0x40,0x96,0xB9,0xA3,0xFF },
    };
    int t = is_text(mode) ? (text_cols == 40 ? 1 : 0) : mode == 4 || mode == 5 ? 2 : mode == 6 ? 3 :
            mode == 0x0D ? 4 : mode == 0x0E ? 5 : mode == 0x10 ? 6 : mode == 0x12 ? 7 : mode == 0x13 ? 8 : 0;
    memset(crtc, 0, sizeof crtc);
    memcpy(crtc, crtc_tab[t], 25);
    if (mode == 7) crtc[0x14] = 0x0F;              /* mono: underline on scan line 15 */
    crtc[0x37] = 0x0F;                             /* ET4000 video system configuration 2: 1 MB, 32-bit DRAM */
    misc_out = mode == 7 ? 0x66 : is_text(mode) ? 0x67 : mode == 0x10 ? 0xA3 : mode == 0x12 ? 0xE3 : 0x63;
    memset(gc, 0, sizeof gc);
    gc[5] = is_text(mode) ? 0x10 : mode == 4 || mode == 5 ? 0x30 : mode == 0x13 ? 0x40 : 0x00;   /* odd/even, CGA shift, 256-colour */
    gc[6] = mode == 7 ? 0x0A : is_text(mode) || is_cga(mode) ? 0x0E : 0x05;   /* memory map: B000h / B800h / A000h 64K */
    gc[7] = 0x0F; gc[8] = 0xFF;                    /* color don't care, bit mask */
    seq[0] = 0x03;                                 /* not in reset */
    seq[2] = is_text(mode) || mode == 4 || mode == 5 ? 0x03 : mode == 6 ? 0x01 : 0x0F;   /* planes written */
    seq[3] = 0x00;                                 /* character map select */
    seq[4] = mode == 0x13 ? 0x0E : is_ega(mode) || mode == 6 ? 0x06 : 0x02;   /* chain-4 in mode 13h */
    seq[1] = mode == 0x0D || mode == 4 || mode == 5 || (is_text(mode) && text_cols == 40) ? 0x09 : 0x01;   /* dot clock halved: 320 wide */
    reg_mode = 0;
    planar_check();
    ega_check();
    if (hv_active) video_b000_map();                         /* mode 7 has its text there */
    crtc[0x0A] = 0x0D; crtc[0x0B] = 0x0E;
    if (clear) {
        if (is_text(mode)) for (u32 i = 0; i < 0x4000; i++) wr16(text_base + i * 2, 0x0720);
        else if (is_cga(mode)) memset(gptr(0xB8000), 0, 0x8000);
        else if (is_ega(mode)) memset(planes, 0, 4 * 65536);
        else {
            /* Through the window's own memory, not the guest's A0000h
               mapping: under VMHV that can be unmapped here (the window
               trapped for an unchained screen), and the kernel faulted on
               its own memset (Dark Forces' second mode 13h set). */
            if (page_phys(0xA0000) != guest_phys(0xA0000))
                dbg(1, "video: mode %02x set with the A000h window %s\n", mode, page_phys(0xA0000) ? "on a plane" : "unmapped");
            if (planes) memset(planes, 0, 4 * 65536);
            memset((void *)(uintptr_t)guest_phys(0xA0000), 0, 0x10000);
        }
    }
    if (dac8 && !vbe_setting) kprintf("video: mode %02x set: DAC back to 6 bits\n", mode);
    dac8 = 0;                                      /* a VGA mode set: 6-bit DAC, all pixel bits (vbe_set keeps 8) */
    dac_mask = 0xFF;
    default_palette(mode == 0x13 ? 1 : is_cga(mode) || mode == 0x0D || mode == 0x0E ? 2 : 0);
    if (!is_text(mode) && mode != 0x13) atc[0x10] = 0x01;   /* graphics, as the BIOS sets it */
    if (is_cga(mode)) cga_color_select(mode == 6 ? 0x3F : 0x30);
    full_redraw = 1;
    last_layout_mode = -1;
    if (!is_text(mode) && !is_cga(mode) && !is_ega(mode) && mode != 0x13) kprintf("video mode %02x: not drawn yet\n", mode);
    else dbg(1, "video mode %02x\n", mode);
}

/* ---------------- drawing ---------------- */
static u16 shadow_text[132 * 60];
static u8 shadow_gfx[GFX_MAXW * GFX_MAXH];
static int sh_cursor = -1, blink_phase;
static u32 frame_no;
static int vd_on;                              /* the Voodoo 3's video processor owns the screen (voodoo3.h) */
static u32 vd_w, vd_h;
static u32 g_x, g_y, g_w, g_h;                 /* the picture's box on the screen: 4:3, all of it (aspect=fill), or whole multiples (scale=off) */
static u16 xmap[4096], txmap[4096];

/* The screen columns [*X0, *X1) (from g_x) that show source columns [x0, x1),
   out to 16-pixel boundaries on the framebuffer: whole write-combining lines. */
static void dest_span(u32 x0, u32 x1, u32 *X0, u32 *X1)
{
    u32 lo = 0, hi = g_w;
    while (lo < hi) { u32 m = (lo + hi) / 2; if (xmap[m] < x0) lo = m + 1; else hi = m; }
    u32 a = lo;
    hi = g_w;
    while (lo < hi) { u32 m = (lo + hi) / 2; if (xmap[m] < x1) lo = m + 1; else hi = m; }
    u32 b = lo;
    a = (g_x + a) & ~15u; a = a < g_x ? 0 : a - g_x;
    b = ((g_x + b + 15) & ~15u) - g_x; if (b > g_w) b = g_w;
    *X0 = a; *X1 = b;
}
static int aspect_fill = -1, no_scale = -1;
static int unscaled = -1;                       /* scale=none / Ctrl+Shift+F8: the VGA's own size, black around it */
static u16 console_cells[80 * 25];
static int console_on;

/* Smooth scaling (scale=smooth, or Ctrl+Shift+F8; off by default): the
   picture enlarged with bilinear blending, as a QEMU window does, instead
   of by whole pixels, which at sizes like 1.6x makes some pixels one wide
   and some two (dithering breaks up, letters come out uneven). Each source row that changed is scaled
   across into hs (g_w wide, kept for the next frame), then every screen
   row is blended from the two source rows it falls between. Needs 8 bits
   a primary in the framebuffer's low three bytes (24 and 32 bpp).

   scale=xbrz (or Ctrl+Shift+F8, which steps sharp, smooth, xBRZ; off by
   default): graphics modes up to 640x480 go through xBRZ (xbrz.c) first,
   enlarged k times (2-6, the least that reaches the
   picture's size), and that is then scaled to fit as above. The source is
   kept in xs; the rows near a changed one are scaled again. Text modes
   are scaled smoothly. Costly (several ms a full frame): on the helper CPU
   (smp.c) it runs after video_lock is let go (video_refresh_late), so DOS
   isn't held up at the VGA registers meanwhile; only the redraw touches
   these buffers, and a redraw on the boot CPU waits for it. */
static int smooth_opt = -1, smooth;
volatile int smooth_toggle_req;
static u32 *hs, hs_cap;
static u16 hx0[4096], vy0[4096];
static u8 hxf[4096], vyf[4096], sdirty[4096];
static u32 s_w, s_h;                           /* the source size the tables are for (xBRZ's output with it on) */
static int xbrz_opt = -1;
static u32 xbrz_k, x_w, x_h;                   /* xBRZ on: its factor and the source size */
static u32 *xs, xs_cap;                         /* the source (x_w * x_h), kept for the next frame */
static u8 *xcor, xdirty[4096];                  /* its corner flags, x_w * x_h */
void xbrz_format(u8 rpos, u8 gpos, u8 bpos);
void xbrz_corners(const u32 *src, u32 w, u32 h, u32 y, u8 *out);
void xbrz_row(const u32 *src, const u8 *cor, u32 w, u32 h, u32 y, u32 k, u32 *dst);

static u32 lerp(u32 a, u32 b, u32 f)            /* f/256 of the way from a to b */
{
    return (((a & 0xFF00FF) * (256 - f) + (b & 0xFF00FF) * f) >> 8 & 0xFF00FF) |
           (((a & 0xFF00) * (256 - f) + (b & 0xFF00) * f) >> 8 & 0xFF00);
}

/* Where screen pixel i of n falls between source pixels (of w): the lower one and the weight of the next. */
static void smooth_map(u32 n, u32 w, u16 *i0, u8 *fr)
{
    for (u32 i = 0; i < n; i++) {
        int32_t v = (int32_t)(((2 * i + 1) * w * 256) / (2 * n)) - 128;
        if (v < 0) v = 0;
        u32 a = (u32)v >> 8, f = (u32)v & 255;
        if (a >= w - 1) { a = w - 1; f = 0; }
        i0[i] = (u16)a; fr[i] = (u8)f;
    }
}

static void smooth_setup(u32 w, u32 h)
{
    smooth = 0;
    xbrz_k = 0;
    if (!smooth_opt || fb_bpp < 24 || rsz != 8 || gsz != 8 || bsz != 8 || rpos > 16 || gpos > 16 || bpos > 16) return;
    if (xbrz_opt && !is_text(video_mode) && w >= 2 && h >= 2 && w * 2 <= 4096 && h * 2 <= 4096) {
        if (w * h > xs_cap) {                   /* any size: the buffers follow the mode */
            if (xs) { phys_release(xs, xs_cap * 4); phys_release(xcor, xs_cap); xs = 0; xcor = 0; xs_cap = 0; }
            xs = phys_try_alloc(w * h * 4);
            xcor = xs ? phys_try_alloc(w * h) : 0;
            if (xs && !xcor) { phys_release(xs, w * h * 4); xs = 0; }
            if (xs) xs_cap = w * h;
            else kprintf("video: no memory for scale=xbrz at %ux%u: smooth\n", w, h);
        }
    }
    if (xbrz_opt && xs && !is_text(video_mode) && w >= 2 && h >= 2 && w * h <= xs_cap) {
        u32 k = (g_w + w - 1) / w, kh = (g_h + h - 1) / h;
        if (kh > k) k = kh;
        if (k < 2) k = 2;                       /* at the screen's size or bigger: 2x, then down */
        if (k > 6) k = 6;
        while (k > 2 && (w * k > 4096 || h * k > 4096)) k--;
        static int formatted;
        if (!formatted) { xbrz_format(rpos, gpos, bpos); formatted = 1; }
        xbrz_k = k; x_w = w; x_h = h;
        w *= k; h *= k;
    }
    if (!xbrz_k && g_w == w && g_h == h) return;   /* pixel for pixel: nothing to smooth (xBRZ, asked for, still runs) */
    if (h > sizeof sdirty || g_h > 4096 || w > 4096 || w < 2 || h < 2) { xbrz_k = 0; return; }
    u32 need = g_w * h * 4;
    if (need > hs_cap) {
        if (hs) phys_release(hs, hs_cap);
        hs = phys_try_alloc(need);
        hs_cap = hs ? need : 0;
        if (!hs) { kprintf("video: no memory for scale=smooth\n"); xbrz_k = 0; return; }
    }
    smooth_map(g_w, w, hx0, hxf);
    smooth_map(g_h, h, vy0, vyf);
    s_w = w; s_h = h;
    smooth = 1;
}

/* Source row y (s_w pixels, framebuffer format) has changed. */
static void bl_row(u32 y, const u32 *src)
{
    u32 *d = hs + y * g_w;
    row_poll();
    for (u32 x = 0; x < g_w; x++) {
        u32 a = hx0[x];
        d[x] = hxf[x] ? lerp(src[a], src[a + 1], hxf[x]) : src[a];
    }
    sdirty[y] = 1;
}

/* Per-CPU scratch for the rows par_for shares out (smp.c), and the
   lists of rows it works through. */
static u32 sc_line[SMP_SLOTS][4096], sc_cvt[SMP_SLOTS][4096], sc_big[SMP_SLOTS][6 * 4096];
static u8 sc_ovr[SMP_SLOTS][GFX_MAXW];
static u16 sm_list[4096];
static u32 sm_bw;
static int par_lost;                           /* a render helper died mid-frame: all of the next one */
static void par(u32 n, u32 chunk, void (*fn)(u32, void *)) { if (!par_for(n, chunk, fn, 0)) par_lost = 1; }

static void bl_draw(u32 i, void *unused)
{
    u32 Y = sm_list[i], y0 = vy0[Y], y1 = y0 + 1 < s_h ? y0 + 1 : y0, f = vyf[Y];
    const u32 *a = hs + y0 * g_w, *b = hs + y1 * g_w;
    if (!f) { put_row(a, g_w, g_x, g_y + Y); return; }
    u32 *line = sc_line[smp_slot()];
    for (u32 x = 0; x < g_w; x++) line[x] = lerp(a[x], b[x], f);
    put_row(line, g_w, g_x, g_y + Y);
}

/* The screen rows that depend on a changed source row. */
static void bl_out(void)
{
    u32 n = 0;
    for (u32 Y = 0; Y < g_h; Y++) {
        u32 y0 = vy0[Y], y1 = y0 + 1 < s_h ? y0 + 1 : y0;
        if (sdirty[y0] || (vyf[Y] && sdirty[y1])) sm_list[n++] = (u16)Y;
    }
    par(n, 16, bl_draw);
    memset(sdirty, 0, s_h);
}
/* The redraw's source row y (framebuffer format) has changed. */
static void smooth_row(u32 y, const u32 *src)
{
    if (!xbrz_k) { bl_row(y, src); return; }
    row_poll();
    memcpy(xs + y * x_w, src, x_w * 4);
    xdirty[y] = 1;
}

/* The helper draws a frame's changed rows after video_lock is let go
   (video_refresh_late): the scan copied them into the shadows, and the
   drawing reads only those, the palette and the layout, which change only
   in a redraw. So DOS's VGA register writes (a page flip every frame in
   DOOM), mode sets and INT 10h wait for the scan alone, not for the
   scaling, xBRZ and framebuffer writes. A redraw on the boot CPU waits for
   the helper's drawing first. */
static volatile int draw_late;                 /* the helper's drawing, still to do */
static u32 late_n;
static void (*late_fn)(u32, void *);
static void smooth_finish(void);

static int xdirty_near(u32 y, int above, int below)
{
    for (int d = -above; d <= below; d++)
        if ((int)y + d >= 0 && (int)y + d < (int)x_h && xdirty[y + d]) return 1;
    return 0;
}

/* After a scan: draw its n rows (fn), and the smooth scaler's output. */
static void draw_rows(void)
{
    par(late_n, 8, late_fn);
    if (smooth) smooth_finish();
}

static void draw_out(u32 n, void (*fn)(u32, void *))
{
    if (!n) return;                                /* nothing changed: nothing to draw */
    late_n = n; late_fn = fn;
    if (smp_on_ap()) { draw_late = 1; return; }    /* the helper: once video_lock is let go */
    draw_rows();
}

static void xb_corner(u32 i, void *unused)
{
    u32 y = sm_list[i];
    row_poll();
    xbrz_corners(xs, x_w, x_h, y, xcor + y * x_w);
}

static void xb_row(u32 i, void *unused)
{
    u32 y = sm_list[i], *big = sc_big[smp_slot()];
    row_poll();
    xbrz_row(xs, xcor, x_w, x_h, y, xbrz_k, big);
    for (u32 r = 0; r < xbrz_k; r++) bl_row(y * xbrz_k + r, big + r * sm_bw);
}

static void smooth_finish(void)
{
    if (xbrz_k) {
        u32 n = 0;
        sm_bw = x_w * xbrz_k;
        for (u32 y = 0; y < x_h; y++) if (xdirty_near(y, 1, 2)) sm_list[n++] = (u16)y;   /* corners: from rows y-1..y+2 */
        par(n, 4, xb_corner);
        n = 0;
        for (u32 y = 0; y < x_h; y++) if (xdirty_near(y, 2, 2)) sm_list[n++] = (u16)y;   /* rows: corners of y-1 and y, pixels of y-1..y+1 */
        par(n, 4, xb_row);
        memset(xdirty, 0, x_h);
    }
    bl_out();
}
static u32 text_pixels_w(void) { return text_cols == 40 ? 640 : (u32)text_cols * 8; }

static void layout(void)
{
    static int last_w, last_h;
    if (aspect_fill < 0) aspect_fill = strstr(cmdline, "aspect=fill") != 0;
    if (no_scale < 0) no_scale = strstr(cmdline, "scale=off") != 0;
    if (xbrz_opt < 0) xbrz_opt = strstr(cmdline, "scale=xbrz") != 0;
    if (unscaled < 0) unscaled = strstr(cmdline, "scale=none") != 0;
    if (smooth_opt < 0) smooth_opt = (strstr(cmdline, "scale=smooth") || xbrz_opt) && !no_scale && !unscaled;
    int text = is_text(video_mode) && !vd_on;
    u32 w = vd_on ? vd_w : vbe_on ? vbe_w : text ? text_pixels_w() : (u32)gfx_w();
    u32 h = vd_on ? vd_h : vbe_on ? vbe_h : text ? (u32)text_rows * 16 : (u32)gfx_h();
    int lmode = vd_on ? 0x1000 : video_mode;      /* the Voodoo 3's picture: a mode of its own */
    if (last_layout_mode == lmode && (int)w == last_w && (int)h == last_h) return;
    last_layout_mode = lmode;
    last_w = (int)w; last_h = (int)h;
    full_redraw = 1;
    clear_fb();
    g_h = fb_h; g_w = fb_h * 4 / 3;
    if (g_w > fb_w || aspect_fill) { g_w = fb_w; g_h = aspect_fill ? fb_h : fb_w * 3 / 4; }
    if (no_scale) {
        /* scale=off: the size a VGA card puts out (low resolutions doubled,
           as it does), times the largest whole number that fits. Too big
           for the screen even at 1: scaled down as without the option. */
        u32 ow = w < 400 ? w * 2 : w, oh = h < 300 ? h * 2 : h;
        u32 s = fb_w / ow < fb_h / oh ? fb_w / ow : fb_h / oh;
        if (s && ow * s <= 4096) { g_w = ow * s; g_h = oh * s; }
    }
    if (unscaled) {
        /* scale=none: exactly what a VGA card puts out (320x200 doubled to
           640x400, 640x480 as is), in the middle of the screen with black
           around it. Far fewer pixels to write on a big screen. Larger than
           the screen: scaled down as without the option. */
        u32 ow = w < 400 ? w * 2 : w, oh = h < 300 ? h * 2 : h;
        if (ow <= fb_w && oh <= fb_h) { g_w = ow; g_h = oh; }
    }
    if (w == fb_w && h == fb_h) { g_w = w; g_h = h; }   /* the screen's own size: as is, whatever its shape */
    if (g_w > 4096) g_w = 4096;
    g_x = (fb_w - g_w) / 2; g_y = (fb_h - g_h) / 2;
    u32 xw = vd_on ? vd_w : vbe_on ? vbe_w : text ? 320 : (u32)gfx_w();
    for (u32 x = 0; x < g_w; x++) xmap[x] = (u16)(x * xw / g_w);
    u32 tw = text_pixels_w();
    for (u32 x = 0; x < g_w; x++) txmap[x] = (u16)(x * tw / g_w);
    smooth_setup(w, h);
}

static int sh_mouse = -1, sh_blink = -1;

/* Text modes are drawn like the graphics ones: each text row (16 scanlines)
   that changed is rendered into a buffer in RAM at its native width (640
   pixels for 80 or 40 columns) and scaled into the picture's box, so it
   fills the screen as games do; nothing is read back from the framebuffer. */
static void text_row(const u16 *cells, int r, int cursor_off, int mcell, u16 m_and, u16 m_xor)
{
    static u32 band[16][132 * 8];
    static u32 line[4096];
    int cw = text_cols == 40 ? 16 : 8;
    int cs = crtc[0x0A] & 0x1F, ce = crtc[0x0B] & 0x1F;
    for (int c = 0; c < text_cols; c++) {
        int i = r * text_cols + c;
        u16 cell = cells[i];
        if (i == mcell) cell = (u16)((cell & m_and) ^ m_xor);
        u8 ch = cell & 0xFF, a = cell >> 8;
        int fg = a & 15, bg = a >> 4;
        if (atc[0x10] & 8) { bg &= 7; if ((a & 0x80) && !blink_phase) fg = bg; }
        u32 pf = pal[atc[fg] & 0x3F], pb = pal[atc[bg] & 0x3F];
        int cur_on = i == cursor_off && blink_phase && !(crtc[0x0A] & 0x20);
        const u8 *g = font8x16 + ch * 16;
        for (int gy = 0; gy < 16; gy++) {
            u8 bits = g[gy];
            if (cur_on && gy >= cs && gy <= ce) bits = 0xFF;
            u32 *p = band[gy] + c * cw;
            for (int bx = 0; bx < 8; bx++) {
                u32 col = (bits & (0x80 >> bx)) ? pf : pb;
                *p++ = col;
                if (cw == 16) *p++ = col;
            }
        }
    }
    if (smooth) {
        for (int gy = 0; gy < 16; gy++) smooth_row((u32)(r * 16 + gy), band[gy]);
        return;
    }
    u32 th = (u32)text_rows * 16;
    u32 y0 = ((u32)r * 16 * g_h + th - 1) / th, y1 = ((u32)(r + 1) * 16 * g_h + th - 1) / th;
    int last = -1;
    for (u32 y = y0; y < y1 && y < g_h; y++) {
        int gy = (int)(y * th / g_h) - r * 16;
        if (gy < 0) gy = 0;
        if (gy > 15) gy = 15;
        if (gy != last) { for (u32 x = 0; x < g_w; x++) line[x] = band[gy][txmap[x]]; last = gy; }
        put_row(line, g_w, g_x, g_y + y);
    }
}

static void refresh_text(const u16 *cells, int cursor_off)
{
    static u8 dirty[60];
    int n = text_cols * text_rows;
    int mcell = -1, px, py;
    u16 m_and = 0xFFFF, m_xor = 0;
    if (!console_on && mouse_pointer(&px, &py, &m_and, &m_xor))
        mcell = (py / 8) * text_cols + px / (text_cols == 40 ? 16 : 8);
    if (mcell >= n) mcell = -1;
    int blink_changed = blink_phase != sh_blink;
    for (int r = 0; r < text_rows; r++) dirty[r] = (u8)full_redraw;
    for (int i = 0; i < n; i++) {
        u16 c = cells[i];
        int blinky = (atc[0x10] & 8) && (c & 0x8000);
        if (c != shadow_text[i] || (blinky && blink_changed)) { dirty[i / text_cols] = 1; shadow_text[i] = c; }
    }
    if (cursor_off != sh_cursor || blink_changed) {
        if (cursor_off >= 0 && cursor_off < n) dirty[cursor_off / text_cols] = 1;
        if (sh_cursor >= 0 && sh_cursor < n) dirty[sh_cursor / text_cols] = 1;
    }
    if (mcell != sh_mouse) {
        if (mcell >= 0) dirty[mcell / text_cols] = 1;
        if (sh_mouse >= 0 && sh_mouse < n) dirty[sh_mouse / text_cols] = 1;
    }
    for (int r = 0; r < text_rows; r++)
        if (dirty[r]) text_row(cells, r, cursor_off, mcell, m_and, m_xor);
    if (smooth) smooth_finish();
    sh_cursor = cursor_off;
    sh_mouse = mcell;
    sh_blink = blink_phase;
}

/* The mouse pointer in graphics modes: an arrow, 1 = outline, 2 = fill. */
static const char *const arrow[16] = {
    "1", "11", "121", "1221", "12221", "122221", "1222221", "12222221",
    "122222221", "1222211111", "1221221", "121 1221", "11  1221", "1    1221", "     1221", "      11" };
static int sh_ptr_y = -100, sh_ptr_x;

/* ---------------- unchained 256-color (planar) VGA ----------------
   With chain-4 off (Mode X/Y, DOOM), video memory is four 64 KB planes: the
   map mask (sequencer 2) picks the planes a write goes to, the read map
   (graphics controller 4) the one a read comes from. With one plane in the
   map mask and plain writes, the guest's A0000h window is mapped straight
   onto a plane: the written one, read-write, or, when the program last
   changed the read map to another plane (DOOM's I_ReadScreen reads all
   four that way), the read one, read-only, so writes fault and are
   emulated. Anything else (several planes, write modes 1-3 such as DOOM's
   latch copies of its status bar, set/reset, a bit mask, a logical
   function) leaves the window unmapped and every access is emulated as in
   the 16-color modes. */
static int planar_on, mapped_plane = -1, mapped_ro, modex_trap, readmap_last;

static void vram_window_dirty(void);
static void map_window_flags(u32 phys, u32 flags)
{
    vram_window_dirty();                           /* the dirty bits of what it shows now, first */
    for (u32 i = 0; i < 16; i++) map_page(0xA0000 + i * 4096, phys + i * 4096, flags);
    tlb_flush_range(0xA0000, 0x10000);
}
static void map_window(u32 phys) { map_window_flags(phys, 7); }

static void apply_map_mask(void)
{
    if (!planar_on) return;
    modex_check();
    if (modex_trap) return;
    u8 m = seq[2] & 15;
    int wp = 0, rp = gc[4] & 3;
    while (wp < 3 && !(m & (1 << wp))) wp++;
    int p = wp, ro = 0;
    if (wp != rp && readmap_last) { p = rp; ro = 1; }
    if (p != mapped_plane || ro != mapped_ro) {
        map_window_flags((u32)(uintptr_t)planes + p * 65536, ro ? 5 : 7);
        mapped_plane = p; mapped_ro = ro;
    }
}

static int planes_live;                   /* the planes hold the screen already (a 16-colour mode went 256-colour) */
static void planar_check(void)
{
    int want = video_mode == 0x13 && !(seq[4] & 8);
    if (want && !planar_on) {
        if (!planes) planes = phys_alloc(4 * 65536);
        /* what the chained screen showed: byte a went to plane a & 3, offset a >> 2 */
        if (!planes_live)
            for (u32 a = 0; a < 64000; a++) planes[(a & 3) * 65536 + (a >> 2)] = rd8(0xA0000 + a);
        planar_on = 1;
        mapped_plane = -1;
        apply_map_mask();
        full_redraw = 1;
        dbg(1, "video: unchained 256-colour (planar)\n");
    } else if (!want && planar_on) {
        map_window(guest_phys(0xA0000));
        planar_on = 0;
        modex_trap = 0;
        mapped_ro = 0;
        mapped_plane = -1;
        full_redraw = 1;
    }
}

/* ---------------- 16-colour planar modes (0Dh, 0Eh, 10h, 12h) ----------------
   The A000h window is left unmapped: each access faults and the instruction
   is emulated (mememu.c) against the four planes through the VGA's logic:
   latches, write modes 0-3, set/reset, rotate and function, bit mask, map
   mask; read modes 0 and 1. */
static int ega_on, ega_dirty;
static u8 latch[4];

/* The graphics controller's memory map (GC 6 bits 2-3) says where the
   planes appear: 0 A0000h-BFFFFh, 1 A0000h-AFFFFh (what the BIOS sets),
   2 B0000h-B7FFFh, 3 B8000h-BFFFFh; outside it the VGA doesn't answer
   (reads FFh, writes go nowhere). A0000h-AFFFFh is always trapped in these
   modes; the B000h/B800h halves only while the map puts the planes there
   (Windows 3.1's VGA driver checks this is a VGA by moving them to B800h). */
static int b_trap;                        /* bit 0: B0000h-B7FFFh trapped, bit 1: B8000h-BFFFFh */
static void ega_bmap(void)
{
    int map = (gc[6] >> 2) & 3;
    int want = !ega_on ? 0 : map == 0 ? 3 : map == 2 ? 1 : map == 3 ? 2 : 0;
    if (want == b_trap) return;
    for (int h = 0; h < 2; h++) {
        int on = (want >> h) & 1;
        if (on == ((b_trap >> h) & 1)) continue;
        for (u32 i = 0; i < 8; i++) {
            u32 a = 0xB0000 + h * 0x8000 + i * 4096;
            if (on) map_page(a, 0, 0); else map_page(a, guest_phys(a), 7);
        }
    }
    tlb_flush();
    b_trap = want;
    if (!(want & 1)) video_b000_map();
    if (vgalog > 0) kprintf("vga: memory map %d, B000h/B800h trapped %d\n", map, want);
}

/* B0000h-B7FFFh when the planes aren't there: the monochrome text buffer
   in mode 7, else nothing answers on a colour VGA (reads FFh, writes go
   nowhere) - Windows 3.1's VGA VxD writes and reads back B000h to see
   whether a second, monochrome adapter is there, and gives up if it finds
   memory. In VMHV only (the v86 monitor keeps it as RAM). */
void video_vgalog_giveup(void)
{
    if (!vgalog || !vdd_code_at) return;
    kprintf("VDD init code when Windows gave up (int 20h calls that ran are patched):\n");
    hv_dump_lin((vdd_code_at & ~0xFFFu) - 0x1000, 0x2000);
    kprintf("VDD locked code:\n");
    hv_dump_lin(0x80015000, 0x2000);
    vdd_code_at = 0;
}

static void video_b000_map_(void);
void video_b000_map(void) { rlock_take(&video_lock); video_b000_map_(); rlock_drop(&video_lock); }
static void video_b000_map_(void)
{
    if (b_trap & 1) return;                                   /* the planes are there */
    u8 *hole = hv_hole_page();
    int empty = hole && video_mode != 7;
    for (u32 i = 0; i < 8; i++) {
        u32 a = 0xB0000 + i * 4096;
        map_page(a, empty ? (u32)(uintptr_t)hole : guest_phys(a), empty ? 5 : 7);
    }
    tlb_flush();
}

static void ega_check(void)
{
    int want = is_ega(video_mode);
    if (want && !ega_on) {
        if (!planes) planes = phys_alloc(4 * 65536);
        for (u32 i = 0; i < 16; i++) map_page(0xA0000 + i * 4096, 0, 0);
        tlb_flush();
        ega_on = 1;
    } else if (!want && ega_on) {
        map_window(guest_phys(0xA0000));
        ega_on = 0;
    }
    ega_bmap();
}

int vga16_window(u32 lin)
{
    if (v3_on) return 0;                                       /* voodoo3: the VGA is the card (v3_window) */
    if (video_et_split(lin)) return 1;                         /* ET4000 split segments: writes emulated */
    if (lin >= 0xA0000 && lin < 0xB0000) return ega_on || modex_trap || (planar_on && mapped_ro);
    if (lin >= 0xB0000 && lin < 0xC0000) return ega_on && ((b_trap >> ((lin - 0xB0000) >> 15)) & 1);
    return 0;
}

/* Offset in the planes for lin through the memory map, -1: not decoded. */
static int vga16_off(u32 lin)
{
    if (!ega_on) return (int)((lin - 0xA0000) & 0xFFFF);          /* unchained 256-colour: A000h */
    static const u32 base[4] = { 0xA0000, 0xA0000, 0xB0000, 0xB8000 }, size[4] = { 0x20000, 0x10000, 0x8000, 0x8000 };
    int map = (gc[6] >> 2) & 3;
    if (lin < base[map] || lin - base[map] >= size[map]) return -1;
    return (int)((lin - base[map]) & 0xFFFF);
}

/* Unchained 256-colour: emulate unless writes go plainly to one plane. */
static void modex_check(void)
{
    u8 m = seq[2] & 15;
    int want = planar_on && ((m & (m - 1)) || !m || (gc[5] & 0x0B) || (gc[1] & 15) || (gc[3] & 0x1F) ||
                             gc[8] != 0xFF);
    if (want && !modex_trap) {
        for (u32 i = 0; i < 16; i++) map_page(0xA0000 + i * 4096, 0, 0);
        tlb_flush_range(0xA0000, 0x10000);
        modex_trap = 1;
        mapped_plane = -1;
        mapped_ro = 0;
    } else if (!want && modex_trap) modex_trap = 0;   /* apply_map_mask maps the window */
}

static int vgamem_log(void)
{
    static int n;
    u16 cs; u32 eip;
    return vga_logging(&cs, &eip) && n++ < 400;
}
static u8 vga16_read_(u32 lin);
u8 vga16_read(u32 lin)
{
    u8 v = vga16_read_(lin);
    if (vgamem_log()) kprintf("vga mem rd %05x -> %02x (GC6 %02x GC5 %02x GC4 %02x)\n", lin, v, gc[6], gc[5], gc[4]);
    return v;
}
static u8 vga16_read_(u32 lin)
{
    if (video_et_split(lin)) return video_et_read(lin);
    int oo = vga16_off(lin);
    if (oo < 0) return 0xFF;
    u32 o = (u32)oo;
    for (int p = 0; p < 4; p++) latch[p] = planes[p * 65536 + o];
    if (!(gc[5] & 8)) return latch[gc[4] & 3];
    u8 r = 0;                                      /* read mode 1: colour compare */
    for (int p = 0; p < 4; p++)
        if (gc[7] & (1 << p)) r |= latch[p] ^ ((gc[2] & (1 << p)) ? 0xFF : 0);
    return (u8)~r;
}

static void vga_write_planes(u8 v, u8 *base, u32 stride);
void vga16_write(u32 lin, u8 v)
{
    if (vgamem_log()) kprintf("vga mem wr %05x <- %02x (GC6 %02x GC5 %02x SEQ2 %02x)\n", lin, v, gc[6], gc[5], seq[2]);
    if (video_et_split(lin)) { video_et_write(lin, v); return; }
    int oo = vga16_off(lin);
    if (oo < 0) return;
    u32 o = (u32)oo;
    ega_dirty = 1;
    vga_write_planes(v, planes + o, 65536);
}

/* A write through the graphics controller: plane p's byte at base + p * stride. */
static void vga_write_planes(u8 v, u8 *base, u32 stride)
{
    int mode = gc[5] & 3, rot = gc[3] & 7, fn = (gc[3] >> 3) & 3;
    u8 mask = gc[8], mm = seq[2] & 15;
    if (rot) v = (u8)((v >> rot) | (v << (8 - rot)));
    if (mode == 3) { mask &= v; }
    for (int p = 0; p < 4; p++) {
        if (!(mm & (1 << p))) continue;
        u8 d;
        u8 *q = base + p * stride;
        if (mode == 1) { *q = latch[p]; continue; }
        if (mode == 0) d = (gc[1] & (1 << p)) ? ((gc[0] & (1 << p)) ? 0xFF : 0) : v;
        else if (mode == 2) d = (v & (1 << p)) ? 0xFF : 0;
        else d = (gc[0] & (1 << p)) ? 0xFF : 0;
        switch (fn) {
        case 1: d &= latch[p]; break;
        case 2: d |= latch[p]; break;
        case 3: d ^= latch[p]; break;
        }
        *q = (u8)((d & mask) | (latch[p] & ~mask));
    }
}

/* A fault on the window: emulate the instruction. */
int vga16_fault(struct emu_cpu *e, u32 cr2)
{
    if (!vga16_window(cr2) && !v3_window(cr2)) return 0;   /* the Voodoo 3's BARs (voodoo3.h) the same way */
    if (mem_emulate(e)) { if (hv_active || v3_window(cr2)) mem_emulate_more(e); return 1; }
    u32 a = e->seg_base[1] + *e->eip;
    kprintf("vga16: can't emulate %02x %02x %02x %02x %02x at %x (address %x)\n",
            rd8(a), rd8(a + 1), rd8(a + 2), rd8(a + 3), rd8(a + 4), a, cr2);
    return 0;
}

static int ega_bpr(void) { return video_mode == 0x0D ? 40 : 80; }   /* BIOS drawing: bytes per row */

/* One row of the screen as DAC indices: CRTC start and offset, pel panning,
   line compare, the attribute controller. */
static void ega_row(int y, u8 *out, int w)
{
    u32 start = (u32)(crtc[0x0C] << 8 | crtc[0x0D]), pitch = crtc[0x13] * 2u;
    int pan = atc[0x13] & 7;
    int lc = crtc[0x18] | (crtc[0x07] & 0x10) << 4 | (crtc[0x09] & 0x40) << 3;
    lc /= scan_per_row();                              /* double-scanned 200-line modes */
    u32 o;
    if (y > lc) { o = (y - lc - 1) * pitch; if (atc[0x10] & 0x20) pan = 0; }
    else o = start + y * pitch;
    u8 cs = (u8)((atc[0x14] & 0x0C) << 4);
    for (int x = 0; x < w; x++) {
        int px = x + pan;
        u32 a = (o + (px >> 3)) & 0xFFFF;
        int b = 7 - (px & 7);
        int idx = ((planes[a] >> b) & 1) | ((planes[65536 + a] >> b) & 1) << 1 |
                  ((planes[131072 + a] >> b) & 1) << 2 | ((planes[196608 + a] >> b) & 1) << 3;
        u8 v = atc[idx & atc[0x12]];
        out[x] = (atc[0x10] & 0x80) ? (u8)(cs | (atc[0x14] & 3) << 4 | (v & 15)) : (u8)(cs | (v & 0x3F));
    }
}

/* One row of the graphics screen as DAC indices. */
static void fetch_row(int y, u8 *out)
{
    int w = gfx_w();
    if (video_mode == 0x13) {
        u32 start = (u32)(crtc[0x0C] << 8 | crtc[0x0D]);
        if (planar_on) {                          /* per plane: offset * 2 addresses a line */
            u32 pitch = crtc[0x13] ? crtc[0x13] * 2u : 80, o = start + y * pitch;
            for (int x = 0; x < w; x++) out[x] = planes[(x & 3) * 65536 + ((o + (x >> 2)) & 0xFFFF)];
        } else {                                  /* chain-4: offset * 8 bytes a line, start in dwords */
            u32 pitch = crtc[0x13] ? crtc[0x13] * 8u : 320, o = start * 4 + y * pitch;
            const u8 *v = gptr(0xA0000);
            if ((o & 0xFFFF) + (u32)w <= 0x10000) memcpy(out, v + (o & 0xFFFF), (u32)w);
            else for (int x = 0; x < w; x++) out[x] = v[(o + x) & 0xFFFF];
        }
        return;
    }
    if (is_cga(video_mode)) {                     /* even rows at B800:0000, odd at B800:2000 */
        const u8 *b = gptr(0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80);
        if (video_mode == 6)
            for (int x = 0; x < w; x++) out[x] = atc[(b[x >> 3] >> (7 - (x & 7))) & 1] & 0x3F;
        else
            for (int x = 0; x < w; x++) out[x] = atc[(b[x >> 2] >> (6 - 2 * (x & 3))) & 3] & 0x3F;
        return;
    }
    if (is_ega(video_mode)) { ega_row(y, out, w); return; }
    memset(out, 0, w);
}

/* The rows refresh_gfx and refresh_vbe found changed, for par_for to draw
   from their shadows: source columns [todo_x0, todo_x1) of row todo_y. */
static u16 todo_y[4096], todo_x0[4096], todo_x1[4096];
static struct { u32 w, h, B, bpp, rowb; int px, py; u32 black, white; } fr;   /* this frame's: a mode set may come meanwhile */

static void gfx_draw(u32 i, void *unused)
{
    u32 y = todo_y[i], w = fr.w, h = fr.h;
    int px = fr.px, py = fr.py, slot = smp_slot();
    const u8 *row = shadow_gfx + y * w;
    int in_new = (int)y >= py && (int)y < py + 16;
    if (smooth) {
        u32 *srow = sc_cvt[slot];
        for (u32 x = 0; x < w; x++) srow[x] = pal[row[x]];
        if (in_new) {
            const char *a = arrow[y - py];
            for (int k = 0; a[k] && px + k < (int)w; k++)
                if (a[k] == '1') srow[px + k] = fr.black; else if (a[k] == '2') srow[px + k] = fr.white;
        }
        smooth_row(y, srow);
        return;
    }
    u32 y0 = g_y + y * g_h / h, y1 = g_y + (y + 1) * g_h / h;
    if (y1 == y0) return;
    u32 X0, X1;                                    /* only the screen columns showing them */
    dest_span(todo_x0[i], todo_x1[i], &X0, &X1);
    if (X0 >= X1) return;
    u32 *line = sc_line[slot];
    for (u32 x = X0; x < X1; x++) line[x] = pal[row[xmap[x]]];
    if (in_new) {
        u8 *ovr = sc_ovr[slot];
        memset(ovr, 0, GFX_MAXW);
        const char *a = arrow[y - py];
        for (int k = 0; a[k] && px + k < (int)w; k++) ovr[px + k] = a[k] == '1' ? 1 : a[k] == '2' ? 2 : 0;
        for (u32 x = X0; x < X1; x++)
            if (ovr[xmap[x]]) line[x] = ovr[xmap[x]] == 1 ? fr.black : fr.white;
    }
    for (u32 yy = y0; yy < y1; yy++) put_row(line + X0, X1 - X0, g_x + X0, yy);   /* from RAM: never read the framebuffer */
}

static void refresh_gfx(void)
{
    static u8 rowbuf[GFX_MAXW];
    int w = gfx_w(), h = gfx_h();
    int px = 0, py = -100;
    u16 ma, mxr;
    if (mouse_pointer(&px, &py, &ma, &mxr)) { if (w < 640) px = px * w / 640; } else py = -100;
    int moved = px != sh_ptr_x || py != sh_ptr_y;
    if (is_ega(video_mode)) {                      /* nothing new in the planes or registers */
        static u8 last_regs[32 + 21];
        int regs_changed = memcmp(last_regs, crtc, 32) || memcmp(last_regs + 32, atc, 21);
        if (!ega_dirty && !full_redraw && !moved && !regs_changed && !pal_changed) return;
        memcpy(last_regs, crtc, 32); memcpy(last_regs + 32, atc, 21);
        ega_dirty = 0;
    }
    u32 n = 0;                                     /* which rows changed, here; drawing them, shared */
    for (int y = 0; y < h; y++) {
        fetch_row(y, rowbuf);
        u8 *sh = shadow_gfx + y * w;
        int in_new = y >= py && y < py + 16, in_old = y >= sh_ptr_y && y < sh_ptr_y + 16;
        u32 x0 = 0, x1 = (u32)w;                   /* the source columns that changed */
        if (!full_redraw) {
            row_span(rowbuf, sh, (u32)w, pal_changed, &x0, &x1);
            if (moved && in_new) span_ptr(&x0, &x1, px, (u32)w);
            if (moved && in_old) span_ptr(&x0, &x1, sh_ptr_x, (u32)w);
            if (x0 >= x1) continue;
        }
        memcpy(sh, rowbuf, w);
        todo_y[n] = (u16)y; todo_x0[n] = (u16)x0; todo_x1[n] = (u16)x1; n++;
    }
    fr.w = (u32)w; fr.h = (u32)h; fr.px = px; fr.py = py;
    fr.black = pack(0, 0, 0); fr.white = pack(63, 63, 63);
    draw_out(n, gfx_draw);
    sh_ptr_x = px; sh_ptr_y = py;
}
static void refresh_fallback(void)
{
    if ((!is_text(video_mode) && !console_on) || (v3_on && !console_on)) return;
    u16 *dst = phys_low(0xB8000);
    const u16 *src = console_on ? console_cells : (const u16 *)gptr(text_base + (crtc[0x0C] << 8 | crtc[0x0D]) * 2);
    memcpy(dst, src, 80 * 25 * 2);
    u16 off = console_on ? 0xFFFF : (u16)((crtc[0x0E] << 8 | crtc[0x0F]) - (crtc[0x0C] << 8 | crtc[0x0D]));
    outb(0x3D4, 0x0E); outb(0x3D5, off >> 8);
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)off);
}


/* ---------------- VESA BIOS (VBE 3.0) ----------------
   Core functions 00h-0Bh: 2.0's set, plus 3.0's CRTC timings on mode set
   (02h, below), scheduled display start (07h BL=02h/04h), pixel clocks
   (0Bh), the linear-framebuffer mode info fields, and state save (04h).
   4 MB of video memory in kernel RAM, identity-mapped and open to the
   guest: protected-mode programs use it as the linear framebuffer at its
   real address (DPMI 0800h hands that back), real-mode ones through the
   A000h window, which function 05h (or the WinFuncPtr far call) maps onto
   a 64 KB bank. 8-bit modes use the VGA DAC; 15/16/32-bit ones are direct
   colour. Tables live in the BIOS segment at F000:C000. */
#define VRAM_SIZE (4u << 20)
#define VBE_DATA 0xFC000u
#define VBE_MIN_CLOCK 1000000u                  /* VBE 3 pixel clocks taken (4F02h, 4F0Bh), Hz */
#define VBE_MAX_CLOCK 400000000u
static u8 *vram, *vshadow;
static u32 vbe_bank;
static u16 vbe_cur;
static const struct { u16 no, w, h; u8 bpp; } vmodes[] = {
    { 0x100, 640, 400, 8 }, { 0x101, 640, 480, 8 }, { 0x103, 800, 600, 8 }, { 0x105, 1024, 768, 8 },
    { 0x110, 640, 480, 15 }, { 0x111, 640, 480, 16 }, { 0x112, 640, 480, 32 },
    { 0x113, 800, 600, 15 }, { 0x114, 800, 600, 16 }, { 0x115, 800, 600, 32 },
    { 0x116, 1024, 768, 15 }, { 0x117, 1024, 768, 16 }, { 0x118, 1024, 768, 32 },
    /* the small ones games and emulators look for (DOS MAME through Allegro:
       320x200, 320x240, 400x300, 512x384), at every depth; numbers past the
       VBE 1.2 ones, as SVGA BIOSes and UniVBE give them */
    { 0x120, 320, 200, 8 }, { 0x10D, 320, 200, 15 }, { 0x10E, 320, 200, 16 }, { 0x121, 320, 200, 32 },
    { 0x122, 320, 240, 8 }, { 0x123, 320, 240, 15 }, { 0x124, 320, 240, 16 }, { 0x125, 320, 240, 32 },
    { 0x126, 400, 300, 8 }, { 0x127, 400, 300, 15 }, { 0x128, 400, 300, 16 }, { 0x129, 400, 300, 32 },
    { 0x12A, 512, 384, 8 }, { 0x12B, 512, 384, 15 }, { 0x12C, 512, 384, 16 }, { 0x12D, 512, 384, 32 },
    { 0x12E, 640, 400, 15 }, { 0x12F, 640, 400, 16 }, { 0x130, 640, 400, 32 },
    /* 24 bits a pixel, packed (3 bytes, blue first), for programs that
       want true colour that way rather than as 32 (GTA): 10Fh is VBE 1.2's
       own 320x200x24, the rest numbered on past the ones above */
    { 0x10F, 320, 200, 24 }, { 0x131, 320, 240, 24 }, { 0x132, 400, 300, 24 }, { 0x133, 512, 384, 24 },
    { 0x134, 640, 400, 24 }, { 0x135, 640, 480, 24 }, { 0x136, 800, 600, 24 }, { 0x137, 1024, 768, 24 },
    /* VBE 1.2's 800x600 in 16 colours: four planes, as mode 12h (see vbe_planar) */
    { 0x102, 800, 600, 4 },
};
#define N_VMODES (int)(sizeof vmodes / sizeof vmodes[0])

static int vbe_bytespp(u32 bpp) { return (bpp + 7) / 8; }

/* A mode's bytes a line as it is set. 24-bit ones get the next power of
   two up (640: 2048, 800 and 1024: 4096), as SVGA BIOSes give them: then
   no line, and so no 3-byte pixel, ever crosses a 64 KB bank, which banked
   24-bit code relies on (it writes a pixel's three bytes with no bank
   check between, and counts window space as if pixels were 2 bytes:
   with 1920 bytes a line the colours shift at every bank). */
static u32 vbe_mode_pitch(int m)
{
    u32 p = vmodes[m].w * vbe_bytespp(vmodes[m].bpp);
    if (vmodes[m].bpp == 24) { u32 q = 1; while (q < p) q <<= 1; p = q; }
    return p;
}

/* Which pages of vram were written since the last redraw, from the page
   tables' dirty bits, so refresh_vbe compares only the rows on those with
   vshadow (a 1024x768x32 screen is 3 MB to compare 60 times a second). The
   guest writes through vram's own pages (the LFB) and the A0000h window.
   A cleared bit counts only once the boot CPU, the guest's, has flushed its
   TLB (video_tlb_tick): before that a write can go through a TLB entry that
   has the bit set already and not set it again. Until it has, everything is
   compared, and the pages cleared before the flush are compared once more
   after it. Off under VMHV: the VM's writes set its own tables' bits. */
#define VPAGES (VRAM_SIZE / 4096)
static u8 vwin[VPAGES];                    /* written through the window, taken before it moved */
static u8 vchk[VPAGES];                    /* compare this round */
static u8 vsus[VPAGES];                    /* cleared since the last flush: TLB entries may still skip the bit */
static volatile u32 vram_flush_req, vram_flush_done;

void video_tlb_tick(void)
{
    u32 r = vram_flush_req;
    if (r == vram_flush_done) return;
    tlb_flush();
    vram_flush_done = r;
}

static int in_vram(u32 p) { return vram && p >= (u32)(uintptr_t)vram && p < (u32)(uintptr_t)vram + VRAM_SIZE; }

/* The window's dirty bits go to the vram pages behind it (under video_lock,
   as every remap of the window is). */
static int vwin_cleared;
static void vram_window_dirty(void)
{
    if (!vram || hv_active) return;
    for (u32 i = 0; i < 16; i++) {
        u32 lin = 0xA0000 + i * 4096, p = page_phys(lin);
        if (in_vram(p) && page_dirty(lin, 1)) { vwin[(p - (u32)(uintptr_t)vram) >> 12] = 1; vwin_cleared = 1; }
    }
}

/* Fills vchk for vram pages [p0, p1). 0: compare every row this round. */
static int vram_harvest(u32 p0, u32 p1)
{
    if (hv_active) return 0;
    int flushed = vram_flush_done == vram_flush_req;
    vwin_cleared = 0;
    vram_window_dirty();
    int cleared = vwin_cleared;
    for (u32 i = p0; i < p1; i++) {
        u8 d = (u8)(page_dirty((u32)(uintptr_t)vram + i * 4096, 1) | vwin[i]);
        cleared |= d;
        vwin[i] = 0;
        if (flushed) { vchk[i] = d | vsus[i]; vsus[i] = d; }
        else vsus[i] |= d;
    }
    if (cleared) vram_flush_req++;                 /* the boot CPU's TLB, at its next tick */
    return flushed;
}

static void vbe_init(void)
{
    vram = phys_alloc(VRAM_SIZE);
    vshadow = phys_alloc(VRAM_SIZE);
    set_user((u32)(uintptr_t)vram, VRAM_SIZE, 1);
}

/* The strings and the mode list (FFFFh-terminated), written when asked for:
   the BIOS image is copied over F000h after video_init. */
static void vbe_tables(void)
{
    static const char *const str[4] = { "vmdos VBE", "vmdos", "vmdos SVGA", "1.0" };
    for (int i = 0; i < 4; i++) {
        u32 a = VBE_DATA + i * 0x20;
        for (int j = 0; str[i][j]; j++) wr8(a + j, (u8)str[i][j]);
        wr8(a + 0x1F, 0);
    }
    for (int i = 0; i < N_VMODES; i++) wr16(VBE_DATA + 0x80 + i * 2, vmodes[i].no);
    wr16(VBE_DATA + 0x80 + N_VMODES * 2, 0xFFFF);
}

/* DPMI 0800h: the framebuffer is mapped already, at its own address. */
u32 video_vram_base(u32 *size) { *size = vram ? VRAM_SIZE : 0; return (u32)(uintptr_t)vram; }   /* hv.c */
int video_vram_range(u32 p, u32 n) { return vram && p >= (u32)(uintptr_t)vram && p + n <= (u32)(uintptr_t)vram + VRAM_SIZE; }

static void vbe_off(void)
{
    if (!vbe_on) return;
    vbe_on = 0;
    et_split = 0;
    et_on = 0;
    map_window(guest_phys(0xA0000));
}

static void vbe_map_bank(u32 b)
{
    vbe_bank = b;
    map_window((u32)(uintptr_t)vram + b * 65536);
}

static int vbe_find(u32 no)
{
    for (int i = 0; i < N_VMODES; i++) if (vmodes[i].no == (no & 0x1FF)) return i;
    return -1;
}

static void vbe_mode_info(int m, u32 a)
{
    memset(gptr(a), 0, 256);
    u32 bpp = vmodes[m].bpp, pitch = vbe_mode_pitch(m);
    if (bpp == 4) {                             /* planar: bytes a line in each plane */
        pitch = vmodes[m].w / 8;
        wr16(a + 0x00, 0x11F);                  /* supported, TTY, colour, graphics; doublescan; no LFB */
    } else
        /* supported, TTY, colour, graphics, LFB; VBE 3: doublescan, interlace
           (both through a CRTCInfoBlock) and triple buffering (4F07h BL=02h/04h) */
        wr16(a + 0x00, 0x79F);
    wr8(a + 0x02, 7);                           /* window A: exists, readable, writable */
    wr16(a + 0x04, 64); wr16(a + 0x06, 64);     /* granularity, size (KB) */
    wr16(a + 0x08, 0xA000);
    wr16(a + 0x0C, rd16(0xF0226)); wr16(a + 0x0E, 0xF000);   /* WinFuncPtr */
    wr16(a + 0x10, (u16)pitch);
    wr16(a + 0x12, vmodes[m].w); wr16(a + 0x14, vmodes[m].h);
    wr8(a + 0x16, 8); wr8(a + 0x17, 16);
    wr8(a + 0x18, bpp == 4 ? 4 : 1);            /* planes */
    wr8(a + 0x19, (u8)bpp); wr8(a + 0x1A, 1);
    wr8(a + 0x1B, bpp == 4 ? 3 : bpp == 8 ? 4 : 6);   /* EGA planar / packed pixel / direct colour */
    u32 mem = bpp == 4 ? 65536 : VRAM_SIZE;     /* planar: one 64 KB plane holds the screen's bytes */
    u32 pages = mem / (pitch * vmodes[m].h);
    u8 last = (u8)(pages > 256 ? 255 : pages - 1);   /* image pages, less one */
    wr8(a + 0x1D, last);
    wr8(a + 0x1E, 1);
    if (bpp == 4) return;                       /* no direct colour, no linear framebuffer */
    static const u8 masks[4][8] = {
        { 5, 10, 5, 5, 5, 0, 1, 15 },           /* 15: x:1 r:5 g:5 b:5 */
        { 5, 11, 6, 5, 5, 0, 0, 0 },            /* 16: r:5 g:6 b:5 */
        { 8, 16, 8, 8, 8, 0, 0, 0 },            /* 24: r:8 g:8 b:8 */
        { 8, 16, 8, 8, 8, 0, 8, 24 },           /* 32: x:8 r:8 g:8 b:8 */
    };
    const u8 *mk = masks[bpp == 15 ? 0 : bpp == 16 ? 1 : bpp == 24 ? 2 : 3];
    if (bpp != 8) memcpy(gptr(a + 0x1F), mk, 8);
    wr32(a + 0x28, hv_active ? hv_lfb() : (u32)(uintptr_t)vram);   /* PhysBasePtr (VMHV: where the VM sees it) */
    /* VBE 3: the linear framebuffer's own layout. The same as the banked
       one here, but VBE 3 clients read these fields (AdvanceMAME takes its
       colour masks from them), so left zero they'd see no colour at all. */
    wr16(a + 0x32, (u16)pitch);                 /* LinBytesPerScanLine */
    wr8(a + 0x34, last);                        /* BnkNumberOfImagePages */
    wr8(a + 0x35, last);                        /* LinNumberOfImagePages */
    if (bpp != 8) memcpy(gptr(a + 0x36), mk, 8);   /* LinRedMaskSize .. LinRsvdFieldPosition */
    wr32(a + 0x3E, VBE_MAX_CLOCK);              /* MaxPixelClock */
}

static void vbe_set(int m, int clear)
{
    v3_mode_set();
    /* A VESA mode set keeps the DAC width, as most SVGA BIOSes do: games
       switch to 8 bits before setting their mode as often as after. The
       default palette it loads is widened to match. */
    int keep8 = dac8;
    vbe_setting = 1;
    video_set_mode(0x13, 0);                    /* VGA state, default 256-colour palette */
    vbe_setting = 0;
    if (keep8) {
        for (int i = 0; i < 256; i++)
            for (int c = 0; c < 3; c++) { u8 v = vga_dac[i][c]; vga_dac[i][c] = (u8)(v << 2 | v >> 4); }
        dac8 = 1;
    }
    video_mode = vmodes[m].no;
    vbe_cur = vmodes[m].no;
    vbe_w = vmodes[m].w; vbe_h = vmodes[m].h; vbe_bpp = vmodes[m].bpp;
    vbe_pitch = vbe_mode_pitch(m);
    vbe_start = 0;
    et_on = 0;
    vbe_mode_w = vbe_w; vbe_mode_h = vbe_h;
    vbe_crtc_follow = 0;
    vbe_flip_pending = 0;
    /* the CRTC as an SVGA has it in this mode: display end in 8-pixel
       character clocks and in lines, no doublescan (AdvanceMAME's VBE 3
       driver reads them back, then retunes them for the size it wants) */
    {
        u32 de = vbe_h - 1;
        crtc[0x01] = (u8)(vbe_w / 8 - 1);
        crtc[0x12] = (u8)de;
        crtc[0x07] = (u8)((crtc[0x07] & ~0x42) | ((de >> 7) & 0x02) | ((de >> 3) & 0x40));
        crtc[0x09] &= 0x60;
    }
    vbe_on = 1;
    if (clear) memset(vram, 0, VRAM_SIZE);
    vbe_map_bank(0);
    full_redraw = 1;
    last_layout_mode = -1;
    kprintf("video: VESA mode %x, %ux%u, %u bpp\n", vbe_cur, vbe_w, vbe_h, vbe_bpp);
}

/* BH=0: set window BL to bank DX; BH=1: get it (INT 10h 4F05h and WinFuncPtr). */
static void video_vbe_window_(struct regs *r);
void video_vbe_window(struct regs *r) { rlock_take(&video_lock); video_vbe_window_(r); rlock_drop(&video_lock); }
static void video_vbe_window_(struct regs *r)
{
    if (BL(r) != 0) { AX(r) = 0x014F; return; }
    if (BH(r) == 1) { DX(r) = (u16)vbe_bank; AX(r) = 0x004F; return; }
    if ((u32)DX(r) * 65536 >= VRAM_SIZE) { AX(r) = 0x014F; return; }
    if (vbe_on) vbe_map_bank(DX(r)); else vbe_bank = DX(r);
    AX(r) = 0x004F;
}

/* ---------------- Tseng ET4000 ----------------
   For drivers that know SVGA chips rather than VESA (Windows 3.1's
   SVGA256/VDDSVGA, games' ET4000 support): the ET4000's BIOS modes 2Eh,
   30h, 38h (640x480, 800x600, 1024x768 in 256 colours) are the VESA modes
   of that size, with the CRTC as the ET4000 programs it (drivers check the
   width and height a mode set leaves there), and its segment select
   register at 3CDh banks the A000h window: bits 0-3 the 64 KB segment
   written, bits 4-7 the one read. With the two different (screen-to-screen
   copies) the window shows the read segment read-only and writes are
   emulated into the write segment. 1 MB, as on most ET4000 cards. */
int video_et_split(u32 lin) { return et_split && vbe_on && lin >= 0xA0000 && lin < 0xB0000; }

/* The window as the segments and the map mask want it: straight onto the
   segment when one segment does both and every plane is written, else the
   read segment read-only, with writes emulated (another segment, or the
   map mask: in chain-4 a byte at address a is in plane a & 3, written only
   if that plane's bit is set; drivers mask the edges of 32-bit writes so). */
static void et_remap(void)
{
    if (!vbe_on || !et_on) return;
    u32 wb = et_seg & 15, rb = et_seg >> 4;
    if (!(seq[4] & 8)) {                     /* chain-4 off: planar, every access emulated */
        if (et_split != 2) { for (u32 i = 0; i < 16; i++) map_page(0xA0000 + i * 4096, 0, 0); tlb_flush(); }
        et_split = 2;
        return;
    }
    if (wb == rb && (seq[2] & 15) == 15) { et_split = 0; vbe_map_bank(wb); return; }
    et_split = 1;
    vbe_bank = wb;
    map_window_flags((u32)(uintptr_t)vram + rb * 65536, 5);  /* the read segment, read-only */
}
static void et_segment(u8 v)
{
    et_seg = v;
    et_remap();
}
/* The ET4000's line pitch and display start, from the CRTC: offset (13h,
   bit 8 in 3Fh bit 7) in 8-byte units, as its 256-colour modes count it;
   start address (0Ch, 0Dh, bits 16-17 in 33h) in 4-byte units. Drivers set
   their own pitch (Windows' SVGA256: 1024 bytes for a 640-wide screen). */
static void et_crtc(void)
{
    if (!et_on || !vbe_on) return;
    u32 pitch = ((u32)(crtc[0x3F] & 0x80) << 1 | crtc[0x13]) * 8;
    u32 start = ((u32)(crtc[0x33] & 3) << 16 | (u32)crtc[0x0C] << 8 | crtc[0x0D]) * 4;
    if (pitch < vbe_w || start + pitch * vbe_h > VRAM_SIZE) return;
    if (pitch != vbe_pitch || start != vbe_start) { vbe_pitch = pitch; vbe_start = start; full_redraw = 1; }
}

/* Chain-4 off (the Windows driver's screen-to-screen copies: write mode 1
   moves four pixels a byte through the latches): the window is a 64 KB
   piece of each plane, segment s (mod 4, 256 KB a plane) at plane offset
   s * 64K; plane p's byte q is pixel 4q + p of the packed screen. */
static u8 *et_planar(u32 lin, u32 seg) { return vram + ((seg & 3) * 65536 + ((lin - 0xA0000) & 0xFFFF)) * 4; }
u8 video_et_read(u32 lin)
{
    if (et_split == 2) {
        u8 *b = et_planar(lin, et_seg >> 4);
        for (int p = 0; p < 4; p++) latch[p] = b[p];
        if (!(gc[5] & 8)) return latch[gc[4] & 3];
        u8 r = 0;                                  /* read mode 1: colour compare */
        for (int p = 0; p < 4; p++)
            if (gc[7] & (1 << p)) r |= latch[p] ^ ((gc[2] & (1 << p)) ? 0xFF : 0);
        return (u8)~r;
    }
    return vram[(u32)(et_seg >> 4) * 65536 + ((lin - 0xA0000) & 0xFFFF)];
}
void video_et_write(u32 lin, u8 v)
{
    if (et_split == 2) { vga_write_planes(v, et_planar(lin, et_seg), 1); return; }
    u32 o = (lin - 0xA0000) & 0xFFFF;
    if (!((seq[2] >> (o & 3)) & 1)) return;                    /* that plane is masked off */
    vram[(u32)(et_seg & 15) * 65536 + o] = v;
}

/* The ET4000's own registers (3CDh, CRTC 20h-3Fh) and BIOS modes are
   there unless the Voodoo 3 is (voodoo3): then the VGA is the Voodoo's,
   as on a real Voodoo 3 PC, and nothing answers as a Tseng (Windows would
   find two display adapters on the same VGA ports). */
static int et_ext(void) { return !v3_on; }

static int et_mode(int mode, int clear)
{
    if (!et_ext()) return 0;
    static const struct { u8 no; u16 vbe; u8 c1, c12, c7, c13; } t[] = {
        { 0x2E, 0x101, 0x4F, 0xDF, 0x3E, 0x50 },        /* 640x480: vertical display end 1DFh */
        { 0x30, 0x103, 0x63, 0x57, 0xF0, 0x64 },        /* 800x600: 257h */
        { 0x38, 0x105, 0x7F, 0xFF, 0xFD, 0x80 },        /* 1024x768: 2FFh */
    };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        if (t[i].no != mode) continue;
        int m = vbe_find(t[i].vbe);
        if (m < 0) return 0;
        vbe_set(m, clear);
        video_mode = mode;
        wr8(BDA + 0x49, (u8)mode);
        wr8(BDA + 0x87, (u8)((rd8(BDA + 0x87) & 0x7F) | (clear ? 0 : 0x80)));   /* vbe_set's inner mode set kept memory; AH=0Fh's bit 7 says what the caller asked */
        text_cols = vbe_w / 8; text_rows = vbe_h / 16;
        wr16(BDA + 0x4A, (u16)text_cols);
        wr8(BDA + 0x84, (u8)(text_rows - 1));
        wr16(BDA + 0x85, 16);
        crtc[0x01] = t[i].c1; crtc[0x12] = t[i].c12; crtc[0x07] = t[i].c7; crtc[0x13] = t[i].c13;
        crtc[0x09] = 0x40;                             /* no double scan */
        crtc[0x14] = 0x60; crtc[0x17] = 0xAB;
        misc_out = mode == 0x2E ? 0xE3 : 0x2F;
        et_seg = 0; et_split = 0;
        et_on = 1;
        return 1;
    }
    return 0;
}

/* The protected-mode interface's routines (4F0Ah): 47h set window,
   48h set display start (BL=00h/80h: CX:DX = byte offset / 4; VBE 3's
   BL=02h/82h: ECX = byte offset, a scheduled flip), 49h set palette. */
static void vbe_flip(u32 st);
/* ---------------- VESA 102h: 800x600 in 16 colours ----------------
   VBE 1.2's planar mode: mode 12h's four planes and VGA write logic with
   an 800x600 CRTC. 100 bytes a line in each plane, 60000 of its 64 KB, so
   the A000h window holds the whole screen and banking stays at 0. The
   16-colour renderer (ega_row) draws it from the CRTC as it draws any
   reprogrammed planar mode: line pitch from the offset register (13h),
   start from 0Ch/0Dh, pel panning from the attribute controller, so the
   logical line length (4F06h) and display start (4F07h) are those. */
static void planar_set(int m, int clear)
{
    video_set_mode(0x12, clear);            /* planes, sequencer, graphics and attribute controllers */
    static const u8 c800[25] = {            /* the CRTC for 800x600 at 56 Hz (VESA's first timing) */
        0x7B, 0x63, 0x64, 0x9E, 0x69, 0x92, 0x6F, 0xF0, 0x00, 0x60, 0x00, 0x00, 0, 0, 0, 0,
        0x58, 0x8A, 0x57, 0x32, 0x00, 0x58, 0x6F, 0xE3, 0xFF };
    memcpy(crtc, c800, sizeof c800);
    planar_vbe = vmodes[m].no;
    text_cols = vmodes[m].w / 8; text_rows = vmodes[m].h / 16;
    wr8(BDA + 0x49, 0x6A);                  /* 800x600x16's mode number on SVGA BIOSes */
    wr16(BDA + 0x4A, (u16)text_cols);
    wr16(BDA + 0x4C, (u16)(text_cols * vmodes[m].h));   /* the page: one screen of a plane */
    wr8(BDA + 0x84, (u8)(text_rows - 1));
    full_redraw = 1; last_layout_mode = -1;
    kprintf("video: VESA mode %x, %ux%u, 16 colours (planar)\n", planar_vbe, vmodes[m].w, vmodes[m].h);
}

/* Its line length in bytes a plane: the offset register counts words. */
static u32 planar_pitch(void) { return crtc[0x13] * 2u; }

/* Display start: off is a byte offset in a plane (the planes wrap at
   64 KB, as on a VGA), pan the pixel (0-7) within that byte. */
static void planar_start(u32 off, int pan)
{
    crtc[0x0C] = (u8)(off >> 8); crtc[0x0D] = (u8)off;
    atc[0x13] = (u8)(pan & 7);
    full_redraw = 1;
}

/* 4F06h and 4F07h in the planar mode. 0: a subfunction it doesn't have. */
static int planar_scanline(struct regs *r)
{
    u32 pitch = planar_pitch();
    if (BL(r) == 0 || BL(r) == 2) {
        u32 p = BL(r) == 0 ? ((u32)CX(r) + 15) / 16 * 2 : ((u32)CX(r) + 1) / 2 * 2;   /* whole words */
        if (p < 100 || p > 510) { AX(r) = 0x024F; return 1; }   /* the screen's width .. the offset register's reach */
        crtc[0x13] = (u8)(p / 2);
        pitch = p;
        full_redraw = 1;
    } else if (BL(r) == 3) {
        pitch = 510;
    } else if (BL(r) != 1) return 0;
    BX(r) = (u16)pitch; CX(r) = (u16)(pitch * 8);
    DX(r) = (u16)(65536 / pitch);
    AX(r) = 0x004F;
    return 1;
}
static int planar_display_start(struct regs *r)
{
    u32 pitch = planar_pitch();
    if ((BL(r) & 0x7F) == 0) {
        u32 off = DX(r) * pitch + CX(r) / 8;
        if (off >= 65536) { AX(r) = 0x014F; return 1; }
        planar_start(off, CX(r) & 7);
    } else if (BL(r) == 1) {
        u32 off = (u32)crtc[0x0C] << 8 | crtc[0x0D];
        BH(r) = 0; CX(r) = (u16)(off % pitch * 8 + (atc[0x13] & 7)); DX(r) = (u16)(off / pitch);
    } else return 0;
    AX(r) = 0x004F;
    return 1;
}

static void video_vbe_pm_(int id, u32 ebx, u32 ecx, u32 edx, u32 pal_lin);
void video_vbe_pm(int id, u32 ebx, u32 ecx, u32 edx, u32 pal_lin) { rlock_take(&video_lock); video_vbe_pm_(id, ebx, ecx, edx, pal_lin); rlock_drop(&video_lock); }
static void video_vbe_pm_(int id, u32 ebx, u32 ecx, u32 edx, u32 pal_lin)
{
    if (id == 0x47) {
        u32 b = edx & 0xFFFF;
        if ((ebx & 0xFF) == 0 && b * 65536 < VRAM_SIZE) { if (vbe_on) vbe_map_bank(b); else vbe_bank = b; }
    } else if (id == 0x48) {
        if ((ebx & 0x7F) == 2) {
            if (vbe_on && ecx <= VRAM_SIZE - vbe_pitch * vbe_h) vbe_flip(ecx);
            return;
        }
        /* CX:DX = byte offset / 4; VBE 3 callers (AdvanceMAME) put the
           offset's low two bits in DX bits 14-15, which a 16 or 24-bit mode
           scrolled by an odd pixel needs (4 MB: the dword address never
           reaches those bits itself). */
        if (planar_vbe) { planar_start((edx & 0xFFFF) << 16 | (ecx & 0xFFFF), 0); return; }   /* planar: the plane offset itself */
        u32 st = ((edx & 0x3FFF) << 16 | (ecx & 0xFFFF)) << 2 | ((edx >> 14) & 3);
        if (vbe_on && st <= VRAM_SIZE - vbe_pitch * vbe_h) vbe_start = st;
    } else if (id == 0x49 && pal_lin) {
        u32 n = ecx & 0xFFFF, first = edx & 0xFFFF;
        for (u32 i = 0; i < n && first + i < 256; i++) {
            vga_dac[first + i][0] = dac_in(rd8(pal_lin + i * 4 + 2));
            vga_dac[first + i][1] = dac_in(rd8(pal_lin + i * 4 + 1));
            vga_dac[first + i][2] = dac_in(rd8(pal_lin + i * 4));
        }
        palette_dirty = 1;
    }
}

/* ---------------- VBE 3.0 CRTC timings ----------------
   4F02h with BX bit 11 set passes a CRTCInfoBlock at ES:DI:
     00h HorizontalTotal  02h HorizontalSyncStart  04h HorizontalSyncEnd   (pixels)
     06h VerticalTotal    08h VerticalSyncStart    0Ah VerticalSyncEnd     (lines)
     0Ch Flags (0 doublescan, 1 interlaced, 2 -hsync, 3 -vsync)
     0Dh PixelClock (Hz)  11h RefreshRate (0.01 Hz)
   The vertical values count scan lines, so a doublescanned mode's are twice
   its height; an interlaced one's are the whole frame's (both fields).
   The BIOS programs the CRTC from them as an SVGA BIOS would: AdvanceMAME's
   vbeline/vbe3 then reads the display-end registers back, checks they are
   the mode's size (or twice it, doublescanned), and rewrites the CRTC for
   the game's own modeline at the same scale, which vbe_crtc_size follows.
   The refresh they give paces the vertical retrace 3DAh reports, so
   programs that sync to it (AdvanceMAME's vsync, its clock calibration)
   run at the modeline's rate. */
struct vbe_timing { u32 ht, hss, hse, vt, vss, vse, flags, clock, refresh; };

/* Reads and checks a CRTCInfoBlock. 0: one no CRTC could show; the mode set
   then fails and leaves the current mode as it was. */
static int vbe_timing_read(u32 a, struct vbe_timing *t)
{
    t->ht = rd16(a + 0x00); t->hss = rd16(a + 0x02); t->hse = rd16(a + 0x04);
    t->vt = rd16(a + 0x06); t->vss = rd16(a + 0x08); t->vse = rd16(a + 0x0A);
    t->flags = rd8(a + 0x0C); t->clock = rd32(a + 0x0D); t->refresh = rd16(a + 0x11);
    if ((t->flags & 3) == 3) return 0;                  /* doublescan and interlace at once */
    if (t->clock < VBE_MIN_CLOCK || t->clock > VBE_MAX_CLOCK) return 0;
    if (t->hss >= t->hse || t->hse > t->ht || t->vss >= t->vse || t->vse > t->vt) return 0;
    if (t->ht < 64 || t->vt < 64) return 0;
    return 1;
}

/* Programs the CRTC from the timings, for the mode vbe_set just set: the
   standard VGA layout (character clocks of 8 pixels, the vertical overflow
   bits in 07h/09h), blanking from the end of the display to the end of sync
   as AdvanceMAME's own CRTC code puts it, sync polarity in the misc output.
   Interlaced: the CRTC counts one field, half the frame's lines. */
static void vbe_timing_set(const struct vbe_timing *t)
{
    int ds = t->flags & 1, il = (t->flags & 2) != 0;
    u32 vt = il ? t->vt / 2 : t->vt, vss = il ? t->vss / 2 : t->vss, vse = il ? t->vse / 2 : t->vse;
    u32 vde = ds ? vbe_h * 2 : il ? vbe_h / 2 : vbe_h;  /* display end, in the CRTC's lines */
    u32 hbs = vbe_w < t->hss ? vbe_w : t->hss, hbe = t->hse;
    u32 vbs = vde < vss ? vde : vss, vbe = vse;
    u32 v6 = vt - 2, v12 = vde - 1;
    crtc[0x11] &= 0x7F;                                 /* registers 0-7 writable */
    crtc[0x00] = (u8)(t->ht / 8 - 5);
    crtc[0x01] = (u8)(vbe_w / 8 - 1);
    crtc[0x02] = (u8)(hbs / 8);
    crtc[0x03] = (u8)(0x80 | ((hbe / 8) & 0x1F));
    crtc[0x04] = (u8)(t->hss / 8);
    crtc[0x05] = (u8)(((hbe / 8) & 0x20) << 2 | ((t->hse / 8) & 0x1F));
    crtc[0x06] = (u8)v6;
    crtc[0x07] = (u8)(((v6 >> 8) & 1) | ((v12 >> 8) & 1) << 1 | ((vss >> 8) & 1) << 2 | ((vbs >> 8) & 1) << 3 |
                      0x10 | ((v6 >> 9) & 1) << 5 | ((v12 >> 9) & 1) << 6 | ((vss >> 9) & 1) << 7);
    crtc[0x09] = (u8)((ds ? 0x80 : 0) | 0x40 | ((vbs >> 9) & 1) << 5);   /* line compare bit 9 set: off */
    crtc[0x10] = (u8)vss;
    crtc[0x11] = (u8)((crtc[0x11] & 0x70) | (vse & 0x0F));
    crtc[0x12] = (u8)v12;
    crtc[0x15] = (u8)vbs;
    crtc[0x16] = (u8)vbe;
    crtc[0x18] = 0xFF;
    misc_out = (u8)((misc_out & 0x3F) | ((t->flags & 4) ? 0x40 : 0) | ((t->flags & 8) ? 0x80 : 0));
    vbe_interlace = il;

    /* the retrace 3DAh shows: once a field, the blanking part of it on */
    u32 hz100 = t->clock / t->ht * 100 / t->vt;         /* frames a second, x100 */
    if (il) hz100 *= 2;                                 /* two fields a frame */
    if (hz100 < 2000 || hz100 > 30000) hz100 = t->refresh >= 2000 && t->refresh <= 30000 ? t->refresh : 7000;
    vr_period = 119318200u / hz100;                     /* PIT ticks (1193182 Hz) a field */
    vr_len = vr_period * (vt > vde ? vt - vde : 1) / vt;
    if (vr_len < vr_period / 64) vr_len = vr_period / 64;
    if (vr_len > vr_period / 4) vr_len = vr_period / 4;

    vbe_crtc_follow = 1;
    kprintf("video: VESA mode %x, VBE 3 timings %u/%u/%u %u/%u/%u %u Hz clock%s%s: %u.%02u Hz\n",
            vbe_cur, t->hss, t->hse, t->ht, t->vss, t->vse, t->vt, t->clock,
            ds ? " doublescan" : "", il ? " interlaced" : "", hz100 / 100, hz100 % 100);
}

/* 4F07h BL=02h (and the PM interface's): a display start that takes effect
   at the next retrace. vmdos shows it at once (the screen is redrawn from
   vbe_start); BL=04h reports it done once a retrace has begun since. */
static void vbe_flip(u32 st)
{
    vbe_start = st;
    vbe_flip_at = vpit_clock();
    vbe_flip_pending = 1;
}
static int vbe_flip_done(void)
{
    if (vbe_flip_pending && vpit_clock() / vr_period != vbe_flip_at / vr_period) vbe_flip_pending = 0;
    return !vbe_flip_pending;
}

/* After a VBE 3 mode set with CRTC timings: the size the display-end
   registers give (AdvanceMAME's vbeline/vbe3 tunes them to the game's own
   size, 224x288 and so on), within what the pitch and memory hold. A
   doublescanned CRTC counts each line twice, an interlaced one each field. */
static void vbe_crtc_size(void)
{
    u32 B = vbe_bytespp(vbe_bpp);
    u32 w = ((u32)crtc[0x01] + 1) * 8;
    u32 de = (crtc[0x12] | (crtc[0x07] & 0x02) << 7 | (crtc[0x07] & 0x40) << 3) + 1u;
    u32 h = de / scan_per_row();
    if (vbe_interlace) h *= 2;
    if (w < 64 || h < 64 || w * B > vbe_pitch || h * vbe_pitch > VRAM_SIZE) return;   /* mid-change */
    if (w == vbe_w && h == vbe_h) return;
    vbe_w = w; vbe_h = h;
    full_redraw = 1;
    last_layout_mode = -1;
    kprintf("video: VESA mode %x shown as %ux%u (CRTC)\n", vbe_cur, vbe_w, vbe_h);
}

/* 4F04h: save / restore the video state. DL: 00h buffer size (BX, in
   64-byte blocks), 01h save, 02h restore, to / from ES:BX. CX says what:
   bits 0-2 as INT 10h AH=1Ch (registers, BIOS data, DAC: video_state does
   those), bit 3 the SVGA state, the VESA side of it: which mode, its size,
   pitch, display start, bank, DAC width and any VBE 3 timings. That block
   comes first in the buffer, the AH=1Ch state after it, so a restore puts
   the mode back before the registers that go with it. */
#define VBE_SVGA_STATE 128
#define VBE_STATE_MAGIC 0x53454256u             /* "VBES" */
static void video_state(struct regs *r);
static void vbe_state(struct regs *r)
{
    u16 what = CX(r) & 0x0F;
    u8 fn = DL(r);
    if (fn > 2) { AX(r) = 0x014F; return; }
    struct regs t = *r;                         /* the AH=1Ch part, past the SVGA block */
    AL(&t) = fn; CX(&t) = what & 7;
    if (what & 8) t.v86_es += VBE_SVGA_STATE / 16;
    if (fn == 0) {
        u32 n = 0;
        if (what & 7) { video_state(&t); n = BX(&t); }
        BX(r) = (u16)(n + (what & 8 ? VBE_SVGA_STATE / 64 : 0));
        AX(r) = 0x004F; return;
    }
    if (what & 8) {
        u32 a = LIN(r->v86_es, BX(r));
        u32 s[16];                              /* 64 bytes of these, then CRTC 20h-3Fh (the ET4000's) */
        if (fn == 1) {
            memset(gptr(a), 0, VBE_SVGA_STATE);
            s[0] = VBE_STATE_MAGIC; s[1] = (u32)vbe_on; s[2] = (u32)video_mode; s[3] = vbe_cur;
            s[4] = vbe_w; s[5] = vbe_h; s[6] = vbe_pitch; s[7] = vbe_start; s[8] = vbe_bank;
            s[9] = (u32)dac8; s[10] = (u32)vbe_crtc_follow; s[11] = (u32)vbe_interlace;
            s[12] = vr_period; s[13] = vr_len; s[14] = (u32)et_on | (u32)et_seg << 8 | (u32)planar_vbe << 16;
            s[15] = misc_out;
            for (int i = 0; i < 16; i++) wr32(a + i * 4, s[i]);
            for (int i = 0; i < 64 - 32; i++) wr8(a + 64 + i, crtc[32 + i]);
        } else {
            for (int i = 0; i < 16; i++) s[i] = rd32(a + i * 4);
            if (s[0] != VBE_STATE_MAGIC) { AX(r) = 0x014F; return; }
            if (s[1]) {
                int m = vbe_find(s[3]);
                if (m < 0) { AX(r) = 0x014F; return; }
                if (!vbe_on || vbe_cur != s[3] || video_mode != (int)s[2])   /* memory kept */
                    if (s[2] == s[3] || !et_mode((int)s[2], 0)) vbe_set(m, 0);   /* else an ET4000 BIOS mode */
                if (s[4] * vbe_bytespp(vbe_bpp) <= s[6] && s[5] * s[6] <= VRAM_SIZE && s[7] <= VRAM_SIZE - s[5] * s[6]) {
                    vbe_w = s[4]; vbe_h = s[5]; vbe_pitch = s[6]; vbe_start = s[7];
                }
                if (s[8] * 65536 < VRAM_SIZE) vbe_map_bank(s[8]);
                if ((int)s[9] != dac8) {
                    for (int i = 0; i < 256; i++)
                        for (int c = 0; c < 3; c++) { u8 v = vga_dac[i][c]; vga_dac[i][c] = s[9] ? (u8)(v << 2 | v >> 4) : v >> 2; }
                    dac8 = (int)s[9];
                }
                vbe_crtc_follow = (int)s[10]; vbe_interlace = (int)s[11];
                if (s[12] >= 1000 && s[13] < s[12]) { vr_period = s[12]; vr_len = s[13]; }
                if (et_on) { et_seg = (u8)(s[14] >> 8); et_remap(); }
                misc_out = (u8)s[15];
            } else if (s[14] >> 16) {          /* the planar VESA mode */
                if (planar_vbe != (u16)(s[14] >> 16)) planar_set(vbe_find(s[14] >> 16), 0);
            } else if (vbe_on || planar_vbe || video_mode != (int)s[2]) {
                video_set_mode((int)s[2], 0);   /* back to a VGA mode, memory kept */
            }
            for (int i = 0; i < 64 - 32; i++) crtc[32 + i] = rd8(a + 64 + i);
            full_redraw = 1; last_layout_mode = -1; palette_dirty = 1;
        }
    }
    if (what & 7) video_state(&t);
    AX(r) = 0x004F;
}

static void vbe_call(struct regs *r)
{
    u32 es_di = LIN(r->v86_es, DI(r));
    if ((AL(r) <= 1 || AL(r) == 9 || (AL(r) == 2 && (BX(r) & 0x0800))) && dpmi_reflected_buffer(r, &es_di))
        dbg(1, "VESA %02x from protected mode: buffer at %x\n", AL(r), es_di);
    vbe_tables();
    switch (AL(r)) {
    case 0x00: {
        int v2 = rd32(es_di) == 0x32454256u;     /* "VBE2" */
        memset(gptr(es_di + 4), 0, v2 ? 508 : 252);
        wr32(es_di, 0x41534556u);                /* "VESA" */
        wr16(es_di + 4, 0x0300);                 /* VBE 3.0 */
        wr32(es_di + 6, 0xF0000000u | (VBE_DATA & 0xFFFF));
        wr32(es_di + 0x0A, 1);                   /* capabilities: DAC switchable to 8 bits */
        wr32(es_di + 0x0E, 0xF0000000u | ((VBE_DATA + 0x80) & 0xFFFF));
        wr16(es_di + 0x12, VRAM_SIZE >> 16);
        if (v2) {
            wr16(es_di + 0x14, 0x0100);
            for (int i = 1; i < 4; i++) wr32(es_di + 0x12 + i * 4, 0xF0000000u | ((VBE_DATA + i * 0x20) & 0xFFFF));
        }
        AX(r) = 0x004F; return; }
    case 0x01: {
        int m = vbe_find(CX(r));
        if (m < 0) { AX(r) = 0x014F; return; }
        vbe_mode_info(m, es_di);
        AX(r) = 0x004F; return; }
    case 0x02: {
        u16 bx = BX(r);
        if ((bx & 0x1FF) < 0x100) { video_set_mode(bx & 0x7F, !(bx & 0x8000)); AX(r) = 0x004F; return; }
        int m = vbe_find(bx);
        if (m < 0) { AX(r) = 0x014F; return; }
        if (vmodes[m].bpp == 4) {               /* planar: the VGA's own timings, a CRTCInfoBlock or not */
            planar_set(m, !(bx & 0x8000));
            AX(r) = 0x004F; return;
        }
        struct vbe_timing t;                    /* VBE 3: bit 11, a CRTCInfoBlock at ES:DI */
        if ((bx & 0x0800) && !vbe_timing_read(es_di, &t)) {
            kprintf("video: VESA mode %x: CRTC timings refused (%u/%u/%u %u/%u/%u, %u Hz, flags %x)\n",
                    bx & 0x1FF, t.hss, t.hse, t.ht, t.vss, t.vse, t.vt, t.clock, t.flags);
            AX(r) = 0x014F; return;
        }
        vbe_set(m, !(bx & 0x8000));
        if (bx & 0x0800) vbe_timing_set(&t);
        AX(r) = 0x004F; return; }
    case 0x03: BX(r) = vbe_on ? vbe_cur : planar_vbe ? planar_vbe : (u16)video_mode; AX(r) = 0x004F; return;
    case 0x05: video_vbe_window(r); return;
    case 0x06: {
        if (planar_vbe && planar_scanline(r)) return;
        if (!vbe_on) { AX(r) = 0x014F; return; }
        u32 B = vbe_bytespp(vbe_bpp);
        if (BL(r) == 0 || BL(r) == 2) {
            u32 p = BL(r) == 0 ? CX(r) * B : (CX(r) + B - 1) / B * B;
            if (p < 8 * B || p * vbe_mode_h > VRAM_SIZE) { AX(r) = 0x024F; return; }   /* narrower than the mode: a VBE 3 retune follows */
            vbe_pitch = p;
            full_redraw = 1;
        }
        u32 pitch = BL(r) == 3 ? (VRAM_SIZE / vbe_h) / B * B : vbe_pitch;
        if (pitch > 0x7FFF) pitch = 0x7FFF / B * B;
        BX(r) = (u16)pitch; CX(r) = (u16)(pitch / B);
        DX(r) = (u16)(VRAM_SIZE / pitch > 0xFFFF ? 0xFFFF : VRAM_SIZE / pitch);
        AX(r) = 0x004F; return; }
    case 0x07: {
        if (planar_vbe && planar_display_start(r)) return;
        if (!vbe_on) { AX(r) = 0x014F; return; }
        u32 B = vbe_bytespp(vbe_bpp);
        if ((BL(r) & 0x7F) == 0) {
            u32 st = DX(r) * vbe_pitch + CX(r) * B;
            if (st + vbe_pitch * vbe_h > VRAM_SIZE) { AX(r) = 0x014F; return; }
            vbe_start = st;
        } else if (BL(r) == 1) {
            BH(r) = 0; CX(r) = (u16)(vbe_start % vbe_pitch / B); DX(r) = (u16)(vbe_start / vbe_pitch);
        } else if ((BL(r) & 0x7F) == 2) {       /* VBE 3: schedule a display start, ECX = byte offset */
            if (r->ecx > VRAM_SIZE - vbe_pitch * vbe_h) { AX(r) = 0x014F; return; }
            vbe_flip(r->ecx);
        } else if (BL(r) == 4) {                /* VBE 3: has the scheduled one been shown? */
            CX(r) = (u16)vbe_flip_done();
        } else { AX(r) = 0x014F; return; }      /* 03h/83h: stereo, not offered */
        AX(r) = 0x004F; return; }
    case 0x08:
        /* BL=0: set the width (BH: 8 for 8 bits, anything less: 6), BL=1: get it.
           Switching rescales what's loaded, so the colours on screen stay. */
        if (BL(r) == 0) {
            int want = BH(r) >= 8;
            if (want != dac8) {
                for (int i = 0; i < 256; i++)
                    for (int c = 0; c < 3; c++) { u8 v = vga_dac[i][c]; vga_dac[i][c] = want ? (u8)(v << 2 | v >> 4) : v >> 2; }
                dac8 = want;
                palette_dirty = 1;
                kprintf("video: %d-bit DAC\n", dac8 ? 8 : 6);
            }
        }
        if (BL(r) <= 1) { BH(r) = dac8 ? 8 : 6; AX(r) = 0x004F; } else AX(r) = 0x014F;
        return;
    case 0x0B:                                   /* VBE 3: the closest pixel clock (BL=0, ECX Hz, DX mode) */
        if (BL(r) != 0 || ((DX(r) & 0x1FF) >= 0x100 && vbe_find(DX(r)) < 0)) { AX(r) = 0x014F; return; }
        /* any clock in range is exact: ECX stays, or the nearest end of the range */
        if (r->ecx < VBE_MIN_CLOCK) r->ecx = VBE_MIN_CLOCK;
        if (r->ecx > VBE_MAX_CLOCK) r->ecx = VBE_MAX_CLOCK;
        AX(r) = 0x004F; return;
    case 0x04: vbe_state(r); return;
    case 0x0A:                                   /* protected-mode interface (bios.asm vbe_pmi) */
        if (BL(r) != 0) { AX(r) = 0x014F; return; }
        r->v86_es = 0xF000; DI(r) = rd16(0xF0228); CX(r) = rd16(0xF022A);
        AX(r) = 0x004F;
        return;
    case 0x09: {
        u32 n = CX(r), first = DX(r);
        if ((BL(r) & 0x7F) == 0)
            for (u32 i = 0; i < n && first + i < 256; i++) {
                vga_dac[first + i][0] = dac_in(rd8(es_di + i * 4 + 2));
                vga_dac[first + i][1] = dac_in(rd8(es_di + i * 4 + 1));
                vga_dac[first + i][2] = dac_in(rd8(es_di + i * 4));
            }
        else if (BL(r) == 1)
            for (u32 i = 0; i < n && first + i < 256; i++) {
                wr8(es_di + i * 4 + 2, vga_dac[first + i][0]); wr8(es_di + i * 4 + 1, vga_dac[first + i][1]);
                wr8(es_di + i * 4, vga_dac[first + i][2]); wr8(es_di + i * 4 + 3, 0);
            }
        else { AX(r) = 0x014F; return; }
        palette_dirty = 1;
        AX(r) = 0x004F; return; }
    }
    dbg(1, "VESA function %02x not supported\n", AL(r));
    AX(r) = 0x014F;
}

static u32 pack8(u32 r8, u32 g8, u32 b8)
{
    return (r8 >> (8 - rsz)) << rpos | (g8 >> (8 - gsz)) << gpos | (b8 >> (8 - bsz)) << bpos;
}

static void vbe_draw(u32 i, void *unused)
{
    u32 y = todo_y[i], w = fr.w, h = fr.h, B = fr.B;
    int px = fr.px, py = fr.py, slot = smp_slot();
    const u8 *src = vshadow + y * fr.rowb;         /* the same as vram's row: copied just now */
    u32 y0 = g_y + y * g_h / h, y1 = g_y + (y + 1) * g_h / h;
    u32 X0 = 0, X1 = g_w, cx0 = 0, cx1 = w;       /* sharp: only the screen columns showing them */
    if (!smooth) {
        dest_span(todo_x0[i], todo_x1[i], &X0, &X1);
        if (X0 >= X1) return;
        cx0 = xmap[X0]; cx1 = xmap[X1 - 1] + 1u;
    }
    u32 *cvt = sc_cvt[slot], *line = sc_line[slot];
    for (u32 x = cx0; x < cx1; x++) {
        u32 c;
        if (B == 1) c = pal[src[x]];
        else if (B == 2) {
            u32 v = ((const u16 *)src)[x];
            if (fr.bpp == 15) c = pack8((v >> 7) & 0xF8, (v >> 2) & 0xF8, (v << 3) & 0xF8);
            else c = pack8((v >> 8) & 0xF8, (v >> 3) & 0xFC, (v << 3) & 0xF8);
        } else if (B == 3) {
            const u8 *p = src + x * 3;              /* blue, green, red */
            c = pack8(p[2], p[1], p[0]);
        } else {
            u32 v = ((const u32 *)src)[x];
            c = pack8((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
        }
        cvt[x] = c;
    }
    if ((int)y >= py && (int)y < py + 16) {
        const char *a = arrow[y - py];
        for (int k = 0; a[k] && px + k < (int)w; k++)
            if (a[k] == '1') cvt[px + k] = fr.black; else if (a[k] == '2') cvt[px + k] = fr.white;
    }
    if (smooth) { smooth_row(y, cvt); return; }
    const u32 *out = g_w == w ? cvt : line;        /* the screen's own size: no scaling */
    if (out == line) for (u32 x = X0; x < X1; x++) line[x] = cvt[xmap[x]];
    for (u32 yy = y0; yy < y1; yy++) put_row(out + X0, X1 - X0, g_x + X0, yy);
}

static void refresh_vbe(void)
{
    u32 w = vbe_w, h = vbe_h, B = vbe_bytespp(vbe_bpp), rowb = w * B;
    int px = 0, py = -100;
    u16 ma, mxr;
    if (!mouse_pointer(&px, &py, &ma, &mxr)) py = -100;
    int moved = px != sh_ptr_x || py != sh_ptr_y;
    static u32 last_start = ~0u, last_pitch, last_rowb, last_h;      /* where the screen is in vram */
    u32 p0 = vbe_start >> 12, p1 = h ? (vbe_start + (h - 1) * vbe_pitch + rowb + 4095) >> 12 : p0;
    if (p0 > VPAGES) p0 = VPAGES;
    if (p1 > VPAGES) p1 = VPAGES;
    int track = vram_harvest(p0, p1) && !full_redraw &&                  /* every round: it clears the bits */
                vbe_start == last_start && vbe_pitch == last_pitch && rowb == last_rowb && h == last_h;
    last_start = vbe_start; last_pitch = vbe_pitch; last_rowb = rowb; last_h = h;
    u32 n = 0;                                     /* which rows changed, here; drawing them, shared */
    for (u32 y = 0; y < h && y < 4096; y++) {
        u32 off = vbe_start + y * vbe_pitch;
        if (off + rowb > VRAM_SIZE) break;
        int in_ptr = moved && (((int)y >= py && (int)y < py + 16) || ((int)y >= sh_ptr_y && (int)y < sh_ptr_y + 16));
        if (track && !in_ptr && !(B == 1 && pal_changed)) {             /* no page under the row written */
            u32 a = off >> 12, b = (off + rowb - 1) >> 12, any = 0;
            for (u32 q = a; q <= b && !any; q++) any = vchk[q];
            if (!any) continue;
        }
        const u8 *src = vram + off;
        u8 *sh = vshadow + y * rowb;
        int in_new = (int)y >= py && (int)y < py + 16, in_old = (int)y >= sh_ptr_y && (int)y < sh_ptr_y + 16;
        u32 x0 = 0, x1 = w;                        /* the source columns that changed */
        if (!full_redraw) {
            row_span(src, sh, rowb, B == 1 && pal_changed, &x0, &x1);
            x0 /= B; x1 = (x1 + B - 1) / B;
            if (moved && in_new) span_ptr(&x0, &x1, px, w);
            if (moved && in_old) span_ptr(&x0, &x1, sh_ptr_x, w);
            if (x0 >= x1) continue;
        }
        memcpy(sh, src, rowb);
        if (g_y + y * g_h / h == g_y + (y + 1) * g_h / h && !smooth) continue;   /* no screen row of its own */
        todo_y[n] = (u16)y; todo_x0[n] = (u16)x0; todo_x1[n] = (u16)x1; n++;
    }
    fr.w = w; fr.h = h; fr.B = B; fr.bpp = vbe_bpp; fr.rowb = rowb; fr.px = px; fr.py = py;
    fr.black = pack(0, 0, 0); fr.white = pack(63, 63, 63);
    draw_out(n, vbe_draw);
    sh_ptr_x = px; sh_ptr_y = py;
}
/* The Voodoo 3's picture (voodoo3.h) while its video processor is on: each
   row as 32-bit pixels from the card, compared with the last frame's
   (vshadow), the rows that changed drawn as a 32-bit VESA mode's are. */
static void refresh_voodoo(void)
{
    static u32 row[2048];
    u32 w = vd_w, h = vd_h, rowb = w * 4, n = 0;
    for (u32 y = 0; y < h && y < 4096; y++) {
        v3_row(y, row);
        u8 *sh = vshadow + y * rowb;
        u32 x0 = 0, x1 = w;
        if (!full_redraw) {
            row_span((const u8 *)row, sh, rowb, 0, &x0, &x1);
            x0 /= 4; x1 = (x1 + 3) / 4;
            if (x0 >= x1) continue;
        }
        memcpy(sh, row, rowb);
        if (g_y + y * g_h / h == g_y + (y + 1) * g_h / h && !smooth) continue;   /* no screen row of its own */
        todo_y[n] = (u16)y; todo_x0[n] = (u16)x0; todo_x1[n] = (u16)x1; n++;
    }
    fr.w = w; fr.h = h; fr.B = 4; fr.bpp = 32; fr.rowb = rowb; fr.px = 0; fr.py = -100;
    fr.black = pack(0, 0, 0); fr.white = pack(63, 63, 63);
    draw_out(n, vbe_draw);
}

/* For the Voodoo 3's status register: the vertical retrace, and which
   frame this is (a retrace has passed when it changes). */
int video_vretrace(void) { return vpit_clock() % vr_period < vr_len; }
u32 video_frame_no(void) { return vpit_clock() / vr_period; }

/* ---------------- on-screen note (speed changes, audio output) ---------------- */
#define OSD_MAX 48
static char osd_msg[OSD_MAX + 1];
static u32 osd_until;

static void video_osd_(const char *msg);
void video_osd(const char *msg) { rlock_take(&video_lock); video_osd_(msg); rlock_drop(&video_lock); }
static void video_osd_(const char *msg)
{
    int i = 0;
    for (; msg[i] && i < OSD_MAX; i++) osd_msg[i] = msg[i];
    osd_msg[i] = 0;
    osd_until = ticks + 2000;
}

/* ---------------- the log, over the running game (Ctrl+Shift+F10) ----------------
   Nothing pauses: the game goes on under it, and pausing is the game's own
   business. The newest lines at the bottom, following the
   log as it grows, over the whole screen; Ctrl+Shift+Up/Down/PgUp/PgDn/Home/End scroll back. */
#define LOGOV_W 76                              /* lib.c's LOG_W */
static volatile int logov_on, logov_scroll;
static int logov_rows;
void video_log_toggle(void) { logov_on = !logov_on; logov_scroll = 0; }
int video_log_on(void) { return logov_on; }
void video_log_scroll(int lines)
{
    long s = (long)logov_scroll + lines;
    logov_scroll = s < 0 ? 0 : s > 100000 ? 100000 : (int)s;
}
int video_log_page(void) { return logov_rows > 2 ? logov_rows - 2 : 1; }

static void draw_log_box(void);
static void draw_log_overlay(void)             /* in video_refresh_, under video_lock */
{
    static int was_on;
    if (!logov_on) {
        if (was_on) { was_on = 0; full_redraw = 1; last_layout_mode = -1; }   /* what it covered, next frame */
        return;
    }
    was_on = 1;
    if (!draw_late) draw_log_box();             /* else after the helper's rows (video_refresh_late) */
}
static void draw_log_box(void)
{
    static char text[64][LOGOV_W + 1];
    static u32 line[4096];
    int s = fb_w >= 1280 && fb_h >= 800 ? 2 : 1;
    int rows = (int)fb_h / (16 * s);                                 /* the whole screen */
    if (rows > 64) rows = 64;
    if (rows < 3) return;
    logov_rows = rows;
    int body = rows - 1;                                             /* row 0: the title */
    logov_scroll = log_tail(&text[0][0], body, logov_scroll);
    u32 w = fb_w < 4096 ? fb_w : 4096, h = (u32)(rows * 16 * s);
    u32 x0 = 0, y0 = 0;
    u32 fg = pack(55, 55, 55), bg = pack(0, 0, 16), tfg = pack(63, 63, 63), tbg = pack(0, 0, 42);
    static const char title_live[] = "vmdos log - Ctrl+Shift+F10 closes, Ctrl+Shift+PgUp/PgDn scrolls";
    static const char title_back[] = "vmdos log (scrolled back: Ctrl+Shift+End for the newest)";
    for (int r = 0; r < rows; r++) {
        const char *t = r == 0 ? (logov_scroll ? title_back : title_live) : text[r - 1];
        u32 f = r == 0 ? tfg : fg, b = r == 0 ? tbg : bg;
        for (int gy = 0; gy < 16 * s; gy++) {
            int fy = gy / s;
            int ended = 0;
            for (u32 x = 0; x < w; x++) {
                int c = (int)(x / (8 * s)) - 1, bit = (int)(x / s) & 7;
                char ch = ' ';
                if (c >= 0 && !ended) { ch = t[c]; if (!ch) { ended = 1; ch = ' '; } }
                if (c < 0 || c >= LOGOV_W) ch = ' ';
                line[x] = (font8x16[(u8)ch * 16 + fy] & (0x80 >> bit)) ? f : b;
            }
            put_row(line, w, x0, y0 + (u32)(r * 16 * s + gy));
        }
    }
    for (u32 x = 0; x < w; x++) line[x] = bg;                        /* what's left under the last row */
    for (u32 y = h; y < fb_h; y++) put_row(line, w, x0, y);
}

static void draw_osd_text(void);
static void draw_osd(void)
{
    if (!osd_msg[0]) return;
    if ((int32_t)(ticks - osd_until) >= 0) {        /* expired: redraw what it covered */
        osd_msg[0] = 0;
        full_redraw = 1;
        last_layout_mode = -1;
        return;
    }
    draw_osd_text();
}

static void draw_osd_text(void)
{
    static u32 line[(OSD_MAX + 2) * 8 * 2];
    int n = 0;
    while (osd_msg[n]) n++;
    int s = fb_w >= 1280 ? 2 : 1;
    while (n > 0 && (u32)((n + 2) * 8 * s) > fb_w) n--;            /* narrow screen: cut the note */
    int w = (n + 2) * 8 * s;
    u32 fg = pack(63, 63, 63), bg = pack(0, 0, 42);
    u32 x0 = fb_w > (u32)w + 8 ? fb_w - w - 8 : 0;
    for (int y = 0; y < 20 * s; y++) {
        int gy = y / s - 2;
        for (int x = 0; x < w; x++) {
            int c = x / (8 * s) - 1, bit = (x / s) & 7;
            int on = c >= 0 && c < n && gy >= 0 && gy < 16 && (font8x16[(u8)osd_msg[c] * 16 + gy] & (0x80 >> bit));
            line[x] = on ? fg : bg;
        }
        put_row(line, w, x0, 8 + y);
    }
}

/* Ctrl+Shift+F9: the screen as it goes to the display (read back from the
   framebuffer), kept in RAM as a 24-bit BMP until the next one; VMSHOT.COM
   saves it to a file (INT 2Fh AX=5650h, video_shot_api). */
volatile int shot_req;
static u8 *shot;
static u32 shot_len;

static u8 widen(u32 v, u32 sz) { return sz >= 8 ? (u8)v : (u8)(v << (8 - sz) | v >> (2 * sz - 8)); }

static void take_shot(void)
{
    shot_req = 0;
    u32 row = (fb_w * 3 + 3) & ~3u, len = 54 + row * fb_h;
    if (!shot) shot = phys_try_alloc(len);
    if (!shot) { video_osd_("Screenshot: no memory"); return; }   /* not video_osd: video_refresh_late has no video_lock, and must not wait for it */
    memset(shot, 0, 54);
    shot[0] = 'B'; shot[1] = 'M';
    *(u32 *)(shot + 2) = len; *(u32 *)(shot + 10) = 54; *(u32 *)(shot + 14) = 40;
    *(u32 *)(shot + 18) = fb_w; *(u32 *)(shot + 22) = fb_h;
    *(u16 *)(shot + 26) = 1; *(u16 *)(shot + 28) = 24; *(u32 *)(shot + 34) = row * fb_h;
    u32 B = (fb_bpp + 7) / 8;
    for (u32 y = 0; y < fb_h; y++) {
        const u8 *s = fb + y * fb_pitch;
        u8 *d = shot + 54 + (fb_h - 1 - y) * row;                   /* BMP rows go bottom up */
        for (u32 x = 0; x < fb_w; x++, s += B, d += 3) {
            u32 v = B == 4 ? *(const u32 *)s : B == 3 ? (u32)(s[0] | s[1] << 8 | s[2] << 16) : *(const u16 *)s;
            u32 r = (v >> rpos) & ((1u << rsz) - 1), g = (v >> gpos) & ((1u << gsz) - 1), b = (v >> bpos) & ((1u << bsz) - 1);
            d[0] = widen(b, bsz); d[1] = widen(g, gsz); d[2] = widen(r, rsz);
        }
    }
    shot_len = len;
    kprintf("video: screenshot %ux%u kept (VMSHOT saves it)\n", fb_w, fb_h);
    video_osd_("Screenshot kept: VMSHOT saves it");
}

/* BL=0: DX:CX = the BMP's size (0: none yet). BL=1: copy CX bytes from
   offset DX:SI to ES:DI; CX = what was copied. BX = 'VM' either way. */
static void video_shot_api_(struct regs *r);
void video_shot_api(struct regs *r) { rlock_take(&video_lock); video_shot_api_(r); rlock_drop(&video_lock); }
static void video_shot_api_(struct regs *r)
{
    u32 off = (u32)DX(r) << 16 | SI(r);
    if (BL(r) == 1) {
        u32 n = CX(r);
        if (off >= shot_len) n = 0;
        else if (n > shot_len - off) n = shot_len - off;
        memcpy(gptr(LIN(r->v86_es, DI(r))), shot + off, n);
        CX(r) = (u16)n;
    } else { DX(r) = (u16)(shot_len >> 16); CX(r) = (u16)shot_len; }
    AX(r) = 0;
    BX(r) = 0x564D;
}

static void video_refresh_(void);
void video_refresh(void) { rlock_take(&video_lock); video_refresh_(); rlock_drop(&video_lock); }
/* The helper CPU, after video_refresh with video_lock let go: the frame's
   drawing (draw_late), a screenshot asked for meanwhile, then the note
   again on top. */
void video_refresh_late(void)
{
    if (!draw_late) return;
    draw_rows();
    if (shot_req) take_shot();
    if (logov_on) draw_log_box();
    if (osd_msg[0] && (int32_t)(ticks - osd_until) < 0) draw_osd_text();
    draw_late = 0;
}

static void video_refresh_(void)
{
    static int busy;
    if (busy) return;
    if (draw_late && !smp_on_ap()) {               /* the helper is drawing: wait (unless it's gone) */
        while (draw_late && smp_active) __asm__ volatile("pause");
        if (draw_late) { draw_late = 0; full_redraw = 1; }
    }
    busy = 1;
    frame_no++;
    if (par_lost) { par_lost = 0; full_redraw = 1; }
    if ((frame_no & 15) == 0) blink_phase ^= 1;
    if (vga_native) { video_native_pointer(); busy = 0; return; }   /* video=vbe: the card shows it */
    if (text_fallback) { refresh_fallback(); busy = 0; return; }
    if (!fb) { busy = 0; return; }
    tick_poll_begin();
    {
        u32 w = 0, h = 0;
        int on = v3_display(&w, &h);
        if (on && w * h * 4 > VRAM_SIZE) h = VRAM_SIZE / (w * 4);   /* vshadow's size */
        if (on != vd_on || (on && (w != vd_w || h != vd_h))) {
            kprintf("video: the Voodoo 3's picture %s (%ux%u)\n", on ? "on" : "off", w, h);
            vd_on = on; vd_w = w; vd_h = h;
            last_layout_mode = -1;
        }
    }
    if (smooth_toggle_req) {                       /* Ctrl+Shift+F8 */
        smooth_toggle_req = 0;
        if (smooth_opt < 0) layout();              /* the command line's choice first */
        if (unscaled) unscaled = 0, smooth_opt = 0, xbrz_opt = 0;  /* sharp -> smooth -> xBRZ -> none -> sharp */
        else if (!smooth_opt) smooth_opt = 1, xbrz_opt = 0;
        else if (!xbrz_opt) xbrz_opt = 1;
        else smooth_opt = 0, xbrz_opt = 0, unscaled = 1;
        last_layout_mode = -1;
        layout();
        const char *m = unscaled ? "No scaling (VGA size, black border)" :
                        !smooth_opt ? "Sharp scaling (whole pixels)" :
                        !smooth ? (xbrz_opt ? "xBRZ scaling: not on this screen" : "Smooth scaling: not on this screen") :
                        xbrz_k ? "xBRZ scaling" : xbrz_opt ? "xBRZ scaling (smooth in text modes)" : "Smooth scaling";
        video_osd(m);
        kprintf("video: %s\n", m);
    }
    layout();
    if (palette_dirty) {
        palette_dirty = 0;                       /* first: the DAC is written without video_lock */
        __asm__ volatile("" ::: "memory");
        int changed = 0;
        for (int i = 0; i < 256; i++) {          /* the pixel mask (3C6h) gates the index first */
            const u8 *d = vga_dac[i & dac_mask];
            u32 c = dac8 ? pack8(d[0], d[1], d[2]) : pack(d[0], d[1], d[2]);
            pal_hit[i] = c != pal[i];
            changed |= pal_hit[i];
            pal[i] = c;
        }
        /* Graphics rows hold DAC indices: redraw just the rows using a changed
           one (none at all for the direct-color VESA modes). Text colors also go
           through the attribute registers, which set palette_dirty too: all of it. */
        if (console_on || is_text(video_mode)) full_redraw = 1;
        else if (changed && (!vbe_on || vbe_bytespp(vbe_bpp) == 1)) pal_changed = 1;
    }
    if (console_on) { refresh_text(console_cells, -1); }
    else if (v3_on) { if (vd_on) refresh_voodoo(); }   /* voodoo3: only the card's picture (A0000h-BFFFFh are its) */
    else if (is_text(video_mode)) {
        u16 start = crtc[0x0C] << 8 | crtc[0x0D];
        u16 cur = (u16)((crtc[0x0E] << 8 | crtc[0x0F]) - start);
        refresh_text((const u16 *)gptr(text_base + start * 2), cur);
    } else if (vbe_on) refresh_vbe();
    else if (video_mode == 0x13 || is_cga(video_mode) || is_ega(video_mode)) refresh_gfx();
    full_redraw = 0;
    pal_changed = 0;
    if (shot_req && !draw_late) take_shot();       /* before the note goes on top (drawn later: then) */
    draw_log_overlay();
    draw_osd();
    tick_poll_end();
    busy = 0;
}

/* A full screen of text over whatever the guest shows: title, then msg.
   attr: 4Fh white on red (vmdos stopped), 1Fh white on blue (the log). */
static void video_console_attr_(const char *title, const char *msg, u16 attr);
void video_console_attr(const char *title, const char *msg, u16 attr) { rlock_take(&video_lock); video_console_attr_(title, msg, attr); rlock_drop(&video_lock); }
static void video_console_attr_(const char *title, const char *msg, u16 attr)
{
    u16 a = (u16)(attr << 8);
    if (vga_native) title = "vmdos stopped (video=vbe: the screen shows only in a text mode)";
    for (int i = 0; i < 80 * 25; i++) console_cells[i] = a | 0x20;
    int row = 1, col = 2;
    for (int i = 0; title[i]; i++) console_cells[row * 80 + col + i] = a | (u8)title[i];
    row = 3;
    for (; *msg && row < 24; msg++) {
        if (*msg == '\n' || col >= 78) { row++; col = 2; if (*msg == '\n') continue; }
        console_cells[row * 80 + col++] = a | (u8)*msg;
    }
    if (vga_native) {                             /* video=vbe: into the card's text memory as it is */
        memcpy(gptr(0xB8000), console_cells, sizeof console_cells);
        return;
    }
    console_on = 1;
    if (!is_text(video_mode)) { video_mode = 3; text_cols = 80; }
    dac8 = 0; dac_mask = 0xFF;
    default_palette(0);
    last_layout_mode = -1;
    video_refresh();
}

void video_console(const char *msg) { video_console_attr("vmdos stopped", msg, 0x4F); }

/* Around a console the guest gets back (Ctrl+Shift+F10, the log): its
   mode, text width and palette, then a full redraw. */
static struct { int on, mode, cols, dac8; u8 mask; u8 dac[256][3]; } saved;
static void video_console_save_(void);
void video_console_save(void) { rlock_take(&video_lock); video_console_save_(); rlock_drop(&video_lock); }
static void video_console_save_(void)
{
    saved.on = 1; saved.mode = video_mode; saved.cols = text_cols;
    saved.dac8 = dac8; saved.mask = dac_mask;                  /* the 8-bit DAC too: the console's is 6-bit */
    memcpy(saved.dac, vga_dac, sizeof vga_dac);
}
static void video_console_restore_(void);
void video_console_restore(void) { rlock_take(&video_lock); video_console_restore_(); rlock_drop(&video_lock); }
static void video_console_restore_(void)
{
    if (!saved.on) return;
    saved.on = 0;
    console_on = 0;
    video_mode = saved.mode; text_cols = saved.cols;
    dac8 = saved.dac8; dac_mask = saved.mask;
    memcpy(vga_dac, saved.dac, sizeof vga_dac);
    palette_dirty = 1;
    full_redraw = 1;
    last_layout_mode = -1;
    video_refresh();
}

void video_text_fallback(void) { text_fallback = 1; }
int video_fb_ready(void) { return fb != 0 || text_fallback; }

/* ---------------- video=vbe (vgabios.c): the card's own BIOS ----------------
   The mode is the card's; video_mode follows the BIOS data area, for the
   mouse driver's ranges. In text modes the INT 33h pointer is drawn into
   video memory as a mouse driver does: the cell's attribute ANDed and
   XORed with the masks (fn 0Ah), put back when it moves or is hidden,
   unless the program has written the cell since. Called on show / hide /
   move (INT 33h) and with each refresh (motion). */
void video_native_sync(void)
{
    if (vga_native) video_mode = rd8(BDA + 0x49) & 0x7F;
}

static int native_height(void)
{
    video_native_sync();
    int m = video_mode;
    if (is_text(m)) { int rows = rd8(BDA + 0x84) + 1; return (rows < 25 ? 25 : rows) * 8; }
    if (m == 0x0F || m == 0x10) return 350;
    if (m == 0x11 || m == 0x12) return 480;
    return 200;
}

static u32 np_addr;
static u16 np_saved, np_drawn;
static int np_on;
static void video_native_pointer_(void);
void video_native_pointer(void) { if (!vga_native) return; rlock_take(&video_lock); video_native_pointer_(); rlock_drop(&video_lock); }
static void video_native_pointer_(void)
{
    int x, y;
    u16 am, xm;
    video_native_sync();
    int shown = is_text(video_mode) && mouse_pointer(&x, &y, &am, &xm);
    u32 a = 0;
    if (shown) {
        u32 cols = rd16(BDA + 0x4A), rows = rd8(BDA + 0x84) + 1u;
        if (!cols || cols > 132) cols = 80;
        if (rows < 25) rows = 25;
        u32 col = (u32)(x < 0 ? 0 : x) / (cols <= 40 ? 16 : 8), row = (u32)(y < 0 ? 0 : y) / 8;
        if (col >= cols) col = cols - 1;
        if (row >= rows) row = rows - 1;
        a = (video_mode == 7 ? 0xB0000 : 0xB8000) + rd16(BDA + 0x4E) + (row * cols + col) * 2;
        if (a + 2 > 0xC0000) shown = 0;
    }
    if (np_on && (!shown || a != np_addr)) {
        if (rd16(np_addr) == np_drawn) wr16(np_addr, np_saved);
        np_on = 0;
    }
    if (!shown || (np_on && rd16(a) == np_drawn)) return;
    np_saved = rd16(a);
    np_drawn = (u16)((np_saved & am) ^ xm);
    wr16(a, np_drawn);
    np_addr = a;
    np_on = 1;
}

void video_init(void)
{
    vga_native_init();
    if (vga_native) { fb = 0; return; }          /* video=vbe: nothing drawn by vmdos */
    vbe_init();
    if (!fb && !bochs_vbe()) {
        kprintf("no linear framebuffer: using VGA text mode\n");
        text_fallback = 1;
    }
}

/* ---------------- VGA ports ---------------- */
/* vgalog (command line): every VGA port access the VMHV guest makes from
   protected mode (Windows' display VxD, not DOS programs or the BIOS), with
   the register index for the data ports, and every INT 10h call. */
static int vga_logging(u16 *cs, u32 *eip)
{
    if (vgalog < 0) vgalog = strstr(cmdline, "vgalog") != 0;
    return vgalog && hv_guest_pm(cs, eip);
}
static int vga_index(u16 port)
{
    switch (port) {
    case 0x3C1: return atc_idx;
    case 0x3C5: return seq_idx;
    case 0x3CF: return gc_idx;
    case 0x3B5: case 0x3D5: return crtc_idx;
    }
    return -1;
}
static u32 video_port_in_(u16 port);
u32 video_port_in(u16 port)
{
    u32 v = video_port_in_(port);
    u16 cs; u32 eip;
    if (port != 0x3DA && port != 0x3BA && vga_logging(&cs, &eip)) {
        int ix = vga_index(port);
        if (ix >= 0) kprintf("vga %04x:%08x in  %03x[%02x] -> %02x\n", cs, eip, port, ix, v & 0xFF);
        else kprintf("vga %04x:%08x in  %03x -> %02x\n", cs, eip, port, v & 0xFF);
    }
    return v;
}
static u32 video_port_in_(u16 port)
{
    switch (port) {
    case 0x3C0: return atc_idx | atc_pas;
    case 0x3C1: return atc[atc_idx & 31];
    case 0x3C2: return 0x10;
    case 0x3D9: return cga_sel;
    case 0x3C4: return seq_idx & 7;
    case 0x3C5: return seq_idx < 8 ? seq[seq_idx] : 0;   /* past the VGA's (and ET4000's): none */
    case 0x3C6: return dac_mask;
    case 0x3C7: return 3;
    case 0x3C8: return dac_widx;
    case 0x3C9: {
        u8 v = vga_dac[dac_ridx][dac_rc];
        if (++dac_rc == 3) { dac_rc = 0; dac_ridx++; }
        return v; }
    case 0x3CA: return 0;
    case 0x3CC: return misc_out;
    case 0x3CD: return et_ext() ? et_seg : 0xFF;   /* ET4000 segment select */
    case 0x3CE: return gc_idx & 15;
    case 0x3CF: return gc[gc_idx & 15];
    case 0x3B4: case 0x3D4: return crtc_idx;
    case 0x3B5: case 0x3D5: return crtc_idx < (et_ext() ? 64 : 0x20) ? crtc[crtc_idx] : 0;   /* past the ET4000's: none
                                                    (wrapped, 50h read as 10h: an ARK chip ID to svgalib) */
    case 0x3BA: case 0x3DA: {
        atc_flip = 0;
        u32 t = vpit_clock();
        u8 v = 0;
        if (t % vr_period < vr_len) v |= 0x09;    /* vertical retrace (70 Hz, or the VBE 3 timings' refresh) */
        else if (t % 38 < 8) v |= 0x01;           /* horizontal blanking */
        return v; }
    }
    return 0xFF;
}

/* A program that sets a graphics mode through the registers itself, with
   no INT 10h (or from text mode with one): graphics on in both the graphics
   controller and the attribute controller. 256-colour (attribute mode bit 6)
   is drawn as mode 13h, else as 16-colour planar; its size then comes from
   the CRTC (ega_geom, m13_geom). Turned back off: text mode 3. A BIOS
   16-colour mode (12h, 0Dh ...) switched to 256 colours by the attribute
   controller is unchained 256-colour too (Ultrabots: mode 12h, then
   Mode X style 320-wide): its planes, already unchained, carry over. */
static void reg_mode_check(void)
{
    if (console_on || vbe_on) return;
    int gfx = (gc[6] & 1) && (atc[0x10] & 1), c256 = atc[0x10] & 0x40;
    int want = !gfx ? (reg_mode ? 3 : -1) :
               is_text(video_mode) || reg_mode ? (c256 ? 0x13 : 0x12) :
               video_mode == 0x13 && !c256 ? 0x12 :
               is_ega(video_mode) && c256 ? 0x13 : -1;
    if (want < 0 || want == video_mode) return;
    kprintf("video: mode set through the registers: %s\n", want == 3 ? "back to text" : want == 0x13 ? "256-colour" : "16-colour planar");
    int from_ega = ega_on;
    video_mode = want;
    reg_mode = want != 3;
    if (want == 3) { text_cols = 80; text_base = 0xB8000; }
    if (want == 0x13) {                            /* the 16-colour window off first, then the planar one on */
        ega_check();
        planes_live = from_ega;
        planar_check();
        planes_live = 0;
    } else {
        planar_check();
        ega_check();
    }
    last_layout_mode = -1;
    full_redraw = 1;
}

static void video_port_out_(u16 port, u8 v);
void video_port_out(u16 port, u8 v)
{
    u16 cs; u32 eip;
    if (vga_logging(&cs, &eip)) {
        int ix = vga_index(port);
        if (port == 0x3C0 && atc_flip) ix = atc_idx;            /* 3C0: index, then data */
        if (ix >= 0) kprintf("vga %04x:%08x out %03x[%02x] <- %02x\n", cs, eip, port, ix, v);
        else kprintf("vga %04x:%08x out %03x <- %02x\n", cs, eip, port, v);
        static int dumped;
        static u32 gc6_eip;
        if (port == 0x3CF && (gc_idx & 15) == 6 && v == 0x0D && !(dumped & 1)) { dumped |= 1; gc6_eip = eip; hv_dump_here("GC6 <- 0D"); }
        if (port == 0x3C0 && v == 0x20 && !atc_flip && !(dumped & 2) && (dumped & 1)) { dumped |= 2; hv_dump_here("ATC <- 20"); kprintf("VDD data:\n"); hv_dump_lin(0x80285000, 0x200);
            kprintf("VDD code after (int 20h calls that ran are patched):\n"); hv_dump_lin(gc6_eip - 0x40, 0x180);
            vdd_code_at = gc6_eip; }
    }
    if (port >= 0x3C6 && port <= 0x3C9) { video_port_out_(port, v); return; }   /* the DAC: data only (palette fades) */
    rlock_take(&video_lock);
    video_port_out_(port, v);
    rlock_drop(&video_lock);
}
static void video_port_out_(u16 port, u8 v)
{
    switch (port) {
    case 0x3C0:
        if (!atc_flip) { atc_idx = v & 0x1F; atc_pas = v & 0x20; }
        else { atc[atc_idx & 31] = v; if (atc_idx < 21) palette_dirty = 1; if (atc_idx == 0x10) reg_mode_check(); }
        atc_flip ^= 1;
        return;
    case 0x3C2: misc_out = v; return;
    case 0x3CD: if (et_ext()) et_segment(v); return;   /* ET4000 segment select */
    case 0x3D9: cga_color_select(v); return;           /* CGA colour select */
    case 0x3C4: seq_idx = v; return;
    case 0x3C5:
        if (seq_idx >= 8) return;                   /* another chip's register (a chipset probe): none here */
        seq[seq_idx] = v;
        if (seq_idx == 4) { planar_check(); et_remap(); }
        else if (seq_idx == 2) { readmap_last = 0; apply_map_mask(); et_remap(); }
        return;
    case 0x3C6: dac_mask = v; palette_dirty = 1; return;
    case 0x3C7: dac_ridx = v; dac_rc = 0; return;
    case 0x3C8: dac_widx = v; dac_wc = 0; return;
    case 0x3C9:
        vga_dac[dac_widx][dac_wc] = dac_in(v);
        if (++dac_wc == 3) { dac_wc = 0; dac_widx++; }
        palette_dirty = 1;
        return;
    case 0x3CE: gc_idx = v; return;
    case 0x3CF:
        gc[gc_idx & 15] = v;
        if ((gc_idx & 15) == 4) readmap_last = 1;
        apply_map_mask();
        if ((gc_idx & 15) == 6) { reg_mode_check(); ega_bmap(); }
        return;
    case 0x3B4: case 0x3D4: crtc_idx = v; return;
    case 0x3B5: case 0x3D5:
        if (crtc_idx >= (et_ext() ? 64 : 0x20)) return;
        crtc[crtc_idx] = v;
        et_crtc();
        if (vbe_on && vbe_crtc_follow && (crtc_idx == 0x01 || crtc_idx == 0x07 || crtc_idx == 0x09 || crtc_idx == 0x12))
            vbe_crtc_size();
        if ((crtc_idx & 31) == 0x0C || (crtc_idx & 31) == 0x0D) full_redraw = 1;
        return;
    }
}

/* ---------------- INT 10h ---------------- */
static u32 cell_addr(int page, int row, int col)
{
    return text_base + page * rd16(BDA + 0x4C) + (row * text_cols + col) * 2;
}

/* Byte-oriented graphics modes (4, 5, 6, 13h): where pixel row y starts and
   bits per pixel. */
static int gfx_bpp(void) { return video_mode == 0x13 ? 8 : video_mode == 6 ? 1 : is_cga(video_mode) ? 2 : 0; }
static u32 gfx_row_addr(int y)
{
    if (video_mode == 0x13) return 0xA0000 + y * 320;
    return 0xB8000 + (y & 1) * 0x2000 + (y >> 1) * 80;
}

/* Mode 13h with chain-4 off (planar_on): the window may be unmapped (every
   guest access emulated), so BIOS drawing goes to the planes, pixel a of
   the 320-wide screen at plane a & 3, offset a >> 2, as the CRTC shows it
   (Windows 9x's IO.SYS prints over its unchained boot logo). */
static u8 *m13_ptr(u32 lin)
{
    u32 a = lin - 0xA0000;
    return planar_on ? planes + (a & 3) * 65536 + ((a >> 2) & 0xFFFF) : gptr(lin);
}

static void put_pixel(int x, int y, u8 c)
{
    if (x < 0 || y < 0 || x >= gfx_w() || y >= gfx_h()) return;
    if (is_ega(video_mode)) {
        u32 a = (u32)(y * ega_bpr() + (x >> 3));
        u8 bit = (u8)(0x80 >> (x & 7));
        ega_dirty = 1;
        for (int p = 0; p < 4; p++) {
            u8 *b = &planes[p * 65536 + a];
            if (c & 0x80) { if (c & (1 << p)) *b ^= bit; }
            else if (c & (1 << p)) *b |= bit;
            else *b &= (u8)~bit;
        }
        return;
    }
    int bpp = gfx_bpp();
    if (!bpp) return;
    u32 a = gfx_row_addr(y);
    if (bpp == 8) { u8 *q = m13_ptr(a + x); *q = (c & 0x80) ? *q ^ (c & 0x7F) : c; return; }
    int per = 8 / bpp, sh = (per - 1 - x % per) * bpp;
    u8 m = (u8)(((1 << bpp) - 1) << sh), v = (u8)((c << sh) & m);
    a += x / per;
    u8 b = rd8(a);
    wr8(a, (c & 0x80) ? b ^ v : (u8)((b & ~m) | v));
}

static u8 get_pixel(int x, int y)
{
    if (x < 0 || y < 0 || x >= gfx_w() || y >= gfx_h()) return 0;
    if (is_ega(video_mode)) {
        u32 a = (u32)(y * ega_bpr() + (x >> 3));
        u8 v = 0;
        for (int p = 0; p < 4; p++) if (planes[p * 65536 + a] & (0x80 >> (x & 7))) v |= (u8)(1 << p);
        return v;
    }
    int bpp = gfx_bpp();
    if (!bpp) return 0;
    u32 a = gfx_row_addr(y);
    if (bpp == 8) return *m13_ptr(a + x);
    int per = 8 / bpp, sh = (per - 1 - x % per) * bpp;
    return (rd8(a + x / per) >> sh) & ((1 << bpp) - 1);
}

static int char_h(void) { return is_ega(video_mode) ? rd16(BDA + 0x85) : 8; }

static void gfx_char(int row, int col, u8 ch, u8 color)
{
    int bpp = is_ega(video_mode) ? 4 : gfx_bpp();
    if (!bpp) return;
    int h = char_h();
    /* The glyphs, as a BIOS finds them: CGA modes take 0-127 from the ROM
       and 128-255 from the table at INT 1Fh (programs such as Willy the Worm
       put their own there); the others use the table at INT 43h. Our
       mode set points INT 43h at the font of the mode's height. */
    const u8 *g;
    u32 v1f = LIN(rd16(0x1F * 4 + 2), rd16(0x1F * 4)), v43 = LIN(rd16(0x43 * 4 + 2), rd16(0x43 * 4));
    if (is_cga(video_mode)) g = ch < 128 ? font8x8 + ch * 8 : v1f ? gptr(v1f + (ch - 128) * 8) : font8x8 + ch * 8;
    else if (v43) g = gptr(v43 + ch * h);
    else g = h == 8 ? font8x8 + ch * 8 : font8x16 + ch * 16 + (h == 14 ? 1 : 0);
    u8 fg = bpp == 8 ? color : (u8)(color & ((1 << bpp) - 1)) | (color & 0x80);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < 8; x++) {
            int on = g[y] & (0x80 >> x);
            if (color & 0x80) { if (on) put_pixel(col * 8 + x, row * h + y, fg); }
            else put_pixel(col * 8 + x, row * h + y, on ? fg : 0);
        }
}

static void scroll(int up, int lines, u8 attr, int r0, int c0, int r1, int c1, int page)
{
    if (r1 >= text_rows) r1 = text_rows - 1;
    if (c1 >= text_cols) c1 = text_cols - 1;
    if (r0 > r1 || c0 > c1) return;
    int h = r1 - r0 + 1;
    if (lines == 0 || lines > h) lines = h;
    if (is_ega(video_mode)) {                      /* a character column is one byte per plane */
        int ch = char_h(), bpr = ega_bpr();
        ega_dirty = 1;
        for (int i = 0; i < h * ch; i++) {
            int dy = up ? r0 * ch + i : r1 * ch + ch - 1 - i;
            int sy = up ? dy + lines * ch : dy - lines * ch;
            int in = up ? sy <= r1 * ch + ch - 1 : sy >= r0 * ch;
            for (int p = 0; p < 4; p++) {
                u8 *d = planes + p * 65536 + dy * bpr + c0;
                if (in) memmove(d, planes + p * 65536 + sy * bpr + c0, c1 - c0 + 1);
                else memset(d, (attr & (1 << p)) ? 0xFF : 0, c1 - c0 + 1);
            }
        }
        return;
    }
    int bpp = gfx_bpp();
    if (bpp) {                                     /* a character column is bpp bytes wide */
        u8 fill = bpp == 8 ? attr : bpp == 2 ? (u8)((attr & 3) * 0x55) : (attr & 1) ? 0xFF : 0;
        for (int i = 0; i < h * 8; i++) {
            int dy = up ? r0 * 8 + i : r1 * 8 + 7 - i;
            int sy = up ? dy + lines * 8 : dy - lines * 8;
            int in = up ? sy <= r1 * 8 + 7 : sy >= r0 * 8;
            if (bpp == 8 && planar_on) {
                u32 da = gfx_row_addr(dy) + c0 * 8, sa = gfx_row_addr(sy) + c0 * 8;
                for (int k = 0; k < (c1 - c0 + 1) * 8; k++) *m13_ptr(da + k) = in ? *m13_ptr(sa + k) : fill;
                continue;
            }
            u8 *d = gptr(gfx_row_addr(dy) + c0 * bpp);
            if (in) memmove(d, gptr(gfx_row_addr(sy) + c0 * bpp), (c1 - c0 + 1) * bpp);
            else memset(d, fill, (c1 - c0 + 1) * bpp);
        }
        return;
    }
    if (!is_text(video_mode)) return;
    for (int i = 0; i < h; i++) {
        int dr = up ? r0 + i : r1 - i;
        int sr = up ? dr + lines : dr - lines;
        int in = up ? sr <= r1 : sr >= r0;
        for (int c = c0; c <= c1; c++)
            wr16(cell_addr(page, dr, c), in ? rd16(cell_addr(page, sr, c)) : (u16)(attr << 8 | ' '));
    }
}

static void set_cursor(int page, int row, int col)
{
    wr16(BDA + 0x50 + page * 2, (u16)(row << 8 | col));
    if (page == rd8(BDA + 0x62)) sync_cursor_crtc();
}

static void teletype(u8 ch, int page, u8 color)
{
    u16 pos = rd16(BDA + 0x50 + page * 2);
    int row = pos >> 8, col = pos & 0xFF;
    switch (ch) {
    case 7: return;
    case 8: if (col) col--; break;
    case 10: row++; break;
    case 13: col = 0; break;
    default:
        if (is_text(video_mode)) wr8(cell_addr(page, row, col), ch);
        else gfx_char(row, col, ch, color);
        col++;
    }
    if (col >= text_cols) { col = 0; row++; }
    if (row >= text_rows) {
        u8 attr = is_text(video_mode) ? rd8(cell_addr(page, text_rows - 1, 0) + 1) : 0;
        scroll(1, 1, attr, 0, 0, text_rows - 1, text_cols - 1, page);
        row = text_rows - 1;
    }
    set_cursor(page, row, col);
}

/* INT 10h AH=1Bh: functionality and state information (64 bytes at
   ES:DI), with the static functionality table at F000:F000. */
#define STATIC_FUNC 0xFF000u
static void state_info(u32 a)
{
    static const u8 sft[16] = { 0xFF, 0xE0, 0x0F, 0, 0, 0, 0, 0x07, 0x02, 0x08, 0xFF, 0x0E, 0, 0, 0, 0 };
    for (int i = 0; i < 16; i++) wr8(STATIC_FUNC + i, sft[i]);
    for (int i = 0; i < 64; i++) wr8(a + i, 0);
    wr16(a, STATIC_FUNC & 0xFFFF); wr16(a + 2, 0xF000);
    wr8(a + 4, (u8)video_mode);
    wr16(a + 5, (u16)text_cols);
    wr16(a + 7, rd16(BDA + 0x4C));
    wr16(a + 9, rd16(BDA + 0x4E));
    for (int i = 0; i < 16; i++) wr8(a + 0x0B + i, rd8(BDA + 0x50 + i));
    wr8(a + 0x1B, rd8(BDA + 0x61)); wr8(a + 0x1C, rd8(BDA + 0x60));
    wr8(a + 0x1D, rd8(BDA + 0x62));
    wr16(a + 0x1E, 0x3D4);
    wr8(a + 0x20, rd8(BDA + 0x65)); wr8(a + 0x21, rd8(BDA + 0x66));
    wr8(a + 0x22, (u8)text_rows);
    wr16(a + 0x23, rd16(BDA + 0x85) ? rd16(BDA + 0x85) : 16);
    wr8(a + 0x25, 0x08);                          /* VGA, colour */
    wr16(a + 0x27, video_mode == 0x13 ? 256 : is_cga(video_mode) ? (video_mode == 6 ? 2 : 4) : 16);
    wr8(a + 0x29, is_text(video_mode) ? 8 : 1);
    wr8(a + 0x2A, video_mode == 0x12 || video_mode == 0x11 ? 3 : video_mode == 0x10 || video_mode == 0x0F ? 1 : is_text(video_mode) ? 2 : 0);
    wr8(a + 0x2D, 0x20);                          /* blink */
    wr8(a + 0x31, 3);                             /* 256 KB */
}

/* INT 10h AH=1Ch: video state size, save, restore (CX bit 0 registers,
   bit 1 BIOS data, bit 2 DAC). Windows saves the screen state this way
   when it switches to and from DOS. */
static void video_state(struct regs *r)
{
    u16 m = CX(r) & 7;
    u32 hw = 4 + 8 + 16 + 32 + 21 + 8, bd = 0x6A - 0x49 + 8, dac = 4 + 768;
    if (AL(r) == 0) {
        u32 n = (m & 1 ? hw : 0) + (m & 2 ? bd : 0) + (m & 4 ? dac : 0);
        BX(r) = (u16)((n + 63) / 64);
        AL(r) = 0x1C;
        return;
    }
    if (AL(r) != 1 && AL(r) != 2) return;
    int save = AL(r) == 1;
    u32 a = LIN(r->v86_es, BX(r));
#define XFER(buf, n) do { for (u32 _i = 0; _i < (n); _i++) { if (save) wr8(a + _i, (buf)[_i]); else (buf)[_i] = rd8(a + _i); } a += (n); } while (0)
    if (m & 1) {
        u8 idx[4] = { seq_idx, gc_idx, crtc_idx, (u8)(atc_idx | atc_pas) };
        u8 mo[8] = { misc_out };
        XFER(idx, 4); XFER(mo, 8); XFER(gc, 16); XFER(crtc, 32); XFER(atc, 21); XFER(seq, 8);
        if (!save) {
            seq_idx = idx[0]; gc_idx = idx[1]; crtc_idx = idx[2]; atc_idx = idx[3] & 0x1F; atc_pas = idx[3] & 0x20;
            misc_out = mo[0];
            planar_check(); ega_check(); reg_mode_check(); apply_map_mask();
            palette_dirty = 1; full_redraw = 1;
        }
    }
    if (m & 2) {
        u8 b[0x6A - 0x49 + 8];
        for (u32 i = 0; i < 0x6A - 0x49; i++) b[i] = rd8(BDA + 0x49 + i);
        for (u32 i = 0; i < 8; i++) b[0x6A - 0x49 + i] = rd8(BDA + 0x84 + i);
        XFER(b, sizeof b);
        if (!save) {
            for (u32 i = 0; i < 0x6A - 0x49; i++) wr8(BDA + 0x49 + i, b[i]);
            for (u32 i = 0; i < 8; i++) wr8(BDA + 0x84 + i, b[0x6A - 0x49 + i]);
        }
    }
    if (m & 4) {
        u8 st[4] = { dac_mask, dac_widx, dac_ridx, 0 };
        XFER(st, 4); XFER(&vga_dac[0][0], 768);
        if (!save) { dac_mask = st[0]; dac_widx = st[1]; dac_ridx = st[2]; palette_dirty = 1; }
    }
#undef XFER
    AL(r) = 0x1C;
}

/* After an INT 10h mode set in VMHV: the register writes it made, for the
   INT 10h stub to do again as real OUTs from the guest (bios.asm), so that
   Windows' display VxD, which traps the VGA ports, sees the mode programmed.
   1: the stub is to continue (BIOS_CONT). */
static int mode_replay(void)
{
    u32 base = 0xF0000u + rd16(0xF0000u + 0x230);
    u32 t = base + 2, n = 0;
#define W(port, val) do { if (n < 160) { wr16(t + n * 3, (u16)(port)); wr8(t + n * 3 + 2, (u8)(val)); n++; } } while (0)
    u16 crt = video_mode == 7 ? 0x3B4 : 0x3D4;
    W(0x3C4, 0); W(0x3C5, 0x01);                                  /* sequencer in reset */
    for (int i = 1; i < 5; i++) { W(0x3C4, i); W(0x3C5, seq[i]); }
    W(0x3C2, misc_out);
    W(0x3C4, 0); W(0x3C5, 0x03);
    W(crt, 0x11); W(crt + 1, crtc[0x11] & 0x7F);                   /* CRTC 0-7 writable */
    for (int i = 0; i < 25; i++) { W(crt, i); W(crt + 1, crtc[i]); }
    W(0x8000 | (crt + 6), 0);                                     /* read 3DAh: attribute index next */
    for (int i = 0; i < 21; i++) { W(0x3C0, i); W(0x3C0, atc[i]); }
    W(0x3C0, 0x20);                                               /* palette back to the display */
    for (int i = 0; i < 9; i++) { W(0x3CE, i); W(0x3CF, gc[i]); }
    W(0x3CE, 0x0F);
#undef W
    wr16(base, (u16)n);
    return 1;
}

static int video_int10_(struct regs *r);
int video_int10(struct regs *r) { rlock_take(&video_lock); int v = video_int10_(r); rlock_drop(&video_lock); return v; }
static int video_int10_(struct regs *r)
{
    int cont = 0;
    if (vgalog < 0) vgalog = strstr(cmdline, "vgalog") != 0;
    if (vgalog) kprintf("INT 10h AX=%04x BX=%04x CX=%04x DX=%04x\n", AX(r), BX(r), CX(r), DX(r));

    int page = BH(r) & 7;
    switch (AH(r)) {
    case 0x00:
        if ((AL(r) & 0x7F) == 0x6A) planar_set(vbe_find(0x102), !(AL(r) & 0x80));   /* SVGA BIOSes' 800x600x16 */
        else if (!et_mode(AL(r) & 0x7F, !(AL(r) & 0x80))) video_set_mode(AL(r), !(AL(r) & 0x80));
        if (video_mode != 0x13) AL(r) = 0x30; else AL(r) = 0x20;
        if (hv_active) cont = mode_replay();
        break;
    case 0x01:
        wr16(BDA + 0x60, CX(r));
        crtc[0x0A] = CH(r) & 0x3F; crtc[0x0B] = CL(r) & 0x1F;
        if ((CH(r) & 0x1F) < 8 && (CL(r) & 0x1F) < 8 && !(CH(r) & 0x20)) {   /* CGA-style shape: scale to 16 lines */
            crtc[0x0A] = (CH(r) & 0x1F) * 2; crtc[0x0B] = (CL(r) & 0x1F) * 2 + 1;
            if ((CL(r) & 0x1F) == 7) crtc[0x0B] = 15;
        }
        break;
    case 0x02: set_cursor(page, DH(r), DL(r)); break;
    case 0x03: DX(r) = rd16(BDA + 0x50 + page * 2); CX(r) = rd16(BDA + 0x60); break;
    case 0x05: {
        int p = AL(r) & 7;
        wr8(BDA + 0x62, p);
        wr16(BDA + 0x4E, p * rd16(BDA + 0x4C));
        u16 start = (u16)(p * rd16(BDA + 0x4C) / 2);
        crtc[0x0C] = start >> 8; crtc[0x0D] = (u8)start;
        sync_cursor_crtc();
        full_redraw = 1;
        break; }
    case 0x06: case 0x07:
        scroll(AH(r) == 6, AL(r), BH(r), CH(r), CL(r), DH(r), DL(r), rd8(BDA + 0x62));
        break;
    case 0x08: {
        u16 pos = rd16(BDA + 0x50 + page * 2);
        if (is_text(video_mode)) AX(r) = rd16(cell_addr(page, pos >> 8, pos & 0xFF));
        else AX(r) = 0;
        break; }
    case 0x09: case 0x0A: {
        u16 pos = rd16(BDA + 0x50 + page * 2);
        int row = pos >> 8, col = pos & 0xFF;
        for (u32 i = 0; i < CX(r); i++, col++) {
            if (col >= text_cols) { col = 0; row++; }
            if (row >= text_rows) break;
            if (is_text(video_mode)) {
                u32 a = cell_addr(page, row, col);
                wr8(a, AL(r));
                if (AH(r) == 9) wr8(a + 1, BL(r));
            } else gfx_char(row, col, AL(r), BL(r));
        }
        break; }
    case 0x0B:                                    /* CGA palette: BH=0 background/border, 1 palette */
        if (BH(r) == 0) {
            if (is_cga(video_mode)) cga_color_select((u8)((cga_sel & 0x20) | (BL(r) & 0x1F)));
            else atc[0x11] = BL(r) & 15;
        } else if (BH(r) == 1 && is_cga(video_mode))
            cga_color_select((u8)((cga_sel & 0x1F) | ((BL(r) & 1) ? 0x20 : 0)));
        break;
    case 0x0C: put_pixel(CX(r), DX(r), AL(r)); break;
    case 0x0D: AL(r) = get_pixel(CX(r), DX(r)); break;
    case 0x0E: teletype(AL(r), rd8(BDA + 0x62), BL(r)); break;
    case 0x0F: AL(r) = (u8)video_mode | (rd8(BDA + 0x87) & 0x80); AH(r) = (u8)text_cols; BH(r) = rd8(BDA + 0x62); break;
    case 0x10:
        switch (AL(r)) {
        case 0x00: if (BL(r) < 16) atc[BL(r)] = BH(r); palette_dirty = 1; break;
        case 0x01: atc[0x11] = BH(r); break;
        case 0x02: for (int i = 0; i < 16; i++) atc[i] = rd8(LIN(r->v86_es, DX(r)) + i);
                   atc[0x11] = rd8(LIN(r->v86_es, DX(r)) + 16); palette_dirty = 1; break;
        case 0x03: if (BL(r)) atc[0x10] |= 8; else atc[0x10] &= ~8; full_redraw = 1; break;
        case 0x07: BH(r) = atc[BL(r) % 21]; break;
        case 0x08: BH(r) = atc[0x11]; break;
        case 0x09: for (int i = 0; i < 16; i++) wr8(LIN(r->v86_es, DX(r)) + i, atc[i]);
                   wr8(LIN(r->v86_es, DX(r)) + 16, atc[0x11]); break;
        case 0x10: vga_dac[BL(r)][0] = dac_in(DH(r)); vga_dac[BL(r)][1] = dac_in(CH(r)); vga_dac[BL(r)][2] = dac_in(CL(r));
                   palette_dirty = 1; break;
        case 0x12: {
            u32 a = LIN(r->v86_es, DX(r));
            for (u32 i = 0; i < CX(r) && BX(r) + i < 256; i++)
                for (int c = 0; c < 3; c++) vga_dac[BX(r) + i][c] = dac_in(rd8(a + i * 3 + c));
            palette_dirty = 1;
            break; }
        case 0x15: DH(r) = vga_dac[BL(r)][0]; CH(r) = vga_dac[BL(r)][1]; CL(r) = vga_dac[BL(r)][2]; break;
        case 0x17: {
            u32 a = LIN(r->v86_es, DX(r));
            for (u32 i = 0; i < CX(r) && BX(r) + i < 256; i++)
                for (int c = 0; c < 3; c++) wr8(a + i * 3 + c, vga_dac[BX(r) + i][c]);
            break; }
        case 0x1A: BX(r) = 0; break;
        }
        break;
    case 0x11:
        if (AL(r) == 0x30) {
            /* 2: 8x14, 3: 8x8, 4: 8x8 upper half, 6: 8x16; 5 and 7 (the 9-dot
               patch lists) point at a 0 byte: an empty list */
            static const u16 offs[8] = { 0, 0, 0xD000, 0xB000, 0xB400, 0xB000, 0xA000, 0xB000 };
            u8 which = BH(r) & 7;
            if (which == 0) { r->v86_es = rd16(0x1F * 4 + 2); BP(r) = rd16(0x1F * 4); }
            else if (which == 1) { r->v86_es = rd16(0x43 * 4 + 2); BP(r) = rd16(0x43 * 4); }
            else { r->v86_es = 0xF000; BP(r) = offs[which]; }
            CX(r) = rd16(BDA + 0x85);
            DL(r) = rd8(BDA + 0x84);
        } else dbg(1, "INT 10h AX=%04x (font) ignored\n", AX(r));
        break;
    case 0x12:
        if (BL(r) == 0x10) { BH(r) = 0; BL(r) = 3; CX(r) = 0x0009; }
        else AL(r) = 0x12;
        break;
    case 0x13: {
        u32 s = LIN(r->v86_es, BP(r));
        u16 save = rd16(BDA + 0x50 + page * 2);
        set_cursor(page, DH(r), DL(r));
        for (u32 i = 0; i < CX(r); i++) {
            u8 ch = rd8(s++), at = BL(r);
            if (AL(r) & 2) at = rd8(s++);
            u16 pos = rd16(BDA + 0x50 + page * 2);
            if (ch >= 32 && is_text(video_mode)) wr8(cell_addr(page, pos >> 8, pos & 0xFF) + 1, at);
            teletype(ch, page, at);
        }
        if (!(AL(r) & 1)) set_cursor(page, save >> 8, save & 0xFF);
        break; }
    case 0x1A:
        if (AL(r) == 0) { AL(r) = 0x1A; BX(r) = 0x0008; }
        break;
    case 0x1B: if (BX(r) == 0) { state_info(LIN(r->v86_es, DI(r))); AL(r) = 0x1B; } break;
    case 0x1C: video_state(r); break;
    case 0x4F: vbe_call(r); break;             /* VESA BIOS */
    default:
        dbg(1, "INT 10h AX=%04x unsupported\n", AX(r));
    }
    return cont;
}

/* Text for the user from the monitor (e.g. why a DPMI client was stopped). */
static void video_puts_(const char *s);
void video_puts(const char *s) { rlock_take(&video_lock); video_puts_(s); rlock_drop(&video_lock); }
static void video_puts_(const char *s)
{
    if (vga_native) video_native_sync();
    if (!is_text(video_mode)) { if (vga_native) return; video_set_mode(3, 1); }
    while (*s) teletype((u8)*s++, rd8(BDA + 0x62), 7);
}
