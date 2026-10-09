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

static int sb_irq = 5, sb_dma = 1, sb_hdma = 5;
static volatile int irq_pending;
static int irq_stat;                        /* SB16 mixer 82h: bit 0 8-bit, bit 1 16-bit IRQ */

static struct {
    int active, autoinit, silence;
    int ch;                                 /* DMA channel of this transfer */
    int bits16, sign;                       /* 16-bit samples (channel 5), signed */
    uint32_t dma_phys, dma_len, dma_pos;    /* dma_len, dma_pos: in bytes or words */
    uint16_t dma_addr;                      /* the address register at the start */
    const volatile uint8_t *mem;
    uint32_t block_len, block_left;         /* in samples */
    uint32_t step, frac;
    int stereo;
    int16_t prev_l, prev_r, cur_l, cur_r;
    int record;                             /* the DMA goes the other way: ADC -> memory */
    int32_t in_acc, in_n;                   /* 48 kHz input samples summed for the next one */
} s;
static int16_t last_in;                     /* latest input sample (DSP 20h) */

/* Direct DAC writes with their PIT time, replayed by sb_render. */
#define DACQ 4096
static struct { uint32_t t; uint8_t v; } dacq[DACQ];
static uint32_t dq_head, dq_tail;
static uint8_t dac_cur = 0x80;
static uint32_t render_t;                   /* PIT time the last render reached */
static uint32_t frame_t0, frame_dt, frame_n; /* this render: frame i at t0 + dt*i/n */
int audio_capture_read(int16_t *mono, int n);   /* audio.cpp: 48 kHz mic samples, 0 when none */
int audio_capture_latest(int rate);
uint32_t vpit_period0(void);

/* ---------------------------------------------------------------- 8237 pair */
/* Channels 0-3: ports 00h-0Fh (bytes). Channels 4-7: ports C0h-DEh (words;
   the page register's bit 0 is ignored, addresses are word addresses). */
static struct { uint16_t addr, count; uint8_t page, mode; } dch[8];
static uint8_t dma_mask = 0xFF, dma_ff[2];
static int dma_valid, dma_tc, dma_done;

static int page_channel(uint16_t port)
{
    switch (port) {
    case 0x87: return 0; case 0x83: return 1; case 0x81: return 2; case 0x82: return 3;
    case 0x8F: return 4; case 0x8B: return 5; case 0x89: return 6; case 0x8A: return 7;
    }
    return -1;
}

int dma_owns(uint16_t port)
{
    return port <= 0x0F || (port >= 0x81 && port <= 0x8F) || (port >= 0xC0 && port <= 0xDF);
}

static void ctl_out(int hi, int reg, uint8_t v)     /* registers 8-15 of a controller */
{
    int b = hi ? 4 : 0;
    switch (reg) {
    case 0xA: if (v & 4) dma_mask |= 1 << (b + (v & 3)); else dma_mask &= ~(1 << (b + (v & 3))); return;
    case 0xB: dch[b + (v & 3)].mode = v; return;
    case 0xC: dma_ff[hi] = 0; return;
    case 0xD: dma_ff[hi] = 0; dma_mask |= 0x0F << b; return;
    case 0xE: dma_mask &= ~(0x0F << b); return;
    case 0xF: dma_mask = (uint8_t)((dma_mask & ~(0x0F << b)) | (v & 0x0F) << b); return;
    }
}

static void reg_out(int ch, int count, uint8_t v)
{
    int hi = ch >= 4;
    uint16_t *r = count ? &dch[ch].count : &dch[ch].addr;
    *r = dma_ff[hi] ? (uint16_t)((*r & 0xFF) | v << 8) : (uint16_t)((*r & 0xFF00) | v);
    dma_ff[hi] ^= 1;
    if (ch == s.ch) dma_valid = 0;
}

void dma_out(uint16_t port, uint8_t v)
{
    if (port < 8) { reg_out(port >> 1, port & 1, v); return; }
    if (port < 0x10) { ctl_out(0, port, v); return; }
    if (port >= 0xC0) {
        int r = (port - 0xC0) >> 1;                 /* register 0-15 */
        if (r < 8) reg_out(4 + (r >> 1), r & 1, v);
        else ctl_out(1, r, v);
        return;
    }
    int ch = page_channel(port);
    if (ch >= 0) { dch[ch].page = v; if (ch == s.ch) dma_valid = 0; }
}

static uint8_t reg_in(int ch, int count)
{
    int hi = ch >= 4;
    uint32_t v;
    if (ch == s.ch && dma_valid) {
        if (count) v = dma_done ? 0xFFFF : s.dma_len - 1 - s.dma_pos;
        else v = (uint16_t)(s.dma_addr + (dma_done ? s.dma_len : s.dma_pos));
    } else v = count ? dch[ch].count : dch[ch].addr;
    uint8_t b = dma_ff[hi] ? (uint8_t)(v >> 8) : (uint8_t)v;
    dma_ff[hi] ^= 1;
    return b;
}

static uint8_t status_in(int hi)
{
    int b = hi ? 4 : 0;
    uint8_t v = 0;
    if (s.ch >= b && s.ch < b + 4) {
        if (dma_tc) v |= 1 << (s.ch - b);
        if (s.active && !s.silence) v |= 0x10 << (s.ch - b);
        dma_tc = 0;
    }
    return v;
}

uint8_t dma_in(uint16_t port)
{
    if (port < 8) return reg_in(port >> 1, port & 1);
    if (port == 0x08) return status_in(0);
    if (port == 0x0F) return dma_mask & 0x0F;
    if (port >= 0xC0) {
        int r = (port - 0xC0) >> 1;
        if (r < 8) return reg_in(4 + (r >> 1), r & 1);
        if (r == 8) return status_in(1);
        if (r == 15) return dma_mask >> 4;
        return 0xFF;
    }
    int ch = page_channel(port);
    return ch >= 0 ? dch[ch].page : 0xFF;
}

static void read_dma_controller(void)
{
    int ch = s.ch;
    s.dma_addr = dch[ch].addr;
    if (ch < 4) s.dma_phys = (uint32_t)dch[ch].page << 16 | dch[ch].addr;
    else s.dma_phys = (uint32_t)(dch[ch].page & 0xFE) << 16 | (uint32_t)dch[ch].addr << 1;
    s.dma_len = (uint32_t)dch[ch].count + 1;
    s.dma_pos = 0;
    dma_valid = 1;
    dma_done = 0;
    dma_tc = 0;
}

static int map_dma_buffer(void)
{
    uint32_t bytes = s.dma_len << (s.ch >= 4);
    if (s.dma_phys + bytes > GUEST_TOP) return 0;
    s.mem = (const volatile uint8_t *)gptr(s.dma_phys);
    return 1;
}

/* ---------------------------------------------------------------- control */

void sb_out_init(int irq, int dma)
{
    sb_irq = irq;
    sb_dma = dma & 3;
    memset(&s, 0, sizeof s);
    s.ch = -1;
}

int sb_irq_line(void) { return sb_irq; }

static void raise_irq(int bits16)
{
    irq_pending = 1;
    irq_stat |= bits16 ? 2 : 1;
}
int  sb_irq_status(void)        { return irq_stat; }
void sb_irq_ack(int bits16)     { irq_stat &= bits16 ? ~2 : ~1; }

/* rate: sample frames per second; len: samples (each channel counts) */
static void start(int record, int autoinit, int bits16, int sign, int stereo,
                  uint32_t rate, uint32_t len, int silence)
{
    s.record = 0;
    s.active = 0;
    s.bits16 = bits16;
    s.sign = sign;
    s.stereo = stereo && !silence;
    s.ch = bits16 ? sb_hdma : sb_dma;
    if (rate < 1000) rate = 1000;
    if (rate > 48000) rate = 48000;
    s.step = (rate << 16) / OUT_RATE;
    s.frac = 0;

    s.silence = silence;
    dsp.paused = 0;                         /* a new transfer ends a pause */
    s.autoinit = autoinit;
    s.block_len = len ? len : 1;
    s.block_left = s.block_len;

    if (!silence) {
        read_dma_controller();
        if (!map_dma_buffer()) {
            dbg(1, "SB: DMA buffer %x not in conventional memory\n", s.dma_phys);
            return;
        }
    }
    dbg(2, "SB: %s %s%s len=%u rate=%u%s dma=%x/%u ch%d\n", record ? "record" : "start",
        autoinit ? "auto" : "single", bits16 ? " 16-bit" : "", s.block_len, rate,
        s.stereo ? " stereo" : "", s.dma_phys, s.dma_len, s.ch);

    /* Detection transfers (a few bytes) finish in microseconds on a real
       card; complete them now so the IRQ arrives before any timeout. */
    if (!autoinit && s.block_len <= 64) {
        if (!silence) {
            s.dma_pos = s.block_len % (s.dma_len ? s.dma_len : 1);
            if (s.dma_pos == 0) { dma_done = 1; dma_tc = 1; }
        }
        raise_irq(bits16);
        return;
    }
    s.active = 1;
    s.record = record;
    s.in_acc = s.in_n = 0;
}

static uint32_t tc_rate(void)
{
    uint32_t rate = 1000000u / (256 - dsp.time_constant);
    if (dsp.mixer[0x0E] & 0x02) rate /= 2;  /* TC was set for twice the frame rate */
    return rate;
}

void sb_out_start(int autoinit, uint32_t len_bytes, int silence)
{
    int stereo = (dsp.mixer[0x0E] & 0x02) != 0;
    start(0, autoinit, 0, 0, stereo, dsp.sb16 && dsp.rate ? dsp.rate : tc_rate(), len_bytes, silence);
}

void sb_in_start(int autoinit, uint32_t len_bytes)
{
    int stereo = (dsp.mixer[0x0E] & 0x02) != 0;
    start(1, autoinit, 0, 0, stereo, dsp.sb16 && dsp.rate ? dsp.rate : tc_rate(), len_bytes, 0);
}

void sb16_start(int record, int autoinit, int bits16, int sign, int stereo, uint32_t len)
{
    start(record, autoinit, bits16, sign, stereo, dsp.rate ? dsp.rate : 1000000u / (256 - dsp.time_constant), len, 0);
}

uint8_t sb_in_sample(void)
{
    if (!s.record) {
        uint32_t per = vpit_period0();           /* polled from a fast timer: its rate */
        last_in = (int16_t)audio_capture_latest(per < 1193 / 2 && per > 20 ? (int)(1193182u / per) : 0);
    }
    return (uint8_t)((last_in >> 8) + 128);
}

void sb_out_stop(void)            { s.active = 0; s.record = 0; irq_pending = 0; irq_stat = 0; dq_head = dq_tail; dac_cur = 0x80; }
void sb_out_exit_autoinit(void)   { s.autoinit = 0; }
void sb_out_raise_irq(void)       { raise_irq(0); }
void sb_out_raise_irq16(void)     { raise_irq(1); }

int sb_take_irq(void)
{
    if (!irq_pending) return 0;
    irq_pending = 0;
    return 1;
}

/* ---------------------------------------------------------------- render */

/* The end of a sample: DMA position, terminal count, block end and IRQ. */
static void advance(void)
{
    if (++s.dma_pos >= s.dma_len) {                      /* 8237 terminal count */
        s.dma_pos = 0;
        dma_tc = 1;
        if (!s.autoinit) dma_done = 1;
    }
}
static void block_step(void)
{
    if (--s.block_left == 0) {
        raise_irq(s.bits16);
        if (s.autoinit) s.block_left = s.block_len;
        else { s.active = 0; s.record = 0; }
    }
}

static int16_t fetch(void)
{
    int16_t v = 0;
    if (!s.silence) {
        if (s.bits16) {
            uint16_t w = (uint16_t)(s.mem[s.dma_pos * 2] | s.mem[s.dma_pos * 2 + 1] << 8);
            v = (int16_t)(s.sign ? w : w ^ 0x8000);
        } else {
            uint8_t b = s.mem[s.dma_pos];
            v = (int16_t)(s.sign ? (int8_t)b * 256 : ((int)b - 128) * 256);
        }
        advance();
    }
    block_step();
    return v;
}

static void next_frame(void)
{
    s.prev_l = s.cur_l;
    s.prev_r = s.cur_r;
    int16_t l = fetch();
    int16_t r = l;
    if (s.stereo && s.active) r = fetch();
    s.cur_l = l;
    s.cur_r = r;
}

static uint32_t dac_writes, dac_dry;
void sb_dac_write(uint8_t v)
{
    dac_writes++;
    uint32_t next = (dq_tail + 1) % DACQ;
    if (next == dq_head) dq_head = (dq_head + 1) % DACQ;   /* full: drop the oldest */
    dacq[dq_tail].t = pit_clock();
    dacq[dq_tail].v = v;
    dq_tail = next;
}

/* A program writing the DAC from a fast timer (Parrot: 12 kHz) means one
   sample per timer tick: play the writes at exactly that rate, about 15 ms
   behind, interpolated, so interrupt jitter doesn't become crackle. */
uint32_t vpit_period0(void);
static int dac_stream;                      /* playing the queue at the timer's rate */
static uint32_t ds_step, ds_frac;           /* timer ticks per 48 kHz frame, 16.16 */
static int32_t ds_prev = 0, ds_cur = 0;
static uint32_t dq_level(void) { return (dq_tail + DACQ - dq_head) % DACQ; }

static int32_t dac_stream_at(void)
{
    uint32_t per = vpit_period0();
    uint32_t rate = 1193182u / per;                       /* samples per second */
    uint32_t target = rate * 15 / 1000 + 8;
    uint32_t lvl = dq_level();
    if (!dac_stream) {                                     /* prebuffer */
        if (lvl < target) return ds_cur;
        dac_stream = 1;
        ds_frac = 0;
    }
    uint32_t step = (rate << 16) / 48000u;
    int32_t err = (int32_t)lvl - (int32_t)target;          /* drift: lean on the rate a little */
    step += (uint32_t)((int32_t)step * (err > 64 ? 64 : err < -64 ? -64 : err) / 1024);
    ds_step = step;
    ds_frac += ds_step;
    while (ds_frac >= 0x10000) {
        ds_frac -= 0x10000;
        if (dq_head == dq_tail) { dac_stream = 0; dac_dry++; break; }  /* ran dry: hold, refill */
        ds_prev = ds_cur;
        ds_cur = ((int32_t)dacq[dq_head].v - 128) * 256;
        dac_cur = dacq[dq_head].v;
        dq_head = (dq_head + 1) % DACQ;
    }
    return ds_prev + (((ds_cur - ds_prev) * (int32_t)((ds_frac & 0xFFFF) >> 2)) >> 14);
}

/* The DAC's value at render frame i (sample-and-hold, as the real one). */
static int32_t dac_at(uint32_t i)
{
    uint32_t per = vpit_period0();
    if (per < 1193 / 2 && per > 20) return dac_stream_at();   /* timer above 2 kHz */
    dac_stream = 0;
    ds_prev = ds_cur = ((int32_t)dac_cur - 128) * 256;
    uint32_t t = frame_t0 + (frame_n ? frame_dt * i / frame_n : frame_dt);
    static uint32_t last_write;
    while (dq_head != dq_tail && (int32_t)(dacq[dq_head].t - t) <= 0) {
        dac_cur = dacq[dq_head].v;
        last_write = dacq[dq_head].t;
        dq_head = (dq_head + 1) % DACQ;
    }
    /* Left at a value after the last sample: ease back to the middle once
       it has been quiet for 100 ms (keeps the DC out of the mix). */
    if (dac_cur != 0x80 && t - last_write > 1193182 / 10 && (i & 15) == 0)
        dac_cur += dac_cur < 0x80 ? 1 : -1;
    return ((int32_t)dac_cur - 128) * 256;
}

static uint32_t frame_base;                 /* frames rendered so far in this sb_render */

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

/* One recorded sample into the DMA buffer (the counterpart of fetch). */
static void store(int16_t v)
{
    volatile uint8_t *m = (volatile uint8_t *)s.mem;
    if (s.bits16) {
        uint16_t w = (uint16_t)(s.sign ? v : v ^ 0x8000);
        m[s.dma_pos * 2] = (uint8_t)w;
        m[s.dma_pos * 2 + 1] = (uint8_t)(w >> 8);
    } else {
        int b = (v >> 8);                               /* rounded to 8 bits */
        if ((v & 0x80) && b < 127) b++;
        m[s.dma_pos] = (uint8_t)(s.sign ? b : b + 128);
    }
    advance();
    block_step();
}

/* frames of 48 kHz input: averaged down to the DSP's rate (s.step) */
static void record_chunk(int frames)
{
    static int16_t in[OPL_MAX_FRAMES];
    int got = audio_capture_read(in, frames);
    for (int i = got; i < frames; i++) in[i] = got ? in[got - 1] : 0;   /* no mic, or late: hold */
    if (frames) last_in = in[frames - 1];
    for (int i = 0; i < frames && s.active && s.record; i++) {
        s.in_acc += in[i]; s.in_n++;
        if (dsp.paused) continue;
        s.frac += s.step;
        while (s.frac >= 0x10000 && s.active && s.record) {
            s.frac -= 0x10000;
            int32_t v = s.in_n ? s.in_acc / s.in_n : 0;
            s.in_acc = s.in_n = 0;
            store((int16_t)v);
            if (s.stereo && s.active) store((int16_t)v);
        }
    }
}

static void render_chunk(int16_t *out, int frames)
{
    if (s.active && s.record) record_chunk(frames);
    for (int i = 0; i < frames; i++) {
        int32_t l, r;
        if (s.active && !s.record && !dsp.paused) {
            s.frac += s.step;
            while (s.frac >= 0x10000 && s.active) {
                s.frac -= 0x10000;
                next_frame();
            }
            int32_t f = s.frac;                          /* linear interpolation */
            l = s.prev_l + (((s.cur_l - s.prev_l) * f) >> 16);
            r = s.prev_r + (((s.cur_r - s.prev_r) * f) >> 16);
        } else {
            l = r = dac_at(frame_base + i);              /* direct DAC (10h) */
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
    /* This call covers the time since the last one: spread it over the frames. */
    uint32_t now = pit_clock();
    uint32_t span = now - render_t;
    if (span > 1193182 / 20 || !render_t) span = (uint32_t)frames * 1193182u / 48000u;   /* first call, or a long gap */
    frame_t0 = now - span;
    frame_dt = span;
    frame_n = (uint32_t)frames;
    frame_base = 0;
    render_t = now;
    static uint32_t rep_t;
    if (now - rep_t > 1193182u && dac_writes) {
        dbg(2, "DAC: %u writes/s, dry %u, level %u, per %u\n", dac_writes, dac_dry, (dq_tail + DACQ - dq_head) % DACQ, vpit_period0());
        dac_writes = dac_dry = 0;
    }
    if (now - rep_t > 1193182u) rep_t = now;
    while (frames > 0) {
        int n = frames > OPL_MAX_FRAMES ? OPL_MAX_FRAMES : frames;
        render_chunk(out, n);
        frame_base += n;
        out += n * 2;
        frames -= n;
    }
}
