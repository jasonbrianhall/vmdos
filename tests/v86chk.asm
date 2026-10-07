; V86CHK.COM: what SMSW and MOV EAX,CR0 report (PE = bit 0).
        org 0x100
        smsw ax
        call phex
        mov eax, cr0
        call phex
        pushf
        pop ax
        call phex
        ret
phex:   mov cx, 4
.h:     rol ax, 4
        push ax
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .o
        add al, 7
.o:     mov dl, al
        mov ah, 2
        int 0x21
        pop ax
        loop .h
        mov dl, ' '
        mov ah, 2
        int 0x21
        ret
