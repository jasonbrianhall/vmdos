/* Sound Blaster Pro 2.0 DSP, mixer and FM chip status.
 *
 *  base+0..3, base+8..9, 388h..38Bh   FM (OPL3), synthesized by dbopl;
 *                                     timer flags are faked for detection.
 *  base+4  mixer index      base+5  mixer data
 *  base+6  DSP reset        base+A  DSP read data
 *  base+C  DSP write / write-buffer status
 *  base+E  DSP read-buffer status (also acknowledges the 8-bit IRQ)
 */
#include "../kernel.h"
#include "dsp.h"
#include "sbout.h"
#include "opl.h"

DspState dsp;

static uint16_t base;
static uint8_t outq[16];
static uint8_t qhead, qtail;
static uint8_t cmd, nargs, argi, args[3];
static uint8_t reset_latch, test_reg, mix_index;
static uint16_t fm_index;       /* bit 8 = OPL3 second register bank */
static uint8_t fm_status, fm_timer_ctl;
static uint8_t busy_count;

static void q_clear(void) { qhead = qtail = 0; }
static void q_push(uint8_t v)
{
    uint8_t next = (qtail + 1) & 15;
    if (next != qhead) { outq[qtail] = v; qtail = next; }
}
static int q_empty(void) { return qhead == qtail; }
static uint8_t q_pop(void)
{
    static uint8_t last = 0xFF;
    if (!q_empty()) { last = outq[qhead]; qhead = (qhead + 1) & 15; }
    return last;
}

static void mixer_reset(void)
{
    memset(dsp.mixer, 0, sizeof dsp.mixer);
    dsp.mixer[0x04] = 0x99;     /* voice */
    dsp.mixer[0x22] = 0x99;     /* master */
    dsp.mixer[0x26] = 0x99;     /* FM */
    dsp.mixer[0x0E] = 0x00;     /* mono, filter on */
    if (dsp.sb16) {             /* SB16 levels: master, voice, MIDI, CD, line, mic... */
        static const uint8_t r[] = {0x30,0xC0, 0x31,0xC0, 0x32,0xC0, 0x33,0xC0, 0x34,0xC0, 0x35,0xC0,
                                    0x36,0x00, 0x37,0x00, 0x38,0x00, 0x39,0x00, 0x3A,0x00, 0x3B,0x00,
                                    0x3C,0x1F, 0x3D,0x15, 0x3E,0x0B, 0x3F,0x00, 0x40,0x00, 0x41,0x00,
                                    0x42,0x00, 0x43,0x00, 0x44,0x80, 0x45,0x80, 0x46,0x80, 0x47,0x80};
        for (unsigned i = 0; i < sizeof r; i += 2) dsp.mixer[r[i]] = r[i + 1];
    }
}

static void dsp_reset(void)
{
    q_clear();
    nargs = argi = 0;
    sb_out_stop();
    dsp.paused = 0;
    dsp.speaker = 0;
    dsp.dac_value = 0x80;
    dsp.rate = 0;
    q_push(0xAA);
}

static int arg_count(uint8_t c)
{
    if (dsp.sb16) {
        if (c >= 0xB0 && c <= 0xCF) return 3;            /* mode, length */
        if (c == 0x41 || c == 0x42) return 2;            /* rate, high byte first */
    }
    switch (c) {
    case 0x10: case 0x40: case 0xE0: case 0xE2: case 0xE4:
        return 1;
    case 0x14: case 0x16: case 0x17: case 0x24: case 0x48:
    case 0x74: case 0x75: case 0x76: case 0x77: case 0x80:
        return 2;
    default:
        return 0;
    }
}

static void exec(void)
{
    uint32_t w = (uint32_t)(args[0] | (args[1] << 8)) + 1;
    switch (cmd) {
    case 0x10: dsp.dac_value = args[0]; break;
    case 0x14: sb_out_start(0, w, 0); break;                     /* 8-bit single-cycle */
    case 0x91: sb_out_start(0, dsp.block_len + 1u, 0); break;    /* high-speed single */
    case 0x1C: case 0x90:                                        /* auto-init (+high-speed) */
        sb_out_start(1, dsp.block_len + 1u, 0);
        break;
    case 0x80: sb_out_start(0, w, 1); break;                     /* silence block */
    /* Recording (the microphone, through the real card's input: audio.cpp) */
    case 0x20: q_push(sb_in_sample()); break;                    /* direct ADC: one sample */
    case 0x24: sb_in_start(0, w); break;                         /* 8-bit single-cycle ADC */
    case 0x2C: sb_in_start(1, dsp.block_len + 1u); break;        /* 8-bit auto-init ADC */
    case 0x98: sb_in_start(1, dsp.block_len + 1u); break;        /* high-speed auto-init ADC */
    case 0x99: sb_in_start(0, dsp.block_len + 1u); break;        /* high-speed single-cycle ADC */
    case 0x40: dsp.time_constant = args[0]; dsp.rate = 0; break;
    case 0x41: case 0x42:                                        /* SB16: rate */
        if (dsp.sb16) dsp.rate = (uint16_t)(args[0] << 8 | args[1]);
        break;
    case 0xD5: if (dsp.sb16) dsp.paused = 1; break;               /* SB16: 16-bit pause */
    case 0xD6: if (dsp.sb16) dsp.paused = 0; break;
    case 0xD9: if (dsp.sb16) sb_out_exit_autoinit(); break;
    case 0x48: dsp.block_len = args[0] | (args[1] << 8); break;
    case 0xD0: dsp.paused = 1; break;
    case 0xD4: dsp.paused = 0; break;
    case 0xD1: dsp.speaker = 1; break;
    case 0xD3: dsp.speaker = 0; break;
    case 0xD8: q_push(dsp.speaker ? 0xFF : 0x00); break;
    case 0xDA: sb_out_exit_autoinit(); break;
    case 0xE0: q_push((uint8_t)~args[0]); break;
    case 0xE1:
        if (dsp.sb16) { q_push(0x04); q_push(0x05); }
        else { q_push(0x03); q_push(0x02); }
        break;
    case 0xE4: test_reg = args[0]; break;
    case 0xE8: q_push(test_reg); break;
    case 0xF2: sb_out_raise_irq(); break;
    case 0xF3: if (dsp.sb16) sb_out_raise_irq16(); else sb_out_raise_irq(); break;
    case 0xF8: q_push(0x00); break;
    default:
        if (dsp.sb16 && cmd >= 0xB0 && cmd <= 0xCF && !(cmd & 1)) {
            /* SB16 Bxh (16-bit) / Cxh (8-bit): bit 3 input, bit 2 auto-init;
               mode bit 4 signed, bit 5 stereo; length = samples - 1 */
            uint32_t len = (uint32_t)(args[1] | (args[2] << 8)) + 1;
            sb16_start((cmd & 8) != 0, (cmd & 4) != 0, cmd < 0xC0,
                       (args[0] & 0x10) != 0, (args[0] & 0x20) != 0, len);
        }
        break;                  /* ADPCM, MIDI etc.: accepted and ignored */
    }
}

void dsp_set_sb16(int on)
{
    dsp.sb16 = on != 0;
    mixer_reset();
    dsp_reset();
    q_clear();
}

void dsp_init(uint16_t b)
{
    base = b;
    mixer_reset();
    dsp.time_constant = 0xA6;   /* ~11 kHz */
    dsp.block_len = 0x7FF;
    dsp_reset();
    q_clear();
}

/* ---------------------------------------------------------------- FM */

static int fm_index_port(uint16_t p)
{
    return p == 0x388 || p == 0x38A || p == base + 0 || p == base + 2 || p == base + 8;
}

static int fm_data_port(uint16_t p)
{
    return p == 0x389 || p == 0x38B || p == base + 1 || p == base + 3 || p == base + 9;
}

static int fm_bank1_port(uint16_t p)
{
    return p == 0x38A || p == 0x38B || p == base + 2 || p == base + 3;
}

static void fm_write(uint16_t reg, uint8_t v)
{
    opl_write(reg, v);
    if (reg != 0x04) return;
    if (v & 0x80) { fm_status = 0; return; }             /* reset IRQ flags */
    fm_timer_ctl = v;
    /* Timers "expire" at once: detection starts a timer, waits ~80 us and
       expects the flag. Masked timers never set their flag. */
    if ((v & 0x01) && !(v & 0x40)) fm_status |= 0xC0;
    if ((v & 0x02) && !(v & 0x20)) fm_status |= 0xA0;
}

/* ---------------------------------------------------------------- ports */

int dsp_owns(uint16_t port)
{
    if (fm_index_port(port) || fm_data_port(port)) return 1;
    switch (port - base) {
    case 0x4: case 0x5: case 0x6: case 0xA: case 0xC: case 0xE: return 1;
    case 0xF: return dsp.sb16;
    default: return 0;
    }
}

uint8_t dsp_in(uint16_t port)
{
    if (fm_index_port(port)) return fm_status;          /* OPL3: low bits read 0 */
    if (fm_data_port(port)) return 0xFF;
    switch (port - base) {
    case 0x5:
        if (dsp.sb16) {
            if (mix_index == 0x80) return 0x02;                 /* IRQ 5 */
            if (mix_index == 0x81) return 0x22;                 /* DMA 1 and 5 */
            if (mix_index == 0x82) return (uint8_t)(0x20 | sb_irq_status());
        }
        return dsp.mixer[mix_index];
    case 0xA: return q_pop();
    case 0xC:                                           /* bit 7: busy */
        /* A real DSP's busy flag flickers even when idle, and some drivers
           wait to see it set before trusting the card (same as DOSBox). */
        return (++busy_count & 8) ? 0xFF : 0x7F;
    case 0xE: sb_irq_ack(0); return q_empty() ? 0x7F : 0xFF;   /* bit 7: data available */
    case 0xF: sb_irq_ack(1); return 0xFF;               /* SB16: acknowledges the 16-bit IRQ */
    default:  return 0xFF;
    }
}

void dsp_out(uint16_t port, uint8_t v)
{
    if (fm_index_port(port)) { fm_index = v | (fm_bank1_port(port) ? 0x100 : 0); return; }
    if (fm_data_port(port)) { fm_write(fm_index, v); return; }
    switch (port - base) {
    case 0x4:
        mix_index = v;
        break;
    case 0x5:
        if (mix_index == 0x00) mixer_reset();
        else dsp.mixer[mix_index] = v;
        break;
    case 0x6:
        dbg(2, "DSP reset %x\n", v);
        if (v & 1) reset_latch = 1;
        else if (reset_latch) { reset_latch = 0; dsp_reset(); }
        break;
    case 0xC:
        dbg(2, "DSP <- %02x\n", v);
        if (nargs) {
            args[argi++] = v;
            if (argi >= nargs) { exec(); nargs = 0; }
        } else {
            cmd = v;
            argi = 0;
            nargs = arg_count(v);
            if (!nargs) exec();
        }
        break;
    }
}
