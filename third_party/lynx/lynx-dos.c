/*
 * lynx-dos.c: LYNX.EXE's part of the network glue. The Watt-32 calls
 * Lynx's DJGPP port makes are third_party/openssl/ossl-dos.c's (shared with
 * OPENSSL.EXE: sockets on ssh's dos-sock.c, select_s, getentropy, ...);
 * here is what Lynx wants on top (lynx-dos.h renames Lynx's calls to these):
 *
 *   lynx_connect     connect() as Lynx's non-blocking loop wants it
 *                    (EINPROGRESS, then EALREADY / EISCONN / the error)
 *   lynx_read_s,     a read the user stopped ('z', Ctrl-G: Lynx's
 *   lynx_SSL_read    HTCheckForInterrupt(), asked each clock tick while
 *                    waiting) returns HT_INTERRUPTED, as Lynx's readers want
 *   lynx_dos_mouse_pending   a mouse button is down: Lynx's DOS idle loop
 *                    (HTDOS.c, lynx-dos.patch) waited for the keyboard only
 *   the rest of Watt-32 Lynx calls (_eth_init, tcp_tick, ...): nothing to do
 *   HOME and TEMP when DOS has none; "run STARTNET" as a Lynx alert
 *
 * Compiled against Watt-32's and OpenSSL's headers, without lynx-dos.h.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <tcp.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <dpmi.h>
#include <curses.h>			/* PDCurses: has_mouse() */
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "poll.h"
#include "dos-sock.h"

#define HT_INTERRUPTED (-29998)		/* Lynx's HTUtils.h */
int HTCheckForInterrupt(void);		/* Lynx's LYUtils.c */
void HTAlert(const char *);		/* Lynx's HTAlert.c */
extern int (*ossl_dos_interrupt)(void);	/* ossl-dos.c */

int wathndlcbrk;
char *shell;				/* LYMail.c's DJGPP code: %SHELL% */
int (*_resolve_hook)(void);		/* Lynx sets it; DNS (dos-sock.c) doesn't ask */

static int interrupted;

static int
check_interrupt(void)
{
	if (HTCheckForInterrupt())
		interrupted = 1;
	return interrupted;
}

static void
net_alert(const char *msg)
{
	HTAlert(msg);
}

/*
 * Before Lynx starts: a home (bookmarks, cookies; Lynx stops without one)
 * and a place for temporary files, when DOS's environment has none:
 * C:\HOME (as ssh) and C:\TEMP. Network trouble ("run STARTNET") is an
 * alert, not exit().
 */
static void __attribute__((constructor))
lynx_dos_defaults(void)
{
	if (getenv("HOME") == NULL) {
		mkdir("C:\\HOME", 0755);
		putenv((char *)"HOME=C:\\HOME");
	}
	if (getenv("TMPDIR") == NULL && getenv("TEMP") == NULL &&
	    getenv("TMP") == NULL) {
		mkdir("C:\\TEMP", 0755);
		putenv((char *)"TEMP=C:\\TEMP");
	}
	shell = getenv("SHELL");
	dos_sock_fail = net_alert;
	ossl_dos_interrupt = check_interrupt;
}

/* --- Watt-32's, that Lynx calls: little to do --- */

int _eth_init(void) { return 0; }
void _eth_release(void) { }
void sig_handler_watt(int sig) { (void)sig; }

/* Lynx's idle loop (HTDOS.c) runs Watt-32's stack: vmdos's runs itself */
WORD
tcp_tick(sock_type *s)
{
	(void)s;
	return 1;
}

/* --- the mouse --- */

extern char LYUseMouse;			/* Lynx's BOOLEAN: USE_MOUSE, -use_mouse */

/*
 * For Lynx's idle loop: a button is down (INT 33h AX=3). PDCurses counts
 * the presses and releases (AX=5/6) itself, so it gets the click next.
 */
int
lynx_dos_mouse_pending(void)
{
	static int avail = -1;
	__dpmi_regs r;

	if (!LYUseMouse)
		return 0;
	if (avail < 0)
		avail = has_mouse();	/* after Lynx's mouse_set() */
	if (!avail)
		return 0;
	memset(&r, 0, sizeof r);
	r.x.ax = 3;
	__dpmi_int(0x33, &r);
	return (r.x.bx & 7) != 0;
}

/* --- reads Lynx can stop --- */

int
lynx_read_s(int fd, char *buf, int len)
{
	int r;

	interrupted = 0;
	r = read_s(fd, buf, len);
	if (r < 0 && interrupted)
		return HT_INTERRUPTED;
	return r;
}

int
lynx_SSL_read(SSL *ssl, void *buf, int len)
{
	int r;

	interrupted = 0;
	r = SSL_read(ssl, buf, len);
	if (r <= 0 && interrupted) {
		ERR_clear_error();
		return HT_INTERRUPTED;
	}
	return r;
}

/* --- connect() --- */

static unsigned char started[NSOCK_FDS];	/* connect() called */

int
lynx_close_s(int fd)
{
	if (dos_is_sock(fd))
		started[fd - SOCK_FD0] = 0;
	return close_s(fd);
}

/*
 * The first call starts it (dos-sock.c: EINPROGRESS when non-blocking);
 * Lynx then selects for writing and calls again, wanting EALREADY (not
 * yet), EISCONN (connected) or the error.
 */
int
lynx_connect(int fd, const struct sockaddr *sa, socklen_t len)
{
	int i, p, r;

	if (!dos_is_sock(fd)) {
		errno = ENOTSOCK;
		return -1;
	}
	i = fd - SOCK_FD0;
	if (!started[i]) {
		r = connect(fd, sa, len);
		if (r == 0 || errno == EINPROGRESS)
			started[i] = 1;
		return r;
	}
	p = dos_sock_poll(fd, POLLIN | POLLOUT);
	if (p & POLLERR) {
		errno = ECONNREFUSED;
		return -1;
	}
	if (p & POLLOUT) {
		if ((p & POLLHUP) && !(p & POLLIN)) {
			errno = ECONNRESET;
			return -1;
		}
		errno = EISCONN;
		return -1;
	}
	errno = EALREADY;
	return -1;
}
