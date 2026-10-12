; REG640.COM: 640x480 16-colour screens set up through the VGA registers,
; as some games (Genesia) do, not with INT 10h AX=0012h. Each shows a white
; frame round all 640x480, colour bars along the top and a red block in the
; bottom-right corner: if the corner block is missing, the screen was taken
; for a smaller one and blown up. Key after each.
;  1. BIOS mode 0Dh (320x200), then the clock, sequencer and CRTC set for 640x480.
;  2. From text mode, every register programmed by hand, no mode set at all.
        org 100h
        ; ---- 1: mode 0Dh, reprogrammed
        mov ax, 000Dh
        int 10h
        cli
        mov dx, 3C2h
        mov al, 0E3h                ; 25 MHz clock, 480-line sync
        out dx, al
        mov dx, 3C4h
        mov ax, 0100h
        out dx, ax
        mov ax, 0101h               ; dot clock not halved
        out dx, ax
        mov ax, 0300h
        out dx, ax
        call set_crtc
        sti
        call draw
        xor ax, ax
        int 16h
        mov ax, 3
        int 10h
        ; ---- 2: from text mode, registers only
        cli
        mov dx, 3C2h
        mov al, 0E3h
        out dx, al
        mov dx, 3C4h
        mov si, seqt
        xor bl, bl
        mov cx, 5
.s:     lodsb
        mov ah, al
        mov al, bl
        out dx, ax
        inc bl
        loop .s
        call set_crtc
        mov dx, 3CEh
        mov si, gct
        mov cx, 9
        xor bl, bl
.g:     lodsb
        mov ah, al
        mov al, bl
        out dx, ax
        inc bl
        loop .g
        mov dx, 3DAh
        in al, dx                   ; attribute flip-flop to index
        mov dx, 3C0h
        mov si, atct
        xor bl, bl
        mov cx, 21
.a:     mov al, bl
        out dx, al
        lodsb
        out dx, al
        inc bl
        loop .a
        mov al, 20h                 ; palette on
        out dx, al
        sti
        call draw
        xor ax, ax
        int 16h
        mov ax, 3
        int 10h
        ret

set_crtc:
        mov dx, 3D4h
        mov ax, 0011h               ; unlock CR0-7
        out dx, ax
        mov si, crtc
        mov cx, 25
        xor bl, bl
.c:     lodsb
        mov ah, al
        mov al, bl
        out dx, ax
        inc bl
        loop .c
        ret

seqt:   db 03h,01h,0Fh,00h,06h
crtc:   db 5Fh,4Fh,50h,82h,54h,80h,0Bh,3Eh,00h,40h,0,0,0,0,0,0
        db 0EAh,8Ch,0DFh,28h,00h,0E7h,04h,0E3h,0FFh
gct:    db 0,0,0,0,0,0,05h,0Fh,0FFh
atct:   db 0,1,2,3,4,5,14h,7,38h,39h,3Ah,3Bh,3Ch,3Dh,3Eh,3Fh
        db 01h,00h,0Fh,00h,00h

; draw: white frame round 640x480, red block in the bottom-right corner,
; colour bars across the top. Planar, write mode 2, 80 bytes a row.
draw:
        mov ax, 0A000h
        mov es, ax
        mov dx, 3CEh
        mov ax, 0205h               ; write mode 2
        out dx, ax
        mov ax, 0FF08h              ; bit mask all
        out dx, ax
        mov dx, 3C4h
        mov ax, 0F02h               ; all planes
        out dx, ax
        ; clear 640x480
        xor di, di
        xor al, al
        mov cx, 38400
        rep stosb
        ; top row 0 and bottom row 479: white
        xor di, di
        mov al, 15
        mov cx, 80
        rep stosb
        mov di, 479*80
        mov cx, 80
        rep stosb
        ; colour bars rows 8-63
        mov bx, 8
.bars:  mov ax, 80
        mul bx
        mov di, ax
        xor al, al
.b1:    mov cx, 5
        rep stosb
        inc al
        cmp al, 16
        jb .b1
        inc bx
        cmp bx, 64
        jb .bars
        ; left and right edges: bit mask 80h at byte 0, 01h at byte 79
        mov bx, 0
.edge:  mov ax, 80
        mul bx
        mov di, ax
        mov dx, 3CEh
        mov ax, 8008h
        out dx, ax
        mov al, [es:di]             ; load latches
        mov al, 15
        mov [es:di], al
        mov ax, 0108h
        out dx, ax
        mov al, [es:di+79]
        mov al, 15
        mov [es:di+79], al
        inc bx
        cmp bx, 480
        jb .edge
        mov dx, 3CEh
        mov ax, 0FF08h
        out dx, ax
        ; red block: rows 440-470, bytes 74-78 (x 592-631)
        mov bx, 440
.blk:   mov ax, 80
        mul bx
        add ax, 74
        mov di, ax
        mov al, 4
        mov cx, 5
        rep stosb
        inc bx
        cmp bx, 471
        jb .blk
        ret
