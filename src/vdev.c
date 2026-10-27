/* Virtual hardware the DOS guest sees through trapped I/O ports:
   8259 PICs, 8254 PIT, 8042 keyboard controller, port 61h, A20, CMOS. */
#include "kernel.h"

/* ---------------- 8259 ---------------- */
struct pic { u8 irr, isr, imr, base, init, icw4, read_isr; };
int mouse_log(void);
static int mlog_n;                                   /* mouselog: the interrupt path, the first 200 events */
#define MLOG(...) do { if (mouse_log() && mlog_n < 200) { mlog_n++; kprintf(__VA_ARGS__); } } while (0)
static struct pic pm = { 0, 0, 0xF8, 0x08, 0, 0, 0 };
static struct pic ps = { 0, 0, 0xFF, 0x70, 0, 0, 0 };
static int chosen_irq = -1;

/* Back to how the BIOS leaves them (a restart into a floppy, floppy.c). */
void vpic_reset(void)
{
    struct pic m = { 0, 0, 0xF8, 0x08, 0, 0, 0 }, s = { 0, 0, 0xFF, 0x70, 0, 0, 0 };
    pm = m; ps = s;
    chosen_irq = -1;
}

void vpic_raise(int irq)
{
    if (irq < 8) pm.irr |= 1 << irq;
    else ps.irr |= 1 << (irq - 8);
}

static int pic_best(struct pic *p, u8 extra)
{
    u8 pend = (p->irr & ~p->imr) | extra;
    for (int i = 0; i < 8; i++) {
        if (p->isr & (1 << i)) return -1;
        if (pend & (1 << i)) return i;
    }
    return -1;
}

int vpic_base0(void) { return pm.base; }
void vpic_lower(int irq) { if (irq < 8) pm.irr &= (u8)~(1 << irq); else ps.irr &= (u8)~(1 << (irq - 8)); }
int chosen_irq_of(int vec)                     /* the IRQ behind a vector the PICs deliver, -1: none */
{
    if (vec >= pm.base && vec < pm.base + 8) return vec - pm.base;
    if (vec >= ps.base && vec < ps.base + 8) return 8 + vec - ps.base;
    return -1;
}
int vpic_pending(void)
{
    int s = pic_best(&ps, 0);
    int m = pic_best(&pm, (s >= 0 && !(pm.imr & 4)) ? 4 : 0);
    if (m < 0) return -1;
    if (m == 2 && s >= 0) { chosen_irq = 8 + s; return ps.base + s; }
    if (m == 2) return -1;
    chosen_irq = m;
    return pm.base + m;
}

void vpic_ack(int vec)
{
    (void)vec;
    int irq = chosen_irq;
    if (irq < 0) return;
    if (irq < 8) { pm.isr |= 1 << irq; pm.irr &= ~(1 << irq); }
    else { ps.isr |= 1 << (irq - 8); ps.irr &= ~(1 << (irq - 8)); pm.isr |= 4; }
    chosen_irq = -1;
}

static int vpic_in_service(int irq)
{
    return irq < 8 ? (pm.isr >> irq) & 1 : (ps.isr >> (irq - 8)) & 1;
}

static void pic_eoi(struct pic *p)
{
    for (int i = 0; i < 8; i++)
        if (p->isr & (1 << i)) { p->isr &= ~(1 << i); return; }
}
void vpic_eoi_master(void) { pic_eoi(&pm); }
void vpic_eoi_irq(int irq)
{
    if (irq < 8) pm.isr &= ~(1 << irq);
    else { ps.isr &= ~(1 << (irq - 8)); if (!ps.isr) pm.isr &= ~4; }
}

static void pic_out(struct pic *p, int a0, u8 v)
{
    if (!a0) {
        if (v & 0x10) { p->init = 1; p->icw4 = v & 1; p->imr = 0; p->isr = 0; p->irr = 0; return; }
        if (v & 0x08) { if ((v & 3) == 2) p->read_isr = 0; else if ((v & 3) == 3) p->read_isr = 1; return; }
        if ((v & 0xE0) == 0x20) pic_eoi(p);                          /* non-specific EOI */
        else if ((v & 0xE0) == 0x60) p->isr &= ~(1 << (v & 7));      /* specific EOI */
        return;
    }
    switch (p->init) {
    case 1: p->base = v & 0xF8; p->init = 2; return;
    case 2: p->init = p->icw4 ? 3 : 0; return;
    case 3: p->init = 0; return;
    }
    if (mouse_log() && (p->base == 0x70 || p == &ps)) MLOG("mouse: guest writes slave PIC mask %02x (IRQ 12 %s)\n", v, v & 0x10 ? "masked" : "open");
    p->imr = v;
}

static u8 pic_in(struct pic *p, int a0)
{
    if (a0) return p->imr;
    return p->read_isr ? p->isr : p->irr;
}

/* ---------------- 8254 ---------------- */
struct pitch { u32 reload, start; u16 latch; u8 mode, access, latched, rflip, wflip, wlo; };
static struct pitch pit[3];
static u32 next_irq0;
static u8 port61;

u32 vpit_clock(void) { return pit_clock(); }

static u32 reload_of(struct pitch *c) { return c->reload ? c->reload : 65536; }

static u16 pit_count(int n)
{
    struct pitch *c = &pit[n];
    u32 rl = reload_of(c), e = pit_clock() - c->start;
    if (n == 2 && !(port61 & 1)) return (u16)rl;
    u32 v;
    if ((c->mode & 3) == 3) { v = rl - ((e * 2) % rl); v &= ~1u; }
    else v = rl - (e % rl);
    return (u16)v;
}

static int pit_out2(void)
{
    struct pitch *c = &pit[2];
    u32 rl = reload_of(c), e = pit_clock() - c->start;
    if (!(port61 & 1)) return 1;
    if (c->mode == 0) return e >= rl;
    if ((c->mode & 3) == 3) return (e % rl) < rl / 2;
    return (e % rl) != rl - 1;
}

static void pit_write(int n, u8 v)
{
    struct pitch *c = &pit[n];
    int done = 0;
    switch (c->access) {
    case 1: c->reload = v; done = 1; break;
    case 2: c->reload = (u32)v << 8; done = 1; break;
    default:
        if (!c->wflip) { c->wlo = v; c->wflip = 1; }
        else { c->reload = c->wlo | (u32)v << 8; c->wflip = 0; done = 1; }
    }
    if (done) {
        c->start = pit_clock();
        if (n == 0) {
            next_irq0 = c->start + reload_of(c);
            static u32 last_rl;
            if (reload_of(c) != last_rl) { last_rl = reload_of(c); dbg(2, "guest PIT ch0: reload %u (%u Hz), mode %u\n", last_rl, 1193182u / last_rl, c->mode); }
            if (reload_of(c) == 65536) video_vgalog_giveup();          /* vgalog: Windows putting the timer back as it quits */
        }
    }
}

static u8 pit_read(int n)
{
    struct pitch *c = &pit[n];
    u16 v;
    if (c->latched) v = c->latch; else v = pit_count(n);
    u8 r;
    switch (c->access) {
    case 1: r = (u8)v; c->latched = 0; break;
    case 2: r = v >> 8; c->latched = 0; break;
    default:
        if (!c->rflip) { r = (u8)v; c->rflip = 1; }
        else { r = v >> 8; c->rflip = 0; c->latched = 0; }
    }
    return r;
}

static void pit_control(u8 v)
{
    int n = v >> 6;
    if (n == 3) return;                       /* read-back: not supported */
    struct pitch *c = &pit[n];
    if (((v >> 4) & 3) == 0) {
        if (!c->latched) { c->latch = pit_count(n); c->latched = 1; c->rflip = 0; }
        return;
    }
    c->access = (v >> 4) & 3;
    c->mode = (v >> 1) & 7;
    c->rflip = c->wflip = 0;
    c->latched = 0;
}

void vdev_tick(void)
{
    u32 now = pit_clock(), per = reload_of(&pit[0]);
    if ((int32_t)(now - next_irq0) >= 0) {
        extern u32 stat_irq0;
        stat_irq0++;
        vpic_raise(0);
        next_irq0 += per;
        if ((int32_t)(now - next_irq0) >= 0)        /* fell behind: drop the backlog */
            next_irq0 = now + per;
    }
}

/* PC speaker: channel 2 as a square wave while ports 61h bits 0-1 are set. */
void speaker_mix(int32_t *lr, int frames)
{
    static u32 phase;
    if ((port61 & 3) != 3) return;
    u32 rl = reload_of(&pit[2]);
    if (rl < 20) return;                           /* above hearing */
    u32 step = (PIT_HZ / rl) * 4096 / 3000;          /* cycles per frame, 16.16 */
    for (int i = 0; i < frames; i++) {
        phase += step;
        int32_t v = (phase & 0x8000) ? 5000 : -5000;
        lr[2 * i] += v;
        lr[2 * i + 1] += v;
    }
}

/* ---------------- keyboard controller ---------------- */
static u8 kq[64];
static u8 kq_head, kq_tail;
static u8 kbd_obuf, kbd_full, kbd_cmd;
static u8 kbd_aux;                    /* the byte in the output buffer is from the mouse */
static u8 ccb = 0x47;                 /* command byte: kbd + aux IRQs, system flag, translate */

/* The PS/2 mouse port (aux device), for programs with their own mouse code:
   commands through D4h, 3-byte packets on IRQ 12 made from the real mouse's
   motion (the INT 33h driver in mouse.c gets the same motion). */
static u8 mq[64];
static u8 mq_head, mq_tail;
static u8 aux_stream, aux_param;      /* reporting on; command waiting for its parameter */
static int aux_dx, aux_dy, aux_b, aux_moved;
static u32 aux_since;                 /* when the unread mouse byte arrived */

u32 vaux_drops;                       /* bios.c's packet assembly starts over after one */
static u8 mq_cont[64];                /* 1: the 2nd or 3rd byte of a movement packet */
static void mq_put_t(u8 v, u8 cont)
{
    u8 n = (mq_tail + 1) % sizeof mq;
    if (n == mq_head) return;
    mq[mq_tail] = v;
    mq_cont[mq_tail] = cont;
    mq_tail = n;
}
static void mq_put(u8 v) { mq_put_t(v, 0); }

/* From mouse_input(): motion in counts (dy positive = down) and buttons. */
void vaux_motion(int dx, int dy, int b)
{
    if (!aux_stream) return;
    aux_dx += dx; aux_dy += dy; aux_b = b & 7;
    aux_moved = 1;
}

static void aux_packet(void)
{
    int dx = aux_dx, dy = -aux_dy;                  /* PS/2: y up */
    if (dx > 255) dx = 255;
    if (dx < -256) dx = -256;
    if (dy > 255) dy = 255;
    if (dy < -256) dy = -256;
    aux_dx -= dx; aux_dy += dy;
    mq_put((u8)(0x08 | aux_b | (dx < 0 ? 0x10 : 0) | (dy < 0 ? 0x20 : 0)));
    mq_put_t((u8)dx, 1);
    mq_put_t((u8)dy, 1);
    if (!aux_dx && !aux_dy) aux_moved = 0;
}

static void aux_data(u8 v)
{
    if (mouse_log()) kprintf("mouse: PS/2 mouse port command %02x%s\n", v, aux_param ? " (parameter)" : "");
    if (aux_param) { aux_param = 0; mq_put(0xFA); return; }    /* sample rate / resolution value */
    mq_put(0xFA);
    switch (v) {
    case 0xFF: aux_stream = 0; mq_put(0xAA); mq_put(0x00); break;      /* reset: self-test passed, ID 0 */
    case 0xF6: case 0xF5: aux_stream = 0; break;                         /* defaults / disable */
    case 0xF4: aux_stream = 1; aux_dx = aux_dy = 0; break;               /* enable reporting */
    case 0xF3: case 0xE8: aux_param = 1; break;                          /* rate / resolution follow */
    case 0xF2: mq_put(0x00); break;                                      /* ID: standard mouse */
    case 0xE9: mq_put((u8)(aux_stream ? 0x20 : 0)); mq_put(2); mq_put(100); break;   /* status */
    case 0xEB: aux_packet(); break;                                      /* read data */
    }
}

/* INT 15h C2xx (BIOS PS/2 mouse services, bios.c) drive the same port, the
   way a BIOS talks to its 8042; bytes are taken from the output buffer by
   the BIOS IRQ 12 handler (trap 74h). */
void vaux_bios_enable(int on)
{
    aux_stream = (u8)on;
    aux_dx = aux_dy = 0; aux_moved = 0;
    if (on) { ccb |= 0x02; ccb &= ~0x20; ps.imr &= ~0x10; pm.imr &= ~0x04; }
}

void vaux_bios_reset(void)
{
    aux_stream = 0; aux_param = 0;
    mq_head = mq_tail = 0;
    if (kbd_full && kbd_aux) kbd_full = 0;
}

int vaux_bios_byte(void)
{
    if (!kbd_full || !kbd_aux) return -1;
    return vkbd_read_data();
}

static void kq_put(u8 v)
{
    u8 n = (kq_tail + 1) % sizeof kq;
    if (n == kq_head) return;
    kq[kq_tail] = v;
    kq_tail = n;
}

/* ---------------- slowdown, like MoSlo ----------------
   Old games time themselves by the CPU. After each 1 ms timer tick the
   monitor waits, so the guest runs only a slice of every millisecond:
   speed= on the command line (percent of full speed, to 0.01%: speed=5,
   speed=0.3, speed=0.05), Ctrl+Shift+F11 slower and Ctrl+Shift+F12 faster,
   or VMSPEED.COM (INT 2Fh AX=5653h; BX = permille, 0 to ask, AX = permille
   back; with CX = 'HP', BX and AX in hundredths of a percent). */
static const u16 speed_steps[] = { 1, 2, 3, 5, 7, 10, 15, 20, 30, 50, 70, 100, 150, 200, 300, 500, 700,
                                   1000, 1500, 2000, 3000, 5000, 7000, 10000 };   /* hundredths of a percent */
#define N_SPEED (sizeof speed_steps / sizeof speed_steps[0])
#define SPEED_FULL 10000u
static u32 speed_u = SPEED_FULL;             /* hundredths of a percent */

static void speed_set(u32 u, const char *why)
{
    if (u < 1) u = 1;
    if (u > SPEED_FULL) u = SPEED_FULL;
    speed_u = u;
    u32 ip = u / 100, fr = u % 100;
    kprintf("speed: %u.%u%u%% of full (%s)\n", ip, fr / 10, fr % 10, why);
    char msg[32] = "Speed ";
    int n = 6;
    if (ip >= 100) msg[n++] = '0' + ip / 100;
    if (ip >= 10) msg[n++] = '0' + ip / 10 % 10;
    msg[n++] = '0' + ip % 10;
    if (fr) {
        msg[n++] = '.';
        msg[n++] = '0' + fr / 10;
        if (fr % 10) msg[n++] = '0' + fr % 10;
    }
    msg[n++] = '%'; msg[n] = 0;
    video_osd(msg);
}

/* INT 2Fh AX=5653h: BX = speed to set (0: just ask); AX = current. In
   permille, or with fine set (CX = 'HP') in hundredths of a percent. */
u32 speed_api(u32 v, int fine)
{
    if (v) speed_set(fine ? v : v * 10, "VMSPEED");
    if (fine) return speed_u;
    return speed_u >= 10 ? speed_u / 10 : 1;
}

void speed_init(void)
{
    char *p = strstr(cmdline, "speed=");
    if (!p) return;
    u32 v = 0, frac = 0;
    for (p += 6; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    if (*p == '.' && p[1] >= '0' && p[1] <= '9') {
        frac = (p[1] - '0') * 10;
        if (p[2] >= '0' && p[2] <= '9') frac += p[2] - '0';
    }
    speed_set(v * 100 + frac, "command line");
}

static void speed_step(int dir)
{
    unsigned i = 0;
    while (i < N_SPEED - 1 && speed_steps[i] < speed_u) i++;    /* nearest step at or above */
    if (dir > 0 && speed_steps[i] == speed_u && i < N_SPEED - 1) i++;
    else if (dir < 0 && i > 0) i--;
    speed_set(speed_steps[i], dir > 0 ? "faster" : "slower");
}

/* The TSC, for waits finer than the PIT's 0.84 us: at 0.1% the guest's
   share of a tick is under a microsecond, at 0.01% a tenth of one. Measured against the PIT the
   first time it's needed; without one (a 386 or 486), the PIT. */
static int tsc_state;                        /* 0: not yet measured, 1: tsc_per_k set, -1: none */
static u32 tsc_per_k;                        /* TSC counts per 1024 PIT clocks */

static u64 rdtsc64(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (u64)hi << 32 | lo;
}

static void tsc_measure(void)                /* interrupts off, just after a tick */
{
    u32 a, b;
    tsc_state = -1;
    __asm__ volatile("pushfl; pop %0; mov %0,%1; xor $0x200000,%0; push %0; popfl; pushfl; pop %0; push %1; popfl"
                     : "=&r"(a), "=&r"(b));
    if (!((a ^ b) & 0x200000)) return;       /* no CPUID */
    u32 eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    if (!(edx & (1u << 4))) return;          /* no TSC */
    u64 t0 = rdtsc64(), t1;
    u32 p0 = pit_clock(), p1;
    do { t1 = rdtsc64(); p1 = pit_clock(); } while (p1 - p0 < 600);   /* half a tick: one wrap at most */
    u32 d = (u32)(t1 - t0), n = p1 - p0;   /* no 64-bit division here: no libgcc */
    tsc_per_k = d / n * 1024 + d % n * 1024 / n;
    tsc_state = tsc_per_k ? 1 : -1;
    kprintf("speed: TSC %u kHz\n", (u32)((u64)tsc_per_k * (PIT_HZ / 1000) >> 10));
}

/* Called at the very end of a timer tick that interrupted the guest. The
   guest gets its share of every 1 ms tick, at the end of it: the monitor
   waits (interrupts on, so keyboard and sound go on) until that share
   before the next tick, then lets it run until the tick takes it back.
   So even at 0.01% it runs a little every millisecond, not in bursts:
   a millisecond of a modern CPU is seconds of an 8088's work. */
void speed_throttle(void)
{
    if (speed_u >= SPEED_FULL) return;
    if (!tsc_state) tsc_measure();
    u32 next = (ticks + 1) * PIT_PER_TICK;   /* the next tick, on pit_clock's scale */
    u64 t0 = tsc_state > 0 ? rdtsc64() : 0;
    u32 now = pit_clock();
    int32_t left = (int32_t)(next - now);    /* PIT clocks to the next tick */
    if (left <= 0 || left > (int32_t)PIT_PER_TICK) return;   /* already late: the tick's own work took it */
    sti();
    if (tsc_state > 0) {
        u32 tick = (u32)((u64)tsc_per_k * PIT_PER_TICK >> 10);
        u32 run = tick / SPEED_FULL * speed_u + tick % SPEED_FULL * speed_u / SPEED_FULL;   /* the guest's share, in TSC counts */
        u32 gap = (u32)((u64)(u32)left * tsc_per_k >> 10);
        u64 end = t0 + gap - (run < gap ? run : gap);
        while (rdtsc64() < end) __asm__ volatile("pause");
    } else {
        u32 target = next - PIT_PER_TICK * speed_u / SPEED_FULL;
        while ((int32_t)(pit_clock() - target) < 0) __asm__ volatile("pause");
    }
    __asm__ volatile("cli");
}

/* Real keyboard bytes (PS/2 IRQ and USB): Ctrl+Shift+F11/F12/F2 stay here. */
/* For the "vmdos stopped" screen: the next queued keyboard byte, or -1. */
int vkbd_take(void)
{
    if (kq_head == kq_tail) return -1;
    u8 v = kq[kq_head];
    kq_head = (kq_head + 1) % sizeof kq;
    return v;
}

static int ctrl, alt, lshift, rshift, e0;               /* the real keyboard's modifiers */

/* After the log viewer had the keyboard: no modifier is down any more,
   for us or the guest (it saw them go down, not up). */
/* The modifiers the guest has seen go down (bit 0 Ctrl, 1 left Shift,
   2 right Shift, 3 Alt). Taken when the Ctrl+Shift+F10 log opens: the
   releases come while the log has the keyboard (and a USB keyboard's go
   through here then too, clearing these), so afterwards the guest is
   given the releases of what was down when it lost the keyboard. */
int vkbd_mods(void) { return (ctrl ? 1 : 0) | (lshift ? 2 : 0) | (rshift ? 4 : 0) | (alt ? 8 : 0); }
void vkbd_release_mods(int m)
{
    if (m & 1) kq_put(0x9D);
    if (m & 2) kq_put(0xAA);
    if (m & 4) kq_put(0xB6);
    if (m & 8) kq_put(0xB8);
    ctrl = alt = lshift = rshift = e0 = 0;
}

void vkbd_inject(u8 sc) { kq_put(sc); }       /* hv.c: keys typed for the guest */

void vkbd_real_scancode(u8 sc)
{
    u8 k = sc & 0x7F;
    int up = sc & 0x80;
    if (sc == 0xE0) e0 = 1;
    else {
        if (k == 0x1D) ctrl = !up;
        else if (k == 0x38) alt = !up;
        else if (k == 0x2A && !e0) lshift = !up;               /* E0 2A: a fake shift around grey keys */
        else if (k == 0x36 && !e0) rshift = !up;
        e0 = 0;
    }
    if (hv_active && ctrl && alt && k == 0x4F) {               /* Ctrl+Alt+End: leave VMHV (hv.c) */
        if (!up) hv_hotkey();
        return;
    }
    int hot = ctrl && (lshift || rshift);                     /* Ctrl+Shift: games rarely use it with F-keys */
    if (hot && (k == 0x57 || k == 0x58)) {
        if (!up) speed_step(k == 0x58 ? 1 : -1);
        return;
    }
    if (hot && k == 0x44) {                                   /* Ctrl+Shift+F10: the log, over the game */
        if (!up) log_view_req = 1;
        return;
    }
    if (hot && video_log_on() && k == 0x1F) {                /* Ctrl+Shift+S: the log into C:\VMDOS\VMDOS.LOG */
        extern volatile int log_save_req;
        if (!up) log_save_req = 1;
        return;
    }
    if (hot && video_log_on() && (k == 0x48 || k == 0x50 || k == 0x49 || k == 0x51 || k == 0x47 || k == 0x4F)) {
        if (!up) {                                            /* Ctrl+Shift+arrows/PgUp/PgDn/Home/End: scroll it */
            int video_log_page(void);
            int pg = video_log_page();
            video_log_scroll(k == 0x48 ? 1 : k == 0x50 ? -1 : k == 0x49 ? pg : k == 0x51 ? -pg : k == 0x47 ? 100000 : -100000);
        }
        return;
    }
    if (hot && k >= 0x3D && k <= 0x3F) {                     /* Ctrl+Shift+F3/F4: volume down/up, F5: mute */
        void sound_volume_key(int k);
        if (!up) sound_volume_key(k == 0x3D ? -1 : k == 0x3E ? 1 : 0);
        return;
    }
    if (hot && k == 0x42) {                                   /* Ctrl+Shift+F8: sharp, smooth, xBRZ, no scaling (video.c) */
        extern volatile int smooth_toggle_req;
        if (!up) smooth_toggle_req = 1;
        return;
    }
    if (hot && k == 0x43) {                                   /* Ctrl+Shift+F9: screenshot (video.c) */
        extern volatile int shot_req;
        if (!up) shot_req = 1;
        return;
    }
    if (hot && k == 0x3C) {                                   /* Ctrl+Shift+F2: next sound output */
        void sound_next_output(void);
        if (!up) sound_next_output();
        return;
    }
    kq_put(sc);
}

void vkbd_refill(void)
{
    if (kbd_full && kbd_aux && ticks - aux_since > 250) {    /* nobody reads the mouse: don't block the keyboard */
        /* Its request goes too (else the next byte, maybe a key, is taken
           as this one's), and the rest of its packet: a byte short, every
           packet after it would be read a byte out of step (the pointer
           wandering, buttons pressed by movement bits). */
        kbd_full = 0;
        ps.irr &= ~(1 << 4);
        vaux_drops++;
        while (mq_head != mq_tail && mq_cont[mq_head]) mq_head = (mq_head + 1) % sizeof mq;
        MLOG("mouse: a byte nobody read in 250 ms dropped, with the rest of its packet\n");
    }
    if (kbd_full) return;
    if (aux_moved && mq_head == mq_tail && !(ccb & 0x20)) aux_packet();
    /* A real PS/2 mouse sends a byte about every millisecond (11 bits at
       10-16 kHz). Handed over the moment the last one is acknowledged, the
       next byte's IRQ 12 hit LSL7's handler right at its STI before IRETD,
       every byte: handlers nested until "DPMI: nesting too deep", and the
       sound stuttered meanwhile. */
    static u32 aux_last;
    if (mq_head != mq_tail && !vpic_in_service(12) && (u32)(pit_clock() - aux_last) >= 1193) {
        aux_last = pit_clock();
        kbd_obuf = mq[mq_head];
        mq_head = (mq_head + 1) % sizeof mq;
        kbd_full = 1; kbd_aux = 1;
        aux_since = ticks;
        MLOG("mouse: byte %02x to the 8042, %s (ccb %02x, slave mask %02x)\n", kbd_obuf, ccb & 2 ? "IRQ 12" : "no IRQ (ccb bit 1 off)", ccb, ps.imr);
        if (ccb & 2) vpic_raise(12);
        return;
    }
    if (kq_head == kq_tail || vpic_in_service(1)) return;
    kbd_obuf = kq[kq_head];
    kq_head = (kq_head + 1) % sizeof kq;
    kbd_full = 1; kbd_aux = 0;
    vpic_raise(1);
}

int vkbd_read_data(void)
{
    /* reading the byte takes back its interrupt request, as the 8042's
       line drops (a program polling with IRQs masked doesn't see it twice) */
    if (kbd_full) { if (kbd_aux) ps.irr &= ~(1 << 4); else pm.irr &= ~(1 << 1); }
    kbd_full = 0;
    return kbd_obuf;
}

static void kbd_command(u8 v)
{
    switch (v) {
    case 0xD1: case 0x60: case 0xD3: case 0xD4: kbd_cmd = v; break;
    case 0xDD: set_a20(0); break;
    case 0xDF: set_a20(1); break;
    case 0x20: kbd_obuf = ccb; kbd_full = 1; kbd_aux = 0; break;
    case 0xA7: ccb |= 0x20; MLOG("mouse: 8042 A7h: mouse port off\n"); break;   /* mouse port off */
    case 0xA8: ccb &= ~0x20; break;                 /* mouse port on */
    case 0xA9: kbd_obuf = 0x00; kbd_full = 1; kbd_aux = 0; break;   /* mouse port test: OK */
    case 0xD0: kbd_obuf = 0x01 | (a20_on ? 2 : 0); kbd_full = 1; kbd_aux = 0; break;
    case 0xAA: kbd_obuf = 0x55; kbd_full = 1; kbd_aux = 0; break;
    case 0xAB: kbd_obuf = 0x00; kbd_full = 1; kbd_aux = 0; break;
    case 0xFE: reboot();
    }
}

static void kbd_data(u8 v)
{
    if (kbd_cmd == 0xD1) { set_a20((v & 2) != 0); kbd_cmd = 0; return; }
    if (kbd_cmd == 0x60) { if (v != ccb) MLOG("mouse: 8042 command byte %02x -> %02x\n", ccb, v); ccb = v; kbd_cmd = 0; return; }
    if (kbd_cmd == 0xD4) { kbd_cmd = 0; aux_data(v); return; }     /* to the mouse */
    if (kbd_cmd == 0xD3) { kbd_cmd = 0; mq_put(v); return; }       /* as if from the mouse */
    if (v == 0xEE) kq_put(0xEE);
    else if (v == 0xFF) { kq_put(0xFA); kq_put(0xAA); }
    else if (v == 0xF2) { kq_put(0xFA); kq_put(0xAB); kq_put(0x41); }
    else kq_put(0xFA);
}

/* ---------------- dispatch ---------------- */
static u8 cmos_index;

static u8 in8(u16 port)
{
    switch (port) {
    case 0x20: case 0x21: return pic_in(&pm, port & 1);
    case 0xA0: case 0xA1: return pic_in(&ps, port & 1);
    case 0x40: case 0x41: case 0x42: return pit_read(port - 0x40);
    case 0x43: return 0xFF;
    case 0x60: {
        int aux = kbd_full && kbd_aux;
        u8 b = (u8)vkbd_read_data();
        if (aux) {
            u16 cs = 0; u32 eip = 0;
            int pm = hv_guest_pm(&cs, &eip);
            MLOG("mouse: guest reads the mouse byte %02x from port 60h (%s %04x:%08x)\n", b, pm ? "protected mode" : "real/v86", cs, eip);
        }
        return b; }
    case 0x61: {
        u8 v = port61 & 0x0F;
        if ((pit_clock() / 18) & 1) v |= 0x10;
        if (pit_out2()) v |= 0x20;
        return v; }
    case 0x64: return (kbd_full ? 1 : 0) | (kbd_full && kbd_aux ? 0x20 : 0) | 0x14;
    case 0x70: return cmos_index;
    case 0x71: {
        u32 kb = hv_ext_kb();                       /* VMHV: the VM's extended memory, not the PC's */
        u8 ix = cmos_index & 0x7F;
        u32 k16 = kb > 0xFFFF ? 0xFFFF : kb, b64 = kb > 15360 ? (kb - 15360) / 64 : 0;   /* KB above 1 MB; 64 KB blocks above 16 MB */
        if (kb && (ix == 0x17 || ix == 0x30)) return (u8)k16;
        if (kb && (ix == 0x18 || ix == 0x31)) return (u8)(k16 >> 8);
        if (kb && ix == 0x34) return (u8)b64;
        if (kb && ix == 0x35) return (u8)(b64 >> 8);
        outb(0x70, ix); return inb(0x71); }
    case 0x92: return a20_on ? 2 : 0;
    case 0x201: return joy_port_in();                    /* game port (joy.c) */
    }
    u8 v;
    if (serial_port(port, 0, &v)) return v;             /* serial=dos: the real UART (serial.c) */
    if (sound_port(port, 0, &v)) return v;
    if (port >= 0x3B0 && port <= 0x3DF) return (u8)video_port_in(port);
    dbg(2, "guest in  %04x\n", port);
    return 0xFF;
}

static void out8(u16 port, u8 v)
{
    switch (port) {
    case 0x20: case 0x21: pic_out(&pm, port & 1, v); return;
    case 0xA0: case 0xA1: pic_out(&ps, port & 1, v); return;
    case 0x40: case 0x41: case 0x42: pit_write(port - 0x40, v); return;
    case 0x43: pit_control(v); return;
    case 0x60: kbd_data(v); return;
    case 0x61:
        if ((v & 1) && !(port61 & 1)) pit[2].start = pit_clock();
        port61 = v; return;
    case 0x64: kbd_command(v); return;
    case 0x70: cmos_index = v; return;
    case 0x71: return;                          /* the real CMOS stays untouched */
    case 0x92: set_a20((v & 2) != 0); return;
    case 0x80: case 0xED: return;               /* POST / delay ports */
    case 0x201: joy_port_out(); return;
    }
    if (serial_port(port, 1, &v)) return;
    if (sound_port(port, 1, &v)) return;
    if (port >= 0x3B0 && port <= 0x3DF) { video_port_out(port, v); return; }
    dbg(2, "guest out %04x <- %02x\n", port, v);
}

u32 perf_ports[1024];

u32 port_in(u16 port, int size)
{
    u32 v = 0;
    if (perf_on) perf_ports[port & 1023]++;
    if (vpci_port(port, size, 0, &v)) return v;           /* PCI configuration, the Voodoo 3's I/O (vpci.c) */
    if (ne2k_port(port, size, 0, &v)) return v;
    for (int i = 0; i < size; i++) v |= (u32)in8(port + i) << (8 * i);
    return v;
}

void port_out(u16 port, u32 val, int size)
{
    if (perf_on) perf_ports[port & 1023]++;
    if (vpci_port(port, size, 1, &val)) return;
    if (ne2k_port(port, size, 1, &val)) return;
    for (int i = 0; i < size; i++) out8(port + i, val >> (8 * i));
}

void vdev_init(void)
{
    for (int i = 0; i < 3; i++) { pit[i].access = 3; pit[i].mode = 3; pit[i].start = pit_clock(); }
    pit[0].mode = 3;
    next_irq0 = pit_clock() + 65536;
}

void vpic_debug(char *buf, int n)
{
    snprintf(buf, n, "PIC irr %02x/%02x isr %02x/%02x imr %02x/%02x", pm.irr, ps.irr, pm.isr, ps.isr, pm.imr, ps.imr);
}
