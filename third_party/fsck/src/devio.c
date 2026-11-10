/* See devio.h. */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __DJGPP__
#include <dpmi.h>
#include <go32.h>
#include <sys/movedata.h>
#endif
#include "devio.h"
#undef off_t
#undef open
#undef close
#undef lseek
#undef read
#undef write
#undef pread
#undef pwrite

#define SECSZ 512
#define MAXFD 4

dev_off_t dev_image_base;
int dev_image_base_set;

static struct dev {
	int used;
	int drive;		/* DOS: 1 = A:, 3 = C:; 0 = an image file */
	int fd;			/* image file */
	int rdonly;
	dev_off_t base;		/* image: byte offset of the partition */
	dev_off_t pos;
} devs[MAXFD];

static struct dev *dev_of(int fd)
{
	if (fd < 0 || fd >= MAXFD || !devs[fd].used) { errno = EBADF; return NULL; }
	return &devs[fd];
}

/* --- sector I/O ------------------------------------------------------- */

#ifdef __DJGPP__
/*
 * FAT32-aware absolute disk read/write (INT 21h AX=7305h), which FreeDOS
 * supports on FAT12, FAT16 and FAT32. The data goes through DJGPP's
 * real-mode transfer buffer (__tb), a few sectors at a time.
 */
static int dos_rw(int drive, uint32_t lba, uint32_t n, void *buf, int wr)
{
	unsigned long tb = __tb;		/* linear address, below 1 MiB */
	uint32_t chunk = (_go32_info_block.size_of_transfer_buffer - 16) / SECSZ;
	if (chunk > 32) chunk = 32;
	while (n) {
		uint32_t k = n < chunk ? n : chunk;
		unsigned long data = tb + 16;
		unsigned char pkt[10];
		__dpmi_regs r;
		pkt[0] = lba; pkt[1] = lba >> 8; pkt[2] = lba >> 16; pkt[3] = lba >> 24;
		pkt[4] = k; pkt[5] = k >> 8;
		pkt[6] = data & 15; pkt[7] = 0;			/* offset */
		pkt[8] = (data >> 4) & 0xff; pkt[9] = data >> 12;	/* segment */
		dosmemput(pkt, sizeof pkt, tb);
		if (wr) dosmemput(buf, k * SECSZ, data);
		memset(&r, 0, sizeof r);
		r.x.ax = 0x7305;
		r.x.cx = 0xFFFF;
		r.h.dl = drive;
		r.x.si = wr ? 1 : 0;
		r.x.ds = tb >> 4;
		r.x.bx = tb & 15;
		__dpmi_int(0x21, &r);
		if (r.x.flags & 1) { errno = EIO; return -1; }
		if (!wr) dosmemget(data, k * SECSZ, buf);
		lba += k; n -= k; buf = (char *)buf + k * SECSZ;
	}
	return 0;
}

/* Flush and invalidate DOS's buffers, so it rereads what fsck changed. */
static void dos_reset(void)
{
	__dpmi_regs r;
	memset(&r, 0, sizeof r);
	r.h.ah = 0x0D;
	__dpmi_int(0x21, &r);
}
#endif

static int sec_rw(struct dev *d, uint32_t lba, uint32_t n, void *buf, int wr)
{
#ifdef __DJGPP__
	if (d->drive) return dos_rw(d->drive, lba, n, buf, wr);
#endif
	dev_off_t o = d->base + (dev_off_t)lba * SECSZ;
	size_t len = (size_t)n * SECSZ;
	ssize_t r;
#ifdef __DJGPP__	/* no pread; llseek for images over 2 GiB */
	if (llseek(d->fd, o, SEEK_SET) != o) return -1;
	r = wr ? write(d->fd, buf, len) : read(d->fd, buf, len);
#else
	r = wr ? pwrite(d->fd, buf, len, (off_t)o) : pread(d->fd, buf, len, (off_t)o);
#endif
	if (r != (ssize_t)len) { if (r >= 0) errno = EIO; return -1; }
	return 0;
}

/* Byte-addressed I/O on top of sectors: whole sectors go straight through,
   a partial sector at either end is read, patched and written back. */
static ssize_t dev_rw(struct dev *d, void *buf, size_t n, dev_off_t off, int wr)
{
	unsigned char tmp[SECSZ];
	char *p = buf;
	size_t left = n;
	if (off < 0) { errno = EINVAL; return -1; }
	if (wr && d->rdonly) { errno = EBADF; return -1; }
	while (left) {
		uint32_t lba = (uint32_t)(off / SECSZ);
		size_t in = (size_t)(off % SECSZ);
		if (in == 0 && left >= SECSZ) {
			uint32_t k = (uint32_t)(left / SECSZ);
			if (sec_rw(d, lba, k, p, wr)) return -1;
			off += (dev_off_t)k * SECSZ; p += k * SECSZ; left -= k * SECSZ;
			continue;
		}
		size_t m = SECSZ - in < left ? SECSZ - in : left;
		if (sec_rw(d, lba, 1, tmp, 0)) return -1;
		if (wr) {
			memcpy(tmp + in, p, m);
			if (sec_rw(d, lba, 1, tmp, 1)) return -1;
		} else
			memcpy(p, tmp + in, m);
		off += m; p += m; left -= m;
	}
	return (ssize_t)n;
}

/* --- partitioned images ---------------------------------------------- */

static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* A FAT boot sector: jump, 512-byte sectors, power-of-two cluster size. */
static int is_fat_boot(const unsigned char *b)
{
	unsigned spc = b[13];
	return (b[0] == 0xEB || b[0] == 0xE9) && b[510] == 0x55 && b[511] == 0xAA &&
	    b[11] == 0 && b[12] == 2 && spc && !(spc & (spc - 1)) && b[16];
}

static int fat_at(struct dev *d, uint32_t lba)
{
	unsigned char b[SECSZ];
	d->base = (dev_off_t)lba * SECSZ;
	return sec_rw(d, 0, 1, b, 0) == 0 && is_fat_boot(b);
}

/*
 * The image is a whole disk (vmdos's esp.img, /dev/sdX): use the first FAT
 * partition in its MBR, or in its GPT if the MBR is protective.
 */
static int find_partition(struct dev *d)
{
	unsigned char b[SECSZ];
	int i;
	d->base = 0;
	if (sec_rw(d, 0, 1, b, 0)) return -1;
	if (is_fat_boot(b) || b[510] != 0x55 || b[511] != 0xAA) return 0;
	for (i = 0; i < 4; i++) {
		const unsigned char *e = b + 446 + 16 * i;
		uint32_t start = le32(e + 8);
		if (e[4] == 0xEE) {		/* GPT */
			unsigned char h[SECSZ], ent[SECSZ];
			d->base = 0;
			if (sec_rw(d, 1, 1, h, 0) || memcmp(h, "EFI PART", 8)) break;
			uint32_t elba = le32(h + 72), n = le32(h + 80), esz = le32(h + 84);
			if (esz < 128 || esz > SECSZ || SECSZ % esz) break;
			for (uint32_t k = 0; k < n && k < 128; k++) {
				if (k % (SECSZ / esz) == 0) {
					d->base = 0;
					if (sec_rw(d, elba + k / (SECSZ / esz), 1, ent, 0)) return -1;
				}
				const unsigned char *g = ent + (k % (SECSZ / esz)) * esz;
				uint32_t first = le32(g + 32);
				if (le32(g + 36)) continue;	/* above 2 TiB */
				if (first && fat_at(d, first)) return 0;
			}
			break;
		}
		if (e[4] && start && fat_at(d, start)) return 0;
	}
	d->base = 0;
	return 0;	/* none found: leave it to the boot sector check */
}

/* --- the calls fsck_msdosfs makes ------------------------------------- */

int dev_open(const char *name, int flags, ...)
{
	int i;
	for (i = 0; i < MAXFD && devs[i].used; i++)
		;
	if (i == MAXFD) { errno = EMFILE; return -1; }
	struct dev *d = &devs[i];
	memset(d, 0, sizeof *d);
	d->rdonly = (flags & O_ACCMODE) == O_RDONLY;
	if (name[0] && name[1] == ':' && name[2] == '\0') {
#ifdef __DJGPP__
		int c = name[0] | 0x20;
		if (c < 'a' || c > 'z') { errno = ENOENT; return -1; }
		d->drive = c - 'a' + 1;
		dos_reset();
		/* Probe: if 7305h isn't there (or the drive isn't FAT), fail now. */
		unsigned char s[SECSZ];
		if (dos_rw(d->drive, 0, 1, s, 0)) { errno = ENODEV; return -1; }
#else
		errno = ENODEV;
		return -1;
#endif
	} else {
		d->fd = open(name, (d->rdonly ? O_RDONLY : O_RDWR)
#ifdef O_BINARY
		    | O_BINARY
#endif
		    );
		if (d->fd < 0) return -1;
		d->base = dev_image_base;
		if (!dev_image_base_set && find_partition(d)) { close(d->fd); return -1; }
		if (d->base)	/* after check.c's "** name" */
			printf(" (FAT partition at sector %lld)", (long long)(d->base / SECSZ));
	}
	d->used = 1;
	return i;
}

int dev_close(int fd)
{
	struct dev *d = dev_of(fd);
	if (!d) return -1;
#ifdef __DJGPP__
	if (d->drive) dos_reset();
	else
#endif
	close(d->fd);
	d->used = 0;
	return 0;
}

dev_off_t dev_lseek(int fd, dev_off_t off, int whence)
{
	struct dev *d = dev_of(fd);
	if (!d) return -1;
	if (whence == SEEK_CUR) off += d->pos;
	else if (whence != SEEK_SET) { errno = EINVAL; return -1; }
	if (off < 0) { errno = EINVAL; return -1; }
	return d->pos = off;
}

ssize_t dev_read(int fd, void *buf, size_t n)
{
	struct dev *d = dev_of(fd);
	ssize_t r;
	if (!d) return -1;
	if ((r = dev_rw(d, buf, n, d->pos, 0)) > 0) d->pos += r;
	return r;
}

ssize_t dev_write(int fd, const void *buf, size_t n)
{
	struct dev *d = dev_of(fd);
	ssize_t r;
	if (!d) return -1;
	if ((r = dev_rw(d, (void *)buf, n, d->pos, 1)) > 0) d->pos += r;
	return r;
}

ssize_t dev_pread(int fd, void *buf, size_t n, dev_off_t off)
{
	struct dev *d = dev_of(fd);
	return d ? dev_rw(d, buf, n, off, 0) : -1;
}

ssize_t dev_pwrite(int fd, const void *buf, size_t n, dev_off_t off)
{
	struct dev *d = dev_of(fd);
	return d ? dev_rw(d, (void *)buf, n, off, 1) : -1;
}
