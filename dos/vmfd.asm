; VMFD.COM: change the disk in vmdos's floppy drive A: or B: (disk images
; on C:).
;   VMFD                          list the drives
;   VMFD A: C:\DISKS\DISK1.IMG    put an image in A: (relative paths work)
;   VMFD B: GAME2.IMG /R          ... in B:, read-only
;   VMFD DISK1.IMG                drive A:
;   VMFD A: /E                    take the disk out of A:
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
.arg:   cmp byte [si], '/'              ; /E: eject
        jne path
        mov al, [si + 1]
        or al, 20h
        cmp al, 'e'
        jne usage
        mov ax, 5646h
        mov bx, 2
        mov cl, [unit]
        int 2Fh
        cmp ax, 5646h
        je novm
list:   mov ax, 5646h
        xor bx, bx
        mov di, buf
        int 2Fh
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
        call skipsp                     ; /R: read-only
        mov byte [ro], 0
        cmp al, '/'
        jne .go
        mov al, [si + 1]
        or al, 20h
        cmp al, 'r'
        jne usage
        mov byte [ro], 1
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
        int 2Fh
        cmp ax, 5646h
        je novm
        or ax, ax
        jz list
        mov bx, ax
        cmp bx, 7
        ja bad
        shl bx, 1
        mov dx, [errs + bx]
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
errs    dw msg_bad, msg_bad, msg_e2, msg_e3, msg_e4, msg_bad, msg_e6, msg_e7
msg_use  db 'Usage: VMFD [A:|B:] [disk image on C: [/R] | /E]', 13, 10
         db '  e.g. VMFD A: C:\DISKS\DISK1.IMG   (/R read-only, /E take it out;', 13, 10
         db '  no arguments: list)', 13, 10, '$'
msg_bad  db 'VMFD: no such drive', 13, 10, '$'
msg_novm db 'VMFD: not running under vmdos', 13, 10, '$'
msg_e2   db 'VMFD: file not found on C:', 13, 10, '$'
msg_e3   db 'VMFD: not a floppy disk image (160K to 2.88M, or a boot sector with a BPB)', 13, 10, '$'
msg_e4   db 'VMFD: the image has to be on drive C:', 13, 10, '$'
msg_e6   db 'VMFD: out of memory', 13, 10, '$'
msg_e7   db 'VMFD: disk error reading C:', 13, 10, '$'
pbuf:    times 128 db 0
tbuf:    times 128 db 0
buf:
