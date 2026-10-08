; VMEMS.SYS - makes vmdos's built-in EMS (LIM 4.0 expanded memory) visible
; the way EMM386 is: a device named EMMXXXX0, with INT 67h pointing into this
; driver's segment (programs check for the name at INT 67h's segment:000Ah).
; The EMS functions themselves run in the vmdos monitor (src/ems.c).
;   DEVICE=C:\VMDOS\VMEMS.SYS          (in FDCONFIG.SYS, after VMXMS.SYS)
; ems=MB on the kernel command line sets the size (0: no EMS; then this
; driver doesn't stay).
; Build: nasm -f bin -o VMEMS.SYS vmems.asm
        org 0

header: dd -1                   ; next driver
        dw 0xC000               ; character device, IOCTL supported
        dw strategy
        dw interrupt
        db "EMMXXXX0"

req:    dd 0
stub:   dd 0                    ; F000:int67, the monitor's trap

int67:  jmp far [cs:stub]

strategy:
        mov [cs:req], bx
        mov [cs:req+2], es
        retf

interrupt:
        push ax
        push bx
        push es
        les bx, [cs:req]
        mov word [es:bx+3], 0x0100      ; done (open, close, IOCTL, output status: all fine)
        cmp byte [es:bx+2], 0           ; INIT?
        jne .out
        call init
.out:   pop es
        pop bx
        pop ax
        retf

resident_end:

; ---- discarded after INIT ----
init:   push ds
        push cx
        push dx
        push cs
        pop ds
        push es
        push bx
        mov ax, 0x5645                  ; the monitor: EMS there? BX = pages, DX = frame
        int 0x2F
        mov [frame], dx
        mov [pages], bx
        pop bx
        pop es
        or ax, ax
        jnz .none
        mov [es:bx+14], word resident_end
        mov [es:bx+16], cs
        mov ax, [pages]                 ; KiB = pages * 16
        mov cl, 4
        shl ax, cl
        mov [kib], ax
        push es
        mov ax, 0xF000                  ; INT 67h -> here -> F000:(pointer table 022Eh)
        mov es, ax
        mov ax, [es:0x22E]
        pop es
        mov [stub], ax
        mov word [stub+2], 0xF000
        mov dx, int67
        mov ax, 0x2567
        int 0x21
        mov dx, msg1
        mov ah, 9
        int 0x21
        mov ax, [kib]
        call number
        mov dx, msg2
        mov ah, 9
        int 0x21
        jmp .done
.none:  mov word [es:bx+14], 0          ; not under vmdos, or ems=0: don't stay
        mov [es:bx+16], cs
.done:  pop dx
        pop cx
        pop ds
        ret

number: xor cx, cx                      ; AX in decimal
        mov bx, 10
.d:     xor dx, dx
        div bx
        push dx
        inc cx
        or ax, ax
        jnz .d
.p:     pop dx
        add dl, '0'
        mov ah, 2
        int 0x21
        loop .p
        ret

kib:    dw 0
pages:  dw 0
frame:  dw 0
msg1:   db "vmdos EMS 4.0: $"
msg2:   db " KiB expanded memory, page frame E000", 13, 10, "$"
