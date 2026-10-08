#ifndef DSP_H
#define DSP_H
#include <stdint.h>

/* Sound Blaster Pro 2.0 DSP (v3.02), or a Sound Blaster 16 (v4.05) when
   dsp.sb16 is set; mixer and FM status, port-level. */

typedef struct {
    uint8_t  time_constant;     /* 40h: rate = 1000000 / (256 - tc) */
    uint16_t block_len;         /* 48h, bytes - 1 */
    uint8_t  speaker;
    uint8_t  dac_value;         /* 10h direct output */
    uint8_t  paused;
    uint8_t  sb16;              /* SB16 mode (sb16=1, VMSB 16) */
    uint16_t rate;              /* 41h/42h: sample rate (0: use the time constant) */
    uint8_t  mixer[256];
} DspState;

extern DspState dsp;

void    dsp_init(uint16_t base);
int     dsp_owns(uint16_t port);     /* any port SBPRO emulates, including FM */
uint8_t dsp_in(uint16_t port);
void    dsp_out(uint16_t port, uint8_t v);
void    dsp_set_sb16(int on);       /* switch model (resets the DSP) */

#endif
