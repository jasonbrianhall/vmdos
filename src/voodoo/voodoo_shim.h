/* What 86Box's Voodoo sources (vid_voodoo*.c, kept as they are upstream)
   need from 86Box and from a C library, on top of vmdos's kernel: the 86Box
   headers they include and <string.h> <stdlib.h> <math.h>... all come here
   (one-line files in voodoo/86box and voodoo/libc).

   Threads, events and mutexes are cooperative green threads (v3sched.c),
   run by the boot CPU when the card is touched or its timers are due;
   timers count PIT clocks. Only the parts the Voodoo 3 uses do anything:
   the Voodoo 1/2 device glue (PCI, mappings, ROMs) compiles but isn't
   reached. */
#ifndef VMDOS_VOODOO_SHIM_H
#define VMDOS_VOODOO_SHIM_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

/* ---------------- C library ---------------- */
typedef int wchar_t_;                       /* <wchar.h>: nothing used */
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRId64 "lld"
#define NULL_ ((void *)0)

void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strncmp(const char *a, const char *b, size_t n);
char *strstr(const char *h, const char *n);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
void kprintf(const char *fmt, ...);
__attribute__((noreturn)) void panic(const char *fmt, ...);

void *v3_calloc(size_t n, size_t sz);       /* v3.c: from the kernel heap, at start-up */
void v3_free(void *p);
#define calloc(n, s) v3_calloc((n), (s))
#define malloc(s)    v3_calloc(1, (s))
#define free(p)      v3_free(p)
static inline int abs_(int x) { return x < 0 ? -x : x; }
#define abs(x) abs_(x)
#define assert(x) ((void)0)
static inline char *getenv(const char *n) { (void)n; return 0; }
static inline int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}
static inline long strtol(const char *s, char **e, int b) { (void)b; long v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0'; if (e) *e = (char *)s; return v; }
static inline int atoi(const char *s) { return (int)strtol(s, 0, 10); }

/* <math.h>: the x87 (the Voodoo files are built with it, see the Makefile) */
static inline double sqrt(double x) { __asm__("fsqrt" : "+t"(x)); return x; }
static inline double log2(double x) { double r; __asm__("fld1; fxch; fyl2x" : "=t"(r) : "0"(x)); return r; }
static inline double fabs(double x) { __asm__("fabs" : "+t"(x)); return x; }
static inline float fabsf(float x) { return (float)fabs(x); }
static inline double floor(double x) { long long i = (long long)x; return (double)(i - (x < (double)i)); }
double round(double x);                    /* v3.c */
static inline double ceil(double x) { long long i = (long long)x; return (double)(i + (x > (double)i)); }
static inline double pow(double b, double e)
{
    double r;                                    /* 2^(e*log2 b), b > 0 */
    __asm__("fyl2x\n\t"
            "fld %%st(0)\n\tfrndint\n\tfsub %%st, %%st(1)\n\tfxch\n\tf2xm1\n\tfld1\n\tfaddp\n\tfscale\n\tfstp %%st(1)"
            : "=t"(r) : "0"(b), "u"(e) : "st(1)");
    return r;
}

/* ---------------- 86Box basics ---------------- */
#define fallthrough __attribute__((fallthrough))
#define ABS(x) ((x) < 0 ? -(x) : (x))
#define UNUSED(x) __attribute__((unused)) x
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define ATOMIC_INT volatile int
#define ATOMIC_UINT volatile uint32_t
#define ATOMIC_DOUBLE volatile double
#define ATOMIC_LOAD(var) (var)
#define ATOMIC_STORE(var, val) ((var) = (val))
#define ATOMIC_INC(var) (++(var))
#define ATOMIC_DEC(var) (--(var))
#define ATOMIC_ADD(var, val) ((var) += (val))
#define ATOMIC_SUB(var, val) ((var) -= (val))
#define pclog(...) do { if (v3_log_on) kprintf(__VA_ARGS__); } while (0)
static inline void pclog_ex(const char *fmt, va_list ap) { char b[256]; vsnprintf(b, sizeof b, fmt, ap); kprintf("%s", b); }
#define fatal(...) v3_fatal(__VA_ARGS__)
void v3_fatal(const char *fmt, ...);
extern int v3_log_on;
static inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint16_t bswap16(uint16_t v) { return (uint16_t)(v >> 8 | v << 8); }

extern int cycles;                          /* bus-time accounting: ignored */
extern int pci_burst_time, pci_nonburst_time, agp_burst_time, agp_nonburst_time;
extern uint32_t mem_size;
extern double cpuclock;                     /* the timers' "CPU cycles": PIT clocks here */
typedef struct { uint32_t pc; } cpu_state_t;
extern cpu_state_t cpu_state;
int device_get_config_int(const char *name);

typedef struct v3_file FILE;                /* rom.h names it */

/* ---------------- devices, memory, I/O, timers, video ----------------
   86Box's own headers (86box/device.h, mem.h, io.h, pci.h, rom.h, timer.h,
   video.h, vid_svga.h ...); v3glue.c does what their functions do here. */
#include <86box/device.h>
#include <86box/mem.h>
#include <86box/io.h>
#include <86box/pci.h>
#include <86box/rom.h>
#include <86box/timer.h>
#include <86box/video.h>
#include <86box/i2c.h>
#include <86box/vid_ddc.h>
uint64_t plat_timer_read(void);
void plat_delay_ms(uint32_t ms);
static inline uint64_t plat_get_ticks(void) { return 0; }

/* ---------------- threads (v3sched.c) ---------------- */
typedef struct v3_thread thread_t;
typedef struct v3_event { volatile int set; } event_t;
typedef struct v3_mutex { volatile int locked; } mutex_t;
thread_t *thread_create(void (*fn)(void *), void *arg);
event_t *thread_create_event(void);
void thread_set_event(event_t *e);
void thread_reset_event(event_t *e);
int thread_wait_event(event_t *e, int timeout);
void thread_destroy_event(event_t *e);
mutex_t *thread_create_mutex(void);
int thread_wait_mutex(mutex_t *m);
int thread_release_mutex(mutex_t *m);
void thread_close_mutex(mutex_t *m);
int thread_wait(thread_t *t);

#endif
