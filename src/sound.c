/* Sound: the guest's Sound Blaster Pro 2.0 + OPL3 (port 220h, IRQ 5, DMA 1),
   AdLib (388h) and MPU-401 / General MIDI (330h), emulated by SBPRO's core
   (src/sb), played through the real card by audio.cpp (HD Audio, AC'97 or
   a real Sound Blaster). Rendered from the timer interrupt. */
#include "kernel.h"
#include "sb/dsp.h"
#include "sb/sbout.h"
#include "sb/opl.h"
#include "sb/mpu.h"

int audio_start(const char *cmdline);
int audio_wanted(int nominal);
void audio_put_stereo(const int16_t *lr, int n);

#define SB_BASE 0x220
#define MPU_BASE 0x330
#define TICKS_PER_RENDER 4                  /* 250 Hz */

static int sound_on, sb_ready;

void sound_init(void)
{
    extern u32 fpu_present;
    if (!fpu_present) { kprintf("sound: off (the FM synth's set-up needs an x87)\n"); return; }
    opl_init(48000);
    dsp_init(SB_BASE);
    mpu_init(MPU_BASE);
    sb_out_init(5, 1);
    if (strstr(cmdline, "sb16=1")) dsp_set_sb16(1);
    sb_ready = 1;
    if (strstr(cmdline, "audio=off")) { kprintf("sound: off\n"); return; }
    sound_on = audio_start(cmdline);
    if (!sound_on) kprintf("sound: no sound card found\n");
}

/* INT 2Fh AX=5642h (VMSB.COM): BX = 0 ask, 2 SB Pro, 16 SB16. Returns AX = 2 or 16. */
int sound_sb_api(int bx)
{
    if (!sb_ready) return 0;
    if (bx == 2 || bx == 16) {
        dsp_set_sb16(bx == 16);
        kprintf("SB: now a Sound Blaster %s\n", bx == 16 ? "16 (DSP 4.05)" : "Pro (DSP 3.02)");
    }
    return dsp.sb16 ? 16 : 2;
}

static void check_irq(void)
{
    if (sb_take_irq()) vpic_raise(sb_irq_line());
}

int sound_port(u16 port, int write, u8 *v)
{
    if (!sb_ready) return 0;
    if (dsp_owns(port)) {
        if (write) dsp_out(port, *v); else *v = dsp_in(port);
        if (debug_level >= 4) kprintf("SB %s %03x %02x\n", write ? "out" : "in ", port, *v);
        check_irq();
        return 1;
    }
    if (mpu_owns(port)) {
        if (write) mpu_out(port, *v); else *v = mpu_in(port);
        return 1;
    }
    if (dma_owns(port)) {
        if (write) dma_out(port, *v); else *v = dma_in(port);
        return 1;
    }
    return 0;
}

void cdaudio_mix(int16_t *buf, int n);       /* cd.c: CD audio, mixed in (buf 0: just advance) */

/* Ctrl+Shift+F2: play through the next output found at boot (speakers, HDMI,
   another card ...). The key only asks; the switch is made at the start of
   the next sound tick, between two renders. */
int audio_out_count(void);
int audio_out_current(void);
const char *audio_out_name(int i);
int audio_out_select(int i);
static volatile int switch_req;

void sound_next_output(void) { switch_req = 1; }

static void switch_output(void)
{
    char msg[64];
    int n = sound_on ? audio_out_count() : 0;
    if (n < 2) {
        snprintf(msg, sizeof msg, n ? "Only output: %s" : "No sound output", n ? audio_out_name(0) : "");
        video_osd(msg);
        return;
    }
    int cur = audio_out_current();
    for (int k = 1; k < n; k++) {                         /* the next one that starts */
        int i = (cur + k) % n;
        if (audio_out_select(i)) {
            snprintf(msg, sizeof msg, "%d/%d %s", i + 1, n, audio_out_name(i));
            video_osd(msg);
            kprintf("sound: output %d of %d: %s\n", i + 1, n, audio_out_name(i));
            return;
        }
    }
    video_osd("No other output would start");
}

void sound_tick(void)
{
    static u32 div;
    if (switch_req) { switch_req = 0; switch_output(); }
    if (++div < TICKS_PER_RENDER) return;
    div = 0;
    if (!sb_ready) { cdaudio_mix(0, 48000 * TICKS_PER_RENDER / TICK_HZ); return; }
    static int16_t buf[2 * 1024];
    int n = sound_on ? audio_wanted(48000 * TICKS_PER_RENDER / TICK_HZ) : 48000 * TICKS_PER_RENDER / TICK_HZ;
    if (n > 1024) n = 1024;
    if (n > 0) {
        sb_render(buf, n);
        cdaudio_mix(buf, n);
        if (sound_on) audio_put_stereo(buf, n);
    }
    check_irq();
}
