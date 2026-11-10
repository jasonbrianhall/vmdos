/* Portable stand-in for FreeBSD's sbin/fsck/fsutil.h (Linux host and DJGPP). */
#ifndef FSUTIL_H
#define FSUTIL_H
#include <stdarg.h>
void pfatal(const char *, ...) __attribute__((format(printf, 1, 2)));
void pwarn(const char *, ...) __attribute__((format(printf, 1, 2)));
void perr(const char *, ...) __attribute__((format(printf, 1, 2)));
void panic(const char *, ...) __attribute__((format(printf, 1, 2), noreturn));
void setcdevname(const char *, int);
const char *cdevname(void);
#endif
