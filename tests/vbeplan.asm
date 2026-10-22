; VBEPLAN.COM: VESA 102h, 800x600 in 16 colours (planar). Checks 4F03h
; reports 102h, sets a logical line of 1024 pixels (4F06h: 128 bytes a
; plane), fills each plane through the map mask so that the byte at
; line r, column c has colour (c + r / 4) & 15, then moves the display
; start to (9, 20) with 4F07h: one byte and one pixel of pan across.
        org 100h
        cpu 386
        mov ax, 4F02h
        mov bx, 102h
        int 10h
        cmp ax, 004Fh
        jne quit
        mov ax, 4F03h
        int 10h
        cmp bx, 102h
        jne quit
        mov ax, 4F06h
        xor bl, bl
        mov cx, 1024
        int 10h
        cmp ax, 004Fh
        jne quit
        cmp bx, 128
        jne quit
        mov ax, 0A000h
        mov es, ax
        xor cx, cx                      ; plane
.plane: mov dx, 3C4h                    ; map mask: this plane only
        mov al, 2
        out dx, al
        inc dx
        mov al, 1
        shl al, cl
        out dx, al
        xor di, di
.byte:  mov ax, di                      ; colour = (column + line / 4) & 15
        and ax, 127
        mov bx, di
        shr bx, 9                       ; line / 4 (128 bytes a line)
        add ax, bx
        shr ax, cl
        and al, 1
        neg al                          ; 0 or FFh
        stosb
        test di, di
        jnz .byte
        inc cx
        cmp cx, 4
        jb .plane
        mov ax, 4F07h
        xor bx, bx
        mov cx, 9
        mov dx, 20
        int 10h
        cmp ax, 004Fh
        jne quit
        sti
.hang:  hlt
        jmp .hang
quit:   mov ax, 0003h
        int 10h
        mov ax, 4C01h
        int 21h
