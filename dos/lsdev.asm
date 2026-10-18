; LSPCI.COM and LSUSB.COM (one source: nasm -DLSPCI / -DLSUSB): the PCI
; devices, or the USB controllers and what's plugged into them, as vmdos
; sees them. DOS can't look itself (vmdos keeps the PCI configuration ports
; and drives the USB controllers), so vmdos makes the listing.
;   LSPCI [/V] [/X]     one line a device; /V subsystem, flags, BARs, IRQ,
;                       capabilities; /X the first 64 bytes of configuration
;                       space in hex
;   LSUSB [/V]          each controller and its devices; /V descriptor classes
; Output goes to standard output (LSPCI > PCI.TXT, LSPCI /V | MORE).
; (INT 2Fh AX=5649h: BL = 0 PCI / 1 USB, BH = flags, EDX = position, CX = size
;  of the buffer at ES:DI; returns CX = bytes, EDX = next position, BX = 'VM'.)
        cpu 386
        org 100h

BUFSZ   equ 4096

%ifdef LSPCI
WHAT    equ 0
%else
WHAT    equ 1
%endif

        mov si, 81h
.opt:   lodsb
        cmp al, ' '
        je .opt
        cmp al, 9
        je .opt
        cmp al, 13
        je dump
        cmp al, '/'
        je .sw
        cmp al, '-'
        jne usage
.sw:    lodsb
        or al, 20h
        cmp al, 'v'
        jne .x
        or byte [flags], 1
        jmp .opt
.x:
%ifdef LSPCI
        cmp al, 'x'
        jne usage
        or byte [flags], 2
        jmp .opt
%else
        jmp usage
%endif

dump:   xor edx, edx
        mov byte [first], 1
.next:  push cs
        pop es
        mov di, buf
        mov cx, BUFSZ
        mov bl, WHAT
        mov bh, [flags]
        mov ax, 5649h
        call vmapi
        cmp byte [first], 0
        je .got
        cmp bx, 564Dh                   ; 'VM'
        jne notvm
        mov byte [first], 0
.got:   jcxz .done
        push edx
        mov ah, 40h                     ; write CX bytes to standard output
        mov bx, 1
        mov dx, buf
        int 21h
        pop edx
        jc .done
        jmp .next
.done:  mov ax, 4C00h
        int 21h

notvm:  mov dx, msg_notvm
        jmp fail
usage:  mov dx, msg_usage
fail:   mov ah, 9
        int 21h
        mov ax, 4C01h
        int 21h

%ifdef LSPCI
msg_notvm db 'LSPCI: not running under vmdos (or a vmdos too old for LSPCI)', 13, 10, '$'
msg_usage db 'LSPCI [/V] [/X]   the PCI devices, as vmdos sees them', 13, 10
          db '  /V   subsystem, flags, BARs, IRQ, capabilities', 13, 10
          db '  /X   the first 64 bytes of configuration space in hex', 13, 10, '$'
%else
msg_notvm db 'LSUSB: not running under vmdos (or a vmdos too old for LSUSB)', 13, 10, '$'
msg_usage db 'LSUSB [/V]   the USB controllers and devices vmdos found', 13, 10
          db '  /V   descriptor classes and slots too', 13, 10, '$'
%endif
%include "vmapi.inc"
first   db 0
flags   db 0
buf:
