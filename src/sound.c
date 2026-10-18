/* Sound: the guest's Sound Blaster Pro 2.0 + OPL3 (port 220h, IRQ 5, DMA 1),
   AdLib (388h) and MPU-401 / General MIDI (330h), emulated by SBPRO's core
   (src/sb), played through the real card by audio.cpp (HD Audio, AC'97 or
   a real Sound Blaster). Rendered from the timer interrupt, or by the
   helper CPU (smp.c); sound_lock keeps the guest's port accesses and the
   render apart. */
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
        rlock_take(&sound_lock);
        dsp_set_sb16(bx == 16);
        rlock_drop(&sound_lock);
        kprintf("SB: now a Sound Blaster %s\n", bx == 16 ? "16 (DSP 4.05)" : "Pro (DSP 3.02)");
    }
    return dsp.sb16 ? 16 : 2;
}

static void check_irq(void)
{
    if (sb_take_irq()) vpic_raise(sb_irq_line());
}

static int sound_port_(u16 port, int write, u8 *v)
{
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

int sound_port(u16 port, int write, u8 *v)
{
    if (!sb_ready) return 0;
    rlock_take(&sound_lock);
    int r = sound_port_(port, write, v);
    rlock_drop(&sound_lock);
    return r;
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

/* Ctrl+Shift+F3 / F4: volume down / up 10 %, F5: mute on and off (the
   samples are scaled on their way to the card; see audio.cpp). */
int audio_vol(void);
int audio_is_muted(void);
void audio_vol_set(int v);
void audio_mute_set(int m);
void sound_volume_key(int k)
{
    char msg[32];
    if (k == 0) audio_mute_set(!audio_is_muted());
    else { audio_vol_set((audio_vol() + 5) / 10 * 10 + k * 10); audio_mute_set(0); }
    if (audio_is_muted()) snprintf(msg, sizeof msg, "Sound muted");
    else snprintf(msg, sizeof msg, "Volume %d%%", audio_vol());
    video_osd(msg);
}

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

/* Every TICKS_PER_RENDER ticks: render the time that has passed. By the
   tick count, not by calls, so ticks counted in arrears (tick_poll, after a
   long stretch with interrupts off) are made up here too. On the helper
   CPU, a render the guest's port access holds up is made up the same way,
   and the IRQ goes to the virtual PIC from the boot CPU (sound_irq_poll). */
void sound_tick(void)
{
    static u32 last;
    if (switch_req) { switch_req = 0; switch_output(); }
    u32 el = ticks - last;
    if (el < TICKS_PER_RENDER) return;
    if (!rlock_try(&sound_lock)) return;
    last = ticks;
    if (el > 40) el = 40;                          /* a long stall: no point making it all up */
    int nominal = (int)(48000 / TICK_HZ * el);
    if (!sb_ready) { cdaudio_mix(0, nominal); rlock_drop(&sound_lock); return; }
    static int16_t buf[2 * 2048];
    int n = sound_on ? audio_wanted(nominal) : nominal;
    if (n > 2048) n = 2048;
    if (n > 0) {
        sb_render(buf, n);
        cdaudio_mix(buf, n);
    }
    if (!smp_on_ap()) check_irq();
    rlock_drop(&sound_lock);
    if (n > 0 && sound_on) audio_put_stereo(buf, n);
}

void sound_irq_poll(void)
{
    if (!sb_ready || !rlock_try(&sound_lock)) return;
    check_irq();
    rlock_drop(&sound_lock);
}
