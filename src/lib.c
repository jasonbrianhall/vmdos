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
void serial_init(void)
{
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
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    for (char *p = buf; *p; p++) {
        if (*p == '\n') serial_putc('\r');
        serial_putc(*p);
        logbuf[logpos++ % sizeof logbuf] = *p;
    }
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
    for (;;) {
        if (top != last) {
            int n = snprintf(screen, sizeof screen, "%s\n\nLog, lines %d-%d of %d (arrows, PgUp/PgDn, Home/End%s):\n",
                             msg, top + 1, top + rows < n_lines ? top + rows : n_lines, n_lines, closable ? ", Esc: back" : "");
            for (int i = top; i < top + rows && i < n_lines && n < (int)sizeof screen - 80; i++)
                n += snprintf(screen + n, sizeof screen - n, "%s\n", loglin + line_at[i]);
            if (closable) video_console_attr("vmdos log", screen, 0x1F);
            else video_console(screen);
            last = top;
        }
        int k = stop_key();
        if (k < 0) { for (int i = 0; i < 2000; i++) inb(0x80); continue; }
        int max = n_lines > rows ? n_lines - rows : 0;
        switch (k) {
        case 0x48: top--; break;                                     /* up */
        case 0x50: top++; break;                                     /* down */
        case 0x49: top -= rows - 1; break;                           /* page up */
        case 0x51: top += rows - 1; break;                           /* page down */
        case 0x47: top = 0; break;                                   /* home */
        case 0x4F: top = max; break;                                 /* end */
        case 0x01: case 0x44: if (closable) return; break;          /* Esc, F10 */
        }
        if (top > max) top = max;
        if (top < 0) top = 0;
    }
}

/* Ctrl+Shift+F10 (vdev.c asks, the timer tick calls this when USB isn't
   busy): the log over the game, which waits until Esc. */
volatile int log_view_req;
void vkbd_release_mods(void);
void log_view(void)
{
    log_view_req = 0;
    while (stop_key() >= 0) ;                                       /* the hotkey's own bytes */
    video_console_save();
    log_screen("The log so far (the game is paused).", 1);
    video_console_restore();
    vkbd_release_mods();                                             /* Ctrl/Shift went up while we had the keyboard */
}

void panic(const char *fmt, ...)
{
    char buf[1024];
    cli();
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    disk_flush();                                 /* don't lose gathered writes (no-op while flushing) */
    kprintf("\n*** vmdos stopped: %s\n", buf);
    log_screen(buf, 0);
    for (;;) __asm__ volatile("cli; hlt");
}
