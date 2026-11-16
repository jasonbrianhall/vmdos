/*
 * dos-res.c: the resolver library calls openbsd-compat's getrrsetbyname()
 * makes (SSHFP records, VerifyHostKeyDNS), answered "no": dos-sock.c's DNS
 * client only looks up addresses. Kept apart from the headers that declare
 * them (Watt-32's resolv.h).
 */
int h_errno;
char _res[4096];		/* struct __res_state: only ever looked at */

int
res_init(void)
{
	return -1;
}

int
res_query(const char *dname, int cls, int type, unsigned char *ans, int len)
{
	(void)dname; (void)cls; (void)type; (void)ans; (void)len;
	h_errno = 3;		/* NO_RECOVERY */
	return -1;
}

int
dn_expand(const unsigned char *msg, const unsigned char *eom,
    const unsigned char *src, char *dst, int len)
{
	(void)msg; (void)eom; (void)src; (void)dst; (void)len;
	return -1;
}

unsigned short
_getshort(const unsigned char *p)
{
	return (unsigned short)(p[0] << 8 | p[1]);
}

unsigned long
_getlong(const unsigned char *p)
{
	return (unsigned long)p[0] << 24 | (unsigned long)p[1] << 16 |
	    (unsigned long)p[2] << 8 | p[3];
}
