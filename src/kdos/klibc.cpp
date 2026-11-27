/* kdos: the C library DOSBox-X's DOS and shell expect, for vmdos's kernel
   (which has no libc). Small and plain: an allocator over memory taken
   from the monitor's heap once, the string and ctype functions, a printf
   and sscanf good for what DOSBox-X formats, time from the CMOS clock.
   No files of the host: there is no host. */
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <new>

extern "C" {
void *kh_alloc(uint32_t bytes);
void kh_log(const char *s);
void kh_panic(const char *s) __attribute__((noreturn));
uint32_t kh_ticks(void);
void kh_rtc(int *y, int *mo, int *d, int *h, int *mi, int *s);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
}

/* ---------------- malloc: first fit, coalescing, over arenas taken from
   the monitor's heap as needed (only ever with shell=kernel) ---------------- */
#define ARENA_SIZE (2u << 20)
struct blk { uint32_t size; uint32_t free; blk *next, *prev; };   /* size: bytes after the header */
static blk *arenas[64];
static int narenas;
static blk *arena_new(size_t need)
{
    size_t bytes = need + sizeof(blk) > ARENA_SIZE ? ((need + sizeof(blk) + 4095) & ~(size_t)4095) : ARENA_SIZE;
    if (narenas == 64) kh_panic("the DOS kernel's heap is full");
    blk *a = (blk *)kh_alloc((uint32_t)bytes);
    if (!a) kh_panic("no memory for the DOS kernel's heap");
    a->size = (uint32_t)(bytes - sizeof(blk)); a->free = 1; a->next = a->prev = nullptr;
    arenas[narenas++] = a;
    return a;
}
extern "C" void *malloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (!n) n = 16;
    for (int pass = 0; pass < 2; pass++) {
        for (int k = 0; k < narenas; k++)
            for (blk *b = arenas[k]; b; b = b->next) {
                if (!b->free || b->size < n) continue;
                if (b->size >= n + sizeof(blk) + 32) {          /* split */
                    blk *r = (blk *)((char *)(b + 1) + n);
                    r->size = b->size - n - sizeof(blk); r->free = 1;
                    r->next = b->next; r->prev = b; if (b->next) b->next->prev = r;
                    b->next = r; b->size = n;
                }
                b->free = 0;
                return b + 1;
            }
        arena_new(n);
    }
    kh_panic("the DOS kernel's heap is full");
}
extern "C" void free(void *p)
{
    if (!p) return;
    blk *b = (blk *)p - 1;
    b->free = 1;
    if (b->next && b->next->free) {                               /* merge with the next */
        b->size += sizeof(blk) + b->next->size;
        b->next = b->next->next; if (b->next) b->next->prev = b;
    }
    if (b->prev && b->prev->free) {                               /* and the previous */
        blk *a = b->prev;
        a->size += sizeof(blk) + b->size;
        a->next = b->next; if (a->next) a->next->prev = a;
    }
}
extern "C" void *calloc(size_t a, size_t b) { void *p = malloc(a * b); memset(p, 0, a * b); return p; }
extern "C" void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    blk *b = (blk *)p - 1;
    if (b->size >= n) return p;
    void *q = malloc(n);
    memcpy(q, p, b->size);
    free(p);
    return q;
}
void *operator new(size_t n) { return malloc(n); }
void *operator new[](size_t n) { return malloc(n); }
void *operator new(size_t n, const std::nothrow_t &) noexcept { return malloc(n); }
void *operator new[](size_t n, const std::nothrow_t &) noexcept { return malloc(n); }
void operator delete(void *p) noexcept { free(p); }
void operator delete[](void *p) noexcept { free(p); }
void operator delete(void *p, size_t) noexcept { free(p); }
void operator delete[](void *p, size_t) noexcept { free(p); }

/* the C++ runtime's failure paths (no exceptions here) */
namespace std {
void __throw_length_error(const char *s) { kh_panic(s); }
void __throw_logic_error(const char *s) { kh_panic(s); }
void __throw_out_of_range(const char *s) { kh_panic(s); }
void __throw_out_of_range_fmt(const char *s, ...) { kh_panic(s); }
void __throw_invalid_argument(const char *s) { kh_panic(s); }
void __throw_bad_alloc() { kh_panic("bad_alloc"); }
void __throw_bad_array_new_length() { kh_panic("bad_array_new_length"); }
void __throw_bad_function_call() { kh_panic("bad_function_call"); }
void __throw_bad_cast() { kh_panic("bad_cast"); }
void __throw_runtime_error(const char *s) { kh_panic(s); }
void __throw_regex_error(int) { kh_panic("regex"); }
void __glibcxx_assert_fail(const char *file, int line, const char *fn, const char *cond) { (void)file; (void)line; (void)fn; kh_panic(cond); }
}
/* The exception-handling entry points. kdos is built without exceptions and
   never throws, but some GCCs build libstdc++'s list.o/tree.o (which kdos
   links) with cleanup code that names them (Fedora's GCC 16 does). Never
   reached; weak, so a toolchain whose objects do bring them in wins. */
extern "C" {
__attribute__((weak)) int __gxx_personality_v0(int, int, unsigned long long, void *, void *) { return 3; /* _URC_FATAL_PHASE1_ERROR */ }
__attribute__((weak)) void _Unwind_Resume(void *) { kh_panic("_Unwind_Resume: an exception in the DOS kernel"); }
__attribute__((weak)) void __cxa_call_unexpected(void *) { kh_panic("__cxa_call_unexpected"); }
__attribute__((weak)) void *__cxa_begin_catch(void *e) { (void)e; kh_panic("__cxa_begin_catch"); }
__attribute__((weak)) void __cxa_end_catch(void) {}
__attribute__((weak)) void __cxa_rethrow(void) { kh_panic("__cxa_rethrow"); }
__attribute__((weak)) void __cxa_throw(void *, void *, void (*)(void *)) { kh_panic("__cxa_throw"); }
__attribute__((weak)) void *__cxa_allocate_exception(size_t) { kh_panic("__cxa_allocate_exception"); }
__attribute__((weak)) void __cxa_bad_cast(void) { kh_panic("bad_cast"); }
__attribute__((weak)) void __cxa_bad_typeid(void) { kh_panic("bad_typeid"); }
}
namespace std { __attribute__((weak)) void terminate() noexcept { kh_panic("std::terminate"); } }

extern "C" {
char __libc_single_threaded = 1;
void *__dso_handle;
void __cxa_pure_virtual() { kh_panic("pure virtual call"); }
int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
int atexit(void (*)(void)) { return 0; }
void abort(void) { kh_panic("abort"); }
void __assert_fail(const char *a, const char *f, unsigned l, const char *fn) { (void)f; (void)l; (void)fn; kh_panic(a); }
void __stack_chk_fail(void) { kh_panic("stack smashed"); }
void __stack_chk_fail_local(void) { kh_panic("stack smashed"); }

/* ---------------- ctype (code page 437: only ASCII is classified) ---------------- */
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
int isprint(int c) { return c >= 32 && c < 127; }
int isgraph(int c) { return c > 32 && c < 127; }
int iscntrl(int c) { return (c >= 0 && c < 32) || c == 127; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int isblank(int c) { return c == ' ' || c == '\t'; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
/* glibc's <ctype.h> inlines toupper/tolower through these tables */
static int32_t up_tab[384], lo_tab[384];
static const int32_t *up_p, *lo_p;
static void case_tabs(void) {
    for (int c = -128; c < 256; c++) { up_tab[c + 128] = c >= 0 ? toupper(c) : c; lo_tab[c + 128] = c >= 0 ? tolower(c) : c; }
    up_p = up_tab + 128; lo_p = lo_tab + 128;
}
extern "C" const int32_t **__ctype_toupper_loc(void) { if (!up_p) case_tabs(); return &up_p; }
extern "C" const int32_t **__ctype_tolower_loc(void) { if (!lo_p) case_tabs(); return &lo_p; }

/* ---------------- strings ---------------- */
int strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (unsigned char)*a - (unsigned char)*b; }
int strcasecmp(const char *a, const char *b)
{
    while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x) return x - y;
    }
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }
char *strncpy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i < n && s[i]; i++) d[i] = s[i];
    for (; i < n; i++) d[i] = 0;
    return d;
}
char *stpcpy(char *d, const char *s) { while ((*d = *s++)) d++; return d; }
char *stpncpy(char *d, const char *s, size_t n) { size_t i = 0; for (; i < n && s[i]; i++) d[i] = s[i]; char *e = d + i; for (; i < n; i++) d[i] = 0; return e; }
char *strcat(char *d, const char *s) { strcpy(d + strlen(d), s); return d; }
char *strncat(char *d, const char *s, size_t n)
{
    char *e = d + strlen(d);
    while (n-- && *s) *e++ = *s++;
    *e = 0;
    return d;
}
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return nullptr; } }
char *strrchr(const char *s, int c) { const char *r = nullptr; for (;; s++) { if (*s == (char)c) r = s; if (!*s) return (char *)r; } }
void *memchr(const void *s, int c, size_t n) { const unsigned char *p = (const unsigned char *)s; for (; n; n--, p++) if (*p == (unsigned char)c) return (void *)p; return nullptr; }
size_t strspn(const char *s, const char *a) { size_t n = 0; while (s[n] && strchr(a, s[n])) n++; return n; }
size_t strcspn(const char *s, const char *a) { size_t n = 0; while (s[n] && !strchr(a, s[n])) n++; return n; }
char *strpbrk(const char *s, const char *a) { s += strcspn(s, a); return *s ? (char *)s : nullptr; }
char *strdup(const char *s) { size_t n = strlen(s) + 1; char *d = (char *)malloc(n); memcpy(d, s, n); return d; }
size_t strnlen(const char *s, size_t m) { size_t n = 0; while (n < m && s[n]) n++; return n; }
char *strtok(char *s, const char *d)
{
    static char *last;
    if (!s) s = last;
    if (!s) return nullptr;
    s += strspn(s, d);
    if (!*s) { last = nullptr; return nullptr; }
    char *e = s + strcspn(s, d);
    if (*e) { *e = 0; last = e + 1; } else last = nullptr;
    return s;
}
char *strerror(int) { return (char *)"error"; }

/* ---------------- numbers ---------------- */
static int digit(int c, int base)
{
    int v = isdigit(c) ? c - '0' : islower(c) ? c - 'a' + 10 : isupper(c) ? c - 'A' + 10 : 99;
    return v < base ? v : -1;
}
unsigned long long strtoull(const char *s, char **end, int base)
{
    const char *p = s;
    while (isspace((unsigned char)*p)) p++;
    int neg = 0;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' && digit(p[2], 16) >= 0) { p += 2; base = 16; }
    else if (base == 0) base = p[0] == '0' ? 8 : 10;
    unsigned long long v = 0;
    const char *st = p;
    for (int d; (d = digit((unsigned char)*p, base)) >= 0; p++) v = v * base + d;
    if (end) *end = (char *)(p == st ? s : p);
    return neg ? -v : v;
}
long long strtoll(const char *s, char **e, int b) { return (long long)strtoull(s, e, b); }
long strtol(const char *s, char **e, int b) { return (long)strtoull(s, e, b); }
unsigned long strtoul(const char *s, char **e, int b) { return (unsigned long)strtoull(s, e, b); }
long __isoc23_strtol(const char *s, char **e, int b) { return strtol(s, e, b); }
unsigned long __isoc23_strtoul(const char *s, char **e, int b) { return strtoul(s, e, b); }
long long __isoc23_strtoll(const char *s, char **e, int b) { return strtoll(s, e, b); }
unsigned long long __isoc23_strtoull(const char *s, char **e, int b) { return strtoull(s, e, b); }
int atoi(const char *s) { return (int)strtol(s, nullptr, 10); }
long atol(const char *s) { return strtol(s, nullptr, 10); }
long long atoll(const char *s) { return strtoll(s, nullptr, 10); }
double strtod(const char *s, char **e) { return (double)strtol(s, e, 10); }
double atof(const char *s) { return (double)atol(s); }
int abs(int v) { return v < 0 ? -v : v; }
long labs(long v) { return v < 0 ? -v : v; }
static unsigned long rnd = 1;
int rand(void) { rnd = rnd * 1103515245 + 12345; return (int)((rnd >> 16) & 0x7FFF); }
void srand(unsigned s) { rnd = s; }

static void qswap(char *a, char *b, size_t n) { while (n--) { char t = *a; *a++ = *b; *b++ = t; } }
void qsort(void *base, size_t n, size_t sz, int (*cmp)(const void *, const void *))
{
    char *b = (char *)base;                                    /* insertion sort: lists are short */
    for (size_t i = 1; i < n; i++)
        for (size_t j = i; j > 0 && cmp(b + (j - 1) * sz, b + j * sz) > 0; j--) qswap(b + (j - 1) * sz, b + j * sz, sz);
}

/* ---------------- printf ---------------- */
struct out { char *p; size_t left; size_t n; };
static void put(out *o, char c) { if (o->left > 1) { *o->p++ = c; o->left--; } o->n++; }
int vsnprintf(char *buf, size_t size, const char *f, va_list ap)
{
    out o = { buf, size, 0 };
    for (; *f; f++) {
        if (*f != '%') { put(&o, *f); continue; }
        f++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; f++) {
            if (*f == '-') left = 1; else if (*f == '0') zero = 1; else if (*f == '+') plus = 1;
            else if (*f == ' ') space = 1; else if (*f == '#') alt = 1; else break;
        }
        int width = 0, prec = -1;
        if (*f == '*') { width = va_arg(ap, int); if (width < 0) { left = 1; width = -width; } f++; }
        else while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = va_arg(ap, int); f++; }
            else while (isdigit((unsigned char)*f)) prec = prec * 10 + (*f++ - '0');
        }
        int lng = 0;
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'j' || *f == 't' || *f == 'L' || *f == 'q') {
            if (*f == 'l' || *f == 'q' || *f == 'L') lng++;
            f++;
        }
        char tmp[72]; const char *s = tmp; int len = 0; char sign = 0;
        switch (*f) {
        case 'c': tmp[0] = (char)va_arg(ap, int); len = 1; break;
        case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; len = (int)strlen(s); if (prec >= 0 && len > prec) len = prec; break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            unsigned long long v; int neg = 0;
            if (*f == 'p') { v = (uintptr_t)va_arg(ap, void *); alt = 1; }
            else if (*f == 'd' || *f == 'i') {
                long long sv = lng >= 2 ? va_arg(ap, long long) : lng ? va_arg(ap, long) : va_arg(ap, int);
                neg = sv < 0; v = neg ? -(unsigned long long)sv : sv;
            } else v = lng >= 2 ? va_arg(ap, unsigned long long) : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned int);
            int base = (*f == 'x' || *f == 'X' || *f == 'p') ? 16 : *f == 'o' ? 8 : 10;
            const char *dig = *f == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char *e = tmp + sizeof tmp; char *q = e;
            do { *--q = dig[v % base]; v /= base; } while (v);
            while (prec >= 0 && e - q < prec) *--q = '0';
            if (alt && base == 16) { *--q = *f == 'X' ? 'X' : 'x'; *--q = '0'; }
            sign = neg ? '-' : plus ? '+' : space ? ' ' : 0;
            s = q; len = (int)(e - q);
            break; }
        case 'f': case 'g': case 'e': {
            double d = va_arg(ap, double);
            long long ip = (long long)d; int neg = d < 0;
            if (neg) { ip = -ip; d = -d; }
            int pr = prec < 0 ? 6 : prec;
            double fr = d - (double)ip;
            char *q = tmp;
            if (neg) *q++ = '-';
            char ib[24]; int k = 0; do { ib[k++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
            while (k) *q++ = ib[--k];
            if (pr) { *q++ = '.'; while (pr-- && q < tmp + 60) { fr *= 10; int dg = (int)fr; *q++ = (char)('0' + dg); fr -= dg; } }
            len = (int)(q - tmp); s = tmp;
            break; }
        case '%': tmp[0] = '%'; len = 1; break;
        case 'n': *va_arg(ap, int *) = (int)o.n; continue;
        default: tmp[0] = '%'; tmp[1] = *f; len = 2; if (!*f) { f--; len = 1; } break;
        }
        int total = len + (sign ? 1 : 0);
        if (!left && !zero) for (int i = total; i < width; i++) put(&o, ' ');
        if (sign) put(&o, sign);
        if (!left && zero) for (int i = total; i < width; i++) put(&o, '0');
        for (int i = 0; i < len; i++) put(&o, s[i]);
        if (left) for (int i = total; i < width; i++) put(&o, ' ');
    }
    if (o.left) *o.p = 0;
    return (int)o.n;
}
int snprintf(char *b, size_t n, const char *f, ...) { va_list a; va_start(a, f); int r = vsnprintf(b, n, f, a); va_end(a); return r; }
int vsprintf(char *b, const char *f, va_list a) { return vsnprintf(b, 0x7FFFFFFF, f, a); }
int sprintf(char *b, const char *f, ...) { va_list a; va_start(a, f); int r = vsnprintf(b, 0x7FFFFFFF, f, a); va_end(a); return r; }
int vasprintf(char **s, const char *f, va_list a)
{
    va_list b; va_copy(b, a);
    int n = vsnprintf(nullptr, 0, f, b); va_end(b);
    *s = (char *)malloc(n + 1);
    return vsnprintf(*s, n + 1, f, a);
}
int printf(const char *f, ...)
{
    char b[512]; va_list a; va_start(a, f); int r = vsnprintf(b, sizeof b, f, a); va_end(a);
    kh_log(b); return r;
}
int puts(const char *s) { kh_log(s); kh_log("\n"); return 0; }
int putchar(int c) { char b[2] = { (char)c, 0 }; kh_log(b); return c; }

/* sscanf: %d %i %u %x %X %o %c %s %[...] with widths and %*, enough for DOSBox-X */
int vsscanf(const char *s, const char *f, va_list ap)
{
    int n = 0;
    const char *st = s;
    for (; *f; f++) {
        if (isspace((unsigned char)*f)) { while (isspace((unsigned char)*s)) s++; continue; }
        if (*f != '%') { if (*s != *f) return n; s++; continue; }
        f++;
        int skip = 0, width = 0, lng = 0;
        if (*f == '*') { skip = 1; f++; }
        while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'h' || *f == 'L') { if (*f != 'h') lng++; f++; }
        if (*f == 'n') { if (!skip) *va_arg(ap, int *) = (int)(s - st); continue; }
        if (*f != 'c' && *f != '[') while (isspace((unsigned char)*s)) s++;
        if (!*s) return n ? n : -1;
        switch (*f) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            char buf[40]; int i = 0;
            int lim = width ? width : 39;
            while (*s && i < lim && (isalnum((unsigned char)*s) || ((*s == '-' || *s == '+') && i == 0))) buf[i++] = *s++;
            buf[i] = 0;
            char *e;
            int base = (*f == 'x' || *f == 'X') ? 16 : *f == 'o' ? 8 : *f == 'i' ? 0 : 10;
            unsigned long long v = strtoull(buf, &e, base);
            if (e == buf) return n;
            s -= (buf + i) - e;
            if (!skip) {
                if (lng >= 2) *va_arg(ap, long long *) = (long long)v;
                else if (lng) *va_arg(ap, long *) = (long)v;
                else *va_arg(ap, int *) = (int)v;
                n++;
            }
            break; }
        case 'c': {
            int w = width ? width : 1;
            char *d = skip ? nullptr : va_arg(ap, char *);
            while (w-- && *s) { if (d) *d++ = *s; s++; }
            if (!skip) n++;
            break; }
        case 's': {
            char *d = skip ? nullptr : va_arg(ap, char *);
            int w = width ? width : 0x7FFFFFFF;
            while (*s && !isspace((unsigned char)*s) && w--) { if (d) *d++ = *s; s++; }
            if (d) *d = 0;
            if (!skip) n++;
            break; }
        case '[': {
            f++;
            int neg = 0; if (*f == '^') { neg = 1; f++; }
            const char *set = f;
            if (*f == ']') f++;
            while (*f && *f != ']') f++;
            size_t setlen = f - set;
            char *d = skip ? nullptr : va_arg(ap, char *);
            int w = width ? width : 0x7FFFFFFF;
            const char *b0 = s;
            while (*s && w--) {
                int in = memchr(set, *s, setlen) != nullptr;
                if (in == neg) break;
                if (d) *d++ = *s;
                s++;
            }
            if (s == b0) return n;
            if (d) *d = 0;
            if (!skip) n++;
            break; }
        default: return n;
        }
    }
    return n;
}
int sscanf(const char *s, const char *f, ...) { va_list a; va_start(a, f); int r = vsscanf(s, f, a); va_end(a); return r; }
int __isoc23_sscanf(const char *s, const char *f, ...) { va_list a; va_start(a, f); int r = vsscanf(s, f, a); va_end(a); return r; }
int __isoc99_sscanf(const char *s, const char *f, ...) { va_list a; va_start(a, f); int r = vsscanf(s, f, a); va_end(a); return r; }

/* ---------------- time: the CMOS clock, local time = the clock's ---------------- */
static int days_before(int y, int m)
{
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int d = cum[m] + (m > 1 && ((y % 4 == 0 && y % 100) || y % 400 == 0));
    return d;
}
time_t mktime(struct tm *t)
{
    int y = t->tm_year + 1900;
    long days = (y - 1970) * 365L + ((y - 1969) / 4) - ((y - 1901) / 100) + ((y - 1601) / 400) + days_before(y, t->tm_mon) + t->tm_mday - 1;
    return (time_t)(days * 86400L + t->tm_hour * 3600L + t->tm_min * 60L + t->tm_sec);
}
time_t time(time_t *p)
{
    struct tm t = {};
    int y, mo, d, h, mi, s;
    kh_rtc(&y, &mo, &d, &h, &mi, &s);
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
    time_t r = mktime(&t);
    if (p) *p = r;
    return r;
}
struct tm *gmtime_r(const time_t *tp, struct tm *t)
{
    long long v = *tp;
    long days = (long)(v / 86400); long rem = (long)(v % 86400);
    if (rem < 0) { rem += 86400; days--; }
    t->tm_hour = rem / 3600; t->tm_min = rem % 3600 / 60; t->tm_sec = rem % 60;
    t->tm_wday = (int)((days + 4) % 7); if (t->tm_wday < 0) t->tm_wday += 7;
    int y = 1970;
    for (;;) {
        int len = ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 366 : 365;
        if (days < len) break;
        days -= len; y++;
    }
    t->tm_year = y - 1900; t->tm_yday = (int)days;
    int m = 0;
    while (m < 11 && days >= days_before(y, m + 1)) m++;
    t->tm_mon = m; t->tm_mday = (int)(days - days_before(y, m)) + 1; t->tm_isdst = 0;
    return t;
}
static struct tm tmbuf;
struct tm *localtime(const time_t *t) { return gmtime_r(t, &tmbuf); }
struct tm *gmtime(const time_t *t) { return gmtime_r(t, &tmbuf); }
struct tm *localtime_r(const time_t *t, struct tm *r) { return gmtime_r(t, r); }
int clock_gettime(int, struct timespec *ts)
{
    uint32_t ms = kh_ticks();
    ts->tv_sec = ms / 1000; ts->tv_nsec = (ms % 1000) * 1000000L;
    return 0;
}
long clock(void) { return (long)kh_ticks() * 1000; }
size_t strftime(char *s, size_t n, const char *, const struct tm *) { if (n) *s = 0; return 0; }

/* ---------------- no host files ---------------- */
void *stderr, *stdout, *stdin;
void *fopen(const char *, const char *) { return nullptr; }
void *fopen64(const char *, const char *) { return nullptr; }
int fclose(void *) { return 0; }
char *fgets(char *, int, void *) { return nullptr; }
size_t fwrite(const void *, size_t, size_t n, void *) { return n; }
size_t fread(void *, size_t, size_t, void *) { return 0; }
int fflush(void *) { return 0; }
int fseek(void *, long, int) { return -1; }
int fseeko64(void *, long long, int) { return -1; }
long ftell(void *) { return -1; }
long long ftello64(void *) { return -1; }
int fileno(void *) { return -1; }
int fprintf(void *, const char *f, ...)
{
    char b[512]; va_list a; va_start(a, f); int r = vsnprintf(b, sizeof b, f, a); va_end(a);
    kh_log(b); return r;
}
int vfprintf(void *, const char *f, va_list a) { char b[512]; int r = vsnprintf(b, sizeof b, f, a); kh_log(b); return r; }
int fputs(const char *s, void *) { kh_log(s); return 0; }
int fputc(int c, void *) { return putchar(c); }
void setbuf(void *, char *) {}
int remove(const char *) { return -1; }
int rename(const char *, const char *) { return -1; }
void *opendir(const char *) { return nullptr; }
void *readdir(void *) { return nullptr; }
int closedir(void *) { return 0; }
int gethostname(char *n, size_t l) { if (l > 5) strcpy(n, "vmdos"); return 0; }
char *getenv(const char *) { return nullptr; }
int stat(const char *, void *) { return -1; }
int access(const char *, int) { return -1; }
char *getcwd(char *b, size_t n) { if (n) *b = 0; return b; }
int *__errno_location(void) { static int e; return &e; }
}
