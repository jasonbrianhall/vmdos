; DPMILFN.COM [X:]: the long-name API (INT 21h 71xxh) from a DPMI program on
; a VMDOSNET drive, the way DJGPP's C library calls it: INT 31h 0300h with
; the strings in DOS memory. A 16-bit DPMI client; drive X: (default V:).
;   71A0h  volume information of X:\ (bit 14 of BX: long names)
;   714Eh / 714Fh / 71A1h  lists X:\ with long names
;   716Ch  creates "X:\Made by a DPMI program.txt", 7141h deletes it again
; DPMILFN X: /3 enters as a 32-bit client (the host takes EDX, ESI, EDI whole).
; DPMILFN X: /P makes the 71xxh calls as INT 21h straight from protected mode
; instead (DS, ES our selector, 16-bit offsets), as a program for a host that
; translates them does (Windows' DOS box, HDPMI).
; Prints through DOS (0300h, AH=09h), so the output can be redirected:
;   DPMILFN OK  fs=...  names: ...
;   DPMILFN FAIL n AX=xxxx
; Build: nasm -f bin tests/dpmilfn.asm -o DPMILFN.COM
        org 100h
        mov [rmseg], cs
        mov si, 81h                     ; the drive letter on the command line
.arg:   lodsb
        cmp al, ' '
        je .arg
        cmp al, 13
        je .go
        cmp al, '/'
        jne .drv
        lodsb
        cmp al, '3'
        je .c32
        mov byte [direct], 1
        jmp .arg
.c32:   mov byte [bits32], 1
        jmp .arg
.drv:   cmp byte [si], ':'
        jne .arg
        and al, 0DFh
        mov [root], al
        mov [mask], al
        mov [newf], al
        jmp .arg
.go:    mov bx, 1000h
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
        movzx eax, byte [bits32]        ; /3: a 32-bit client (32-bit offsets and frames)
        xor edi, edi
        xor esi, esi
        call far [entry]
        jc quit
        ; --- protected mode: DS = our segment's selector ---
        push ds
        pop es

        mov byte [stage], '1'           ; 71A0h: volume information
        call clr
        mov word [rm_eax], 71A0h
        mov word [rm_edx], root
        mov word [rm_edi], fsname
        mov word [rm_ecx], 32
        call dos
        jc fail
        test word [rm_ebx], 4000h       ; supports long names
        jz fail

        mov byte [stage], '2'           ; 714Eh: find first X:\*
        call clr
        mov word [rm_eax], 714Eh
        mov word [rm_ecx], 0016h        ; may be hidden, system, folders
        mov word [rm_esi], 1            ; DOS date and time
        mov word [rm_edx], mask
        mov word [rm_edi], fdata
        call dos
        jc fail
        mov ax, [rm_eax]
        mov [handle], ax
        mov di, names
.next:  mov si, fdata + 2Ch             ; the long name
.cp:    lodsb
        or al, al
        jz .end
        cmp di, names_end - 4
        jae .full
        mov [di], al
        inc di
        jmp .cp
.end:   mov word [di], ', '
        add di, 2
        call clr                        ; 714Fh: find next
        mov word [rm_eax], 714Fh
        mov ax, [handle]
        mov [rm_ebx], ax
        mov word [rm_esi], 1
        mov word [rm_edi], fdata
        call dos
        jnc .next
.full:  mov byte [di], '$'
        call clr                        ; 71A1h: close the search
        mov word [rm_eax], 71A1h
        mov ax, [handle]
        mov [rm_ebx], ax
        call dos

        mov byte [stage], '3'           ; 716Ch: create a long-named file
        call clr
        mov word [rm_eax], 716Ch
        mov word [rm_ebx], 2            ; read/write
        mov word [rm_edx], 12h          ; create, or truncate
        mov word [rm_esi], newf
        call dos
        jc fail
        mov byte [stage], '6'           ; a handle of its own (not stdin..stdprn)
        mov ax, [rm_eax]
        cmp ax, 5
        jb fail
        mov byte [stage], '3'
        call clr                        ; write to it, close it
        mov [handle], ax
        mov [rm_ebx], ax
        mov word [rm_eax], 4000h
        mov word [rm_ecx], hello_len
        mov word [rm_edx], hello
        call dos
        jc fail
        call clr
        mov word [rm_eax], 3E00h
        mov ax, [handle]
        mov [rm_ebx], ax
        call dos
        jc fail

        mov byte [stage], '4'           ; 7143h: its attributes, by the long name
        call clr
        mov word [rm_eax], 7143h
        mov word [rm_ebx], 0
        mov word [rm_edx], newf
        call dos
        jc fail

        mov byte [stage], '5'           ; 7141h: delete it
        call clr
        mov word [rm_eax], 7141h
        mov word [rm_esi], 0
        mov word [rm_edx], newf
        call dos
        jc fail

.ok:    mov dx, okmsg
        call print
        mov dx, fsname
        call printz
        mov dx, namesmsg
        call print
        mov dx, names
        call print
        mov dx, crlf
        call print
        mov ax, 4C00h
        int 21h

fail:   mov dx, failmsg
        call print
        mov al, [stage]
        mov [failn], al
        mov ax, [rm_eax]
        mov di, failax
        mov cx, 4
.h:     rol ax, 4
        push ax
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .d
        add al, 7
.d:     mov [di], al
        inc di
        pop ax
        loop .h
        mov dx, failn
        call print
        mov ax, 4C01h
        int 21h
quit:   mov dx, nodpmi
        mov ah, 9
        int 21h
        mov ax, 4C02h
        int 21h

clr:    push di                         ; the real-mode call structure: zero, DS = ES = us
        push cx
        push ax
        mov di, rmcs
        mov cx, 25
        xor ax, ax
        rep stosw
        mov ax, [rmseg]
        mov [rm_ds], ax
        mov [rm_es], ax
        pop ax
        pop cx
        pop di
        ret
dos:    cmp byte [direct], 0            ; /P: 71xxh as INT 21h from protected mode
        je .rm
        cmp byte [rm_eax + 1], 71h
        jne .rm
        pushad
        push ds
        pop es
        mov eax, [rm_eax]
        mov ebx, [rm_ebx]
        mov ecx, [rm_ecx]
        mov edx, [rm_edx]
        mov esi, [rm_esi]
        mov edi, [rm_edi]
        int 21h
        mov [rm_eax], eax
        mov [rm_ebx], ebx
        mov [rm_ecx], ecx
        mov [rm_edx], edx
        mov [rm_esi], esi
        mov [rm_edi], edi
        pushf
        pop word [rm_flags]
        push ds
        pop es
        popad
        test byte [rm_flags], 1
        jz .x
        stc
.x:     ret
.rm:    push di                         ; INT 21h in real mode (0300h); CF = its carry
        mov di, rmcs
        mov bx, 0021h
        xor cx, cx
        mov ax, 0300h
        int 31h
        pop di
        jc .e
        test byte [rm_flags], 1
        jz .e
        stc
.e:     ret
print:  call clr                        ; AH=09h: DS:DX ($-terminated)
        mov [rm_edx], dx
        mov word [rm_eax], 0900h
        jmp dos
printz: mov si, dx                      ; an ASCIIZ string: $ in place of the 0
.z:     lodsb
        or al, al
        jnz .z
        mov byte [si - 1], '$'
        jmp print

entry   dd 0
rmseg   dw 0
handle  dw 0
stage   db '0'
direct  db 0
bits32  db 0
root    db 'V:\', 0
mask    db 'V:\*', 0
newf    db 'V:\Made by a DPMI program.txt', 0
hello   db 'written through INT 31h 0300h, AX=716Ch', 13, 10
hello_len equ $ - hello
okmsg   db 'DPMILFN OK  fs=$'
namesmsg db '  names: $'
crlf    db 13, 10, '$'
failmsg db 'DPMILFN FAIL $'
failn   db '0 AX='
failax  db '0000', 13, 10, '$'
nodpmi  db 'no DPMI', 13, 10, '$'

rmcs:                                   ; DPMI real-mode call structure
rm_edi  dd 0
rm_esi  dd 0
rm_ebp  dd 0
        dd 0
rm_ebx  dd 0
rm_edx  dd 0
rm_ecx  dd 0
rm_eax  dd 0
rm_flags dw 0
rm_es   dw 0
rm_ds   dw 0
        dw 0, 0, 0, 0, 0, 0             ; FS GS IP CS SP SS
fsname  times 32 db 0
fdata   times 320 db 0
names   times 400 db 0
names_end:
