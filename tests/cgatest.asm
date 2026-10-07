; CGATEST.COM: CGA modes 4 and 6 through INT 10h and direct B800h writes.
; Mode 4: colour bars written to video memory, a diagonal via AH=0Ch, text
; via AH=0Eh, palette/background via AH=0Bh. Keys step through; Esc quits.
        org 100h
        mov ax, 0004h
        int 10h
        mov ax, 0B800h
        mov es, ax
        ; bars: rows 0-99, four 80-pixel bands of colours 0-3 (20 bytes each)
        xor bx, bx                      ; row
bars:   mov ax, bx
        shr ax, 1
        mov dx, 80
        mul dx
        mov di, ax
        test bl, 1
        jz .even
        add di, 2000h
.even:  mov al, 00h
        mov cx, 20
        rep stosb
        mov al, 55h
        mov cx, 20
        rep stosb
        mov al, 0AAh
        mov cx, 20
        rep stosb
        mov al, 0FFh
        mov cx, 20
        rep stosb
        inc bx
        cmp bx, 100
        jb bars
        ; diagonal via BIOS
        xor cx, cx
diag:   mov dx, cx
        shr dx, 1
        add dx, 100
        mov ax, 0C03h
        xor bh, bh
        int 10h
        inc cx
        cmp cx, 200
        jb diag
        ; text
        mov ah, 02h
        xor bh, bh
        mov dx, 1505h                   ; row 21, col 5
        int 10h
        mov si, msg
txt:    lodsb
        or al, al
        jz txtend
        mov ah, 0Eh
        mov bl, 2
        int 10h
        jmp txt
txtend: call key
        mov ax, 0B00h                   ; palette 0, blue background
        mov bx, 0101h
        dec bh
        int 10h
        mov ax, 0B00h
        mov bx, 0100h                   ; BH=1 BL=0: palette 0
        int 10h
        call key
        mov ax, 0006h
        int 10h
        mov ax, 0B800h
        mov es, ax
        xor di, di
        mov cx, 1000h
        mov ax, 0F0F0h
        rep stosw                       ; stripes in the even rows
        mov ah, 0Eh
        mov al, 'X'
        mov bl, 1
        int 10h
        call key
        mov ax, 0003h
        int 10h
        ret
key:    xor ah, ah
        int 16h
        cmp al, 27
        jne .r
        mov ax, 0003h
        int 10h
        mov ax, 4C00h
        int 21h
.r:     ret
msg     db 'CGA mode 4', 0
