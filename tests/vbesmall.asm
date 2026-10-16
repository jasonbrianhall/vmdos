; VBESMALL.COM: lists the VESA modes (4F00h, then 4F01h for each), as
; Allegro does, then sets 320x200x8 (mode 120h) and draws colour bars
; through the A000h window for a few seconds.
        cpu 286
        org 100h
        mov ax, 4F00h
        mov di, info
        int 10h
        cmp ax, 004Fh
        jne fail
        lds si, [info + 0Eh]            ; the mode list
.next:  lodsw
        cmp ax, 0FFFFh
        je .done
        push ds
        push si
        push cs
        pop ds
        mov [mode], ax
        mov cx, ax
        mov ax, 4F01h
        mov di, minfo
        int 10h
        mov ax, [mode]
        call puthex4
        mov dl, ' '
        call putc
        mov ax, [minfo + 12h]
        call putdec
        mov dl, 'x'
        call putc
        mov ax, [minfo + 14h]
        call putdec
        mov dl, 'x'
        call putc
        mov al, [minfo + 19h]
        xor ah, ah
        call putdec
        mov dl, ' '
        call putc
        call putc
        pop si
        pop ds
        jmp .next
.done:  push cs
        pop ds
        mov ah, 0                       ; a key or 2 s, then the picture
        int 1Ah
        mov [t0], dx
.w:     mov ah, 0
        int 1Ah
        sub dx, [t0]
        cmp dx, 36
        jb .w
        mov ax, 4F02h
        mov bx, 120h
        int 10h
        cmp ax, 004Fh
        jne fail
        mov ax, 0A000h
        mov es, ax
        xor di, di
        xor dx, dx                      ; y
.row:   xor cx, cx                      ; x
.px:    mov ax, cx
        shr ax, 3                       ; 40 bars of 8 pixels
        add ax, dx
        stosb
        inc cx
        cmp cx, 320
        jb .px
        inc dx
        cmp dx, 200
        jb .row
        mov ah, 0
        int 1Ah
        mov [t0], dx
.w2:    mov ah, 0
        int 1Ah
        sub dx, [t0]
        cmp dx, 91
        jb .w2
        mov ax, 3
        int 10h
        mov dx, m_ok
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h
fail:   push cs
        pop ds
        mov dx, m_fail
        mov ah, 9
        int 21h
        mov ax, 4C01h
        int 21h

putc:   push ax
        mov ah, 2
        int 21h
        pop ax
        ret
puthex4:
        push ax
        mov al, ah
        call puthex2
        pop ax
puthex2:
        push ax
        shr al, 4
        call .nib
        pop ax
        and al, 0Fh
.nib:   push ax
        add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     mov dl, al
        call putc
        pop ax
        ret
putdec: push bx
        push cx
        push dx
        mov bx, 10
        xor cx, cx
.d:     xor dx, dx
        div bx
        push dx
        inc cx
        or ax, ax
        jnz .d
.o:     pop dx
        add dl, '0'
        call putc
        loop .o
        pop dx
        pop cx
        pop bx
        ret

mode:   dw 0
t0:     dw 0
m_ok:   db 'VBESMALL done', 13, 10, '$'
m_fail: db 'VESA call failed', 13, 10, '$'
info:   db 'VBE2'
        times 508 db 0
minfo:  times 256 db 0
