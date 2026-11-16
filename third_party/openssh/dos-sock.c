/*
 * dos-sock.c: sockets for ssh, sftp and scp on vmdos's own TCP.
 *
 * vmdos runs TCP connections itself (src/tcp9p.cpp, INT 2Fh AX=564Eh
 * BL=13-18), IPv4 and IPv6, on the PC's network card, beside DOS's packet
 * driver. This is the BSD socket side of it for the programs: socket(),
 * connect(), read()/write() through dos_read/dos_write, poll(), and
 * getaddrinfo() with a small DNS client (vmdos sends the question and
 * waits for the answer). The addresses, gateways and DNS servers are
 * vmdos's TCP/IP settings (VMTCP.CFG, as DHCP or VMTCP set them); vmdos
 * finds the next hop's MAC itself. Watt-32's headers give the types and
 * constants; none of its code is used.
 *
 * Not there: listening sockets (so no port forwarding), UDP sockets,
 * reverse DNS.
 */
#include "includes.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include <sys/movedata.h>

#include "log.h"
#include "dos-co.h"
#include "dos-sock.h"

#undef read
#undef write
#undef close
#undef poll

#ifndef EAI_OVERFLOW
#define EAI_OVERFLOW EAI_FAIL
#endif

/* --- vmdos's TCP/IP settings (VMTCP.CFG) --- */

static int cfg_read;
static unsigned char my4[4], mask4[4], gw4[4], my6[16], my6ll[16], gw6[16];
static int have4, have_gw4, have6, have_gw6, my6_len = 64;
static unsigned char ns4[4][4], ns6[2][16];
static int nns4, nns6;
static unsigned char mac[6];

static void
word(char *dst, size_t len, const char *src)
{
	size_t i = 0;

	while (*src == ' ' || *src == '\t')
		src++;
	while (src[i] && !isspace((unsigned char)src[i]) && i + 1 < len) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';
}

static void
kernel(__dpmi_regs *r, int fn)
{
	r->x.ax = 0x564e;
	r->h.bl = fn;
	__dpmi_int(0x2f, r);
}

/* Read VMTCP.CFG once: exit with a message if there is nothing to go on. */
void
dos_sock_config(void)
{
	const char *path = getenv("MTCPCFG");
	char line[256], key[32], val[128];
	__dpmi_regs r;
	FILE *f;

	if (cfg_read)
		return;
	cfg_read = 1;
	if (path == NULL)
		path = "C:\\VMDOS\\NET\\VMTCP.CFG";
	if ((f = fopen(path, "r")) == NULL) {
		fprintf(stderr, "ssh: can't read the TCP/IP settings (%s): "
		    "run STARTNET first\n", path);
		exit(255);
	}
	while (fgets(line, sizeof line, f) != NULL) {
		char *v = line, *slash;

		word(key, sizeof key, line);
		while (*v == ' ' || *v == '\t')
			v++;
		v += strlen(key);
		word(val, sizeof val, v);
		if (strcasecmp(key, "IPADDR") == 0)
			have4 = inet_pton(AF_INET, val, my4) == 1 &&
			    (my4[0] | my4[1] | my4[2] | my4[3]);
		else if (strcasecmp(key, "NETMASK") == 0)
			inet_pton(AF_INET, val, mask4);
		else if (strcasecmp(key, "GATEWAY") == 0)
			have_gw4 = inet_pton(AF_INET, val, gw4) == 1;
		else if (strcasecmp(key, "NAMESERVER") == 0 && nns4 < 4)
			nns4 += inet_pton(AF_INET, val, ns4[nns4]) == 1;
		else if (strcasecmp(key, "IPV6ADDR") == 0) {
			if ((slash = strchr(val, '/')) != NULL) {
				*slash = '\0';
				my6_len = atoi(slash + 1);
				if (my6_len <= 0 || my6_len > 128)
					my6_len = 64;
			}
			have6 = inet_pton(AF_INET6, val, my6) == 1;
		} else if (strcasecmp(key, "IPV6GATEWAY") == 0) {
			if ((slash = strchr(val, '%')) != NULL)
				*slash = '\0';
			have_gw6 = inet_pton(AF_INET6, val, gw6) == 1;
		} else if (strcasecmp(key, "IPV6NAMESERVER") == 0 && nns6 < 2)
			nns6 += inet_pton(AF_INET6, val, ns6[nns6]) == 1;
	}
	fclose(f);

	/* The card's MAC (and that there is one): the link-local address. */
	memset(&r, 0, sizeof r);
	r.x.es = __tb >> 4;
	r.x.di = __tb & 15;
	kernel(&r, 0);
	if (r.x.bx != 0x564d || r.x.ax != 1) {
		fprintf(stderr, "ssh: no network card that vmdos drives "
		    "(this needs vmdos)\n");
		exit(255);
	}
	dosmemget(__tb, 6, mac);
	memset(my6ll, 0, 16);
	my6ll[0] = 0xfe; my6ll[1] = 0x80;
	my6ll[8] = mac[0] ^ 2; my6ll[9] = mac[1]; my6ll[10] = mac[2];
	my6ll[11] = 0xff; my6ll[12] = 0xfe;
	my6ll[13] = mac[3]; my6ll[14] = mac[4]; my6ll[15] = mac[5];
	if (!have4 && !have6) {
		fprintf(stderr, "ssh: no IP address in %s: run STARTNET (DHCP) "
		    "or set one with VMTCP IPADDR\n", path);
		exit(255);
	}
	/* Close what a program before us left open. */
	memset(&r, 0, sizeof r);
	r.x.dx = 0xffff;
	kernel(&r, 14);
}

/* --- the kernel's connection settings (src/tcp9p.hpp, Tcp9pCfg) --- */

struct __attribute__((packed)) kcfg {
	unsigned char family;
	unsigned short port;
	unsigned char server[16];
	unsigned char hop_mac[6];
	unsigned char my4[4];
	unsigned char my6[16], my6ll[16];
	unsigned short local_port;
	unsigned char hop4[4];
	unsigned char hop6[16];
};

static int
link_local(const unsigned char *a)
{
	return a[0] == 0xfe && (a[1] & 0xc0) == 0x80;
}

static int
on_link6(const unsigned char *d)
{
	int bits = my6_len, i;

	if (link_local(d))
		return 1;
	if (!have6)
		return 0;
	for (i = 0; bits >= 8; i++, bits -= 8)
		if (d[i] != my6[i])
			return 0;
	return bits == 0 || ((d[i] ^ my6[i]) >> (8 - bits)) == 0;
}

/* Fill in where to send to sa (family, addresses, next hop). */
static int
route(const struct sockaddr *sa, struct kcfg *c)
{
	memset(c, 0, sizeof *c);
	memcpy(c->my4, my4, 4);
	if (have6)
		memcpy(c->my6, my6, 16);
	memcpy(c->my6ll, my6ll, 16);
	if (sa->sa_family == AF_INET) {
		const struct sockaddr_in *s4 = (const struct sockaddr_in *)sa;
		const unsigned char *d = (const unsigned char *)&s4->sin_addr;
		int i, local = 1;

		if (!have4) {
			errno = ENETUNREACH;
			return -1;
		}
		c->family = 4;
		c->port = ntohs(s4->sin_port);
		memcpy(c->server, d, 4);
		for (i = 0; i < 4; i++)
			if ((d[i] ^ my4[i]) & mask4[i])
				local = 0;
		if (local)
			memcpy(c->hop4, d, 4);
		else if (have_gw4)
			memcpy(c->hop4, gw4, 4);
		else {
			errno = ENETUNREACH;
			return -1;
		}
		return 0;
	}
	if (sa->sa_family == AF_INET6) {
		const struct sockaddr_in6 *s6 = (const struct sockaddr_in6 *)sa;
		const unsigned char *d = s6->sin6_addr.s6_addr;

		c->family = 6;
		c->port = ntohs(s6->sin6_port);
		memcpy(c->server, d, 16);
		if (on_link6(d))
			memcpy(c->hop6, d, 16);
		else if (have6 && have_gw6)
			memcpy(c->hop6, gw6, 16);
		else {
			errno = ENETUNREACH;
			return -1;
		}
		return 0;
	}
	errno = EAFNOSUPPORT;
	return -1;
}

/* --- sockets: fds SOCK_FD0... --- */

struct sk {
	int used;
	int family;
	int h;			/* the kernel's connection, -1 none */
	int nonblock;
	int err;		/* for SO_ERROR */
	struct sockaddr_storage peer;
	socklen_t peerlen;
	unsigned short lport;
};
static struct sk sks[NSOCK_FDS];

static struct sk *
sk_get(int fd)
{
	if (fd < SOCK_FD0 || fd >= SOCK_FD0 + NSOCK_FDS ||
	    !sks[fd - SOCK_FD0].used)
		return NULL;
	return &sks[fd - SOCK_FD0];
}

int
dos_is_sock(int fd)
{
	return sk_get(fd) != NULL;
}

/* AX state, CX bytes waiting, DX unacknowledged (BL=17). */
static int
k_status(int h, unsigned *waiting, unsigned *unacked)
{
	__dpmi_regs r;

	memset(&r, 0, sizeof r);
	r.x.dx = h;
	kernel(&r, 17);
	if (waiting)
		*waiting = r.x.cx;
	if (unacked)
		*unacked = r.x.dx;
	return r.x.ax == 0xffff ? 4 : r.x.ax;
}

static void
idle(void)
{
	if (co_ssh_state == CO_RUNNING)
		co_yield();
	else {
		dos_alarm_check();
		__dpmi_yield();
	}
}

int
socket(int domain, int type, int protocol)
{
	int i;

	(void)protocol;
	dos_sock_config();
	if (domain != AF_INET && domain != AF_INET6) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	if ((type & 0xff) != SOCK_STREAM) {
		errno = EPROTONOSUPPORT;
		return -1;
	}
	for (i = 0; i < NSOCK_FDS; i++)
		if (!sks[i].used) {
			memset(&sks[i], 0, sizeof sks[i]);
			sks[i].used = 1;
			sks[i].family = domain;
			sks[i].h = -1;
			return SOCK_FD0 + i;
		}
	errno = EMFILE;
	return -1;
}

static int
state_errno(int st)
{
	return st == 3 ? ECONNREFUSED : st == 2 ? ECONNRESET : ENOTCONN;
}

int
connect(int fd, const struct sockaddr *sa, socklen_t len)
{
	struct sk *s = sk_get(fd);
	struct kcfg c;
	__dpmi_regs r;
	unsigned long t0;
	int st;

	if (s == NULL) {
		errno = ENOTSOCK;
		return -1;
	}
	if (s->h >= 0) {
		errno = EISCONN;
		return -1;
	}
	if (route(sa, &c) == -1)
		return -1;
	dosmemput(&c, sizeof c, __tb);
	memset(&r, 0, sizeof r);
	r.x.es = __tb >> 4;
	r.x.di = __tb & 15;
	kernel(&r, 13);
	if (r.x.ax == 0xffff) {
		errno = ENOBUFS;
		return -1;
	}
	s->h = r.x.ax;
	memcpy(&s->peer, sa, len < sizeof s->peer ? len : sizeof s->peer);
	s->peerlen = len;
	if (s->nonblock) {
		errno = EINPROGRESS;
		return -1;
	}
	t0 = _farpeekl(_dos_ds, 0x46c);
	while ((st = k_status(s->h, NULL, NULL)) == 0) {
		if (_farpeekl(_dos_ds, 0x46c) - t0 > 60 * 18) {	/* 60 s */
			s->err = ETIMEDOUT;
			errno = ETIMEDOUT;
			return -1;
		}
		idle();
	}
	if (st != 1) {
		errno = s->err = state_errno(st);
		return -1;
	}
	return 0;
}

ssize_t
dos_sock_read(int fd, void *buf, size_t len)
{
	struct sk *s = sk_get(fd);
	__dpmi_regs r;
	size_t room = _go32_info_block.size_of_transfer_buffer;

	if (s == NULL || s->h < 0) {
		errno = ENOTCONN;
		return -1;
	}
	for (;;) {
		memset(&r, 0, sizeof r);
		r.x.dx = s->h;
		r.x.es = __tb >> 4;
		r.x.di = __tb & 15;
		r.x.cx = len < room ? len : room;
		kernel(&r, 16);
		if (r.x.cx > 0) {
			dosmemget(__tb, r.x.cx, buf);
			return r.x.cx;
		}
		if (r.x.ax == 2)
			return 0;			/* the server closed it */
		if (r.x.ax >= 3) {
			errno = r.x.ax == 3 ? ECONNRESET : ENOTCONN;
			return -1;
		}
		if (s->nonblock) {
			errno = EAGAIN;
			return -1;
		}
		idle();
	}
}

ssize_t
dos_sock_write(int fd, const void *buf, size_t len)
{
	struct sk *s = sk_get(fd);
	const char *p = buf;
	size_t done = 0, room = _go32_info_block.size_of_transfer_buffer;
	__dpmi_regs r;

	if (s == NULL || s->h < 0) {
		errno = ENOTCONN;
		return -1;
	}
	while (done < len) {
		size_t n = len - done < room ? len - done : room;

		dosmemput(p + done, n, __tb);
		memset(&r, 0, sizeof r);
		r.x.dx = s->h;
		r.x.ds = __tb >> 4;
		r.x.si = __tb & 15;
		r.x.cx = n;
		kernel(&r, 15);
		if (r.x.ax == 0xffff) {
			if (done)
				break;
			errno = EPIPE;
			return -1;
		}
		done += r.x.ax;
		if (r.x.ax < n) {			/* full for now */
			if (s->nonblock) {
				if (done)
					break;
				errno = EAGAIN;
				return -1;
			}
			idle();
		}
	}
	return done;
}

/* poll(): POLLIN, POLLOUT, POLLHUP, POLLERR for a socket. */
int
dos_sock_poll(int fd, int events)
{
	struct sk *s = sk_get(fd);
	unsigned waiting;
	int st, rev = 0;

	if (s == NULL)
		return POLLNVAL;
	if (s->h < 0)
		return events & POLLOUT ? POLLOUT | POLLHUP : POLLHUP;
	st = k_status(s->h, &waiting, NULL);
	if ((events & POLLIN) && (waiting > 0 || st >= 2))
		rev |= POLLIN;
	if ((events & POLLOUT) && st >= 1)
		rev |= POLLOUT;
	if (st >= 2 && waiting == 0)
		rev |= POLLHUP;
	if (st >= 3)
		rev |= POLLERR;
	return rev;
}

int
dos_sock_close(int fd)
{
	struct sk *s = sk_get(fd);
	__dpmi_regs r;
	unsigned unacked;
	unsigned long t0;

	if (s == NULL) {
		errno = EBADF;
		return -1;
	}
	if (s->h >= 0) {
		/* What's queued goes first (up to 3 s), then the connection. */
		t0 = _farpeekl(_dos_ds, 0x46c);
		while (k_status(s->h, NULL, &unacked) == 1 && unacked > 0 &&
		    _farpeekl(_dos_ds, 0x46c) - t0 < 3 * 18)
			__dpmi_yield();
		memset(&r, 0, sizeof r);
		r.x.dx = s->h;
		kernel(&r, 14);
	}
	s->used = 0;
	return 0;
}

int
dos_sock_setnb(int fd, int nb)
{
	struct sk *s = sk_get(fd);

	if (s == NULL)
		return -1;
	if (nb >= 0)
		s->nonblock = nb;
	return s->nonblock;
}

int
getsockopt(int fd, int level, int name, void *val, socklen_t *len)
{
	struct sk *s = sk_get(fd);

	if (s == NULL) {
		errno = ENOTSOCK;
		return -1;
	}
	if (level == SOL_SOCKET && name == SO_ERROR && *len >= sizeof(int)) {
		int e = s->err;

		if (e == 0 && s->h >= 0) {
			int st = k_status(s->h, NULL, NULL);

			if (st >= 2)
				e = state_errno(st);
		}
		*(int *)val = e;
		s->err = 0;
		*len = sizeof(int);
		return 0;
	}
	if (*len >= sizeof(int)) {
		*(int *)val = 0;
		*len = sizeof(int);
	}
	return 0;
}

int
setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
	(void)level; (void)name; (void)val; (void)len;
	if (sk_get(fd) == NULL) {
		errno = ENOTSOCK;
		return -1;
	}
	return 0;	/* TCP_NODELAY, SO_KEEPALIVE, IP_TOS: vmdos's own way */
}

int
getpeername(int fd, struct sockaddr *sa, socklen_t *len)
{
	struct sk *s = sk_get(fd);

	if (s == NULL || s->h < 0) {
		errno = s == NULL ? ENOTSOCK : ENOTCONN;
		return -1;
	}
	if (*len > s->peerlen)
		*len = s->peerlen;
	memcpy(sa, &s->peer, *len);
	return 0;
}

int
getsockname(int fd, struct sockaddr *sa, socklen_t *len)
{
	struct sk *s = sk_get(fd);
	struct sockaddr_storage ss;
	socklen_t n;

	if (s == NULL) {
		errno = ENOTSOCK;
		return -1;
	}
	memset(&ss, 0, sizeof ss);
	if (s->family == AF_INET) {
		struct sockaddr_in *a = (struct sockaddr_in *)&ss;

		a->sin_family = AF_INET;
		memcpy(&a->sin_addr, my4, 4);
		n = sizeof *a;
	} else {
		struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
		const struct sockaddr_in6 *p = (const struct sockaddr_in6 *)&s->peer;

		a->sin6_family = AF_INET6;
		memcpy(&a->sin6_addr, link_local(p->sin6_addr.s6_addr) ||
		    !have6 ? my6ll : my6, 16);
		n = sizeof *a;
	}
	if (*len > n)
		*len = n;
	memcpy(sa, &ss, *len);
	return 0;
}

int
shutdown(int fd, int how)
{
	(void)how;
	return sk_get(fd) != NULL ? 0 : (errno = ENOTSOCK, -1);
}

/* No listening sockets: no port forwarding, no X11, no agent. */
int
bind(int fd, const struct sockaddr *sa, socklen_t len)
{
	(void)fd; (void)sa; (void)len;
	errno = EOPNOTSUPP;
	return -1;
}

int
listen(int fd, int n)
{
	(void)fd; (void)n;
	errno = EOPNOTSUPP;
	return -1;
}

int
accept(int fd, struct sockaddr *sa, socklen_t *len)
{
	(void)fd; (void)sa; (void)len;
	errno = EOPNOTSUPP;
	return -1;
}

int
sendmsg(int fd, const struct msghdr *m, int flags)
{
	(void)fd; (void)m; (void)flags;
	errno = ENOSYS;
	return -1;
}

int
recvmsg(int fd, struct msghdr *m, int flags)
{
	(void)fd; (void)m; (void)flags;
	errno = ENOSYS;
	return -1;
}

/* --- addresses --- */

const char *
inet_ntop(int af, const void *src, char *dst, size_t size)
{
	const unsigned char *a = src;
	char buf[48], *p = buf;
	int i, best = -1, bestlen = 0;

	if (af == AF_INET) {
		snprintf(buf, sizeof buf, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
	} else if (af == AF_INET6) {
		for (i = 0; i < 8; ) {		/* the longest run of zeros: :: */
			int j = i;

			while (j < 8 && a[2 * j] == 0 && a[2 * j + 1] == 0)
				j++;
			if (j - i > bestlen && j - i > 1) {
				best = i;
				bestlen = j - i;
			}
			i = j > i ? j : i + 1;
		}
		for (i = 0; i < 8; i++) {
			if (i == best) {
				p += sprintf(p, "::");
				i += bestlen - 1;
				continue;
			}
			if (p > buf && p[-1] != ':')
				*p++ = ':';
			p += sprintf(p, "%x", a[2 * i] << 8 | a[2 * i + 1]);
		}
		*p = '\0';
	} else {
		errno = EAFNOSUPPORT;
		return NULL;
	}
	if (strlen(buf) >= size) {
		errno = ENOSPC;
		return NULL;
	}
	strcpy(dst, buf);
	return dst;
}

static int
pton4(const char *s, unsigned char *a)
{
	int i;

	for (i = 0; i < 4; i++) {
		unsigned v = 0, digits = 0;

		while (isdigit((unsigned char)*s) && digits < 4) {
			v = v * 10 + (*s++ - '0');
			digits++;
		}
		if (!digits || v > 255 || (digits > 1 && s[-(int)digits] == '0'))
			return 0;
		a[i] = v;
		if (i < 3 && *s++ != '.')
			return 0;
	}
	return *s == '\0';
}

int
inet_pton(int af, const char *src, void *dst)
{
	unsigned char a[16], *out = dst;
	int i = 0, gap = -1;

	if (af == AF_INET) {
		if (!pton4(src, a))
			return 0;
		memcpy(out, a, 4);
		return 1;
	}
	if (af != AF_INET6) {
		errno = EAFNOSUPPORT;
		return -1;
	}
	memset(a, 0, 16);
	if (src[0] == ':' && src[1] == ':') {
		gap = 0;
		src += 2;
	}
	while (*src && i < 16) {
		const char *p = src;
		unsigned v = 0, digits = 0;

		if (strchr(p, '.') != NULL && strchr(p, ':') == NULL) {
			if (i > 12 || !pton4(p, a + i))	/* ::ffff:1.2.3.4 */
				return 0;
			i += 4;
			src += strlen(src);
			break;
		}
		while (isxdigit((unsigned char)*src) && digits < 4) {
			v = v * 16 + (isdigit((unsigned char)*src) ? *src - '0' :
			    tolower((unsigned char)*src) - 'a' + 10);
			src++;
			digits++;
		}
		if (!digits)
			return 0;
		a[i++] = v >> 8;
		a[i++] = v & 0xff;
		if (*src == '\0')
			break;
		if (*src++ != ':')
			return 0;
		if (*src == ':') {
			if (gap >= 0)
				return 0;
			gap = i;
			src++;
			if (*src == '\0')
				break;
		}
	}
	if (*src != '\0')
		return 0;
	if (gap >= 0) {
		int n = i - gap;

		if (i == 16)
			return 0;
		memmove(a + 16 - n, a + gap, n);
		memset(a + gap, 0, 16 - n - gap);
	} else if (i != 16)
		return 0;
	memcpy(out, a, 16);
	return 1;
}

/* The DNS: one question (A or AAAA) to each name server in turn. */
static int
dns_ask(const char *name, int qtype, unsigned char *out, int max)
{
	static unsigned short id;
	unsigned char q[300], *p;
	struct {
		struct kcfg c;
		unsigned short len, timeout_ms;
		unsigned char data[1400];
	} __attribute__((packed)) u;
	unsigned char *ans = u.data;
	const char *l;
	int n = 0, srv, nsrv = nns4 + (have6 ? nns6 : 0), answered = 0;

	if (strlen(name) > 253)
		return 0;
	memset(q, 0, 12);
	id = (unsigned short)(id * 31 + _farpeekl(_dos_ds, 0x46c) + 1);
	q[0] = id >> 8; q[1] = id & 0xff;
	q[2] = 0x01;			/* recursion desired */
	q[5] = 1;			/* one question */
	p = q + 12;
	for (l = name; *l; ) {
		const char *dot = strchr(l, '.');
		int ll = dot ? dot - l : (int)strlen(l);

		if (ll == 0 || ll > 63)
			return 0;
		*p++ = ll;
		memcpy(p, l, ll);
		p += ll;
		l += ll;
		if (*l == '.')
			l++;
	}
	*p++ = 0;
	*p++ = 0; *p++ = qtype;
	*p++ = 0; *p++ = 1;

	for (srv = 0; srv < nsrv && !answered; srv++) {
		struct sockaddr_storage ss;
		__dpmi_regs r;
		int alen, i, qd, an, off;

		memset(&ss, 0, sizeof ss);
		if (srv < nns4) {
			struct sockaddr_in *a = (struct sockaddr_in *)&ss;

			a->sin_family = AF_INET;
			a->sin_port = htons(53);
			memcpy(&a->sin_addr, ns4[srv], 4);
		} else {
			struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;

			a->sin6_family = AF_INET6;
			a->sin6_port = htons(53);
			memcpy(&a->sin6_addr, ns6[srv - nns4], 16);
		}
		if (route((struct sockaddr *)&ss, &u.c) == -1)
			continue;
		u.len = p - q;
		u.timeout_ms = 2500;
		memcpy(u.data, q, p - q);
		dosmemput(&u, sizeof u, __tb);
		memset(&r, 0, sizeof r);
		r.x.es = __tb >> 4;
		r.x.di = __tb & 15;
		kernel(&r, 18);
		if (r.x.ax != 0 || r.x.cx < 12)
			continue;
		alen = r.x.cx;
		dosmemget(__tb + offsetof(typeof(u), data), alen, ans);
		if (ans[0] != q[0] || ans[1] != q[1])
			continue;
		if ((ans[3] & 0x0f) != 0 && (ans[3] & 0x0f) != 3)
			continue;		/* not NOERROR/NXDOMAIN: ask another */
		answered = 1;		/* an answer, even "none": done */
		if ((ans[3] & 0x0f) != 0)
			break;
		qd = ans[4] << 8 | ans[5];
		an = ans[6] << 8 | ans[7];
		off = 12;
		for (i = 0; i < qd && off < alen; i++) {	/* skip questions */
			while (off < alen && ans[off] && (ans[off] & 0xc0) != 0xc0)
				off += ans[off] + 1;
			off += (off < alen && (ans[off] & 0xc0) == 0xc0) ? 2 : 1;
			off += 4;
		}
		for (i = 0; i < an && off + 10 <= alen; i++) {
			int type, rdlen;

			while (off < alen && ans[off] && (ans[off] & 0xc0) != 0xc0)
				off += ans[off] + 1;
			off += (off < alen && (ans[off] & 0xc0) == 0xc0) ? 2 : 1;
			if (off + 10 > alen)
				break;
			type = ans[off] << 8 | ans[off + 1];
			rdlen = ans[off + 8] << 8 | ans[off + 9];
			off += 10;
			if (off + rdlen > alen)
				break;
			if (type == qtype && rdlen == (qtype == 1 ? 4 : 16) &&
			    n < max) {
				memcpy(out + n * 16, ans + off, rdlen);
				n++;
			}
			off += rdlen;		/* CNAMEs: the answers follow */
		}
	}
	return n;
}

static int
port_of(const char *serv, const struct addrinfo *hints, int *port)
{
	char *end;
	long v;

	*port = 0;
	if (serv == NULL)
		return 0;
	v = strtol(serv, &end, 10);
	if (*serv && *end == '\0' && v >= 0 && v <= 65535) {
		*port = (int)v;
		return 0;
	}
	if (hints && (hints->ai_flags & AI_NUMERICSERV))
		return EAI_NONAME;
	if (strcmp(serv, "ssh") == 0)
		*port = 22;
	else
		return EAI_SERVICE;
	return 0;
}

static struct addrinfo *
ai_new(int family, const void *addr, int port, const struct addrinfo *hints)
{
	struct addrinfo *ai = malloc(sizeof *ai);
	struct sockaddr_storage *ss = malloc(sizeof *ss);

	if (ai == NULL || ss == NULL) {
		free(ai);
		free(ss);
		return NULL;
	}
	memset(ai, 0, sizeof *ai);
	memset(ss, 0, sizeof *ss);
	ai->ai_family = family;
	ai->ai_socktype = hints && hints->ai_socktype ? hints->ai_socktype :
	    SOCK_STREAM;
	ai->ai_protocol = IPPROTO_TCP;
	ai->ai_addr = (struct sockaddr *)ss;
	if (family == AF_INET) {
		struct sockaddr_in *a = (struct sockaddr_in *)ss;

		a->sin_family = AF_INET;
		a->sin_port = htons(port);
		memcpy(&a->sin_addr, addr, 4);
		ai->ai_addrlen = sizeof *a;
	} else {
		struct sockaddr_in6 *a = (struct sockaddr_in6 *)ss;

		a->sin6_family = AF_INET6;
		a->sin6_port = htons(port);
		memcpy(&a->sin6_addr, addr, 16);
		ai->ai_addrlen = sizeof *a;
	}
	return ai;
}

int
getaddrinfo(const char *host, const char *serv, const struct addrinfo *hints,
    struct addrinfo **res)
{
	int family = hints ? hints->ai_family : AF_UNSPEC, port, e, i, n4 = 0,
	    n6 = 0, pass;
	unsigned char a4[8 * 16], a6[8 * 16], bin[16];
	struct addrinfo *head = NULL, **tail = &head;

	*res = NULL;
	if ((e = port_of(serv, hints, &port)) != 0)
		return e;
	if (host == NULL || *host == '\0') {
		if (hints && (hints->ai_flags & AI_PASSIVE))
			return EAI_NONAME;
		host = family == AF_INET6 ? "::1" : "127.0.0.1";
	}
	if (inet_pton(AF_INET, host, bin) == 1) {
		if (family == AF_INET6)
			return EAI_ADDRFAMILY;
		memcpy(a4, bin, 4);
		n4 = 1;
	} else if (inet_pton(AF_INET6, host, bin) == 1) {
		if (family == AF_INET)
			return EAI_ADDRFAMILY;
		memcpy(a6, bin, 16);
		n6 = 1;
	} else {
		if (hints && (hints->ai_flags & AI_NUMERICHOST))
			return EAI_NONAME;
		dos_sock_config();
		if (family != AF_INET)
			n6 = dns_ask(host, 28, a6, 8);
		if (family != AF_INET6)
			n4 = dns_ask(host, 1, a4, 8);
		if (n4 + n6 == 0)
			return EAI_NONAME;
	}
	/* IPv6 first when this PC has a global IPv6 address, else IPv4 first. */
	for (pass = 0; pass < 2; pass++) {
		int v6 = (pass == 0) == (have6 != 0);

		for (i = 0; i < (v6 ? n6 : n4); i++) {
			struct addrinfo *ai = ai_new(v6 ? AF_INET6 : AF_INET,
			    v6 ? a6 + i * 16 : a4 + i * 16, port, hints);

			if (ai == NULL) {
				freeaddrinfo(head);
				return EAI_MEMORY;
			}
			*tail = ai;
			tail = &ai->ai_next;
		}
	}
	*res = head;
	return 0;
}

void
freeaddrinfo(struct addrinfo *ai)
{
	while (ai != NULL) {
		struct addrinfo *next = ai->ai_next;

		free(ai->ai_addr);
		free(ai->ai_canonname);
		free(ai);
		ai = next;
	}
}

int
getnameinfo(const struct sockaddr *sa, socklen_t salen, char *host,
    socklen_t hostlen, char *serv, socklen_t servlen, int flags)
{
	(void)salen;
	if (host != NULL && hostlen > 0) {
		const void *a;

		if ((flags & NI_NAMEREQD))	/* no reverse DNS */
			return EAI_NONAME;
		a = sa->sa_family == AF_INET ?
		    (const void *)&((const struct sockaddr_in *)sa)->sin_addr :
		    (const void *)&((const struct sockaddr_in6 *)sa)->sin6_addr;
		if (inet_ntop(sa->sa_family, a, host, hostlen) == NULL)
			return EAI_OVERFLOW;
	}
	if (serv != NULL && servlen > 0) {
		int port = sa->sa_family == AF_INET ?
		    ntohs(((const struct sockaddr_in *)sa)->sin_port) :
		    ntohs(((const struct sockaddr_in6 *)sa)->sin6_port);

		if (snprintf(serv, servlen, "%d", port) >= (int)servlen)
			return EAI_OVERFLOW;
	}
	return 0;
}

char *
gai_strerror(int e)
{
	switch (e) {
	case EAI_NONAME: return (char *)"Name or service not known";
	case EAI_AGAIN: return (char *)"Temporary failure in name resolution";
	case EAI_FAIL: return (char *)"Non-recoverable failure in name resolution";
	case EAI_FAMILY: return (char *)"Address family not supported";
	case EAI_MEMORY: return (char *)"Memory allocation failure";
	case EAI_SERVICE: return (char *)"Service not supported";
	case EAI_ADDRFAMILY: return (char *)"No address of that family for the host";
	}
	return (char *)"Name resolution failed";
}

struct servent *
getservbyname(const char *name, const char *proto)
{
	static struct servent se;
	static char *aliases[1];

	(void)proto;
	if (strcmp(name, "ssh") != 0)
		return NULL;
	se.s_name = (char *)"ssh";
	se.s_aliases = aliases;
	se.s_port = htons(22);
	se.s_proto = (char *)"tcp";
	return &se;
}

/* --- not here: syslog, the resolver library (SSHFP records) --- */

void openlog(const char *id, int opt, int fac) { (void)id; (void)opt; (void)fac; }
void closelog(void) { }
void syslog(int pri, const char *fmt, ...) { (void)pri; (void)fmt; }
