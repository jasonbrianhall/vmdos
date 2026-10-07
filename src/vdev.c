/* Virtual hardware the DOS guest sees through trapped I/O ports:
   8259 PICs, 8254 PIT, 8042 keyboard controller, port 61h, A20, CMOS. */
#include "kernel.h"

/* ---------------- 8259 ---------------- */
struct pic { u8 irr, isr, imr, base, init, icw4, read_isr; };
static struct pic pm = { 0, 0, 0xF8, 0x08, 0, 0, 0 };
static struct pic ps = { 0, 0, 0xFF, 0x70, 0, 0, 0 };
static int chosen_irq = -1;

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

/* ---------------- keyboard controller ---------------- */
static u8 kq[64];
static u8 kq_head, kq_tail;
static u8 kbd_obuf, kbd_full, kbd_cmd;

static void kq_put(u8 v)
{
    u8 n = (kq_tail + 1) % sizeof kq;
    if (n == kq_head) return;
    kq[kq_tail] = v;
    kq_tail = n;
}

void vkbd_real_scancode(u8 sc) { kq_put(sc); }

void vkbd_refill(void)
{
    if (kbd_full || kq_head == kq_tail || vpic_in_service(1)) return;
    kbd_obuf = kq[kq_head];
    kq_head = (kq_head + 1) % sizeof kq;
    kbd_full = 1;
    vpic_raise(1);
}

int vkbd_read_data(void)
{
    kbd_full = 0;
    return kbd_obuf;
}

static void kbd_command(u8 v)
{
    switch (v) {
    case 0xD1: case 0x60: kbd_cmd = v; break;
    case 0xDD: set_a20(0); break;
    case 0xDF: set_a20(1); break;
    case 0x20: kbd_obuf = 0x45; kbd_full = 1; break;
    case 0xD0: kbd_obuf = 0x01 | (a20_on ? 2 : 0); kbd_full = 1; break;
    case 0xAA: kbd_obuf = 0x55; kbd_full = 1; break;
    case 0xAB: kbd_obuf = 0x00; kbd_full = 1; break;
    case 0xFE: reboot();
    }
}

static void kbd_data(u8 v)
{
    if (kbd_cmd == 0xD1) { set_a20((v & 2) != 0); kbd_cmd = 0; return; }
    if (kbd_cmd == 0x60) { kbd_cmd = 0; return; }
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
    case 0x64: return (kbd_full ? 1 : 0) | 0x14;
    case 0x70: return cmos_index;
    case 0x71: outb(0x70, cmos_index & 0x7F); return inb(0x71);
    case 0x92: return a20_on ? 2 : 0;
    }
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
