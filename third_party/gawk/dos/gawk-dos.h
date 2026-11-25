/* gawk-dos.h -- included first in every gawk file when building for DOS
   with DJGPP (see gawk-dos.c).  DOS runs one program at a time: gawk's
   pipes ("cmd" | getline, print | "cmd") and system() go through DJGPP's
   popen() and system(), which run the command and pass its output through
   a temporary file.  No SIGCHLD; and DJGPP has no wide characters, so the
   single-byte ones are in gawk-dos.c (a DOS code page: one byte, one
   character). */
#ifndef GAWK_DOS_H
#define GAWK_DOS_H
#define PIPES_SIMULATED 1
#include <stddef.h>
#include <signal.h>
#include <wchar.h>
#include <wctype.h>
#ifndef SIGCHLD
#  define SIGCHLD 305
#endif
size_t mbrtowc (wchar_t *, const char *, size_t, mbstate_t *);
size_t mbrlen (const char *, size_t, mbstate_t *);
size_t wcrtomb (char *, wchar_t, mbstate_t *);
wint_t btowc (int);
int wctob (wint_t);
int mbsinit (const mbstate_t *);
wint_t towlower (wint_t);
wint_t towupper (wint_t);
int iswalnum (wint_t);
int iswupper (wint_t);
int iswlower (wint_t);
int iswspace (wint_t);
int iswprint (wint_t);
wctype_t wctype (const char *);
int iswctype (wint_t, wctype_t);
#endif
