/* BSD type names DJGPP's <sys/types.h> doesn't have (forced in by the Makefile). */
#ifndef COMPAT_BSDTYPES_H
#define COMPAT_BSDTYPES_H
#include <stdint.h>
#include <sys/types.h>
typedef uint8_t u_int8_t;
typedef uint16_t u_int16_t;
typedef unsigned int u_int32_t;	/* not uint32_t (unsigned long here): matches the %u formats */
typedef uint64_t u_int64_t;
typedef unsigned int u_int;
typedef unsigned int uint;
typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned long u_long;
#endif
