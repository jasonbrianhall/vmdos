/* USB mass storage disks (blk.h): the driver is usb.cpp's (bulk-only
   transport, SCSI READ/WRITE(10)); this is its face for disk.c. Sticks come
   and go: a slot is kept for a disk that was unplugged (sectors() 0) and
   given to it again, or to another one (gen() changes). */
#include "blk.h"

int usb_msd_count(void);
u64 usb_msd_sectors(int i);
const char *usb_msd_name(int i);
u32 usb_msd_gen(int i);
int usb_msd_rw(int i, u64 lba, u32 count, void *buf, int write);

const struct blk_driver blk_usb = {
    "USB", 0, 0, 1, usb_msd_count, usb_msd_count, usb_msd_sectors, usb_msd_name, usb_msd_gen, usb_msd_rw,
};
