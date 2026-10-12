; VMSHOT.COM: save the screenshot taken with Ctrl+Shift+F9 as a BMP.
;   VMSHOT            to VMSHOT.BMP
;   VMSHOT WAR2.BMP   to WAR2.BMP
; The picture is what vmdos sent to the display, at the screen's size.
; (INT 2Fh AX=5650h: BL=0 gives DX:CX = its size; BL=1 copies CX bytes
; from offset DX:SI to ES:DI, CX = what was copied; BX = 'VM'.)
        org 100h
CHUNK   equ 8000h
        mov si, 81h                     ; the file name: first word of the command tail
.skip:  lodsb
        cmp al, ' '
        je .skip
        cmp al, 9
        je .skip
        dec si
        mov di, si
.end:   lodsb
        cmp al, ' '
        jbe .term
        jmp .end
.term:  dec si
        mov byte [si], 0
        cmp si, di
        jne .named
        mov di, defname
.named: mov [fname], di

        mov ax, 5650h
        xor bx, bx
        int 2Fh
        cmp bx, 564Dh
        jne .novm
        mov ax, cx
        or ax, dx
        jz .none
        mov [size], cx
        mov [size + 2], dx

        mov ah, 3Ch                     ; create
        xor cx, cx
        mov dx, [fname]
        int 21h
        jc .ferr
        mov [handle], ax
        xor si, si                      ; DX:SI = offset
        xor dx, dx
.loop:  push dx
        push si
        mov ax, 5650h
        mov bx, 1
        mov cx, CHUNK
        push ds
        pop es
        mov di, buf
        int 2Fh
        pop si
        pop dx
        jcxz .done
        push dx
        push cx
        mov ah, 40h                     ; write CX bytes
        mov bx, [handle]
        mov dx, buf
        int 21h
        pop cx
        pop dx
        jc .werr
        cmp ax, cx
        jne .werr
        add si, cx
        adc dx, 0
        jmp .loop
.done:  mov ah, 3Eh
        mov bx, [handle]
        int 21h
        mov dx, msg_ok
        mov ah, 9
        int 21h
        mov si, [fname]
.pn:    lodsb
        or al, al
        jz .crlf
        mov dl, al
        mov ah, 2
        int 21h
        jmp .pn
.crlf:  mov dx, crlf
        jmp .exit0
.werr:  mov ah, 3Eh
        mov bx, [handle]
        int 21h
        mov dx, msg_werr
        jmp .exit1
.ferr:  mov dx, msg_ferr
        jmp .exit1
.none:  mov dx, msg_none
        jmp .exit1
.novm:  mov dx, msg_novm
.exit1: mov ah, 9
        int 21h
        mov ax, 4C01h
        int 21h
.exit0: mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

defname  db 'VMSHOT.BMP', 0
msg_ok   db 'Screenshot saved to $'
crlf     db 13, 10, '$'
msg_none db 'No screenshot yet: press Ctrl+Shift+F9 first (in a game or anywhere).', 13, 10, '$'
msg_novm db 'Not running under vmdos.', 13, 10, '$'
msg_ferr db 'Can', 39, 't create the file.', 13, 10, '$'
msg_werr db 'Write failed (disk full?).', 13, 10, '$'
fname    dw 0
handle   dw 0
size     dd 0
buf:
