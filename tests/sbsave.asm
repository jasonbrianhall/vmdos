; SBSAVE.COM: records about 3 s from the Sound Blaster the way voice
; programs do (8-bit, 11025 Hz, auto-init DMA over a 16 KiB buffer, two
; 8 KiB halves, an IRQ per half) and writes it to C:\REC.RAW (unsigned
; 8-bit mono), to check by ear or with a script. Port 220h, IRQ 5, DMA 1.
        org 100h
        mov dx, 226h
        mov al, 1
        out dx, al
        mov cx, 100
.w:     loop .w
        xor al, al
        out dx, al
        call dspin
        cmp al, 0AAh
        jne fail

        mov ax, 3500h + 0Dh
        int 21h
        mov [old], bx
        mov [old+2], es
        mov dx, isr
        mov ax, 2500h + 0Dh
        int 21h
        in al, 21h
        and al, ~20h
        out 21h, al

        mov ah, 3Ch                     ; create C:\REC.RAW
        xor cx, cx
        mov dx, fname
        int 21h
        jc fail
        mov [fh], ax

        mov ax, ds                      ; buffer's physical address (vmdos doesn't
        mov dx, ax                      ; mind a 64 KiB crossing; a real card would)
        shr dx, 12
        shl ax, 4
        add ax, buf
        adc dx, 0
        mov word [bufoff], buf
.prog:  mov bx, ax
        mov al, 5
        out 0Ah, al
        out 0Ch, al
        mov al, 55h                     ; auto-init, write to memory, ch 1
        out 0Bh, al
        mov al, bl
        out 02h, al
        mov al, bh
        out 02h, al
        mov al, dl
        out 83h, al
        mov ax, HALF * 2 - 1
        out 03h, al
        mov al, ah
        out 03h, al
        mov al, 1
        out 0Ah, al

        mov al, 40h                     ; 11025 Hz: 256 - 1000000/11025 = 165
        call dsp
        mov al, 165
        call dsp
        mov al, 48h                     ; block = one half
        call dsp
        mov ax, HALF - 1
        call dsp
        mov al, ah
        call dsp
        mov al, 2Ch                     ; auto-init ADC
        call dsp

.next:  xor ax, ax                      ; wait for a half (or ~3 s)
        mov es, ax
        mov bx, [es:46Ch]
.wi:    cmp byte [done], 0
        jne .got
        mov ax, [es:46Ch]
        sub ax, bx
        cmp ax, 55
        jb .wi
        mov dx, msg_to
        mov ah, 9
        int 21h
        jmp .stop
.got:   mov byte [done], 0
        mov dx, [bufoff]                ; the half just filled
        test byte [half], 1
        jz .h0
        add dx, HALF
.h0:    inc byte [half]
        mov ah, 40h
        mov bx, [fh]
        mov cx, HALF
        int 21h
        dec byte [count]
        jnz .next
.stop:  mov al, 0DAh                    ; exit auto-init
        call dsp
        mov al, 0D0h
        call dsp
        mov ah, 3Eh
        mov bx, [fh]
        int 21h
        lds dx, [cs:old]
        mov ax, 2500h + 0Dh
        int 21h
        push cs
        pop ds
        mov dx, msg_ok
        mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

fail:   mov dx, msg_f
        mov ah, 9
        int 21h
        mov ax, 4C01h
        int 21h

isr:    push ax
        push dx
        mov dx, 22Eh
        in al, dx
        mov byte [cs:done], 1
        mov al, 20h
        out 20h, al
        pop dx
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
dspin:  push dx
        mov dx, 22Eh
.b:     in al, dx
        test al, 80h
        jz .b
        mov dx, 22Ah
        in al, dx
        pop dx
        ret

HALF    equ 8192
fname   db 'C:\REC.RAW', 0
msg_f   db 'failed', 13, 10, '$'
msg_to  db 'no IRQ', 13, 10, '$'
msg_ok  db 'C:\REC.RAW written', 13, 10, '$'
done    db 0
half    db 0
count   db 4                            ; 4 halves: 32 KiB, ~3 s
fh      dw 0
bufoff  dw 0
old     dd 0
        align 16
buf:
