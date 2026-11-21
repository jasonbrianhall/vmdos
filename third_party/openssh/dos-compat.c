/*
 * dos-compat.c: what OpenSSH's ssh needs that DJGPP lacks, for
 * vmdos (https://github.com/jasonbrianhall/vmdos).
 *
 *  - getentropy(): DOS has no /dev/urandom. Gathered from RDSEED/RDRAND
 *    when the CPU has them, RDTSC jitter against the 8254 timer, the
 *    BIOS tick count and clock, and a seed file kept between runs; all
 *    condensed with SHA-512. arc4random() (openbsd-compat) keys ChaCha20
 *    from it.
 *  - poll()/ppoll() for sockets, the console and files together.
 */
#include "includes.h"

#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dos.h>
#include <pc.h>
#include <go32.h>
#include <sys/farptr.h>
#include <dpmi.h>
#include <io.h>
#include <sys/exceptn.h>
#include <termios.h>
#include <conio.h>
#include <stdarg.h>
#include <sys/fsext.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/stat.h>
#include "xmalloc.h"
#include "log.h"
#include "dos-sock.h"



static inline unsigned long long rdtsc(void)
{
	unsigned int lo, hi;
	__asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
	return ((unsigned long long)hi << 32) | lo;
}

static void cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
	__asm__ __volatile__("cpuid"
	    : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
	    : "a"(leaf), "c"(sub));
}

/* RDSEED (leaf 7 EBX bit 18) or RDRAND (leaf 1 ECX bit 30): 32 bits, or 0. */
static int hw_random(int seed, unsigned *out)
{
	unsigned char ok;
	int i;

	for (i = 0; i < 32; i++) {
		if (seed)
			__asm__ __volatile__("rdseed %0; setc %1"
			    : "=r"(*out), "=qm"(ok));
		else
			__asm__ __volatile__("rdrand %0; setc %1"
			    : "=r"(*out), "=qm"(ok));
		if (ok)
			return 1;
	}
	return 0;
}

/* The 8254's channel 0 count: changes about every 838 ns. */
static unsigned pit_count(void)
{
	unsigned lo, hi;

	outportb(0x43, 0x00);
	lo = inportb(0x40);
	hi = inportb(0x40);
	return lo | (hi << 8);
}

#define SEED_NAME "SSHSEED.BIN"

/* The seed file: next to HOME's .ssh, else the program's directory. */
static void seed_path(char *buf, size_t len)
{
	const char *home = getenv("HOME");

	if (home != NULL && *home != '\0')
		snprintf(buf, len, "%s/.ssh/" SEED_NAME, home);
	else
		snprintf(buf, len, "C:/" SEED_NAME);
}

static int pool_ready;
static SHA2_CTX pool;	/* SHA-512 over everything gathered so far */

static void gather(void)
{
	unsigned char buf[64];
	unsigned r[4], v, have_rdrand, have_rdseed;
	unsigned long long t, prev;
	char path[512];
	int fd, i, n;
	time_t now;

	SHA512Init(&pool);

	/* Hardware random numbers. */
	cpuid(0, 0, r);
	have_rdrand = have_rdseed = 0;
	if (r[0] >= 1) {
		cpuid(1, 0, r);
		have_rdrand = (r[2] >> 30) & 1;
	}
	cpuid(0, 0, r);
	if (r[0] >= 7) {
		cpuid(7, 0, r);
		have_rdseed = (r[1] >> 18) & 1;
	}
	for (i = 0, n = 0; i < 64; i++) {
		if ((have_rdseed && hw_random(1, &v)) ||
		    (have_rdrand && hw_random(0, &v))) {
			SHA512Update(&pool, (u_char *)&v, sizeof v);
			n++;
		}
	}

	/* What was saved last time. */
	seed_path(path, sizeof path);
	if ((fd = open(path, O_RDONLY | O_BINARY)) != -1) {
		while ((i = read(fd, buf, sizeof buf)) > 0)
			SHA512Update(&pool, buf, i);
		close(fd);
	}

	/* Clocks: wall time, BIOS ticks, PID, the address of the stack. */
	now = time(NULL);
	SHA512Update(&pool, (u_char *)&now, sizeof now);
	v = _farpeekl(_dos_ds, 0x46c);
	SHA512Update(&pool, (u_char *)&v, sizeof v);
	v = getpid();
	SHA512Update(&pool, (u_char *)&v, sizeof v);
	v = (unsigned)(uintptr_t)&v;
	SHA512Update(&pool, (u_char *)&v, sizeof v);

	/*
	 * Timing jitter: how many TSC cycles each 8254 step takes. Without
	 * RDRAND this is most of it, so take plenty (about 0.2 s).
	 */
	n = n >= 32 ? 2048 : 1 << 18;
	prev = rdtsc();
	for (i = 0; i < n; i++) {
		unsigned c = pit_count();
		while (pit_count() == c)
			;
		t = rdtsc();
		v = (unsigned)(t - prev) ^ (c << 16);
		prev = t;
		SHA512Update(&pool, (u_char *)&v, sizeof v);
	}
	pool_ready = 1;
}

/* Keep a new seed for next time, drawn from (not revealing) the pool. */
static void save_seed(void)
{
	SHA2_CTX c;
	u_char out[SHA512_DIGEST_LENGTH];
	char path[512];
	int fd;

	c = pool;
	SHA512Update(&c, (const u_char *)"seed", 4);
	SHA512Final(out, &c);
	seed_path(path, sizeof path);
	if ((fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
	    0600)) != -1) {
		write(fd, out, sizeof out);
		close(fd);
	}
	explicit_bzero(out, sizeof out);
}

int
getentropy(void *s, size_t len)
{
	static unsigned counter;
	u_char out[SHA512_DIGEST_LENGTH];
	u_char *p = s;
	SHA2_CTX c;
	size_t n;

	if (len > 256) {
		errno = EIO;
		return -1;
	}
	if (!pool_ready) {
		gather();
		save_seed();
	}
	while (len > 0) {
		unsigned long long t = rdtsc();

		/* Fresh timing each call, then a block out of the pool. */
		SHA512Update(&pool, (u_char *)&t, sizeof t);
		c = pool;
		counter++;
		SHA512Update(&c, (u_char *)&counter, sizeof counter);
		SHA512Final(out, &c);
		n = len < sizeof out ? len : sizeof out;
		memcpy(p, out, n);
		p += n;
		len -= n;
	}
	explicit_bzero(out, sizeof out);
	return 0;
}

/*
 * poll() and ppoll() for sockets, pipes, the console and files together.
 * DJGPP's select() can't be used: it reads the clock through
 * gettimeofday() and mktime(), which can spin for ever in libc's timesub()
 * (see dos_gettimeofday below). Sockets ask vmdos (dos-sock.c), pipes are
 * in memory (dos-co.c), the console is dos-term.c's, files are always
 * ready. dos-compat.h renames poll to dos_poll.
 */
#undef poll

int
dos_poll(struct pollfd *fds, int nfds, int timeout_ms)
{
	unsigned long start = _farpeekl(_dos_ds, 0x46c);
	int i, n, k;

	for (;;) {
		n = 0;
		for (i = 0; i < nfds; i++) {
			int fd = fds[i].fd, ev = fds[i].events;

			fds[i].revents = 0;
			if (fd < 0)
				continue;
			if (dos_is_pipe(fd)) {
				if ((ev & (POLLIN | POLLRDNORM)) &&
				    dos_pipe_ready(fd, 0))
					fds[i].revents |= ev & (POLLIN | POLLRDNORM);
				if ((ev & (POLLOUT | POLLWRNORM)) &&
				    dos_pipe_ready(fd, 1))
					fds[i].revents |= ev & (POLLOUT | POLLWRNORM);
				continue;
			}
			if (dos_is_sock(fd)) {
				int rv = dos_sock_poll(fd, ev & (POLLIN | POLLOUT));

				fds[i].revents = (rv & (POLLHUP | POLLERR |
				    POLLNVAL)) | (rv & ev);
				continue;
			}
			if (ev & (POLLOUT | POLLWRNORM))
				fds[i].revents |= ev & (POLLOUT | POLLWRNORM);
			if (ev & (POLLIN | POLLRDNORM)) {
				k = dos_term_readable(fd);
				if (k == -1)	/* not raw console: file or cooked */
					k = !isatty(fd) || kbhit();
				if (k)
					fds[i].revents |= ev & (POLLIN | POLLRDNORM);
			}
		}
		for (i = 0; i < nfds; i++)
			if (fds[i].revents)
				n++;
		if (n > 0 || timeout_ms == 0)
			return n;
		if (timeout_ms > 0 &&
		    (_farpeekl(_dos_ds, 0x46c) - start) * 55 >= (unsigned)timeout_ms)
			return 0;
		if (co_ssh_state == CO_RUNNING)
			co_yield();	/* the other side of sftp/scp */
		else {
			dos_alarm_check();
			__dpmi_yield();
		}
	}
}

/*
 * gettimeofday() and time() from DOS's date and time, without DJGPP's
 * mktime(), which can loop for ever in timesub() (seen under vmdos: ssh
 * hung after login, inside select() -> gettimeofday() -> mktime()). The
 * DOS clock is taken as UTC unless TZ says otherwise... it isn't consulted:
 * ssh only needs a clock that moves forward.
 */
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

#undef gettimeofday
#undef time
int
dos_gettimeofday(struct timeval *tv, struct timezone *tz)
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
dos_time(time_t *tp)
{
	struct timeval tv;

	dos_gettimeofday(&tv, NULL);
	if (tp != NULL)
		*tp = tv.tv_sec;
	return tv.tv_sec;
}

int
dos_ppoll(struct pollfd *fds, unsigned int nfds, const struct timespec *tmo,
    const sigset_t *sigmask)
{
	int ms = -1;

	(void)sigmask;
	if (tmo != NULL) {
		long long t = (long long)tmo->tv_sec * 1000 +
		    (tmo->tv_nsec + 999999) / 1000000;
		ms = t > 0x7fffffff ? 0x7fffffff : (int)t;
	}
	return dos_poll(fds, (int)nfds, ms);
}

/* Single-user DOS: no other users to become, no groups, no socketpair. */
int seteuid(uid_t uid) { (void)uid; return 0; }
int setegid(gid_t gid) { (void)gid; return 0; }
int initgroups(const char *user, gid_t gid) { (void)user; (void)gid; return 0; }

int
socketpair(int d, int type, int protocol, int sv[2])
{
	(void)d; (void)type; (void)protocol; (void)sv;
	errno = ENOSYS;
	return -1;
}

/*
 * Ctrl-C and Ctrl-Break: DJGPP turns them into SIGINT. While ssh has the
 * console in raw mode (ISIG off) they belong to the remote side, so pass
 * them through as characters; dos-compat.h sends tcsetattr here.
 */
#undef tcsetattr
static int
is_console(int fd)
{
	int info = _get_dev_info(fd);	/* (not isatty: fd 0 has our FSEXT hook) */

	return fd >= 0 && !dos_is_pipe(fd) && !dos_is_sock(fd) && info != -1 &&
	    (info & 0x80) && (info & 0x03);
}

int
dos_tcsetattr(int fd, int act, const struct termios *t)
{
	int r = tcsetattr(fd, act, t);

	if (is_console(fd)) {
		/* Ctrl-C a character in raw mode (ISIG off), SIGINT otherwise */
		__djgpp_set_ctrl_c((t->c_lflag & ISIG) ? 1 : 0);
		dos_term_mode(t);
		r = 0;		/* dos-term.c does the console's modes */
	}
	return r;
}

void dos_term_hook_stdin(void);

/*
 * Ctrl-C is the program's, not DOS's: DOS's BREAK off, and its INT 23h
 * (which ends the program when DOS sees ^C during one of its calls)
 * pointed at an IRET, both put back at exit. (Watt-32 used to do this.)
 * In raw mode Ctrl-C goes to the remote side; at prompts dos-term.c raises
 * SIGINT for it.
 */
static int old_break = -1;
static __dpmi_raddr old_int23;
static int iret_sel;

static void
ctrl_c_restore(void)
{
	if (iret_sel) {
		__dpmi_set_real_mode_interrupt_vector(0x23, &old_int23);
		__dpmi_free_dos_memory(iret_sel);
		iret_sel = 0;
	}
	if (old_break >= 0)
		setcbrk(old_break);
}

static void
ctrl_c_ours(void)
{
	__dpmi_raddr iret;
	int seg;
	unsigned char op = 0xcf;	/* IRET */

	old_break = getcbrk();
	setcbrk(0);
	if ((seg = __dpmi_allocate_dos_memory(1, &iret_sel)) != -1) {
		dosmemput(&op, 1, seg * 16);
		__dpmi_get_real_mode_interrupt_vector(0x23, &old_int23);
		iret.segment = seg;
		iret.offset16 = 0;
		__dpmi_set_real_mode_interrupt_vector(0x23, &iret);
	} else
		iret_sel = 0;
	atexit(ctrl_c_restore);
}

__attribute__((constructor)) static void
dos_init(void)
{
	/* Bytes to and from the remote side as they are. */
	/* Files as they are: no CR/LF translation, no stop at Ctrl-Z
	   (sftp and scp copy files byte for byte). */
	_fmode = O_BINARY;
	dos_term_hook_stdin();	/* line editing for libc's console reads */
	ctrl_c_ours();
	setmode(STDOUT_FILENO, O_BINARY);
	/* ~/.ssh (known_hosts, keys): C:\HOME unless HOME says otherwise. */
	if (getenv("HOME") == NULL) {
		mkdir("C:/HOME", 0755);
		setenv("HOME", "C:/HOME", 1);
	}
	/* Without TERM the remote side gets none: dos-term.c is a Linux console. */
	if (getenv("TERM") == NULL)
		setenv("TERM", "linux", 1);
}

/*
 * fcntl() on sockets and pipes: F_GETFL/F_SETFL as ssh uses them
 * (O_NONBLOCK); files and the console to DJGPP's fcntl. dos-compat.h
 * renames fcntl to dos_fcntl.
 */
#undef fcntl

int
dos_fcntl(int fd, int cmd, ...)
{
	va_list ap;
	long arg;

	va_start(ap, cmd);
	arg = va_arg(ap, long);
	va_end(ap);
	if (dos_is_pipe(fd)) {
		switch (cmd) {
		case F_GETFL:
			return (dos_pipe_wr(fd) ? O_WRONLY : O_RDONLY) |
			    (dos_pipe_setnb(fd, -1) ? O_NONBLOCK : 0);
		case F_SETFL:
			dos_pipe_setnb(fd, (arg & O_NONBLOCK) != 0);
			return 0;
		default:
			return 0;
		}
	}
	if (dos_is_sock(fd)) {
		switch (cmd) {
		case F_GETFL:
			return O_RDWR | (dos_sock_setnb(fd, -1) ? O_NONBLOCK : 0);
		case F_SETFL:
			dos_sock_setnb(fd, (arg & O_NONBLOCK) != 0);
			return 0;
		default:
			return 0;
		}
	}
	return fcntl(fd, cmd, arg);
}

/* strerror() for the network errnos too (DJGPP's stops at EILSEQ; the
   numbers are Watt-32's headers'). */
#undef strerror
char *
dos_strerror(int e)
{
	static const struct { int e; const char *s; } net[] = {
		{ EWOULDBLOCK, "Resource temporarily unavailable" },
		{ EINPROGRESS, "Operation now in progress" },
		{ EALREADY, "Operation already in progress" },
		{ ENOTSOCK, "Socket operation on non-socket" },
		{ EDESTADDRREQ, "Destination address required" },
		{ EMSGSIZE, "Message too long" },
		{ EPROTOTYPE, "Protocol wrong type for socket" },
		{ ENOPROTOOPT, "Protocol not available" },
		{ EPROTONOSUPPORT, "Protocol not supported" },
		{ ESOCKTNOSUPPORT, "Socket type not supported" },
		{ EOPNOTSUPP, "Operation not supported" },
		{ EPFNOSUPPORT, "Protocol family not supported" },
		{ EAFNOSUPPORT, "Address family not supported by protocol" },
		{ EADDRINUSE, "Address already in use" },
		{ EADDRNOTAVAIL, "Cannot assign requested address" },
		{ ENETDOWN, "Network is down" },
		{ ENETUNREACH, "Network is unreachable" },
		{ ENETRESET, "Network dropped connection on reset" },
		{ ECONNABORTED, "Software caused connection abort" },
		{ ECONNRESET, "Connection reset by peer" },
		{ ENOBUFS, "No buffer space available" },
		{ EISCONN, "Transport endpoint is already connected" },
		{ ENOTCONN, "Transport endpoint is not connected" },
		{ ESHUTDOWN, "Cannot send after transport endpoint shutdown" },
		{ ETIMEDOUT, "Connection timed out" },
		{ ECONNREFUSED, "Connection refused" },
		{ EHOSTDOWN, "Host is down" },
		{ EHOSTUNREACH, "No route to host" },
	};
	size_t i;

	for (i = 0; i < sizeof net / sizeof net[0]; i++)
		if (net[i].e == e)
			return (char *)net[i].s;
	return strerror(e);
}

/* For the rest of libc too: these replace DJGPP's at link time. */
int
gettimeofday(struct timeval *tv, struct timezone *tz)
{
	return dos_gettimeofday(tv, tz);
}

time_t
(time)(time_t *tp)
{
	return dos_time(tp);
}

/*
 * The remote user name when none was given (user@, -l, User): DOS has no
 * login name to use, so USER (or LOGNAME) if set, else ask, as PuTTY does.
 */
char *
dos_ask_user(const char *def)
{
	const char *u = getenv("USER");
	char buf[256];

	(void)def;
	if (u == NULL || *u == '\0')
		u = getenv("LOGNAME");
	if (u != NULL && *u != '\0')
		return xstrdup(u);
	if (readpassphrase("login as: ", buf, sizeof buf, RPP_ECHO_ON) == NULL ||
	    buf[0] == '\0')
		fatal("No user name given (user@host, -l, or SET USER=)");
	return xstrdup(buf);
}


/* Pipes and sockets aren't terminals (DOS calls both character devices). */
#undef isatty
int
dos_isatty(int fd)
{
	if (dos_is_pipe(fd) || dos_is_sock(fd))
		return 0;
	if (is_console(fd))
		return 1;
	return isatty(fd);
}

/* Nothing to close: DOS programs don't hand handles on (and ssh inside
   sftp/scp mustn't close their files and pipes). */
void
closefrom(int lowfd)
{
	(void)lowfd;
}

/* readv/writev through dos_read/dos_write (pipes, sockets, the console). */
#undef readv
#undef writev
ssize_t
dos_writev(int fd, const struct iovec *iov, int n)
{
	ssize_t total = 0, r;
	int i;

	for (i = 0; i < n; i++) {
		if (iov[i].iov_len == 0)
			continue;
		r = dos_write(fd, iov[i].iov_base, iov[i].iov_len);
		if (r < 0)
			return total > 0 ? total : -1;
		total += r;
		if ((size_t)r < iov[i].iov_len)
			break;
	}
	return total;
}

ssize_t
dos_readv(int fd, const struct iovec *iov, int n)
{
	ssize_t total = 0, r;
	int i;

	for (i = 0; i < n; i++) {
		if (iov[i].iov_len == 0)
			continue;
		r = dos_read(fd, iov[i].iov_base, iov[i].iov_len);
		if (r < 0)
			return total > 0 ? total : -1;
		total += r;
		if ((size_t)r < iov[i].iov_len)
			break;
	}
	return total;
}

/* stat() and friends with POSIX file type bits (dos-compat.h). */
#undef stat
#undef lstat
#undef fstat
#define DJ_IFMT  0xf000		/* DJGPP's values */
#define DJ_IFREG 0x0000
#define DJ_IFBLK 0x1000
#define DJ_IFCHR 0x2000
#define DJ_IFDIR 0x3000
#define DJ_IFIFO 0x4000
#define DJ_IFLNK 0x8000

static void
posix_mode(struct stat *st)
{
	unsigned m = st->st_mode, t;

	switch (m & DJ_IFMT) {
	case DJ_IFDIR: t = S_IFDIR; break;
	case DJ_IFCHR: t = S_IFCHR; break;
	case DJ_IFBLK: t = S_IFBLK; break;
	case DJ_IFIFO: t = S_IFIFO; break;
	case DJ_IFLNK: t = S_IFLNK; break;
	default:       t = S_IFREG; break;
	}
	st->st_mode = t | (m & 07777);
}

int
dos_stat(const char *p, struct stat *st)
{
	int r = stat(p, st);

	if (r == 0)
		posix_mode(st);
	return r;
}

int
dos_lstat(const char *p, struct stat *st)
{
	int r = lstat(p, st);

	if (r == 0)
		posix_mode(st);
	return r;
}

int
dos_fstat(int fd, struct stat *st)
{
	int r;

	if (dos_is_pipe(fd) || dos_is_sock(fd)) {
		memset(st, 0, sizeof *st);
		st->st_mode = (dos_is_sock(fd) ? S_IFSOCK : S_IFIFO) | 0600;
		return 0;
	}
	if ((r = fstat(fd, st)) == 0)
		posix_mode(st);
	return r;
}

/*
 * gmtime() (and localtime(): the DOS clock is taken as UTC, as in
 * dos_gettimeofday) by plain arithmetic: DJGPP's tz code got dates wrong.
 */
struct tm *
dos_gmtime_r(const time_t *tp, struct tm *tm)
{
	long long t = *tp, days = t / 86400, rem = t % 86400, z, era;
	unsigned doe, yoe, doy, mp;

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
	{
		static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181,
		    212, 243, 273, 304, 334 };
		int y = tm->tm_year + 1900, leap = (y % 4 == 0 &&
		    (y % 100 != 0 || y % 400 == 0));

		tm->tm_yday = cum[tm->tm_mon] + tm->tm_mday - 1 +
		    (leap && tm->tm_mon > 1);
	}
	return tm;
}

struct tm *
dos_gmtime(const time_t *tp)
{
	static struct tm tm;

	return dos_gmtime_r(tp, &tm);
}

/*
 * basename() as POSIX (and OpenBSD, whose sftp and scp expect it) has it:
 * trailing separators don't count ("dir/" is "dir", "/" is "/", "" is ".").
 * / and \ both separate, and a drive ("C:") is skipped, as in DJGPP's own:
 * scp's local paths are C:\DIR\FILE.
 */
#undef basename
char *
dos_basename(const char *path)
{
	static char bname[FILENAME_MAX + 1];
	const char *start, *end;
	size_t len;

#define IS_SEP(c) ((c) == '/' || (c) == '\\')
	if (path == NULL || *path == '\0')
		return strcpy(bname, ".");
	if (((path[0] >= 'A' && path[0] <= 'Z') ||
	    (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':')
		path += 2;
	if (*path == '\0')
		return strcpy(bname, ".");
	end = path + strlen(path) - 1;
	while (end > path && IS_SEP(*end))
		end--;
	if (end == path && IS_SEP(*end))
		return strcpy(bname, "/");
	start = end;
	while (start > path && !IS_SEP(start[-1]))
		start--;
	len = end - start + 1;
	if (len >= sizeof bname) {
		errno = ENAMETOOLONG;
		return NULL;
	}
	memcpy(bname, start, len);
	bname[len] = '\0';
	return bname;
#undef IS_SEP
}
