/* Sound Blaster Pro playback engine, from SBPRO (sbpro2hda), adapted for
 * vmdos: the 8237 DMA controller is virtual (dma_in/dma_out), the DMA
 * buffer is read straight from guest memory, and the IRQ goes to the
 * virtual PIC.
 *
 * Rendering runs in the timer interrupt: every rendered 48 kHz frame
 * advances the SB stream by rate/48000 samples. When a DSP block finishes,
 * the IRQ is flagged (sb_take_irq).
 */
#include "../kernel.h"
#include "dsp.h"
#include "sbout.h"
#include "opl.h"

#define OUT_RATE 48000

static int sb_irq = 5, sb_dma = 1;
static volatile int irq_pending;

static struct {
    int active, autoinit, silence;
    uint32_t dma_phys, dma_len, dma_pos;
    const volatile uint8_t *mem;
    uint32_t block_len, block_left;
    uint32_t step, frac;
    int stereo;
    int16_t prev_l, prev_r, cur_l, cur_r;
} s;

/* ---------------------------------------------------------------- 8237 */
static struct { uint16_t addr, count; uint8_t page, mode; } dch[4];
static uint8_t dma_mask = 0x0F, dma_ff;
static int dma_valid, dma_tc, dma_done;

static int page_channel(uint16_t port)
{
    switch (port) { case 0x87: return 0; case 0x83: return 1; case 0x81: return 2; case 0x82: return 3; }
    return -1;
}

int dma_owns(uint16_t port) { return port <= 0x0F || (port >= 0x81 && port <= 0x8F); }

void dma_out(uint16_t port, uint8_t v)
{
    if (port < 8) {
        int ch = port >> 1;
        uint16_t *r = (port & 1) ? &dch[ch].count : &dch[ch].addr;
        *r = dma_ff ? (uint16_t)((*r & 0xFF) | v << 8) : (uint16_t)((*r & 0xFF00) | v);
        dma_ff ^= 1;
        if (ch == sb_dma) dma_valid = 0;
        return;
    }
    switch (port) {
    case 0x0A: if (v & 4) dma_mask |= 1 << (v & 3); else dma_mask &= ~(1 << (v & 3)); return;
    case 0x0B: dch[v & 3].mode = v; return;
    case 0x0C: dma_ff = 0; return;
    case 0x0D: dma_ff = 0; dma_mask = 0x0F; return;
    case 0x0E: dma_mask = 0; return;
    case 0x0F: dma_mask = v & 0x0F; return;
    }
    int ch = page_channel(port);
    if (ch >= 0) { dch[ch].page = v; if (ch == sb_dma) dma_valid = 0; }
}

uint8_t dma_in(uint16_t port)
{
    if (port < 8) {
        int ch = port >> 1;
        uint32_t v;
        if (ch == sb_dma && dma_valid) {
            if (port & 1) v = dma_done ? 0xFFFF : s.dma_len - 1 - s.dma_pos;
            else v = (s.dma_phys & 0xFFFF) + (dma_done ? s.dma_len : s.dma_pos);
        } else v = (port & 1) ? dch[ch].count : dch[ch].addr;
        uint8_t b = dma_ff ? (uint8_t)(v >> 8) : (uint8_t)v;
        dma_ff ^= 1;
        return b;
    }
    if (port == 0x08) {
        uint8_t v = 0;
        if (dma_tc) v |= 1 << sb_dma;
        if (s.active && !s.silence) v |= 0x10 << sb_dma;
        dma_tc = 0;
        return v;
    }
    if (port == 0x0F) return dma_mask;
    int ch = page_channel(port);
    return ch >= 0 ? dch[ch].page : 0xFF;
}

static void read_dma_controller(void)
{
    s.dma_phys = (uint32_t)dch[sb_dma].page << 16 | dch[sb_dma].addr;
    s.dma_len = (uint32_t)dch[sb_dma].count + 1;
    s.dma_pos = 0;
    dma_valid = 1;
    dma_done = 0;
    dma_tc = 0;
}

static int map_dma_buffer(void)
{
    if (s.dma_phys + s.dma_len > GUEST_TOP) return 0;
    s.mem = (const volatile uint8_t *)gptr(s.dma_phys);
    return 1;
}

/* ---------------------------------------------------------------- control */

void sb_out_init(int irq, int dma)
{
    sb_irq = irq;
    sb_dma = dma & 3;
    memset(&s, 0, sizeof s);
}

int sb_irq_line(void) { return sb_irq; }

void sb_out_start(int autoinit, uint32_t len_bytes, int silence)
{
    uint32_t tc = dsp.time_constant;
    uint32_t rate = 1000000u / (256 - tc);

    s.stereo = (dsp.mixer[0x0E] & 0x02) && !silence;
    if (s.stereo) rate /= 2;                /* TC was set for twice the frame rate */
    if (rate < 1000) rate = 1000;
    if (rate > 48000) rate = 48000;
    s.step = (rate << 16) / OUT_RATE;
    s.frac = 0;

    s.silence = silence;
    dsp.paused = 0;                         /* a new transfer ends a D0h pause */
    s.autoinit = autoinit;
    s.block_len = len_bytes ? len_bytes : 1;
    s.block_left = s.block_len;

    if (!silence) {
        read_dma_controller();
        if (!map_dma_buffer()) {
            dbg(1, "SB: DMA buffer %x not in conventional memory\n", s.dma_phys);
            s.active = 0;
            return;
        }
    }
    dbg(2, "SB: start %s len=%u rate=%u%s dma=%x/%u\n", autoinit ? "auto" : "single",
        s.block_len, rate, s.stereo ? " stereo" : "", s.dma_phys, s.dma_len);

    /* Detection transfers (a few bytes) finish in microseconds on a real
       card; complete them now so the IRQ arrives before any timeout. */
    if (!autoinit && s.block_len <= 64) {
        if (!silence) {
            s.dma_pos = s.block_len % (s.dma_len ? s.dma_len : 1);
            if (s.dma_pos == 0) { dma_done = 1; dma_tc = 1; }
        }
        s.active = 0;
        irq_pending = 1;
        return;
    }
    s.active = 1;
}

void sb_out_stop(void)            { s.active = 0; irq_pending = 0; }
void sb_out_exit_autoinit(void)   { s.autoinit = 0; }
void sb_out_raise_irq(void)       { irq_pending = 1; }

int sb_take_irq(void)
{
    if (!irq_pending) return 0;
    irq_pending = 0;
    return 1;
}

/* ---------------------------------------------------------------- render */

static uint8_t fetch(void)
{
    uint8_t b = 0x80;
    if (!s.silence) {
        b = s.mem[s.dma_pos];
        if (++s.dma_pos >= s.dma_len) {                  /* 8237 terminal count */
            s.dma_pos = 0;
            dma_tc = 1;
            if (!s.autoinit) dma_done = 1;
        }
    }
    if (--s.block_left == 0) {
        irq_pending = 1;
        if (s.autoinit) s.block_left = s.block_len;
        else s.active = 0;
    }
    return b;
}

static void next_frame(void)
{
    s.prev_l = s.cur_l;
    s.prev_r = s.cur_r;
    int16_t l = (int16_t)(((int)fetch() - 128) * 256);
    int16_t r = l;
    if (s.stereo && s.active) r = (int16_t)(((int)fetch() - 128) * 256);
    s.cur_l = l;
    s.cur_r = r;
}

static int32_t mix[2 * OPL_MAX_FRAMES];
void speaker_mix(int32_t *lr, int frames);            /* vdev.c: PC speaker */

/* Soft limiter: linear up to 3/4 of full scale, then 4:1 until the rail,
   so loud moments (music + effects together) squash instead of crackle. */
static inline int16_t limit(int32_t v)
{
    const int32_t knee = 24576;
    if (v > knee) { v = knee + (v - knee) / 4; if (v > 32767) v = 32767; }
    else if (v < -knee) { v = -knee + (v + knee) / 4; if (v < -32768) v = -32768; }
    return (int16_t)v;
}

static void render_chunk(int16_t *out, int frames)
{
    for (int i = 0; i < frames; i++) {
        int32_t l, r;
        if (s.active && !dsp.paused) {
            s.frac += s.step;
            while (s.frac >= 0x10000 && s.active) {
                s.frac -= 0x10000;
                next_frame();
            }
            int32_t f = s.frac;                          /* linear interpolation */
            l = s.prev_l + (((s.cur_l - s.prev_l) * f) >> 16);
            r = s.prev_r + (((s.cur_r - s.prev_r) * f) >> 16);
        } else {
            l = r = ((int32_t)dsp.dac_value - 128) * 256;  /* direct DAC (10h) */
        }
        if (!dsp.speaker) l = r = 0;
        mix[i * 2] = l;
        mix[i * 2 + 1] = r;
    }
    opl_mix(mix, frames);
    speaker_mix(mix, frames);
    for (int i = 0; i < frames * 2; i++)
        out[i] = limit(mix[i]);
}

void sb_render(int16_t *out, int frames)
{
    while (frames > 0) {
        int n = frames > OPL_MAX_FRAMES ? OPL_MAX_FRAMES : frames;
        render_chunk(out, n);
        out += n * 2;
        frames -= n;
    }
}
