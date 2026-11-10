/*
 * devio: the disk under fsck_msdosfs. Every open/lseek/read/write/pread/
 * pwrite/close in the FreeBSD sources goes through here (ext.h includes
 * this after the system headers). A name like "C:" is a DOS drive, read and
 * written by absolute sector (INT 21h AX=7305h); anything else is an
 * ordinary file (a disk image), which is how the Linux build is tested.
 * Offsets are 64-bit even where the C library's off_t is not (DJGPP).
 */
#ifndef DEVIO_H
#define DEVIO_H
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

typedef int64_t dev_off_t;

int dev_open(const char *name, int flags, ...);
int dev_close(int fd);
dev_off_t dev_lseek(int fd, dev_off_t off, int whence);
ssize_t dev_read(int fd, void *buf, size_t n);
ssize_t dev_write(int fd, const void *buf, size_t n);
ssize_t dev_pread(int fd, void *buf, size_t n, dev_off_t off);
ssize_t dev_pwrite(int fd, const void *buf, size_t n, dev_off_t off);
/* Set before dev_open: where in an image file the partition starts (bytes). */
extern dev_off_t dev_image_base;
extern int dev_image_base_set;	/* else: found from an MBR or GPT */

#define off_t dev_off_t
#define open dev_open
#define close(fd) dev_close(fd)
#define lseek(fd, o, w) dev_lseek(fd, o, w)
#define read(fd, b, n) dev_read(fd, b, n)
#define write(fd, b, n) dev_write(fd, b, n)
#define pread(fd, b, n, o) dev_pread(fd, b, n, o)
#define pwrite(fd, b, n, o) dev_pwrite(fd, b, n, o)
/* mmap would bypass this layer (and the image offset), so it always fails
   and fsck_msdosfs falls back to reading the FAT. */
#undef mmap
#define mmap(a, l, p, f, fd, o) ((void)(fd), MAP_FAILED)
#endif
