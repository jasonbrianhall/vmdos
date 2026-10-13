# VMHV: one trip into the AMD-V guest and back.
#
# void svm_run(u32 vmcb_pa, u32 host_save_pa, u32 *g)
#   g[0..5] = the guest's EBX ECX EDX ESI EDI EBP (VMRUN keeps EAX, ESP,
#   EIP, EFLAGS and the segments in the VMCB; the rest are ours to carry).
# Called with interrupts off. VMRUN runs with GIF clear and IF set, so a
# real interrupt that arrives while the guest runs ends the run (the INTR
# intercept) and is taken by the host once it sets GIF again; the C side
# opens a window for it (sti; nop; cli) after every exit.

.section .text
.code32
.global svm_run
svm_run:
    push %ebp
    push %ebx
    push %esi
    push %edi
    mov  28(%esp), %eax          # g
    push %eax                    # (%esp) = g; vmcb at 24(%esp), host save at 28(%esp)
    mov  28(%esp), %eax
    vmsave %eax                  # the host's FS, GS, TR, LDTR
    mov  24(%esp), %eax
    vmload %eax                  # the guest's
    mov  (%esp), %edi
    mov  0(%edi), %ebx
    mov  4(%edi), %ecx
    mov  8(%edi), %edx
    mov  12(%edi), %esi
    mov  20(%edi), %ebp
    mov  16(%edi), %edi
    mov  24(%esp), %eax
    clgi
    sti
    vmrun %eax
    cli                          # GIF is clear: nothing gets in between
    vmsave %eax                  # EAX is the VMCB again: keep the guest's FS, GS, TR, LDTR
    push %edi
    mov  4(%esp), %edi           # g
    mov  %ebx, 0(%edi)
    mov  %ecx, 4(%edi)
    mov  %edx, 8(%edi)
    mov  %esi, 12(%edi)
    mov  %ebp, 20(%edi)
    pop  %eax
    mov  %eax, 16(%edi)
    mov  28(%esp), %eax
    vmload %eax                  # the host's back
    stgi
    add  $4, %esp
    pop  %edi
    pop  %esi
    pop  %ebx
    pop  %ebp
    ret

.section .note.GNU-stack,"",@progbits
