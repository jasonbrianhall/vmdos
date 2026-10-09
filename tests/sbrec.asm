; SBREC.COM: records 8000 bytes from the Sound Blaster (DSP 24h, 8-bit
; single-cycle ADC DMA at 8 kHz, DMA 1, IRQ 5, port 220h) and prints the
; smallest and largest sample and how many differ from the first: a silent
; mic gives about 80 80, a signal a wide range. Then DSP 20h (direct ADC) x8.
        org 100h
        mov dx, 226h                    ; DSP reset
        mov al, 1
        out dx, al
        mov cx, 100
.w:     loop .w
        xor al, al
        out dx, al
        mov dx, 22Eh
.rd:    in al, dx
        test al, 80h
        jz .rd
        mov dx, 22Ah
        in al, dx
        cmp al, 0AAh
        jne fail

        mov ax, 3500h + 0Dh             ; IRQ 5 -> INT 0Dh
        int 21h
        mov [old], bx
        mov [old+2], es
        mov dx, isr
        mov ax, 2500h + 0Dh
        int 21h
        in al, 21h
        and al, ~20h
        out 21h, al

        mov ax, ds                      ; buffer's physical address
        mov dx, ax
        shr dx, 12
        shl ax, 4
        add ax, buf
        adc dx, 0
        mov [page], dl
        mov bx, ax
        mov al, 5                       ; mask channel 1
        out 0Ah, al
        out 0Ch, al
        mov al, 45h                     ; single, write to memory (device -> memory), ch 1
        out 0Bh, al
        mov al, bl
        out 02h, al
        mov al, bh
        out 02h, al
        mov al, [page]
        out 83h, al
        mov ax, LEN - 1
        out 03h, al
        mov al, ah
        out 03h, al
        mov al, 1
        out 0Ah, al                     ; unmask

        mov al, 40h                     ; time constant: 8 kHz (256 - 125)
        call dsp
        mov al, 131
        call dsp
        mov al, 24h
        call dsp
        mov ax, LEN - 1
        call dsp
        mov al, ah
        call dsp

        xor ax, ax                      ; wait for the IRQ (or ~5 s)
        mov es, ax
        mov bx, [es:46Ch]
.wi:    cmp byte [done], 0
        jne .got
        mov ax, [es:46Ch]
        sub ax, bx
        cmp ax, 91
        jb .wi
        mov dx, msg_to
        mov ah, 9
        int 21h
.got:   mov si, buf                     ; min, max, differing
        mov cx, LEN
        mov bl, 0FFh
        mov bh, 0
        xor di, di
        mov ah, [si]
.s:     lodsb
        cmp al, bl
        jae .n1
        mov bl, al
.n1:    cmp al, bh
        jbe .n2
        mov bh, al
.n2:    cmp al, ah
        je .n3
        inc di
.n3:    loop .s
        mov dx, msg_mm
        mov ah, 9
        int 21h
        mov al, bl
        call hex
        mov al, ' '
        call chr
        mov al, bh
        call hex
        mov dx, msg_d
        mov ah, 9
        int 21h
        mov ax, di
        xchg al, ah
        call hex
        mov al, ah
        call hex
        mov dx, msg_20
        mov ah, 9
        int 21h
        mov cx, 8
.d20:   mov al, 20h
        call dsp
        call dspin
        call hex
        mov al, ' '
        call chr
        loop .d20

        lds dx, [old]
        mov ax, 2500h + 0Dh
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
        mov dx, 22Eh                    ; acknowledge
        in al, dx
        mov byte [cs:done], 1
        mov al, 20h
        out 20h, al
        pop dx
        pop ax
        iret

dsp:    push dx                         ; AL to the DSP
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

hex:    push ax
        shr al, 4
        call nib
        pop ax
        push ax
        and al, 0Fh
        call nib
        pop ax
        ret
nib:    add al, '0'
        cmp al, '9'
        jbe chr
        add al, 7
chr:    push ax
        push dx
        mov dl, al
        mov ah, 2
        int 21h
        pop dx
        pop ax
        ret

LEN     equ 8000
msg_f   db 'no Sound Blaster', 13, 10, '$'
msg_to  db 'no IRQ', 13, 10, '$'
msg_mm  db 'min/max $'
msg_d   db '  differing $'
msg_20  db 13, 10, 'direct: $'
done    db 0
page    db 0
old     dd 0
        align 16
buf:
