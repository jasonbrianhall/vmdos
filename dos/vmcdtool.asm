; VMCD.COM: change the disc in a vmdos CD-ROM drive.
;   VMCD                        list the drives and images
;   VMCD D: C:\ISOS\WAR2.ISO    put an ISO file (or a CUE sheet) from C: in drive D:
;   VMCD 1 WAR2.ISO             the same by drive number (relative paths work)
;   VMCD WAR2.ISO               drive 1
;   VMCD 1 2                    image 2 of the list (ISOs loaded at boot)
;   VMCD 1 0                    empty drive 1
        org 100h
        mov si, 81h
        call skipsp
        cmp al, 13
        je list
        mov byte [unit], 1
        call drive                      ; drive given first?
        jc .arg
        mov [unit], al
        call skipsp
        cmp al, 13
        je usage
.arg:   call alldigits                  ; image number, or a path
        jc path
        call number
        mov dl, al
        mov cl, [unit]
        xor ch, ch
        mov ax, 5644h
        mov bx, 2
        int 2Fh
        cmp ax, 5644h
        je novm
        or ax, ax
        jnz bad
list:   mov ax, 5644h
        mov bx, 2
        xor cx, cx
        mov di, buf
        int 2Fh
        cmp ax, 5644h
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
        mov ah, 0Dh                     ; flush DOS buffers: the monitor reads the disk itself
        int 21h
        mov si, pbuf
        mov di, tbuf
        mov ah, 60h                     ; full path (C:\ISOS\WAR2.ISO, 8.3 names)
        int 21h
        mov dx, msg_e2
        jc say
        mov si, tbuf
        mov cl, [unit]
        xor ch, ch
        mov ax, 5644h
        mov bx, 3
        int 2Fh
        cmp ax, 5644h
        je novm
        or ax, ax
        jz list
        mov bx, ax
        cmp bx, 8
        ja bad
        shl bx, 1
        mov dx, [errs + bx]
        jmp say

; drive at SI: "N" or "X:" followed by a blank -> AL = drive number, SI
; past it; else CF and SI unchanged.
drive:  push si
        call alldigits
        jc .letter
        call number
        add sp, 2
        clc
        ret
.letter:
        mov al, [si]
        or al, 20h
        cmp al, 'a'
        jb .no
        cmp al, 'z'
        ja .no
        cmp byte [si + 1], ':'
        jne .no
        cmp byte [si + 2], ' '
        ja .no
        sub al, 'a'
        mov [letter], al
        add si, 2
        push si
        mov ax, 150Dh                   ; MSCDEX/SHSUCDX: CD drive letters, in drive order
        push cs
        pop es
        mov bx, letters
        int 2Fh
        pop si
        mov di, letters
        mov cx, 26
        mov al, [letter]
        repne scasb
        jne .nocd
        mov ax, di
        sub ax, letters                 ; 1-based index
        add sp, 2
        clc
        ret
.nocd:  mov dx, msg_nocd
        jmp say
.no:    pop si
        stc
        ret

; Is the token at SI all digits? CF if not.
alldigits:
        push si
        mov ah, 0
.l:     lodsb
        cmp al, ' '
        jbe .e
        cmp al, '0'
        jb .n
        cmp al, '9'
        ja .n
        mov ah, 1
        jmp .l
.e:     pop si
        cmp ah, 1
        ret                             ; CF = (ah < 1): no digits
.n:     pop si
        stc
        ret

skipsp: lodsb
        cmp al, ' '
        je skipsp
        cmp al, 9
        je skipsp
        dec si
        ret

number:                                 ; decimal at SI -> AL (SI past it)
        xor bh, bh
.more:  lodsb
        cmp al, '0'
        jb .end
        cmp al, '9'
        ja .end
        sub al, '0'
        xchg al, bh
        mov bl, 10
        mul bl
        add al, bh
        mov bh, al
        jmp .more
.end:   dec si
        mov al, bh
        ret

unit    db 1
letter  db 0
errs    dw msg_bad, msg_bad, msg_e2, msg_e3, msg_e4, msg_e5, msg_e6, msg_e7, msg_e8
msg_use  db 'Usage: VMCD [drive] [image number | ISO or CUE file on C:]', 13, 10
         db '  e.g. VMCD D: C:\ISOS\WAR2.ISO   (no arguments: list)', 13, 10, '$'
msg_bad  db 'VMCD: no such drive or image', 13, 10, '$'
msg_novm db 'VMCD: not running under vmdos', 13, 10, '$'
msg_nocd db 'VMCD: that is not a vmdos CD-ROM drive', 13, 10, '$'
msg_e2   db 'VMCD: file not found on C:', 13, 10, '$'
msg_e3   db 'VMCD: not an ISO 9660 CD image', 13, 10, '$'
msg_e4   db 'VMCD: the image has to be on drive C:', 13, 10, '$'
msg_e5   db 'VMCD: no such CD-ROM drive (more with cdrives=N on the kernel command line)', 13, 10, '$'
msg_e6   db 'VMCD: out of memory for the image', 13, 10, '$'
msg_e7   db 'VMCD: disk error reading C:', 13, 10, '$'
msg_e8   db 'VMCD: unusable CUE sheet (BIN missing or unsupported track mode; see the log)', 13, 10, '$'
letters: times 26 db 0FFh
pbuf:    times 128 db 0
tbuf:    times 128 db 0
buf:
