/* The emulated Voodoo 3 (src/voodoo) as a Linux program: the card's code
   as the kernel builds it, the kernel's side stubbed. Draws through the
   card's registers what a Glide program would (a fill, a Gouraud triangle
   through the setup unit, a textured one through the command FIFO in the
   frame buffer, a swap to the overlay) and writes what the video processor
   shows to a .ppm, comparing a few pixels with what they must be.

   make -C tests/voodoo   (needs gcc-multilib), then ./hosttest out.ppm, and
   ./hosttest out.ppm direct: the FIFO written straight into the card's memory
   (as through vmdos's mapped pages) instead of through the trap. Also:
   slice (the card's time slices end after a read or two), jit (86Box's JIT
   draws the pixels), random (400 triangles with modes picked at random:
   the buffers' checksums), random jitcmp (each of them by the C loop and by
   the JIT, compared; RGAME=1 picks only modes the JIT takes, RSEED= another
   set, RCOUNT= how many). mp=N: the triangles drawn by N render helper
   CPUs (v3.c v3_mp_start), here threads, as vmdos does: the same checksums.
   mpfault: the first of them stops a while in, as one that faulted would. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
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
int host_osfxsr;                              /* "jit": SSE on, so the card's JIT draws the pixels (v3.c) */
void *v3_exec_alloc(u32 n)                    /* the JIT's code: memory it can run */
{
    void *p = mmap(0, n, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? 0 : p;
}
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
void vpic_raise(int irq) { (void)irq; }
int v3_map_own;
static int slice;                             /* "slice": a 1 GHz TSC that also moves on 1 ms at every read, */
static uint64_t slice_tsc;                    /* so the card's time slices (v3sched.c) end after a few reads: */
uint64_t tsc_read(void) { return slice ? (slice_tsc += 1000000) + (uint64_t)fake_clock * 838 : 0; }   /* the green */
uint32_t tsc_khz(void) { return slice ? 1000000 : 0; }   /* threads stop and go on wherever they can. Else no */
                                                          /* TSC rate: the card's clock is pit_clock */
u32 video_frame_no(void) { return fake_clock / 17045; }

int perf_on;                                   /* lib.c's: v3.c's access counts ("perf") */

/* smp.c's render helpers, as threads ("mp=N") */
static int mp_cpus, mp_fault;
static int (*mp_fn)(int, int);
static volatile int mp_dead[4];
static void *mp_thread(void *p)
{
    int k = (int)(intptr_t)p, drawn = 0;
    for (;;) {
        if (!mp_fn(k, 0)) { sched_yield(); continue; }
        drawn += mp_fn(k, 1);
        if (mp_fault && k == 0 && drawn > 150) { mp_dead[0] = 1; printf("render helper 0 stops (as a fault)\n"); return 0; }
    }
}
/* smp.c's par_for ("par=N": N threads, each call, as the boot CPU and the
   render helpers in VMHV; else one) and smp_slot */
static int par_cpus;
static __thread int my_slot;
int smp_slot(void) { return my_slot; }
struct par_job { u32 n, chunk; void (*fn)(u32, void *); void *arg; volatile u32 next; };
static void par_run(struct par_job *j)
{
    for (;;) {
        u32 a = __atomic_fetch_add(&j->next, j->chunk, __ATOMIC_SEQ_CST);
        if (a >= j->n) return;
        u32 b = a + j->chunk < j->n ? a + j->chunk : j->n;
        for (u32 i = a; i < b; i++) j->fn(i, j->arg);
    }
}
static struct par_job *par_cur;
static void *par_thread(void *p) { my_slot = (int)(intptr_t)p; par_run(par_cur); return 0; }
int par_for(u32 n, u32 chunk, void (*fn)(u32, void *), void *arg)
{
    struct par_job j = { n, chunk, fn, arg, 0 };
    if (par_cpus < 2) { for (u32 i = 0; i < n; i++) fn(i, arg); return 1; }
    pthread_t t[8];
    par_cur = &j;
    for (int k = 1; k < par_cpus; k++) pthread_create(&t[k], 0, par_thread, (void *)(intptr_t)k);
    par_run(&j);
    for (int k = 1; k < par_cpus; k++) pthread_join(t[k], 0);
    return 1;
}
int smp_render_count(void) { return mp_cpus; }
int smp_render_take(int n, int (*fn)(int, int))
{
    mp_fn = fn;
    for (int k = 0; k < n; k++) {
        pthread_t t;
        pthread_create(&t, 0, mp_thread, (void *)(intptr_t)k);
        pthread_detach(t);
    }
    return n;
}
int smp_render_kick(int k) { return !mp_dead[k]; }

/* ---------------- the card's registers ---------------- */
#define BAR0 0xC0000000u
#define BAR1 0xC2000000u
static void io(u32 reg, u32 v) { v3_mmio_write(BAR0 + reg, v, 4); }        /* I/O remap */
static void cmd(u32 reg, u32 v) { v3_mmio_write(BAR0 + 0x80000 + reg, v, 4); }
static void r3d(u32 reg, u32 v) { v3_mmio_write(BAR0 + 0x200000 + reg, v, 4); }
static u32 r3d_read(u32 reg) { return v3_mmio_read(BAR0 + 0x200000 + reg, 4); }
static u32 f2u(float f) { u32 u; memcpy(&u, &f, 4); return u; }
uint8_t *v3_test_vram(void);
static int direct;                            /* "direct": FIFO writes straight into vram, as through */
static void lfb(u32 a, u32 v)                 /* vmdos's mapped pages (v3.c fifo_feed finds them) */
{
    if (direct && a >= 0x400000 && a < 0x410000) *(u32 *)(v3_test_vram() + a) = v;
    else v3_mmio_write(BAR1 + a, v, 4);
}

static void idle(void)                        /* as grSstIdle: until the status says not busy */
{
    int v3_test_rendering(void);
    time_t t0 = time(0);
    for (int i = 0; i < 100000; i++) {
        if (mp_cpus && v3_test_rendering() && time(0) - t0 < 20) {   /* "mp": the helpers draw, the card's time stands */
            sched_yield();                                            /* still, as when the card's code drew them here */
            i--;
            continue;
        }
        fake_clock += 100;
        if (!(r3d_read(0) & 0x7a0)) return;   /* not busy, its FIFO empty */
    }
    printf("still busy!\n");
    if (getenv("BUSYDBG")) { void v3_debug(void); v3_debug(); }
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
    if (getenv("SHOTSUM")) {                   /* the picture itself, to compare par=N with without */
        u32 h = 2166136261u;
        for (u32 i = 0; i < W * H; i++) h = (h ^ (shot[i] & 0xffffff)) * 16777619u;
        printf("picture %08x\n", h);
    }
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

/* "random": triangles with modes picked at random (colour path, alpha
   blending and test, depth test, fog, textures in several formats), the
   frame and depth buffers checksummed every 25: the JIT ("jit") must give
   the C loop's sums. */
static u32 rnd_state = 12345;   /* RSEED= another */
static u32 rnd(void) { u32 x = rnd_state; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return rnd_state = x; }   /* xorshift32 */
static float rndf(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFF) / 65535.0f; }
static u32 sum_vram(void)
{
    const u8 *v = v3_test_vram();
    u32 h = 2166136261u;
    for (u32 a = FRONT; a < 0x200000 + STRIDE * H; a++) h = (h ^ v[a]) * 16777619u;
    return h;
}
void v3_test_jit(int on);
static int jit_cmp;
static u32 m_cp, m_fbz, m_am, m_fm, m_tm;                           /* "random jitcmp": each triangle by both, compared */
static u8 *snap, *res;
static void random_tris(int n);
static int one_tri(int i);
static void random_tris(int n)
{
    if (!jit_cmp) { for (int i = 0; i < n; i++) one_tri(i); return; }
    u32 len = 0x200000 + STRIDE * H;
    snap = malloc(len); res = malloc(len);
    u8 *v = v3_test_vram();
    for (u32 a = 0x800000; a < 0x900000; a++) v[a] = (u8)rnd();
    int bad = 0, minor = 0;
    for (int i = 0; i < n; i++) {
        u32 seed = rnd_state;
        idle();
        memcpy(snap, v, len);
        v3_test_jit(0); one_tri(i); idle();
        memcpy(res, v, len);
        memcpy(v, snap, len);
        rnd_state = seed;
        v3_test_jit(1); one_tri(i); idle();
        u32 nd = 0, first = 0;
        if (getenv("JALL")) printf("jitok? %d: cp %08x fbz %08x am %08x fog %02x tm %08x\n", i, m_cp, m_fbz, m_am, m_fm, m_tm);
        for (u32 a = 0; a < len; a += 2)
            if (*(u16 *)(v + a) != *(u16 *)(res + a)) { if (!nd++) first = a; }
        if (nd && nd <= 32) minor++;                 /* a texel off here and there (perspective rounding) */
        else if (nd) {
            bad++;
            printf("jitcmp %d: cp %08x fbz %08x am %08x fog %02x tm %08x:", i, m_cp, m_fbz, m_am, m_fm, m_tm);
            printf(" %u words differ, first at %s %u,%u: C %04x JIT %04x\n", nd, first >= 0x200000 ? "aux" : "colour",
                   (first % 0x200000) % STRIDE / 2, (first % 0x200000) / STRIDE, *(u16 *)(res + first), *(u16 *)(v + first));
        }
        memcpy(v, res, len);                  /* go on from the C loop's */
    }
    extern int v3_jit_used, v3_jit_refused;
    printf("jitcmp: %d of %d triangles differ (%d more by at most 32 pixels); JIT drew %d of %d\n", bad, n, minor,
           v3_jit_used, v3_jit_used + v3_jit_refused);
}
static int one_tri(int i)
{
    static const u8 fmts[] = { 0, 2, 3, 4, 8, 0xA, 0xB, 0xC, 0xD };
    if (!jit_cmp && i == 0) { u8 *v = v3_test_vram(); for (u32 a = 0x800000; a < 0x900000; a++) v[a] = (u8)rnd(); }   /* textures */
    {
        u32 cp = rnd() & 0x0FFFFFFF;
        if (rnd() & 1) cp |= 1u << 27; else cp &= ~(1u << 27);          /* textures on or off */
        if (getenv("RNOTEX")) cp &= ~(1u << 27);
        if (getenv("RCP")) cp &= strtoul(getenv("RCP"), 0, 16);
        if (getenv("RCPSET")) cp |= strtoul(getenv("RCPSET"), 0, 16);
        r3d(0x104, cp);                                                 /* fbzColorPath */
        { u32 fm = rnd() & 0x3F, am = rnd();
          if (getenv("RFOG")) fm &= strtoul(getenv("RFOG"), 0, 16);
          if (getenv("RAM")) am &= strtoul(getenv("RAM"), 0, 16);
          if (getenv("RAFIX")) am = strtoul(getenv("RAFIX"), 0, 16);
          if (getenv("RV")) printf("fog %02x alpha %08x\n", fm, am);
          r3d(0x108, fm); r3d(0x10c, am); m_fm = fm; m_am = am; }                             /* fogMode alphaMode */
        u32 fbz = (rnd() & 0x001FFFFE) | 1 | 1 << 9;                    /* clipping, RGB writes */
        fbz &= ~(3u << 14 | 1u << 17);                                  /* the front buffer, Y origin top */
        if (getenv("RFBZ")) fbz &= strtoul(getenv("RFBZ"), 0, 16);
        r3d(0x110, fbz);                                                /* fbzMode */
        r3d(0x12c, rnd()); r3d(0x130, rnd()); r3d(0x134, rnd());        /* fogColor zaColor chromaKey */
        r3d(0x144, rnd()); r3d(0x148, rnd());                           /* color0 color1 */
        u32 tm = (rnd() & 0x3FFFF0FF) | (u32)fmts[rnd() % sizeof fmts] << 8;
        if (getenv("RTM")) tm &= strtoul(getenv("RTM"), 0, 16);
        if (getenv("RTMSET")) tm |= strtoul(getenv("RTMSET"), 0, 16);
        r3d(0x300, tm);                                                 /* textureMode */
        r3d(0x304, 12 | 12 << 6);                                       /* tLOD: 32x32 */
        r3d(0x30c, 0x800000 + (rnd() & 0x7) * 0x10000);                 /* texBaseAddr */
        if (getenv("RGAME")) {                                         /* only what the JIT takes: does it match there? */
            fbz &= ~(1u << 2 | 1u << 13 | 1u << 18 | 1u << 20);
            if (((cp >> 14) & 3) == 2) cp &= ~(3u << 14);
            if ((cp & 3) == 3) cp &= ~3u;
            cp &= ~(1u << 18 | 1u << 7);
            if (((cp >> 10) & 7) == 2 || ((cp >> 10) & 7) >= 6) cp &= ~(7u << 10);
            if (!(cp & 1u << 27)) {                                     /* no texture: nothing taken from one */
                if ((cp & 3) == 1) cp &= ~3u;
                if (((cp >> 2) & 3) == 1) cp &= ~(3u << 2);
                if (((cp >> 10) & 7) >= 4) cp &= ~(7u << 10);
                if (((cp >> 19) & 7) >= 4) cp &= ~(7u << 19);
                cp &= ~(1u << 7);
            }
            if (fbz & 1u << 19) fbz &= ~(1u << 19);
            for (int sh = 14; sh <= 23; sh += 9) if (((tm >> sh) & 7) >= 4) tm &= ~(4u << sh);
            if ((tm >> 13) & 1) tm &= ~(3u << 18);
            if ((tm >> 22) & 1) tm &= ~(3u << 27);
            r3d(0x104, cp); r3d(0x110, fbz); r3d(0x300, tm);
            u32 am = m_am & ~(8u << 12); if (((am >> 8) & 15) == 15) am &= ~(15u << 8);
            u32 fm = m_fm; if ((fm & 0x21) == 1) fm &= ~0x18u;
            r3d(0x10c, am); r3d(0x108, fm); m_am = am; m_fm = fm;
        }
        if (getenv("RV")) printf("tri %d: cp %08x fbz %08x tm %08x\n", i, cp, fbz, tm);
        m_cp = cp; m_fbz = fbz; m_tm = tm;
        r3d(0x260, 0xFF);                                               /* sSetupMode: everything */
        for (int k = 0; k < 3; k++) {
            r3d(0x264, f2u(rndf(-20, W + 20))); r3d(0x268, f2u(rndf(-20, H + 20)));
            r3d(0x26c, rnd());                                          /* sARGB */
            r3d(0x280, f2u(rndf(0, 65535)));                            /* sVz */
            float w = rndf(0.2f, 2.0f);
            r3d(0x284, f2u(w)); r3d(0x288, f2u(w)); r3d(0x294, f2u(w));
            r3d(0x28c, f2u(rndf(0, 64) * w)); r3d(0x290, f2u(rndf(0, 64) * w));
            r3d(0x298, f2u(rndf(0, 64) * w)); r3d(0x29c, f2u(rndf(0, 64) * w));
            r3d(k ? 0x2a0 : 0x2a4, 0);                                  /* sBeginTriCMD, sDrawTriCMD */
        }
        if (getenv("RV")) idle();
        if (getenv("RDUMP") && i == atoi(getenv("RN") ? getenv("RN") : "0")) {
            idle();
            FILE *f = fopen(getenv("RDUMP"), "wb");
            fwrite(v3_test_vram(), 1, 0x200000 + STRIDE * H, f);
            fclose(f);
            exit(0);
        }
        if (!jit_cmp && i % 25 == 24) { idle(); printf("random %3d: %08x\n", i + 1, sum_vram()); if (getenv("SHOTSUM")) grab(); }
    }
    return 0;
}

int main(int argc, char **argv)
{
    int bad = 0;
    setvbuf(stdout, 0, _IONBF, 0);
    shot = calloc(W * H, 4);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "direct")) direct = 1;
        if (!strcmp(argv[i], "slice")) slice = 1;
        if (!strcmp(argv[i], "jit")) host_osfxsr = 1;
        if (!strcmp(argv[i], "jitcmp")) host_osfxsr = jit_cmp = 1;
        if (!strncmp(argv[i], "mp=", 3)) mp_cpus = atoi(argv[i] + 3);
        if (!strcmp(argv[i], "mpfault")) mp_fault = 1;
        if (!strncmp(argv[i], "par=", 4)) par_cpus = atoi(argv[i] + 4);
    }
    v3_init();
    if (!v3_on) return 1;
    v3_mp_start();

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
    v3_mmio_write(BAR0 + 0x100000 + 0x70, 0, 4);          /* a 2D command (NOP) right after: its launch waits for */
    idle();                                                /* the render thread to draw the triangle (UT's hang) */
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

    /* the FIFO again: a jump back to its base, then red into BACK from there */
    lfb(a, (u32)FIFO << 4 | 3 << 3); a += 4;              /* packet 0: JMP to the base */
    a = FIFO;
    lfb(a, 1 | (0x148 >> 2) << 3 | 1 << 16); a += 4;       /* packet 1: color1 */
    lfb(a, 0x00FF0000); a += 4;
    lfb(a, 1 | (0x124 >> 2) << 3 | 1 << 16); a += 4;      /* packet 1: fastfillCMD */
    lfb(a, 0); a += 4;
    idle();
    r3d(0x128, 1);                                         /* swapbufferCMD: shown anew */
    for (int i = 0; i < 5; i++) { fake_clock += 17045; v3_tick(); }
    idle();
    printf("command FIFO again: read pointer %x (wrote up to %x)\n", v3_mmio_read(BAR0 + 0x8002c, 4), a);
    grab();
    bad += !check("FIFO from its base again: red", 100, 100, 0xFF0000);

    if (getenv("RSEED")) rnd_state = strtoul(getenv("RSEED"), 0, 0);
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "dacbench")) {    /* the Windows driver's gamma table: each */
        v3_mmio_write(BAR0 + 0x50, 0, 4);
        for (int k = 0; k < 4; k++) printf("dacbench: status after a DAC write: %08x\n", v3_mmio_read(BAR0, 4));
        u32 t0 = fake_clock;                                               /* entry after waiting for the retrace, */
        for (int e = 0; e < 768; e++) {                                    /* every access ~25 us (a nested VM exit) */
            while (!(v3_mmio_read(BAR0, 4) & 0x40)) fake_clock += 30;      /* out of a retrace, */
            while (v3_mmio_read(BAR0, 4) & 0x40) fake_clock += 30;         /* then into a new one */
            v3_mmio_write(BAR0 + 0x50, e & 255, 4); fake_clock += 30;
            v3_mmio_write(BAR0 + 0x54, 0x010101u * (e & 255), 4); fake_clock += 30;
        }
        printf("dacbench: 768 entries in %u ms of card time\n", (fake_clock - t0) * 1000u / 1193182u);
    }
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "fillbench")) {   /* 4 MB into the frame buffer a dword at a time, */
        struct timespec t0, t1;                                            /* as a trapped fill (a mode switch's clear) */
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (u32 a = 0; a < (4u << 20); a += 4) v3_mmio_write(BAR1 + (8u << 20) + a, 0x12345678, 4);
        v3_settle();
        clock_gettime(CLOCK_MONOTONIC, &t1);
        printf("fillbench: 4 MB a dword at a time: %.0f ms\n", (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
    }
    for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "random")) { random_tris(getenv("RCOUNT") ? atoi(getenv("RCOUNT")) : 400); grab(); }
    if (getenv("SHOTSUM")) {                   /* the same memory as a tiled desktop (128x32-byte tiles), and half mode */
        io(0xe8, (STRIDE / 128) | (STRIDE / 128) << 16);
        io(0x5c, 1 | 1 << 18 | 1 << 10 | 1 << 11 | 1 << 24);
        grab();
        io(0x5c, 1 | 1 << 18 | 1 << 10 | 1 << 11 | 1 << 24 | 1 << 4);
        grab();
    }

    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        fprintf(f, "P6 %d %d 255\n", W, H);
        for (int i = 0; i < W * H; i++) { u8 p[3] = { shot[i] >> 16, shot[i] >> 8, shot[i] }; fwrite(p, 1, 3, f); }
        fclose(f);
    }
    printf("%s\n", bad ? "FAILED" : "all good");
    return bad != 0;
}
