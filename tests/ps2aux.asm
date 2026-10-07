; PS2AUX.COM: a program with its own PS/2 mouse code: hooks IRQ 12 (INT 74h),
; enables the 8042 mouse port and reporting, counts 3-byte packets for
; about 5 seconds (move the mouse) and prints "PS2AUX packets nnnn, x y".
        org 100h
        mov ax, 3574h
        int 21h
        mov [old], bx
        mov [old + 2], es
        mov dx, irq12
        mov ax, 2574h
        int 21h
        cli
        call wait_in
        mov al, 0A8h                    ; mouse port on
        out 64h, al
        call wait_in
        mov al, 20h                     ; read command byte
        out 64h, al
        call wait_out
        in al, 60h
        or al, 2                        ; mouse IRQ on
        mov ah, al
        call wait_in
        mov al, 60h
        out 64h, al
        call wait_in
        mov al, ah
        out 60h, al
        mov al, 0F4h                    ; mouse: enable reporting
        call to_mouse
        call wait_out
        in al, 60h                      ; ACK
        in al, 0A1h
        and al, 0EFh                    ; unmask IRQ 12
        out 0A1h, al
        in al, 21h
        and al, 0FBh                    ; and the cascade
        out 21h, al
        sti
        push 40h
        pop es
        mov bx, [es:6Ch]
        add bx, 91
.w:     cmp bx, [es:6Ch]
        jne .w
        cli
        mov al, 0F5h                    ; reporting off
        call to_mouse
        sti
        push ds
        lds dx, [old]
        mov ax, 2574h
        int 21h
        pop ds
        mov dx, msg
        mov ah, 9
        int 21h
        mov ax, [packets]
        call hex
        mov dl, ','
        mov ah, 2
        int 21h
        mov ax, [xsum]
        call hex
        mov dl, ' '
        mov ah, 2
        int 21h
        mov ax, [ysum]
        call hex
        mov ax, 4C00h
        int 21h
to_mouse:
        push ax
        call wait_in
        mov al, 0D4h
        out 64h, al
        call wait_in
        pop ax
        out 60h, al
        ret
wait_in:
        in al, 64h
        test al, 2
        jnz wait_in
        ret
wait_out:
        in al, 64h
        test al, 1
        jz wait_out
        ret
irq12:  push ax
        push bx
        push ds
        push cs
        pop ds
        in al, 60h
        mov bl, [phase]
        or bl, bl
        jnz .b1
        test al, 8                      ; first byte has bit 3 set
        jz .done
        mov [b0], al
        inc byte [phase]
        jmp .done
.b1:    cmp bl, 1
        jne .b2
        mov [b1], al
        inc byte [phase]
        jmp .done
.b2:    mov byte [phase], 0
        inc word [packets]
        mov bl, [b1]
        mov bh, 0
        test byte [b0], 10h
        jz .xp
        mov bh, 0FFh
.xp:    add [xsum], bx
        mov bl, al
        mov bh, 0
        test byte [b0], 20h
        jz .yp
        mov bh, 0FFh
.yp:    add [ysum], bx
.done:  mov al, 20h
        out 0A0h, al
        out 20h, al
        pop ds
        pop bx
        pop ax
        iret
hex:    push cx
        mov cx, 4
.h:     rol ax, 4
        push ax
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .d
        add al, 7
.d:     mov dl, al
        mov ah, 2
        int 21h
        pop ax
        loop .h
        pop cx
        ret
old     dd 0
packets dw 0
xsum    dw 0
ysum    dw 0
phase   db 0
b0      db 0
b1      db 0
msg     db 'PS2AUX packets $'
