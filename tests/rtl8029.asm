; RTL8029.COM: the NE2000 as a PCI card (Realtek RTL8029, vmdos's default;
; ne2k=isa turns it back into the ISA card).
; 1. finds 10ECh:8029h through the PCI BIOS (INT 1Ah B102h), prints its
;    bus/devfn, class, I/O base (BAR0), BAR size and IRQ
; 2. reads the 8390's RTL8029 ID (page 0 0Ah/0Bh: 50h 43h, "PC") there
; 3. moves the BAR to D600h, as Windows may: the ID is there, and the old
;    address reads FFh; then puts it back
        org 100h
        cpu 386
        mov ax, 0B102h
        mov cx, 8029h
        mov dx, 10ECh
        xor si, si
        int 1Ah
        jc none
        mov [bdf], bx
        mov dx, m_found
        call str
        mov ax, [bdf]
        call hex16
        mov dx, m_class
        call str
        mov di, 08h
        call cfgrd
        shr ecx, 16
        mov ax, cx
        call hex16
        mov di, 10h
        call cfgrd
        and cx, 0FFE0h
        mov [io], cx
        mov dx, m_io
        call str
        mov ax, [io]
        call hex16
        mov ecx, 0FFFFFFFFh             ; the BAR's size
        mov di, 10h
        call cfgwr
        call cfgrd
        mov dx, m_size
        call str
        and cx, 0FFFCh
        neg cx
        mov ax, cx
        call hex16
        movzx ecx, word [io]
        call cfgwr
        mov di, 3Ch
        call cfgrd
        mov dx, m_irq
        call str
        movzx ax, cl
        call dec
        call crlf

        mov dx, [io]
        call id
        mov ecx, 0D600h                 ; move it
        mov di, 10h
        call cfgwr
        mov dx, m_moved
        call str
        mov dx, 0D600h
        call id
        mov dx, m_old
        call str
        mov dx, [io]
        add dx, 0Ah
        in al, dx
        call hex8
        call crlf
        movzx ecx, word [io]            ; and back
        mov di, 10h
        call cfgwr
        mov dx, m_back
        call str
        mov dx, [io]
        call id
        mov ax, 4C00h
        int 21h

none:   mov dx, m_none
        call str
        mov ax, 4C01h
        int 21h

id:     ; DX = base: page 0, prints the ID bytes
        push dx
        mov al, 21h                     ; page 0, stop, no DMA
        out dx, al
        mov dx, m_id
        call str
        pop dx
        push dx
        add dx, 0Ah
        in al, dx
        call hex8
        mov al, ' '
        call chr
        pop dx
        add dx, 0Bh
        in al, dx
        call hex8
        jmp crlf

cfgrd:  mov ax, 0B10Ah
        mov bx, [bdf]
        int 1Ah
        ret
cfgwr:  mov ax, 0B10Dh
        mov bx, [bdf]
        int 1Ah
        ret

str:    mov ah, 9
        int 21h
        ret
chr:    push dx
        mov dl, al
        mov ah, 2
        int 21h
        pop dx
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

m_none  db 'no RTL8029 (10EC:8029)', 13, 10, '$'
m_found db 'RTL8029 at bus/devfn $'
m_class db ', class $'
m_io    db ', I/O $'
m_size  db ' (size $'
m_irq   db '), IRQ $'
m_id    db 'ID: $'
m_moved db 'BAR moved to D600h, $'
m_old   db 'old address reads $'
m_back  db 'back at the start, $'
m_crlf  db 13, 10, '$'
bdf     dw 0
io      dw 0
