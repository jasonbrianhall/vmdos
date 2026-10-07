#ifndef OPL_H
#define OPL_H
#include <stdint.h>

/* OPL3 FM synthesis (DOSBox's dbopl): the game's chip plus two private ones
   for the General MIDI synth. */

#define OPL_MAX_FRAMES 1024
#define OPL_CHIPS 3              /* game FM + two for the MIDI synth */

#ifdef __cplusplus
extern "C" {
#endif
void opl_init(int rate);                        /* load time only (uses the FPU) */
void opl_write(uint32_t reg, uint8_t val);      /* game chip, reg 0x000-0x1FF */
void midi_opl_write(int chip, uint32_t reg, uint8_t val); /* MIDI synth chip 0/1 */
void opl_mix(int32_t *out, int frames);         /* adds interleaved stereo */
extern int32_t opl_peak;                        /* debug: peak output level */
#ifdef __cplusplus
}
#endif

#endif
