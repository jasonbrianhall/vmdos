/* Linked at 4 MiB: linear 0..1 MiB+64 KiB belongs to the v86 guest. */
ENTRY(_start)
SECTIONS
{
    . = 4M;
    __kernel_start = .;
    .text : ALIGN(4K) {
        KEEP(*(.multiboot))
        *(.text .text.*)
    }
    .rodata : ALIGN(4K) { *(.rodata .rodata.*) }
    .data : ALIGN(4K) { *(.data .data.*) }
    .bss : ALIGN(4K) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        __bss_end = .;
    }
    __kernel_end = .;
    /DISCARD/ : { *(.eh_frame) *(.note*) *(.comment) }
}
