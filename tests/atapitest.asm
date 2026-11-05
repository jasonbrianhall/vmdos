; ATAPITEST.COM: the guest's IDE channel with ATAPI CD drives (atapi.c),
; driven by PIO as an IDE driver does (Windows' ESDI_506): for the master
; and the slave, IDENTIFY PACKET DEVICE (model), INQUIRY (product), READ
; CAPACITY, READ TOC (tracks), READ(10) of sectors 16-17 in one command
; (the ISO 9660 descriptor "CD001", and a Joliet one: escape "%/@").
; Polled, with nIEN set (no IRQs).   nasm -f bin -o ATAPITEST.COM atapitest.asm
        cpu 486
        bits 16
        org 100h

BASE    equ 170h
CTRL    equ 376h

start:  mov dx, CTRL
        mov al, 2                       ; nIEN
        out dx, al
        mov byte [devsel], 0A0h
        mov si, msg_master
        call drive
        mov byte [devsel], 0B0h
        mov si, msg_slave
        call drive
        mov ax, 4C00h
        int 21h

; one drive: [devsel] A0h master, B0h slave
drive:  call puts
        mov dx, BASE + 6
        mov al, [devsel]
        out dx, al
        mov dx, BASE + 7                ; IDENTIFY PACKET DEVICE (no DRQ: no drive)
        mov al, 0A1h
        out dx, al
        call wait_drq
        jnc .there
        mov si, msg_none
        jmp puts
.there:
        mov di, buf
        mov cx, 256
        mov dx, BASE
        rep insw
        mov si, msg_model
        call puts
        mov si, buf + 27 * 2            ; model: words 27-46, high byte first
        mov cx, 20
.mod:   lodsw
        xchg al, ah
        push ax
        call putc
        pop ax
        mov al, ah
        call putc
        loop .mod
        call crlf

        mov si, cdb_inq                 ; INQUIRY
        call packet
        jc .fail
        mov si, msg_inq
        call puts
        mov si, buf + 16
        mov cx, 16
.inq:   lodsb
        call putc
        loop .inq
        call crlf

        mov si, cdb_tur                 ; TEST UNIT READY (unit attention first: again)
        call packet
        mov si, cdb_tur
        call packet
        jc .fail

        mov si, cdb_cap                 ; READ CAPACITY
        call packet
        jc .fail
        mov si, msg_cap
        call puts
        mov eax, [buf]
        bswap eax
        inc eax
        call putdec
        call crlf

        mov si, cdb_toc                 ; READ TOC
        call packet
        jc .fail
        mov si, msg_toc
        call puts
        mov al, [buf + 2]
        call puthex
        mov al, '-'
        call putc
        mov al, [buf + 3]
        call puthex
        call crlf

        mov si, cdb_rd                  ; READ(10) 16, 2 sectors
        call packet
        jc .fail
        mov si, msg_pvd
        call puts
        mov si, buf + 1
        mov cx, 5
.pvd:   lodsb
        call putc
        loop .pvd
        cmp byte [buf + 2048], 2        ; a supplementary descriptor, Joliet's escape
        jne .nojol
        cmp word [buf + 2048 + 88], '%/'
        jne .nojol
        mov si, msg_joliet
        call puts
        jmp short .rdone
.nojol: mov si, msg_nojol
        call puts
.rdone: mov si, msg_bytes
        call puts
        mov eax, [got]
        call putdec
        call crlf
        ret
.fail:  mov si, msg_fail
        call puts
        mov dx, BASE + 1
        in al, dx
        call puthex
        call crlf
        ret

; PACKET with the 12-byte command at SI; data in to buf, [got] bytes. CF: error.
packet: mov dword [got], 0
        mov dx, BASE + 1
        xor al, al                      ; PIO
        out dx, al
        mov dx, BASE + 4
        mov al, 0                       ; byte count limit 0800h
        out dx, al
        inc dx
        mov al, 08h
        out dx, al
        mov dx, BASE + 7
        mov al, 0A0h
        out dx, al
        call wait_drq
        jc .err
        mov cx, 6
        mov dx, BASE
        rep outsw
        mov di, buf
.loop:  mov dx, BASE + 7
        in al, dx
        test al, 80h
        jnz .loop
        test al, 1
        jnz .err
        test al, 8
        jz .done
        mov dx, BASE + 4                ; a block: its byte count
        in al, dx
        mov cl, al
        inc dx
        in al, dx
        mov ch, al
        movzx ecx, cx
        add [got], ecx
        shr cx, 1
        mov dx, BASE
        rep insw
        jmp .loop
.done:  clc
        ret
.err:   stc
        ret

wait_drq:
        mov cx, 0FFFFh
.w:     mov dx, BASE + 7
        in al, dx
        test al, 1
        jnz .bad
        test al, 8
        jnz .ok
        loop .w
.bad:   stc
        ret
.ok:    clc
        ret

puts:   lodsb
        test al, al
        jz .r
        call putc
        jmp puts
.r:     ret
putc:   push dx
        push ax
        mov dl, al
        mov ah, 2
        int 21h
        pop ax
        pop dx
        ret
crlf:   mov al, 13
        call putc
        mov al, 10
        jmp putc
puthex: push ax
        shr al, 4
        call .d
        pop ax
.d:     and al, 15
        add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     jmp putc
putdec: mov ebx, 10
        xor cx, cx
.dv:    xor edx, edx
        div ebx
        push dx
        inc cx
        test eax, eax
        jnz .dv
.pr:    pop ax
        add al, '0'
        call putc
        loop .pr
        ret

devsel  db 0
got     dd 0
cdb_inq db 12h, 0, 0, 0, 36, 0, 0, 0, 0, 0, 0, 0
cdb_tur db 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
cdb_cap db 25h, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
cdb_toc db 43h, 0, 0, 0, 0, 0, 0, 3, 24h, 0, 0, 0
cdb_rd  db 28h, 0, 0, 0, 0, 16, 0, 0, 2, 0, 0, 0
msg_master db 'master: ', 0
msg_slave  db 'slave: ', 0
msg_none   db 'none', 13, 10, 0
msg_model  db 'model ', 0
msg_inq    db '  INQUIRY ', 0
msg_cap    db '  sectors ', 0
msg_toc    db '  tracks ', 0
msg_pvd    db '  sector 16: ', 0
msg_joliet db ', Joliet', 0
msg_nojol  db ', no Joliet', 0
msg_bytes  db ', bytes ', 0
msg_fail   db '  failed, error ', 0
buf     times 4096 db 0
