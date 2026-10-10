; VMXMS.SYS - makes vmdos's built-in XMS driver visible the way HIMEM is:
; a device named XMSXXXX0 that hooks INT 2Fh (AX=4300h/4310h). The XMS
; functions themselves run in the vmdos monitor. Load it first:
;   DEVICE=C:\VMDOS\VMXMS.SYS
; Build: nasm -f bin -o VMXMS.SYS vmxms.asm
        org 0

header: dd -1                   ; next driver
        dw 0x8000               ; character device
        dw strategy
        dw interrupt
        db "XMSXXXX0"

req:    dd 0
old2f:  dd 0
entry:  dd 0                    ; the monitor's XMS entry point

int2f:  cmp ax, 0x4300
        jne .e
        mov al, 0x80
        iret
.e:     cmp ax, 0x4310
        jne .chain
        les bx, [cs:entry]
        iret
.chain: jmp far [cs:old2f]

strategy:
        mov [cs:req], bx
        mov [cs:req+2], es
        retf

interrupt:
        push ax
        push bx
        push es
        les bx, [cs:req]
        cmp byte [es:bx+2], 0           ; INIT?
        jne .unknown
        mov word [es:bx+3], 0x0100      ; status: done
        mov word [es:bx+14], resident_end
        mov [es:bx+16], cs
        call init
        jnc .out
        mov word [es:bx+14], 0          ; no XMS here (vmdos noxms, or not vmdos): don't stay
        jmp .out
.unknown:
        mov word [es:bx+3], 0x8103      ; done, error: unknown command
.out:   pop es
        pop bx
        pop ax
        retf

resident_end:

; ---- discarded after INIT ----
init:   push ds
        push dx
        push cs
        pop ds
        mov ax, 0x4300                  ; is there XMS? (vmdos's noxms hides it)
        int 0x2F
        cmp al, 0x80
        je .xms
        mov dx, msg_none
        mov ah, 9
        int 0x21
        pop dx
        pop ds
        stc
        ret
.xms:   mov ax, 0x4310                  ; the monitor answers this at the INT trap
        int 0x2F
        mov [entry], bx
        mov [entry+2], es
        mov ax, 0x352F
        int 0x21
        mov [old2f], bx
        mov [old2f+2], es
        mov dx, int2f
        mov ax, 0x252F
        int 0x21
        mov dx, msg
        mov ah, 9
        int 0x21
        pop dx
        pop ds
        clc
        ret
msg:    db "vmdos XMS 3.0 (HMA, extended memory, UMBs)", 13, 10, "$"
msg_none: db "vmdos XMS: hidden (noxms), not loaded", 13, 10, "$"
