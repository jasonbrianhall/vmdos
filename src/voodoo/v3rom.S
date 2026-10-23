/* The Voodoo 3 3000's BIOS (roms/voodoo3/3k12sd.rom, the file 86Box uses),
   built into vmdos when the build has it; without it voodoo3 stays off. */
        .section .rodata
        .balign 16
        .globl v3_rom_start, v3_rom_end
v3_rom_start:
#ifdef V3ROM
        .incbin V3ROM
#endif
v3_rom_end:
        .section .note.GNU-stack,"",@progbits
