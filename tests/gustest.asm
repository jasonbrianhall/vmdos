; GUSTEST.COM: the Gravis UltraSound (ULTRASND=240,3,3,7,7).
; 1. DRAM: pokes AA 55 at 0 and 1 through 347h and reads them back; then
;    4Ch read back after 07h (07) and 3X3h after 3X2h <- 05h (05), as on a
;    real GF1 (Gravis's SETUP looks)
; 2. uploads 1 KB by DMA (channel 3, MSB inverted) and waits for the TC
;    IRQ (IRQ 7); prints the IRQ status (80) and DRAM 0 and 16 (40 C0)
; 3. timer 1 at 10 ms for 1 s: prints the IRQ count (about 0064)
; 4. plays the upload on voice 0, looped with a wave IRQ, for 1 s: prints
;    the loop IRQs (about 002B at 14 voices) and the position
        org 100h
GUS     equ 240h
VOICE   equ GUS + 102h
REG     equ GUS + 103h
DATALO  equ GUS + 104h
DATAHI  equ GUS + 105h
DRAMIO  equ GUS + 107h

        mov ax, 004Ch                   ; reset, then run
        call wreg8
        call delay
        mov ax, 014Ch
        call wreg8
        call delay

        xor bx, bx                      ; poke and peek
        call dramaddr
        mov dx, DRAMIO
        mov al, 0AAh
        out dx, al
        mov bx, 1
        call dramaddr
        mov dx, DRAMIO
        mov al, 55h
        out dx, al
        mov dx, msg_d
        call str
        xor bx, bx
        call peek
        mov bx, 1
        call peek

        mov ax, 074Ch                   ; run, DAC on, IRQs on
        call wreg8
        mov dx, msg_r
        call str
        mov dx, REG
        mov al, 4Ch
        out dx, al
        mov dx, DATAHI
        in al, dx
        call hex
        mov al, ' '
        call chr
        mov dx, VOICE
        mov al, 5
        out dx, al
        inc dx
        in al, dx
        call hex
        mov dx, GUS                     ; latches: DMA 3, then IRQ 7
        mov al, 0Bh
        out dx, al
        mov dx, GUS + 0Bh
        mov al, 2
        out dx, al
        mov dx, GUS
        mov al, 4Bh
        out dx, al
        mov dx, GUS + 0Bh
        mov al, 4
        out dx, al
        mov dx, GUS
        mov al, 08h                     ; line out on, latches on
        out dx, al

        mov ax, 3500h + 0Fh             ; IRQ 7 -> INT 0Fh
        int 21h
        mov [old], bx
        mov [old+2], es
        mov dx, isr
        mov ax, 2500h + 0Fh
        int 21h
        in al, 21h
        and al, ~80h
        out 21h, al

        mov di, buf                     ; 1 KB unsigned square wave, 16 high, 16 low
        xor bx, bx
.f:     mov al, 0C0h
        test bl, 16
        jz .h
        mov al, 40h
.h:     mov [di], al
        inc di
        inc bx
        cmp bx, 1024
        jb .f

        mov al, 7                       ; channel 3: mask, single, memory -> device
        out 0Ah, al
        out 0Ch, al
        mov al, 4Bh
        out 0Bh, al
        mov ax, ds
        mov dx, ax
        shr dx, 12
        shl ax, 4
        add ax, buf
        adc dx, 0
        out 06h, al
        mov al, ah
        out 06h, al
        mov al, dl
        out 82h, al
        mov ax, 1023
        out 07h, al
        mov al, ah
        out 07h, al
        mov al, 3
        out 0Ah, al                     ; unmask

        mov al, 42h                     ; DRAM 0
        xor bx, bx
        call wreg16
        mov byte [done], 0
        mov ax, 0A141h                  ; go, IRQ at the end, invert the MSB
        call wreg8
        call waitirq
        mov dx, msg_i
        call str
        mov al, [irqst]
        call hex
        mov dx, msg_m
        call str
        xor bx, bx
        call peek
        mov bx, 16
        call peek

        mov ax, (256 - 125) * 256 + 46h ; timer 1: 125 x 80 us
        call wreg8
        mov ax, 0445h
        call wreg8
        mov dx, GUS + 8
        mov al, 4
        out dx, al
        inc dx
        mov al, 1
        out dx, al
        mov word [tcount], 0
        call second
        mov dx, GUS + 9
        xor al, al
        out dx, al
        mov ax, 0045h
        call wreg8
        mov dx, msg_t
        call str
        mov ax, [tcount]
        call hex16

        mov ax, 0CD0Eh                  ; 14 voices
        call wreg8
        mov dx, VOICE
        xor al, al
        out dx, al
        mov ax, 0300h                   ; stopped while it's set up
        call wreg8
        mov ax, 030Dh
        call wreg8
        mov al, 01h                     ; a quarter of the GF1's rate
        mov bx, 256
        call wreg16
        mov al, 02h                     ; start 0
        xor bx, bx
        call wreg16
        mov al, 03h
        call wreg16
        mov al, 0Ah                     ; position 0
        call wreg16
        mov al, 0Bh
        call wreg16
        mov al, 04h                     ; end 1023
        mov bx, 1023 >> 7
        call wreg16
        mov al, 05h
        mov bx, (1023 & 127) << 9
        call wreg16
        mov al, 09h
        mov bx, 0F000h
        call wreg16
        mov ax, 070Ch
        call wreg8
        mov word [wcount], 0
        mov ax, 2800h                   ; play: loop, wave IRQ
        call wreg8
        call second
        mov ax, 0300h
        call wreg8
        mov al, 09h
        xor bx, bx
        call wreg16
        mov dx, msg_w
        call str
        mov ax, [wcount]
        call hex16
        mov dx, msg_p
        call str
        mov dx, REG
        mov al, 8Ah
        out dx, al
        mov dx, DATALO
        in ax, dx
        call hex16
        mov dx, REG
        mov al, 8Bh
        out dx, al
        mov dx, DATALO
        in ax, dx
        call hex16
        mov dx, msg_nl
        call str

        in al, 21h
        or al, 80h
        out 21h, al
        lds dx, [old]
        mov ax, 2500h + 0Fh
        int 21h
        mov ax, 4C00h
        int 21h

isr:    push ax
        push dx
        mov dx, GUS + 6
        in al, dx
        or [cs:irqst], al
        test al, 80h
        jz .nd
        push ax
        mov dx, REG                     ; reading 41h acknowledges the DMA
        mov al, 41h
        out dx, al
        mov dx, DATAHI
        in al, dx
        mov byte [cs:done], 1
        pop ax
.nd:    test al, 04h
        jz .nt
        push ax
        mov dx, REG                     ; timer 1: off and on again
        mov al, 45h
        out dx, al
        mov dx, DATAHI
        xor al, al
        out dx, al
        mov al, 4
        out dx, al
        inc word [cs:tcount]
        pop ax
.nt:    test al, 60h
        jz .nv
.vl:    mov dx, REG                     ; voices: 8Fh until none is left
        mov al, 8Fh
        out dx, al
        mov dx, DATAHI
        in al, dx
        mov ah, al
        and ah, 0C0h
        cmp ah, 0C0h
        je .nv
        test al, 80h
        jnz .vl
        inc word [cs:wcount]
        jmp .vl
.nv:    mov al, 20h
        out 20h, al
        pop dx
        pop ax
        iret

wreg8:  push dx                         ; AL = register, AH = value
        mov dx, REG
        out dx, al
        mov dx, DATAHI
        mov al, ah
        out dx, al
        pop dx
        ret

wreg16: push dx                         ; AL = register, BX = value
        mov dx, REG
        out dx, al
        mov dx, DATALO
        mov ax, bx
        out dx, ax
        pop dx
        ret

dramaddr: mov al, 43h                   ; BX = DRAM address (below 64 KB)
        call wreg16
        mov ax, 0044h
        jmp wreg8

peek:   call dramaddr                   ; prints DRAM[BX]
        mov dx, DRAMIO
        in al, dx
        call hex
        mov al, ' '
        jmp chr

delay:  mov cx, 7                       ; a few us: 7 port reads
        mov dx, GUS + 6
.d:     in al, dx
        loop .d
        ret

second: mov cx, 18                      ; about 1 s of BIOS ticks
        push es
        xor ax, ax
        mov es, ax
.s1:    mov bx, [es:46Ch]
.s2:    cmp bx, [es:46Ch]
        je .s2
        loop .s1
        pop es
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

str:    mov ah, 9
        int 21h
        ret

hex16:  push ax
        mov al, ah
        call hex
        pop ax
hex:    push ax
        shr al, 4
        call nib
        pop ax
nib:    and al, 0Fh
        add al, '0'
        cmp al, '9'
        jbe chr
        add al, 7
chr:    push dx
        mov dl, al
        mov ah, 2
        int 21h
        pop dx
        ret

msg_d   db 'DRAM: $'
msg_r   db 13, 10, '4Ch, 3X3h: $'
msg_i   db 13, 10, 'DMA IRQ status: $'
msg_m   db ' DRAM: $'
msg_t   db 13, 10, 'Timer 1 IRQs in 1 s: $'
msg_w   db 13, 10, 'Voice 0 loop IRQs in 1 s: $'
msg_p   db ' position: $'
msg_to  db ' (no IRQ) $'
msg_nl  db 13, 10, '$'
old     dd 0
done    db 0
irqst   db 0
tcount  dw 0
wcount  dw 0
buf:    times 1024 db 0
