/* Small C library: memory/string helpers, printf, serial log. */
#include "kernel.h"

int debug_level = 1;
static int serial_ok;

/* 4 bytes at a time (rep movsl / stosl): byte loops into the framebuffer of
   a real graphics card are one bus write per byte, which made drawing a
   1080p frame take about a second. */
void *memcpy(void *d, const void *s, size_t n)
{
    void *r = d;
    size_t w = n >> 2, b = n & 3;
    __asm__ volatile("cld; rep movsl" : "+D"(d), "+S"(s), "+c"(w) :: "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(b) :: "memory");
    return r;
}
void *memmove(void *d, const void *s, size_t n)
{
    u8 *dd = d; const u8 *ss = s;
    if (dd <= ss || dd >= ss + n) return memcpy(d, s, n);
    dd += n; ss += n;
    while (n--) *--dd = *--ss;
    return d;
}
void *memset(void *d, int c, size_t n)
{
    void *r = d;
    u32 v = (u8)c * 0x01010101u;
    size_t w = n >> 2, b = n & 3;
    __asm__ volatile("cld; rep stosl" : "+D"(d), "+c"(w) : "a"(v) : "memory");
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(b) : "a"(v) : "memory");
    return r;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const u8 *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) { if (*a != *b) return (u8)*a - (u8)*b; if (!*a) return 0; }
    return 0;
}
char *strstr(const char *h, const char *n)
{
    size_t l = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, l)) return (char *)h;
    return l ? 0 : (char *)h;
}

int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    size_t o = 0;
#define PUT(c) do { if (o + 1 < n) buf[o] = (c); o++; } while (0)
    for (; *fmt; fmt++) {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        int zero = 0, width = 0, left = 0;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { zero = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == 'l') fmt++;
        char tmp[16]; int len = 0; const char *s = tmp;
        u32 v; int neg = 0;
        switch (*fmt) {
        case 'd': { int x = va_arg(ap, int); if (x < 0) { neg = 1; v = -(u32)x; } else v = x; goto dec; }
        case 'u': v = va_arg(ap, u32);
        dec:      do { tmp[15 - len++] = '0' + v % 10; v /= 10; } while (v);
                  if (neg) tmp[15 - len++] = '-';
                  s = tmp + 16 - len; break;
        case 'p': PUT('0'); PUT('x'); zero = 1; width = 8; /* fall through */
        case 'x': case 'X': {
                  const char *dig = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
                  v = va_arg(ap, u32);
                  do { tmp[15 - len++] = dig[v & 15]; v >>= 4; } while (v);
                  s = tmp + 16 - len; break; }
        case 'c': tmp[0] = (char)va_arg(ap, int); len = 1; break;
        case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; len = strlen(s); break;
        case '%': tmp[0] = '%'; len = 1; break;
        default:  tmp[0] = '?'; len = 1; break;
        }
        if (!left) for (int i = len; i < width; i++) PUT(zero ? '0' : ' ');
        for (int i = 0; i < len; i++) PUT(s[i]);
        if (left) for (int i = len; i < width; i++) PUT(' ');
    }
    if (n) buf[o < n ? o : n - 1] = 0;
    return (int)o;
#undef PUT
}

int snprintf(char *buf, size_t n, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

#define COM1 0x3F8
/* serial=off: no log on COM1; serial=dos: nor that, and COM1/COM2 are
   DOS's (serial.c). Read from the loader's command line, before the
   first line of the log, so the port never sees it. */
static int serial_mode;                          /* 0: the log, 1: off, 2: DOS's */

/* Time stamps on the log ("[  12.345] "): seconds since the CPU was reset
   (the TSC), so the first line also shows how long the firmware and the
   loader took. The TSC is timed against PIT channel 2 for 10 ms in
   serial_init. logtime=off leaves them out. */
static u32 tsc_per_ms;                           /* 0: no TSC (or no PIT channel 2 to time it) */
static int log_times;                            /* time stamps on (logtime=off: off) */
static u32 tsc_per_us_q10;                       /* kdelay_us: TSC counts a microsecond, times 1024 */
static u32 p80_per_ms = 1000;                    /* kdelay_us without a TSC: port 80h/61h read pairs a ms */
static int at_bol = 1;

static u64 tsc_now(void)
{
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (u64)hi << 32 | lo;
}

static void tsc_calibrate(void)
{
    u32 a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(d & 0x10)) return;                     /* no TSC */
    u8 p61 = inb(0x61);
    outb(0x61, (p61 & ~0x02) | 0x01);            /* gate on, speaker off */
    outb(0x43, 0xB0);                            /* channel 2, mode 0, lo/hi */
    outb(0x42, 11932 & 0xFF); outb(0x42, 11932 >> 8);   /* 10 ms at 1.193182 MHz */
    u64 t0 = tsc_now();
    u32 n = 0;
    while (!(inb(0x61) & 0x20) && ++n < 50000000) ;
    u64 t1 = tsc_now();
    outb(0x61, p61);
    if (n >= 50000000) return;                   /* no PIT channel 2 (some new chipsets) */
    u32 dlo = (u32)(t1 - t0), dhi = (u32)((t1 - t0) >> 32);
    if (dhi) return;
    tsc_per_ms = dlo / 10;
    tsc_per_us_q10 = tsc_per_ms / 1000 * 1024 + tsc_per_ms % 1000 * 1024 / 1000;
}

/* Without a TSC (a 386 or 486): time the same read pair kdelay_us loops on
   against 10 ms of PIT channel 2. A port 80h read is ~1 us on old boards
   but can be 15 us on new ones, so it's measured, never assumed. */
static void p80_calibrate(void)
{
    u8 p61 = inb(0x61);
    outb(0x61, (p61 & ~0x02) | 0x01);
    outb(0x43, 0xB0);
    outb(0x42, 11932 & 0xFF); outb(0x42, 11932 >> 8);
    u32 n = 0;
    while (!(inb(0x61) & 0x20) && ++n < 50000000) inb(0x80);
    outb(0x61, p61);
    if (n < 50000000 && n >= 10) p80_per_ms = n / 10;
}

u64 tsc_read(void) { return tsc_now(); }
u32 tsc_khz(void) { return tsc_per_ms; }

/* perf: what one access to each legacy port costs on this PC. A port that
   takes ~15 us instead of ~1 is usually trapped to SMM by the firmware. */
static void io_probe(void)
{
    if (!tsc_per_ms) return;
    static const struct { u16 port; const char *what; } p[] = {
        { 0x80, "80h (POST)" }, { 0x61, "61h (PIT gate)" }, { 0x21, "21h (PIC mask)" },
        { 0x40, "40h (PIT)" }, { 0x64, "64h (keyboard status)" }, { 0x3F8 + 5, "3FDh (COM1)" } };
    char b[256]; int n = 0;
    u32 fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl));
    for (unsigned i = 0; i < sizeof p / sizeof *p; i++) {
        u64 t0 = tsc_now();
        for (int k = 0; k < 64; k++) (void)inb(p[i].port);
        u32 per = (u32)(tsc_now() - t0) / 64, mhz = tsc_per_ms / 1000;
        u32 ns = mhz ? per * 1000 / mhz : 0;
        n += snprintf(b + n, sizeof b - n, "%s %u.%02u us%s", p[i].what, ns / 1000, ns % 1000 / 10,
                      i + 1 < sizeof p / sizeof *p ? ", " : "");
    }
    if (fl & 0x200) sti();
    kprintf("perf: one read of port %s\n", b);
}

void kdelay_us(u32 us)
{
    if (tsc_per_us_q10) {
        u64 end = tsc_now() + (((u64)us * tsc_per_us_q10) >> 10);
        while (tsc_now() < end) __asm__ volatile("rep; nop");      /* PAUSE */
        return;
    }
    while (us) {                                 /* 1 ms at a time: no overflow */
        u32 step = us > 1000 ? 1000 : us;
        us -= step;
        for (u32 n = step * p80_per_ms / 1000 + 1; n; n--) { (void)inb(0x61); inb(0x80); }
    }
}

static u32 ms_since_reset(void)
{
    u64 t = tsc_now();
    u32 lo = (u32)t, hi = (u32)(t >> 32), q, r;
    if (!tsc_per_ms || hi >= tsc_per_ms) return 0;
    __asm__("divl %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "rm"(tsc_per_ms));
    (void)r;
    return q;
}
int serial_mode_dos(void) { return serial_mode == 2; }

void serial_init(const char *cmd)
{
    const char *s = cmd ? strstr(cmd, "serial=") : 0;
    if (s && (s == cmd || s[-1] == ' ')) {
        if (!strncmp(s + 7, "dos", 3)) serial_mode = 2;
        else if (!strncmp(s + 7, "off", 3)) serial_mode = 1;
    }
    tsc_calibrate();                             /* kdelay_us needs it even with logtime=off */
    if (!tsc_per_ms) p80_calibrate();
    log_times = !(cmd && strstr(cmd, "logtime=off"));
    if (serial_mode) return;                     /* serial_ok stays 0: the port untouched */
    outb(COM1 + 1, 0);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 1);           /* 115200 */
    outb(COM1 + 1, 0);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x03);
    serial_ok = inb(COM1 + 5) != 0xFF;
}

static void serial_putc(char c)
{
    if (!serial_ok) return;
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) ;
    outb(COM1, c);
}

/* The last few KiB of the log, shown on the "vmdos stopped" screen: on a
   real PC there is usually no serial port to read it from. */
static char logbuf[16384];
static u32 logpos;

void kprintf(const char *fmt, ...)
{
    char buf[256], ts[24];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    rlock_take(&log_lock);                        /* the helper CPU logs too */
    for (char *p = buf; *p; p++) {
        if (at_bol && tsc_per_ms && log_times && *p != '\n') {
            u32 ms = ms_since_reset();
            snprintf(ts, sizeof ts, "[%4u.%03u] ", ms / 1000, ms % 1000);
            for (char *t = ts; *t; t++) { serial_putc(*t); bootcon_putc(*t); logbuf[logpos++ % sizeof logbuf] = *t; }
        }
        at_bol = *p == '\n';
        if (*p == '\n') serial_putc('\r');
        serial_putc(*p);
        bootcon_putc(*p);
        logbuf[logpos++ % sizeof logbuf] = *p;
    }
    rlock_drop(&log_lock);
}

/* The log so far onto the boot console, when it starts late (a
   framebuffer above 4 GiB: only reachable once paging is on). */
void log_to_bootcon(void)
{
    rlock_take(&log_lock);
    for (u32 p = logpos > sizeof logbuf ? logpos - sizeof logbuf : 0; p < logpos; p++)
        bootcon_putc(logbuf[p % sizeof logbuf]);
    rlock_drop(&log_lock);
}

/* ---- the "vmdos stopped" screen: the message, then the log, scrollable
   with the arrow keys / PgUp / PgDn / Home / End (PS/2 or USB keyboard). */
#define LOG_W 76
static char loglin[16384 + 2048];
static u16 line_at[2048];
static int n_lines;

static void log_lines(void)
{
    u32 end = logpos, start = logpos > sizeof logbuf ? logpos - sizeof logbuf : 0;
    int o = 0, col = 0;
    n_lines = 0;
    line_at[n_lines++] = 0;
    for (u32 p = start; p < end && o < (int)sizeof loglin - 2 && n_lines < 2047; p++) {
        char c = logbuf[p % sizeof logbuf];
        if (c == '\n' || col == LOG_W) {
            loglin[o++] = 0; col = 0;
            line_at[n_lines++] = (u16)o;
            if (c == '\n') continue;
        }
        if (c < 32 || c > 126) continue;
        loglin[o++] = c; col++;
    }
    loglin[o] = 0;
    if (n_lines > 1 && !loglin[line_at[n_lines - 1]]) n_lines--;    /* the empty line after the last newline */
}

/* ---- the log for DOS: DMESG.COM ----
   INT 2Fh AX=564Ch: EDX = log position to start at (0: the oldest kept),
   CX = buffer size at ES:DI (at least 2). Copies the log from there, '\n'
   as CR LF; returns CX = bytes copied (0: the end), EDX = position to ask
   for next, AX = 0, BX = 'VM'. */
void log_api(struct regs *r)
{
    u32 start = logpos > sizeof logbuf ? logpos - sizeof logbuf : 0, p = r->edx;
    if (p < start) p = start;
    u32 a = LIN(r->v86_es, DI(r)), n = 0, max = CX(r);
    while (p < logpos && n + 2 <= max) {
        char c = logbuf[p++ % sizeof logbuf];
        if (c == '\n') wr8(a + n++, '\r');
        wr8(a + n++, (u8)c);
    }
    CX(r) = (u16)n;
    r->edx = p;
    r->eax = (r->eax & 0xFFFF0000u);
    BX(r) = 0x564D;
}

/* The log into C:\VMDOS\VMDOS.LOG, a file DMESG /K made: its own sectors,
   overwritten in place (newest lines kept), so no FAT or directory sector
   changes and DOS's file system is never touched. The file must start with
   "vmdos log" (so a file of the user's is never overwritten). Pads with
   Ctrl+Z and spaces. 0 done, else what's wrong. */
const char *log_save(void)
{
    static int busy;
    static struct fatvol v;
    static struct extent ext[64];
    static u8 sec[512];
    u32 c, size;
    if (busy) return "busy";
    busy = 1;
    const char *err = 0;
    if (disk_volume(&v)) err = "C: has no FAT partition";
    else if (fat_lookup(&v, "\\VMDOS\\VMDOS.LOG", &c, &size)) err = "no C:\\VMDOS\\VMDOS.LOG (DMESG /K makes it)";
    else if (size < 4096) err = "C:\\VMDOS\\VMDOS.LOG is too small (DMESG /K makes it again)";
    int n = 0;
    if (!err) {
        if (size > 1024 * 1024) size = 1024 * 1024;
        size &= ~511u;
        n = fat_extents(&v, c, size, ext, 64);
        if (n <= 0) err = "C:\\VMDOS\\VMDOS.LOG is too fragmented";
        else if (disk_read(ext[0].lba, 1, sec) || memcmp(sec, "vmdos log", 9)) err = "C:\\VMDOS\\VMDOS.LOG isn't one DMESG /K made";
    }
    if (!err) {
        /* header, then the newest part of the log that fits (CR LF lines) */
        static const char hdr[] = "vmdos log, saved by vmdos itself\r\n\r\n";
        u32 start = logpos > sizeof logbuf ? logpos - sizeof logbuf : 0, len = 0;
        for (u32 p = start; p < logpos; p++) len += logbuf[p % sizeof logbuf] == '\n' ? 2 : 1;
        u32 room = size - (sizeof hdr - 1) - 1;
        while (len > room) len -= logbuf[start++ % sizeof logbuf] == '\n' ? 2 : 1;
        u32 p = start, h = 0;
        int cr = 0, eof = 0;
        for (int e = 0; e < n && !err; e++)
            for (u32 s = 0; s < ext[e].count && !err; s++) {
                for (int b = 0; b < 512; b++) {
                    u8 ch;
                    if (h < sizeof hdr - 1) ch = (u8)hdr[h++];
                    else if (cr) { ch = '\n'; cr = 0; }
                    else if (p < logpos) {
                        ch = (u8)logbuf[p++ % sizeof logbuf];
                        if (ch == '\n') { ch = '\r'; cr = 1; }
                    } else if (!eof) { ch = 0x1A; eof = 1; }        /* Ctrl+Z: TYPE stops here */
                    else ch = ' ';
                    sec[b] = ch;
                }
                if (disk_write(ext[e].lba + s, 1, sec)) err = "a disk write failed";
            }
        disk_flush();
    }
    busy = 0;
    return err;
}

void usb_tick(void);
static int stop_key(void)
{
    if ((inb(0x64) & 0x21) == 0x01) return inb(0x60);              /* PS/2 keyboard byte */
    usb_tick();                                                      /* USB keyboards (safe: guarded in usb.cpp) */
    return vkbd_take();
}

/* closable: the Ctrl+Shift+F10 log, which Esc (or F10) closes; else the
   "vmdos stopped" screen, which never returns. */
static void log_screen(const char *msg, int closable)
{
    static char screen[4096];
    log_lines();
    int msg_rows = 1, col = 0;                                       /* as video_console wraps it */
    for (const char *q = msg; *q; q++) {
        if (*q == '\n' || col >= 76) { msg_rows++; col = 0; if (*q == '\n') continue; }
        col++;
    }
    int rows = 24 - 3 - msg_rows - 3;
    if (rows < 3) rows = 3;
    int top = n_lines > rows ? n_lines - rows : 0, last = -1;
    const char *saved = 0;
    for (;;) {
        if (top != last) {
            int n = snprintf(screen, sizeof screen, "%s\n\nLog, lines %d-%d of %d (arrows, PgUp/PgDn, Home/End, S: save%s):%s%s\n",
                             msg, top + 1, top + rows < n_lines ? top + rows : n_lines, n_lines, closable ? ", Esc: back" : "",
                             saved ? " " : "", saved ? saved : "");
            for (int i = top; i < top + rows && i < n_lines && n < (int)sizeof screen - 80; i++)
                n += snprintf(screen + n, sizeof screen - n, "%s\n", loglin + line_at[i]);
            if (closable) video_console_attr("vmdos log", screen, 0x1F);
            else video_console(screen);
            last = top;
        }
        int k = stop_key();
        if (k < 0) { kdelay_us(2000); continue; }
        int max = n_lines > rows ? n_lines - rows : 0;
        switch (k) {
        case 0x48: top--; break;                                     /* up */
        case 0x50: top++; break;                                     /* down */
        case 0x49: top -= rows - 1; break;                           /* page up */
        case 0x51: top += rows - 1; break;                           /* page down */
        case 0x47: top = 0; break;                                   /* home */
        case 0x4F: top = max; break;                                 /* end */
        case 0x01: case 0x44: if (closable) return; break;          /* Esc, F10 */
        case 0x1F: {                                                 /* S: into C:\VMDOS\VMDOS.LOG */
            const char *e = log_save();
            saved = e ? e : "saved to C:\\VMDOS\\VMDOS.LOG";
            last = -1;
            break; }
        }
        if (top > max) top = max;
        if (top < 0) top = 0;
    }
}

/* Ctrl+Shift+F10 (vdev.c asks, the timer tick calls this when USB isn't
   busy): the log over the game, which waits until Esc. */
volatile int log_view_req;
int vkbd_mods(void);
void vkbd_release_mods(int m);
void log_view(void)
{
    int mods = vkbd_mods();                                         /* what the guest saw held: Ctrl, Shift */
    log_view_req = 0;
    while (stop_key() >= 0) ;                                       /* the hotkey's own bytes */
    video_console_save();
    log_screen("The log so far (the game is paused).", 1);
    video_console_restore();
    vkbd_release_mods(mods);                                         /* Ctrl/Shift went up while we had the keyboard */
}

void panic(const char *fmt, ...)
{
    char buf[1024];
    cli();
    smp_active = 0;                               /* the helper CPU stops drawing over the screen */
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    disk_flush();                                 /* don't lose gathered writes (no-op while flushing) */
    kprintf("\n*** vmdos stopped: %s\n", buf);
    const char *se = log_save();                  /* into C:\VMDOS\VMDOS.LOG, if DMESG /K made it */
    kprintf(se ? "log not saved: %s\n" : "log saved to C:\\VMDOS\\VMDOS.LOG\n", se);
    if (bootcon_on && !video_fb_ready()) for (;;) __asm__ volatile("cli; hlt");   /* the boot console shows it */
    bootcon_stop();
    log_screen(buf, 0);
    for (;;) __asm__ volatile("cli; hlt");
}

int perf_on;
void perf_io_probe(void)
{
    /* "perf" or "perf=..." anywhere on the command line (any case) */
    for (const char *c = cmdline; *c; c++)
        if ((c == cmdline || c[-1] == ' ' || c[-1] == '\t') && (c[0] | 32) == 'p' && (c[1] | 32) == 'e' &&
            (c[2] | 32) == 'r' && (c[3] | 32) == 'f' && (!c[4] || c[4] == ' ' || c[4] == '=' || c[4] == '\t'))
            perf_on = 1;
    if (!perf_on) return;
    kprintf("perf: on, a report every 5 s\n");
    io_probe();
}
