; SEL40.COM: a 16-bit DPMI client loads selector 0040h (the BIOS data area,
; which DOS extenders' programs such as Warcraft II's setup expect) and
; reads the BIOS tick count through it. Prints "SEL40 OK" or a failure.
        org 100h
        mov bx, 1000h                   ; keep 64 KB, free the rest
        mov ah, 4Ah
        int 21h
        mov ax, 1687h
        int 2Fh
        or ax, ax
        jnz nodpmi
        mov [entry], di
        mov [entry + 2], es
        ; host data area
        mov ax, si
        or ax, ax
        jz .nodata
        mov bx, si
        mov ah, 48h
        int 21h
        jc nodpmi
        mov es, ax
.nodata:
        xor ax, ax                      ; 16-bit client
        call far [entry]
        jc nodpmi
        ; protected mode now, DS = our data selector
        mov ax, 40h
        mov es, ax                      ; #GP here without the descriptor
        mov ax, [es:6Ch]
        mov ax, 0002h                   ; selector for B800h: write to the screen
        mov bx, 0B800h                  ; (INT 21h can't take a DS selector here)
        int 31h
        mov es, ax
        mov si, okmsg
        mov di, 160 * 24
.p:     lodsb
        cmp al, '$'
        je .x
        mov ah, 0Eh
        stosw
        jmp .p
.x:     mov ax, 4C00h
        int 21h
nodpmi: mov dx, failmsg
done:   mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h
entry   dd 0
okmsg   db 'SEL40 OK$'
failmsg db 'SEL40: no DPMI', 13, 10, '$'
