/* VMHV: DOS in a hardware virtual machine (AMD-V), with memory it manages
   itself.

   vmdos normally runs DOS in virtual-8086 mode, so a program that wants
   the bare CPU (HIMEM, EMM386, Windows 3.x, DOS extenders' raw/VCPI
   kernels) can't have it. VMHV restarts the PC's DOS inside an AMD-V
   guest instead: the guest owns ring 0, its own descriptor tables, paging
   and memory manager, and sees a 386 with 640 KB of conventional memory
   and 8 MB of extended memory (1-9 MB), nothing else. vmdos stays
   underneath as the hardware:

   - Memory: the nested page tables (NPT) give guest-physical 0-1 MB the
     usual guest RAM (BIOS segment, video memory and the VGA window
     included) and 1-9 MB a block of its own. They share the host's page
     tables for linear 0-10 MiB, where the host maps the same thing, so
     A20 and the VGA window's trapping work unchanged, and the guest's
     memory is at the same linear address for the host (Sound Blaster DMA
     and the disk code use it directly).
   - BIOS: the F000h stubs ("HLT; db id") reach bios_service() as before:
     HLT exits in real mode; in virtual-8086 mode (under EMM386, Windows)
     HLT raises #GP, which is intercepted first. For a guest that pages,
     the BIOS code sees linear 0-1 MiB through the guest's own page
     tables (gview), so buffers in UMBs or the EMS frame are found.
   - Ports: every IN/OUT exits to the virtual PIC, PIT, 8042, VGA, Sound
     Blaster and the rest (vdev.c). The PCI ports read FFh: no devices.
   - Interrupts: real ones end the run and the host takes them as usual;
     the virtual PIC's are injected when the guest's IF allows.

   The guest owns the machine until it reboots (Ctrl+Alt+Del, port 64h
   FEh, a triple fault...), which restarts vmdos: Ctrl+Alt+End sends
   Ctrl+Alt+Del in (SMARTDRV and DOS flush on it, Windows shows its
   reboot screen), twice within 3 s offers a forced restart.

   This is a draft: AMD-V only (no Intel VT-x yet), a 32-bit (non-PAE)
   guest paging walk, planar VGA only from code below 1 MiB, no CD-ROM
   in the VM (VMCD's interface is the v86 monitor's). */
#include "kernel.h"

/* ---- the VMCB ---- */
#define C_CR      0x000
#define C_EXC     0x008
#define C_MISC1   0x00C
#define C_MISC2   0x010
#define C_IOPM    0x040
#define C_MSRPM   0x048
#define C_ASID    0x058
#define C_TLB     0x05C
#define C_VINTR   0x060
#define C_SHADOW  0x068
#define C_EXIT    0x070
#define C_INFO1   0x078
#define C_INFO2   0x080
#define C_EXITINT 0x088
#define C_NP      0x090
#define C_EVENT   0x0A8
#define C_NCR3    0x0B0
#define C_CLEAN   0x0C0
#define S_ES      0x400
#define S_CS      0x410
#define S_SS      0x420
#define S_DS      0x430
#define S_FS      0x440
#define S_GS      0x450
#define S_GDTR    0x460
#define S_LDTR    0x470
#define S_IDTR    0x480
#define S_TR      0x490
#define S_CPL     0x4CB
#define S_EFER    0x4D0
#define S_CR4     0x548
#define S_CR3     0x550
#define S_CR0     0x558
#define S_DR7     0x560
#define S_DR6     0x568
#define S_RFLAGS  0x570
#define S_RIP     0x578
#define S_RSP     0x5D8
#define S_RAX     0x5F8
#define S_GPAT    0x668

/* intercepts */
#define I1_INTR     (1u << 0)
#define I1_VINTR    (1u << 4)
#define I1_HLT      (1u << 24)
#define I1_IOIO     (1u << 27)
#define I1_MSR      (1u << 28)
#define I1_SHUTDOWN (1u << 31)
#define I2_SVM      0x7Fu            /* VMRUN VMMCALL VMLOAD VMSAVE STGI CLGI SKINIT */

/* exit codes */
#define X_EXC(n)  (0x40 + (n))
#define X_INTR    0x60
#define X_VINTR   0x64
#define X_HLT     0x78
#define X_IOIO    0x7B
#define X_MSR     0x7C
#define X_SHUT    0x7F
#define X_NPF     0x400

#define EV_VALID  (1u << 31)
#define EV_EXC    (3u << 8)
#define EV_ERR    (1u << 11)

#define VM_TOP    (9u << 20)          /* guest RAM: 0-640K, (UMB area), 1-9 MB */
#define EXT_START 0x110000u           /* above the HMA, which is guest_ram's */

static u8 *vmcb, *hsave, *hvsave, *iopm, *msrpm, *ext;
static u64 *npt_pdpt, *npt_pd[4];
static u64 *gview_pt;
static u32 g[6];                      /* guest EBX ECX EDX ESI EDI EBP */
int hv_active;
static volatile int hv_force;
static u8 menu_key;                   /* the FDCONFIG menu choice to type (scan code) */
static int menu_wait;                 /* found by VMHV, not given: let the user pick first */
static u32 menu_t0, menu_ms = 3000;  /* how long it waits: MENUDEFAULT's timeout less 1/2 s */

void svm_run(u32 vmcb_pa, u32 host_save_pa, u32 *g);
int hv_host_pae(void);
u64 hv_host_pde(u32 lin);
u64 *hv_host_pt0(void);
void hv_swap_pde0(u64 pde);
extern volatile int hv_tlb_dirty;
u32 video_vram_base(u32 *size);
void vkbd_inject(u8 sc);

#define V8(o)  (*(volatile u8 *)(vmcb + (o)))
#define V16(o) (*(volatile u16 *)(vmcb + (o)))
#define V32(o) (*(volatile u32 *)(vmcb + (o)))
#define V64(o) (*(volatile u64 *)(vmcb + (o)))

static void rdmsr(u32 m, u32 *lo, u32 *hi) { __asm__ volatile("rdmsr" : "=a"(*lo), "=d"(*hi) : "c"(m)); }
static void wrmsr(u32 m, u32 lo, u32 hi) { __asm__ volatile("wrmsr" :: "a"(lo), "d"(hi), "c"(m)); }
static void cpuid(u32 leaf, u32 *a, u32 *b, u32 *c, u32 *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static void setseg(u32 o, u16 sel, u32 base, u32 limit, u16 attr)
{
    V16(o) = sel; V16(o + 2) = attr; V32(o + 4) = limit; V64(o + 8) = base;
}

/* ---------------- memory ---------------- */

/* Host pointer for a guest-physical address (NULL: not RAM the VM has). */
static u8 *gpa_ptr(u32 gpa)
{
    if (gpa < 0x100000) return (u8 *)(uintptr_t)guest_phys(gpa);
    if (gpa < EXT_START) return (u8 *)(uintptr_t)guest_phys(a20_on ? gpa : gpa - 0x100000);
    if (gpa < VM_TOP) return ext + (gpa - EXT_START);
    if (video_vram_range(gpa, 1)) return (u8 *)(uintptr_t)gpa;
    return 0;
}

static u64 *npt_pt(u32 gpa)                    /* NPT page table for gpa (made if needed) */
{
    u64 *pd = npt_pd[gpa >> 30];
    u64 *e = &pd[(gpa >> 21) & 511];
    if (!(*e & 1)) *e = (u32)(uintptr_t)phys_alloc(4096) | 7;
    return (u64 *)(uintptr_t)(u32)(*e & ~0xFFFull);
}

static int npt_init(void)
{
    /* host linear 1 MB+64K..9 MB: the VM's extended memory (the DPMI
       window's place; nothing else runs now), 9-10 MB unmapped */
    for (u32 a = EXT_START; a < 10u << 20; a += 4096)
        map_page(a, a < VM_TOP ? (u32)(uintptr_t)ext + (a - EXT_START) : 0, a < VM_TOP ? 7 : 0);
    npt_pdpt = phys_alloc(4096);
    for (int i = 0; i < 4; i++) {
        npt_pd[i] = phys_alloc(4096);
        npt_pdpt[i] = (u32)(uintptr_t)npt_pd[i] | 1;
    }
    for (u32 a = 0; a < 10u << 20; a += 2u << 20)              /* shared with the host */
        npt_pd[0][a >> 21] = hv_host_pde(a) | 7;
    u32 vsize, vbase = video_vram_base(&vsize);                 /* the VBE framebuffer, at its address */
    for (u32 a = 0; a < vsize; a += 4096)
        npt_pt(vbase + a)[((vbase + a) >> 12) & 511] = (vbase + a) | 7;
    gview_pt = phys_alloc(4096);
    return 0;
}

/* The guest's paging, for the BIOS code: guest linear -> guest physical.
   32-bit two-level tables, 4 MB pages with PSE. -1: not present. */
static int gwalk(u32 lin, u32 *gpa)
{
    u32 cr0 = V32(S_CR0), cr4 = V32(S_CR4);
    if (!(cr0 & 0x80000000u)) { *gpa = lin; return 0; }
    if (cr4 & 0x20) return -1;                                  /* PAE guest: not yet */
    u8 *pd = gpa_ptr(V32(S_CR3) & ~0xFFFu);
    if (!pd) return -1;
    u32 pde = ((u32 *)pd)[lin >> 22];
    if (!(pde & 1)) return -1;
    if ((pde & 0x80) && (cr4 & 0x10)) { *gpa = (pde & 0xFFC00000u) | (lin & 0x3FFFFF); return 0; }
    u8 *pt = gpa_ptr(pde & ~0xFFFu);
    if (!pt) return -1;
    u32 pte = ((u32 *)pt)[(lin >> 12) & 1023];
    if (!(pte & 1)) return -1;
    *gpa = (pte & ~0xFFFu) | (lin & 0xFFF);
    return 0;
}

/* One guest byte at a guest linear address (string I/O). */
static u8 *glin(u32 lin)
{
    u32 gpa;
    if (gwalk(lin, &gpa)) return 0;
    return gpa_ptr(gpa);
}

/* While the BIOS code runs for a guest that pages, the host's linear
   0-1 MiB+64K is what the guest's page tables make of it. */
static int gview;
static int gview_begin(void)
{
    if (gview || !(V32(S_CR0) & 0x80000000u)) return 0;
    if (V32(S_CR4) & 0x20) return -1;
    u64 *pt0 = hv_host_pt0();
    for (u32 i = 0; i < 512; i++) {
        u32 lin = i << 12, gpa;
        /* Not mapped by the guest (Windows leaves video memory unmapped
           in its DOS sessions to catch the first touch): the VM's own page
           at that address, where the write would go once it is mapped.
           The host must never fault on the guest's behalf. */
        u64 e = pt0[i];
        if (lin < GUEST_TOP && !gwalk(lin, &gpa)) {
            if (gpa < 0x200000) e = pt0[gpa >> 12];             /* as the VM sees it (A20, VGA window) */
            else { u8 *p = gpa_ptr(gpa & ~0xFFFu); if (p) e = (u32)(uintptr_t)p | 7; }
        }
        gview_pt[i] = e;
    }
    hv_swap_pde0((u32)(uintptr_t)gview_pt | 7);
    gview = 1;
    return 0;
}
static void gview_end(void)
{
    if (gview) { hv_swap_pde0(0); gview = 0; }
}

/* ---------------- guest registers <-> struct regs ---------------- */
static void get_regs(struct regs *r)
{
    memset(r, 0, sizeof *r);
    r->eax = V32(S_RAX);
    r->ebx = g[0]; r->ecx = g[1]; r->edx = g[2]; r->esi = g[3]; r->edi = g[4]; r->ebp = g[5];
    r->eip = V32(S_RIP); r->eflags = V32(S_RFLAGS); r->esp = V32(S_RSP);
    r->cs = V16(S_CS); r->ss = V16(S_SS);
    r->v86_es = V16(S_ES); r->v86_ds = V16(S_DS); r->v86_fs = V16(S_FS); r->v86_gs = V16(S_GS);
}

static void put_seg(u32 o, u32 sel)               /* real / v86 mode: base = selector * 16 */
{
    if (V16(o) == (u16)sel) return;
    V16(o) = (u16)sel;
    V64(o + 8) = (u32)(u16)sel << 4;
}

static void put_regs(struct regs *r)
{
    V32(S_RAX) = r->eax;
    g[0] = r->ebx; g[1] = r->ecx; g[2] = r->edx; g[3] = r->esi; g[4] = r->edi; g[5] = r->ebp;
    V32(S_RIP) = r->eip; V32(S_RFLAGS) = r->eflags; V32(S_RSP) = r->esp;
    put_seg(S_CS, r->cs); put_seg(S_SS, r->ss);
    put_seg(S_ES, r->v86_es); put_seg(S_DS, r->v86_ds); put_seg(S_FS, r->v86_fs); put_seg(S_GS, r->v86_gs);
}

static int real_or_v86(void) { return !(V32(S_CR0) & 1) || (V32(S_RFLAGS) & EFL_VM); }
/* For vgalog: hex of the VM's code around CS:EIP and the top of its stack
   (what WIN386 does around a VGA access, for disassembling). */
static u8 *glin(u32 lin);
void hv_dump_here(const char *why)
{
    u32 a = (u32)V64(S_CS + 8) + V32(S_RIP) - 0x100;
    kprintf("code %s: %04x:%08x, from %08x:\n", why, V16(S_CS), V32(S_RIP), V32(S_RIP) - 0x100);
    for (u32 l = 0; l < 0x200; l += 32) {
        char b[100]; int n = 0;
        for (u32 i = 0; i < 32; i++) { u8 *p = glin(a + l + i); n += snprintf(b + n, sizeof b - n, "%02x", p ? *p : 0); }
        kprintf("  %s\n", b);
    }
    u32 sp = (u32)V64(S_SS + 8) + (V16(S_SS + 2) & 0x400 ? V32(S_RSP) : (V32(S_RSP) & 0xFFFF));
    char b[200]; int n = 0;
    for (u32 i = 0; i < 24; i++) { u8 *q = glin(sp + i * 4); n += snprintf(b + n, sizeof b - n, " %08x", q ? *(u32 *)q : 0xDEADDEAD); }
    kprintf("stack %04x:%08x:%s\n", V16(S_SS), V32(S_RSP), b);
}
/* For vgalog: the VM is in protected mode (not v86); its CS:EIP. */
int hv_guest_pm(u16 *cs, u32 *eip)
{
    if (!hv_active || real_or_v86()) return 0;
    *cs = V16(S_CS); *eip = V32(S_RIP);
    return 1;
}

/* ---------------- stopping ---------------- */
__attribute__((noreturn)) static void hv_stop(const char *fmt, ...)
{
    char msg[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    gview_end();
    disk_flush();
    kprintf("VMHV: %s\n", msg);
    char full[800];
    snprintf(full, sizeof full, "%s\n\nCS:EIP %04x:%08x  CR0 %08x  EFLAGS %08x\n\nPress a key to restart vmdos.",
             msg, V16(S_CS), V32(S_RIP), V32(S_CR0), V32(S_RFLAGS));
    video_console_attr("VMHV stopped", full, 0x4F);
    while (vkbd_take() >= 0) ;
    for (;;) { int k = vkbd_take(); if (k >= 0 && k < 0x80) break; idle_wait(); }
    reboot();
}

/* Ctrl+Alt+End (vdev.c). */
void hv_hotkey(void)
{
    static u32 last;
    if (last && ticks - last < 3000) { hv_force = 1; last = 0; return; }
    last = ticks;
    vkbd_inject(0xE0); vkbd_inject(0x53);              /* Del, with the Ctrl and Alt already held */
    vkbd_inject(0xE0); vkbd_inject(0xD3);
    video_osd("Ctrl+Alt+End again: force restart");
}

static void force_exit(void)
{
    hv_force = 0;
    video_console_save();
    video_console_attr("VMHV",
        "Restart vmdos now?\n\nAnything the VM hasn't written to the disk yet (open files,\n"
        "SMARTDRV's cache, unsaved work in Windows) is lost.\n\n"
        "Y: restart    any other key: back to the VM", 0x1F);
    while (vkbd_take() >= 0) ;
    for (;;) {
        int k = vkbd_take();
        if (k == 0x15) { disk_flush(); reboot(); }
        if (k >= 0 && k < 0x80 && k != 0x1D && k != 0x38 && k != 0xE0) break;
        idle_wait();
    }
    video_console_restore();
}

/* ---------------- BIOS stubs ---------------- */
static void iret16(struct regs *r, u32 keep)
{
    u32 cur = r->eflags, sp = SP(r);
    u16 ip = rd16(LIN(r->ss, sp)), cs = rd16(LIN(r->ss, sp + 2)), fl = rd16(LIN(r->ss, sp + 4));
    SP(r) = (u16)(sp + 6);
    r->eip = ip; r->cs = cs;
    u32 f = (cur & 0xFFFF0000u) | fl | 2;
    if (cur & EFL_VM) f = (f & ~0x3000u) | (cur & 0x3000u);  /* v86: IOPL stays */
    r->eflags = (f & ~keep) | (cur & keep);
}

static int wait_irq;                  /* the guest waits (HLT, a BIOS call that needs input) */

/* "HLT; db id" at F000:xxxx? Then run the service. 1 if it was one. */
static int bios_stub(void)
{
    if (V32(S_CS + 8) != 0xF0000u || (V32(S_RIP) & 0xFFFF) >= 0x8000 || !real_or_v86()) return 0;
    if (gview_begin()) hv_stop("the guest uses PAE paging (not supported yet)");
    u32 a = 0xF0000u + (V32(S_RIP) & 0xFFFF);
    if (rd8(a) != 0xF4) return 0;
    int id = rd8(a + 1);
    struct regs r;
    get_regs(&r);
    if (id == 0x16 && menu_key) {                                   /* type the menu choice */
        int now = !menu_wait;                                       /* VMHV n: at once */
        if (menu_wait) {                                            /* VMHV: the user may pick first */
            if (!menu_t0) menu_t0 = ticks | 1;                      /* the menu's first look at the keyboard */
            if (rd16(BDA + 0x1A) != rd16(BDA + 0x1C)) menu_key = 0; /* a key is waiting: theirs */
            else if (ticks - menu_t0 >= menu_ms) now = 1;
        }
        if (now && menu_key) {
            vkbd_inject(menu_key); vkbd_inject(menu_key | 0x80);
            menu_key = 0;
        }
    }
    int res = bios_service(&r, id, 1);
    switch (res) {
    case BIOS_DONE: iret16(&r, 0); break;
    case BIOS_DONEF: iret16(&r, EFL_CF | EFL_ZF); break;
    case BIOS_CONT: IP(&r) += 2; break;
    case BIOS_RETRY: r.eflags |= EFL_IF; wait_irq = 1; break;       /* run it again after an interrupt */
    default: hv_stop("BIOS service %02x returned %d", id, res);
    }
    put_regs(&r);
    return 1;
}

/* The VESA protected-mode interface (INT 10h 4F0Ah): programs copy its
   routines ("HLT; db id; RET") into their own code and call them near, in
   protected mode at any privilege level: HLT exits at ring 0 and faults at
   ring 3. Set window, display start, palette (from ES:EDI, copied out of
   the guest through its page tables). 1 if it was one. */
static int vbe_pm_stub(void)
{
    if (real_or_v86()) return 0;
    u32 a = (u32)V64(S_CS + 8) + V32(S_RIP);
    u8 *p0 = glin(a), *p1 = glin(a + 1), *p2 = glin(a + 2);
    if (!p0 || !p1 || !p2 || *p0 != 0xF4 || *p1 < 0x47 || *p1 > 0x49 || *p2 != 0xC3) return 0;
    static u8 pal[256 * 4];
    u32 pal_lin = 0;
    if (*p1 == 0x49) {
        u32 src = (u32)V64(S_ES + 8) + ((V16(S_CS + 2) & 0x400) ? g[4] : (g[4] & 0xFFFF));
        u32 n = (g[1] & 0xFFFF) > 256 ? 256 : (g[1] & 0xFFFF);
        for (u32 i = 0; i < n * 4; i++) { u8 *q = glin(src + i); pal[i] = q ? *q : 0; }
        pal_lin = (u32)(uintptr_t)pal;
    }
    video_vbe_pm(*p1, g[0], g[1], g[2], pal_lin);
    V32(S_RIP) += 2;                                              /* on to the RET */
    return 1;
}

/* A string instruction that ran off the end of a 64 KB segment in real or
   V86 mode: an 8086 wraps the offset around, a 386 raises #GP, and old
   programs (Caveman Ugh-Lympics) count on the wrap. vmdos's V86 monitor
   emulates it; in the VM the #GP is intercepted before EMM386 gets it
   (which doesn't), and the same code runs here. 1 if it was one. */
static int wrap_string(void)
{
    if (gview_begin()) return 0;
    struct regs r;
    get_regs(&r);
    if (!v86_wrap_string(&r)) return 0;
    put_regs(&r);
    return 1;
}

/* ---------------- I/O ---------------- */
static void io_exit(void)
{
    u32 info = V32(C_INFO1), next = V32(C_INFO2);
    u16 port = (u16)(info >> 16);
    int in = info & 1, str = info & 4, rep = info & 8;
    int sz = (info & 0x10) ? 1 : (info & 0x20) ? 2 : 4;
    if (!str) {
        u32 m = sz == 1 ? 0xFF : sz == 2 ? 0xFFFF : 0xFFFFFFFFu;
        if (in) V32(S_RAX) = (V32(S_RAX) & ~m) | (port_in(port, sz) & m);
        else port_out(port, V32(S_RAX) & m, sz);
        V32(S_RIP) = next;
        return;
    }
    /* INS / OUTS */
    int a32 = (info >> 8) & 1;                                    /* bits 7-9: A16 A32 A64 */
    u32 am = a32 ? 0xFFFFFFFFu : 0xFFFF;
    static const u32 segs[6] = { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS };
    u32 sbase = (u32)V64(segs[(info >> 10) & 7] + 8), dbase = (u32)V64(S_ES + 8);
    int step = (V32(S_RFLAGS) & EFL_DF) ? -sz : sz;
    u32 count = rep ? (g[1] & am) : 1;
    for (; count; count--) {
        if (in) {
            u32 v = port_in(port, sz);
            for (int i = 0; i < sz; i++) { u8 *p = glin(dbase + ((g[4] + i) & am)); if (p) *p = (u8)(v >> (8 * i)); }
            g[4] = (g[4] & ~am) | ((g[4] + step) & am);
        } else {
            u32 v = 0;
            for (int i = 0; i < sz; i++) { u8 *p = glin(sbase + ((g[3] + i) & am)); v |= (u32)(p ? *p : 0xFF) << (8 * i); }
            port_out(port, v, sz);
            g[3] = (g[3] & ~am) | ((g[3] + step) & am);
        }
        if (rep) g[1] = (g[1] & ~am) | ((g[1] - 1) & am);
    }
    V32(S_RIP) = next;
}

/* ---------------- MSRs: the guest sees its own EFER and PAT; the rest
   read 0 (the time stamp counter: the real one) and writes are dropped ---------------- */
static void msr_exit(void)
{
    u32 m = g[1];
    if (V32(C_INFO1) & 1) {                                        /* WRMSR */
        u64 v = (u64)g[2] << 32 | V32(S_RAX);
        if (m == 0xC0000080) V64(S_EFER) = v | 0x1000;            /* SVME stays on for the guest */
        else if (m == 0x277) V64(S_GPAT) = v;
        else dbg(1, "VMHV: WRMSR %x <- %x:%x ignored\n", m, g[2], V32(S_RAX));
    } else {
        u32 lo = 0, hi = 0;
        if (m == 0x10) __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
        else if (m == 0xC0000080) lo = (u32)V64(S_EFER) & ~0x1000u;
        else if (m == 0x277) { lo = (u32)V64(S_GPAT); hi = (u32)(V64(S_GPAT) >> 32); }
        else dbg(1, "VMHV: RDMSR %x\n", m);
        V32(S_RAX) = lo; g[2] = hi;
    }
    V32(S_RIP) += 2;
}

/* ---------------- nested page faults ----------------
   The trapped VGA window (planar modes), and writes to the empty upper
   memory area: guest-physical C0000-EFFFF reads FFh like a PC's empty bus
   (so EMM386 finds it free for UMBs and the EMS page frame) and drops
   writes. Both emulate the instruction (mememu.c), reading the guest's
   memory through its own page tables (emu_ptr). */
static u8 *hole_page, *vrom;
#define VROM_SIZE 0x8000u
#define HOLE_LO 0xC0000u
#define HOLE_HI 0xF0000u

/* The emulator's view of guest memory: guest linear -> guest physical
   (the guest's paging), then the VGA window's logic, or the page the VM
   has there (below 2 MB as the host's page table maps it now: A20, a
   mapped VGA plane, the read-only empty area). */
static u8 *emu_ptr(u32 lin, u32 *gpa, int write)
{
    if (gwalk(lin, gpa)) return 0;
    if (*gpa < 0x200000) {
        u64 e = hv_host_pt0()[*gpa >> 12];
        if (!(e & 1) || (write && !(e & 2))) return 0;
        return (u8 *)(uintptr_t)((u32)(e & ~0xFFFull) + (*gpa & 0xFFF));
    }
    return gpa_ptr(*gpa);
}
u8 hv_emu_rd8(u32 lin)
{
    u32 gpa;
    u8 *p = emu_ptr(lin, &gpa, 0);
    if (gpa >= 0xA0000 && gpa < 0xC0000 && vga16_window(gpa)) return vga16_read(gpa);
    return p ? *p : 0xFF;
}
void hv_emu_wr8(u32 lin, u8 v)
{
    u32 gpa;
    u8 *p = emu_ptr(lin, &gpa, 1);
    if (gpa >= 0xA0000 && gpa < 0xC0000 && vga16_window(gpa)) vga16_write(gpa, v);
    else if (p) *p = v;                                           /* else dropped (empty space) */
}

static void emulate_at(u32 gpa, int vga)
{
    u32 csb = (u32)V64(S_CS + 8), eip = V32(S_RIP);
    if (V32(S_CR4) & 0x20) hv_stop("the guest uses PAE paging (not supported yet)");
    u32 r[8] = { V32(S_RAX), g[1], g[2], g[0], V32(S_RSP), g[5], g[3], g[4] };
    u32 rip = eip, fl = V32(S_RFLAGS);
    int code32 = !real_or_v86() && (V16(S_CS + 2) & 0x400);
    struct emu_cpu e = { { &r[0], &r[1], &r[2], &r[3], &r[4], &r[5], &r[6], &r[7] }, &rip, &fl,
                         { (u32)V64(S_ES + 8), csb, (u32)V64(S_SS + 8), (u32)V64(S_DS + 8),
                           (u32)V64(S_FS + 8), (u32)V64(S_GS + 8) }, code32 };
    if (!(vga ? vga16_fault(&e, gpa) : mem_emulate(&e)))
        hv_stop("an access the emulator can't do, address %x", gpa);
    V32(S_RAX) = r[0]; g[1] = r[1]; g[2] = r[2]; g[0] = r[3]; V32(S_RSP) = r[4];
    g[5] = r[5]; g[3] = r[6]; g[4] = r[7];
    V32(S_RIP) = rip; V32(S_RFLAGS) = fl;
}

static void npf_exit(void)
{
    u32 gpa = (u32)V64(C_INFO2);
    if (gpa >= 0xA0000 && gpa < 0xC0000 && vga16_window(gpa)) { emulate_at(gpa, 1); return; }
    if (gpa >= HOLE_LO && gpa < HOLE_HI && (V32(C_INFO1) & 2)) { emulate_at(gpa, 0); return; }
    hv_stop("the guest touched memory it doesn't have: physical %x (%s)", gpa,
            (V32(C_INFO1) & 2) ? "write" : "read");
}

/* ---------------- interrupts ---------------- */
static u32 vintr_base = 1u << 24;                                 /* V_INTR_MASKING */

static void inject(void)
{
    if ((u32)V64(C_EVENT) & EV_VALID) return;                     /* something already going in */
    vkbd_refill();
    int vec = vpic_pending();
    if (vec < 0) { V32(C_VINTR) = vintr_base; V32(C_MISC1) &= ~I1_VINTR; return; }
    if ((V32(S_RFLAGS) & EFL_IF) && !(V32(C_SHADOW) & 1)) {
        vpic_ack(vec);
        V64(C_EVENT) = (u32)vec | EV_VALID;                       /* external interrupt */
        V32(C_VINTR) = vintr_base;
        V32(C_MISC1) &= ~I1_VINTR;
    } else {                                                      /* tell us when it can take one */
        V32(C_VINTR) = vintr_base | (1u << 8) | (0xFu << 16) | (1u << 20);
        V32(C_MISC1) |= I1_VINTR;
    }
}

static void wait_for_guest_irq(void)
{
    for (;;) {
        vkbd_refill();
        if (vpic_pending() >= 0 || hv_force) return;
        idle_wait();
    }
}

/* ---------------- setup ---------------- */
static int svm_check(void)
{
    u32 a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    if (b == 0x756E6547) return 5;                                 /* "Genu"ineIntel: VT-x (hvvmx.c) */
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a < 0x8000000A) return 1;
    cpuid(0x80000001, &a, &b, &c, &d);
    if (!(c & 4)) return 1;
    u32 lo, hi;
    rdmsr(0xC0010114, &lo, &hi);                                  /* VM_CR */
    if (lo & 0x10) return 2;                                      /* SVMDIS: off in the firmware */
    cpuid(0x8000000A, &a, &b, &c, &d);
    if (!(d & 1)) return 3;                                       /* no nested paging */
    if (!hv_host_pae()) return 6;
    return 0;
}

static const char *const hv_err[] = {
    "", "this CPU has no hardware virtualization (AMD-V or VT-x)", "AMD-V is turned off in the firmware setup",
    "this CPU's AMD-V has no nested paging", "not enough memory for the VM",
    "", "vmdos runs without PAE paging (nopae)",
    "VT-x is turned off in the firmware setup",
    "this CPU's VT-x lacks EPT or unrestricted guest (Intel CPUs from 2010 on have them)",
    "VT-x could not be started",
};
static int vmx;                       /* Intel VT-x (hvvmx.c) instead of AMD-V */
int vmx_check(void);
int vmx_setup(u8 *shadow);
void vmx_guest_init(void);
void vmx_run(u8 *shadow, u32 *g);

static int hv_setup(void)
{
    int e = svm_check();
    if (e == 5) { e = vmx_check(); vmx = !e; }
    if (e) return e;
    if (!hv_host_pae()) return 6;
    if (!ext) ext = phys_try_alloc(VM_TOP - EXT_START);
    if (!ext) return 4;
    if (vmx) {                                                    /* the VMCB is VT-x's shadow of the VMCS */
        vmcb = phys_alloc(4096);
        npt_init();                                               /* (maps the VM's memory into the host) */
        return vmx_setup(vmcb);
    }
    vmcb = phys_alloc(4096); hsave = phys_alloc(4096); hvsave = phys_alloc(4096);
    iopm = phys_alloc(12288); msrpm = phys_alloc(8192);
    memset(iopm, 0xFF, 12288);                                    /* every port exits */
    memset(msrpm, 0xFF, 8192);                                    /* every MSR exits */
    npt_init();
    u32 lo, hi;
    rdmsr(0xC0000080, &lo, &hi);
    wrmsr(0xC0000080, lo | 0x1000, hi);                           /* EFER.SVME */
    wrmsr(0xC0010117, (u32)(uintptr_t)hsave, 0);                  /* VM_HSAVE_PA */
    return 0;
}

static void vmcb_init(struct regs *r)
{
    memset(vmcb, 0, 4096);
    V32(C_EXC) = 1u << 13;                                        /* #GP: HLT in v86 mode (BIOS stubs) */
    V32(C_MISC1) = I1_INTR | I1_HLT | I1_IOIO | I1_MSR | I1_SHUTDOWN;
    V32(C_MISC2) = I2_SVM;
    V64(C_IOPM) = (u32)(uintptr_t)iopm;
    V64(C_MSRPM) = (u32)(uintptr_t)msrpm;
    V32(C_ASID) = 1;
    V8(C_TLB) = 1;
    V32(C_VINTR) = vintr_base;
    V64(C_NP) = 1;
    V64(C_NCR3) = (u32)(uintptr_t)npt_pdpt;

    setseg(S_CS, (u16)r->cs, (u32)(u16)r->cs << 4, 0xFFFF, 0x009B);
    setseg(S_SS, (u16)r->ss, (u32)(u16)r->ss << 4, 0xFFFF, 0x0093);
    setseg(S_DS, (u16)r->v86_ds, (u32)(u16)r->v86_ds << 4, 0xFFFF, 0x0093);
    setseg(S_ES, (u16)r->v86_es, (u32)(u16)r->v86_es << 4, 0xFFFF, 0x0093);
    setseg(S_FS, (u16)r->v86_fs, (u32)(u16)r->v86_fs << 4, 0xFFFF, 0x0093);
    setseg(S_GS, (u16)r->v86_gs, (u32)(u16)r->v86_gs << 4, 0xFFFF, 0x0093);
    setseg(S_GDTR, 0, 0, 0xFFFF, 0);
    setseg(S_IDTR, 0, 0, 0x3FF, 0);
    setseg(S_LDTR, 0, 0, 0xFFFF, 0x0082);
    setseg(S_TR, 0, 0, 0xFFFF, 0x008B);
    V8(S_CPL) = 0;
    V64(S_EFER) = 0x1000;
    V64(S_CR0) = 0x10;                                            /* real mode, ET */
    V64(S_DR6) = 0xFFFF0FF0; V64(S_DR7) = 0x400;
    V64(S_RFLAGS) = 0x202;
    V64(S_RIP) = r->eip & 0xFFFF;
    V64(S_RSP) = r->esp & 0xFFFF;
    V64(S_RAX) = r->eax;
    V64(S_GPAT) = 0x0007040600070406ull;
    g[0] = r->ebx; g[1] = r->ecx; g[2] = r->edx; g[3] = r->esi; g[4] = r->edi; g[5] = r->ebp;
}

/* The FDCONFIG.SYS menu choice for the VM: the first "MENU n = ..." line
   that mentions VMHV. 0 if none (or no FDCONFIG.SYS). Also notes
   MENUDEFAULT's timeout, so the choice goes in just before it runs out. */
static int menu_choice(void)
{
    static struct fatvol v;
    static u8 buf[4096 + 1];
    u32 clus, size;
    struct extent ext1[1];
    if (disk_volume(&v) || fat_lookup(&v, "\\FDCONFIG.SYS", &clus, &size) || !size) return 0;
    if (fat_extents(&v, clus, size, ext1, 1) < 1) return 0;
    u32 n = size < 4096 ? size : 4096, secs = (n + 511) / 512;
    if (secs > ext1[0].count) secs = ext1[0].count;
    if (disk_read(ext1[0].lba, secs, buf)) return 0;
    if (n > secs * 512) n = secs * 512;
    buf[n] = 0;
    int choice = 0;
    for (char *l = (char *)buf; *l; ) {
        if (!strncmp(l, "MENUDEFAULT", 11) || !strncmp(l, "menudefault", 11)) {
            char *c = l;
            while (*c && *c != '\n' && *c != ',') c++;
            if (*c == ',') {
                u32 sec = 0;
                for (c++; *c == ' '; c++) ;
                while (*c >= '0' && *c <= '9') sec = sec * 10 + (u32)(*c++ - '0');
                menu_ms = sec ? sec * 1000 - 500 : 0;
            }
        }
        if (!choice && (l[0] | 32) == 'm' && (l[1] | 32) == 'e' && (l[2] | 32) == 'n' && (l[3] | 32) == 'u' &&
            l[4] == ' ' && l[5] >= '1' && l[5] <= '9')
            for (char *p = l; *p && *p != '\n'; p++)
                if ((p[0] | 32) == 'v' && (p[1] | 32) == 'm' && (p[2] | 32) == 'h' && (p[3] | 32) == 'v') { choice = l[5]; break; }
        while (*l && *l != '\n') l++;
        if (*l) l++;
    }
    return choice;
}

/* Restart the PC's DOS in the VM. Returns only on failure (an hv_err code). */
void vpic_reset(void);
void xms_restart(void);
void ems_restart(void);
void mouse_restart(void);
void cdaudio_stop(void);
void sb_out_stop(void);
extern int xms_hidden;

int hv_start(int key)
{
    int e = hv_setup();
    if (e) { kprintf("VMHV: %s\n", hv_err[e]); return e; }
    kprintf("VMHV: restarting DOS in a virtual machine (%s, 640 KB + %u KB)\n", vmx ? "Intel VT-x" : "AMD-V", (VM_TOP >> 10) - 1024);
    disk_flush();
    cdaudio_stop();
    sb_out_stop();
    mouse_restart();
    ems_restart();
    xms_restart();
    xms_hidden = 1;                                               /* the VM brings its own (HIMEM) */
    vpic_reset();
    vdev_init();
    bios_init();
    memset(ext, 0, VM_TOP - EXT_START);
    if (!hole_page) { hole_page = phys_alloc(4096); memset(hole_page, 0xFF, 4096); }
    for (u32 a = HOLE_LO; a < HOLE_HI; a += 4096) map_page(a, (u32)(uintptr_t)hole_page, 5);   /* read-only */
    /* A video BIOS ROM at C000h (32 KB), as on any PC with a VGA card: the
       55AAh signature, a checksum, and INT 10h entering it (a far jump to
       vmdos's INT 10h stub). Windows' display code expects one there. */
    if (!vrom) {
        vrom = phys_alloc(VROM_SIZE);
        memset(vrom, 0xFF, VROM_SIZE);
        u16 svc10 = rd16(0xF0000u + 0x10 * 2);
        u8 hdr[] = { 0x55, 0xAA, VROM_SIZE / 512, 0xCB,          /* init entry: RETF */
                     0xEA, (u8)svc10, (u8)(svc10 >> 8), 0x00, 0xF0 };   /* C000:0004: JMP F000:int10 */
        memcpy(vrom, hdr, sizeof hdr);
        memcpy(vrom + 0x1E, "IBM VGA", 7);            /* where some software looks for the name */
        u8 sum = 0;
        for (u32 i = 0; i < VROM_SIZE - 1; i++) sum += vrom[i];
        vrom[VROM_SIZE - 1] = (u8)-sum;
    }
    for (u32 a = 0; a < VROM_SIZE; a += 4096) map_page(HOLE_LO + a, (u32)(uintptr_t)vrom + a, 5);
    tlb_flush();
    wr16(0x10 * 4, 0x0004); wr16(0x10 * 4 + 2, 0xC000);     /* INT 10h into the ROM */
    menu_wait = !key;
    menu_t0 = 0;
    if (!key) key = menu_choice();
    dbg(1, "VMHV: menu choice %c, after %u ms\n", key ? key : '-', menu_ms);
    if (key >= '1' && key <= '9') menu_key = (u8)(key - '1' + 2); /* scan codes 02h-0Ah */
    static struct regs r0;
    memset(&r0, 0, sizeof r0);
    bios_boot(&r0);
    vmcb_init(&r0);
    if (vmx) vmx_guest_init();
    hv_active = 1;

    for (;;) {
        if (hv_force) force_exit();
        if (debug_level >= 3) {                                    /* debug=3: the text screen to the log, every 20 s */
            static u32 last_dump;
            if (ticks - last_dump >= 20000) {
                last_dump = ticks;
                kprintf("---- screen (mode %02x) ----\n", video_mode);
                for (int y = 0; y < 25; y++) {
                    char line[81];
                    int n = 0;
                    for (int x = 0; x < 80; x++) { u8 c = rd8(0xB8000 + (y * 80 + x) * 2); line[x] = c >= 32 && c < 127 ? (char)c : ' '; if (c > 32) n = x + 1; }
                    line[n] = 0;
                    kprintf("%s\n", line);
                }
            }
        }
        if (wait_irq) {
            wait_irq = 0;
            wait_for_guest_irq();
        }
        inject();
        V8(C_TLB) = hv_tlb_dirty ? 1 : 0;
        hv_tlb_dirty = 0;
        V32(C_CLEAN) = 0;
        if (vmx) vmx_run(vmcb, g);
        else svm_run((u32)(uintptr_t)vmcb, (u32)(uintptr_t)hvsave, g);

        u64 ei = V64(C_EXITINT);                                  /* an event cut short: send it again */
        V64(C_EVENT) = (ei & EV_VALID) ? ei : 0;
        u32 code = V32(C_EXIT);
        {
            static u32 n, last, nio, nhlt, ngp, nintr;
            n++;
            if (debug_level >= 3 && n < 300)
                kprintf("x%u %x %04x:%04x i1 %x i2 %x fl %x\n", n, code, V16(S_CS), V32(S_RIP), (u32)V64(C_INFO1), (u32)V64(C_INFO2), V32(S_RFLAGS));
            if (code == X_IOIO) nio++; else if (code == X_HLT) nhlt++; else if (code == X_EXC(13)) ngp++; else if (code == X_INTR) nintr++;
            if (debug_level >= 2 && ticks - last >= 2000) {
                last = ticks;
                kprintf("VMHV: %u exits (io %u hlt %u gp %u intr %u), last %x, at %04x:%08x CR0 %x FL %x\n",
                        n, nio, nhlt, ngp, nintr, code, V16(S_CS), V32(S_RIP), V32(S_CR0), V32(S_RFLAGS));
            }
        }
        switch (code) {
        case X_INTR: break;
        case X_VINTR:
            V32(C_VINTR) = vintr_base;
            V32(C_MISC1) &= ~I1_VINTR;
            break;
        case X_IOIO: io_exit(); break;
        case X_MSR: msr_exit(); break;
        case X_HLT:
            if (bios_stub() || vbe_pm_stub()) break;
            V32(S_RIP) += 1;
            if (!(V32(S_RFLAGS) & EFL_IF)) hv_stop("the guest has halted (HLT with interrupts off): it is safe to restart");
            wait_irq = 1;
            break;
        case X_EXC(13):
            if ((V32(S_RFLAGS) & EFL_VM) ? bios_stub() : vbe_pm_stub()) break;
            if (real_or_v86() && wrap_string()) break;
            if (debug_level >= 2) {
                static int shown;
                if (shown++ < 8) {
                    u32 a = (u32)V64(S_CS + 8) + V32(S_RIP);
                    u8 *p = glin(a);
                    kprintf("VMHV: #GP err %x at %04x:%08x (lin %x): %02x %02x %02x %02x, IDTR %x/%x\n",
                            (u32)V64(C_INFO1), V16(S_CS), V32(S_RIP), a, p ? p[0] : 0, p ? p[1] : 0, p ? p[2] : 0, p ? p[3] : 0,
                            (u32)V64(S_IDTR + 8), V32(S_IDTR + 4));
                    u32 sp = (u32)V64(S_SS + 8) + (V16(S_SS + 2) & 0x400 ? V32(S_RSP) : (V32(S_RSP) & 0xFFFF));
                    u32 st[4];
                    for (int i = 0; i < 4; i++) { u8 *q = glin(sp + i * 4); st[i] = q ? *(u32 *)q : 0xDEAD; }
                    u32 gd = (u32)V64(S_GDTR + 8);
                    u8 *d8 = glin(gd + 8);
                    kprintf("      SS %04x attr %x ESP %x stack %x %x %x %x; GDT %x/%x [8] = %08x %08x; CS attr %x CPL %u\n",
                            V16(S_SS), V16(S_SS + 2), V32(S_RSP), st[0], st[1], st[2], st[3], gd, V32(S_GDTR + 4),
                            d8 ? ((u32 *)d8)[0] : 0, d8 ? ((u32 *)d8)[1] : 0, V16(S_CS + 2), V8(S_CPL));
                }
            }
            V64(C_EVENT) = 13 | EV_EXC | EV_ERR | EV_VALID | (u64)(u32)V64(C_INFO1) << 32;
            break;
        case X_NPF: npf_exit(); break;
        case X_SHUT:
            kprintf("VMHV: the guest triple-faulted: restarting\n");
            reboot();
        case 0xF00: break;                                        /* VT-x: done in hvvmx.c (CPUID, CR access) */
        case 0xF01:                                               /* VT-x: entry failed, or an exit it can't do */
            hv_stop("VT-x: %s %u (%x)", (V32(C_INFO1) >> 16) == 1 ? "VM entry failed, VMX instruction error" :
                    (V32(C_INFO1) >> 16) == 2 ? "VM entry failed on the guest state, reason" : "unexpected VM exit, reason",
                    V32(C_INFO1) & 0xFFFF, (u32)V64(C_INFO2));
        default:
            if (code >= 0x80 && code <= 0x86) {                   /* SVM instructions: not for the guest */
                V64(C_EVENT) = 6 | EV_EXC | EV_VALID;
                break;
            }
            hv_stop("unexpected exit %x (info %x %x)", code, (u32)V64(C_INFO1), (u32)V64(C_INFO2));
        }
        gview_end();
        __asm__ volatile("sti; nop; cli" ::: "memory");          /* the host's interrupts */
    }
}

/* CMOS extended memory sizes (registers 17h/18h, 30h/31h) and the INT 15h
   memory calls, for the VM (bios.c, vdev.c). */
u32 hv_ext_kb(void) { return hv_active ? (VM_TOP >> 10) - 1024 : 0; }

int hv_e820(struct regs *r)
{
    static const struct { u32 base, len, type; } map[] = {
        { 0, 0xA0000, 1 }, { 0xF0000, 0x10000, 2 }, { 0x100000, VM_TOP - 0x100000, 1 },
    };
    u32 i = r->ebx;
    if (i >= 3 || r->ecx < 20) return -1;
    u32 a = LIN(r->v86_es, DI(r));
    wr32(a, map[i].base); wr32(a + 4, 0); wr32(a + 8, map[i].len); wr32(a + 12, 0); wr32(a + 16, map[i].type);
    r->eax = 0x534D4150;
    r->ecx = 20;
    r->ebx = i + 1 < 3 ? i + 1 : 0;
    return 0;
}
