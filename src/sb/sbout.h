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
/* Recording: 8-bit samples from the microphone (48 kHz mono from the real
   card, audio_capture_read) averaged down to the DSP's rate and written into
   the game's DMA buffer, block by block as playback reads it. */
void sb_in_start(int autoinit, uint32_t len_bytes);
uint8_t sb_in_sample(void);                 /* DSP 20h: the latest input sample */
/* SB16 (DSP 4.05) Bxh/Cxh: 8- or 16-bit (channel 5), at dsp.rate; len in samples */
void sb16_start(int record, int autoinit, int bits16, int sign, int stereo, uint32_t len);
void sb_out_raise_irq16(void);
int  sb_irq_status(void);                   /* SB16 mixer 82h bits 0-1 */
void sb_irq_ack(int bits16);                /* base+Eh (8-bit) / base+Fh (16-bit) read */
int  sb_take_irq(void);                     /* 1: raise the SB IRQ now */
int  sb_irq_line(void);

/* The 8237 (channels 0-3, ports 00h-0Fh and the page registers). */
int     dma_owns(uint16_t port);
uint8_t dma_in(uint16_t port);
void    dma_out(uint16_t port, uint8_t v);

#endif
