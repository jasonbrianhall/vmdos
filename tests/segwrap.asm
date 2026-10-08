; SEGWRAP.COM: string instructions that run past offset FFFFh must wrap
; to offset 0 of the segment, as on an 8086 (a 386 in v86 mode faults).
; Needs KVM or real hardware: QEMU TCG does not check segment limits, so
; without KVM the access goes past the segment and this reports FAILED.
        org 100h
        mov ax, cs
        add ax, 1000h                   ; a scratch segment 64 KiB above us
        mov ds, ax
        mov es, ax
        cld
        ; pattern 0..15 at DS:FFF8..FFFF and DS:0000..0007 (wrapped)
        mov di, 0FFF8h
        xor al, al
        mov cx, 16
.fill:  mov [di], al                    ; byte stores never fault
        inc di
        inc al
        loop .fill
        ; rep movsw from FFF9 (crosses FFFF) to ES:1000
        mov si, 0FFF9h
        mov di, 1000h
        mov cx, 4
        rep movsw
        cmp si, 1                       ; FFF9 + 8, wrapped
        jne fail
        mov si, 1000h
        mov bx, expect1
.chk:   mov al, [si]
        cmp al, [cs:bx]
        jne fail
        inc si
        inc bx
        cmp bx, expect1 + 8
        jne .chk
        ; lodsw at FFFF: low byte from FFFF (7), high byte from 0000 (8)
        mov si, 0FFFFh
        lodsw
        cmp ax, 0807h
        jne fail
        ; repe cmpsb across the wrap: FFFE.. vs a copy at 2000
        mov word [2000h], 0706h
        mov word [2002h], 0908h
        mov si, 0FFFEh
        mov di, 2000h
        mov cx, 4
        repe cmpsb
        jne fail
        or cx, cx
        jnz fail
        push cs
        pop ds
        mov dx, ok
        jmp short say
fail:   push cs
        pop ds
        mov dx, bad
say:    mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h
expect1 db 1, 2, 3, 4, 5, 6, 7, 8
ok      db 'SEGWRAP: OK', 13, 10, '$'
bad     db 'SEGWRAP: FAILED', 13, 10, '$'
