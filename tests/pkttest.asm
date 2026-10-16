; PKTTEST.COM: exercises a packet driver on INT 60h. Takes EtherType 88B5h
; (local experimental), sends a broadcast "hello" frame, then prints each
; 88B5h frame that comes back for ~10 s and answers the first one.
        cpu 286
        org 100h
        mov ax, 3560h
        int 21h
        lea di, [bx + 3]
        mov si, sig
        mov cx, 8
        repe cmpsb
        je .have
        mov dx, m_nodrv
        jmp die
.have:  mov ax, 01FFh               ; driver_info
        int 60h
        jc .fail
        push cs
        pop es
        mov di, name                ; copy the name (DS:SI)
.cn:    lodsb
        stosb
        or al, al
        jnz .cn
        push cs
        pop ds
        mov dx, m_name
        call puts
        mov si, name
        call putz
        call crlf

        mov ax, 0201h               ; access_type, class 1
        mov bx, 0FFFFh
        xor dl, dl
        mov si, etype
        mov cx, 2
        push cs
        pop es
        mov di, receiver
        int 60h
        jc .fail
        mov [handle], ax

        mov ah, 6                   ; get_address
        mov bx, [handle]
        mov di, frame + 6
        mov cx, 6
        int 60h
        jc .fail
        mov dx, m_mac
        call puts
        mov si, frame + 6
        call putmac
        call crlf

        mov ah, 4                   ; send_pkt
        mov si, frame
        mov cx, frame_len
        int 60h
        jc .fail
        mov dx, m_sent
        call puts

        mov ah, 0                   ; ~10 s
        int 1Ah
        mov [t0], dx
.wait:  cmp byte [got], 0
        je .idle
        mov byte [got], 0
        mov dx, m_got
        call puts
        mov ax, [rlen]
        call putdec
        mov dx, m_from
        call puts
        mov si, rbuf + 6
        call putmac
        mov dx, m_say
        call puts
        mov si, rbuf + 14
        call putz
        call crlf
        cmp byte [answered], 0
        jne .idle
        mov byte [answered], 1
        mov si, rbuf + 6            ; answer it, to its sender
        mov di, reply
        push cs
        pop es
        movsw
        movsw
        movsw
        mov si, frame + 6
        movsw
        movsw
        movsw
        mov ah, 4
        mov si, reply
        mov cx, reply_len
        int 60h
        jc .fail
        mov dx, m_replied
        call puts
.idle:  mov ah, 0
        int 1Ah
        sub dx, [t0]
        cmp dx, 182
        jb .wait

        mov ah, 18h                 ; statistics
        mov bx, [handle]
        int 60h
        jc .fail
        push cs
        pop es
        mov ax, [si]                ; packets in
        push cs
        pop ds
        push ax
        mov dx, m_stat
        call puts
        pop ax
        call putdec
        call crlf
        mov ah, 3                   ; release_type
        mov bx, [handle]
        int 60h
        mov dx, m_done
        call puts
        mov ax, 4C00h
        int 21h

.fail:  push cs
        pop ds
        push dx
        mov dx, m_err
        call puts
        pop ax
        mov al, ah
        xor ah, ah
        call putdec
        call crlf
        mov ax, 4C01h
        int 21h

die:    call puts
        mov ax, 4C01h
        int 21h

; the receiver: AX=0 -> ES:DI a buffer, AX=1 -> DS:SI filled
receiver:
        or ax, ax
        jnz .full
        cmp byte [cs:got], 0
        jne .none
        cmp cx, 600
        ja .none
        mov [cs:rlen], cx
        push cs
        pop es
        mov di, rbuf
        retf
.none:  xor di, di
        mov es, di
        retf
.full:  mov byte [cs:got], 1
        retf

crlf:   mov dx, m_crlf
puts:   mov ah, 9
        int 21h
        ret
putz:   lodsb
        or al, al
        jz .e
        mov dl, al
        mov ah, 2
        int 21h
        jmp putz
.e:     ret
putmac: mov cx, 6
.l:     lodsb
        call puthex2
        dec cx
        jz .e
        mov dl, ':'
        mov ah, 2
        int 21h
        jmp .l
.e:     ret
puthex2:
        push ax
        shr al, 4
        call .nib
        pop ax
        and al, 0Fh
.nib:   add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     mov dl, al
        mov ah, 2
        int 21h
        ret
putdec: mov bx, 10
        xor cx, cx
.d:     xor dx, dx
        div bx
        push dx
        inc cx
        or ax, ax
        jnz .d
.p:     pop dx
        add dl, '0'
        mov ah, 2
        int 21h
        loop .p
        ret

sig:    db 'PKT DRVR'
etype:  db 88h, 0B5h
frame:  db 0FFh, 0FFh, 0FFh, 0FFh, 0FFh, 0FFh
        times 6 db 0
        db 88h, 0B5h
        db 'hello from vmdos', 0
        times 64 - ($ - frame) db 0
frame_len equ $ - frame
reply:  times 12 db 0
        db 88h, 0B5h
        db 'reply from vmdos', 0
        times 64 - ($ - reply) db 0
reply_len equ $ - reply
handle: dw 0
t0:     dw 0
got:    db 0
answered: db 0
rlen:   dw 0
m_nodrv: db 'no packet driver on INT 60h', 13, 10, '$'
m_name: db 'driver: $'
m_mac:  db 'MAC: $'
m_sent: db 'sent broadcast hello', 13, 10, '$'
m_got:  db 'got $'
m_from: db ' bytes from $'
m_say:  db ': $'
m_replied: db 'replied', 13, 10, '$'
m_stat: db 'frames in: $'
m_done: db 'PKTTEST done', 13, 10, '$'
m_err:  db 'packet driver error $'
m_crlf: db 13, 10, '$'
name:   times 64 db 0
rbuf:   times 600 db 0
