; REBOOT.COM: restart the machine, without losing what was just saved.
;   REBOOT
; DOS writes out its buffers (INT 21h AH=0Dh), SMARTDRV (if loaded) its
; cache (INT 2Fh AX=4A10h BX=0001h); then INT 19h, which vmdos turns into a
; restart after its own disk cache's gathered writes are on the disk. If
; that comes back (another BIOS): the keyboard controller's reset line,
; then the reset vector.
; Build: nasm -f bin reboot.asm -o REBOOT.COM

        org 0x100

        mov dx, msg
        mov ah, 9
        int 0x21
        mov ah, 0x0D                    ; DOS: buffers out
        int 0x21
        mov ax, 0x4A10                  ; SMARTDRV: flush (no-op without it)
        mov bx, 0x0001
        int 0x2F
        mov ah, 0x0D
        int 0x21
        int 0x19                        ; vmdos: flush its writes and restart
        cli
        mov cx, 0xFFFF
.kbc:   in al, 0x64                     ; wait for the controller's input buffer
        test al, 2
        loopnz .kbc
        mov al, 0xFE                    ; pulse the reset line
        out 0x64, al
        mov cx, 0xFFFF
.wait:  loop .wait
        jmp 0xFFFF:0x0000

msg     db 'Restarting...', 13, 10, '$'
