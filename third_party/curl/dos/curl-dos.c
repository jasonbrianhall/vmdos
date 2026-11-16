/*
 * curl-dos.c: what CURL.EXE needs beyond curl, mbedTLS and ssh's socket
 * layer (third_party/openssh/dos-sock.c). curl's DOS code is written for
 * Watt-32 and calls a few of its functions; here they are, on dos-sock.c:
 *   select_s()    curl's select() (curl_setup.h): sockets ask vmdos, the
 *                 console and files are ready
 *   close_s()     closing a socket (CURL_SCLOSE)
 *   ioctlsocket() curl's ioctl() on sockets: FIONBIO (non-blocking)
 * and recv()/send(), which dos-sock.c leaves to ssh's read()/write().
 * The clock reads DOS's date and time (curl-dos.h), as ssh's does.
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <conio.h>
#include <unistd.h>
#include <dos.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <tcp.h>
#include <poll.h>
#include "dos-sock.h"

#undef gettimeofday
#undef time

/* --- the clock --- */

static long long
days_from_civil(int y, unsigned m, unsigned d)
{
	int era;
	unsigned yoe, doy, doe;

	y -= m <= 2;
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (unsigned)(y - era * 400);
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return (long long)era * 146097 + (long long)doe - 719468;
}

int
curl_dos_gettimeofday(struct timeval *tv, struct timezone *tz)
{
	struct dosdate_t d1, d2;
	struct dostime_t t;

	(void)tz;
	do {
		_dos_getdate(&d1);
		_dos_gettime(&t);
		_dos_getdate(&d2);
	} while (d1.day != d2.day);
	if (tv != NULL) {
		tv->tv_sec = (time_t)(days_from_civil(d1.year, d1.month,
		    d1.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second);
		tv->tv_usec = t.hsecond * 10000;
	}
	return 0;
}

time_t
curl_dos_time(time_t *tp)
{
	struct timeval tv;

	curl_dos_gettimeofday(&tv, NULL);
	if (tp != NULL)
		*tp = tv.tv_sec;
	return tv.tv_sec;
}

/* --- Watt-32's calls, on dos-sock.c --- */

int
recv(int s, void *buf, int len, int flags)
{
	(void)flags;
	return (int)dos_sock_read(s, buf, (size_t)len);
}

int
send(int s, const void *buf, int len, int flags)
{
	(void)flags;
	return (int)dos_sock_write(s, buf, (size_t)len);
}

int
close_s(int s)
{
	return dos_sock_close(s);
}

int
ioctlsocket(int s, long cmd, void *argp)
{
	if (!dos_is_sock(s)) {
		errno = ENOTSOCK;
		return -1;
	}
	if (cmd == (long)FIONBIO) {
		dos_sock_setnb(s, argp && *(int *)argp != 0);
		return 0;
	}
	if (cmd == (long)FIONREAD) {	/* "something to read": 1 or 0 */
		if (argp)
			*(int *)argp = (dos_sock_poll(s, POLLIN) & POLLIN) != 0;
		return 0;
	}
	errno = EINVAL;
	return -1;
}

int
select_s(int n, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tmo)
{
	unsigned long start = _farpeekl(_dos_ds, 0x46c);	/* BIOS ticks, 55 ms */
	long ms = tmo ? tmo->tv_sec * 1000L + tmo->tv_usec / 1000 : -1;
	fd_set r, w;
	int fd, k;

	for (;;) {
		FD_ZERO(&r);
		FD_ZERO(&w);
		k = 0;
		for (fd = 0; fd < n; fd++) {
			int want_r = rd && FD_ISSET(fd, rd), want_w = wr && FD_ISSET(fd, wr);

			if (!want_r && !want_w)
				continue;
			if (dos_is_sock(fd)) {
				int ev = dos_sock_poll(fd, (want_r ? POLLIN : 0) | (want_w ? POLLOUT : 0));

				if (want_r && (ev & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
					FD_SET(fd, &r), k++;
				if (want_w && (ev & (POLLOUT | POLLHUP | POLLERR | POLLNVAL)))
					FD_SET(fd, &w), k++;
			} else {	/* the console and files */
				if (want_r && (fd != 0 || !isatty(0) || kbhit()))
					FD_SET(fd, &r), k++;
				if (want_w)
					FD_SET(fd, &w), k++;
			}
		}
		if (k || ms == 0 ||
		    (ms > 0 && (long)(_farpeekl(_dos_ds, 0x46c) - start) * 55 >= ms)) {
			if (rd)
				*rd = r;
			if (wr)
				*wr = w;
			if (ex)
				FD_ZERO(ex);
			return k;
		}
		__dpmi_yield();
	}
}
