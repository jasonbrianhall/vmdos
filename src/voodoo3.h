/* The emulated 3dfx Voodoo 3 (src/voodoo/v3.c, 86Box's Voodoo core): what
   the rest of vmdos calls. Off unless the command line has voodoo3; then
   it is a PCI card (bus 0, device 1) with its registers (BAR0) at linear
   addresses nothing is mapped at, so every guest access faults and is
   emulated; its frame buffer (BAR1) is mapped onto the card's memory except
   where the card must see the writes (the command FIFO); its I/O registers
   at V3_IO_BASE (D100h).
   vmdos's own VGA stays the VGA; once a program turns on the Voodoo's
   video processor (Glide does), the screen shows the Voodoo's picture. */
#ifndef VMDOS_VOODOO3_H
#define VMDOS_VOODOO3_H

#include <stdint.h>

extern int v3_on;
void v3_init(void);                           /* boot, before XMS and DPMI take the heap */
int v3_window(uint32_t phys);                 /* in BAR0 or BAR1 */
int v3_range(uint32_t phys, uint32_t len);    /* [phys, +len) inside one of them (DPMI 0800h) */
uint32_t v3_mmio_read(uint32_t phys, int size);
void v3_mmio_write(uint32_t phys, uint32_t val, int size);
int v3_io(uint16_t port, int size, int write, uint32_t *val);   /* its I/O BAR; 1: handled */
uint32_t v3_cfg_read(int reg, int size);      /* PCI configuration space */
void v3_cfg_write(int reg, uint32_t val, int size);
void v3_tick(void);                           /* every timer tick (boot CPU) */
void v3_mode_set(void);                       /* INT 10h set a mode: the VGA shows again */
void v3_hv_sync(void);                        /* VMHV made its nested page tables: map the frame buffer there */
/* The picture, when the Voodoo's video processor owns the screen. */
int v3_display(uint32_t *w, uint32_t *h);
void v3_row(uint32_t y, uint32_t *out);       /* 0x00RRGGBB, w pixels */

/* vpci.c: the PCI bus the guest sees (configuration mechanism 1, the PCI BIOS) */
int vpci_port(uint16_t port, int size, int write, uint32_t *val);
struct regs;
int vpci_int1a(struct regs *r);               /* AH=B1h: 1 handled */
void vpci_bios_patch(void);                   /* the BIOS32 directory on, with voodoo3 */

#endif
