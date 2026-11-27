@ECHO OFF
REM KDOSEXEC.BAT: run at the start of the vmdos command shell (boot option
REM shell=kernel), in place of AUTOEXEC.BAT (which is FreeDOS's). Before it
REM the shell has already set PATH, MTCPCFG and BLASTER=A220 I5 D1 T4 P330,
REM and given every CD drive a letter (D:, E: ...): no VMCD.SYS, SHSUCDX or
REM DOSLFN here, this DOS has CD drives and long file names itself.
REM
REM Sound Blaster 16 (16-bit sound; Creative's own programs like Parrot
REM want it): take the REM off the next two lines. SB Pro otherwise.
REM C:\VMDOS\VMSB 16 >NUL
REM SET BLASTER=A220 I5 D1 H5 P330 T6
REM
REM Gravis UltraSound: take the REM off the next two lines. Games that see
REM ULTRASND use it in place of the Sound Blaster; for MIDI music on it
REM (Doom, ULTRAMID) they want Gravis's patches in C:\ULTRASND\MIDI.
REM The card is at 240h, IRQ 7, DMA 3 (boot option gus=2X0 moves it,
REM gus=off takes it away; keep ULTRASND matching).
REM SET ULTRASND=240,3,3,7,7
REM SET ULTRADIR=C:\ULTRASND
REM
REM A CD image in drive D: at the start (VMCD with nothing after it lists
REM the drives and images; VMCD D: 0 empties D:):
REM VMCD D: C:\ISOS\GAME.ISO >NUL
REM
REM A folder QEMU shares (make run SHARE=dir) as drive V:, with long names:
REM VMDOSNET V: share
REM
REM Keyboard layout other than US: take the REM off and give yours (MKEYB
REM with a question-mark switch lists them: GR German, FR French, UK ...).
REM C:\VMDOS\MKEYB.EXE GR
REM
REM Mouse driver (CuteMouse): some games need it (Warcraft II), most don't,
REM since vmdos has a mouse built in.
REM C:\VMDOS\CTMOUSE.COM
REM
REM DIR sorted with directories first:
SET DIRCMD=/O:GN
