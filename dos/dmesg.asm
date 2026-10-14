; DMESG.COM: vmdos's log, as on its Ctrl+Shift+F10 screen.
;   DMESG               the log on standard output (DMESG > LOG.TXT, DMESG | MORE)
;   DMESG /K            make C:\VMDOS\VMDOS.LOG (32 KiB): when vmdos stops,
;                       or on S in its Ctrl+Shift+F10 log, vmdos writes its
;                       log into that file itself, in place (for a guest
;                       that has crashed or hung; read it after a restart)
; (INT 2Fh AX=564Ch: EDX = log position, CX = size of the buffer at ES:DI;
;  returns CX = bytes, CR LF lines, EDX = next position, BX = 'VM'.)
        cpu 386
        org 100h

BUFSZ   equ 4096
LOGSZ   equ 32768

        mov si, 81h
.opt:   lodsb
        cmp al, ' '
        je .opt
        cmp al, 9
        je .opt
        cmp al, 13
        je dump
        cmp al, '/'
        je .sw
        cmp al, '-'
        jne usage
.sw:    lodsb
        or al, 20h
        cmp al, 'k'
        je keep
        jmp usage

; ---- the log on standard output ----
dump:   xor edx, edx
        mov byte [first], 1
.next:  push cs
        pop es
        mov di, buf
        mov cx, BUFSZ
        xor bx, bx
        mov ax, 564Ch
        int 2Fh
        cmp byte [first], 0
        je .got
        cmp bx, 564Dh                   ; 'VM'
        jne notvm
        mov byte [first], 0
.got:   jcxz .done
        push edx
        mov ah, 40h                     ; write CX bytes to standard output
        mov bx, 1
        mov dx, buf
        int 21h
        pop edx
        jc .done
        jmp .next
.done:  mov ax, 4C00h
        int 21h

notvm:  mov dx, msg_notvm
        jmp fail

; ---- /K: reserve C:\VMDOS\VMDOS.LOG ----
keep:   mov ax, 564Ch                   ; under vmdos at all?
        xor edx, edx
        xor cx, cx
        xor bx, bx
        push cs
        pop es
        mov di, buf
        int 2Fh
        cmp bx, 564Dh
        jne notvm
        mov di, buf                     ; first block: the signature, Ctrl+Z, spaces
        mov cx, BUFSZ
        mov al, ' '
        rep stosb
        mov si, sig
        mov di, buf
.cp:    lodsb
        stosb
        or al, al
        jnz .cp
        mov byte [di - 1], 1Ah
        mov ah, 3Ch                     ; create (or truncate)
        xor cx, cx
        mov dx, logname
        int 21h
        jc .err
        mov bx, ax
        mov bp, LOGSZ / BUFSZ
.wr:    mov ah, 40h
        mov cx, BUFSZ
        mov dx, buf
        int 21h
        jc .err
        cmp ax, BUFSZ
        jne .err                        ; disk full
        cmp bp, LOGSZ / BUFSZ           ; after the first block: spaces only
        jne .w2
        push bx
        mov di, buf
        mov cx, BUFSZ
        mov al, ' '
        rep stosb
        pop bx
.w2:    dec bp
        jnz .wr
        mov ah, 3Eh
        int 21h
        mov dx, msg_kept
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h
.err:   mov dx, msg_err
        jmp fail

usage:  mov dx, msg_usage
fail:   mov ah, 9
        int 21h
        mov ax, 4C01h
        int 21h

logname db 'C:\VMDOS\VMDOS.LOG', 0
sig     db 'vmdos log, reserved by DMESG /K: vmdos writes its log here when it stops,', 13, 10
        db 'or on S in its Ctrl+Shift+F10 log.', 13, 10, 0
msg_kept db 'C:\VMDOS\VMDOS.LOG is ready: vmdos saves its log there when it stops,', 13, 10
        db 'or when you press S in its Ctrl+Shift+F10 log.', 13, 10, '$'
msg_err db 'DMESG: can', 39, 't make C:\VMDOS\VMDOS.LOG (no C:\VMDOS folder, or the disk is full)', 13, 10, '$'
msg_notvm db 'DMESG: not running under vmdos', 13, 10, '$'
msg_usage db 'DMESG       vmdos', 39, 's log (DMESG > LOG.TXT, DMESG | MORE)', 13, 10
        db 'DMESG /K    make C:\VMDOS\VMDOS.LOG, where vmdos saves its log when it', 13, 10
        db '            stops or on S in its Ctrl+Shift+F10 log (after a crash or hang)', 13, 10, '$'
first   db 0
buf:
