; PCI32.COM: the PCI BIOS as 32-bit code finds it (Windows' PCI.VXD): looks
; for the BIOS32 service directory ("_32_", checksum) in E0000h-FFFFFh,
; asks it for "$PCI", then calls that with AX=B101h, B102h (the Voodoo 3,
; 121Ah:0005h) and B10Ah (its BAR0), through DPMI selectors, and prints
; what came back.  nasm -f bin -o PCI32.COM pci32.asm
        cpu 686
        bits 16
        org 100h

start:  mov [rmseg], cs
        mov bx, (progend - start + 100h + 15) / 16 + 1
        mov ah, 4Ah
        int 21h
        mov ax, 1687h
        int 2Fh
        test ax, ax
        jnz fail
        mov [entry], di
        mov [entry + 2], es
        mov bx, si
        mov ah, 48h
        int 21h
        jc fail
        mov es, ax
        mov ax, 1
        call far [entry]
        jc fail

        mov bx, 0                       ; selectors: flat data, flat code32, F0000h code32
        call desc
        mov [flat], ax
        mov cx, 0C0F2h
        call rights
        mov bx, 0
        call desc
        mov [code0], ax
        mov cx, 0C0FAh
        call rights
        mov bx, 000Fh                   ; base 000F0000h
        call desc
        mov [codef], ax
        mov cx, 0C0FAh
        call rights
        mov fs, [flat]

        mov esi, 0E0000h                ; the directory: 16-byte aligned, checksum 0
.scan:  cmp dword [fs:esi], '_32_'
        jne .next
        xor al, al
        xor ecx, ecx
.sum:   add al, [fs:esi + ecx]
        inc cx
        cmp cx, 16
        jb .sum
        test al, al
        jz .found
.next:  add esi, 16
        cmp esi, 100000h
        jb .scan
        mov dx, m_nodir
        jmp msg_exit

.found: mov eax, [fs:esi + 4]           ; its entry, physical
        mov [b32off], eax
        mov ax, [code0]
        mov [b32sel], ax
        mov eax, '$PCI'
        xor ebx, ebx
        o32 call far [b32off]
        test al, al
        jnz .nosvc
        add ebx, edx                    ; the service: base + entry (base F0000h: our selector)
        sub ebx, 0F0000h
        mov [pcioff], ebx
        mov ax, [codef]
        mov [pcisel], ax

        mov ax, 0B101h
        o32 call far [pcioff]
        jc .err
        cmp edx, 20494350h
        jne .err
        mov ax, 0B102h                  ; find 121Ah:0005h
        mov cx, 0005h
        mov dx, 121Ah
        xor si, si
        o32 call far [pcioff]
        jc .err
        mov [devfn], bx
        mov ax, 0B10Ah                  ; its BAR0
        mov di, 10h
        o32 call far [pcioff]
        jc .err
        mov eax, ecx
        mov di, m_bar
        call hex8
        mov ax, [devfn]
        movzx eax, ax
        mov di, m_dev
        call hex8
        mov dx, m_ok
        jmp msg_exit
.nosvc: mov dx, m_nosvc
        jmp msg_exit
.err:   mov dx, m_err
msg_exit:                               ; DX: the message, printed by DOS in real mode (DPMI 0300h)
        mov [rmregs + 14h], dx          ; EDX
        mov word [rmregs + 1Ch], 0900h  ; EAX
        mov ax, [rmseg]
        mov [rmregs + 24h], ax          ; DS
        push ds
        pop es
        xor edi, edi
        mov di, rmregs
        mov ax, 0300h
        mov bx, 21h
        xor cx, cx
        int 31h
        mov ax, 4C00h
        int 21h
fail:   mov dx, m_fail                  ; still in real mode
        mov ah, 9
        int 21h
        mov ax, 4C01h
        int 21h

desc:   push bx                         ; a descriptor, base BX:0000, 4 GB
        xor ax, ax
        mov cx, 1
        int 31h
        pop cx
        push ax
        mov bx, ax
        xor dx, dx
        mov ax, 7
        int 31h
        mov ax, 8
        mov cx, 0FFFFh
        mov dx, 0FFFFh
        int 31h
        pop ax
        ret
rights: mov bx, ax                      ; CX: access rights
        push ax
        mov ax, 9
        int 31h
        pop ax
        ret

hex8:   mov cx, 8                       ; EAX as 8 hex digits at DI
.h:     rol eax, 4
        mov bl, al
        and bx, 0Fh
        mov bl, [hexd + bx]
        mov [di], bl
        inc di
        loop .h
        ret

rmregs  times 32h db 0
rmseg   dw 0
hexd    db '0123456789ABCDEF'
entry   dd 0
b32off  dd 0
b32sel  dw 0
pcioff  dd 0
pcisel  dw 0
devfn   dw 0
flat    dw 0
code0   dw 0
codef   dw 0
m_ok    db 'PCI32 OK devfn '
m_dev   db '00000000 BAR0 '
m_bar   db '00000000', 13, 10, '$'
m_nodir db 'PCI32: no BIOS32 directory', 13, 10, '$'
m_nosvc db 'PCI32: no $PCI service', 13, 10, '$'
m_err   db 'PCI32: PCI BIOS call failed', 13, 10, '$'
m_fail  db 'PCI32: no DPMI', 13, 10, '$'
progend:
