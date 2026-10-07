# vmdos: FreeDOS in virtual-8086 mode under a small 32-bit kernel.
#
#   make                 vmdos.elf + dos.img (fetches FreeDOS on first use)
#   make iso             vmdos.iso: GRUB, boots on BIOS and UEFI
#   make run             QEMU, BIOS, -kernel/-initrd
#   make run-iso         QEMU, BIOS, from the ISO
#   make run-efi         QEMU, UEFI (OVMF), from the ISO
#   make efi             vmdos.efi + dos.img: run straight from UEFI (no GRUB);
#                        both go in the same folder of the EFI system partition
#   make esp             esp.img: a FAT32 disk with EFI/BOOT/BOOTX64.EFI + dos.img
#   make run-efi-app     QEMU, UEFI (OVMF), booting esp.img
#
# FREEDOS=dir with KERNEL.SYS and COMMAND.COM (default: freedos/, fetched)
# EXTRA=dir whose contents are copied into C:\ too (games, tools)
# DISK_MB=size of C: (default 64; at least 34 for FAT32)

FREEDOS  ?= freedos
DISK_MB  ?= 64
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
OBJS     := $(addprefix $(BUILD)/,boot.o cpu.o lib.o v86.o vdev.o bios.o video.o biosblob.o usb.o pci.o \
              audio.o sound.o sb/dsp.o sb/sbout.o sb/mpu.o sb/gmsynth.o sb/gmtables.o sb/fpmath.o \
              sb/opl.o sb/dbopl.o)
# SBPRO's FM synth: dbopl's one-time table setup uses the x87 (opl_init saves
# and restores the FPU around it); everything that runs later is integer.
FPFLAGS  := $(filter-out -mgeneral-regs-only,$(CFLAGS)) -mfpmath=387 -mno-sse -mno-mmx
LIBGCC   := $(shell $(CC) -m32 -print-libgcc-file-name)

all: vmdos.elf dos.img

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

vmdos.elf: $(OBJS) src/linker.ld
	ld -m elf_i386 -T src/linker.ld -o $@ $(OBJS) $(LIBGCC)

$(BUILD)/fat32lba.bin: boot/boot32lb.asm boot/magic.mac | $(BUILD)
	nasm -f bin -i boot/ $< -o $@

$(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM:
	sh tools/fetch-freedos.sh $(FREEDOS)

# Rebuild C: when EXTRA names another folder or anything in it changes
# (a checksum of the listing: names may contain spaces, which make can't take).
$(BUILD)/extra.stamp: FORCE | $(BUILD)
	@{ echo "$(EXTRA)"; [ -z "$(EXTRA)" ] || find "$(EXTRA)" -printf '%p %s %T@\n' | sort; } | md5sum > $@.new
	@cmp -s $@.new $@ && rm $@.new || mv $@.new $@
FORCE:

dos.img: $(BUILD)/fat32lba.bin tools/mkdisk.py dos/FDCONFIG.SYS dos/AUTOEXEC.BAT $(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM \
         $(BUILD)/extra.stamp
	python3 tools/mkdisk.py $@ $(DISK_MB) $(BUILD)/fat32lba.bin \
	    $(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM dos/FDCONFIG.SYS dos/AUTOEXEC.BAT \
	    $(if $(EXTRA),"--contents=$(EXTRA)")

# grub-mkrescue (Debian/Ubuntu) or grub2-mkrescue (Fedora/RHEL/openSUSE).
GRUB_MKRESCUE ?= $(firstword $(shell command -v grub-mkrescue grub2-mkrescue 2>/dev/null))

iso: vmdos.iso
vmdos.iso: vmdos.elf dos.img grub.cfg
	@test -n "$(GRUB_MKRESCUE)" || { echo "grub-mkrescue / grub2-mkrescue not found."; \
	  echo "  Debian/Ubuntu: sudo apt install grub-common grub-pc-bin grub-efi-amd64-bin xorriso mtools"; \
	  echo "  Fedora:        sudo dnf install grub2-tools-extra grub2-pc-modules grub2-efi-x64-modules xorriso mtools"; \
	  echo "Or skip GRUB: make efi / make esp"; exit 1; }
	rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	cp vmdos.elf $(BUILD)/iso/boot/
	gzip -9c dos.img > $(BUILD)/iso/boot/dos.img.gz
	cp grub.cfg $(BUILD)/iso/boot/grub/
	$(GRUB_MKRESCUE) -o $@ $(BUILD)/iso

# ---- UEFI application (gnu-efi): efi/loader.c with the kernel embedded ----
EFI_INC  ?= $(firstword $(wildcard /usr/include/efi))
EFI_CRT0 ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'crt0-efi-x86_64.o' -o -path '*gnuefi*x64*crt0.o' \) 2>/dev/null))
EFI_LDS  ?= $(firstword $(shell find /usr/lib /usr/lib64 \( -name 'elf_x86_64_efi.lds' -o -name 'elf_x64_efi.lds' \) 2>/dev/null))
EFI_LIBS ?= $(shell find /usr/lib /usr/lib64 \( -name libgnuefi.a -o -name libefi.a \) 2>/dev/null | sort -u)
EFI_CFLAGS := -I$(EFI_INC) -I$(EFI_INC)/x86_64 -I$(BUILD)/efi -fpic -ffreestanding -fno-stack-protector \
              -fno-stack-check -fshort-wchar -mno-red-zone -maccumulate-outgoing-args -O2 -Wall

efi: vmdos.efi dos.img

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

ESP_MB ?= $(shell echo $$(( $(DISK_MB) + 40 )))
esp: esp.img
esp.img: vmdos.efi dos.img tools/mkdisk.py
	python3 tools/mkdisk.py --type=EF $@ $(ESP_MB) - vmdos.efi=EFI/BOOT/BOOTX64.EFI dos.img=EFI/BOOT/dos.img

OVMF     ?= $(firstword $(wildcard /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/qemu/OVMF.fd))
QDISPLAY ?=
QEMU_MEM ?= 512
QEMU_ARGS ?= -m $(QEMU_MEM) -serial stdio $(QDISPLAY)

run: vmdos.elf dos.img
	qemu-system-i386 -kernel vmdos.elf -initrd dos.img $(QEMU_ARGS)
run-iso: vmdos.iso
	qemu-system-i386 -cdrom vmdos.iso $(QEMU_ARGS)
run-efi: vmdos.iso
	qemu-system-x86_64 -bios $(OVMF) -cdrom vmdos.iso $(QEMU_ARGS)
run-efi-app: esp.img
	qemu-system-x86_64 -bios $(OVMF) -drive format=raw,file=esp.img $(QEMU_ARGS)

clean:
	rm -rf $(BUILD) vmdos.elf vmdos.iso vmdos.efi dos.img esp.img

.PHONY: FORCE all iso efi esp run run-iso run-efi run-efi-app clean
-include $(OBJS:.o=.d)
