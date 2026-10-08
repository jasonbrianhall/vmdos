@ECHO OFF
PROMPT $P$G
PATH C:\;C:\VMDOS
SET BLASTER=A220 I5 D1 T4 P330
REM Sound Blaster 16 (16-bit sound; Creative's own programs like Parrot
REM want it): take the REM off the next two lines. SB Pro otherwise.
REM C:\VMDOS\VMSB 16 >NUL
REM SET BLASTER=A220 I5 D1 H5 P330 T6
LH C:\VMDOS\SHSUCDX /Q /D:?VMCD0001 >NUL
REM Mouse driver (CuteMouse): some games need it (Warcraft II), most don't,
REM since vmdos has a mouse built in. Not needed? Put REM in front of the LH line.
LH C:\VMDOS\CTMOUSE.COM
ECHO.
ECHO FreeDOS on vmdos (virtual-8086 mode)
ECHO.
