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
QEMU_ARGS ?= -m 128 -serial stdio $(QDISPLAY)

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

.PHONY: all iso efi esp run run-iso run-efi run-efi-app clean
-include $(OBJS:.o=.d)
