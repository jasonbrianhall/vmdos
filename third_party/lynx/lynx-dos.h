/*
 * lynx-dos.h: forced into every Lynx source file (-include) when building
 * LYNX.EXE for vmdos (tools/build-lynx.sh). Lynx's DJGPP port calls
 * Watt-32 (read_s, write_s, select_s, ioctlsocket, sock_init, ...); those
 * are third_party/openssl/ossl-dos.c's (shared with OPENSSL.EXE), on
 * vmdos's own TCP through ssh's socket layer; a few of them Lynx sees
 * through lynx-dos.c first. Watt-32 gives only the headers.
 */
#ifndef LYNX_DOS_H
#define LYNX_DOS_H

#ifdef __DJGPP__
/* lynx-dos.c: connect() as Lynx's non-blocking loop wants it; reads that
   return HT_INTERRUPTED when the user stops a page */
#define connect lynx_connect
#define read_s lynx_read_s
#define close_s lynx_close_s
#define SSL_read lynx_SSL_read

#include <sys/types.h>
#include <netdb.h>		/* NI_NUMERICHOST, ... (HTFTP.c) */
#include <tcp.h>		/* read_s, write_s, close_s, select_s */

#include "../openssl/dos-time.h"	/* the clock: ossl-dos.c's, not DJGPP's */

/* Lynx's PDCurses mouse code (LYStrings.c) is Windows': its Win9x
   double-click guard off (and a millisecond clock for it), no console
   title to show the click in */
#define system_is_NT	1
#define GetTickCount()	((int)(clock() / (CLOCKS_PER_SEC / 1000)))
#define SetConsoleTitle(s) ((void)(s))

extern int wathndlcbrk;		/* Watt-32's, for LYClean.c: always 0 */
#endif

#endif
