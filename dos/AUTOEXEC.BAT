@ECHO OFF
PROMPT $P$G
PATH C:\
SET BLASTER=A220 I5 D1 T4 P330
LH SHSUCDX /Q /D:?VMCD0001 >NUL
REM Mouse: built into vmdos. For a DOS driver instead (CuteMouse): LH CTMOUSE
ECHO.
ECHO FreeDOS on vmdos (virtual-8086 mode)
ECHO.
