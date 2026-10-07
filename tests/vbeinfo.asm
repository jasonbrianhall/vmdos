; VBEINFO.COM: what INT 10h 4F00h reports: version, memory, the mode list
; (up to its FFFFh end, at most 64 entries) and the OEM string.
        org 100h
        mov ax, 4F00h
        mov di, info
        mov dword [info], '2EBV'        ; ask for VBE 2.0 information
        int 10h
        cmp ax, 004Fh
        jne none
        mov dx, vmsg
        call puts
        mov ax, [info + 4]
        call hex
        mov dx, mmsg
        call puts
        mov ax, [info + 12h]            ; 64 KB blocks
        call hex
        mov dx, lmsg
        call puts
        lds si, [info + 0Eh]
        mov cx, 64
.m:     lodsw
        cmp ax, 0FFFFh
        je .end
        push ds
        push cs
        pop ds
        call hex
        mov dl, ' '
        mov ah, 2
        int 21h
        pop ds
        loop .m
        push cs
        pop ds
        mov dx, nomsg                   ; no FFFFh within 64 entries
        call puts
        jmp short .oem
.end:   push cs
        pop ds
.oem:   mov dx, omsg
        call puts
        lds si, [info + 6]
.o:     lodsb
        or al, al
        jz .x
        mov dl, al
        mov ah, 2
        int 21h
        jmp .o
.x:     push cs
        pop ds
        mov dx, crlf
        call puts
        ret
none:   mov dx, nvmsg
        call puts
        ret
puts:   mov ah, 9
        int 21h
        ret
hex:    push cx                         ; AX in hex (keeps CX)
        mov cx, 4
.h:     rol ax, 4
        push ax
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .d
        add al, 7
.d:     mov dl, al
        mov ah, 2
        int 21h
        pop ax
        loop .h
        pop cx
        ret
vmsg    db 'VBE $'
mmsg    db '  memory (64K) $'
lmsg    db 13, 10, 'modes: $'
nomsg   db ' (no end!)$'
omsg    db 13, 10, 'OEM: $'
nvmsg   db 'no VESA', 13, 10, '$'
crlf    db 13, 10, '$'
info    times 512 db 0
