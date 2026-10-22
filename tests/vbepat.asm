; VBEPAT.COM: a VESA mode filled through the banked window, every byte of
; the 4 MB set to a known function of its address, then optionally a wider
; logical line (4F06h) and a display start (4F07h). Build with
;   nasm -DMODE=111h [-DLW=pixels] [-DSX=x -DSY=y] [-DPMI]
; The screen then shows bytes from (SY * pitch + SX * Bpp) on: byte at A is
; low 8 bits of (A >> 2) + 37 * (A >> 12). -DPMI banks and scrolls through
; the protected-mode interface (4F0Ah) instead, from a 16-bit DPMI client;
; -DRMI from a DPMI client too, but through INT 10h simulated in real mode
; (DPMI 0300h), as SciTech's VBE library (UniVBE's VBETEST) calls it.
        org 100h
        cpu 386
%ifndef SX
%define SX 0
%define SY 0
%endif
        mov bx, 1000h
        mov ah, 4Ah
        int 21h
        mov ax, 4F01h                   ; bytes per pixel, for the scroll
        mov cx, MODE
        mov di, info
        int 10h
        mov ax, 4F02h
        mov bx, MODE
        int 10h
        cmp ax, 004Fh
        jne quit
%ifdef LW
        mov ax, 4F06h
        xor bl, bl
        mov cx, LW
        int 10h
        cmp ax, 004Fh
        jne quit
%endif
        mov ax, 4F06h                   ; the pitch it has now
        mov bl, 1
        int 10h
        mov [pitch], bx
%ifdef RMI
%define DPMI
%endif
%ifdef PMI
%define DPMI
%endif
%ifdef DPMI
%ifdef PMI
        mov ax, 4F0Ah
        xor bx, bx
        int 10h
        cmp ax, 004Fh
        jne quit
        push ds
        push es
        pop ds
        mov si, di
        push cs
        pop es
        mov di, pmi
        rep movsb
        pop ds
%endif
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
        mov ax, 0002h
        mov bx, 0A000h
        int 31h
        jc quit
        mov es, ax
%else
        mov ax, 0A000h
        mov es, ax
%endif
        xor ebp, ebp                    ; bank
.bank:  xor bx, bx
        mov dx, bp
%ifdef PMI
        mov ax, pmi
        add ax, [pmi]
        call ax
%elifdef RMI
        mov word [rm + 1Ch], 4F05h      ; EAX
        mov [rm + 10h], bx              ; EBX
        mov [rm + 14h], dx              ; EDX
        call rmint
%else
        mov ax, 4F05h
        int 10h
%endif
        xor di, di
.px:    mov eax, ebp
        shl eax, 16
        mov ax, di                      ; eax = address
        mov ebx, eax
        shr ebx, 2
        shr eax, 12
        imul eax, eax, 37
        add eax, ebx
        mov [es:di], al
        inc di
        jnz .px
        inc bp
        cmp bp, 64
        jb .bank
        ; display start (SX, SY)
        movzx eax, word [pitch]
        mov ecx, SY
        imul eax, ecx
        movzx ecx, byte [info + 19h]    ; bits per pixel
        add ecx, 7
        shr ecx, 3
        imul ecx, ecx, SX
        add eax, ecx                    ; byte offset
%ifdef PMI
        mov ecx, eax                    ; CX:DX = offset / 4, VBE 3: low bits in DX 14-15
        shr ecx, 2
        mov edx, ecx
        shr edx, 16
        and eax, 3
        shl eax, 14
        or edx, eax
        and ecx, 0FFFFh
        xor bx, bx
        mov ax, pmi
        add ax, [pmi + 2]
        call ax
%elifdef RMI
        mov word [rm + 1Ch], 4F07h
        mov word [rm + 10h], 0
        mov word [rm + 18h], SX         ; ECX
        mov word [rm + 14h], SY
        call rmint
%else
        mov ax, 4F07h
        xor bx, bx
        mov cx, SX
        mov dx, SY
        int 10h
%endif
        sti
.hang:  hlt
        jmp .hang
rmint:  push es                          ; DPMI 0300h: INT 10h in real mode, regs at ds:rm
        push ds
        pop es
        mov edi, rm
        mov ax, 0300h
        mov bx, 0010h
        xor cx, cx
        int 31h
        pop es
        ret
quit:   mov ax, 0003h
        int 10h
        mov ax, 4C01h
        int 21h
pitch   dw 0
rm      times 32h db 0
entry   dd 0
pmi     times 64 db 0
info    times 256 db 0
