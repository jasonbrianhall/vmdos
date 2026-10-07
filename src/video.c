/* The guest's display: VGA text (B800h) and mode 13h (A000h) kept in guest
   memory, drawn onto the boot framebuffer (GRUB / GOP / Bochs VBE), plus the
   VGA registers and INT 10h. */
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

static void copy_row(u32 x, u32 n, u32 ysrc, u32 ydst)
{
    if (ydst >= fb_h || x >= fb_w) return;
    if (x + n > fb_w) n = fb_w - x;
    u32 b = (fb_bpp + 7) / 8;
    memcpy(fb + ydst * fb_pitch + x * b, fb + ysrc * fb_pitch + x * b, n * b);
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
    map_mmio(lfb, W * H * 4);
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
static int full_redraw = 1, last_layout_mode = -1;

static const u8 ega_atc[16] = { 0, 1, 2, 3, 4, 5, 0x14, 7, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F };
static const u8 cga16[16][3] = {
    { 0, 0, 0 }, { 0, 0, 42 }, { 0, 42, 0 }, { 0, 42, 42 }, { 42, 0, 0 }, { 42, 0, 42 }, { 42, 21, 0 }, { 42, 42, 42 },
    { 21, 21, 21 }, { 21, 21, 63 }, { 21, 63, 21 }, { 21, 63, 63 }, { 63, 21, 21 }, { 63, 21, 63 }, { 63, 63, 21 }, { 63, 63, 63 } };

static void default_palette(int graphics256)
{
    memset(vga_dac, 0, sizeof vga_dac);
    if (!graphics256) {
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
    palette_dirty = 1;
}

/* ---------------- modes ---------------- */
static int text_cols = 80, text_rows = 25;
static u32 text_base = 0xB8000;

static int is_text(int m) { return m <= 3 || m == 7; }
static void planar_check(void);

static void sync_cursor_crtc(void)
{
    u8 page = rd8(BDA + 0x62);
    u16 pos = rd16(BDA + 0x50 + page * 2);
    u16 off = (u16)(rd16(BDA + 0x4E) / 2 + (pos >> 8) * text_cols + (pos & 0xFF));
    crtc[0x0E] = off >> 8;
    crtc[0x0F] = (u8)off;
}

void video_set_mode(int mode, int clear)
{
    mode &= 0x7F;
    video_mode = mode;
    text_cols = (mode == 0 || mode == 1 || mode == 4 || mode == 5 || mode == 0x0D || mode == 0x13) ? 40 : 80;
    text_rows = 25;
    text_base = mode == 7 ? 0xB0000 : 0xB8000;
    wr8(BDA + 0x49, (u8)mode);
    wr16(BDA + 0x4A, (u16)text_cols);
    wr16(BDA + 0x4C, is_text(mode) ? (text_cols == 40 ? 0x800 : 0x1000) : (mode == 0x13 ? 0xFA00 : 0x4000));
    wr16(BDA + 0x4E, 0);
    for (int i = 0; i < 8; i++) wr16(BDA + 0x50 + i * 2, 0);
    wr16(BDA + 0x60, 0x0607);
    wr8(BDA + 0x62, 0);
    wr16(BDA + 0x63, mode == 7 ? 0x3B4 : 0x3D4);
    wr8(BDA + 0x65, mode == 0x13 ? 0x0E : 0x29);
    wr8(BDA + 0x66, 0x30);
    wr8(BDA + 0x84, (u8)(text_rows - 1));
    wr16(BDA + 0x85, mode == 0x13 ? 8 : 16);
    wr8(BDA + 0x87, (rd8(BDA + 0x87) & 0x7F) | (clear ? 0 : 0x80) | 0x60);
    wr8(BDA + 0x88, 0x09);
    wr8(BDA + 0x89, 0x11);
    memset(crtc, 0, sizeof crtc);
    seq[2] = 0x0F;                                 /* all planes */
    seq[4] = mode == 0x13 ? 0x0E : 0x02;           /* chain-4 in mode 13h */
    crtc[0x13] = text_cols == 40 && is_text(mode) ? 0x14 : 0x28;
    planar_check();
    crtc[0x0A] = 0x0D; crtc[0x0B] = 0x0E;
    if (clear) {
        if (is_text(mode)) for (u32 i = 0; i < 0x4000; i++) wr16(text_base + i * 2, 0x0720);
        else memset(gptr(0xA0000), 0, 0x10000);
    }
    default_palette(!is_text(mode));
    full_redraw = 1;
    last_layout_mode = -1;
    if (!is_text(mode) && mode != 0x13) kprintf("video mode %02x: not drawn yet\n", mode);
    else dbg(1, "video mode %02x\n", mode);
}

/* ---------------- drawing ---------------- */
static u16 shadow_text[132 * 60];
static u8 shadow_gfx[64000];
static int sh_cursor = -1, blink_phase;
static u32 frame_no;
static int t_scale, t_ox, t_oy;
static u32 g_x, g_y, g_w, g_h;
static u16 xmap[4096];
static u16 console_cells[80 * 25];
static int console_on;

static void layout(void)
{
    if (last_layout_mode == video_mode) return;
    last_layout_mode = video_mode;
    full_redraw = 1;
    clear_fb();
    int tw = 80 * 8, th = text_rows * 16;
    t_scale = (int)(fb_w / tw < fb_h / th ? fb_w / tw : fb_h / th);
    if (t_scale < 1) t_scale = 1;
    t_ox = ((int)fb_w - tw * t_scale) / 2; if (t_ox < 0) t_ox = 0;
    t_oy = ((int)fb_h - th * t_scale) / 2; if (t_oy < 0) t_oy = 0;
    g_h = fb_h; g_w = fb_h * 4 / 3;
    if (g_w > fb_w) { g_w = fb_w; g_h = fb_w * 3 / 4; }
    if (g_w > 4096) g_w = 4096;
    g_x = (fb_w - g_w) / 2; g_y = (fb_h - g_h) / 2;
    for (u32 x = 0; x < g_w; x++) xmap[x] = (u16)(x * 320 / g_w);
}

static void draw_cell(int col, int row, u16 cell, int cursor)
{
    static u32 line[132 * 8 * 8];
    u8 ch = cell & 0xFF, a = cell >> 8;
    int fg = a & 15, bg = a >> 4;
    if (atc[0x10] & 8) { bg &= 7; if ((a & 0x80) && !blink_phase) fg = bg; }
    u32 pf = pal[atc[fg] & 0x3F], pb = pal[atc[bg] & 0x3F];
    int s = t_scale, xs = (text_cols == 40 ? 2 : 1) * s;
    int cs = crtc[0x0A] & 0x1F, ce = crtc[0x0B] & 0x1F;
    int cur_on = cursor && blink_phase && !(crtc[0x0A] & 0x20);
    const u8 *g = font8x16 + ch * 16;
    for (int gy = 0; gy < 16; gy++) {
        u8 bits = g[gy];
        if (cur_on && gy >= cs && gy <= ce) bits = 0xFF;
        u32 *p = line;
        for (int bx = 0; bx < 8; bx++) {
            u32 c = (bits & (0x80 >> bx)) ? pf : pb;
            for (int k = 0; k < xs; k++) *p++ = c;
        }
        u32 x = t_ox + col * 8 * xs, y = t_oy + (row * 16 + gy) * s;
        put_row(line, 8 * xs, x, y);
        for (int k = 1; k < s; k++) copy_row(x, 8 * xs, y, y + k);
    }
}

static int sh_mouse = -1;

static void refresh_text(const u16 *cells, int cursor_off)
{
    int n = text_cols * text_rows;
    int mcell = -1, px, py;
    u16 m_and, m_xor;
    if (!console_on && mouse_pointer(&px, &py, &m_and, &m_xor))
        mcell = (py / 8) * text_cols + px / (text_cols == 40 ? 16 : 8);
    if (mcell >= n) mcell = -1;
    for (int i = 0; i < n; i++) {
        u16 c = cells[i];
        int cur = i == cursor_off;
        int blinky = (atc[0x10] & 8) && (c & 0x8000);
        if (full_redraw || c != shadow_text[i] || cur || i == sh_cursor || i == mcell || i == sh_mouse ||
            (blinky && (frame_no & 15) == 0)) {
            draw_cell(i % text_cols, i / text_cols, i == mcell ? (u16)((c & m_and) ^ m_xor) : c, cur);
            shadow_text[i] = c;
        }
    }
    sh_cursor = cursor_off;
    sh_mouse = mcell;
}

/* The mouse pointer in graphics modes: an arrow, 1 = outline, 2 = fill. */
static const char *const arrow[16] = {
    "1", "11", "121", "1221", "12221", "122221", "1222221", "12222221",
    "122222221", "1222211111", "1221221", "121 1221", "11  1221", "1    1221", "     1221", "      11" };
static int sh_ptr_y = -100, sh_ptr_x;

/* ---------------- unchained 256-colour (planar) VGA ----------------
   With chain-4 off (Mode X/Y, DOOM), video memory is four 64 KB planes and
   the map mask (sequencer 2) picks which ones a CPU write goes to. The
   guest's A0000h window is mapped onto one plane's pages; when the mask
   selects several, pages written meanwhile (PTE dirty bits) are copied to
   the others when the mask changes or the screen is drawn. Reads come from
   the plane the window shows. */
static u8 *planes;                 /* 4 x 64 KiB */
static int planar_on, mapped_plane = -1;
static int bcast_src = -1;
static u8 bcast_mask;

static void map_window(u32 phys)
{
    for (u32 i = 0; i < 16; i++) map_page(0xA0000 + i * 4096, phys + i * 4096, 7);
    tlb_flush();
    for (u32 i = 0; i < 16; i++) page_dirty(0xA0000 + i * 4096, 1);
    tlb_flush();
}

static void bcast_flush(int keep)
{
    if (bcast_src < 0) return;
    int any = 0;
    for (u32 i = 0; i < 16; i++) {
        if (!page_dirty(0xA0000 + i * 4096, 1)) continue;
        any = 1;
        for (int q = 0; q < 4; q++)
            if (q != bcast_src && (bcast_mask & (1 << q)))
                memcpy(planes + q * 65536 + i * 4096, planes + bcast_src * 65536 + i * 4096, 4096);
    }
    if (any) tlb_flush();
    if (!keep) bcast_src = -1;
}

static void apply_map_mask(void)
{
    if (!planar_on) return;
    bcast_flush(0);
    u8 m = seq[2] & 15;
    int p = 0;
    while (p < 3 && m && !(m & (1 << p))) p++;
    if (p != mapped_plane) { map_window((u32)(uintptr_t)planes + p * 65536); mapped_plane = p; }
    if (m & (m - 1)) { bcast_src = p; bcast_mask = m; }
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
        bcast_flush(0);
        map_window(guest_phys(0xA0000));
        planar_on = 0;
        mapped_plane = -1;
        full_redraw = 1;
    }
}

static void refresh_13h(void)
{
    static u32 line[4096];
    static u8 ovr[320];
    static u8 rowbuf[320];
    const u8 *src = gptr(0xA0000);
    u32 start = 0, pitch = 320;
    if (planar_on) {
        bcast_flush(1);
        start = (u32)(crtc[0x0C] << 8 | crtc[0x0D]);
        pitch = crtc[0x13] ? crtc[0x13] * 2u : 80;
    }
    int px = 0, py = -100;
    u16 ma, mxr;
    if (mouse_pointer(&px, &py, &ma, &mxr)) px /= 2; else py = -100;
    int moved = px != sh_ptr_x || py != sh_ptr_y;
    u32 black = pack(0, 0, 0), white = pack(63, 63, 63);
    for (int y = 0; y < 200; y++) {
        const u8 *s = src + y * 320;
        if (planar_on) {
            u32 o = start + y * pitch;
            for (int x = 0; x < 320; x++) rowbuf[x] = planes[(x & 3) * 65536 + ((o + (x >> 2)) & 0xFFFF)];
            s = rowbuf;
        }
        int in_new = y >= py && y < py + 16, in_old = y >= sh_ptr_y && y < sh_ptr_y + 16;
        if (!full_redraw && !(moved && (in_new || in_old)) && !memcmp(s, shadow_gfx + y * 320, 320)) continue;
        memcpy(shadow_gfx + y * 320, s, 320);
        for (u32 x = 0; x < g_w; x++) line[x] = pal[s[xmap[x]]];
        if (in_new) {
            memset(ovr, 0, sizeof ovr);
            const char *a = arrow[y - py];
            for (int i = 0; a[i] && px + i < 320; i++) ovr[px + i] = a[i] == '1' ? 1 : a[i] == '2' ? 2 : 0;
            for (u32 x = 0; x < g_w; x++)
                if (ovr[xmap[x]]) line[x] = ovr[xmap[x]] == 1 ? black : white;
        }
        u32 y0 = g_y + y * g_h / 200, y1 = g_y + (y + 1) * g_h / 200;
        put_row(line, g_w, g_x, y0);
        for (u32 yy = y0 + 1; yy < y1; yy++) copy_row(g_x, g_w, y0, yy);
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
    } else if (video_mode == 0x13) refresh_13h();
    full_redraw = 0;
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
    case 0x3C4: seq_idx = v; return;
    case 0x3C5:
        seq[seq_idx & 7] = v;
        if ((seq_idx & 7) == 4) planar_check();
        else if ((seq_idx & 7) == 2) apply_map_mask();
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
    case 0x3CF: gc[gc_idx & 15] = v; return;
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

static void gfx_char(int row, int col, u8 ch, u8 color)
{
    if (video_mode != 0x13) return;
    const u8 *g = font8x8 + ch * 8;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            u32 a = 0xA0000 + (row * 8 + y) * 320 + col * 8 + x;
            if (color & 0x80) { if (g[y] & (0x80 >> x)) wr8(a, rd8(a) ^ (color & 0x7F)); }
            else wr8(a, (g[y] & (0x80 >> x)) ? color : 0);
        }
}

static void scroll(int up, int lines, u8 attr, int r0, int c0, int r1, int c1, int page)
{
    if (r1 >= text_rows) r1 = text_rows - 1;
    if (c1 >= text_cols) c1 = text_cols - 1;
    if (r0 > r1 || c0 > c1) return;
    int h = r1 - r0 + 1;
    if (lines == 0 || lines > h) lines = h;
    if (video_mode == 0x13) {
        for (int i = 0; i < h * 8; i++) {
            int dy = up ? r0 * 8 + i : r1 * 8 + 7 - i;
            int sy = up ? dy + lines * 8 : dy - lines * 8;
            int in = up ? sy <= r1 * 8 + 7 : sy >= r0 * 8;
            u8 *d = gptr(0xA0000 + dy * 320 + c0 * 8);
            if (in) memmove(d, gptr(0xA0000 + sy * 320 + c0 * 8), (c1 - c0 + 1) * 8);
            else memset(d, attr, (c1 - c0 + 1) * 8);
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
    case 0x0C:
        if (video_mode == 0x13 && CX(r) < 320 && DX(r) < 200) {
            u32 a = 0xA0000 + DX(r) * 320 + CX(r);
            wr8(a, (AL(r) & 0x80) ? rd8(a) ^ (AL(r) & 0x7F) : AL(r));
        }
        break;
    case 0x0D:
        AL(r) = (video_mode == 0x13 && CX(r) < 320 && DX(r) < 200) ? rd8(0xA0000 + DX(r) * 320 + CX(r)) : 0;
        break;
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
    case 0x4F: AX(r) = 0x014F; break;          /* VESA BIOS: not yet */
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
