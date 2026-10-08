; VMCD.SYS: CD-ROM device driver for vmdos's CD drives (ISO files on C:,
; chosen with VMCD.COM, or loaded as boot modules). Every request goes to
; the monitor (INT 2Fh AX=5644h);
; SHSUCDX or MSCDEX then gives each drive a letter:
;   DEVICE=C:\VMDOS\VMCD.SYS    (in FDCONFIG.SYS)
;   SHSUCDX /D:?VMCD0001        (in AUTOEXEC.BAT)
; Not under vmdos it doesn't stay in memory (and says nothing).
        org 0
hdr:    dd -1
        dw 0C800h                       ; character device, IOCTL, open/close
        dw strategy
        dw interrupt
        db 'VMCD0001'
        dw 0                            ; reserved
        db 0                            ; drive letter (set by SHSUCDX/MSCDEX)
units:  db 1                            ; number of drives
rh:     dd 0

strategy:
        mov [cs:rh], bx
        mov [cs:rh + 2], es
        retf

interrupt:
        push ax
        push bx
        push dx
        push di
        push es
        les di, [cs:rh]
        cmp byte [es:di + 2], 0
        je init
        mov ax, 5644h
        mov bx, 1
        mov dx, cs
        int 2Fh                         ; the monitor does the request
done:   pop es
        pop di
        pop dx
        pop bx
        pop ax
        retf
resident_end:

init:   push cx
        push ds
        push cs
        pop ds
        mov ax, 5644h
        xor bx, bx
        int 2Fh
        cmp ax, 5644h                   ; unchanged: not vmdos
        je .none
        or ax, ax
        jz .none
        mov [units], al
        mov [es:di + 13], al
        mov word [es:di + 14], resident_end
        mov [es:di + 16], cs
        mov word [es:di + 3], 0100h
        add al, '0'
        mov [msg_n], al
        mov dx, msg_ok
        jmp short .say
.none:  mov byte [es:di + 13], 0
        mov word [es:di + 14], 0        ; don't stay
        mov [es:di + 16], cs
        mov word [es:di + 3], 810Ch
        jmp short .out                  ; quietly: no images is the usual case
.say:   mov ah, 9
        int 21h
.out:   pop ds
        pop cx
        jmp done

msg_ok   db 'VMCD: '
msg_n    db '0 CD-ROM drive(s); VMCD.COM changes the disc', 13, 10, '$'
