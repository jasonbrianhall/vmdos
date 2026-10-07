; VMCD.COM: the CD images vmdos has, and which is in each drive.
;   VMCD            list them
;   VMCD 1 2        put image 2 in CD drive 1 (for the next disc of a game)
;   VMCD 1 0        empty drive 1
        org 100h
        mov si, 81h
        call number
        jc list
        mov cl, al                      ; drive
        call number
        jc usage
        mov dl, al                      ; image
        mov ax, 5644h
        mov bx, 2
        xor ch, ch
        int 2Fh
        or ax, ax
        jnz bad
list:   mov ax, 5644h
        mov bx, 2
        xor cx, cx
        mov di, buf
        int 2Fh
        cmp ax, 5644h
        je novm
        mov dx, buf
        jmp short say
bad:    mov dx, msg_bad
        jmp short say
usage:  mov dx, msg_use
        jmp short say
novm:   mov dx, msg_novm
say:    mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

number:                                 ; next decimal number on the command line -> AL, CF if none
        lodsb
        cmp al, ' '
        je number
        cmp al, 9
        je number
        cmp al, '0'
        jb .no
        cmp al, '9'
        ja .no
        sub al, '0'
        mov bh, al                      ; the number so far
.more:  lodsb
        cmp al, '0'
        jb .end
        cmp al, '9'
        ja .end
        sub al, '0'
        xchg al, bh                     ; AL = so far, BH = digit
        mov bl, 10
        mul bl
        add al, bh
        mov bh, al
        jmp .more
.end:   dec si
        mov al, bh
        clc
        ret
.no:    dec si
        stc
        ret

msg_use  db 'Usage: VMCD [drive image]   (no arguments: list the CD images)', 13, 10, '$'
msg_bad  db 'VMCD: no such drive or image', 13, 10, '$'
msg_novm db 'VMCD: not running under vmdos', 13, 10, '$'
buf:
