# FreeDOS FAT16 / FAT32 (LBA) boot sectors (boot/), laid over C:'s boot
# sector in memory at boot (bios_boot).
.section .rodata
.global fd_boot16, fd_boot32
fd_boot16:
    .incbin FAT16_BIN
fd_boot32:
    .incbin FAT32_BIN
.section .note.GNU-stack,"",@progbits
