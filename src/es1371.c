/* Ensoniq AudioPCI 97 (ES1371), on by default (es1371=off: not there,
   es1371=N: on IRQ N, default 9): a PCI sound card on the guest's bus (vpci.c) at
   00:02.0, I/O at D280h (64 bytes), for Windows 9x's "Creative AudioPCI
   (ES1371,ES1373) (WDM)" driver and others written for the chip (Linux's
   snd-ens1371, the BeOS and NT drivers).

   What there is: the PCI function (config space, a movable I/O BAR, power
   management capability); the chip's registers; the two playback channels
   (DAC1, DAC2) as bus masters reading 8/16-bit mono/stereo frames from guest
   memory in loop mode, through the sample rate converter (its RAM: the
   rates and volumes the driver writes there, linear interpolation to
   48 kHz); the record channel (ADC), writing silence at its rate so a
   program recording gets its interrupts; the sample counters and their
   interrupts; an AC'97 codec (a SigmaTel STAC9721: master, PCM, the inputs'
   mixer registers, reset, power status) whose master and PCM volumes apply;
   the MIDI UART's output, to the General MIDI synth (sb/gmsynth.c). Not
   there: legacy SB emulation by I/O traps (vmdos's own Sound Blaster is at
   220h anyway), S/PDIF, the game port's remap (joy.c's 201h stays).

   Rendered into the 48 kHz mix by sound_tick (sound.c) under sound_lock;
   the guest's port accesses take sound_lock too. The IRQ is a PCI one
   (level), raised on the virtual PIC as an edge whenever a new source
   comes up, again when the driver acknowledges one with another still
   pending, and if one stays pending unserviced for long. */
#include "kernel.h"
#include "es1371.h"
#include "sb/gmsynth.h"

int es_on;
int es_present(void) { return es_on; }
static int es_irq_default = 9;

#define ES_IO_DEFAULT 0xD280           /* 10-bit ISA aliases 280h-2BFh: nothing there. Not D500h: its aliases
                                          100h-13Fh are the Voodoo 3's D100h's (100h-1FFh), and Windows 9x,
                                          taking PCI I/O as 10-bit too, then had no range left for it (Code 12) */

/* Interrupt/chip select control (00h) */
#define CTL_ADC_EN    (1u << 4)
#define CTL_DAC2_EN   (1u << 5)
#define CTL_DAC1_EN   (1u << 6)
#define CTL_UART_EN   (1u << 3)
/* Interrupt status (04h) */
#define ST_INTR       (1u << 31)
#define ST_UART       (1u << 3)
#define ST_DAC1       (1u << 2)
#define ST_DAC2       (1u << 1)
#define ST_ADC        (1u << 0)
/* Serial interface control (20h) */
#define SI_P1_INT_EN  (1u << 8)
#define SI_P2_INT_EN  (1u << 9)
#define SI_R1_INT_EN  (1u << 10)
#define SI_P1_PAUSE   (1u << 11)
#define SI_P2_PAUSE   (1u << 12)
/* SRC interface (10h) */
#define SRC_WE        (1u << 24)
/* Codec (14h) */
#define CODEC_RDY     (1u << 31)
#define CODEC_PIRD    (1u << 23)
/* UART */
#define UST_RXRDY     0x01
#define UST_TXRDY     0x02
#define UST_TXINT     0x04
#define UST_RXINT     0x80
/* Legacy control (18h) */
#define LEG_FORCE_IRQ (1u << 24)

struct ch {
    u32 addr;                          /* frame register 1: the buffer's physical address */
    u16 size, count;                   /* frame register 2: longwords - 1, the current longword */
    u8 sub;                            /* bytes into the current longword */
    u16 samp_ct;                       /* samples per interrupt - 1 */
    int curr;                          /* counting down */
    u32 acc;                           /* the SRC's accumulator: 1 << 19 per input sample */
    int s0l, s0r, s1l, s1r;            /* the two input samples being interpolated */
    /* perf: the buffer in 32 parts, each one's samples hashed as they're
       played: the same as a lap before (not silence) is a part the driver
       didn't refill in time, a lap-old echo you hear as a crackle */
    int seg;
    u32 seg_hash, seg_nz, lap_hash[32];
    u32 st_frames, st_irqs, st_segs, st_stale, st_pos;
};

static struct {
    /* PCI */
    u8 cmd, latency, line;
    u16 bar, pmcsr;
    /* the chip */
    u32 ctrl, status, legacy, si, src_reg, codec_reg, spdif;
    u8 page, uart_ctrl, uart_status, uart_res;
    u32 uart_fifo[8];
    u16 src[128];
    struct ch ch[3];                   /* DAC1, DAC2, ADC */
    u16 ac97[64];
    int vol_l, vol_r;                  /* master x PCM, Q15 */
    int irq_pending, stuck;
    u32 st_raised, st_reraised, st_acks, st_t0, st_clip_dac, st_clip_mix, st_out;
} es;

/* ---------------- guest memory ---------------- */

static volatile u8 *mem(u32 a)
{
    if (hv_active && a >= (10u << 20)) return hv_gpa_ptr(a);   /* VMHV RAM above 10 MB isn't at its own address */
    return gptr(a);
}
static int rd_s16(u32 a) { volatile u8 *p = mem(a); return p ? (int)*(volatile int16_t *)p : 0; }
static int rd_u8(u32 a) { volatile u8 *p = mem(a); return p ? *p : 0x80; }
static void wr_u8(u32 a, u8 v) { volatile u8 *p = mem(a); if (p) *p = v; }
static void wr_16(u32 a, u16 v) { volatile u8 *p = mem(a); if (p) *(volatile u16 *)p = v; }

/* ---------------- the AC'97 codec ---------------- */

/* 1.5 dB steps: attenuation (master, 0 = 0 dB) and gain (PCM out, 8 = 0 dB), Q15 */
static const u16 att_tab[32] = {
    32767,27571,23198,19519,16423,13818,11627,9783,8231,6925,5827,4903,4125,3471,2920,2457,
    2068,1740,1464,1232,1036,872,734,617,519,437,368,309,260,219,184,155 };
static const u16 gain_tab[32] = {
    65535,65535,65535,65535,65381,55011,46286,38945,32768,27571,23198,19519,16423,13818,11627,9783,
    8231,6925,5827,4903,4125,3471,2920,2457,2068,1740,1464,1232,1036,872,734,617 };

static void ac97_volumes(void)
{
    u16 m = es.ac97[0x02 >> 1], p = es.ac97[0x18 >> 1];
    if ((m & 0x8000) || (p & 0x8000)) { es.vol_l = es.vol_r = 0; return; }
    es.vol_l = (int)((u32)att_tab[(m >> 8) & 0x1F] * gain_tab[(p >> 8) & 0x1F] >> 15);
    es.vol_r = (int)((u32)att_tab[m & 0x1F] * gain_tab[p & 0x1F] >> 15);
}

static void ac97_reset(void)
{
    memset(es.ac97, 0, sizeof es.ac97);
    static const u8 regs[] = { 0x02, 0x04, 0x06, 0x1C };
    for (unsigned i = 0; i < sizeof regs; i++) es.ac97[regs[i] >> 1] = 0x8000;
    es.ac97[0x0C >> 1] = es.ac97[0x0E >> 1] = 0x8008;
    for (int r = 0x10; r <= 0x18; r += 2) es.ac97[r >> 1] = 0x8808;
    ac97_volumes();
}

static u16 ac97_read(int reg)
{
    reg &= 0x7E;
    switch (reg) {
    case 0x00: return 0x0000;                       /* reset: the basic feature set */
    case 0x26: return (es.ac97[reg >> 1] & 0xFF00) | 0x000F;   /* ADC, DAC, analog, Vref ready */
    case 0x7C: return 0x8384;                       /* SigmaTel */
    case 0x7E: return 0x7609;                       /* STAC9721 */
    }
    return es.ac97[reg >> 1];
}

static void ac97_write(int reg, u16 v)
{
    reg &= 0x7E;
    u16 mask;
    switch (reg) {
    case 0x00: ac97_reset(); return;
    case 0x02: case 0x04:                           /* 5-bit volumes: a 6th bit reads as 1Fh */
        if (v & 0x2000) v = (v & ~0x3F00) | 0x1F00;
        if (v & 0x0020) v = (v & ~0x003F) | 0x001F;
        mask = 0x9F1F; break;
    case 0x06: if (v & 0x20) v = (v & ~0x3F) | 0x1F; mask = 0x801F; break;
    case 0x0A: mask = 0x801E; break;
    case 0x0C: mask = 0x801F; break;
    case 0x0E: mask = 0x805F; break;
    case 0x10: case 0x12: case 0x14: case 0x16: case 0x18: mask = 0x9F1F; break;
    case 0x1A: mask = 0x0707; break;
    case 0x1C: mask = 0x8F0F; break;
    case 0x20: mask = 0xB380; break;
    case 0x26: mask = 0xFF00; break;
    default: return;                                /* read-only or not there */
    }
    es.ac97[reg >> 1] = v & mask;
    ac97_volumes();
}

/* ---------------- interrupts ---------------- */

static u32 sources(void)
{
    u32 s = 0;
    if ((es.status & ST_DAC1) && (es.si & SI_P1_INT_EN)) s |= ST_DAC1;
    if ((es.status & ST_DAC2) && (es.si & SI_P2_INT_EN)) s |= ST_DAC2;
    if ((es.status & ST_ADC) && (es.si & SI_R1_INT_EN)) s |= ST_ADC;
    if (es.uart_status & (UST_TXINT | UST_RXINT)) s |= ST_UART;
    return s;
}

/* After a change: INTR and the UART bit anew; the IRQ when a source came
   up that wasn't there before (old), or when one remains after an ack. */
static void update_irq(u32 old, int acked)
{
    u32 s = sources();
    es.status = (es.status & ~(ST_INTR | ST_UART)) | (s & ST_UART) | (s ? ST_INTR : 0);
    if ((s & ~old) || (s && acked)) es.irq_pending = 1;
    if (!s) es.stuck = 0;
}

static void uart_update(void)
{
    u32 old = sources();
    es.uart_status &= ~UST_TXINT;
    if ((es.uart_ctrl & 0x60) == 0x20 && (es.uart_status & UST_TXRDY)) es.uart_status |= UST_TXINT;
    update_irq(old, 0);
}

int es_take_irq(void)
{
    if (!es.irq_pending) return 0;
    es.irq_pending = 0;
    es.st_raised++;
    return 1;
}

int es_irq_line(void) { return es.line && es.line < 16 ? es.line : es_irq_default; }

/* ---------------- the channels ---------------- */

static int ch_fmt(int i) { return (es.si >> (2 * i)) & 3; }     /* bit 0 stereo, bit 1 16-bit */

static void ch_start(int i)
{
    struct ch *c = &es.ch[i];
    c->seg = -1;
    c->seg_hash = c->seg_nz = 0;
    for (int k = 0; k < 32; k++) c->lap_hash[k] = 0;
    c->sub = 0;
    c->curr = c->samp_ct;
    c->s0l = c->s0r = c->s1l = c->s1r = 0;
}

static void ch_advance(struct ch *c, int bytes)
{
    c->sub += bytes;
    if (c->sub >= 4) {
        c->sub -= 4;
        if (++c->count > c->size) c->count = 0;     /* loop mode */
    }
}

static void ch_counted(int i)
{
    struct ch *c = &es.ch[i];
    if (--c->curr >= 0) return;
    c->curr = c->samp_ct;
    static const u32 bit[3] = { ST_DAC1, ST_DAC2, ST_ADC }, en[3] = { SI_P1_INT_EN, SI_P2_INT_EN, SI_R1_INT_EN };
    if (!(es.si & en[i])) return;
    c->st_irqs++;
    u32 old = sources();
    es.status |= bit[i];
    update_irq(old, 0);
}

static void dac_fetch(int i)
{
    struct ch *c = &es.ch[i];
    int f = ch_fmt(i), l, r;
    u32 a = c->addr + (u32)c->count * 4 + c->sub;
    if (f & 2) { l = rd_s16(a); r = (f & 1) ? rd_s16(a + 2) : l; ch_advance(c, (f & 1) ? 4 : 2); }
    else { l = (rd_u8(a) ^ 0x80) << 8; r = (f & 1) ? (rd_u8(a + 1) ^ 0x80) << 8 : l; ch_advance(c, (f & 1) ? 2 : 1); }
    c->s0l = c->s1l; c->s0r = c->s1r;
    c->s1l = l; c->s1r = r;
    if (perf_on) {
        int seg = (int)((u32)c->count * 32 / ((u32)c->size + 1));
        if (seg != c->seg) {
            if (c->seg >= 0 && c->seg_nz) {
                c->st_segs++;
                if (c->seg_hash == c->lap_hash[c->seg]) c->st_stale++;
            }
            if (c->seg >= 0) c->lap_hash[c->seg] = c->seg_nz ? c->seg_hash : 0;
            c->seg = seg; c->seg_hash = 2166136261u; c->seg_nz = 0;
        }
        c->seg_hash = (c->seg_hash ^ ((u32)(u16)l << 16 | (u16)r)) * 16777619u;
        c->seg_nz |= (u32)(l | r);
        c->st_frames++;
    }
    ch_counted(i);
}

static u32 dac_vf(int i);

/* perf: every 5 s while a DAC plays, how the driver set it up and kept up */
static void es_stats(void)
{
    if (!perf_on) return;
    if (!es.st_t0) { es.st_t0 = ticks; return; }
    u32 dt = ticks - es.st_t0;
    if (dt < 5000) return;
    es.st_t0 = ticks;
    for (int i = 0; i < 2; i++) {
        struct ch *c = &es.ch[i];
        if (c->st_frames) {
            static const char *const fmt[4] = { "8-bit mono", "8-bit stereo", "16-bit mono", "16-bit stereo" };
            kprintf("es1371: DAC%d %u Hz %s, buffer %u bytes, IRQ every %u samples: played %u Hz, %u IRQs/s, "
                    "position read %u/s, parts not refilled %u of %u\n", i + 1, (dac_vf(i) * 3000u + (1u << 14)) >> 15,
                    fmt[ch_fmt(i)], ((u32)c->size + 1) * 4, (u32)c->samp_ct + 1, c->st_frames * 1000u / dt,
                    c->st_irqs * 1000u / dt, c->st_pos * 1000u / dt, c->st_stale, c->st_segs);
        }
        c->st_frames = c->st_irqs = c->st_segs = c->st_stale = c->st_pos = 0;
    }
    if (es.st_raised || es.st_reraised)
        kprintf("es1371: IRQ raised %u/s, acknowledged %u/s, raised again unserviced (60 ms) %u\n",
                es.st_raised * 1000u / dt, es.st_acks * 1000u / dt, es.st_reraised);
    if (es.st_out)
        kprintf("es1371: AC'97 master %04x PCM %04x (x%u.%02u, %u.%02u), SRC volume DAC1 %04x/%04x DAC2 %04x/%04x "
                "(1000h = 1); clipped: DACs summed %u, the mix %u of %u samples\n",
                es.ac97[0x02 >> 1], es.ac97[0x18 >> 1], (u32)es.vol_l >> 15, ((u32)es.vol_l & 0x7FFF) * 100u >> 15,
                (u32)es.vol_r >> 15, ((u32)es.vol_r & 0x7FFF) * 100u >> 15, es.src[0x7C], es.src[0x7D],
                es.src[0x7E], es.src[0x7F], es.st_clip_dac, es.st_clip_mix, es.st_out);
    es.st_raised = es.st_reraised = es.st_acks = es.st_clip_dac = es.st_clip_mix = es.st_out = 0;
}

static u32 dac_vf(int i)                             /* the SRC's variable frequency: 1 << 15 per 3000 Hz */
{
    int b = 0x70 + 4 * i;
    return ((u32)(es.src[b + 1] & 0xFC00) << 5) | (es.src[b + 3] & 0x7FFF);
}

static void adc_step(void)
{
    struct ch *c = &es.ch[2];
    u32 n = (es.src[0x78] >> 4) & 0x1F;
    u32 vf = ((u32)(es.src[0x79] & 0xFC00) << 5) | (es.src[0x7B] & 0x7FFF);
    if (!n || !vf) return;
    c->acc += n << 15;                               /* rate = 48000 * n * 2^15 / vf */
    while (c->acc >= vf) {
        c->acc -= vf;
        int f = ch_fmt(2);
        u32 a = c->addr + (u32)c->count * 4 + c->sub;
        int bytes = ((f & 2) ? 2 : 1) * ((f & 1) ? 2 : 1);
        for (int k = 0; k < bytes; k += (f & 2) ? 2 : 1) {
            if (f & 2) wr_16(a + k, 0); else wr_u8(a + k, 0x80);
        }
        ch_advance(c, bytes);
        ch_counted(2);
    }
}

static int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

void es_render(int16_t *buf, int n)
{
    if (!es_on) return;
    int run[2];
    run[0] = (es.ctrl & CTL_DAC1_EN) && !(es.si & SI_P1_PAUSE);
    run[1] = (es.ctrl & CTL_DAC2_EN) && !(es.si & SI_P2_PAUSE);
    int adc = (es.ctrl & CTL_ADC_EN) != 0;
    u32 vf[2] = { dac_vf(0), dac_vf(1) };
    int vl[2] = { (int16_t)es.src[0x7C], (int16_t)es.src[0x7E] };   /* the SRC's volumes: 1000h = 1 */
    int vr[2] = { (int16_t)es.src[0x7D], (int16_t)es.src[0x7F] };
    if ((run[0] && vf[0]) || (run[1] && vf[1]) || adc) {
        for (int k = 0; k < n; k++) {
            int l = 0, r = 0;
            for (int i = 0; i < 2; i++) {
                if (!run[i] || !vf[i]) continue;
                struct ch *c = &es.ch[i];
                c->acc += vf[i];
                while (c->acc >= (1u << 19)) { c->acc -= 1u << 19; dac_fetch(i); }
                int fr = (int)(c->acc >> 4);         /* 15 bits */
                int sl = c->s0l + (((c->s1l - c->s0l) * fr) >> 15);
                int sr = c->s0r + (((c->s1r - c->s0r) * fr) >> 15);
                l += (sl * vl[i]) >> 12;
                r += (sr * vr[i]) >> 12;
            }
            if (adc) adc_step();
            if (es.vol_l | es.vol_r) {
                if (perf_on && (l != clamp16(l) || r != clamp16(r))) es.st_clip_dac++;
                l = (int)(((int64_t)clamp16(l) * es.vol_l) >> 15);
                r = (int)(((int64_t)clamp16(r) * es.vol_r) >> 15);
                int ml = buf[2 * k] + l, mr = buf[2 * k + 1] + r;
                if (perf_on) { es.st_out++; if (ml != clamp16(ml) || mr != clamp16(mr)) es.st_clip_mix++; }
                buf[2 * k] = (int16_t)clamp16(ml);
                buf[2 * k + 1] = (int16_t)clamp16(mr);
            }
        }
    }
    /* a level interrupt nobody took (some 60 ms at 250 renders a second): again */
    if (es.status & ST_INTR) { if (++es.stuck >= 15) { es.stuck = 0; es.irq_pending = 1; es.st_reraised++; } }
    es_stats();
}

/* ---------------- the registers ---------------- */

static u32 frame_read(int off)
{
    int p = es.page & 0xF;
    if (p >= 0xE) return es.uart_fifo[((p & 1) << 2) + ((off >> 2) & 3)];
    struct ch *c = p == 0xC ? &es.ch[off >= 0x38] : p == 0xD && off < 0x38 ? &es.ch[2] : 0;
    if (!c) return 0xFFFFFFFFu;
    if ((off & 4) && p == 0xC) c->st_pos++;
    return (off & 4) ? c->size | (u32)c->count << 16 : c->addr;
}

static void frame_write(int off, u32 v)
{
    int p = es.page & 0xF;
    if (p >= 0xE) { es.uart_fifo[((p & 1) << 2) + ((off >> 2) & 3)] = v; return; }
    struct ch *c = p == 0xC ? &es.ch[off >= 0x38] : p == 0xD && off < 0x38 ? &es.ch[2] : 0;
    if (!c) return;
    if (off & 4) { c->size = (u16)v; c->count = (u16)(v >> 16); c->sub = 0; }
    else c->addr = v;
}

static u32 reg_read(int off)                         /* off: longword aligned */
{
    switch (off) {
    case 0x00: return es.ctrl & 0xFF0FFFFFu;
    case 0x04: return es.status;
    case 0x08: return (u32)(es.uart_status & 0x87) << 8 | (u32)(es.uart_res & 1) << 16 | 0xFF000000u;
    case 0x0C: return es.page & 0xF;
    case 0x10: return (es.src_reg & 0xFF7F0000u) | es.src[es.src_reg >> 25];
    case 0x14: return es.codec_reg | CODEC_RDY;
    case 0x18: return (es.legacy & 0xFFFF07FDu) | 0xF800;
    case 0x1C: return es.spdif;
    case 0x20: return es.si | 0xFF800000u;
    case 0x24: case 0x28: case 0x2C: {
        struct ch *c = &es.ch[(off - 0x24) >> 2];
        return c->samp_ct | (u32)(u16)(c->curr < 0 ? 0 : c->curr) << 16; }
    default: return frame_read(off);
    }
}

static void reg_write(int off, u32 v, u32 m)         /* m: the bytes written */
{
    u32 old = sources();
    switch (off) {
    case 0x00: {
        u32 nv = (es.ctrl & ~m) | (v & m);
        static const u32 en[3] = { CTL_DAC1_EN, CTL_DAC2_EN, CTL_ADC_EN };
        for (int i = 0; i < 3; i++) if (!(es.ctrl & en[i]) && (nv & en[i])) ch_start(i);
        es.ctrl = nv;
        break; }
    case 0x04: break;                                /* read-only on the ES1371 */
    case 0x08:
        if (m & 0xFF) {                              /* UART data: out to the GM synth, at once */
            if (es.ctrl & CTL_UART_EN) gm_midi_byte((u8)v);
            es.uart_status |= UST_TXRDY;
            uart_update();
        }
        if (m & 0xFF00) {                            /* UART control */
            es.uart_ctrl = (u8)(v >> 8) & 0xE3;
            es.uart_status = (es.uart_status & ~(UST_RXRDY | UST_RXINT)) | UST_TXRDY;
            uart_update();
        }
        if (m & 0xFF0000) es.uart_res = (u8)(v >> 16) & 1;
        break;
    case 0x0C: if (m & 0xFF) es.page = (u8)v & 0xF; break;
    case 0x10:
        es.src_reg = v & 0xFF7FFFFFu;                /* never busy */
        if (v & SRC_WE) {
            int a = (int)(v >> 25);
            es.src[a] = (u16)v;
            if (a >= 0x70 && a < 0x78 && ((a & 3) == 1 || (a & 3) == 2)) {   /* the accumulator */
                struct ch *c = &es.ch[(a - 0x70) >> 2];
                int b = a & ~3;
                c->acc = ((((u32)es.src[b + 1] & 0xFF) << 15) | (es.src[b + 2] & 0x7FFF)) & ((1u << 19) - 1);
            }
        }
        break;
    case 0x14:
        if (v & CODEC_PIRD) es.codec_reg = (v & 0x007F0000u) | ac97_read((int)(v >> 16) & 0x7F);
        else { es.codec_reg = v & 0x00FFFFFFu; ac97_write((int)(v >> 16) & 0x7F, (u16)v); }
        break;
    case 0x18:
        if (m & 0xFF) es.legacy |= 1;               /* the legacy interrupt: acknowledged */
        if (!(es.legacy & LEG_FORCE_IRQ) && (v & m & LEG_FORCE_IRQ)) es.irq_pending = 1;   /* the driver's IRQ test */
        es.legacy = (es.legacy & ~(m & 0xFFFF0000u)) | (v & m & 0xFFFF0000u);
        break;
    case 0x1C: es.spdif = (es.spdif & ~m) | (v & m); break;
    case 0x20: {
        es.si = (((es.si & ~m) | (v & m)) & 0x007FFFFFu);
        if (!(es.si & SI_P1_INT_EN)) es.status &= ~ST_DAC1;     /* the acknowledgment: enable off, on */
        if (!(es.si & SI_P2_INT_EN)) es.status &= ~ST_DAC2;
        if (!(es.si & SI_R1_INT_EN)) es.status &= ~ST_ADC;
        if ((old & ~sources()) & (ST_DAC1 | ST_DAC2 | ST_ADC)) es.st_acks++;
        update_irq(old, 1);
        return; }
    case 0x24: case 0x28: case 0x2C:
        if (m & 0xFFFF) {
            int i = (off - 0x24) >> 2;
            static const u32 en[3] = { CTL_DAC1_EN, CTL_DAC2_EN, CTL_ADC_EN };
            es.ch[i].samp_ct = (u16)v;
            if (!(es.ctrl & en[i])) es.ch[i].curr = (u16)v;
        }
        break;
    default:
        if (off >= 0x30 && m == 0xFFFFFFFFu) frame_write(off, v);
        else if (off >= 0x30) frame_write(off, (frame_read(off) & ~m) | (v & m));
        break;
    }
    update_irq(old, 0);
}

static void chip_reset(void)
{
    es.ctrl = 0xFCFF0000u;
    es.status = 0x7FFFDEC0u;                         /* bit 13 off: the real ES1371 reads 1 there (reserved),
                                                        but Creative's 5.12.01 WDM driver takes it for the
                                                        ES1373's GPIO interrupt and crashes on a null DPC */
    es.uart_status = UST_TXRDY;
    es.uart_ctrl = 0;
    es.uart_res = 0;
    es.page = 0;
    es.src_reg = 0x00470000u;
    es.codec_reg = 0;
    es.legacy = 0x0000F801u;
    es.spdif = 0xC0200004u;
    es.si = 0;
    memset(es.src, 0, sizeof es.src);
    memset(es.ch, 0, sizeof es.ch);
    memset(es.uart_fifo, 0xFF, sizeof es.uart_fifo);
    es.irq_pending = es.stuck = 0;
    ac97_reset();
}

/* The I/O BAR: 1 if port is the card's. */
int es_io(u16 port, int size, int write, u32 *val)
{
    if (!es_on || !(es.cmd & 1) || (u16)(port - es.bar) >= 0x40) return 0;
    int off = port & 0x3F, sh = (off & 3) * 8;
    u32 m = (size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu) << sh;
    rlock_take(&sound_lock);
    if (write) reg_write(off & 0x3C, *val << sh, m);
    else {
        u32 v = reg_read(off & 0x3C) >> sh;
        *val = size == 1 ? (v & 0xFF) : size == 2 ? (v & 0xFFFF) : v;
    }
    int irq = es_take_irq();
    rlock_drop(&sound_lock);
    if (irq) vpic_raise(es_irq_line());
    if (debug_level >= 4) kprintf("ES1371 %s %02x/%d %08x\n", write ? "out" : "in ", off, size, *val);
    return 1;
}

/* ---------------- PCI configuration space ---------------- */

static u8 cfg_byte(int r)
{
    switch (r) {
    case 0x00: return 0x74; case 0x01: return 0x12;  /* Ensoniq */
    case 0x02: return 0x71; case 0x03: return 0x13;  /* ES1371 */
    case 0x04: return es.cmd;
    case 0x06: return 0x10;                          /* capability list */
    case 0x07: return 0x02;                          /* medium DEVSEL# */
    case 0x08: return 0x02;                          /* revision */
    case 0x0A: return 0x01; case 0x0B: return 0x04;  /* multimedia audio */
    case 0x0D: return es.latency;
    case 0x10: return (u8)(es.bar & 0xC0) | 0x01;    /* I/O, 64 bytes */
    case 0x11: return (u8)(es.bar >> 8);
    case 0x2C: return 0x74; case 0x2D: return 0x12;  /* subsystem 1274:1371 */
    case 0x2E: return 0x71; case 0x2F: return 0x13;
    case 0x34: return 0xDC;
    case 0x3C: return es.line;
    case 0x3D: return 0x01;                          /* INTA# */
    case 0x3E: return 0x0C; case 0x3F: return 0x80;
    case 0xDC: return 0x01;                          /* power management, version 1.1 */
    case 0xDE: return 0x31; case 0xDF: return 0x6C;
    case 0xE0: return (u8)es.pmcsr; case 0xE1: return (u8)(es.pmcsr >> 8);
    }
    return 0;
}

u32 es_cfg_read(int reg, int size)
{
    u32 v = 0;
    for (int i = 0; i < size; i++) v |= (u32)cfg_byte((reg + i) & 0xFF) << (8 * i);
    return v;
}

void es_cfg_write(int reg, u32 val, int size)
{
    for (int i = 0; i < size; i++) {
        u8 b = (u8)(val >> (8 * i));
        switch ((reg + i) & 0xFF) {
        case 0x04: es.cmd = b & 0x05; break;         /* I/O space, bus master */
        case 0x0D: es.latency = b; break;
        case 0x10: es.bar = (es.bar & 0xFF00) | (b & 0xC0); break;
        case 0x11: es.bar = (es.bar & 0x00C0) | (u16)b << 8; break;
        case 0x3C: es.line = b; break;
        case 0xE0: es.pmcsr = (es.pmcsr & 0xFF00) | (b & 0x03); break;
        case 0xE1: es.pmcsr = (es.pmcsr & 0x00FF) | (u16)(b & 0x01) << 8; break;
        }
    }
}

/* Boot and every restart (bios_init through vpci_bios_patch): on unless
   es1371=off, the card as a PC's BIOS leaves it (I/O on, BAR and IRQ set). */
void es_boot(void)
{
    static int said;
    const char *p = strstr(cmdline, "es1371");
    if (p && !strncmp(p + 6, "=off", 4)) return;
    if (p && p[6] == '=' && p[7] >= '1' && p[7] <= '9') {
        int n = p[7] - '0';
        if (p[8] >= '0' && p[8] <= '5' && n == 1) n = 10 + (p[8] - '0');
        if (n != 2) es_irq_default = n;
    }
    rlock_take(&sound_lock);
    memset(&es, 0, sizeof es);
    es.bar = ES_IO_DEFAULT;
    es.line = (u8)es_irq_default;
    es.cmd = 0x05;
    es.latency = 0x40;
    chip_reset();
    es_on = 1;
    rlock_drop(&sound_lock);
    if (!said++) kprintf("sound: Ensoniq AudioPCI (ES1371) at PCI 00:%02x.0, I/O %xh, IRQ %d\n",
                         ES_DEVFN >> 3, ES_IO_DEFAULT, es_irq_default);
}
