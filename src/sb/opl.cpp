/* SBPRO <-> dbopl glue. Three chips: 0 = the game's FM (AdLib / SB Pro OPL3),
   1 and 2 = SBPRO's own General MIDI synth (36 voices) behind the MPU-401. Each is constructed
   with placement new, since a JLM has no C++ runtime to run constructors. */
#include <stdint.h>
#include <stddef.h>
#include "dbopl.h"
#include "opl.h"

inline void *operator new(size_t, void *p) noexcept { return p; }

alignas(8) static unsigned char handler_mem[OPL_CHIPS][sizeof(DBOPL::Handler)];
static DBOPL::Handler *chips[OPL_CHIPS];
static Bit32s buf[2 * OPL_MAX_FRAMES];

extern "C" void gm_tables_init(int opl_rate_x100);   /* gmsynth tables, uses the FPU */

extern "C" void opl_init(int rate)
{
    /* Table setup uses the FPU once; keep whatever state the machine had. */
    alignas(4) unsigned char fpu_state[108];
    __asm__ volatile("fnsave %0\n\tfninit" : "=m"(fpu_state));
    for (int i = 0; i < OPL_CHIPS; i++) {
        chips[i] = new (handler_mem[i]) DBOPL::Handler();
        chips[i]->Init((Bitu)rate);
    }
    gm_tables_init(4971591);                /* 14318180 / 288 Hz, x100 */
    __asm__ volatile("frstor %0" : : "m"(fpu_state));
}

/* The game chip's writes are kept with the time they were made (pit_clock)
   and made at that point of the render, between the frames they fall
   between: a program that plays samples by writing a level to an operator
   at ~8 kHz (Silent Service II's ASOUND.SS: "Dive! Dive!" by register 43h
   from a polled PIT channel 2; other AdLib speech and drum drivers) is
   heard at its own rate instead of one write per 4 ms render (a buzz). */
extern "C" uint32_t pit_clock(void);
#define OPLQ 4096
static struct { uint32_t t; uint16_t reg; uint8_t val; } oplq[OPLQ];
static uint32_t oq_head, oq_tail;

extern "C" void opl_write(uint32_t reg, uint8_t val)
{
    if (!chips[0]) return;
    uint32_t next = (oq_tail + 1) % OPLQ;
    if (next == oq_head) {                      /* nobody rendering: the oldest goes now */
        chips[0]->WriteReg(oplq[oq_head].reg, oplq[oq_head].val);
        oq_head = (oq_head + 1) % OPLQ;
    }
    oplq[oq_tail].t = pit_clock();
    oplq[oq_tail].reg = (uint16_t)reg;
    oplq[oq_tail].val = val;
    oq_tail = next;
}

extern "C" void midi_opl_write(int chip, uint32_t reg, uint8_t val)
{
    if (chips[1 + chip]) chips[1 + chip]->WriteReg(reg, val);
}

/* gain: x/4. The MIDI synth plays up to 18 voices at once, so it is mixed
   a little lower than a game's own FM chip. A chip with every voice silent
   (the MIDI synth when no MIDI plays, the game's chip between tunes) is
   skipped: it would only add zeros. */
static void mix_chip(DBOPL::Handler *h, int32_t *out, int frames, int gain)
{
    if (h->chip.Silent()) return;
    h->Generate(buf, (Bitu)frames);
    const Bit32s *b = buf;
    if (h->chip.opl3Active) {
        int n = frames * 2;
        if (gain == 4) for (int i = 0; i < n; i++) out[i] += b[i];
        else           for (int i = 0; i < n; i++) out[i] += b[i] * gain >> 2;
    } else {
        if (gain == 4)
            for (int i = 0; i < frames; i++, out += 2) { out[0] += b[i]; out[1] += b[i]; }
        else
            for (int i = 0; i < frames; i++, out += 2) { int32_t v = b[i] * gain >> 2; out[0] += v; out[1] += v; }
    }
}

/* Adds both chips' output to 'out' (interleaved stereo), the frames
   covering PIT time [t0, t1): the game chip in pieces, each queued write
   made at its frame. */
/* The frame of [0, frames) a write at t falls in; frames + 1: after t1 (a later render's) */
static int oq_frame(uint32_t t, uint32_t t0, uint32_t len, int frames)
{
    int32_t d = (int32_t)(t - t0);
    if (d <= 0) return 0;
    if ((uint32_t)d >= len) return frames + 1;
    return (int)((uint64_t)(uint32_t)d * (uint32_t)frames / len);
}

extern "C" void opl_mix(int32_t *out, int frames, uint32_t t0, uint32_t t1)
{
    if (frames > OPL_MAX_FRAMES) frames = OPL_MAX_FRAMES;
    if (chips[0]) {
        uint32_t len = t1 - t0;
        int done = 0;
        for (;;) {
            int next = frames;
            while (oq_head != oq_tail) {            /* the writes due by frame 'done' */
                int f = oq_frame(oplq[oq_head].t, t0, len, frames);
                if (f > done) { if (f < next) next = f; break; }
                chips[0]->WriteReg(oplq[oq_head].reg, oplq[oq_head].val);
                oq_head = (oq_head + 1) % OPLQ;
            }
            if (done >= frames) break;
            mix_chip(chips[0], out + 2 * done, next - done, 4);   /* to the next write */
            done = next;
        }
    }
    for (int i = 1; i < OPL_CHIPS; i++)
        if (chips[i]) mix_chip(chips[i], out, frames, 3);
}
