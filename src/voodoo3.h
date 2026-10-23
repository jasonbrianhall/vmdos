/* The emulated 3dfx Voodoo 3 (src/voodoo/v3.c, 86Box's card with its own
   BIOS): what the rest of vmdos calls. Off unless the command line has
   voodoo3; then it is the PC's video card: a PCI card (bus 0, device 1)
   with its registers (BAR0) and frame buffer (BAR1) at linear addresses
   nothing is mapped at, its I/O at V3_IO_BASE (D100h), the VGA's ports and
   A0000h-BFFFFh (unmapped: every access trapped and emulated), its BIOS at
   C000h. vmdos's own VGA steps aside; the screen shows the card's picture. */
#ifndef VMDOS_VOODOO3_H
#define VMDOS_VOODOO3_H

#include <stdint.h>

extern int v3_on;
void v3_init(void);                           /* boot, before XMS and DPMI take the heap */
int v3_window(uint32_t phys);                 /* the card's: a BAR, or A0000h-BFFFFh */
int v3_range(uint32_t phys, uint32_t len);    /* [phys, +len) inside one of them (DPMI 0800h) */
uint32_t v3_mmio_read(uint32_t phys, int size);
void v3_mmio_write(uint32_t phys, uint32_t val, int size);
int v3_io(uint16_t port, int size, int write, uint32_t *val);   /* its I/O BAR; 1: handled */
uint32_t v3_cfg_read(int reg, int size);      /* PCI configuration space */
void v3_cfg_write(int reg, uint32_t val, int size);
void v3_tick(void);                           /* every timer tick (boot CPU) */
void v3_mode_set(void);                       /* (the card's BIOS sets modes now: nothing) */
void v3_hv_sync(void);                        /* VMHV made its nested page tables: map the frame buffer there */
/* The picture, when the Voodoo's video processor owns the screen. */
int v3_display(uint32_t *w, uint32_t *h);
void v3_row(uint32_t y, uint32_t *out);       /* 0x00RRGGBB, w pixels */
const uint8_t *v3_rom(uint32_t *len);         /* its BIOS, for C000h (0: voodoo3 off) */

/* vpci.c: the PCI bus the guest sees (configuration mechanism 1, the PCI BIOS) */
int vpci_port(uint16_t port, int size, int write, uint32_t *val);
struct regs;
int vpci_int1a(struct regs *r);               /* AH=B1h: 1 handled */
void vpci_bios_patch(void);                   /* with voodoo3: the BIOS32 directory on, the card's BIOS at C000h */

#endif
