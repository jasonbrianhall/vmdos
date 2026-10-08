/* The virtual-8086 monitor: traps from the guest (#GP on sensitive
   instructions), interrupt reflection, and virtual IRQ delivery. */
#include "kernel.h"

extern u8 v86_stack_top[];

int vif;                    /* guest's IF */
static u32 vflags_hi;       /* guest's IOPL/NT bits as it last set them (CPU detection) */

#define FLAGS_USER 0x0DD5u  /* CF PF AF ZF SF TF DF OF */

static u16 guest_flags16(struct regs *r)
{
    return (u16)((r->eflags & FLAGS_USER) | 2 | (vif ? EFL_IF : 0) | vflags_hi);
}

static void set_guest_flags(struct regs *r, u32 v)
{
    r->eflags = (r->eflags & ~FLAGS_USER) | (v & FLAGS_USER);
    vif = !!(v & EFL_IF);
    vflags_hi = v & 0x7000;
}

void v86_push16(struct regs *r, u16 v)
{
    SP(r) -= 2;
    wr16(LIN(r->ss, SP(r)), v);
}

u16 v86_pop16(struct regs *r)
{
    u16 v = rd16(LIN(r->ss, SP(r)));
    SP(r) += 2;
    return v;
}

static void push32(struct regs *r, u32 v) { v86_push16(r, v >> 16); v86_push16(r, (u16)v); }
static u32 pop32(struct regs *r) { u32 lo = v86_pop16(r); return lo | (u32)v86_pop16(r) << 16; }

void v86_reflect(struct regs *r, int vec)
{
    v86_push16(r, guest_flags16(r));
    v86_push16(r, (u16)r->cs);
    v86_push16(r, IP(r));
    r->eflags &= ~(EFL_TF | 0x40000u);
    vif = 0;
    r->eip = rd16(vec * 4);
    r->cs = rd16(vec * 4 + 2);
}

/* IRET emulation; keep: flag bits whose current value (set by a BIOS
   service) should survive instead of the stacked ones. */
static void iret16(struct regs *r, u32 keep)
{
    u32 cur = r->eflags;
    r->eip = v86_pop16(r);
    r->cs = v86_pop16(r);
    set_guest_flags(r, v86_pop16(r));
    r->eflags = (r->eflags & ~keep) | (cur & keep);
}

static void dump(struct regs *r, const char *why)
{
    u32 a = LIN(r->cs, r->eip);
    panic("%s\nvector %u err %x  CS:IP %04x:%04x  bytes %02x %02x %02x %02x %02x %02x\n"
          "AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x BP=%04x\n"
          "SS:SP=%04x:%04x DS=%04x ES=%04x FS=%04x GS=%04x FL=%x",
          why, r->vec, r->err, r->cs & 0xFFFF, IP(r),
          rd8(a), rd8(a + 1), rd8(a + 2), rd8(a + 3), rd8(a + 4), rd8(a + 5),
          AX(r), BX(r), CX(r), DX(r), SI(r), DI(r), BP(r),
          r->ss & 0xFFFF, SP(r), r->v86_ds & 0xFFFF, r->v86_es & 0xFFFF,
          r->v86_fs & 0xFFFF, r->v86_gs & 0xFFFF, r->eflags);
}

/* Wait (real HLT) until the guest has an interrupt it will take. */
void wait_for_irq(void)
{
    for (;;) {
        vkbd_refill();
        if (vif && vpic_pending() >= 0) return;
        idle_wait();
    }
}

/* Deliver a pending virtual IRQ; returns the frame to resume (a DPMI
   client's protected-mode handler may mean a mode switch). */
u32 stat_pmirq, stat_rmirq, stat_reflirq;
int dpmi_route_rm_irqs = -1;
static struct regs *deliver(struct regs *r)
{
    vkbd_refill();
    if (!vif) return r;
    int pm = !(r->eflags & EFL_VM);
    if (dpmi_route_rm_irqs < 0) dpmi_route_rm_irqs = !strstr(cmdline, "norouteirq");
    int vec = vpic_pending();
    if (vec < 0) {
        if (mouse_callback_due()) {
            if (!pm) mouse_start_callback(r);
            else return dpmi_rm_iret_call(r, 0xF000, mouse_begin_callback());   /* DOS/4GW games */
        }
        return r;
    }
    vpic_ack(vec);
    if (dpmi_pm_hooked(vec) && (pm || dpmi_route_rm_irqs)) { stat_pmirq++; return dpmi_hw_interrupt(r, vec); }
    if (pm) { stat_reflirq++; return dpmi_reflect_irq(r, vec); }
    stat_rmirq++;
    v86_reflect(r, vec);
    return r;
}

static void do_int(struct regs *r, int n, u16 ip0)
{
    u32 tgt = (u32)rd16(n * 4 + 2) << 4 | 0;
    tgt += rd16(n * 4);
    dbg(3, "INT %02x AX=%04x BX=%04x CX=%04x DX=%04x from %04x:%04x\n",
        n, AX(r), BX(r), CX(r), DX(r), r->cs & 0xFFFF, ip0);
    /* XMS lives in the monitor: answer its installation check before the
       INT 2Fh chain (DOS's own handler ends the chain without passing
       unknown calls on). */
    if (n == 0x2F && (AX(r) == 0x4300 || AX(r) == 0x4310)) {
        bios_service(r, 0x2F, 0);
        return;
    }
    if (n == 0x2F && AX(r) == 0x1687) { dpmi_detect(r); return; }    /* DPMI host */
    if (n == 0x2F && AX(r) == 0x5644) { cd_api(r); return; }            /* VMCD.SYS / VMCD.COM */
    if (n == 0x2F && AX(r) == 0x5645) { ems_query(r); return; }         /* VMEMS.SYS */
    if (n == 0x2F && AX(r) == 0x5642) {                                 /* VMSB */
        extern int sound_sb_api(int bx);
        AX(r) = (u16)sound_sb_api(BX(r));
        BX(r) = 0x564D;
        return;
    }
    if (n == 0x2F && AX(r) == 0x5653) {                                 /* VMSPEED */
        AX(r) = (u16)speed_api(BX(r));
        BX(r) = 0x564D;
        return;
    }
    /* The mouse driver too, unless a DOS mouse driver has taken INT 33h
       (DOS leaves unused vectors on a bare IRET). */
    if (n == 0x33 && (tgt == bios_stub_entry(0x33) || rd8(tgt) == 0xCF)) {
        bios_service(r, 0x33, 0);
        return;
    }
    if (bios_stub_is_direct(n) && tgt == bios_stub_entry(n)) {
        /* Nobody hooked it: run the BIOS service without a round trip. */
        if (bios_service(r, n, 0) == BIOS_RETRY) {
            IP(r) = ip0;
            vif = 1;
            wait_for_irq();
        }
        return;
    }
    v86_reflect(r, n);
}

static u32 seg_value(struct regs *r, int seg)
{
    switch (seg) {
    case 0: return r->v86_es;
    case 1: return r->cs;
    case 2: return r->ss;
    case 4: return r->v86_fs;
    case 5: return r->v86_gs;
    default: return r->v86_ds;
    }
}

/* String instructions that faulted because an offset reached the end of
   the segment (a word at offset FFFFh, or a MOVSW running into it): a 386
   raises #GP, an 8086 wraps around to offset 0, which old programs (and
   the FreeDOS kernel copying into their buffers) rely on. Emulated byte
   by byte with 16-bit offsets, all REP iterations at once. */
static u32 wrap_rd(u32 seg, u16 off, int s)
{
    u32 v = 0;
    for (int i = 0; i < s; i++) v |= (u32)rd8(LIN(seg, (u16)(off + i))) << (8 * i);
    return v;
}
static void wrap_wr(u32 seg, u16 off, u32 v, int s)
{
    for (int i = 0; i < s; i++) wr8(LIN(seg, (u16)(off + i)), (u8)(v >> (8 * i)));
}
static void sub_flags(struct regs *r, u32 a, u32 b, int s)
{
    u32 m = s == 1 ? 0xFF : s == 2 ? 0xFFFF : 0xFFFFFFFFu, sign = m ^ (m >> 1);
    u32 d = (a - b) & m;
    u32 f = r->eflags & ~(u32)(EFL_CF | EFL_PF | EFL_AF | EFL_ZF | EFL_SF | EFL_OF);
    if ((a & m) < (b & m)) f |= EFL_CF;
    if (!d) f |= EFL_ZF;
    if (d & sign) f |= EFL_SF;
    if ((a ^ b) & (a ^ d) & sign) f |= EFL_OF;
    if ((a ^ b ^ d) & 0x10) f |= EFL_AF;
    u8 p = (u8)d; p ^= p >> 4; p ^= p >> 2; p ^= p >> 1;
    if (!(p & 1)) f |= EFL_PF;
    r->eflags = f;
}
static void string_wrap(struct regs *r, u8 op, int s, int seg, int rep)
{
    int step = (r->eflags & EFL_DF) ? -s : s;
    u32 sseg = seg_value(r, seg), dseg = r->v86_es;
    u32 count = rep ? CX(r) : 1;
    int cmp = op == 0xA6 || op == 0xA7 || op == 0xAE || op == 0xAF;
    u32 am = s == 1 ? 0xFF : s == 2 ? 0xFFFF : 0xFFFFFFFFu;
    for (; count; count--) {
        switch (op & ~1) {
        case 0xA4: wrap_wr(dseg, DI(r), wrap_rd(sseg, SI(r), s), s); SI(r) += step; DI(r) += step; break;
        case 0xA6: sub_flags(r, wrap_rd(sseg, SI(r), s), wrap_rd(dseg, DI(r), s), s); SI(r) += step; DI(r) += step; break;
        case 0xAA: wrap_wr(dseg, DI(r), r->eax & am, s); DI(r) += step; break;
        case 0xAC: { u32 v = wrap_rd(sseg, SI(r), s);
                     if (s == 1) AL(r) = (u8)v; else if (s == 2) AX(r) = (u16)v; else r->eax = v;
                     SI(r) += step; break; }
        case 0xAE: sub_flags(r, r->eax & am, wrap_rd(dseg, DI(r), s), s); DI(r) += step; break;
        }
        if (rep) CX(r)--;
        if (rep && cmp && ((rep == 2) != ((r->eflags & EFL_ZF) != 0))) break;   /* REPE stops on NZ, REPNE on Z */
    }
}

static void gp_handler(struct regs *r)
{
    u16 ip0 = IP(r), ip = ip0;
    int op32 = 0, a32 = 0, rep = 0, seg = 3;
    u8 op;
    for (;;) {
        op = rd8(LIN(r->cs, ip));
        ip++;
        switch (op) {
        case 0x66: op32 = 1; continue;
        case 0x67: a32 = 1; continue;
        case 0xF2: rep = 1; continue;                               /* REPNE */
        case 0xF3: rep = 2; continue;                               /* REP / REPE */
        case 0xF0: continue;
        case 0x26: seg = 0; continue;
        case 0x2E: seg = 1; continue;
        case 0x36: seg = 2; continue;
        case 0x3E: seg = 3; continue;
        case 0x64: seg = 4; continue;
        case 0x65: seg = 5; continue;
        }
        break;
    }
    int sz = op32 ? 4 : 2;
    switch (op) {
    case 0xFA: vif = 0; IP(r) = ip; return;                         /* CLI */
    case 0xFB: vif = 1; IP(r) = ip; return;                         /* STI */
    case 0x9C:                                                      /* PUSHF */
        if (op32) push32(r, (r->eflags & FLAGS_USER) | 2 | (vif ? EFL_IF : 0) | vflags_hi);
        else v86_push16(r, guest_flags16(r));
        IP(r) = ip; return;
    case 0x9D:                                                      /* POPF */
        set_guest_flags(r, op32 ? pop32(r) : v86_pop16(r));
        IP(r) = ip; return;
    case 0xCF:                                                      /* IRET */
        if (op32) {
            r->eip = pop32(r) & 0xFFFF;
            r->cs = pop32(r) & 0xFFFF;
            set_guest_flags(r, pop32(r));
        } else iret16(r, 0);
        return;
    case 0xCD: {                                                    /* INT n */
        u8 n = rd8(LIN(r->cs, ip));
        IP(r) = ip + 1;
        do_int(r, n, ip0);
        return; }
    case 0xCC: IP(r) = ip; v86_reflect(r, 3); return;               /* INT3 */
    case 0x0F: {
        u8 op2 = rd8(LIN(r->cs, ip));
        if (op2 == 0x06) { IP(r) = ip + 1; return; }               /* CLTS: as EMM386, ignored */
        if (op2 == 0x20 || op2 == 0x22) {                           /* MOV r32, CRn / CRn, r32 */
            u8 m = rd8(LIN(r->cs, ip + 1));
            if ((m >> 6) == 3) {
                if (op2 == 0x20) {
                    u32 v = 0;
                    if (((m >> 3) & 7) == 0) { __asm__ volatile("mov %%cr0,%0" : "=r"(v)); v &= 0x8005003F; }
                    u32 *g[8] = { &r->eax, &r->ecx, &r->edx, &r->ebx, &r->esp, &r->ebp, &r->esi, &r->edi };
                    *g[m & 7] = v;
                }
                IP(r) = ip + 2;
                return;
            }
        }
        break; }
    case 0xCE: IP(r) = ip; if (r->eflags & EFL_OF) v86_reflect(r, 4); return;
    case 0xF4:                                                      /* HLT */
        if ((r->cs & 0xFFFF) == 0xF000 && ip0 < 0x8000) {
            int res = bios_service(r, rd8(LIN(r->cs, ip)), 1);
            if (res == BIOS_SWITCH) return;                  /* r is gone: mode switch */
            if (res == BIOS_DONE) iret16(r, 0);
            else if (res == BIOS_DONEF) iret16(r, EFL_CF | EFL_ZF);
            else if (res == BIOS_CONT) IP(r) = ip + 1;
            else { vif = 1; wait_for_irq(); }
            return;
        }
        IP(r) = ip;
        wait_for_irq();
        return;
    case 0xE4: case 0xE5: case 0xEC: case 0xED: {                   /* IN */
        u16 port = (op & 8) ? DX(r) : rd8(LIN(r->cs, ip++));
        int s = (op & 1) ? sz : 1;
        u32 v = port_in(port, s);
        if (s == 1) AL(r) = (u8)v; else if (s == 2) AX(r) = (u16)v; else r->eax = v;
        IP(r) = ip; return; }
    case 0xE6: case 0xE7: case 0xEE: case 0xEF: {                   /* OUT */
        u16 port = (op & 8) ? DX(r) : rd8(LIN(r->cs, ip++));
        int s = (op & 1) ? sz : 1;
        port_out(port, s == 1 ? AL(r) : s == 2 ? AX(r) : r->eax, s);
        IP(r) = ip; return; }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {                   /* INS / OUTS */
        int s = (op & 1) ? sz : 1;
        int step = (r->eflags & EFL_DF) ? -s : s;
        u32 count = rep ? (a32 ? r->ecx : CX(r)) : 1;
        for (; count; count--) {
            if (op < 0x6E) {
                u32 a = LIN(r->v86_es, a32 ? r->edi : DI(r));
                u32 v = port_in(DX(r), s);
                if (s == 1) wr8(a, v); else if (s == 2) wr16(a, v); else wr32(a, v);
                if (a32) r->edi += step; else DI(r) += step;
            } else {
                u32 a = LIN(seg_value(r, seg), a32 ? r->esi : SI(r));
                u32 v = s == 1 ? rd8(a) : s == 2 ? rd16(a) : rd32(a);
                port_out(DX(r), v, s);
                if (a32) r->esi += step; else SI(r) += step;
            }
            if (rep) { if (a32) r->ecx--; else CX(r)--; }
        }
        IP(r) = ip; return; }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:   /* MOVS CMPS STOS LODS SCAS */
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        if (a32) break;
        dbg(1, "segment wrap: string op %02x at %04x:%04x, SI=%04x DI=%04x CX=%04x\n", op, r->cs, ip0, SI(r), DI(r), CX(r));
        string_wrap(r, op, (op & 1) ? sz : 1, seg, rep);
        IP(r) = ip; return;
    }
    dump(r, "unhandled instruction in the DOS guest (#GP)");
}

static u32 refresh_div;

struct regs *isr_dispatch(struct regs *r)
{
    int from_v86 = (r->eflags & EFL_VM) != 0;
    int from_pm = !from_v86 && (r->cs & 3) == 3;     /* a DPMI client */
    u32 vec = r->vec;

    if (vec >= 0x20) {
        int irq = vec - 0x20;
        if (irq == 7 || irq == 15) {                 /* spurious? */
            outb(irq == 7 ? 0x20 : 0xA0, 0x0B);
            if (!(inb(irq == 7 ? 0x20 : 0xA0) & 0x80)) {
                if (irq == 15) outb(0x20, 0x20);
                goto out;
            }
        }
        if (irq == 0) {
            ticks++;
            vdev_tick();
            if (usb_ready) sound_tick();
            /* A protected-mode client can't change IF with POPF (IOPL 0), and
               PUSHF/CLI/.../POPF is common anyway: if its virtual IF has
               been off for a few ms with an interrupt waiting, take that as
               a POPF that turned interrupts back on. */
            static u32 vif_off;
            static int vifhack = -1;
            if (vifhack < 0) vifhack = !!strstr(cmdline, "vifhack");      /* old heuristic, off */
            if (vifhack && from_pm && !vif && vpic_pending() >= 0) {
                if (++vif_off >= 4) { vif = 1; vif_off = 0; }
            } else vif_off = 0;
        } else if (irq == 1) {
            vkbd_real_scancode(inb(0x60));
        } else if (irq == 12) {
            mouse_ps2_byte(inb(0x60));
        }
        if (irq >= 8) outb(0xA0, 0x20);
        outb(0x20, 0x20);
        if (irq == 0 && (from_v86 || from_pm)) speed_throttle();
        if (irq == 0 && usb_ready && (ticks & 7) == 0) {
            static int in_usb;
            if (!in_usb) { in_usb = 1; usb_tick(); in_usb = 0; }
        }
        if (irq == 0 && debug_level >= 2 && ticks % 5000 == 0) {
            extern int vif;
            void vpic_debug(char *buf, int n);
            int dpmi_depth(char *buf, int n);
            char b1[64], b2[64];
            vpic_debug(b1, sizeof b1);
            dpmi_depth(b2, sizeof b2);
            kprintf("[%us] irqs pm %u reflected %u rm %u, vif %u, mode %s, %x:%x  %s  %s\n", ticks / 1000,
                    stat_pmirq, stat_reflirq, stat_rmirq, vif, from_v86 ? "v86" : from_pm ? "pm" : "kernel",
                    r->cs, r->eip, b1, b2);
            if (from_pm) { void dpmi_dump_code(u32, u32, int); dpmi_dump_code(r->cs, r->eip - 0x30, 0x60); }
        }
        if (irq == 0 && ++refresh_div >= TICK_HZ / 60) {
            refresh_div = 0;
            video_refresh();
        }
        goto out;
    }

    if (from_pm) {
        r = dpmi_exception(r);
        goto out;
    }
    if (!from_v86) {
        u32 cr2;
        __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        panic("kernel exception %u, error %x, EIP %x, CR2 %x", vec, r->err, r->eip, cr2);
    }

    switch (vec) {
    case 13: gp_handler(r); break;
    case 14: {                                        /* the 16-color VGA window */
        u32 cr2;
        __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        struct emu_cpu e = { { &r->eax, &r->ecx, &r->edx, &r->ebx, &r->esp, &r->ebp, &r->esi, &r->edi },
                             &r->eip, &r->eflags,
                             { r->v86_es << 4, r->cs << 4, r->ss << 4, r->v86_ds << 4, r->v86_fs << 4, r->v86_gs << 4 }, 0 };
        if (!vga16_fault(&e, cr2)) dump(r, "page fault in the DOS guest");
        break; }
    case 0: case 1: case 5: case 6: case 7:          /* real-mode style: guest's own vector */
        v86_reflect(r, vec);
        break;
    case 16:                                          /* x87 error: a PC reports it on IRQ 13 */
        if (rd16(0x75 * 4 + 2) == 0xF000) __asm__ volatile("fnclex");   /* nobody handles it */
        else vpic_raise(13);
        break;
    default:
        dump(r, "exception in the DOS guest");
    }
out:
    {
        struct regs *sw = dpmi_take_switch();
        if (sw) r = sw;
    }
    if (from_v86 || from_pm) r = deliver(r);
    dpmi_espfix(r);
    return r;
}

void guest_start(void)
{
    struct regs *r = (struct regs *)((uintptr_t)v86_stack_top - sizeof(struct regs));
    memset(r, 0, sizeof *r);
    r->eflags = EFL_VM | EFL_IF | 2;
    vif = 1;
    bios_boot(r);
    kprintf("starting guest at %04x:%04x\n", r->cs, IP(r));
    v86_enter(r);
}
