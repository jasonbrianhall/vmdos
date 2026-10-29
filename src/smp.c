/* A second CPU for the screen and the sound.

   Everything vmdos does besides running DOS happens in the timer interrupt,
   on the one CPU DOS has: the 60 Hz redraw of the framebuffer (video.c)
   and the Sound Blaster / OPL3 / MIDI render (sound.c) are the big parts,
   and while they run DOS stands still. With a second core, that core does
   both instead, in a loop that follows the timer ticks; the boot CPU keeps
   DOS, the devices, USB, the network and the disks. When VMHV starts, the
   helper is parked for good (smp_park): VMHV runs on the boot CPU alone,
   as without one (DOOM crashed in VMHV with the helper running).

   Between ticks the helper halts, and the boot CPU's timer tick wakes it
   with an IPI (vector 30h, smp_tick) when it is halted: MONITOR/MWAIT on
   the tick counter is often missing (VMs hide it) and a PAUSE loop keeps
   the core at 100% even when there is nothing to draw. Without a local
   APIC it can take that on, it waits as before.

   With more CPUs, up to three more start as render helpers (render=N:
   at most N; render=0: none). They take no part but this: when the
   helper's redraw has rows to scale and write out (par_for), they take
   chunks of them alongside it, so a frame that one CPU can't draw in
   time at 1080p or with xBRZ is split. Between those they halt, woken by
   the helper's IPI; a still screen never wakes them.

   The helper is found in the ACPI MADT (the RSDP from vmdos.efi, rsdp= on
   the command line, or the BIOS areas) and started with INIT-SIPI-SIPI
   through a page below 1 MiB (its contents put back afterwards). It takes
   no interrupts but its wake-up IPI. A fault on it stops it, and the boot CPU takes the
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
#define AP_WAKE_VEC 0x30                       /* cpu.c's IDT: ap_wake_isr */
#define AP_SPURIOUS_VEC 0x3F                   /* low four bits set: older APICs force them */
static u32 ap_id;                              /* the helper's APIC ID */
static int ap_x2apic;
static volatile int ap_apic_ready, ap_hlt_ok;  /* its APIC is set for the wake-up IPI; one has come */
static volatile int ap_probe_done;
static volatile int ap_sleeping;               /* halted till the next tick's IPI */
static int ap_wake_setup(void);
static void ap_wait(u32 last);
static void udelay(u32 us);
static u64 ap_pat;                            /* PAT: the boot CPU's (write-combining framebuffer) */

/* Render helpers (par_for) */
#define MAX_RENDER (SMP_SLOTS - 1)
static u8 rh_stack[MAX_RENDER][AP_STACK] __attribute__((aligned(16)));
static u32 rh_apic[MAX_RENDER];
static int n_render;                           /* started; rh_ok says which take wake-ups */
static volatile int ap_role;                   /* the CPU being started: 0 the helper, 1 + i render helper i */
static volatile int rh_alive[MAX_RENDER], rh_ok[MAX_RENDER], rh_probed[MAX_RENDER];
static volatile int rh_sleeping[MAX_RENDER];
static volatile u32 cpu_wakes[2 + MAX_RENDER];  /* wake-up IPIs taken, by smp_cpu() */
static volatile int rh_fault;                  /* one faulted: par_for draws alone from then on */
static volatile u32 rh_fault_vec, rh_fault_eip;
static void render_main(int i);

struct rlock video_lock, sound_lock, log_lock;

/* Which CPU this is: 0 the boot CPU, 1 the helper, 2 + i render helper i. */
int smp_cpu(void)
{
    u32 sp;
    __asm__ volatile("mov %%esp,%0" : "=r"(sp));
    if (sp - (u32)(uintptr_t)ap_stack < AP_STACK) return 1;
    u32 o = sp - (u32)(uintptr_t)rh_stack;
    return o < sizeof rh_stack ? 2 + (int)(o / AP_STACK) : 0;
}

int smp_on_ap(void) { return smp_cpu() != 0; }

/* Per-CPU scratch for par_for's work: 0 on the boot CPU and the helper
   (they never draw at the same time), 1 + i on render helper i. */
int smp_slot(void) { int c = smp_cpu(); return c >= 2 ? c - 1 : 0; }

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
    int me = smp_cpu() + 1;
    if (lock_mine(l, me)) return;
    while (__atomic_exchange_n(&l->held, 1, __ATOMIC_ACQUIRE))
        while (l->held) __asm__ volatile("pause");
    lock_got(l, me, fl);
}

int rlock_try(struct rlock *l)
{
    u32 fl = irq_save();
    int me = smp_cpu() + 1;
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
    int c = smp_cpu();
    if (c >= 2) {                              /* a render helper: it takes no locks; the helper reports it */
        rh_ok[c - 2] = 0;
        rh_fault_vec = r->vec; rh_fault_eip = r->eip;
        __atomic_store_n(&rh_fault, 1, __ATOMIC_SEQ_CST);
        for (;;) __asm__ volatile("cli; hlt");
    }
    ap_hlt_ok = 0;
    ap_sleeping = 0;
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
    int role = ap_role;
    if (has_pat) __asm__ volatile("wbinvd; wrmsr; wbinvd" :: "a"((u32)ap_pat), "d"((u32)(ap_pat >> 32)), "c"(0x277) : "memory");
    idt_load();
    if (role) render_main(role - 1);
    ap_apic_ready = ap_wake_setup();
    ap_alive = 1;
    if (ap_apic_ready) {                       /* the boot CPU's test IPI (smp_start): HLT only once one got here */
        __asm__ volatile("sti" ::: "memory");
        for (int i = 0; i < 50000 && !cpu_wakes[1]; i++) udelay(1);
        __asm__ volatile("cli" ::: "memory");
        ap_hlt_ok = cpu_wakes[1] != 0;
    }
    ap_probe_done = 1;

    u32 last = ticks, frame = 0;
    for (;;) {
        u32 t;
        while ((t = ticks) == last) ap_wait(last);
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
    for (int i = 0; i < 100000 && (lapic[0x300 / 4] & (1u << 12)); i++) kdelay_us(1);   /* delivery pending */
}

static void wrmsr(u32 m, u64 v) { __asm__ volatile("wrmsr" :: "c"(m), "a"((u32)v), "d"((u32)(v >> 32)) : "memory"); }
static u32 lapic_pa;                          /* where the boot CPU's xAPIC registers are */
static int apic_ok;                            /* lapic mapped (or x2APIC): the boot CPU's is usable */

/* ---------------- the boot CPU's APIC timer: a fast guest PIT ----------------
   The host tick is 1 kHz, so the guest's IRQ 0 came at most 1000 times a
   second whatever DOS set the PIT to. Programs that play samples from the
   timer (Creative's Parrot / VOICE.DRV: DSP 10h at ~8 kHz; PC speaker and
   Covox players) ran slow and low. When DOS sets channel 0 faster than the
   tick, vdev.c asks for an interrupt at its next due time from the local
   APIC timer (one-shot, divide by 1), calibrated once against the PIT.
   No APIC, or one software-disabled: as before. nofastpit: off. */
static int ft_state;                           /* 0 untried, 1 usable, 2 being measured, -1 not */
static u32 ft_per_k;                           /* APIC timer counts per 1024 PIT clocks */
static u32 ft_p0, ft_c0;                       /* the measurement's start: PIT clock, APIC count */
static u64 ft_t0;                              /* and TSC */

static u32 apic_rd(u32 off) { return x2apic ? (u32)rdmsr(0x800 + (off >> 4)) : lapic[off / 4]; }
static void apic_wr(u32 off, u32 v) { if (x2apic) wrmsr(0x800 + (off >> 4), v); else lapic[off / 4] = v; }

/* The APIC timer's rate, measured over 20 ms of host ticks from the first
   one (no busy wait: pit_clock wants interrupts on). */
static int fast_timer_measure(void);
static int fast_timer_start(void)
{
    if (!apic_ok || strstr(cmdline, "nofastpit")) return 0;
    u32 svr = apic_rd(0xF0);
    if (!(svr & 0x100)) { kprintf("fast PIT: the local APIC is software-disabled\n"); return 0; }
    apic_wr(0xF0, (svr & ~0xFFu) | AP_SPURIOUS_VEC);   /* a spurious one: within our IDT, just IRET */
    apic_wr(0x320, 0x10000 | FAST_TIMER_VEC);  /* masked, one-shot */
    apic_wr(0x3E0, 0xB);                       /* divide by 1 */
    apic_wr(0x380, 0xFFFFFFFFu);
    ft_t0 = tsc_read();
    ft_p0 = pit_clock();
    ft_c0 = apic_rd(0x390);
    return 1;
}

/* Each host tick (vdev.c), before the guest's timer is looked at. */
void fast_timer_tick(void)
{
    if (ft_state == 2) fast_timer_measure();
    else if (!ft_state && apic_ok) ft_state = fast_timer_start() ? 2 : -1;   /* at boot: ready before DOS wants it */
}

static int fast_timer_measure(void)
{
    u32 p1 = pit_clock(), c1 = apic_rd(0x390);
    if (p1 - ft_p0 < 20 * PIT_PER_TICK) return 0;
    u64 dt = tsc_read() - ft_t0;
    apic_wr(0x380, 0);
    u64 r = (u64)(ft_c0 - c1) * 1024 / (p1 - ft_p0);
    /* Host ticks lost while it was measured (interrupts off a long time:
       USB set-up at boot) make the PIT clock fall behind and the rate come
       out too high (1.1 GHz seen, for a 100 MHz bus clock): the fast PIT's
       interrupts then came 10 times late. The TSC, timed against PIT
       channel 2 without interrupts (lib.c), doesn't lose them: it times
       the measurement when there is one. */
    u32 khz = tsc_khz();
    if (khz && dt) {
        u64 akhz = (u64)(ft_c0 - c1) * khz / dt;          /* APIC timer kHz */
        u64 rt = akhz * 1024 * 1000 / PIT_HZ;
        u64 diff = rt > r ? rt - r : r - rt;
        if (diff * 50 > rt)
            kprintf("fast PIT: host ticks lost while measuring (%u kHz by the tick count): timed by the TSC\n",
                    (u32)(r * (PIT_HZ / 1000) >> 10));
        r = rt;
    }
    if (c1 == 0 || r < 1024 || r > 0xFFFFFFFFull) {    /* ran out, too slow, absurd */
        kprintf("fast PIT: APIC timer unusable (%u counts in %u PIT clocks)\n", ft_c0 - c1, p1 - ft_p0);
        ft_state = -1;
        return 0;
    }
    ft_per_k = (u32)r;
    ft_state = 1;
    kprintf("fast PIT: APIC timer at %u kHz\n", (u32)(r * (PIT_HZ / 1000) >> 10));
    return 1;
}

void fast_timer_arm(u32 pit_clocks)
{
    if (smp_on_ap()) return;
    if (!pit_clocks) {
        if (ft_state == 1) apic_wr(0x380, 0);  /* stops a one-shot count */
        return;
    }
    if (!ft_state) { ft_state = fast_timer_start() ? 2 : -1; return; }
    if (ft_state != 1) return;
    u32 n = (u32)((u64)pit_clocks * ft_per_k >> 10);
    apic_wr(0x320, FAST_TIMER_VEC);            /* one-shot, unmasked */
    apic_wr(0x380, n ? n : 1);
}

void fast_timer_eoi(void) { if (ft_state == 1) apic_wr(0xB0, 0); }
int fast_timer_ok(void) { return ft_state == 1; }

/* On the helper: software-enable its local APIC (INIT leaves it off, and
   off it takes no fixed IPIs), let every priority through, and have the
   wake-up IPI's vector. Its LINT pins and timer stay masked from INIT. */
static int ap_wake_setup(void)
{
    u64 base = rdmsr(0x1B);
    if (!(base & (1u << 11))) return 0;        /* globally off */
    ap_x2apic = (base >> 10) & 1;
    if (ap_x2apic) {
        wrmsr(0x808, 0);                       /* TPR */
        wrmsr(0x80F, 0x100 | AP_SPURIOUS_VEC); /* SVR: on */
    } else {
        if (!lapic || ((u32)base & 0xFFFFF000u) != lapic_pa) return 0;   /* not where it's mapped */
        lapic[0x80 / 4] = 0;
        lapic[0xF0 / 4] = 0x100 | AP_SPURIOUS_VEC;
    }
    return 1;
}

/* boot.S's ap_wake_isr. */
void ap_wake_eoi(void)
{
    cpu_wakes[smp_cpu()]++;
    if (ap_x2apic) wrmsr(0x80B, 0);
    else lapic[0xB0 / 4] = 0;
}

/* On the helper: till ticks moves on from last. Halted, the boot CPU's
   tick wakes it: one of the two always sees the other's write (ap_sleeping
   here, ticks there, each fenced before the read), and an IPI that comes
   between the check and the HLT waits for the STI's one-instruction shadow
   to end, so it wakes the HLT rather than going before it. */
static void ap_wait(u32 last)
{
    if (ap_hlt_ok) {
        __atomic_exchange_n(&ap_sleeping, 1, __ATOMIC_SEQ_CST);         /* XCHG: a full fence */
        if (ticks == last) __asm__ volatile("sti; hlt; cli" ::: "memory");
        ap_sleeping = 0;
    } else if (has_mwait) {
        __asm__ volatile("monitor" :: "a"(&ticks), "c"(0), "d"(0));
        if (ticks == last) __asm__ volatile("mwait" :: "a"(0), "c"(0));
    } else __asm__ volatile("pause");
}

/* Boot CPU, its timer tick, after ticks++. */
void smp_tick(void)
{
    if (!ap_hlt_ok) return;
    __asm__ volatile("lock orl $0,(%%esp)" ::: "memory");             /* ticks++ out before ap_sleeping is read */
    if (ap_sleeping) apic_ipi(ap_id, 0x00004000 | AP_WAKE_VEC);       /* fixed, edge, assert */
}

/* ---------------- par_for: the helper's rows, shared ----------------
   The helper posts a job (fn over 0..n-1 in chunks), wakes the render
   helpers that are halted, and takes chunks itself; each CPU claims the
   next chunk with a locked XADD. A job's fields change only while no
   render helper is inside one (job_active) and claims are blocked
   (job_next past any chunk count), so a late one can't take a chunk of
   the wrong job. Everything runs on this CPU when there are no render
   helpers, on the boot CPU, or for a handful of rows. */
#define JOB_BLOCKED 0x80000000u
static struct { void (*fn)(u32, void *); void *arg; u32 n, chunk, nchunks; } job;
static volatile u32 job_gen, job_next = JOB_BLOCKED, job_done, job_active;

static u32 xadd(volatile u32 *p, u32 v)
{
    __asm__ volatile("lock xaddl %0,%1" : "+r"(v), "+m"(*p) :: "memory");   /* SMP: a 486 or later */
    return v;
}

static void run_chunks(void)
{
    for (;;) {
        u32 c = xadd(&job_next, 1);
        if (c >= job.nchunks) return;
        u32 a = c * job.chunk, b = a + job.chunk;
        if (b > job.n) b = job.n;
        for (u32 i = a; i < b; i++) job.fn(i, job.arg);
        xadd(&job_done, 1);
    }
}

/* 0: a render helper faulted part way (some rows may be undrawn). */
int par_for(u32 n, u32 chunk, void (*fn)(u32, void *), void *arg)
{
    if (!n_render || rh_fault || smp_cpu() != 1 || n < 2 * chunk) {
        for (u32 i = 0; i < n; i++) fn(i, arg);
        return 1;
    }
    while (job_active && !rh_fault) __asm__ volatile("pause");
    job_next = JOB_BLOCKED;
    job.fn = fn; job.arg = arg; job.n = n; job.chunk = chunk; job.nchunks = (n + chunk - 1) / chunk;
    job_done = 0;
    __asm__ volatile("" ::: "memory");
    job_next = 0;
    xadd(&job_gen, 1);                         /* locked: a fence before rh_sleeping is read */
    for (int i = 0; i < n_render; i++)
        if (rh_ok[i] && rh_sleeping[i]) apic_ipi(rh_apic[i], 0x00004000 | AP_WAKE_VEC);
    run_chunks();
    while (job_done < job.nchunks && !rh_fault) __asm__ volatile("pause");
    job_next = JOB_BLOCKED;
    while (job_active && !rh_fault) __asm__ volatile("pause");
    if (!rh_fault) return 1;
    static int told;
    if (!told) {
        told = 1;
        kprintf("smp: a render helper faulted (exception %u, EIP %x): the helper draws alone\n", rh_fault_vec, rh_fault_eip);
    }
    return 0;
}

/* A render helper: its wake-up test, then jobs. Between jobs it spins a
   little (a frame's jobs come close together) and then halts. */
static void render_main(int i)
{
    int ready = ap_wake_setup();
    rh_alive[i] = 1;
    if (ready) {
        __asm__ volatile("sti" ::: "memory");
        for (int k = 0; k < 50000 && !cpu_wakes[2 + i]; k++) udelay(1);
        __asm__ volatile("cli" ::: "memory");
    }
    rh_ok[i] = ready && cpu_wakes[2 + i] != 0;
    rh_probed[i] = 1;
    if (!rh_ok[i]) for (;;) __asm__ volatile("cli; hlt");      /* no wake-ups: it would have to poll */
    u32 seen = job_gen;
    for (;;) {
        for (int s = 0; s < 4000 && job_gen == seen; s++) __asm__ volatile("pause");
        while (job_gen == seen) {
            __atomic_exchange_n(&rh_sleeping[i], 1, __ATOMIC_SEQ_CST);  /* XCHG: fenced before job_gen is read */
            if (job_gen == seen) __asm__ volatile("sti; hlt; cli" ::: "memory");
            rh_sleeping[i] = 0;
        }
        seen = job_gen;
        xadd(&job_active, 1);
        if (job_gen == seen) {
            __asm__ volatile("mov %%cr3,%%eax; mov %%eax,%%cr3" ::: "eax", "memory");   /* the helper's page table changes */
            run_chunks();
        }
        xadd(&job_active, (u32)-1);
    }
}

static void udelay(u32 us) { kdelay_us(us); }   /* TSC-timed, interrupts off or on */

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
static int pick_cpus(const u8 *madt, u32 self, int *ncpu, u32 *ids, int max)
{
    u32 len = *(const u32 *)(madt + 4);
    int n = 0;
    *ncpu = 0;
    for (u32 o = 44; o + 2 <= len && madt[o + 1] >= 2; o += madt[o + 1]) {
        u32 id, flags;
        if (madt[o] == 0 && madt[o + 1] >= 8) { id = madt[o + 3]; flags = *(const u32 *)(madt + o + 4); }
        else if (madt[o] == 9 && madt[o + 1] >= 16) { id = *(const u32 *)(madt + o + 4); flags = *(const u32 *)(madt + o + 8); }
        else continue;
        if (!(flags & 1)) continue;
        ++*ncpu;
        if (id != self && n < max && (x2apic || id < 255)) ids[n++] = id;
    }
    return n;
}

/* INIT-SIPI-SIPI to CPU id through the trampoline at tramp; *alive turns 1
   once it runs. */
static int start_cpu(u32 id, u32 tramp, volatile int *alive)
{
    apic_ipi(id, 0x0000C500);                                        /* INIT, assert */
    udelay(200);
    apic_ipi(id, 0x00008500);                                        /* INIT, deassert */
    udelay(10000);
    for (int k = 0; k < 2 && !*alive; k++) {
        apic_ipi(id, 0x00004600 | tramp >> 12);                      /* STARTUP */
        for (int i = 0; i < (k ? 100000 : 200) && !*alive; i++) udelay(1);
    }
    for (int i = 0; i < 100000 && !*alive; i++) udelay(1);
    return *alive;
}

/* ---------------- start-up ---------------- */
/* tramp: a free page below 1 MiB (physical), 0 if there's none. */
void smp_start(u32 tramp)
{
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
        lapic_pa = pa;
        lapic = map_mmio64(pa, 4096);
        if (!lapic) { kprintf("smp: can't map the local APIC at %x\n", pa); return; }
    }
    apic_ok = 1;                                                    /* the fast timer may use it, nosmp or not */
    if (strstr(cmdline, "nosmp")) { kprintf("smp: off (nosmp)\n"); return; }
    const u8 *madt = madt_find();
    if (!madt) { kprintf("smp: no ACPI MADT: one CPU\n"); return; }
    u32 self = apic_id();
    int ncpu;
    u32 ids[1 + MAX_RENDER];
    int nids = pick_cpus(madt, self, &ncpu, ids, 1 + MAX_RENDER);
    if (!nids) { kprintf("smp: one CPU\n"); return; }
    if (!tramp) { kprintf("smp: no free page below 1 MiB to start a CPU from\n"); return; }
    u32 id = ids[0];
    ap_id = id;

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
    ap_role = 0;
    __asm__ volatile("wbinvd" ::: "memory");

    if (!start_cpu(id, tramp, &ap_alive)) {
        memcpy(page, saved, 4096);
        kprintf("smp: CPU %u (APIC ID) didn't start: one CPU\n", id);
        return;
    }
    if (ap_apic_ready) apic_ipi(id, 0x00004000 | AP_WAKE_VEC);       /* the test: does a wake-up get there? */
    for (int i = 0; i < 100000 && !ap_probe_done; i++) udelay(1);

    /* Render helpers: the rest, up to render=N. */
    int want = nids - 1;
    const char *rc = strstr(cmdline, "render=");
    if (rc && rc[7] >= '0' && rc[7] <= '9' && rc[7] - '0' < want) want = rc[7] - '0';
    int rendering = 0;
    for (int i = 0; i < want; i++) {
        ap_role = 1 + i;
        ap_esp = (u32)(uintptr_t)rh_stack[i] + AP_STACK;
        rh_apic[i] = ids[1 + i];
        __asm__ volatile("wbinvd" ::: "memory");
        if (!start_cpu(ids[1 + i], tramp, &rh_alive[i])) { kprintf("smp: CPU %u (APIC ID) didn't start\n", ids[1 + i]); break; }
        apic_ipi(ids[1 + i], 0x00004000 | AP_WAKE_VEC);
        for (int k = 0; k < 100000 && !rh_probed[i]; k++) udelay(1);
        n_render = i + 1;
        rendering += rh_ok[i] != 0;
    }
    memcpy(page, saved, 4096);
    static const char *const more[] = { "", ", 1 more draws with it", ", 2 more draw with it", ", 3 more draw with it" };
    kprintf("smp: %d CPUs; CPU with APIC ID %u does the screen and the sound%s%s%s\n", ncpu, id,
            x2apic ? " (x2APIC)" : "", ap_hlt_ok ? "" : has_mwait ? ", waits with MWAIT" : ", waits by polling",
            more[rendering < 4 ? rendering : 3]);
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
