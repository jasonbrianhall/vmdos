; FONTTEST.COM: text in EGA 640x350 (mode 10h) the way programs draw it
; themselves (SimCity): glyphs from the INT 43h table, 40:85h bytes each,
; written straight into video memory. Line 1: INT 43h; line 2: the 8x14 font
; from INT 10h AX=1130h BH=2; line 3: the BIOS teletype. All three should
; read "The quick brown fox 0123". A key returns to text mode.
        org 100h
        mov ax, 0010h
        int 10h
        xor ax, ax                      ; line 1: INT 43h, height from 40:85h
        mov es, ax
        mov si, [es:43h * 4]
        mov ax, [es:43h * 4 + 2]
        mov [fseg], ax
        mov [foff], si
        mov ax, 40h
        mov es, ax
        mov ax, [es:85h]
        mov [fh], ax
        mov word [row], 2
        call line
        mov ax, 1130h                   ; line 2: the 8x14 font from the BIOS
        mov bh, 2
        int 10h
        mov [fseg], es
        mov [foff], bp
        mov [fh], cx
        mov word [row], 5
        call line
        mov ah, 02h                     ; line 3: BIOS teletype at row 8
        xor bh, bh
        mov dx, 0800h
        int 10h
        mov si, text
.t:     lodsb
        or al, al
        jz .k
        mov ah, 0Eh
        mov bx, 000Fh
        int 10h
        jmp .t
.k:     xor ah, ah
        int 16h
        mov ax, 0003h
        int 10h
        mov ax, 4C00h
        int 21h

line:   mov si, text                    ; draw text at character row [row]
        xor bx, bx                      ; column
.c:     lodsb
        or al, al
        jz .e
        push si
        push bx
        xor ah, ah
        mul word [fh]
        add ax, [foff]
        mov si, ax                      ; glyph
        mov ax, [row]
        mul word [fh]                   ; first pixel row
        mov di, ax
        mov ax, 80
        mul di
        add ax, bx
        mov di, ax                      ; byte in video memory
        mov cx, [fh]
        push ds
        mov ds, [fseg]
        mov ax, 0A000h
        mov es, ax
.r:     movsb
        add di, 79
        loop .r
        pop ds
        pop bx
        pop si
        inc bx
        jmp .c
.e:     ret

text    db 'The quick brown fox 0123', 0
fseg    dw 0
foff    dw 0
fh      dw 0
row     dw 0
