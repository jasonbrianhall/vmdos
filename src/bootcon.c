/* The boot console: the kernel's log, drawn on the screen as it is
   written, from the kernel's first line until DOS starts (or vmdos stops
   with its own screen). On a real PC there is usually no serial port, and
   without this nothing shows between vmdos.efi (or GRUB) and DOS: a hang
   in there would look like a blank screen.

   It draws straight into the framebuffer the loader set up, before paging
   (at its physical address) and after (at its mapping), with its own font
   cells and nothing from video.c, so it works before the video code is
   up. 32-bit pixels only (UEFI GOP and GRUB's mode are); anything else:
   the log stays on serial and the stop screen. When the screen fills, it
   jumps up by half a screen (one redraw per half screen: framebuffer
   writes before paging are uncached and slow). */
#include "kernel.h"
#include "version.h"

#define MAX_COLS 160
#define MAX_ROWS 80
static volatile u8 *fb;
static u32 pitch, width, height, scale;
static u8 rpos, gpos, bpos;
static int cols, rows, row, col, busy;
int bootcon_on;
static char text[MAX_ROWS][MAX_COLS];         /* what is on screen, for the jump */

static u32 rgb(u8 r, u8 g, u8 b) { return (u32)r << rpos | (u32)g << gpos | (u32)b << bpos; }

static void glyph(int r, int c, u8 ch, u32 fg, u32 bg)
{
    const u8 *f = vga_font16 + ch * 16;
    u32 x0 = (u32)c * 8 * scale, y0 = (u32)r * 16 * scale;
    for (u32 y = 0; y < 16 * scale; y++) {
        volatile u32 *p = (volatile u32 *)(fb + (y0 + y) * pitch) + x0;
        u8 bits = f[y / scale];
        for (u32 x = 0; x < 8 * scale; x++) p[x] = (bits & (0x80 >> (x / scale))) ? fg : bg;
    }
}

static u32 fg_of(int r) { return r == 0 ? rgb(255, 255, 255) : rgb(200, 200, 200); }
static u32 bg_of(int r) { return r == 0 ? rgb(0, 0, 170) : 0; }

static void draw_row(int r)
{
    for (int c = 0; c < cols; c++) glyph(r, c, (u8)(text[r][c] ? text[r][c] : ' '), fg_of(r), bg_of(r));
}

static int dos_on_other;                      /* video=vbe on another card: DOS runs there, the log stays here */

static void title(void)
{
    char t[MAX_COLS + 1];
    int n = dos_on_other ? snprintf(t, sizeof t, " vmdos %s log: DOS is on the other screen (also on the serial port)", VMDOS_VERSION)
                         : snprintf(t, sizeof t, " vmdos %s starting: its log (also on the serial port)", VMDOS_VERSION);
    if (n > cols) n = cols;
    memset(text[0], ' ', MAX_COLS);
    memcpy(text[0], t, (u32)n);
    draw_row(0);
}

/* A 32 bpp framebuffer at addr: its physical address before paging (below
   4 GiB), or its mapping after (cpu.c starts it late for one above 4 GiB).
   0: not usable. */
int bootcon_start(u64 addr, u32 p, u32 w, u32 h, u32 bpp, u8 rp, u8 gp, u8 bp)
{
    if (bpp != 32 || !addr || addr + (u64)p * h > 0x100000000ull || w < 320 || h < 200) return 0;
    fb = (volatile u8 *)(uintptr_t)addr; pitch = p; width = w; height = h;
    rpos = rp; gpos = gp; bpos = bp;
    scale = w >= 1600 && h >= 1000 ? 2 : 1;
    cols = (int)(w / (8 * scale)); rows = (int)(h / (16 * scale));
    if (cols > MAX_COLS) cols = MAX_COLS;
    if (rows > MAX_ROWS) rows = MAX_ROWS;
    for (u32 y = 0; y < h; y++) {                     /* clear what the loader left */
        volatile u32 *q = (volatile u32 *)(fb + y * pitch);
        for (u32 x = 0; x < w; x++) q[x] = 0;
    }
    memset(text, 0, sizeof text);
    row = 1; col = 0;
    bootcon_on = 1;
    title();
    return 1;
}

/* Paging is on: the framebuffer is at lin now (0: not mapped; stop). */
void bootcon_remap(u32 lin)
{
    if (!bootcon_on) return;
    if (!lin) { bootcon_on = 0; return; }
    fb = (volatile u8 *)(uintptr_t)lin;
}

void bootcon_stop(void) { bootcon_on = 0; }

void bootcon_dos_started(void)
{
    if (!bootcon_on) return;
    dos_on_other = 1;
    title();
}

static void newline(void)
{
    col = 0;
    if (++row < rows) return;
    int keep = (rows - 1) / 2;                        /* jump: the last half stays */
    for (int r = 1; r < rows; r++) {
        if (r <= keep) memcpy(text[r], text[rows - keep + r - 1], MAX_COLS);
        else memset(text[r], 0, MAX_COLS);
        draw_row(r);
    }
    row = keep + 1;
}

void bootcon_putc(char c)
{
    if (!bootcon_on || __atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE)) return;   /* another CPU's or an interrupt's line while one is drawn: serial only */
    if (c == '\n') newline();
    else if (c == '\r') col = 0;                      /* rewrite the line (a countdown) */
    else if (c >= 32 && c < 127) {
        if (col >= cols) newline();
        text[row][col] = c;
        glyph(row, col, (u8)c, fg_of(row), bg_of(row));
        col++;
    }
    __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
}
