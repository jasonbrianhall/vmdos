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

# int vmx_enter(u32 *r, int launched)   (Intel VT-x, hvvmx.c)
#   r[0..6] = the guest's EAX EBX ECX EDX ESI EDI EBP; VMLAUNCH the first
#   time, VMRESUME after. Returns 0 after a VM exit (through
#   vmx_exit_entry, the host RIP in the VMCS, on the stack as it was
#   here: the host RSP is written just before entering), 1 if the entry
#   itself failed.
.global vmx_enter, vmx_exit_entry
vmx_enter:
    push %ebp
    push %ebx
    push %esi
    push %edi
    mov  20(%esp), %eax          # r
    push %eax                    # (%esp) = r; launched at 28(%esp)
    mov  $0x6C14, %edx           # HOST_RSP
    vmwrite %esp, %edx
    mov  28(%esp), %ecx
    mov  (%esp), %edi
    mov  4(%edi), %ebx
    mov  12(%edi), %edx
    mov  16(%edi), %esi
    mov  24(%edi), %ebp
    mov  0(%edi), %eax
    test %ecx, %ecx
    mov  8(%edi), %ecx
    mov  20(%edi), %edi
    jnz  1f
    vmlaunch
    jmp  2f
1:  vmresume
2:  add  $4, %esp                # the entry failed (the guest's registers are lost: r keeps them)
    pop  %edi
    pop  %esi
    pop  %ebx
    pop  %ebp
    mov  $1, %eax
    ret

vmx_exit_entry:                  # interrupts off (RFLAGS = 2), ESP as written above
    push %edi
    mov  4(%esp), %edi           # r
    mov  %eax, 0(%edi)
    mov  %ebx, 4(%edi)
    mov  %ecx, 8(%edi)
    mov  %edx, 12(%edi)
    mov  %esi, 16(%edi)
    mov  %ebp, 24(%edi)
    pop  %eax
    mov  %eax, 20(%edi)
    add  $4, %esp
    pop  %edi
    pop  %esi
    pop  %ebx
    pop  %ebp
    xor  %eax, %eax
    ret

.section .note.GNU-stack,"",@progbits
