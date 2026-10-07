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

/* Called from interrupt handlers: motion in counts (dy positive = down). */
void mouse_input(int dx, int dy, int b)
{
    raw_dx += dx;
    raw_dy += dy;
    raw_buttons = b & 7;
    have_input = 1;
}

/* ---------------- PS/2 mouse on the real 8042 ---------------- */
static int kbc_wait_write(void) { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 2)) return 1; return 0; }
static int kbc_wait_read(void) { for (int i = 0; i < 100000; i++) if (inb(0x64) & 1) return 1; return 0; }
static int aux_cmd(u8 c)
{
    kbc_wait_write(); outb(0x64, 0xD4);
    kbc_wait_write(); outb(0x60, c);
    return kbc_wait_read() ? inb(0x60) : -1;
}

void mouse_ps2_init(void)
{
    if (inb(0x64) == 0xFF) return;                     /* no 8042 */
    kbc_wait_write(); outb(0x64, 0xA8);                /* enable the aux port */
    kbc_wait_write(); outb(0x64, 0x20);
    if (!kbc_wait_read()) return;
    u8 cfg = inb(0x60);
    cfg |= 0x02;                                       /* aux IRQ 12 */
    cfg &= ~0x20;                                      /* aux clock on */
    kbc_wait_write(); outb(0x64, 0x60);
    kbc_wait_write(); outb(0x60, cfg);
    if (aux_cmd(0xF6) != 0xFA) { kprintf("mouse: no PS/2 mouse\n"); return; }
    aux_cmd(0xF4);                                     /* stream mode on */
    outb(0xA1, inb(0xA1) & ~0x10);                     /* unmask IRQ 12 */
    present = 1;
    kprintf("mouse: PS/2\n");
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

/* "mouselog" on the command line: what a program asks of the mouse. */
static int mlog = -1;
int mouse_log(void) { if (mlog < 0) mlog = !!strstr(cmdline, "mouselog"); return mlog; }

void mouse_int33(struct regs *r)
{
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
}
