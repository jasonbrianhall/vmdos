;; ADLIBDIG.COM: AdLib digitized sound, the way Silent Service II's ASOUND.SS
; plays "Dive! Dive!": an OPL2 tone keyed on, its frequency set to 0 after
; ~2 ms (the phase frozen: the operator puts out a steady level), then each
; sample written as the carrier's total level (register 43h), timed by
; polling PIT channel 2 (mode 2, count 150: 7954 Hz) for its wrap. 3 s of
; a 440 Hz sine. On real hardware or a working emulator: a (quiet) 440 Hz
; tone, not a buzz.
        org 100h
        mov si, setup
.set:   lodsw
        cmp al, 0FFh
        je .go
        call opl
        jmp .set
.go:    mov al, 0B6h                ; ~2 ms of the tone (channel 2 as a clock)
        out 43h, al
        xor al, al
        out 42h, al
        out 42h, al
        in al, 61h
        or al, 1
        and al, 0FDh
        out 61h, al
        mov cx, 20                  ; 20 x 128 clocks (mode 3 counts by 2)
.run:   call wrap
        loop .run
        mov ax, 20B0h               ; key on, frequency 0: the phase stops
        call opl
        mov ax, 00A0h
        call opl
        mov al, 94h                 ; channel 2: LSB only, mode 2, count 150
        out 43h, al
        mov al, 150
        out 42h, al
        cli
        mov cx, 23862               ; 3 s at 7954 Hz
        xor bp, bp
.smp:   call wrap
        mov bx, bp
        add bp, 3624                ; 440 Hz at 7954 Hz
        mov bl, bh
        xor bh, bh
        mov bl, [sine + bx]
        shr bl, 2
        mov ah, [level + bx]
        mov al, 43h
        call opl
        loop .smp
        sti
        mov ax, 00B0h               ; key off
        call opl
        in al, 61h
        and al, 0FCh
        out 61h, al
        mov ax, 4C00h
        int 21h

; Wait for channel 2's count (LSB) to wrap.
wrap:   mov ah, 0FFh
.l:     mov al, 80h
        out 43h, al
        in al, 42h
        cmp ah, al
        mov ah, al
        jnc .l
        ret

; OPL register AL <- AH
opl:    push dx
        push cx
        mov dx, 388h
        out dx, al
        mov cx, 6
.d1:    in al, dx
        loop .d1
        inc dx
        mov al, ah
        out dx, al
        dec dx
        mov cx, 35
.d2:    in al, dx
        loop .d2
        pop cx
        pop dx
        ret

setup:  db 20h,23h, 23h,28h, 40h,3Fh, 43h,3Fh, 60h,0AFh, 63h,0AFh, 80h,0Dh, 83h,0Fh
        db 0C0h,05h, 0E0h,0, 0E3h,2, 0B0h,1, 0A0h,8Fh, 0B0h,2Eh, 0FFh,0
level:  db 3Fh,2Dh,26h,23h,20h,1Dh,1Bh,19h,18h,16h,15h,14h,13h,12h,11h,10h
        db 10h,0Fh,0Eh,0Eh,0Dh,0Dh,0Ch,0Ch,0Bh,0Bh,0Ah,0Ah,09h,09h,09h,08h
        db 08h,07h,07h,07h,06h,06h,06h,06h,05h,05h,05h,04h,04h,04h,04h,03h
        db 03h,03h,03h,02h,02h,02h,02h,02h,01h,01h,01h,01h,00h,00h,00h,00h
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
