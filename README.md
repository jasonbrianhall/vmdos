# vmdos

FreeDOS running in virtual-8086 mode under a small 32-bit protected-mode
kernel. Boots from GRUB (BIOS or UEFI) or QEMU `-kernel`. The kernel emulates
the BIOS and the PC hardware DOS touches; FreeDOS's kernel and FreeCOM run
unmodified, with drive C: a FAT32 RAM disk loaded as a boot module.

## Build and run

```
# Fedora: sudo dnf install gcc nasm python3 mtools dosfstools gnu-efi-devel glibc-devel.i686 libgcc.i686 \
#         grub2-tools-extra grub2-pc-modules grub2-efi-x64-modules xorriso qemu-system-x86 edk2-ovmf
sudo apt install build-essential gcc-multilib nasm python3 mtools dosfstools gnu-efi \
                 grub-pc-bin grub-efi-amd64-bin grub-common xorriso qemu-system-x86 ovmf
make            # vmdos.elf + dos.img (first run fetches KERNEL.SYS/COMMAND.COM into freedos/)
make efi        # vmdos.efi + dos.img: a UEFI application, no GRUB needed
make esp        # esp.img: FAT32 disk with EFI/BOOT/BOOTX64.EFI + dos.img; dd it to a USB stick
make iso        # vmdos.iso (GRUB), hybrid BIOS/UEFI
make run        # QEMU, BIOS (-kernel)
make run-efi-app  # QEMU, UEFI (OVMF), booting esp.img (OVMF takes ~40 s to read dos.img without KVM)
make run-efi    # QEMU, UEFI, from vmdos.iso
```

QEMU targets: `SOUND=hda|ac97|sb|none` (default hda), `AUDIODEV=pa|alsa|sdl|wav`
(default pa: PulseAudio/PipeWire; wav records vmdos.wav), `USB=1` for a USB
keyboard and mouse, `QEMU_MEM=` (512), `KARGS="debug=2 ..."` for the ISO's kernel command line.

### UEFI without GRUB

Copy `vmdos.efi` and `dos.img` into one folder of the EFI system partition,
e.g. `/boot/efi/EFI/vmdos/`, then either add a firmware boot entry:

```
sudo efibootmgr -c -d /dev/nvme0n1 -p 1 -L vmdos -l '\EFI\vmdos\vmdos.efi'
```

or chainload it from GRUB:

```
menuentry "FreeDOS (vmdos.efi)" {
    insmod part_gpt
    insmod fat
    insmod chain
    search --no-floppy --file --set=root /EFI/vmdos/vmdos.efi
    chainloader /EFI/vmdos/vmdos.efi
}
```

The loader puts the 32-bit kernel at 16 MiB, leaves long mode and enters it
as GRUB would. Load options go to the kernel (`debug=2`, `nopae`); `debug`
alone pauses before leaving the firmware. A small ESP may need a smaller C:
(`make efi DISK_MB=34`).

Adding programs: put them in a folder (e.g. `games/POP/...`) and build with
`EXTRA=games`; its contents land in C:\ (here C:\POP). `make esp EXTRA=games`
or `make iso EXTRA=games` rebuild C: and the image that carries it (esp.img and
vmdos.iso hold their own copy of dos.img). By hand: `mcopy -s -i dos.img@@1M POP ::/`.

Options: `EXTRA=dir` copies a directory's contents into C:\, `DISK_MB=` sets
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

C: lives in RAM: changes are lost at power-off. The kernel loads at 16 MiB.
Give a machine or VM at least 256 MB (QEMU targets use 512 MB, `QEMU_MEM=`): GRUB needs room to unpack
dos.img.gz and place it in one piece, or it stops with "out of memory". Ctrl+Alt+Del restarts.

## What works

- FreeDOS 1.x kernel and FreeCOM to the `C:\>` prompt: GRUB on BIOS or UEFI,
  or `vmdos.efi` straight from the UEFI firmware.
- BIOS: INT 10h (text modes, mode 13h, DAC/palette), 11h, 12h, 13h (CHS and
  LBA), 15h (A20, wait, config), 16h, 1Ah (RTC time/date), keyboard IRQ.
- Keyboards: PS/2, and USB on xHCI (boot protocol, hubs, hot-plug; polled).
  `usb=off` on the command line skips USB.
- Mouse: PS/2 and USB mice behind an INT 33h driver in the monitor (no
  MOUSE.COM): position, buttons, ranges, mickeys, press/release counts and
  the program's event handler (0Ch/14h). The pointer is drawn over text and
  mode 13h by the renderer. A DOS mouse driver you load (CTMOUSE) takes over.
- XMS 3.0 in the monitor (don't load HIMEM): HMA, extended memory (32 MB,
  `xms=MB`) and 160 KB of upper memory (C800h-EFFFh). `VMXMS.SYS`, loaded
  first in FDCONFIG.SYS, is the HIMEM-style front (device XMSXXXX0, INT 2Fh
  hook). `DOS=HIGH,UMB`, LOADHIGH/DEVICEHIGH and FreeCOM's XMS swapping work:
  about 620 KB free for programs.
- DPMI 0.9 host in the monitor, for DOS extenders: DOS/4GW (DOOM runs: demo,
  menus, keyboard, mouse; with sound set to "none" for now), PMODE/W. Clients run at ring 3 with LDT descriptors;
  INT 31h descriptor, memory, interrupt, real-mode call/callback, DOS
  memory, physical-mapping and virtual-IF services; protected-mode hardware
  interrupt and exception handlers. Client memory appears at linear
  2-16 MiB (DOS/4GW's DOS/16M core keeps 24-bit addresses), so the kernel
  now loads at 16 MiB. Not yet: DOS/32A (it insists on its own XMS mode).
- Unchained 256-colour VGA (Mode X/Y, as DOOM uses): four planes, map mask,
  page flipping via the CRTC start address.
- Sound: Sound Blaster Pro 2.0 (220h, IRQ 5, DMA 1) with OPL3, AdLib (388h)
  and MPU-401 General MIDI (330h), from SBPRO; PC speaker. Played through HD
  Audio, AC'97 or a real Sound Blaster (`audio=hda|hdmi|ac97|sb|off`,
  `latency=ms`, as in baremetaldoom). `BLASTER=A220 I5 D1 T4 P330` is set.
- Virtual 8259 pair, 8254 (guest can reprogram channel 0), 8042, port 61h,
  A20 (port 92h, 8042, INT 15h), VGA DAC/CRTC/attribute/status ports.
- Text and mode 13h drawn to the GRUB/GOP framebuffer (any size, 15/16/24/32 bpp,
  above 4 GiB too, via PAE), or Bochs VBE under QEMU `-kernel`.

## Not yet

- DOOM with Sound Blaster sound: DOS/16M corrupts its selector table during
  DMX's DMA-buffer setup and hangs. Debug with `debug=2` (5 s heartbeat of
  IRQ/vIF/DPMI state); `norouteirq` stops routing real-mode IRQs to
  protected-mode handlers.
- EGA 16-colour modes, Mode X's 240-line timing, VESA, EMS, SB16 (16-bit)
  sound, saving C: to a real disk.

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
| `efi/loader.c`, `efi/tramp.S` | vmdos.efi: UEFI loader, long mode to 32-bit handoff |
| `src/usb.cpp`, `src/pci.cpp` | xHCI keyboard driver (from baremetaldoom) |
| `src/audio.cpp`, `src/sound.c` | sound card driver (from baremetaldoom), SB glue |
| `src/sb/` | SBPRO core: DSP, playback + virtual 8237, OPL3 (dbopl), GM synth, MPU-401 |
| `src/mouse.c` | PS/2 + USB mouse, INT 33h |
| `src/dpmi.c` | DPMI host |
| `src/xms.c`, `dos/vmxms.asm` | XMS driver; VMXMS.SYS, its DOS-side front |
| `tools/mkdisk.py` | builds dos.img (MBR + FAT32 + boot sector + files) |

License: GPL-2.0-or-later (it includes FreeDOS's boot sector).
