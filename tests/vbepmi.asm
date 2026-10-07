; VBEPMI.COM: the VBE 2.0 protected-mode interface (4F0Ah). Sets 640x480x8,
; copies the interface table into its own segment, enters protected mode
; (16-bit DPMI client) and calls the copied SetPalette routine to make
; colour 0 red: the whole screen turns red for about 2 seconds.
        org 100h
        mov bx, 1000h
        mov ah, 4Ah
        int 21h
        mov ax, 4F02h
        mov bx, 101h
        int 10h
        mov ax, 4F0Ah
        xor bx, bx
        int 10h
        cmp ax, 004Fh
        jne quit
        push ds
        push es
        pop ds
        mov si, di
        push cs
        pop es
        mov di, pmi
        rep movsb                       ; copy the table (CX bytes)
        pop ds
        mov ax, 1687h
        int 2Fh
        or ax, ax
        jnz quit
        mov [entry], di
        mov [entry + 2], es
        mov bx, si
        mov ah, 48h
        int 21h
        jc quit
        mov es, ax
        xor ax, ax
        call far [entry]
        jc quit
        ; --- protected mode: SetPalette(CX=1, DX=0, ES:EDI = red) ---
        push ds
        pop es
        mov edi, red
        mov cx, 1
        xor dx, dx
        xor bx, bx
        mov ax, pmi
        add ax, [pmi + 4]
        call ax
        mov ax, 40h
        mov fs, ax
        mov bx, [fs:6Ch]
        add bx, 36
.w:     sti
        cmp bx, [fs:6Ch]
        jne .w
quit:   mov ax, 4C00h
        int 21h
red     db 0, 0, 63, 0                  ; B, G, R, pad
entry   dd 0
pmi     times 64 db 0
