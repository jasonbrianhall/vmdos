/* Gravis UltraSound: the GF1 wavetable chip and its 1 MB of DRAM, modelled
 * on DOSBox's gus.cpp. The game uploads samples (DMA through the virtual
 * 8237, or poked through 3X7h) and plays them on up to 32 voices, each with
 * its own loop points, pitch, logarithmic volume ramp and pan. The voices
 * are mixed at the GF1's own rate (617400 Hz / active voices: 44.1 kHz with
 * 14) and interpolated up to 48 kHz into the Sound Blaster's mix.
 *
 *  2X0h mix control     2X6h IRQ status       2X8h/2X9h timer control
 *  2XBh IRQ/DMA latch   3X0h/3X1h MIDI        3X2h voice  3X3h register
 *  3X4h/3X5h data low/high                    3X7h DRAM byte
 *
 * DMA happens at once when the game starts it (or when it unmasks the
 * channel); voice, ramp and timer IRQs are raised as the sound is rendered.
 */
#include "kernel.h"
#include "gus.h"
#include "sb/gmsynth.h"
#include "sb/sbout.h"

#define DRAM_SIZE  (1u << 20)
#define WAVE_FRACT 9                        /* voice addresses: 20.9 fixed point */
#define OUT_RATE   48000

static u8 dram[DRAM_SIZE];

struct voice {
    u32 start, end, addr;                   /* 20.9, as the registers hold them */
    u16 fc;                                 /* 0x01: bits 15-1, 6.9 samples per GF1 frame */
    u8 wctl, rctl;                          /* 0x00, 0x0D (bit 7, the IRQ, is kept apart) */
    u8 rate, rstart, rend, pan;             /* 0x06, 0x07, 0x08, 0x0C */
    int vol;                                /* 0x09 bits 15-4: 12-bit logarithmic */
    u32 rcount;                             /* GF1 frames since the last ramp step */
};

static struct voice vc[32];
static u16 base;
static int enabled, irq_line = 7, dma_ch = 3;
static int voices = 14;
static u32 active_mask = (1u << 14) - 1;
static u8 mixctl = 0x0B, irq_status, voice_sel, reg_sel, reset_reg;
static u16 reg_data;
static u8 dmactl, timerctl, sampctl, adlib_cmd;
static u16 dma_addr;
static u32 dram_addr;
static u32 wave_irq, ramp_irq;              /* a bit per voice */
static int irq_chan;
static volatile int irq_req;

static struct {
    int running, masked, reached, irq;
    u8 value;
    int32_t left, period;                       /* in 1/12 MHz: 80 us = 960, 320 us = 3840 */
} tm[2];

/* the interpolator to 48 kHz */
static u32 step, frac;
static int32_t prev_l, prev_r, cur_l, cur_r;

/* Pan 0 (left) .. 15 (right), constant power, Q15. */
static const u16 pan_l[16] = { 32767, 32587, 32051, 31163, 29934, 28377, 26509, 24351,
                               21925, 19260, 16384, 13328, 10126, 6813, 3425, 0 };

/* The GF1's volume: 4-bit exponent and 8-bit mantissa, 6 dB a step; 4095 is
   unity (Q16). */
static inline int32_t vol_amp(int v) { return (int32_t)((u32)(256 + (v & 255)) << (v >> 8) >> 8); }

static void check_irq(void)
{
    if (irq_status && (mixctl & 0x08)) irq_req = 1;
}

/* Bits 5-6 of the IRQ status from the voices' flags, and the voice 8Fh
   reports next. */
static void check_voice_irq(void)
{
    irq_status &= 0x9F;
    u32 total = (ramp_irq | wave_irq) & active_mask;
    if (!total) return;
    if (ramp_irq) irq_status |= 0x40;
    if (wave_irq) irq_status |= 0x20;
    check_irq();
    while (!(total & (1u << irq_chan)))
        if (++irq_chan >= voices) irq_chan = 0;
}

static void set_voices(int n)
{
    if (n < 14) n = 14;
    if (n > 32) n = 32;
    voices = n;
    active_mask = n == 32 ? 0xFFFFFFFFu : (1u << n) - 1;
    step = (u32)(((u64)(617400 / n) << 16) / OUT_RATE);
    if (irq_chan >= n) irq_chan = 0;
}

static void reset(void)
{
    irq_status = 0;
    memset(tm, 0, sizeof tm);
    adlib_cmd = 0x55;
    dmactl = timerctl = sampctl = 0;
    mixctl = 0x0B;
    wave_irq = ramp_irq = 0;
    irq_chan = 0;
    for (int i = 0; i < 32; i++) {
        struct voice *v = &vc[i];
        memset(v, 0, sizeof *v);
        v->wctl = 1;
        v->rctl = 1;
        v->pan = 7;
    }
    set_voices(14);
}

void gus_init(u16 port, int irq, int dma)
{
    base = port;
    irq_line = irq;
    dma_ch = dma;
    reset();
    reset_reg = 7;
    enabled = 1;
}

void gus_stop(void)
{
    if (!enabled) return;
    reset();
    irq_req = 0;
}

int gus_irq_line(void) { return irq_line; }

int gus_take_irq(void)
{
    if (!irq_req) return 0;
    irq_req = 0;
    return 1;
}

int gus_owns(u16 port)
{
    if (!enabled) return 0;
    switch (port - base) {
    case 0x0: case 0x6: case 0x8: case 0x9: case 0xA: case 0xB: case 0xF: return 1;
    }
    return port >= base + 0x100 && port <= base + 0x107;
}

/* ---------------------------------------------------------------- DMA */

void gus_dma_poll(void)
{
    if (!enabled || !(dmactl & 1)) return;
    u32 a = (u32)dma_addr << 4;
    if (dmactl & 4) a = (a & 0xC0000) | ((a & 0x1FFFF) << 1);    /* 16-bit channel: word addresses */
    a &= DRAM_SIZE - 1;
    int read = (dmactl & 2) != 0;                               /* DRAM -> memory */
    int n = dma_xfer(dma_ch, dram + a, DRAM_SIZE - a, read);
    if (n < 0) return;                                          /* masked: when it's unmasked */
    if (!read && (dmactl & 0x80)) {                             /* unsigned data: flip the MSB */
        if (dmactl & 0x40) for (int i = 1; i < n; i += 2) dram[a + i] ^= 0x80;
        else for (int i = 0; i < n; i++) dram[a + i] ^= 0x80;
    }
    dbg(2, "GUS: DMA %s %u bytes at %05x (ch %d)\n", read ? "read" : "write", n, a, dma_ch);
    dmactl &= ~1;
    if (dmactl & 0x20) { irq_status |= 0x80; check_irq(); }
}

/* ---------------------------------------------------------------- registers */

static void wave_ctl(struct voice *v, int i, u8 val)
{
    u32 old = wave_irq;
    v->wctl = val & 0x7F;
    if ((val & 0xA0) == 0xA0) wave_irq |= 1u << i; else wave_irq &= ~(1u << i);
    if (old != wave_irq) check_voice_irq();
}

static void ramp_ctl(struct voice *v, int i, u8 val)
{
    u32 old = ramp_irq;
    v->rctl = val & 0x7F;
    if ((val & 0xA0) == 0xA0) ramp_irq |= 1u << i; else ramp_irq &= ~(1u << i);
    if (old != ramp_irq) check_voice_irq();
}

static void write_reg(void)
{
    struct voice *v = &vc[voice_sel & 31];
    int i = voice_sel & 31;
    u8 hi = (u8)(reg_data >> 8);
    switch (reg_sel) {
    case 0x00: wave_ctl(v, i, hi); return;
    case 0x01: v->fc = reg_data; return;
    case 0x02: v->start = (v->start & 0xFFFF) | (u32)(reg_data & 0x1FFF) << 16; return;
    case 0x03: v->start = (v->start & ~0xFFFFu) | reg_data; return;
    case 0x04: v->end = (v->end & 0xFFFF) | (u32)(reg_data & 0x1FFF) << 16; return;
    case 0x05: v->end = (v->end & ~0xFFFFu) | reg_data; return;
    case 0x06: v->rate = hi; return;
    case 0x07: v->rstart = hi; return;
    case 0x08: v->rend = hi; return;
    case 0x09: v->vol = reg_data >> 4; return;
    case 0x0A: v->addr = (v->addr & 0xFFFF) | (u32)(reg_data & 0x1FFF) << 16; return;
    case 0x0B: v->addr = (v->addr & ~0xFFFFu) | reg_data; return;
    case 0x0C: v->pan = hi & 15; return;
    case 0x0D: ramp_ctl(v, i, hi); return;
    case 0x0E: set_voices((hi & 31) + 1); check_voice_irq(); return;
    case 0x41:
        dmactl = hi;
        gus_dma_poll();
        return;
    case 0x42: dma_addr = reg_data; return;
    case 0x43: dram_addr = (dram_addr & 0xF0000) | reg_data; return;
    case 0x44: dram_addr = (dram_addr & 0xFFFF) | (u32)(hi & 15) << 16; return;
    case 0x45:
        timerctl = hi;
        tm[0].irq = (hi & 4) != 0;
        if (!tm[0].irq) irq_status &= ~4;
        tm[1].irq = (hi & 8) != 0;
        if (!tm[1].irq) irq_status &= ~8;
        return;
    case 0x46: tm[0].value = hi; tm[0].period = (256 - hi) * 960; return;
    case 0x47: tm[1].value = hi; tm[1].period = (256 - hi) * 3840; return;
    case 0x49: sampctl = hi; return;                            /* recording isn't emulated */
    case 0x4C:
        reset_reg = hi;
        if (!(hi & 1)) reset();
        return;
    }
}

static u16 read_reg(void)
{
    struct voice *v = &vc[voice_sel & 31];
    u32 m = 1u << (voice_sel & 31);
    switch (reg_sel) {
    case 0x41: {                                                /* reading acknowledges the DMA IRQ */
        u8 r = (u8)((dmactl & 0xBF) | (irq_status & 0x80) >> 1);
        irq_status &= 0x7F;
        return (u16)(r << 8); }
    case 0x42: return dma_addr;
    case 0x45: return (u16)(timerctl << 8);
    case 0x49: return (u16)(sampctl << 8);
    case 0x4C: return (u16)(reset_reg << 8);
    case 0x80: return (u16)((v->wctl | (wave_irq & m ? 0x80 : 0)) << 8);
    case 0x81: return v->fc;
    case 0x82: return (u16)(v->start >> 16);
    case 0x83: return (u16)v->start;
    case 0x84: return (u16)(v->end >> 16);
    case 0x85: return (u16)v->end;
    case 0x86: return (u16)(v->rate << 8);
    case 0x87: return (u16)(v->rstart << 8);
    case 0x88: return (u16)(v->rend << 8);
    case 0x89: return (u16)(v->vol << 4);
    case 0x8A: return (u16)(v->addr >> 16);
    case 0x8B: return (u16)v->addr;
    case 0x8C: return (u16)(v->pan << 8);
    case 0x8D: return (u16)((v->rctl | (ramp_irq & m ? 0x80 : 0)) << 8);
    case 0x8E: return (u16)((0xC0 | (voices - 1)) << 8);
    case 0x8F: {                                                /* the next voice with an IRQ; acknowledges it */
        u8 r = 0x20;
        u32 cm = 1u << irq_chan;
        if (!(ramp_irq & cm)) r |= 0x40;
        if (!(wave_irq & cm)) r |= 0x80;
        r |= (u8)irq_chan;
        ramp_irq &= ~cm;
        wave_irq &= ~cm;
        check_voice_irq();
        return (u16)(r << 8); }
    }
    return 0xFFFF;
}

static void timer_ctl(u8 v)                                     /* 2X9h after 04h in 2X8h, as on the AdLib */
{
    if (v & 0x80) { tm[0].reached = tm[1].reached = 0; return; }
    tm[0].masked = (v & 0x40) != 0;
    tm[1].masked = (v & 0x20) != 0;
    for (int t = 0; t < 2; t++) {
        if (v & (1 << t)) {
            if (!tm[t].running) { tm[t].left = tm[t].period ? tm[t].period : 256 * (t ? 3840 : 960); tm[t].running = 1; }
        } else tm[t].running = 0;
    }
}

/* the IRQ latch (2XBh after 2X0h bit 6 set): 1-7 IRQ 2, 5, 3, 7, 11, 12, 15;
   the DMA latch: 1-5 channel 1, 3, 5, 6, 7 */
static void latch(u8 v)
{
    static const int8_t irqs[8] = { 0, 9, 5, 3, 7, 11, 12, 15 }, dmas[8] = { 0, 1, 3, 5, 6, 7, 0, 0 };
    if (mixctl & 0x40) { if (irqs[v & 7]) irq_line = irqs[v & 7]; }
    else if (dmas[v & 7]) dma_ch = dmas[v & 7];
}

void gus_out(u16 port, u8 v)
{
    switch (port - base) {
    case 0x000: mixctl = v; check_irq(); return;
    case 0x008: adlib_cmd = v; return;
    case 0x009: if (adlib_cmd == 4) timer_ctl(v); return;
    case 0x00B: latch(v); return;
    case 0x101: gm_midi_byte(v); return;                        /* MIDI out: the GM synth */
    case 0x102: voice_sel = v & 31; return;
    case 0x103: reg_sel = v; reg_data = 0; return;
    case 0x104: reg_data = (u16)((reg_data & 0xFF00) | v); return;
    case 0x105: reg_data = (u16)((reg_data & 0x00FF) | v << 8); write_reg(); return;
    case 0x107: dram[dram_addr] = v; return;
    }
}

u8 gus_in(u16 port)
{
    switch (port - base) {
    case 0x006: return irq_status;
    case 0x008: {                                               /* timer status */
        u8 r = 0;
        if (tm[0].reached) r |= 0x40;
        if (tm[1].reached) r |= 0x20;
        if (r) r |= 0x80;
        if (irq_status & 4) r |= 4;
        if (irq_status & 8) r |= 2;
        return r; }
    case 0x100: return 0x02;                                    /* MIDI: transmit empty, nothing received */
    case 0x101: return 0x00;
    case 0x102: return voice_sel;
    case 0x103: return reg_sel;
    case 0x104: return (u8)read_reg();
    case 0x105: return (u8)(read_reg() >> 8);
    case 0x107: return dram[dram_addr];
    }
    return 0xFF;
}

/* ---------------------------------------------------------------- render */

static inline int32_t sample(const struct voice *v)
{
    u32 a = (v->addr >> WAVE_FRACT) & (DRAM_SIZE - 1);
    int32_t f = v->addr & ((1 << WAVE_FRACT) - 1), s0, s1;
    if (v->wctl & 4) {                                          /* 16-bit: within a 256 KB bank, in words */
        u32 p0 = (a & 0xC0000) | ((a & 0x1FFFF) << 1);
        u32 a1 = (a & 0xC0000) | ((a + 1) & 0x3FFFF);
        u32 p1 = (a1 & 0xC0000) | ((a1 & 0x1FFFF) << 1);
        s0 = (int16_t)(dram[p0] | dram[p0 + 1] << 8);
        s1 = (int16_t)(dram[p1] | dram[p1 + 1] << 8);
    } else {
        s0 = (int8_t)dram[a] << 8;
        s1 = (int8_t)dram[(a + 1) & (DRAM_SIZE - 1)] << 8;
    }
    return s0 + (((s1 - s0) * f) >> WAVE_FRACT);
}

/* The position past an end: a loop, a turn round, or the voice stops. */
static void wave_update(struct voice *v, int i)
{
    if (v->wctl & 3) return;
    u32 add = v->fc >> 1;
    int32_t left;
    if (v->wctl & 0x40) { v->addr -= add; left = (int32_t)(v->start - v->addr); }
    else { v->addr += add; left = (int32_t)(v->addr - v->end); }
    if (left < 0) return;
    if (v->wctl & 0x20) wave_irq |= 1u << i;
    if (v->rctl & 4) return;                                    /* rollover: play on past the end */
    if (v->wctl & 8) {
        if (v->wctl & 0x10) v->wctl ^= 0x40;
        v->addr = v->wctl & 0x40 ? v->end - left : v->start + left;
    } else {
        v->wctl |= 1;
        v->addr = v->wctl & 0x40 ? v->start : v->end;
    }
}

/* A volume ramp step every 1, 8, 64 or 512 frames (bits 7-6 of 06h). */
static void ramp_update(struct voice *v, int i)
{
    if (v->rctl & 3) return;
    if (++v->rcount < 1u << (3 * (v->rate >> 6))) return;
    v->rcount = 0;
    int inc = v->rate & 63, lo = v->rstart << 4, hi = v->rend << 4, left;
    if (v->rctl & 0x40) { v->vol -= inc; left = lo - v->vol; }
    else { v->vol += inc; left = v->vol - hi; }
    if (left >= 0) {
        if (v->rctl & 0x20) ramp_irq |= 1u << i;
        if (v->rctl & 8) {
            if (v->rctl & 0x10) v->rctl ^= 0x40;
            v->vol = v->rctl & 0x40 ? hi - left : lo + left;
        } else {
            v->rctl |= 1;
            v->vol = v->rctl & 0x40 ? lo : hi;
        }
    }
    if (v->vol < 0) v->vol = 0;
    if (v->vol > 4095) v->vol = 4095;
}

static void gf1_frame(int32_t *l, int32_t *r)
{
    int32_t sl = 0, sr = 0;
    for (int i = 0; i < voices; i++) {
        struct voice *v = &vc[i];
        if ((v->wctl & 3) && (v->rctl & 3) && v->vol == 0) continue;
        int32_t s = (sample(v) * vol_amp(v->vol)) >> 16;
        sl += (s * pan_l[v->pan]) >> 15;
        sr += (s * pan_l[15 - v->pan]) >> 15;
        wave_update(v, i);
        ramp_update(v, i);
    }
    *l = sl >> 1;
    *r = sr >> 1;
}

static void timers_run(int frames)
{
    for (int t = 0; t < 2; t++) {
        if (!tm[t].running) continue;
        tm[t].left -= frames * 250;                             /* 12 MHz / 48 kHz */
        while (tm[t].left <= 0) {
            if (!tm[t].masked) tm[t].reached = 1;
            if (tm[t].irq) { irq_status |= 4 << t; check_irq(); }
            tm[t].left += tm[t].period ? tm[t].period : 256 * (t ? 3840 : 960);
        }
    }
}

void gus_mix(int32_t *lr, int frames)
{
    if (!enabled) return;
    timers_run(frames);
    if (!(reset_reg & 1)) return;                               /* held in reset */
    u32 wi = wave_irq, ri = ramp_irq;
    for (int i = 0; i < frames; i++) {
        frac += step;
        while (frac >= 0x10000) {
            frac -= 0x10000;
            prev_l = cur_l;
            prev_r = cur_r;
            gf1_frame(&cur_l, &cur_r);
        }
        int32_t f = (int32_t)(frac >> 8);
        lr[i * 2] += prev_l + (((cur_l - prev_l) * f) >> 8);
        lr[i * 2 + 1] += prev_r + (((cur_r - prev_r) * f) >> 8);
    }
    if (wi != wave_irq || ri != ramp_irq) check_voice_irq();
}
