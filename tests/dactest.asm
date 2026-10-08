; DACTEST.COM: speech-style playback through the direct DAC (DSP 10h),
; one sample per timer interrupt at 10 kHz (PIT channel 0 reprogrammed),
; as the old talking programs do: 2 s of a 500 Hz triangle (20 samples per
; cycle). Port 220h. Restores the 18.2 Hz timer and INT 8 at the end.
        org 100h
        mov dx, 226h
        mov al, 1
        out dx, al
        mov cx, 100
.w:     loop .w
        xor al, al
        out dx, al
        mov al, 0D1h                    ; speaker on
        call dsp
        mov ax, 3508h
        int 21h
        mov [old], bx
        mov [old+2], es
        mov dx, isr
        mov ax, 2508h
        int 21h
        cli
        mov al, 36h                     ; channel 0: 10 kHz
        out 43h, al
        mov ax, 119
        out 40h, al
        mov al, ah
        out 40h, al
        sti
.wait:  cmp word [left], 0
        jne .wait
        cli
        mov al, 36h
        out 43h, al
        xor al, al
        out 40h, al
        out 40h, al
        sti
        lds dx, [cs:old]
        mov ax, 2508h
        int 21h
        push cs
        pop ds
        mov dx, msg
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

isr:    push ax
        push bx
        push dx
        cmp word [cs:left], 0
        je .eoi
        dec word [cs:left]
        mov bx, [cs:ph]                 ; triangle: 0..9 up, 10..19 down
        mov al, bl
        cmp bl, 10
        jb .up
        mov al, 19
        sub al, bl
.up:    mov ah, 20                      ; 10 steps of 20 around 128
        mul ah
        add al, 28
        push ax
        mov al, 10h
        call dsp
        pop ax
        call dsp
        inc bx
        cmp bx, 20
        jb .s
        xor bx, bx
.s:     mov [cs:ph], bx
.eoi:   mov al, 20h
        out 20h, al
        pop dx
        pop bx
        pop ax
        iret

dsp:    push dx
        push ax
        mov dx, 22Ch
.b:     in al, dx
        test al, 80h
        jnz .b
        pop ax
        out dx, al
        pop dx
        ret

left    dw 20000
ph      dw 0
old     dd 0
msg     db 'done', 13, 10, '$'
