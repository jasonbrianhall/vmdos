; VMSPEED.COM: set or show how fast vmdos runs DOS, in percent of full speed.
;   VMSPEED 2      run at 2%      VMSPEED 0.5   half a percent
;   VMSPEED 100    full speed     VMSPEED       show the current setting
; (INT 2Fh AX=5653h, BX = permille or 0; returns AX = permille, BX = 'VM'.)
; Ctrl+F11 / Ctrl+F12 change it while a program runs.
        org 100h
        mov si, 81h
        xor bx, bx                      ; permille
        xor cx, cx                      ; digits seen
.skip:  lodsb
        cmp al, ' '
        je .skip
        cmp al, 9
        je .skip
.int:   cmp al, '0'
        jb .frac
        cmp al, '9'
        ja .frac
        sub al, '0'
        cbw
        xchg ax, bx
        mov dx, 10
        mul dx
        add bx, ax
        inc cx
        lodsb
        jmp .int
.frac:  mov ax, bx                      ; percent -> permille
        mov dx, 10
        mul dx
        mov bx, ax
        cmp byte [si - 1], '.'
        jne .call
        lodsb
        cmp al, '0'
        jb .call
        cmp al, '9'
        ja .call
        sub al, '0'
        cbw
        add bx, ax
        inc cx
.call:  or cx, cx
        jnz .set
        xor bx, bx                      ; no number: just ask
.set:   cmp bx, 1000
        jbe .ok
        mov bx, 1000
.ok:    mov ax, 5653h
        int 2Fh
        cmp bx, 564Dh
        jne .novm
        push ax
        mov dx, msg
        mov ah, 9
        int 21h
        pop ax
        xor dx, dx
        mov cx, 10
        div cx                          ; AX = percent, DX = tenths
        push dx
        call pnum
        pop dx
        or dx, dx
        jz .pct
        push dx
        mov dl, '.'
        mov ah, 2
        int 21h
        pop dx
        add dl, '0'
        mov ah, 2
        int 21h
.pct:   mov dx, pct
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h
.novm:  mov dx, novm
        mov ah, 9
        int 21h
        mov ax, 4C01h
        int 21h

pnum:   xor cx, cx                      ; print AX in decimal
.d:     xor dx, dx
        mov bx, 10
        div bx
        push dx
        inc cx
        or ax, ax
        jnz .d
.o:     pop dx
        add dl, '0'
        mov ah, 2
        int 21h
        loop .o
        ret

msg     db 'vmdos speed: $'
pct     db '% of full (Ctrl+F11 slower, Ctrl+F12 faster)', 13, 10, '$'
novm    db 'VMSPEED: not running under vmdos', 13, 10, '$'
