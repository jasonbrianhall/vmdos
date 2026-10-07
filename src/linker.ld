/* Linked at 16 MiB. Linear 0..1 MiB+64 KiB belongs to the v86 guest, and
   2..16 MiB is where DPMI clients' memory blocks appear: DOS/4GW's DOS/16M
   core keeps 24-bit linear addresses in places. */
ENTRY(_start)
SECTIONS
{
    . = 16M;
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
