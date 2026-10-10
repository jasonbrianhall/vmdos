# vmdos: FreeDOS in virtual-8086 mode under a small 32-bit kernel.
#
#   make                 vmdos.elf (+ dos.img with RAMDISK=1; fetches FreeDOS on first use)
#   make iso             vmdos.iso: GRUB, boots on BIOS and UEFI
#   make run             QEMU, BIOS, -kernel/-initrd
#   make run-iso         QEMU, BIOS, from the ISO
#   make run-efi         QEMU, UEFI (OVMF), from the ISO
#   make efi             vmdos.efi: run straight from UEFI (no GRUB);
#                        both go in the same folder of the EFI system partition
#   make esp             esp.img: the EFI disk, also C: (EFI/BOOT/BOOTX64.EFI + FreeDOS)
#   make run-efi-app     QEMU, UEFI (OVMF), booting esp.img (SATA)
#   make run-usb         QEMU, UEFI (OVMF), booting esp.img as a USB stick
#   make run-bios        QEMU, legacy BIOS (SeaBIOS), booting esp.img (GRUB in the MBR)
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

FREEDOS  ?= freedos
DISK_MB  ?= 64
C        ?= disk
RAMDISK  ?=
USE_RAM  := $(if $(filter 1 yes,$(RAMDISK))$(filter ram,$(C)),1)
RAM_IMG  := $(if $(USE_RAM),dos.img)
EXTRA    ?=
FB_W     ?= 640
FB_H     ?= 480
BUILD    := build

CC       := gcc
CFLAGS   := -m32 -march=i386 -mtune=i486 -ffreestanding -fno-builtin -fno-pic -fno-pie \
            -fno-stack-protector -fcf-protection=none -mgeneral-regs-only \
            -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns \
            -O2 -fno-strict-aliasing -fno-delete-null-pointer-checks --param=min-pagesize=0 -Wall -Wextra -Wno-unused-parameter -MMD
CXXFLAGS := $(filter-out -fno-delete-null-pointer-checks,$(CFLAGS)) -fno-delete-null-pointer-checks \
            -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit -std=gnu++17
OBJS     := $(addprefix $(BUILD)/,boot.o cpu.o lib.o v86.o vdev.o bios.o video.o biosblob.o bootblob.o disk.o fat.o ahci.o ide.o hd.o joy.o ems.o usb.o pci.o xms.o mouse.o dpmi.o mememu.o cd.o floppy.o \
              audio.o sound.o sb/dsp.o sb/sbout.o sb/mpu.o sb/gmsynth.o sb/gmtables.o sb/fpmath.o \
              sb/opl.o sb/dbopl.o)
# SBPRO's FM synth: dbopl's one-time table setup uses the x87 (opl_init saves
# and restores the FPU around it); everything that runs later is integer.
FPFLAGS  := $(filter-out -mgeneral-regs-only,$(CFLAGS)) -mfpmath=387 -mno-sse -mno-mmx
LIBGCC   := $(shell $(CC) -m32 -print-libgcc-file-name)

all: vmdos.elf $(RAM_IMG)

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

$(BUILD)/boot.o: src/boot.S | $(BUILD)
	$(CC) -m32 -DFB_W=$(FB_W) -DFB_H=$(FB_H) -c $< -o $@

$(BUILD)/bios.bin: src/bios.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/biosblob.o: src/biosblob.S $(BUILD)/bios.bin
	$(CC) -m32 -DBIOS_BIN='"$(BUILD)/bios.bin"' -c $< -o $@

$(BUILD)/bootblob.o: src/bootblob.S $(BUILD)/fat16.bin $(BUILD)/fat32lba.bin
	$(CC) -m32 -DFAT16_BIN='"$(BUILD)/fat16.bin"' -DFAT32_BIN='"$(BUILD)/fat32lba.bin"' -c $< -o $@

vmdos.elf: $(OBJS) src/linker.ld
	ld -m elf_i386 -T src/linker.ld -o $@ $(OBJS) $(LIBGCC)

$(BUILD)/fat32lba.bin: boot/boot32lb.asm boot/magic.mac | $(BUILD)
	nasm -f bin -i boot/ $< -o $@
$(BUILD)/fat16.bin: boot/boot.asm boot/magic.mac | $(BUILD)
	nasm -f bin -DISFAT16 -i boot/ $< -o $@

$(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM:
	sh tools/fetch-freedos.sh $(FREEDOS)

# Rebuild C: when EXTRA names another folder or anything in it changes
# (a checksum of the listing: names may contain spaces, which make can't take).
$(BUILD)/extra.stamp: FORCE | $(BUILD)
	@{ echo "$(EXTRA)"; [ -z "$(EXTRA)" ] || find "$(EXTRA)" -printf '%p %s %T@\n' | sort; } | md5sum > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@
FORCE:

$(BUILD)/VMSPEED.COM: dos/vmspeed.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMSB.COM: dos/vmsb.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMCD.SYS: dos/vmcd.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMCD.COM: dos/vmcdtool.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMFD.COM: dos/vmfd.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMHD.SYS: dos/vmhd.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMHD.COM: dos/vmhdtool.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/SHSUCDX.COM: third_party/shsucd/shsucdx.nsm | $(BUILD)
	nasm -O9 -w-all -Di8086 -i third_party/shsucd/ $< -o $@   # 8086 build: assembles with any NASM

$(BUILD)/VMXMS.SYS: dos/vmxms.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/VMEMS.SYS: dos/vmems.asm | $(BUILD)
	nasm -f bin $< -o $@

# C:\ root: FreeDOS and the two configuration files; vmdos's tools and
# drivers go in C:\VMDOS (on the PATH).
VMDOS_FILES := $(BUILD)/VMXMS.SYS $(BUILD)/VMEMS.SYS $(BUILD)/VMCD.SYS $(BUILD)/VMCD.COM $(BUILD)/VMFD.COM $(BUILD)/VMHD.SYS $(BUILD)/VMHD.COM $(BUILD)/VMSPEED.COM $(BUILD)/VMSB.COM $(BUILD)/SHSUCDX.COM \
               third_party/ctmouse/CTMOUSE.COM
DOS_DEPS  := $(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM dos/FDCONFIG.SYS dos/AUTOEXEC.BAT $(VMDOS_FILES)
DOS_FILES := $(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM dos/FDCONFIG.SYS dos/AUTOEXEC.BAT \
             $(foreach f,$(VMDOS_FILES),$(f)=VMDOS/$(notdir $(f)))

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
	@$(if $(USE_RAM),true,sed -i '/dos.img/d' $@.new)
	@for f in $(ISO_RAM); do n=$$(basename $$f); sed -i "/^}/i\    module /boot/cd/$$n $$n" $@.new; done
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@

vmdos.iso: vmdos.elf $(RAM_IMG) $(BUILD)/grub.cfg $(ISO_RAM)
	@test -n "$(GRUB_MKRESCUE)" || { echo "grub-mkrescue / grub2-mkrescue not found."; \
	  echo "  Debian/Ubuntu: sudo apt install grub-common grub-pc-bin grub-efi-amd64-bin xorriso mtools"; \
	  echo "  Fedora:        sudo dnf install grub2-tools-extra grub2-pc-modules grub2-efi-x64-modules xorriso mtools"; \
	  echo "Or skip GRUB: make efi / make esp"; exit 1; }
	rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	cp vmdos.elf $(BUILD)/iso/boot/
	$(if $(USE_RAM),gzip -9c dos.img > $(BUILD)/iso/boot/dos.img.gz)
	cp $(BUILD)/grub.cfg $(BUILD)/iso/boot/grub/grub.cfg
	$(if $(ISO_RAM),mkdir -p $(BUILD)/iso/boot/cd && cp $(ISO_RAM) $(BUILD)/iso/boot/cd/)
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/iso

# ---- UEFI application (gnu-efi): efi/loader.c with the kernel embedded ----
EFI_INC  ?= $(firstword $(wildcard /usr/include/efi))
EFI_CRT0 ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'crt0-efi-x86_64.o' -o -path '*gnuefi*x64*crt0.o' \) 2>/dev/null))
EFI_LDS  ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'elf_x86_64_efi.lds' -o -name 'elf_x64_efi.lds' \) 2>/dev/null))
EFI_LIBS ?= $(shell find /usr/lib /usr/lib64 \( -name libgnuefi.a -o -name libefi.a \) 2>/dev/null | sort -u)
EFI_CFLAGS := -I$(EFI_INC) -I$(EFI_INC)/x86_64 -I$(BUILD)/efi -fpic -ffreestanding -fno-stack-protector \
              -fno-stack-check -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -O2 -Wall

efi: vmdos.efi $(RAM_IMG)

$(BUILD)/efi/kernel.bin $(BUILD)/efi/kernel_layout.h: vmdos.elf
	mkdir -p $(BUILD)/efi
	objcopy -O binary vmdos.elf $(BUILD)/efi/kernel.bin
	@nm vmdos.elf | awk '/ __kernel_start$$/ {print "#define KERNEL_BASE 0x" $$1} \
	                     / __kernel_end$$/   {print "#define KERNEL_END 0x" $$1} \
	                     / _start$$/         {print "#define KERNEL_ENTRY 0x" $$1}' > $(BUILD)/efi/kernel_layout.h
	@test $$(grep -c define $(BUILD)/efi/kernel_layout.h) = 3 || { echo "kernel layout symbols missing"; exit 1; }

vmdos.efi: efi/loader.c efi/tramp.S efi/image.S $(BUILD)/efi/kernel.bin $(BUILD)/efi/kernel_layout.h
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
EXTRA_MB := $(if $(EXTRA),$(shell du -sm "$(EXTRA)" | cut -f1),0)
ESP_MB   ?= $(shell echo $$(( $(DISK_MB) + $(EXTRA_MB) + $(ISO_MB) + $(ESP_FREE) )))
GRUB_MKIMAGE ?= $(firstword $(shell command -v grub-mkimage grub2-mkimage 2>/dev/null))
GRUB_LIB ?= $(firstword $(wildcard /usr/lib/grub /usr/share/grub2 /usr/lib64/grub))
GRUB_OK  := $(and $(GRUB_MKIMAGE),$(wildcard $(GRUB_LIB)/i386-pc/boot.img),$(wildcard $(GRUB_LIB)/x86_64-efi/normal.mod))
GRUB     ?= $(if $(GRUB_OK),1,0)
GRUB_MODS := part_msdos part_gpt fat normal configfile search search_fs_file test echo sleep chain multiboot boot \
             video video_fb all_video gfxterm
VMDOS_EFI_DIR := $(if $(filter 1,$(GRUB)),EFI/vmdos,EFI/BOOT)
# Rebuild esp.img when what goes on it changes (RAM disk on/off, ISO list, GRUB).
$(BUILD)/esp.opts: FORCE | $(BUILD)
	@echo '$(USE_RAM) $(ISO_C) $(GRUB)' > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@
$(BUILD)/vmdos.cfg: FORCE | $(BUILD)
	@echo '$(KARGS_ALL)' > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@
$(BUILD)/grub/early.cfg: | $(BUILD)
	@mkdir -p $(BUILD)/grub
	printf 'search --no-floppy --file --set=root /boot/vmdos.elf\nset prefix=($$root)/boot/grub\nconfigfile $$prefix/grub.cfg\n' > $@
$(BUILD)/grub/core.img: $(BUILD)/grub/early.cfg
	$(GRUB_MKIMAGE) -O i386-pc -d $(GRUB_LIB)/i386-pc -o $@ -c $< -p /boot/grub biosdisk vbe vga $(GRUB_MODS)
$(BUILD)/grub/BOOTX64.EFI: $(BUILD)/grub/early.cfg
	$(GRUB_MKIMAGE) -O x86_64-efi -d $(GRUB_LIB)/x86_64-efi -o $@ -c $< -p /boot/grub efi_gop efi_uga $(GRUB_MODS)
$(BUILD)/grub/grub.cfg: FORCE | $(BUILD)
	@mkdir -p $(BUILD)/grub
	@{ echo '# vmdos boot menu (made by make esp: changes here are overwritten;'; \
	   echo '# put your own entries in /boot/grub/custom.cfg)'; \
	   echo 'insmod all_video'; \
	   echo 'set default=0'; \
	   echo 'set timeout=5'; \
	   echo 'if [ "$$grub_platform" = "efi" ]; then'; \
	   echo '  menuentry "FreeDOS (vmdos)" {'; \
	   echo '    chainloader /EFI/vmdos/vmdos.efi'; \
	   echo '  }'; \
	   echo '  menuentry "FreeDOS (vmdos), no XMS (Windows 3.1 Setup)" {'; \
	   echo '    chainloader /EFI/vmdos/vmdos.efi noxms'; \
	   echo '  }'; \
	   echo 'else'; \
	   echo '  menuentry "FreeDOS (vmdos)" {'; \
	   echo '    multiboot /boot/vmdos.elf $(KARGS_ALL)'; \
	   $(if $(USE_RAM),echo '    module /EFI/vmdos/dos.img dos.img';) \
	   echo '  }'; \
	   echo '  menuentry "FreeDOS (vmdos), no XMS (Windows 3.1 Setup)" {'; \
	   echo '    multiboot /boot/vmdos.elf $(KARGS_ALL) noxms'; \
	   $(if $(USE_RAM),echo '    module /EFI/vmdos/dos.img dos.img';) \
	   echo '  }'; \
	   echo 'fi'; \
	   echo 'if [ -f $$prefix/custom.cfg ]; then source $$prefix/custom.cfg; fi'; } > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@
ESP_GRUB_DEPS  := $(if $(filter 1,$(GRUB)),vmdos.elf $(BUILD)/grub/core.img $(BUILD)/grub/BOOTX64.EFI $(BUILD)/grub/grub.cfg)
ESP_GRUB_FILES  = $(if $(filter 1,$(GRUB)),$(BUILD)/grub/BOOTX64.EFI=EFI/BOOT/BOOTX64.EFI vmdos.elf=boot/vmdos.elf \
                    $(BUILD)/grub/grub.cfg=boot/grub/grub.cfg --bios=$(GRUB_LIB)/i386-pc/boot.img$(comma)$(BUILD)/grub/core.img)
esp: esp.img
esp.img: vmdos.efi $(RAM_IMG) $(DOS_DEPS) $(BUILD)/fat16.bin $(BUILD)/fat32lba.bin $(BUILD)/vmdos.cfg $(BUILD)/esp.opts $(BUILD)/extra.stamp tools/mkdisk.py $(ISO) $(ESP_GRUB_DEPS)
	$(if $(filter 1,$(GRUB)),,@echo "esp.img: UEFI only (GRUB=0, or grub-mkimage / GRUB's i386-pc and x86_64-efi modules not found)")
	python3 tools/mkdisk.py $(if $(FRESH),,--update) $(filter --bios=%,$(ESP_GRUB_FILES)) --type=EF --boot16=$(BUILD)/fat16.bin $@ $(ESP_MB) $(BUILD)/fat32lba.bin \
	    $(DOS_FILES) vmdos.efi=$(VMDOS_EFI_DIR)/$(if $(filter 1,$(GRUB)),vmdos.efi,BOOTX64.EFI) \
	    $(if $(USE_RAM),dos.img=$(VMDOS_EFI_DIR)/dos.img) $(BUILD)/vmdos.cfg=$(VMDOS_EFI_DIR)/vmdos.cfg \
	    $(filter-out --bios=%,$(ESP_GRUB_FILES)) $(ISO_C) $(if $(EXTRA),"--contents=$(EXTRA)")
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
# KVM when /dev/kvm is usable, else plain emulation (ACCEL= to override)
ACCEL ?= -accel kvm -accel tcg
QEMU_ARGS ?= $(ACCEL) -m $(QEMU_MEM) -serial stdio $(QSOUND_$(SOUND)) $(QUSB) $(QUSBHOST) $(QDISPLAY)
# C=disk: esp.img on an AHCI controller (C:), after the CD in the boot order.
BOOTDISK ?= 1
QESP  = -device ahci,id=ahci -drive if=none,id=cdisk,format=raw,file=esp.img \
        -device ide-hd,drive=cdisk,bus=ahci.0,bootindex=$(BOOTDISK)
QDISK = $(if $(filter ram,$(C)),,$(QESP))
DISK_DEP := $(if $(filter ram,$(C)),,esp.img)

run: vmdos.elf $(RAM_IMG) $(DISK_DEP)
	qemu-system-i386 -kernel vmdos.elf $(if $(USE_RAM),-initrd $(subst $(space),$(comma),$(strip dos.img $(ISO_RAM)))) \
	    -append "$(KARGS_ALL)" $(QDISK) $(QEMU_ARGS)
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

clean:
	rm -rf $(BUILD) vmdos.elf vmdos.iso vmdos.efi dos.img esp.img

.PHONY: FORCE all iso efi esp run run-iso run-efi run-efi-app run-usb run-bios clean
-include $(OBJS:.o=.d)
