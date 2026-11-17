/*
 * ossl-dos.c: what OpenSSL's DJGPP port wants from Watt-32 and from the OS,
 * on vmdos's own TCP (ssh's dos-sock.c: socket(), connect(), getaddrinfo()
 * through INT 2Fh AX=564Eh; VMTCP.CFG's addresses and DNS). In OPENSSL.EXE
 * and LYNX.EXE (which adds its own on top: third_party/lynx/lynx-dos.c).
 * Compiled against Watt-32's headers so the names match; no Watt-32 code is
 * linked.
 *
 *   getentropy()      OpenSSL's seed (tools/build-openssl.sh points its
 *                     getrandom seeding here): RDSEED/RDRAND when the CPU
 *                     has them, TSC-against-8254 jitter, the clocks
 *   read_s, write_s, close_s, send, recv, select_s, select,
 *   ioctlsocket, sock_init   the sockets (OpenSSL's BIO_s_socket, s_client)
 *
 *   ossl_dos_time, ossl_dos_gettimeofday, ossl_dos_gmtime(_r)
 *                     the clock (dos-time.h renames OpenSSL's and Lynx's
 *                     calls to these): DJGPP's own got the date wrong
 *
 * A wait for data calls ossl_dos_interrupt (when set) each clock tick: if
 * it says so the read fails with EINTR (LYNX: 'z' stops a page).
 * SSL_CERT_FILE defaults to C:\VMDOS\NET\CACERT.PEM, the CAs WGET uses.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netdb.h>
#include <tcp.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dos.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <sys/farptr.h>
#include "poll.h"
#include "dos-co.h"
#include "dos-sock.h"

int (*ossl_dos_interrupt)(void);
int _watt_do_exit;

/* --- for dos-sock.c: no coroutines or alarms here (as WGET) --- */

int co_current, co_ssh_state = CO_NONE, co_ssh_in = -1, co_ssh_out = -1;

void co_yield(void) { }
void dos_alarm_check(void) { }

void *
xcalloc(size_t n, size_t size)
{
	void *p = calloc(n ? n : 1, size ? size : 1);

	if (p == NULL) {
		fputs("out of memory\n", stderr);
		exit(1);
	}
	return p;
}

static void __attribute__((constructor))
ossl_dos_defaults(void)
{
	if (getenv("SSL_CERT_FILE") == NULL)
		putenv((char *)"SSL_CERT_FILE=C:\\VMDOS\\NET\\CACERT.PEM");
}

/* --- the clock: DOS's, taken as UTC (as ssh's dos-compat.c) --- */

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
ossl_dos_gettimeofday(struct timeval *tv, void *tz)
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
ossl_dos_time(time_t *tp)
{
	struct timeval tv;

	ossl_dos_gettimeofday(&tv, NULL);
	if (tp != NULL)
		*tp = tv.tv_sec;
	return tv.tv_sec;
}

struct tm *
ossl_dos_gmtime_r(const time_t *tp, struct tm *tm)
{
	static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243,
	    273, 304, 334 };
	long long t = *tp, days = t / 86400, rem = t % 86400, z, era;
	unsigned doe, yoe, doy, mp;
	int y, leap;

	if (rem < 0) {
		rem += 86400;
		days--;
	}
	memset(tm, 0, sizeof *tm);
	tm->tm_hour = rem / 3600;
	tm->tm_min = (rem % 3600) / 60;
	tm->tm_sec = rem % 60;
	tm->tm_wday = (int)(((days % 7) + 11) % 7);	/* 1970-01-01: Thursday */
	z = days + 719468;
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (unsigned)(z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp = (5 * doy + 2) / 153;
	tm->tm_mday = doy - (153 * mp + 2) / 5 + 1;
	tm->tm_mon = mp < 10 ? mp + 2 : mp - 10;
	tm->tm_year = (int)(yoe + era * 400 + (tm->tm_mon <= 1)) - 1900;
	y = tm->tm_year + 1900;
	leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
	tm->tm_yday = cum[tm->tm_mon] + tm->tm_mday - 1 + (leap && tm->tm_mon > 1);
	return tm;
}

struct tm *
ossl_dos_gmtime(const time_t *tp)
{
	static struct tm tm;

	return ossl_dos_gmtime_r(tp, &tm);
}

/* --- entropy --- */

static uint64_t
tsc(void)
{
	uint32_t lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return (uint64_t)hi << 32 | lo;
}

static void
cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
	__asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
	    : "a"(leaf), "c"(sub));
}

/* 1 RDRAND, 2 RDSEED (and RDRAND), 0 neither */
static int
hw_rng(void)
{
	static int have = -1;
	uint32_t r[4];

	if (have >= 0)
		return have;
	have = 0;
	cpuid(0, 0, r);
	if (r[0] >= 7) {
		uint32_t r7[4];

		cpuid(7, 0, r7);
		if ((r7[1] >> 18) & 1)
			have = 2;
	}
	cpuid(1, 0, r);
	if (have == 0 && ((r[2] >> 30) & 1))
		have = 1;
	return have;
}

static int
hw_word(uint32_t *v)
{
	unsigned char ok = 0;
	int t;

	for (t = 0; t < 10 && !ok; t++) {
		if (hw_rng() == 2)
			__asm__ volatile("rdseed %0; setc %1" : "=r"(*v), "=qm"(ok));
		else
			__asm__ volatile("rdrand %0; setc %1" : "=r"(*v), "=qm"(ok));
	}
	return ok;
}

/* The 8254's counter 0 against the TSC: how long a few reads take varies */
static uint32_t
jitter(void)
{
	uint32_t j = 0;
	int k;

	for (k = 0; k < 16; k++) {
		uint64_t t0 = tsc();
		unsigned char lo, hi;

		outportb(0x43, 0x00);
		lo = inportb(0x40);
		hi = inportb(0x40);
		j = (j << 5 | j >> 27) ^ (uint32_t)(tsc() - t0) ^ (hi << 8 | lo);
	}
	return j;
}

/* Any length (OpenSSL asks for its whole pool at once, not POSIX's 256) */
int
getentropy(void *buf, size_t len)
{
	unsigned char *p = buf;
	size_t i;

	for (i = 0; i < len; i++) {
		uint32_t v = 0;

		if (hw_rng())
			hw_word(&v);
		v ^= jitter();
		v ^= (uint32_t)ossl_dos_time(NULL) ^ (uint32_t)clock() ^ (uint32_t)tsc();
		p[i] = (unsigned char)(v ^ v >> 8 ^ v >> 16 ^ v >> 24);
	}
	return 0;
}

/* --- Watt-32's start and debugging: nothing to do --- */

static void
net_fail(const char *msg)
{
	fprintf(stderr, "%s\n", msg);
	exit(1);
}

int
watt_sock_init(size_t tcp_size, size_t udp_size, size_t time_size)
{
	(void)tcp_size; (void)udp_size; (void)time_size;
	if (dos_sock_fail == NULL)
		dos_sock_fail = net_fail;	/* not "ssh: ..." */
	return 0;
}

void dbug_init(void) { }

/* --- sockets --- */

static unsigned long
ticks(void)
{
	return _farpeekl(_dos_ds, 0x46c);
}

int
read_s(int fd, char *buf, int len)
{
	unsigned long last;

	if (!dos_is_sock(fd))
		return read(fd, buf, len);
	if (dos_sock_setnb(fd, -1) == 0) {
		last = ticks();
		while (!(dos_sock_poll(fd, POLLIN) &
		    (POLLIN | POLLHUP | POLLERR | POLLNVAL))) {
			if (ticks() != last) {
				last = ticks();
				if (ossl_dos_interrupt && ossl_dos_interrupt()) {
					errno = EINTR;
					return -1;
				}
			}
			__dpmi_yield();
		}
	}
	return dos_sock_read(fd, buf, len);
}

int
write_s(int fd, const char *buf, int len)
{
	if (!dos_is_sock(fd))
		return write(fd, buf, len);
	return dos_sock_write(fd, buf, len);
}

int
close_s(int fd)
{
	if (!dos_is_sock(fd))
		return close(fd);
	return dos_sock_close(fd);
}

int
send(int fd, const void *buf, int len, int flags)
{
	(void)flags;
	return write_s(fd, buf, len);
}

int
recv(int fd, void *buf, int len, int flags)
{
	(void)flags;
	return read_s(fd, buf, len);
}

int
ioctlsocket(int fd, long cmd, void *arg)
{
	if (!dos_is_sock(fd)) {
		errno = ENOTSOCK;
		return -1;
	}
	if (cmd == (long)FIONBIO) {
		dos_sock_setnb(fd, *(int *)arg != 0);
		return 0;
	}
	if (cmd == (long)FIONREAD) {
		*(int *)arg = (dos_sock_poll(fd, POLLIN) & POLLIN) ? 1 : 0;
		return 0;
	}
	errno = EINVAL;
	return -1;
}

/*
 * select(): sockets as dos-sock.c's poll says; the console (stdin) when a
 * key is waiting; other descriptors (files) are always ready. The timeout
 * in BIOS ticks, rounded up.
 */
static int
fd_ready(int fd)
{
	if (fd == 0 && isatty(0))
		return kbhit();
	return 1;
}

int
select_s(int n, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
	fd_set r_out, w_out;
	unsigned long t0 = ticks(), lim = 0;
	int fd, cnt;

	if (tv != NULL)
		lim = ((unsigned long)tv->tv_sec * 1000000UL + tv->tv_usec) /
		    54925UL + (tv->tv_sec || tv->tv_usec ? 1 : 0);
	for (;;) {
		FD_ZERO(&r_out);
		FD_ZERO(&w_out);
		cnt = 0;
		for (fd = 0; fd < n; fd++) {
			int sock = dos_is_sock(fd);

			if (rd && FD_ISSET(fd, rd) && (sock ?
			    (dos_sock_poll(fd, POLLIN) & (POLLIN | POLLHUP | POLLERR)) != 0 :
			    fd_ready(fd))) {
				FD_SET(fd, &r_out);
				cnt++;
			}
			if (wr && FD_ISSET(fd, wr) && (!sock ||
			    (dos_sock_poll(fd, POLLOUT) & (POLLOUT | POLLHUP | POLLERR)))) {
				FD_SET(fd, &w_out);
				cnt++;
			}
		}
		if (cnt || (tv != NULL && ticks() - t0 >= lim))
			break;
		__dpmi_yield();
	}
	if (rd)
		*rd = r_out;
	if (wr)
		*wr = w_out;
	if (ex)
		FD_ZERO(ex);
	return cnt;
}

/* select() too (s_client): DJGPP's own knows only DOS's file handles */
int
select(int n, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
	return select_s(n, rd, wr, ex, tv);
}

/* No UDP sockets on vmdos's TCP (DTLS, ...): say so */
int
sendto(int fd, const void *buf, int len, int flags, const struct sockaddr *to,
    socklen_t tolen)
{
	(void)fd; (void)buf; (void)len; (void)flags; (void)to; (void)tolen;
	errno = EOPNOTSUPP;
	return -1;
}

int
recvfrom(int fd, void *buf, int len, int flags, struct sockaddr *from,
    socklen_t *fromlen)
{
	(void)fd; (void)buf; (void)len; (void)flags; (void)from; (void)fromlen;
	errno = EOPNOTSUPP;
	return -1;
}

/* --- gethostbyname() for what doesn't use getaddrinfo(): IPv4 only --- */

int h_errno;

struct hostent *
gethostbyname(const char *name)
{
	static struct hostent he;
	static struct in_addr a;
	static char *addrs[2], hname[256];
	struct addrinfo hints, *ai;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(name, NULL, &hints, &ai) != 0 || ai == NULL) {
		h_errno = HOST_NOT_FOUND;
		return NULL;
	}
	a = ((struct sockaddr_in *)ai->ai_addr)->sin_addr;
	freeaddrinfo(ai);
	snprintf(hname, sizeof hname, "%s", name);
	addrs[0] = (char *)&a;
	addrs[1] = NULL;
	he.h_name = hname;
	he.h_aliases = addrs + 1;
	he.h_addrtype = AF_INET;
	he.h_length = sizeof a;
	he.h_addr_list = addrs;
	return &he;
}
