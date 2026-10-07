/* The few math functions dbopl's one-time table setup needs, on the x87.
   Only used from opl_init(), which saves and restores the FPU state. */
#include <stdint.h>

static double x87_log2(double x)
{
    double r;
    __asm__ ("fld1\n\t"
             "fxch\n\t"
             "fyl2x" : "=t"(r) : "0"(x));
    return r;
}

static double x87_round(double x)
{
    double r;
    __asm__ ("frndint" : "=t"(r) : "0"(x));
    return r;
}

static double x87_exp2(double x)
{
    double n = x87_round(x), f = x - n, p, r;
    __asm__ ("f2xm1" : "=t"(p) : "0"(f));
    p += 1.0;
    __asm__ ("fscale" : "=t"(r) : "0"(p), "u"(n));
    return r;
}

double pow(double a, double b)
{
    if (b == 0.0) return 1.0;
    if (a == 0.0) return 0.0;
    return x87_exp2(b * x87_log2(a));
}

double log10(double x)
{
    return x87_log2(x) * 0.30102999566398119521;
}

double sin(double x)
{
    double r;
    __asm__ ("fsin" : "=t"(r) : "0"(x));
    return r;
}

long labs(long v) { return v < 0 ? -v : v; }
