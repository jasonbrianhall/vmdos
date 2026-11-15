; VBELFB.COM: how fast a VESA mode is drawn through its linear frame buffer
; from a DPMI client (16-bit, 32-bit addressing): MODE (default 101h) with
; the LFB bit, DPMI 0800h maps PhysBasePtr, the whole screen filled again
; and again for 2 s (36 BIOS ticks); prints the frames drawn.
;   nasm -f bin [-DMODE=101h] [-DBYTES] [-DHANG] -o VBELFB.COM tests/vbelfb.asm
; -DHANG: then a white row 100 and it stays in the mode (a screenshot to compare).
        org 100h
        cpu 386
%ifndef MODE
%define MODE 101h
%endif
        mov ax, 4F01h
        mov cx, MODE
        mov di, info
        int 10h
        cmp ax, 004Fh
        jne fail
        test byte [info], 80h           ; an LFB
        jz fail
        movzx eax, word [info + 10h]
        movzx edx, word [info + 14h]
        mul edx
        mov [total], eax
        mov bx, 1000h
        mov ah, 4Ah
        int 21h
        mov ax, 1687h
        int 2Fh
        or ax, ax
        jnz fail
        mov [entry], di
        mov [entry + 2], es
        mov bx, si
        mov ah, 48h
        int 21h
        jc fail
        mov es, ax
        xor ax, ax
        call far [entry]
        jc fail
        mov word [rm + 1Ch], 4F02h      ; the mode, through DPMI 0300h
        mov word [rm + 10h], MODE | 4000h
        call rmint
        cmp word [rm + 1Ch], 004Fh
        jne pfail
        mov bx, [info + 2Ah]            ; DPMI 0800h: PhysBasePtr, 4 MB
        mov cx, [info + 28h]
        mov si, 40h
        xor di, di
        mov ax, 0800h
        int 31h
        jc pfail
        mov [lin], cx
        mov [lin + 2], bx
        xor ax, ax                      ; a selector on it
        mov cx, 1
        int 31h
        jc pfail
        mov [lsel], ax
        mov bx, ax
        mov cx, [lin + 2]
        mov dx, [lin]
        mov ax, 0007h
        int 31h
        mov bx, [lsel]
        mov cx, 3Fh
        mov dx, 0FFFFh
        mov ax, 0008h
        int 31h
        mov ax, 0002h
        mov bx, 40h
        int 31h
        mov fs, ax
        mov es, [lsel]

        mov ax, [fs:6Ch]
.sync:  cmp ax, [fs:6Ch]
        je .sync
        mov ax, [fs:6Ch]
        add ax, 36
        mov [endtick], ax
frame:  mov al, [frames]
        mov ah, al
        push ax
        shl eax, 16
        pop ax
        xor edi, edi
        mov ecx, [total]
        cld
%ifdef BYTES
.b:     a32 stosb
        dec ecx
        jnz .b
%else
        shr ecx, 2
        a32 rep stosd
%endif
        inc dword [frames]
        mov ax, [fs:6Ch]
        cmp ax, [endtick]
        jb frame
%ifdef HANG
        mov edi, 640 * 100              ; -DHANG: a white row 100, then stay in the mode
        mov ecx, 640
        mov al, 0Fh
        a32 rep stosb
        sti
.h:     jmp .h
%endif

        mov word [rm + 1Ch], 3
        call rmint
        mov eax, [frames]
        call dec32
        mov si, msg
        call puts
        mov ax, 4C00h
        int 21h
pfail:  mov word [rm + 1Ch], 3
        call rmint
fail:   mov si, failmsg
        call puts
        mov ax, 4C01h
        int 21h
rmint:  push es                         ; DPMI 0300h: INT 10h in real mode, regs at ds:rm
        push ds
        pop es
        mov edi, rm
        mov ax, 0300h
        mov bx, 0010h
        xor cx, cx
        int 31h
        pop es
        ret
puts:   lodsb
        cmp al, '$'
        je .e
        mov dl, al
        mov ah, 2
        int 21h
        jmp puts
.e:     ret
dec32:  mov ebx, 10
        xor cx, cx
.d:     xor edx, edx
        div ebx
        push dx
        inc cx
        test eax, eax
        jnz .d
.p:     pop dx
        add dl, '0'
        mov ah, 2
        int 21h
        loop .p
        ret
msg     db ' frames in 2 s (LFB)', 13, 10, '$'
failmsg db 'VBE LFB mode failed', 13, 10, '$'
total   dd 0
frames  dd 0
lin     dd 0
lsel    dw 0
endtick dw 0
entry   dd 0
rm      times 32h db 0
info    times 256 db 0
