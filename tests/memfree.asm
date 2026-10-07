; MEMFREE.COM: largest free conventional block (what a program can get), in KB.
        org 0x100
        mov bx, 0x1000              ; shrink ourselves to 64 KB first
        mov ah, 0x4A
        int 0x21
        mov ah, 0x48
        mov bx, 0xFFFF
        int 0x21                    ; fails, BX = largest block in paragraphs
        mov ax, bx
        mov cl, 6
        shr ax, cl                  ; KB
        add ax, 64                  ; plus what this program holds
        call pdec
        mov dx, msg
        mov ah, 9
        int 0x21
        ret
pdec:   xor cx, cx
        mov bx, 10
.d:     xor dx, dx
        div bx
        push dx
        inc cx
        or ax, ax
        jnz .d
.p:     pop dx
        add dl, '0'
        mov ah, 2
        int 0x21
        loop .p
        ret
msg:    db " KB free for a program", 13, 10, "$"
