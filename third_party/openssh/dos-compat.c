/*
 * dos-compat.c: what OpenSSH's ssh needs that DJGPP + Watt-32 lack, for
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
#include <tcp.h>
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

static unsigned char sock_nb[256];	/* sockets set non-blocking (dos_fcntl) */


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
 * poll() and ppoll() for sockets, the console and files together.
 * Watt-32's own poll() only knows its sockets, and DJGPP's select() can't
 * be used: it reads the clock through gettimeofday() and mktime(), which
 * can spin for ever in libc's timesub() (see dos_gettimeofday below). So:
 * sockets through Watt-32's poll() (it runs tcp_tick), the console through
 * dos-term.c, and files are always ready. dos-compat.h renames poll to
 * dos_poll.
 */
#undef poll
int poll(struct pollfd *, int, int);	/* Watt-32's */

static int
is_socket(int fd)
{
	return fd >= 0 && __FSEXT_get_function(fd) != NULL;
}

int
dos_poll(struct pollfd *fds, int nfds, int timeout_ms)
{
	struct pollfd sk[64];
	int map[64];
	unsigned long start = _farpeekl(_dos_ds, 0x46c);
	int i, n, nsk, k;

	for (;;) {
		n = 0;
		nsk = 0;
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
			if (is_socket(fd)) {
				if (nsk < 64) {
					sk[nsk] = fds[i];
					sk[nsk].events &= POLLIN | POLLOUT | POLLPRI;
					map[nsk++] = i;
				}
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
		if (nsk > 0 && poll(sk, nsk, 0) > 0) {
			for (k = 0; k < nsk; k++)
				fds[map[k]].revents |= sk[k].revents;
		} else if (nsk == 0)
			tcp_tick(NULL);
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
int
dos_tcsetattr(int fd, int act, const struct termios *t)
{
	int r = tcsetattr(fd, act, t);

	if (r == 0 && isatty(fd) && __FSEXT_get_function(fd) == NULL) {
		__djgpp_set_ctrl_c((t->c_lflag & ISIG) ? 1 : 0);
		dos_term_mode(t);
	}
	return r;
}

/*
 * Watt-32's settings from vmdos's TCP/IP settings (mTCP's config file,
 * %MTCPCFG%, which DHCP or VMTCP fills in), unless there is a WATTCP.CFG:
 * the same address, netmask, gateway and DNS servers, set statically, so
 * ssh never asks DHCP itself (vmdos keeps the lease renewed).
 */
#define M_MAXNS 4
static char m_ip[64], m_mask[64], m_gw[64], m_vec[16], m_host[64];
static char m_ns[M_MAXNS][64];

static int m_nns;

static long
mtcp_config(int pass, const struct config_table *cfg)
{
	int i;

	if (pass == 1) {
		if (*m_vec)
			tcp_inject_config(cfg, "PKT.VECTOR", m_vec);
		return 1;
	}
	tcp_inject_config(cfg, "MY_IP", m_ip);
	if (*m_mask)
		tcp_inject_config(cfg, "NETMASK", m_mask);
	if (*m_gw)
		tcp_inject_config(cfg, "GATEWAY", m_gw);
	for (i = 0; i < m_nns; i++)
		tcp_inject_config(cfg, "NAMESERVER", m_ns[i]);
	if (*m_host)
		tcp_inject_config(cfg, "HOSTNAME", m_host);
	if (getenv("WATTDBG") != NULL) {	/* Watt-32's packet trace */
		tcp_inject_config(cfg, "DEBUG.FILE", "C:/WATTDBG.TXT");
		tcp_inject_config(cfg, "DEBUG.MODE", "ALL");
		tcp_inject_config(cfg, "DEBUG.PROTO", "ALL");
		tcp_inject_config(cfg, "DEBUG.LINEBUF", "1");
	}
	return 1;
}

static void
copy_word(char *dst, size_t len, const char *src)
{
	size_t i = 0;

	while (*src == ' ' || *src == '\t')
		src++;
	while (src[i] && src[i] != ' ' && src[i] != '\t' &&
	    src[i] != '\r' && src[i] != '\n' && i + 1 < len) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

static void
read_mtcp_config(void)
{
	const char *path = getenv("MTCPCFG");
	char line[256];
	FILE *f;

	if (getenv("WATTCP.CFG") != NULL || getenv("WATTCP_CFG") != NULL)
		return;		/* the user's own WATTCP.CFG */
	if (path == NULL)
		path = "C:\\VMDOS\\NET\\VMTCP.CFG";
	if ((f = fopen(path, "r")) == NULL) {
		fprintf(stderr, "ssh: can't read the TCP/IP settings (%s): "
		    "run STARTNET first\n", path);
		exit(255);
	}
	while (fgets(line, sizeof line, f) != NULL) {
		char key[32], *v = line;

		copy_word(key, sizeof key, line);
		while (*v == ' ' || *v == '\t')
			v++;
		v += strlen(key);
		if (strcasecmp(key, "IPADDR") == 0)
			copy_word(m_ip, sizeof m_ip, v);
		else if (strcasecmp(key, "NETMASK") == 0)
			copy_word(m_mask, sizeof m_mask, v);
		else if (strcasecmp(key, "GATEWAY") == 0)
			copy_word(m_gw, sizeof m_gw, v);
		else if (strcasecmp(key, "PACKETINT") == 0)
			copy_word(m_vec, sizeof m_vec, v);
		else if (strcasecmp(key, "HOSTNAME") == 0)
			copy_word(m_host, sizeof m_host, v);
		else if (strcasecmp(key, "NAMESERVER") == 0 && m_nns < M_MAXNS)
			copy_word(m_ns[m_nns++], sizeof m_ns[0], v);

	}
	fclose(f);
	/* Never DHCP or BOOTP of its own: the address DOS already has. */
	if (*m_ip == '\0' || strcmp(m_ip, "0.0.0.0") == 0) {
		fprintf(stderr, "ssh: no IP address in %s: run STARTNET "
		    "(DHCP) or set one with VMTCP IPADDR\n", path);
		exit(255);
	}
	_watt_no_config = 1;
	_watt_user_config(mtcp_config);
	if (getenv("WATTDBG") != NULL)
		dbug_init();
}

__attribute__((constructor)) static void
dos_init(void)
{
	/* Bytes to and from the remote side as they are. */
	/* Files as they are: no CR/LF translation, no stop at Ctrl-Z
	   (sftp and scp copy files byte for byte). */
	_fmode = O_BINARY;
	setmode(STDOUT_FILENO, O_BINARY);
	/* ~/.ssh (known_hosts, keys): C:\HOME unless HOME says otherwise. */
	if (getenv("HOME") == NULL) {
		mkdir("C:/HOME", 0755);
		setenv("HOME", "C:/HOME", 1);
	}
	/* Without TERM the remote side gets none: dos-term.c is a Linux console. */
	if (getenv("TERM") == NULL)
		setenv("TERM", "linux", 1);
	read_mtcp_config();
}

/*
 * fcntl() on Watt-32's sockets: F_GETFL/F_SETFL as ssh uses them
 * (O_NONBLOCK) go to Watt-32 through FIONBIO; files and the console to
 * DJGPP's fcntl. dos-compat.h renames fcntl to dos_fcntl.
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
	if (fd >= 0 && fd < 256 && __FSEXT_get_function(fd) != NULL) {
		int on;

		switch (cmd) {
		case F_GETFL:
			return O_RDWR | (sock_nb[fd] ? O_NONBLOCK : 0);
		case F_SETFL:
			on = (arg & O_NONBLOCK) != 0;
			if (ioctlsocket(fd, FIONBIO, (char *)&on) == -1)
				return -1;
			sock_nb[fd] = on;
			return 0;
		case F_GETFD:
		case F_SETFD:
			return 0;
		}
	}
	return fcntl(fd, cmd, arg);
}

/* strerror() for Watt-32's network errnos too (DJGPP's stops at EILSEQ). */
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
		{ EAFNOSUPPORT, "IPv6 isn't supported yet (Watt-32's IPv6 TCP doesn't work)" },
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

/* For Watt-32 and the rest of libc too: these replace DJGPP's at link time. */
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

/*
 * IPv6: Watt-32's IPv6 TCP doesn't work (its state machine never parses
 * IPv6 segments, and more), so say so plainly rather than time out.
 */
#undef connect
int W32_CALL connect(int, const struct sockaddr *, socklen_t);

int W32_CALL
dos_connect(int s, const struct sockaddr *sa, socklen_t len)
{
	if (sa != NULL && sa->sa_family == AF_INET6) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	return connect(s, sa, len);
}

/* Pipes and sockets aren't terminals (DOS calls both character devices). */
#undef isatty
int
dos_isatty(int fd)
{
	if (dos_is_pipe(fd) || is_socket(fd))
		return 0;
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

	if (dos_is_pipe(fd)) {
		memset(st, 0, sizeof *st);
		st->st_mode = S_IFIFO | 0600;
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
