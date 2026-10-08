; VMSB.COM: which Sound Blaster vmdos shows DOS programs.
;   VMSB 16     a Sound Blaster 16 (DSP 4.05, 16-bit sound on DMA 5)
;   VMSB PRO    a Sound Blaster Pro 2.0 (DSP 3.02; the default)
;   VMSB        show which
; Prints the BLASTER line to match; exits with errorlevel 16 or 2.
; (INT 2Fh AX=5642h, BX = 0 ask / 2 / 16; returns AX = 2 or 16, BX = 'VM'.)
        org 100h
        mov si, 81h
        xor bx, bx
.skip:  lodsb
        cmp al, ' '
        je .skip
        cmp al, 9
        je .skip
        cmp al, '1'
        jne .p
        mov bx, 16
        jmp .call
.p:     or al, 20h
        cmp al, 'p'
        jne .call
        mov bx, 2
.call:  mov ax, 5642h
        int 2Fh
        cmp bx, 564Dh
        jne .novm
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
.novm:  mov dx, novm
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

pro     db 'Sound Blaster Pro 2.0 (DSP 3.02)', 13, 10
        db 'SET BLASTER=A220 I5 D1 T4 P330', 13, 10, '$'
sb16    db 'Sound Blaster 16 (DSP 4.05)', 13, 10
        db 'SET BLASTER=A220 I5 D1 H5 P330 T6', 13, 10, '$'
novm    db 'VMSB: not running under vmdos', 13, 10, '$'
