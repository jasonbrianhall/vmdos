/* Small C library: memory/string helpers, printf, serial log. */
#include "kernel.h"

int debug_level = 1;
static int serial_ok;

void *memcpy(void *d, const void *s, size_t n)
{
    u8 *dd = d; const u8 *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *memmove(void *d, const void *s, size_t n)
{
    u8 *dd = d; const u8 *ss = s;
    if (dd < ss) while (n--) *dd++ = *ss++;
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
void *memset(void *d, int c, size_t n)
{
    u8 *dd = d;
    while (n--) *dd++ = (u8)c;
    return d;
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
static char logbuf[4096];
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

/* The last n log lines (each cut to width), oldest first, into out. */
static void log_tail(char *out, int size, int n, int width)
{
    u32 end = logpos, start = logpos > sizeof logbuf ? logpos - sizeof logbuf : 0, p = end;
    while (p > start && logbuf[(p - 1) % sizeof logbuf] == '\n') p--;      /* trailing newlines */
    int lines = 0;
    while (p > start && lines < n) { p--; if (logbuf[p % sizeof logbuf] == '\n') lines++; }
    if (p > start || lines >= n) p++;
    int o = 0, col = 0;
    for (; p < end && o < size - 1; p++) {
        char c = logbuf[p % sizeof logbuf];
        if (c == '\n') { out[o++] = c; col = 0; continue; }
        if (c < 32 || c > 126 || col >= width) continue;
        out[o++] = c; col++;
    }
    out[o] = 0;
}

void panic(const char *fmt, ...)
{
    char buf[1024];
    cli();
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    {                                               /* the message, then the log that led to it */
        static char screen[2048];
        int n = snprintf(screen, sizeof screen, "%s\n\nLast log lines:\n", buf);
        int msg_rows = 3;
        for (const char *q = buf; *q; q++) if (*q == '\n') msg_rows++;
        msg_rows += (int)strlen(buf) / 76;
        int rows = 20 - msg_rows;
        if (rows < 4) rows = 4;
        log_tail(screen + n, (int)sizeof screen - n, rows, 76);
        kprintf("\n*** vmdos stopped: %s\n", buf);
        video_console(screen);
    }
    for (;;) __asm__ volatile("cli; hlt");
}
