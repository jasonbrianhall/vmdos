; PMMOUSE.COM: a 16-bit DPMI client installs a mouse event handler the way
; DOS extenders do it (a DPMI real-mode callback passed to INT 33h function
; 0Ch through 0300h), then waits in protected mode for events. Writes
; "PMMOUSE OK nnnn" (events seen) or "PMMOUSE NONE" at the bottom of the
; screen. Move the mouse during the 5 seconds it waits.
        org 100h
        mov bx, 1000h
        mov ah, 4Ah
        int 21h
        mov ax, 1687h
        int 2Fh
        or ax, ax
        jnz quit
        mov [entry], di
        mov [entry + 2], es
        mov bx, si
        mov ah, 48h
        int 21h
        jc quit
        mov es, ax
        xor ax, ax
        call far [entry]
        jc quit
        ; --- protected mode ---
        mov [dsel], ds
        push ds
        pop es
        ; mouse reset via 0300h
        mov word [rm_ax], 0
        call int33
        ; real-mode callback -> pm_handler, register structure cbregs
        push ds
        mov ax, cs
        mov ds, ax
        mov si, pm_handler
        pop es
        mov di, cbregs
        mov ax, 0303h
        int 31h
        push es
        pop ds
        jc fail
        ; INT 33h AX=000Ch CX=001Fh ES:DX = callback
        mov word [rm_ax], 000Ch
        mov word [rm_cx], 001Fh
        mov [rm_dx], dx
        mov [rm_es], cx
        call int33
        ; wait ~5 s (91 ticks) for events
        mov ax, 40h
        mov fs, ax
        mov bx, [fs:6Ch]
        add bx, 91
w_wait:  sti
        cmp word [events], 0
        jne w_ok
        mov ax, [fs:6Ch]
        cmp ax, bx
        jne w_wait
fail:   mov si, nonemsg
        jmp show
w_ok:    mov cx, 30                      ; let a few more come
w_more:  mov ax, [fs:6Ch]
w_t:     cmp ax, [fs:6Ch]
        je w_t
        loop w_more
        mov si, okmsg
show:   mov word [rm_ax], 000Ch          ; remove the handler
        mov word [rm_cx], 0
        call int33
        mov ax, 0002h
        mov bx, 0B800h
        int 31h
        mov es, ax
        mov di, 160 * 24
        mov ah, 0Eh
.p:     lodsb
        or al, al
        jz .n
        stosw
        jmp .p
.n:     mov ax, [events]
        mov cx, 4
.h:     rol ax, 4
        push ax
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .d
        add al, 7
.d:     mov ah, 0Eh
        stosw
        pop ax
        loop .h
quit:   mov ax, 4C00h
        int 21h

int33:  push es                         ; simulate INT 33h with rmregs
        push ds
        pop es
        mov di, rmregs
        mov bx, 33h
        xor cx, cx
        mov ax, 0300h
        int 31h
        pop es
        ret

; Real-mode callback: DS:SI = real-mode stack, ES:DI = the register structure.
pm_handler:
        push ax
        push ds
        mov ax, [cs:dsel_cs]
        mov ds, ax
        inc word [events]
        pop ds
        pop ax
        cld
        lodsw                           ; simulate the RETF of the real-mode caller
        mov [es:di + 2Ah], ax
        lodsw
        mov [es:di + 2Ch], ax
        add word [es:di + 2Eh], 4
        iret

dsel_cs equ dsel                        ; (COM: CS and DS share the segment)
entry   dd 0
dsel    dw 0
events  dw 0
okmsg   db 'PMMOUSE OK ', 0
nonemsg db 'PMMOUSE NONE ', 0
rmregs:
rm_edi  dd 0
rm_esi  dd 0
rm_ebp  dd 0
        dd 0
rm_ebx  dd 0
rm_dx   dw 0
        dw 0
rm_cx   dw 0
        dw 0
rm_ax   dw 0
        dw 0
rm_fl   dw 0
rm_es   dw 0
rm_ds   dw 0
rm_fs   dw 0
rm_gs   dw 0
rm_ip   dw 0
rm_cs   dw 0
rm_sp   dw 0
rm_ss   dw 0
cbregs  times 32h db 0
