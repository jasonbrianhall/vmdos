# vmdos

FreeDOS running in virtual-8086 mode under a small 32-bit protected-mode
kernel. Boots from GRUB (BIOS or UEFI) or QEMU `-kernel`. The kernel emulates
the BIOS and the PC hardware DOS touches; FreeDOS's kernel and FreeCOM run
unmodified. Drive C: is the FAT partition of the EFI disk (SATA, IDE or USB), so
changes are kept; a RAM disk (dos.img) is the option and the fallback.

## Build and run

```
# Fedora: sudo dnf install gcc nasm python3 mtools dosfstools gnu-efi-devel glibc-devel.i686 libgcc.i686 \
#         grub2-tools-extra grub2-pc-modules grub2-efi-x64-modules xorriso qemu-system-x86 edk2-ovmf
sudo apt install build-essential gcc-multilib nasm python3 mtools dosfstools gnu-efi \
                 grub-pc-bin grub-efi-amd64-bin grub-common xorriso qemu-system-x86 ovmf
make            # vmdos.elf (first run fetches KERNEL.SYS/COMMAND.COM into freedos/)
make efi        # vmdos.efi: a UEFI application, no GRUB needed
make esp        # esp.img: the EFI disk, also C: (FreeDOS at its root, EFI/BOOT/BOOTX64.EFI); dd it to a disk
make iso        # vmdos.iso (GRUB), hybrid BIOS/UEFI
make run        # QEMU, BIOS (-kernel)
make run-efi-app  # QEMU, UEFI (OVMF), booting esp.img
make run-efi    # QEMU, UEFI, from vmdos.iso
```

### Booting from a USB stick

Ready-made image: the [releases](https://github.com/jasonbrianhall/vmdos/releases)
have `vmdos-usb.img.xz` (FreeDOS + vmdos, no games, a 2000 MiB C:) built by
GitHub Actions (`.github/workflows/release.yml`: a `v*` tag makes a release,
running the workflow by hand updates the `latest` pre-release). Write it with
`xz -dc vmdos-usb.img.xz | sudo dd of=/dev/sdX bs=4M conv=fsync`, or Etcher /
Rufus. Or build your own with games:

```
make esp FRESH=1 EXTRA=games ISO="war2.iso"   # esp.img: EFI boot files + FreeDOS + your games
sudo dd if=esp.img of=/dev/sdX bs=4M conv=fsync   # sdX = the stick (lsblk); everything on it is replaced
```

Plug it in, pick the stick in the PC's boot menu (UEFI or legacy BIOS). The stick is then
C: (vmdos reads it through its own USB driver: xHCI, EHCI or UHCI, bulk-only mass
storage, USB 2 and 3), so changes and saved games stay on it. Turn Secure
Boot off (vmdos.efi isn't signed). `make run-usb` tries the same in QEMU.
Afterwards the stick can be mounted on Linux to add games (it's FAT).

### Boot menu (UEFI and legacy BIOS)

esp.img boots both ways through GRUB: on UEFI, `EFI/BOOT/BOOTX64.EFI` is
GRUB, which chainloads `EFI/vmdos/vmdos.efi`; on a legacy BIOS (or CSM),
GRUB's boot code in the MBR and in the gap before the partition boots
`boot/vmdos.elf`. Both read `boot/grub/grub.cfg` (made by `make esp`). Your
own entries go in `boot/grub/custom.cfg` on the stick, which nothing
overwrites. The menu waits 5 s; its second entry boots with `noxms` (no
XMS driver, for Windows 3.1 Setup):

```
menuentry "QuickBASIC clone" {
    if [ "$grub_platform" = "efi" ]; then chainloader /EFI/qb/qb.efi
    else multiboot /qb/qb.elf; fi
}
```

`make run-bios` boots esp.img under SeaBIOS; `GRUB=0` (or no GRUB tools:
Fedora grub2-tools grub2-pc-modules grub2-efi-x64-modules, Debian
grub-common grub-pc-bin grub-efi-amd64-bin) makes the old UEFI-only image
with vmdos.efi as BOOTX64.EFI.

### Drive C:

The RAM disk (dos.img) is only built and packed with `RAMDISK=1` (or
`C=ram`): then esp.img carries EFI/BOOT/dos.img, the ISO carries the GRUB
module and `make run` passes it to QEMU, as a fallback for machines whose
disk vmdos can't drive (not SATA, IDE or USB) and for `c=ram`.

C:\ holds FreeDOS (KERNEL.SYS, COMMAND.COM), FDCONFIG.SYS and AUTOEXEC.BAT;
vmdos's drivers and tools (VMXMS.SYS, VMCD.SYS, VMCD, VMSPEED, SHSUCDX,
CTMOUSE) are in C:\VMDOS, which is on the PATH.

`C=disk` (default): C: is the FAT partition of `esp.img`, which the run
targets attach as a SATA (AHCI) disk (`run-usb`: as a USB stick); what DOS writes stays there. Rebuilding
updates esp.img in place: programs and FreeDOS files are refreshed,
AUTOEXEC.BAT, FDCONFIG.SYS and files already copied from `EXTRA` are kept
(`FRESH=1` starts over). `C=ram`: C: is dos.img in RAM, changes lost (kernel
option `c=ram`; `make run-efi-app KARGS=c=ram` writes it to
EFI/vmdos/vmdos.cfg, which vmdos.efi reads, and to boot/grub/grub.cfg).

The kernel picks C: from the FAT partitions on SATA (AHCI) disks, IDE disks
(parallel ATA, or SATA in IDE mode; PIO, `ide=off` skips them) and USB
sticks that hold KERNEL.SYS at their root, preferring the one vmdos.efi
started from (it passes the partition's start and disk signature); other
partitions are only touched when you put one in a VMHD drive (below; DOS
sees a one-partition C: and can't write outside it). No such partition, or a disk vmdos can't drive (NVMe): the RAM
disk if built with `RAMDISK=1`. To use an
existing ESP on a real machine, copy esp.img's root files (KERNEL.SYS,
COMMAND.COM, FDCONFIG.SYS, AUTOEXEC.BAT and the VMDOS folder)
to the ESP's root and vmdos.efi + dos.img to a folder on it.

#### Other disks: VMHD

Partitions of the other disks (SATA, IDE, USB, sticks plugged in later
too) become DOS drives only when you ask. VMHD.SYS reserves empty drive
letters at boot (`DEVICE=C:\VMDOS\VMHD.SYS` in FDCONFIG.SYS: D: and E:;
`VMHD.SYS 4` for four, up to 8; an install updated from an older one has to
add the line), and VMHD.COM fills them:

    VMHD                 lists the drives and every disk's partitions
    VMHD D: 3            puts partition 3 in D:
    VMHD D: 3 /R         ... read-only
    VMHD D: /E           takes it out

Only a FAT partition (FAT12/16/32; MBR primary or logical, GPT, or a disk
without a partition table) goes in, never C: and never one that's in
another drive. One with FAT but a partition type that isn't DOS's (an EFI
system partition, a Linux- or hidden-typed one, a GPT partition other than
basic data) needs a Y first.

FDISK (FreeDOS's, not included) works as on any PC: the BIOS shows every
other disk as hard disk 81h, 82h ... (C:'s disk is 80h, a one-partition
view of C:). They appear once the DOS kernel has set up its drives, so DOS
never gives their partitions letters by itself. A partition FDISK made
gets its letter from VMHD (`VMHD D: 2`; no file system yet: a Y first),
and `FORMAT D:` formats it (VMHD.SYS gives FORMAT the device parameters,
FAT16 or FAT32 by the partition's type and size). No restart needed. VMHD's drives come before the CD drives, so
those move along: the first CD drive is F: with two VMHD drives.

QEMU targets use KVM when `/dev/kvm` is usable (else plain emulation, which
is many times slower; `ACCEL=` overrides). `SOUND=hda|ac97|sb|none` (default hda), `AUDIODEV=pa|alsa|sdl|wav`
(default pa: PulseAudio/PipeWire; wav records vmdos.wav), `USB=1` for a USB
keyboard and mouse, `USBHOST=0079:0011` to hand the PC's own USB devices
(vendor:product as `lsusb` shows them; a gamepad, say) to vmdos on an xHCI
controller (QEMU needs access to /dev/bus/usb: root or a udev rule), `QEMU_MEM=` (512), `KARGS="debug=2 ..."` for the ISO's kernel command line.

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

Screen: the picture is scaled to the largest 4:3 box that fits (text too),
black bars at the sides of a wide screen; `aspect=fill` on the kernel command
line (KARGS, or EFI/vmdos/vmdos.cfg on the stick) stretches it to the whole
screen. The framebuffer is mapped write-combining (`nowc` turns that off).

Slowdown for games that time themselves by the CPU (as MoSlo does):
`VMSPEED 2` in DOS runs at 2% of full speed (`VMSPEED 0.5`, `VMSPEED 100`,
`VMSPEED` alone shows it), Ctrl+Shift+F11 / Ctrl+Shift+F12 step it slower /
faster while a program runs (shown at the top right), and `speed=N` on the
kernel command line sets it from boot (`KARGS="speed=2"` for the ISO).

Sound output: when vmdos finds more than one (speakers/headphones, HDMI or
DisplayPort, a second card, AC'97, a Sound Blaster, the PC speaker),
Ctrl+Shift+F2 switches to the next one; the new one is shown at the top right.

Ctrl+Shift+F10 shows vmdos's log over whatever is running (handy on a PC
with no serial port, when a game misbehaves): the game pauses, the arrow
keys, PgUp/PgDn and Home/End scroll, and Esc goes back to the game. The
"vmdos stopped" screen shows the same log.

CD-ROM: any ISO file on C: can be put in the CD drive (F:, after VMHD's D:
and E:; D: without VMHD.SYS) while running: `VMCD F: C:\ISOS\WAR2.ISO` (or `VMCD 1 WAR2.ISO`, relative paths work; 8.3
names). It is read straight from the disk, nothing is copied to RAM; `VMCD`
lists the drives. `ISO="game.iso disc2.iso"` copies ISOs to C:\ISOS (8.3
names) and puts the first in the drive at boot (`cd=/ISOS/GAME.ISO` on the
kernel command line). `cdrives=N` gives up to 4 drives. With `C=ram`, ISO=
images are held in RAM as boot modules instead, one drive each, and
`VMCD 1 2` swaps them (also: ISOs next to vmdos.efi, GRUB module lines).

Disk speed: C: on a real disk is cached (8 MiB by default, `diskcache=MB` on
the kernel command line, 0 to turn it off). Reads fetch 32 KiB at a time and
sequential writes are gathered into one disk command; gathered writes reach
the disk within 50 ms (and before a restart), so switch off a moment after
saving, not in the same instant.

Floppy drives: A: and B: hold disk images from C: (there is no floppy
controller): `VMFD A: C:\DISKS\DISK1.IMG`, `VMFD B: GAME2.IMG /R` (read-only),
`VMFD A: /E` (empty), `VMFD` alone lists them; `fda=/DISKS/BOOT.IMG` (and
`fdb=`) on the kernel command line fill them at boot. A new image is a disk
change to DOS, so "insert disk 2" works; an empty drive says "not ready".
Writes go into the image file on C:, and are refused if that file has been
replaced, resized or moved since it went in. 160K to 2.88M images, DMF
included; other sizes are taken from the boot sector.

Self-booting floppies (booter games): `VMFD A: C:\DISKS\GAME.IMG /BOOT`
restarts the PC from that disk, without DOS (`VMFD A: /BOOT` boots what's in
A: already); `fda=/DISKS/GAME.IMG boot=a` on the kernel command line boots it
instead of DOS. To get back to DOS, restart the machine.

CUE/BIN images work like ISOs (`VMCD F: C:\GAMES\QUAKE.CUE`, `cd=` too), with
their CD audio tracks: one BIN or one per track, MODE1/2048, MODE1/2352,
MODE2/2352, MODE2/2336 and AUDIO tracks, PREGAP. C: only has 8.3 names, so
the BIN names in the sheet have to be 8.3 ones (a single BIN may also just be
named like the sheet: QUAKE.CUE + QUAKE.BIN).

Real CD/DVD drives on SATA (AHCI), IDE and USB get a drive letter each after the
image drives (G: with one image drive and VMHD's two before it): the disc is read from the drive,
swapping discs works, and eject/close requests go to the drive. `cdphys=off`
leaves them out. In QEMU: `-M q35 -drive if=none,id=cd,media=cdrom,file=game.iso
-device ide-cd,drive=cd,bus=ide.2`, `-drive file=game.iso,if=ide,index=2,media=cdrom`
(IDE, the default machine), or `-device qemu-xhci -drive
if=none,id=ucd,media=cdrom,file=game.iso -device usb-storage,drive=ucd` for USB.

CD audio (MSCDEX play, stop/pause, resume, track position, audio status,
volume) plays through the sound card, mixed in with the Sound Blaster: from
a CUE image's audio tracks, or read digitally from a real drive. A drive that
can't read audio digitally is told to play the tracks itself, through its own
output (a headphone jack, or an analog cable to a sound card). With no sound
card a play still runs for its length, so games waiting on a track go on.
`tests/cdplay.asm` (CDPLAY.COM) plays a track and shows the position.

Mouse trouble in a game: boot with `KARGS="mouselog"` (or `mouselog` in the
load options); the log shows every INT 33h call, event-handler call and
command sent to the PS/2 mouse port. If the built-in driver doesn't satisfy
a game, try CuteMouse (loaded by AUTOEXEC.BAT; see below).

The kernel loads at 16 MiB.
Give a machine or VM at least 256 MB (QEMU targets use 512 MB, `QEMU_MEM=`): GRUB needs room to unpack
dos.img.gz and place it in one piece, or it stops with "out of memory". Ctrl+Alt+Del restarts.

## What works

- FreeDOS 1.x kernel and FreeCOM to the `C:\>` prompt: GRUB on BIOS or UEFI,
  or `vmdos.efi` straight from the UEFI firmware.
- BIOS: INT 10h (text modes, mode 13h, DAC/palette), 11h, 12h, 13h (CHS and
  LBA), 15h (A20, wait, config), 16h, 1Ah (RTC time/date), keyboard IRQ.
- USB controllers: xHCI (USB 3), EHCI (USB 2) and UHCI (USB 1.1, the
  companions EHCI hands low/full-speed devices to on Intel boards and in
  QEMU/libvirt); OHCI (AMD/NVIDIA companions) not yet. Everything below
  works on any of them.
- Keyboards: PS/2, and USB (boot protocol, hubs, hot-plug; polled).
  `usb=off` on the command line skips USB.
- Joysticks: USB gamepads as the PC game port (201h, INT 15h AH=84h); the
  first is joystick A, a second joystick B (with one, its buttons 3 and 4
  are B's). Any HID gamepad with X/Y or a hat switch; the DragonRise SNES
  clones (0079:0011) by their own layout: D-pad, B = button 1, A = 2,
  Y = 3, X = 4. Games calibrate as with any analog stick. The game port
  has four buttons, so L, R, Select and Start (other pads: buttons 5, 6,
  9, 10) do nothing, unless `joykeys=39,2A,01,1C` on the kernel command
  line (vmdos.cfg) makes them press keys: set-1 scancodes in hex for L,
  R, Select, Start (here Space, Left Shift, Esc, Enter), 1xx for
  E0-prefixed ones, 0 for none.
- Mouse: PS/2 and USB mice behind an INT 33h driver in the monitor (no
  MOUSE.COM): position, buttons, ranges, mickeys, press/release counts and
  the program's event handler (0Ch/14h; for DOS extenders' programs too,
  run from protected mode through their real-mode callback), and a PS/2
  mouse on the virtual keyboard controller (IRQ 12 packets) for programs
  with their own mouse code. The pointer is drawn over text and
  mode 13h by the renderer. BIOS PS/2 mouse services (INT 15h C2xx, IRQ 12
  handler) let a real DOS driver run too: AUTOEXEC.BAT loads CuteMouse
  (`LH C:\VMDOS\CTMOUSE.COM`), which takes over INT 33h. Some games need it
  (Warcraft II), most don't: put REM in front of that line to use the
  built-in driver.
- XMS 3.0 in the monitor (don't load HIMEM): HMA, extended memory (1 GB or half the free
  RAM below 4 GB, `xms=MB`; the old XMS 2.0 calls report at most 64 MB) and 96 KB of upper memory (C800h-DFFFh; 160 KB to EFFFh with
  `ems=0`). `VMXMS.SYS`, loaded first in FDCONFIG.SYS, is the HIMEM-style
  front (device XMSXXXX0, INT 2Fh hook). `DOS=HIGH,UMB`, LOADHIGH/DEVICEHIGH
  and FreeCOM's XMS swapping work: about 620 KB free for programs.
  `noxms` hides the XMS driver altogether (no installation check answer,
  VMXMS.SYS doesn't load, DOS loads low), for software that brings its
  own, such as Windows 3.1x Setup.
- EMS 4.0 (expanded memory, INT 67h) in the monitor, as EMM386 gives it:
  32 MB (`ems=MB`, `ems=0` for none), page frame E000h, functions 40h-5Ch
  (allocate, map, map multiple, reallocate, save/restore and partial page
  maps, handle names and directory, move/exchange region, mappable pages,
  hardware info). `VMEMS.SYS` is the EMMXXXX0 device programs look for
  (Master of Magic). No VCPI: DOS extenders use the DPMI host.
- DPMI 0.9 host in the monitor, for DOS extenders: DOS/4GW (DOOM runs: demo,
  menus, keyboard, mouse, Sound Blaster effects and music), PMODE/W. Clients run at ring 3 with LDT descriptors;
  INT 31h descriptor, memory, interrupt, real-mode call/callback, DOS
  memory, physical-mapping and virtual-IF services; protected-mode hardware
  interrupt and exception handlers; selector 0040h for the BIOS data
  area. Client memory appears at linear
  2-16 MiB (DOS/4GW's DOS/16M core keeps 24-bit addresses), so the kernel
  now loads at 16 MiB; what doesn't fit there comes from above 16 MiB, up to
  1 GB in all (`dpmi=MB`), and 0503h grows blocks in place where it can
  (DJGPP programs such as Quake). Not yet: DOS/32A (it insists on its own XMS mode).
- VESA BIOS 2.0: 640x400 to 1024x768 in 8, 15, 16 and 32 bits per pixel,
  4 MB, banked window (4F05h / WinFuncPtr) and linear framebuffer (DPMI
  0800h maps it), scan line length, display start, palette, and the
  protected-mode interface (4F00h-4F0Ah).
- Graphics: CGA modes 4, 5, 6 (palettes and background via INT 10h AH=0Bh
  or port 3D9h), mode 13h, unchained 256-color (Mode X/Y, as DOOM uses:
  single-plane access mapped directly, latch copies and the rest emulated),
  and the EGA/VGA 16-color modes 0Dh, 0Eh, 10h, 12h. In the 16-color modes
  every access to A000h faults and the instruction is emulated against the
  four planes with the VGA's latches, write modes 0-3, set/reset, bit mask
  and read modes; the screen honors the CRTC start/offset, pel panning and
  line compare. INT 10h draws pixels and text in all of them, with the
  program's own font from INT 1Fh (CGA characters 128-255) or INT 43h.
- Sound: Sound Blaster Pro 2.0 (220h, IRQ 5, DMA 1) with OPL3, AdLib (388h)
  and MPU-401 General MIDI (330h), from SBPRO; PC speaker. Played through HD
  Audio, AC'97 or a real Sound Blaster (`audio=hda|hdmi|ac97|sb|off`,
  `latency=ms`, as in baremetaldoom). `BLASTER=A220 I5 D1 T4 P330` is set.
  A real Sound Blaster's DMA ring is in 64 KB reserved below 16 MB.
  Recording (DSP 20h/24h/2Ch/98h/99h, 8-bit) from the PC's mic jack (else
  internal mic, else line in) when playing through HD Audio analog;
  silence otherwise. Test: `tests/sbrec.asm`.
  `VMSB 16` (or `sb16=1`) switches to a Sound Blaster 16 (DSP 4.05: 16-bit
  play/record on DMA 5, `BLASTER=A220 I5 D1 H5 P330 T6`); `VMSB PRO` back.
  See AUTOEXEC.BAT. Test: `tests/sb16test.asm`.
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
| `src/usb.cpp`, `src/usb2.inc`, `src/pci.cpp` | USB: xHCI (usb.cpp), EHCI and UHCI (usb2.inc); keyboards, mice (from baremetaldoom), gamepads, hubs and mass storage (C: on a stick) |
| `src/audio.cpp`, `src/sound.c` | sound card driver (from baremetaldoom), SB glue |
| `src/sb/` | SBPRO core: DSP, playback + virtual 8237, OPL3 (dbopl), GM synth, MPU-401 |
| `src/mouse.c` | PS/2 + USB mouse, INT 33h |
| `src/disk.c`, `src/ahci.cpp`, `src/ide.cpp`, `src/fat.c` | drive C: (SATA / IDE / USB partition or RAM disk), SATA and IDE drivers, FAT reader |
| `src/dpmi.c` | DPMI host |
| `src/mememu.c` | instruction emulator for the trapped 16-color VGA window |
| `src/xms.c`, `dos/vmxms.asm` | XMS driver; VMXMS.SYS, its DOS-side front |
| `src/floppy.c`, `dos/vmfd.asm` | Floppy drives A:/B: from disk images on C:, VMFD.COM |
| `src/cd.c`, `dos/vmcd.asm`, `dos/vmcdtool.asm` | CD-ROM: ISO and CUE/BIN images, real SATA/IDE/USB drives, CD audio (monitor side), VMCD.SYS driver, VMCD.COM |
| `third_party/shsucd/` | SHSUCDX by Jason Hood (unmodified, zlib-style licence) |
| `tools/mkdisk.py` | builds dos.img (MBR + FAT16/FAT32 + boot sector + files) |

License: GPL-2.0-or-later (it includes FreeDOS's boot sector).
