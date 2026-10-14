; M13TEXT.COM: BIOS text over an unchained mode 13h screen (Windows 9x's
; IO.SYS prints over its boot logo): mode 13h, chain-4 off, map mask on two
; planes (every window access trapped), then 30 lines through INT 10h AH=0Eh
; so the screen scrolls. Back in text mode it prints "M13TEXT DONE".
        org 100h
        mov ax, 0013h
        int 10h
        mov dx, 3C4h
        mov ax, 0604h                   ; chain-4 off
        out dx, ax
        mov ax, 0302h                   ; map mask: planes 0 and 1
        out dx, ax
        mov cx, 30
.l:     mov si, line
.c:     lodsb
        test al, al
        jz .n
        mov ah, 0Eh
        mov bx, 000Fh
        int 10h
        jmp .c
.n:     loop .l
        xor ah, ah                      ; wait for a key
        int 16h
        mov ax, 0003h
        int 10h
        mov dx, done
        mov ah, 9
        int 21h
        ret
line:   db 'Starting Windows 98...', 13, 10, 0
done:   db 'M13TEXT DONE', 13, 10, '$'
