/* The virtual-8086 monitor: traps from the guest (#GP on sensitive
   instructions), interrupt reflection, and virtual IRQ delivery. */
#include "kernel.h"

extern u8 v86_stack_top[];

int vif;                    /* guest's IF */
/* Guest's NT bit as it last set it (CPU detection) plus IOPL, which reads
   as 3 whatever the guest writes: in virtual-8086 mode POPF/IRET can't
   change IOPL, and DOS extenders use exactly that to tell real mode from
   V86. DOS/32A clears IOPL and, if it stays 0, takes the machine for real
   mode and starts its own raw/XMS kernel (LGDT, LIDT, MOV CR0); seeing it
   stuck at 3 it asks for VCPI and DPMI instead and runs on our DPMI host. */
#define VIOPL 0x3000u
static u32 vflags_hi = VIOPL;

#define FLAGS_USER 0x0DD5u  /* CF PF AF ZF SF TF DF OF */

static u16 guest_flags16(struct regs *r)
{
    return (u16)((r->eflags & FLAGS_USER) | 2 | (vif ? EFL_IF : 0) | vflags_hi);
}

static void set_guest_flags(struct regs *r, u32 v)
{
    r->eflags = (r->eflags & ~FLAGS_USER) | (v & FLAGS_USER);
    vif = !!(v & EFL_IF);
    vflags_hi = (v & 0x4000) | VIOPL;
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

/* The guest tried to switch the CPU to protected mode itself (LGDT,
   LIDT, LMSW or MOV CR0 with PE): a virtual-8086 monitor can't hand it
   the bare CPU. Say so plainly rather than as a generic #GP. */
int guest_from_floppy;                   /* bios.c: started from a floppy's boot sector */
static void raw_pm(struct regs *r, const char *insn)
{
    char why[400];
    snprintf(why, sizeof why,
             "the program switches the CPU to protected mode itself (%s)\n"
             "vmdos runs it in virtual-8086 mode and can't give it the bare CPU.\n%s",
             insn, guest_from_floppy
             ? "This disk is made to boot on the bare machine: boot it with GRUB\n"
               "(or straight from the firmware) instead of VMFD /BOOT or boot=a.\n"
             : "DOS programs get protected mode from vmdos's DPMI host (or VCPI);\n"
               "this one wants the machine to itself.\n");
    dump(r, why);
}

/* The guest is idle: DOS's prompt (FreeDOS waits for a key by asking
   INT 16h AH=01 "is there one?" in a loop, with INT 28h in between),
   programs polling the keyboard. Many such polls within one millisecond
   mean it is just spinning: halt the real CPU until the next interrupt
   (at most a millisecond, the timer), so an idle DOS doesn't keep a core
   busy. A game that polls once a frame never comes near the threshold. */
void guest_idle_poll(void)
{
    static u32 last, polls;
    if (ticks != last) { last = ticks; polls = 0; fs9p_idle(); }
    if (++polls > 4) { idle_wait(); polls = 0; }
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

/* The real 8042: a byte its interrupt never came for (lost while the
   interrupt was masked, a click that landed as the guest started, the
   host's window not focused) sits in the output buffer, and the 8042 sends
   nothing else until it is read: the keyboard seems dead. Every 32 ms: a
   byte waiting since the last look with no IRQ 1 or 12 between is read and
   handed on as the interrupt would have. */
static u32 kbc_irqs;
static void kbc_unstick(void)
{
    static u32 seen;
    static int waiting, logged;
    u8 st = inb(0x64);
    if (st == 0xFF || !(st & 1)) { waiting = 0; return; }
    if (waiting && kbc_irqs == seen) {
        u8 b = inb(0x60);
        if (logged++ < 4) kprintf("kbd: a %s byte (%02x) waited with no interrupt: taken\n", st & 0x20 ? "mouse" : "keyboard", b);
        if (st & 0x20) mouse_ps2_byte(b); else vkbd_real_scancode(b);
        waiting = 0;
        return;
    }
    waiting = 1;
    seen = kbc_irqs;
}

/* Deliver a pending virtual IRQ; returns the frame to resume (a DPMI
   client's protected-mode handler may mean a mode switch). */
u32 stat_pmirq, stat_rmirq, stat_reflirq, stat_irq0, stat_vec8;
int dpmi_route_rm_irqs = -1;
static struct regs *deliver(struct regs *r)
{
    vkbd_refill();
    if (!vif) return r;
    int pm = !(r->eflags & EFL_VM);
    /* norouteirq: no IRQ taken in real mode goes straight to a PM handler;
       norouteirq=0,1: only those IRQs (to find which one a program minds) */
    static u16 route_mask = 0xFFFF;
    if (dpmi_route_rm_irqs < 0) {
        const char *o = strstr(cmdline, "norouteirq");
        dpmi_route_rm_irqs = !o;
        if (o && o[10] == '=') {
            route_mask = 0xFFFF;
            for (const char *p = o + 11; *p >= '0' && *p <= '9'; ) {
                int n = 0;
                while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
                if (n < 16) route_mask &= (u16)~(1u << n);
                if (*p == ',') p++;
            }
            dpmi_route_rm_irqs = 1;
            kprintf("DPMI: IRQs taken in real mode not routed to PM handlers: mask %04x\n", (u16)~route_mask);
        } else if (o) route_mask = 0;
    }
    /* deferirq: IRQ 0 taken while a DPMI client waits in DOS for a call it
       made goes to its PM handler when the call returns, not in the
       middle of DOS (Dark Forces' cutscenes end early otherwise; norouteirq=0
       fixes them but loses those ticks). Taken back from the PIC meanwhile,
       so the keyboard and the rest still get through. */
    static int defer = -1, deferred;
    if (defer < 0) defer = strstr(cmdline, "deferirq") != 0;
    if (deferred && pm) { deferred = 0; vpic_raise(0); }
    int dpmi_in_rm_call(void);
    int vec = vpic_pending();
    if (defer && vec >= 0 && !pm && chosen_irq_of(vec) == 0 && dpmi_pm_hooked(vec) && dpmi_in_rm_call()) {
        vpic_lower(0);
        deferred = 1;
        vec = vpic_pending();
    }
    if (vec < 0) {
        if (mouse_callback_due()) {
            if (!pm) mouse_start_callback(r);
            else return dpmi_rm_iret_call(r, 0xF000, mouse_begin_callback());   /* DOS/4GW games */
        }
        return r;
    }
    vpic_ack(vec);
    if (vec == vpic_base0()) stat_vec8++;
    int irqn = chosen_irq_of(vec);
    if (dpmi_pm_hooked(vec) && (pm || (dpmi_route_rm_irqs && irqn >= 0 && ((route_mask >> irqn) & 1)))) { stat_pmirq++; return dpmi_hw_interrupt(r, vec); }
    if (pm) { stat_reflirq++; return dpmi_reflect_irq(r, vec); }
    stat_rmirq++;
    v86_reflect(r, vec);
    return r;
}

/* EXEPACK loaded with INT 21h AX=4B03h (load overlay) below 64 KiB.
   EXEPACK's unpacker counts on segment arithmetic wrapping at 1 MiB: run
   from below 64 KiB with A20 on it reads the HMA instead and stops with
   "Packed file is corrupt" (or runs garbage). FreeDOS turns A20 off
   when it starts a program from AX=4B00h, but not after AX=4B03h, and
   a caller may call DOS again (turning it back on) before it jumps in:
   Life & Death loads its surgery modules over itself that way. So the
   unpacker's first byte becomes a HLT; when the program gets there, A20
   goes off, the byte goes back, and the unpacker runs as on a plain PC. */
static int ovl_pending;
static u16 ovl_cs, ovl_ip, ovl_sp, ovl_seg;
static u32 xp_at;                       /* the HLT put in for the unpacker (0: none) */
static u8 xp_byte;

static const u8 exepack_code[] = { 0x8C, 0xC0, 0x05, 0x10, 0x00, 0x0E, 0x1F, 0xA3, 0x04, 0x00 };

static void exepack_find(u16 seg)
{
    for (u32 p = seg; p < 0x9FFF; p++) {
        u32 a = p << 4, code;
        if (rd16(a + 0x0E) == 0x4252) code = a + 0x10;           /* "RB": 16-byte header */
        else if (rd16(a + 0x10) == 0x4252) code = a + 0x12;      /* 18-byte header (skip_len) */
        else continue;
        int i = 0;
        while (i < (int)sizeof exepack_code && rd8(code + i) == exepack_code[i]) i++;
        if (i < (int)sizeof exepack_code) continue;
        if (xp_at && rd8(xp_at) == 0xF4) wr8(xp_at, xp_byte);    /* an older one never ran */
        xp_at = code; xp_byte = rd8(code);
        wr8(code, 0xF4);
        dbg(1, "EXEPACK overlay at %04x: A20 off when its unpacker (%04x:%04x) starts\n",
            seg, p, code - a);
        return;
    }
}

/* DOS's IRET back to the program that asked for the overlay. */
static void ovl_check_return(struct regs *r)
{
    if (!ovl_pending || (r->cs & 0xFFFF) != ovl_cs || IP(r) != ovl_ip || SP(r) != ovl_sp) return;
    ovl_pending = 0;
    if (!(r->eflags & EFL_CF) && ovl_seg < 0x1000) exepack_find(ovl_seg);
}

/* A HLT: the one in front of an EXEPACK unpacker? */
static int exepack_hlt(struct regs *r, u16 ip0)
{
    if (!xp_at || LIN(r->cs, ip0) != xp_at || rd8(xp_at) != 0xF4) return 0;
    wr8(xp_at, xp_byte);
    xp_at = 0;
    xms_exec_a20_off();
    IP(r) = ip0;
    return 1;
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
    if (n == 0x2F && (AX(r) == 0x4300 || AX(r) == 0x4310) && !xms_hidden) {
        bios_service(r, 0x2F, 0);
        return;
    }
    if (n == 0x2F && AX(r) == 0x1687) { dpmi_detect(r); return; }    /* DPMI host */
    if (n == 0x2F && AX(r) == 0x1680) { idle_wait(); AL(r) = 0; return; }   /* release time slice: idle */
    if (n == 0x21 && AH(r) == 0x4B) hd_dos_up();                        /* a program runs: DOS has its drives */
    if (n == 0x21 && AX(r) == 0x4B03 && !xms_hidden) {                   /* overlay: see ovl_check_return */
        ovl_pending = 1; ovl_cs = (u16)r->cs; ovl_ip = IP(r); ovl_sp = SP(r);
        ovl_seg = rd16(LIN(r->v86_es, BX(r)));                          /* the parameter block: load segment */
    }
    if (n == 0x28) guest_idle_poll();                                    /* DOS idle (then the vector, for TSRs) */
    if (n == 0x2F && AX(r) == 0x5644) { cd_api(r); return; }            /* VMCD.SYS / VMCD.COM */
    if (n == 0x2F && AH(r) == 0x11 && fs9p_redir(r)) return;           /* DOS's redirector call for a 9p drive */
    if (n == 0x21 && AH(r) == 0x71 && fs9p_lfn(r)) return;             /* long names on a 9p drive */
    if (n == 0x2F && AX(r) == 0x5652) { fs9p_api(r); return; }         /* VMDOSNET.COM */
    if (n == 0x2F && AX(r) == 0x5645) { ems_query(r); return; }         /* VMEMS.SYS */
    if (n == 0x2F && AX(r) == 0x5646) { fd_api(r); return; }            /* VMFD.COM */
    if (n == 0x2F && AX(r) == 0x5647) { hd_api(r); return; }            /* VMHD.SYS / VMHD.COM */
    if (n == 0x2F && AX(r) == 0x5642) {                                 /* VMSB */
        extern int sound_sb_api(int bx);
        extern int audio_capture_stats(u32 *peak, u32 *clips, u32 *resyncs);
        extern int audio_capture_boost(int db);
        if (BX(r) == 1) {               /* mic: AX = source, CX = peak, DX = clipped, SI = resyncs */
            u32 pk, cl, rs;
            AX(r) = (u16)audio_capture_stats(&pk, &cl, &rs);
            CX(r) = (u16)pk; DX(r) = (u16)(cl > 0xFFFF ? 0xFFFF : cl); SI(r) = (u16)rs;
        } else if (BX(r) == 3) {        /* mic boost CX dB: AX = what it got (FFFF: none) */
            AX(r) = (u16)audio_capture_boost((short)CX(r));
        } else AX(r) = (u16)sound_sb_api(BX(r));
        BX(r) = 0x564D;
        return;
    }
    if (n == 0x2F && AX(r) == 0x5650) { video_shot_api(r); return; }   /* VMSHOT */
    if (n == 0x2F && AX(r) == 0x5648) {                                 /* VMHV: only returns on failure */
        AX(r) = (u16)hv_start(BL(r));
        BX(r) = 0x564D;
        return;
    }
    if (n == 0x2F && AX(r) == 0x564C) { log_api(r); return; }         /* DMESG */
    if (n == 0x2F && AX(r) == 0x564E) { net_api(r); return; }         /* VMPKT */
    if (n == 0x2F && AX(r) == 0x5649) { info_api(r); return; }        /* LSPCI / LSUSB */
    if (n == 0x2F && AX(r) == 0x5653) {                                 /* VMSPEED */
        AX(r) = (u16)speed_api(BX(r), CX(r) == 0x4850);                  /* CX = 'HP': hundredths of a percent */
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

/* hv.c (VMHV): the same for a guest in real or V86 mode in the VM, whose
   #GP the VM intercepts before the guest's own handler (EMM386's, which
   doesn't do this) sees it. 1 if it was such a string instruction. */
int v86_wrap_string(struct regs *r)
{
    u16 ip = IP(r);
    int op32 = 0, a32 = 0, rep = 0, seg = 3;
    u8 op;
    for (int n = 0; n < 15; n++) {
        op = rd8(LIN(r->cs, ip));
        ip++;
        switch (op) {
        case 0x66: op32 = 1; continue;
        case 0x67: a32 = 1; continue;
        case 0xF2: rep = 1; continue;
        case 0xF3: rep = 2; continue;
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
    if (a32) return 0;
    switch (op) {
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        dbg(1, "segment wrap: string op %02x at %04x:%04x, SI=%04x DI=%04x CX=%04x\n", op, r->cs, IP(r), SI(r), DI(r), CX(r));
        string_wrap(r, op, (op & 1) ? (op32 ? 4 : 2) : 1, seg, rep);
        IP(r) = ip;
        return 1;
    }
    return 0;
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
        ovl_check_return(r);
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
                if (op2 == 0x22 && ((m >> 3) & 7) == 0 && guest_from_floppy) {
                    /* MOV CR0 with PE, from a booted floppy: a mode switch. (DOS
                       programs read CR0 with PE already set and may write it back,
                       e.g. for cache bits: those writes stay ignored.) */
                    u32 *g[8] = { &r->eax, &r->ecx, &r->edx, &r->ebx, &r->esp, &r->ebp, &r->esi, &r->edi };
                    if (*g[m & 7] & 1) raw_pm(r, "MOV CR0 with PE set");
                }
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
        if (op2 == 0x01) {                                          /* LGDT / LIDT / LMSW */
            u8 m = rd8(LIN(r->cs, ip + 1)), reg = (m >> 3) & 7;
            if (reg == 2) raw_pm(r, "LGDT");
            if (reg == 3) raw_pm(r, "LIDT");
            if (reg == 6) raw_pm(r, "LMSW");
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
        if (exepack_hlt(r, ip0)) return;
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

static struct regs *isr_dispatch_inner(struct regs *r);

/* perf: TSC counts the boot CPU spends in the monitor, by cause, and how
   many times; the rest of the time is the guest's. Logged every 5 s. */
enum { PF_IRQ0, PF_IRQ_OTHER, PF_GP_V86, PF_PF_V86, PF_V86_OTHER, PF_GP_PM, PF_PF_PM, PF_PM_OTHER, PF_N };
static const char *const pf_name[PF_N] = { "timer", "other IRQs", "#GP v86", "#PF v86", "v86 other", "#GP pm", "#PF pm", "pm other" };
static u64 pf_tsc[PF_N];
static u32 pf_cnt[PF_N];
enum { PT_USB, PT_NET, PT_SOUND, PT_DISK, PT_VIDEO, PT_OTHER, PT_N };
static const char *const pt_name[PT_N] = { "usb", "net", "sound", "disk", "video", "rest" };
static u64 pt_tsc[PT_N];
#define PERF_PART(slot, stmt) do { if (perf_on) { u64 _t = tsc_read(); stmt; pt_tsc[slot] += tsc_read() - _t; } else { stmt; } } while (0)

static u32 pct(u64 part, u64 whole)          /* tenths of a percent, no 64-bit division */
{
    if (part > whole) part = whole;
    while (whole >> 22) { whole >>= 1; part >>= 1; }
    return whole ? (u32)part * 1000u / (u32)whole : 0;
}

static void perf_report(void)
{
    static u64 since;
    u64 now = tsc_read();
    if (!since) {                             /* the first: start counting from here */
        since = now;
        for (int i = 0; i < 1024; i++) perf_ports[i] = 0;
        for (int i = 0; i < PF_N; i++) { pf_tsc[i] = 0; pf_cnt[i] = 0; }
        for (int i = 0; i < PT_N; i++) pt_tsc[i] = 0;
        return;
    }
    u64 total = now - since, mon = 0;
    since = now;
    char b[400]; int n = 0;
    for (int i = 0; i < PF_N; i++) mon += pf_tsc[i];
    u32 g = pct(total - (mon < total ? mon : total), total);
    n += snprintf(b + n, sizeof b - n, "perf: guest %u.%u%%", g / 10, g % 10);
    for (int i = 0; i < PF_N; i++) {
        if (!pf_cnt[i]) continue;
        u32 p = pct(pf_tsc[i], total);
        n += snprintf(b + n, sizeof b - n, ", %s %u.%u%% (%u/s)", pf_name[i], p / 10, p % 10, pf_cnt[i] / 5);
    }
    kprintf("%s\n", b);
    u64 parts = 0;
    for (int i = 0; i < PT_OTHER; i++) parts += pt_tsc[i];
    pt_tsc[PT_OTHER] = pf_tsc[PF_IRQ0] > parts ? pf_tsc[PF_IRQ0] - parts : 0;
    n = snprintf(b, sizeof b, "perf: timer tick:");
    for (int i = 0; i < PT_N; i++) {
        u32 p = pct(pt_tsc[i], total);
        n += snprintf(b + n, sizeof b - n, " %s %u.%u%%", pt_name[i], p / 10, p % 10);
    }
    kprintf("%s\n", b);
    /* the guest's busiest ports */
    n = snprintf(b, sizeof b, "perf: guest ports/s:");
    for (int k = 0; k < 6; k++) {
        int best = -1;
        for (int i = 0; i < 1024; i++) if (perf_ports[i] && (best < 0 || perf_ports[i] > perf_ports[best])) best = i;
        if (best < 0) break;
        n += snprintf(b + n, sizeof b - n, " %03xh %u", best, perf_ports[best] / 5);
        perf_ports[best] = 0;
    }
    kprintf("%s\n", b);
    for (int i = 0; i < 1024; i++) perf_ports[i] = 0;
    for (int i = 0; i < PF_N; i++) { pf_tsc[i] = 0; pf_cnt[i] = 0; }
    for (int i = 0; i < PT_N; i++) pt_tsc[i] = 0;
}

struct regs *isr_dispatch(struct regs *r)
{
    if (!perf_on || smp_on_ap()) return isr_dispatch_inner(r);
    u32 vec = r->vec;
    int slot = vec == 0x20 ? PF_IRQ0 : vec > 0x20 ? PF_IRQ_OTHER
             : (r->eflags & EFL_VM) ? (vec == 13 ? PF_GP_V86 : vec == 14 ? PF_PF_V86 : PF_V86_OTHER)
             : (vec == 13 ? PF_GP_PM : vec == 14 ? PF_PF_PM : PF_PM_OTHER);
    u64 t0 = tsc_read();
    r = isr_dispatch_inner(r);
    pf_tsc[slot] += tsc_read() - t0;
    pf_cnt[slot]++;
    static u32 last_report;
    if (slot == PF_IRQ0 && ticks - last_report >= 5000) { last_report = ticks; perf_report(); }   /* ticks can skip a value */
    return r;
}

static struct regs *isr_dispatch_inner(struct regs *r)
{
    int from_v86 = (r->eflags & EFL_VM) != 0;
    int from_pm = !from_v86 && (r->cs & 3) == 3;     /* a DPMI client */
    u32 vec = r->vec;

    if (smp_on_ap()) smp_ap_fault(r);                /* the helper CPU takes no interrupts: a fault */
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
            smp_tick();
            vdev_tick();
            if (usb_ready) PERF_PART(PT_SOUND, if (smp_active) sound_irq_poll(); else sound_tick());
            if (usb_ready) PERF_PART(PT_DISK, disk_tick());
            if (net_ready) PERF_PART(PT_NET, net_tick());
            if (usb_ready && (ticks & 7) == 4) { void cdaudio_pump(void); cdaudio_pump(); }   /* CD audio: refill */
            if ((ticks & 31) == 16) kbc_unstick();
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
            kbc_irqs++;
            vkbd_real_scancode(inb(0x60));
        } else if (irq == 3 || irq == 4) {
            serial_irq(irq);                         /* serial=dos: the UART's, for the guest */
        } else if (irq == 12) {
            kbc_irqs++;
            mouse_ps2_byte(inb(0x60));
        }
        if (irq >= 8) outb(0xA0, 0x20);
        outb(0x20, 0x20);
        static int in_usb;
        if (irq == 0 && usb_ready && (ticks & 7) == 0) {
            if (!in_usb) { in_usb = 1; PERF_PART(PT_USB, usb_tick()); in_usb = 0; }
        }
        if (irq == 0 && log_view_req) {                                       /* Ctrl+Shift+F10 */
            extern int vif;
            log_view_req = 0;
            if (video_log_on()) { video_log_toggle(); goto log_done; }        /* closing it */
            if (from_v86) {                       /* where a hung DOS is stuck */
                u32 a = r->cs * 16 + (r->eip & 0xFFFF);
                kprintf("log: guest at %04x:%04x (V86, IF %u), code %02x %02x %02x %02x %02x %02x\n", r->cs, r->eip, vif,
                        *(u8 *)a, *(u8 *)(a + 1), *(u8 *)(a + 2), *(u8 *)(a + 3), *(u8 *)(a + 4), *(u8 *)(a + 5));
            } else kprintf("log: guest at %x:%x (%s)\n", r->cs, r->eip, from_pm ? "protected mode" : "kernel");
            video_log_toggle();
        }
    log_done:
        if (irq == 0 && log_save_req && !in_usb && usb_quiet()) {             /* Ctrl+Shift+S in the log */
            log_save_req = 0;
            const char *e = log_save();
            video_osd(e ? e : "Log saved to C:\\VMDOS\\VMDOS.LOG");
        }
        if (irq == 0 && debug_level >= 2 && ticks % 5000 == 0) {
            extern int vif;
            void vpic_debug(char *buf, int n);
            int dpmi_depth(char *buf, int n);
            char b1[64], b2[64];
            vpic_debug(b1, sizeof b1);
            dpmi_depth(b2, sizeof b2);
            static u32 last_irq0, last_vec8;
            kprintf("[%us] timer: %u/s raised, %u/s delivered\n", ticks / 1000, (stat_irq0 - last_irq0) / 5, (stat_vec8 - last_vec8) / 5);
            last_irq0 = stat_irq0; last_vec8 = stat_vec8;
            kprintf("[%us] irqs pm %u reflected %u rm %u, vif %u, mode %s, %x:%x  %s  %s\n", ticks / 1000,
                    stat_pmirq, stat_reflirq, stat_rmirq, vif, from_v86 ? "v86" : from_pm ? "pm" : "kernel",
                    r->cs, r->eip, b1, b2);
            if (from_pm) { void dpmi_dump_code(u32, u32, int); dpmi_dump_code(r->cs, r->eip - 0x30, 0x60); }
            { void dpmi_timer_report(void); dpmi_timer_report(); }
        }
        if (irq == 0) video_tlb_tick();
        if (irq == 0) v3_tick();                                         /* the Voodoo 3: retraces, its FIFO's wake-up */
        if (irq == 0 && ++refresh_div >= TICK_HZ / 60) {
            refresh_div = 0;
            if (!smp_active) PERF_PART(PT_VIDEO, video_refresh());   /* else the helper CPU's */
        }
        if (irq == 0 && (from_v86 || from_pm)) speed_throttle();   /* last: the tick's work comes out of the wait, not the guest's share */
        goto out;
    }

    if (vec == 1 && !from_pm) {                       /* DPMI's breakpoint (dpmi.c step_arm) hit outside the client */
        u32 dr6;
        __asm__ volatile("mov %%dr6,%0" : "=r"(dr6));
        if (dr6 & 1) { void dpmi_bp_off(void); dpmi_bp_off(); goto out; }
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
    case 6:                                           /* invalid opcode: the guest's INT 6, and the log says what it was */
        if (from_v86) {
            static int told;
            if (told++ < 8) {
                u32 a = r->cs * 16 + (r->eip & 0xFFFF);
                kprintf("invalid opcode in DOS at %04x:%04x: %02x %02x %02x %02x %02x %02x %02x %02x; "
                        "AX=%04x BX=%04x CX=%04x DX=%04x SI=%04x DI=%04x SP=%04x DS=%04x ES=%04x; INT 6 at %04x:%04x\n",
                        r->cs, r->eip & 0xFFFF, rd8(a), rd8(a + 1), rd8(a + 2), rd8(a + 3), rd8(a + 4), rd8(a + 5),
                        rd8(a + 6), rd8(a + 7), AX(r), BX(r), CX(r), DX(r), SI(r), DI(r), SP(r), r->v86_ds & 0xFFFF,
                        r->v86_es & 0xFFFF, rd16(6 * 4 + 2), rd16(6 * 4));
                kprintf("  code before it: %02x %02x %02x %02x %02x %02x %02x %02x\n", rd8(a - 8), rd8(a - 7),
                        rd8(a - 6), rd8(a - 5), rd8(a - 4), rd8(a - 3), rd8(a - 2), rd8(a - 1));
            }
        }
        v86_reflect(r, vec);
        break;
    case 0: case 1: case 5: case 7:                  /* real-mode style: guest's own vector */
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
    const char *hv = strstr(cmdline, "vmhv");             /* vmhv / vmhv=2: boot DOS in VMHV's VM */
    if (hv && (hv == cmdline || hv[-1] == ' ')) {
        int e = hv_start(hv[4] == '=' ? hv[5] : 0);
        kprintf("vmhv: %d; booting DOS as usual\n", e);
    }
    bios_boot(r);
    kprintf("starting guest at %04x:%04x\n", r->cs, IP(r));
    v86_enter(r);
}
