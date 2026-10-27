; DACDIR.COM: Sound Blaster direct DAC (DSP 10h) from a fast timer, the way
; Creative's Parrot (VOICE.DRV) plays: the PIT at 8000 Hz, and each IRQ 0
; writes one sample of a 440 Hz sine. Runs 3 s (24000 IRQs), then puts the
; timer back. On a working emulator: a clean 440 Hz tone lasting 3 s.
        org 100h
        mov ax, 3508h
        int 21h
        mov [old8], bx
        mov [old8+2], es
        mov dx, 22Ch
        call wr
        mov al, 0D1h                ; speaker on
        out dx, al
        cli
        mov ax, 2508h
        mov dx, isr
        int 21h
        mov al, 36h
        out 43h, al
        mov ax, 149                 ; 1193182 / 8000
        out 40h, al
        mov al, ah
        out 40h, al
        sti
.wait:  cmp word [count], 24000
        jb .wait
        cli
        mov al, 36h
        out 43h, al
        xor al, al
        out 40h, al
        out 40h, al
        push ds
        lds dx, [old8]
        mov ax, 2508h
        int 21h
        pop ds
        sti
        mov dx, 22Ch
        call wr
        mov al, 10h
        out dx, al
        call wr
        mov al, 80h
        out dx, al
        mov ax, 4C00h
        int 21h

wr:     in al, dx
        test al, 80h
        jnz wr
        ret

isr:    push ax
        push bx
        push dx
        push ds
        push cs
        pop ds
        mov dx, 22Ch
        call wr
        mov al, 10h
        out dx, al
        call wr
        mov bx, [phase]
        add word [phase], 3604      ; 440 Hz at 8000 Hz
        mov bl, bh
        xor bh, bh
        mov al, [sine + bx]
        out dx, al
        inc word [count]
        mov al, 20h
        out 20h, al
        pop ds
        pop dx
        pop bx
        pop ax
        iret

old8    dd 0
phase   dw 0
count   dw 0
sine:
        db 128,130,133,135,138,140,143,145,148,150,152,155,157,159,162,164
        db 166,169,171,173,175,177,179,181,184,186,188,190,191,193,195,197
        db 199,200,202,204,205,207,208,210,211,212,214,215,216,217,218,219
        db 220,221,222,223,224,224,225,226,226,227,227,227,228,228,228,228
        db 228,228,228,228,228,227,227,227,226,226,225,224,224,223,222,221
        db 220,219,218,217,216,215,214,212,211,210,208,207,205,204,202,200
        db 199,197,195,193,191,190,188,186,184,181,179,177,175,173,171,169
        db 166,164,162,159,157,155,152,150,148,145,143,140,138,135,133,130
        db 128,126,123,121,118,116,113,111,108,106,104,101,99,97,94,92
        db 90,87,85,83,81,79,77,75,72,70,68,66,65,63,61,59
        db 57,56,54,52,51,49,48,46,45,44,42,41,40,39,38,37
        db 36,35,34,33,32,32,31,30,30,29,29,29,28,28,28,28
        db 28,28,28,28,28,29,29,29,30,30,31,32,32,33,34,35
        db 36,37,38,39,40,41,42,44,45,46,48,49,51,52,54,56
        db 57,59,61,63,65,66,68,70,72,75,77,79,81,83,85,87
        db 90,92,94,97,99,101,104,106,108,111,113,116,118,121,123,126
