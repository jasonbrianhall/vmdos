/* A 3dfx Voodoo 3 for the DOS guest: 86Box's Voodoo 3D and 2D engines
   (vid_voodoo_*.c, as upstream) behind the Voodoo 3's own registers, which
   come from 86Box's vid_voodoo_banshee.c (Sarah Walker, the 86Box team;
   GPL v2) with its SVGA side taken out: vmdos's VGA (video.c) is the VGA,
   and the card's VGA registers in its I/O space lead there.

   The card: PCI 121Ah:0005h, 16 MB. BAR0 (32 MB) holds the registers (I/O
   remap, command FIFO, 2D, 3D, texture download, 3D LFB), BAR1 (32 MB) the
   linear frame buffer (where Glide writes its command FIFO), BAR2 256 I/O
   ports. Both memory BARs sit at linear addresses with nothing mapped, so
   every access faults and the instruction is emulated (mememu.c) into
   v3_mmio_read / v3_mmio_write. The engines run on green threads
   (v3sched.c); everything here runs on the boot CPU with the guest's x87
   state put aside (v3_enter). The screen: v3_display / v3_row, called by
   video.c's redraw, show the desktop surface and the overlay (where Glide
   puts the 3D picture) once the video processor is on. */
#include <voodoo_shim.h>
#include <86box/vid_voodoo_common.h>
#include <86box/vid_voodoo_banshee_blitter.h>
#include <86box/vid_voodoo_display.h>
#include <86box/vid_voodoo_fb.h>
#include <86box/vid_voodoo_fifo.h>
#include <86box/vid_voodoo_regs.h>
#include <86box/vid_voodoo_render.h>
#include <86box/vid_voodoo_texture.h>
#include "../voodoo3.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

/* ---------------- the kernel ---------------- */
extern char cmdline[512];
void *phys_alloc(u32 bytes);
void *phys_try_alloc(u32 bytes);
u32 linear_hole(u32 len, u32 align);
u32 video_port_in(u16 port);
void video_port_out(u16 port, u8 v);
int video_vretrace(void);
u32 video_frame_no(void);
extern u8 vga_dac[256][3];
extern u8 crtc[64];
void v3_run(void);
void map_page(u32 lin, u32 phys, u32 flags);
void tlb_flush_range(u32 lin, u32 len);
void hv_map_gpa(u32 gpa, u32 host);
void v3_poll_timers(void);
void v3_sched_init(void);

/* ---------------- what the Voodoo code expects of 86Box ---------------- */
int v3_log_on;
int cycles;
int pci_burst_time = 1, pci_nonburst_time = 1;
uint32_t mem_size;
double cpuclock = 33333333.0;
cpu_state_t cpu_state;
monitor_t monitors[1];
int changeframecount = 2;

static u8 *arena;                              /* small allocations: one block, never given back */
static u32 arena_left;
void *v3_calloc(size_t n, size_t sz)
{
    u32 len = (u32)(n * sz);
    if (len >= 32768) {
        void *p = phys_try_alloc((len + 4095) & ~4095u);
        if (!p) panic("voodoo3: out of memory (%u bytes)", len);
        return p;                              /* zeroed */
    }
    len = (len + 15) & ~15u;
    if (len > arena_left) {
        arena = phys_alloc(256 * 1024);
        arena_left = 256 * 1024;
    }
    void *p = arena;
    arena += len; arena_left -= len;
    return p;
}
void v3_free(void *p) { (void)p; }

void v3_fatal(const char *fmt, ...)
{
    char b[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    kprintf("voodoo3: %s", b);                /* 86Box stops; vmdos goes on */
}

uint32_t mem_readl_phys(uint32_t addr)        /* AGP: none, it's a PCI card */
{
    (void)addr;
    return 0xFFFFFFFFu;
}

static int config_int(const char *n)
{
    if (!strcmp(n, "bilinear")) return 1;
    if (!strcmp(n, "dithersub")) return 1;
    if (!strcmp(n, "dacfilter")) return 0;
    if (!strcmp(n, "render_threads")) return 1;
    if (!strcmp(n, "recompiler")) return 0;
    if (!strcmp(n, "chromakey")) return 1;
    if (!strcmp(n, "memory")) return 16;
    return 0;
}
int device_get_config_int(const char *name) { return config_int(name); }

/* ---------------- the card ---------------- */
#define VRAM_MB 16
/* Its 256 I/O ports at D100h, not the usual D000h: Windows 9x without a
   PnP BIOS takes the board's own devices (DMA, PIC, PIT, keyboard: 00h-FFh)
   as ISA with 10-bit decoding, so their aliases cover every xx00h-xxFFh
   with xx & 3 == 0, and a card there gets Code 12 with nothing in the
   conflict list. D100h-D1FFh aliases 100h-1FFh, where nothing is. */
#define V3_IO_BASE 0xD100

typedef struct banshee_t {
    u8 *vram;
    u32 vram_size, vram_mask;
    u8 *changedvram;

    uint8_t pci_regs[256];

    uint32_t memBaseAddr0;
    uint32_t memBaseAddr1;
    uint32_t ioBaseAddr;

    uint32_t agpInit0;
    uint32_t dramInit0, dramInit1;
    uint32_t lfbMemoryConfig;
    uint32_t miscInit0, miscInit1;
    uint32_t pciInit0;
    uint32_t vgaInit0, vgaInit1;

    uint32_t command_2d;
    uint32_t srcBaseAddr_2d;

    uint32_t pllCtrl0, pllCtrl1, pllCtrl2;

    uint32_t dacMode;
    int      dacAddr;
    uint32_t pallook[512];                     /* the video processor's CLUTs (gamma) */
    int      pal_written;

    uint32_t vidDesktopOverlayStride;
    uint32_t vidDesktopStartAddr;
    uint32_t vidProcCfg;
    uint32_t vidScreenSize;
    uint32_t vidSerialParallelPort;
    uint32_t vidChromaKeyMin;
    uint32_t vidChromaKeyMax;

    uint32_t agpReqSize;
    uint32_t agpHostAddressHigh;
    uint32_t agpHostAddressLow;
    uint32_t agpGraphicsAddress;
    uint32_t agpGraphicsStride;
    uint32_t agpMoveCMD;

    int overlay_pix_fmt;

    uint32_t hwCurPatAddr, hwCurLoc, hwCurC0, hwCurC1;

    uint32_t intrCtrl;

    voodoo_t *voodoo;
    voodoo_set_t set;

    uint32_t overlay_addr;                     /* the overlay buffer shown (latched at the swap) */
    u32 last_frame;
} banshee_t;

enum {
    Init_status          = 0x00,
    Init_pciInit0        = 0x04,
    Init_lfbMemoryConfig = 0x0c,
    Init_miscInit0       = 0x10,
    Init_miscInit1       = 0x14,
    Init_dramInit0       = 0x18,
    Init_dramInit1       = 0x1c,
    Init_agpInit0        = 0x20,
    Init_vgaInit0        = 0x28,
    Init_vgaInit1        = 0x2c,
    Init_2dCommand       = 0x30,
    Init_2dSrcBaseAddr   = 0x34,
    Init_strapInfo       = 0x38,

    PLL_pllCtrl0 = 0x40,
    PLL_pllCtrl1 = 0x44,
    PLL_pllCtrl2 = 0x48,

    DAC_dacMode = 0x4c,
    DAC_dacAddr = 0x50,
    DAC_dacData = 0x54,

    Video_vidProcCfg                   = 0x5c,
    Video_maxRgbDelta                  = 0x58,
    Video_hwCurPatAddr                 = 0x60,
    Video_hwCurLoc                     = 0x64,
    Video_hwCurC0                      = 0x68,
    Video_hwCurC1                      = 0x6c,
    Video_vidSerialParallelPort        = 0x78,
    Video_vidChromaKeyMin              = 0x8c,
    Video_vidChromaKeyMax              = 0x90,
    Video_vidScreenSize                = 0x98,
    Video_vidOverlayStartCoords        = 0x9c,
    Video_vidOverlayEndScreenCoords    = 0xa0,
    Video_vidOverlayDudx               = 0xa4,
    Video_vidOverlayDudxOffsetSrcWidth = 0xa8,
    Video_vidOverlayDvdy               = 0xac,
    Video_vidOverlayDvdyOffset         = 0xe0,
    Video_vidDesktopStartAddr          = 0xe4,
    Video_vidDesktopOverlayStride      = 0xe8,
};

enum {
    cmdBaseAddr0  = 0x20,
    cmdBaseSize0  = 0x24,
    cmdBump0      = 0x28,
    cmdRdPtrL0    = 0x2c,
    cmdRdPtrH0    = 0x30,
    cmdAMin0      = 0x34,
    cmdAMax0      = 0x3c,
    cmdStatus0    = 0x40,
    cmdFifoDepth0 = 0x44,
    cmdHoleCnt0   = 0x48,

    cmdBaseAddr1  = 0x50,
    cmdBaseSize1  = 0x50 + 0x4,
    cmdBump1      = 0x50 + 0x8,
    cmdRdPtrL1    = 0x50 + 0xc,
    cmdRdPtrH1    = 0x50 + 0x10,
    cmdAMin1      = 0x50 + 0x14,
    cmdAMax1      = 0x50 + 0x1c,
    cmdStatus1    = 0x50 + 0x20,
    cmdFifoDepth1 = 0x50 + 0x24,
    cmdHoleCnt1   = 0x50 + 0x28,

    Agp_agpReqSize         = 0x00,
    Agp_agpHostAddressLow  = 0x04,
    Agp_agpHostAddressHigh = 0x08,
    Agp_agpGraphicsAddress = 0x0C,
    Agp_agpGraphicsStride  = 0x10,
    Agp_agpMoveCMD         = 0x14,
};

#define VGAINIT0_RAMDAC_8BIT                (1 << 2)
#define VGAINIT0_EXTENDED_SHIFT_OUT         (1 << 12)

#define VIDPROCCFG_VIDPROC_ENABLE           (1 << 0)
#define VIDPROCCFG_CURSOR_MODE              (1 << 1)
#define VIDPROCCFG_INTERLACE                (1 << 3)
#define VIDPROCCFG_HALF_MODE                (1 << 4)
#define VIDPROCCFG_OVERLAY_ENABLE           (1 << 8)
#define VIDPROCCFG_DESKTOP_CLUT_BYPASS      (1 << 10)
#define VIDPROCCFG_OVERLAY_CLUT_BYPASS      (1 << 11)
#define VIDPROCCFG_DESKTOP_CLUT_SEL         (1 << 12)
#define VIDPROCCFG_OVERLAY_CLUT_SEL         (1 << 13)
#define VIDPROCCFG_H_SCALE_ENABLE           (1 << 14)
#define VIDPROCCFG_V_SCALE_ENABLE           (1 << 15)
#define VIDPROCCFG_DESKTOP_PIX_FORMAT       ((banshee->vidProcCfg >> 18) & 7)
#define VIDPROCCFG_OVERLAY_PIX_FORMAT_SHIFT (21)
#define VIDPROCCFG_OVERLAY_PIX_FORMAT_MASK  (7 << VIDPROCCFG_OVERLAY_PIX_FORMAT_SHIFT)
#define VIDPROCCFG_DESKTOP_TILE             (1 << 24)
#define VIDPROCCFG_OVERLAY_TILE             (1 << 25)
#define VIDPROCCFG_2X_MODE                  (1 << 26)
#define VIDPROCCFG_HWCURSOR_ENA             (1 << 27)

#define OVERLAY_START_X_MASK                (0xfff)
#define OVERLAY_START_Y_SHIFT               (12)
#define OVERLAY_START_Y_MASK                (0xfff << OVERLAY_START_Y_SHIFT)
#define OVERLAY_END_X_MASK                  (0xfff)
#define OVERLAY_END_Y_SHIFT                 (12)
#define OVERLAY_END_Y_MASK                  (0xfff << OVERLAY_END_Y_SHIFT)
#define OVERLAY_SRC_WIDTH_SHIFT             (19)
#define OVERLAY_SRC_WIDTH_MASK              (0x1fff << OVERLAY_SRC_WIDTH_SHIFT)
#define VID_STRIDE_OVERLAY_SHIFT            (16)
#define VID_STRIDE_OVERLAY_MASK             (0x7fff << VID_STRIDE_OVERLAY_SHIFT)
#define VID_DUDX_MASK                       (0xffffff)
#define VID_DVDY_MASK                       (0xffffff)

#define PIX_FORMAT_8                        0
#define PIX_FORMAT_RGB565                   1
#define PIX_FORMAT_RGB24                    2
#define PIX_FORMAT_RGB32                    3

#define VIDSERIAL_DDC_DCK_W                 (1 << 19)
#define VIDSERIAL_DDC_DDA_W                 (1 << 20)
#define VIDSERIAL_DDC_DCK_R                 (1 << 21)
#define VIDSERIAL_DDC_DDA_R                 (1 << 22)
#define VIDSERIAL_I2C_SCK_R                 (1 << 26)
#define VIDSERIAL_I2C_SDA_R                 (1 << 27)

#define MISCINIT0_Y_ORIGIN_SWAP_SHIFT       (18)
#define MISCINIT0_Y_ORIGIN_SWAP_MASK        (0xfff << MISCINIT0_Y_ORIGIN_SWAP_SHIFT)

int v3_on;
static banshee_t *card;
static u32 bar0_lin, bar1_lin;                 /* where the BARs were put: unmapped linear space */

/* BAR1 pages mapped straight onto vram (lfb_remap) */
static u8 *lfb_mapped;
static int lfb_direct, lfb_dirty;

#define banshee_log(...) do { if (v3_log_on > 1) kprintf("voodoo3: " __VA_ARGS__); } while (0)

/* ---------------- the guest's x87 state, around the card's code ----------------
   The card's code uses the x87 (triangle setup, LOD); the guest's state is
   in the registers (vmdos never switches it), so it is saved on the way in
   and put back on the way out. */
static int depth;
static u8 fpu_save[108] __attribute__((aligned(16)));
static void v3_enter(void)
{
    if (depth++) return;
    __asm__ volatile("fnsave %0\n\tfninit" : "=m"(fpu_save));
}
static void v3_leave(void)
{
    if (--depth) return;
    __asm__ volatile("frstor %0" : : "m"(fpu_save));
}

/* ---------------- retrace and swaps ---------------- */
static void banshee_vsync_callback(banshee_t *banshee)
{
    voodoo_t *voodoo = banshee->voodoo;

    voodoo->retrace_count++;
    thread_wait_mutex(voodoo->swap_mutex);
    if (voodoo->swap_pending && (voodoo->retrace_count > voodoo->swap_interval)) {
        if (voodoo->swap_count > 0)
            voodoo->swap_count--;
        voodoo->swap_pending = 0;
        thread_release_mutex(voodoo->swap_mutex);

        memset(voodoo->dirty_line, 1, sizeof(voodoo->dirty_line));
        voodoo->retrace_count = 0;
        banshee->overlay_addr = voodoo->leftOverlayBuf & 0xffffff;
        thread_set_event(voodoo->wake_fifo_thread);
        voodoo->frame_count++;
    } else
        thread_release_mutex(voodoo->swap_mutex);
}

/* A vertical retrace since the last look: what the card does at one. */
static void check_vsync(void)
{
    u32 f = video_frame_no();
    if (f != card->last_frame) {
        card->last_frame = f;
        banshee_vsync_callback(card);
    }
}

/* Called by vid_voodoo_reg.c when a swap needs no retrace. */
void banshee_set_overlay_addr(void *priv, UNUSED(uint32_t addr))
{
    banshee_t *banshee = (banshee_t *) priv;
    banshee->overlay_addr = banshee->voodoo->leftOverlayBuf & 0xffffff;
    memset(banshee->voodoo->dirty_line, 1, sizeof(banshee->voodoo->dirty_line));
}

/* 86Box's Banshee generates these filter tables for its video output; the
   output here doesn't filter, but vid_voodoo_display.c links to it. */
void voodoo_generate_vb_filters(voodoo_t *voodoo, int fcr, int fcg) { (void)voodoo; (void)fcr; (void)fcg; }

/* ---------------- I/O space (BAR2, and the I/O remap in BAR0) ---------------- */
static uint32_t banshee_ext_inl(uint16_t addr, void *priv);
static void     banshee_ext_outl(uint16_t addr, uint32_t val, void *priv);
static void acc_log(const char *what, u32 a, u32 v);

static void banshee_out(uint16_t addr, uint8_t val) { video_port_out(addr, val); }
static uint8_t banshee_in(uint16_t addr) { return (uint8_t)video_port_in(addr); }

static void
banshee_ext_out(uint16_t addr, uint8_t val, void *priv)
{
    uint32_t reg;
    uint32_t shift;

    switch (addr & 0xff) {
        case 0xb0 ... 0xdf:
            acc_log("vga out", (addr & 0xff) + 0x300, val);
            banshee_out((addr & 0xff) + 0x300, val);
            break;

        default:
            shift = (addr & 3) << 3;
            reg   = banshee_ext_inl(addr & 0xfc, priv);
            banshee_ext_outl(addr & 0xfc, (reg & ~(0xff << shift)) | (val << shift), priv);
            break;
    }
}

static void
banshee_ext_outw(uint16_t addr, uint16_t val, void *priv)
{
    uint32_t reg;
    uint32_t shift;

    switch (addr & 0xff) {
        case 0xb0 ... 0xdf:
            banshee_ext_out(addr, val & 0xff, priv);
            banshee_ext_out(addr + 1, val >> 8, priv);
            break;

        default:
            shift = (addr & 2) << 3;
            reg   = banshee_ext_inl(addr & 0xfc, priv);
            banshee_ext_outl(addr & 0xfc, (reg & ~(0xffff << shift)) | (val << shift), priv);
            break;
    }
}

/* v3log: the first accesses to the card's I/O registers and configuration
   space in the log, to see what a driver asks of it (not the FIFO, the 3D
   or the frame buffer). */
static int acc_left = 4000;
static void acc_log(const char *what, u32 a, u32 v)
{
    if (!v3_log_on || acc_left <= 0) return;
    if (!--acc_left) kprintf("voodoo3: access log full\n");
    else kprintf("voodoo3: %s %02x %08x\n", what, a, v);
}

static void
banshee_ext_outl(uint16_t addr, uint32_t val, void *priv)
{
    banshee_t *banshee = (banshee_t *) priv;
    voodoo_t  *voodoo  = banshee->voodoo;

    acc_log("io out", addr & 0xff, val);

    switch (addr & 0xff) {
        case Init_pciInit0:
            banshee->pciInit0 = val;
            break;

        case Init_lfbMemoryConfig:
            banshee->lfbMemoryConfig  = val;
            voodoo->tile_base         = (val & 0x1fff) << 12;
            voodoo->tile_stride       = 1024 << ((val >> 13) & 7);
            voodoo->tile_stride_shift = 10 + ((val >> 13) & 7);
            voodoo->tile_x            = ((val >> 16) & 0x7f) * 128;
            voodoo->tile_x_real       = ((val >> 16) & 0x7f) * 128 * 32;
            lfb_dirty = 1;
            break;

        case Init_miscInit0:
            banshee->miscInit0    = val;
            voodoo->y_origin_swap = (val & MISCINIT0_Y_ORIGIN_SWAP_MASK) >> MISCINIT0_Y_ORIGIN_SWAP_SHIFT;
            break;
        case Init_miscInit1:
            banshee->miscInit1 = val;
            break;
        case Init_dramInit0:
            banshee->dramInit0 = val;
            break;
        case Init_dramInit1:
            banshee->dramInit1 = val;
            break;
        case Init_agpInit0:
            banshee->agpInit0 = val;
            break;

        case Init_2dCommand:
            banshee->command_2d = val;
            break;
        case Init_2dSrcBaseAddr:
            banshee->srcBaseAddr_2d = val;
            break;
        case Init_vgaInit0:
            if ((val ^ banshee->vgaInit0) & VGAINIT0_EXTENDED_SHIFT_OUT)
                kprintf("voodoo3: extended shift out %s\n", (val & VGAINIT0_EXTENDED_SHIFT_OUT) ? "on" : "off");
            banshee->vgaInit0 = val;
            break;
        case Init_vgaInit1:
            banshee->vgaInit1 = val;
            break;

        case PLL_pllCtrl0:
            banshee->pllCtrl0 = val;
            break;
        case PLL_pllCtrl1:
            banshee->pllCtrl1 = val;
            break;
        case PLL_pllCtrl2:
            banshee->pllCtrl2 = val;
            break;

        case DAC_dacMode:
            banshee->dacMode = val;
            break;
        case DAC_dacAddr:
            banshee->dacAddr = val & 0x1ff;
            break;
        case DAC_dacData:
            banshee->pallook[banshee->dacAddr] = val & 0xffffff;
            banshee->pal_written = 1;
            break;

        case Video_vidProcCfg:
            if ((val ^ banshee->vidProcCfg) & (VIDPROCCFG_VIDPROC_ENABLE | VIDPROCCFG_OVERLAY_ENABLE))
                kprintf("voodoo3: video processor %s, overlay %s (vidProcCfg %08x)\n",
                        (val & VIDPROCCFG_VIDPROC_ENABLE) ? "on" : "off", (val & VIDPROCCFG_OVERLAY_ENABLE) ? "on" : "off", val);
            banshee->vidProcCfg      = val;
            banshee->overlay_pix_fmt = (val & VIDPROCCFG_OVERLAY_PIX_FORMAT_MASK) >> VIDPROCCFG_OVERLAY_PIX_FORMAT_SHIFT;
            break;

        case Video_maxRgbDelta:
            banshee->voodoo->scrfilterThreshold = val;
            banshee->voodoo->scrfilterEnabled   = val > 0;
            voodoo_threshold_check(banshee->voodoo);
            break;

        case Video_hwCurPatAddr:
            banshee->hwCurPatAddr = val;
            break;
        case Video_hwCurLoc:
            banshee->hwCurLoc = val;
            break;
        case Video_hwCurC0:
            banshee->hwCurC0 = val;
            break;
        case Video_hwCurC1:
            banshee->hwCurC1 = val;
            break;

        case Video_vidSerialParallelPort:
            banshee->vidSerialParallelPort = val;
            break;

        case Video_vidChromaKeyMin:
            banshee->vidChromaKeyMin = val;
            break;
        case Video_vidChromaKeyMax:
            banshee->vidChromaKeyMax = val;
            break;

        case Video_vidScreenSize:
            banshee->vidScreenSize = val;
            voodoo->h_disp         = (val & 0xfff) + 1;
            voodoo->v_disp         = (val >> 12) & 0xfff;
            break;
        case Video_vidOverlayStartCoords:
            voodoo->overlay.vidOverlayStartCoords = val;
            voodoo->overlay.start_x               = val & OVERLAY_START_X_MASK;
            voodoo->overlay.start_y               = (val & OVERLAY_START_Y_MASK) >> OVERLAY_START_Y_SHIFT;
            voodoo->overlay.size_x                = voodoo->overlay.end_x - voodoo->overlay.start_x;
            voodoo->overlay.size_y                = voodoo->overlay.end_y - voodoo->overlay.start_y;
            break;
        case Video_vidOverlayEndScreenCoords:
            voodoo->overlay.vidOverlayEndScreenCoords = val;
            voodoo->overlay.end_x                     = val & OVERLAY_END_X_MASK;
            voodoo->overlay.end_y                     = (val & OVERLAY_END_Y_MASK) >> OVERLAY_END_Y_SHIFT;
            voodoo->overlay.size_x                    = (voodoo->overlay.end_x - voodoo->overlay.start_x) + 1;
            voodoo->overlay.size_y                    = (voodoo->overlay.end_y - voodoo->overlay.start_y) + 1;
            break;
        case Video_vidOverlayDudx:
            voodoo->overlay.vidOverlayDudx = val & VID_DUDX_MASK;
            break;
        case Video_vidOverlayDudxOffsetSrcWidth:
            voodoo->overlay.vidOverlayDudxOffsetSrcWidth = val;
            voodoo->overlay.overlay_bytes                = (val & OVERLAY_SRC_WIDTH_MASK) >> OVERLAY_SRC_WIDTH_SHIFT;
            break;
        case Video_vidOverlayDvdy:
            voodoo->overlay.vidOverlayDvdy = val & VID_DVDY_MASK;
            break;
        case Video_vidOverlayDvdyOffset:
            voodoo->overlay.vidOverlayDvdyOffset = val;
            break;

        case Video_vidDesktopStartAddr:
            banshee->vidDesktopStartAddr = val & 0xffffff;
            break;
        case Video_vidDesktopOverlayStride:
            banshee->vidDesktopOverlayStride = val;
            break;
        default:
            break;
    }
}

static uint8_t
banshee_ext_in(uint16_t addr, void *priv)
{
    switch (addr & 0xff) {
        case 0xb0 ... 0xdf:
            return banshee_in((addr & 0xff) + 0x300);
        default:
            return banshee_ext_inl(addr & 0xfc, priv) >> ((addr & 3) << 3);
    }
}

static uint16_t
banshee_ext_inw(uint16_t addr, void *priv)
{
    uint16_t ret;

    switch (addr & 0xff) {
        case 0xb0 ... 0xdf:
            ret = banshee_ext_in(addr, priv);
            ret |= banshee_ext_in(addr + 1, priv) << 8;
            break;

        default:
            ret = banshee_ext_inl(addr & 0xfc, priv) >> ((addr & 2) << 3);
            break;
    }

    return ret;
}

static uint32_t
banshee_status(banshee_t *banshee)
{
    voodoo_t *voodoo       = banshee->voodoo;
    int       fifo_entries = FIFO_ENTRIES;
    int       swap_count   = voodoo->swap_count;
    int       written      = voodoo->cmd_written + voodoo->cmd_written_fifo;
    int       busy         = (written - voodoo->cmd_read) || (voodoo->cmdfifo_depth_rd != voodoo->cmdfifo_depth_wr) || (voodoo->cmdfifo_depth_rd_2 != voodoo->cmdfifo_depth_wr_2) || RENDER_VOODOO_BUSY(voodoo, 0) || RENDER_VOODOO_BUSY(voodoo, 1) || RENDER_VOODOO_BUSY(voodoo, 2) || RENDER_VOODOO_BUSY(voodoo, 3) || voodoo->voodoo_busy;
    uint32_t  ret          = 0;

    if (fifo_entries < 0x20)
        ret |= 0x1f - fifo_entries;
    else
        ret |= 0x1f;
    if (fifo_entries)
        ret |= 0x20;
    if (swap_count < 7)
        ret |= (swap_count << 28);
    else
        ret |= (7 << 28);
    if (video_vretrace())
        ret |= 0x40;

    if (busy)
        ret |= 0x780; /*Busy*/

    if (voodoo->cmdfifo_depth_rd != voodoo->cmdfifo_depth_wr)
        ret |= (1 << 11);

    if (voodoo->cmdfifo_depth_rd_2 != voodoo->cmdfifo_depth_wr_2)
        ret |= (1 << 12);

    if (!voodoo->voodoo_busy)
        voodoo_wake_fifo_thread(voodoo);

    return ret;
}

static uint32_t
banshee_ext_inl(uint16_t addr, void *priv)
{
    banshee_t      *banshee = (banshee_t *) priv;
    const voodoo_t *voodoo  = banshee->voodoo;
    uint32_t        ret     = 0xffffffff;

    switch (addr & 0xff) {
        case Init_status:
            ret = banshee_status(banshee);
            break;
        case Init_pciInit0:
            ret = banshee->pciInit0;
            break;
        case Init_lfbMemoryConfig:
            ret = banshee->lfbMemoryConfig;
            break;

        case Init_miscInit0:
            ret = banshee->miscInit0;
            break;
        case Init_miscInit1:
            ret = banshee->miscInit1;
            break;
        case Init_dramInit0:
            ret = banshee->dramInit0;
            break;
        case Init_dramInit1:
            ret = banshee->dramInit1;
            break;
        case Init_agpInit0:
            ret = banshee->agpInit0;
            break;

        case Init_vgaInit0:
            ret = banshee->vgaInit0;
            break;
        case Init_vgaInit1:
            ret = banshee->vgaInit1;
            break;

        case Init_2dCommand:
            ret = banshee->command_2d;
            break;
        case Init_2dSrcBaseAddr:
            ret = banshee->srcBaseAddr_2d;
            break;
        case Init_strapInfo:
            ret = 0x00000040; /*8 MB SGRAM, PCI, IRQ enabled, 32kB BIOS*/
            break;

        case PLL_pllCtrl0:
            ret = banshee->pllCtrl0;
            break;
        case PLL_pllCtrl1:
            ret = banshee->pllCtrl1;
            break;
        case PLL_pllCtrl2:
            ret = banshee->pllCtrl2;
            break;

        case DAC_dacMode:
            ret = banshee->dacMode;
            break;
        case DAC_dacAddr:
            ret = banshee->dacAddr;
            break;
        case DAC_dacData:
            ret = banshee->pallook[banshee->dacAddr];
            break;

        case Video_maxRgbDelta:
            ret = voodoo->scrfilterThreshold;
            break;

        case Video_vidProcCfg:
            ret = banshee->vidProcCfg;
            break;

        case Video_hwCurPatAddr:
            ret = banshee->hwCurPatAddr;
            break;
        case Video_hwCurLoc:
            ret = banshee->hwCurLoc;
            break;
        case Video_hwCurC0:
            ret = banshee->hwCurC0;
            break;
        case Video_hwCurC1:
            ret = banshee->hwCurC1;
            break;

        case Video_vidSerialParallelPort:
            /* no monitor on the DDC lines, nothing on the I2C bus: both read high */
            ret = (banshee->vidSerialParallelPort & ~(VIDSERIAL_DDC_DCK_R | VIDSERIAL_DDC_DDA_R | VIDSERIAL_I2C_SCK_R | VIDSERIAL_I2C_SDA_R))
                | VIDSERIAL_DDC_DCK_R | VIDSERIAL_DDC_DDA_R | VIDSERIAL_I2C_SCK_R | VIDSERIAL_I2C_SDA_R;
            break;

        case Video_vidChromaKeyMin:
            ret = banshee->vidChromaKeyMin;
            break;
        case Video_vidChromaKeyMax:
            ret = banshee->vidChromaKeyMax;
            break;
        case Video_vidScreenSize:
            ret = banshee->vidScreenSize;
            break;
        case Video_vidOverlayStartCoords:
            ret = voodoo->overlay.vidOverlayStartCoords;
            break;
        case Video_vidOverlayEndScreenCoords:
            ret = voodoo->overlay.vidOverlayEndScreenCoords;
            break;
        case Video_vidOverlayDudx:
            ret = voodoo->overlay.vidOverlayDudx;
            break;
        case Video_vidOverlayDudxOffsetSrcWidth:
            ret = voodoo->overlay.vidOverlayDudxOffsetSrcWidth;
            break;
        case Video_vidOverlayDvdy:
            ret = voodoo->overlay.vidOverlayDvdy;
            break;
        case Video_vidOverlayDvdyOffset:
            ret = voodoo->overlay.vidOverlayDvdyOffset;
            break;

        case Video_vidDesktopStartAddr:
            ret = banshee->vidDesktopStartAddr;
            break;
        case Video_vidDesktopOverlayStride:
            ret = banshee->vidDesktopOverlayStride;
            break;

        default:
            break;
    }

    if ((addr & 0xff) != Init_status) acc_log("io in ", addr & 0xff, ret);
    return ret;
}

/* ---------------- BAR0: registers ---------------- */
static uint32_t banshee_reg_readl(uint32_t addr, void *priv);

static uint32_t
banshee_cmd_read(banshee_t *banshee, uint32_t addr)
{
    const voodoo_t *voodoo = banshee->voodoo;
    uint32_t        ret    = 0xffffffff;

    switch (addr & 0x1fc) {
        case Agp_agpHostAddressLow:
            ret = banshee->agpHostAddressLow;
            break;
        case Agp_agpHostAddressHigh:
            ret = banshee->agpHostAddressHigh;
            break;
        case Agp_agpGraphicsAddress:
            ret = banshee->agpGraphicsAddress;
            break;
        case Agp_agpGraphicsStride:
            ret = banshee->agpGraphicsStride;
            break;
        case Agp_agpReqSize:
            ret = banshee->agpReqSize;
            break;
        case Agp_agpMoveCMD:
            ret = banshee->agpMoveCMD;
            break;

        case cmdBaseAddr0:
            ret = voodoo->cmdfifo_base >> 12;
            break;
        case cmdRdPtrL0:
            ret = voodoo->cmdfifo_rp;
            break;
        case cmdFifoDepth0:
            ret = voodoo->cmdfifo_depth_wr - voodoo->cmdfifo_depth_rd;
            break;
        case cmdStatus0:
            ret = voodoo->cmd_status;
            break;
        case cmdBaseSize0:
            ret = voodoo->cmdfifo_size;
            if (voodoo->cmdfifo_enabled)
                ret |= 0x100;
            if (voodoo->cmdfifo_in_agp)
                ret |= 0x200;
            break;

        case cmdBaseAddr1:
            ret = voodoo->cmdfifo_base_2 >> 12;
            break;
        case cmdRdPtrL1:
            ret = voodoo->cmdfifo_rp_2;
            break;
        case cmdFifoDepth1:
            ret = voodoo->cmdfifo_depth_wr_2 - voodoo->cmdfifo_depth_rd_2;
            break;
        case cmdStatus1:
            ret = voodoo->cmd_status_2;
            break;
        case cmdBaseSize1:
            ret = voodoo->cmdfifo_size_2;
            if (voodoo->cmdfifo_enabled_2)
                ret |= 0x100;
            if (voodoo->cmdfifo_in_agp_2)
                ret |= 0x200;
            break;

        case cmdRdPtrH0:
        case cmdRdPtrH1:
            ret = 0;
            break;
        case cmdAMin0:
            ret = voodoo->cmdfifo_amin;
            break;
        case cmdAMax0:
            ret = voodoo->cmdfifo_amax;
            break;
        case cmdHoleCnt0:
            ret = voodoo->cmdfifo_holecount;
            break;
        case cmdAMin1:
            ret = voodoo->cmdfifo_amin_2;
            break;
        case cmdAMax1:
            ret = voodoo->cmdfifo_amax_2;
            break;
        case cmdHoleCnt1:
            ret = voodoo->cmdfifo_holecount_2;
            break;

        case 0x108:
            break;

        default:
            banshee_log("unknown cmd read 0x%08x\n", addr);
            break;
    }

    return ret;
}

static uint32_t
banshee_reg_readl(uint32_t addr, void *priv)
{
    banshee_t *banshee = (banshee_t *) priv;
    voodoo_t  *voodoo  = banshee->voodoo;
    uint32_t   ret     = 0xffffffff;

    switch (addr & 0x1f00000) {
        case 0x0000000: /*IO remap*/
            if (!(addr & 0x80000))
                ret = banshee_ext_inl(addr & 0xff, banshee);
            else
                ret = banshee_cmd_read(banshee, addr);
            break;

        case 0x0100000: /*2D registers*/
            voodoo_flush(voodoo);
            switch (addr & 0x1fc) {
                case SST_status:
                    ret = banshee_status(banshee);
                    break;
                case SST_intrCtrl:
                    ret = banshee->intrCtrl & 0x0030003f;
                    break;
                case 0x08:
                    ret = voodoo->banshee_blt.clip0Min;
                    break;
                case 0x0c:
                    ret = voodoo->banshee_blt.clip0Max;
                    break;
                case 0x10:
                    ret = voodoo->banshee_blt.dstBaseAddr;
                    break;
                case 0x14:
                    ret = voodoo->banshee_blt.dstFormat;
                    break;
                case 0x34:
                    ret = voodoo->banshee_blt.srcBaseAddr;
                    break;
                case 0x38:
                    ret = voodoo->banshee_blt.commandExtra;
                    break;
                case 0x5c:
                    ret = voodoo->banshee_blt.srcXY;
                    break;
                case 0x60:
                    ret = voodoo->banshee_blt.colorBack;
                    break;
                case 0x64:
                    ret = voodoo->banshee_blt.colorFore;
                    break;
                case 0x68:
                    ret = voodoo->banshee_blt.dstSize;
                    break;
                case 0x6c:
                    ret = voodoo->banshee_blt.dstXY;
                    break;
                case 0x70:
                    ret = voodoo->banshee_blt.command;
                    break;
                default:
                    banshee_log("2D read %08x\n", addr);
            }
            break;

        case 0x0200000:
        case 0x0300000:
        case 0x0400000:
        case 0x0500000: /*3D registers*/
            switch (addr & 0x3fc) {
                case SST_status:
                    ret = banshee_status(banshee);
                    break;
                case SST_intrCtrl:
                    ret = banshee->intrCtrl & 0x0030003f;
                    break;
                case SST_fbzColorPath:
                    voodoo_flush(voodoo);
                    ret = voodoo->params.fbzColorPath;
                    break;
                case SST_fogMode:
                    voodoo_flush(voodoo);
                    ret = voodoo->params.fogMode;
                    break;
                case SST_alphaMode:
                    voodoo_flush(voodoo);
                    ret = voodoo->params.alphaMode;
                    break;
                case SST_fbzMode:
                    voodoo_flush(voodoo);
                    ret = voodoo->params.fbzMode;
                    break;
                case SST_lfbMode:
                    voodoo_flush(voodoo);
                    ret = voodoo->lfbMode;
                    break;
                case SST_clipLeftRight:
                    ret = voodoo->params.clipRight | (voodoo->params.clipLeft << 16);
                    break;
                case SST_clipLowYHighY:
                    ret = voodoo->params.clipHighY | (voodoo->params.clipLowY << 16);
                    break;
                case SST_clipLeftRight1:
                    ret = voodoo->params.clipRight1 | (voodoo->params.clipLeft1 << 16);
                    break;
                case SST_clipTopBottom1:
                    ret = voodoo->params.clipHighY1 | (voodoo->params.clipLowY1 << 16);
                    break;
                case SST_stipple:
                    voodoo_flush(voodoo);
                    ret = voodoo->params.stipple;
                    break;
                case SST_color0:
                    voodoo_flush(voodoo);
                    ret = voodoo->params.color0;
                    break;
                case SST_color1:
                    voodoo_flush(voodoo);
                    ret = voodoo->params.color1;
                    break;
                case SST_fbiPixelsIn:
                    ret = voodoo->fbiPixelsIn & 0xffffff;
                    break;
                case SST_fbiChromaFail:
                    ret = voodoo->fbiChromaFail & 0xffffff;
                    break;
                case SST_fbiZFuncFail:
                    ret = voodoo->fbiZFuncFail & 0xffffff;
                    break;
                case SST_fbiAFuncFail:
                    ret = voodoo->fbiAFuncFail & 0xffffff;
                    break;
                case SST_fbiPixelsOut:
                    ret = voodoo->fbiPixelsOut & 0xffffff;
                    break;
                default:
                    banshee_log("3D read %08x\n", addr);
                    break;
            }
            break;

        case 0x1000000:
        case 0x1100000:
        case 0x1200000:
        case 0x1300000:
        case 0x1400000:
        case 0x1500000:
        case 0x1600000:
        case 0x1700000:
        case 0x1800000:
        case 0x1900000:
        case 0x1a00000:
        case 0x1b00000:
        case 0x1c00000:
        case 0x1d00000:
        case 0x1e00000:
        case 0x1f00000: /*3D LFB*/
            voodoo_flush(voodoo);
            ret = voodoo_fb_readl(addr & 0xfffffc, voodoo);
            break;

        default:
            break;
    }

    return ret;
}

void
banshee_cmd_write(void *priv, uint32_t addr, uint32_t val)
{
    banshee_t *banshee = (banshee_t *) priv;
    voodoo_t  *voodoo  = banshee->voodoo;

    switch (addr & 0x1fc) {
        case Agp_agpHostAddressLow:
            banshee->agpHostAddressLow = val;
            break;
        case Agp_agpHostAddressHigh:
            banshee->agpHostAddressHigh = val;
            break;
        case Agp_agpGraphicsAddress:
            banshee->agpGraphicsAddress = val;
            break;
        case Agp_agpGraphicsStride:
            banshee->agpGraphicsStride = val;
            break;
        case Agp_agpReqSize:
            banshee->agpReqSize = val;
            break;
        case Agp_agpMoveCMD:
            banshee->agpMoveCMD = val;             /* a PCI card: no AGP to move from */
            break;

        case cmdBaseAddr0:
            lfb_dirty = 1;
            voodoo->cmdfifo_base = (val & 0xfff) << 12;
            voodoo->cmdfifo_end  = voodoo->cmdfifo_base + (((voodoo->cmdfifo_size & 0xff) + 1) << 12);
            break;

        case cmdBaseSize0:
            lfb_dirty = 1;
            voodoo->cmdfifo_size    = val;
            voodoo->cmdfifo_end     = voodoo->cmdfifo_base + (((voodoo->cmdfifo_size & 0xff) + 1) << 12);
            voodoo->cmdfifo_enabled = val & 0x100;
            if (!voodoo->cmdfifo_enabled)
                voodoo->cmdfifo_in_sub = 0;
            voodoo->cmdfifo_in_agp = val & 0x200;
            kprintf("voodoo3: command FIFO 0 %s at %x, %u KB\n", voodoo->cmdfifo_enabled ? "on" : "off",
                    voodoo->cmdfifo_base, (voodoo->cmdfifo_end - voodoo->cmdfifo_base) >> 10);
            break;

        case cmdBump0:                             /* hole counting off: the host says how many words */
            voodoo->cmdfifo_depth_wr += val & 0xffff;
            voodoo_wake_fifo_thread(voodoo);
            break;
        case cmdRdPtrL0:
            voodoo->cmdfifo_rp = val;
            break;
        case cmdAMin0:
            voodoo->cmdfifo_amin = val;
            break;
        case cmdAMax0:
            voodoo->cmdfifo_amax = val;
            break;
        case cmdFifoDepth0:
            voodoo->cmdfifo_depth_rd = 0;
            voodoo->cmdfifo_depth_wr = val & 0xffff;
            break;
        case cmdHoleCnt0:
            voodoo->cmdfifo_holecount = val;
            break;

        case cmdBaseAddr1:
            lfb_dirty = 1;
            voodoo->cmdfifo_base_2 = (val & 0xfff) << 12;
            voodoo->cmdfifo_end_2  = voodoo->cmdfifo_base_2 + (((voodoo->cmdfifo_size_2 & 0xff) + 1) << 12);
            break;

        case cmdBaseSize1:
            lfb_dirty = 1;
            voodoo->cmdfifo_size_2    = val;
            voodoo->cmdfifo_end_2     = voodoo->cmdfifo_base_2 + (((voodoo->cmdfifo_size_2 & 0xff) + 1) << 12);
            voodoo->cmdfifo_enabled_2 = val & 0x100;
            if (!voodoo->cmdfifo_enabled_2)
                voodoo->cmdfifo_in_sub_2 = 0;
            voodoo->cmdfifo_in_agp_2 = val & 0x200;
            break;

        case cmdBump1:
            voodoo->cmdfifo_depth_wr_2 += val & 0xffff;
            voodoo_wake_fifo_thread(voodoo);
            break;
        case cmdRdPtrL1:
            voodoo->cmdfifo_rp_2 = val;
            break;
        case cmdAMin1:
            voodoo->cmdfifo_amin_2 = val;
            break;
        case cmdAMax1:
            voodoo->cmdfifo_amax_2 = val;
            break;
        case cmdFifoDepth1:
            voodoo->cmdfifo_depth_rd_2 = 0;
            voodoo->cmdfifo_depth_wr_2 = val & 0xffff;
            break;
        case cmdHoleCnt1:
            voodoo->cmdfifo_holecount_2 = val;
            break;

        default:
            banshee_log("unknown cmd write %08x <- %08x\n", addr, val);
            break;
    }
}

static void
banshee_reg_writew(uint32_t addr, uint16_t val, void *priv)
{
    banshee_t *banshee = (banshee_t *) priv;
    voodoo_t  *voodoo  = banshee->voodoo;

    switch (addr & 0x1f00000) {
        case 0x0000000: /*IO remap*/
            if (!(addr & 0x80000))
                banshee_ext_outw(addr & 0xff, val, banshee);
            break;

        case 0x1000000:
        case 0x1100000:
        case 0x1200000:
        case 0x1300000: /*3D LFB*/
        case 0x1400000:
        case 0x1500000:
        case 0x1600000:
        case 0x1700000:
        case 0x1800000:
        case 0x1900000:
        case 0x1a00000:
        case 0x1b00000:
        case 0x1c00000:
        case 0x1d00000:
        case 0x1e00000:
        case 0x1f00000:
            voodoo_queue_command(voodoo, (addr & 0xffffff) | FIFO_WRITEW_FB, val);
            break;

        default:
            break;
    }
}

static void
banshee_reg_writel(uint32_t addr, uint32_t val, void *priv)
{
    banshee_t *banshee = (banshee_t *) priv;
    voodoo_t  *voodoo  = banshee->voodoo;

    voodoo->last_write_addr = addr;

    switch (addr & 0x1f00000) {
        case 0x0000000: /*IO remap*/
            if (!(addr & 0x80000))
                banshee_ext_outl(addr & 0xff, val, banshee);
            else
                banshee_cmd_write(banshee, addr, val);
            break;

        case 0x0100000: /*2D registers*/
            if ((addr & 0x3fc) == SST_intrCtrl) {
                banshee->intrCtrl = val & 0x0030003f;
            } else {
                voodoo_queue_command(voodoo, (addr & 0x1fc) | FIFO_WRITEL_2DREG, val);
            }
            break;

        case 0x0200000:
        case 0x0300000:
        case 0x0400000:
        case 0x0500000: /*3D registers*/
            switch (addr & 0x3fc) {
                case SST_intrCtrl:
                    banshee->intrCtrl = val & 0x0030003f;
                    break;

                case SST_userIntrCMD:
                    banshee_log("userIntrCMD write %08x\n", val);
                    break;

                case SST_swapbufferCMD:
                case SST_triangleCMD:
                case SST_ftriangleCMD:
                case SST_fastfillCMD:
                case SST_nopCMD:
                    voodoo->cmd_written++;
                    voodoo_queue_command(voodoo, (addr & 0x3fc) | FIFO_WRITEL_REG, val);
                    if (!voodoo->voodoo_busy)
                        voodoo_wake_fifo_threads(voodoo->set, voodoo);
                    break;

                case SST_swapPending:
                    thread_wait_mutex(voodoo->swap_mutex);
                    voodoo->swap_count++;
                    thread_release_mutex(voodoo->swap_mutex);
                    break;

                default:
                    voodoo_queue_command(voodoo, (addr & 0x3ffffc) | FIFO_WRITEL_REG, val);
                    break;
            }
            break;

        case 0x0600000:
        case 0x0700000: /*TMU0 Texture download*/
            voodoo->tex_count++;
            voodoo_queue_command(voodoo, (addr & 0x1ffffc) | FIFO_WRITEL_TEX, val);
            break;

        case 0x1000000:
        case 0x1100000:
        case 0x1200000:
        case 0x1300000: /*3D LFB*/
        case 0x1400000:
        case 0x1500000:
        case 0x1600000:
        case 0x1700000:
        case 0x1800000:
        case 0x1900000:
        case 0x1a00000:
        case 0x1b00000:
        case 0x1c00000:
        case 0x1d00000:
        case 0x1e00000:
        case 0x1f00000:
            voodoo_queue_command(voodoo, (addr & 0xfffffc) | FIFO_WRITEL_FB, val);
            break;

        default:
            break;
    }
}

/* ---------------- BAR1: the linear frame buffer ---------------- */
static uint32_t
linear_addr(banshee_t *banshee, uint32_t addr)
{
    const voodoo_t *voodoo = banshee->voodoo;

    addr &= 0x1ffffff;
    if (addr >= voodoo->tile_base) {
        int x;
        int y;

        addr -= voodoo->tile_base;
        x = addr & (voodoo->tile_stride - 1);
        y = addr >> voodoo->tile_stride_shift;

        addr = voodoo->tile_base + (x & 127) + ((x >> 7) * 128 * 32) + ((y & 31) * 128) + (y >> 5) * voodoo->tile_x_real;
    }
    return addr;
}

static uint8_t
banshee_read_linear(uint32_t addr, banshee_t *banshee)
{
    addr = linear_addr(banshee, addr);
    if (addr >= banshee->vram_size)
        return 0xff;
    return banshee->vram[addr & banshee->vram_mask];
}

static uint16_t
banshee_read_linear_w(uint32_t addr, banshee_t *banshee)
{
    if (addr & 1)
        return banshee_read_linear(addr, banshee) | (banshee_read_linear(addr + 1, banshee) << 8);
    addr = linear_addr(banshee, addr);
    if (addr >= banshee->vram_size)
        return 0xffff;
    return *(uint16_t *) &banshee->vram[addr & banshee->vram_mask];
}

static uint32_t
banshee_read_linear_l(uint32_t addr, banshee_t *banshee)
{
    if (addr & 3)
        return banshee_read_linear_w(addr, banshee) | (banshee_read_linear_w(addr + 2, banshee) << 16);
    addr = linear_addr(banshee, addr);
    if (addr >= banshee->vram_size)
        return 0xffffffff;
    return *(uint32_t *) &banshee->vram[addr & banshee->vram_mask];
}

static void
banshee_write_linear(uint32_t addr, uint8_t val, banshee_t *banshee)
{
    addr = linear_addr(banshee, addr);
    if (addr >= banshee->vram_size)
        return;
    banshee->changedvram[addr >> 12]           = changeframecount;
    banshee->vram[addr & banshee->vram_mask] = val;
}

static void
banshee_write_linear_w(uint32_t addr, uint16_t val, banshee_t *banshee)
{
    if (addr & 1) {
        banshee_write_linear(addr, val, banshee);
        banshee_write_linear(addr + 1, val >> 8, banshee);
        return;
    }
    addr = linear_addr(banshee, addr);
    if (addr >= banshee->vram_size)
        return;
    banshee->changedvram[addr >> 12]                          = changeframecount;
    *(uint16_t *) &banshee->vram[addr & banshee->vram_mask] = val;
}

static void
banshee_write_linear_l(uint32_t addr, uint32_t val, banshee_t *banshee)
{
    voodoo_t *voodoo = banshee->voodoo;

    if (addr & 3) {
        banshee_write_linear_w(addr, val, banshee);
        banshee_write_linear_w(addr + 2, val >> 16, banshee);
        return;
    }

    voodoo->last_write_addr = addr;
    addr = linear_addr(banshee, addr);

    if (addr >= banshee->vram_size)
        return;

    banshee->changedvram[addr >> 12]                          = changeframecount;
    *(uint32_t *) &banshee->vram[addr & banshee->vram_mask] = val;
    if (voodoo->cmdfifo_enabled && addr >= voodoo->cmdfifo_base && addr < voodoo->cmdfifo_end) {
        if (addr == voodoo->cmdfifo_base && !voodoo->cmdfifo_holecount) {
            /*Reset pointers*/
            voodoo->cmdfifo_amin = voodoo->cmdfifo_base;
            voodoo->cmdfifo_amax = voodoo->cmdfifo_base;
            voodoo->cmdfifo_depth_wr++;
            voodoo_wake_fifo_thread(voodoo);
        } else if (voodoo->cmdfifo_holecount) {
            voodoo->cmdfifo_holecount--;
            if (!voodoo->cmdfifo_holecount) {
                /*Filled in holes, resume normal operation*/
                voodoo->cmdfifo_depth_wr += ((voodoo->cmdfifo_amax - voodoo->cmdfifo_amin) >> 2);
                voodoo->cmdfifo_amin = voodoo->cmdfifo_amax;
                voodoo_wake_fifo_thread(voodoo);
            }
        } else if (addr == voodoo->cmdfifo_amax + 4) {
            /*In-order write*/
            voodoo->cmdfifo_amin = addr;
            voodoo->cmdfifo_amax = addr;
            voodoo->cmdfifo_depth_wr++;
            voodoo_wake_fifo_thread(voodoo);
        } else {
            /*Out-of-order write*/
            if (addr < voodoo->cmdfifo_amin) {
                /*Reset back to start. Note that write is still out of order!*/
                voodoo->cmdfifo_amin = voodoo->cmdfifo_base - 4;
            }
            voodoo->cmdfifo_amax      = addr;
            voodoo->cmdfifo_holecount = ((voodoo->cmdfifo_amax - voodoo->cmdfifo_amin) >> 2) - 1;
        }
    }

    if (voodoo->cmdfifo_enabled_2 && addr >= voodoo->cmdfifo_base_2 && addr < voodoo->cmdfifo_end_2) {
        if (addr == voodoo->cmdfifo_base_2 && !voodoo->cmdfifo_holecount_2) {
            /*Reset pointers*/
            voodoo->cmdfifo_amin_2 = voodoo->cmdfifo_base_2;
            voodoo->cmdfifo_amax_2 = voodoo->cmdfifo_base_2;
            voodoo->cmdfifo_depth_wr_2++;
            voodoo_wake_fifo_thread(voodoo);
        } else if (voodoo->cmdfifo_holecount_2) {
            voodoo->cmdfifo_holecount_2--;
            if (!voodoo->cmdfifo_holecount_2) {
                /*Filled in holes, resume normal operation*/
                voodoo->cmdfifo_depth_wr_2 += ((voodoo->cmdfifo_amax_2 - voodoo->cmdfifo_amin_2) >> 2);
                voodoo->cmdfifo_amin_2 = voodoo->cmdfifo_amax_2;
                voodoo_wake_fifo_thread(voodoo);
            }
        } else if (addr == voodoo->cmdfifo_amax_2 + 4) {
            /*In-order write*/
            voodoo->cmdfifo_amin_2 = addr;
            voodoo->cmdfifo_amax_2 = addr;
            voodoo->cmdfifo_depth_wr_2++;
            voodoo_wake_fifo_thread(voodoo);
        } else {
            /*Out-of-order write*/
            if (addr < voodoo->cmdfifo_amin_2) {
                voodoo->cmdfifo_amin_2 = voodoo->cmdfifo_base_2 - 4;
            }
            voodoo->cmdfifo_amax_2      = addr;
            voodoo->cmdfifo_holecount_2 = ((voodoo->cmdfifo_amax_2 - voodoo->cmdfifo_amin_2) >> 2) - 1;
        }
    }
}

/* ---------------- BAR1 mapped straight onto vram ----------------
   Trapping every frame buffer access is slow, and nothing needs to see
   most of them: the guest's writes to plain vram (textures, its own 2D,
   reading back a picture) go to the same memory the engines draw in. So
   BAR1's pages are mapped onto vram, user and writable, except those the
   card must see written: the command FIFOs (hole counting), the tiled
   part above tile_base (its addresses are swizzled) and past the end of
   vram. In VMHV the same pages go into the VM's nested page tables.
   v3trap on the command line keeps every page trapped. */
static void lfb_remap(int all)
{
    banshee_t *b = card;
    voodoo_t *v = b->voodoo;
    int ok = lfb_direct && (b->pci_regs[4] & 2) && b->memBaseAddr1 == bar1_lin;
    u32 lo = ~0u, hi = 0, n = 0;
    lfb_dirty = 0;
    for (u32 a = 0; a < b->vram_size; a += 4096) {
        int want = ok && a + 4096 <= v->tile_base &&
                   !(v->cmdfifo_enabled && a < v->cmdfifo_end && a + 4096 > v->cmdfifo_base) &&
                   !(v->cmdfifo_enabled_2 && a < v->cmdfifo_end_2 && a + 4096 > v->cmdfifo_base_2);
        if (!all && want == lfb_mapped[a >> 12]) continue;
        lfb_mapped[a >> 12] = (u8)want;
        u32 host = want ? (u32)(uintptr_t)b->vram + a : 0;
        map_page(bar1_lin + a, host, want ? 7 : 0);
        hv_map_gpa(bar1_lin + a, host);
        if (a < lo) lo = a;
        hi = a + 4096;
        n += want;
    }
    if (hi) tlb_flush_range(bar1_lin + lo, hi - lo);
    if (hi && v3_log_on) kprintf("voodoo3: frame buffer %x-%x remapped (%u pages direct)\n", lo, hi, n);
}

/* VMHV has made its nested page tables: the direct pages into them too. */
void v3_hv_sync(void)
{
    if (!v3_on) return;
    lfb_remap(1);
}

/* ---------------- what the kernel calls ---------------- */
/* Whatever the card does between two looks: retraces, timers, the
   FIFO and render threads. */
static void v3_catch_up(void)
{
    if (lfb_dirty) lfb_remap(0);
    check_vsync();
    v3_poll_timers();
    v3_run();
}

int v3_window(u32 phys)
{
    if (!v3_on) return 0;
    u32 m0 = card->memBaseAddr0, m1 = card->memBaseAddr1;
    if (!(card->pci_regs[4] & 2)) return 0;   /* memory decoding off */
    return (m0 && phys - m0 < (32u << 20)) || (m1 && phys - m1 < (32u << 20));
}

int v3_range(u32 phys, u32 len)
{
    if (!v3_on || !len) return 0;
    return v3_window(phys) && v3_window(phys + len - 1) &&
           ((phys - card->memBaseAddr0 < (32u << 20)) == (phys + len - 1 - card->memBaseAddr0 < (32u << 20)));
}

u32 v3_mmio_read(u32 phys, int size)
{
    u32 v = 0xFFFFFFFFu;
    v3_enter();
    if (phys - card->memBaseAddr0 < (32u << 20)) {
        u32 a = phys - card->memBaseAddr0;
        u32 l = banshee_reg_readl(a & ~3u, card);
        if (size == 4 && !(a & 3)) v = l;
        else if (size == 2 && (a & 3) != 3) v = (l >> ((a & 3) * 8)) & 0xFFFF;
        else if (size == 1) v = (l >> ((a & 3) * 8)) & 0xFF;
        else {                                  /* straddling two registers: a byte at a time */
            v = 0;
            for (int i = 0; i < size; i++) v |= ((banshee_reg_readl((a + i) & ~3u, card) >> (((a + i) & 3) * 8)) & 0xFF) << (8 * i);
        }
    } else {
        u32 a = phys - card->memBaseAddr1;
        v = size == 4 ? banshee_read_linear_l(a, card) : size == 2 ? banshee_read_linear_w(a, card) : banshee_read_linear(a, card);
    }
    v3_catch_up();
    v3_leave();
    return v;
}

void v3_mmio_write(u32 phys, u32 val, int size)
{
    v3_enter();
    if (phys - card->memBaseAddr0 < (32u << 20)) {
        u32 a = phys - card->memBaseAddr0;
        if (size == 4 && !(a & 3)) banshee_reg_writel(a, val, card);
        else if (size == 2 && !(a & 1)) banshee_reg_writew(a, (u16)val, card);
        else for (int i = 0; i < size; i++) {
            u32 b = a + i;
            if (!(b & 0x1f80000)) banshee_ext_out(b & 0xff, (u8)(val >> (8 * i)), card);   /* IO remap */
        }
    } else {
        u32 a = phys - card->memBaseAddr1;
        if (size == 4) banshee_write_linear_l(a, val, card);
        else if (size == 2) banshee_write_linear_w(a, (u16)val, card);
        else banshee_write_linear(a, (u8)val, card);
    }
    v3_catch_up();
    v3_leave();
}

int v3_io(u16 port, int size, int write, u32 *val)
{
    if (!v3_on || !(card->pci_regs[4] & 1) || !card->ioBaseAddr) return 0;
    if ((u16)(port - card->ioBaseAddr) >= 0x100) return 0;
    u16 a = port - card->ioBaseAddr;
    v3_enter();
    if (write) {
        if (size == 4) banshee_ext_outl(a, *val, card);
        else if (size == 2) banshee_ext_outw(a, (u16)*val, card);
        else banshee_ext_out(a, (u8)*val, card);
    } else
        *val = size == 4 ? banshee_ext_inl(a, card) : size == 2 ? banshee_ext_inw(a, card) : banshee_ext_in(a, card);
    v3_catch_up();
    v3_leave();
    return 1;
}

void v3_tick(void)
{
    if (!v3_on || depth) return;
    v3_enter();
    v3_catch_up();
    v3_leave();
}

/* INT 10h set a VGA mode: the card's BIOS turns the video processor off. */
void v3_mode_set(void)
{
    if (!v3_on) return;
    card->vidProcCfg &= ~(VIDPROCCFG_VIDPROC_ENABLE | VIDPROCCFG_OVERLAY_ENABLE);
    card->vgaInit0 &= ~VGAINIT0_EXTENDED_SHIFT_OUT;
}

/* ---------------- PCI configuration space (86Box's banshee_pci_read/write) ---------------- */
static u8 cfg_read8(int addr)
{
    banshee_t *banshee = card;
    switch (addr) {
        case 0x00: return 0x1a; /*3DFX*/
        case 0x01: return 0x12;
        case 0x02: return 0x05; /*Voodoo 3*/
        case 0x03: return 0x00;
        case 0x04: return banshee->pci_regs[0x04] & 0x27;
        case 0x05: return 0;
        case 0x06: return 0x10; /*capabilities list*/
        case 0x07: return banshee->pci_regs[0x07] & 0x36;
        case 0x08: return 1;    /*Revision ID*/
        case 0x09: return 0;
        case 0x0a: return 0x00; /*VGA*/
        case 0x0b: return 0x03; /*display controller*/
        case 0x0d: return banshee->pci_regs[0x0d] & 0xf8;
        case 0x13: return banshee->memBaseAddr0 >> 24;
        case 0x17: return banshee->memBaseAddr1 >> 24;
        case 0x18: return 0x01 | (u8)banshee->ioBaseAddr;
        case 0x19: return banshee->ioBaseAddr >> 8;
        case 0x1a: return banshee->ioBaseAddr >> 16;
        case 0x1b: return banshee->ioBaseAddr >> 24;
        case 0x2c: case 0x2d: case 0x2e: case 0x2f: return banshee->pci_regs[addr];
        case 0x34: return 0x60;
        case 0x3c: return banshee->pci_regs[0x3c];
        case 0x3d: return 0x01; /*INTA*/
        case 0x3e: return 0x04;
        case 0x3f: return 0xff;
        case 0x40: return 0x01;
        case 0x50: return banshee->pci_regs[0x50];
        case 0x60: return 0x01; /*power management capability*/
        case 0x62: return 0x21;
        case 0x64: case 0x65: case 0x66: case 0x67: return banshee->pci_regs[addr];
        default: return 0;
    }
}

static void cfg_write8(int addr, u8 val)
{
    banshee_t *banshee = card;
    lfb_dirty = 1;
    switch (addr) {
        case 0x04: banshee->pci_regs[0x04] = val & 0x27; return;
        case 0x07: banshee->pci_regs[0x07] = val & 0x3e; return;
        case 0x0d: banshee->pci_regs[0x0d] = val & 0xf8; return;
        case 0x13: banshee->memBaseAddr0 = (val & 0xfe) << 24; break;
        case 0x17: banshee->memBaseAddr1 = (val & 0xfe) << 24; break;
        case 0x18: return;
        case 0x19: banshee->ioBaseAddr = (banshee->ioBaseAddr & 0xffff00ff) | val << 8; break;
        case 0x1a: banshee->ioBaseAddr = (banshee->ioBaseAddr & 0xff00ffff) | val << 16; break;
        case 0x1b: banshee->ioBaseAddr = (banshee->ioBaseAddr & 0x00ffffff) | (u32)val << 24; break;
        case 0x3c: case 0x50: case 0x65: case 0x67: banshee->pci_regs[addr] = val; return;
        case 0x64: banshee->pci_regs[0x64] = val & 0x03; return;
        case 0x66: banshee->pci_regs[0x66] = val & 0xc0; return;
        default: return;
    }
    if (addr == 0x13 || addr == 0x17) {
        u32 b = addr == 0x13 ? banshee->memBaseAddr0 : banshee->memBaseAddr1;
        if (b != 0xfe000000u && b != (addr == 0x13 ? bar0_lin : bar1_lin))   /* not a size probe, not where it was */
            kprintf("voodoo3: BAR%d moved to %x: only %x is trapped for sure\n", addr == 0x13 ? 0 : 1, b, addr == 0x13 ? bar0_lin : bar1_lin);
    }
}

u32 v3_cfg_read(int reg, int size)
{
    u32 v = 0;
    for (int i = 0; i < size; i++) v |= (u32)cfg_read8(reg + i) << (8 * i);
    acc_log(size == 4 ? "cfg rd4" : size == 2 ? "cfg rd2" : "cfg rd1", reg, v);
    return v;
}

void v3_cfg_write(int reg, u32 val, int size)
{
    acc_log(size == 4 ? "cfg wr4" : size == 2 ? "cfg wr2" : "cfg wr1", reg, val);
    for (int i = 0; i < size; i++) cfg_write8(reg + i, (u8)(val >> (8 * i)));
    if (lfb_dirty) lfb_remap(0);
}

/* ---------------- the picture ---------------- */
static int owns_screen(void)
{
    return v3_on && (card->vidProcCfg & VIDPROCCFG_VIDPROC_ENABLE) && (card->vgaInit0 & VGAINIT0_EXTENDED_SHIFT_OUT);
}

int v3_display(u32 *w, u32 *h)
{
    if (!owns_screen()) return 0;
    u32 sw = card->vidScreenSize & 0xfff, sh = (card->vidScreenSize >> 12) & 0xfff;
    if (!sw || !sh) {                             /* no screen size: the CRTC's display end */
        sw = ((crtc[1] | (crtc[0x1a] & 4) << 6) + 1) * 8;
        sh = (crtc[0x12] | (crtc[7] & 2) << 7 | (crtc[7] & 0x40) << 3 | (crtc[0x1b] & 4) << 8) + 1;
        if (card->vidProcCfg & VIDPROCCFG_2X_MODE) sw *= 2;
    }
    if (sw > 2048) sw = 2048;
    if (sh > 1536) sh = 1536;
    *w = sw; *h = sh;
    return 1;
}

static u32 clut(u32 c, int sel, int bypass)
{
    if (bypass || !card->pal_written) return c;
    const u32 *p = &card->pallook[sel ? 256 : 0];
    return (p[(c >> 16) & 0xff] & 0xff0000) | (p[(c >> 8) & 0xff] & 0xff00) | (p[c & 0xff] & 0xff);
}

static u32 px565(u16 v) { return ((v & 0xf800) << 8 | (v & 0xe000) << 3) | ((v & 0x07e0) << 5 | (v & 0x0600) >> 1) | ((v & 0x1f) << 3 | (v & 0x1c) >> 2); }

/* One pixel of a surface in vram: fmt as the video processor counts them. */
static u32 sample(u32 base, u32 stride, int tiled, int fmt, u32 x, u32 y)
{
    u32 bpp = fmt == PIX_FORMAT_8 ? 1 : fmt == PIX_FORMAT_RGB24 ? 3 : fmt == PIX_FORMAT_RGB32 ? 4 : 2;
    u32 a;
    if (tiled) {                                  /* 128-byte by 32-line tiles; stride in tiles */
        u32 bx = x * bpp;
        a = base + (y >> 5) * stride * 128 * 32 + (bx >> 7) * 128 * 32 + (y & 31) * 128 + (bx & 127);
    } else a = base + y * stride + x * bpp;
    const u8 *p = card->vram + (a & card->vram_mask);
    switch (fmt) {
    case PIX_FORMAT_8: {
        const u8 *d = vga_dac[*p];
        int s = (card->vgaInit0 & VGAINIT0_RAMDAC_8BIT) ? 0 : 2;
        return (u32)(d[0] << s) << 16 | (u32)(d[1] << s) << 8 | (u32)(d[2] << s); }
    case PIX_FORMAT_RGB24: return p[0] | p[1] << 8 | p[2] << 16;
    case PIX_FORMAT_RGB32: return *(const u32 *)p & 0xffffff;
    default: return px565(*(const u16 *)p);
    }
}

void v3_row(u32 y, u32 *out)
{
    u32 w, h;
    if (!v3_display(&w, &h)) return;
    banshee_t *banshee = card;
    voodoo_t *voodoo = banshee->voodoo;
    u32 cfg = banshee->vidProcCfg;
    int dfmt = VIDPROCCFG_DESKTOP_PIX_FORMAT, dtile = !!(cfg & VIDPROCCFG_DESKTOP_TILE);
    u32 dstride = banshee->vidDesktopOverlayStride & 0x3fff;
    u32 dy = (cfg & VIDPROCCFG_HALF_MODE) ? y >> 1 : y;
    int ov = (cfg & VIDPROCCFG_OVERLAY_ENABLE) && voodoo->overlay.size_x > 0 && voodoo->overlay.size_y > 0 &&
             (int)y >= voodoo->overlay.start_y && (int)y <= voodoo->overlay.end_y;
    u32 ox0 = ov ? (u32)voodoo->overlay.start_x : w, ox1 = ov ? (u32)voodoo->overlay.end_x + 1 : w;
    for (u32 x = 0; x < w; x++) {
        if (x >= ox0 && x < ox1) continue;
        u32 c = sample(banshee->vidDesktopStartAddr, dstride, dtile, dfmt, x, dy);
        if (dfmt != PIX_FORMAT_8) c = clut(c, cfg & VIDPROCCFG_DESKTOP_CLUT_SEL, cfg & VIDPROCCFG_DESKTOP_CLUT_BYPASS);
        out[x] = c;
    }
    if (!ov) return;
    /* The overlay (Glide's 3D picture): scaled if asked, point sampled. */
    u32 ostride = (banshee->vidDesktopOverlayStride & VID_STRIDE_OVERLAY_MASK) >> VID_STRIDE_OVERLAY_SHIFT;
    int otile = !!(cfg & VIDPROCCFG_OVERLAY_TILE);
    int ofmt = banshee->overlay_pix_fmt == 7 ? 1 : banshee->overlay_pix_fmt;   /* 565 dithered: as 565 */
    if (ofmt != PIX_FORMAT_RGB565 && ofmt != PIX_FORMAT_RGB24 && ofmt != PIX_FORMAT_RGB32 && ofmt != PIX_FORMAT_8) ofmt = PIX_FORMAT_RGB565;   /* YUV: not done */
    u32 ry = y - voodoo->overlay.start_y;
    u32 sy = (cfg & VIDPROCCFG_V_SCALE_ENABLE) ? (u32)(((uint64_t)ry * voodoo->overlay.vidOverlayDvdy) >> 20) : ry;
    u32 dudx = (cfg & VIDPROCCFG_H_SCALE_ENABLE) ? voodoo->overlay.vidOverlayDudx : (1u << 20);
    u32 sx = 0;
    if (ox1 > w) ox1 = w;
    for (u32 x = ox0; x < ox1; x++, sx += dudx) {
        u32 c = sample(banshee->overlay_addr, ostride, otile, ofmt, sx >> 20, sy);
        out[x] = clut(c, cfg & VIDPROCCFG_OVERLAY_CLUT_SEL, cfg & VIDPROCCFG_OVERLAY_CLUT_BYPASS);
    }
}

/* ---------------- start-up ---------------- */

void v3_init(void)
{
    if (!strstr(cmdline, "voodoo3")) return;
    v3_log_on = strstr(cmdline, "v3log") ? 2 : 0;
    bar0_lin = linear_hole(64u << 20, 32u << 20);
    if (!bar0_lin) { kprintf("voodoo3: no linear space for its BARs, off\n"); return; }
    bar1_lin = bar0_lin + (32u << 20);
    v3_sched_init();

    v3_enter();
    banshee_t *banshee = calloc(1, sizeof(banshee_t));
    card = banshee;
    banshee->vram_size = VRAM_MB << 20;
    banshee->vram_mask = banshee->vram_size - 1;
    banshee->vram = phys_try_alloc(banshee->vram_size);
    banshee->changedvram = calloc(1, banshee->vram_size >> 12);
    if (!banshee->vram) { v3_leave(); kprintf("voodoo3: no memory for its %u MB, off\n", VRAM_MB); return; }

    banshee->dramInit0 = 1 << 27;                /* as 86Box's Voodoo 3 3000: 16 MB SDRAM */
    banshee->dramInit1 = 1 << 30;
    banshee->vidSerialParallelPort = VIDSERIAL_DDC_DCK_W | VIDSERIAL_DDC_DDA_W;
    banshee->memBaseAddr0 = bar0_lin;
    banshee->memBaseAddr1 = bar1_lin;
    banshee->ioBaseAddr = V3_IO_BASE;
    banshee->pci_regs[0x04] = 3;                  /* the BIOS left I/O and memory on */
    banshee->pci_regs[0x3c] = 10;                 /* not the NE2000's 11; the card raises none */
    banshee->pci_regs[0x2c] = 0x1a;               /* subsystem: Voodoo 3 3000 */
    banshee->pci_regs[0x2d] = 0x12;
    banshee->pci_regs[0x2e] = 0x3a;
    banshee->pci_regs[0x2f] = 0x00;

    voodoo_t *voodoo = voodoo_2d3d_card_init(VOODOO_3);
    banshee->voodoo        = voodoo;
    voodoo->priv           = banshee;
    banshee->set.voodoos[0] = voodoo;
    banshee->set.nr_cards  = 1;
    voodoo->set            = &banshee->set;
    voodoo->vram           = banshee->vram;
    voodoo->vram_max       = banshee->vram_size;
    voodoo->changedvram    = banshee->changedvram;
    voodoo->fb_mem         = banshee->vram;
    voodoo->fb_mask        = banshee->vram_mask;
    voodoo->tex_mem[0]     = banshee->vram;
    voodoo->tex_mem_w[0]   = (uint16_t *) banshee->vram;
    voodoo->tex_mem[1]     = banshee->vram;
    voodoo->tex_mem_w[1]   = (uint16_t *) banshee->vram;
    voodoo->texture_mask   = banshee->vram_mask;
    voodoo->cmd_status     = (1 << 28);
    voodoo->cmd_status_2   = (1 << 28);
    voodoo_generate_filter_v1(voodoo);
    timer_disable(&voodoo->timer);                /* the Voodoo 1/2 scan-out: not this card's */
    /* as the card's BIOS leaves it: no tiled part in the linear frame buffer */
    banshee_ext_outl(Init_lfbMemoryConfig, (banshee->vram_size >> 12) | 0x7f << 16, banshee);
    v3_leave();

    v3_on = 1;
    lfb_mapped = calloc(1, banshee->vram_size >> 12);
    lfb_direct = !strstr(cmdline, "v3trap");
    lfb_remap(1);
    vpci_bios_patch();
    kprintf("voodoo3: Voodoo 3 3000 (86Box's Voodoo), %u MB, PCI 00:01.0, registers at %x, frame buffer at %x, I/O at %x\n",
            VRAM_MB, bar0_lin, bar1_lin, V3_IO_BASE);
}
