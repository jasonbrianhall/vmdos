; M12X.COM: a 256-colour unchained screen made from BIOS mode 12h, as
; Ultrabots does: INT 10h AX=0012h, then the attribute controller and
; graphics controller switched to 256 colours and the CRTC to 320x240
; (double-scanned), chain-4 left off. Shows 16 vertical bars of 16
; shades each (all 256 colours) over the whole 320x240, a white frame
; round the edge. Stripes in a few dark colours: drawn as 16-colour.
        org 100h
        mov ax, 0012h
        int 10h
        cli
        mov dx, 3DAh
        in al, dx                   ; attribute controller to index
        mov dx, 3C0h
        mov al, 30h                 ; mode control, palette on
        out dx, al
        mov al, 41h                 ; graphics, 8 bits a pixel
        out dx, al
        mov al, 33h                 ; pel panning 0
        out dx, al
        xor al, al
        out dx, al
        mov dx, 3CEh
        mov ax, 4005h               ; 256-colour shift
        out dx, ax
        mov dx, 3D4h
        mov ax, 0011h               ; unprotect CRTC 0-7
        out dx, ax
        mov ax, 4109h               ; 2 scan lines a row: 480 -> 240
        out dx, ax
        mov ax, 2813h               ; offset: 80 bytes a line a plane
        out dx, ax
        mov ax, 0014h               ; no dword mode
        out dx, ax
        mov ax, 0E317h              ; byte mode
        out dx, ax
        mov ax, 4F01h               ; 80 character clocks: 320 pixels
        out dx, ax
        sti

        push word 0A000h
        pop es
        xor bx, bx                  ; y
.row:   xor si, si                  ; x
.px:    mov ax, si
        mov cl, 4                   ; colour: (x / 20) * 16 + y / 15
        xor dx, dx
        mov di, 20
        div di
        shl al, cl
        mov bp, ax
        mov ax, bx
        mov di, 15
        xor dx, dx
        div di
        or ax, bp
        mov ah, al
        cmp si, 0
        je .edge
        cmp si, 319
        je .edge
        cmp bx, 0
        je .edge
        cmp bx, 239
        jne .put
.edge:  mov ah, 15
.put:   push ax
        mov cx, si                  ; plane x & 3
        and cl, 3
        mov ax, 0102h
        shl ah, cl
        mov dx, 3C4h
        out dx, ax
        pop ax
        mov di, bx                  ; offset y * 80 + x / 4
        imul di, di, 80
        mov dx, si
        shr dx, 2
        add di, dx
        mov [es:di], ah
        inc si
        cmp si, 320
        jb .px
        inc bx
        cmp bx, 240
        jb .row

        xor ax, ax
        int 16h
        mov ax, 3
        int 10h
        ret
