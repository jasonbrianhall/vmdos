/* The PCI bus the DOS guest sees, when there is something on it (the
   emulated Voodoo 3, voodoo3 on the command line): a host bridge at 00:00.0
   and the card at 00:01.0, through configuration mechanism 1 (ports CF8h
   and CFCh-CFFh) and the PCI BIOS (INT 1Ah AX=B1xxh). Without it the ports
   read FFh as before: no PCI. The PC's real PCI bus is never shown. */
#include "kernel.h"
#include "voodoo3.h"

#define V3_DEVFN (1 << 3)                      /* device 1, function 0 */

static u32 cf8;

/* Configuration space of bus 0 devfn, -1 if nothing is there. */
static int present(int bus, int devfn) { return bus == 0 && (devfn == 0 || devfn == V3_DEVFN); }

static u32 cfg_read(int devfn, int reg, int size)
{
    if (devfn == V3_DEVFN) return v3_cfg_read(reg, size);
    /* the host bridge: an Intel 440FX, nothing to set */
    static const u8 bridge[0x40] = { 0x86, 0x80, 0x37, 0x12, 0x06, 0x00, 0x80, 0x02, 0x02, 0x00, 0x00, 0x06 };
    u32 v = 0;
    for (int i = 0; i < size; i++) v |= (u32)(reg + i < 0x40 ? bridge[reg + i] : 0) << (8 * i);
    return v;
}

static void cfg_write(int devfn, int reg, u32 val, int size)
{
    if (devfn == V3_DEVFN) v3_cfg_write(reg, val, size);
}

int vpci_port(u16 port, int size, int write, u32 *val)
{
    if (!v3_on) return 0;
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
        if (write) cfg_write(devfn, reg, *val, size);
        else *val = cfg_read(devfn, reg, size);
        return 1;
    }
    return v3_io(port, size, write, val);
}

/* The BIOS32 service directory in the BIOS image (bios.asm): turned on, so
   32-bit code (Windows' PCI.VXD) finds the PCI BIOS. With a bus to show
   only; the BIOS image is copied anew at times, so bios_init calls this too. */
void vpci_bios_patch(void)
{
    if (!v3_on) return;
    u32 d = 0xF0000u + rd16(0xF0234);
    wr8(d + 2, '2');
    wr8(d + 10, 0);
    u8 sum = 0;
    for (int i = 0; i < 16; i++) sum += rd8(d + i);
    wr8(d + 10, (u8)-sum);
}

static void done(struct regs *r, u8 ah)
{
    AH(r) = ah;
    if (ah) r->eflags |= EFL_CF; else r->eflags &= ~EFL_CF;
}

int vpci_int1a(struct regs *r)
{
    if (!v3_on || AH(r) != 0xB1) return 0;
    int bus = BH(r), devfn = BL(r), reg = DI(r) & 0xFF;
    switch (AL(r)) {
    case 0x01:                                    /* installation check */
        r->edx = 0x20494350;                      /* "PCI " */
        AL(r) = 0x01;                             /* configuration mechanism 1 */
        BH(r) = 0x02; BL(r) = 0x10;               /* version 2.10 */
        *(u8 *)&r->ecx = 0;                       /* last bus */
        done(r, 0);
        return 1;
    case 0x02: {                                  /* find device CX of vendor DX, SI-th */
        int n = SI(r);
        for (int d = 0; d < 256; d += 8) {
            if (!present(0, d) || cfg_read(d, 0, 2) != DX(r) || cfg_read(d, 2, 2) != CX(r)) continue;
            if (n--) continue;
            BH(r) = 0; BL(r) = (u8)d;
            done(r, 0);
            return 1;
        }
        done(r, 0x86);
        return 1; }
    case 0x03: {                                  /* find class ECX (24 bits), SI-th */
        int n = SI(r);
        for (int d = 0; d < 256; d += 8) {
            if (!present(0, d) || (cfg_read(d, 8, 4) >> 8) != (r->ecx & 0xFFFFFF)) continue;
            if (n--) continue;
            BH(r) = 0; BL(r) = (u8)d;
            done(r, 0);
            return 1;
        }
        done(r, 0x86);
        return 1; }
    case 0x08: case 0x09: case 0x0A: {            /* read byte, word, dword */
        int size = AL(r) == 0x08 ? 1 : AL(r) == 0x09 ? 2 : 4;
        if ((size == 2 && (reg & 1)) || (size == 4 && (reg & 3))) { done(r, 0x87); return 1; }
        u32 v = present(bus, devfn) ? cfg_read(devfn, reg, size) : 0xFFFFFFFFu;
        if (size == 1) *(u8 *)&r->ecx = (u8)v;
        else if (size == 2) CX(r) = (u16)v;
        else r->ecx = v;
        done(r, 0);
        return 1; }
    case 0x0B: case 0x0C: case 0x0D: {           /* write byte, word, dword */
        int size = AL(r) == 0x0B ? 1 : AL(r) == 0x0C ? 2 : 4;
        if ((size == 2 && (reg & 1)) || (size == 4 && (reg & 3))) { done(r, 0x87); return 1; }
        if (present(bus, devfn)) cfg_write(devfn, reg, size == 1 ? (u8)r->ecx : size == 2 ? CX(r) : r->ecx, size);
        done(r, 0);
        return 1; }
    }
    done(r, 0x81);                                /* function not supported */
    return 1;
}
