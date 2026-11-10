#ifndef COMPAT_PARAM_H
#define COMPAT_PARAM_H
#include_next <sys/param.h>
#include <sys/cdefs.h>
#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif
#ifndef roundup2
#define roundup2(x, y) (((x) + ((y) - 1)) & (~((y) - 1)))
#endif
#ifndef powerof2
#define powerof2(x) ((((x) - 1) & (x)) == 0)
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#endif
