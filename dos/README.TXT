Virtual Monitor DOS - Quick References
=======================

KEYBOARD SHORTCUTS (work in any game or at the DOS prompt)

  Ctrl+Shift+F2    Next sound output (PC Speaker, AC97, HDA, SB, or USB Audio)
  Ctrl+Shift+F3    Volume down 10%
  Ctrl+Shift+F4    Volume up 10%
  Ctrl+Shift+F5    Mute / unmute
  Ctrl+Shift+F8    Smooth scaling on / off (games like Warcraft 2 look much better but games like DOOM do not)
  Ctrl+Shift+F9    Screenshot (kept in memory; save it with VMSHOT)
  Ctrl+Shift+F10   vmdos log (game pauses; arrows/PgUp/PgDn scroll,
                     Esc goes back)
  Ctrl+Shift+F11   Slower (for games that run too fast)
  Ctrl+Shift+F12   Faster
  Ctrl+Alt+Del     Restart

  Changes show briefly at the top right of the screen.

DOS COMMANDS (in C:\VMDOS, on the PATH)

  VMSHOT [file]         Save the last Ctrl+Shift+F9 screenshot as a BMP
                          (default VMSHOT.BMP)
  VMSPEED [percent]     Show or set the speed (VMSPEED 2, VMSPEED 0.05, VMSPEED 100)
  DMESG                 vmdos's log (DMESG > LOG.TXT, DMESG | MORE)
  DMESG /K              Make C:\VMDOS\VMDOS.LOG: vmdos saves its log there
                          when it stops, or on S in the Ctrl+Shift+F10 log
                          (for a crash or hang: read it after restarting)
  LSPCI [/V] [/X]       The PCI devices (/V details, /X config space in hex)
  LSUSB [/V]            The USB controllers and devices (/V classes)
  VMCD                  List CD drives and images
  VMCD F: C:\ISOS\X.ISO Put an ISO (or CUE sheet) in a CD drive (has to be on C:)
  VMFD A: DISK1.IMG     Put a floppy image in A: or B: (/E takes it out,
                          /R read-only, /BOOT boots it; will only boot 16 bit code)
  VMHD                  List other disks' partitions
  VMHD D: 3             Put partition 3 in D: (/R read-only, /E out)
  VMPKT                 Packet driver for the network card (INT 60h)
  VMIPX                 IPX for network games (load VMPKT first)
  VMNIC [n | AUTO]      List the network cards, or pick the one to use
  VMSB 16 | PRO         Sound Blaster 16 or Pro (VMSB alone shows which)
  VMSB MIC              Mic level meter (written for parrot but the parrot game doesn't
                          work because of the way it handles audio)

SCREEN RESOLUTION AND SCALING

  vmdos can't switch the PC's real screen mode while DOS runs: the
  firmware only sets modes before the kernel starts, and switching later
  would need a driver for each graphics card. So the screen mode is
  picked once at boot (boot menu, "screen resolution", or vga=) and every
  DOS mode is scaled into it. Your monitor would scale anyway: a flat
  panel only shows its own size. 640x480 games look best at vga=640x480
  (drawn 1:1, the monitor stretches it) or vga=max (vmdos scales, the
  monitor doesn't).

BOOT OPTIONS

  Set when building the stick:  make esp KARGS="scale=smooth speed=50"
  or add them on the stick: EFI\VMDOS\VMDOS.CFG (one line, options
  separated by spaces) for a UEFI boot, the multiboot lines in
  BOOT\GRUB\GRUB.CFG for a legacy BIOS boot. The log's "cmdline:" line
  shows what the kernel got.

  Screen
    (default)      Whole-pixel scaling to the largest 4:3 box that fits
    scale=smooth   Blended (bilinear) scaling; Ctrl+Shift+F8 toggles it
    scale=sharp    Whole-pixel scaling, no blending (the default)
    scale=off      No resampling: 640x480 is drawn 1:1, centered, with
                   black borders (whole multiples on bigger screens)
    aspect=fill    Stretch to the whole screen
    nowc           Framebuffer not write-combining
    vga=ask        UEFI: list the screen modes at boot and ask which
    vga=N          UEFI: screen mode N (the number vga=ask shows)
    vga=WxH        UEFI: the mode of that size (vga=640x480)
    vga=max        UEFI: the largest mode (usually the panel's own size)
                   On a legacy BIOS boot, pick the size in the boot menu
                   instead ("screen resolution"), which UEFI has too.
  Speed
    speed=N        Run at N% of full speed from boot
  Sound
    audio=usb|hda|hdmi|ac97|sb|off   Pick the sound output
    latency=ms     Sound buffer length
    sb16=1         Sound Blaster 16 instead of Pro
  Memory
    noxms          No XMS driver (also in the boot menu)
    xms=MB  ems=MB  ems=0  dpmi=MB
  Disks
    cd=/ISOS/GAME.ISO     ISO in the CD drive at boot
    cdrives=N             Up to 4 CD drives
    fda=/DISKS/BOOT.IMG   Floppy image in A: at boot (boot=a boots it)
    diskcache=MB          C: cache (0 turns it off)
    usb=off  ide=off  ehci=off  c=ram
  Trouble
    debug=1|2|3    More in the log
    mouselog       Log the mouse driver's calls
    bootlog=off    No boot messages on screen

More: README.md at https://github.com/jasonbrianhall/vmdos
