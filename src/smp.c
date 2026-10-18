/* A second CPU for the screen and the sound.

   Everything vmdos does besides running DOS happens in the timer interrupt,
   on the one CPU DOS has: the 60 Hz redraw of the framebuffer (video.c)
   and the Sound Blaster / OPL3 / MIDI render (sound.c) are the big parts,
   and while they run DOS stands still. With a second core, that core does
   both instead, in a loop that follows the timer ticks; the boot CPU keeps
   DOS, the devices, USB, the network and the disks. When VMHV starts, the
   helper is parked for good (smp_park): VMHV runs on the boot CPU alone,
   as without one (DOOM crashed in VMHV with the helper running).

   The helper is found in the ACPI MADT (the RSDP from vmdos.efi, rsdp= on
   the command line, or the BIOS areas) and started with INIT-SIPI-SIPI
   through a page below 1 MiB (its contents put back afterwards). It never
   takes interrupts. A fault on it stops it, and the boot CPU takes the
   work back. nosmp: don't.

   What both CPUs touch is locked: video_lock (the video state against mode
   changes, register writes, INT 10h), sound_lock (the Sound Blaster core and
   the CD audio ring) and the log. The helper only ever tries video_lock and
   sound_lock (a frame or a render held up is made up on a later tick), so
   it can't hold up the boot CPU for more than one redraw or render. */
#include "kernel.h"

#define AP_STACK (64u << 10)
static u8 ap_stack[AP_STACK] __attribute__((aligned(16)));
u32 ap_cr0, ap_cr3, ap_cr4, ap_esp;            /* boot.S's ap_entry32 loads these */
extern u8 ap_tramp_start[], ap_tramp_end[], ap_tramp_gdtr[];
extern u8 ap_tramp_jump[];
extern u64 gdt[];
void ap_entry32(void);
void idt_load(void);
void *map_firmware(u32 phys, u32 len);
u32 ap_tables(void);
void ap_tables_sync(void);

volatile int smp_active;                       /* the helper does the screen and the sound */
static volatile int ap_alive;
static volatile int ap_busy, ap_park;         /* in a round of work; told to stop for good */
static int has_mwait, has_pat;
static u64 ap_pat;                            /* PAT: the boot CPU's (write-combining framebuffer) */

struct rlock video_lock, sound_lock, log_lock;

int smp_on_ap(void)
{
    u32 sp;
    __asm__ volatile("mov %%esp,%0" : "=r"(sp));
    return sp - (u32)(uintptr_t)ap_stack < AP_STACK;
}

/* ---------------- locks ----------------
   Spinlocks that the CPU holding one may take again; interrupts stay off
   while one is held (the timer handler takes them too). */
static inline u32 irq_save(void)
{
    u32 fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    return fl;
}
static inline void irq_restore(u32 fl) { if (fl & EFL_IF) sti(); }

static int lock_mine(struct rlock *l, int me)
{
    if (l->owner != me) return 0;                /* only this CPU ever sets it to me */
    l->depth++;
    return 1;
}

static void lock_got(struct rlock *l, int me, u32 fl) { l->owner = me; l->depth = 1; l->fl = fl; }

void rlock_take(struct rlock *l)
{
    u32 fl = irq_save();
    int me = smp_on_ap() + 1;
    if (lock_mine(l, me)) return;
    while (__atomic_exchange_n(&l->held, 1, __ATOMIC_ACQUIRE))
        while (l->held) __asm__ volatile("pause");
    lock_got(l, me, fl);
}

int rlock_try(struct rlock *l)
{
    u32 fl = irq_save();
    int me = smp_on_ap() + 1;
    if (lock_mine(l, me)) return 1;
    if (__atomic_exchange_n(&l->held, 1, __ATOMIC_ACQUIRE)) { irq_restore(fl); return 0; }
    lock_got(l, me, fl);
    return 1;
}

void rlock_drop(struct rlock *l)
{
    if (--l->depth) return;
    u32 fl = l->fl;
    l->owner = 0;
    __atomic_store_n(&l->held, 0, __ATOMIC_RELEASE);
    irq_restore(fl);
}

/* The helper faulted, holding what it held: let go, and the boot CPU
   carries on alone. */
static void force_drop(struct rlock *l)
{
    if (l->owner != 2) return;
    l->depth = 0; l->owner = 0;
    __atomic_store_n(&l->held, 0, __ATOMIC_RELEASE);
}

void smp_ap_fault(struct regs *r)
{
    smp_active = 0;
    ap_alive = 0;
    ap_busy = 0;
    force_drop(&video_lock);
    force_drop(&sound_lock);
    force_drop(&log_lock);
    kprintf("smp: the helper CPU faulted (exception %u, error %x, EIP %x): screen and sound back on CPU 0\n",
            r->vec, r->err, r->eip);
    for (;;) __asm__ volatile("cli; hlt");
}

/* ---------------- the helper's loop ---------------- */
void ap_main(void)
{
    if (has_pat) __asm__ volatile("wbinvd; wrmsr; wbinvd" :: "a"((u32)ap_pat), "d"((u32)(ap_pat >> 32)), "c"(0x277) : "memory");
    idt_load();
    ap_alive = 1;

    u32 last = ticks, frame = 0;
    for (;;) {
        u32 t;
        while ((t = ticks) == last) {
            if (has_mwait) {
                __asm__ volatile("monitor" :: "a"(&ticks), "c"(0), "d"(0));
                if (ticks == last) __asm__ volatile("mwait" :: "a"(0), "c"(0));
            } else __asm__ volatile("pause");
        }
        frame += t - last;
        last = t;
        __atomic_store_n(&ap_busy, 1, __ATOMIC_SEQ_CST);                         /* then look: smp_park's other half */
        if (ap_park) { ap_busy = 0; for (;;) __asm__ volatile("cli; hlt"); }
        if (!smp_active) { ap_busy = 0; continue; }
        ap_tables_sync();                                                          /* the boot CPU's page table changes */
        __asm__ volatile("mov %%cr3,%%eax; mov %%eax,%%cr3" ::: "eax", "memory");
        sound_tick();
        if (frame >= TICK_HZ / 60 && rlock_try(&video_lock)) {
            frame = 0;
            video_refresh();
            rlock_drop(&video_lock);
            video_refresh_late();
        }
        __atomic_store_n(&ap_busy, 0, __ATOMIC_RELEASE);
    }
}

/* ---------------- local APIC ---------------- */
static volatile u32 *lapic;
static int x2apic;

static u64 rdmsr(u32 m) { u32 lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return (u64)hi << 32 | lo; }

static u32 apic_id(void) { return x2apic ? (u32)rdmsr(0x802) : lapic[0x20 / 4] >> 24; }

static void apic_ipi(u32 dest, u32 cmd)
{
    if (x2apic) { __asm__ volatile("wrmsr" :: "c"(0x830), "a"(cmd), "d"(dest) : "memory"); return; }
    lapic[0x310 / 4] = dest << 24;
    lapic[0x300 / 4] = cmd;
    for (int i = 0; i < 100000 && (lapic[0x300 / 4] & (1u << 12)); i++) inb(0x80);   /* delivery pending */
}

static void udelay(u32 us) { while (us--) inb(0x80); }   /* ~1 us each, interrupts off or on */

/* ---------------- ACPI: the RSDP, then the MADT's CPUs ---------------- */
static int sum_ok(const u8 *p, u32 n) { u8 s = 0; while (n--) s += *p++; return s == 0; }

static u32 rsdp_scan(u32 from, u32 len)
{
    const u8 *p = phys_low(from);
    for (u32 o = 0; o + 20 <= len; o += 16)
        if (!memcmp(p + o, "RSD PTR ", 8) && sum_ok(p + o, 20)) return from + o;
    return 0;
}

static u32 rsdp_find(void)
{
    const char *c = strstr(cmdline, "rsdp=");
    if (c) {
        u32 v = 0;
        c += 5;
        if (c[0] == '0' && (c[1] | 32) == 'x') c += 2;
        for (;; c++) {
            int d = *c >= '0' && *c <= '9' ? *c - '0' : (*c | 32) >= 'a' && (*c | 32) <= 'f' ? (*c | 32) - 'a' + 10 : -1;
            if (d < 0) break;
            v = v << 4 | (u32)d;
        }
        return v;
    }
    u32 ebda = (u32)*(u16 *)phys_low(0x40E) << 4;
    u32 r = ebda >= 0x80000 && ebda < 0xA0000 ? rsdp_scan(ebda, 1024) : 0;
    return r ? r : rsdp_scan(0xE0000, 0x20000);
}

/* A whole ACPI table at phys (its length from its header), or 0. */
static const u8 *acpi_table(u32 phys)
{
    const u8 *h = map_firmware(phys, 36);
    if (!h) return 0;
    u32 len = *(const u32 *)(h + 4);
    if (len < 36 || len > (1u << 20)) return 0;
    const u8 *t = map_firmware(phys, len);
    return t && sum_ok(t, len) ? t : 0;
}

static const u8 *madt_find(void)
{
    u32 rp = rsdp_find();
    if (!rp) return 0;
    const u8 *r = rp + 36 <= GUEST_TOP ? phys_low(rp) : map_firmware(rp, 36);
    if (!r) return 0;
    int xsdt = r[15] >= 2 && *(const u64 *)(r + 24) && *(const u64 *)(r + 24) < 0x100000000ull;
    const u8 *sdt = acpi_table(xsdt ? (u32)*(const u64 *)(r + 24) : *(const u32 *)(r + 16));
    if (!sdt) return 0;
    u32 n = (*(const u32 *)(sdt + 4) - 36) / (xsdt ? 8 : 4);
    for (u32 i = 0; i < n; i++) {
        u64 a = xsdt ? *(const u64 *)(sdt + 36 + i * 8) : *(const u32 *)(sdt + 36 + i * 4);
        if (!a || a >= 0x100000000ull) continue;
        const u8 *h = map_firmware((u32)a, 36);
        if (h && !memcmp(h, "APIC", 4)) return acpi_table((u32)a);
    }
    return 0;
}

/* The APIC ID of an enabled CPU that isn't this one, or -1. */
static int64_t pick_cpu(const u8 *madt, u32 self, int *ncpu)
{
    u32 len = *(const u32 *)(madt + 4);
    int64_t pick = -1;
    *ncpu = 0;
    for (u32 o = 44; o + 2 <= len && madt[o + 1] >= 2; o += madt[o + 1]) {
        u32 id, flags;
        if (madt[o] == 0 && madt[o + 1] >= 8) { id = madt[o + 3]; flags = *(const u32 *)(madt + o + 4); }
        else if (madt[o] == 9 && madt[o + 1] >= 16) { id = *(const u32 *)(madt + o + 4); flags = *(const u32 *)(madt + o + 8); }
        else continue;
        if (!(flags & 1)) continue;
        ++*ncpu;
        if (id != self && pick < 0 && (x2apic || id < 255)) pick = id;
    }
    return pick;
}

/* ---------------- start-up ---------------- */
/* tramp: a free page below 1 MiB (physical), 0 if there's none. */
void smp_start(u32 tramp)
{
    if (strstr(cmdline, "nosmp")) { kprintf("smp: off (nosmp)\n"); return; }
    u32 a = 0, b, c, d;
    __asm__ volatile("pushfl; pop %0; mov %0,%1; xor $0x200000,%0; push %0; popfl; pushfl; pop %0; push %1; popfl"
                     : "=&r"(a), "=&r"(b));
    if (!((a ^ b) & 0x200000)) return;                              /* no CPUID: a 386 or early 486 */
    a = 1;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    if (!(d & (1u << 9)) || !(d & (1u << 5))) { kprintf("smp: no local APIC\n"); return; }
    has_mwait = (c >> 3) & 1;
    has_pat = (d >> 16) & 1;
    if (has_pat) ap_pat = rdmsr(0x277);
    u64 base = rdmsr(0x1B);
    if (!(base & (1u << 11))) { kprintf("smp: the local APIC is off\n"); return; }
    x2apic = (base >> 10) & 1;
    if (!x2apic) {
        u32 pa = (u32)base & 0xFFFFF000u;
        lapic = map_mmio64(pa, 4096);
        if (!lapic) { kprintf("smp: can't map the local APIC at %x\n", pa); return; }
    }
    const u8 *madt = madt_find();
    if (!madt) { kprintf("smp: no ACPI MADT: one CPU\n"); return; }
    u32 self = apic_id();
    int ncpu;
    int64_t id = pick_cpu(madt, self, &ncpu);
    if (id < 0) { kprintf("smp: one CPU\n"); return; }
    if (!tramp) { kprintf("smp: no free page below 1 MiB to start a CPU from\n"); return; }

    /* The trampoline: real mode at tramp, into protected mode on our GDT,
       then ap_entry32 (paging on with this CPU's CR0/3/4, the stack). */
    static u8 saved[4096];
    u8 *page = phys_low(tramp);
    u32 n = (u32)(ap_tramp_end - ap_tramp_start);
    memcpy(saved, page, 4096);
    memcpy(page, ap_tramp_start, n);
    struct __attribute__((packed)) { u16 lim; u32 base; } *gdtr = (void *)(page + (ap_tramp_gdtr - ap_tramp_start));
    gdtr->lim = 9 * 8 - 1;
    gdtr->base = (u32)(uintptr_t)gdt;
    *(u32 *)(page + (ap_tramp_jump - ap_tramp_start)) = (u32)(uintptr_t)ap_entry32;
    __asm__ volatile("mov %%cr0,%0" : "=r"(ap_cr0));
    __asm__ volatile("mov %%cr3,%0" : "=r"(ap_cr3));
    u32 own = ap_tables();                                           /* its own 0-2 MiB (VMHV): cpu.c */
    if (own) ap_cr3 = own;
    __asm__ volatile("mov %%cr4,%0" : "=r"(ap_cr4));
    ap_esp = (u32)(uintptr_t)ap_stack + AP_STACK;
    __asm__ volatile("wbinvd" ::: "memory");

    apic_ipi((u32)id, 0x0000C500);                                   /* INIT, assert */
    udelay(200);
    apic_ipi((u32)id, 0x00008500);                                   /* INIT, deassert */
    udelay(10000);
    for (int k = 0; k < 2 && !ap_alive; k++) {
        apic_ipi((u32)id, 0x00004600 | tramp >> 12);                 /* STARTUP */
        for (int i = 0; i < (k ? 100000 : 200) && !ap_alive; i++) udelay(1);
    }
    for (int i = 0; i < 100000 && !ap_alive; i++) udelay(1);
    memcpy(page, saved, 4096);
    if (!ap_alive) { kprintf("smp: CPU %u (APIC ID) didn't start: one CPU\n", (u32)id); return; }
    kprintf("smp: %d CPUs; CPU with APIC ID %u does the screen and the sound%s\n", ncpu, (u32)id,
            x2apic ? " (x2APIC)" : "");
}

/* VMHV (hv.c) is starting: the screen and the sound back on the boot CPU,
   once the helper has finished the round it's in, and the helper halted. */
void smp_park(void)
{
    if (!ap_alive) return;
    __atomic_store_n(&ap_park, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&smp_active, 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&ap_busy, __ATOMIC_SEQ_CST) && ap_alive) __asm__ volatile("pause");
    kprintf("smp: VMHV runs on one CPU; the helper is halted\n");
}

/* From kmain, last thing before DOS: the helper takes over. */
void smp_go(void) { if (ap_alive) smp_active = 1; }
