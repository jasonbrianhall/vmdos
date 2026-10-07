; XMSCHK.COM: INT 2Fh AX=4300h result, the INT 2Fh vector, and XMS version.
        org 0x100
        mov ax, 0x4300
        int 0x2F
        call phex8
        mov ax, 0x352F
        int 0x21
        mov ax, es
        call phex16
        mov ax, bx
        call phex16
        mov ax, 0x4310
        int 0x2F
        mov [ent], bx
        mov [ent+2], es
        mov ax, es
        call phex16
        mov ah, 0
        call far [ent]
        call phex16
        ret
phex16: push ax
        mov al, ah
        call phex8
        pop ax
phex8:  push ax
        mov cl, 4
        shr al, cl
        call nib
        pop ax
        push ax
        call nib
        mov dl, ' '
        mov ah, 2
        int 0x21
        pop ax
        ret
nib:    and al, 15
        add al, '0'
        cmp al, '9'
        jbe .o
        add al, 7
.o:     mov dl, al
        mov ah, 2
        int 0x21
        ret
ent:    dd 0
