/*
 * wget.c: WGET.EXE, downloads over HTTP and HTTPS for vmdos.
 *
 *   WGET [-k] [-q] [--ca FILE] URL [FILE]
 *
 * A DJGPP program (tools/build-wget.sh): the network is vmdos's own TCP
 * through ssh's socket layer (third_party/openssh/dos-sock.c: socket(),
 * connect(), getaddrinfo() over INT 2Fh AX=564Eh; VMTCP.CFG's addresses
 * and DNS), TLS 1.2 and 1.3 is mbedTLS's, certificates are checked against
 * CACERT.PEM (C:\VMDOS\NET, next to VMTCP.CFG) and the host name. Nothing
 * of it is in vmdos's kernel. HTTP/1.1 with Connection: close; chunked
 * bodies; up to 8 redirects. The same file builds on Linux (plain BSD
 * sockets) to test it there.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "psa/crypto.h"
#include "mbedtls/platform_time.h"

#ifdef __DJGPP__
#include <fcntl.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include "dos-sock.h"
#define net_read(fd, p, n) dos_sock_read(fd, p, n)
#define net_write(fd, p, n) dos_sock_write(fd, p, n)
#define net_close(fd) dos_sock_close(fd)
#define DEFAULT_CA "C:\\VMDOS\\NET\\CACERT.PEM"
#else
#define net_read(fd, p, n) read(fd, p, n)
#define net_write(fd, p, n) write(fd, p, n)
#define net_close(fd) close(fd)
#define DEFAULT_CA "/etc/ssl/certs/ca-certificates.crt"
#endif

static int insecure, quiet;
static const char *ca_file;

static void
die(const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "wget: ");
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	exit(1);
}

/* --- entropy: RDRAND where the CPU has it, the TSC's jitter, the clock --- */

static uint64_t
tsc(void)
{
	uint32_t lo, hi;

	__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
	return (uint64_t)hi << 32 | lo;
}

static int
have_rdrand(void)
{
	uint32_t a = 1, b, c = 0, d;

	__asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
	return (c >> 30) & 1;
}

int
mbedtls_hardware_poll(void *data, unsigned char *out, size_t len,
    size_t *olen)
{
	static int rd = -1;
	size_t i;

	(void)data;
	if (rd < 0)
		rd = have_rdrand();
	for (i = 0; i < len; i++) {
		uint32_t v = 0, j = 0;
		unsigned char ok = 0;
		int k, t;

		for (t = 0; rd && t < 10 && !ok; t++)
			__asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
		for (k = 0; k < 8; k++) {	/* a few noisy bits from timing */
			uint64_t t0 = tsc();
			volatile int spin;
			for (spin = 0; spin < 50; spin++)
				;
			j = j * 31 + (uint32_t)(tsc() - t0);
		}
		out[i] = (unsigned char)(v ^ j ^ (j >> 8) ^ (j >> 16) ^ (uint32_t)time(NULL) ^ (uint32_t)clock());
	}
	*olen = len;
	return 0;
}

/* Milliseconds for mbedTLS (TLS 1.3 ticket ages): coarse is enough. */
mbedtls_ms_time_t
mbedtls_ms_time(void)
{
#ifdef __DJGPP__
	return (mbedtls_ms_time_t)time(NULL) * 1000;
#else
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (mbedtls_ms_time_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#endif
}

/* --- the connection: TCP, TLS over it --- */

static int fd = -1, tls;
static mbedtls_ssl_context ssl;
static mbedtls_ssl_config conf;
static mbedtls_ctr_drbg_context drbg;
static mbedtls_entropy_context entropy;
static mbedtls_x509_crt cas;
static int tls_ready, cas_loaded;

static int
bio_send(void *ctx, const unsigned char *p, size_t n)
{
	ssize_t r = net_write(fd, p, n);

	(void)ctx;
	if (r < 0)
		return errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	return (int)r;
}

static int
bio_recv(void *ctx, unsigned char *p, size_t n)
{
	ssize_t r = net_read(fd, p, n);

	(void)ctx;
	if (r < 0)
		return errno == EAGAIN ? MBEDTLS_ERR_SSL_WANT_READ : MBEDTLS_ERR_SSL_INTERNAL_ERROR;
	if (r == 0)
		return MBEDTLS_ERR_SSL_CONN_EOF;
	return (int)r;
}

static void
tls_init(void)
{
	static const char pers[] = "vmdos wget";
	int e;

	if (tls_ready)
		return;
	if (psa_crypto_init() != PSA_SUCCESS)
		die("TLS: no crypto");
	mbedtls_entropy_init(&entropy);
	mbedtls_ctr_drbg_init(&drbg);
	mbedtls_x509_crt_init(&cas);
	if ((e = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
	    (const unsigned char *)pers, sizeof pers - 1)) != 0)
		die("TLS: no random numbers (-%x)", -e);
	if (!insecure) {
		e = mbedtls_x509_crt_parse_file(&cas, ca_file);
		if (e < 0)
			die("can't read the certificate authorities in %s "
			    "(-k skips the check)", ca_file);
		cas_loaded = 1;
	}
	tls_ready = 1;
}

static const char *
verify_text(uint32_t f)
{
	if (f & MBEDTLS_X509_BADCERT_EXPIRED)
		return "its certificate has expired (or this PC's clock is wrong)";
	if (f & MBEDTLS_X509_BADCERT_FUTURE)
		return "its certificate isn't valid yet (or this PC's clock is wrong)";
	if (f & MBEDTLS_X509_BADCERT_CN_MISMATCH)
		return "its certificate is for another name";
	if (f & MBEDTLS_X509_BADCERT_NOT_TRUSTED)
		return "its certificate isn't from a known authority";
	return "its certificate isn't valid";
}

static void
disconnect(void)
{
	if (tls) {
		mbedtls_ssl_close_notify(&ssl);
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&conf);
		tls = 0;
	}
	if (fd >= 0)
		net_close(fd);
	fd = -1;
}

static void
connect_to(const char *host, const char *port, int use_tls)
{
	struct addrinfo hints, *ai, *a;
	int e;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if ((e = getaddrinfo(host, port, &hints, &ai)) != 0)
		die("%s: %s", host, gai_strerror(e));
	for (a = ai; a; a = a->ai_next) {
		if ((fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol)) < 0)
			continue;
		if (connect(fd, a->ai_addr, a->ai_addrlen) == 0)
			break;
		net_close(fd);
		fd = -1;
	}
	freeaddrinfo(ai);
	if (fd < 0)
		die("%s port %s: %s", host, port, strerror(errno));
	if (!use_tls)
		return;
	tls_init();
	mbedtls_ssl_init(&ssl);
	mbedtls_ssl_config_init(&conf);
	tls = 1;
	if ((e = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
	    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
		die("TLS setup (-%x)", -e);
	mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
	mbedtls_ssl_conf_authmode(&conf, insecure ? MBEDTLS_SSL_VERIFY_NONE :
	    MBEDTLS_SSL_VERIFY_REQUIRED);
	mbedtls_ssl_conf_ca_chain(&conf, &cas, NULL);
	if ((e = mbedtls_ssl_setup(&ssl, &conf)) != 0 ||
	    (e = mbedtls_ssl_set_hostname(&ssl, host)) != 0)
		die("TLS setup (-%x)", -e);
	mbedtls_ssl_set_bio(&ssl, NULL, bio_send, bio_recv, NULL);
	while ((e = mbedtls_ssl_handshake(&ssl)) != 0) {
		uint32_t vf;

		if (e == MBEDTLS_ERR_SSL_WANT_READ || e == MBEDTLS_ERR_SSL_WANT_WRITE)
			continue;
		vf = mbedtls_ssl_get_verify_result(&ssl);
		if (e == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && vf)
			die("%s: %s (-k skips the check)", host, verify_text(vf));
		if (e == MBEDTLS_ERR_SSL_CONN_EOF)
			die("%s closed the connection during the TLS handshake", host);
		{
			char m[100];
			mbedtls_strerror(e, m, sizeof m);
			die("%s: TLS handshake failed: %s", host, m);
		}
	}
	if (!quiet)
		fprintf(stderr, "%s, %s\n", mbedtls_ssl_get_version(&ssl),
		    mbedtls_ssl_get_ciphersuite(&ssl));
}

static void
c_send(const char *p, size_t n)
{
	while (n) {
		int r;

		if (tls) {
			r = mbedtls_ssl_write(&ssl, (const unsigned char *)p, n);
			if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE)
				continue;
		} else
			r = (int)net_write(fd, p, n);
		if (r <= 0)
			die("sending the request failed");
		p += r;
		n -= (size_t)r;
	}
}

/* Bytes; 0: the end. */
static size_t
c_recv(unsigned char *p, size_t n)
{
	for (;;) {
		int r;

		if (!tls) {
			ssize_t s = net_read(fd, p, n);
			if (s < 0 && errno == EINTR)
				continue;
			if (s < 0)
				die("receiving failed: %s", strerror(errno));
			return (size_t)s;
		}
		r = mbedtls_ssl_read(&ssl, p, n);
		if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE ||
		    r == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
			continue;
		if (r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF || r == 0)
			return 0;
		if (r < 0) {
			char m[100];
			mbedtls_strerror(r, m, sizeof m);
			die("TLS: %s", m);
		}
		return (size_t)r;
	}
}

/* --- HTTP --- */

struct url {
	int tls;
	char host[256], port[8], path[2048];
};

static void
parse_url(const char *u, struct url *o)
{
	const char *p = u, *h;
	size_t hl, n = 0;

	if (strncmp(p, "https://", 8) == 0) {
		o->tls = 1;
		p += 8;
	} else if (strncmp(p, "http://", 7) == 0) {
		o->tls = 0;
		p += 7;
	} else
		die("the URL has to start with http:// or https://");
	h = p;
	if (*p == '[') {		/* [IPv6] */
		h = ++p;
		while (*p && *p != ']')
			p++;
		hl = (size_t)(p - h);
		if (*p == ']')
			p++;
	} else {
		while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#')
			p++;
		hl = (size_t)(p - h);
	}
	if (!hl || hl >= sizeof o->host)
		die("no host name in the URL");
	memcpy(o->host, h, hl);
	o->host[hl] = '\0';
	strcpy(o->port, o->tls ? "443" : "80");
	if (*p == ':') {
		size_t k = 0;
		for (p++; *p >= '0' && *p <= '9' && k < sizeof o->port - 1; p++)
			o->port[k++] = *p;
		o->port[k] = '\0';
		if (!k)
			die("bad port in the URL");
	}
	if (*p != '/')
		o->path[n++] = '/';
	for (; *p && *p != '#' && n + 4 < sizeof o->path; p++) {
		unsigned char c = (unsigned char)*p;
		if (c <= ' ' || c >= 0x7f) {	/* spaces and the like, escaped */
			static const char hx[] = "0123456789ABCDEF";
			o->path[n++] = '%';
			o->path[n++] = hx[c >> 4];
			o->path[n++] = hx[c & 15];
		} else
			o->path[n++] = (char)c;
	}
	o->path[n] = '\0';
}

static unsigned char in[16384];
static size_t in_n, in_pos;
static enum { B_LENGTH, B_CHUNKED, B_CLOSE } body_mode;
static uint64_t body_left, length;
static int length_known;

static int
hdr_is(const char *l, const char *name)
{
	size_t n = strlen(name), i;

	for (i = 0; i < n; i++) {
		char a = l[i];
		if (a >= 'A' && a <= 'Z')
			a = (char)(a + 32);
		if (a != name[i])
			return 0;
	}
	return l[n] == ':';
}

static const char *
hdr_val(const char *l)
{
	const char *v = strchr(l, ':') + 1;

	while (*v == ' ' || *v == '\t')
		v++;
	return v;
}

/* Ask for u: the status; *loc the Location header, if any. */
static int
request(const struct url *u, char *loc, size_t locsz, char *status_line, size_t slsz)
{
	static char rq[3000], head[16385];
	int std = strcmp(u->port, u->tls ? "443" : "80") == 0, n, status;
	size_t hend = 0;
	char *l, *e;

	n = snprintf(rq, sizeof rq, "GET %s HTTP/1.1\r\nHost: %s%s%s\r\n"
	    "User-Agent: vmdos-wget/1.0\r\nAccept: */*\r\n"
	    "Accept-Encoding: identity\r\nConnection: close\r\n\r\n",
	    u->path, u->host, std ? "" : ":", std ? "" : u->port);
	c_send(rq, (size_t)n);
	in_n = in_pos = 0;
	while (!hend) {
		size_t r, i, from;

		if (in_n == sizeof in)
			die("%s: the headers are too long", u->host);
		r = c_recv(in + in_n, sizeof in - in_n);
		if (!r)
			die("%s closed the connection without an answer", u->host);
		from = in_n >= 3 ? in_n - 3 : 0;
		in_n += r;
		for (i = from; i + 3 < in_n; i++)
			if (memcmp(in + i, "\r\n\r\n", 4) == 0) {
				hend = i + 4;
				break;
			}
	}
	memcpy(head, in, hend);
	head[hend] = '\0';
	in_pos = hend;
	if (strncmp(head, "HTTP/1.", 7) != 0 || head[8] != ' ')
		die("%s: not an HTTP answer", u->host);
	status = atoi(head + 9);
	body_mode = B_CLOSE;
	length_known = 0;
	length = 0;
	*loc = '\0';
	for (l = strchr(head, '\n'); l && l[1]; l = e + 1) {
		l++;
		if ((e = strchr(l, '\r')) == NULL)
			break;
		*e = '\0';
		if (hdr_is(l, "content-length")) {
			length = strtoull(hdr_val(l), NULL, 10);
			length_known = 1;
			if (body_mode != B_CHUNKED)
				body_mode = B_LENGTH;
		} else if (hdr_is(l, "transfer-encoding") &&
		    strstr(hdr_val(l), "hunked")) {
			body_mode = B_CHUNKED;
			length_known = 0;
		} else if (hdr_is(l, "location"))
			snprintf(loc, locsz, "%s", hdr_val(l));
	}
	if ((e = strchr(head, '\r')) != NULL)
		*e = '\0';
	snprintf(status_line, slsz, "%s", head + 9);
	body_left = length;
	return status;
}

/* A Location against the URL it came with. */
static void
join_url(const struct url *b, const char *loc, char *out, size_t sz)
{
	const char *sch = b->tls ? "https" : "http";
	int std = strcmp(b->port, b->tls ? "443" : "80") == 0;
	char hp[300], dir[2048], *q, *last, *p;

	if (strncmp(loc, "http://", 7) == 0 || strncmp(loc, "https://", 8) == 0) {
		snprintf(out, sz, "%s", loc);
		return;
	}
	if (loc[0] == '/' && loc[1] == '/') {
		snprintf(out, sz, "%s:%s", sch, loc);
		return;
	}
	snprintf(hp, sizeof hp, "%s%s%s", b->host, std ? "" : ":", std ? "" : b->port);
	if (loc[0] == '/') {
		snprintf(out, sz, "%s://%s%s", sch, hp, loc);
		return;
	}
	snprintf(dir, sizeof dir, "%s", b->path);
	if ((q = strchr(dir, '?')) != NULL)
		*q = '\0';
	for (last = dir, p = dir; *p; p++)
		if (*p == '/')
			last = p;
	last[1] = '\0';
	snprintf(out, sz, "%s://%s%s%s", sch, hp, dir, loc);
}

/* Raw body bytes: the rest of what the headers came with, then more. */
static size_t
raw(unsigned char *p, size_t n)
{
	if (in_pos < in_n) {
		size_t k = in_n - in_pos < n ? in_n - in_pos : n;
		memcpy(p, in + in_pos, k);
		in_pos += k;
		return k;
	}
	return c_recv(p, n);
}

static int
raw_byte(void)
{
	static unsigned char b[4096];
	static size_t n, pos;

	if (in_pos < in_n)
		return in[in_pos++];
	if (pos == n) {
		n = c_recv(b, sizeof b);
		pos = 0;
		if (!n)
			return -1;
	}
	return b[pos++];
}

/* The body, decoded, to f: bytes written. */
static uint64_t
body(FILE *f, const char *name)
{
	static unsigned char buf[32768];
	uint64_t total = 0;
	time_t last = 0;

	for (;;) {
		size_t n = 0;

		if (body_mode == B_LENGTH) {
			if (!body_left)
				break;
			n = raw(buf, body_left < sizeof buf ? (size_t)body_left : sizeof buf);
			if (!n)
				die("the server closed the connection early");
			body_left -= n;
		} else if (body_mode == B_CLOSE) {
			if (!(n = raw(buf, sizeof buf)))
				break;
		} else {		/* chunked: the size line, the data, CR LF */
			if (!body_left) {
				uint64_t v = 0;
				int c, any = 0, ext = 0;

				while ((c = raw_byte()) >= 0 && c != '\n') {
					int d;
					if (ext || c == '\r')
						continue;
					if (c == ';') {
						ext = 1;
						continue;
					}
					d = c >= '0' && c <= '9' ? c - '0' :
					    (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
					if (d >= 0) {
						v = v * 16 + (uint64_t)d;
						any = 1;
					}
				}
				if (c < 0 || !any)
					die("a broken chunked body");
				if (!v)
					break;	/* the last chunk (trailers not needed) */
				body_left = v;
			}
			while (n < sizeof buf && body_left) {
				int c = raw_byte();
				if (c < 0)
					die("the server closed the connection early");
				buf[n++] = (unsigned char)c;
				body_left--;
			}
			if (!body_left) {	/* the CR LF after the data */
				int c;
				while ((c = raw_byte()) >= 0 && c != '\n')
					;
			}
		}
		if (n && fwrite(buf, 1, n, f) != n)
			die("writing %s failed (disk full?)", name);
		total += n;
		if (!quiet && time(NULL) != last) {
			last = time(NULL);
			if (length_known && length)
				fprintf(stderr, "\r%llu bytes %llu%%  ", (unsigned long long)total,
				    (unsigned long long)(total * 100 / length));
			else
				fprintf(stderr, "\r%llu bytes  ", (unsigned long long)total);
		}
	}
	return total;
}

/* The file name from the URL's last part; 8.3 when DOS has no long names. */
static void
name_from(const char *url, char *out, size_t sz)
{
	const char *p = strstr(url, "://"), *s, *e;
	size_t n = 0;

	p = p ? p + 3 : url;
	p = strchr(p, '/');
	s = e = NULL;
	if (p) {
		for (e = p; *e && *e != '?' && *e != '#'; e++)
			;
		for (s = e; s > p && s[-1] != '/'; s--)
			;
	}
	if (!s || s == e) {
		snprintf(out, sz, "index.htm");
		return;
	}
	while (s < e && n + 1 < sz)
		out[n++] = *s++;
	out[n] = '\0';
#ifdef __DJGPP__
	if (!_use_lfn(NULL)) {		/* NAME.EXT: the last dot's extension */
		char b[13], *dot = strrchr(out, '.');
		size_t k = 0, i;

		for (i = 0; out + i < (dot ? dot : out + n) && k < 8; i++)
			if (strchr("<>:\"/\\|?*+,;=[] .", out[i]) == NULL)
				b[k++] = out[i];
		if (!k)
			b[k++] = '_';
		if (dot) {
			size_t x = 0;
			b[k++] = '.';
			for (i = 1; dot[i] && x < 3; i++)
				if (strchr("<>:\"/\\|?*+,;=[] .", dot[i]) == NULL)
					b[k++] = dot[i], x++;
			if (!x)
				k--;
		}
		b[k] = '\0';
		snprintf(out, sz, "%s", b);
	}
#endif
}

static void
usage(void)
{
	fprintf(stderr,
	    "WGET [-k] [-q] [--ca FILE] URL [FILE]: download over HTTP or HTTPS\n"
	    "  WGET https://example.com/file.zip        saves file.zip here\n"
	    "  WGET http://host/page.html PAGE.HTM      saves PAGE.HTM\n"
	    "  -k  don't check the server's certificate\n"
	    "  -q  no progress\n"
	    "  --ca FILE  the certificate authorities (default %s)\n"
	    "Run STARTNET first.\n", DEFAULT_CA);
	exit(1);
}

int
main(int argc, char **argv)
{
	const char *url0 = NULL, *file = NULL;
	char url[2048], loc[2048], next[2048], status_line[200], name[300];
	struct url u;
	int i, hops, status = 0;
	uint64_t got;
	FILE *f;

	ca_file = getenv("WGET_CA");
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-k") == 0 || strcmp(argv[i], "/k") == 0 || strcmp(argv[i], "/K") == 0)
			insecure = 1;
		else if (strcmp(argv[i], "-q") == 0)
			quiet = 1;
		else if (strcmp(argv[i], "--ca") == 0 && i + 1 < argc)
			ca_file = argv[++i];
		else if (argv[i][0] == '-' || (argv[i][0] == '/' && argv[i][1] && !argv[i][2]))
			usage();
		else if (!url0)
			url0 = argv[i];
		else if (!file)
			file = argv[i];
		else
			usage();
	}
	if (!url0)
		usage();
	if (!ca_file)
		ca_file = DEFAULT_CA;
#ifdef __DJGPP__
	dos_sock_config();
#endif
	snprintf(url, sizeof url, "%s", url0);
	for (hops = 0;; hops++) {
		if (hops > 8)
			die("too many redirects");
		parse_url(url, &u);
		if (!quiet)
			fprintf(stderr, "Getting %s\n", url);
		connect_to(u.host, u.port, u.tls);
		status = request(&u, loc, sizeof loc, status_line, sizeof status_line);
		if ((status == 301 || status == 302 || status == 303 ||
		    status == 307 || status == 308) && *loc) {
			disconnect();
			join_url(&u, loc, next, sizeof next);
			snprintf(url, sizeof url, "%s", next);
			continue;
		}
		break;
	}
	if (status < 200 || status > 299) {
		disconnect();
		die("%s", status_line);
	}
	if (!file) {
		name_from(url, name, sizeof name);
		file = name;
	}
	if ((f = fopen(file, "wb")) == NULL)
		die("can't create %s: %s", file, strerror(errno));
	if (!quiet) {
		if (length_known)
			fprintf(stderr, "Saving to %s (%llu bytes)\n", file, (unsigned long long)length);
		else
			fprintf(stderr, "Saving to %s\n", file);
	}
	got = body(f, file);
	if (fclose(f) != 0)
		die("writing %s failed (disk full?)", file);
	disconnect();
	if (!quiet)
		fprintf(stderr, "\r%llu bytes saved to %s\n", (unsigned long long)got, file);
	return 0;
}
