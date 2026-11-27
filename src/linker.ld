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
        EXCLUDE_FILE(*kdos.o) *(.text .text.*)
    }
    __text_end = .;
    .rodata : ALIGN(4K) { EXCLUDE_FILE(*kdos.o) *(.rodata .rodata.*) }
    .init_array : ALIGN(4) {                  /* C++ constructors (kdos): run by kdos_boot */
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array .ctors))
        __init_array_end = .;
    }
    .data : ALIGN(4K) { EXCLUDE_FILE(*kdos.o) *(.data .data.*) }
    .bss : ALIGN(4K) {
        __bss_start = .;
        EXCLUDE_FILE(*kdos.o) *(COMMON)
        EXCLUDE_FILE(*kdos.o) *(.bss .bss.*)
        __bss_end = .;
    }
    __kernel_end = .;
    /* kdos (shell=kernel: src/kdos, DOSBox-X's DOS and shell), all of it
       after the kernel: the heap leaves these pages alone only when
       shell=kernel is given (cpu.c), and uses them like any other free
       memory otherwise. Its zeroed data is cleared by kdos_boot. */
    .kdos : ALIGN(4K) {
        __kdos_start = .;
        *kdos.o(.text .text.* .rodata .rodata.* .gcc_except_table .gcc_except_table.*)
        __kdos_data_start = .;                /* as loaded: kept for a restart (kdos_boot) */
        *kdos.o(.data .data.*)
        __kdos_data_end = .;
    }
    .kdos_bss (NOLOAD) : ALIGN(16) {
        __kdos_bss_start = .;
        *kdos.o(.bss .bss.* COMMON)
        . = ALIGN(4K);
        __kdos_end = .;
    }
    __image_end = .;                          /* what a loader must load and zero (vmdos.efi) */
    /DISCARD/ : { *(.eh_frame) *(.note*) *(.comment) }
}
