/* VMHV on Intel VT-x.

   hv.c is written against AMD-V: its exit handling reads and writes a
   VMCB. On Intel the same page serves as a shadow: before each VM entry
   the guest state hv.c may have changed is copied from it into the VMCS,
   and after each exit the VMCS is copied back and the exit is translated
   into AMD-V's terms (exit code, EXITINFO1/2, the event that was being
   delivered), so every handler in hv.c works on both. What only VT-x
   needs is handled here: CPUID (always exits), CR0/CR4 bits the CPU
   insists on (NE, VMXE: guest/host masks and read shadows), CR3 and
   INVLPG exits on CPUs that can't turn them off.

   Memory: EPT can't share the host's page tables as AMD's nested paging
   does (its entries have another format), so it mirrors them: every
   change to the host's linear 0-10 MiB (map_page: A20, the VGA window,
   VESA banks, the empty upper memory) is copied into the EPT, and the
   EPT TLB is flushed (INVEPT) before the next entry.

   Needs EPT and "unrestricted guest" (to run real mode and big real mode
   natively): Intel CPUs from Westmere (2010) on. */
#include "kernel.h"

/* the shadow VMCB (offsets as in hv.c) */
#define C_MISC1   0x00C
#define C_TLB     0x05C
#define C_SHADOW  0x068
#define C_EXIT    0x070
#define C_INFO1   0x078
#define C_INFO2   0x080
#define C_EXITINT 0x088
#define C_EVENT   0x0A8
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
#define S_RFLAGS  0x570
#define S_RIP     0x578
#define S_RSP     0x5D8
#define S_RAX     0x5F8
#define I1_VINTR  (1u << 4)
#define X_EXC(n)  (0x40 + (n))
#define X_INTR    0x60
#define X_VINTR   0x64
#define X_HLT     0x78
#define X_IOIO    0x7B
#define X_MSR     0x7C
#define X_SHUT    0x7F
#define X_SVMINS  0x81                 /* hv.c injects #UD */
#define X_NPF     0x400
#define X_NOP     0xF00                /* handled here: nothing more to do */
#define X_FAIL    0xF01                /* VM entry failed / can't go on: INFO1 says why */

static u8 *sh;
#define V16(o) (*(volatile u16 *)(sh + (o)))
#define V32(o) (*(volatile u32 *)(sh + (o)))
#define V64(o) (*(volatile u64 *)(sh + (o)))

/* ---- VMCS fields ---- */
enum {
    G_ES_SEL = 0x800, H_ES_SEL = 0xC00, H_CS_SEL = 0xC02, H_SS_SEL = 0xC04, H_DS_SEL = 0xC06,
    H_FS_SEL = 0xC08, H_GS_SEL = 0xC0A, H_TR_SEL = 0xC0C,
    IO_BITMAP_A = 0x2000, IO_BITMAP_B = 0x2002, LINK_PTR = 0x2800, G_DEBUGCTL = 0x2802, EPT_PTR = 0x201A, GPA = 0x2400,
    PIN_CTL = 0x4000, PROC_CTL = 0x4002, EXC_BITMAP = 0x4004, CR3_TGT_COUNT = 0x400A,
    EXIT_CTL = 0x400C, EXIT_MSR_STORE = 0x400E, EXIT_MSR_LOAD = 0x4010, ENTRY_CTL = 0x4012,
    ENTRY_MSR_LOAD = 0x4014, ENTRY_INTR = 0x4016, ENTRY_EXC_ERR = 0x4018, ENTRY_LEN = 0x401A,
    PROC_CTL2 = 0x401E,
    INSN_ERR = 0x4400, EXIT_REASON = 0x4402, EXIT_INTR = 0x4404, EXIT_INTR_ERR = 0x4406,
    IDT_VEC = 0x4408, IDT_VEC_ERR = 0x440A, EXIT_LEN = 0x440C, EXIT_INSN = 0x440E,
    G_ES_LIM = 0x4800, G_GDTR_LIM = 0x4810, G_IDTR_LIM = 0x4812, G_ES_AR = 0x4814,
    G_INTR_STATE = 0x4824, G_ACTIVITY = 0x4826, G_SYSENTER_CS = 0x482A, H_SYSENTER_CS = 0x4C00,
    CR0_MASK = 0x6000, CR4_MASK = 0x6002, CR0_SHADOW = 0x6004, CR4_SHADOW = 0x6006,
    EXIT_QUAL = 0x6400,
    G_CR0 = 0x6800, G_CR3 = 0x6802, G_CR4 = 0x6804, G_ES_BASE = 0x6806, G_GDTR_BASE = 0x6816,
    G_IDTR_BASE = 0x6818, G_DR7 = 0x681A, G_RSP = 0x681C, G_RIP = 0x681E, G_RFLAGS = 0x6820,
    G_PEND_DBG = 0x6822, G_SYSENTER_ESP = 0x6824, G_SYSENTER_EIP = 0x6826,
    H_CR0 = 0x6C00, H_CR3 = 0x6C02, H_CR4 = 0x6C04, H_FS_BASE = 0x6C06, H_GS_BASE = 0x6C08,
    H_TR_BASE = 0x6C0A, H_GDTR_BASE = 0x6C0C, H_IDTR_BASE = 0x6C0E, H_SYSENTER_ESP = 0x6C10,
    H_SYSENTER_EIP = 0x6C12, H_RSP = 0x6C14, H_RIP = 0x6C16,
};
/* ES CS SS DS FS GS LDTR TR: selector, limit, AR, base are consecutive fields 2 apart */
static const u32 shseg[8] = { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS, S_LDTR, S_TR };

static int vmwrite(u32 f, u32 v)
{
    u8 bad;
    __asm__ volatile("vmwrite %2, %1; setna %0" : "=q"(bad) : "r"(f), "rm"(v) : "cc", "memory");
    return bad;
}
static u32 vmread(u32 f)
{
    u32 v = 0;
    __asm__ volatile("vmread %1, %0" : "=rm"(v) : "r"(f) : "cc");
    return v;
}
static void vmwrite64(u32 f, u64 v) { vmwrite(f, (u32)v); vmwrite(f + 1, (u32)(v >> 32)); }
static u64 rdmsr64(u32 m) { u32 lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return (u64)hi << 32 | lo; }
static void wrmsr64(u32 m, u64 v) { __asm__ volatile("wrmsr" :: "a"((u32)v), "d"((u32)(v >> 32)), "c"(m)); }
static void cpuid4(u32 leaf, u32 sub, u32 *r)
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

int vmx_enter(u32 *r, int launched);   /* hvasm.S: 0 after an exit, 1 if the entry failed */
extern u8 vmx_exit_entry[];
extern u64 gdt[];
extern volatile int hv_tlb_dirty;
extern void (*hv_map_hook)(u32 lin);
u64 hv_host_pte(u32 lin);
u32 video_vram_base(u32 *size);
void hv_io_bitmap(u8 *bm);                     /* hv.c: every port exits but video=vbe's card's */

static u8 *vmxon_region, *vmcs;
static u64 *ept_pml4, *ept_pdpt, *ept_pd[4];
static u64 eptp;
static int launched, invept_type;
static u32 cr0_fixed0, cr0_fixed1, cr4_fixed0, cr4_fixed1, cr0_mask, cr4_mask;
static u32 proc_ctl;
static u32 exit_len, exit_rip;

/* ---------------- EPT ---------------- */
#define EPT_RWX 7u
#define EPT_WB  (6u << 3 | 1u << 6)       /* write-back, ignore the guest's PAT */
#define EPT_UC  0u                        /* uncached (video=vbe: the card's VGA memory) */
#define EPT_WC  (1u << 3)                 /* write-combining (with the guest's PAT: the card's LFB) */

static u64 *ept_pt(u32 gpa)
{
    u64 *e = &ept_pd[gpa >> 30][(gpa >> 21) & 511];
    if (!(*e & 7)) *e = (u32)(uintptr_t)phys_alloc(4096) | EPT_RWX;
    return (u64 *)(uintptr_t)(u32)(*e & ~0xFFFull);
}

/* The EPT entry for guest-physical page gpa from the host's PTE for the
   same linear address (the host maps the VM's memory at it). */
static void ept_sync(u32 lin)
{
    u64 h = hv_host_pte(lin);
    u64 e = 0;
    if (h & 1) e = (h & 0x000FFFFFFFFFF000ull) | 5 | ((h & 2) ? 2 : 0) | ((h & 0x10) ? EPT_UC : EPT_WB);   /* R X, W if writable; the host's uncached pages (the card's) uncached */
    ept_pt(lin)[(lin >> 12) & 511] = e;
}

extern void (*hv_ept_map)(u32 gpa, u32 host);
static void ept_map(u32 gpa, u32 host)
{
    ept_pt(gpa)[(gpa >> 12) & 511] = host ? host | EPT_RWX | EPT_WB : 0;
}

static void ept_init(void)
{
    ept_pml4 = phys_alloc(4096);
    ept_pdpt = phys_alloc(4096);
    ept_pml4[0] = (u32)(uintptr_t)ept_pdpt | EPT_RWX;
    for (int i = 0; i < 4; i++) {
        ept_pd[i] = phys_alloc(4096);
        ept_pdpt[i] = (u32)(uintptr_t)ept_pd[i] | EPT_RWX;
    }
    for (u32 a = 0; a < 10u << 20; a += 4096) ept_sync(a);
    for (u32 a = 10u << 20; a < hv_vm_top(); a += 4096)         /* RAM above 10 MB: straight to its block */
        ept_pt(a)[(a >> 12) & 511] = (u32)(uintptr_t)hv_gpa_ptr(a) | EPT_RWX | EPT_WB;
    u32 vsize, vbase = video_vram_base(&vsize);                 /* the VBE framebuffer, at hv_lfb() */
    if (!vga_native)
        for (u32 a = 0; a < vsize; a += 4096) ept_pt(hv_lfb() + a)[((hv_lfb() + a) >> 12) & 511] = (vbase + a) | EPT_RWX | EPT_WB;
    for (int i = 0; i < 6; i++) {                               /* video=vbe: the card's memory where it is (its LFB) */
        u32 base, len;
        if (!vga_native_bar(i, &base, &len) || base < hv_vm_top()) continue;
        if (len > (512u << 20)) len = 512u << 20;
        for (u32 a = 0; a < len && base + a >= base; a += 4096)
            ept_pt(base + a)[((base + a) >> 12) & 511] = (base + a) | EPT_RWX | EPT_WC;
    }
    eptp = (u32)(uintptr_t)ept_pml4 | 6 | 3 << 3;                /* write-back, 4-level walk */
    hv_map_hook = ept_sync;
    hv_ept_map = ept_map;
    v3_hv_sync();
}

static void invept(void)
{
    struct { u64 eptp, zero; } d = { eptp, 0 };
    u32 t = invept_type;
    __asm__ volatile("invept %0, %1" :: "m"(d), "r"(t) : "cc", "memory");
}

/* ---------------- setup ---------------- */
static u32 adjust(u32 want, u32 msr, int *missing)
{
    u64 m = rdmsr64(msr);
    u32 v = (want | (u32)m) & (u32)(m >> 32);                   /* allowed 0s, allowed 1s */
    if (missing && (v & want) != want) *missing = 1;
    return v;
}

/* 0 when VT-x can run the VM, else an hv_err code. */
int vmx_check(void)
{
    u32 r[4];
    cpuid4(1, 0, r);
    if (!(r[2] & 0x20)) return 1;                               /* no VMX */
    u64 fc = rdmsr64(0x3A);                                     /* IA32_FEATURE_CONTROL */
    if (fc & 1) { if (!(fc & 4)) return 7; }                    /* locked with VMX off: the firmware's choice */
    else wrmsr64(0x3A, fc | 5);                                 /* lock it with VMX on */
    u64 basic = rdmsr64(0x480);
    if (!(rdmsr64(0x482) >> 63)) return 8;                      /* no secondary controls */
    u32 p2 = (u32)(rdmsr64(0x48B) >> 32);
    if ((p2 & 0x82) != 0x82) return 8;                          /* EPT and unrestricted guest */
    u64 cap = rdmsr64(0x48C);
    if (!(cap & (1u << 6)) || !(cap & (1u << 14)) || !(cap & (1u << 20))) return 8;   /* 4-level, WB, INVEPT */
    invept_type = (cap & (1u << 25)) ? 1 : 2;                   /* single context, else all */
    (void)basic;
    return 0;
}

int vmx_setup(u8 *shadow)
{
    sh = shadow;
    u64 basic = rdmsr64(0x480);
    int true_ctls = (basic >> 55) & 1;
    vmxon_region = phys_alloc(4096);
    vmcs = phys_alloc(4096);
    *(u32 *)vmxon_region = (u32)basic & 0x7FFFFFFF;
    *(u32 *)vmcs = (u32)basic & 0x7FFFFFFF;
    cr0_fixed0 = (u32)rdmsr64(0x486); cr0_fixed1 = (u32)rdmsr64(0x487);
    cr4_fixed0 = (u32)rdmsr64(0x488); cr4_fixed1 = (u32)rdmsr64(0x489);

    u32 cr0, cr4;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    if ((cr0 & cr0_fixed0) != cr0_fixed0) {                     /* NE, as VMX operation requires */
        cr0 |= cr0_fixed0;
        __asm__ volatile("mov %0,%%cr0" :: "r"(cr0));
    }
    __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
    cr4 |= cr4_fixed0 | 0x2000;                                 /* VMXE */
    __asm__ volatile("mov %0,%%cr4" :: "r"(cr4));
    u64 pa = (u32)(uintptr_t)vmxon_region;
    u8 bad;
    __asm__ volatile("vmxon %1; setna %0" : "=q"(bad) : "m"(pa) : "cc", "memory");
    if (bad) { kprintf("VMHV: VMXON failed\n"); return 9; }
    pa = (u32)(uintptr_t)vmcs;
    __asm__ volatile("vmclear %1; setna %0" : "=q"(bad) : "m"(pa) : "cc", "memory");
    __asm__ volatile("vmptrld %1; setna %0" : "=q"(bad) : "m"(pa) : "cc", "memory");
    if (bad) { kprintf("VMHV: VMPTRLD failed\n"); return 9; }

    int missing = 0;
    vmwrite(PIN_CTL, adjust(1u << 0 | 1u << 3, true_ctls ? 0x48D : 0x481, &missing));   /* external interrupts, NMI */
    proc_ctl = adjust(1u << 7 | 1u << 24 | 1u << 31, true_ctls ? 0x48E : 0x482, &missing); /* HLT, all I/O, secondary */
    if (vga_native) {                                           /* video=vbe: I/O bitmaps, the card's ports not exiting */
        int nobm = 0;
        u32 c = adjust(1u << 7 | 1u << 25 | 1u << 31, true_ctls ? 0x48E : 0x482, &nobm);
        if (!nobm && !(c & (1u << 24))) {
            u8 *bm = phys_alloc(8192);
            hv_io_bitmap(bm);
            vmwrite64(IO_BITMAP_A, (u32)(uintptr_t)bm);
            vmwrite64(IO_BITMAP_B, (u32)(uintptr_t)bm + 4096);
            proc_ctl = c;
        }                                                       /* else every port exits; hv.c sends the card's on */
    }
    vmwrite(PROC_CTL, proc_ctl);
    vmwrite(PROC_CTL2, adjust(1u << 1 | 1u << 7, 0x48B, &missing));                      /* EPT, unrestricted guest */
    vmwrite(EXIT_CTL, adjust(0, true_ctls ? 0x48F : 0x483, 0));                          /* 32-bit host */
    vmwrite(ENTRY_CTL, adjust(0, true_ctls ? 0x490 : 0x484, 0));
    if (missing) { kprintf("VMHV: VT-x controls not available\n"); return 8; }
    vmwrite(EXC_BITMAP, 1u << 13);                              /* #GP: HLT in v86 mode, segment wrap */
    vmwrite(CR3_TGT_COUNT, 0);
    vmwrite(EXIT_MSR_STORE, 0); vmwrite(EXIT_MSR_LOAD, 0); vmwrite(ENTRY_MSR_LOAD, 0);
    vmwrite64(LINK_PTR, ~0ull);
    vmwrite64(G_DEBUGCTL, 0);

    /* CR0/CR4 bits VMX fixes (NE, VMXE; PE and PG are the guest's own with
       unrestricted guest): the guest reads its own values from the shadows */
    cr0_mask = (cr0_fixed0 & ~0x80000001u) | ~cr0_fixed1;
    cr4_mask = cr4_fixed0 | ~cr4_fixed1;
    vmwrite(CR0_MASK, cr0_mask);
    vmwrite(CR4_MASK, cr4_mask);

    /* the host, as it is when it enters */
    struct __attribute__((packed)) { u16 lim; u32 base; } gd, id;
    __asm__ volatile("sgdt %0; sidt %1" : "=m"(gd), "=m"(id));
    u32 cr3;
    __asm__ volatile("mov %%cr0,%0; mov %%cr3,%1; mov %%cr4,%2" : "=r"(cr0), "=r"(cr3), "=r"(cr4));
    vmwrite(H_CR0, cr0); vmwrite(H_CR3, cr3); vmwrite(H_CR4, cr4);
    vmwrite(H_CS_SEL, 0x08); vmwrite(H_SS_SEL, 0x10); vmwrite(H_DS_SEL, 0x10); vmwrite(H_ES_SEL, 0x10);
    vmwrite(H_FS_SEL, 0x10); vmwrite(H_GS_SEL, 0x10); vmwrite(H_TR_SEL, 0x18);
    u64 t = gdt[3];
    vmwrite(H_TR_BASE, (u32)((t >> 16) & 0xFFFFFF) | (u32)((t >> 56) << 24));
    vmwrite(H_FS_BASE, 0); vmwrite(H_GS_BASE, 0);
    vmwrite(H_GDTR_BASE, gd.base); vmwrite(H_IDTR_BASE, id.base);
    vmwrite(H_SYSENTER_CS, 0); vmwrite(H_SYSENTER_ESP, 0); vmwrite(H_SYSENTER_EIP, 0);
    vmwrite(H_RIP, (u32)(uintptr_t)vmx_exit_entry);

    ept_init();
    vmwrite64(EPT_PTR, eptp);
    kprintf("VMHV: Intel VT-x with EPT and unrestricted guest\n");
    return 0;
}

/* ---------------- shadow <-> VMCS ---------------- */
static u32 ar_to_attr(u32 ar) { return (ar & 0x10000) ? 0 : (ar & 0xFF) | ((ar >> 4) & 0xF00); }
static u32 attr_to_ar(u32 a) { return (a & 0x80) ? (a & 0xFF) | ((a & 0xF00) << 4) : 0x10000; }

static void put_guest(void)
{
    for (int i = 0; i < 8; i++) {
        u32 o = shseg[i];
        vmwrite(G_ES_SEL + i * 2, V16(o));
        vmwrite(G_ES_LIM + i * 2, V32(o + 4));
        u32 ar = attr_to_ar(V16(o + 2));
        if (i == 1 || i == 7) ar &= ~0x10000u;                  /* CS and TR are always usable */
        vmwrite(G_ES_AR + i * 2, ar);
        vmwrite(G_ES_BASE + i * 2, (u32)V64(o + 8));
    }
    vmwrite(G_GDTR_LIM, V32(S_GDTR + 4)); vmwrite(G_GDTR_BASE, (u32)V64(S_GDTR + 8));
    vmwrite(G_IDTR_LIM, V32(S_IDTR + 4)); vmwrite(G_IDTR_BASE, (u32)V64(S_IDTR + 8));
    u32 cr0 = V32(S_CR0), cr4 = V32(S_CR4);
    vmwrite(CR0_SHADOW, cr0);
    vmwrite(G_CR0, (cr0 & cr0_fixed1) | (cr0_fixed0 & ~0x80000001u));
    vmwrite(CR4_SHADOW, cr4);
    vmwrite(G_CR4, (cr4 & cr4_fixed1) | cr4_fixed0);
    vmwrite(G_CR3, V32(S_CR3));
    vmwrite(G_DR7, V32(S_DR7));
    vmwrite(G_RSP, V32(S_RSP));
    vmwrite(G_RIP, V32(S_RIP));
    vmwrite(G_RFLAGS, V32(S_RFLAGS));
}

static void get_guest(void)
{
    for (int i = 0; i < 8; i++) {
        u32 o = shseg[i];
        V16(o) = (u16)vmread(G_ES_SEL + i * 2);
        V32(o + 4) = vmread(G_ES_LIM + i * 2);
        V16(o + 2) = (u16)ar_to_attr(vmread(G_ES_AR + i * 2));
        V64(o + 8) = vmread(G_ES_BASE + i * 2);
    }
    V32(S_GDTR + 4) = vmread(G_GDTR_LIM); V64(S_GDTR + 8) = vmread(G_GDTR_BASE);
    V32(S_IDTR + 4) = vmread(G_IDTR_LIM); V64(S_IDTR + 8) = vmread(G_IDTR_BASE);
    V64(S_CR0) = (vmread(G_CR0) & ~cr0_mask) | (vmread(CR0_SHADOW) & cr0_mask);
    V64(S_CR4) = (vmread(G_CR4) & ~cr4_mask) | (vmread(CR4_SHADOW) & cr4_mask);
    V64(S_CR3) = vmread(G_CR3);
    V64(S_DR7) = vmread(G_DR7);
    V64(S_RSP) = vmread(G_RSP);
    V64(S_RIP) = vmread(G_RIP);
    V64(S_RFLAGS) = vmread(G_RFLAGS);
    *(volatile u8 *)(sh + S_CPL) = (u8)((V16(S_SS + 2) >> 5) & 3);
}

/* the first entry: everything hv.c put in the shadow */
void vmx_guest_init(void)
{
    put_guest();
    vmwrite(G_INTR_STATE, 0);
    vmwrite(G_ACTIVITY, 0);
    vmwrite(G_PEND_DBG, 0);
    vmwrite(G_SYSENTER_CS, 0); vmwrite(G_SYSENTER_ESP, 0); vmwrite(G_SYSENTER_EIP, 0);
    launched = 0;
}

/* ---------------- exits VT-x has and AMD-V doesn't ---------------- */
static u32 *gpr(u32 *r, int n)                                  /* r: eax ebx ecx edx esi edi ebp */
{
    static u32 rsp;
    switch (n) {
    case 0: return &r[0]; case 1: return &r[2]; case 2: return &r[3]; case 3: return &r[1];
    case 4: rsp = V32(S_RSP); return &rsp;
    case 5: return &r[6]; case 6: return &r[4]; default: return &r[5];
    }
}

static void cr_access(u32 *r)
{
    u32 q = vmread(EXIT_QUAL), cr = q & 15, type = (q >> 4) & 3, n = (q >> 8) & 15;
    if (type == 0) {                                            /* MOV to CR */
        u32 v = *gpr(r, n);
        if (cr == 0) V64(S_CR0) = v;
        else if (cr == 4) V64(S_CR4) = v;
        else if (cr == 3) V64(S_CR3) = v;
    } else if (type == 1) {                                     /* MOV from CR (CR3 exiting) */
        u32 *p = gpr(r, n);
        *p = cr == 3 ? V32(S_CR3) : cr == 0 ? V32(S_CR0) : V32(S_CR4);
        if (n == 4) V64(S_RSP) = *p;
    } else if (type == 3) {                                     /* LMSW: PE can be set, not cleared */
        u32 v = (q >> 16) & 15, c = V32(S_CR0);
        V64(S_CR0) = (c & ~0xEu) | (v & 0xE) | ((c | v) & 1);
    }
    V64(S_RIP) = V32(S_RIP) + exit_len;
}

static void do_cpuid(u32 *r)
{
    u32 o[4];
    cpuid4(r[0], r[2], o);
    if (r[0] == 1) o[2] &= ~0x20u;                              /* no VMX inside */
    r[0] = o[0]; r[1] = o[1]; r[2] = o[2]; r[3] = o[3];
    V64(S_RIP) = V32(S_RIP) + exit_len;
}

/* ---------------- task switches ----------------
   VT-x always exits on one (AMD-V lets them run): a far CALL or JMP to a
   TSS or task gate, an IRET with NT set, an interrupt or exception through
   a task gate. CWSDPMI (DJGPP programs: Quake ...) switches between itself
   and the program this way. Done here as the CPU does it, for 32-bit TSSes,
   without its checks (a bad TSS stops the VM as before). */
static u32 lin_rd32(u32 a) { return hv_emu_rd8(a) | hv_emu_rd8(a + 1) << 8 | hv_emu_rd8(a + 2) << 16 | (u32)hv_emu_rd8(a + 3) << 24; }
static void lin_wr32(u32 a, u32 v) { for (int i = 0; i < 4; i++) hv_emu_wr8(a + i, (u8)(v >> (8 * i))); }

/* The descriptor for selector sel (GDT, or the LDT now loaded): 0 if
   past the table's limit. *at: its linear address. */
static int desc_read(u32 sel, u32 *d0, u32 *d1, u32 *at)
{
    u32 base, lim;
    if (sel & 4) { base = (u32)V64(S_LDTR + 8); lim = V32(S_LDTR + 4); if (!(V16(S_LDTR + 2) & 0x80)) return 0; }
    else { base = (u32)V64(S_GDTR + 8); lim = V32(S_GDTR + 4); }
    if ((sel | 7) > lim) return 0;
    *at = base + (sel & ~7u);
    *d0 = lin_rd32(*at); *d1 = lin_rd32(*at + 4);
    return 1;
}

/* Shadow segment o from descriptor d0:d1 (attr in AMD-V's packed form). */
static void seg_from_desc(u32 o, u16 sel, u32 d0, u32 d1)
{
    u32 lim = (d0 & 0xFFFF) | (d1 & 0xF0000);
    if (d1 & (1u << 23)) lim = lim << 12 | 0xFFF;
    V16(o) = sel;
    V16(o + 2) = (u16)(((d1 >> 8) & 0xFF) | ((d1 >> 12) & 0xF00));
    V32(o + 4) = lim;
    V64(o + 8) = (d0 >> 16) | ((d1 & 0xFF) << 16) | (d1 & 0xFF000000u);
}

/* Segment register o loaded with sel, as a task switch does. */
static void seg_load(u32 o, u16 sel, int v86)
{
    if (v86) { V16(o) = sel; V16(o + 2) = 0xF3; V32(o + 4) = 0xFFFF; V64(o + 8) = (u32)sel << 4; return; }
    u32 d0, d1, at;
    if (!(sel & ~3u) || !desc_read(sel, &d0, &d1, &at)) { V16(o) = sel; V16(o + 2) = 0; V32(o + 4) = 0; V64(o + 8) = 0; return; }
    if ((d1 & (1u << 12)) && !(d1 & (1u << 8))) { d1 |= 1u << 8; lin_wr32(at + 4, d1); }   /* accessed */
    seg_from_desc(o, sel, d0, d1);
}

/* r: eax ebx ecx edx esi edi ebp. 0 done, -1 can't (left to stop the VM). */
static int task_switch(u32 *r)
{
    u32 q = vmread(EXIT_QUAL), src = (q >> 30) & 3;             /* 0 CALL, 1 IRET, 2 JMP, 3 IDT task gate */
    u16 nsel = (u16)q;
    u32 nd0, nd1, nat, od0, od1, oat;
    if (!desc_read(nsel, &nd0, &nd1, &nat)) return -1;
    u32 ntype = (nd1 >> 8) & 0xF;
    if (ntype != 9 && ntype != 0xB) return -1;                  /* a 32-bit TSS */
    u32 ob = (u32)V64(S_TR + 8);
    u16 osel = V16(S_TR);
    seg_from_desc(S_TR, nsel, nd0, nd1);                        /* the new TSS's base and limit, for now */
    u32 nb = (u32)V64(S_TR + 8);
    if (V32(S_TR + 4) < 0x67) return -1;

    /* The event through a task gate, and where the old task goes on. */
    u32 iv = (u32)V64(C_EXITINT), ev_type = (iv >> 8) & 7;
    int has_err = src == 3 && (iv & (1u << 31)) && (iv & (1u << 11));
    u32 err = (u32)(V64(C_EXITINT) >> 32);
    u32 eip = V32(S_RIP);
    if (src != 3 || ev_type == 4 || ev_type == 5 || ev_type == 6) eip += exit_len;   /* past the instruction */
    if (src == 3) V64(C_EXITINT) = 0;                           /* delivered by the switch: not again */

    /* Save the old task. */
    u32 fl = V32(S_RFLAGS);
    if (src == 1) fl &= ~(1u << 14);                            /* IRET: NT clear in the saved copy */
    lin_wr32(ob + 0x20, eip);
    lin_wr32(ob + 0x24, fl);
    lin_wr32(ob + 0x28, r[0]); lin_wr32(ob + 0x2C, r[2]); lin_wr32(ob + 0x30, r[3]); lin_wr32(ob + 0x34, r[1]);
    lin_wr32(ob + 0x38, V32(S_RSP)); lin_wr32(ob + 0x3C, r[6]); lin_wr32(ob + 0x40, r[4]); lin_wr32(ob + 0x44, r[5]);
    static const u32 segs[6] = { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS };
    for (int i = 0; i < 6; i++) lin_wr32(ob + 0x48 + 4 * i, V16(segs[i]));

    /* Busy bits and the back link. */
    if ((src == 1 || src == 2) && desc_read(osel, &od0, &od1, &oat)) lin_wr32(oat + 4, od1 & ~(1u << 9));
    if (src != 1) { nd1 |= 1u << 9; lin_wr32(nat + 4, nd1); }
    if (src == 0 || src == 3) lin_wr32(nb, osel);
    seg_from_desc(S_TR, nsel, nd0, nd1 | (1u << 9));

    /* Load the new one. */
    /* All of it read through the old page tables, then the new CR3. */
    u32 ncr3 = lin_rd32(nb + 0x1C), neip = lin_rd32(nb + 0x20), nfl = lin_rd32(nb + 0x24);
    r[0] = lin_rd32(nb + 0x28); r[2] = lin_rd32(nb + 0x2C); r[3] = lin_rd32(nb + 0x30); r[1] = lin_rd32(nb + 0x34);
    V64(S_RSP) = lin_rd32(nb + 0x38); r[6] = lin_rd32(nb + 0x3C); r[4] = lin_rd32(nb + 0x40); r[5] = lin_rd32(nb + 0x44);
    u16 sel[6];
    for (int i = 0; i < 6; i++) sel[i] = (u16)lin_rd32(nb + 0x48 + 4 * i);
    u16 ldt = (u16)lin_rd32(nb + 0x60);
    if (V32(S_CR0) & 0x80000000u) V64(S_CR3) = ncr3;
    if (src == 0 || src == 3) nfl |= 1u << 14;                  /* NT: nested */
    nfl = (nfl & 0x003F7FD7u) | 2;
    V64(S_RFLAGS) = nfl;
    V64(S_RIP) = neip;
    int v86 = (nfl >> 17) & 1;
    u32 ld0, ld1, lat;
    if ((ldt & ~3u) && !(ldt & 4) && desc_read(ldt & ~3u, &ld0, &ld1, &lat)) seg_from_desc(S_LDTR, ldt, ld0, ld1);
    else { V16(S_LDTR) = ldt; V16(S_LDTR + 2) = 0; V32(S_LDTR + 4) = 0; V64(S_LDTR + 8) = 0; }
    for (int i = 0; i < 6; i++) seg_load(segs[i], sel[i], v86);
    *(volatile u8 *)(sh + S_CPL) = v86 ? 3 : (u8)(sel[1] & 3);
    V64(S_CR0) = V32(S_CR0) | 8;                                /* TS */

    if (has_err) {                                              /* the exception's error code, on the new stack */
        u32 esp = V32(S_RSP) - 4;
        if (!(V16(S_SS + 2) & 0x400)) esp &= 0xFFFF;            /* 16-bit stack */
        lin_wr32((u32)V64(S_SS + 8) + esp, err);
        V64(S_RSP) = (V32(S_RSP) & (V16(S_SS + 2) & 0x400 ? 0 : 0xFFFF0000u)) | esp;
    }
    return 0;
}

/* ---------------- one run ---------------- */
void vmx_run(u8 *shadow, u32 *g)
{
    sh = shadow;
    put_guest();
    if (launched && V32(S_RIP) != exit_rip) vmwrite(G_INTR_STATE, 0);   /* an instruction was done for it */
    u64 ev = V64(C_EVENT);
    if ((u32)ev & (1u << 31)) {
        u32 info = (u32)ev, type = (info >> 8) & 7;
        /* Real mode (unrestricted guest, PE clear): exceptions push no
           error code, and VM entry refuses one (instruction error 7).
           hv.c reflects #GP and #PF with theirs, as protected mode wants. */
        if (!(V32(S_CR0) & 1)) info &= ~(1u << 11);
        vmwrite(ENTRY_INTR, info);
        if (info & (1u << 11)) vmwrite(ENTRY_EXC_ERR, (u32)(ev >> 32));
        if (type == 4 || type == 5 || type == 6) vmwrite(ENTRY_LEN, exit_len);
    } else vmwrite(ENTRY_INTR, 0);
    u32 pc = (V32(C_MISC1) & I1_VINTR) ? proc_ctl | 1u << 2 : proc_ctl & ~(1u << 2);   /* interrupt window */
    vmwrite(PROC_CTL, pc);
    if (*(volatile u8 *)(sh + C_TLB) || !launched) invept();

    u32 r[7] = { V32(S_RAX), g[0], g[1], g[2], g[3], g[4], g[5] };
    if (vmx_enter(r, launched)) {
        u32 e = vmread(INSN_ERR);
        V32(C_EXIT) = X_FAIL; V64(C_INFO1) = 0x10000 | e; V64(C_INFO2) = 0;
        V64(C_EXITINT) = 0;
        return;
    }
    launched = 1;
    get_guest();
    u32 reason = vmread(EXIT_REASON);
    exit_len = vmread(EXIT_LEN);
    V32(C_SHADOW) = (vmread(G_INTR_STATE) & 3) ? 1 : 0;      /* blocking by STI or MOV SS */
    exit_rip = V32(S_RIP);
    u32 iv = vmread(IDT_VEC);
    V64(C_EXITINT) = (iv & (1u << 31)) ? ((u64)vmread(IDT_VEC_ERR) << 32 | iv) : 0;
    V64(C_INFO1) = 0; V64(C_INFO2) = 0;
    u32 code = X_NOP;
    if (reason & (1u << 31)) {                                  /* VM entry failed on the guest state */
        code = X_FAIL;
        V64(C_INFO1) = 0x20000 | (reason & 0xFFFF);
        V64(C_INFO2) = vmread(EXIT_QUAL);
    } else switch (reason & 0xFFFF) {
    case 0: {                                                   /* exception (#GP) or NMI */
        u32 ei = vmread(EXIT_INTR);
        if (((ei >> 8) & 7) == 2) break;                        /* NMI: nothing to do here */
        code = X_EXC(ei & 0xFF);
        V64(C_INFO1) = vmread(EXIT_INTR_ERR);
        break; }
    case 1: code = X_INTR; break;
    case 2: code = X_SHUT; break;
    case 7: code = X_VINTR; break;
    case 9:                                                     /* task switch */
        if (task_switch(r)) { code = X_FAIL; V64(C_INFO1) = 0x30000 | 9; V64(C_INFO2) = vmread(EXIT_QUAL); }
        break;
    case 10: do_cpuid(r); break;
    case 12: code = X_HLT; break;
    case 13: case 14: case 55: V64(S_RIP) = V32(S_RIP) + exit_len; break;   /* INVD, INVLPG, XSETBV */
    case 18: case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27:
    case 50: case 53: code = X_SVMINS; break;                   /* VMX instructions: #UD */
    case 28: cr_access(r); break;
    case 30: {                                                  /* I/O */
        u32 q = vmread(EXIT_QUAL), ii = vmread(EXIT_INSN);
        u32 sz = (q & 7) + 1;
        u32 info = (q & 0xFFFF0000u) | ((q >> 3) & 1) | (((q >> 4) & 1) << 2) | (((q >> 5) & 1) << 3) |
                   (sz == 1 ? 0x10 : sz == 2 ? 0x20 : 0x40);
        if (q & 0x10) info |= (1u << (7 + ((ii >> 7) & 3))) | (((ii >> 15) & 7) << 10);
        code = X_IOIO;
        V64(C_INFO1) = info;
        V64(C_INFO2) = V32(S_RIP) + exit_len;
        break; }
    case 31: code = X_MSR; V64(C_INFO1) = 0; break;
    case 32: code = X_MSR; V64(C_INFO1) = 1; break;
    case 48: {                                                  /* EPT violation */
        u32 q = vmread(EXIT_QUAL);
        code = X_NPF;
        V64(C_INFO1) = ((q >> 3) & 1) | (q & 2);               /* present, write */
        V64(C_INFO2) = vmread(GPA);
        break; }
    default:
        code = X_FAIL;
        V64(C_INFO1) = 0x30000 | (reason & 0xFFFF);
        V64(C_INFO2) = vmread(EXIT_QUAL);
    }
    V32(C_EXIT) = code;
    V64(S_RAX) = r[0];
    g[0] = r[1]; g[1] = r[2]; g[2] = r[3]; g[3] = r[4]; g[4] = r[5]; g[5] = r[6];
}
