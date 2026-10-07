# The 64 KiB BIOS segment image (bios.asm), copied to F000:0000 at boot.
.section .rodata
.global bios_bin, bios_bin_end
bios_bin:
    .incbin BIOS_BIN
bios_bin_end:
.section .note.GNU-stack,"",@progbits
