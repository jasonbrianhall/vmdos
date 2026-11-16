; VMRES.COM: set or show the text screen's size (columns x rows), for a
; bigger DOS prompt (SSH, EDIT, VIM and the rest take it from the BIOS).
;   VMRES            show the current size and the sizes there are
;   VMRES 80x50      80 columns, 50 rows (the screen is cleared)
;   VMRES 132        132 columns, the rows as they are (25 if none fit)
;   VMRES 50         80 columns, 50 rows
; Sizes: 80x25 80x28 80x43 80x50 80x60 132x25 132x43 132x50 132x60.
; Plain INT 10h, as on a real VGA: 80x28/43/50 are mode 3 with the BIOS's
; 8x14 or 8x8 font (AX=1111h/1112h, 350 lines for 43: AX=1201h BL=30h),
; 80x60 and 132 columns the VESA text modes 108h-10Ch (AX=4F02h).
        org 100h
        mov si, 81h
        call skipsp
        cmp al, 13
        je show
        cmp al, '/'
        je usage
        cmp al, '?'
        je usage
        call num                        ; columns (or rows, alone)
        jc usage
        mov [cols], bx
        xor bx, bx
        cmp al, 'x'
        je .rows
        cmp al, 'X'
        je .rows
        cmp al, '*'
        jne .one
.rows:  lodsb
        call num
        jc usage
.one:   call skipsp0
        cmp al, 13
        jne usage
        mov [rows], bx
        mov ax, [cols]
        cmp ax, 80                      ; one number: 80 or 132 is columns,
        je .cols                        ; anything else is rows on 80
        cmp ax, 132
        je .cols
        or bx, bx
        jnz .find
        mov [rows], ax
        mov word [cols], 80
        jmp .find
.cols:  or bx, bx
        jnz .find
        call currows                    ; the rows as they are
        mov [rows], ax
        mov di, sizes                   ; does that many fit at these columns?
.try:   mov ax, [di]
        or ax, ax
        jz .r25
        cmp ax, [cols]
        jne .next
        mov ax, [di + 2]
        cmp ax, [rows]
        je .find
.next:  add di, 8
        jmp .try
.r25:   mov word [rows], 25
.find:  mov di, sizes
.look:  mov ax, [di]
        or ax, ax
        jz nosize
        cmp ax, [cols]
        jne .skip
        mov ax, [di + 2]
        cmp ax, [rows]
        je set
.skip:  add di, 8
        jmp .look

; DI: the entry. +4: VESA mode or 0; +6: font (AL for AH=11h: 14h, 11h, 12h), scan-line choice (AL for 12h BL=30h).
set:    mov bx, [di + 4]
        or bx, bx
        jz .vga
        mov ax, 4F02h
        int 10h
        cmp ax, 004Fh
        jne novesa
        jmp done
.vga:   mov al, [di + 7]                ; 0: 200, 1: 350, 2: 400 scan lines
        mov ah, 12h
        mov bl, 30h
        int 10h
        mov ax, 0003h
        int 10h
        mov al, [di + 6]
        cmp al, 14h                     ; the 8x16 font: mode 3 as it is
        je done
        mov ah, 11h
        xor bl, bl
        int 10h
done:   call cursz                      ; did it take?
        mov ax, [rows]
        cmp ax, [cur_r]
        jne notset
        mov ax, [cols]
        cmp ax, [cur_c]
        jne notset
        mov dx, msg_now
        mov ah, 9
        int 21h
        call psize
        mov dx, crlf
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

show:   call cursz
        mov dx, msg_now
        mov ah, 9
        int 21h
        call psize
        mov dx, msg_list
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

usage:  mov dx, msg_use
        mov ah, 9
        int 21h
        mov ax, 4C01h
        int 21h

nosize: mov dx, msg_nosize
        jmp fail
novesa: mov dx, msg_novesa
        jmp fail
notset: mov dx, msg_notset
        mov ah, 9
        int 21h
        call psize
        mov dx, crlf
fail:   mov ah, 9
        int 21h
        mov dx, msg_list
        mov ah, 9
        int 21h
        mov ax, 4C02h
        int 21h

; The screen's size now, from the BIOS data: [cur_c] x [cur_r].
cursz:  push es
        xor ax, ax
        mov es, ax
        mov ax, [es:44Ah]
        mov [cur_c], ax
        pop es
        call currows
        mov [cur_r], ax
        ret
; AX = rows (40:84h + 1; 25 if the BIOS doesn't keep it).
currows:
        push es
        xor ax, ax
        mov es, ax
        mov al, [es:484h]
        pop es
        or al, al
        jnz .ok
        mov al, 24
.ok:    inc ax
        ret

psize:  mov ax, [cur_c]
        call pnum
        mov dl, 'x'
        mov ah, 2
        int 21h
        mov ax, [cur_r]
        jmp pnum

; AX in decimal.
pnum:   mov bx, 10
        xor cx, cx
.div:   xor dx, dx
        div bx
        push dx
        inc cx
        or ax, ax
        jnz .div
.out:   pop dx
        add dl, '0'
        mov ah, 2
        int 21h
        loop .out
        ret

; A decimal number at SI-1 (AL its first character): BX, AL the character after it. CF: none.
num:    xor bx, bx
        xor cx, cx
.dig:   cmp al, '0'
        jb .end
        cmp al, '9'
        ja .end
        sub al, '0'
        cbw
        xchg ax, bx
        mov dx, 10
        mul dx
        add bx, ax
        cmp bx, 1000
        jae .bad
        inc cx
        lodsb
        jmp .dig
.end:   or cx, cx
        jz .bad
        clc
        ret
.bad:   stc
        ret

skipsp: lodsb
skipsp0:
        cmp al, ' '
        je skipsp
        cmp al, 9
        je skipsp
        ret

cols:   dw 0
rows:   dw 0
cur_c:  dw 0
cur_r:  dw 0
; columns, rows, VESA mode (0: VGA mode 3), AH=11h font, AH=12h BL=30h scan lines
sizes:  dw 80, 25, 0
        db 14h, 2
        dw 80, 28, 0
        db 11h, 2
        dw 80, 43, 0
        db 12h, 1
        dw 80, 50, 0
        db 12h, 2
        dw 80, 60, 108h
        db 0, 0
        dw 132, 25, 109h
        db 0, 0
        dw 132, 43, 10Ah
        db 0, 0
        dw 132, 50, 10Bh
        db 0, 0
        dw 132, 60, 10Ch
        db 0, 0
        dw 0

msg_now:    db 'Screen: $'
msg_list:   db 13, 10, 'Sizes: 80x25 80x28 80x43 80x50 80x60 132x25 132x43 132x50 132x60', 13, 10, '$'
msg_use:    db 'VMRES [COLSxROWS]: set or show the text screen size', 13, 10
            db '  VMRES 80x50   VMRES 132x43   VMRES 132   VMRES 50   VMRES 80x25', 13, 10
            db 'Sizes: 80x25 80x28 80x43 80x50 80x60 132x25 132x43 132x50 132x60', 13, 10, '$'
msg_nosize: db 'VMRES: no such size$'
msg_novesa: db 'VMRES: the video BIOS has no VESA text mode for that size$'
msg_notset: db 'VMRES: the video BIOS left the screen at $'
crlf:       db 13, 10, '$'
