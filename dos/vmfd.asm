; VMFD.COM: change the disk in vmdos's floppy drive A: or B: (disk images
; on any drive: on C: or a VMHD drive vmdos uses the file itself; on any
; other drive, such as a CD or a VMDOSNET share, VMFD reads it in through
; DOS and vmdos holds a read-only copy).
;   VMFD                          list the drives
;   VMFD A: C:\DISKS\DISK1.IMG    put an image in A: (relative paths work)
;   VMFD B: GAME2.IMG /R          ... in B:, read-only
;   VMFD DISK1.IMG                drive A:
;   VMFD A: /E                    take the disk out of A:
;   VMFD A: GAME.IMG /BOOT        restart the PC from that disk (a self-
;                                 booting game: no DOS); VMFD A: /BOOT
;                                 boots what is in A: already
        org 100h
        mov si, 81h
        call skipsp
        cmp al, 13
        je list
        mov byte [unit], 0
        mov al, [si]                    ; "A:" / "B:" first?
        or al, 20h
        cmp al, 'a'
        jb .arg
        cmp al, 'b'
        ja .arg
        cmp byte [si + 1], ':'
        jne .arg
        cmp byte [si + 2], ' '
        ja .arg
        sub al, 'a'
        mov [unit], al
        add si, 2
        call skipsp
        cmp al, 13
        je usage
.arg:   cmp byte [si], '/'              ; /E: eject, /BOOT: boot what's in it
        jne path
        mov al, [si + 1]
        or al, 20h
        cmp al, 'b'
        je boot
        cmp al, 'e'
        jne usage
        mov ax, 5646h
        mov bx, 2
        mov cl, [unit]
        call vmapi
        cmp ax, 5646h
        je novm
list:   mov ax, 5646h
        xor bx, bx
        mov di, buf
        call vmapi
        cmp ax, 5646h
        je novm
        mov dx, buf
        jmp short say
bad:    mov dx, msg_bad
        jmp short say
usage:  mov dx, msg_use
        jmp short say
novm:   mov dx, msg_novm
say:    mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

path:   mov di, pbuf                    ; token -> ASCIIZ
.cp:    lodsb
        cmp al, ' '
        jbe .end
        stosb
        cmp di, pbuf + 127
        jb .cp
.end:   mov byte [di], 0
        dec si
        mov byte [ro], 0
.opt:   call skipsp                     ; /R read-only, /BOOT boot it
        cmp al, '/'
        jne .go
        mov al, [si + 1]
        or al, 20h
        cmp al, 'r'
        je .r
        cmp al, 'b'
        jne usage
        mov byte [doboot], 1
        jmp .skip
.r:     mov byte [ro], 1
.skip:  lodsb                           ; past the option
        cmp al, ' '
        ja .skip
        dec si
        jmp .opt
.go:    mov ah, 0Dh                     ; flush DOS buffers: the monitor reads the disk itself
        int 21h
        mov si, pbuf
        mov di, tbuf
        mov ah, 60h                     ; full path (C:\DISKS\DISK1.IMG, 8.3 names)
        int 21h
        mov dx, msg_e2
        jc say
        mov si, tbuf
        mov cl, [unit]
        mov dl, [ro]
        mov ax, 5646h
        mov bx, 1
        call vmapi
        cmp ax, 5646h
        je novm
        cmp ax, 4                       ; a drive vmdos can't read itself: copy it in
        jne .chk
        call copy
.chk:   or ax, ax
        jnz .err
        cmp byte [doboot], 0
        je list
        jmp boot
.err:   cmp ax, 0FFFFh                  ; copy: said already
        je .quit
        mov bx, ax
        cmp bx, 7
        ja bad
        shl bx, 1
        mov dx, [errs + bx]
        jmp say
.quit:  mov ax, 4C01h
        int 21h

; Read the image tbuf names through DOS and pass it to vmdos (INT 2Fh
; AX=5646h BX=4 begin, 5 data, 6 done). AX = 0, an fd_mount error, or
; FFFFh (a DOS error, said here).
copy:   mov dx, tbuf
        mov ax, 3D00h                   ; open, read-only
        int 21h
        jnc .op
        mov dx, msg_e2
        jmp .say
.op:    mov [fh], ax
        mov bx, ax
        mov ax, 4202h                   ; its size, DX:AX
        xor cx, cx
        xor dx, dx
        int 21h
        jc .rerr
        test ax, 1FFh                   ; whole sectors, 512 bytes to 4M
        jnz .bad3
        cmp dx, 40h
        ja .bad3
        jb .sz
        or ax, ax
        jnz .bad3
.sz:    mov cl, 9
        shr ax, cl
        mov cl, 7
        shl dx, cl
        or dx, ax                       ; sectors
        jz .bad3
        push dx
        mov ax, 4200h                   ; back to the start
        mov bx, [fh]
        xor cx, cx
        xor dx, dx
        int 21h
        pop dx
        jc .rerr
        mov si, tbuf
        mov cl, [unit]
        mov bx, 4
        mov ax, 5646h
        call vmapi
        or ax, ax
        jnz .close
.rd:    mov ah, 3Fh
        mov bx, [fh]
        mov cx, CHUNK
        mov dx, cbuf
        int 21h
        jc .rerr
        or ax, ax
        jz .done
        mov dx, ax
        mov si, cbuf
        mov cl, [unit]
        mov bx, 5
        mov ax, 5646h
        call vmapi
        or ax, ax
        jz .rd
        jmp short .close
.done:  mov cl, [unit]
        mov bx, 6
        mov ax, 5646h
        call vmapi
        or ax, ax
        jnz .close
        mov dx, msg_copy
        push ax
        mov ah, 9
        int 21h
        pop ax
.close: push ax
        mov ah, 3Eh
        mov bx, [fh]
        int 21h
        pop ax
        ret
.bad3:  mov ax, 3
        jmp .close
.rerr:  mov dx, msg_rd
        mov ah, 9
        int 21h
        mov ax, 0FFFFh
        jmp .close
.say:   mov ah, 9
        int 21h
        mov ax, 0FFFFh
        ret
        cmp bx, 7
        ja bad
        shl bx, 1
        mov dx, [errs + bx]
        jmp say

boot:   mov dx, msg_boot
        mov ah, 9
        int 21h
        mov ah, 0Dh                     ; DOS's buffers out first: this is a restart
        int 21h
        mov ax, 5646h
        mov bx, 3
        mov cl, [unit]
        call vmapi                      ; returns only when the drive is empty
        cmp ax, 5646h
        je novm
        mov dx, msg_empty
        jmp say

skipsp: lodsb
        cmp al, ' '
        je skipsp
        cmp al, 9
        je skipsp
        dec si
        ret

unit    db 0
ro      db 0
doboot  db 0
fh      dw 0
CHUNK   equ 16384
errs    dw msg_bad, msg_bad, msg_e2, msg_e3, msg_e4, msg_bad, msg_e6, msg_e7
msg_use  db 'Usage: VMFD [A:|B:] [disk image [/R] [/BOOT] | /E | /BOOT]', 13, 10
         db '  e.g. VMFD A: C:\DISKS\DISK1.IMG   (/R read-only, /E take it out,', 13, 10
         db '  /BOOT restart the PC from the disk; no arguments: list)', 13, 10, '$'
msg_boot db 'Restarting from the floppy...', 13, 10, '$'
msg_empty db 'VMFD: no disk in that drive', 13, 10, '$'
msg_bad  db 'VMFD: no such drive', 13, 10, '$'
msg_novm db 'VMFD: not running under vmdos', 13, 10, '$'
msg_e2   db 'VMFD: file not found', 13, 10, '$'
msg_rd   db 'VMFD: error reading the file', 13, 10, '$'
msg_copy db 'VMFD: vmdos can', 39, 't read that drive itself, so it has a read-only copy', 13, 10, '$'
msg_e3   db 'VMFD: not a floppy disk image (160K to 2.88M, or a boot sector with a BPB)', 13, 10, '$'
msg_e4   db 'VMFD: vmdos can', 39, 't read that drive', 13, 10, '$'
msg_e6   db 'VMFD: out of memory', 13, 10, '$'
msg_e7   db 'VMFD: disk error reading the image', 13, 10, '$'
pbuf:    times 128 db 0
tbuf:    times 128 db 0
%include "vmapi.inc"                   ; vmdos's INT 2Fh stub, directly (VMHV)

buf:                                    ; the list, or a piece of a copy
cbuf    equ buf
