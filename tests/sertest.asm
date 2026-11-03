; SERTEST: drives every serial port in the BIOS data area straight on the
; UART, the way a terminal program or a bit-banging device programmer does
; (serial=dos). For each port: the scratch register, loopback data at
; 115200 baud (LSR polled), the modem lines looped back (MCR -> MSR, as a
; PIC/EPROM programmer toggles DTR/RTS and reads CTS/DSR), a receive
; interrupt (which IRQ it came on), and how many IN instructions to the
; LSR run in one timer tick (trapped accesses are far fewer).
;   nasm -f bin tests/sertest.asm -o SERTEST.COM
        org 100h
        cpu 386

start:
        mov dx, hello
        call puts
        ; hook IRQ 3-7 and 9-11: each counts, clears the UART under test, EOIs
        xor ax, ax
        mov es, ax
        mov si, irqtab
.hook:  lodsb
        cmp al, 0FFh
        je .hooked
        movzx bx, al
        mov di, bx
        cmp bl, 8
        jb .m
        add di, 70h - 8 - 8             ; vector 70h + (irq - 8)
.m:     add di, 8
        shl di, 2
        mov ax, [es:di]
        mov dx, [es:di + 2]
        mov bp, bx
        shl bp, 2
        mov [oldvec + bp], ax
        mov [oldvec + bp + 2], dx
        cli
        mov ax, bx
        imul ax, ax, stub1 - stub0
        add ax, stub0
        mov [es:di], ax
        mov [es:di + 2], cs
        sti
        jmp .hook
.hooked:
        in al, 21h
        mov [mask1], al
        in al, 0A1h
        mov [mask2], al
        and al, 0F1h                    ; IRQ 9-11 open
        out 0A1h, al
        mov al, [mask1]
        and al, 043h                    ; IRQ 2-5, 7 open
        out 21h, al

        xor si, si                      ; BDA slot
.port:  push 40h
        pop es
        mov bx, si
        shl bx, 1
        mov ax, [es:bx]
        or ax, ax
        jz .next
        mov [base], ax
        mov dx, s_com
        call puts
        mov ax, si
        add al, '1'
        call putc
        mov dx, s_at
        call puts
        mov ax, [base]
        call puthex
        mov dx, s_h
        call puts
        push si
        call testport
        pop si
.next:  inc si
        cmp si, 4
        jb .port

        ; unhook
        cli
        mov al, [mask1]
        out 21h, al
        mov al, [mask2]
        out 0A1h, al
        xor ax, ax
        mov es, ax
        mov si, irqtab
.unh:   lodsb
        cmp al, 0FFh
        je .done
        movzx bx, al
        mov di, bx
        cmp bl, 8
        jb .m2
        add di, 70h - 8 - 8
.m2:    add di, 8
        shl di, 2
        shl bx, 2
        mov ax, [oldvec + bx]
        mov [es:di], ax
        mov ax, [oldvec + bx + 2]
        mov [es:di + 2], ax
        jmp .unh
.done:  sti
        mov dx, s_end
        call puts
        mov ax, 4C00h
        int 21h

; ---- one port at [base] ----
testport:
        mov dx, [base]
        add dx, 7                       ; scratch
        mov al, 5Ah
        out dx, al
        in al, dx
        cmp al, 5Ah
        mov dx, s_scr
        call okfail

        mov dx, [base]
        add dx, 1
        xor al, al
        out dx, al                      ; IER 0
        add dx, 2
        mov al, 80h
        out dx, al                      ; DLAB
        sub dx, 3
        mov al, 1
        out dx, al                      ; divisor 1: 115200
        inc dx
        xor al, al
        out dx, al
        add dx, 2
        mov al, 3
        out dx, al                      ; 8N1
        inc dx
        mov al, 10h
        out dx, al                      ; MCR: loopback
        mov dx, [base]
        in al, dx                       ; drain
        in al, dx

        ; loopback data, LSR polled
        xor cx, cx                      ; errors
        mov bl, 0
.lb:    mov dx, [base]
        add dx, 5
        mov di, 0
.tx:    in al, dx
        test al, 20h
        jnz .txok
        dec di
        jnz .tx
.txok:  mov dx, [base]
        mov al, bl
        out dx, al
        add dx, 5
        mov di, 0
.rx:    in al, dx
        test al, 1
        jnz .rxok
        dec di
        jnz .rx
        inc cx
        jmp .lbn
.rxok:  mov dx, [base]
        in al, dx
        cmp al, bl
        je .lbn
        inc cx
.lbn:   add bl, 37
        inc byte [lbcount]
        cmp byte [lbcount], 32
        jb .lb
        mov byte [lbcount], 0
        cmp cx, 0
        mov dx, s_lb
        call okfail

        ; modem lines: MCR DTR -> DSR, RTS -> CTS, OUT1 -> RI, OUT2 -> DCD
        xor cx, cx
        mov si, mcrtab
.ml:    lodsw                           ; al: MCR bits, ah: MSR status expected
        cmp al, 0FFh
        je .mld
        mov dx, [base]
        add dx, 4
        or al, 10h
        out dx, al
        add dx, 2
        in al, dx
        and al, 0F0h
        cmp al, ah
        je .ml
        inc cx
        jmp .ml
.mld:   cmp cx, 0
        mov dx, s_ml
        call okfail

        ; receive interrupt (loopback, IER = received data, OUT2 on)
        mov di, cnt
        mov cx, 16
        xor ax, ax
        push ds
        pop es
        rep stosw
        mov dx, [base]
        add dx, 4
        mov al, 18h                     ; loopback + OUT2
        out dx, al
        mov dx, [base]
        inc dx
        mov al, 1
        out dx, al                      ; IER: RDA
        mov dx, [base]
        mov al, 0A5h
        out dx, al
        push 40h
        pop es
        mov ax, [es:6Ch]
        add ax, 3
.wi:    mov bx, [es:6Ch]
        cmp bx, ax
        jne .wi
        mov dx, [base]
        inc dx
        xor al, al
        out dx, al                      ; IER 0
        mov dx, s_irq
        call puts
        xor bx, bx
        xor cx, cx
.ic:    cmp word [cnt + bx], 0
        je .icn
        inc cx
        mov ax, bx
        shr ax, 1
        call putdec
        mov al, 'x'
        call putc
        mov ax, [cnt + bx]
        call putdec
        mov al, ' '
        call putc
.icn:   add bx, 2
        cmp bx, 32
        jb .ic
        or cx, cx
        jnz .icd
        mov dx, s_none
        call puts
.icd:   mov dx, s_crlf
        call puts

        ; speed: INs to the LSR in one timer tick
        mov dx, [base]
        add dx, 4
        xor al, al
        out dx, al                      ; out of loopback
        push 40h
        pop es
        mov ax, [es:6Ch]
.w0:    cmp ax, [es:6Ch]
        je .w0
        mov bp, [es:6Ch]
        mov dx, [base]
        add dx, 5
        xor ebx, ebx
.sp:    in al, dx
        in al, dx
        in al, dx
        in al, dx
        add ebx, 4
        cmp bp, [es:6Ch]
        je .sp
        mov dx, s_spd
        call puts
        mov eax, ebx
        call putdec32
        mov dx, s_crlf
        call puts
        ret

; ZF set: OK
okfail: pushf
        call puts
        popf
        mov dx, s_ok
        je .p
        mov dx, s_fail
.p:     jmp puts

; ---- IRQ stubs: stubN for IRQ N ----
%macro STUB 1
stub%1:
        push ax
        push dx
        inc word [cs:cnt + %1 * 2]
        mov dx, [cs:base]
        in al, dx                       ; RBR: clears the received-data interrupt
        add dx, 2
        in al, dx                       ; IIR
        mov al, 20h
%if %1 >= 8
        out 0A0h, al
%else
        nop                             ; (every stub the same size)
        nop
%endif
        out 20h, al
        pop dx
        pop ax
        iret
%endmacro
%assign i 0
%rep 16
STUB i
%assign i i + 1
%endrep

puts:   mov ah, 9
        int 21h
        ret
putc:   push dx
        mov dl, al
        mov ah, 2
        int 21h
        pop dx
        ret
puthex: push cx
        mov cx, 4
.h:     rol ax, 4
        push ax
        and al, 0Fh
        add al, '0'
        cmp al, '9'
        jbe .d
        add al, 7
.d:     call putc
        pop ax
        loop .h
        pop cx
        ret
putdec: movzx eax, ax
putdec32:
        push ebx
        push ecx
        push edx
        mov ebx, 10
        xor ecx, ecx
.d1:    xor edx, edx
        div ebx
        push dx
        inc cx
        or eax, eax
        jnz .d1
.d2:    pop ax
        add al, '0'
        call putc
        loop .d2
        pop edx
        pop ecx
        pop ebx
        ret

hello   db 'SERTEST: serial ports straight on the UART', 13, 10, '$'
s_com   db 'COM$'
s_at    db ' at $'
s_h     db 'h', 13, 10, '$'
s_scr   db '  scratch register  $'
s_lb    db '  loopback 115200   $'
s_ml    db '  modem lines       $'
s_irq   db '  receive IRQ       $'
s_none  db 'none$'
s_spd   db '  LSR reads a tick  $'
s_ok    db 'ok', 13, 10, '$'
s_fail  db 'FAIL', 13, 10, '$'
s_crlf  db 13, 10, '$'
s_end   db 'SERTEST done', 13, 10, '$'
irqtab  db 3, 4, 5, 6, 7, 9, 10, 11, 0FFh
mcrtab  db 01h, 20h, 02h, 10h, 04h, 40h, 08h, 80h, 03h, 30h, 0Fh, 0F0h, 00h, 00h, 0FFh, 0
lbcount db 0
mask1   db 0
mask2   db 0
base    dw 0
cnt     times 16 dw 0
oldvec  times 16 dd 0
