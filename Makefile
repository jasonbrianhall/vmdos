# vmdos: FreeDOS in virtual-8086 mode under a small 32-bit kernel.
#
#   make                 vmdos.elf (+ dos.img with RAMDISK=1; fetches FreeDOS on first use)
#   make iso             vmdos.iso: GRUB, boots on BIOS and UEFI
#   make run             QEMU, BIOS, -kernel/-initrd
#   make run-iso         QEMU, BIOS, from the ISO
#   make run-efi         QEMU, UEFI (OVMF), from the ISO
#   make efi             vmdos.efi: run straight from UEFI (no GRUB);
#                        both go in the same folder of the EFI system partition
#   make esp             esp.img: the EFI disk, also C: (EFI/BOOT/BOOTX64.EFI + FreeDOS),
#                        FAT32 (ESP_FAT=16: FAT16)
#   make run-efi-app     QEMU, UEFI (OVMF), booting esp.img (SATA)
#   make run-usb         QEMU, UEFI (OVMF), booting esp.img as a USB stick
#   make run-bios        QEMU, legacy BIOS (SeaBIOS), booting esp.img (GRUB in the MBR)
#   make net             rebuild the TCP/IP tools and VMDOSNET in net/bin with Open Watcom
#                        (WATCOM=/path, else fetched into .watcom/ on first use)
#   make watcom          just fetch Open Watcom into .watcom/
#
# FREEDOS=dir with KERNEL.SYS and COMMAND.COM (default: freedos/, fetched)
# EXTRA=dir whose contents are copied into C:\ too (games, tools)
# RAMDISK=1: also build the RAM disk dos.img and pack it (esp.img, the ISO,
#   make run) as a fallback / for c=ram. Off by default. C=ram implies it.
# DISK_MB=size of the RAM disk dos.img (default 64; at least 34 for FAT32)
# C=disk (default): C: is esp.img's FAT partition on an AHCI disk, so changes
#   are kept; ISO= files go to C:\ISOS (the first in CD drive 1). C=ram: C:
#   is dos.img in RAM (changes lost), ISO= files are loaded into RAM.
# FRESH=1: make esp.img anew (else it is updated, keeping what DOS changed)
# Every image also carries build/floppy.img, a 1.44M FreeDOS boot floppy:
#   with no C: (no disk with KERNEL.SYS vmdos can drive, no RAM disk dos.img)
#   vmdos boots it as A: rather than stopping.
# VERSION: shown by vmdos.efi and the kernel (default: git describe)
# CPU=486: a kernel that runs on a 486 (no CMOV etc.; built in build-486/).
#   Default 686 (Pentium Pro and later). The Voodoo 3 code is always 686:
#   voodoo3 needs a P6 anyway.

FREEDOS  ?= freedos
DISK_MB  ?= 64
C        ?= disk
RAMDISK  ?=
USE_RAM  := $(if $(filter 1 yes,$(RAMDISK))$(filter ram,$(C)),1)
RAM_IMG  := $(if $(USE_RAM),dos.img)
EXTRA    ?=
FB_W     ?= 640
FB_H     ?= 480
CPU      ?= 686
ifeq ($(CPU),486)
ARCH     := -march=i486 -mtune=i486
BUILD    := build-486
else ifeq ($(CPU),686)
ARCH     := -march=i686 -mtune=generic
BUILD    := build
else
$(error CPU=$(CPU): 486 or 686)
endif
VERSION  ?= $(shell git describe --tags --match 'v*' --always --dirty 2>/dev/null || echo unknown)
FLOPPY   := $(BUILD)/floppy.img

CC       := gcc
CFLAGS   := -m32 $(ARCH) -I$(BUILD) -ffreestanding -fno-builtin -fno-pic -fno-pie \
            -fno-stack-protector -fcf-protection=none -mgeneral-regs-only \
            -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
            -O2 -fno-strict-aliasing -fno-delete-null-pointer-checks --param=min-pagesize=0 -Wall -Wextra -Wno-unused-parameter -MMD
CXXFLAGS := $(filter-out -fno-delete-null-pointer-checks,$(CFLAGS)) -fno-delete-null-pointer-checks \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -std=gnu++17
# voodoo3: 86Box's Voodoo (src/voodoo/vid_voodoo*.c, as upstream) and the Voodoo 3 around it
VOODOO_OBJS := $(addprefix voodoo/,vid_voodoo.o vid_voodoo_banshee_blitter.o vid_voodoo_blitter.o \
              vid_voodoo_display.o vid_voodoo_fb.o vid_voodoo_fifo.o vid_voodoo_reg.o vid_voodoo_render.o \
              vid_voodoo_setup.o vid_voodoo_texture.o vid_svga.o vid_svga_render.o timer.o \
              v3card.o v3.o v3sched.o v3rom.o)
OBJS     := $(addprefix $(BUILD)/,boot.o cpu.o bootcon.o lib.o v86.o vdev.o bios.o video.o biosblob.o bootblob.o disk.o fat.o blk_ahci.o blk_ide.o blk_virtio.o balloon_virtio.o acpi.o pvpanic.o agent.o clip.o remote.o blk_nvme.o blk_storvsc.o blk_usb.o virtio.o vmbus.o hd.o joy.o serial.o ems.o usb.o pci.o xms.o mouse.o dpmi.o mememu.o cd.o atapi.o net.o ipxnet.o dhcpc.o nic_e1000.o nic_igb.o nic_igc.o nic_rtl8139.o nic_r8169.o nic_tg3.o nic_alx.o nic_atl1c.o nic_virtio.o nic_netvsc.o nic_tulip.o nic_ne2k.o fs9p.o nic_usb.o tcp9p.o ne2k.o sysinfo.o floppy.o hv.o hvvmx.o hvasm.o smp.o xbrz.o \
              audio.o sound.o gus.o sb/dsp.o sb/sbout.o sb/mpu.o sb/gmsynth.o sb/gmtables.o sb/fpmath.o \
              sb/opl.o sb/dbopl.o vpci.o es1371.o vgabios.o media.o $(VOODOO_OBJS))
# kdos (shell=kernel): DOSBox-X's DOS kernel and shell in the monitor
# (src/kdos, C++ with its own small libc; src/kdos_host.c is the monitor's
# side). It uses std::string/map/vector with libstdc++'s headers: string's
# templates are instantiated here (_GLIBCXX_ASSERTIONS turns off its extern
# templates), and the few library objects it needs (red-black trees, lists,
# dynamic_cast) come out of GCC's 32-bit libstdc++.a / libsupc++.a.
KDOS_SRCS := $(wildcard src/kdos/*.cpp)
KDOS_OBJS := $(patsubst src/kdos/%.cpp,$(BUILD)/kdos/%.o,$(KDOS_SRCS))
KDOS_RT   := $(addprefix $(BUILD)/kdos/rt/,tree.o list.o class_type_info.o dyncast.o si_class_type_info.o tinfo.o vmi_class_type_info.o)
KDOS_CXXFLAGS := -m32 $(ARCH) -O2 -fno-exceptions -frtti -fno-pic -fno-pie -fno-stack-protector -fcf-protection=none \
              -mno-sse -mno-mmx -mfpmath=387 -fno-threadsafe-statics -fno-use-cxa-atexit -std=gnu++17 \
              -fno-asynchronous-unwind-tables -fno-delete-null-pointer-checks -fno-strict-aliasing \
              -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -D_GLIBCXX_ASSERTIONS -w -I$(BUILD) -Isrc/kdos/compat -Isrc/kdos -MMD
LIBCXX_DIR := $(dir $(shell g++ -m32 -print-file-name=libstdc++.a))
$(KDOS_OBJS): $(BUILD)/version.h
$(BUILD)/kdos/%.o: src/kdos/%.cpp | $(BUILD)
	@mkdir -p $(BUILD)/kdos
	g++ $(KDOS_CXXFLAGS) -c $< -o $@
$(KDOS_RT): | $(BUILD)
	@mkdir -p $(BUILD)/kdos/rt
	cd $(BUILD)/kdos/rt && ar x $(LIBCXX_DIR)libstdc++.a tree.o list.o && \
	  ar x $(LIBCXX_DIR)libsupc++.a class_type_info.o dyncast.o si_class_type_info.o tinfo.o vmi_class_type_info.o
# one object, with the few names its libc shares with the monitor's renamed
$(BUILD)/kdos.o: $(KDOS_OBJS) $(KDOS_RT)
	ld -m elf_i386 -r -o $@ $^
	objcopy --redefine-sym vsnprintf=kdos_vsnprintf --redefine-sym snprintf=kdos_snprintf \
	        --redefine-sym labs=kdos_labs --redefine-sym machine=kdos_machine $@
OBJS += $(BUILD)/kdos_host.o $(BUILD)/kdos.o

# SBPRO's FM synth: dbopl's one-time table setup uses the x87 (opl_init saves
# and restores the FPU around it); everything that runs later is integer.
FPFLAGS  := $(filter-out -mgeneral-regs-only,$(CFLAGS)) -mfpmath=387 -mno-sse -mno-mmx
LIBGCC   := $(shell $(CC) -m32 -print-libgcc-file-name)

all: vmdos.elf $(RAM_IMG) $(FLOPPY)

# The version, for vmdos.efi and the kernel (rewritten only when it changes).
$(BUILD)/version.h: FORCE | $(BUILD)
	@echo '#define VMDOS_VERSION "$(VERSION)"' > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@
$(BUILD)/cpu.o $(BUILD)/bootcon.o $(BUILD)/agent.o: $(BUILD)/version.h

# Which CPU the top-level outputs (vmdos.elf and what's made from it) were
# built for: switching CPU= relinks them from that CPU's build directory.
.vmdos-cpu: FORCE
	@echo $(CPU) > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	g++ $(CXXFLAGS) -c $< -o $@

$(BUILD)/sb/%.o: src/sb/%.c | $(BUILD)
	@mkdir -p $(BUILD)/sb
	$(CC) $(CFLAGS) -c $< -o $@
$(BUILD)/sb/gmtables.o $(BUILD)/sb/fpmath.o: $(BUILD)/sb/%.o: src/sb/%.c | $(BUILD)
	@mkdir -p $(BUILD)/sb
	$(CC) $(FPFLAGS) -c $< -o $@
$(BUILD)/sb/%.o: src/sb/%.cpp | $(BUILD)
	@mkdir -p $(BUILD)/sb
	g++ $(filter-out -mgeneral-regs-only,$(CXXFLAGS)) -mfpmath=387 -mno-sse -mno-mmx -Wno-unused -Wno-switch -Wno-implicit-fallthrough -c $< -o $@

# The Voodoo code uses the x87 (v3.c saves the guest's state around it) and
# needs a Pentium Pro or later (CMOV); its own <string.h> etc. are voodoo/libc.
VOODOO_FLAGS := $(filter-out $(ARCH) -Wall -Wextra,$(FPFLAGS)) -march=i686 -mtune=generic \
              -DV3_CODEGEN -DVMDOS_V3 -DTEX_CACHE_MAX=16 -Isrc/voodoo/libc -Isrc/voodoo -w
$(BUILD)/voodoo/%.o: src/voodoo/%.c | $(BUILD)
	@mkdir -p $(BUILD)/voodoo
	$(CC) $(VOODOO_FLAGS) -c $< -o $@
# Its BIOS, built in when roms/voodoo3/3k12sd.rom is there.
V3ROM := $(wildcard roms/voodoo3/3k12sd.rom)
$(BUILD)/voodoo/v3rom.o: src/voodoo/v3rom.S $(V3ROM) | $(BUILD)
	@mkdir -p $(BUILD)/voodoo
	$(CC) -m32 $(if $(V3ROM),-DV3ROM='"$(V3ROM)"') -c $< -o $@

$(BUILD)/boot.o: src/boot.S | $(BUILD)
	$(CC) -m32 -DFB_W=$(FB_W) -DFB_H=$(FB_H) -c $< -o $@

$(BUILD)/bios.bin: src/bios.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/biosblob.o: src/biosblob.S $(BUILD)/bios.bin
	$(CC) -m32 -DBIOS_BIN='"$(BUILD)/bios.bin"' -c $< -o $@

$(BUILD)/bootblob.o: src/bootblob.S $(BUILD)/fat16.bin $(BUILD)/fat32lba.bin
	$(CC) -m32 -DFAT16_BIN='"$(BUILD)/fat16.bin"' -DFAT32_BIN='"$(BUILD)/fat32lba.bin"' -c $< -o $@

vmdos.elf: $(OBJS) src/linker.ld .vmdos-cpu
	ld -m elf_i386 -T src/linker.ld -o $@ $(OBJS) $(LIBGCC)

$(BUILD)/fat32lba.bin: boot/boot32lb.asm boot/magic.mac | $(BUILD)
	nasm -f bin -i boot/ $< -o $@
$(BUILD)/fat16.bin: boot/boot.asm boot/magic.mac | $(BUILD)
	nasm -f bin -DISFAT16 -i boot/ $< -o $@
$(BUILD)/fat12.bin: boot/boot.asm boot/magic.mac | $(BUILD)
	nasm -f bin -DISFAT12 -i boot/ $< -o $@

$(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM &:
	sh tools/fetch-freedos.sh $(FREEDOS)

# Rebuild C: when EXTRA names another folder or anything in it changes
# (a checksum of the listing: names may contain spaces, which make can't take).
$(BUILD)/extra.stamp: FORCE | $(BUILD)
	@{ echo "$(EXTRA)"; [ -z "$(EXTRA)" ] || find "$(EXTRA)" -printf '%p %s %T@\n' | sort; } | md5sum > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@
FORCE:

$(BUILD)/VMSPEED.COM: dos/vmspeed.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/DMESG.COM: dos/dmesg.asm dos/vmapi.inc | $(BUILD)
	nasm -f bin -i dos/ $< -o $@

$(BUILD)/LSPCI.COM: dos/lsdev.asm dos/vmapi.inc | $(BUILD)
	nasm -f bin -i dos/ -DLSPCI $< -o $@

$(BUILD)/LSUSB.COM: dos/lsdev.asm dos/vmapi.inc | $(BUILD)
	nasm -f bin -i dos/ -DLSUSB $< -o $@

$(BUILD)/VMSHOT.COM: dos/vmshot.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMRES.COM: dos/vmres.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMSB.COM: dos/vmsb.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMMIC.COM: dos/vmmic.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMHV.COM: dos/vmhv.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/REBOOT.COM: dos/reboot.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/POWEROFF.COM: dos/reboot.asm | $(BUILD)
	nasm -f bin -DPOWEROFF $< -o $@

$(BUILD)/hvasm.o: src/hvasm.S | $(BUILD)
	$(CC) -m32 -c $< -o $@

$(BUILD)/VMCD.SYS: dos/vmcd.asm dos/vmapi.inc | $(BUILD)
	nasm -f bin -i dos/ $< -o $@

$(BUILD)/VMCD.COM: dos/vmcdtool.asm dos/vmapi.inc | $(BUILD)
	nasm -f bin -i dos/ $< -o $@

$(BUILD)/VMPKT.COM: dos/vmpkt.asm dos/vmapi.inc | $(BUILD)
	nasm -f bin -i dos/ $< -o $@

$(BUILD)/PKTTEST.COM: tests/pkttest.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/SERTEST.COM: tests/sertest.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/IPXTEST.COM: tests/ipxtest.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMNIC.COM: dos/vmnic.asm dos/vmapi.inc | $(BUILD)
	nasm -f bin -i dos/ $< -o $@

$(BUILD)/VMIPX.COM: dos/vmipx.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMFD.COM: dos/vmfd.asm dos/vmapi.inc | $(BUILD)
	nasm -f bin -i dos/ $< -o $@

$(BUILD)/VMHD.SYS: dos/vmhd.asm dos/vmapi.inc | $(BUILD)
	nasm -f bin -i dos/ $< -o $@

$(BUILD)/VMHD.COM: dos/vmhdtool.asm dos/vmapi.inc | $(BUILD)
	nasm -f bin -i dos/ $< -o $@

$(BUILD)/SHSUCDX.COM: third_party/shsucd/shsucdx.nsm | $(BUILD)
	nasm -O9 -w-all -Di8086 -i third_party/shsucd/ $< -o $@   # 8086 build: assembles with any NASM

$(BUILD)/SUBST.COM: dos/subst.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMXMS.SYS: dos/vmxms.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMEMS.SYS: dos/vmems.asm | $(BUILD)
	nasm -f bin $< -o $@

# C:\ root: FreeDOS and the two configuration files; vmdos's tools and
# drivers go in C:\VMDOS (on the PATH).
VMDOS_FILES := $(BUILD)/VMXMS.SYS $(BUILD)/VMEMS.SYS $(BUILD)/VMCD.SYS $(BUILD)/VMCD.COM $(BUILD)/VMFD.COM $(BUILD)/VMHD.SYS $(BUILD)/VMHD.COM $(BUILD)/VMSPEED.COM $(BUILD)/VMSHOT.COM $(BUILD)/VMRES.COM $(BUILD)/VMSB.COM $(BUILD)/VMMIC.COM $(BUILD)/VMHV.COM $(BUILD)/DMESG.COM $(BUILD)/LSPCI.COM $(BUILD)/LSUSB.COM $(BUILD)/VMPKT.COM $(BUILD)/VMIPX.COM $(BUILD)/VMNIC.COM $(BUILD)/PKTTEST.COM $(BUILD)/IPXTEST.COM $(BUILD)/SERTEST.COM $(BUILD)/SHSUCDX.COM $(BUILD)/REBOOT.COM $(BUILD)/POWEROFF.COM \
               third_party/ctmouse/CTMOUSE.COM third_party/himemx/HIMEMX.EXE third_party/jemm/JEMM386.EXE \
               third_party/doslfn/DOSLFN.COM third_party/doslfn/CP437UNI.TBL \
               dos/bin/EDIT.EXE dos/bin/TREE.EXE dos/bin/ZCOPY.EXE $(BUILD)/SUBST.COM \
	       third_party/fsck/bin/FSCK.EXE \
               $(addprefix third_party/dosutils/bin/,ATTRIB.COM CHOICE.EXE DELTREE.COM FDISK.EXE FIND.EXE FORMAT.EXE LABEL.EXE MEM.EXE \
                 MKEYB.EXE MORE.EXE MOVE.EXE NANSI.SYS SORT.EXE XCOPY.EXE)
# On C: only (the boot floppy has no room): LESS (DJGPP, needs DPMI), ZIP, UNZIP
C_ONLY_FILES := $(addprefix third_party/dosutils/bin/,LESS.EXE ZIP.EXE UNZIP.EXE)
# FSCK (FreeBSD fsck_msdosfs, DJGPP): third_party/fsck/README.vmdos
C_ONLY_FILES += third_party/fsck/bin/FSCK.EXE
# Vim (DJGPP) and vi; its _VIMRC and a few runtime files (VIM74\) next to it
VIM_RT := $(shell cd third_party/vim/RT && find . -type f | sed 's|^./||' | sort)
C_ONLY_FILES += third_party/vim/bin/VIM.EXE third_party/vim/bin/VI.BAT
# GNU bash 5.3 (DJGPP, with fork and pipes for DOS): third_party/bash/README.vmdos.
# Built from source into deps/ (see "bash and the Unix tools" below); UNIX= leaves
# it and the tools out.
UNIX ?= 1
C_ONLY_FILES += $(if $(UNIX),deps/bash/BASH.EXE)
# Felix BASIC (DJGPP) in its own folder, C:\VMDOS\BASIC; BASIC.BAT (C:\VMDOS,
# on the PATH) starts it: third_party/felixbasic/README.vmdos
C_ONLY_FILES += third_party/felixbasic/bin/BASIC.BAT
FELIX_FILES  := third_party/felixbasic/bin/FELIXBAS.EXE third_party/felixbasic/README.vmdos third_party/felixbasic/LICENSE.md
C_ONLY_DEPS  := $(C_ONLY_FILES) $(addprefix third_party/vim/RT/,$(VIM_RT)) $(FELIX_FILES)
C_ONLY_MAP   := $(foreach f,$(VIM_RT),third_party/vim/RT/$(f)=VMDOS/$(f))
C_ONLY_MAP   += third_party/felixbasic/bin/FELIXBAS.EXE=VMDOS/BASIC/FELIXBAS.EXE \
                third_party/felixbasic/README.vmdos=VMDOS/BASIC/README.TXT third_party/felixbasic/LICENSE.md=VMDOS/BASIC/LICENSE.TXT
# The Unix tools for bash (ls, cp, grep, sed, find, ...: suckless sbase, DJGPP)
# in C:\BIN, last on DOS's PATH and first on bash's: third_party/sbase/README.vmdos
SBASE_BIN    := $(if $(UNIX),$(addprefix deps/sbase/,$(addsuffix .EXE,$(shell grep -v '^\#' third_party/sbase/TOOLS | tr a-z A-Z))))
C_ONLY_DEPS  += $(SBASE_BIN)
C_ONLY_MAP   += $(foreach f,$(SBASE_BIN),$(f)=BIN/$(notdir $(f)))
# GNU awk (DJGPP), as GAWK and AWK, in C:\BIN too: third_party/gawk/README.vmdos
GAWK_BIN     := $(if $(UNIX),deps/gawk/GAWK.EXE deps/gawk/AWK.EXE)
C_ONLY_DEPS  += $(GAWK_BIN)
C_ONLY_MAP   += $(foreach f,$(GAWK_BIN),$(f)=BIN/$(notdir $(f)))
# OpenSSH's ssh, sftp and scp (DJGPP, on vmdos's own TCP) in C:\VMDOS\NET:
# third_party/openssh/README.vmdos
C_ONLY_DEPS  += $(addprefix third_party/openssh/bin/,SSH.EXE SFTP.EXE SCP.EXE)
C_ONLY_MAP   += $(foreach f,SSH.EXE SFTP.EXE SCP.EXE,third_party/openssh/bin/$(f)=VMDOS/NET/$(f))
# FLT, Felix Terminal (SSH terminal: Sixel, Kitty, VT100) in C:\VMDOS\NET:
# third_party/flt/README.vmdos
C_ONLY_DEPS  += third_party/flt/FLT.EXE
C_ONLY_MAP   += third_party/flt/FLT.EXE=VMDOS/NET/FLT.EXE

# Felix Chirp
C_ONLY_DEPS  += third_party/flt/CHIRP.EXE
C_ONLY_MAP   += third_party/flt/CHIRP.EXE=VMDOS/CHIRP.EXE

# WGET (DJGPP, mbedTLS, ssh's socket layer: tools/build-wget.sh) in C:\VMDOS\NET
# when it's been built, with the certificate authorities it checks https against
WGET_EXE     := $(wildcard third_party/wget/bin/WGET.EXE)
# curl the same way (tools/build-curl.sh), with the same CAs
CURL_EXE     := $(wildcard third_party/curl/bin/CURL.EXE)
C_ONLY_DEPS  += $(CURL_EXE)
C_ONLY_MAP   += $(if $(CURL_EXE),$(CURL_EXE)=VMDOS/NET/CURL.EXE)
C_ONLY_DEPS  += $(WGET_EXE) third_party/cacert/CACERT.PEM
C_ONLY_MAP   += $(if $(WGET_EXE),$(WGET_EXE)=VMDOS/NET/WGET.EXE) third_party/cacert/CACERT.PEM=VMDOS/NET/CACERT.PEM
# The openssl command (OpenSSL 3, DJGPP, ssh's socket layer: tools/build-openssl.sh)
# in C:\VMDOS\NET with its OPENSSL.CNF: third_party/openssl/README.vmdos
OPENSSL_EXE  := $(wildcard third_party/openssl/bin/OPENSSL.EXE)
C_ONLY_DEPS  += $(OPENSSL_EXE) $(if $(OPENSSL_EXE),third_party/openssl/OPENSSL.CNF)
C_ONLY_MAP   += $(if $(OPENSSL_EXE),$(OPENSSL_EXE)=VMDOS/NET/OPENSSL.EXE third_party/openssl/OPENSSL.CNF=VMDOS/NET/OPENSSL.CNF)
# Lynx, the text web browser (DJGPP, OpenSSL, PDCurses, ssh's socket layer:
# tools/build-lynx.sh) in C:\VMDOS\NET with its LYNX.CFG: third_party/lynx/README.vmdos
LYNX_EXE     := $(wildcard third_party/lynx/bin/LYNX.EXE)
C_ONLY_DEPS  += $(LYNX_EXE) $(if $(LYNX_EXE),third_party/lynx/LYNX.CFG)
C_ONLY_MAP   += $(if $(LYNX_EXE),$(LYNX_EXE)=VMDOS/NET/LYNX.EXE third_party/lynx/LYNX.CFG=VMDOS/NET/LYNX.CFG)
# TCP/IP (IPv4 + IPv6) and the DOSBox IPX bridge, in C:\VMDOS\NET: built with
# Open Watcom on mTCP (make net), committed prebuilt in net/bin.
NET_FILES := net/bin/VMTCP.EXE net/bin/DHCP.EXE net/bin/PING.EXE net/bin/NSLOOKUP.EXE net/bin/VMIPXBOX.EXE \
             net/bin/VMDOSNET.EXE \
             dos/net/VMTCP.CFG     # the default settings (kept when esp.img is updated)
DOS_DEPS  := $(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM dos/FDCONFIG.SYS dos/AUTOEXEC.BAT dos/STARTNET.BAT dos/README.TXT $(VMDOS_FILES) $(NET_FILES) $(C_ONLY_DEPS)
DOS_FILES := $(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM dos/FDCONFIG.SYS dos/AUTOEXEC.BAT dos/STARTNET.BAT dos/README.TXT \
             $(foreach f,$(VMDOS_FILES) $(C_ONLY_FILES),$(f)=VMDOS/$(notdir $(f))) $(foreach f,$(NET_FILES),$(f)=VMDOS/NET/$(notdir $(f))) \
             $(C_ONLY_MAP)

# The fallback boot floppy (A:): FreeDOS and vmdos's tools, configured for A:.
FLOPPY_FILES := $(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM dos/floppy/FDCONFIG.SYS dos/floppy/AUTOEXEC.BAT dos/floppy/STARTNET.BAT \
                $(foreach f,$(VMDOS_FILES),$(f)=VMDOS/$(notdir $(f))) $(foreach f,$(NET_FILES),$(f)=VMDOS/NET/$(notdir $(f)))
$(FLOPPY): $(BUILD)/fat12.bin $(VMDOS_FILES) $(NET_FILES) $(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM dos/floppy/FDCONFIG.SYS \
           dos/floppy/AUTOEXEC.BAT dos/floppy/STARTNET.BAT tools/mkfloppy.py
	python3 tools/mkfloppy.py $@ $(BUILD)/fat12.bin $(FLOPPY_FILES)

dos.img: $(BUILD)/fat32lba.bin $(BUILD)/fat16.bin $(DOS_DEPS) tools/mkdisk.py $(BUILD)/extra.stamp
	python3 tools/mkdisk.py --boot16=$(BUILD)/fat16.bin $@ $(DISK_MB) $(BUILD)/fat32lba.bin $(DOS_FILES) \
	    $(if $(EXTRA),"--contents=$(EXTRA)")

# grub-mkrescue (Debian/Ubuntu) or grub2-mkrescue (Fedora/RHEL/openSUSE).
GRUB_MKRESCUE ?= $(firstword $(shell command -v grub-mkrescue grub2-mkrescue 2>/dev/null))

# ISO="game.iso disc2.iso": CD-ROM images (no spaces in the names). C=disk:
# copied to C:\ISOS under 8.3 names, the first one in drive 1 at boot (cd=);
# VMCD 1 C:\ISOS\OTHER.ISO changes the disc while running. C=ram: held in
# RAM as boot modules, one drive each; VMCD 1 2 swaps.
ISO_MB   := $(if $(ISO),$(shell du -cm $(ISO) | tail -1 | cut -f1),0)
space    := $(empty) $(empty)
iso83     = $(shell n=$$(basename "$(1)"); b=$${n%.*}; echo "$$b" | tr a-z A-Z | tr -cd 'A-Z0-9_-' | cut -c1-8).ISO
ISO_C    := $(foreach f,$(ISO),$(f)=ISOS/$(call iso83,$(f)))
ISO_RAM  := $(if $(filter ram,$(C)),$(ISO))
KARGS_ALL = $(strip $(KARGS) $(if $(filter ram,$(C)),c=ram,$(if $(ISO),cd=/ISOS/$(call iso83,$(firstword $(ISO))))))

iso: vmdos.iso
# KARGS: kernel command line for the ISO's GRUB entry (e.g. KARGS="debug=2 audio=ac97").
$(BUILD)/grub.cfg: grub.cfg FORCE | $(BUILD)
	@sed 's|multiboot /boot/vmdos.elf|multiboot /boot/vmdos.elf $(KARGS_ALL)|' $< > $@.new
	@$(if $(USE_RAM),true,sed -i 's|module /boot/dos.img.gz dos.img|module /boot/floppy.img floppy.img|' $@.new)
	@for f in $(ISO_RAM); do n=$$(basename $$f); sed -i "/^}/i\    module /boot/cd/$$n $$n" $@.new; done
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@

vmdos.iso: vmdos.elf $(RAM_IMG) $(FLOPPY) $(BUILD)/grub.cfg $(ISO_RAM)
	@test -n "$(GRUB_MKRESCUE)" || { echo "grub-mkrescue / grub2-mkrescue not found."; \
	  echo "  Debian/Ubuntu: sudo apt install grub-common grub-pc-bin grub-efi-amd64-bin xorriso mtools"; \
	  echo "  Fedora:        sudo dnf install grub2-tools-extra grub2-pc-modules grub2-efi-x64-modules xorriso mtools"; \
	  echo "Or skip GRUB: make efi / make esp"; exit 1; }
	rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	cp vmdos.elf $(BUILD)/iso/boot/
	$(if $(USE_RAM),gzip -9c dos.img > $(BUILD)/iso/boot/dos.img.gz,cp $(FLOPPY) $(BUILD)/iso/boot/floppy.img)
	$(if $(MEMDISK),cp $(FLOPPY) $(BUILD)/iso/boot/floppy.img; cp $(MEMDISK) $(BUILD)/iso/boot/memdisk)
	cp $(BUILD)/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(if $(ISO_RAM),mkdir -p $(BUILD)/iso/boot/cd && cp $(ISO_RAM) $(BUILD)/iso/boot/cd/)
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/iso

# ---- UEFI application (gnu-efi): efi/loader.c with the kernel embedded ----
EFI_INC  ?= $(firstword $(wildcard /usr/include/efi))
EFI_CRT0 ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'crt0-efi-x86_64.o' -o -path '*gnuefi*x64*crt0.o' \) 2>/dev/null))
EFI_LDS  ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'elf_x86_64_efi.lds' -o -name 'elf_x64_efi.lds' \) 2>/dev/null))
EFI_LIBS ?= $(shell find /usr/lib /usr/lib64 \( -name libgnuefi.a -o -name libefi.a \) 2>/dev/null | sort -u)
EFI_CFLAGS := -I$(EFI_INC) -I$(EFI_INC)/x86_64 -I$(BUILD)/efi -I$(BUILD) -fpic -ffreestanding -fno-stack-protector \
              -fno-stack-check -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -O2 -Wall

efi: vmdos.efi $(RAM_IMG) $(FLOPPY)

$(BUILD)/efi/kernel.bin $(BUILD)/efi/kernel_layout.h: vmdos.elf
	mkdir -p $(BUILD)/efi
	objcopy -O binary vmdos.elf $(BUILD)/efi/kernel.bin
	@nm vmdos.elf | awk '/ __kernel_start$$/ {print "#define KERNEL_BASE 0x" $$1} \
	                     / __image_end$$/    {print "#define KERNEL_END 0x" $$1} \
	                     / _start$$/         {print "#define KERNEL_ENTRY 0x" $$1}' > $(BUILD)/efi/kernel_layout.h
	@test $$(grep -c define $(BUILD)/efi/kernel_layout.h) = 3 || { echo "kernel layout symbols missing"; exit 1; }

vmdos.efi: efi/loader.c efi/tramp.S efi/image.S $(BUILD)/efi/kernel.bin $(BUILD)/efi/kernel_layout.h $(BUILD)/version.h
	@test -n "$(EFI_CRT0)" -a -n "$(EFI_LDS)" || { echo "gnu-efi not found (Debian/Ubuntu: gnu-efi, Fedora: gnu-efi-devel)"; exit 1; }
	gcc $(EFI_CFLAGS) -c efi/loader.c -o $(BUILD)/efi/loader.o
	gcc -c efi/tramp.S -o $(BUILD)/efi/tramp.o
	gcc -c -DKERNEL_BIN='"$(BUILD)/efi/kernel.bin"' efi/image.S -o $(BUILD)/efi/image.o
	ld -nostdlib -znocombreloc -shared -Bsymbolic -T $(EFI_LDS) $(EFI_CRT0) \
	   $(BUILD)/efi/loader.o $(BUILD)/efi/tramp.o $(BUILD)/efi/image.o $(EFI_LIBS) $(EFI_LIBS) -o $(BUILD)/efi/vmdos.so
	objcopy -j .text -j .sdata -j .data -j .rodata -j .dynamic -j .dynsym -j .rel -j .rela \
	        -j '.rel.*' -j '.rela.*' -j .reloc -O efi-app-x86_64 --subsystem=10 $(BUILD)/efi/vmdos.so $@

# esp.img: the EFI system partition, also C:, bootable both ways:
#  - UEFI: EFI/BOOT/BOOTX64.EFI is GRUB (x86_64-efi), which chainloads
#    EFI/vmdos/vmdos.efi (KARGS in EFI/vmdos/vmdos.cfg, dos.img next to it
#    with RAMDISK=1);
#  - legacy BIOS: GRUB (i386-pc) in the MBR and the gap before the
#    partition, which boots boot/vmdos.elf (Multiboot).
# Both read boot/grub/grub.cfg, which picks the entry for the platform and
# then reads boot/grub/custom.cfg if you made one (your own menu entries;
# never overwritten; with it, the menu waits 5 s). GRUB=0 (or no GRUB tools):
# UEFI only, vmdos.efi as BOOTX64.EFI. Updated in place unless FRESH=1.
ESP_FREE ?= 256
# ESP_FAT=32 (default): esp.img's partition is FAT32, its clusters as big as
# its size allows (16 KiB at 2000 MB), so it can be grown to fill a big USB
# stick and hold files past 2 GiB. ESP_FAT=16: FAT16 (at most ~4 GB), as
# before, for DOSes that don't know FAT32 (MS-DOS 6.22 booting from C:).
ESP_FAT  ?= 32
EXTRA_MB := $(if $(EXTRA),$(shell du -sm "$(EXTRA)" | cut -f1),0)
ESP_MB   ?= $(shell echo $$(( $(DISK_MB) + $(EXTRA_MB) + $(ISO_MB) + $(ESP_FREE) )))
GRUB_MKIMAGE ?= $(firstword $(shell command -v grub-mkimage grub2-mkimage 2>/dev/null))
GRUB_LIB ?= $(firstword $(wildcard /usr/lib/grub /usr/share/grub2 /usr/lib64/grub))
GRUB_OK  := $(and $(GRUB_MKIMAGE),$(wildcard $(GRUB_LIB)/i386-pc/boot.img),$(wildcard $(GRUB_LIB)/x86_64-efi/normal.mod))
GRUB     ?= $(if $(GRUB_OK),1,0)
# BIOS/CSM menu: "FreeDOS from C:, booted normally (no vmdos)" chainloads
# the partition's own FreeDOS boot sector (the one mkdisk.py writes), so C:
# boots as on a plain PC; the vmdos drivers in FDCONFIG.SYS stay out of memory.
# syslinux's memdisk (Debian/Ubuntu syslinux-common, Fedora syslinux): the
# BIOS/CSM menu's last entry boots floppy.img on the bare machine with it
# (real FreeDOS, no vmdos). Not found: no such entry.
MEMDISK  ?= $(firstword $(wildcard /usr/lib/syslinux/memdisk /usr/share/syslinux/memdisk /usr/lib/syslinux/bios/memdisk /usr/lib/syslinux/modules/bios/memdisk))
# linux16 for memdisk: its own module upstream (Debian, Ubuntu, Arch), part
# of linux.mod on Fedora/RHEL (no linux16.mod there). The entry tries
# linux16 and falls back to linux / initrd.
GRUB_LINUX16 = $(basename $(notdir $(wildcard $(GRUB_LIB)/i386-pc/linux16.mod $(GRUB_LIB)/i386-pc/linux.mod)))
MEMDISK_ENTRY = echo '  menuentry "FreeDOS from the built-in floppy, booted normally (no vmdos)" {'; \
	echo '    if linux16 /boot/memdisk floppy; then initrd16 /EFI/vmdos/floppy.img'; \
	echo '    else linux /boot/memdisk floppy; initrd /EFI/vmdos/floppy.img; fi'; echo '  }';
GRUB_MODS := part_msdos part_gpt fat normal configfile search search_fs_file test echo sleep chain multiboot boot \
             video video_fb all_video gfxterm videoinfo read
VMDOS_EFI_DIR := $(if $(filter 1,$(GRUB)),EFI/vmdos,EFI/BOOT)
# Rebuild esp.img when what goes on it changes (RAM disk on/off, ISO list, GRUB).
$(BUILD)/esp.opts: FORCE | $(BUILD)
	@echo '$(USE_RAM) $(ISO_C) $(GRUB) $(ESP_FAT) $(OEM_PATH) $(ESP_TYPE) $(MEMDISK)' > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@
$(BUILD)/vmdos.cfg: FORCE | $(BUILD)
	@echo '$(KARGS_ALL)' > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@
$(BUILD)/grub/early.cfg: | $(BUILD)
	@mkdir -p $(BUILD)/grub
	printf 'search --no-floppy --file --set=root /boot/vmdos.elf\nset prefix=($$root)/boot/grub\nconfigfile $$prefix/grub.cfg\n' > $@
# Rebuild the GRUB images when the modules they carry change.
$(BUILD)/grub/mods: FORCE | $(BUILD)
	@mkdir -p $(BUILD)/grub
	@echo '$(GRUB_MODS)' > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@
$(BUILD)/grub/core.img: $(BUILD)/grub/early.cfg $(BUILD)/grub/mods
	$(GRUB_MKIMAGE) -O i386-pc -d $(GRUB_LIB)/i386-pc -o $@ -c $< -p /boot/grub biosdisk vbe vga $(GRUB_LINUX16) $(GRUB_MODS)
$(BUILD)/grub/BOOTX64.EFI: $(BUILD)/grub/early.cfg $(BUILD)/grub/mods
	$(GRUB_MKIMAGE) -O x86_64-efi -d $(GRUB_LIB)/x86_64-efi -o $@ -c $< -p /boot/grub efi_gop efi_uga $(GRUB_MODS)
# The fixed choices in the boot menu's screen resolution submenu.
GRUB_RES ?= 640x480 800x600 1024x768 1280x1024 1280x720 1920x1080
VGA_TITLE_ask := Ask: list the modes this screen has
# BIOS/CSM boots: GRUB sets the screen mode (gfxpayload), so vga= from KARGS
# is turned into that here; vga=N (a UEFI mode number) has no meaning there.
GRUB_ASK = echo '      set pager=1'; echo '      videoinfo'; echo '      set pager=0'; \
	echo '      echo "Size to use, as WIDTHxHEIGHT (e.g. 1024x768), then Enter (Enter alone: $(FB_W)x$(FB_H)):"'; \
	echo '      read vmdos_res'; \
	echo '      if [ -n "$${vmdos_res}" ]; then set gfxpayload=$${vmdos_res}x32$(comma)$${vmdos_res}; fi';
VGA_ARG := $(firstword $(patsubst vga=%,%,$(filter vga=%,$(KARGS_ALL))))
bios_vga = $(if $(filter ask,$(1)),$(GRUB_ASK),$(if $(filter max,$(1)),echo '      set gfxpayload=auto';,$(if $(findstring x,$(1)),echo '      set gfxpayload=$(1)x32$(comma)$(1)';)))
VGA_TITLE_max := Largest (usually the panel native size)
# video=vbe in KARGS (the card's own VGA BIOS, BIOS/CSM boots only): GRUB
# leaves the card in text mode instead of setting a framebuffer mode.
bios_video = $(if $(filter video=vbe,$(KARGS_ALL)),echo '    set gfxpayload=text';,$(call bios_vga,$(VGA_ARG)))
$(BUILD)/grub/grub.cfg: FORCE | $(BUILD)
	@mkdir -p $(BUILD)/grub
	@{ echo '# vmdos boot menu (made by make esp: changes here are overwritten;'; \
	   echo '# put your own entries in /boot/grub/custom.cfg)'; \
	   echo 'insmod all_video'; \
	   echo 'set default=0'; \
	   echo 'set timeout=5'; \
	   echo 'if [ "$$grub_platform" = "efi" ]; then'; \
	   echo '  menuentry "FreeDOS (vmdos)" {'; \
	   echo '    chainloader /EFI/vmdos/vmdos.efi $(KARGS_ALL)'; \
	   echo '  }'; \
	   echo '  menuentry "FreeDOS (vmdos) noxms" {'; \
	   echo '    chainloader /EFI/vmdos/vmdos.efi $(KARGS_ALL) noxms'; \
	   echo '  }'; \
	   echo '  submenu "FreeDOS (vmdos), screen resolution" {'; \
	   $(foreach v,ask max $(GRUB_RES),echo '    menuentry "$(or $(VGA_TITLE_$(v)),$(v))" {'; \
	     echo '      chainloader /EFI/vmdos/vmdos.efi vga=$(v) $(KARGS_ALL)'; echo '    }';) \
	   echo '  }'; \
           echo '  menuentry "FreeDOS (vmdos) vmhv mode (for games and operating systems that insist on owning memory [aka Windows 98])" {'; \
           echo '    chainloader /EFI/vmdos/vmdos.efi $(KARGS_ALL) vmhv'; \
           echo '  }'; \
	   echo '  menuentry "FreeDOS (vmdos) with Voodoo 3" {'; \
	   echo '    chainloader /EFI/vmdos/vmdos.efi $(KARGS_ALL) voodoo3'; \
	   echo '  }'; \
	   echo '  menuentry "FreeDOS (vmdos), video card'"'"'s own BIOS (VBE, if its ROM has one), pick the card" {'; \
	   echo '    chainloader /EFI/vmdos/vmdos.efi $(filter-out video=% display=%,$(KARGS_ALL)) video=vbe display=ask'; \
	   echo '  }'; \
	   echo 'else'; \
	   echo '  menuentry "FreeDOS (vmdos)" {'; \
	   echo '    multiboot /boot/vmdos.elf $(KARGS_ALL)'; \
	   $(bios_video) \
	   echo '    module /EFI/vmdos/$(if $(USE_RAM),dos.img dos.img,floppy.img floppy.img)'; \
	   echo '  }'; \
	   echo '  menuentry "FreeDOS (vmdos) noxms" {'; \
	   echo '    multiboot /boot/vmdos.elf $(KARGS_ALL) noxms'; \
	   $(bios_video) \
	   echo '    module /EFI/vmdos/$(if $(USE_RAM),dos.img dos.img,floppy.img floppy.img)'; \
	   echo '  }'; \
	   echo '  submenu "FreeDOS (vmdos), screen resolution" {'; \
	   echo '    menuentry "$(VGA_TITLE_ask)" {'; \
	   echo '      multiboot /boot/vmdos.elf $(KARGS_ALL)'; \
	   $(GRUB_ASK) \
	   echo '      module /EFI/vmdos/$(if $(USE_RAM),dos.img dos.img,floppy.img floppy.img)'; echo '    }'; \
	   echo '    menuentry "$(VGA_TITLE_max)" {'; \
	   echo '      multiboot /boot/vmdos.elf $(KARGS_ALL)'; echo '      set gfxpayload=auto'; \
	   echo '      module /EFI/vmdos/$(if $(USE_RAM),dos.img dos.img,floppy.img floppy.img)'; echo '    }'; \
	   $(foreach r,$(GRUB_RES),echo '    menuentry "$(r)" {'; \
	     echo '      multiboot /boot/vmdos.elf $(KARGS_ALL)'; echo '      set gfxpayload=$(r)x32,$(r)'; \
	     echo '      module /EFI/vmdos/$(if $(USE_RAM),dos.img dos.img,floppy.img floppy.img)'; echo '    }';) \
	   echo '  }'; \
	   echo '  menuentry "FreeDOS (vmdos) with Voodoo 3" {'; \
	   echo '    multiboot /boot/vmdos.elf $(KARGS_ALL) voodoo3'; \
	   $(bios_video) \
	   echo '    module /EFI/vmdos/$(if $(USE_RAM),dos.img dos.img,floppy.img floppy.img)'; \
	   echo '  }'; \
	   echo '  menuentry "FreeDOS (vmdos), video card'"'"'s own BIOS (VBE, no scaling)" {'; \
	   echo '    multiboot /boot/vmdos.elf $(filter-out video=% vga=%,$(KARGS_ALL)) video=vbe'; \
	   echo '    set gfxpayload=text'; \
	   echo '    module /EFI/vmdos/$(if $(USE_RAM),dos.img dos.img,floppy.img floppy.img)'; \
	   echo '  }'; \
	   echo '  menuentry "FreeDOS (vmdos), video card'"'"'s own BIOS (VBE), pick the card" {'; \
	   echo '    multiboot /boot/vmdos.elf $(filter-out video=% vga=% display=%,$(KARGS_ALL)) video=vbe display=ask'; \
	   echo '    set gfxpayload=text'; \
	   echo '    module /EFI/vmdos/$(if $(USE_RAM),dos.img dos.img,floppy.img floppy.img)'; \
	   echo '  }'; \
	   echo '  menuentry "FreeDOS from C:, booted normally (no vmdos)" {'; \
	   echo '    chainloader +1'; \
	   echo '  }'; \
	   $(if $(MEMDISK),$(MEMDISK_ENTRY)) \
	   echo 'fi'; \
	   echo 'if [ -f $$prefix/custom.cfg ]; then source $$prefix/custom.cfg; fi'; } > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@
# OEM_PATH=1 (default): the same boot program also as
# EFI/Microsoft/Boot/bootmgfw.efi, for firmware (some InsydeH2O Acer/HP/Lenovo
# laptops) that ignores boot entries and only starts the Windows Boot Manager
# path or EFI/BOOT/BOOTX64.EFI. vmdos.efi finds dos.img / vmdos.cfg in
# EFI/vmdos or EFI/BOOT when they aren't next to it; GRUB searches for its files.
OEM_PATH ?= 1
# ESP_TYPE: esp.img's MBR partition type. 0C (FAT32 LBA, active) with GRUB,
# as on Windows and Rufus BIOS+UEFI sticks: some InsydeH2O and other CSMs
# won't legacy-boot a disk whose active partition is EF, while UEFI firmware
# reads EFI/BOOT from any FAT partition. EF without GRUB (UEFI only).
ESP_TYPE ?= $(if $(filter 1,$(GRUB)),$(if $(filter 16,$(ESP_FAT)),0E,0C),EF)
ESP_OEM_FILE = $(if $(filter 1,$(OEM_PATH)),$(if $(filter 1,$(GRUB)),$(BUILD)/grub/BOOTX64.EFI,vmdos.efi)=EFI/Microsoft/Boot/bootmgfw.efi)
ESP_GRUB_DEPS  := $(if $(filter 1,$(GRUB)),vmdos.elf $(BUILD)/grub/core.img $(BUILD)/grub/BOOTX64.EFI $(BUILD)/grub/grub.cfg)
ESP_GRUB_FILES  = $(if $(filter 1,$(GRUB)),$(BUILD)/grub/BOOTX64.EFI=EFI/BOOT/BOOTX64.EFI vmdos.elf=boot/vmdos.elf \
                    $(BUILD)/grub/grub.cfg=boot/grub/grub.cfg $(if $(MEMDISK),$(MEMDISK)=boot/memdisk) --bios=$(GRUB_LIB)/i386-pc/boot.img$(comma)$(BUILD)/grub/core.img)
esp: esp.img
esp.img: vmdos.efi $(RAM_IMG) $(FLOPPY) $(DOS_DEPS) $(BUILD)/fat16.bin $(BUILD)/fat32lba.bin $(BUILD)/vmdos.cfg $(BUILD)/esp.opts $(BUILD)/extra.stamp tools/mkdisk.py $(ISO) $(ESP_GRUB_DEPS)
	$(if $(filter 1,$(GRUB)),,@echo "esp.img: UEFI only (GRUB=0, or grub-mkimage / GRUB's i386-pc and x86_64-efi modules not found)")
	python3 tools/mkdisk.py $(if $(FRESH),,--update) $(filter --bios=%,$(ESP_GRUB_FILES)) --type=$(ESP_TYPE) $(if $(filter 16,$(ESP_FAT)),--boot16=$(BUILD)/fat16.bin) $@ $(ESP_MB) $(BUILD)/fat32lba.bin \
	    $(DOS_FILES) vmdos.efi=$(VMDOS_EFI_DIR)/$(if $(filter 1,$(GRUB)),vmdos.efi,BOOTX64.EFI) \
	    $(if $(USE_RAM),dos.img=$(VMDOS_EFI_DIR)/dos.img) $(FLOPPY)=$(VMDOS_EFI_DIR)/floppy.img $(BUILD)/vmdos.cfg=$(VMDOS_EFI_DIR)/vmdos.cfg \
	    $(filter-out --bios=%,$(ESP_GRUB_FILES)) $(ESP_OEM_FILE) $(ISO_C) $(if $(EXTRA),"--contents=$(EXTRA)")
	$(if $(USE_RAM),,@MTOOLS_SKIP_CHECK=1 mdel -i $@@@1M ::/EFI/BOOT/dos.img ::/EFI/vmdos/dos.img 2>/dev/null || true)
	@touch $@

OVMF     ?= $(firstword $(wildcard /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/qemu/OVMF.fd))
QDISPLAY ?=
QEMU_MEM ?= $(shell echo $$(( 512 + $(if $(ISO_RAM),$(ISO_MB),0) * 2 )))
# Sound card QEMU gives the machine: SOUND=hda (default), ac97, sb (a real SB16
# for vmdos to play through), or none. AUDIODEV: pa (PulseAudio / PipeWire),
# alsa, sdl, or wav (writes vmdos.wav).
SOUND    ?= hda
AUDIODEV ?= pa
comma    := ,
QAUDIO   := -audiodev $(if $(filter wav,$(AUDIODEV)),wav$(comma)path=vmdos.wav,$(AUDIODEV)),id=snd0
QSOUND_hda  := $(QAUDIO) -device intel-hda -device hda-duplex,audiodev=snd0
QSOUND_ac97 := $(QAUDIO) -device AC97,audiodev=snd0
QSOUND_sb   := $(QAUDIO) -device sb16,audiodev=snd0
QSOUND_none :=
# USB=1: keyboard and mouse on an xHCI controller (as on many UEFI PCs).
QUSB     := $(if $(filter 1,$(USB)),-device qemu-xhci -device usb-kbd -device usb-mouse)
# USBHOST=0079:0011 (vendor:product, as lsusb shows it; several with spaces):
# hands the PC's own USB devices (a gamepad ...) to the guest on an xHCI
# controller. QEMU needs access to /dev/bus/usb (root, or a udev rule).
comma    := ,
QUSBHOST := $(if $(USBHOST),-device qemu-xhci$(comma)id=hostxhci $(foreach d,$(USBHOST),-device usb-host$(comma)bus=hostxhci.0$(comma)vendorid=0x$(word 1,$(subst :, ,$(d)))$(comma)productid=0x$(word 2,$(subst :, ,$(d)))))
# NET=lan: the network card on a LAN shared by every QEMU started with
# NET=lan on this PC (UDP multicast), each with its own MAC: two of them play
# IPX games (VMPKT, VMIPX). Default: QEMU's user-mode network. NIC=e1000
# (default), e1000e, igb, rtl8139, virtio-net-pci, or usb (a USB Ethernet adapter, CDC-ECM, on
# an xHCI controller).
NIC      ?= e1000
QNETDEV_lan  := -netdev socket,id=vmnet,mcast=230.0.0.1:1234
QNETDEV_user := -netdev user,id=vmnet
QNETMAC  := $(if $(filter lan,$(NET)),$(comma)mac=52:54:00$(shell od -An -N3 -tx1 /dev/urandom | tr ' ' ':' | tr -d '\n'))
QNET     := -nic none $(QNETDEV_$(or $(NET),user)) $(if $(filter usb,$(NIC)),-device qemu-xhci$(comma)id=netxhci -device usb-net$(comma)netdev=vmnet$(comma)bus=netxhci.0,-device $(NIC)$(comma)netdev=vmnet)$(QNETMAC)
# KVM when /dev/kvm is usable, else plain emulation (ACCEL= to override)
ACCEL ?= -accel kvm -accel tcg
# SHARE=dir: that folder as virtio-9p tag "share" (SHARE_TAG=...): VMDOSNET V: share
QSHARE   := $(if $(SHARE),-virtfs local$(comma)path=$(SHARE)$(comma)mount_tag=$(or $(SHARE_TAG),share)$(comma)security_model=none)
QEMU_ARGS ?= $(ACCEL) -m $(QEMU_MEM) -serial stdio $(QNET) $(QSOUND_$(SOUND)) $(QUSB) $(QUSBHOST) $(QDISPLAY) $(QSHARE)
# C=disk: esp.img on an AHCI controller (C:), after the CD in the boot order.
BOOTDISK ?= 1
# DISK=virtio: on a virtio-blk disk instead (DISK=ide: IDE, DISK=nvme: NVMe).
DISK ?= ahci
QESP_ahci   = -device ahci,id=ahci -drive if=none,id=cdisk,format=raw,file=esp.img \
              -device ide-hd,drive=cdisk,bus=ahci.0,bootindex=$(BOOTDISK)
QESP_ide    = -drive if=none,id=cdisk,format=raw,file=esp.img -device ide-hd,drive=cdisk,bootindex=$(BOOTDISK)
QESP_virtio = -drive if=none,id=cdisk,format=raw,file=esp.img -device virtio-blk-pci,drive=cdisk,bootindex=$(BOOTDISK)
QESP_nvme   = -drive if=none,id=cdisk,format=raw,file=esp.img -device nvme,drive=cdisk,serial=vmdos,bootindex=$(BOOTDISK)
QESP  = $(QESP_$(DISK))
QDISK = $(if $(filter ram,$(C)),,$(QESP))
DISK_DEP := $(if $(filter ram,$(C)),,esp.img)

run: vmdos.elf $(RAM_IMG) $(FLOPPY) $(DISK_DEP)
	qemu-system-i386 -kernel vmdos.elf -initrd $(subst $(space),$(comma),$(strip $(if $(USE_RAM),dos.img,$(FLOPPY)) $(ISO_RAM))) \
	    -append "$(KARGS_ALL)" $(QDISK) $(QEMU_ARGS)
# VMHV (src/hv.c): boot straight into the AMD-V virtual machine. Needs a
# QEMU whose TCG applies nested paging to guests that haven't turned
# paging on (current QEMU; 8.2 doesn't), or KVM on an AMD host with
# nested virtualization. QEMU=path picks the binary; VMHV=2 types menu choice 2.
QEMU ?= qemu-system-i386
run-hv: vmdos.elf $(RAM_IMG) $(FLOPPY) $(DISK_DEP)
	$(QEMU) -cpu EPYC -kernel vmdos.elf -initrd $(subst $(space),$(comma),$(strip $(if $(USE_RAM),dos.img,$(FLOPPY)) $(ISO_RAM))) \
	    -append "$(KARGS_ALL) vmhv$(if $(VMHV),=$(VMHV))" $(QDISK) $(QEMU_ARGS)
run-iso: vmdos.iso $(DISK_DEP)
	qemu-system-i386 -drive if=none,id=cd0,media=cdrom,file=vmdos.iso -device ide-cd,drive=cd0,bootindex=0 \
	    $(QDISK) $(QEMU_ARGS)
run-efi: vmdos.iso $(DISK_DEP)
	qemu-system-x86_64 -bios $(OVMF) -drive if=none,id=cd0,media=cdrom,file=vmdos.iso -device ide-cd,drive=cd0,bootindex=0 \
	    $(QDISK) $(QEMU_ARGS)
run-efi-app: BOOTDISK = 0
run-efi-app: esp.img
	qemu-system-x86_64 -bios $(OVMF) $(QESP) $(QEMU_ARGS)
# esp.img on a SATA disk, legacy BIOS (SeaBIOS) boot through GRUB.
run-bios: BOOTDISK = 0
run-bios: esp.img
	qemu-system-i386 $(QESP) $(QEMU_ARGS)
# esp.img as a USB stick (xHCI), booted by the firmware, C: on the stick.
run-usb: esp.img
	qemu-system-x86_64 -bios $(OVMF) -device qemu-xhci,id=xhci \
	    -drive if=none,id=stick,format=raw,file=esp.img -device usb-storage,bus=xhci.0,drive=stick,bootindex=0 $(QEMU_ARGS)

# bash and the Unix tools are built from pinned sources (GNU's and suckless's
# git; gawk from GNU's), with the DJGPP cross compiler: one on PATH or in $(DJGPP), else
# tools/get-djgpp.sh fetches one into deps/djgpp. deps/ is kept by make clean
# (rm -rf deps rebuilds it all).
DJGPP_GCC := $(if $(DJGPP)$(shell command -v i586-pc-msdosdjgpp-gcc 2>/dev/null),,deps/djgpp/bin/i586-pc-msdosdjgpp-gcc)
deps/djgpp/bin/i586-pc-msdosdjgpp-gcc: tools/get-djgpp.sh
	sh tools/get-djgpp.sh deps/djgpp
deps/bash/BASH.EXE: tools/build-bash.sh third_party/bash/bash-5.3-djgpp.patch $(wildcard third_party/bash/dos/*) | $(DJGPP_GCC)
	sh tools/build-bash.sh
deps/sbase/.built: tools/build-sbase.sh third_party/sbase/sbase-dos.patch third_party/sbase/TOOLS \
		$(wildcard third_party/sbase/dos/* third_party/sbase/dos/include/*) | $(DJGPP_GCC)
	sh tools/build-sbase.sh && touch $@
deps/gawk/.built: tools/build-gawk.sh third_party/gawk/gawk-dos.patch \
		$(wildcard third_party/gawk/dos/* third_party/gawk/dos/include/*) | $(DJGPP_GCC)
	sh tools/build-gawk.sh && touch $@
ifneq ($(UNIX),)
$(SBASE_BIN): deps/sbase/.built ;
$(GAWK_BIN): deps/gawk/.built ;
endif

clean:
	rm -rf build build-486 .vmdos-cpu vmdos.elf vmdos.iso vmdos.efi dos.img esp.img

# Rebuild net/bin (VMTCP, DHCP, PING, NSLOOKUP, VMIPXBOX, VMDOSNET) with Open
# Watcom. WATCOM=/path uses an installed copy; otherwise make watcom (run by
# make net) fetches one into .watcom/ the first time (tools/fetch-watcom.sh).
WATCOM ?= $(CURDIR)/.watcom
WATCOM_WPP = $(firstword $(wildcard $(WATCOM)/binl64/wpp $(WATCOM)/binl/wpp))
watcom:
ifeq ($(origin WATCOM),file)
	@[ -n "$(WATCOM_WPP)" ] || sh tools/fetch-watcom.sh "$(WATCOM)"
else
	@[ -n "$(WATCOM_WPP)" ] || { echo "WATCOM=$(WATCOM): no binl64/wpp or binl/wpp there" >&2; exit 1; }
endif
net: watcom
	$(MAKE) -C net install WATCOM=$(abspath $(WATCOM))
.PHONY: watcom net

.PHONY: FORCE all iso efi esp run run-hv run-iso run-efi run-efi-app run-usb run-bios clean
-include $(OBJS:.o=.d)
