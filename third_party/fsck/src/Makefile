# fsck for vmdos: FreeBSD's fsck_msdosfs (sbin/fsck_msdosfs) on top of devio.c.
#   make            fsck      Linux build, checks image files (for testing)
#   make dos        FSCK.EXE  DOS (DJGPP, 32-bit DPMI), checks drives (C:)
SRCS = main.c check.c boot.c fat.c dir.c fsutil.c devio.c
HDRS = dosfs.h ext.h fsutil.h devio.h
CFLAGS = -std=gnu11 -O2 -Wall -Icompat -I.
DJGPP_CC ?= i586-pc-msdosdjgpp-gcc

fsck: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) -o $@ $(SRCS)

dos: FSCK.EXE
FSCK.EXE: $(SRCS) $(HDRS)
	$(DJGPP_CC) $(CFLAGS) -Icompat-dos -include compat-dos/bsdtypes.h -s -o $@ $(SRCS)

clean:
	rm -f fsck FSCK.EXE *.o
