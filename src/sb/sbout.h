#ifndef SBOUT_H
#define SBOUT_H
#include <stdint.h>

/* The Sound Blaster's playback engine (from SBPRO): pulls 8-bit samples
   from the game's DMA buffer through the virtual 8237, resamples to
   48 kHz and flags the IRQ at the end of each block. */

void sb_out_init(int irq, int dma);
void sb_out_start(int autoinit, uint32_t len_bytes, int silence);
void sb_out_stop(void);
void sb_out_exit_autoinit(void);
void sb_out_raise_irq(void);
void sb_render(int16_t *out, int frames);   /* interleaved stereo, 48 kHz */
int  sb_take_irq(void);                     /* 1: raise the SB IRQ now */
int  sb_irq_line(void);

/* The 8237 (channels 0-3, ports 00h-0Fh and the page registers). */
int     dma_owns(uint16_t port);
uint8_t dma_in(uint16_t port);
void    dma_out(uint16_t port, uint8_t v);

#endif
