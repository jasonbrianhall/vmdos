/*
 * curl-dos.h: included first into every source of CURL.EXE (curl's and
 * mbedTLS's: tools/build-curl.sh, -include). DJGPP's gettimeofday() and
 * time() go through mktime(), which can loop for ever under vmdos (ssh
 * found it: third_party/openssh/dos-compat.c); these read DOS's date and
 * time instead (curl-dos.c).
 */
#ifndef CURL_DOS_H
#define CURL_DOS_H
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>

int curl_dos_gettimeofday(struct timeval *, struct timezone *);
time_t curl_dos_time(time_t *);
#define gettimeofday(tv, tz) curl_dos_gettimeofday(tv, tz)
#define time(t) curl_dos_time(t)
#endif
