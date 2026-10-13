; VSTATE.COM: the video BIOS calls Windows uses around a DOS session.
; INT 10h AX=1A00h (display combination), AX=1C00h (state size), save the
; state, change mode and palette, restore it; AX=1B00h (state info); and
; the ROM signature at C000:0000 (VMHV's VM has one; vmdos in V86 mode
; doesn't). Prints what it finds; the screen must be text again at the end.
; Build: nasm -f bin tests/vstate.asm -o VSTATE.COM

        org 0x100
        mov ax, 0x1A00
        int 0x10
        mov dx, s_1a
        call say
        mov ax, bx                      ; expect 1A / 0008
        call hex
        mov ax, 0x1C00
        mov cx, 7
        int 0x10
        push bx
        push ax
        mov dx, s_1c
        call say
        pop ax
        call hex                        ; expect xx1C
        pop ax
        call hex                        ; blocks of 64 bytes
        mov ax, 0x1C01                  ; save everything
        mov cx, 7
        mov bx, buf
        int 0x10
        mov ax, 0x0012                  ; another mode and a palette change
        int 0x10
        mov ax, 0x1010
        xor bx, bx
        mov dh, 63
        int 0x10
        mov ax, 0x0003                  ; text again (BIOS data back via restore)
        int 0x10
        mov ax, 0x1C02
        mov cx, 7
        mov bx, buf
        int 0x10
        mov dx, s_rs
        call say
        call hex
        mov ax, 0x1B00
        xor bx, bx
        mov di, buf2
        int 0x10
        mov dx, s_1b
        call say
        call hex                        ; expect xx1B
        mov al, [buf2 + 4]              ; mode
        xor ah, ah
        call hex
        push ds
        mov ax, 0xC000
        mov ds, ax
        mov ax, [0]
        pop ds
        mov dx, s_rom
        call say
        call hex                        ; AA55 with a ROM
        mov dx, crlf
        call say
        ret

say:    push ax
        mov ah, 9
        int 0x21
        pop ax
        ret
hex:    push ax
        mov cx, 4
.h:     rol ax, 4
        push ax
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .o
        add al, 7
.o:     mov dl, al
        mov ah, 2
        int 0x21
        pop ax
        loop .h
        mov dl, ' '
        mov ah, 2
        int 0x21
        pop ax
        ret

s_1a:   db 13, 10, '1A00 AX BX: $'
s_1c:   db 13, 10, '1C00 AX BX: $'
s_rs:   db 13, 10, '1C02 AX: $'
s_1b:   db 13, 10, '1B00 AX mode: $'
s_rom:  db 13, 10, 'C000:0000: $'
crlf:   db 13, 10, '$'
buf2:   times 64 db 0
buf:
