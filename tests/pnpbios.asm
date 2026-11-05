; PNPBIOS.COM: the Plug and Play BIOS as Windows 9x's BIOS enumerator uses
; it: finds the "$PnP" header in F0000h-FFFFFh (checksum), calls function
; 00h (number of nodes, largest node) and 01h for each node in turn, and
; prints each node's handle, ID, size and its first I/O range and IRQ.
;   nasm -f bin -o PNPBIOS.COM pnpbios.asm
        cpu 386
        bits 16
        org 100h

start:  mov ax, 0F000h
        mov es, ax
        xor di, di
.scan:  cmp dword [es:di], 'PnP' << 8 | '$'
        je .found
        add di, 16
        jnz .scan
        mov dx, msg_none
        jmp fail
.found: movzx cx, byte [es:di + 5]      ; checksum over its length
        xor al, al
        mov si, di
.sum:   add al, [es:si]
        inc si
        loop .sum
        test al, al
        jz .sumok
        mov dx, msg_sum
        jmp fail
.sumok: mov ax, [es:di + 0Dh]
        mov [entry], ax
        mov ax, [es:di + 0Fh]
        mov [entry + 2], ax
        mov dx, msg_hdr
        call puts
        mov ax, di
        call hex4
        call crlf

        push word 0F000h                ; 00h: BiosSelector, NodeSize *, NumNodes *
        push ds
        push word nodesize
        push ds
        push word numnodes
        push word 0
        call far [entry]
        add sp, 12
        test ax, ax
        jnz bad
        mov dx, msg_count
        call puts
        mov al, [numnodes]
        call hex2
        mov dx, msg_size
        call puts
        mov ax, [nodesize]
        call hex4
        call crlf

        mov byte [node], 0
.next:  mov al, [node]
        mov [cur], al
        push word 0F000h                ; 01h: BiosSelector, Control, buffer *, Node *
        push word 1
        push ds
        push word buf
        push ds
        push word node
        push word 1
        call far [entry]
        add sp, 14
        test ax, ax
        jnz bad
        mov al, [buf + 2]               ; handle
        call hex2
        mov al, ' '
        call putc
        mov dx, msg_pnp
        call puts
        mov al, [buf + 5]
        call hex2
        mov al, [buf + 6]
        call hex2
        mov dx, msg_sz
        call puts
        mov ax, [buf]
        call hex4
        mov si, buf + 12                ; the allocated resources: first I/O, first IRQ
        mov word [io1], 0FFFFh
        mov word [irq1], 0
.res:   mov al, [si]
        cmp al, 79h
        je .resend
        test al, 80h
        jnz .large
        cmp al, 47h
        jne .notio
        cmp word [io1], 0FFFFh
        jne .notio
        mov ax, [si + 2]
        mov [io1], ax
.notio: mov al, [si]
        cmp al, 22h
        jne .small
        mov ax, [si + 1]
        mov [irq1], ax
.small: mov al, [si]
        and al, 7
        movzx ax, al
        add si, ax
        inc si
        jmp .res
.large: mov ax, [si + 1]
        add si, ax
        add si, 3
        jmp .res
.resend:
        cmp word [io1], 0FFFFh
        je .noio
        mov dx, msg_io
        call puts
        mov ax, [io1]
        call hex4
.noio:  cmp word [irq1], 0
        je .noirq
        mov dx, msg_irq
        call puts
        mov ax, [irq1]
        bsf ax, ax
        call hex2
.noirq: call crlf
        cmp byte [node], 0FFh
        jne .next
        mov dx, msg_ok
        call puts
        mov ax, 4C00h
        int 21h

bad:    push ax
        mov dx, msg_err
        call puts
        pop ax
        call hex4
        call crlf
        mov ax, 4C01h
        int 21h
fail:   call puts
        mov ax, 4C01h
        int 21h

puts:   mov ah, 9
        int 21h
        ret
putc:   push dx
        mov dl, al
        mov ah, 2
        int 21h
        pop dx
        ret
crlf:   mov al, 13
        call putc
        mov al, 10
        jmp putc
hex4:   push ax
        mov al, ah
        call hex2
        pop ax
hex2:   push ax
        shr al, 4
        call .dig
        pop ax
.dig:   and al, 15
        add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     jmp putc

msg_none  db 'PNPBIOS: no $PnP header', 13, 10, '$'
msg_sum   db 'PNPBIOS: bad header checksum', 13, 10, '$'
msg_hdr   db 'PNPBIOS: header at F000:$'
msg_count db 'nodes $'
msg_size  db ', largest $'
msg_pnp   db 'PNP$'
msg_sz    db ' size $'
msg_io    db ' io $'
msg_irq   db ' irq $'
msg_err   db 'PNPBIOS: error $'
msg_ok    db 'PNPBIOS: all nodes read', 13, 10, '$'
entry     dd 0
numnodes  db 0
nodesize  dw 0
node      db 0
cur       db 0
io1       dw 0
irq1      dw 0
buf       times 512 db 0
