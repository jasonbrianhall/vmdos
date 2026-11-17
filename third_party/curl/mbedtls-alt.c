/*
 * mbedtls-alt.c: what mbedTLS asks of the platform for CURL.EXE (as for
 * WGET, third_party/wget/wget_mbedtls_config.h, the same configuration):
 * entropy (RDRAND where the CPU has it, the TSC's jitter, the clock) and
 * milliseconds (DJGPP has no clock_gettime()).
 */
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include "mbedtls/platform_time.h"

static uint64_t
tsc(void)
{
	uint32_t lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return (uint64_t)hi << 32 | lo;
}

static int
have_rdrand(void)
{
	uint32_t a = 1, b, c = 0, d;

	__asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
	return (c >> 30) & 1;
}

int mbedtls_hardware_poll(void *, unsigned char *, size_t, size_t *);

int
mbedtls_hardware_poll(void *data, unsigned char *out, size_t len,
    size_t *olen)
{
	static int rd = -1;
	size_t i;

	(void)data;
	if (rd < 0)
		rd = have_rdrand();
	for (i = 0; i < len; i++) {
		uint32_t v = 0, j = 0;
		unsigned char ok = 0;
		int k, t;

		for (t = 0; rd && t < 10 && !ok; t++)
			__asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
		for (k = 0; k < 8; k++) {	/* a few noisy bits from timing */
			uint64_t t0 = tsc();
			volatile int spin;
			for (spin = 0; spin < 50; spin++)
				;
			j = j * 31 + (uint32_t)(tsc() - t0);
		}
		out[i] = (unsigned char)(v ^ j ^ (j >> 8) ^ (j >> 16) ^
		    (uint32_t)time(NULL) ^ (uint32_t)clock());
	}
	*olen = len;
	return 0;
}

mbedtls_ms_time_t
mbedtls_ms_time(void)
{
#ifdef __DJGPP__
	return (mbedtls_ms_time_t)time(NULL) * 1000;
#else
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (mbedtls_ms_time_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}
