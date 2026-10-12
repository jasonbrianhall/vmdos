; DAC8.COM: the VESA 8-bit DAC (4F08h), as SVGA games use it. VESA mode
; 101h (640x480x256), DAC switched to 8 bits, then two ramps with 8-bit
; palette values: rows 0-49 grey, set through ports 3C8h/3C9h; rows 50-99
; red, set through 4F09h. Both must run smoothly from black to full
; brightness; with a 6-bit-only DAC they repeat four times instead. Rows
; 100-101: the DAC width the BIOS reported (bar: 8 bits). Q: text mode.
        org 100h
        mov ax, 4F02h
        mov bx, 0101h
        int 10h
        mov ax, 4F08h
        mov bx, 0800h               ; BL=0 set, BH=8 bits
        int 10h
        mov [width], bh
        ; grey: indices 0-127 = 2*i, through the ports (8-bit values)
        mov dx, 3C8h
        xor al, al
        out dx, al
        inc dx
        xor cx, cx
.g:     mov al, cl
        shl al, 1
        out dx, al
        out dx, al
        out dx, al
        inc cx
        cmp cx, 128
        jb .g
        ; red: indices 128-255 = 2*i, through 4F09h (B, G, R, 0)
        mov di, pal
        xor cx, cx
.r:     xor al, al
        stosb                       ; blue
        stosb                       ; green
        mov al, cl
        shl al, 1
        stosb                       ; red
        xor al, al
        stosb
        inc cx
        cmp cx, 128
        jb .r
        push ds
        pop es
        mov ax, 4F09h
        xor bl, bl
        mov cx, 128
        mov dx, 128
        mov di, pal
        int 10h
        ; draw: window A, bank 0 (rows 0-101)
        mov ax, 0A000h
        mov es, ax
        xor di, di
        xor bx, bx                  ; row
.row:   xor si, si                  ; x
.px:    mov ax, si
        xor dx, dx
        mov cx, 5
        div cx                      ; x/5: 0..127
        cmp bx, 50
        jb .st
        add al, 128
.st:    stosb
        inc si
        cmp si, 640
        jb .px
        inc bx
        cmp bx, 100
        jb .row
        ; width bar: rows 100-101, 8 pixels per reported bit, colour 127 (white)
        mov cl, [width]
        xor ch, ch
        shl cx, 3
        mov al, 127
        rep stosb
.key:   xor ax, ax                  ; Q: back to text (other keys: ignored,
        int 16h                     ; so the log, Ctrl+Shift+F10, can be
        or al, 20h                  ; opened and closed over it)
        cmp al, 'q'
        jne .key
        mov ax, 3
        int 10h
        ret
width:  db 0
pal:    times 512 db 0
