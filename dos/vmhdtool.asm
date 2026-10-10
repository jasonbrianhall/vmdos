; VMHD.COM: put a partition of one of vmdos's other disks (SATA, IDE, USB)
; in a VMHD.SYS drive, or take it out. Nothing is put in by itself.
;   VMHD                list the drives and every disk's partitions
;   VMHD D: 3           put partition 3 (as listed) in D:
;   VMHD D: 3 /R        ... read-only
;   VMHD D: /E          take it out of D:
; A partition with a FAT file system but a partition type that isn't a
; DOS one (EFI system, Linux, hidden ...) is put in only after a Y.
        org 100h
        mov si, 81h
        call skipsp
        cmp al, 13
        je list
        mov al, [si]                    ; "D:"
        or al, 20h
        cmp al, 'a'
        jb usage
        cmp al, 'z'
        ja usage
        cmp byte [si + 1], ':'
        jne usage
        sub al, 'a'
        mov [drive], al
        add si, 2
        call skipsp
        cmp al, '/'
        jne .num
        mov al, [si + 1]
        or al, 20h
        cmp al, 'e'
        jne usage
        call flush
        mov ax, 5647h                   ; take it out
        mov bx, 4
        mov cl, [drive]
        int 2Fh
        cmp ax, 5647h
        je novm
        or ax, ax
        jnz nodrv
        jmp list
.num:   xor dx, dx                      ; partition number
.dig:   lodsb
        cmp al, '0'
        jb .numend
        cmp al, '9'
        ja .numend
        sub al, '0'
        xchg ax, dx
        mov ah, 10
        mul ah
        add al, dl
        xchg ax, dx
        xor dh, dh
        jmp .dig
.numend: dec si
        or dl, dl
        jz usage
        mov [part], dl
        call skipsp                     ; /R
        cmp al, '/'
        jne attach
        mov al, [si + 1]
        or al, 20h
        cmp al, 'r'
        jne usage
        or byte [flags], 1

attach: call flush
        mov ax, 5647h
        mov bx, 3
        mov cl, [drive]
        mov dl, [part]
        mov dh, [flags]
        mov di, buf
        int 2Fh
        cmp ax, 5647h
        je novm
        or ax, ax
        jz list
        cmp ax, 6
        jne .err
        mov dx, msg_w1                  ; not a DOS partition: ask
        call say
        mov dx, buf
        call say
        mov dx, msg_w2
        call say
        mov ah, 8                       ; a key, no echo
        int 21h
        or al, 20h
        cmp al, 'y'
        jne .no
        mov dx, msg_yes
        call say
        or byte [flags], 2
        jmp attach
.no:    mov dx, msg_no
        jmp out
.err:   cmp ax, 7
        ja .bad
        mov bx, ax
        shl bx, 1
        mov dx, [errs + bx]
        jmp out
.bad:   mov dx, msg_e2
        jmp out

list:   mov ax, 5647h
        mov bx, 2
        mov di, buf
        int 2Fh
        cmp ax, 5647h
        je novm
        mov dx, buf
        jmp short out
nodrv:  mov dx, msg_e1
        jmp short out
usage:  mov dx, msg_use
        jmp short out
novm:   mov dx, msg_novm
out:    call say
        mov ax, 4C00h
        int 21h

say:    mov ah, 9
        int 21h
        ret

flush:  mov ah, 0Dh                     ; DOS's buffers out before a drive changes
        int 21h
        ret

skipsp: lodsb
        cmp al, ' '
        je skipsp
        cmp al, 9
        je skipsp
        dec si
        ret

drive   db 0
part    db 0
flags   db 0                            ; 1 read-only, 2 confirmed
errs    dw 0, msg_e1, msg_e2, msg_e3, msg_e4, msg_e5, 0, msg_e7
msg_use  db 'Usage: VMHD [drive: partition [/R] | drive: /E]', 13, 10
         db '  VMHD         lists the drives and the partitions', 13, 10
         db '  VMHD D: 3    puts partition 3 in D: (/R read-only)', 13, 10
         db '  VMHD D: /E   takes it out of D:', 13, 10, '$'
msg_novm db 'VMHD: not running under vmdos', 13, 10, '$'
msg_e1   db 'VMHD: not a VMHD drive (DEVICE=C:\VMDOS\VMHD.SYS in FDCONFIG.SYS gives them)', 13, 10, '$'
msg_e2   db 'VMHD: no such partition (VMHD alone lists them)', 13, 10, '$'
msg_e3   db 'VMHD: that partition is C:', 13, 10, '$'
msg_e4   db 'VMHD: that partition is in another drive already', 13, 10, '$'
msg_e5   db 'VMHD: no FAT file system on that partition: DOS can', 39, 't use it', 13, 10, '$'
msg_e7   db 'VMHD: disk error', 13, 10, '$'
msg_w1   db 'Warning: not a DOS partition: ', '$'
msg_w2   db 13, 10, 'It holds a FAT file system, but its partition type says it belongs', 13, 10
         db 'to something else (another OS, the firmware). Writing to it from DOS', 13, 10
         db 'could damage it. Put it in anyway (Y/N)? $'
msg_yes  db 'Y', 13, 10, '$'
msg_no   db 'N', 13, 10, 'VMHD: left out', 13, 10, '$'
buf:
