; CDPLAY.COM: CD audio through MSCDEX/SHSUCDX (INT 2Fh AX=1510h).
;   CDPLAY E 2      play track 2 of drive E: to its end
; Prints the disc's tracks, then the Q-channel position (track, time in the
; track) twice a second while the drive says busy. Keys: P pause (STOP
; AUDIO), R resume, S stop twice (pause, then reset), Q quit (stops).
        org 100h
        mov si, 81h
        call skipsp
        lodsb
        and al, 0DFh
        sub al, 'A'
        jb usage
        cmp al, 25
        ja usage
        mov [drv], al
        call skipsp
        xor bx, bx                      ; track number
.num:   lodsb
        sub al, '0'
        jb .got
        cmp al, 9
        ja .got
        imul bx, bx, 10
        add bl, al
        jmp .num
.got:   or bx, bx
        jz usage
        mov [trk], bl

        mov byte [ib], 10               ; audio disc info: first, last, lead-out
        mov cx, 7
        call ioctl
        jc fail
        mov al, [ib + 1]
        mov [first], al
        mov al, [ib + 2]
        mov [last], al
        mov eax, [ib + 3]
        mov [leadout], eax
        mov dx, m_disc
        call puts
        mov al, [first]
        call putdec
        mov dl, '-'
        call putc
        mov al, [last]
        call putdec
        call crlf

        mov al, [first]                 ; list the tracks
.list:  cmp al, [last]
        ja .listed
        push ax
        mov [ib + 1], al
        mov byte [ib], 11
        mov cx, 7
        call ioctl
        pop ax
        jc fail
        push ax
        mov dx, m_trk
        call puts
        pop ax
        push ax
        call putdec
        mov dl, ' '
        call putc
        mov eax, [ib + 2]
        call putmsf
        test byte [ib + 6], 40h
        mov dx, m_audio
        jz .k
        mov dx, m_data
.k:     call puts
        pop ax
        inc al
        jmp .list
.listed:
        mov al, [trk]                   ; start: the track's; end: the next one's (or the lead-out)
        cmp al, [first]
        jb usage
        cmp al, [last]
        ja usage
        mov [ib + 1], al
        mov byte [ib], 11
        mov cx, 7
        call ioctl
        jc fail
        mov eax, [ib + 2]
        mov [start], eax
        mov eax, [leadout]
        mov al, [trk]
        cmp al, [last]
        mov eax, [leadout]
        je .end
        mov al, [trk]
        inc al
        mov [ib + 1], al
        mov byte [ib], 11
        mov cx, 7
        call ioctl
        jc fail
        mov eax, [ib + 2]
.end:   call frames
        push eax
        mov eax, [start]
        call frames
        mov ebx, eax
        pop eax
        sub eax, ebx
        mov [count], eax
        call play
        jc fail
        mov dx, m_play
        call puts

.loop:  mov ah, 1                       ; a key?
        int 16h
        jz .wait
        xor ah, ah
        int 16h
        and al, 0DFh
        cmp al, 'Q'
        je .quit
        cmp al, 'P'
        je .pause
        cmp al, 'S'
        je .pause
        cmp al, 'R'
        jne .wait
        mov byte [req + 2], 136         ; RESUME AUDIO
        call simple
        mov dx, m_resume
        call puts
        jmp .wait
.pause: mov byte [req + 2], 133         ; STOP AUDIO
        call simple
        mov dx, m_pause
        call puts
.wait:  xor ah, ah                      ; ~0.5 s
        int 1Ah
        mov bx, dx
.w2:    xor ah, ah
        int 1Ah
        sub dx, bx
        cmp dx, 9
        jb .w2
        mov byte [ib], 12               ; Q-channel: track, time in it
        mov cx, 11
        call ioctl
        jc fail
        mov dx, m_q
        call puts
        mov al, [ib + 2]
        call putdec
        mov dl, ' '
        call putc
        mov al, [ib + 4]
        call putdec
        mov dl, ':'
        call putc
        mov al, [ib + 5]
        call put2
        mov dl, '.'
        call putc
        mov al, [ib + 6]
        call put2
        test word [req + 3], 200h       ; busy: playing
        mov dx, m_busy
        jnz .b
        mov dx, m_idle
.b:     call puts
        mov byte [ib], 15               ; audio status: paused?
        mov cx, 11
        call ioctl
        test word [req + 3], 200h
        jnz .loop
        test byte [ib + 1], 1
        jnz .loop                       ; paused: wait for R / Q
        mov dx, m_done
        call puts
        ret
.quit:  mov byte [req + 2], 133
        call simple
        call simple                     ; twice: reset
        mov dx, m_done
        call puts
        ret

; IOCTL input, the control block at ib (CX bytes). CF on error.
ioctl:  call clear
        mov byte [req], 26
        mov byte [req + 2], 3
        mov word [req + 14], ib
        mov [req + 16], cs
        mov [req + 18], cx
        jmp send

; PLAY AUDIO [start] (Red Book) for [count] frames.
play:   call clear
        mov byte [req], 22
        mov byte [req + 2], 132
        mov byte [req + 13], 1
        mov eax, [start]
        mov [req + 14], eax
        mov eax, [count]
        mov [req + 18], eax
        jmp send

simple: mov al, [req + 2]
        call clear
        mov [req + 2], al
        mov byte [req], 13
send:   mov ax, 1510h
        xor cx, cx
        mov cl, [drv]
        mov bx, req
        push cs
        pop es
        int 2Fh
        test word [req + 3], 8000h
        jz .ok
        stc
        ret
.ok:    clc
        ret

clear:  push di
        push cx
        push ax
        mov di, req
        mov cx, 13
        xor al, al
        rep stosb
        pop ax
        pop cx
        pop di
        ret

; EAX: Red Book address (frame, sec, min bytes) -> frames
frames: push ebx
        push ecx
        movzx ebx, al                   ; frames
        movzx ecx, ah                   ; seconds
        shr eax, 16
        and eax, 0FFh                   ; minutes
        imul eax, eax, 60
        add eax, ecx
        imul eax, eax, 75
        add eax, ebx
        pop ecx
        pop ebx
        ret

putmsf: push eax
        shr eax, 16
        call putdec
        mov dl, ':'
        call putc
        pop eax
        push eax
        mov al, ah
        call put2
        mov dl, '.'
        call putc
        pop eax
        call put2
        ret

put2:   cmp al, 10
        jae putdec
        push ax
        mov dl, '0'
        call putc
        pop ax
putdec: xor ah, ah                      ; AL, unsigned
        mov bl, 10
        div bl
        push ax
        or al, al
        jz .one
        call putdec
.one:   pop ax
        mov dl, ah
        add dl, '0'
putc:   mov ah, 2
        int 21h
        ret
crlf:   mov dx, m_crlf
puts:   mov ah, 9
        int 21h
        ret

skipsp: cmp byte [si], ' '
        jne .r
        inc si
        jmp skipsp
.r:     ret

fail:   mov dx, m_fail
        call puts
        mov ax, [req + 3]
        mov al, ah                      ; not quite hex, but the low byte is the error
        mov al, [req + 3]
        call putdec
        call crlf
        mov ax, 4C01h
        int 21h
usage:  mov dx, m_use
        call puts
        mov ax, 4C01h
        int 21h

m_use   db 'Usage: CDPLAY drive track   (e.g. CDPLAY E 2)', 13, 10, '$'
m_fail  db 'CDPLAY: request failed, error $'
m_disc  db 'tracks $'
m_trk   db ' track $'
m_audio db ' audio', 13, 10, '$'
m_data  db ' data', 13, 10, '$'
m_play  db 'playing', 13, 10, '$'
m_pause db 'stop audio', 13, 10, '$'
m_resume db 'resume', 13, 10, '$'
m_q     db 'T$'
m_busy  db ' busy', 13, 10, '$'
m_idle  db ' idle', 13, 10, '$'
m_done  db 'done', 13, 10, '$'
m_crlf  db 13, 10, '$'
drv     db 0
trk     db 0
first   db 0
last    db 0
leadout dd 0
start   dd 0
count   dd 0
req     times 32 db 0
ib      times 16 db 0
