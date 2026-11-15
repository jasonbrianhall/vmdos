; VBESPD.COM: how fast a VESA mode is drawn through the banked window.
; Sets MODE (default 101h, 640x480x256), fills the whole screen again and
; again for 2 s (36 BIOS ticks), switching banks with 4F05h (INT 10h, or
; the mode's WinFuncPtr with -DFAR), and prints the frames drawn.
;   nasm -f bin [-DMODE=101h] [-DFAR] [-DBYTES] -o VBESPD.COM tests/vbespd.asm
; -DDPMI: from a 16-bit DPMI client (INT 10h reflected; with -DRMI through
; DPMI 0300h instead). -DBYTES writes a byte at a time (STOSB) instead of REP STOSD.
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
        mov ax, [info + 4]              ; granularity, KB
        test ax, ax
        jz fail
        mov [gran], ax
        mov bx, ax
        mov ax, 64
        xor dx, dx
        div bx
        mov [step], ax
        movzx eax, word [info + 10h]    ; bytes a line
        movzx edx, word [info + 14h]    ; lines
        mul edx
        mov [total], eax
        mov ax, 4F02h
        mov bx, MODE
        int 10h
        cmp ax, 004Fh
        jne fail

%ifdef DPMI
        mov bx, 1000h                   ; give back what DOS gave beyond 64 KB
        mov ah, 4Ah
        int 21h
        mov ax, 1687h                   ; a 16-bit DPMI client; INT 10h reflected
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
        mov ax, 0002h
        mov bx, 40h
        int 31h
        mov fs, ax
        mov ax, 0002h
        mov bx, 0A000h
        int 31h
        mov [asel], ax
%else
        push 40h
        pop fs
        mov word [asel], 0A000h
%endif
        mov ax, [fs:6Ch]
.sync:  cmp ax, [fs:6Ch]
        je .sync
        mov ax, [fs:6Ch]
%ifndef TICKS
%define TICKS 36
%endif
        add ax, TICKS
        mov [endtick], ax
        mov es, [asel]
frame:  mov ebx, [total]                ; bytes left this frame
        xor dx, dx                      ; window position (granules)
.bank:  push bx
        xor bx, bx
%ifdef FAR
        call far [info + 0Ch]
%elifdef RMI
        mov word [rm + 1Ch], 4F05h      ; DPMI 0300h: INT 10h in real mode
        mov word [rm + 10h], 0
        mov [rm + 14h], dx
        push es
        push ds
        pop es
        mov edi, rm
        mov ax, 0300h
        mov bx, 0010h
        xor cx, cx
        int 31h
        pop es
%else
        mov ax, 4F05h
        int 10h
%endif
        pop bx
        inc word [banks]
        mov ecx, 10000h                 ; 64 KB a bank
        cmp ebx, ecx
        jae .full
        mov ecx, ebx
.full:  sub ebx, ecx
        xor edi, edi
        mov al, [frames]
%ifdef BYTES
        cld
.b:     stosb
        dec ecx
        jnz .b
%else
        mov ah, al
        push ax
        shl eax, 16
        pop ax
        shr ecx, 2
        cld
        rep stosd
%endif
        mov ax, [step]                  ; next bank: 64 KB on
        add [pos], ax
        mov dx, [pos]
        test ebx, ebx
        jnz .bank
        mov word [pos], 0
        xor dx, dx
        inc dword [frames]
        mov ax, [fs:6Ch]
        cmp ax, [endtick]
        jb frame

        mov ax, 3
        int 10h
        mov eax, [frames]
        call dec32
        mov si, msg
        call puts
        movzx eax, word [banks]
        call dec32
        mov si, msg2
        call puts
        mov ax, 4C00h
        int 21h
puts:   lodsb                           ; AH=02h: no pointers, so DPMI hosts pass it
        cmp al, '$'
        je .e
        mov dl, al
        mov ah, 2
        int 21h
        jmp puts
.e:     ret
fail:   mov ax, 3
        int 10h
        mov si, failmsg
        call puts
        mov ax, 4C01h
        int 21h

dec32:  mov ebx, 10                     ; EAX in decimal
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

msg     db ' frames in 2 s, $'
msg2    db ' bank switches (mod 65536)', 13, 10, '$'
failmsg db 'VBE mode failed', 13, 10, '$'
gran    dw 0
step    dw 0
pos     dw 0
banks   dw 0
total   dd 0
frames  dd 0
endtick dw 0
asel    dw 0
entry   dd 0
rm      times 32h db 0
info    times 256 db 0
