@ECHO OFF
PROMPT $P$G
PATH A:\;A:\VMDOS
SET BLASTER=A220 I5 D1 T4 P330
SET DIRCMD=/O:GN
LH A:\VMDOS\SHSUCDX /Q /D:?VMCD0001 >NUL
LH A:\VMDOS\CTMOUSE.COM >NUL
ECHO.
ECHO FreeDOS on vmdos, from the built-in floppy (A:)
ECHO.
ECHO vmdos found no disk with FreeDOS (KERNEL.SYS) to use as C:, so it
ECHO started from this RAM floppy instead. Changes to A: are lost at power-off.
ECHO VMHD lists the PC's disks and puts a partition in a drive (VMHD C: 1).
ECHO The vmdos log (what it found, and why not C:): Ctrl+Shift+F10.
ECHO.
