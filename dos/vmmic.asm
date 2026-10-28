; VMMIC.COM: which microphone vmdos records from (Sound Blaster recording,
; VMSB MIC).
;   VMMIC          list the microphones; * marks the one in use
;   VMMIC 1        the HD Audio one (mic jack, else internal mic, else line
;                  in; only while sound plays through HD Audio analog)
;   VMMIC 2        the USB one (a USB microphone, headset or webcam)
;   VMMIC OFF      none (recording gets silence)
;   VMMIC AUTO     a USB one when it's plugged in, else HD Audio (the default)
; Exits with errorlevel = the one in use (0 none, 1, 2).
; (INT 2Fh AX=5642h, as VMSB: BX=4 lists into ES:DI, BX=5 picks CX;
;  returns BX = 'VM'.)
        cpu 286
        org 100h
        mov si, 81h
.skip:  lodsb
        cmp al, ' '
        je .skip
        cmp al, 9
        je .skip
        cmp al, 13
        je list
        mov cx, 1
        cmp al, '1'
        je pick
        inc cx
        cmp al, '2'
        je pick
        xor cx, cx
        cmp al, '0'
        je pick
        or al, 20h
        cmp al, 'o'
        je pick
        mov cx, 0FFFFh
        cmp al, 'a'
        je pick
        mov dx, msg_usage
        call puts
        mov ax, 4C01h
        int 21h

pick:   mov ax, 5642h
        mov bx, 5
        int 2Fh
        cmp bx, 564Dh
        jne novm
        or ax, ax
        jnz list
        mov dx, msg_nomic
        call puts

list:   mov ax, 5642h
        mov bx, 4
        push cs
        pop es
        mov di, names
        int 2Fh
        cmp bx, 564Dh
        jne novm
        mov [inuse], al
        mov [choice], cx
        mov dx, msg_head
        call puts
        mov bl, 1                       ; 1 HD Audio, 2 USB
        mov si, names
.mic:   mov dl, ' '
        cmp bl, [inuse]
        jne .mark
        mov dl, '*'
.mark:  call putc
        mov dl, ' '
        call putc
        mov dl, bl
        add dl, '0'
        call putc
        mov dx, msg_sp2
        call puts
        cmp byte [si], 0
        jne .name
        mov dx, msg_hda_none
        cmp bl, 1
        je .p
        mov dx, msg_usb_none
.p:     call puts
        jmp .next
.name:  push si
.c:     lodsb
        or al, al
        jz .e
        mov dl, al
        call putc
        jmp .c
.e:     pop si
        mov dx, msg_crlf
        call puts
.next:  add si, 64
        inc bl
        cmp bl, 3
        jb .mic
        cmp byte [inuse], 0
        jne .ch
        mov dx, msg_inuse0
        call puts
.ch:    mov dx, msg_auto
        cmp word [choice], 0FFFFh
        je .s
        mov dx, msg_off
        cmp word [choice], 0
        je .s
        mov dx, msg_set
.s:     call puts
        mov al, [inuse]
        mov ah, 4Ch
        int 21h

novm:   mov dx, msg_novm
        call puts
        mov ax, 4C00h
        int 21h

putc:   push ax
        mov ah, 2
        int 21h
        pop ax
        ret
puts:   push ax
        mov ah, 9
        int 21h
        pop ax
        ret

msg_head:     db 'Microphones (* in use)', 13, 10, '$'
msg_sp2:      db '  $'
msg_crlf:     db 13, 10, '$'
msg_hda_none: db '(HD Audio: none, or sound isn', 39, 't playing through its analog output)', 13, 10, '$'
msg_usb_none: db '(USB: none plugged in)', 13, 10, '$'
msg_inuse0:   db 'None in use: recording gets silence.', 13, 10, '$'
msg_auto:     db 'Picked automatically: USB when plugged in (VMMIC n to choose).', 13, 10, '$'
msg_off:      db 'Microphone off (VMMIC AUTO to turn it back on).', 13, 10, '$'
msg_set:      db 'Chosen with VMMIC (VMMIC AUTO to let vmdos pick).', 13, 10, '$'
msg_nomic:    db 'VMMIC: that microphone isn', 39, 't there', 13, 10, '$'
msg_novm:     db 'VMMIC: not running under vmdos', 13, 10, '$'
msg_usage:    db 'usage: VMMIC         list the microphones', 13, 10
              db '       VMMIC 1 | 2   use microphone 1 (HD Audio) or 2 (USB)', 13, 10
              db '       VMMIC OFF     no microphone', 13, 10
              db '       VMMIC AUTO    USB when plugged in, else HD Audio', 13, 10, '$'
inuse:  db 0
choice: dw 0
names:  times 2 * 64 db 0
