/* The PC game port (201h) and INT 15h AH=84h, for USB gamepads (usb.cpp).
   Up to two pads: the first is joystick A, the second joystick B. With
   only one, its buttons 3 and 4 are B's buttons (as 4-button sticks do).

   Port 201h: writing starts the four one-shots; bits 0-3 (A X, A Y, B X,
   B Y) read 1 until a time that grows with the axis position, then 0;
   bits 4-7 are the buttons, 0 = pressed. A stick that isn't there never
   times out (its bits stay 1), so games see it's missing. Timing is by
   the PIT clock, as the guest's own timers. */
#include "kernel.h"

static struct pad {
    int on;
    u8 x, y;                              /* 0 left/up, 128 centre, 255 right/down */
    u8 btn;                               /* bits 0-3: game port buttons 1-4 */
} pads[2];
static u32 shot;                          /* pit_clock() of the last write to 201h */

void joy_set(int pad, int on, int x, int y, int buttons)
{
    if (pad < 0 || pad > 1) return;
    struct pad *p = &pads[pad];
    if (on && !p->on) kprintf("joystick: %c connected\n", 'A' + pad);
    if (!on && p->on) kprintf("joystick: %c gone\n", 'A' + pad);
    p->on = on;
    p->x = (u8)(x < 0 ? 0 : x > 255 ? 255 : x);
    p->y = (u8)(y < 0 ? 0 : y > 255 ? 255 : y);
    p->btn = on ? (u8)(buttons & 15) : 0;
}

int joy_present(void) { return pads[0].on || pads[1].on; }

/* An axis' one-shot: about 24 us + up to 1.1 ms (a 100 kOhm stick), in PIT clocks. */
static u32 shot_len(u8 v) { return (24 + (u32)v * 1100 / 255) * 1193 / 1000; }

/* The four buttons as the port has them (bits 4-7, 1 = pressed here). */
static u8 buttons(void)
{
    u8 b = pads[0].btn & 3;                                     /* A: 1, 2 */
    if (pads[1].on) b |= (pads[1].btn & 3) << 2;                /* B: the second pad's 1, 2 */
    else b |= pads[0].btn & 12;                                  /* or the first pad's 3, 4 */
    return b;
}

u8 joy_port_in(void)
{
    if (!joy_present()) return 0xFF;
    u32 e = pit_clock() - shot;
    u8 v = (u8)((~buttons() & 15) << 4);
    u8 ax[4] = { pads[0].x, pads[0].y, pads[1].x, pads[1].y };
    for (int i = 0; i < 4; i++) {
        int on = pads[i / 2].on;
        if (!on || e < shot_len(ax[i])) v |= (u8)(1 << i);      /* still running (or no stick: forever) */
    }
    return v;
}

void joy_port_out(void) { shot = pit_clock(); }

/* INT 15h AH=84h: DX=0 buttons in AL bits 4-7 (as the port), DX=1 the
   positions in AX, BX (A) and CX, DX (B), 0-255. CF: no joystick. */
int joy_bios(struct regs *r)
{
    if (!joy_present()) return 0;
    if (DX(r) == 0) AL(r) = (u8)((~buttons() & 15) << 4);
    else if (DX(r) == 1) {
        AX(r) = pads[0].on ? pads[0].x : 0; BX(r) = pads[0].on ? pads[0].y : 0;
        CX(r) = pads[1].on ? pads[1].x : 0; DX(r) = pads[1].on ? pads[1].y : 0;
    } else return 0;
    return 1;
}
