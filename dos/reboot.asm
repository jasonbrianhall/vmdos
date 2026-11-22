; REBOOT.COM: get everything onto the disk, then restart the machine.
; POWEROFF.COM (-DPOWEROFF): the same, then off (vmdos: ACPI S5) instead of 5.
; REBOOT /S [n]: 1-3, then a soft restart: vmdos starts DOS again from C:
;   without resetting the PC (no firmware or GRUB; vmdos's CDs, floppies and
;   settings stay), typing FDCONFIG.SYS's menu choice n if given (INT 2Fh
;   AX=5657h BX=2 CL=n). Not under vmdos: a full restart as usual.
;   1. DOS's buffers out (INT 21h AH=0Dh)
;   2. a disk cache's (SMARTDRV and compatibles: INT 2Fh AX=4A10h BX=1)
;   3. DOS's buffers again (anything the cache flush left behind)
;   4. vmdos's write-back cache out to the disk, waiting for it
;      (INT 2Fh AX=5657h; BX = 'VM' back when vmdos did it)
;   5. restart: INT 19h, then the keyboard controller, then the reset vector
; nasm -f bin -o REBOOT.COM REBOOT.ASM
        cpu 386
        org 100h

%ifndef POWEROFF
        mov si, 81h                     ; /S (or -S) and a menu choice 1-9
.arg:   lodsb
        cmp al, 13
        je .args
        cmp al, '/'
        je .sw
        cmp al, '-'
        je .sw
        cmp al, '1'
        jb .arg
        cmp al, '9'
        ja .arg
        mov [menu], al
        jmp .arg
.sw:    lodsb
        or al, 20h
        cmp al, 's'
        jne .usage
        mov byte [soft], 1
        jmp .arg
.usage: mov dx, usage
        mov ah, 09h
        int 21h
        mov ax, 4C01h
        int 21h
.args:  cmp byte [soft], 0
        je .hardmsg
        mov dx, msg_soft
        jmp .say
.hardmsg:
%endif
        mov dx, msg
.say:   mov ah, 09h
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
%else
        cmp byte [soft], 0
        je .hard
        mov ax, 5657h                   ; 4+5 soft: vmdos restarts DOS when we wait
        mov bx, 2
        mov cl, [menu]
        call vmapi
        cmp bx, 564Dh
        jne .nosoft
.idle:  sti                             ; vmdos takes over at the next interrupt
        hlt
        jmp .idle
.nosoft:
        mov dx, nosoft
        mov ah, 09h
        int 21h
.hard:
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
msg_soft: db 'Restarting DOS (soft: the PC is not reset)...', 13, 10, '$'
nosoft: db 'Not under vmdos: restarting the PC.', 13, 10, '$'
usage:  db 'REBOOT        write the disk caches out, then restart the PC', 13, 10
        db 'REBOOT /S     ... then restart DOS on vmdos as it is (the PC is not', 13, 10
        db '              reset; its CDs, floppies and settings stay)', 13, 10
        db 'REBOOT /S 2   ... choosing 2 at FDCONFIG.SYS', 39, 's menu', 13, 10, '$'
soft:   db 0
menu:   db 0
%endif
done:   db 'Disk cache written.', 13, 10, '$'
