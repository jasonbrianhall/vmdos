; EGATEST.COM: 16-colour planar modes through the VGA's write logic.
; Mode 12h: 16 colour bars with write mode 2, a white frame with set/reset,
; a bit-masked checkerboard, a latch copy (write mode 1) of the bars, a
; write mode 3 band, BIOS pixels and text. Key: mode 0Dh, same bars, text.
; Key: back to text mode.
        org 100h
        mov ax, 0012h
        int 10h
        mov ax, 0A000h
        mov es, ax
        mov dx, 3CEh
        mov ax, 0205h                   ; write mode 2
        out dx, ax
        ; bars: rows 0-99, 16 bars of 5 bytes (40 pixels)
        xor bx, bx
.row:   mov ax, 80
        mul bx
        mov di, ax
        xor al, al
.bar:   mov cx, 5
        rep stosb
        inc al
        cmp al, 16
        jb .bar
        inc bx
        cmp bx, 100
        jb .row
        ; write mode 0 with set/reset = 15 on all planes: white line at row 110
        mov dx, 3CEh                    ; (mul clobbers DX)
        mov ax, 0005h
        out dx, ax
        mov ax, 0F00h                   ; set/reset value 15
        out dx, ax
        mov ax, 0F01h                   ; enable set/reset all planes
        out dx, ax
        mov di, 110 * 80
        mov cx, 80
        rep stosb                       ; data ignored
        ; bit mask 0AAh: checkerboard-ish rows 120-139 in colour 15
        mov ax, 0AA08h
        out dx, ax
        push dx
        mov bx, 120
.chk:   mov ax, 80
        mul bx
        mov di, ax
        mov cx, 80
.cb:    mov al, [es:di]                 ; load latches
        stosb
        loop .cb
        inc bx
        cmp bx, 140
        jb .chk
        pop dx
        mov ax, 0FF08h
        out dx, ax
        mov ax, 0001h                   ; set/reset off
        out dx, ax
        ; latch copy: rows 0-39 to rows 150-189 (write mode 1)
        mov ax, 0105h
        out dx, ax
        push ds
        push es
        pop ds
        xor si, si
        mov di, 150 * 80
        mov cx, 40 * 80
        rep movsb
        pop ds
        ; write mode 3: set/reset colour 12, rows 200-209, pattern 0F0h
        mov ax, 0305h
        out dx, ax
        mov ax, 0C00h
        out dx, ax
        mov di, 200 * 80
        mov cx, 10 * 80
        mov al, 0F0h
        rep stosb
        mov ax, 0005h
        out dx, ax
        mov ax, 0000h
        out dx, ax
        ; BIOS pixels: diagonal in colour 14
        xor cx, cx
.dg:    mov dx, cx
        shr dx, 1
        add dx, 240
        mov ax, 0C0Eh
        xor bh, bh
        int 10h
        inc cx
        cmp cx, 400
        jb .dg
        ; BIOS text at row 20
        mov ah, 02h
        xor bh, bh
        mov dx, 1405h
        int 10h
        mov si, msg
        call puts
        call key
        ; mode 0Dh
        mov ax, 000Dh
        int 10h
        mov dx, 3CEh
        mov ax, 0205h
        out dx, ax
        xor bx, bx
.row2:  mov ax, 40
        mul bx
        mov di, ax
        xor al, al
.bar2:  mov cx, 2
        rep stosb
        inc al
        cmp al, 16
        jb .bar2
        mov cx, 8                       ; pad to 40 bytes
        rep stosb
        inc bx
        cmp bx, 100
        jb .row2
        mov dx, 3CEh
        mov ax, 0005h
        out dx, ax
        mov ah, 02h
        xor bh, bh
        mov dx, 1003h
        int 10h
        mov si, msg2
        call puts
        call key
        mov ax, 0003h
        int 10h
        ret
puts:   lodsb
        or al, al
        jz .e
        mov ah, 0Eh
        mov bl, 10
        int 10h
        jmp puts
.e:     ret
key:    xor ah, ah
        int 16h
        ret
msg     db 'EGA/VGA mode 12h', 0
msg2    db 'mode 0Dh', 0
