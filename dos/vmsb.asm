; VMSB.COM: which Sound Blaster vmdos shows DOS programs, and the mic.
;   VMSB 16       a Sound Blaster 16 (DSP 4.05, 16-bit sound on DMA 5)
;   VMSB PRO      a Sound Blaster Pro 2.0 (DSP 3.02; the default)
;   VMSB          show which
;   VMSB MIC      mic level meter for 10 s (talk into it); Esc stops
;   VMSB BOOST n  mic boost, n dB (0, 10, 20, 30: the codec rounds it)
; Prints the BLASTER line to match; exits with errorlevel 16 or 2.
; (INT 2Fh AX=5642h, BX = 0 ask / 2 / 16 / 1 mic stats / 3 boost CX dB;
;  returns BX = 'VM'.)
        org 100h
        mov si, 81h
.skip:  lodsb
        cmp al, ' '
        je .skip
        cmp al, 9
        je .skip
        xor bx, bx
        cmp al, '1'
        jne .p
        mov bx, 16
        jmp model
.p:     or al, 20h
        cmp al, 'p'
        jne .m
        mov bx, 2
        jmp model
.m:     cmp al, 'm'
        je mic
        cmp al, 'b'
        je boost
        jmp model

model:  mov ax, 5642h
        int 2Fh
        cmp bx, 564Dh
        jne novm
        push ax
        mov dx, pro
        cmp al, 16
        jne .show
        mov dx, sb16
.show:  mov ah, 9
        int 21h
        pop ax
        mov ah, 4Ch
        int 21h

novm:   mov dx, msg_novm
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

boost:  lodsb                           ; skip to the number
        cmp al, 13
        je .num
        cmp al, '0'
        jb boost
        cmp al, '9'
        ja boost
        dec si
.num:   xor cx, cx
.d:     lodsb
        cmp al, '0'
        jb .go
        cmp al, '9'
        ja .go
        sub al, '0'
        cbw
        xchg ax, cx
        mov dx, 10
        mul dx
        add cx, ax
        jmp .d
.go:    mov ax, 5642h
        mov bx, 3
        int 2Fh
        cmp bx, 564Dh
        jne novm
        cmp ax, 0FFFFh
        jne .ok
        mov dx, msg_nomic
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h
.ok:    push ax
        mov dx, msg_boost
        mov ah, 9
        int 21h
        pop ax
        call pnum
        mov dx, msg_db
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

mic:    mov ax, 5642h                   ; reset the counters, and which input
        mov bx, 1
        int 2Fh
        cmp bx, 564Dh
        jne novm
        or ax, ax
        jnz .have
        mov dx, msg_nomic
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h
.have:  mov dx, src_jack
        cmp ax, 2
        jb .s
        mov dx, src_int
        je .s
        mov dx, src_line
.s:     mov ah, 9
        int 21h
        mov word [left], 40             ; 40 x 1/4 s
.loop:  xor ax, ax                      ; wait ~1/4 s (5 ticks)
        mov es, ax
        mov bx, [es:46Ch]
.w:     mov ah, 1
        int 16h
        jz .nk
        xor ah, ah
        int 16h
        cmp al, 27
        je .end
.nk:    mov ax, [es:46Ch]
        sub ax, bx
        cmp ax, 5
        jb .w
        mov ax, 5642h
        mov bx, 1
        int 2Fh
        add [clips], dx
        mov [resync], si
        mov ax, cx                      ; peak -> percent
        mov bx, 100
        mul bx
        mov bx, 32768
        div bx
        push ax
        mov dl, 13
        mov ah, 2
        int 21h
        pop ax
        push ax
        mov cx, 3                       ; "100% "
        call pnum3
        mov dx, msg_pct
        mov ah, 9
        int 21h
        pop ax
        shr ax, 1                       ; bar: 50 columns
        mov cx, 50
.bar:   mov dl, '#'
        or ax, ax
        jnz .b1
        mov dl, '.'
        jmp .b2
.b1:    dec ax
.b2:    push ax
        mov ah, 2
        int 21h
        pop ax
        loop .bar
        dec word [left]
        jnz .loop
.end:   mov dx, msg_clip
        mov ah, 9
        int 21h
        mov ax, [clips]
        call pnum
        mov dx, msg_rs
        mov ah, 9
        int 21h
        mov ax, [resync]
        call pnum
        mov dx, crlf
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

pnum3:  push ax                         ; AX right-aligned in 3 columns
        cmp ax, 100
        jae .n
        mov dl, ' '
        mov ah, 2
        int 21h
        pop ax
        push ax
        cmp ax, 10
        jae .n
        mov dl, ' '
        mov ah, 2
        int 21h
.n:     pop ax
pnum:   xor cx, cx                      ; AX in decimal
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

left    dw 0
clips   dw 0
resync  dw 0
pro     db 'Sound Blaster Pro 2.0 (DSP 3.02)', 13, 10
        db 'SET BLASTER=A220 I5 D1 T4 P330', 13, 10, '$'
sb16    db 'Sound Blaster 16 (DSP 4.05)', 13, 10
        db 'SET BLASTER=A220 I5 D1 H5 P330 T6', 13, 10, '$'
msg_novm db 'VMSB: not running under vmdos', 13, 10, '$'
msg_nomic db 'No microphone (it needs HD Audio analog output on the same card)', 13, 10, '$'
msg_boost db 'Mic boost $'
msg_db  db ' dB', 13, 10, '$'
src_jack db 'Mic jack. Talk; Esc stops.', 13, 10, '$'
src_int db 'Internal mic. Talk; Esc stops.', 13, 10, '$'
src_line db 'Line in. Talk; Esc stops.', 13, 10, '$'
msg_pct db '% $'
msg_clip db 13, 10, 'clipped samples: $'
msg_rs  db '   resyncs: $'
crlf    db 13, 10, '$'
