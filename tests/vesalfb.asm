; VESALFB.COM: a 16-bit DPMI client draws through the VESA linear
; framebuffer: 4F01h for PhysBasePtr, 4F02h with the LFB bit, DPMI 0800h to
; map it, a selector over it, then 480 lines of colour (y & 255) with
; 32-bit addressing. Waits about 3 s, then returns to text mode.
        org 100h
        mov bx, 1000h
        mov ah, 4Ah
        int 21h
        mov ax, 4F01h
        mov cx, 101h
        mov di, minfo
        int 10h
        cmp ax, 004Fh
        jne quit
        mov ax, 4F02h
        mov bx, 4101h                   ; 640x480x8, linear framebuffer
        int 10h
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
        ; --- protected mode ---
        mov bx, [minfo + 2Ah]           ; PhysBasePtr high
        mov cx, [minfo + 28h]           ; low
        mov si, 4                       ; 4 MB
        xor di, di
        mov ax, 0800h
        int 31h
        jc quit
        mov [lin], cx
        mov [lin + 2], bx
        xor ax, ax                      ; a descriptor
        mov cx, 1
        int 31h
        jc quit
        mov [sel], ax
        mov bx, ax
        mov cx, [lin + 2]
        mov dx, [lin]
        mov ax, 0007h
        int 31h
        mov bx, [sel]
        mov cx, 0004h                   ; limit 4 MB - 1
        mov dx, 0FFFFh
        mov ax, 0008h
        int 31h
        mov es, [sel]
        xor edi, edi
        xor bx, bx                      ; y
.row:   mov al, bl
        mov ecx, 640
        a32 rep stosb
        inc bx
        cmp bx, 480
        jb .row
        mov ax, 40h
        mov fs, ax
        mov bx, [fs:6Ch]
        add bx, 55
.w:     sti
        cmp bx, [fs:6Ch]
        jne .w
        mov ax, 0003h
        int 10h
quit:   mov ax, 4C00h
        int 21h
entry   dd 0
lin     dd 0
sel     dw 0
minfo   times 256 db 0
