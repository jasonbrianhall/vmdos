# The 32-bit kernel (vmdos.elf as a flat binary from its load address).
.section .rodata
.global kernel_image, kernel_image_end
.balign 16
kernel_image:
    .incbin KERNEL_BIN
kernel_image_end:
.section .note.GNU-stack,"",@progbits
