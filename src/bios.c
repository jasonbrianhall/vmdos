/* The guest's BIOS: data area, IVT, and the services behind the bios.asm stubs
   (INT 08h/09h/11h-17h/1Ah; INT 10h is in video.c). */
#include "kernel.h"

extern const u8 bios_bin[], bios_bin_end[];

#define BIOS_SEG 0xF000
#define BIOS_LIN 0xF0000u
#define FONT16_OFF 0xA000
#define FONT8_OFF 0xB000
#define FONT14_OFF 0xD000                   /* 8x14 (EGA 350-line modes): the 8x16 font's rows 1-14 */

static u32 disk_cyls;
#define DISK_HEADS 255
#define DISK_SPT 63

u32 bios_stub_entry(int vec) { return BIOS_LIN + rd16(BIOS_LIN + vec * 2); }
int bios_stub_is_direct(int vec) { return (vec >= 0x10 && vec <= 0x17) || vec == 0x1A || vec == 0x2F || vec == 0x33; }

static void set_cf(struct regs *r, int c) { if (c) r->eflags |= EFL_CF; else r->eflags &= ~EFL_CF; }
static void set_zf(struct regs *r, int z) { if (z) r->eflags |= EFL_ZF; else r->eflags &= ~EFL_ZF; }

/* ---------------- CMOS clock ---------------- */
static u8 cmos(u8 reg) { outb(0x70, reg); return inb(0x71); }
static u8 cmos_bcd(u8 reg)     /* the RTC value, as BCD whatever the RTC's own format */
{
    u8 v = cmos(reg);
    if (cmos(0x0B) & 4) v = (u8)(((v / 10) << 4) | (v % 10));
    return v;
}
static int bcd2bin(u8 v) { return (v >> 4) * 10 + (v & 15); }
static void rtc_wait(void) { for (int i = 0; i < 100000 && (cmos(0x0A) & 0x80); i++) ; }

/* ---------------- keyboard ---------------- */
static const char kb_norm[] = "\0\x1b" "1234567890-=\b\tqwertyuiop[]\r\0asdfghjkl;'`\0\\zxcvbnm,./\0*\0 ";
static const char kb_shift[] = "\0\x1b" "!@#$%^&*()_+\b\tQWERTYUIOP{}\r\0ASDFGHJKL:\"~\0|ZXCVBNM<>?\0*\0 ";
static const char kb_pad[] = "789-456+1230.";
static const u8 kb_pad_ctrl[] = { 0x77, 0x8D, 0x84, 0x8E, 0x73, 0x8F, 0x74, 0x90, 0x75, 0x91, 0x76, 0x92, 0x93 };

static int kbuf_put(u16 w)
{
    u16 head = rd16(BDA + 0x1A), tail = rd16(BDA + 0x1C);
    u16 start = rd16(BDA + 0x80), end = rd16(BDA + 0x82);
    u16 next = tail + 2;
    if (next >= end) next = start;
    if (next == head) return 0;
    wr16(BDA + tail, w);
    wr16(BDA + 0x1C, next);
    return 1;
}

static int kbuf_get(u16 *w, int remove)
{
    u16 head = rd16(BDA + 0x1A), tail = rd16(BDA + 0x1C);
    if (head == tail) return 0;
    *w = rd16(BDA + head);
    if (remove) {
        head += 2;
        if (head >= rd16(BDA + 0x82)) head = rd16(BDA + 0x80);
        wr16(BDA + 0x1A, head);
    }
    return 1;
}

static void keyboard_irq(void)
{
    static int e0, skip;
    u8 sc = (u8)vkbd_read_data();
    if (skip) { skip--; return; }
    if (sc == 0xE0) { e0 = 1; return; }
    if (sc == 0xE1) { skip = 5; return; }               /* Pause */
    if (sc == 0xFA || sc == 0xFE || sc == 0xEE || sc == 0 || sc == 0xFF) return;   /* AAh is also LShift release */
    int ext = e0; e0 = 0;
    int rel = sc & 0x80, code = sc & 0x7F;
    u8 f = rd8(BDA + 0x17), f2 = rd8(BDA + 0x18), f3 = rd8(BDA + 0x96);

    /* modifiers */
    u8 bit = 0, bit2 = 0, bit3 = 0;
    switch (code) {
    case 0x2A: if (ext) return; bit = 0x02; break;
    case 0x36: if (ext) return; bit = 0x01; break;
    case 0x1D: bit = 0x04; if (ext) bit3 = 0x04; else bit2 = 0x01; break;
    case 0x38: bit = 0x08; if (ext) bit3 = 0x08; else bit2 = 0x02; break;
    }
    if (bit) {
        if (rel) { f2 &= ~bit2; f3 &= ~bit3; }
        else { f2 |= bit2; f3 |= bit3; }
        if (bit <= 2) { if (rel) f &= ~bit; else f |= bit; }
        else {                                     /* either Ctrl / either Alt */
            int held = bit == 4 ? ((f2 & 1) || (f3 & 4)) : ((f2 & 2) || (f3 & 8));
            if (held) f |= bit; else f &= ~bit;
        }
        wr8(BDA + 0x17, f); wr8(BDA + 0x18, f2); wr8(BDA + 0x96, (f3 & ~2) | 0x10 | (ext ? 2 : 0));
        return;
    }
    u8 lockbit = code == 0x3A ? 0x40 : code == 0x45 && !ext ? 0x20 : code == 0x46 && !ext ? 0x10 : 0;
    if (lockbit) {
        u8 lb2 = lockbit;
        if (rel) f2 &= ~lb2;
        else if (!(f2 & lb2)) { f ^= lockbit; f2 |= lb2; }
        wr8(BDA + 0x17, f); wr8(BDA + 0x18, f2);
        return;
    }
    if (rel) return;

    int shift = f & 3, ctrl = f & 4, alt = f & 8;
    u16 w = 0;
    if (ctrl && alt && code == 0x53) { kprintf("Ctrl+Alt+Del\n"); reboot(); }

    if (code >= 0x3B && code <= 0x44) {                 /* F1..F10 */
        int i = code - 0x3B;
        w = (u16)((alt ? 0x68 + i : ctrl ? 0x5E + i : shift ? 0x54 + i : code) << 8);
    } else if (code == 0x57 || code == 0x58) {          /* F11, F12 */
        int i = code - 0x57;
        w = (u16)((alt ? 0x8B : ctrl ? 0x89 : shift ? 0x87 : 0x85) + i) << 8;
    } else if (code >= 0x47 && code <= 0x53 && code != 0x4A && code != 0x4E) {
        int i = code - 0x47;
        int digits = !ext && (!!(f & 0x20) ^ !!shift);
        if (alt && ext) w = (u16)((code + 0x50) << 8);
        else if (ctrl) w = (u16)(kb_pad_ctrl[i] << 8 | (ext ? 0xE0 : 0));
        else if (digits) w = (u16)(code << 8 | (u8)kb_pad[i]);
        else w = (u16)(code << 8 | (ext ? 0xE0 : 0));
        if (code == 0x52 && !digits) wr8(BDA + 0x17, f ^ 0x80);
    } else if (ext && code == 0x1C) {
        w = ctrl ? 0xE00A : 0xE00D;
    } else if (ext && code == 0x35) {
        w = 0xE02F;
    } else if (ext && code == 0x46) {                   /* Ctrl+Break */
        wr16(BDA + 0x1A, rd16(BDA + 0x80)); wr16(BDA + 0x1C, rd16(BDA + 0x80));
        wr8(BDA + 0x71, 0x80);
        w = 0;
        kbuf_put(0);
        return;
    } else if (code < 0x3A || code == 0x4A || code == 0x4E || code == 0x56) {
        char c;
        if (code == 0x4A) c = '-';
        else if (code == 0x4E) c = '+';
        else if (code == 0x56) c = shift ? '|' : '\\';
        else c = shift ? kb_shift[code] : kb_norm[code];
        if (c >= 'a' && c <= 'z' && (f & 0x40)) c -= 32;
        else if (c >= 'A' && c <= 'Z' && (f & 0x40)) c += 32;
        if (alt) {
            if (code >= 0x02 && code <= 0x0D) w = (u16)((code + 0x76) << 8);
            else if (code == 0x0F) w = 0xA500;
            else if (code == 0x1C) w = 0x1C00;
            else w = (u16)(code << 8);
        } else if (ctrl) {
            if ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') w = (u16)(code << 8 | ((c | 0x20) - 'a' + 1));
            else switch (code) {
                case 0x1A: w = 0x1A1B; break;
                case 0x2B: w = 0x2B1C; break;
                case 0x1B: w = 0x1B1D; break;
                case 0x07: w = 0x071E; break;
                case 0x0C: w = 0x0C1F; break;
                case 0x03: w = 0x0300; break;
                case 0x0E: w = 0x0E7F; break;
                case 0x1C: w = 0x1C0A; break;
                case 0x01: w = 0x011B; break;
                case 0x0F: w = 0x9400; break;
                case 0x39: w = 0x3920; break;
                default: return;
            }
        } else if (shift && code == 0x0F) w = 0x0F00;
        else w = (u16)(code << 8 | (u8)c);
    } else return;
    if (!kbuf_put(w)) dbg(1, "keyboard buffer full\n");
}

static int int16(struct regs *r)
{
    u16 w;
    switch (AH(r)) {
    case 0x00: case 0x10:
        if (!kbuf_get(&w, 1)) return BIOS_RETRY;
        if (AH(r) == 0x00 && (w & 0xFF) == 0xE0 && (w >> 8)) w &= 0xFF00;
        AX(r) = w;
        break;
    case 0x01: case 0x11:
        if (!kbuf_get(&w, 0)) { set_zf(r, 1); break; }   /* no key */
        if (AH(r) == 0x01 && (w & 0xFF) == 0xE0 && (w >> 8)) w &= 0xFF00;
        AX(r) = w;
        set_zf(r, 0);
        break;
    case 0x02: AL(r) = rd8(BDA + 0x17); break;
    case 0x12: {
        u8 f2 = rd8(BDA + 0x18), f3 = rd8(BDA + 0x96);
        AL(r) = rd8(BDA + 0x17);
        AH(r) = (f2 & 0x73) | (f3 & 0x0C) | ((f2 & 0x04) << 5);
        break; }
    case 0x05: AL(r) = kbuf_put(CX(r)) ? 0 : 1; break;
    case 0x0A: BX(r) = 0x41AB; break;
    }
    return BIOS_DONEF;
}

/* ---------------- disk ---------------- */
static u8 disk_status;

static int disk_rw(u32 lba, u32 count, u32 buf, int write)
{
    if (buf + count * 512 > GUEST_TOP) return 0x09;
    return write ? disk_write(lba, count, gptr(buf)) : disk_read(lba, count, gptr(buf));
}

static int int13(struct regs *r)
{
    int st = 0;
    u8 fn = AH(r);
    if (DL(r) < 0x80) { fd_int13(r); return BIOS_DONEF; }   /* A:, B: (floppy.c) */
    if (DL(r) > 0x80 && DL(r) < 0x80 + hd_bios_disks()) { hd_int13(r); wr8(BDA + 0x74, AH(r)); return BIOS_DONEF; }   /* the other disks (hd.c) */
    if (DL(r) != 0x80) {
        if (fn == 0x00) { set_cf(r, 0); AH(r) = 0; return BIOS_DONEF; }
        AH(r) = 0x01;
        set_cf(r, 1);
        return BIOS_DONEF;
    }
    switch (fn) {
    case 0x00: case 0x0D: disk_flush(); break;                  /* reset: the disk up to date */
    case 0x04: case 0x0C: case 0x10: case 0x11: case 0x47: break;
    case 0x01: AH(r) = disk_status; set_cf(r, disk_status != 0); return BIOS_DONEF;
    case 0x02: case 0x03: {
        u32 cyl = CH(r) | ((u32)(CL(r) & 0xC0) << 2), sec = CL(r) & 63, head = DH(r);
        if (!sec || head >= DISK_HEADS) { st = 0x04; break; }
        u32 lba = (cyl * DISK_HEADS + head) * DISK_SPT + sec - 1;
        st = disk_rw(lba, AL(r), LIN(r->v86_es, BX(r)), fn == 3);
        if (st) AL(r) = 0;
        break; }
    case 0x08: {
        u32 mc = disk_cyls - 1;
        CH(r) = (u8)mc;
        CL(r) = (u8)(((mc >> 2) & 0xC0) | DISK_SPT);
        DH(r) = DISK_HEADS - 1;
        DL(r) = (u8)hd_bios_disks();
        break; }
    case 0x15:
        AH(r) = 3;
        CX(r) = (u16)(disk_sectors >> 16);
        DX(r) = (u16)disk_sectors;
        set_cf(r, 0);
        return BIOS_DONEF;
    case 0x41:
        if (BX(r) != 0x55AA) { st = 1; break; }
        BX(r) = 0xAA55; AH(r) = 0x21; CX(r) = 1;
        set_cf(r, 0);
        return BIOS_DONEF;
    case 0x42: case 0x43: case 0x44: {
        u32 p = LIN(r->v86_ds, SI(r));
        u16 count = rd16(p + 2);
        u32 buf = LIN(rd16(p + 6), rd16(p + 4));
        u32 lba = rd32(p + 8);
        if (rd32(p + 12)) { st = 0x04; break; }
        if (fn != 0x44) st = disk_rw(lba, count, buf, fn == 0x43);
        if (st) wr16(p + 2, 0);
        break; }
    case 0x48: {
        u32 p = LIN(r->v86_ds, SI(r));
        if (rd16(p) < 26) { st = 1; break; }
        wr16(p, 26); wr16(p + 2, 2);
        wr32(p + 4, disk_cyls); wr32(p + 8, DISK_HEADS); wr32(p + 12, DISK_SPT);
        wr32(p + 16, disk_sectors); wr32(p + 20, 0); wr16(p + 24, 512);
        break; }
    default:
        dbg(1, "INT 13h AH=%02x unsupported\n", fn);
        st = 1;
    }
    disk_status = (u8)st;
    wr8(BDA + 0x74, (u8)st);
    AH(r) = (u8)st;
    set_cf(r, st != 0);
    return BIOS_DONEF;
}

/* ---------------- INT 15h ---------------- */
static int int15_ps2(struct regs *r);
static int int15(struct regs *r)
{
    switch (AH(r)) {
    case 0x24:
        switch (AL(r)) {
        case 0: set_a20(0); AH(r) = 0; break;
        case 1: set_a20(1); AH(r) = 0; break;
        case 2: AH(r) = 0; AL(r) = (u8)a20_on; break;
        case 3: AH(r) = 0; BX(r) = 3; break;
        default: AH(r) = 0x86; set_cf(r, 1); return BIOS_DONEF;
        }
        set_cf(r, 0);
        return BIOS_DONEF;
    case 0x4F: set_cf(r, 1); return BIOS_DONEF;           /* keep the scan code */
    case 0x86: {
        u32 us = (u32)CX(r) << 16 | DX(r);
        u32 clocks = us / 1000 * 1193 + (us % 1000) * 1193 / 1000;
        u32 end = pit_clock() + clocks;
        while ((int32_t)(pit_clock() - end) < 0) idle_wait();
        set_cf(r, 0);
        return BIOS_DONEF; }
    case 0x84:                                            /* joystick (joy.c) */
        if (joy_bios(r)) { set_cf(r, 0); return BIOS_DONEF; }
        set_cf(r, 1);
        return BIOS_DONEF;
    case 0x88: AX(r) = 0; set_cf(r, 0); return BIOS_DONEF;
    case 0x90: case 0x91: AH(r) = 0; set_cf(r, 0); return BIOS_DONEF;
    case 0xC0:
        r->v86_es = BIOS_SEG;
        BX(r) = rd16(BIOS_LIN + 0x200);
        AH(r) = 0;
        set_cf(r, 0);
        return BIOS_DONEF;
    case 0xC2: return int15_ps2(r);
    }
    dbg(2, "INT 15h AX=%04x unsupported\n", AX(r));
    AH(r) = 0x86;
    set_cf(r, 1);
    return BIOS_DONEF;
}

/* ---------------- PS/2 BIOS mouse (INT 15h C2xx) ----------------
   For DOS mouse drivers such as CuteMouse (CTMOUSE.COM): they register a
   far handler (C207h) and the BIOS IRQ 12 handler (int74 in bios.asm,
   trap 74h) calls it with status, X, Y and 0 on the stack for every
   3-byte packet from the virtual PS/2 mouse port (vdev.c). */
#define PS2_HANDLER (BIOS_LIN + rd16(BIOS_LIN + 0x22C))
static int ps2_on, ps2_n;
static u8 ps2_pkt[3];

static int int15_ps2(struct regs *r)
{
    int err = 0;
    if (mouse_log()) kprintf("mouse: INT 15h AX=%04x BX=%04x ES=%04x\n", AX(r), BX(r), r->v86_es);
    switch (AL(r)) {
    case 0x00:                                          /* enable (BH=1) / disable (BH=0) */
        if (BH(r) > 1) err = 1;
        else if (BH(r) == 1 && !rd32(PS2_HANDLER)) err = 5;
        else { ps2_on = BH(r); ps2_n = 0; vaux_bios_enable(ps2_on); }
        break;
    case 0x01:                                          /* reset: disabled, ID 0 */
        ps2_on = 0; ps2_n = 0; vaux_bios_reset();
        BH(r) = 0; BL(r) = 0xAA;
        break;
    case 0x02: if (BH(r) > 6) err = 2; break;           /* sample rate */
    case 0x03: if (BH(r) > 3) err = 2; break;           /* resolution */
    case 0x04: BH(r) = 0; break;                        /* device ID: standard mouse */
    case 0x05:                                          /* initialise, BH = packet size */
        if (BH(r) < 1 || BH(r) > 8) { err = 2; break; }
        ps2_on = 0; ps2_n = 0; vaux_bios_reset();
        break;
    case 0x06:
        if (BH(r) == 0) { BL(r) = ps2_on ? 0x20 : 0; CL(r) = 2; DL(r) = 100; }
        else if (BH(r) > 2) err = 1;
        break;
    case 0x07:                                          /* handler ES:BX (0:0 = none) */
        wr16(PS2_HANDLER, BX(r)); wr16(PS2_HANDLER + 2, r->v86_es);
        break;
    default: err = 1;
    }
    AH(r) = (u8)err;
    set_cf(r, err != 0);
    return BIOS_DONEF;
}

/* Trap 74h, from int74: take the mouse byte; at the end of a packet return
   DX=1 with AX status, BX X, CX Y for the stub to pass on, else DX=0. */
static void ps2_irq(struct regs *r)
{
    DX(r) = 0;
    int b = vaux_bios_byte();
    if (b < 0) return;
    if (ps2_n == 0 && !(b & 0x08)) return;              /* resync on byte 0 (bit 3 set) */
    ps2_pkt[ps2_n++] = (u8)b;
    if (ps2_n < 3) return;
    ps2_n = 0;
    if (!ps2_on || !rd32(PS2_HANDLER)) return;
    AX(r) = ps2_pkt[0]; BX(r) = ps2_pkt[1]; CX(r) = ps2_pkt[2]; DX(r) = 1;
}

/* ---------------- INT 1Ah ---------------- */
static int int1a(struct regs *r)
{
    switch (AH(r)) {
    case 0x00:
        CX(r) = rd16(BDA + 0x6E); DX(r) = rd16(BDA + 0x6C);
        AL(r) = rd8(BDA + 0x70); wr8(BDA + 0x70, 0);
        break;
    case 0x01: wr16(BDA + 0x6E, CX(r)); wr16(BDA + 0x6C, DX(r)); wr8(BDA + 0x70, 0); break;
    case 0x02:
        rtc_wait();
        CH(r) = cmos_bcd(4); CL(r) = cmos_bcd(2); DH(r) = cmos_bcd(0); DL(r) = 0;
        break;
    case 0x04:
        rtc_wait();
        CH(r) = cmos_bcd(0x32); CL(r) = cmos_bcd(9); DH(r) = cmos_bcd(8); DL(r) = cmos_bcd(7);
        if (CH(r) < 0x19 || CH(r) > 0x21) CH(r) = CL(r) >= 0x80 ? 0x19 : 0x20;
        break;
    case 0x03: case 0x05: break;                         /* setting the clock: ignored */
    default: set_cf(r, 1); return BIOS_DONEF;
    }
    set_cf(r, 0);
    return BIOS_DONEF;
}

int bios_service(struct regs *r, int id, int via_stub)
{
    (void)via_stub;
    switch (id) {
    case 0x06: {
        u16 ip = rd16(LIN(r->ss, SP(r))), cs = rd16(LIN(r->ss, SP(r) + 2));
        u32 a = LIN(cs, ip);
        panic("invalid opcode at %04x:%04x (%02x %02x %02x %02x) and no handler for INT 6",
              cs, ip, rd8(a), rd8(a + 1), rd8(a + 2), rd8(a + 3)); }
    case 0x08: {
        u32 t = rd32(BDA + 0x6C) + 1;
        if (t >= 0x1800B0) { t = 0; wr8(BDA + 0x70, 1); }
        wr32(BDA + 0x6C, t);
        return BIOS_CONT; }
    case 0x09:
        keyboard_irq();
        vpic_eoi_irq(1);
        return BIOS_DONE;
    case 0x10: video_int10(r); return BIOS_DONEF;
    case 0x11: AX(r) = rd16(BDA + 0x10); return BIOS_DONEF;
    case 0x12: AX(r) = rd16(BDA + 0x13); return BIOS_DONEF;
    case 0x13: return int13(r);
    case 0x14: AH(r) = 0x80; return BIOS_DONEF;
    case 0x15: return int15(r);
    case 0x16: return int16(r);
    case 0x17: AH(r) = 0x09; return BIOS_DONEF;
    case 0x18: panic("No bootable disk (INT 18h)");
    case 0x19: reboot();
    case 0x1A: return int1a(r);
    case 0x2F:
        dbg(3, "BIOS INT 2Fh AX=%04x via %s\n", AX(r), via_stub ? "chain" : "direct");
        if (xms_hidden) ;                                       /* noxms: none here */
        else if (AX(r) == 0x4300) AL(r) = 0x80;                /* XMS driver installed */
        else if (AX(r) == 0x4310) { r->v86_es = BIOS_SEG; BX(r) = rd16(BIOS_LIN + 0x206); }
        return BIOS_DONE;
    case 0x43: xms_call(r); return BIOS_CONT;                  /* XMS entry, then RETF */
    case 0x33: mouse_int33(r); return BIOS_DONE;
    case 0x50: case 0x54: case 0x55: case 0x56: return dpmi_rm_trap(r, id);
    case 0x34: mouse_cb_regs(r); return BIOS_CONT;
    case 0x46: video_vbe_window(r); return BIOS_CONT;         /* VESA WinFuncPtr */
    case 0x35: mouse_cb_done(); return BIOS_CONT;
    case 0x74: ps2_irq(r); return BIOS_CONT;
    case 0x67: ems_int67(r); return BIOS_DONE;
    }
    if (id >= 0x60 && id < 0x70) return dpmi_rm_trap(r, id);   /* DPMI real-mode callbacks */
    return BIOS_DONE;
}

/* ---------------- setup ---------------- */
void bios_init(void)
{
    memset(gptr(0), 0, GUEST_TOP);
    memcpy(gptr(BIOS_LIN), bios_bin, bios_bin_end - bios_bin);
    memcpy(gptr(BIOS_LIN + FONT16_OFF), vga_font16, 4096);
    memcpy(gptr(BIOS_LIN + FONT8_OFF), vga_font8, 2048);
    for (int c = 0; c < 256; c++) memcpy(gptr(BIOS_LIN + FONT14_OFF + c * 14), vga_font16 + c * 16 + 1, 14);
    for (int v = 0; v < 256; v++) {
        wr16(v * 4, rd16(BIOS_LIN + v * 2));
        wr16(v * 4 + 2, BIOS_SEG);
    }
    wr16(0x1E * 4, rd16(BIOS_LIN + 0x202));
    wr16(0x1F * 4, FONT8_OFF + 1024);
    wr16(0x43 * 4, FONT8_OFF);
    wr32(0x41 * 4, 0);
    wr32(0x46 * 4, 0);

    /* BIOS data area */
    u16 equip = 0x0065;                                  /* 80x25 color, PS/2 mouse port, two floppy drives (floppy.c) */
    extern u32 fpu_present;
    if (fpu_present) equip |= 2;
    wr16(BDA + 0x10, equip);
    wr16(BDA + 0x13, 640);
    wr16(BDA + 0x1A, 0x1E); wr16(BDA + 0x1C, 0x1E);
    wr16(BDA + 0x80, 0x1E); wr16(BDA + 0x82, 0x3E);
    wr8(BDA + 0x75, 1);                                  /* one hard disk */
    wr8(BDA + 0x8F, 0x77);                               /* both floppy drives: 80 tracks, change line */
    wr8(BDA + 0x96, 0x10);                               /* 101/102-key keyboard */
    wr8(BDA + 0x17, 0x20);                               /* Num Lock on */

    rtc_wait();
    u32 secs = bcd2bin(cmos_bcd(4)) * 3600 + bcd2bin(cmos_bcd(2)) * 60 + bcd2bin(cmos_bcd(0));
    wr32(BDA + 0x6C, secs * 19663 / 1080);

    video_set_mode(3, 1);

    disk_cyls = disk_sectors / (DISK_HEADS * DISK_SPT);
    if (disk_cyls > 1024) disk_cyls = 1024;
    if (!disk_cyls) disk_cyls = 1;
    kprintf("disk: %u sectors (%u MiB), CHS %u/%u/%u\n", disk_sectors, disk_sectors >> 11,
            disk_cyls, DISK_HEADS, DISK_SPT);
}

/* FreeDOS boot sectors (boot/), laid over C:'s boot sector in memory so a
   partition formatted elsewhere (an ESP made by mkfs or Windows) boots too;
   the disk itself is not changed. */
extern const u8 fd_boot16[512], fd_boot32[512];

/* Start the guest from floppy drive d's boot sector, as a BIOS would:
   loaded at 0000:7C00, DL = the drive. Booters ("self-booting" games)
   run without DOS. 0, or -1 when the drive is empty. */
int fd_boot_sector(int d, u8 *out);
extern int guest_from_floppy;
static int boot_floppy(struct regs *r, int d)
{
    static u8 sec[512];
    if (fd_boot_sector(d, sec)) return -1;
    guest_from_floppy = 1;
    memcpy(gptr(0x7C00), sec, 512);
    r->cs = 0; r->eip = 0x7C00;
    r->ss = 0; r->esp = 0x7C00;
    r->v86_ds = r->v86_es = r->v86_fs = r->v86_gs = 0;
    r->eax = r->ebx = r->ecx = r->esi = r->edi = r->ebp = 0;
    r->edx = (u32)d;
    kprintf("booting floppy %c:%s\n", 'A' + d, sec[510] == 0x55 && sec[511] == 0xAA ? "" : " (no 55AA signature; started anyway)");
    return 0;
}

/* VMFD A: /BOOT: restart the guest into a floppy, without DOS: the
   virtual hardware and the BIOS go back to how they are at power-on
   (interrupt vectors, BIOS data, PIC, timer, A20 off, text mode, no EMS
   pages mapped, mouse driver reset, CD audio and Sound Blaster stopped).
   -1 when the drive is empty (nothing changed). */
void vpic_reset(void);
void xms_restart(void);
void ems_restart(void);
void mouse_restart(void);
void cdaudio_stop(void);
void sb_out_stop(void);
extern int vif;
int bios_restart_floppy(struct regs *r, int d)
{
    static u8 sec[512];
    if (fd_boot_sector(d, sec)) return -1;
    disk_flush();
    cdaudio_stop();
    sb_out_stop();
    mouse_restart();
    ems_restart();
    xms_restart();
    vpic_reset();
    vdev_init();
    bios_init();
    boot_floppy(r, d);
    r->eflags = EFL_VM | EFL_IF | 2;
    vif = 1;
    return 0;
}

void bios_boot(struct regs *r)
{
    const char *bo = strstr(cmdline, "boot=");                      /* boot=a / boot=b: a floppy, not DOS */
    if (bo && (bo == cmdline || bo[-1] == ' ') && ((bo[5] | 32) == 'a' || (bo[5] | 32) == 'b')) {
        if (!boot_floppy(r, (bo[5] | 32) - 'a')) return;
        kprintf("boot=%c: the drive is empty (fd%c= for an image); booting C:\n", bo[5], bo[5] | 32);
    }
    if (disk_kind()[0] == 'R' && disk_image[0] == 0x1F && disk_image[1] == 0x8B)
        panic("dos.img is gzip-compressed. GRUB unpacks modules itself; for QEMU -initrd use the plain image.");
    static struct fatvol v;
    u8 m[512];
    if (disk_read(0, 1, m) || m[510] != 0x55 || m[511] != 0xAA) panic("C: has no boot signature in sector 0");
    if (disk_volume(&v)) panic("C: has no FAT partition");
    if (v.type == 12) panic("C: is FAT12; FreeDOS needs FAT16 or FAT32 here");
    int part = -1;
    if (v.base) {
        for (int i = 0; i < 4; i++) if (*(u32 *)(m + 446 + i * 16 + 8) == v.base) part = i;
        memcpy(gptr(0x600), m, 512);                 /* as an MBR would leave it */
    }
    u8 *b = gptr(0x7C00);
    if (disk_read(v.base, 1, b)) panic("C: boot sector unreadable");
    const u8 *code = v.type == 32 ? fd_boot32 : fd_boot16;
    int bpb_end = v.type == 32 ? 0x5A : 0x3E;
    memcpy(b, code, 3);
    memcpy(b + bpb_end, code + bpb_end, 510 - bpb_end);
    *(u32 *)(b + 0x1C) = v.base;                     /* hidden sectors = partition start */
    b[v.type == 32 ? 0x40 : 0x24] = 0x80;            /* drive number */
    kprintf("booting FAT%d partition %d at LBA %u (%s)\n", v.type, part, v.base, disk_kind());

    r->cs = 0; r->eip = 0x7C00;
    r->ss = 0; r->esp = 0x7C00;
    r->v86_ds = r->v86_es = r->v86_fs = r->v86_gs = 0;
    r->edx = 0x80;
    r->esi = part >= 0 ? 0x7BE + part * 16 : 0;
    r->ebp = r->esi;
}
