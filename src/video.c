/* The guest's display: VGA text (B800h), CGA graphics (modes 4-6, B800h) and
   mode 13h (A000h) kept in guest memory, drawn onto the boot framebuffer
   (GRUB / GOP / Bochs VBE), plus the VGA registers and INT 10h. */
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

static void put_row(const u32 *px, u32 n, u32 x, u32 y)
{
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
u8 crtc[32];
int palette_dirty = 1;
static u8 crtc_idx, seq_idx, gc_idx, atc_idx, atc_flip, misc_out = 0x67;
static u8 seq[8], gc[16], atc[21];
static u8 dac_widx, dac_wc, dac_ridx, dac_rc, dac_mask = 0xFF;
static u32 pal[256];

/* VESA BIOS state (see the VBE section) */
static int vbe_on;
static u32 vbe_w, vbe_h, vbe_bpp, vbe_pitch, vbe_start;
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

/* Graphics mode size in pixels. */
static int gfx_w(void) { return video_mode == 6 || video_mode == 0x0E || video_mode == 0x10 || video_mode == 0x12 ? 640 : 320; }
static int gfx_h(void) { return video_mode == 0x10 ? 350 : video_mode == 0x12 ? 480 : 200; }
int video_gfx_height(void) { return vbe_on ? (int)vbe_h : is_text(video_mode) ? 200 : gfx_h(); }

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
void video_set_mode(int mode, int clear)
{
    mode &= 0x7F;
    vbe_off();
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
    wr8(BDA + 0x87, (rd8(BDA + 0x87) & 0x7F) | (clear ? 0 : 0x80) | 0x60);
    wr8(BDA + 0x88, 0x09);
    wr8(BDA + 0x89, 0x11);
    memset(crtc, 0, sizeof crtc);
    memset(gc, 0, sizeof gc);
    gc[6] = is_text(mode) || is_cga(mode) ? 0x0E : 0x05;   /* memory map: B800h / A000h 64K */
    gc[7] = 0x0F; gc[8] = 0xFF;                    /* color don't care, bit mask */
    seq[2] = 0x0F;                                 /* all planes */
    seq[4] = mode == 0x13 ? 0x0E : is_ega(mode) ? 0x06 : 0x02;   /* chain-4 in mode 13h */
    crtc[0x13] = text_cols == 40 && (is_text(mode) || mode == 0x0D) ? 0x14 : 0x28;
    crtc[0x18] = 0xFF; crtc[0x07] = 0x10; crtc[0x09] = 0x40;    /* line compare off */
    planar_check();
    ega_check();
    crtc[0x0A] = 0x0D; crtc[0x0B] = 0x0E;
    if (clear) {
        if (is_text(mode)) for (u32 i = 0; i < 0x4000; i++) wr16(text_base + i * 2, 0x0720);
        else if (is_cga(mode)) memset(gptr(0xB8000), 0, 0x8000);
        else if (is_ega(mode)) memset(planes, 0, 4 * 65536);
        else memset(gptr(0xA0000), 0, 0x10000);
    }
    default_palette(mode == 0x13 ? 1 : is_cga(mode) || mode == 0x0D || mode == 0x0E ? 2 : 0);
    if (is_cga(mode)) cga_color_select(mode == 6 ? 0x3F : 0x30);
    full_redraw = 1;
    last_layout_mode = -1;
    if (!is_text(mode) && !is_cga(mode) && !is_ega(mode) && mode != 0x13) kprintf("video mode %02x: not drawn yet\n", mode);
    else dbg(1, "video mode %02x\n", mode);
}

/* ---------------- drawing ---------------- */
static u16 shadow_text[132 * 60];
static u8 shadow_gfx[640 * 480];
static int sh_cursor = -1, blink_phase;
static u32 frame_no;
static u32 g_x, g_y, g_w, g_h;                 /* the picture's box on the screen: 4:3, or all of it (aspect=fill) */
static u16 xmap[4096], txmap[4096];
static int aspect_fill = -1;
static u16 console_cells[80 * 25];
static int console_on;

static u32 text_pixels_w(void) { return text_cols == 40 ? 640 : (u32)text_cols * 8; }

static void layout(void)
{
    if (last_layout_mode == video_mode) return;
    last_layout_mode = video_mode;
    full_redraw = 1;
    clear_fb();
    if (aspect_fill < 0) aspect_fill = strstr(cmdline, "aspect=fill") != 0;
    g_h = fb_h; g_w = fb_h * 4 / 3;
    if (g_w > fb_w || aspect_fill) { g_w = fb_w; g_h = aspect_fill ? fb_h : fb_w * 3 / 4; }
    if (g_w > 4096) g_w = 4096;
    g_x = (fb_w - g_w) / 2; g_y = (fb_h - g_h) / 2;
    u32 w = vbe_on ? vbe_w : is_text(video_mode) ? 320 : (u32)gfx_w();
    for (u32 x = 0; x < g_w; x++) xmap[x] = (u16)(x * w / g_w);
    u32 tw = text_pixels_w();
    for (u32 x = 0; x < g_w; x++) txmap[x] = (u16)(x * tw / g_w);
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

static void map_window_flags(u32 phys, u32 flags)
{
    for (u32 i = 0; i < 16; i++) map_page(0xA0000 + i * 4096, phys + i * 4096, flags);
    tlb_flush();
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

static void planar_check(void)
{
    int want = video_mode == 0x13 && !(seq[4] & 8);
    if (want && !planar_on) {
        if (!planes) planes = phys_alloc(4 * 65536);
        /* what the chained screen showed: byte a went to plane a & 3, offset a >> 2 */
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
}

int vga16_window(u32 lin) { return (ega_on || modex_trap || (planar_on && mapped_ro)) && lin >= 0xA0000 && lin < 0xB0000; }

/* Unchained 256-colour: emulate unless writes go plainly to one plane. */
static void modex_check(void)
{
    u8 m = seq[2] & 15;
    int want = planar_on && ((m & (m - 1)) || !m || (gc[5] & 0x0B) || (gc[1] & 15) || (gc[3] & 0x1F) ||
                             gc[8] != 0xFF);
    if (want && !modex_trap) {
        for (u32 i = 0; i < 16; i++) map_page(0xA0000 + i * 4096, 0, 0);
        tlb_flush();
        modex_trap = 1;
        mapped_plane = -1;
        mapped_ro = 0;
    } else if (!want && modex_trap) modex_trap = 0;   /* apply_map_mask maps the window */
}

u8 vga16_read(u32 lin)
{
    u32 o = (lin - 0xA0000) & 0xFFFF;
    for (int p = 0; p < 4; p++) latch[p] = planes[p * 65536 + o];
    if (!(gc[5] & 8)) return latch[gc[4] & 3];
    u8 r = 0;                                      /* read mode 1: colour compare */
    for (int p = 0; p < 4; p++)
        if (gc[7] & (1 << p)) r |= latch[p] ^ ((gc[2] & (1 << p)) ? 0xFF : 0);
    return (u8)~r;
}

void vga16_write(u32 lin, u8 v)
{
    u32 o = (lin - 0xA0000) & 0xFFFF;
    int mode = gc[5] & 3, rot = gc[3] & 7, fn = (gc[3] >> 3) & 3;
    u8 mask = gc[8], mm = seq[2] & 15;
    ega_dirty = 1;
    if (rot) v = (u8)((v >> rot) | (v << (8 - rot)));
    if (mode == 3) { mask &= v; }
    for (int p = 0; p < 4; p++) {
        if (!(mm & (1 << p))) continue;
        u8 d;
        if (mode == 1) { planes[p * 65536 + o] = latch[p]; continue; }
        if (mode == 0) d = (gc[1] & (1 << p)) ? ((gc[0] & (1 << p)) ? 0xFF : 0) : v;
        else if (mode == 2) d = (v & (1 << p)) ? 0xFF : 0;
        else d = (gc[0] & (1 << p)) ? 0xFF : 0;
        switch (fn) {
        case 1: d &= latch[p]; break;
        case 2: d |= latch[p]; break;
        case 3: d ^= latch[p]; break;
        }
        planes[p * 65536 + o] = (u8)((d & mask) | (latch[p] & ~mask));
    }
}

/* A fault on the window: emulate the instruction. */
int vga16_fault(struct emu_cpu *e, u32 cr2)
{
    if (!vga16_window(cr2)) return 0;
    if (mem_emulate(e)) return 1;
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
    if (video_mode == 0x0D || video_mode == 0x0E) lc >>= 1;   /* double-scanned 200-line modes */
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
        if (planar_on) {
            u32 start = (u32)(crtc[0x0C] << 8 | crtc[0x0D]), pitch = crtc[0x13] ? crtc[0x13] * 2u : 80;
            u32 o = start + y * pitch;
            for (int x = 0; x < 320; x++) out[x] = planes[(x & 3) * 65536 + ((o + (x >> 2)) & 0xFFFF)];
        } else memcpy(out, gptr(0xA0000 + y * 320), 320);
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

static void refresh_gfx(void)
{
    static u32 line[4096];
    static u8 ovr[640];
    static u8 rowbuf[640];
    int w = gfx_w(), h = gfx_h();
    int px = 0, py = -100;
    u16 ma, mxr;
    if (mouse_pointer(&px, &py, &ma, &mxr)) { if (w == 320) px /= 2; } else py = -100;
    int moved = px != sh_ptr_x || py != sh_ptr_y;
    if (is_ega(video_mode)) {                      /* nothing new in the planes or registers */
        static u8 last_regs[32 + 21];
        int regs_changed = memcmp(last_regs, crtc, 32) || memcmp(last_regs + 32, atc, 21);
        if (!ega_dirty && !full_redraw && !moved && !regs_changed) return;
        memcpy(last_regs, crtc, 32); memcpy(last_regs + 32, atc, 21);
        ega_dirty = 0;
    }
    u32 black = pack(0, 0, 0), white = pack(63, 63, 63);
    for (int y = 0; y < h; y++) {
        fetch_row(y, rowbuf);
        u8 *sh = shadow_gfx + y * w;
        int in_new = y >= py && y < py + 16, in_old = y >= sh_ptr_y && y < sh_ptr_y + 16;
        if (!full_redraw && !(moved && (in_new || in_old)) && !memcmp(rowbuf, sh, w)) continue;
        memcpy(sh, rowbuf, w);
        for (u32 x = 0; x < g_w; x++) line[x] = pal[rowbuf[xmap[x]]];
        if (in_new) {
            memset(ovr, 0, sizeof ovr);
            const char *a = arrow[y - py];
            for (int i = 0; a[i] && px + i < w; i++) ovr[px + i] = a[i] == '1' ? 1 : a[i] == '2' ? 2 : 0;
            for (u32 x = 0; x < g_w; x++)
                if (ovr[xmap[x]]) line[x] = ovr[xmap[x]] == 1 ? black : white;
        }
        u32 y0 = g_y + y * g_h / h, y1 = g_y + (y + 1) * g_h / h;
        if (y1 == y0) continue;
        for (u32 yy = y0; yy < y1; yy++) put_row(line, g_w, g_x, yy);   /* from RAM: never read the framebuffer */
    }
    sh_ptr_x = px; sh_ptr_y = py;
}

static void refresh_fallback(void)
{
    if (!is_text(video_mode) && !console_on) return;
    u16 *dst = phys_low(0xB8000);
    const u16 *src = console_on ? console_cells : (const u16 *)gptr(text_base + (crtc[0x0C] << 8 | crtc[0x0D]) * 2);
    memcpy(dst, src, 80 * 25 * 2);
    u16 off = console_on ? 0xFFFF : (u16)((crtc[0x0E] << 8 | crtc[0x0F]) - (crtc[0x0C] << 8 | crtc[0x0D]));
    outb(0x3D4, 0x0E); outb(0x3D5, off >> 8);
    outb(0x3D4, 0x0F); outb(0x3D5, (u8)off);
}


/* ---------------- VESA BIOS (VBE 2.0) ----------------
   4 MB of video memory in kernel RAM, identity-mapped and open to the
   guest: protected-mode programs use it as the linear framebuffer at its
   real address (DPMI 0800h hands that back), real-mode ones through the
   A000h window, which function 05h (or the WinFuncPtr far call) maps onto
   a 64 KB bank. 8-bit modes use the VGA DAC; 15/16/32-bit ones are direct
   colour. Tables live in the BIOS segment at F000:C000. */
#define VRAM_SIZE (4u << 20)
#define VBE_DATA 0xFC000u
static u8 *vram, *vshadow;
static u32 vbe_bank;
static u16 vbe_cur;
static const struct { u16 no, w, h; u8 bpp; } vmodes[] = {
    { 0x100, 640, 400, 8 }, { 0x101, 640, 480, 8 }, { 0x103, 800, 600, 8 }, { 0x105, 1024, 768, 8 },
    { 0x110, 640, 480, 15 }, { 0x111, 640, 480, 16 }, { 0x112, 640, 480, 32 },
    { 0x113, 800, 600, 15 }, { 0x114, 800, 600, 16 }, { 0x115, 800, 600, 32 },
    { 0x116, 1024, 768, 15 }, { 0x117, 1024, 768, 16 }, { 0x118, 1024, 768, 32 },
};
#define N_VMODES (int)(sizeof vmodes / sizeof vmodes[0])

static int vbe_bytespp(u32 bpp) { return (bpp + 7) / 8; }

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
int video_vram_range(u32 p, u32 n) { return vram && p >= (u32)(uintptr_t)vram && p + n <= (u32)(uintptr_t)vram + VRAM_SIZE; }

static void vbe_off(void)
{
    if (!vbe_on) return;
    vbe_on = 0;
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
    u32 bpp = vmodes[m].bpp, B = vbe_bytespp(bpp), pitch = vmodes[m].w * B;
    wr16(a + 0x00, 0x9F);                       /* supported, TTY, colour, graphics, LFB */
    wr8(a + 0x02, 7);                           /* window A: exists, readable, writable */
    wr16(a + 0x04, 64); wr16(a + 0x06, 64);     /* granularity, size (KB) */
    wr16(a + 0x08, 0xA000);
    wr16(a + 0x0C, rd16(0xF0226)); wr16(a + 0x0E, 0xF000);   /* WinFuncPtr */
    wr16(a + 0x10, (u16)pitch);
    wr16(a + 0x12, vmodes[m].w); wr16(a + 0x14, vmodes[m].h);
    wr8(a + 0x16, 8); wr8(a + 0x17, 16); wr8(a + 0x18, 1);
    wr8(a + 0x19, (u8)bpp); wr8(a + 0x1A, 1);
    wr8(a + 0x1B, bpp == 8 ? 4 : 6);            /* packed pixel / direct colour */
    u32 pages = VRAM_SIZE / (pitch * vmodes[m].h);
    wr8(a + 0x1D, (u8)(pages > 256 ? 255 : pages - 1));
    wr8(a + 0x1E, 1);
    static const u8 masks[3][8] = { { 5, 10, 5, 5, 5, 0, 1, 15 }, { 5, 11, 6, 5, 5, 0, 0, 0 }, { 8, 16, 8, 8, 8, 0, 8, 24 } };
    if (bpp != 8) memcpy(gptr(a + 0x1F), masks[bpp == 15 ? 0 : bpp == 16 ? 1 : 2], 8);
    wr32(a + 0x28, (u32)(uintptr_t)vram);       /* PhysBasePtr */
    wr16(a + 0x32, (u16)pitch);                 /* VBE 3: linear bytes per line */
}

static void vbe_set(int m, int clear)
{
    video_set_mode(0x13, 0);                    /* VGA state, default 256-colour palette */
    video_mode = vmodes[m].no;
    vbe_cur = vmodes[m].no;
    vbe_w = vmodes[m].w; vbe_h = vmodes[m].h; vbe_bpp = vmodes[m].bpp;
    vbe_pitch = vbe_w * vbe_bytespp(vbe_bpp);
    vbe_start = 0;
    vbe_on = 1;
    if (clear) memset(vram, 0, VRAM_SIZE);
    vbe_map_bank(0);
    full_redraw = 1;
    last_layout_mode = -1;
    kprintf("video: VESA mode %x, %ux%u, %u bpp\n", vbe_cur, vbe_w, vbe_h, vbe_bpp);
}

/* BH=0: set window BL to bank DX; BH=1: get it (INT 10h 4F05h and WinFuncPtr). */
void video_vbe_window(struct regs *r)
{
    if (BL(r) != 0) { AX(r) = 0x014F; return; }
    if (BH(r) == 1) { DX(r) = (u16)vbe_bank; AX(r) = 0x004F; return; }
    if ((u32)DX(r) * 65536 >= VRAM_SIZE) { AX(r) = 0x014F; return; }
    if (vbe_on) vbe_map_bank(DX(r)); else vbe_bank = DX(r);
    AX(r) = 0x004F;
}

/* The protected-mode interface's routines (4F0Ah): 47h set window,
   48h set display start (CX:DX = byte offset / 4), 49h set palette. */
void video_vbe_pm(int id, u32 ebx, u32 ecx, u32 edx, u32 pal_lin)
{
    if (id == 0x47) {
        u32 b = edx & 0xFFFF;
        if ((ebx & 0xFF) == 0 && b * 65536 < VRAM_SIZE) { if (vbe_on) vbe_map_bank(b); else vbe_bank = b; }
    } else if (id == 0x48) {
        u32 st = ((edx & 0xFFFF) << 16 | (ecx & 0xFFFF)) << 2;
        if (vbe_on && st + vbe_pitch * vbe_h <= VRAM_SIZE) vbe_start = st;
    } else if (id == 0x49 && pal_lin) {
        u32 n = ecx & 0xFFFF, first = edx & 0xFFFF;
        for (u32 i = 0; i < n && first + i < 256; i++) {
            vga_dac[first + i][0] = rd8(pal_lin + i * 4 + 2) & 63;
            vga_dac[first + i][1] = rd8(pal_lin + i * 4 + 1) & 63;
            vga_dac[first + i][2] = rd8(pal_lin + i * 4) & 63;
        }
        palette_dirty = 1;
    }
}

static void vbe_call(struct regs *r)
{
    u32 es_di = LIN(r->v86_es, DI(r));
    if ((AL(r) <= 1 || AL(r) == 9) && dpmi_reflected_buffer(r, &es_di))
        dbg(1, "VESA %02x from protected mode: buffer at %x\n", AL(r), es_di);
    vbe_tables();
    switch (AL(r)) {
    case 0x00: {
        int v2 = rd32(es_di) == 0x32454256u;     /* "VBE2" */
        memset(gptr(es_di + 4), 0, v2 ? 508 : 252);
        wr32(es_di, 0x41534556u);                /* "VESA" */
        wr16(es_di + 4, 0x0200);
        wr32(es_di + 6, 0xF0000000u | (VBE_DATA & 0xFFFF));
        wr32(es_di + 0x0A, 0);
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
        vbe_set(m, !(bx & 0x8000));
        AX(r) = 0x004F; return; }
    case 0x03: BX(r) = vbe_on ? vbe_cur : (u16)video_mode; AX(r) = 0x004F; return;
    case 0x05: video_vbe_window(r); return;
    case 0x06: {
        if (!vbe_on) { AX(r) = 0x014F; return; }
        u32 B = vbe_bytespp(vbe_bpp);
        if (BL(r) == 0 || BL(r) == 2) {
            u32 p = BL(r) == 0 ? CX(r) * B : (CX(r) + B - 1) / B * B;
            if (p < vbe_w * B || p * vbe_h > VRAM_SIZE) { AX(r) = 0x024F; return; }
            vbe_pitch = p;
            full_redraw = 1;
        }
        u32 pitch = BL(r) == 3 ? (VRAM_SIZE / vbe_h) / B * B : vbe_pitch;
        if (pitch > 0x7FFF) pitch = 0x7FFF / B * B;
        BX(r) = (u16)pitch; CX(r) = (u16)(pitch / B);
        DX(r) = (u16)(VRAM_SIZE / pitch > 0xFFFF ? 0xFFFF : VRAM_SIZE / pitch);
        AX(r) = 0x004F; return; }
    case 0x07: {
        if (!vbe_on) { AX(r) = 0x014F; return; }
        u32 B = vbe_bytespp(vbe_bpp);
        if ((BL(r) & 0x7F) == 0) {
            u32 st = DX(r) * vbe_pitch + CX(r) * B;
            if (st + vbe_pitch * vbe_h > VRAM_SIZE) { AX(r) = 0x014F; return; }
            vbe_start = st;
        } else if (BL(r) == 1) {
            BH(r) = 0; CX(r) = (u16)(vbe_start % vbe_pitch / B); DX(r) = (u16)(vbe_start / vbe_pitch);
        }
        AX(r) = 0x004F; return; }
    case 0x08:
        if (BL(r) <= 1) { BH(r) = 6; AX(r) = 0x004F; } else AX(r) = 0x014F;   /* 6-bit DAC only */
        return;
    case 0x0A:                                   /* protected-mode interface (bios.asm vbe_pmi) */
        if (BL(r) != 0) { AX(r) = 0x014F; return; }
        r->v86_es = 0xF000; DI(r) = rd16(0xF0228); CX(r) = rd16(0xF022A);
        AX(r) = 0x004F;
        return;
    case 0x09: {
        u32 n = CX(r), first = DX(r);
        if ((BL(r) & 0x7F) == 0)
            for (u32 i = 0; i < n && first + i < 256; i++) {
                vga_dac[first + i][0] = rd8(es_di + i * 4 + 2) & 63;
                vga_dac[first + i][1] = rd8(es_di + i * 4 + 1) & 63;
                vga_dac[first + i][2] = rd8(es_di + i * 4) & 63;
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

static void refresh_vbe(void)
{
    static u32 line[4096];
    static u32 cvt[4096];
    u32 w = vbe_w, h = vbe_h, B = vbe_bytespp(vbe_bpp), rowb = w * B;
    int px = 0, py = -100;
    u16 ma, mxr;
    if (!mouse_pointer(&px, &py, &ma, &mxr)) py = -100;
    int moved = px != sh_ptr_x || py != sh_ptr_y;
    u32 black = pack(0, 0, 0), white = pack(63, 63, 63);
    for (u32 y = 0; y < h; y++) {
        u32 off = vbe_start + y * vbe_pitch;
        if (off + rowb > VRAM_SIZE) break;
        const u8 *src = vram + off;
        u8 *sh = vshadow + y * rowb;
        int in_new = (int)y >= py && (int)y < py + 16, in_old = (int)y >= sh_ptr_y && (int)y < sh_ptr_y + 16;
        if (!full_redraw && !(moved && (in_new || in_old)) && !memcmp(src, sh, rowb)) continue;
        memcpy(sh, src, rowb);
        u32 y0 = g_y + y * g_h / h, y1 = g_y + (y + 1) * g_h / h;
        if (y1 == y0) continue;
        for (u32 x = 0; x < w; x++) {
            u32 c;
            if (B == 1) c = pal[src[x]];
            else if (B == 2) {
                u32 v = ((const u16 *)src)[x];
                if (vbe_bpp == 15) c = pack8((v >> 7) & 0xF8, (v >> 2) & 0xF8, (v << 3) & 0xF8);
                else c = pack8((v >> 8) & 0xF8, (v >> 3) & 0xFC, (v << 3) & 0xF8);
            } else {
                u32 v = ((const u32 *)src)[x];
                c = pack8((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
            }
            cvt[x] = c;
        }
        if (in_new) {
            const char *a = arrow[y - py];
            for (int i = 0; a[i] && px + i < (int)w; i++)
                if (a[i] == '1') cvt[px + i] = black; else if (a[i] == '2') cvt[px + i] = white;
        }
        for (u32 x = 0; x < g_w; x++) line[x] = cvt[xmap[x]];
        for (u32 yy = y0; yy < y1; yy++) put_row(line, g_w, g_x, yy);
    }
    sh_ptr_x = px; sh_ptr_y = py;
}

/* ---------------- on-screen note (speed changes) ---------------- */
static char osd_msg[32];
static u32 osd_until;

void video_osd(const char *msg)
{
    int i = 0;
    for (; msg[i] && i < 31; i++) osd_msg[i] = msg[i];
    osd_msg[i] = 0;
    osd_until = ticks + 2000;
}

static void draw_osd(void)
{
    static u32 line[40 * 8 * 2];
    if (!osd_msg[0]) return;
    if ((int32_t)(ticks - osd_until) >= 0) {        /* expired: redraw what it covered */
        osd_msg[0] = 0;
        full_redraw = 1;
        last_layout_mode = -1;
        return;
    }
    int n = 0;
    while (osd_msg[n]) n++;
    int s = fb_w >= 1280 ? 2 : 1, w = (n + 2) * 8 * s;
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

void video_refresh(void)
{
    static int busy;
    if (busy) return;
    busy = 1;
    frame_no++;
    if ((frame_no & 15) == 0) blink_phase ^= 1;
    if (text_fallback) { refresh_fallback(); busy = 0; return; }
    if (!fb) { busy = 0; return; }
    layout();
    if (palette_dirty) {
        for (int i = 0; i < 256; i++) pal[i] = pack(vga_dac[i][0], vga_dac[i][1], vga_dac[i][2]);
        palette_dirty = 0;
        full_redraw = 1;
    }
    if (console_on) { refresh_text(console_cells, -1); }
    else if (is_text(video_mode)) {
        u16 start = crtc[0x0C] << 8 | crtc[0x0D];
        u16 cur = (u16)((crtc[0x0E] << 8 | crtc[0x0F]) - start);
        refresh_text((const u16 *)gptr(text_base + start * 2), cur);
    } else if (vbe_on) refresh_vbe();
    else if (video_mode == 0x13 || is_cga(video_mode) || is_ega(video_mode)) refresh_gfx();
    full_redraw = 0;
    draw_osd();
    busy = 0;
}

void video_console(const char *msg)
{
    for (int i = 0; i < 80 * 25; i++) console_cells[i] = 0x4F20;
    int row = 1, col = 2;
    const char *t = "vmdos stopped";
    for (int i = 0; t[i]; i++) console_cells[row * 80 + col + i] = 0x4F00 | (u8)t[i];
    row = 3;
    for (; *msg && row < 24; msg++) {
        if (*msg == '\n' || col >= 78) { row++; col = 2; if (*msg == '\n') continue; }
        console_cells[row * 80 + col++] = 0x4F00 | (u8)*msg;
    }
    console_on = 1;
    if (!is_text(video_mode)) { video_mode = 3; text_cols = 80; }
    default_palette(0);
    last_layout_mode = -1;
    video_refresh();
}

void video_text_fallback(void) { text_fallback = 1; }

void video_init(void)
{
    vbe_init();
    if (!fb && !bochs_vbe()) {
        kprintf("no linear framebuffer: using VGA text mode\n");
        text_fallback = 1;
    }
}

/* ---------------- VGA ports ---------------- */
u32 video_port_in(u16 port)
{
    switch (port) {
    case 0x3C0: return atc_idx;
    case 0x3C1: return atc[atc_idx % 21];
    case 0x3C2: return 0x10;
    case 0x3D9: return cga_sel;
    case 0x3C4: return seq_idx;
    case 0x3C5: return seq[seq_idx & 7];
    case 0x3C6: return dac_mask;
    case 0x3C7: return 3;
    case 0x3C8: return dac_widx;
    case 0x3C9: {
        u8 v = vga_dac[dac_ridx][dac_rc];
        if (++dac_rc == 3) { dac_rc = 0; dac_ridx++; }
        return v; }
    case 0x3CA: return 0;
    case 0x3CC: return misc_out;
    case 0x3CE: return gc_idx;
    case 0x3CF: return gc[gc_idx & 15];
    case 0x3B4: case 0x3D4: return crtc_idx;
    case 0x3B5: case 0x3D5: return crtc[crtc_idx & 31];
    case 0x3BA: case 0x3DA: {
        atc_flip = 0;
        u32 t = vpit_clock();
        u8 v = 0;
        if (t % 17045 < 1100) v |= 0x09;          /* vertical retrace (70 Hz) */
        else if (t % 38 < 8) v |= 0x01;           /* horizontal blanking */
        return v; }
    }
    return 0xFF;
}

void video_port_out(u16 port, u8 v)
{
    switch (port) {
    case 0x3C0:
        if (!atc_flip) atc_idx = v & 0x1F;
        else if (atc_idx < 21) { atc[atc_idx] = v; palette_dirty = 1; }
        atc_flip ^= 1;
        return;
    case 0x3C2: misc_out = v; return;
    case 0x3D9: cga_color_select(v); return;           /* CGA colour select */
    case 0x3C4: seq_idx = v; return;
    case 0x3C5:
        seq[seq_idx & 7] = v;
        if ((seq_idx & 7) == 4) planar_check();
        else if ((seq_idx & 7) == 2) { readmap_last = 0; apply_map_mask(); }
        return;
    case 0x3C6: dac_mask = v; return;
    case 0x3C7: dac_ridx = v; dac_rc = 0; return;
    case 0x3C8: dac_widx = v; dac_wc = 0; return;
    case 0x3C9:
        vga_dac[dac_widx][dac_wc] = v & 63;
        if (++dac_wc == 3) { dac_wc = 0; dac_widx++; }
        palette_dirty = 1;
        return;
    case 0x3CE: gc_idx = v; return;
    case 0x3CF:
        gc[gc_idx & 15] = v;
        if ((gc_idx & 15) == 4) readmap_last = 1;
        apply_map_mask();
        return;
    case 0x3B4: case 0x3D4: crtc_idx = v; return;
    case 0x3B5: case 0x3D5:
        crtc[crtc_idx & 31] = v;
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
    if (bpp == 8) { wr8(a + x, (c & 0x80) ? rd8(a + x) ^ (c & 0x7F) : c); return; }
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
    if (bpp == 8) return rd8(a + x);
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
       default INT 43h table is 8x8, so the 14/16-line modes use the
       built-in 8x16 font unless a program pointed INT 43h elsewhere. */
    const u8 *g;
    u32 v1f = LIN(rd16(0x1F * 4 + 2), rd16(0x1F * 4)), v43 = LIN(rd16(0x43 * 4 + 2), rd16(0x43 * 4));
    if (is_cga(video_mode)) g = ch < 128 ? font8x8 + ch * 8 : v1f ? gptr(v1f + (ch - 128) * 8) : font8x8 + ch * 8;
    else if (v43 && (h == 8 || v43 != 0xFB000)) g = gptr(v43 + ch * h);
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

void video_int10(struct regs *r)
{
    int page = BH(r) & 7;
    switch (AH(r)) {
    case 0x00: video_set_mode(AL(r), !(AL(r) & 0x80)); if (video_mode != 0x13) AL(r) = 0x30; else AL(r) = 0x20; break;
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
        case 0x10: vga_dac[BL(r)][0] = DH(r) & 63; vga_dac[BL(r)][1] = CH(r) & 63; vga_dac[BL(r)][2] = CL(r) & 63;
                   palette_dirty = 1; break;
        case 0x12: {
            u32 a = LIN(r->v86_es, DX(r));
            for (u32 i = 0; i < CX(r) && BX(r) + i < 256; i++)
                for (int c = 0; c < 3; c++) vga_dac[BX(r) + i][c] = rd8(a + i * 3 + c) & 63;
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
            static const u16 offs[8] = { 0, 0, 0xA000, 0xB000, 0xB400, 0xA000, 0xA000, 0xA000 };
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
    case 0x4F: vbe_call(r); break;             /* VESA BIOS */
    default:
        dbg(1, "INT 10h AX=%04x unsupported\n", AX(r));
    }
}

/* Text for the user from the monitor (e.g. why a DPMI client was stopped). */
void video_puts(const char *s)
{
    if (!is_text(video_mode)) video_set_mode(3, 1);
    while (*s) teletype((u8)*s++, rd8(BDA + 0x62), 7);
}
