#ifndef ES1371_H
#define ES1371_H
#include <stdint.h>

/* The emulated Ensoniq AudioPCI 97 (ES1371), es1371 on the command line
   (es1371.c): on the guest's PCI bus (vpci.c), mixed into sound.c's output. */
#define ES_DEVFN (2 << 3)                     /* 00:02.0 */

extern int es_on;
void es_boot(void);                            /* boot and restarts: on from the command line, reset */
uint32_t es_cfg_read(int reg, int size);       /* PCI configuration space */
void es_cfg_write(int reg, uint32_t val, int size);
int es_io(uint16_t port, int size, int write, uint32_t *val);   /* its I/O BAR: 1 handled */
void es_render(int16_t *buf, int n);           /* mixed into n stereo 48 kHz frames (sound_lock held) */
int es_take_irq(void);                         /* 1: raise its IRQ now */
int es_irq_line(void);

#endif
