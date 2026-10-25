#ifndef GUS_H
#define GUS_H
#include <stdint.h>

/* Gravis UltraSound (GF1, 1 MB DRAM): base 2X0h-2XFh and 3X0h-3X7h,
   ULTRASND=240,3,3,7,7 by default. */

void    gus_init(uint16_t base, int irq, int dma);
int     gus_owns(uint16_t port);
uint8_t gus_in(uint16_t port);
void    gus_out(uint16_t port, uint8_t v);
void    gus_dma_poll(void);             /* after an 8237 write: a DMA waiting on an unmask */
void    gus_mix(int32_t *lr, int frames);  /* adds interleaved stereo, 48 kHz */
int     gus_take_irq(void);             /* 1: raise the GF1 IRQ now */
int     gus_irq_line(void);
void    gus_stop(void);                 /* a restart: the card as after a reset */

#endif
