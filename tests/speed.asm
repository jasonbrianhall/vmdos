; SPEED.COM: loop iterations during one BIOS clock tick (55 ms), in hex
; (thousands). For checking speed= and Ctrl+Alt+F11/F12.
        org 100h
        push 40h
        pop es
        mov ax, [es:6Ch]
.sync:  cmp ax, [es:6Ch]                ; wait for a tick edge
        je .sync
        mov ax, [es:6Ch]
        xor si, si                      ; count / 1000 in SI
        xor cx, cx
.loop:  inc cx
        cmp cx, 1000
        jb .same
        xor cx, cx
        inc si
.same:  cmp ax, [es:6Ch]
        je .loop
        mov ax, si
        mov cx, 4
.h:     rol ax, 4
        push ax
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .d
        add al, 7
.d:     mov dl, al
        mov ah, 2
        int 21h
        pop ax
        loop .h
        mov dx, msg
        mov ah, 9
        int 21h
        ret
msg     db ' thousand loops per tick', 13, 10, '$'
