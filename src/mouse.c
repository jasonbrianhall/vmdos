/* The mouse: a PS/2 mouse on the real 8042's aux port and USB mice (usb.cpp)
   feed an INT 33h driver that lives in the monitor (no MOUSE.COM needed).
   The pointer is drawn by the renderer over the guest's screen, never into
   guest memory. A program's event handler (function 0Ch) is called through
   the mouse_cb stub in the BIOS segment. */
#include "kernel.h"

/* ---------------- state ---------------- */
static volatile int raw_dx, raw_dy, raw_buttons;    /* from the IRQ handlers */
static volatile int have_input;

static int mx, my;                    /* virtual screen coordinates */
static int fx, fy;                    /* sub-pixel remainders (x8) */
static int xmin, xmax, ymin, ymax;
static int buttons;
static int mick_x, mick_y;            /* mickey counters since the last fn 0Bh */
static int ratio_x = 8, ratio_y = 16; /* mickeys per 8 pixels */
static int show = -1;                 /* visible when 0 */
static int press_cnt[3], release_cnt[3];
static int press_x[3], press_y[3], release_x[3], release_y[3];
static u16 handler_mask, handler_off, handler_seg;
static u16 pending_cond;
static int in_callback;
static u16 text_and = 0xFFFF, text_xor = 0x7700;  /* fn 0Ah: screen / cursor masks */
static int present;

int mouse_present(void) { return present; }

/* A tablet (absolute pointer: QEMU's usb-tablet, virt-manager's default):
   where the host's pointer is, 0..32767 across the screen. vmdos's INT 33h
   pointer goes there; a driver or game on the PS/2 port (CuteMouse) gets
   the motion, as counts that take its default 640x200 pointer (8 and 16
   mickeys per 8 pixels) as far as the host's moved. */
static volatile int abs_x = -1, abs_y;
static int abs_px = -1, abs_py;
void mouse_absolute(int x, int y, int b)
{
    abs_x = x; abs_y = y;
    raw_buttons = b & 7;
    have_input = 1;
    int px = x * 640 / 32768, py = y * 400 / 32768;
    vaux_motion(abs_px < 0 ? 0 : px - abs_px, abs_px < 0 ? 0 : py - abs_py, b);
    abs_px = px; abs_py = py;
}

/* Called from interrupt handlers: motion in counts (dy positive = down). */
void mouse_input(int dx, int dy, int b)
{
    raw_dx += dx;
    raw_dy += dy;
    raw_buttons = b & 7;
    have_input = 1;
    vaux_motion(dx, dy, b);
}

/* ---------------- PS/2 mouse on the real 8042 ---------------- */
static int kbc_wait_write(void) { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return 1; return 0; }
static int kbc_wait_read(void) { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return 1; return 0; }
static void kbc_flush(void) { for (int i = 0; i < 64 && (inb(0x64) & 1); i++) { inb(0x60); kdelay_us(50); } }

/* A byte from the mouse within ms milliseconds, or -1. The keyboard port is
   off while the mouse is set up, so whatever comes is the mouse's (some
   laptop ECs don't set the aux bit, 20h, in the status). */
static int aux_read(int ms)
{
    for (int i = 0; i < ms * 10; i++) {
        if (inb(0x64) & 1) return inb(0x60);
        kdelay_us(100);
    }
    return -1;
}

/* A command to the mouse: its answer (FAh = ACK), retried on FEh (resend). */
static int aux_cmd(u8 c)
{
    int r = -1;
    for (int t = 0; t < 3; t++) {
        kbc_wait_write(); outb(0x64, 0xD4);
        kbc_wait_write(); outb(0x60, c);
        r = aux_read(50);
        if (r != 0xFE) break;
    }
    return r;
}

/* Reset (FFh): ACK, then AAh (self-test passed) and the ID. A touchpad takes
   a while (Synaptics: up to ~1 s). The reset is what puts a touchpad the
   firmware left in its own mode (HP's BIOS setup drives Synaptics and Elan
   pads in absolute mode: 6-byte packets that aren't PS/2 mouse packets)
   back to a plain 3-byte PS/2 mouse. */
static int aux_reset(void)
{
    int a = aux_cmd(0xFF);
    if (a != 0xFA) return a;
    int st = aux_read(1500);
    if (st == 0xAA) aux_read(100);                     /* the ID, 00h */
    return st;
}

void mouse_ps2_init(void)
{
    if (inb(0x64) == 0xFF) return;                     /* no 8042 */
    /* Both ports off while the mouse is set up, so its answers can't be
       mixed with keystrokes, and no IRQs from the 8042 meanwhile. */
    kbc_wait_write(); outb(0x64, 0xAD);                /* keyboard port off */
    kbc_wait_write(); outb(0x64, 0xA7);                /* aux port off */
    kbc_flush();                                       /* stale bytes */
    kbc_wait_write(); outb(0x64, 0x20);
    u8 cfg = 0x47;
    if (kbc_wait_read()) cfg = inb(0x60);
    else kprintf("kbd: 8042 command byte unreadable\n");
    if ((cfg & 0x51) != 0x41) kprintf("kbd: 8042 command byte %02x: keyboard IRQ, clock and translation on\n", cfg);
    u8 probe = (cfg | 0x40) & ~0x03;                   /* translation; no IRQs while probing */
    probe &= ~0x30;                                    /* both clocks on */
    kbc_wait_write(); outb(0x64, 0x60);
    kbc_wait_write(); outb(0x60, probe);

    kbc_wait_write(); outb(0x64, 0xA8);                /* aux port on */
    kdelay_us(1000);
    kbc_flush();
    int st = aux_reset();
    if (st == -1) { kdelay_us(100000); st = aux_reset(); }   /* a pad still waking up */
    int ok;
    if (st == 0xAA || st == 0xFC) {                    /* FCh: self-test failed; it may still work */
        if (st == 0xFC) kprintf("mouse: PS/2 device self-test failed, trying it anyway\n");
        ok = aux_cmd(0xF6) == 0xFA;                    /* defaults: 100/s, 4 counts/mm, 1:1 */
    } else {
        /* No reset answer: an EC that only passes some commands. The old
           way: set defaults alone. */
        if (mouse_log()) kprintf("mouse: reset answered %s%02x\n", st < 0 ? "nothing " : "", st & 0xFF);
        ok = aux_cmd(0xF6) == 0xFA;
    }
    int id = -1;
    if (ok && aux_cmd(0xF2) == 0xFA) id = aux_read(50);   /* ID: 00 = standard mouse */
    if (ok) ok = aux_cmd(0xF4) == 0xFA;                /* stream mode on */
    kbc_flush();

    cfg = probe | 0x01;                                /* keyboard IRQ 1 */
    if (ok) cfg |= 0x02;                               /* aux IRQ 12 */
    kbc_wait_write(); outb(0x64, 0x60);
    kbc_wait_write(); outb(0x60, cfg);
    /* The keyboard side too, whatever the firmware left: a legacy BIOS /
       CSM boot can hand over the 8042 with the keyboard port or its
       scanning off, and then nothing comes (a PS/2 keyboard seems dead;
       USB ones work). */
    kbc_wait_write(); outb(0x64, 0xAE);                /* keyboard port on */
    kbc_wait_write(); outb(0x60, 0xF4);                /* the keyboard: scanning on */
    if (kbc_wait_read()) inb(0x60);                    /* its ACK */
    if (!ok) {
        kbc_wait_write(); outb(0x64, 0xA7);            /* aux port off: no stray bytes */
        kprintf("mouse: no PS/2 mouse (reset: %s%02x)\n", st < 0 ? "no answer " : "", st & 0xFF);
        return;
    }
    outb(0xA1, inb(0xA1) & ~0x10);                     /* unmask IRQ 12 */
    present = 1;
    kprintf("mouse: PS/2 (id %02x)\n", id & 0xFF);
}

void mouse_ps2_byte(u8 b)
{
    static u8 pkt[3];
    static int n;
    if (n == 0 && (b & 0xC8) != 0x08) return;          /* byte 0: bit 3 set, no overflow (skips ACKs) */
    pkt[n++] = b;
    if (n < 3) return;
    n = 0;
    int dx = pkt[1] - ((pkt[0] << 4) & 0x100);
    int dy = pkt[2] - ((pkt[0] << 3) & 0x100);
    mouse_input(dx, -dy, pkt[0] & 7);
}

void mouse_usb_attached(void) { present = 1; }

/* ---------------- motion, buttons, events ---------------- */
static void clamp(void)
{
    if (mx < xmin) mx = xmin;
    if (mx > xmax) mx = xmax;
    if (my < ymin) my = ymin;
    if (my > ymax) my = ymax;
}

static int granular_x(void) { return (video_mode <= 1 || video_mode == 4 || video_mode == 5 || video_mode == 0x0D || video_mode == 0x13) ? 2 : 1; }

static void reset_ranges(void)
{
    xmin = ymin = 0;
    xmax = 639;
    ymax = video_gfx_height() - 1;
    mx = (xmax + 1) / 2; my = (ymax + 1) / 2;
}

/* Take what the IRQ handlers collected (called with interrupts off). */
void mouse_update(void)
{
    if (!have_input) return;
    int dx = raw_dx, dy = raw_dy, b = raw_buttons;
    raw_dx = raw_dy = 0;
    have_input = 0;
    u16 cond = 0;
    if (abs_x >= 0) {                                 /* a tablet: straight to the place */
        int tx = xmin + abs_x * (xmax - xmin + 1) / 32768, ty = ymin + abs_y * (ymax - ymin + 1) / 32768;
        abs_x = -1;
        tx &= ~(granular_x() - 1);
        if (tx != mx || ty != my) {
            mick_x += (tx - mx) * ratio_x / 8 / granular_x();
            mick_y += (ty - my) * ratio_y / 8;
            mx = tx; my = ty;
            clamp();
            cond |= 1;
        }
    }
    if (dx || dy) {
        mick_x += dx; mick_y += dy;
        fx += dx * 8; fy += dy * 8;                     /* pixels = mickeys * 8 / ratio */
        int px = fx / ratio_x, py = fy / ratio_y;
        fx -= px * ratio_x; fy -= py * ratio_y;
        int ox = mx, oy = my;
        mx += px * granular_x(); my += py;
        clamp();
        if (mx != ox || my != oy || dx || dy) cond |= 1;
    }
    for (int i = 0; i < 3; i++) {                     /* left, right, middle */
        int bit = 1 << i;
        if ((b & bit) && !(buttons & bit)) { press_cnt[i]++; press_x[i] = mx; press_y[i] = my; cond |= 2 << (2 * i); }
        if (!(b & bit) && (buttons & bit)) { release_cnt[i]++; release_x[i] = mx; release_y[i] = my; cond |= 4 << (2 * i); }
    }
    buttons = b;
    pending_cond |= cond;
}

static int report_x(void) { return video_mode <= 3 || video_mode == 7 ? mx & ~(8 * granular_x() - 1) : mx & ~(granular_x() - 1); }
static int report_y(void) { return video_mode <= 3 || video_mode == 7 ? my & ~7 : my; }

/* For the renderer: where the pointer is, if it's shown. */
int mouse_pointer(int *x, int *y, u16 *and_mask, u16 *xor_mask)
{
    if (!present || show < 0) return 0;
    *x = mx; *y = my;
    *and_mask = text_and; *xor_mask = text_xor;
    return 1;
}

/* ---------------- the program's event handler ---------------- */
#define MOUSE_CB_PTR 0xF0208u     /* bios.asm: offset of mouse_cb */
#define MOUSE_HANDLER 0xF020Au    /* bios.asm: dd the handler */

int mouse_callback_due(void)
{
    mouse_update();
    return !in_callback && handler_seg && (pending_cond & handler_mask);
}

void mouse_start_callback(struct regs *r)
{
    static int n;
    if (mouse_log() && n++ < 10) kprintf("mouse: event handler %04x:%04x called, events %x\n", handler_seg, handler_off, pending_cond & handler_mask);
    in_callback = 1;
    v86_push16(r, (u16)((r->eflags & 0x0DD5) | 2 | (vif ? EFL_IF : 0)));
    v86_push16(r, (u16)r->cs);
    v86_push16(r, IP(r));
    vif = 0;
    r->cs = 0xF000;
    r->eip = rd16(MOUSE_CB_PTR);
}

/* For a DPMI client in protected mode: the callback runs on a real-mode
   excursion (dpmi.c); returns the stub's offset in F000h. */
u16 mouse_begin_callback(void)
{
    static int n;
    if (mouse_log() && n++ < 10) kprintf("mouse: event handler called (protected-mode program), events %x\n", pending_cond & handler_mask);
    in_callback = 1;
    return rd16(MOUSE_CB_PTR);
}

/* TRAP 0x34 in mouse_cb: load the event registers. */
void mouse_cb_regs(struct regs *r)
{
    AX(r) = pending_cond & handler_mask;
    pending_cond = 0;
    BX(r) = (u16)buttons;
    CX(r) = (u16)report_x();
    DX(r) = (u16)report_y();
    SI(r) = (u16)mick_x;
    DI(r) = (u16)mick_y;
    wr16(MOUSE_HANDLER, handler_off);
    wr16(MOUSE_HANDLER + 2, handler_seg);
}

void mouse_cb_done(void) { in_callback = 0; }

/* ---------------- INT 33h ---------------- */
static void soft_reset(void)
{
    show = -1;
    reset_ranges();
    ratio_x = 8; ratio_y = 16;
    handler_mask = 0; handler_seg = handler_off = 0;
    mick_x = mick_y = 0;
    text_and = 0xFFFF; text_xor = 0x7700;
    for (int i = 0; i < 3; i++) press_cnt[i] = release_cnt[i] = 0;
    pending_cond = 0;
}

void mouse_restart(void) { soft_reset(); in_callback = 0; }

/* "mouselog" on the command line: what a program asks of the mouse. */
static int mlog = -1;
int mouse_log(void) { if (mlog < 0) mlog = !!strstr(cmdline, "mouselog"); return mlog; }

void mouse_int33(struct regs *r)
{
    video_native_sync();                               /* video=vbe: the card's mode, for the ranges */
    mouse_update();
    if (mouse_log()) {
        static u32 polls;
        int poll = AX(r) == 3 || AX(r) == 0x0B || AX(r) == 5 || AX(r) == 6;
        if (!poll || polls++ < 8)
            kprintf("mouse: INT 33h AX=%04x BX=%04x CX=%04x DX=%04x ES=%04x%s\n", AX(r), BX(r), CX(r), DX(r),
                    r->v86_es & 0xFFFF, poll && polls == 8 ? " (no more polls logged)" : "");
    }
    switch (AX(r)) {
    case 0x00: case 0x21:
        soft_reset();
        if (!present) { AX(r) = 0; break; }
        AX(r) = 0xFFFF; BX(r) = 2;
        break;
    case 0x01: if (show < 0) show++; break;
    case 0x02: show--; break;
    case 0x03: BX(r) = (u16)buttons; CX(r) = (u16)report_x(); DX(r) = (u16)report_y(); break;
    case 0x04: mx = (int16_t)CX(r); my = (int16_t)DX(r); clamp(); break;
    case 0x05: case 0x06: {                            /* press / release info */
        int i = BX(r) > 2 ? 0 : BX(r);
        int release = AL(r) == 0x06;
        AX(r) = (u16)buttons;
        if (release) {
            BX(r) = (u16)release_cnt[i]; CX(r) = (u16)release_x[i]; DX(r) = (u16)release_y[i];
            release_cnt[i] = 0;
        } else {
            BX(r) = (u16)press_cnt[i]; CX(r) = (u16)press_x[i]; DX(r) = (u16)press_y[i];
            press_cnt[i] = 0;
        }
        break; }
    case 0x07: xmin = (int16_t)CX(r); xmax = (int16_t)DX(r);
               if (xmin > xmax) { int t = xmin; xmin = xmax; xmax = t; } clamp(); break;
    case 0x08: ymin = (int16_t)CX(r); ymax = (int16_t)DX(r);
               if (ymin > ymax) { int t = ymin; ymin = ymax; ymax = t; } clamp(); break;
    case 0x09: break;                                  /* graphics cursor shape: standard arrow only */
    case 0x0A: if (BX(r) == 0) { text_and = CX(r); text_xor = DX(r); } break;
    case 0x0B: CX(r) = (u16)mick_x; DX(r) = (u16)mick_y; mick_x = mick_y = 0; break;
    case 0x0C: handler_mask = CX(r); handler_off = DX(r); handler_seg = (u16)r->v86_es; break;
    case 0x0F: if (CX(r)) ratio_x = CX(r); if (DX(r)) ratio_y = DX(r); break;
    case 0x10: break;                                  /* conditional off: ignored */
    case 0x13: break;
    case 0x14: {
        u16 m = handler_mask, o = handler_off, s = handler_seg;
        handler_mask = CX(r); handler_off = DX(r); handler_seg = (u16)r->v86_es;
        CX(r) = m; DX(r) = o; r->v86_es = s;
        break; }
    case 0x15: BX(r) = 64; break;                      /* state buffer size */
    case 0x16: case 0x17: break;
    case 0x1A: break;
    case 0x1B: BX(r) = 50; CX(r) = 50; DX(r) = 50; break;
    case 0x1D: case 0x1E: BX(r) = 0; break;
    case 0x24: BX(r) = 0x0626; CX(r) = 0x040C; break;  /* v6.26, PS/2, IRQ 12 */
    default: dbg(1, "INT 33h AX=%04x unsupported\n", AX(r));
    }
    video_native_pointer();                            /* video=vbe: shown / hidden / moved now, in video memory */
}
