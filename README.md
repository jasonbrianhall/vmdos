# vmdos

FreeDOS running in virtual-8086 mode under a small 32-bit protected-mode
kernel. Boots from GRUB (BIOS or UEFI) or QEMU `-kernel`. The kernel emulates
the BIOS and the PC hardware DOS touches; FreeDOS's kernel and FreeCOM run
unmodified. Drive C: is the FAT partition of the EFI disk (AHCI/SATA), so
changes are kept; a RAM disk (dos.img) is the option and the fallback.

## Build and run

```
# Fedora: sudo dnf install gcc nasm python3 mtools dosfstools gnu-efi-devel glibc-devel.i686 libgcc.i686 \
#         grub2-tools-extra grub2-pc-modules grub2-efi-x64-modules xorriso qemu-system-x86 edk2-ovmf
sudo apt install build-essential gcc-multilib nasm python3 mtools dosfstools gnu-efi \
                 grub-pc-bin grub-efi-amd64-bin grub-common xorriso qemu-system-x86 ovmf
make            # vmdos.elf + dos.img (first run fetches KERNEL.SYS/COMMAND.COM into freedos/)
make efi        # vmdos.efi + dos.img: a UEFI application, no GRUB needed
make esp        # esp.img: the EFI disk, also C: (FreeDOS at its root, EFI/BOOT/BOOTX64.EFI); dd it to a disk
make iso        # vmdos.iso (GRUB), hybrid BIOS/UEFI
make run        # QEMU, BIOS (-kernel)
make run-efi-app  # QEMU, UEFI (OVMF), booting esp.img
make run-efi    # QEMU, UEFI, from vmdos.iso
```

### Drive C:

`C=disk` (default): C: is the FAT partition of `esp.img`, which the run
targets attach as a SATA (AHCI) disk; what DOS writes stays there. Rebuilding
updates esp.img in place: programs and FreeDOS files are refreshed,
AUTOEXEC.BAT, FDCONFIG.SYS and files already copied from `EXTRA` are kept
(`FRESH=1` starts over). `C=ram`: C: is dos.img in RAM, changes lost (kernel
option `c=ram`; `make run-efi-app KARGS=c=ram` writes it to
EFI/BOOT/vmdos.cfg, which vmdos.efi reads).

The kernel picks C: from the FAT partitions on AHCI disks that hold
KERNEL.SYS at their root, preferring the one vmdos.efi started from; other
partitions are never touched (DOS sees a one-partition disk and can't write
outside it). No such partition, or a disk that isn't AHCI (NVMe, USB): the
RAM disk, from dos.img next to vmdos.efi or the GRUB module. To use an
existing ESP on a real machine, copy esp.img's root files (KERNEL.SYS,
COMMAND.COM, FDCONFIG.SYS, AUTOEXEC.BAT, VM*.*, SHSUCDX.COM, CTMOUSE.COM)
to the ESP's root and vmdos.efi + dos.img to a folder on it.

QEMU targets use KVM when `/dev/kvm` is usable (else plain emulation, which
is many times slower; `ACCEL=` overrides). `SOUND=hda|ac97|sb|none` (default hda), `AUDIODEV=pa|alsa|sdl|wav`
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
the size of C: (8 or more; FAT16 with 8 KB clusters up to 2 GB, since
FreeDOS walks the cluster chain on every seek and DOOM's WAD on 512-byte
FAT32 clusters took minutes to load), `FREEDOS=dir` uses your own KERNEL.SYS and
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

Slowdown for games that time themselves by the CPU (as MoSlo does):
`VMSPEED 2` in DOS runs at 2% of full speed (`VMSPEED 0.5`, `VMSPEED 100`,
`VMSPEED` alone shows it; it's in C:\), Ctrl+F11 / Ctrl+F12 step it slower /
faster while a program runs (shown at the top right), and `speed=N` on the
kernel command line sets it from boot (`KARGS="speed=2"` for the ISO).

CD-ROM: any ISO file on C: can be put in the CD drive (D:) while running:
`VMCD D: C:\ISOS\WAR2.ISO` (or `VMCD 1 WAR2.ISO`, relative paths work; 8.3
names). It is read straight from the disk, nothing is copied to RAM; `VMCD`
lists the drives. `ISO="game.iso disc2.iso"` copies ISOs to C:\ISOS (8.3
names) and puts the first in the drive at boot (`cd=/ISOS/GAME.ISO` on the
kernel command line). `cdrives=N` gives up to 4 drives. With `C=ram`, ISO=
images are held in RAM as boot modules instead, one drive each, and
`VMCD 1 2` swaps them (also: ISOs next to vmdos.efi, GRUB module lines).

Mouse trouble in a game: boot with `KARGS="mouselog"` (or `mouselog` in the
load options); the log shows every INT 33h call, event-handler call and
command sent to the PS/2 mouse port. If the built-in driver doesn't satisfy
a game, try `CTMOUSE` first.

The kernel loads at 16 MiB.
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
  the program's event handler (0Ch/14h; for DOS extenders' programs too,
  run from protected mode through their real-mode callback), and a PS/2
  mouse on the virtual keyboard controller (IRQ 12 packets) for programs
  with their own mouse code. The pointer is drawn over text and
  mode 13h by the renderer. BIOS PS/2 mouse services (INT 15h C2xx, IRQ 12
  handler) let a real DOS driver run too: CuteMouse is in C:\ (`LH CTMOUSE`
  before a game that wants a DOS driver); it takes over INT 33h.
- XMS 3.0 in the monitor (don't load HIMEM): HMA, extended memory (32 MB,
  `xms=MB`) and 160 KB of upper memory (C800h-EFFFh). `VMXMS.SYS`, loaded
  first in FDCONFIG.SYS, is the HIMEM-style front (device XMSXXXX0, INT 2Fh
  hook). `DOS=HIGH,UMB`, LOADHIGH/DEVICEHIGH and FreeCOM's XMS swapping work:
  about 620 KB free for programs.
- DPMI 0.9 host in the monitor, for DOS extenders: DOS/4GW (DOOM runs: demo,
  menus, keyboard, mouse, Sound Blaster effects and music), PMODE/W. Clients run at ring 3 with LDT descriptors;
  INT 31h descriptor, memory, interrupt, real-mode call/callback, DOS
  memory, physical-mapping and virtual-IF services; protected-mode hardware
  interrupt and exception handlers; selector 0040h for the BIOS data
  area. Client memory appears at linear
  2-16 MiB (DOS/4GW's DOS/16M core keeps 24-bit addresses), so the kernel
  now loads at 16 MiB. Not yet: DOS/32A (it insists on its own XMS mode).
- VESA BIOS 2.0: 640x400 to 1024x768 in 8, 15, 16 and 32 bits per pixel,
  4 MB, banked window (4F05h / WinFuncPtr) and linear framebuffer (DPMI
  0800h maps it), scan line length, display start, palette, and the
  protected-mode interface (4F00h-4F0Ah).
- Graphics: CGA modes 4, 5, 6 (palettes and background via INT 10h AH=0Bh
  or port 3D9h), mode 13h, unchained 256-colour (Mode X/Y, as DOOM uses:
  single-plane access mapped directly, latch copies and the rest emulated),
  and the EGA/VGA 16-colour modes 0Dh, 0Eh, 10h, 12h. In the 16-colour modes
  every access to A000h faults and the instruction is emulated against the
  four planes with the VGA's latches, write modes 0-3, set/reset, bit mask
  and read modes; the screen honours the CRTC start/offset, pel panning and
  line compare. INT 10h draws pixels and text in all of them, with the
  program's own font from INT 1Fh (CGA characters 128-255) or INT 43h.
- Sound: Sound Blaster Pro 2.0 (220h, IRQ 5, DMA 1) with OPL3, AdLib (388h)
  and MPU-401 General MIDI (330h), from SBPRO; PC speaker. Played through HD
  Audio, AC'97 or a real Sound Blaster (`audio=hda|hdmi|ac97|sb|off`,
  `latency=ms`, as in baremetaldoom). `BLASTER=A220 I5 D1 T4 P330` is set.
  A real Sound Blaster's DMA ring is in 64 KB reserved below 16 MB.
- Virtual 8259 pair, 8254 (guest can reprogram channel 0), 8042, port 61h,
  A20 (port 92h, 8042, INT 15h), VGA DAC/CRTC/attribute/status ports.
- Text and mode 13h drawn to the GRUB/GOP framebuffer (any size, 15/16/24/32 bpp,
  above 4 GiB too, via PAE), or Bochs VBE under QEMU `-kernel`.

## Not yet

- Mode X's 240-line timing, VBE 3.0, EMS, SB16 (16-bit)
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
| `boot/boot.asm`, `boot/boot32lb.asm` | FreeDOS FAT16 and FAT32 LBA boot sectors (from the FreeDOS kernel, GPL) |
| `efi/loader.c`, `efi/tramp.S` | vmdos.efi: UEFI loader, long mode to 32-bit handoff |
| `src/usb.cpp`, `src/pci.cpp` | xHCI keyboard driver (from baremetaldoom) |
| `src/audio.cpp`, `src/sound.c` | sound card driver (from baremetaldoom), SB glue |
| `src/sb/` | SBPRO core: DSP, playback + virtual 8237, OPL3 (dbopl), GM synth, MPU-401 |
| `src/mouse.c` | PS/2 + USB mouse, INT 33h |
| `src/disk.c`, `src/ahci.cpp`, `src/fat.c` | drive C: (AHCI partition or RAM disk), SATA driver, FAT reader |
| `src/dpmi.c` | DPMI host |
| `src/mememu.c` | instruction emulator for the trapped 16-colour VGA window |
| `src/xms.c`, `dos/vmxms.asm` | XMS driver; VMXMS.SYS, its DOS-side front |
| `src/cd.c`, `dos/vmcd.asm`, `dos/vmcdtool.asm` | CD-ROM images: monitor side, VMCD.SYS driver, VMCD.COM |
| `third_party/shsucd/` | SHSUCDX by Jason Hood (unmodified, zlib-style licence) |
| `tools/mkdisk.py` | builds dos.img (MBR + FAT16/FAT32 + boot sector + files) |

License: GPL-2.0-or-later (it includes FreeDOS's boot sector).
