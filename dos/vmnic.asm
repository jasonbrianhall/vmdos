; VMNIC.COM: which network card vmdos uses (VMPKT and VMIPX run on it).
;   VMNIC          list the cards; * marks the one in use
;   VMNIC 2        use card 2 (programs on VMPKT carry on: the new card
;                  is given the address they already have)
;   VMNIC AUTO     let vmdos pick: one with a link (the default)
; (INT 2Fh AX=564Eh: BL=6 lists, BL=7 picks; dos/vmapi.inc.)
        cpu 286
        org 100h
        mov si, 81h
.skip:  lodsb
        cmp al, ' '
        je .skip
        cmp al, 9
        je .skip
        cmp al, 13
        je list
        or al, 20h
        cmp al, 'a'
        je .auto
        sub al, '0'
        cmp al, 1
        jb usage
        cmp al, 8
        ja usage
        dec al
        mov cl, al
        jmp .pick
.auto:  mov cl, 0FFh
.pick:  mov ax, 564Eh
        mov bl, 7
        call vmapi
        cmp bx, 564Dh
        jne novm
        or ax, ax
        jz list
        mov dx, msg_nocard
        cmp ax, 1
        je .e
        mov dx, msg_nostart
.e:     call puts
        mov ax, 4C01h
        int 21h

list:   mov ax, 564Eh
        mov bl, 6
        push cs
        pop es
        mov di, recs
        call vmapi
        cmp bx, 564Dh
        jne novm
        mov [count], ax
        mov [inuse], cx
        mov [auto], dl
        or ax, ax
        jnz .have
        mov dx, msg_none
        call puts
        mov ax, 4C01h
        int 21h
.have:  mov dx, msg_head
        call puts
        cmp byte [auto], 0
        je .h2
        mov dx, msg_auto
        call puts
.h2:    mov dx, msg_crlf
        call puts
        mov cx, [count]
        cmp cx, 8
        jbe .n
        mov cx, 8
.n:     xor bx, bx                      ; index
        mov si, recs
.card:  push cx
        mov dl, ' '
        cmp bx, [inuse]
        jne .mark
        mov dl, '*'
.mark:  call putc
        mov dl, ' '
        call putc
        mov ax, bx
        inc ax
        add al, '0'
        mov dl, al
        call putc
        mov dx, msg_sp2
        call puts
        cmp byte [si], 4                ; a USB adapter: no PCI address
        jne .pci
        mov dx, msg_usb
        call puts
        jmp .addrd
.pci:   mov al, [si + 3]                ; bus:dev.fn
        call puthex2
        mov dl, ':'
        call putc
        mov al, [si + 4]
        call puthex2
        mov dl, '.'
        call putc
        mov dl, [si + 5]
        add dl, '0'
        call putc
.addrd: mov dl, ' '
        call putc
        ; name, padded to 10
        push si
        lea si, [si + 22]
        mov cx, 10
.nm:    lodsb
        or al, al
        jz .pad
        mov dl, al
        call putc
        loop .nm
        jmp .nmd
.pad:   mov dl, ' '
        call putc
        loop .pad
.nmd:   pop si
        mov dl, ' '
        call putc
        mov al, [si + 7]                ; vendor:device
        call puthex2
        mov al, [si + 6]
        call puthex2
        mov dl, ':'
        call putc
        mov al, [si + 9]
        call puthex2
        mov al, [si + 8]
        call puthex2
        mov dl, ' '
        call putc
        push si
        lea si, [si + 10]
        call putmac
        pop si
        mov dx, msg_off
        cmp byte [si + 1], 0
        je .st
        mov dx, msg_down
        cmp byte [si + 2], 0
        je .st
        mov dx, msg_up
.st:    call puts
        ; the address it sends with, if not its own
        cmp bx, [inuse]
        jne .next
        push si
        lea di, [si + 16]
        add si, 10
        push cs
        pop es
        mov cx, 6
        repe cmpsb
        pop si
        je .next
        mov dx, msg_as
        call puts
        push si
        lea si, [si + 16]
        call putmac
        pop si
        mov dx, msg_crlf
        call puts
.next:  add si, 32
        inc bx
        pop cx
        dec cx
        jnz .card
        mov ax, 4C00h
        int 21h

usage:  mov dx, msg_usage
        call puts
        mov ax, 4C01h
        int 21h
novm:   mov dx, msg_novm
        call puts
        mov ax, 4C01h
        int 21h

%include "vmapi.inc"

putc:   push ax
        mov ah, 2
        int 21h
        pop ax
        ret
puts:   push ax
        mov ah, 9
        int 21h
        pop ax
        ret
putmac: push cx
        mov cx, 6
.l:     lodsb
        call puthex2
        dec cx
        jz .e
        mov dl, ':'
        call putc
        jmp .l
.e:     pop cx
        ret
puthex2:
        push ax
        shr al, 4
        call .nib
        pop ax
        and al, 0Fh
.nib:   push ax
        add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     mov dl, al
        call putc
        pop ax
        ret

msg_head:   db 'Network cards (* in use)$'
msg_auto:   db ', picked automatically (VMNIC n to choose)$'
msg_crlf:   db 13, 10, '$'
msg_sp2:    db '  $'
msg_usb:    db 'USB    $'
msg_up:     db '  link up', 13, 10, '$'
msg_down:   db '  no link', 13, 10, '$'
msg_off:    db '  not running', 13, 10, '$'
msg_as:     db '        sending as $'
msg_none:   db 'vmdos found no network card it can drive (DMESG says why)', 13, 10, '$'
msg_nocard: db 'VMNIC: no such card', 13, 10, '$'
msg_nostart: db 'VMNIC: that card is not running (DMESG says why)', 13, 10, '$'
msg_novm:   db 'VMNIC: not running under vmdos', 13, 10, '$'
msg_usage:  db 'usage: VMNIC         list the network cards', 13, 10
            db '       VMNIC n       use card n', 13, 10
            db '       VMNIC AUTO    let vmdos pick one with a link', 13, 10, '$'
count:  dw 0
inuse:  dw 0
auto:   db 0
recs:   times 8 * 32 db 0
