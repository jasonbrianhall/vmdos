; GFXTEST.COM: mode 13h palette bars, PIT/timer check, then back to text.
        org 0x100
        mov ax, 0x13
        int 0x10
        push 0xA000
        pop es
        xor di, di
        mov dx, 200
.row:   xor cx, cx
.col:   mov al, cl
        mov [es:di], al
        inc di
        inc cx
        cmp cx, 320
        jne .col
        dec dx
        jnz .row
        mov ax, 0x1300 + 0              ; write a string with INT 10h
        mov ah, 0x0E
        mov si, msg
.p:     lodsb
        or al, al
        jz .k
        mov bl, 15
        int 0x10
        jmp .p
.k:     xor ax, ax
        int 0x16
        mov ax, 3
        int 0x10
        mov ah, 9
        mov dx, done
        int 0x21
        ret
msg:    db "Mode 13h OK - press a key", 0
done:   db "Back in text mode.", 13, 10, "$"
