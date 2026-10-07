# Long mode -> 32-bit protected mode (paging off), then into the kernel the way
# a Multiboot loader would enter it. Position-independent: efi/loader.c copies
# it to a page below 4 GiB and calls it after ExitBootServices with
#   rdi = struct handoff { u32 src, dst, copy_len, zero_len, mbi, entry; }
# (also below 4 GiB). Never returns.

.text
.global tramp_start, tramp_end
.code64
tramp_start:
    cli
    mov  %rdi, %rbx
    sub  $16, %rsp               # GDTR on the stack (the page may be read-only)
    movw $23, (%rsp)
    lea  tgdt(%rip), %rax
    mov  %rax, 2(%rsp)
    lgdt (%rsp)
    lea  t32(%rip), %rax
    pushq $0x08
    push %rax
    lretq                        # to 32-bit compatibility mode

.code32
t32:
    mov  $0x10, %eax
    mov  %eax, %ds
    mov  %eax, %es
    mov  %eax, %ss
    mov  %eax, %fs
    mov  %eax, %gs
    mov  %cr0, %eax
    and  $0x7FFFFFFF, %eax       # paging off: leaves long mode
    mov  %eax, %cr0
    jmp  1f
1:  mov  $0xC0000080, %ecx       # EFER.LME off
    rdmsr
    and  $~0x100, %eax
    wrmsr
    mov  %cr4, %eax
    and  $~0x20, %eax            # PAE off (the kernel sets up its own paging)
    mov  %eax, %cr4
    cld
    mov  0(%ebx), %esi           # kernel copy (if it couldn't be placed earlier)
    mov  4(%ebx), %edi
    mov  8(%ebx), %ecx
    rep movsb
    mov  12(%ebx), %ecx          # zero .bss
    xor  %eax, %eax
    rep stosb
    mov  20(%ebx), %edx
    mov  16(%ebx), %ebx
    mov  $0x2BADB002, %eax
    jmp  *%edx

.align 8
tgdt:
    .quad 0
    .quad 0x00CF9A000000FFFF     # 0x08 32-bit code
    .quad 0x00CF92000000FFFF     # 0x10 data
tramp_end:

.section .note.GNU-stack,"",@progbits
