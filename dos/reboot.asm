; REBOOT.COM: get everything onto the disk, then restart the machine.
; POWEROFF.COM (-DPOWEROFF): the same, then off (vmdos: ACPI S5) instead of 5.
;   1. DOS's buffers out (INT 21h AH=0Dh)
;   2. a disk cache's (SMARTDRV and compatibles: INT 2Fh AX=4A10h BX=1)
;   3. DOS's buffers again (anything the cache flush left behind)
;   4. vmdos's write-back cache out to the disk, waiting for it
;      (INT 2Fh AX=5657h; BX = 'VM' back when vmdos did it)
;   5. restart: INT 19h, then the keyboard controller, then the reset vector
; nasm -f bin -o REBOOT.COM REBOOT.ASM
        cpu 386
        org 100h

        mov dx, msg
        mov ah, 09h
        int 21h

        mov ah, 0Dh                     ; 1. DOS's buffers
        int 21h

        mov ax, 4A10h                   ; 2. SMARTDRV: commit the cache
        mov bx, 1
        int 2Fh

        mov ah, 0Dh                     ; 3. DOS's buffers again
        int 21h

%ifdef POWEROFF
        mov ax, 5657h                   ; 4+5. vmdos: its write cache out, then off (ACPI S5)
        mov bx, 1
        call vmapi
        mov dx, cant                    ; back: not under vmdos, or no way to turn it off
        mov ah, 09h
        int 21h
        mov ax, 4C01h
        int 21h
%endif
        mov ax, 5657h                   ; 4. vmdos's write cache, synchronously
        xor bx, bx
        call vmapi
        cmp bx, 564Dh                   ; 'VM': vmdos has it on the disk
        jne .reboot
        mov dx, done
        mov ah, 09h
        int 21h

.reboot:
        int 19h                         ; 5. restart (vmdos: a full reset)
        cli
        mov cx, 0FFFFh                  ; else the keyboard controller's reset line
.kbc:   in al, 64h
        test al, 2                      ; input buffer empty?
        loopnz .kbc
        mov al, 0FEh
        out 64h, al
        mov cx, 0FFFFh
.wait:  loop .wait
        jmp 0FFFFh:0000h                ; else the reset vector

; vmdos's own INT 2Fh calls go straight to the INT 2Fh stub in vmdos's BIOS
; (F000h: HLT; db 2Fh), not through the INT 2Fh chain: under VMHV, DOS's
; handler would end the chain before vmdos saw the call. Not under vmdos:
; a plain INT 2Fh (AX=5657h is nobody else's: BX stays 0).
vmapi:  push ds
        push si
        push ax
        mov ax, 0F000h
        mov ds, ax
        mov si, [2Fh * 2]               ; INT 2Fh's entry in vmdos's BIOS
        cmp si, 8000h                   ; (its stubs are all below 8000h)
        jae .chain
        cmp word [si], 2FF4h            ; HLT; db 2Fh
        jne .chain
        mov [cs:vmapi_ptr], si
        mov word [cs:vmapi_ptr + 2], 0F000h
        pop ax
        pop si
        pop ds
        pushf                           ; as INT does
        cli
        call far [cs:vmapi_ptr]
        ret
.chain: pop ax
        pop si
        pop ds
        int 2Fh
        ret

vmapi_ptr: dd 0
%ifdef POWEROFF
msg:    db 'Turning off...', 13, 10, '$'
cant:   db 'This machine could not be turned off (not under vmdos, or no ACPI power-off).', 13, 10, '$'
%else
msg:    db 'Restarting...', 13, 10, '$'
%endif
done:   db 'Disk cache written.', 13, 10, '$'
