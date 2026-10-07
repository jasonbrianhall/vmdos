# Multiboot (v1) entry, interrupt stubs, and the way into virtual-8086 mode.

.set MB_MAGIC,  0x1BADB002
.set MB_FLAGS,  (1<<0) | (1<<1) | (1<<2)   # page-align modules, memory info, video mode
.set MB_CHECK,  -(MB_MAGIC + MB_FLAGS)

.section .multiboot, "a"
.align 4
    .long MB_MAGIC, MB_FLAGS, MB_CHECK
    .long 0, 0, 0, 0, 0          # a.out fields (unused for ELF)
    .long 0                      # mode_type: linear graphics
    .long FB_W, FB_H, 32         # preferred width, height, depth

.section .bss
.align 16
boot_stack: .skip 64 << 10
.global kernel_stack_top
boot_stack_top:
.align 16
v86_stack:  .skip 64 << 10       # ring-0 stack for traps from the guest (TSS.esp0)
.global v86_stack_top
v86_stack_top:

.section .data
.global fpu_present
fpu_present: .long 0
fpu_sw:   .word 0x5A5A
.global kernel_stack_top
kernel_stack_top: .long v86_stack_top

.align 8
.global gdt
gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF     # 0x08 code
    .quad 0x00CF92000000FFFF     # 0x10 data
    .quad 0                      # 0x18 TSS, filled in by cpu.c
gdt_ptr:
    .word gdt_ptr - gdt - 1
    .long gdt

.section .text
.code32
.global _start
_start:
    cli
    mov  $boot_stack_top, %esp
    mov  %eax, %esi              # multiboot magic
    mov  %ebx, %edi              # multiboot info
    mov  $__bss_start, %edx      # zero .bss
1:  cmp  $__bss_end, %edx
    jae  2f
    movb $0, (%edx)
    inc  %edx
    jmp  1b
2:  mov  $boot_stack_top, %esp
    lgdt gdt_ptr
    ljmp $0x08, $3f
3:  mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %ss
    mov  %ax, %fs
    mov  %ax, %gs
    # x87: FNINIT then FNSTSW leaves the status word alone when there's none.
    mov  %cr0, %eax
    and  $~((1<<2) | (1<<3)), %eax   # EM, TS off
    mov  %eax, %cr0
    fninit
    fnstsw fpu_sw
    cmpw $0, fpu_sw
    jne  5f
    movl $1, fpu_present
    or   $((1<<1) | (1<<5)), %eax    # MP; NE (native FPU errors, 486 and later)
    jmp  6f
5:  or   $(1<<2), %eax               # EM: no FPU, x87 instructions trap
6:  mov  %eax, %cr0
    push %edi
    push %esi
    call kmain
4:  cli
    hlt
    jmp  4b

.global reload_gdt
reload_gdt:
    lgdt gdt_ptr
    ret

# ---- interrupt stubs: vectors 0..47 ----
.macro STUB n
isr\n:
.if (\n == 8) || (\n == 10) || (\n == 11) || (\n == 12) || (\n == 13) || (\n == 14) || (\n == 17)
.else
    push $0
.endif
    push $\n
    jmp  isr_common
.endm

.altmacro
.set i, 0
.rept 48
    STUB %i
    .set i, i+1
.endr

.macro ISRADDR n
    .long isr\n
.endm
.section .rodata
.global isr_table
isr_table:
.set i, 0
.rept 48
    ISRADDR %i
    .set i, i+1
.endr
.section .text

isr_common:
    pusha
    push %ds
    push %es
    push %fs
    push %gs
    mov  $0x10, %ax
    mov  %ax, %ds
    mov  %ax, %es
    mov  %ax, %fs
    mov  %ax, %gs
    cld
    push %esp
    call isr_dispatch
    add  $4, %esp
isr_return:
    pop  %gs
    pop  %fs
    pop  %es
    pop  %ds
    popa
    add  $8, %esp
    iret

# v86_enter(struct regs *r): load a v86 frame and go.
.global v86_enter
v86_enter:
    mov  4(%esp), %esp
    jmp  isr_return

.section .note.GNU-stack,"",@progbits
