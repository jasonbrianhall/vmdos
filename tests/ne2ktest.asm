; NE2KTEST.COM: drives an NE2000 at 300h, IRQ 11 the way the classic
; drivers (Crynwr, Linux ne.c) do: reset, PROM read by remote DMA, ring at
; 46h-80h, then sends a broadcast "hello from ne2k" (EtherType 88B5h) and
; for ~10 s prints the 88B5h frames that come in, counting interrupts.
; Runs the same on QEMU's ne2k_isa and on vmdos's emulation.
        cpu 286
        org 100h
BASE    equ 300h
IRQV    equ 73h                         ; IRQ 11
PSTART  equ 46h
PSTOP   equ 80h

%macro outp 2                           ; outp reg, value
        mov dx, BASE + %1
        mov al, %2
        out dx, al
%endmacro
%macro inp 1
        mov dx, BASE + %1
        in al, dx
%endmacro

        inp 0
        cmp al, 0FFh
        jne .there
        mov dx, m_none
        jmp die
.there: inp 1Fh                         ; reset
        out dx, al
        xor cx, cx
.rst:   inp 7
        test al, 80h
        jnz .rok
        loop .rst
        mov dx, m_noreset
        jmp die
.rok:   outp 0, 21h                     ; page 0, stop, no DMA
        outp 0Eh, 49h                   ; DCR: word, normal, FIFO 8
        outp 0Ah, 0
        outp 0Bh, 0
        outp 0Ch, 20h                   ; RCR: monitor
        outp 0Dh, 02h                   ; TCR: loopback
        outp 0Fh, 0
        outp 7, 0FFh
        ; the PROM: 32 bytes from 0
        outp 8, 0
        outp 9, 0
        outp 0Ah, 32
        outp 0Bh, 0
        outp 0, 0Ah                     ; remote read, start
        mov dx, BASE + 10h
        mov di, prom
        push cs
        pop es
        mov cx, 16
        rep insw
        mov si, prom                    ; the MAC: every other byte
        mov di, mac
        mov cx, 6
.pm:    lodsw
        stosb
        loop .pm
        mov dx, m_mac
        call puts
        mov si, mac
        call putmac
        mov dx, m_sig
        call puts
        mov al, [prom + 28]
        call puthex2
        mov al, [prom + 30]
        call puthex2
        call crlf

        ; start it
        outp 0, 21h
        outp 0Eh, 49h
        outp 0Ah, 0
        outp 0Bh, 0
        outp 0Ch, 04h                   ; RCR: broadcast too
        outp 0Dh, 02h
        outp 1, PSTART
        outp 3, PSTART
        outp 2, PSTOP
        outp 4, 40h
        outp 7, 0FFh
        outp 0Fh, 0
        outp 0, 61h                     ; page 1
        mov si, mac
        mov dx, BASE + 1
        mov cx, 6
.par:   lodsb
        out dx, al
        inc dx
        loop .par
        outp 7, PSTART + 1              ; CURR
        mov dx, BASE + 8
        mov cx, 8
        xor al, al
.mar:   out dx, al
        inc dx
        loop .mar
        outp 0, 22h                     ; page 0, start
        outp 0Dh, 0                     ; TCR: normal

        ; IRQ 11
        mov ax, 2500h + IRQV
        mov dx, isr
        int 21h
        in al, 0A1h
        and al, 0F7h
        out 0A1h, al
        in al, 21h
        and al, 0FBh
        out 21h, al
        outp 0Fh, 03h                   ; IMR: PRX, PTX

        ; the frame to 4000h
        mov si, mac                     ; source
        mov di, frame + 6
        movsw
        movsw
        movsw
        outp 8, 0
        outp 9, 40h
        outp 0Ah, 64
        outp 0Bh, 0
        outp 0, 12h                     ; remote write, start
        mov dx, BASE + 10h
        mov si, frame
        mov cx, 32
        rep outsw
        xor cx, cx
.rdc:   inp 7
        test al, 40h
        jnz .rdcok
        loop .rdc
        mov dx, m_nordc
        jmp die
.rdcok: outp 7, 40h
        outp 4, 40h
        outp 5, 64
        outp 6, 0
        outp 0, 26h                     ; transmit
        mov ah, 0
        int 1Ah
        mov [t0], dx
.ptx:   test byte [ibits], 02h
        jnz .sent
        mov ah, 0
        int 1Ah
        sub dx, [t0]
        cmp dx, 18
        jb .ptx
        mov dx, m_noptx
        jmp die
.sent:  mov dx, m_sent
        call puts

        ; receive for ~10 s
        mov ah, 0
        int 1Ah
        mov [t0], dx
.rx:    call drain
        mov ah, 0
        int 1Ah
        sub dx, [t0]
        cmp dx, 182
        jb .rx
        outp 0Fh, 0
        outp 0, 21h                     ; stop
        in al, 0A1h
        or al, 08h
        out 0A1h, al
        mov dx, m_irqs
        call puts
        mov ax, [nirq]
        call putdec
        mov dx, m_frames
        call puts
        mov ax, [nrx]
        call putdec
        call crlf
        mov ax, 4C00h
        int 21h

; take every frame between BNRY+1 and CURR (interrupts off: the handler
; must not find the 8390 on register page 1 or mid remote DMA)
drain:  cli
        call drain1
        sti
        ret
drain1: outp 0, 62h                     ; page 1: CURR
        inp 7
        mov [curr], al
        outp 0, 22h
        inp 3
        inc al
        cmp al, PSTOP
        jb .nx
        mov al, PSTART
.nx:    cmp al, [curr]
        je .done
        mov [page], al
        ; header + the first 60 bytes
        outp 8, 0
        mov dx, BASE + 9
        mov al, [page]
        out dx, al
        outp 0Ah, 64
        outp 0Bh, 0
        outp 0, 0Ah
        mov dx, BASE + 10h
        push cs
        pop es
        mov di, rbuf
        mov cx, 32
        rep insw
        outp 7, 40h
        inc word [nrx]
        cmp word [rbuf + 4 + 12], 0B588h
        jne .skip
        mov dx, m_got
        call puts
        mov si, rbuf + 4 + 6
        call putmac
        mov dx, m_say
        call puts
        mov si, rbuf + 4 + 14
        mov byte [rbuf + 63], 0
        call putz
        call crlf
.skip:  mov al, [rbuf + 1]              ; next page
        dec al
        cmp al, PSTART
        jae .b
        mov al, PSTOP - 1
.b:     mov dx, BASE + 3
        out dx, al
        jmp drain1
.done:  ret

isr:    push ax
        push dx
        inc word [cs:nirq]
        mov dx, BASE + 7
        in al, dx
        and al, 03h
        or [cs:ibits], al
        out dx, al                      ; ack PRX/PTX
        mov al, 20h
        out 0A0h, al
        out 20h, al
        pop dx
        pop ax
        iret

die:    call puts
        mov ax, 4C01h
        int 21h
crlf:   mov dx, m_crlf
puts:   mov ah, 9
        int 21h
        ret
putz:   lodsb
        or al, al
        jz .e
        cmp al, 20h
        jb .e
        mov dl, al
        mov ah, 2
        int 21h
        jmp putz
.e:     ret
putmac: mov cx, 6
.l:     lodsb
        call puthex2
        loop .l
        ret
puthex2:
        push ax
        shr al, 4
        call .nib
        pop ax
        and al, 0Fh
.nib:   add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     mov dl, al
        mov ah, 2
        int 21h
        ret
putdec: mov bx, 10
        xor cx, cx
.d:     xor dx, dx
        div bx
        push dx
        inc cx
        or ax, ax
        jnz .d
.p:     pop dx
        add dl, '0'
        mov ah, 2
        int 21h
        loop .p
        ret

frame:  db 0FFh, 0FFh, 0FFh, 0FFh, 0FFh, 0FFh
        times 6 db 0
        db 88h, 0B5h
        db 'hello from ne2k', 0
        times 64 - ($ - frame) db 0
t0:     dw 0
nirq:   dw 0
nrx:    dw 0
ibits:  db 0
curr:   db 0
page:   db 0
m_none: db 'no NE2000 at 300h', 13, 10, '$'
m_noreset: db 'NE2000 did not reset', 13, 10, '$'
m_nordc: db 'remote DMA never completed', 13, 10, '$'
m_noptx: db 'transmit never completed', 13, 10, '$'
m_mac:  db 'NE2000 PROM address $'
m_sig:  db ', signature $'
m_sent: db 'sent broadcast hello', 13, 10, '$'
m_got:  db 'got from $'
m_say:  db ': $'
m_irqs: db 'interrupts $'
m_frames: db ', frames $'
m_crlf: db 13, 10, '$'
prom:   times 32 db 0
mac:    times 6 db 0
rbuf:   times 64 db 0
