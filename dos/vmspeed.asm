; VMSPEED.COM: set or show how fast vmdos runs DOS, in percent of full speed.
;   VMSPEED 2      run at 2%      VMSPEED 0.5   half a percent
;   VMSPEED 0.05   five hundredths of a percent (the finest step: 0.01)
;   VMSPEED 100    full speed     VMSPEED       show the current setting
; (INT 2Fh AX=5653h, CX = 'HP', BX = hundredths of a percent or 0; returns
;  AX = hundredths of a percent, BX = 'VM'. Without CX = 'HP': permille.)
; Ctrl+Shift+F11 / Ctrl+Shift+F12 change it while a program runs.
        org 100h
        mov si, 81h
        xor bx, bx                      ; whole percent
        xor di, di                      ; digits seen
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
        cmp bx, 100
        jbe .intok
        mov bx, 100                     ; more than 100%: full speed
.intok: inc di
        lodsb
        jmp .int
.frac:  mov ax, bx                      ; percent -> hundredths
        mov dx, 100
        mul dx
        mov bx, ax
        cmp byte [si - 1], '.'
        jne .call
        lodsb                           ; tenths
        cmp al, '0'
        jb .call
        cmp al, '9'
        ja .call
        sub al, '0'
        mov ah, 10
        mul ah
        add bx, ax
        inc di
        lodsb                           ; hundredths
        cmp al, '0'
        jb .call
        cmp al, '9'
        ja .call
        sub al, '0'
        xor ah, ah
        add bx, ax
.call:  or di, di
        jnz .set
        xor bx, bx                      ; no number: just ask
.set:   cmp bx, 10000
        jbe .ok
        mov bx, 10000
.ok:    mov ax, 5653h
        mov cx, 4850h                   ; 'HP': hundredths of a percent
        int 2Fh
        cmp bx, 564Dh
        jne .novm
        push ax
        mov dx, msg
        mov ah, 9
        int 21h
        pop ax
        xor dx, dx
        mov cx, 100
        div cx                          ; AX = percent, DX = hundredths
        push dx
        call pnum
        pop dx
        or dx, dx
        jz .pct
        mov ax, dx
        mov cl, 10
        div cl                          ; AL = tenths, AH = hundredths
        push ax
        mov dl, '.'
        mov ah, 2
        int 21h
        pop ax
        push ax
        mov dl, al
        add dl, '0'
        mov ah, 2
        int 21h
        pop ax
        or ah, ah
        jz .pct
        mov dl, ah
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
pct     db '% of full (Ctrl+Shift+F11 slower, +F12 faster)', 13, 10, '$'
novm    db 'VMSPEED: not running under vmdos', 13, 10, '$'
