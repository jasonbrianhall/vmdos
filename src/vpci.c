/* The PCI bus the DOS guest sees, when there is something on it (the
   emulated Voodoo 3, voodoo3 on the command line; the Ensoniq AudioPCI,
   on unless es1371=off; the NE2000 as a Realtek RTL8029, unless ne2k=isa):
   a host bridge at 00:00.0, the Voodoo at 00:01.0, the AudioPCI at
   00:02.0 (es1371.c) and the RTL8029 at 00:03.0 (ne2k.c), through configuration mechanism 1 (ports CF8h
   and CFCh-CFFh) and the PCI BIOS (INT 1Ah AX=B1xxh). Without it the ports
   read FFh as before: no PCI. video=vbe (vgabios.c): the PC's real bus
   instead, read-only except for the video card's own function, for its
   VESA BIOS, which finds its BARs there; the AudioPCI and the RTL8029
   are added to it in device numbers nothing there answers to. */
#include "kernel.h"
#include "voodoo3.h"
#include "es1371.h"

/* ne2k.c */
#define NE2K_DEVFN (3 << 3)                    /* 00:03.0 */
int ne2k_pci_on(void);
u32 ne2k_cfg_read(int reg, int size);
void ne2k_cfg_write(int reg, u32 val, int size);
void ne2k_pci_reset(void);

#define V3_DEVFN (1 << 3)                      /* device 1, function 0 */

static u32 cf8;

/* The emulated devices besides the Voodoo: their own configuration
   space, and where they are (devfn) on bus 0. */
static struct vfn {
    int (*on)(void);
    u32 (*rd)(int reg, int size);
    void (*wr)(int reg, u32 val, int size);
    int devfn;
} vfns[] = {
    { es_present, es_cfg_read, es_cfg_write, ES_DEVFN },
    { ne2k_pci_on, ne2k_cfg_read, ne2k_cfg_write, NE2K_DEVFN },
};
#define NVFN (int)(sizeof vfns / sizeof vfns[0])

static int any_vfn(void)
{
    for (int i = 0; i < NVFN; i++) if (vfns[i].on()) return 1;
    return 0;
}
static int bus_on(void) { return v3_on || vga_native || any_vfn(); }

/* video=vbe: the real bus, through the real ports (interrupts off: the
   kernel's drivers use them too). */
static u32 real_cfg(int bus, int devfn, int reg, int size, int write, u32 val)
{
    u32 fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl));
    outl(0xCF8, 0x80000000u | (u32)bus << 16 | (u32)devfn << 8 | (reg & 0xFC));
    u16 p = (u16)(0xCFC + (reg & 3));
    u32 v = 0;
    if (write) { if (size == 1) outb(p, (u8)val); else if (size == 2) outw(p, (u16)val); else outl(p, val); }
    else v = size == 1 ? inb(p) : size == 2 ? inw(p) : inl(p);
    if (fl & EFL_IF) sti();
    return v;
}

/* video=vbe: the emulated devices on the real bus 0, each in a device
   number nothing there answers to (from 02h up), found once. */
static void place(void)
{
    static int done;
    if (!vga_native || done) return;
    done = 1;
    int dev = 2;
    for (int i = 0; i < NVFN; i++) {
        while (dev < 32 && (real_cfg(0, dev << 3, 0, 2, 0, 0) & 0xFFFF) != 0xFFFF) dev++;
        if (dev < 32) vfns[i].devfn = dev++ << 3;
    }
}
static struct vfn *vfn_at(int bus, int devfn)
{
    if (bus) return 0;
    place();
    for (int i = 0; i < NVFN; i++) if (vfns[i].devfn == devfn && vfns[i].on()) return &vfns[i];
    return 0;
}

/* Configuration space of bus devfn, -1 if nothing is there. */
static int present(int bus, int devfn)
{
    if (vfn_at(bus, devfn)) return 1;
    if (vga_native) return (real_cfg(bus, devfn, 0, 2, 0, 0) & 0xFFFF) != 0xFFFF;
    return bus == 0 && (devfn == 0 || (v3_on && devfn == V3_DEVFN));
}

static u32 cfg_read(int bus, int devfn, int reg, int size)
{
    struct vfn *f = vfn_at(bus, devfn);
    if (f) return f->rd(reg, size);
    if (vga_native) return real_cfg(bus, devfn, reg, size, 0, 0);
    if (devfn == V3_DEVFN) return v3_cfg_read(reg, size);
    /* the host bridge: an Intel 440FX, nothing to set */
    static const u8 bridge[0x40] = { 0x86, 0x80, 0x37, 0x12, 0x06, 0x00, 0x80, 0x02, 0x02, 0x00, 0x00, 0x06 };
    u32 v = 0;
    for (int i = 0; i < size; i++) v |= (u32)(reg + i < 0x40 ? bridge[reg + i] : 0) << (8 * i);
    return v;
}

static void cfg_write(int bus, int devfn, int reg, u32 val, int size)
{
    struct vfn *f = vfn_at(bus, devfn);
    if (f) { f->wr(reg, val, size); return; }
    if (vga_native) { if (vga_native_pci(bus, devfn)) real_cfg(bus, devfn, reg, size, 1, val); return; }
    if (devfn == V3_DEVFN) v3_cfg_write(reg, val, size);
}

int vpci_port(u16 port, int size, int write, u32 *val)
{
    if (!bus_on()) return 0;
    if (port >= 0xCF8 && port <= 0xCFB) {
        if (port != 0xCF8 || size != 4) {      /* mechanism 2's byte ports: not here */
            if (!write) *val = 0xFFFFFFFFu;
            return 1;
        }
        if (write) cf8 = *val & 0x80FFFFFCu;
        else *val = cf8;
        return 1;
    }
    if (port >= 0xCFC && port <= 0xCFF) {
        int bus = (cf8 >> 16) & 0xFF, devfn = (cf8 >> 8) & 0xFF, reg = (cf8 & 0xFC) + (port & 3);
        if (!(cf8 & 0x80000000u) || !present(bus, devfn)) {
            if (!write) *val = 0xFFFFFFFFu;
            return 1;
        }
        if (write) cfg_write(bus, devfn, reg, *val, size);
        else *val = cfg_read(bus, devfn, reg, size);
        return 1;
    }
    if (es_io(port, size, write, val)) return 1;
    return v3_on ? v3_io(port, size, write, val) : 0;
}

/* ---------------- the Plug and Play BIOS (bios.asm pnp_entry) ----------------
   Its device nodes: the board's devices with their resources, every I/O
   range a 16-bit decoder. Windows 9x without a PnP BIOS takes the devices
   it detects itself (PIC, keyboard ...) as 10-bit ISA decoders, aliased
   every 400h, and then finds no free I/O range for a PCI card's 256 bytes
   (the Voodoo 3: Code 12); with these it keeps the PCI BIOS's assignments.
   An installed Windows switches to them through Add New Hardware (it
   detects "Plug and Play BIOS"). nopnp on the command line: none. */
static u8 *pn, *pn_node, pn_handle;
static void pn_w16(u32 v) { *pn++ = (u8)v; *pn++ = (u8)(v >> 8); }
static void pn_w32(u32 v) { pn_w16(v); pn_w16(v >> 16); }
static void pn_begin(u16 id, u8 base, u8 sub, u8 itf)  /* PNPxxxx (EISA-compressed), its type code */
{
    pn_node = pn;
    pn += 2;                                           /* size, at the end */
    *pn++ = pn_handle++;
    *pn++ = 0x41; *pn++ = 0xD0;                        /* "PNP" */
    *pn++ = (u8)(id >> 8); *pn++ = (u8)id;
    *pn++ = base; *pn++ = sub; *pn++ = itf;
    pn_w16(0x0003);                                    /* can't be disabled, not configurable */
}
static void pn_io(u16 base, u8 len) { *pn++ = 0x47; *pn++ = 1; pn_w16(base); pn_w16(base); *pn++ = 1; *pn++ = len; }   /* 16-bit decode */
static void pn_irq(int n) { *pn++ = 0x22; pn_w16(1u << n); }
static void pn_mem(u32 base, u32 len, int rw) { *pn++ = 0x86; pn_w16(9); *pn++ = (u8)rw; pn_w32(base); pn_w32(len); }
static void pn_end(void)
{
    for (int i = 0; i < 3; i++) { *pn++ = 0x79; *pn++ = 0; }   /* allocated, possible, compatible IDs: each ended */
    u32 n = (u32)(pn - pn_node);
    pn_node[0] = (u8)n; pn_node[1] = (u8)(n >> 8);
}

static void pnp_bios(void)
{
    if (strstr(cmdline, "nopnp")) return;
    u32 hdr = 0xF0000u + rd16(0xF0242);
    pn = gptr(0xF0000u + rd16(0xF0244));
    pn_handle = 0;
    extern u32 fpu_present;
    u32 top = hv_vm_top();

    /* Not the PCI bus, the keyboard or the DMA controllers: a Windows
       installed without a PnP BIOS keeps the ones it detected (the PCI bus
       can't be removed at all) and the BIOS's copies stay yellow beside
       them; they are found without it as before. */
    pn_begin(0x0C01, 0x05, 0x00, 0x00);                /* system board: its memory */
    pn_mem(0, 0xA0000, 1);
    pn_mem(0xF0000, 0x10000, 0);
    if (top > 0x100000) pn_mem(0x100000, top - 0x100000, 1);
    pn_end();
    pn_begin(0x0000, 0x08, 0x00, 0x01);                /* AT interrupt controllers */
    pn_io(0x20, 2); pn_io(0xA0, 2); pn_irq(2);
    pn_end();
    pn_begin(0x0100, 0x08, 0x02, 0x01);                /* AT timer */
    pn_io(0x40, 4); pn_irq(0);
    pn_end();
    pn_begin(0x0B00, 0x08, 0x03, 0x01);                /* AT real-time clock */
    pn_io(0x70, 2); pn_irq(8);
    pn_end();
    pn_begin(0x0F13, 0x09, 0x02, 0x00);                /* PS/2 mouse */
    pn_irq(12);
    pn_end();
    pn_begin(0x0800, 0x04, 0x01, 0x00);                /* speaker */
    pn_io(0x61, 1);
    pn_end();
    if (fpu_present) {
        pn_begin(0x0C04, 0x0B, 0x40, 0x00);            /* math coprocessor */
        pn_io(0xF0, 16); pn_irq(13);
        pn_end();
    }
    pn_begin(0x0C02, 0x08, 0x80, 0x00);                /* the board's other ports: POST, A20 */
    pn_io(0x80, 1); pn_io(0x92, 1);
    pn_end();
    pn_w16(0);                                         /* the end */

    wr8(hdr + 3, 'P');
    wr8(hdr + 8, 0);
    u8 sum = 0;
    for (int i = 0; i < 0x21; i++) sum += rd8(hdr + i);
    wr8(hdr + 8, (u8)-sum);
}

/* The BIOS32 service directory in the BIOS image (bios.asm): turned on, so
   32-bit code (Windows' PCI.VXD) finds the PCI BIOS. With a bus to show
   only; the BIOS image is copied anew at times, so bios_init calls this too
   (and ne2k_init, which comes after it at boot). */
void vpci_bios32(void)
{
    if (!v3_on && !any_vfn()) return;
    u32 d = 0xF0000u + rd16(0xF0234);
    wr8(d + 2, '2');
    wr8(d + 10, 0);
    u8 sum = 0;
    for (int i = 0; i < 16; i++) sum += rd8(d + i);
    wr8(d + 10, (u8)-sum);
    pnp_bios();                                          /* and the Plug and Play BIOS */
}

/* bios_init (boot, restarts): the emulated cards as a PC's BIOS leaves
   them, the BIOS32 directory, the Voodoo's BIOS. */
void vpci_bios_patch(void)
{
    es_boot();                                           /* es1371: on, and reset at each restart */
    ne2k_pci_reset();
    vpci_bios32();
    if (!v3_on) return;
    u32 n;
    const u8 *rom = v3_rom(&n);                          /* and the card's BIOS at C000h, as a PC's BIOS copies it */
    memcpy(gptr(0xC0000), rom, n);
}

static void done(struct regs *r, u8 ah)
{
    AH(r) = ah;
    if (ah) r->eflags |= EFL_CF; else r->eflags &= ~EFL_CF;
}

int vpci_int1a(struct regs *r)
{
    if (!bus_on() || AH(r) != 0xB1) return 0;
    int bus = BH(r), devfn = BL(r), reg = DI(r) & 0xFF;
    switch (AL(r)) {
    case 0x01:                                    /* installation check */
        r->edx = 0x20494350;                      /* "PCI " */
        AL(r) = 0x01;                             /* configuration mechanism 1 */
        BH(r) = 0x02; BL(r) = 0x10;               /* version 2.10 */
        *(u8 *)&r->ecx = vga_native ? 0xFF : 0;   /* last bus */
        done(r, 0);
        return 1;
    case 0x02: {                                  /* find device CX of vendor DX, SI-th */
        int n = SI(r);
        for (int b = 0; b < (vga_native ? 256 : 1); b++)
        for (int d = 0; d < 256; d += vga_native ? 1 : 8) {
            if (!present(b, d) || cfg_read(b, d, 0, 2) != DX(r) || cfg_read(b, d, 2, 2) != CX(r)) continue;
            if (n--) continue;
            BH(r) = (u8)b; BL(r) = (u8)d;
            done(r, 0);
            return 1;
        }
        done(r, 0x86);
        return 1; }
    case 0x03: {                                  /* find class ECX (24 bits), SI-th */
        int n = SI(r);
        for (int b = 0; b < (vga_native ? 256 : 1); b++)
        for (int d = 0; d < 256; d += vga_native ? 1 : 8) {
            if (!present(b, d) || (cfg_read(b, d, 8, 4) >> 8) != (r->ecx & 0xFFFFFF)) continue;
            if (n--) continue;
            BH(r) = (u8)b; BL(r) = (u8)d;
            done(r, 0);
            return 1;
        }
        done(r, 0x86);
        return 1; }
    case 0x08: case 0x09: case 0x0A: {            /* read byte, word, dword */
        int size = AL(r) == 0x08 ? 1 : AL(r) == 0x09 ? 2 : 4;
        if ((size == 2 && (reg & 1)) || (size == 4 && (reg & 3))) { done(r, 0x87); return 1; }
        u32 v = present(bus, devfn) ? cfg_read(bus, devfn, reg, size) : 0xFFFFFFFFu;
        if (size == 1) *(u8 *)&r->ecx = (u8)v;
        else if (size == 2) CX(r) = (u16)v;
        else r->ecx = v;
        done(r, 0);
        return 1; }
    case 0x0B: case 0x0C: case 0x0D: {           /* write byte, word, dword */
        int size = AL(r) == 0x0B ? 1 : AL(r) == 0x0C ? 2 : 4;
        if ((size == 2 && (reg & 1)) || (size == 4 && (reg & 3))) { done(r, 0x87); return 1; }
        if (present(bus, devfn)) cfg_write(bus, devfn, reg, size == 1 ? (u8)r->ecx : size == 2 ? CX(r) : r->ecx, size);
        done(r, 0);
        return 1; }
    }
    done(r, 0x81);                                /* function not supported */
    return 1;
}
