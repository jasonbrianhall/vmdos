/* Virtual hardware the DOS guest sees through trapped I/O ports:
   8259 PICs, 8254 PIT, 8042 keyboard controller, port 61h, A20, CMOS. */
#include "kernel.h"

/* ---------------- 8259 ---------------- */
struct pic { u8 irr, isr, imr, base, init, icw4, read_isr; };
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
            dbg(1, "guest PIT ch0: reload %u (%u Hz)\n", reload_of(c), PIT_HZ / reload_of(c));
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

static void mq_put(u8 v)
{
    u8 n = (mq_tail + 1) % sizeof mq;
    if (n == mq_head) return;
    mq[mq_tail] = v;
    mq_tail = n;
}

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
    mq_put((u8)dx);
    mq_put((u8)dy);
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
   speed= on the command line (percent of full speed, e.g. speed=5 or
   speed=0.3), Ctrl+Shift+F11 slower and Ctrl+Shift+F12 faster, or
   VMSPEED.COM (INT 2Fh AX=5653h, BX = permille, 0 to ask). */
static const u16 speed_steps[] = { 1, 2, 3, 5, 7, 10, 15, 20, 30, 50, 70, 100, 150, 200, 300, 500, 700, 1000 };   /* permille */
#define N_SPEED (sizeof speed_steps / sizeof speed_steps[0])
static u32 speed_pm = 1000;

static void speed_set(u32 pm, const char *why)
{
    if (pm < 1) pm = 1;
    if (pm > 1000) pm = 1000;
    speed_pm = pm;
    kprintf("speed: %u.%u%% of full (%s)\n", pm / 10, pm % 10, why);
    char msg[32] = "Speed ";
    int n = 6;
    if (pm / 10 >= 100) msg[n++] = '0' + pm / 1000;
    if (pm / 10 >= 10) msg[n++] = '0' + pm / 100 % 10;
    msg[n++] = '0' + pm / 10 % 10;
    if (pm % 10) { msg[n++] = '.'; msg[n++] = '0' + pm % 10; }
    msg[n++] = '%'; msg[n] = 0;
    video_osd(msg);
}

/* INT 2Fh AX=5653h: BX = permille to set (0: just ask); AX = current. */
u32 speed_api(u32 pm)
{
    if (pm) speed_set(pm, "VMSPEED");
    return speed_pm;
}

void speed_init(void)
{
    char *p = strstr(cmdline, "speed=");
    if (!p) return;
    u32 v = 0, frac = 0;
    for (p += 6; *p >= '0' && *p <= '9'; p++) v = v * 10 + (*p - '0');
    if (*p == '.' && p[1] >= '0' && p[1] <= '9') frac = p[1] - '0';
    speed_set(v * 10 + frac, "command line");
}

static void speed_step(int dir)
{
    unsigned i = 0;
    while (i < N_SPEED - 1 && speed_steps[i] < speed_pm) i++;    /* nearest step at or above */
    if (dir > 0 && speed_steps[i] == speed_pm && i < N_SPEED - 1) i++;
    else if (dir < 0 && i > 0) i--;
    speed_set(speed_steps[i], dir > 0 ? "faster" : "slower");
}

/* Called at the end of a timer tick that interrupted the guest: wait in
   proportion to how long the guest has run since the last wait (so the
   share is right however regularly ticks arrive), with interrupts on so
   the monitor's clock, keyboard and sound go on meanwhile. */
void speed_throttle(void)
{
    static u32 last;
    u32 now = pit_clock();
    if (speed_pm >= 1000) { last = now; return; }
    u32 ran = now - last;
    if (ran > 1193 * 20) ran = 1193 * 20;
    u32 wait = ran * (1000 - speed_pm) / speed_pm;
    if (wait > 1193 * 200) wait = 1193 * 200;
    sti();
    while (pit_clock() - now < wait) __asm__ volatile("pause");
    __asm__ volatile("cli");
    last = pit_clock();
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

void vkbd_real_scancode(u8 sc)
{
    static int ctrl, alt, lshift, rshift, e0;
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
    (void)alt;
    int hot = ctrl && (lshift || rshift);                     /* Ctrl+Shift: games rarely use it with F-keys */
    if (hot && (k == 0x57 || k == 0x58)) {
        if (!up) speed_step(k == 0x58 ? 1 : -1);
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
    if (kbd_full && kbd_aux && ticks - aux_since > 250) kbd_full = 0;   /* nobody reads the mouse: don't block the keyboard */
    if (kbd_full) return;
    if (aux_moved && mq_head == mq_tail && !(ccb & 0x20)) aux_packet();
    if (mq_head != mq_tail && !vpic_in_service(12)) {
        kbd_obuf = mq[mq_head];
        mq_head = (mq_head + 1) % sizeof mq;
        kbd_full = 1; kbd_aux = 1;
        aux_since = ticks;
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
    case 0xA7: ccb |= 0x20; break;                  /* mouse port off */
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
    if (kbd_cmd == 0x60) { ccb = v; kbd_cmd = 0; return; }
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
    case 0x60: return (u8)vkbd_read_data();
    case 0x61: {
        u8 v = port61 & 0x0F;
        if ((pit_clock() / 18) & 1) v |= 0x10;
        if (pit_out2()) v |= 0x20;
        return v; }
    case 0x64: return (kbd_full ? 1 : 0) | (kbd_full && kbd_aux ? 0x20 : 0) | 0x14;
    case 0x70: return cmos_index;
    case 0x71: outb(0x70, cmos_index & 0x7F); return inb(0x71);
    case 0x92: return a20_on ? 2 : 0;
    }
    u8 v;
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
    }
    if (sound_port(port, 1, &v)) return;
    if (port >= 0x3B0 && port <= 0x3DF) { video_port_out(port, v); return; }
    dbg(2, "guest out %04x <- %02x\n", port, v);
}

u32 port_in(u16 port, int size)
{
    u32 v = 0;
    for (int i = 0; i < size; i++) v |= (u32)in8(port + i) << (8 * i);
    return v;
}

void port_out(u16 port, u32 val, int size)
{
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
