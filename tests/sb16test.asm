; SB16TEST.COM: the Sound Blaster 16 side (run VMSB 16 first).
; 1. DSP version (E1h): 04 05
; 2. records 8000 16-bit signed mono samples at 22050 Hz (DSP B8h, DMA 5)
;    and prints min/max and the mixer's IRQ status (82h) in the interrupt
; 3. plays 1 s of a 441 Hz 16-bit square wave (DSP B0h, DMA 5)
; Port 220h, IRQ 5.
        org 100h
        mov dx, 226h                    ; DSP reset
        mov al, 1
        out dx, al
        mov cx, 100
.w:     loop .w
        xor al, al
        out dx, al
        call dspin
        cmp al, 0AAh
        jne fail
        mov dx, msg_v
        call str
        mov al, 0E1h
        call dsp
        call dspin
        call hex
        call dspin
        call hex

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

        mov ax, ds                      ; buffer: physical address, word aligned
        mov dx, ax
        shr dx, 12
        shl ax, 4
        add ax, buf
        adc dx, 0
        mov [phys], ax
        mov [phys+2], dx

        mov al, 44h + 1                 ; record: channel 5, device -> memory
        call dma5
        mov al, 42h                     ; input rate 22050 (high byte first)
        call dsp
        mov al, 56h
        call dsp
        mov al, 22h
        call dsp
        mov al, 0B8h                    ; 16-bit ADC, single cycle
        call dsp
        mov al, 10h                     ; signed mono
        call dsp
        mov ax, LEN - 1
        call dsp
        mov al, ah
        call dsp
        call waitirq

        mov si, buf                     ; signed min / max
        mov cx, LEN
        mov bx, 7FFFh
        mov di, 8000h
.s:     lodsw
        cmp ax, bx
        jge .n1
        mov bx, ax
.n1:    cmp ax, di
        jle .n2
        mov di, ax
.n2:    loop .s
        mov dx, msg_mm
        call str
        mov ax, bx
        call hex16
        mov al, ' '
        call chr
        mov ax, di
        call hex16
        mov dx, msg_st
        call str
        mov al, [irqst]
        call hex

        push ds
        pop es
        mov di, buf                     ; play: square wave, 25 samples high, 25 low
        mov cx, LEN
        xor bx, bx
.f:     mov ax, 2000h
        cmp bx, 25
        jb .h
        neg ax
.h:     stosw
        inc bx
        cmp bx, 50
        jb .f2
        xor bx, bx
.f2:    loop .f
        mov al, 48h + 1                 ; play: channel 5, memory -> device
        call dma5
        mov al, 0D1h
        call dsp
        mov al, 41h
        call dsp
        mov al, 56h
        call dsp
        mov al, 22h
        call dsp
        mov byte [cnt], 3               ; three times through the buffer
.again: mov byte [done], 0
        mov al, 0B0h                    ; 16-bit DAC, single cycle
        call dsp
        mov al, 10h
        call dsp
        mov ax, LEN - 1
        call dsp
        mov al, ah
        call dsp
        call waitirq
        mov al, 48h + 1
        call dma5
        dec byte [cnt]
        jnz .again
        mov dx, msg_p
        call str

        lds dx, [old]
        mov ax, 2500h + 0Dh
        int 21h
        mov ax, 4C00h
        int 21h

fail:   mov dx, msg_f
        call str
        mov ax, 4C01h
        int 21h

dma5:   push ax                         ; AL = mode; channel 5 over buf, LEN words
        mov al, 5
        out 0D4h, al                    ; mask channel 5
        out 0D8h, al                    ; flip-flop
        pop ax
        out 0D6h, al
        mov ax, [phys]
        mov dx, [phys+2]
        shr dx, 1
        rcr ax, 1                       ; word address
        out 0C4h, al
        mov al, ah
        out 0C4h, al
        mov al, [phys+2]
        and al, 0FEh
        out 8Bh, al
        mov ax, LEN - 1
        out 0C6h, al
        mov al, ah
        out 0C6h, al
        mov al, 1
        out 0D4h, al                    ; unmask
        ret

waitirq: push es
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
        call str
.got:   pop es
        ret

isr:    push ax
        push dx
        mov dx, 224h                    ; mixer 82h: which IRQ
        mov al, 82h
        out dx, al
        inc dx
        in al, dx
        mov [cs:irqst], al
        mov dx, 22Fh                    ; acknowledge the 16-bit IRQ
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

str:    push ax
        mov ah, 9
        int 21h
        pop ax
        ret
hex16:  push ax
        mov al, ah
        call hex
        pop ax
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
msg_v   db 'DSP $'
msg_to  db ' no IRQ', 13, 10, '$'
msg_mm  db 13, 10, 'recorded min/max $'
msg_st  db '  IRQ status $'
msg_p   db 13, 10, 'played', 13, 10, '$'
done    db 0
cnt     db 0
irqst   db 0
phys    dd 0
old     dd 0
        align 16
buf:
