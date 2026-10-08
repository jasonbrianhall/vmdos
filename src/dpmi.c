/* DPMI 0.9 host, for DOS extenders (DOS/4GW, DOS/32A, PMODE/W, CWSDPMI
   clients...). One client at a time.

   The client runs in 16- or 32-bit protected mode at ring 3 with IOPL 0,
   using LDT descriptors. Every port access, CLI/STI, HLT and software INT
   traps (#GP) into this file, the same way the v86 guest's do: INT 31h is
   served here, other interrupts go to the client's protected-mode handlers
   or are reflected to real mode (v86). Hardware interrupts go to the
   client's protected-mode handler if it installed one (on a host stack,
   returning through a trap stub), otherwise to the real-mode handler.

   Memory: DOS memory is linear 0-1 MiB (+HMA) as for the v86 guest; blocks
   from INT 31h/0501h are kernel pages opened to ring 3, linear = physical.

   Mode switches: a guest context (struct ctx) is turned back into an
   interrupt frame at the top of the ring-0 stack (resume); isr_dispatch
   returns that frame. Real-mode excursions (reflected interrupts, 0300h
   calls, DOS memory calls) and protected-mode visits (IRQ handlers,
   real-mode callbacks, exception handlers) nest on a small stack. */
#include "kernel.h"

extern u8 v86_stack_top[];
extern u64 gdt[];

/* ---------------- guest contexts ---------------- */
struct ctx {
    u32 eax, ebx, ecx, edx, esi, edi, ebp, esp, eip, eflags;
    u32 cs, ss, ds, es, fs, gs;
    int pm, vif;
};

static struct regs *switch_frame;
struct regs *dpmi_take_switch(void) { struct regs *r = switch_frame; switch_frame = 0; return r; }

static void get_ctx(struct regs *r, struct ctx *c)
{
    c->eax = r->eax; c->ebx = r->ebx; c->ecx = r->ecx; c->edx = r->edx;
    c->esi = r->esi; c->edi = r->edi; c->ebp = r->ebp;
    c->eip = r->eip; c->eflags = r->eflags; c->cs = r->cs & 0xFFFF;
    c->esp = r->esp; c->ss = r->ss & 0xFFFF;
    c->vif = vif;
    if (r->eflags & EFL_VM) {
        c->pm = 0;
        c->ds = r->v86_ds & 0xFFFF; c->es = r->v86_es & 0xFFFF;
        c->fs = r->v86_fs & 0xFFFF; c->gs = r->v86_gs & 0xFFFF;
    } else {
        c->pm = 1;
        c->ds = r->ds & 0xFFFF; c->es = r->es & 0xFFFF; c->fs = r->fs & 0xFFFF; c->gs = r->gs & 0xFFFF;
    }
}

static int sel_valid_data(u32 sel);
struct regs *dpmi_pm_trap(struct ctx *c, int id, int arg);

/* After a client's CLI it runs single-stepped (TF), so a POPF or IRET that
   turns interrupts back on is seen: at IOPL 0 those can't change IF
   themselves, and PUSHF/CLI/.../POPF is common. */
static int stepping;
static u32 step_cs, step_eip, step_ss, step_esp;

/* Build the frame for c at the top of the ring-0 stack and resume it. */
static struct regs *resume(struct ctx *c)
{
    struct ctx k = *c;                         /* c may live in the old frame's area */
    u32 top = (u32)(uintptr_t)v86_stack_top;
    struct regs *r = (struct regs *)(uintptr_t)(top - sizeof(struct regs) + (k.pm ? 16 : 0));
    memset(r, 0, k.pm ? sizeof(struct regs) - 16 : sizeof(struct regs));
    r->eax = k.eax; r->ebx = k.ebx; r->ecx = k.ecx; r->edx = k.edx;
    r->esi = k.esi; r->edi = k.edi; r->ebp = k.ebp;
    r->eip = k.eip; r->cs = k.cs; r->esp = k.esp; r->ss = k.ss;
    if (k.pm) {
        if (stepping && !k.vif) {
            k.eflags |= EFL_TF;
            step_cs = k.cs; step_eip = k.eip; step_ss = k.ss; step_esp = k.esp;
        } else if (stepping) {
            stepping = 0;
            k.eflags &= ~EFL_TF;
        }
        r->eflags = (k.eflags & 0x0DD5) | EFL_IF | 2;
        r->ds = sel_valid_data(k.ds) ? k.ds : 0;
        r->es = sel_valid_data(k.es) ? k.es : 0;
        r->fs = sel_valid_data(k.fs) ? k.fs : 0;
        r->gs = sel_valid_data(k.gs) ? k.gs : 0;
    } else {
        r->eflags = (k.eflags & 0x0DD5 & ~EFL_TF) | EFL_VM | EFL_IF | 2;
        r->eip &= 0xFFFF; r->esp &= 0xFFFF;
        r->v86_ds = k.ds; r->v86_es = k.es; r->v86_fs = k.fs; r->v86_gs = k.gs;
    }
    vif = k.vif;
    switch_frame = r;
    return r;
}

/* ---------------- the LDT ---------------- */
#define LDT_N 2048
static u64 *ldt;
static u8 ldt_used[LDT_N];
#define SEL(i) ((u32)((i) << 3) | 7)
#define IDX(s) (((s) & 0xFFFF) >> 3)
#define HOST_CS_I 1          /* host stubs: base F0000h, client's bitness */
#define HOST_SS_I 2          /* the locked stack for interrupts */
#define HOST_DS_I 3          /* scratch: real-mode stack for callbacks */
#define FIRST_FREE 16
#define HOST_CS SEL(HOST_CS_I)
#define HOST_SS SEL(HOST_SS_I)

static u64 make_desc(u32 base, u32 limit, u8 access, u8 flags)
{
    if (limit > 0xFFFFF) { limit >>= 12; flags |= 0x8; }
    return (limit & 0xFFFF) | ((u64)(base & 0xFFFFFF) << 16) | ((u64)access << 40) |
           ((u64)((limit >> 16) & 0xF) << 48) | ((u64)(flags & 0xF) << 52) | ((u64)(base >> 24) << 56);
}
static u32 d_base(u64 d) { return (u32)((d >> 16) & 0xFFFFFF) | (u32)((d >> 56) & 0xFF) << 24; }
static __attribute__((unused)) u32 d_limit(u64 d)
{
    u32 l = (u32)(d & 0xFFFF) | (u32)((d >> 48) & 0xF) << 16;
    return (d & (1ull << 55)) ? (l << 12) | 0xFFF : l;
}
static int d_big(u64 d) { return (d >> 54) & 1; }

extern u64 gdt[];
static u64 *desc_of(u32 sel)
{
    sel &= 0xFFFF;
    if ((sel & ~3u) == 0x40) return &gdt[8];       /* selector 0040h: the BIOS data area */
    if (!(sel & 4) || !ldt) return 0;
    u32 i = IDX(sel);
    return i < LDT_N && ldt_used[i] ? &ldt[i] : 0;
}
static int sel_valid_data(u32 sel)
{
    if (!(sel & 0xFFFC)) return 1;              /* null */
    u64 *d = desc_of(sel);
    return d && (*d & (1ull << 47)) && ((*d >> 44) & 1) && (((*d >> 40) & 0xA) != 0x8);   /* present, code/data, readable */
}
static u32 sel_base(u32 sel) { u64 *d = desc_of(sel); return d ? d_base(*d) : 0; }
static int sel_big(u32 sel) { u64 *d = desc_of(sel); return d ? d_big(*d) : 0; }

static int alloc_desc(int n)
{
    for (int i = FIRST_FREE; i + n <= LDT_N; i++) {
        int ok = 1;
        for (int j = 0; j < n && ok; j++) ok = !ldt_used[i + j];
        if (!ok) continue;
        for (int j = 0; j < n; j++) {
            ldt_used[i + j] = 1;
            ldt[i + j] = make_desc(0, 0, 0xF2, 0);      /* data, present, DPL 3 */
        }
        return i;
    }
    return -1;
}

static int new_sel(u32 base, u32 limit, int code, int big)
{
    int i = alloc_desc(1);
    if (i < 0) return 0;
    ldt[i] = make_desc(base, limit, code ? 0xFA : 0xF2, big ? 4 : 0);
    return SEL(i);
}

static void ldt_install(void)
{
    if (!ldt) ldt = phys_alloc(LDT_N * 8);
    u32 base = (u32)(uintptr_t)ldt, limit = LDT_N * 8 - 1;
    gdt[4] = make_desc(base, limit, 0x82, 0);
    __asm__ volatile("lldt %w0" ::"r"(0x20));
}

/* ---------------- client state ---------------- */
static int client, client32;
static u16 psp_seg, env_seg, rm_stack_seg;
static u32 locked_stack;                       /* linear */
#define LOCKED_SIZE 0x10000
static struct { u16 sel; u32 off; } pm_vec[256], exc_vec[32];
static struct { u16 seg, sel; } seg_cache[64];
static struct ctx entry_ctx;                    /* the caller of the switch entry */
static u16 entry_ax, entry_es;

static u16 stubs(int slot) { return rd16(0xF0000 + slot); }
#define S_ENTRY 0x20E
#define S_RMRET 0x210
#define S_RAWRM 0x212
#define S_CB0 0x214
#define S_HWRET 0x216
#define S_EXCRET 0x218
#define S_CBRET 0x21A
#define S_RAWPM 0x21C
#define S_PMRETF 0x21E
#define S_RMRETF 0x220
#define S_DEFINT 0x222
#define S_INTSTUB 0x224

/* Run INT n in real mode through the stub: the monitor's INT handling
   (XMS, DPMI and mouse answers, BIOS shortcuts) applies as for any INT. */
static struct regs *go_real_int(struct ctx *rm, int n)
{
    u16 off = stubs(S_INTSTUB);
    wr8(0xF0000 + off + 1, (u8)n);
    rm->cs = 0xF000; rm->eip = off;
    return resume(rm);
}

static u32 lin(u32 sel, u32 off) { return sel_base(sel) + off; }

/* Is [a, a+n) something the client may hand us? (DOS memory or one of its blocks) */
#define MAX_BLOCKS 512
static struct { u32 lin, size; u8 used; } blk[MAX_BLOCKS];
u8 *xms_pool_range(u32 *len);
static int alloc_block(u32 size);
static int lin_ok(u32 a, u32 n)
{
    if (a + n < a) return 0;
    if (a + n <= GUEST_TOP) return 1;
    {
        u32 xl;
        u32 xb = (u32)(uintptr_t)xms_pool_range(&xl);
        if (xl && a >= xb && a + n <= xb + xl) return 1;
    }
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (blk[i].size && blk[i].used && a >= blk[i].lin && a + n <= blk[i].lin + blk[i].size) return 1;
    if (locked_stack && a >= locked_stack && a + n <= locked_stack + LOCKED_SIZE) return 1;
    return 0;
}

/* ---------------- stacks ---------------- */
static int ss_big(struct ctx *c) { return c->pm ? sel_big(c->ss) : 0; }
static u32 stack_lin(struct ctx *c, u32 sp) { return c->pm ? sel_base(c->ss) + sp : (c->ss << 4) + (sp & 0xFFFF); }

static void push(struct ctx *c, u32 v, int wide)
{
    int big = ss_big(c);
    u32 sp = big ? c->esp : (c->esp & 0xFFFF);
    sp -= wide ? 4 : 2;
    if (!big) sp &= 0xFFFF;
    u32 a = stack_lin(c, sp);
    if (lin_ok(a, 4)) { if (wide) wr32(a, v); else wr16(a, (u16)v); }
    c->esp = big ? sp : (c->esp & 0xFFFF0000u) | sp;
}
static u32 pop(struct ctx *c, int wide)
{
    int big = ss_big(c);
    u32 sp = big ? c->esp : (c->esp & 0xFFFF);
    u32 a = stack_lin(c, sp), v = 0;
    if (lin_ok(a, 4)) v = wide ? rd32(a) : rd16(a);
    sp += wide ? 4 : 2;
    if (!big) sp &= 0xFFFF;
    c->esp = big ? sp : (c->esp & 0xFFFF0000u) | sp;
    return v;
}

/* ---------------- nesting: excursions to real mode and visits to PM ---------------- */
enum { X_REFLECT, X_REFLECT_HW, X_SIM, X_DOSALLOC, X_DOSFREE, X_DOSRESIZE, X_HW, X_CB, X_EXC };
#define XDEPTH 24
static struct xent { int kind; struct ctx saved; u32 a, b, lk; } xs[XDEPTH];   /* lk: locked-stack ESP a PM visit started at */
static int xdepth;

static struct xent *xpush(int kind, struct ctx *saved)
{
    if (xdepth >= XDEPTH) panic("DPMI: nesting too deep");
    struct xent *x = &xs[xdepth++];
    x->kind = kind;
    x->saved = *saved;
    x->lk = 0;
    return x;
}

static u16 rm_sp(void) { return (u16)(0x2000 - (xdepth & 3) * 0x800); }   /* in rm_stack_seg */
#define RM_STACK_PARAS 0x200

/* Start a real-mode excursion: run CS:IP in v86 with an IRET (iret=1) or
   RETF return frame pointing at the dpmi_rmret stub. */
static struct regs *go_real(struct ctx *rm, u32 cs, u32 ip, int iret)
{
    if (iret) push(rm, (rm->eflags & 0x0DD5) | 2 | (rm->vif ? EFL_IF : 0), 0);
    push(rm, 0xF000, 0);
    push(rm, stubs(S_RMRET), 0);
    rm->cs = cs; rm->eip = ip;
    return resume(rm);
}

static void rm_from_pm(struct ctx *rm, struct ctx *pm)
{
    *rm = *pm;
    rm->pm = 0;
    rm->eflags &= ~EFL_TF;
    rm->ds = rm->es = rm->fs = rm->gs = rm_stack_seg;
    rm->ss = rm_stack_seg;
    rm->esp = rm_sp();
}

/* Reflect interrupt n into real mode (registers go as they are). */
static struct regs *reflect(struct ctx *c, int n, int kind)
{
    xpush(kind, c);
    struct ctx rm;
    rm_from_pm(&rm, c);
    rm.vif = c->vif;                            /* the INT itself clears IF for the handler */
    return go_real_int(&rm, n);
}

/* Where a new visit to protected mode may start on the locked stack: below
   whatever is still live there. That is the context we come from when it
   is on the locked stack, else the innermost saved context that is (a PM
   handler that went to real mode, where an IRQ or a callback now brings us
   back to PM). Starting at the top in that case overwrote the outer
   handler's saved registers and IRET frame (DOS/4GW in Warcraft II popped
   garbage selectors). */
#define LOCKED_GAP 0x800      /* below a live visit's start: what its handler pushed before switching stacks */
static u32 locked_esp(const struct ctx *from)
{
    if (from->pm && (from->ss & 0xFFFF) == HOST_SS) return from->esp;
    u32 e = LOCKED_SIZE - 16;
    for (int i = 0; i < xdepth; i++) {
        if (xs[i].saved.pm && (xs[i].saved.ss & 0xFFFF) == HOST_SS && xs[i].saved.esp < e) e = xs[i].saved.esp;
        if (xs[i].lk && xs[i].lk - LOCKED_GAP < e) e = xs[i].lk - LOCKED_GAP;
    }
    return e & ~3u;
}

/* Enter a protected-mode handler on the locked stack, returning (IRET or
   RETF) into the stub at HOST_CS:ret_slot. */
static struct regs *visit_pm(struct ctx *from, int kind, u16 sel, u32 off, int ret_slot, int iret, struct ctx *pmregs)
{
    xpush(kind, from);
    struct ctx c = *pmregs;
    c.pm = 1;
    c.eflags &= ~EFL_TF;
    c.esp = locked_esp(from);
    if (c.esp < 0x400) panic("DPMI: locked stack overflow (nesting %d)", xdepth);
    c.ss = HOST_SS;
    if (iret) push(&c, (from->eflags & 0x0DD5) | 2 | (from->vif ? EFL_IF : 0), client32);
    push(&c, HOST_CS, client32);
    push(&c, stubs(ret_slot), client32);
    c.cs = sel; c.eip = off;
    c.vif = 0;
    xs[xdepth - 1].lk = c.esp;
    return resume(&c);
}

/* ---------------- detection and entry ---------------- */
static int cpu_type(void)
{
    u32 a, b;
    __asm__ volatile("pushfl; pop %0; mov %0,%1; xor $0x200000,%0; push %0; popfl; pushfl; pop %0; push %1; popfl"
                     : "=&r"(a), "=&r"(b));
    if (!((a ^ b) & 0x200000)) return 3;
    u32 eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    int fam = (eax >> 8) & 15;
    return fam > 6 ? 6 : fam < 4 ? 4 : fam;
}

void dpmi_detect(struct regs *r)
{
    AX(r) = 0;
    BX(r) = 1;                                  /* 32-bit programs supported */
    CL(r) = (u8)cpu_type();
    DX(r) = 0x005A;                             /* 0.90 */
    SI(r) = RM_STACK_PARAS;
    r->v86_es = 0xF000;
    DI(r) = stubs(S_ENTRY);
    dbg(1, "DPMI: detection\n");
}

static void reset_client(void)
{
    if (psp_seg && env_seg) wr16(LIN(psp_seg, 0x2C), env_seg);
    for (int i = 0; i < MAX_BLOCKS; i++) if (blk[i].used == 1) blk[i].used = 0;  /* memory stays for reuse */
    memset(ldt_used, 0, sizeof ldt_used);
    memset(seg_cache, 0, sizeof seg_cache);
    xdepth = 0;
    client = 0;
    psp_seg = env_seg = 0;
}

static int seg_to_sel(u16 seg)
{
    for (int i = 0; i < 64; i++) if (seg_cache[i].sel && seg_cache[i].seg == seg) return seg_cache[i].sel;
    int s = new_sel((u32)seg << 4, 0xFFFF, 0, 0);
    for (int i = 0; i < 64; i++) if (!seg_cache[i].sel) { seg_cache[i].seg = seg; seg_cache[i].sel = (u16)s; break; }
    return s;
}

static struct regs *enter_pm(struct regs *r)
{
    struct ctx c = entry_ctx;
    psp_seg = BX(r);
    client32 = entry_ax & 1;
    rm_stack_seg = entry_es;
    ldt_install();
    memset(ldt_used, 0, sizeof ldt_used);
    ldt_used[0] = 1;
    ldt_used[HOST_CS_I] = ldt_used[HOST_SS_I] = ldt_used[HOST_DS_I] = 1;
    ldt[HOST_CS_I] = make_desc(0xF0000, 0xFFFF, 0xFA, client32 ? 4 : 0);
    if (!locked_stack) {
        int b = alloc_block(LOCKED_SIZE);
        if (b < 0) panic("DPMI: out of memory for the host stack");
        blk[b].used = 2;                       /* the host's: never freed */
        locked_stack = blk[b].lin;
    }
    ldt[HOST_SS_I] = make_desc(locked_stack, LOCKED_SIZE - 1, 0xF2, 4);
    ldt[HOST_DS_I] = make_desc(0, 0xFFFF, 0xF2, 0);
    for (int i = 0; i < 256; i++) pm_vec[i].sel = 0;
    for (int i = 0; i < 32; i++) exc_vec[i].sel = 0;
    memset(seg_cache, 0, sizeof seg_cache);
    xdepth = 0;

    /* the client continues where it called us, now in protected mode */
    struct ctx p = c;
    p.pm = 1;
    p.cs = new_sel((u32)c.cs << 4, 0xFFFF, 1, 0);
    p.ss = seg_to_sel((u16)c.ss);
    p.ds = seg_to_sel((u16)c.ds);
    p.es = new_sel((u32)psp_seg << 4, 0xFF, 0, 0);
    p.fs = p.gs = 0;
    env_seg = rd16(LIN(psp_seg, 0x2C));
    if (env_seg) wr16(LIN(psp_seg, 0x2C), (u16)new_sel((u32)env_seg << 4, 0xFFFF, 0, 0));
    p.eflags &= ~EFL_CF;
    client = 1;
    kprintf("DPMI: %u-bit client entered protected mode (PSP %04x, SS:ESP %x:%x)\n", client32 ? 32 : 16, psp_seg, c.ss, c.esp);
    return resume(&p);
}

/* ---------------- termination ---------------- */
static struct regs *terminate(struct ctx *c, u8 code, const char *why)
{
    if (why) {
        kprintf("DPMI: %s\n", why);
        video_puts("\r\nDPMI client stopped: ");
        video_puts(why);
        video_puts("\r\n");
    }
    u16 psp = psp_seg;
    reset_client();
    struct ctx rm;
    memset(&rm, 0, sizeof rm);
    rm.ss = rm_stack_seg; rm.esp = 0x2000;
    rm.ds = rm.es = psp;
    rm.eax = 0x4C00 | code;
    rm.eflags = 2;
    rm.vif = 1;
    (void)c;
    return go_real(&rm, rd16(0x21 * 4 + 2), rd16(0x21 * 4), 1);   /* never comes back */
}

/* ---------------- INT 31h ---------------- */
static void ok(struct ctx *c) { c->eflags &= ~EFL_CF; }
static void err(struct ctx *c, u16 code) { c->eflags |= EFL_CF; c->eax = (c->eax & 0xFFFF0000u) | code; }
#define rAX (c->eax & 0xFFFF)
#define rBX (c->ebx & 0xFFFF)
#define rCX (c->ecx & 0xFFFF)
#define rDX (c->edx & 0xFFFF)
#define rSI (c->esi & 0xFFFF)
#define rDI (c->edi & 0xFFFF)
#define SET16(f, v) (c->f = (c->f & 0xFFFF0000u) | (u16)(v))
#define rEDI (client32 ? c->edi : rDI)
#define rESI (client32 ? c->esi : rSI)
#define rEDX (client32 ? c->edx : rDX)

static int desc_rights_ok(u8 acc) { return (acc & 0x60) == 0x60 && (acc & 0x10); }

/* Client memory lives at linear 2-16 MiB (the "window"), backed by kernel
   pages from anywhere: DOS/4GW's DOS/16M core keeps 24-bit addresses.
   Blocks that don't fit there (Quake's DJGPP heap grows past 14 MiB) are
   kernel heap pages above 16 MiB, at their own (identity-mapped) address,
   opened to ring 3. Blocks keep their place when freed, for reuse. The
   free-memory report offers up to dpmi=MB (default 256) in all, but its
   "largest block" stays what the window has, so DOS/4GW stays inside it. */
#define WIN_LO 0x00200000u
#define WIN_HI 0x01000000u
#define RESERVE (16u << 20)             /* heap the kernel keeps for itself */

static u32 dpmi_cap(void)
{
    static u32 cap;
    if (!cap) {
        cap = 256;
        const char *o = strstr(cmdline, "dpmi=");
        if (o) { cap = 0; for (o += 5; *o >= '0' && *o <= '9'; o++) cap = cap * 10 + (u32)(*o - '0'); }
        if (cap < 4) cap = 4;
        if (cap > 2048) cap = 2048;
        cap <<= 20;
    }
    return cap;
}
static u32 dpmi_used(void)
{
    u32 n = 0;
    for (int i = 0; i < MAX_BLOCKS; i++) if (blk[i].size && blk[i].used == 1) n += blk[i].size;
    return n;
}
/* What a client could still get, in bytes: the cap, and the kernel's heap. */
static u32 dpmi_avail(void)
{
    u32 cap = dpmi_cap(), used = dpmi_used(), f = phys_free();
    u32 a = used < cap ? cap - used : 0;
    u32 h = f > RESERVE ? f - RESERVE : 0;
    for (int i = 0; i < MAX_BLOCKS; i++) if (blk[i].size && !blk[i].used) h += blk[i].size;   /* freed, reusable */
    return a < h ? a : h;
}

static u32 win_find(u32 size)
{
    u32 a = WIN_LO;
    for (;;) {
        if (a + size > WIN_HI || a + size < a) return 0;
        int clash = 0;
        for (int i = 0; i < MAX_BLOCKS; i++)
            if (blk[i].size && a < blk[i].lin + blk[i].size && blk[i].lin < a + size) {
                a = blk[i].lin + blk[i].size;
                clash = 1;
            }
        if (!clash) return a;
    }
}

static u32 win_largest(void)
{
    u32 best = 0, a = WIN_LO;
    while (a < WIN_HI) {
        u32 next = WIN_HI;
        int inside = 0;
        for (int i = 0; i < MAX_BLOCKS; i++) {
            if (!blk[i].size) continue;
            if (blk[i].lin <= a && a < blk[i].lin + blk[i].size) { a = blk[i].lin + blk[i].size; inside = 1; break; }
            if (blk[i].lin > a && blk[i].lin < next) next = blk[i].lin;
        }
        if (inside) continue;
        if (next - a > best) best = next - a;
        a = next;
    }
    for (int i = 0; i < MAX_BLOCKS; i++)          /* freed blocks can be handed out again */
        if (blk[i].size && !blk[i].used && blk[i].size > best) best = blk[i].size;
    return best;
}

static int new_slot(void)
{
    for (int i = 0; i < MAX_BLOCKS; i++) if (!blk[i].size) return i;
    return -1;
}

static int alloc_block(u32 size)
{
    size = (size + 4095) & ~4095u;
    if (!size) size = 4096;
    int fit = -1;
    for (int i = 0; i < MAX_BLOCKS; i++)          /* reuse a freed block of this size or larger */
        if (blk[i].size && !blk[i].used && blk[i].size >= size && (fit < 0 || blk[i].size < blk[fit].size)) fit = i;
    if (fit >= 0) { blk[fit].used = 1; memset(gptr(blk[fit].lin), 0, blk[fit].size); return fit; }
    if (size > dpmi_avail()) return -1;
    if (phys_free() < size + RESERVE) return -1;
    int i = new_slot();
    if (i < 0) return -1;
    u32 lin = win_find(size);
    u8 *p = phys_try_alloc(size);
    if (!p) return -1;
    if (lin) for (u32 o = 0; o < size; o += 4096) map_page(lin + o, (u32)(uintptr_t)p + o, 7);
    else { lin = (u32)(uintptr_t)p; set_user(lin, size, 1); }       /* above 16 MiB, where it is */
    blk[i].lin = lin; blk[i].size = size; blk[i].used = 1;
    memset(gptr(lin), 0, size);
    return i;
}

/* Grow block o in place: window blocks into free window space after them. */
static int grow_in_place(int o, u32 size)
{
    u32 end = blk[o].lin + blk[o].size, more = size - blk[o].size;
    if (blk[o].lin < WIN_LO || end + more > WIN_HI || end + more < end) return 0;
    for (int i = 0; i < MAX_BLOCKS; i++)
        if (i != o && blk[i].size && blk[i].lin < end + more && end < blk[i].lin + blk[i].size) return 0;
    if (more > dpmi_avail() || phys_free() < more + RESERVE) return 0;
    u8 *p = phys_try_alloc(more);
    if (!p) return 0;
    for (u32 a = 0; a < more; a += 4096) map_page(end + a, (u32)(uintptr_t)p + a, 7);
    memset(gptr(end), 0, more);
    blk[o].size = size;
    return 1;
}

static struct regs *sim_real(struct ctx *c, int kind_call)   /* 0300h / 0301h / 0302h */
{
    u32 s = lin(c->es, rEDI);
    if (!lin_ok(s, 0x32)) { err(c, 0x8021); return resume(c); }
    struct xent *x = xpush(X_SIM, c);
    x->a = s;
    struct ctx rm;
    memset(&rm, 0, sizeof rm);
    rm.edi = rd32(s); rm.esi = rd32(s + 4); rm.ebp = rd32(s + 8);
    rm.ebx = rd32(s + 0x10); rm.edx = rd32(s + 0x14); rm.ecx = rd32(s + 0x18); rm.eax = rd32(s + 0x1C);
    rm.eflags = rd16(s + 0x20);
    rm.es = rd16(s + 0x22); rm.ds = rd16(s + 0x24); rm.fs = rd16(s + 0x26); rm.gs = rd16(s + 0x28);
    rm.ss = rd16(s + 0x30); rm.esp = rd16(s + 0x2E);
    if (!rm.ss && !rm.esp) { rm.ss = rm_stack_seg; rm.esp = rm_sp(); }
    rm.vif = 0;
    dbg(2, "DPMI %04x: int/call %02x AX=%04x BX=%04x CX=%04x DX=%04x DS=%04x ES=%04x\n", kind_call,
        c->ebx & 0xFF, rm.eax & 0xFFFF, rm.ebx & 0xFFFF, rm.ecx & 0xFFFF, rm.edx & 0xFFFF, rm.ds, rm.es);
    /* copy CX words of stack parameters */
    u32 words = rCX, from = lin(c->ss, sel_big(c->ss) ? c->esp : (c->esp & 0xFFFF));
    rm.esp = (rm.esp - words * 2) & 0xFFFF;
    if (words && lin_ok(from, words * 2)) memmove(gptr((rm.ss << 4) + rm.esp), gptr(from), words * 2);
    if (kind_call == 0x0300) return go_real_int(&rm, c->ebx & 0xFF);
    return go_real(&rm, rd16(s + 0x2C), rd16(s + 0x2A), kind_call == 0x0302);
}

static struct regs *dos_call(struct ctx *c, int kind, u16 ax, u16 bx, u16 es)
{
    xpush(kind, c);
    struct ctx rm;
    rm_from_pm(&rm, c);
    rm.eax = ax; rm.ebx = bx; rm.es = es;
    rm.vif = 1;
    return go_real_int(&rm, 0x21);
}

#define MAX_CB 16
static struct { u16 sel; u32 off; u16 ssel; u32 soff; u8 used; } cbs[MAX_CB];

static struct regs *int31(struct ctx *c)
{
    u16 fn = (u16)rAX;
    dbg(2, "DPMI %04x BX=%04x CX=%04x DX=%04x ES:EDI=%x:%x DS:ESI=%x:%x\n", fn, rBX, rCX, rDX, c->es, c->edi, c->ds, c->esi);
    ok(c);
    switch (fn) {
    case 0x0000: {
        int n = rCX ? rCX : 1, i = alloc_desc(n);
        if (i < 0) { err(c, 0x8011); break; }
        dbg(2, "  -> %x (%d)\n", SEL(i), n);
        SET16(eax, SEL(i)); break; }
    case 0x0001: {
        u64 *d = desc_of(rBX);
        if (!d || IDX(rBX) < FIRST_FREE) { err(c, 0x8022); break; }
        ldt_used[IDX(rBX)] = 0;
        for (int i = 0; i < 64; i++) if (seg_cache[i].sel == rBX) seg_cache[i].sel = 0;
        break; }
    case 0x0002: { int s = seg_to_sel((u16)rBX); if (!s) err(c, 0x8011); else SET16(eax, s); break; }
    case 0x0003: SET16(eax, 8); break;
    case 0x0004: case 0x0005: break;
    case 0x0006: { u64 *d = desc_of(rBX); if (!d) { err(c, 0x8022); break; }
        u32 b = d_base(*d); SET16(ecx, b >> 16); SET16(edx, b); break; }
    case 0x0007: { u64 *d = desc_of(rBX); if (!d || IDX(rBX) < FIRST_FREE) { err(c, 0x8022); break; }
        u32 b = rCX << 16 | rDX;
        *d = (*d & ~0xFF0000FFFFFF0000ull) | ((u64)(b & 0xFFFFFF) << 16) | ((u64)(b >> 24) << 56); break; }
    case 0x0008: { u64 *d = desc_of(rBX); if (!d || IDX(rBX) < FIRST_FREE) { err(c, 0x8022); break; }
        u32 l = rCX << 16 | rDX;
        if (l > 0xFFFFF && (l & 0xFFF) != 0xFFF) { err(c, 0x8025); break; }
        u64 acc = (*d >> 40) & 0xFF, fl = (*d >> 52) & 0x7;
        *d = make_desc(d_base(*d), l, (u8)acc, (u8)fl); break; }
    case 0x0009: { u64 *d = desc_of(rBX); if (!d || IDX(rBX) < FIRST_FREE) { err(c, 0x8022); break; }
        u8 acc = (u8)rCX, hi = (u8)(rCX >> 8);
        if (!desc_rights_ok(acc)) { err(c, 0x8021); break; }
        *d = (*d & ~(0xFFull << 40) & ~(0xDull << 52)) | ((u64)acc << 40) | ((u64)(hi >> 4 & 0xD) << 52); break; }
    case 0x000A: { u64 *d = desc_of(rBX); if (!d) { err(c, 0x8022); break; }
        int i = alloc_desc(1); if (i < 0) { err(c, 0x8011); break; }
        ldt[i] = (*d & ~(0xFFull << 40)) | ((u64)0xF2 << 40);
        SET16(eax, SEL(i)); break; }
    case 0x000B: { u64 *d = desc_of(rBX); u32 a = lin(c->es, rEDI);
        if (!d || !lin_ok(a, 8)) { err(c, 0x8022); break; }
        wr32(a, (u32)*d); wr32(a + 4, (u32)(*d >> 32));
        dbg(2, "  000b %x -> base %x acc %x\n", rBX, d_base(*d), (u32)(*d >> 40) & 0xFF); break; }
    case 0x000C: { u64 *d = desc_of(rBX); u32 a = lin(c->es, rEDI);
        if (!d || IDX(rBX) < FIRST_FREE || !lin_ok(a, 8)) { err(c, 0x8022); break; }
        u64 v = rd32(a) | (u64)rd32(a + 4) << 32;
        dbg(2, "  000c %x <- base %x limit %x acc %x fl %x\n", rBX, d_base(v), (u32)(v & 0xFFFF) | (u32)((v >> 32) & 0xF0000), (u32)(v >> 40) & 0xFF, (u32)(v >> 52) & 0xF);
        if (!desc_rights_ok((u8)(v >> 40))) { err(c, 0x8021); dbg(2, "  000c refused\n"); break; }
        *d = v; break; }
    case 0x000D: { u32 i = IDX(rBX);                  /* specific descriptor (the reserved 04h-7Ch range) */
        if (i <= HOST_DS_I || i >= FIRST_FREE || ldt_used[i]) { err(c, 0x8011); break; }
        ldt_used[i] = 1; ldt[i] = make_desc(0, 0, 0xF2, 0); break; }
    case 0x0100: return dos_call(c, X_DOSALLOC, 0x4800, (u16)rBX, 0);
    case 0x0101: { u64 *d = desc_of(rDX); if (!d) { err(c, 0x8022); break; }
        struct xent *x;
        u16 seg = (u16)(d_base(*d) >> 4);
        struct regs *nr = dos_call(c, X_DOSFREE, 0x4900, 0, seg);
        x = &xs[xdepth - 1]; x->a = rDX;
        return nr; }
    case 0x0102: { u64 *d = desc_of(rDX); if (!d) { err(c, 0x8022); break; }
        u16 seg = (u16)(d_base(*d) >> 4);
        struct regs *nr = dos_call(c, X_DOSRESIZE, 0x4A00, (u16)rBX, seg);
        xs[xdepth - 1].a = rDX; xs[xdepth - 1].b = rBX;
        return nr; }
    case 0x0200: { u8 n = (u8)rBX; SET16(ecx, rd16(n * 4 + 2)); SET16(edx, rd16(n * 4)); break; }
    case 0x0201: { u8 n = (u8)rBX; wr16(n * 4, (u16)rDX); wr16(n * 4 + 2, (u16)rCX); break; }
    case 0x0202: { u8 n = (u8)rBX; if (n >= 32) { err(c, 0x8021); break; }
        if (exc_vec[n].sel) { SET16(ecx, exc_vec[n].sel); c->edx = client32 ? exc_vec[n].off : (c->edx & 0xFFFF0000u) | (u16)exc_vec[n].off; }
        else { SET16(ecx, HOST_CS); c->edx = client32 ? stubs(S_EXCRET) : (c->edx & 0xFFFF0000u) | stubs(S_EXCRET); }
        break; }
    case 0x0203: { u8 n = (u8)rBX; if (n >= 32) { err(c, 0x8021); break; }
        exc_vec[n].sel = (rCX == HOST_CS) ? 0 : (u16)rCX; exc_vec[n].off = rEDX; break; }
    case 0x0204: { u8 n = (u8)rBX;
        if (pm_vec[n].sel) { SET16(ecx, pm_vec[n].sel); c->edx = client32 ? pm_vec[n].off : (c->edx & 0xFFFF0000u) | (u16)pm_vec[n].off; }
        else { SET16(ecx, HOST_CS); u32 o = stubs(S_DEFINT) + n * 4; c->edx = client32 ? o : (c->edx & 0xFFFF0000u) | o; }
        break; }
    case 0x0205: { u8 n = (u8)rBX;
        if (rCX == HOST_CS && rEDX == (u32)(stubs(S_DEFINT) + n * 4)) pm_vec[n].sel = 0;
        else { pm_vec[n].sel = (u16)rCX; pm_vec[n].off = rEDX; }
        break; }
    case 0x0300: case 0x0301: case 0x0302: return sim_real(c, fn);
    case 0x0303: {
        int i;
        for (i = 0; i < MAX_CB && cbs[i].used; i++) ;
        if (i == MAX_CB) { err(c, 0x8015); break; }
        cbs[i].used = 1; cbs[i].sel = (u16)c->ds; cbs[i].off = rESI; cbs[i].ssel = (u16)c->es; cbs[i].soff = rEDI;
        SET16(ecx, 0xF000); SET16(edx, stubs(S_CB0) + i * 4); break; }
    case 0x0304: {
        int i = (rDX - stubs(S_CB0)) / 4;
        if (rCX != 0xF000 || i < 0 || i >= MAX_CB || !cbs[i].used) { err(c, 0x8024); break; }
        cbs[i].used = 0; break; }
    case 0x0305: SET16(eax, 0); SET16(ebx, 0xF000); SET16(ecx, stubs(S_RMRETF));
        SET16(esi, HOST_CS); c->edi = client32 ? stubs(S_PMRETF) : (c->edi & 0xFFFF0000u) | stubs(S_PMRETF); break;
    case 0x0306: SET16(ebx, 0xF000); SET16(ecx, stubs(S_RAWRM));
        SET16(esi, HOST_CS); c->edi = client32 ? stubs(S_RAWPM) : (c->edi & 0xFFFF0000u) | stubs(S_RAWPM); break;
    case 0x0400: SET16(eax, 0x005A); SET16(ebx, 0x0001); c->ecx = (c->ecx & 0xFFFFFF00u) | (u8)cpu_type();
        SET16(edx, 0x0870); break;
    case 0x0500: { u32 a = lin(c->es, rEDI);
        if (!lin_ok(a, 0x30)) { err(c, 0x8021); break; }
        u32 f = dpmi_avail(), w = win_largest();
        if (w > f) w = f;
        for (int i = 0; i < 0x30; i += 4) wr32(a + i, 0xFFFFFFFF);
        wr32(a, w);                                 /* largest block: what the window has (DOS/4GW) */
        wr32(a + 4, f >> 12); wr32(a + 8, f >> 12); /* max unlocked / locked allocation: everything (Quake) */
        wr32(a + 0x10, f >> 12); wr32(a + 0x14, f >> 12);
        wr32(a + 0x18, (f + dpmi_used()) >> 12);
        break; }
    case 0x0501: { int i = alloc_block(rBX << 16 | rCX);
        if (i < 0) { err(c, 0x8013); break; }
        SET16(ebx, blk[i].lin >> 16); SET16(ecx, blk[i].lin); SET16(esi, (i + 1) >> 16); SET16(edi, i + 1); break; }
    case 0x0502: { u32 h = rSI << 16 | rDI;
        if (!h || h > MAX_BLOCKS || blk[h - 1].used != 1) { err(c, 0x8023); break; }
        blk[h - 1].used = 0; break; }
    case 0x0503: { u32 h = rSI << 16 | rDI, size = rBX << 16 | rCX;
        if (!h || h > MAX_BLOCKS || !blk[h - 1].used) { err(c, 0x8023); break; }
        int o = h - 1;
        size = (size + 4095) & ~4095u;
        if (size <= blk[o].size || grow_in_place(o, size)) { SET16(ebx, blk[o].lin >> 16); SET16(ecx, blk[o].lin); break; }
        blk[o].used = 0;                            /* so alloc_block can't hand it back */
        int n = alloc_block(size);
        if (n < 0) { blk[o].used = 1; err(c, 0x8013); break; }
        memcpy(gptr(blk[n].lin), gptr(blk[o].lin), blk[o].size);
        SET16(ebx, blk[n].lin >> 16); SET16(ecx, blk[n].lin); SET16(esi, (n + 1) >> 16); SET16(edi, n + 1); break; }
    case 0x0600: case 0x0601: case 0x0602: case 0x0603: case 0x0702: case 0x0703: case 0x0801: break;
    case 0x0604: SET16(ebx, 0); SET16(ecx, 4096); break;
    case 0x0800: { u32 p = rBX << 16 | rCX, n = rSI << 16 | rDI;
        if (p < 0x100000) { SET16(ebx, p >> 16); SET16(ecx, p); break; }   /* below 1 MiB: as is */
        if (video_vram_range(p, n)) { SET16(ebx, p >> 16); SET16(ecx, p); break; }   /* VESA LFB: mapped as is */
        void *m = map_mmio64_user(p, n);
        if (!m) { err(c, 0x8021); break; }
        u32 l = (u32)(uintptr_t)m;
        SET16(ebx, l >> 16); SET16(ecx, l); break; }
    case 0x0900: c->eax = (c->eax & ~0xFFu) | (u8)c->vif; c->vif = 0; break;
    case 0x0901: c->eax = (c->eax & ~0xFFu) | (u8)c->vif; c->vif = 1; break;
    case 0x0902: c->eax = (c->eax & ~0xFFu) | (u8)c->vif; break;
    case 0x0E00: SET16(eax, 0x45); break;
    case 0x0E01: break;
    case 0x0A00: err(c, 0x8001); break;            /* vendor API: none (DOS/4GW asks; harmless) */
    case 0x0506: case 0x0507: err(c, 0x8001); break;   /* DPMI 1.0 page attributes: not offered */
    default:
        dbg(1, "DPMI function %04x not supported\n", fn);
        err(c, 0x8001);
    }
    return resume(c);
}

/* ---------------- default handling of a protected-mode interrupt ---------------- */
static struct regs *default_int(struct ctx *c, int n)
{
    if (n == 0x31) return int31(c);
    if (n == 0x21 && ((c->eax >> 8) & 0xFF) == 0x4C) {
        kprintf("DPMI: client exits (%u)\n", c->eax & 0xFF);
        return terminate(c, (u8)c->eax, 0);
    }
    if (n == 0x2F && (c->eax & 0xFFFF) == 0x1686) { SET16(eax, 0); return resume(c); }
    if (n == 0x2F && (c->eax & 0xFFFF) == 0x1680) { c->eax &= ~0xFFu; return resume(c); }
    return reflect(c, n, X_REFLECT);
}

/* A software interrupt from the client: its own handler, else the default. */
static struct regs *pm_int(struct ctx *c, int n)
{
    dbg(3, "PM INT %02x AX=%04x BX=%04x CX=%04x DX=%08x %s from %x:%x\n", n, c->eax & 0xFFFF, c->ebx & 0xFFFF,
        c->ecx & 0xFFFF, c->edx, pm_vec[n].sel ? "client" : "default", c->cs, c->eip);
    if (pm_vec[n].sel) {
        push(c, (c->eflags & 0x0DD5) | 2 | (c->vif ? EFL_IF : 0), client32);
        push(c, c->cs, client32);
        push(c, c->eip, client32);
        c->cs = pm_vec[n].sel; c->eip = pm_vec[n].off;
        c->eflags &= ~EFL_TF;
        return resume(c);
    }
    return default_int(c, n);
}

/* ---------------- hardware interrupts ---------------- */
int dpmi_pm_hooked(int vec) { return client && pm_vec[vec & 0xFF].sel; }

struct regs *dpmi_hw_interrupt(struct regs *r, int vec)
{
    struct ctx from;
    get_ctx(r, &from);
    if (!from.pm) dbg(2, "DPMI: IRQ vector %x from real mode to PM handler (IVT %04x:%04x)\n", vec,
                      rd16(vec * 4 + 2), rd16(vec * 4));
    struct ctx p = from;
    if (!p.pm) { p.ds = p.es = p.fs = p.gs = 0; }
    return visit_pm(&from, X_HW, pm_vec[vec].sel, pm_vec[vec].off, S_HWRET, 1, &p);
}

/* A service in the monitor (INT 10h VESA) was reached by reflecting a
   protected-mode INT: ES:DI is then the host's scratch segment, not the
   program's buffer, so hand back the buffer the program meant (its PM
   ES:EDI), as a host that translates these calls would. */
int dpmi_reflected_buffer(struct regs *r, u32 *lin)
{
    if (!client || !xdepth || xs[xdepth - 1].kind != X_REFLECT || (r->v86_es & 0xFFFF) != rm_stack_seg) return 0;
    struct ctx *pm = &xs[xdepth - 1].saved;
    *lin = sel_base(pm->es) + (client32 ? pm->edi : (pm->edi & 0xFFFF));
    return 1;
}

/* Run a real-mode routine that ends with IRET (the mouse driver's event
   callback) for a client in protected mode; it resumes as it was. */
struct regs *dpmi_rm_iret_call(struct regs *r, u16 cs, u16 ip)
{
    struct ctx c;
    get_ctx(r, &c);
    if (!client) return r;
    xpush(X_REFLECT_HW, &c);
    struct ctx rm;
    rm_from_pm(&rm, &c);
    rm.vif = 0;
    return go_real(&rm, cs, ip, 1);
}

struct regs *dpmi_reflect_irq(struct regs *r, int vec)
{
    struct ctx c;
    get_ctx(r, &c);
    if (!client) return r;
    return reflect(&c, vec, X_REFLECT_HW);
}

/* ---------------- exceptions and traps from protected mode ---------------- */
static const char *const exc_names[] = { "divide error", "debug", "NMI", "breakpoint", "overflow",
    "bound", "invalid opcode", "no coprocessor", "double fault", "", "invalid TSS", "segment not present",
    "stack fault", "general protection", "page fault", "", "coprocessor error", "alignment check" };

static struct regs *pm_fault(struct ctx *c, int n, u32 errc)
{
    {
        u32 a = sel_base(c->cs) + c->eip;
        if (lin_ok(a, 8))
            dbg(1, "DPMI exception %u err %x at %x:%x (cs base %x big %u): %02x %02x %02x %02x %02x %02x %02x %02x"
                " DS %x(%x) ES %x(%x) FS %x GS %x EAX %x EBX %x ESI %x EDI %x\n",
                n, errc, c->cs, c->eip, sel_base(c->cs), sel_big(c->cs),
                rd8(a), rd8(a + 1), rd8(a + 2), rd8(a + 3), rd8(a + 4), rd8(a + 5), rd8(a + 6), rd8(a + 7),
                c->ds, sel_base(c->ds), c->es, sel_base(c->es), c->fs, c->gs, c->eax, c->ebx, c->esi, c->edi);
    }
    if (n < 32 && exc_vec[n].sel) {
        struct ctx h = *c;
        struct regs *nr;
        /* DPMI 0.9 frame: return address, error code, eip, cs, eflags, esp, ss */
        h.esp = locked_esp(c);
        xpush(X_EXC, c);
        h.ss = HOST_SS;
        push(&h, c->ss, client32);
        push(&h, c->esp, client32);
        push(&h, (c->eflags & 0x0DD5) | 2 | (c->vif ? EFL_IF : 0), client32);
        push(&h, c->cs, client32);
        push(&h, c->eip, client32);
        push(&h, errc, client32);
        push(&h, HOST_CS, client32);
        push(&h, stubs(S_EXCRET), client32);
        h.cs = exc_vec[n].sel; h.eip = exc_vec[n].off;
        h.vif = 0;
        xs[xdepth - 1].lk = h.esp;
        nr = resume(&h);
        return nr;
    }
    kprintf("DPMI fault %u err %x CS:EIP %x:%x SS:ESP %x:%x (base %x big %u) DS %x(%x) ES %x(%x) FS %x GS %x\n"
            "  EAX %x EBX %x ECX %x EDX %x ESI %x EDI %x EBP %x FL %x\n",
            n, errc, c->cs, c->eip, c->ss, c->esp, sel_base(c->ss), sel_big(c->ss), c->ds, sel_base(c->ds),
            c->es, sel_base(c->es), c->fs, c->gs, c->eax, c->ebx, c->ecx, c->edx, c->esi, c->edi, c->ebp, c->eflags);
    char msg[160];
    u32 cr2 = 0;
    if (n == 14) __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
    snprintf(msg, sizeof msg, "%s (exception %u, error %x) at %04x:%08x%s%x", n < 18 ? exc_names[n] : "exception",
             n, errc, c->cs, c->eip, n == 14 ? ", address " : "", cr2);
    return terminate(c, 0xFF, msg);
}

static int code32(struct ctx *c) { return sel_big(c->cs); }

static struct regs *pm_gp(struct ctx *c, u32 errc)
{
    u32 base = sel_base(c->cs), eip = c->eip;
    if (!lin_ok(base + eip, 16)) return pm_fault(c, 13, errc);
    if (errc & 2) {                                       /* INT n: IDT entry */
        int n = errc >> 3;
        u8 op = rd8(base + eip);
        c->eip += op == 0xCD ? 2 : 1;
        if (!code32(c)) c->eip &= 0xFFFF;
        if (op == 0xCE && !(c->eflags & EFL_OF)) return resume(c);
        return pm_int(c, n);
    }
    if (errc) return pm_fault(c, 13, errc);

    int op32 = code32(c), a32 = code32(c), rep = 0, seg = -1;
    u32 ip = eip;
    u8 op;
    for (;;) {
        op = rd8(base + ip++);
        switch (op) {
        case 0x66: op32 ^= 1; continue;
        case 0x67: a32 ^= 1; continue;
        case 0xF2: case 0xF3: rep = 1; continue;
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
#define NEXT(x) do { c->eip = (x); if (!code32(c)) c->eip &= 0xFFFF; } while (0)
    switch (op) {
    case 0xFA: c->vif = 0; stepping = 1; NEXT(ip); return resume(c);
    case 0xFB: c->vif = 1; NEXT(ip); return resume(c);
    case 0xF4:
        if ((c->cs & 0xFFFF) == HOST_CS) {
            int id = rd8(base + ip);
            return dpmi_pm_trap(c, id, rd8(base + ip + 1));
        }
        if (rd8(base + ip) >= 0x47 && rd8(base + ip) <= 0x49 && rd8(base + ip + 1) == 0xC3) {
            /* the VESA protected-mode interface (4F0Ah), copied into the program */
            u32 pal = sel_base(c->es) + (code32(c) ? c->edi : (c->edi & 0xFFFF));
            video_vbe_pm(rd8(base + ip), c->ebx, c->ecx, c->edx, lin_ok(pal, 4) ? pal : 0);
            NEXT(ip + 1);
            return resume(c);
        }
        NEXT(ip);
        {
            struct ctx k = *c;
            vif = 1;
            wait_for_irq();
            k.vif = 1;
            return resume(&k);
        }
    case 0xE4: case 0xE5: case 0xEC: case 0xED: {
        u16 port = (op & 8) ? (u16)c->edx : rd8(base + ip++);
        int s = (op & 1) ? sz : 1;
        u32 v = port_in(port, s);
        if (s == 1) c->eax = (c->eax & ~0xFFu) | (u8)v;
        else if (s == 2) c->eax = (c->eax & 0xFFFF0000u) | (u16)v;
        else c->eax = v;
        NEXT(ip); return resume(c); }
    case 0xE6: case 0xE7: case 0xEE: case 0xEF: {
        u16 port = (op & 8) ? (u16)c->edx : rd8(base + ip++);
        int s = (op & 1) ? sz : 1;
        port_out(port, s == 1 ? (u8)c->eax : s == 2 ? (u16)c->eax : c->eax, s);
        NEXT(ip); return resume(c); }
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
        int s = (op & 1) ? sz : 1;
        int step = (c->eflags & EFL_DF) ? -s : s;
        u32 count = rep ? (a32 ? c->ecx : (c->ecx & 0xFFFF)) : 1;
        u32 sb = op < 0x6E ? sel_base(c->es) : sel_base(seg == 0 ? c->es : seg == 1 ? c->cs : seg == 2 ? c->ss :
                                                         seg == 4 ? c->fs : seg == 5 ? c->gs : c->ds);
        for (; count; count--) {
            u32 *ix = op < 0x6E ? &c->edi : &c->esi;
            u32 off = a32 ? *ix : (*ix & 0xFFFF);
            u32 a = sb + off;
            if (!lin_ok(a, s)) return pm_fault(c, 13, 0);
            if (op < 0x6E) {
                u32 v = port_in((u16)c->edx, s);
                if (s == 1) wr8(a, v); else if (s == 2) wr16(a, v); else wr32(a, v);
            } else port_out((u16)c->edx, s == 1 ? rd8(a) : s == 2 ? rd16(a) : rd32(a), s);
            if (a32) *ix += step; else *ix = (*ix & 0xFFFF0000u) | (u16)(*ix + step);
            if (rep) { if (a32) c->ecx--; else c->ecx = (c->ecx & 0xFFFF0000u) | (u16)(c->ecx - 1); }
        }
        NEXT(ip); return resume(c); }
    case 0x0F:
        if (rd8(base + ip) == 0x06) { NEXT(ip + 1); return resume(c); }       /* CLTS */
        if (rd8(base + ip) == 0x20 && (rd8(base + ip + 1) >> 6) == 3) {       /* MOV r32, CRn */
            u8 m = rd8(base + ip + 1);
            u32 v = 0;
            if (((m >> 3) & 7) == 0) { __asm__ volatile("mov %%cr0,%0" : "=r"(v)); v &= 0x8005003F; }
            u32 *g[8] = { &c->eax, &c->ecx, &c->edx, &c->ebx, &c->esp, &c->ebp, &c->esi, &c->edi };
            *g[m & 7] = v;
            NEXT(ip + 2); return resume(c);
        }
        break;
    }
#undef NEXT
    return pm_fault(c, 13, 0);
}

struct regs *dpmi_exception(struct regs *r)
{
    struct ctx c;
    get_ctx(r, &c);
    if (!client) panic("protected-mode fault without a DPMI client (vector %u at %x:%x)", r->vec, r->cs, r->eip);
    switch (r->vec) {
    case 13: return pm_gp(&c, r->err);
    case 14: {                                        /* the 16-colour VGA window? */
        u32 cr2;
        __asm__ volatile("mov %%cr2,%0" : "=r"(cr2));
        if (vga16_window(cr2)) {
            int pm = c.pm;
            struct emu_cpu e = { { &c.eax, &c.ecx, &c.edx, &c.ebx, &c.esp, &c.ebp, &c.esi, &c.edi }, &c.eip, &c.eflags,
                                 { pm ? sel_base(c.es) : c.es << 4, pm ? sel_base(c.cs) : c.cs << 4,
                                   pm ? sel_base(c.ss) : c.ss << 4, pm ? sel_base(c.ds) : c.ds << 4,
                                   pm ? sel_base(c.fs) : c.fs << 4, pm ? sel_base(c.gs) : c.gs << 4 },
                                 pm && sel_big(c.cs) };
            if (vga16_fault(&e, cr2)) return resume(&c);
        }
        return pm_fault(&c, 14, r->err); }
    case 8: case 10: case 11: case 12: case 17: return pm_fault(&c, r->vec, r->err);
    case 16:
        if (exc_vec[16].sel) return pm_fault(&c, 16, 0);
        __asm__ volatile("fnclex");
        return resume(&c);
    case 1: {
        u32 dr6;
        __asm__ volatile("mov %%dr6,%0" : "=r"(dr6));
        if (!(dr6 & 0x4000) || !stepping) {            /* not our single step */
            if (!stepping) { c.eflags &= ~EFL_TF; return resume(&c); }
            return pm_fault(&c, 1, 0);
        }
        __asm__ volatile("mov %0,%%dr6" ::"r"(0));
        /* what did the stepped instruction do to IF? */
        u32 a = sel_base(step_cs) + step_eip;
        int o32 = sel_big(step_cs);
        u8 op = lin_ok(a, 4) ? rd8(a) : 0;
        while (op == 0x66 || op == 0x67 || op == 0xF0 || op == 0x2E || op == 0x3E || op == 0x26 ||
               op == 0x36 || op == 0x64 || op == 0x65) {
            if (op == 0x66) o32 ^= 1;
            op = rd8(++a);
        }
        u32 sp = sel_base(step_ss) + (sel_big(step_ss) ? step_esp : (step_esp & 0xFFFF));
        if (op == 0x9D && lin_ok(sp, 4)) c.vif = (rd16(sp) & EFL_IF) != 0;                 /* POPF */
        else if (op == 0xCF && lin_ok(sp, 12)) c.vif = (rd16(sp + (o32 ? 8 : 4)) & EFL_IF) != 0;   /* IRET */
        return resume(&c); }
    default: return pm_fault(&c, r->vec, 0);
    }
}

/* ---------------- returns through the stubs ---------------- */
static void copy_back_rm(struct ctx *pm, struct ctx *rm)
{
    pm->eax = rm->eax; pm->ebx = rm->ebx; pm->ecx = rm->ecx; pm->edx = rm->edx;
    pm->esi = rm->esi; pm->edi = rm->edi; pm->ebp = rm->ebp;
    pm->eflags = (pm->eflags & ~0x0DD5u) | (rm->eflags & 0x0DD5);
}

/* Protected-mode traps: id = byte after the HLT in the host code segment. */
struct regs *dpmi_pm_trap(struct ctx *c, int id, int arg)
{
    switch (id) {
    case 0x58: {                               /* default handler, reached by chaining */
        u32 ip = pop(c, client32), cs = pop(c, client32), fl = pop(c, client32);
        c->eip = ip; c->cs = cs & 0xFFFF;
        c->eflags = (c->eflags & ~0x0DD5u) | (fl & 0x0DD5);
        if (xdepth && xs[xdepth - 1].kind == X_HW && (cs & 0xFFFF) == HOST_CS) {
            /* a hardware interrupt the client passed on: reflect it, then finish the visit */
            return reflect(c, arg, X_REFLECT_HW);
        }
        return default_int(c, arg); }
    case 0x51: {                               /* hardware interrupt handler returned */
        if (!xdepth || xs[xdepth - 1].kind != X_HW) break;
        struct ctx s = xs[--xdepth].saved;
        return resume(&s); }
    case 0x52: {                               /* exception handler returned (RETF) */
        if (!xdepth || xs[xdepth - 1].kind != X_EXC) break;
        xdepth--;
        struct ctx n = *c;
        pop(&n, client32);                     /* error code */
        n.eip = pop(&n, client32);
        n.cs = pop(&n, client32) & 0xFFFF;
        u32 fl = pop(&n, client32);
        u32 esp = pop(&n, client32), ss = pop(&n, client32);
        n.eflags = (n.eflags & ~0x0DD5u) | (fl & 0x0DD5);
        n.vif = (fl & EFL_IF) != 0;
        n.esp = esp; n.ss = ss & 0xFFFF;
        return resume(&n); }
    case 0x53: {                               /* real-mode callback's PM procedure returned (IRET) */
        if (!xdepth || xs[xdepth - 1].kind != X_CB) break;
        xdepth--;
        u32 s = lin(c->es, client32 ? c->edi : (c->edi & 0xFFFF));
        struct ctx rm;
        memset(&rm, 0, sizeof rm);
        if (!lin_ok(s, 0x32)) return terminate(c, 0xFF, "bad real-mode callback structure");
        rm.edi = rd32(s); rm.esi = rd32(s + 4); rm.ebp = rd32(s + 8);
        rm.ebx = rd32(s + 0x10); rm.edx = rd32(s + 0x14); rm.ecx = rd32(s + 0x18); rm.eax = rd32(s + 0x1C);
        rm.eflags = rd16(s + 0x20);
        rm.es = rd16(s + 0x22); rm.ds = rd16(s + 0x24); rm.fs = rd16(s + 0x26); rm.gs = rd16(s + 0x28);
        rm.eip = rd16(s + 0x2A); rm.cs = rd16(s + 0x2C); rm.esp = rd16(s + 0x2E); rm.ss = rd16(s + 0x30);
        rm.vif = (rm.eflags & EFL_IF) != 0;
        dbg(2, "DPMI callback returns to %04x:%04x SS:SP %04x:%04x\n", rm.cs, rm.eip, rm.ss, rm.esp);
        return resume(&rm); }
    case 0x57: {                               /* raw switch protected -> real */
        struct ctx rm = *c;
        rm.pm = 0;
        rm.ds = c->eax & 0xFFFF; rm.es = c->ecx & 0xFFFF; rm.ss = c->edx & 0xFFFF;
        rm.esp = c->ebx & 0xFFFF; rm.cs = c->esi & 0xFFFF; rm.eip = c->edi & 0xFFFF;
        rm.fs = rm.gs = 0;
        return resume(&rm); }
    }
    return pm_fault(c, 13, 0);
}

/* Real-mode (v86) traps from the dpmi_* stubs in the BIOS segment. */
int dpmi_rm_trap(struct regs *r, int id)
{
    struct ctx c;
    get_ctx(r, &c);
    if (id == 0x54) {                          /* switch entry: remember the caller */
        if (client) reset_client();            /* an earlier client that didn't exit through us */
        entry_ctx = c;
        entry_ctx.eip = pop(&entry_ctx, 0);    /* the far call's return address */
        entry_ctx.cs = pop(&entry_ctx, 0);
        entry_ax = (u16)c.eax;
        entry_es = (u16)c.es;
        return BIOS_CONT;
    }
    if (id == 0x55) { enter_pm(r); return BIOS_SWITCH; }
    if (id == 0x56) {                          /* raw switch real -> protected */
        if (!client) return BIOS_CONT;
        struct ctx p = c;
        p.pm = 1;
        p.ds = c.eax & 0xFFFF; p.es = c.ecx & 0xFFFF; p.ss = c.edx & 0xFFFF;
        p.esp = client32 ? c.ebx : (c.ebx & 0xFFFF);
        p.cs = c.esi & 0xFFFF; p.eip = client32 ? c.edi : (c.edi & 0xFFFF);
        p.fs = p.gs = 0;
        resume(&p);
        return BIOS_SWITCH;
    }
    if (id >= 0x60 && id < 0x60 + MAX_CB) {    /* real-mode callback */
        int i = id - 0x60;
        if (!client || !cbs[i].used) return BIOS_CONT;
        u32 s = sel_base(cbs[i].ssel) + cbs[i].soff;
        if (!lin_ok(s, 0x32)) return BIOS_CONT;
        wr32(s, c.edi); wr32(s + 4, c.esi); wr32(s + 8, c.ebp); wr32(s + 0xC, 0);
        wr32(s + 0x10, c.ebx); wr32(s + 0x14, c.edx); wr32(s + 0x18, c.ecx); wr32(s + 0x1C, c.eax);
        wr16(s + 0x20, (u16)((c.eflags & 0x0DD5) | 2 | (c.vif ? EFL_IF : 0)));
        wr16(s + 0x22, c.es); wr16(s + 0x24, c.ds); wr16(s + 0x26, c.fs); wr16(s + 0x28, c.gs);
        wr16(s + 0x2A, (u16)c.eip); wr16(s + 0x2C, c.cs); wr16(s + 0x2E, (u16)c.esp); wr16(s + 0x30, c.ss);
        dbg(2, "DPMI callback %d from %04x:%04x SS:SP %04x:%04x -> %x:%x struct %x:%x\n", i, c.cs, c.eip, c.ss, c.esp,
            cbs[i].sel, cbs[i].off, cbs[i].ssel, cbs[i].soff);
        struct ctx p = c;
        p.pm = 1;
        ldt[HOST_DS_I] = make_desc(c.ss << 4, 0xFFFF, 0xF2, 0);
        p.ds = SEL(HOST_DS_I);
        p.esi = c.esp & 0xFFFF;
        p.es = cbs[i].ssel; p.edi = cbs[i].soff;
        p.fs = p.gs = 0;
        visit_pm(&c, X_CB, cbs[i].sel, cbs[i].off, S_CBRET, 1, &p);
        return BIOS_SWITCH;
    }
    if (id == 0x50) {                          /* a real-mode excursion came back */
        if (!xdepth) return BIOS_CONT;
        struct xent *x = &xs[--xdepth];
        struct ctx pm = x->saved;
        switch (x->kind) {
        case X_REFLECT:
            copy_back_rm(&pm, &c);
            break;
        case X_REFLECT_HW:
            break;
        case X_SIM: {
            u32 s = x->a;
            dbg(2, "  -> AX=%04x BX=%04x CX=%04x DX=%04x CF=%u\n", c.eax & 0xFFFF, c.ebx & 0xFFFF,
                c.ecx & 0xFFFF, c.edx & 0xFFFF, c.eflags & 1);
            wr32(s, c.edi); wr32(s + 4, c.esi); wr32(s + 8, c.ebp);
            wr32(s + 0x10, c.ebx); wr32(s + 0x14, c.edx); wr32(s + 0x18, c.ecx); wr32(s + 0x1C, c.eax);
            wr16(s + 0x20, (u16)((c.eflags & 0x0DD5) | 2));
            wr16(s + 0x22, c.es); wr16(s + 0x24, c.ds); wr16(s + 0x26, c.fs); wr16(s + 0x28, c.gs);
            pm.eflags &= ~EFL_CF;
            break; }
        case X_DOSALLOC:
            if (c.eflags & EFL_CF) {
                pm.eflags |= EFL_CF;
                pm.eax = (pm.eax & 0xFFFF0000u) | (u16)c.eax;
                pm.ebx = (pm.ebx & 0xFFFF0000u) | (u16)c.ebx;
            } else {
                u32 paras = pm.ebx & 0xFFFF, n = (paras + 0xFFF) >> 12;
                int i = alloc_desc(n ? n : 1);
                if (i < 0) { pm.eflags |= EFL_CF; pm.eax = (pm.eax & 0xFFFF0000u) | 8; break; }
                for (u32 k = 0; k < (n ? n : 1); k++) {
                    u32 left = paras > k * 0x1000 ? (paras - k * 0x1000) * 16 : 16;
                    ldt[i + k] = make_desc(((u32)(u16)c.eax << 4) + k * 0x10000, (k == 0 ? paras * 16 : left) - 1, 0xF2, 0);
                }
                pm.eflags &= ~EFL_CF;
                pm.eax = (pm.eax & 0xFFFF0000u) | (u16)c.eax;
                pm.edx = (pm.edx & 0xFFFF0000u) | SEL(i);
                dbg(2, "  -> DOS block %04x (%x paragraphs), selector %x\n", c.eax & 0xFFFF, paras, SEL(i));
            }
            break;
        case X_DOSFREE:
            if (c.eflags & EFL_CF) { pm.eflags |= EFL_CF; pm.eax = (pm.eax & 0xFFFF0000u) | (u16)c.eax; }
            else { pm.eflags &= ~EFL_CF; if (IDX(x->a) >= FIRST_FREE) ldt_used[IDX(x->a)] = 0; }
            break;
        case X_DOSRESIZE:
            if (c.eflags & EFL_CF) {
                pm.eflags |= EFL_CF;
                pm.eax = (pm.eax & 0xFFFF0000u) | (u16)c.eax;
                pm.ebx = (pm.ebx & 0xFFFF0000u) | (u16)c.ebx;
            } else {
                u64 *d = desc_of(x->a);
                if (d) *d = make_desc(d_base(*d), x->b * 16 - 1, 0xF2, 0);
                pm.eflags &= ~EFL_CF;
            }
            break;
        default:
            return BIOS_CONT;
        }
        resume(&pm);
        return BIOS_SWITCH;
    }
    return BIOS_CONT;
}

/* Before returning to a client on a 16-bit stack segment: set up the
   espfix segment (see isr_return in boot.S). */
extern u32 espfix_on, espfix_ptr;
void dpmi_espfix(struct regs *r)
{
    if (r->eflags & EFL_VM || (r->cs & 3) != 3 || sel_big(r->ss)) return;
    u32 frame = (u32)(uintptr_t)&r->eip;
    u32 esp = (r->esp & 0xFFFF0000u) | 0x1000;
    gdt[5] = make_desc(frame - esp, 0xFFFFFFFF, 0x92, 4);
    espfix_ptr = esp;
    espfix_on = 1;

}

int dpmi_depth(char *buf, int n)
{
    int o = snprintf(buf, n, "x%d:", xdepth);
    for (int i = 0; i < xdepth && o < n - 4; i++) o += snprintf(buf + o, n - o, " %d", xs[i].kind);
    return xdepth;
}

void dpmi_dump_code(u32 sel, u32 off, int n)
{
    u32 a = sel_base(sel) + off;
    kprintf("code %x:%x (base %x):", sel, off, sel_base(sel));
    for (int i = 0; i < n; i++) if (lin_ok(a + i, 1)) kprintf(" %02x", rd8(a + i));
    kprintf("\n");
}
