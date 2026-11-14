; LOSTIRQ.COM: a keyboard IRQ lost with its byte still in the 8042 (an INT 9
; handler that sends the EOI without reading port 60h), then the BIOS's INT 9
; again and INT 16h AH=00 for a key. The 8042 hands nothing on while a byte
; is unread, so without a new IRQ 1 the keyboard (and the PS/2 mouse) stay dead
; and this waits forever; vkbd_refill (src/vdev.c) requests the IRQ again
; after 100 ms, and the key that was pressed arrives: C:\KEY.TXT says GOT a OK
; for an "a". nasm -f bin -o LOSTIRQ.COM tests/lostirq.asm
        org 100h
        mov ax, 3509h
        int 21h
        mov [old9], bx
        mov [old9+2], es
        mov dx, lose
        mov ax, 2509h
        int 21h
        mov dx, msg1
        mov ah, 9
        int 21h
wait1:  sti
        hlt
        cmp byte [hit], 0
        je wait1
        push ds
        lds dx, [old9]
        mov ax, 2509h
        int 21h
        pop ds
        mov dx, msg2
        mov ah, 9
        int 21h
        xor ah, ah
        int 16h                 ; the stuck key, if the 8042 is ever read again
        mov [keyb], ax
        mov ah, 3Ch
        xor cx, cx
        mov dx, fname
        int 21h
        mov bx, ax
        mov al, [keyb]
        mov [obuf+4], al
        mov ah, 40h
        mov cx, outlen
        mov dx, obuf
        int 21h
        mov ah, 3Eh
        int 21h
        mov dx, msg3
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h
lose:   push ax
        mov byte [cs:hit], 1
        mov al, 20h
        out 20h, al             ; EOI, port 60h not read
        pop ax
        iret
old9    dd 0
hit     db 0
keyb    dw 0
fname   db 'C:\KEY.TXT', 0
obuf    db 'GOT ? OK', 13, 10
outlen  equ $ - obuf
msg1    db 'press a key$'
msg2    db 13, 10, 'lost; waiting$'
msg3    db 13, 10, 'done$'
