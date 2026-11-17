/*
 * dos-time.h: forced into every OpenSSL file (and Lynx's, through
 * lynx-dos.h): the clock without DJGPP's time code, which got the date
 * wrong here (certificates made "in 2027", the CAs "expired"), as it did
 * for ssh (third_party/openssh/README.vmdos). ossl-dos.c reads the DOS
 * clock and takes it as UTC, as ssh does; localtime() is gmtime().
 */
#ifndef DOS_TIME_H
#define DOS_TIME_H

#if defined(__DJGPP__) && !defined(__ASSEMBLER__)
#include <time.h>
#include <sys/time.h>

time_t ossl_dos_time(time_t *);
int ossl_dos_gettimeofday(struct timeval *, void *);
struct tm *ossl_dos_gmtime(const time_t *);
struct tm *ossl_dos_gmtime_r(const time_t *, struct tm *);

#define time(p)			ossl_dos_time(p)
#define gettimeofday(t, z)	ossl_dos_gettimeofday(t, z)
#define gmtime(t)		ossl_dos_gmtime(t)
#define gmtime_r(t, r)		ossl_dos_gmtime_r(t, r)
#define localtime(t)		ossl_dos_gmtime(t)
#define localtime_r(t, r)	ossl_dos_gmtime_r(t, r)
#endif

#endif
