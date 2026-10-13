; HVPLANAR.COM: for VMHV (DOS booted in the VM without EMM386). Sets mode
; 12h (640x480, 16 colours, planar: every VGA access is emulated), switches
; to 32-bit protected mode with paging, and from code above 1 MB writes the
; VGA window through a different linear address than its physical one:
; linear 4-8 MB maps physical 0-4 MB (4 MB pages), the code runs at linear
; 6 MB (physical 2 MB), the window is at linear 4 MB + A0000h. That is what
; DOS/4GW games (DOOM) do under EMM386. Expect a white top half and a blue
; bottom half; a key ends it.
; Build: nasm -f bin tests/hvplanar.asm -o HVPLANAR.COM

        org 0x100
        bits 16
        mov ax, 0x0012
        int 0x10
        mov dx, 0x3C4                   ; map mask: all planes
        mov ax, 0x0F02
        out dx, ax
        mov ax, cs
        movzx eax, ax
        shl eax, 4
        mov [rmbase], eax
        lea ebx, [eax + gdt]
        mov [gdtr + 2], ebx
        mov word [gdt + 0x18 + 2], ax   ; 16-bit code/data at our segment
        shr eax, 16
        mov [gdt + 0x18 + 4], al
        mov [gdt + 0x20 + 4], al
        mov eax, [rmbase]
        mov [gdt + 0x20 + 2], ax
        mov ax, cs
        mov [rmjmp], ax
        in al, 0x92                     ; A20 on
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
        mov esi, [cs:rmbase]            ; copy the 32-bit part to physical 2 MB
        add esi, blob_src
        mov edi, 0x200000
        mov ecx, blob_len
        a32 rep movsb
        mov edi, 0x300000               ; page directory at 3 MB
        xor eax, eax
        mov ecx, 1024
        a32 rep stosd
        mov dword [dword 0x300000], 0x00000083      ; linear 0-4 MB -> 0-4 MB
        mov dword [dword 0x300004], 0x00000083      ; linear 4-8 MB -> 0-4 MB
        mov eax, cr4
        or eax, 0x10                    ; PSE
        mov cr4, eax
        mov eax, 0x300000
        mov cr3, eax
        mov eax, cr0
        or eax, 0x80000000
        mov cr0, eax
        o32 jmp dword 0x08:0x600000     ; linear 6 MB = physical 2 MB

back16: mov ax, 0x20
        mov ds, ax
        mov es, ax
        mov ss, ax
        mov eax, cr0
        and eax, 0x7FFFFFFE             ; paging and protection off
        mov cr0, eax
        db 0xEA                         ; jmp far real-mode segment:real
        dw real
rmjmp:  dw 0                            ; patched
real:   mov ax, cs
        mov ds, ax
        mov es, ax
        mov ss, ax
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
text_end:
blob_src equ text_end                   ; the 32-bit part follows, at this offset in our segment
        section blob align=1 follows=.text vstart=0x600000
        bits 32
blob:   mov edi, 0x4A0000               ; the VGA window through linear 4 MB+
        mov ecx, 640 * 240 / 8
        mov al, 0xFF                    ; white (all planes set)
        rep stosb
        mov dx, 0x3C4
        mov ax, 0x0102                  ; plane 0 only: blue
        out dx, ax
        mov al, 0xFF
        mov ecx, 640 * 240 / 8
        rep stosb
        mov dx, 0x3C4
        mov ax, 0x0F02
        out dx, ax
        jmp 0x18:back16
blob_len equ $ - blob
