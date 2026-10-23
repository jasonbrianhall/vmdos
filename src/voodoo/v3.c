/* The Voodoo 3 in vmdos: 86Box's card as 86Box runs it (the Voodoo 3 3000
   of vid_voodoo_banshee.c on 86Box's SVGA core, vid_svga.c, with its own
   BIOS ROM, built in), and under it what 86Box's machine gives it: memory
   and I/O mappings, PCI configuration, the ROM, timers, the monitor it
   draws into. With voodoo3 on the command line the card is the PC's VGA:
   ports 3B0h-3DFh, A0000h-BFFFFh and INT 10h (its BIOS, copied to C0000h)
   are the card's, vmdos's own VGA steps aside, and the screen shows what
   the card draws (monitors[0]'s bitmap, as 86Box's window would).

   Guest accesses to the card's memory fault and are emulated (mememu.c)
   into v3_mmio_read / v3_mmio_write, which find the mapping 86Box's code
   registered; port accesses come through v3_io. The frame buffer (BAR1) is
   mapped straight onto the card's memory where the card needn't see the
   writes. The card's code runs with the guest's x87 state put aside
   (v3_enter); its threads are green threads (v3sched.c), its timers
   86Box's timer.c counting PIT clocks. */
#include <voodoo_shim.h>
#include <86box/vid_svga.h>
#include <86box/vid_voodoo_common.h>
#include <86box/machine.h>
#include <86box/pit.h>
#include "../voodoo3.h"

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

/* ---------------- the kernel ---------------- */
extern char cmdline[512];
void *phys_alloc(u32 bytes);
void *phys_try_alloc(u32 bytes);
u32 linear_hole(u32 len, u32 align);
void map_page(u32 lin, u32 phys, u32 flags);
void tlb_flush_range(u32 lin, u32 len);
void hv_map_gpa(u32 gpa, u32 host);
void v3_run(void);
void v3_poll_timers(void);
void v3_sched_init(void);

/* v3card.c */
const device_t *v3_device(void);
voodoo_t *v3_card_voodoo(void *p);
svga_t *v3_card_svga(void *p);
uint32_t v3_card_bar(void *p, int n);
int v3_card_mem_on(void *p);

/* v3rom.S: roms/voodoo3/3k12sd.rom, when the build had it */
extern const u8 v3_rom_start[], v3_rom_end[];

/* ---------------- what 86Box's code expects of 86Box ---------------- */
int v3_log_on;
int cycles;
int pci_burst_time = 1, pci_nonburst_time = 1, agp_burst_time = 1, agp_nonburst_time = 1;
double cpuclock = 1193182.0;                   /* timers count PIT clocks */
uint64_t VGACONST1, VGACONST2;
cpu_state_t cpu_state;
machine_t machines[1];
int machine;
double isa_timing = 1.0;
int suppress_overscan, enable_overscan;
int xga_active, ibm8514_active;
monitor_t monitors[MONITORS_NUM];
int monitor_index_global;
uint32_t *video_6to8, *video_8togs, *video_8to32, *video_15to32, *video_16to32;
uint8_t edatlookup[4][4];
dbcs_font_t *fontdatksc5601, *fontdatksc5601_user;

static u8 *arena;                              /* small allocations: one block, never given back */
static u32 arena_left;
void *v3_calloc(size_t n, size_t sz)
{
    u32 len = (u32)(n * sz);
    if (len >= 32768) {
        void *p = phys_try_alloc((len + 4095) & ~4095u);
        if (!p) panic("voodoo3: out of memory (%u bytes)", len);
        memset(p, 0, len);
        return p;
    }
    len = (len + 15) & ~15u;
    if (len > arena_left) {
        arena = phys_alloc(256 * 1024);
        memset(arena, 0, 256 * 1024);
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

double round(double x) { return x < 0 ? -floor(-x + 0.5) : floor(x + 0.5); }

int device_get_config_int(const char *n)
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
const char *device_get_config_bios(const char *n) { (void)n; return ""; }
const char *device_get_bios_file(const device_t *d, const char *n, unsigned int i) { (void)d; (void)n; (void)i; return ""; }
uint32_t device_get_bios_local(const device_t *d, const char *n) { (void)d; (void)n; return 0; }
uint64_t device_get_bios_flags(const device_t *d, const char *n) { (void)d; (void)n; return 0; }
void video_clamp_vram(uint64_t f, int *s) { (void)f; (void)s; }

uint32_t mem_readl_phys(uint32_t addr) { (void)addr; return 0xFFFFFFFFu; }   /* AGP only; this is the PCI card */
void dma_bm_read(uint32_t a, uint8_t *d, int n, int s) { (void)a; (void)s; memset(d, 0xFF, n); }

/* no monitor on the DDC lines, nothing on the I2C bus */
void *i2c_gpio_init(char *n) { (void)n; return 0; }
void i2c_gpio_close(void *p) { (void)p; }
void i2c_gpio_set(void *p, uint8_t scl, uint8_t sda) { (void)p; (void)scl; (void)sda; }
uint8_t i2c_gpio_get_scl(void *p) { (void)p; return 1; }
uint8_t i2c_gpio_get_sda(void *p) { (void)p; return 1; }
void *i2c_gpio_get_bus(void *p) { (void)p; return 0; }
void *ddc_init(void *i2c) { (void)i2c; return 0; }
void ddc_close(void *e) { (void)e; }

/* XGA and 8514/A: not on this card */
uint8_t xga_read_test(uint32_t a, void *p) { (void)a; (void)p; return 0xFF; }
void xga_write_test(uint32_t a, uint8_t v, void *p) { (void)a; (void)v; (void)p; }
void xga_recalctimings(svga_t *s) { (void)s; }
void xga_set_poll(svga_t *s) { (void)s; }
void rivatimer_init(void) { }
void ibm8514_recalctimings(svga_t *s) { (void)s; }
void ibm8514_set_poll(svga_t *s) { (void)s; }

/* ---------------- the monitor ----------------
   86Box's window: the card draws scan lines into monitors[0].target_buffer
   and, once a frame, names the part of it that is the picture. */
static volatile int blit_x, blit_y, blit_w, blit_h;
void video_inform_monitor(int type, const video_timings_t *t, int m) { (void)type; (void)t; (void)m; }
void video_wait_for_buffer_monitor(int m) { (void)m; }
void set_screen_size_monitor(int x, int y, int m) { (void)x; (void)y; (void)m; }
uint8_t video_force_resize_get_monitor(int m) { (void)m; return 0; }
void video_force_resize_set_monitor(uint8_t r, int m) { (void)r; (void)m; }
void video_blit_memtoscreen_monitor(int x, int y, int w, int h, int m)
{
    (void)m;
    blit_x = x; blit_y = y; blit_w = w; blit_h = h;
}
void video_lightpen_check_trigger_strobe(int a, int b, int c, int d, double e, int m) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)m; }
void video_lightpen_hsync(void) { }
void video_lightpen_vsync(void) { }

static u32 calc_6to8(int c) { int ic = c == 64 ? 63 : c & 0x3f; return (u32)(ic * 255 / 63) & 0xff; }
static u32 rgb(int r, int g, int b) { return 0xff000000u | (u32)r << 16 | (u32)g << 8 | (u32)b; }
static u32 calc_15to32(int c) { return rgb(((c >> 10) & 31) * 255 / 31, ((c >> 5) & 31) * 255 / 31, (c & 31) * 255 / 31); }
static u32 calc_16to32(int c) { return rgb(((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31); }
static u32 calc_8to32(int c) { return rgb(((c >> 5) & 7) * 255 / 7, ((c >> 2) & 7) * 255 / 7, (c & 3) * 255 / 3); }

static void video_tables(void)                 /* 86Box's video_init and video_monitor_init, what of them the card uses */
{
    for (int c = 0; c < 4; c++)
        for (int d = 0; d < 4; d++) {
            edatlookup[c][d] = 0;
            if (c & 1) edatlookup[c][d] |= 1;
            if (d & 1) edatlookup[c][d] |= 2;
            if (c & 2) edatlookup[c][d] |= 0x10;
            if (d & 2) edatlookup[c][d] |= 0x20;
        }
    video_6to8 = calloc(4, 256);
    video_8togs = calloc(4, 256);
    video_8to32 = calloc(4, 256);
    for (int c = 0; c < 256; c++) {
        video_6to8[c] = calc_6to8(c);
        video_8togs[c] = c | c << 16 | c << 24;
        video_8to32[c] = calc_8to32(c);
    }
    video_15to32 = calloc(4, 65536);
    video_16to32 = calloc(4, 65536);
    for (int c = 0; c < 65536; c++) {
        video_15to32[c] = calc_15to32(c & 0x7fff);
        video_16to32[c] = calc_16to32(c);
    }
    monitor_t *m = &monitors[0];
    m->mon_xsize = 640; m->mon_ysize = 480;
    m->mon_res_x = 640; m->mon_res_y = 480;
    m->mon_bpp = 8;
    m->mon_changeframecount = 2;
    bitmap_t *b = calloc(1, sizeof *b);
    b->w = 2048; b->h = 2048;
    b->dat = calloc(2048 * 2048, 4);
    for (int i = 0; i < 2048; i++) b->line[i] = b->dat + i * 2048;
    for (int i = 2048; i < 2112; i++) b->line[i] = b->dat + 2047 * 2048;
    m->target_buffer = b;
    m->mon_pal_lookup = calloc(4, 256);
    m->mon_cga_palette = calloc(1, sizeof(int));
    m->mon_force_resize = 1;
}

/* ---------------- memory mappings (86Box's mem.c, the part a card uses) ---------------- */
static mem_mapping_t *maps;                    /* newest first */

void mem_mapping_add(mem_mapping_t *map, uint32_t base, uint32_t size,
                     uint8_t (*read_b)(uint32_t, void *), uint16_t (*read_w)(uint32_t, void *), uint32_t (*read_l)(uint32_t, void *),
                     void (*write_b)(uint32_t, uint8_t, void *), void (*write_w)(uint32_t, uint16_t, void *), void (*write_l)(uint32_t, uint32_t, void *),
                     uint8_t *exec, uint32_t fl, void *priv)
{
    map->base = base; map->size = size; map->mask = 0xffffffff;
    map->read_b = read_b; map->read_w = read_w; map->read_l = read_l;
    map->write_b = write_b; map->write_w = write_w; map->write_l = write_l;
    map->exec = exec; map->flags = fl; map->priv = priv;
    map->enable = size != 0;
    map->prev = 0; map->next = maps;
    maps = map;
}
void mem_mapping_set_handler(mem_mapping_t *map,
                             uint8_t (*read_b)(uint32_t, void *), uint16_t (*read_w)(uint32_t, void *), uint32_t (*read_l)(uint32_t, void *),
                             void (*write_b)(uint32_t, uint8_t, void *), void (*write_w)(uint32_t, uint16_t, void *), void (*write_l)(uint32_t, uint32_t, void *))
{
    map->read_b = read_b; map->read_w = read_w; map->read_l = read_l;
    map->write_b = write_b; map->write_w = write_w; map->write_l = write_l;
}
void mem_mapping_set_addr(mem_mapping_t *map, uint32_t base, uint32_t size) { map->base = base; map->size = size; map->enable = 1; }
void mem_mapping_set_p(mem_mapping_t *map, void *p) { map->priv = p; }
void mem_mapping_enable(mem_mapping_t *map) { map->enable = 1; }
void mem_mapping_disable(mem_mapping_t *map) { map->enable = 0; }

static mem_mapping_t *map_at(u32 a)
{
    for (mem_mapping_t *m = maps; m; m = m->next)
        if (m->enable && a - m->base < m->size) return m;
    return 0;
}

/* One access the width it was made, or as narrower ones where the mapping
   has no handler that wide (86Box's mem_read*). Nothing there: 0xFF's. */
static u32 map_read(u32 a, int size)
{
    mem_mapping_t *m = map_at(a);
    if (m && size == 4 && m->read_l) return m->read_l(a, m->priv);
    if (m && size >= 2 && m->read_w)
        return size == 2 ? m->read_w(a, m->priv) : m->read_w(a, m->priv) | map_read(a + 2, 2) << 16;
    u32 v = 0;
    for (int i = 0; i < size; i++) {
        mem_mapping_t *n = map_at(a + i);
        v |= (u32)(n && n->read_b ? n->read_b(a + i, n->priv) : 0xFF) << (8 * i);
    }
    return v;
}
static void map_write(u32 a, u32 v, int size)
{
    mem_mapping_t *m = map_at(a);
    if (m && size == 4 && m->write_l) { m->write_l(a, v, m->priv); return; }
    if (m && size >= 2 && m->write_w) {
        m->write_w(a, (u16)v, m->priv);
        if (size == 4) map_write(a + 2, v >> 16, 2);
        return;
    }
    for (int i = 0; i < size; i++) {
        mem_mapping_t *n = map_at(a + i);
        if (n && n->write_b) n->write_b(a + i, (u8)(v >> (8 * i)), n->priv);
    }
}

/* ---------------- I/O ports (86Box's io.c, the part a card uses) ---------------- */
#define MAX_IO 32
static struct io_h {
    u16 base, size;
    uint8_t (*inb)(uint16_t, void *); uint16_t (*inw)(uint16_t, void *); uint32_t (*inl)(uint16_t, void *);
    void (*outb)(uint16_t, uint8_t, void *); void (*outw)(uint16_t, uint16_t, void *); void (*outl)(uint16_t, uint32_t, void *);
    void *priv;
} ioh[MAX_IO];
static int nio;

void io_sethandler(uint16_t base, uint16_t size,
                   uint8_t (*inb)(uint16_t, void *), uint16_t (*inw)(uint16_t, void *), uint32_t (*inl)(uint16_t, void *),
                   void (*outb)(uint16_t, uint8_t, void *), void (*outw)(uint16_t, uint16_t, void *), void (*outl)(uint16_t, uint32_t, void *),
                   void *priv)
{
    if (nio >= MAX_IO) { kprintf("voodoo3: too many I/O handlers\n"); return; }
    struct io_h h = { base, size, inb, inw, inl, outb, outw, outl, priv };
    ioh[nio++] = h;
}
void io_removehandler(uint16_t base, uint16_t size,
                      uint8_t (*inb)(uint16_t, void *), uint16_t (*inw)(uint16_t, void *), uint32_t (*inl)(uint16_t, void *),
                      void (*outb)(uint16_t, uint8_t, void *), void (*outw)(uint16_t, uint16_t, void *), void (*outl)(uint16_t, uint32_t, void *),
                      void *priv)
{
    (void)inw; (void)inl; (void)outw; (void)outl;
    for (int i = 0; i < nio; i++) {                /* 86Box's is per port: every handler of these inside the range */
        struct io_h *h = &ioh[i];
        if (h->base >= base && h->base + h->size <= base + size && h->inb == inb && h->outb == outb && h->priv == priv)
            ioh[i--] = ioh[--nio];
    }
}
static struct io_h *io_at(u16 p)
{
    for (int i = 0; i < nio; i++) if ((u16)(p - ioh[i].base) < ioh[i].size) return &ioh[i];
    return 0;
}
static u32 io_read(u16 p, int size)
{
    struct io_h *h = io_at(p);
    if (h && size == 4 && h->inl) return h->inl(p, h->priv);
    if (h && size >= 2 && h->inw) return size == 2 ? h->inw(p, h->priv) : h->inw(p, h->priv) | io_read(p + 2, 2) << 16;
    u32 v = 0;
    for (int i = 0; i < size; i++) {
        struct io_h *b = io_at(p + i);
        v |= (u32)(b && b->inb ? b->inb(p + i, b->priv) : 0xFF) << (8 * i);
    }
    return v;
}
static void io_write(u16 p, u32 v, int size)
{
    struct io_h *h = io_at(p);
    if (h && size == 4 && h->outl) { h->outl(p, v, h->priv); return; }
    if (h && size >= 2 && h->outw) {
        h->outw(p, (u16)v, h->priv);
        if (size == 4) io_write(p + 2, v >> 16, 2);
        return;
    }
    for (int i = 0; i < size; i++) {
        struct io_h *b = io_at(p + i);
        if (b && b->outb) b->outb(p + i, (u8)(v >> (8 * i)), b->priv);
    }
}

/* ---------------- PCI ---------------- */
static uint8_t (*pci_rd)(int func, int addr, int len, void *priv);
static void (*pci_wr)(int func, int addr, int len, uint8_t val, void *priv);
static void *pci_priv;
void pci_add_card(uint8_t type, uint8_t (*read)(int, int, int, void *), void (*write)(int, int, int, uint8_t, void *), void *priv, uint8_t *slot)
{
    (void)type;
    pci_rd = read; pci_wr = write; pci_priv = priv;
    if (slot) *slot = 1;
}
void pci_irq(uint8_t slot, uint8_t pci_int, int level, int set, uint8_t *irq_state) { (void)slot; (void)pci_int; (void)level; (void)set; (void)irq_state; }

/* ---------------- the BIOS ROM ---------------- */
static rom_t *the_rom;
int rom_present(const char *fn) { (void)fn; return v3_rom_end > v3_rom_start; }
int rom_init(rom_t *rom, const char *fn, uint32_t addr, int sz, int mask, int off, uint32_t flags)
{
    (void)fn; (void)off;
    u32 n = (u32)(v3_rom_end - v3_rom_start);
    rom->rom = calloc(1, sz);
    memset(rom->rom, 0xFF, sz);
    memcpy(rom->rom, v3_rom_start, n < (u32)sz ? n : (u32)sz);
    rom->sz = sz;
    rom->mask = mask;
    mem_mapping_add(&rom->mapping, addr, sz, rom_read, rom_readw, rom_readl, 0, 0, 0, rom->rom, flags, rom);
    the_rom = rom;
    return 1;
}
uint8_t rom_read(uint32_t addr, void *priv)
{
    const rom_t *rom = priv;
    return rom->rom[addr & rom->mask];
}
uint16_t rom_readw(uint32_t addr, void *priv) { return rom_read(addr, priv) | rom_read(addr + 1, priv) << 8; }
uint32_t rom_readl(uint32_t addr, void *priv) { return rom_readw(addr, priv) | (u32)rom_readw(addr + 2, priv) << 16; }

/* ---------------- the guest's x87 state, around the card's code ---------------- */
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

/* ---------------- the card ---------------- */
int v3_on;
static void *card;
static voodoo_t *vdo;
static svga_t *svga;
static u32 bar0_lin, bar1_lin;                 /* where the BARs were put: unmapped linear space */
#define V3_IO_BASE 0xD100   /* not D000h: Windows 9x without a PnP BIOS takes the board's devices at 00h-FFh as 10-bit ISA decoders, whose aliases cover D000h-D0FFh */

/* BAR1 pages mapped straight onto vram where the card needn't see the
   writes: not the command FIFOs (hole counting), not the tiled part above
   tile_base, not past the end of vram. Every frame is then drawn anew (the
   card's dirty-page marks miss these writes). In VMHV the same pages go
   into the VM's nested page tables. v3trap: none. */
static u8 *lfb_mapped;
static int lfb_direct, lfb_pages;
static u32 redraw_n;
static u32 lfb_key[8];
static void lfb_remap(int all)
{
    u32 key[8] = { v3_card_bar(card, 1), (u32)v3_card_mem_on(card), vdo->tile_base,
                   vdo->cmdfifo_enabled ? vdo->cmdfifo_base : 1, vdo->cmdfifo_enabled ? vdo->cmdfifo_end : 1,
                   vdo->cmdfifo_enabled_2 ? vdo->cmdfifo_base_2 : 1, vdo->cmdfifo_enabled_2 ? vdo->cmdfifo_end_2 : 1, 0 };
    if (!all && !memcmp(key, lfb_key, sizeof key)) return;
    memcpy(lfb_key, key, sizeof key);
    int ok = lfb_direct && key[1] && key[0] == bar1_lin;
    u32 lo = ~0u, hi = 0, n = 0;
    for (u32 a = 0; a < (u32)svga->vram_max; a += 4096) {
        int want = ok && a + 4096 <= vdo->tile_base &&
                   !(vdo->cmdfifo_enabled && a < vdo->cmdfifo_end && a + 4096 > vdo->cmdfifo_base) &&
                   !(vdo->cmdfifo_enabled_2 && a < vdo->cmdfifo_end_2 && a + 4096 > vdo->cmdfifo_base_2);
        if (!all && want == lfb_mapped[a >> 12]) continue;
        lfb_mapped[a >> 12] = (u8)want;
        u32 host = want ? (u32)(uintptr_t)svga->vram + a : 0;
        map_page(bar1_lin + a, host, want ? 7 : 0);
        hv_map_gpa(bar1_lin + a, host);
        if (a < lo) lo = a;
        hi = a + 4096;
        n += want;
    }
    if (hi) tlb_flush_range(bar1_lin + lo, hi - lo);
    lfb_pages = 0;
    for (u32 a = 0; a < (u32)svga->vram_max; a += 4096) lfb_pages += lfb_mapped[a >> 12];
    if (hi && v3_log_on) kprintf("voodoo3: frame buffer %x-%x remapped (%u pages direct)\n", lo, hi, n);
}

void v3_hv_sync(void)
{
    if (v3_on) lfb_remap(1);
}

/* Whatever the card does between two looks: its timers (scan lines,
   retraces, the FIFO thread's wake-up), the FIFO and render threads. */
static void catch_up(void)
{
    lfb_remap(0);
    v3_poll_timers();
    v3_run();
}

/* A timer that is always there: timer.c's list mustn't run empty. */
static pc_timer_t keep_timer;
static void keep_cb(void *p) { (void)p; timer_advance_u64(&keep_timer, TIMER_USEC * 500000); }

/* ---------------- what the kernel calls ---------------- */
static int legacy(u32 a) { return a - 0xA0000 < 0x20000; }   /* the VGA's memory */

int v3_window(u32 phys)
{
    if (!v3_on) return 0;
    return legacy(phys) || phys - bar0_lin < (64u << 20);   /* the VGA's memory, BAR0, BAR1 */
}

int v3_range(u32 phys, u32 len)
{
    if (!v3_on || !len) return 0;
    return phys - bar0_lin < (64u << 20) && len <= (64u << 20) - (phys - bar0_lin);   /* inside BAR0 and BAR1 */
}

u32 v3_mmio_read(u32 phys, int size)
{
    v3_enter();
    catch_up();
    u32 v = map_read(phys, size);
    v3_leave();
    return v;
}

void v3_mmio_write(u32 phys, u32 val, int size)
{
    v3_enter();
    map_write(phys, val, size);
    catch_up();
    v3_leave();
}

static int acc_left = 4000;                    /* v3log: the card's own I/O registers and its configuration space */
static void acc_log(const char *what, u32 a, u32 v)
{
    if (!v3_log_on || acc_left <= 0) return;
    if (!--acc_left) kprintf("voodoo3: access log full\n");
    else kprintf("voodoo3: %s %03x %08x\n", what, a, v);
}

int v3_io(u16 port, int size, int write, u32 *val)
{
    if (!v3_on) return 0;
    struct io_h *h = io_at(port);
    if (!h) {
        if (port - 0x3B0u < 0x30) {            /* the VGA's ports the card doesn't decode now (mono/colour) */
            if (!write) *val = size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFFu;
            return 1;
        }
        return 0;
    }
    v3_enter();
    catch_up();
    if (write) io_write(port, *val, size);
    else *val = io_read(port, size);
    if (port - V3_IO_BASE < 0x100u) acc_log(write ? "io out" : "io in ", port, *val);
    v3_leave();
    return 1;
}

u32 v3_cfg_read(int reg, int size)
{
    u32 v = 0;
    v3_enter();
    for (int i = 0; i < size; i++) v |= (u32)pci_rd(0, reg + i, 1, pci_priv) << (8 * i);
    v3_leave();
    acc_log("cfg rd", reg, v);
    return v;
}

void v3_cfg_write(int reg, u32 val, int size)
{
    acc_log("cfg wr", reg, val);
    v3_enter();
    for (int i = 0; i < size; i++) pci_wr(0, reg + i, 1, (u8)(val >> (8 * i)), pci_priv);
    catch_up();
    v3_leave();
}

void v3_tick(void)
{
    if (!v3_on || depth) return;
    v3_enter();
    catch_up();
    if (lfb_pages && svga->fb_only && !(++redraw_n % 32))    /* the frame buffer written past the card: */
        svga->fullchange = changeframecount;                 /* all of it drawn anew, 30 times a second */
    v3_leave();
}

void v3_mode_set(void) { }                     /* INT 10h is the card's own BIOS */

/* The picture: the part of the monitor's bitmap the card last named. */
int v3_display(u32 *w, u32 *h)
{
    int bw = blit_w, bh = blit_h;
    if (!v3_on || bw <= 0 || bh <= 0) return 0;
    if (blit_x + bw > 2048) bw = 2048 - blit_x;
    if (blit_y + bh > 2048) bh = 2048 - blit_y;
    *w = bw;
    *h = bh;
    return bw > 0 && bh > 0;
}

void v3_row(u32 y, u32 *out)
{
    u32 w, h;
    if (!v3_display(&w, &h) || y >= h) return;
    const u32 *src = monitors[0].target_buffer->line[blit_y + y] + blit_x;
    for (u32 x = 0; x < w; x++) out[x] = src[x] & 0xffffff;
}

/* The BIOS image, for the guest's C000h. */
const u8 *v3_rom(u32 *len)
{
    if (!v3_on) return 0;
    u32 n = (u32)(v3_rom_end - v3_rom_start), sz = v3_rom_start[2] * 512u;   /* the size its header gives */
    *len = sz && sz < n ? sz : n;
    return v3_rom_start;
}

/* ---------------- start-up ---------------- */
void v3_init(void)
{
    if (!strstr(cmdline, "voodoo3")) return;
    if (!rom_present(0)) {
        kprintf("voodoo3: vmdos was built without the Voodoo 3 BIOS (roms/voodoo3/3k12sd.rom): off\n");
        return;
    }
    v3_log_on = strstr(cmdline, "v3log") ? 2 : 0;
    lfb_direct = !strstr(cmdline, "v3trap");
    bar0_lin = linear_hole(64u << 20, 32u << 20);
    if (!bar0_lin) { kprintf("voodoo3: no linear space for its BARs, off\n"); return; }
    bar1_lin = bar0_lin + (32u << 20);

    v3_enter();
    TIMER_USEC = (uint64_t)((cpuclock / 1000000.0) * 4294967296.0);
    VGACONST1 = (uint64_t)(cpuclock / 25175000.0 * 4294967296.0);
    VGACONST2 = (uint64_t)(cpuclock / 28322000.0 * 4294967296.0);
    v3_sched_init();
    timer_add(&keep_timer, keep_cb, 0, 0);
    timer_set_delay_u64(&keep_timer, TIMER_USEC * 500000);
    video_tables();

    const device_t *d = v3_device();
    card = d->init(d);
    vdo = v3_card_voodoo(card);
    svga = v3_card_svga(card);

    lfb_mapped = calloc(1, svga->vram_max >> 12);
    /* as the PC's BIOS leaves a PCI card: its BARs, I/O and memory on */
    v3_cfg_write(0x10, bar0_lin, 4);
    v3_cfg_write(0x14, bar1_lin, 4);
    v3_cfg_write(0x18, V3_IO_BASE, 4);
    v3_cfg_write(0x3C, 10, 1);
    v3_cfg_write(0x04, 3, 1);
    v3_on = 1;
    for (u32 a = 0xA0000; a < 0xC0000; a += 4096) map_page(a, 0, 0);   /* the VGA's memory: the card's */
    tlb_flush_range(0xA0000, 0x20000);
    vpci_bios_patch();                         /* the PCI BIOS for 32-bit code, its BIOS at C000h (bios_init: again at each restart) */
    lfb_remap(1);
    v3_leave();
    kprintf("voodoo3: Voodoo 3 3000 (86Box), 16 MB, its BIOS %u KB; registers at %x, frame buffer at %x, I/O at %x\n",
            v3_rom_start[2] / 2u, bar0_lin, bar1_lin, V3_IO_BASE);
}

