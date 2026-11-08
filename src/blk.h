/* Disk drivers: one blk_*.c(pp) each, for disk.c (C: on a real disk) and,
   through disk.c's disk_dev_*, VMHD (hd.c). A driver finds its disks when
   started and reads and writes them in 512-byte sectors. disk.c takes the
   drivers in the order of blk_drivers[] (disk.c), each disk as a disk
   number of its own. */
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "kernel.h"

struct blk_driver {
    const char *kind;                     /* "SATA", "IDE", "USB", "virtio", "NVMe": in messages, VMHD */
    const char *off;                      /* command-line option that skips it ("ahci=off"), 0: none */
    int dma;                              /* rw's buffer must be identity-mapped vmdos memory (disk.c bounces) */
    int hotplug;                          /* disks come and go: count() asked again later, sectors() 0 while gone */
    int (*start)(void);                   /* find the disks: how many (0 if none, or no such controller) */
    int (*count)(void);                   /* hotplug: how many disk slots now (0: as start said) */
    u64 (*sectors)(int i);
    const char *(*name)(int i);
    u32 (*gen)(int i);                    /* hotplug: changes when the disk in slot i changes (0: never) */
    int (*rw)(int i, u64 lba, u32 n, void *buf, int write);   /* n <= 128; 0 done, else failed */
    /* Optional, a write that runs on while vmdos goes on (disk.c's write-back):
       wstart takes it (n <= 128, buf copied at once): 0 under way, 1 can't
       now (rw it instead); wpoll: 0 still under way, 1 done, -1 failed; with
       wait, not 0 (it waits, and gives up after a while itself). One at a
       time; rw waits for it. */
    int (*wstart)(int i, u64 lba, u32 n, const void *buf);
    int (*wpoll)(int i, int wait);
};

extern const struct blk_driver blk_ahci, blk_ide, blk_virtio, blk_nvme, blk_usb;

#ifdef __cplusplus
}
#endif
