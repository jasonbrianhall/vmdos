/* DJGPP has no mmap: fsck_msdosfs then reads the FAT into memory instead. */
#ifndef COMPAT_MMAN_H
#define COMPAT_MMAN_H
#include <stddef.h>
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_NOCORE 0
#define MAP_FAILED ((void *)-1)

#define munmap(a, l) ((void)(a))
#endif
