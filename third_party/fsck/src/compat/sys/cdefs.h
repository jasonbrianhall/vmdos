#ifndef COMPAT_CDEFS_H
#define COMPAT_CDEFS_H
#include_next <sys/cdefs.h>
#ifndef __printflike
#define __printflike(a, b) __attribute__((format(printf, a, b)))
#endif
#ifndef __dead2
#define __dead2 __attribute__((noreturn))
#endif
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef __FBSDID
#define __FBSDID(s)
#endif
#ifndef nitems
#define nitems(x) (sizeof(x) / sizeof((x)[0]))
#endif
#endif
#ifndef __RCSID
#define __RCSID(s)
#endif
#ifndef __nonstring
#define __nonstring __attribute__((__nonstring__))
#endif
#include <stdint.h>
#include <limits.h>
#ifndef LONG_BIT
#define LONG_BIT (sizeof(long) * CHAR_BIT)
#endif
