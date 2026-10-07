# vmdos: FreeDOS in virtual-8086 mode under a small 32-bit kernel.
#
#   make                 vmdos.elf + dos.img (fetches FreeDOS on first use)
#   make iso             vmdos.iso: GRUB, boots on BIOS and UEFI
#   make run             QEMU, BIOS, -kernel/-initrd
#   make run-iso         QEMU, BIOS, from the ISO
#   make run-efi         QEMU, UEFI (OVMF), from the ISO
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
OBJS     := $(addprefix $(BUILD)/,boot.o cpu.o lib.o v86.o vdev.o bios.o video.o biosblob.o)

all: vmdos.elf dos.img

$(BUILD):
	mkdir -p $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/boot.o: src/boot.S | $(BUILD)
	$(CC) -m32 -DFB_W=$(FB_W) -DFB_H=$(FB_H) -c $< -o $@

$(BUILD)/bios.bin: src/bios.asm | $(BUILD)
	nasm -f bin $< -o $@

$(BUILD)/biosblob.o: src/biosblob.S $(BUILD)/bios.bin
	$(CC) -m32 -DBIOS_BIN='"$(BUILD)/bios.bin"' -c $< -o $@

vmdos.elf: $(OBJS) src/linker.ld
	ld -m elf_i386 -T src/linker.ld -o $@ $(OBJS)

$(BUILD)/fat32lba.bin: boot/boot32lb.asm boot/magic.mac | $(BUILD)
	nasm -f bin -i boot/ $< -o $@

$(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM:
	sh tools/fetch-freedos.sh $(FREEDOS)

dos.img: $(BUILD)/fat32lba.bin tools/mkdisk.py dos/FDCONFIG.SYS dos/AUTOEXEC.BAT $(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM
	python3 tools/mkdisk.py $@ $(DISK_MB) $(BUILD)/fat32lba.bin \
	    $(FREEDOS)/KERNEL.SYS $(FREEDOS)/COMMAND.COM dos/FDCONFIG.SYS dos/AUTOEXEC.BAT \
	    $(if $(EXTRA),$(wildcard $(EXTRA)/*))

iso: vmdos.iso
vmdos.iso: vmdos.elf dos.img grub.cfg
	rm -rf $(BUILD)/iso && mkdir -p $(BUILD)/iso/boot/grub
	cp vmdos.elf $(BUILD)/iso/boot/
	gzip -9c dos.img > $(BUILD)/iso/boot/dos.img.gz
	cp grub.cfg $(BUILD)/iso/boot/grub/
	grub-mkrescue -o $@ $(BUILD)/iso 2>/dev/null

OVMF     ?= $(firstword $(wildcard /usr/share/ovmf/OVMF.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/ovmf/OVMF_CODE.fd /usr/share/qemu/OVMF.fd))
QDISPLAY ?=
QEMU_ARGS ?= -m 128 -serial stdio $(QDISPLAY)

run: vmdos.elf dos.img
	qemu-system-i386 -kernel vmdos.elf -initrd dos.img $(QEMU_ARGS)
run-iso: vmdos.iso
	qemu-system-i386 -cdrom vmdos.iso $(QEMU_ARGS)
run-efi: vmdos.iso
	qemu-system-x86_64 -bios $(OVMF) -cdrom vmdos.iso $(QEMU_ARGS)

clean:
	rm -rf $(BUILD) vmdos.elf vmdos.iso dos.img

.PHONY: all iso run run-iso run-efi clean
-include $(OBJS:.o=.d)
