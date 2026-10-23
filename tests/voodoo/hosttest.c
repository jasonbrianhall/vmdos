/* The emulated Voodoo 3 (src/voodoo) as a Linux program: the card's code
   as the kernel builds it, the kernel's side stubbed. Draws through the
   card's registers what a Glide program would (a fill, a Gouraud triangle
   through the setup unit, a textured one through the command FIFO in the
   frame buffer, a swap to the overlay) and writes what the video processor
   shows to a .ppm, comparing a few pixels with what they must be.

   make -C tests/voodoo   (needs gcc-multilib), then ./hosttest out.ppm */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../../src/voodoo3.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

/* ---------------- the kernel, stubbed ---------------- */
char cmdline[512] = "voodoo3";
u8 vga_dac[256][3];
u8 crtc[64];
void kprintf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); }
void panic(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); exit(1); }
void *phys_alloc(u32 n) { void *p = aligned_alloc(4096, (n + 4095) & ~4095u); memset(p, 0, n); return p; }
void *phys_try_alloc(u32 n) { return phys_alloc(n); }
u32 linear_hole(u32 len, u32 align) { (void)len; (void)align; return 0xC0000000u; }
static u8 ports[0x40];
u32 video_port_in(u16 port) { return ports[port & 0x3F]; }
void video_port_out(u16 port, u8 v) { ports[port & 0x3F] = v; }
static u32 fake_clock;                        /* PIT clocks: moved by the test */
u32 pit_clock(void) { return fake_clock; }
int video_vretrace(void) { return 0; }
void map_page(u32 lin, u32 phys, u32 flags) { (void)lin; (void)phys; (void)flags; }   /* no paging here: */
void tlb_flush_range(u32 lin, u32 len) { (void)lin; (void)len; }                   /* every access traps */
void hv_map_gpa(u32 gpa, u32 host) { (void)gpa; (void)host; }
void vpci_bios_patch(void) { }
u32 video_frame_no(void) { return fake_clock / 17045; }

/* ---------------- the card's registers ---------------- */
#define BAR0 0xC0000000u
#define BAR1 0xC2000000u
static void io(u32 reg, u32 v) { v3_mmio_write(BAR0 + reg, v, 4); }        /* I/O remap */
static void cmd(u32 reg, u32 v) { v3_mmio_write(BAR0 + 0x80000 + reg, v, 4); }
static void r3d(u32 reg, u32 v) { v3_mmio_write(BAR0 + 0x200000 + reg, v, 4); }
static u32 r3d_read(u32 reg) { return v3_mmio_read(BAR0 + 0x200000 + reg, 4); }
static u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }
static void lfb(u32 a, u32 v) { v3_mmio_write(BAR1 + a, v, 4); }

static void idle(void)                        /* as grSstIdle: until the status says not busy */
{
    for (int i = 0; i < 100000; i++) {
        fake_clock += 100;
        if (!(r3d_read(0) & 0x7a0)) return;   /* not busy, its FIFO empty */
    }
    printf("still busy!\n");
}

enum { W = 640, H = 480, STRIDE = W * 2, FRONT = 0, BACK = 0x100000, FIFO = 0x400000 };

static u32 *shot;
static void grab(void)
{
    u32 w, h;
    for (int i = 0; i < 10; i++) { fake_clock += 19886; v3_tick(); }   /* frames go by: the card draws them */
    if (!v3_display(&w, &h)) { printf("the video processor doesn't own the screen\n"); exit(1); }
    if (w != W || h != H) printf("screen %ux%u\n", w, h);
    for (u32 y = 0; y < H; y++) v3_row(y, shot + y * W);
}
static int check(const char *what, int x, int y, u32 want)
{
    u32 got = shot[y * W + x] & 0xFFFFFF;
    int ok = abs((int)(got >> 16) - (int)(want >> 16)) < 12 && abs((int)((got >> 8) & 255) - (int)((want >> 8) & 255)) < 12 &&
             abs((int)(got & 255) - (int)(want & 255)) < 12;
    printf("%-34s (%3d,%3d): %06x, want %06x  %s\n", what, x, y, got, want, ok ? "ok" : "WRONG");
    return ok;
}

/* what the card's BIOS would do for 640x480: the VGA's timings (mode 12h's) */
static void outb(u16 p, u8 v) { u32 x = v; v3_io(p, 1, 1, &x); }
static u8 inb(u16 p) { u32 x = 0; v3_io(p, 1, 0, &x); return x; }
static void vga_640x480(void)
{
    static const u8 crtc[25] = { 0x5F, 0x4F, 0x50, 0x82, 0x54, 0x80, 0x0B, 0x3E, 0x00, 0x40, 0, 0, 0, 0, 0, 0,
                                 0xEA, 0x8C, 0xDF, 0x28, 0x00, 0xE7, 0x04, 0xE3, 0xFF };
    outb(0x3C2, 0xE3);
    outb(0x3C4, 0); outb(0x3C5, 3);
    outb(0x3C4, 1); outb(0x3C5, 1);
    outb(0x3D4, 0x11); outb(0x3D5, 0);
    for (int i = 0; i < 25; i++) { outb(0x3D4, i); outb(0x3D5, crtc[i]); }
    inb(0x3DA); outb(0x3C0, 0x20);                         /* the attribute controller: palette on */
}

int main(int argc, char **argv)
{
    int bad = 0;
    shot = calloc(W * H, 4);
    v3_init();
    if (!v3_on) return 1;

    /* PCI: the IDs and the BARs */
    printf("PCI %08x class %06x BAR0 %08x BAR1 %08x BAR2 %08x\n", v3_cfg_read(0, 4), v3_cfg_read(8, 4) >> 8,
           v3_cfg_read(0x10, 4), v3_cfg_read(0x14, 4), v3_cfg_read(0x18, 4));
    v3_cfg_write(0x10, 0xFFFFFFFF, 4);
    printf("BAR0 size probe: %08x\n", v3_cfg_read(0x10, 4));
    v3_cfg_write(0x10, BAR0, 4);

    /* the screen: 640x480, 16 bpp desktop at FRONT */
    vga_640x480();
    io(0x0c, 0x1fff);                                      /* lfbMemoryConfig: nothing tiled */
    io(0x28, 1 << 12);                                     /* vgaInit0: extended shift out */
    io(0x98, W | H << 12);                                 /* vidScreenSize */
    io(0xe4, FRONT);                                       /* vidDesktopStartAddr */
    io(0xe8, STRIDE | STRIDE << 16);                       /* vidDesktopOverlayStride */
    io(0x5c, 1 | 1 << 18 | 1 << 10 | 1 << 11);             /* vidProcCfg: on, 565, CLUTs bypassed */

    /* 3D: draw into FRONT */
    r3d(0x1ec, FRONT);                                     /* colBufferAddr */
    r3d(0x1f0, STRIDE);                                    /* colBufferStride */
    r3d(0x1f4, 0x200000);                                  /* auxBufferAddr */
    r3d(0x1f8, STRIDE);
    r3d(0x118, 0 << 16 | W);                               /* clipLeftRight */
    r3d(0x11c, 0 << 16 | H);                               /* clipLowYHighY */
    r3d(0x110, 1 | 1 << 9);                                /* fbzMode: clip, RGB writes */

    /* a fill: dark blue */
    r3d(0x148, 0x00000080);                                /* color1 */
    r3d(0x124, 0);                                         /* fastfillCMD */
    idle();
    grab();
    bad += !check("fill", 10, 10, 0x000080);

    /* a Gouraud triangle through the setup unit: red, green, blue corners */
    r3d(0x104, 1 << 8 | 1 << 14);                          /* fbzColorPath: iterated RGB */
    r3d(0x260, 1);                                         /* sSetupMode: RGB */
    r3d(0x264, f2u(320)); r3d(0x268, f2u(40));  r3d(0x26c, 0xFF0000); r3d(0x2a4, 0);   /* sBeginTriCMD */
    r3d(0x264, f2u(560)); r3d(0x268, f2u(440)); r3d(0x26c, 0x00FF00); r3d(0x2a0, 0);   /* sDrawTriCMD */
    r3d(0x264, f2u(80));  r3d(0x268, f2u(440)); r3d(0x26c, 0x0000FF); r3d(0x2a0, 0);
    idle();
    grab();
    printf("pixels in %u, chroma fail %u, z fail %u, alpha fail %u, out %u\n", r3d_read(0x14c), r3d_read(0x150), r3d_read(0x154), r3d_read(0x158), r3d_read(0x15c));
    bad += !check("triangle, near the red corner", 320, 50, 0xF80000);
    bad += !check("triangle, middle", 320, 300, 0x555555);
    bad += !check("outside the triangle: fill", 600, 50, 0x000080);

    /* the command FIFO in the frame buffer, as Glide feeds it (hole
       counting): a packet 1 writing color1, then fastfillCMD into BACK */
    cmd(0x24, 0);                                          /* cmdBaseSize0: off while setting up */
    cmd(0x20, FIFO >> 12);                                 /* cmdBaseAddr0 */
    cmd(0x2c, FIFO);                                       /* cmdRdPtrL0 */
    cmd(0x34, FIFO - 4);                                   /* cmdAMin0 */
    cmd(0x3c, FIFO - 4);                                   /* cmdAMax0 */
    cmd(0x44, 0);                                          /* cmdFifoDepth0 */
    cmd(0x48, 0);                                          /* cmdHoleCnt0 */
    cmd(0x24, (64 - 1) | 1 << 8);                          /* 64 KB, on */
    u32 a = FIFO;
    /* packet 1: 3D register writes, count 3, increment, starting at colBufferAddr */
    lfb(a, 1 | (0x1ec >> 2) << 3 | 1 << 15 | 2 << 16); a += 4;
    lfb(a, BACK); a += 4;                                  /* colBufferAddr */
    lfb(a, STRIDE); a += 4;                                /* colBufferStride */
    lfb(a, 1 | (0x148 >> 2) << 3 | 1 << 16); a += 4;       /* packet 1: color1 */
    lfb(a, 0x0000FF00); a += 4;
    lfb(a, 1 | (0x124 >> 2) << 3 | 1 << 16); a += 4;      /* packet 1: fastfillCMD */
    lfb(a, 0); a += 4;
    idle();
    printf("command FIFO: depth %u, read pointer %x (wrote up to %x)\n",
           v3_mmio_read(BAR0 + 0x80044, 4), v3_mmio_read(BAR0 + 0x8002c, 4), a);

    /* swap: show BACK through the overlay, as Glide does */
    io(0x9c, 0);                                           /* vidOverlayStartCoords */
    io(0xa0, (W - 1) | (H - 1) << 12);                     /* vidOverlayEndScreenCoords */
    io(0xa4, 0);                                           /* vidOverlayDudx: 1:1 */
    io(0xa8, (u32)STRIDE << 19);                           /* vidOverlayDudxOffsetSrcWidth: its width in bytes */
    io(0xac, 0);                                           /* vidOverlayDvdy */
    io(0x5c, 1 | 1 << 18 | 1 << 8 | 1 << 21 | 1 << 10 | 1 << 11);   /* + overlay, 565 */
    r3d(0x250, BACK);                                      /* leftOverlayBuf */
    r3d(0x128, 1);                                         /* swapbufferCMD, at a retrace */
    for (int i = 0; i < 5; i++) { fake_clock += 17045; v3_tick(); }
    idle();
    grab();
    bad += !check("overlay after the swap: green", 100, 100, 0x00FF00);

    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        fprintf(f, "P6 %d %d 255\n", W, H);
        for (int i = 0; i < W * H; i++) { u8 p[3] = { shot[i] >> 16, shot[i] >> 8, shot[i] }; fwrite(p, 1, 3, f); }
        fclose(f);
    }
    printf("%s\n", bad ? "FAILED" : "all good");
    return bad != 0;
}
