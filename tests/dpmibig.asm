; DPMIBIG.COM: DPMI memory beyond 16 MiB, the way DJGPP programs (Quake) use it.
; A 16-bit DPMI client: reads the free-memory information (0500h), allocates
; 64 MiB (0501h), writes both ends through a 4 GiB selector, grows the block
; to 96 MiB (0503h, as DJGPP's sbrk does) and checks the data came along.
; Writes the result on the bottom line of the screen:
;   DPMIBIG OK  free=xxxxxxxx (pages, max locked)  lin=xxxxxxxx  grown=xxxxxxxx
;   DPMIBIG FAIL n
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
        push ds
        pop es
        mov byte [stage], '1'
        mov di, meminfo                 ; 0500h: free memory information
        mov ax, 0500h
        int 31h
        jc fail
        mov eax, [meminfo + 8]          ; max locked allocation, pages
        mov [free_pages], eax
        cmp eax, (96 << 20) >> 12
        jb fail
        mov byte [stage], '2'
        mov bx, 0400h                   ; 0501h: 64 MiB
        xor cx, cx
        mov ax, 0501h
        int 31h
        jc fail
        mov [lin], cx
        mov [lin + 2], bx
        mov [handle], di
        mov [handle + 2], si
        mov byte [stage], '3'
        xor ax, ax                      ; a descriptor for it
        mov cx, 1
        int 31h
        jc fail
        mov [sel], ax
        call setbase
        jc fail
        mov bx, [sel]                   ; limit 4 GiB
        mov cx, 0FFFFh
        mov dx, 0FFFFh
        mov ax, 0008h
        int 31h
        jc fail
        mov byte [stage], '4'
        mov es, [sel]
        mov ebx, 0
        mov dword [es:ebx], 11223344h
        mov ebx, (64 << 20) - 4
        mov dword [es:ebx], 55667788h
        mov byte [stage], '5'
        push ds                         ; 0503h: grow to 96 MiB
        pop es
        mov bx, 0600h
        xor cx, cx
        mov si, [handle + 2]
        mov di, [handle]
        mov ax, 0503h
        int 31h
        jc fail
        mov [lin2], cx
        mov [lin2 + 2], bx
        mov [lin], cx
        mov [lin + 2], bx
        call setbase
        jc fail
        mov byte [stage], '6'
        mov es, [sel]
        mov ebx, 0
        cmp dword [es:ebx], 11223344h
        jne fail
        mov ebx, (64 << 20) - 4
        cmp dword [es:ebx], 55667788h
        jne fail
        mov ebx, (96 << 20) - 4
        mov dword [es:ebx], 99AABBCCh
        cmp dword [es:ebx], 99AABBCCh
        jne fail
        push ds
        pop es
        mov si, okmsg
        call show
        mov eax, [free_pages]
        call hex
        mov si, linmsg
        call show
        mov eax, [first_lin]
        call hex
        mov si, grownmsg
        call show
        mov eax, [lin2]
        call hex
        jmp done
fail:   push ds
        pop es
        mov si, failmsg
        call show
        mov al, [stage]
        call putc
done:   mov ax, 4C00h
        int 21h
quit:   mov dx, nodpmi
        mov ah, 9
        int 21h
        mov ax, 4C01h
        int 21h

setbase:                                ; base of [sel] = [lin]
        mov eax, [lin]
        cmp dword [first_lin], 0
        jne .s
        mov [first_lin], eax
.s:     mov bx, [sel]
        mov cx, [lin + 2]
        mov dx, [lin]
        mov ax, 0007h
        int 31h
        ret

; text at the bottom line of the screen, through a selector for B800h
show:   push es
        call scr
.l:     lodsb
        or al, al
        jz .e
        call putc
        jmp .l
.e:     pop es
        ret
putc:   push es
        push di
        push ax
        call scr
        mov di, [col]
        pop ax
        mov ah, 0Eh
        stosw
        mov [col], di
        pop di
        pop es
        ret
scr:    push ax
        push bx
        mov ax, 0002h
        mov bx, 0B800h
        int 31h
        mov es, ax
        pop bx
        pop ax
        ret
hex:    mov cx, 8
.h:     rol eax, 4
        push eax
        and al, 15
        add al, '0'
        cmp al, '9'
        jbe .d
        add al, 7
.d:     call putc
        pop eax
        loop .h
        ret

entry   dd 0
sel     dw 0
lin     dd 0
lin2    dd 0
first_lin dd 0
handle  dd 0
free_pages dd 0
stage   db '0'
col     dw 160 * 24
okmsg   db 'DPMIBIG OK  free pages=', 0
linmsg  db '  lin=', 0
grownmsg db '  grown=', 0
failmsg db 'DPMIBIG FAIL ', 0
nodpmi  db 'no DPMI', 13, 10, '$'
meminfo times 48 db 0
