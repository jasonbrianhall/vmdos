# vmdos

FreeDOS running in virtual-8086 mode under a small 32-bit protected-mode
kernel. Boots from GRUB (BIOS or UEFI) or QEMU `-kernel`. The kernel emulates
the BIOS and the PC hardware DOS touches; FreeDOS's kernel and FreeCOM run
unmodified, with drive C: a FAT32 RAM disk loaded as a boot module.

## Build and run

```
sudo apt install build-essential gcc-multilib nasm python3 mtools dosfstools \
                 grub-pc-bin grub-efi-amd64-bin grub-common xorriso qemu-system-x86 ovmf
make            # vmdos.elf + dos.img (first run fetches KERNEL.SYS/COMMAND.COM into freedos/)
make run        # QEMU, BIOS
make run-efi    # QEMU, UEFI (OVMF), from vmdos.iso
make iso        # vmdos.iso, hybrid BIOS/UEFI; dd it to a USB stick
```

Options: `EXTRA=dir` copies a directory's files into C:\, `DISK_MB=` sets
the size of C: (34 or more), `FREEDOS=dir` uses your own KERNEL.SYS and
COMMAND.COM. Kernel command line `debug=2` or `debug=3` logs ports and
interrupts to COM1.

Booting from an existing GRUB (copy `vmdos.elf` and `dos.img` to `/boot/vmdos/`):

```
menuentry "FreeDOS (vmdos)" {
    insmod all_video
    search --no-floppy --file --set=root /boot/vmdos/vmdos.elf
    multiboot /boot/vmdos/vmdos.elf
    module /boot/vmdos/dos.img dos.img
}
```

C: lives in RAM: changes are lost at power-off. Ctrl+Alt+Del restarts.

## What works

- FreeDOS 1.x kernel and FreeCOM to the `C:\>` prompt, on BIOS and UEFI.
- BIOS: INT 10h (text modes, mode 13h, DAC/palette), 11h, 12h, 13h (CHS and
  LBA), 15h (A20, wait, config), 16h, 1Ah (RTC time/date), keyboard IRQ.
- Virtual 8259 pair, 8254 (guest can reprogram channel 0), 8042, port 61h,
  A20 (port 92h, 8042, INT 15h), VGA DAC/CRTC/attribute/status ports.
- Text and mode 13h drawn to the GRUB/GOP framebuffer (any size, 15/16/24/32 bpp),
  or Bochs VBE under QEMU `-kernel`.

## Not yet

- USB keyboards (PS/2 only; many UEFI machines need USB).
- Sound (SBPRO), EGA/planar modes and Mode X, VESA, XMS/EMS, DPMI,
  mouse, saving C: to a real disk, framebuffers above 4 GiB.

## Layout

| File | |
|---|---|
| `src/boot.S` | Multiboot header, entry, interrupt stubs, entry into v86 |
| `src/cpu.c` | kmain, memory, paging, GDT/TSS/IDT, real PIC/PIT |
| `src/v86.c` | the monitor: #GP decoding, INT/IRET/PUSHF/POPF/CLI/STI, I/O, IRQ delivery |
| `src/vdev.c` | virtual PIC, PIT, keyboard controller, A20, CMOS |
| `src/bios.c`, `src/bios.asm` | BIOS data area, IVT, services; F000h stub segment |
| `src/video.c` | VGA state, INT 10h, framebuffer renderer |
| `boot/boot32lb.asm` | FreeDOS FAT32 LBA boot sector (from the FreeDOS kernel, GPL) |
| `tools/mkdisk.py` | builds dos.img (MBR + FAT32 + boot sector + files) |

License: GPL-2.0-or-later (it includes FreeDOS's boot sector).
