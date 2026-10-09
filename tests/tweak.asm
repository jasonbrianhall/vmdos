; TWEAK.COM: tweaked 256-colour VGA modes, to check how they are drawn.
; 256x256 chained (as Nesticle), a key, then Mode X 320x240, a key.
; Each shows 16x16-pixel colour blocks with a white border.
        org 100h
        mov ax, 13h
        int 10h
        mov dx, 3D4h                    ; unlock CRTC 0-7
        mov al, 11h
        out dx, al
        inc dx
        in al, dx
        and al, 7Fh
        out dx, al
        dec dx
        mov si, t256
        call crtc
        mov ax, 0A000h
        mov es, ax
        xor di, di                      ; 256 x 256, chained: byte (y * 256 + x)
        xor bx, bx                      ; bl = x, bh = y
.p:     mov al, bl
        shr al, 4
        mov ah, bh
        and ah, 0F0h
        or al, ah
        cmp bl, 0
        je .w
        cmp bl, 255
        je .w
        cmp bh, 0
        je .w
        cmp bh, 255
        je .w
        jmp .s
.w:     mov al, 15
.s:     stosb
        inc bx
        jnz .p
        xor ah, ah
        int 16h

        mov ax, 13h                     ; Mode X 320x240: unchained
        int 10h
        mov dx, 3C4h
        mov ax, 0604h                   ; chain-4 off
        out dx, ax
        mov dx, 3C2h
        mov al, 0E3h
        out dx, al
        mov dx, 3D4h
        mov al, 11h
        out dx, al
        inc dx
        in al, dx
        and al, 7Fh
        out dx, al
        dec dx
        mov si, tx
        call crtc
        xor cx, cx                      ; y
.y:     xor bx, bx                      ; x
.x:     mov ax, bx                      ; colour: (x/16) | (y/16)*16
        shr ax, 4
        mov dx, cx
        shr dx, 4
        shl dx, 4
        or al, dl
        cmp bx, 0
        je .xw
        cmp bx, 319
        je .xw
        cmp cx, 0
        je .xw
        cmp cx, 239
        je .xw
        jmp .xs
.xw:    mov al, 15
.xs:    push ax
        mov ah, 1                       ; map mask: plane x & 3
        push cx
        mov cl, bl
        and cl, 3
        shl ah, cl
        pop cx
        mov al, 2
        mov dx, 3C4h
        out dx, ax
        pop ax
        push ax
        mov ax, cx                      ; offset y * 80 + x / 4
        mov dx, 80
        mul dx
        mov di, bx
        shr di, 2
        add di, ax
        pop ax
        mov [es:di], al
        inc bx
        cmp bx, 320
        jb .x
        inc cx
        cmp cx, 240
        jb .y
        xor ah, ah
        int 16h
        mov ax, 3
        int 10h
        ret

crtc:   lodsw                           ; pairs (index, value), 0FFFFh ends
        cmp ax, 0FFFFh
        je .e
        out dx, ax
        jmp crtc
.e:     ret

t256    dw 5F00h, 3F01h, 4002h, 8203h, 4A04h, 9A05h, 2306h, 0B207h, 0008h, 6109h
        dw 0A10h, 0AC11h, 0FF12h, 2013h, 4014h, 0715h, 1A16h, 0A317h, 0FFFFh
tx      dw 0D06h, 3E07h, 4109h, 0EA10h, 0AC11h, 0DF12h, 0014h, 0E715h, 0616h, 0E317h, 0FFFFh
