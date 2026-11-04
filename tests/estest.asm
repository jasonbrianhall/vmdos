; ESTEST.COM: the Ensoniq AudioPCI (ES1371; es1371 on vmdos's command line).
; 1. finds 1274:1371 through the PCI BIOS (INT 1Ah B102h), prints its
;    bus/device, I/O base (BAR0) and IRQ (3Ch)
; 2. resets the SRC, sets DAC2 to 22050 Hz, reads the AC'97 codec's
;    vendor ID (7Ch/7Eh: 8384h 7609h, SigmaTel STAC9721)
; 3. plays 2 s of a 441 Hz 16-bit stereo square wave on DAC2 (bus master,
;    loop mode), interrupt every 512 samples (expect about 86), then prints
;    the interrupt count and where the frame counter got to.
        org 100h
        cpu 386
        mov ax, 0B101h
        int 1Ah
        jc nopci
        cmp edx, 20494350h
        jne nopci
        mov ax, 0B102h
        mov cx, 1371h
        mov dx, 1274h
        xor si, si
        int 1Ah
        jc nocard
        mov [bdf], bx
        mov ax, 0B10Ah                  ; BAR0
        mov di, 10h
        int 1Ah
        and cx, 0FFC0h
        mov [io], cx
        mov ax, 0B108h                  ; interrupt line
        mov bx, [bdf]
        mov di, 3Ch
        int 1Ah
        mov [irq], cl

        mov dx, m_found
        call str
        mov ax, [bdf]
        call hex16
        mov dx, m_io
        call str
        mov ax, [io]
        call hex16
        mov dx, m_irq
        call str
        movzx ax, byte [irq]
        call dec
        call crlf

        mov eax, 00400000h              ; SRC off while its RAM is cleared
        mov dx, 10h
        call outd
        xor bp, bp
.clr:   mov ax, bp
        xor bx, bx
        call src_wr
        inc bp
        cmp bp, 128
        jb .clr
        mov al, 75h                     ; DAC2 22050 Hz: freq = 22050 * 32768 / 3000 = 3ACCDh
        mov bx, 1C00h
        call src_wr
        mov al, 77h
        mov bx, 2CCDh
        call src_wr
        mov al, 7Eh                     ; DAC2 volume L/R: 1000h = 1
        mov bx, 1000h
        call src_wr
        mov al, 7Fh
        call src_wr
        xor eax, eax
        mov dx, 10h
        call outd

        mov al, 0                       ; codec: reset, master 0 dB, PCM 0 dB
        xor bx, bx
        call codec_wr
        mov al, 2
        xor bx, bx
        call codec_wr
        mov al, 18h
        mov bx, 0808h
        call codec_wr
        mov dx, m_codec
        call str
        mov al, 7Ch
        call codec_rd
        call hex16
        mov al, ' '
        call chr
        mov al, 7Eh
        call codec_rd
        call hex16
        call crlf

        mov di, buf                     ; 1000 frames: 20 periods of 50 samples
        mov cx, 20
.per:   push cx
        mov cx, 25
        mov ax, 8000
.hi:    stosw
        stosw
        loop .hi
        mov cx, 25
        mov ax, -8000
.lo:    stosw
        stosw
        loop .lo
        pop cx
        loop .per

        mov ax, ds
        movzx eax, ax
        shl eax, 4
        add eax, buf
        mov [phys], eax
        mov eax, 0Ch                    ; memory page: DAC frames
        mov dx, 0Ch
        call outd
        mov eax, [phys]
        mov dx, 38h
        call outd
        mov eax, 999                    ; 1000 longwords, count 0
        mov dx, 3Ch
        call outd
        mov eax, 20Ch                   ; DAC2 16-bit stereo, its interrupt on
        mov dx, 20h
        call outd
        mov eax, 511                    ; an interrupt every 512 samples
        mov dx, 28h
        call outd

        movzx ax, byte [irq]            ; hook the IRQ
        mov bl, al
        add al, 8
        cmp bl, 8
        jb .v
        add al, 70h - 16
.v:     mov [vec], al
        mov ah, 35h
        int 21h
        mov [old], bx
        mov [old+2], es
        mov al, [vec]
        mov ah, 25h
        mov dx, isr
        int 21h
        mov cl, [irq]
        cmp cl, 8
        jb .m
        sub cl, 8
        mov ah, 1
        shl ah, cl
        not ah
        in al, 0A1h
        and al, ah
        out 0A1h, al
        mov cl, 2
.m:     mov ah, 1
        shl ah, cl
        not ah
        in al, 21h
        and al, ah
        out 21h, al
        sti

        mov dx, 0                       ; DAC2 on
        call ind
        or eax, 20h
        call outd
        push es
        push 40h
        pop es
        mov bx, [es:6Ch]
.wait:  mov ax, [es:6Ch]
        sub ax, bx
        cmp ax, 36
        jb .wait
        pop es
        mov eax, 0Ch
        mov dx, 0Ch
        call outd
        mov dx, 3Ch                     ; where the frame counter is
        call ind
        mov [frame], eax
        mov dx, 0
        call ind
        and eax, ~20h                   ; DAC2 off
        call outd

        cli
        push ds
        mov al, [vec]
        mov ah, 25h
        lds dx, [old]
        int 21h
        pop ds
        sti
        mov dx, m_int
        call str
        mov ax, [count]
        call dec
        mov dx, m_frame
        call str
        mov eax, [frame]
        shr eax, 16
        call dec
        call crlf
        mov ax, 4C00h
        int 21h

nopci:  mov dx, m_nopci
        jmp short fail
nocard: mov dx, m_nocard
fail:   call str
        mov ax, 4C01h
        int 21h

isr:    push eax
        push dx
        push ds
        push cs
        pop ds
        mov dx, [io]
        add dx, 4
        in eax, dx
        test al, 2
        jz .eoi
        inc word [count]
        mov dx, [io]                    ; acknowledge: the interrupt enable off, on
        add dx, 20h
        in eax, dx
        and eax, ~200h
        out dx, eax
        or eax, 200h
        out dx, eax
.eoi:   mov al, 20h
        cmp byte [irq], 8
        jb .m
        out 0A0h, al
.m:     out 20h, al
        pop ds
        pop dx
        pop eax
        iret

; port io+DX
outd:   add dx, [io]
        out dx, eax
        ret
ind:    add dx, [io]
        in eax, dx
        sub dx, [io]
        ret

src_wr: ; AL = SRC RAM address, BX = data
        movzx eax, al
        shl eax, 25
        or eax, 1000000h
        mov ax, bx
        mov dx, 10h
        jmp outd

codec_wr: ; AL = register, BX = value
        movzx eax, al
        shl eax, 16
        mov ax, bx
        mov dx, 14h
        call outd
        ret

codec_rd: ; AL = register -> AX
        movzx eax, al
        shl eax, 16
        or eax, 800000h
        mov dx, 14h
        call outd
.w:     in eax, dx
        test eax, eax
        jns .w
        ret

str:    mov ah, 9
        int 21h
        ret
chr:    mov dl, al
        mov ah, 2
        int 21h
        ret
crlf:   mov dx, m_crlf
        jmp str
hex16:  push ax
        mov al, ah
        call hex8
        pop ax
hex8:   push ax
        shr al, 4
        call hex4
        pop ax
hex4:   and al, 0Fh
        add al, '0'
        cmp al, '9'
        jbe .d
        add al, 7
.d:     jmp chr
dec:    xor cx, cx
        mov bx, 10
.d1:    xor dx, dx
        div bx
        push dx
        inc cx
        test ax, ax
        jnz .d1
.d2:    pop ax
        add al, '0'
        call chr
        loop .d2
        ret

m_nopci  db 'no PCI BIOS', 13, 10, '$'
m_nocard db 'no ES1371 (1274:1371)', 13, 10, '$'
m_found  db 'ES1371 at bus/devfn $'
m_io     db ', I/O $'
m_irq    db ', IRQ $'
m_codec  db 'AC97 vendor ID: $'
m_int    db 'interrupts in 2 s: $'
m_frame  db ', frame counter: $'
m_crlf   db 13, 10, '$'
bdf      dw 0
io       dw 0
irq      db 0
vec      db 0
count    dw 0
old      dd 0
phys     dd 0
frame    dd 0
align 4
buf:
