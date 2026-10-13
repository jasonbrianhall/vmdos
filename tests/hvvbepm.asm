; HVVBEPM.COM: for VMHV (DOS in the VM without EMM386). Sets VESA mode
; 101h (640x480, 256 colours, through the 64 KB window), copies the VESA
; protected-mode interface (4F0Ah) into its own code as games do (Warcraft
; II), switches to 32-bit protected mode and, for banks 0-4, calls the
; copied set-window routine near and fills the window with colour bank+1.
; Expect five horizontal stripes (blue, green, cyan, red, magenta); only
; blue means the bank switch didn't happen. A key ends it.
; Build: nasm -f bin tests/hvvbepm.asm -o HVVBEPM.COM

        org 0x100
        bits 16
        mov ax, 0x4F02
        mov bx, 0x0101
        int 0x10
        mov ax, 0x4F0A                  ; the protected-mode interface
        xor bl, bl
        int 0x10
        push ds
        push es
        pop ds                          ; DS:DI = table, CX = length
        mov si, di
        pop es                          ; ES = ours
        mov di, pmi
        cld
        rep movsb
        push es
        pop ds
        mov ax, cs
        movzx eax, ax
        shl eax, 4
        mov [rmbase], eax
        movzx ebx, word [pmi]           ; set window: offset 0 in the table
        add ebx, eax
        add ebx, pmi
        mov [setwin], ebx               ; its linear address
        lea ebx, [eax + gdt]
        mov [gdtr + 2], ebx
        mov [gdt + 0x18 + 2], ax
        mov [gdt + 0x20 + 2], ax
        shr eax, 16
        mov [gdt + 0x18 + 4], al
        mov [gdt + 0x20 + 4], al
        mov ax, cs
        mov [rmjmp], ax
        in al, 0x92
        or al, 2
        out 0x92, al
        cli
        lgdt [gdtr]
        mov eax, cr0
        or al, 1
        mov cr0, eax
        jmp 0x18:pm16
pm16:   mov ax, 0x10
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov esp, 0x2F0000
        mov esi, [cs:rmbase]
        add esi, blob_src
        mov edi, 0x200000
        mov ecx, blob_len
        a32 rep movsb
        mov eax, [cs:setwin]
        mov [dword 0x200000 + (setwin32 - blob)], eax
        o32 jmp dword 0x08:0x200000

back16: mov ax, 0x20
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov eax, cr0
        and eax, 0x7FFFFFFE
        mov cr0, eax
        db 0xEA
        dw real
rmjmp:  dw 0
real:   mov ax, cs
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov sp, 0xFFFE
        sti
        xor ah, ah
        int 0x16
        mov ax, 0x0003
        int 0x10
        mov ax, 0x4C00
        int 0x21

align 8
gdt:    dq 0
        dq 0x00CF9A000000FFFF           ; 08: flat 32-bit code
        dq 0x00CF92000000FFFF           ; 10: flat 32-bit data
        dq 0x00009A000000FFFF           ; 18: 16-bit code (base patched)
        dq 0x000092000000FFFF           ; 20: 16-bit data (base patched)
gdtr:   dw 0x27
        dd 0
rmbase: dd 0
setwin: dd 0
pmi:    times 64 db 0
text_end:
blob_src equ text_end
        section blob align=1 follows=.text vstart=0x200000
        bits 32
blob:   xor esi, esi                    ; bank
.bank:  xor ebx, ebx                    ; BL = 0: window A
        mov edx, esi
        call [setwin32]
        mov edi, 0xA0000
        mov ecx, 0x10000
        lea eax, [esi + 1]
        rep stosb
        inc esi
        cmp esi, 5
        jb .bank
        jmp 0x18:back16
setwin32: dd 0
blob_len equ $ - blob
