/* The few helpers fsck_msdosfs takes from FreeBSD's sbin/fsck/fsutil.c. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "fsutil.h"

static const char *dev = "?";
static int preen;

void setcdevname(const char *cd, int pr) { dev = cd; preen = pr; }
const char *cdevname(void) { return dev; }

static void vmsg(int fatal, const char *fmt, va_list ap)
{
	if (!fatal && preen)
		printf("%s: ", dev);
	vprintf(fmt, ap);
	if (fatal && preen) {
		printf("\n%s: UNEXPECTED INCONSISTENCY; RUN FSCK MANUALLY.\n", dev);
		exit(8);
	}
}

void pfatal(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); vmsg(1, fmt, ap); va_end(ap);
}

void pwarn(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); vmsg(0, fmt, ap); va_end(ap);
}

void perr(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); vmsg(1, fmt, ap); va_end(ap);
	printf("\n");
}

void panic(const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); vmsg(1, fmt, ap); va_end(ap);
	exit(8);
}
