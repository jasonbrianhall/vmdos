@ECHO OFF
PROMPT $P$G
PATH C:\;C:\VMDOS;C:\VMDOS\NET
SET MTCPCFG=C:\VMDOS\NET\VMTCP.CFG
SET BLASTER=A220 I5 D1 T4 P330
SET DIRCMD=/O:GN
REM Sound Blaster 16 (16-bit sound; Creative's own programs like Parrot
REM want it): take the REM off the next two lines. SB Pro otherwise.
REM C:\VMDOS\VMSB 16 >NUL
REM SET BLASTER=A220 I5 D1 H5 P330 T6
REM Gravis UltraSound: take the REM off the next two lines. Games that see
REM ULTRASND use it in place of the Sound Blaster; for MIDI music on it
REM (Doom, ULTRAMID) they want Gravis's patches in C:\ULTRASND\MIDI.
REM SET ULTRASND=240,3,3,7,7
REM SET ULTRADIR=C:\ULTRASND
LH C:\VMDOS\SHSUCDX /Q /D:?VMCD0001 >NUL
REM A folder QEMU shares (make run SHARE=dir) as drive V:, with long names:
REM VMDOSNET V: share /LFN
REM Mouse driver (CuteMouse): some games need it (Warcraft II), most don't,
REM since vmdos has a mouse built in. Not needed? Put REM in front of the LH line.
LH C:\VMDOS\CTMOUSE.COM
ECHO.
ECHO FreeDOS on vmdos (virtual-8086 mode)
ECHO.
