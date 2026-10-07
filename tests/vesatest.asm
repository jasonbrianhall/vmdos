; VESATEST.COM: VBE from real mode. Checks 4F00h ("VESA"), sets 640x480x8
; (101h) and fills it through the 64 KB window, switching banks with 4F05h:
; colour = (x + y) & 255 with a white frame. Key: 640x480x16 (111h), red /
; green / blue / white bands. Key: text mode.
        org 100h
        mov ax, 4F00h
        mov di, info
        int 10h
        cmp ax, 004Fh
        jne novesa
        cmp word [info], 'VE'
        jne novesa
        mov ax, 4F02h
        mov bx, 101h
        int 10h
        cmp ax, 004Fh
        jne novesa
        mov ax, 0A000h
        mov es, ax
        ; 8-bit: pixel (x,y) at y*640+x; bank = addr >> 16
        xor bp, bp                      ; y
.row8:  xor si, si                      ; x
.px8:   mov ax, 640
        mul bp                          ; DX:AX = y*640
        add ax, si
        adc dx, 0
        mov di, ax
        cmp dx, [bank]
        je .same
        mov [bank], dx
        push ax
        mov ax, 4F05h
        xor bx, bx
        int 10h
        pop ax
.same:  mov ax, si
        add ax, bp
        cmp si, 0
        je .white
        cmp si, 639
        je .white
        cmp bp, 0
        je .white
        cmp bp, 479
        jne .put
.white: mov al, 15
.put:   stosb
        inc si
        cmp si, 640
        jb .px8
        inc bp
        cmp bp, 480
        jb .row8
        xor ah, ah
        int 16h
        ; 16-bit mode: four bands of 120 lines
        mov ax, 4F02h
        mov bx, 111h
        int 10h
        mov word [bank], 0FFFFh
        xor bp, bp
.row16: mov ax, bp                      ; colour by band
        mov cx, 120
        xor dx, dx
        div cx
        mov bx, ax
        shl bx, 1
        mov cx, [colours + bx]
        mov [colour], cx
        xor si, si
.px16:  mov ax, 1280
        mul bp
        add ax, si
        adc dx, 0
        add ax, si
        adc dx, 0
        mov di, ax
        cmp dx, [bank]
        je .same16
        mov [bank], dx
        mov ax, 4F05h
        xor bx, bx
        int 10h
.same16:
        mov ax, [colour]
        stosw
        inc si
        cmp si, 640
        jb .px16
        inc bp
        cmp bp, 480
        jb .row16
        xor ah, ah
        int 16h
        mov ax, 0003h
        int 10h
        ret
novesa: mov dx, nomsg
        mov ah, 9
        int 21h
        ret
bank    dw 0
colour  dw 0
colours dw 0F800h, 07E0h, 001Fh, 0FFFFh
nomsg   db 'No VESA', 13, 10, '$'
info    times 512 db 0
