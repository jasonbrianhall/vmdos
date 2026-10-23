/* 86Box's threads, events, mutexes and timers for the Voodoo code, on a
   kernel without any: cooperative green threads on the boot CPU.

   The Voodoo has a FIFO thread (it runs the queued register writes and the
   command FIFO) and a render thread (triangles). Each gets its own stack
   and runs until it waits: on an event nobody has set, a mutex someone
   holds, or plat_delay_ms (waiting for a vertical retrace to swap). The
   "main thread" is whoever called into the card: a guest access trapped
   by the kernel, or the timer tick. v3_run lets the green threads run
   until none can go on; a main-thread wait runs them until what it waits
   for has happened. Nothing is ever preempted, so 86Box's locking works
   as written and needs no atomics.

   Timers (the FIFO thread's wake-up delay) count PIT clocks; due ones fire
   in v3_poll, from the tick and from every access to the card. */
#include <voodoo_shim.h>
#include <86box/timer.h>

typedef uint32_t u32;
typedef uint8_t u8;
u32 pit_clock(void);
void *phys_alloc(u32 bytes);

#define STACK_SIZE (64 * 1024)
#define MAX_THREADS 6

struct v3_thread {
    u32 esp;                                  /* saved while switched out */
    void (*fn)(void *);
    void *arg;
    event_t *wait_ev;                         /* waiting for it to be set */
    mutex_t *wait_mx;                         /* waiting for it to be free */
    int sleeping;                             /* plat_delay_ms: until the next v3_run */
    int done;
};

static struct v3_thread threads[MAX_THREADS];
static int nthreads;
static struct v3_thread *cur;                 /* 0: the main thread */
static u32 main_esp;
static int in_run;

/* Save callee-saved registers on this stack, store ESP in *save, switch to
   ESP to (which was saved the same way, or made by thread_create). */
void v3_switch(u32 *save, u32 to);
__asm__(".text\n.globl v3_switch\nv3_switch:\n"
        "  mov 4(%esp),%eax\n  mov 8(%esp),%edx\n"
        "  push %ebp\n  push %ebx\n  push %esi\n  push %edi\n"
        "  mov %esp,(%eax)\n  mov %edx,%esp\n"
        "  pop %edi\n  pop %esi\n  pop %ebx\n  pop %ebp\n  ret\n");

static void thread_start(void)
{
    cur->fn(cur->arg);
    cur->done = 1;
    for (;;) {                                 /* back to the scheduler for good */
        struct v3_thread *t = cur;
        cur = 0;
        v3_switch(&t->esp, main_esp);
    }
}

thread_t *thread_create(void (*fn)(void *), void *arg)
{
    if (nthreads >= MAX_THREADS) panic("voodoo: too many threads");
    struct v3_thread *t = &threads[nthreads++];
    u8 *stack = phys_alloc(STACK_SIZE);
    u32 *sp = (u32 *)(stack + STACK_SIZE);
    *--sp = 0;                                 /* thread_start's return address: never used */
    *--sp = (u32)(uintptr_t)thread_start;      /* v3_switch's ret */
    *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;   /* ebp ebx esi edi */
    t->esp = (u32)(uintptr_t)sp;
    t->fn = fn; t->arg = arg;
    return t;
}

int thread_wait(thread_t *t) { (void)t; return 0; }

static int runnable(struct v3_thread *t)
{
    if (t->done || t->sleeping) return 0;
    if (t->wait_ev) return t->wait_ev->set;
    if (t->wait_mx) return !t->wait_mx->locked;
    return 1;
}

/* A green thread gives the CPU back to v3_run. */
static void yield(void)
{
    struct v3_thread *t = cur;
    cur = 0;
    v3_switch(&t->esp, main_esp);
}

/* Main thread: run the green threads until none can go on (or, with
   until, until it says so). */
static void run_until(int (*until)(void *), void *p)
{
    if (cur || in_run) return;
    in_run = 1;
    for (int i = 0; i < nthreads; i++) threads[i].sleeping = 0;
    for (int pass = 0; pass < 1000000; pass++) {
        int any = 0;
        for (int i = 0; i < nthreads; i++) {
            struct v3_thread *t = &threads[i];
            if (!runnable(t)) continue;
            t->wait_ev = 0; t->wait_mx = 0;
            any = 1;
            cur = t;
            v3_switch(&main_esp, t->esp);
            if (until && until(p)) { in_run = 0; return; }
        }
        if (!any) break;
    }
    in_run = 0;
}
void v3_run(void) { run_until(0, 0); }

/* ---------------- events ---------------- */
event_t *thread_create_event(void) { return calloc(1, sizeof(event_t)); }
void thread_destroy_event(event_t *e) { (void)e; }
void thread_set_event(event_t *e) { e->set = 1; }
void thread_reset_event(event_t *e) { e->set = 0; }

static int ev_set(void *p) { return ((event_t *)p)->set; }
int thread_wait_event(event_t *e, int timeout)
{
    (void)timeout;
    if (e->set) return 0;
    if (cur) {
        cur->wait_ev = e;
        yield();
        return e->set ? 0 : 1;
    }
    run_until(ev_set, e);                      /* the main thread: let the others get it done */
    return e->set ? 0 : 1;
}

/* ---------------- mutexes ---------------- */
mutex_t *thread_create_mutex(void) { return calloc(1, sizeof(mutex_t)); }
void thread_close_mutex(mutex_t *m) { (void)m; }
static int mx_free(void *p) { return !((mutex_t *)p)->locked; }
int thread_wait_mutex(mutex_t *m)
{
    while (m->locked) {
        if (cur) { cur->wait_mx = m; yield(); continue; }
        run_until(mx_free, m);
        if (m->locked) {                       /* its holder can't go on: a deadlock, don't hang the PC */
            kprintf("voodoo: main thread can't get a mutex, taking it\n");
            break;
        }
    }
    m->locked = 1;
    return 1;
}
int thread_release_mutex(mutex_t *m) { m->locked = 0; return 1; }

void plat_delay_ms(uint32_t ms)
{
    (void)ms;
    if (cur) { cur->sleeping = 1; yield(); }
}

/* ---------------- time ----------------
   86Box's timers (timer.c, as upstream) count "CPU cycles" in tsc: here
   PIT clocks (cpuclock is PIT_HZ), moved on to now whenever the card is
   looked at. A long wait (the boot CPU busy elsewhere) is caught up at most
   50 ms at a time: the card's time falls behind rather than rendering a
   second of scan lines in one go. */
uint64_t tsc;
static u32 last_pit;

uint64_t plat_timer_read(void) { return tsc; }

void v3_poll_timers(void)
{
    u32 now = pit_clock(), d = now - last_pit;
    last_pit = now;
    if (d > 59659) d = 59659;                  /* 50 ms */
    tsc += d;
    if (TIMER_VAL_LESS_THAN_VAL(timer_target, tsc)) timer_process();
}

void v3_sched_init(void)
{
    last_pit = pit_clock();
    timer_init();
}
