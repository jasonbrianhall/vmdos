; MODEXTST.COM: unchained 256-colour plane writes. Writes pixel x of row 0
; (value x & 0FFh) one column at a time, setting the map mask for each
; column as DOOM's patch drawing does, then reads every plane back through
; the read map select and counts mismatches. Prints "OK" or "BAD nnnn".
        org 100h
        mov ax, 0013h
        int 10h
        mov dx, 3C4h
        mov ax, 0604h                   ; chain-4 off
        out dx, ax
        mov ax, 0A000h
        mov es, ax
        xor bx, bx                      ; x
.w:     mov cl, bl
        and cl, 3
        mov ah, 1
        shl ah, cl
        mov al, 2
        mov dx, 3C4h
        out dx, ax                      ; map mask = 1 << (x & 3)
        mov di, bx
        shr di, 2
        mov [es:di], bl
        inc bx
        cmp bx, 320
        jb .w
        xor bp, bp                      ; mismatches
        xor bx, bx
.r:     mov cl, bl
        and cl, 3
        mov ah, cl
        mov al, 4
        mov dx, 3CEh
        out dx, ax                      ; read map = x & 3
%ifdef SAMEMASK
        mov ah, 1
        shl ah, cl
        mov al, 2
        mov dx, 3C4h
        out dx, ax                      ; map mask = the same plane
%endif
        mov di, bx
        shr di, 2
        mov al, [es:di]
        cmp al, bl
        je .ok
        inc bp
.ok:    inc bx
        cmp bx, 320
        jb .r
        mov ax, 0003h
        int 10h
        mov dx, ok
        or bp, bp
        jz .p
        mov dx, bad
.p:     mov ah, 9
        int 21h
        mov ax, bp
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
        ret
ok      db 'OK $'
bad     db 'BAD $'
