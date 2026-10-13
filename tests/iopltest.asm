; IOPLTEST.COM: the check DOS/32A uses to tell real mode from V86 mode.
; It clears IOPL with POPF and reads it back: in real mode the write
; sticks (IOPL 0, so DOS/32A starts its own raw/XMS kernel with LGDT,
; LIDT and MOV CR0), in V86 mode it can't (DOS/32A asks for VCPI, then
; DPMI). Under vmdos it should print "V86" and IOPL 3.
; Build: nasm -f bin tests/iopltest.asm -o IOPLTEST.COM
        org 0x100
        pushf
        pop ax
        and ah, 0xCF                ; IOPL := 0
        push ax
        popf
        pushf
        pop ax
        mov bl, ah
        shr bl, 4
        and bl, 3                   ; BL = IOPL as read back
        mov dx, v86
        test bl, bl
        jnz .p
        mov dx, real
.p:     mov ah, 9
        int 0x21
        mov dl, bl
        add dl, '0'
        mov ah, 2
        int 0x21
        mov dx, crlf
        mov ah, 9
        int 0x21
        ret
real:   db 'real mode (IOPL settable): DOS/32A would use its own kernel, IOPL $'
v86:    db 'V86 (IOPL fixed): DOS/32A will look for VCPI/DPMI, IOPL $'
crlf:   db 13, 10, '$'
