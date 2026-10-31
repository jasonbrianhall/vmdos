/* video=vbe: DOS gets the PC's own video card, as on a plain PC, instead of
   vmdos's emulated VGA drawn onto a framebuffer (video=fb, the default; GOP
   on a UEFI boot, GRUB's VESA mode on a BIOS/CSM one). Only on a legacy
   BIOS / CSM boot: there the card's VGA BIOS (its option ROM) has run and
   is still at C000h, with INT 10h pointing into it. A UEFI boot has no VGA
   BIOS (no VBE), so video=vbe is ignored there.

   The guest then gets the card's ROM (copied to C000h, as a BIOS shadows
   it), the interrupt vectors and BIOS data it set up, A0000h-BFFFFh mapped
   onto the card's memory, the VGA ports (3B0h-3DFh) and the card's I/O BARs
   straight to the hardware (no trap), and the card's PCI configuration
   space (the rest of the bus read-only) through ports CF8h/CFCh and the
   PCI BIOS, which VESA BIOSes use to find themselves. Its modes are the
   card's own (the monitor's real resolution, no scaling), DPMI 0800h maps
   its linear framebuffer write-combining. vmdos draws nothing: no scaling,
   overlay, log screen or screenshots; the INT 33h pointer is drawn into
   video memory in text modes only. */
#include "kernel.h"

int vga_native;

static u8 *rom;                       /* the card's ROM, as the CSM left it at C000h */
static u32 rom_len;
static u16 ivt_save[256][2];          /* the vectors that point into it (offset, segment) */
static u8 ivt_ours[256];
static u8 bda_save[0x100];            /* the real BIOS data area */
static int gpu_bus = -1, gpu_devfn;
static struct { u32 base, len; } mem_bar[6];

static u32 cfg_rd(int bus, int devfn, int reg)
{
    u32 fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl));
    outl(0xCF8, 0x80000000u | (u32)bus << 16 | (u32)devfn << 8 | (reg & 0xFC));
    u32 v = inl(0xCFC);
    if (fl & EFL_IF) sti();
    return v;
}

static void cfg_wr(int bus, int devfn, int reg, u32 v)
{
    u32 fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl));
    outl(0xCF8, 0x80000000u | (u32)bus << 16 | (u32)devfn << 8 | (reg & 0xFC));
    outl(0xCFC, v);
    if (fl & EFL_IF) sti();
}

/* The card: the display controller whose IDs are in the ROM's PCI data
   structure ("PCIR"), else the first VGA-compatible one decoding I/O. */
static int find_card(u16 ven, u16 dev)
{
    for (int pass = 0; pass < 2; pass++)
        for (int bus = 0; bus < 256; bus++)
            for (int df = 0; df < 256; df++) {
                u32 id = cfg_rd(bus, df, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (!(df & 7)) df += 7; continue; }
                u32 cls = cfg_rd(bus, df, 8) >> 8;
                int ok = pass == 0 ? (cls >> 16) == 3 && (id & 0xFFFF) == ven && id >> 16 == dev
                                   : (cls >> 8) == 0x0300 && (cfg_rd(bus, df, 4) & 1);
                if (ok) { gpu_bus = bus; gpu_devfn = df; return 1; }
                if (!(df & 7) && !(cfg_rd(bus, df, 0x0C) & 0x800000)) df += 7;   /* single-function */
            }
    return 0;
}

/* Its BARs: I/O ones go to the guest, memory ones are noted (the LFB). */
static void card_bars(void)
{
    for (int i = 0; i < 6; i++) {
        int reg = 0x10 + i * 4;
        u32 v = cfg_rd(gpu_bus, gpu_devfn, reg);
        if (!v) continue;
        u32 fl;
        __asm__ volatile("pushf; pop %0; cli" : "=r"(fl));
        u32 cmd = cfg_rd(gpu_bus, gpu_devfn, 4);
        cfg_wr(gpu_bus, gpu_devfn, 4, cmd & ~3u);           /* decoding off while it's sized */
        cfg_wr(gpu_bus, gpu_devfn, reg, 0xFFFFFFFFu);
        u32 sz = cfg_rd(gpu_bus, gpu_devfn, reg);
        cfg_wr(gpu_bus, gpu_devfn, reg, v);
        cfg_wr(gpu_bus, gpu_devfn, 4, cmd);
        if (fl & EFL_IF) sti();
        if (v & 1) {
            u32 base = v & 0xFFFCu, len = (~(sz & 0xFFFCu) + 1) & 0xFFFF;
            if (!len || len > 0x1000) len = 0x100;
            kprintf("video=vbe: card I/O %x-%x\n", base, base + len - 1);
            if (base >= 0x400) io_allow((u16)base, len);
            continue;
        }
        int is64 = (v & 6) == 4;
        u32 hi = is64 && i < 5 ? cfg_rd(gpu_bus, gpu_devfn, reg + 4) : 0;
        if (!hi) {
            mem_bar[i].base = v & ~0xFu;
            mem_bar[i].len = ~(sz & ~0xFu) + 1;
            kprintf("video=vbe: card memory %x, %u KiB%s\n", mem_bar[i].base, mem_bar[i].len >> 10, v & 8 ? " (prefetchable)" : "");
        }
        if (is64) i++;
    }
}

/* ---- display= with video=vbe: which card ----
   The PC's BIOS starts one card (the one legacy VGA goes to) and leaves its
   BIOS at C000h. display=ask lists every VGA-class card and asks,
   display=N takes card N. Another card than the BIOS's gets legacy VGA
   routed to it (the VGA Enable bit of the bridges above it, I/O and memory
   decoding off on the other cards), its x86 option ROM copied from its
   expansion ROM BAR, and that ROM's init entry run by the guest before the
   boot sector (bios.asm rom_post), as a BIOS runs it. */
#define MAX_CARDS 8
static struct { int bus, devfn; u32 id, cls; } cards[MAX_CARDS];
static int n_cards, rom_init = -1;    /* rom_init: AX for the ROM's init entry (bus, devfn), -1: none */

static void scan_cards(void)
{
    for (int bus = 0; bus < 256; bus++)
        for (int df = 0; df < 256; df++) {
            u32 id = cfg_rd(bus, df, 0);
            if ((id & 0xFFFF) == 0xFFFF) { if (!(df & 7)) df += 7; continue; }
            u32 cls = cfg_rd(bus, df, 8) >> 8;
            if ((cls >> 16) == 3 && n_cards < MAX_CARDS) {
                cards[n_cards].bus = bus; cards[n_cards].devfn = df;
                cards[n_cards].id = id; cards[n_cards].cls = cls;
                n_cards++;
            }
            if (!(df & 7) && !(cfg_rd(bus, df, 0x0C) & 0x800000)) df += 7;
        }
}

static const char *vendor_name(u16 v)
{
    switch (v) {
    case 0x8086: return "Intel";
    case 0x1002: return "AMD";
    case 0x10DE: return "NVIDIA";
    case 0x1234: return "QEMU/Bochs";
    case 0x15AD: return "VMware";
    case 0x1AF4: return "virtio";
    case 0x80EE: return "VirtualBox";
    case 0x121A: return "3dfx";
    case 0x102B: return "Matrox";
    case 0x5333: return "S3";
    case 0x1013: return "Cirrus";
    default: return "?";
    }
}

/* The menu: on the text screen the PC's BIOS left (GRUB keeps text mode for
   video=vbe) and the log. Keys from the PS/2 controller, polled (a USB
   keyboard too, through the BIOS's legacy emulation: USB isn't ours yet). */
static u16 *tscr;
static int trow;
static void tline(const char *s)
{
    if (!tscr || trow >= 25) return;
    for (int c = 0; c < 80; c++) tscr[trow * 80 + c] = 0x0700 | (u8)(*s ? *s++ : ' ');
    trow++;
}

static u16 pit_count(void) { outb(0x43, 0x00); u16 c = inb(0x40); return c | (u16)(inb(0x40) << 8); }
static void sleep_ms(int ms)                     /* as disk.c: PIT channel 0 reloads once a millisecond */
{
    u16 prev = pit_count();
    while (ms > 0) { u16 c = pit_count(); if (c > prev) ms--; prev = c; }
}

static int read_digit(int n, int secs)           /* 1..n, 0: Enter or time out */
{
    while (vkbd_take() >= 0) ;
    for (int ms = secs * 1000; ms > 0; ms -= 10) {
        u8 st = inb(0x64);
        if ((st & 0x21) == 0x01) vkbd_real_scancode(inb(0x60));
        int k;
        while ((k = vkbd_take()) >= 0) {
            if (k >= 0x02 && k <= 0x0A && k - 1 <= n) return k - 1;
            if (k == 0x1C) return 0;
        }
        sleep_ms(10);
    }
    return 0;
}

static int pick_card(int cur)
{
    const char *v = strstr(cmdline, "display=");
    if (!v || (v != cmdline && v[-1] != ' ')) return cur;
    v += 8;
    scan_cards();
    for (int i = 0; i < n_cards; i++)
        if (cards[i].bus == gpu_bus && cards[i].devfn == gpu_devfn) cur = i;
    if (!strncmp(v, "ask", 3)) {
        static char line[100];
        tscr = phys_low(0xB8000);
        trow = 0;
        for (int r = 0; r < 25; r++) tline("");
        trow = 0;
        tline("vmdos: video cards (* = the one the PC's BIOS started)");
        tline("");
        kprintf("video=vbe: video cards (* = the one the PC's BIOS started)\n");
        for (int i = 0; i < n_cards; i++) {
            snprintf(line, sizeof line, " %c%d: %s %04x:%04x at PCI %02x:%02x.%u%s", i == cur ? '*' : ' ', i + 1,
                      vendor_name(cards[i].id & 0xFFFF), cards[i].id & 0xFFFF, cards[i].id >> 16,
                      cards[i].bus, cards[i].devfn >> 3, cards[i].devfn & 7,
                      (cards[i].cls >> 8) == 0x0300 ? "" : " (not VGA-compatible)");
            tline(line);
            kprintf("%s\n", line);
        }
        if (n_cards < 2) { tline(""); tline("Only one card: nothing to choose."); kprintf("video=vbe: one card\n"); sleep_ms(2000); return cur; }
        tline("");
        snprintf(line, sizeof line, "Press 1-%d (Enter or 15 s: keep %d)", n_cards, cur + 1);
        tline(line);
        kprintf("%s\n", line);
        int k = read_digit(n_cards, 15);
        return k ? k - 1 : cur;
    }
    int k = 0;
    for (; *v >= '0' && *v <= '9' && k < 100; v++) k = k * 10 + (*v - '0');
    if (k < 1 || k > n_cards) {
        if (k) kprintf("video=vbe: display=%d, but there are %d video cards (display=ask lists them)\n", k, n_cards);
        return cur;
    }
    return k - 1;
}

/* A bridge whose range holds bus b but not bus not_b: turning its VGA
   Enable off keeps legacy VGA from b without touching b's own decoding. */
static int bridge_cuts(int b, int not_b)
{
    for (int bus = 0; bus < 256; bus++)
        for (int df = 0; df < 256; df++) {
            u32 id = cfg_rd(bus, df, 0);
            if ((id & 0xFFFF) == 0xFFFF) { if (!(df & 7)) df += 7; continue; }
            u32 hdr = cfg_rd(bus, df, 0x0C);
            if (((hdr >> 16) & 0x7F) == 1) {
                u32 r = cfg_rd(bus, df, 0x18), sec = (r >> 8) & 0xFF, sub = (r >> 16) & 0xFF;
                if ((u32)b >= sec && (u32)b <= sub && ((u32)not_b < sec || (u32)not_b > sub)) return 1;
            }
            if (!(df & 7) && !(hdr & 0x800000)) df += 7;
        }
    return 0;
}

/* Legacy VGA (3B0h-3DFh, A0000h-BFFFFh) to card c only: the VGA Enable bit
   of the bridges above it, off on the others. A card those bridges already
   cut off keeps its decoding (its framebuffer stays: the log screen); one
   that shares the path to c gets I/O and memory decoding turned off. */
static void route_vga(int c)
{
    int tb = cards[c].bus;
    for (int bus = 0; bus < 256; bus++)
        for (int df = 0; df < 256; df++) {
            u32 id = cfg_rd(bus, df, 0);
            if ((id & 0xFFFF) == 0xFFFF) { if (!(df & 7)) df += 7; continue; }
            u32 hdr = cfg_rd(bus, df, 0x0C);
            if (((hdr >> 16) & 0x7F) == 1) {                        /* a PCI-PCI bridge */
                u32 b = cfg_rd(bus, df, 0x18), sec = (b >> 8) & 0xFF, sub = (b >> 16) & 0xFF;
                u32 bc = cfg_rd(bus, df, 0x3C);
                if ((u32)tb >= sec && (u32)tb <= sub) {
                    cfg_wr(bus, df, 0x3C, bc | (8u << 16));       /* VGA Enable */
                    cfg_wr(bus, df, 4, (cfg_rd(bus, df, 4) & 0xFFFF) | 7);
                } else if (bc & (8u << 16)) cfg_wr(bus, df, 0x3C, bc & ~(8u << 16));
            }
            if (!(df & 7) && !(hdr & 0x800000)) df += 7;
        }
    for (int i = 0; i < n_cards; i++) {
        u32 cmd = cfg_rd(cards[i].bus, cards[i].devfn, 4) & 0xFFFF;
        if (i == c) cfg_wr(cards[i].bus, cards[i].devfn, 4, cmd | 7);
        else if (!bridge_cuts(cards[i].bus, tb)) cfg_wr(cards[i].bus, cards[i].devfn, 4, cmd & ~3u);
    }
}

/* Card i's memory BARs hold physical address a (and it still decodes them). */
static int card_holds(int i, u64 a)
{
    int bus = cards[i].bus, df = cards[i].devfn;
    u32 cmd = cfg_rd(bus, df, 4) & 0xFFFF;
    if (!(cmd & 2)) return 0;
    int hit = 0;
    for (int k = 0; k < 6 && !hit; k++) {
        int reg = 0x10 + k * 4;
        u32 v = cfg_rd(bus, df, reg);
        if (!v || (v & 1)) continue;
        int is64 = (v & 6) == 4 && k < 5;
        u32 hi = is64 ? cfg_rd(bus, df, reg + 4) : 0;
        u32 fl;
        __asm__ volatile("pushf; pop %0; cli" : "=r"(fl));
        cfg_wr(bus, df, 4, cmd & ~3u);
        cfg_wr(bus, df, reg, 0xFFFFFFFFu);
        u32 lo_sz = cfg_rd(bus, df, reg);
        cfg_wr(bus, df, reg, v);
        u32 hi_sz = 0xFFFFFFFFu;
        if (is64) { cfg_wr(bus, df, reg + 4, 0xFFFFFFFFu); hi_sz = cfg_rd(bus, df, reg + 4); cfg_wr(bus, df, reg + 4, hi); }
        cfg_wr(bus, df, 4, cmd);
        if (fl & EFL_IF) sti();
        u64 base = (u64)hi << 32 | (v & ~0xFu);
        u64 size = ~((u64)hi_sz << 32 | (lo_sz & ~0xFu)) + 1;
        if (!is64) size &= 0xFFFFFFFFu;
        if (size && a >= base && a - base < size) hit = 1;
        if (is64) k++;
    }
    return hit;
}

/* The log screen: the boot framebuffer (GOP) is on another card than the
   one DOS gets, and that card still decodes it: the boot log stays on it,
   live, while DOS runs on the VBE card. Not on the same card (its BIOS
   reprograms it). */
static int log_screen;
int vga_native_logscreen(void) { return vga_native && log_screen; }

static void find_log_screen(int c)
{
    u64 fbp = boot_fb_phys();
    if (!fbp || !bootcon_on) return;
    for (int i = 0; i < n_cards; i++)
        if (i != c && card_holds(i, fbp)) {
            log_screen = 1;
            kprintf("video=vbe: the log stays on card %04x:%04x's screen (PCI %02x:%02x.%u)\n", cards[i].id & 0xFFFF,
                    cards[i].id >> 16, cards[i].bus, cards[i].devfn >> 3, cards[i].devfn & 7);
            return;
        }
}

/* Card c's x86 option ROM into rom/rom_len. 0: none to be had. */
static int card_rom(int c)
{
    int bus = cards[c].bus, df = cards[c].devfn;
    u32 v = cfg_rd(bus, df, 0x30), cmd = cfg_rd(bus, df, 4) & 0xFFFF;
    cfg_wr(bus, df, 0x30, 0xFFFFF800u);
    u32 sz = cfg_rd(bus, df, 0x30) & 0xFFFFF800u;
    cfg_wr(bus, df, 0x30, v);
    if (!sz) { kprintf("video=vbe: the card has no option ROM\n"); return 0; }
    sz = ~sz + 1;
    u32 base = v & 0xFFFFF800u;
    if (!base) { kprintf("video=vbe: the PC's BIOS gave the card's option ROM no address\n"); return 0; }
    if (sz > (1u << 20)) sz = 1u << 20;
    const u8 *r = map_mmio64(base, sz);
    if (!r) { kprintf("video=vbe: the option ROM at %x can't be mapped\n", base); return 0; }
    cfg_wr(bus, df, 4, cmd | 2);
    cfg_wr(bus, df, 0x30, base | 1);
    int ok = 0;
    for (u32 off = 0; off + 0x1A < sz; ) {                         /* the images: the x86 one (code type 0) */
        if (r[off] != 0x55 || r[off + 1] != 0xAA) break;
        u32 p = off + (r[off + 0x18] | r[off + 0x19] << 8);
        if (p + 0x18 > sz || memcmp(r + p, "PCIR", 4)) break;
        u32 ilen = (r[p + 0x10] | r[p + 0x11] << 8) * 512u;
        if (r[p + 0x14] == 0) {
            u32 len = r[off + 2] * 512u;
            if (!len || len > ilen) len = ilen;
            if (len > 0x20000) len = 0x20000;
            if (rom) phys_release(rom, rom_len);
            rom_len = (len + 2047) & ~2047u;
            rom = phys_alloc(rom_len);
            memset(rom, 0, rom_len);
            memcpy(rom, r + off, len);
            kprintf("video=vbe: the card's option ROM, %u KiB (at %x)\n", len >> 10, base + off);
            ok = 1;
            break;
        }
        if ((r[p + 0x15] & 0x80) || !ilen) break;
        off += ilen;
    }
    cfg_wr(bus, df, 0x30, v);
    cfg_wr(bus, df, 4, cmd);
    if (!ok) kprintf("video=vbe: no x86 image in the card's option ROM\n");
    return ok;
}

int vga_native_post(void) { return vga_native ? rom_init : -1; }

/* ---- video=vbe on a UEFI boot ----
   Nothing ran a VGA BIOS, but a card's option ROM often carries a legacy
   x86 image next to its UEFI driver (discrete cards: both kinds of PC).
   vmdos.efi picks the card (display=, as here), takes that image from the
   copy UEFI keeps of the card's ROM, and passes it as the module "vgarom"
   with vbecard=BB:DD.F; the card is then started as on a BIOS boot when the
   card isn't the one the PC's BIOS started. No image (Intel's integrated
   graphics have none of their own): the framebuffer (GOP), as before. */
static u8 *mod_rom;
static u32 mod_rom_len;
static int uefi_rom;
void vga_native_rom_module(u8 *p, u32 len) { mod_rom = p; mod_rom_len = len; }

static int hexn(const char **p, int max)
{
    int v = 0, n = 0;
    for (; n < 2; n++, (*p)++) {
        char c = **p | 32;
        if (c >= '0' && c <= '9') v = v * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') v = v * 16 + (c - 'a' + 10);
        else break;
    }
    return n && v <= max ? v : -1;
}

static void uefi_init(void)
{
    const char *b = strstr(cmdline, "vbecard=");
    if (!mod_rom || !b) {
        kprintf("video=vbe: UEFI boot, and vmdos.efi found no legacy video BIOS on the card; using the framebuffer (GOP)\n");
        return;
    }
    b += 8;
    int bus = hexn(&b, 255), dev = *b == ':' ? (b++, hexn(&b, 31)) : -1, fn = *b == '.' ? (b++, *b - '0') : -1;
    if (bus < 0 || dev < 0 || fn < 0 || fn > 7) { kprintf("video=vbe: bad vbecard=\n"); return; }
    scan_cards();
    int c = -1;
    for (int i = 0; i < n_cards; i++) if (cards[i].bus == bus && cards[i].devfn == (dev << 3 | fn)) c = i;
    if (c < 0) { kprintf("video=vbe: no video card at PCI %02x:%02x.%u\n", bus, dev, fn); return; }
    rom_len = (mod_rom_len + 2047) & ~2047u;
    rom = phys_alloc(rom_len);
    memset(rom, 0, rom_len);
    memcpy(rom, mod_rom, mod_rom_len);
    route_vga(c);
    find_log_screen(c);
    gpu_bus = bus; gpu_devfn = dev << 3 | fn;
    rom_init = gpu_bus << 8 | gpu_devfn;
    uefi_rom = 1;
    u32 id = cards[c].id;
    kprintf("video=vbe: UEFI boot: card %04x:%04x at PCI %02x:%02x.%u, its legacy BIOS (%u KiB) starts it before DOS\n",
            id & 0xFFFF, id >> 16, bus, dev, fn, mod_rom_len >> 10);
    card_bars();
    io_allow(0x3B0, 0x30);
    io_allow(0x1CE, 2);
    vga_native = 1;
}

void vga_native_init(void)
{
    const char *o = strstr(cmdline, "video=");
    if (!o || (o != cmdline && o[-1] != ' ') || strncmp(o + 6, "vbe", 3)) return;
    if (strstr(cmdline, "voodoo3")) { kprintf("video=vbe: not with voodoo3\n"); return; }
    if (!strncmp(cmdline, "vmdos.efi", 9)) { uefi_init(); return; }
    const u8 *r = phys_low(0xC0000);
    const u16 *ivt = phys_low(0);
    u32 i10 = LIN(ivt[0x10 * 2 + 1], ivt[0x10 * 2]);
    u32 len = r[2] * 512u;
    if (r[0] != 0x55 || r[1] != 0xAA || !len) { kprintf("video=vbe: no video BIOS at C000h; using the framebuffer\n"); return; }
    if (len > 0x20000) len = 0x20000;
    if (i10 < 0xC0000 || i10 >= 0xC0000 + len) {
        kprintf("video=vbe: INT 10h is at %04x:%04x, not in the video BIOS; using the framebuffer\n", ivt[0x21], ivt[0x20]);
        return;
    }
    rom_len = (len + 2047) & ~2047u;
    rom = phys_alloc(rom_len);
    memcpy(rom, r, len);
    for (int v = 0; v < 256; v++) {
        u32 a = LIN(ivt[v * 2 + 1], ivt[v * 2]);
        if (a >= 0xC0000 && a < 0xC0000 + len) { ivt_save[v][0] = ivt[v * 2]; ivt_save[v][1] = ivt[v * 2 + 1]; ivt_ours[v] = 1; }
    }
    memcpy(bda_save, phys_low(0x400), sizeof bda_save);
    u16 pcir = *(const u16 *)(r + 0x18);
    int named = pcir + 8u < len && !memcmp(r + pcir, "PCIR", 4);
    int found = find_card(named ? *(const u16 *)(r + pcir + 4) : 0xFFFF, named ? *(const u16 *)(r + pcir + 6) : 0xFFFF);
    if (!found) gpu_bus = -1;
    int pick = pick_card(-1);
    if (pick >= 0 && (cards[pick].bus != gpu_bus || cards[pick].devfn != gpu_devfn)) {
        if (card_rom(pick)) {
            route_vga(pick);
            find_log_screen(pick);
            gpu_bus = cards[pick].bus; gpu_devfn = cards[pick].devfn;
            rom_init = gpu_bus << 8 | gpu_devfn;
            memset(ivt_ours, 0, sizeof ivt_ours);          /* the new card's BIOS sets its own vectors */
            found = 1;
            kprintf("video=vbe: card %d chosen; its BIOS starts it before DOS\n", pick + 1);
        } else kprintf("video=vbe: keeping the card the PC's BIOS started\n");
    }
    if (found) {
        u32 id = cfg_rd(gpu_bus, gpu_devfn, 0);
        kprintf("video=vbe: the card is %04x:%04x at PCI %02x:%02x.%u\n", id & 0xFFFF, id >> 16, gpu_bus, gpu_devfn >> 3, gpu_devfn & 7);
        card_bars();
    } else kprintf("video=vbe: the card isn't on the PCI bus\n");
    io_allow(0x3B0, 0x30);                                /* the VGA */
    io_allow(0x1CE, 2);                                   /* Bochs / QEMU VBE (SeaVGABIOS) */
    vga_native = 1;
    if (rom_init < 0)
        kprintf("video=vbe: the card's own BIOS (%u KiB at C000h, INT 10h %04x:%04x), mode %02x\n",
                len >> 10, ivt[0x21], ivt[0x20], bda_save[0x49]);
}

/* bios_init: after vmdos's BIOS has set up the IVT and BIOS data. */
void vga_native_bios(void)
{
    if (!vga_native) return;
    for (u32 a = 0xA0000; a < 0xC0000; a += 4096) map_page(a, a, 7);   /* (map_page: the card's) */
    tlb_flush();
    memcpy(gptr(0xC0000), rom, rom_len);
    for (int v = 0; v < 256; v++)
        if (ivt_ours[v]) { wr16(v * 4, ivt_save[v][0]); wr16(v * 4 + 2, ivt_save[v][1]); }
    wr16(0x42 * 4, rd16(0xF0000 + 0xFF * 2)); wr16(0x42 * 4 + 2, 0xF000);   /* the BIOS's own INT 10h: an IRET */
    if (uefi_rom) return;                                 /* no BIOS data from a UEFI boot: the card's BIOS sets it */
    for (u32 a = 0x49; a <= 0x66; a++) wr8(BDA + a, bda_save[a]);
    for (u32 a = 0x84; a <= 0x8A; a++) wr8(BDA + a, bda_save[a]);
    wr32(BDA + 0xA8, *(u32 *)&bda_save[0xA8]);            /* the video save pointer table (in the ROM) */
    wr16(BDA + 0x10, (rd16(BDA + 0x10) & ~0x30u) | (*(u16 *)&bda_save[0x10] & 0x30u));   /* initial video mode */
}

u16 vga_native_rom_end(void) { return vga_native ? (u16)((0xC0000 + rom_len) >> 4) : 0; }

/* How much to map from phys: the rest of the BAR (up to 64 MiB), as the
   emulated VESA card maps all its memory, for programs that ask for less
   than they draw. 0: not the card's memory. */
u32 vga_native_lfb(u32 phys, u32 len)
{
    if (!vga_native) return 0;
    for (int i = 0; i < 6; i++)
        if (mem_bar[i].len && phys >= mem_bar[i].base && phys - mem_bar[i].base + len <= mem_bar[i].len) {
            u32 n = mem_bar[i].len - (phys - mem_bar[i].base);
            return n > (64u << 20) ? (len > (64u << 20) ? len : 64u << 20) : n;
        }
    return 0;
}

/* VMHV: the card's ROM at C000h (its length) and its memory BARs below
   4 GiB (the LFB), for the VM's memory map. */
u32 vga_native_rom_len(void) { return vga_native ? rom_len : 0; }
int vga_native_bar(int i, u32 *base, u32 *len)
{
    if (!vga_native || i < 0 || i > 5 || !mem_bar[i].len) return 0;
    *base = mem_bar[i].base; *len = mem_bar[i].len;
    return 1;
}

int vga_native_pci(int bus, int devfn) { return vga_native && bus == gpu_bus && devfn == gpu_devfn; }
