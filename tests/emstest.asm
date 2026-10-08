; EMSTEST.COM: LIM 4.0 expanded memory as games use it. Checks the EMMXXXX0
; name at INT 67h's segment, status / page frame / page counts, allocates 8
; pages, maps them through the frame, writes and reads them back, moves data
; between conventional memory and EMS (57h), saves and restores the page map
; (4Eh) and frees the handle. Prints "EMSTEST OK, nnnn pages free" or
; "EMSTEST FAIL n".
        org 100h
        mov byte [stage], '1'
        xor ax, ax                      ; the driver's name at INT 67h's segment:000Ah
        mov es, ax
        mov es, [es:67h * 4 + 2]
        mov di, 0Ah
        mov si, emmname
        mov cx, 8
        repe cmpsb
        jne fail
        push cs
        pop es
        mov byte [stage], '2'
        mov ah, 40h                     ; status
        int 67h
        or ah, ah
        jnz fail
        mov ah, 41h                     ; page frame
        int 67h
        or ah, ah
        jnz fail
        mov [frame], bx
        mov ah, 42h                     ; pages
        int 67h
        or ah, ah
        jnz fail
        mov [freep], bx
        cmp bx, 8
        jb fail
        mov byte [stage], '3'
        mov ah, 43h                     ; allocate 8 pages
        mov bx, 8
        int 67h
        or ah, ah
        jnz fail
        mov [handle], dx
        mov byte [stage], '4'
        xor bp, bp                      ; write: logical page n -> physical n & 3, mark it n
.w:     mov ax, bp
        and al, 3
        mov ah, 44h
        mov bx, bp
        mov dx, [handle]
        int 67h
        or ah, ah
        jnz fail
        call seg_of
        mov es, ax
        mov ax, bp
        mov [es:0], al
        mov [es:3FFFh], al
        add al, 0A0h
        mov [es:2000h], al
        inc bp
        cmp bp, 8
        jb .w
        mov byte [stage], '5'
        xor bp, bp                      ; read back: map each at physical 3 - (n & 3)
.r:     mov ax, bp
        and al, 3
        mov ah, 3
        sub ah, al
        mov al, ah
        mov ah, 44h
        mov bx, bp
        mov dx, [handle]
        int 67h
        or ah, ah
        jnz fail
        mov ax, bp
        and al, 3
        mov ah, 3
        sub ah, al
        mov al, ah
        xor ah, ah
        push bp
        mov bp, ax
        call seg_of
        pop bp
        mov es, ax
        mov ax, bp
        cmp [es:0], al
        jne fail
        cmp [es:3FFFh], al
        jne fail
        add al, 0A0h
        cmp [es:2000h], al
        jne fail
        inc bp
        cmp bp, 8
        jb .r
        mov byte [stage], '6'
        push cs                         ; 57h: move 20000 bytes conventional -> EMS (page 2, offset 100)
        pop es
        mov di, buf
        mov cx, 20000
        mov al, 5Ah
.f:     stosb
        inc al
        loop .f
        mov dword [mv_len], 20000
        mov byte [mv_st], 0
        mov word [mv_sh], 0
        mov word [mv_so], buf
        mov [mv_ss], cs
        mov byte [mv_dt], 1
        mov ax, [handle]
        mov [mv_dh], ax
        mov word [mv_do], 100
        mov word [mv_ds], 2
        mov si, mv_len
        mov ax, 5700h
        int 67h
        or ah, ah
        jnz fail
        mov di, buf                     ; clear the buffer, move it back
        mov cx, 20000
        xor al, al
        rep stosb
        mov byte [mv_st], 1             ; swap source and destination
        mov ax, [handle]
        mov [mv_sh], ax
        mov word [mv_so], 100
        mov word [mv_ss], 2
        mov byte [mv_dt], 0
        mov word [mv_dh], 0
        mov word [mv_do], buf
        mov [mv_ds], cs
        mov si, mv_len
        mov ax, 5700h
        int 67h
        or ah, ah
        jnz fail
        mov si, buf
        mov cx, 20000
        mov ah, 5Ah
.c:     lodsb
        cmp al, ah
        jne fail
        inc ah
        loop .c
        mov byte [stage], '7'
        mov ax, 4400h                   ; physical 0 <- logical 1, then save the map
        mov bx, 1
        mov dx, [handle]
        int 67h
        mov ax, 4E00h                   ; save the page map, change it, restore it
        mov di, map
        int 67h
        or ah, ah
        jnz fail
        mov ax, 4400h                   ; physical 0 <- logical 7
        mov bx, 7
        mov dx, [handle]
        int 67h
        mov es, [frame]
        cmp byte [es:0], 7
        jne fail
        mov ax, 4E01h
        mov si, map
        int 67h
        or ah, ah
        jnz fail
        mov es, [frame]
        cmp byte [es:0], 1              ; logical 1 is back
        jne fail
        mov byte [stage], '8'
        mov ah, 45h                     ; free
        mov dx, [handle]
        int 67h
        or ah, ah
        jnz fail
        mov ah, 42h
        int 67h
        cmp bx, [freep]
        jne fail
        mov dx, okmsg
        mov ah, 9
        int 21h
        mov ax, [freep]
        call number
        mov dx, okmsg2
        jmp short say
fail:   mov al, [stage]
        mov [failn], al
        mov dx, failmsg
say:    mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

seg_of: mov ax, bp                      ; segment of physical page BP & 3
        and ax, 3
        push cx
        mov cl, 10
        shl ax, cl
        pop cx
        add ax, [frame]
        ret

number: xor cx, cx
        mov bx, 10
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

emmname db 'EMMXXXX0'
stage   db '0'
frame   dw 0
freep   dw 0
handle  dw 0
okmsg   db 'EMSTEST OK, $'
okmsg2  db ' pages free', 13, 10, '$'
failmsg db 'EMSTEST FAIL '
failn   db '0', 13, 10, '$'
mv_len  dd 0
mv_st   db 0
mv_sh   dw 0
mv_so   dw 0
mv_ss   dw 0
mv_dt   db 0
mv_dh   dw 0
mv_do   dw 0
mv_ds   dw 0
map     times 64 db 0
buf:
