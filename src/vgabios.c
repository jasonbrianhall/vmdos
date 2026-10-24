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

void vga_native_init(void)
{
    const char *o = strstr(cmdline, "video=");
    if (!o || (o != cmdline && o[-1] != ' ') || strncmp(o + 6, "vbe", 3)) return;
    if (!strncmp(cmdline, "vmdos.efi", 9)) {
        kprintf("video=vbe: a UEFI boot has no VGA BIOS (VBE); using the framebuffer (GOP)\n");
        return;
    }
    if (strstr(cmdline, "voodoo3")) { kprintf("video=vbe: not with voodoo3\n"); return; }
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
    if (find_card(named ? *(const u16 *)(r + pcir + 4) : 0xFFFF, named ? *(const u16 *)(r + pcir + 6) : 0xFFFF)) {
        u32 id = cfg_rd(gpu_bus, gpu_devfn, 0);
        kprintf("video=vbe: the card is %04x:%04x at PCI %02x:%02x.%u\n", id & 0xFFFF, id >> 16, gpu_bus, gpu_devfn >> 3, gpu_devfn & 7);
        card_bars();
    } else kprintf("video=vbe: the card isn't on the PCI bus\n");
    io_allow(0x3B0, 0x30);                                /* the VGA */
    io_allow(0x1CE, 2);                                   /* Bochs / QEMU VBE (SeaVGABIOS) */
    vga_native = 1;
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

int vga_native_pci(int bus, int devfn) { return vga_native && bus == gpu_bus && devfn == gpu_devfn; }
