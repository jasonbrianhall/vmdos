; IPXTEST.COM: two of these on one LAN find each other over IPX.
; Opens socket 4545h, listens with four ECBs (ESRs count them), broadcasts
; "hello" every 2 s, answers the first hello from each other node with a
; unicast "reply", runs an AES event, and after ~20 s prints what it saw.
        cpu 286
        org 100h
        mov ax, 7A00h
        int 2Fh
        cmp al, 0FFh
        je .have
        mov dx, m_noipx
        jmp die
.have:  mov [ipx], di
        mov [ipx + 2], es
        mov [hello_ecb + 38], cs
        mov [reply_ecb + 38], cs
        mov [aes_ecb + 6], cs
        push cs
        pop es
        mov bx, 9                       ; our address
        mov si, myaddr
        call far [ipx]
        mov dx, m_node
        call puts
        mov si, myaddr + 4
        call putmac
        call crlf

        mov bx, 0                       ; open 4545h
        mov al, 0
        mov dx, 4545h
        call far [ipx]
        or al, al
        jz .open
        mov dx, m_open
        jmp die
.open:  mov cx, 4                       ; four listening ECBs
        mov si, lecbs
.l:     call listen
        add si, LECB_SIZE
        loop .l

        mov bx, 7                       ; an AES event in 18 ticks
        mov ax, 18
        mov si, aes_ecb
        call far [ipx]

        mov ah, 0
        int 1Ah
        mov [t0], dx
        mov word [tsend], 0FFFFh

.loop:  mov ah, 0
        int 1Ah
        sub dx, [t0]
        cmp dx, 364                     ; ~20 s
        jae .end
        ; a hello every 2 s
        mov ax, dx
        sub ax, [tsend]
        cmp ax, 36
        jb .rx
        mov [tsend], dx
        mov si, hello_ecb
        cmp byte [si + 8], 0
        jne .rx
        mov di, si
        add di, 28
        mov ax, 0FFFFh
        mov [di], ax
        mov [di + 2], ax
        mov [di + 4], ax
        mov bx, 3
        call far [ipx]
        inc word [n_sent]
.rx:    mov cx, 4
        mov si, lecbs
.c:     cmp byte [si + 8], 0            ; in use?
        jne .cn
        push cx
        push si
        call got
        pop si
        call listen
        pop cx
.cn:    add si, LECB_SIZE
        loop .c
        mov bx, 0Ah                     ; relinquish control
        call far [ipx]
        jmp .loop

.end:   mov dx, m_sum1
        call puts
        mov ax, [n_sent]
        call putdec
        mov dx, m_sum2
        call puts
        mov ax, [n_hello]
        call putdec
        mov dx, m_sum3
        call puts
        mov ax, [n_reply]
        call putdec
        mov dx, m_sum4
        call puts
        mov ax, [n_esr]
        call putdec
        mov dx, m_sum5
        call puts
        mov al, [aes_fired]
        xor ah, ah
        call putdec
        call crlf
        mov bx, 1                       ; close
        mov dx, 4545h
        call far [ipx]
        mov dx, m_done
        call puts
        mov ax, 4C00h
        int 21h

; SI = a listening ECB
listen: mov word [si + 4], rx_esr
        mov [si + 6], cs
        mov word [si + 10], 4545h
        mov word [si + 34], 1
        lea ax, [si + LECB_BUF]
        mov [si + 36], ax
        mov [si + 38], cs
        mov word [si + 40], 600
        mov bx, 4
        call far [ipx]
        ret

; SI = an ECB that got a packet
got:    cmp byte [si + 9], 0
        jne .x
        lea bx, [si + LECB_BUF]         ; the IPX header
        cmp word [bx + 30], 'he'
        jne .r
        inc word [n_hello]
        cmp word [n_hello], 1
        jne .ans
        mov dx, m_hello
        call puts
        push si
        lea si, [bx + 22]
        call putmac
        pop si
        call crlf
.ans:   ; answer it (once per sender would need a table: just the first three)
        cmp word [n_hello], 3
        ja .x
        mov di, reply_ecb
        cmp byte [di + 8], 0
        jne .x
        push si
        add si, 28                      ; its immediate address
        add di, 28
        push cs
        pop es
        movsw
        movsw
        movsw
        pop si
        push si
        lea si, [bx + 18]               ; network + node of the sender -> our destination
        mov di, reply_hdr + 6
        movsw
        movsw
        movsw
        movsw
        movsw
        pop si
        mov bx, 3
        push si
        mov si, reply_ecb
        call far [ipx]
        pop si
        ret
.r:     cmp word [bx + 30], 're'
        jne .x
        inc word [n_reply]
        cmp word [n_reply], 1
        jne .x
        mov dx, m_reply
        call puts
        push si
        lea si, [bx + 22]
        call putmac
        pop si
        call crlf
.x:     ret

rx_esr: inc word [cs:n_esr]
        retf
aes_esr:
        mov byte [cs:aes_fired], 1
        retf

crlf:   mov dx, m_crlf
puts:   mov ah, 9
        int 21h
        ret
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

ipx:    dd 0
t0:     dw 0
tsend:  dw 0
n_sent: dw 0
n_hello: dw 0
n_reply: dw 0
n_esr:  dw 0
aes_fired: db 0
myaddr: times 12 db 0

hello_ecb:
        dd 0, 0
        db 0, 0
        dw 4545h
        times 16 db 0
        times 6 db 0FFh
        dw 1
        dw hello_hdr, 0                 ; segment patched below at start? (COM: CS)
        dw hello_len
hello_hdr:
        dw 0, 0
        db 0, 4
        dd 0
        times 6 db 0FFh
        dw 4545h
        times 12 db 0
        db 'hello', 0
hello_len equ $ - hello_hdr

reply_ecb:
        dd 0, 0
        db 0, 0
        dw 4545h
        times 16 db 0
        times 6 db 0
        dw 1
        dw reply_hdr, 0
        dw reply_len
reply_hdr:
        dw 0, 0
        db 0, 4
        times 10 db 0
        dw 4545h
        times 12 db 0
        db 'reply', 0
reply_len equ $ - reply_hdr

aes_ecb:
        dd 0
        dw aes_esr, 0
        times 50 db 0

LECB_BUF  equ 48
LECB_SIZE equ 48 + 600
lecbs:  times 4 * LECB_SIZE db 0

m_noipx: db 'no IPX driver', 13, 10, '$'
m_open: db 'open socket failed', 13, 10, '$'
m_node: db 'IPXTEST node $'
m_hello: db 'hello from $'
m_reply: db 'reply from $'
m_sum1: db 'sent $'
m_sum2: db ' hellos; got $'
m_sum3: db ' hellos, $'
m_sum4: db ' replies; ESRs $'
m_sum5: db '; AES $'
m_done: db 'IPXTEST done', 13, 10, '$'
m_crlf: db 13, 10, '$'

die:    call puts
        mov ax, 4C01h
        int 21h
