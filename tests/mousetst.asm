; MOUSETST.COM: INT 33h reset/show/position/handler test. Any key exits.
; Arg "g" runs it in mode 13h.
        org 0x100
        cmp byte [0x82], 'g'
        jne .t
        mov ax, 0x13
        int 0x10
.t:     xor ax, ax
        int 0x33
        mov [inst], ax
        mov ax, 1
        int 0x33
        mov ax, 0x0C                ; handler for every event
        mov cx, 0x7F
        push cs
        pop es
        mov dx, handler
        int 0x33
.loop:  mov ax, 3
        int 0x33
        mov [bx_], bx
        mov [cx_], cx
        mov [dx_], dx
        mov ah, 2                   ; cursor to row 20, col 0
        xor bh, bh
        mov dx, 0x1400
        int 0x10
        mov ax, [inst]
        call phex
        mov ax, [cx_]
        call phex
        mov ax, [dx_]
        call phex
        mov ax, [bx_]
        call phex
        mov ax, [calls]
        call phex
        mov ax, [lastev]
        call phex
        mov ah, 1
        int 0x16
        jz .loop
        xor ax, ax
        int 0x16
        xor ax, ax                  ; reset removes the handler
        int 0x33
        mov ax, 3
        int 0x10
        ret
handler:                            ; far, AX = events
        inc word [cs:calls]
        mov [cs:lastev], ax
        retf
phex:   mov cx, 4
.h:     rol ax, 4
        push ax
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .o
        add al, 7
.o:     mov ah, 0x0E
        mov bl, 15
        int 0x10
        pop ax
        loop .h
        push ax
        mov ax, 0x0E20
        int 0x10
        pop ax
        ret
inst:   dw 0
calls:  dw 0
lastev: dw 0
bx_:    dw 0
cx_:    dw 0
dx_:    dw 0
