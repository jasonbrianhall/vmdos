; VMHD.COM: put a partition of one of vmdos's other disks (SATA, IDE, USB)
; in a VMHD.SYS drive, or take it out. Nothing is put in by itself.
;   VMHD                list the drives and every disk's partitions
;   VMHD D: 3           put partition 3 (as listed) in D:
;   VMHD D: 3 /R        ... read-only
;   VMHD D: /E          take it out of D:
;   (FDISK works on the other disks directly: they are BIOS disks 81h on.)
; A partition with a FAT file system but a partition type that isn't a
; DOS one (EFI system, Linux, hidden ...), and a new one with no file
; system yet (for FORMAT), go in only after a Y.
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
        mov di, buf
        int 2Fh
        cmp ax, 5647h
        je novm
        or ax, ax
        jnz nodrv
        mov dx, buf                     ; what was done
        jmp out
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
result: cmp ax, 5647h
        je novm
        or ax, ax
        jnz .notok
        push ds                         ; a partition went in: DOS sets the drive up now
        mov ah, 32h                     ; (get DPB: media check, BPB); an empty DPB looks
        mov dl, [drive]                 ; like FAT32 to FreeDOS, which then refuses
        inc dl                          ; INT 25h/26h (FORMAT's writes)
        int 21h
        pop ds
        mov dx, buf                     ; what was done, what comes next
        jmp out
.notok:
        cmp ax, 6
        jne .err
        call ask                        ; the warning in buf: Y goes on
        or byte [flags], 2
        jmp attach
.err:   cmp ax, 8
        ja .bad
        mov bx, ax
        shl bx, 1
        mov dx, [errs + bx]
        or dx, dx
        jz .bad
        jmp out
.bad:   mov dx, msg_e2
        jmp out

; Warning (in buf) and Y/N: returns on Y, else ends the program.
ask:    mov dx, msg_w1
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
        jmp say
.no:    mov dx, msg_no
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
errs    dw 0, msg_e1, msg_e2, msg_e3, msg_e4, msg_e5, 0, msg_e7, msg_e8
msg_use  db 'VMHD gives partitions of your other disks a drive letter.', 13, 10
         db '  VMHD            lists your disks and their partitions, with numbers,', 13, 10
         db '                  and says what you can do with each one', 13, 10
         db '  VMHD D: 3       puts partition [3] of that list in drive D:', 13, 10
         db '  VMHD D: 3 /R    the same, read-only (nothing can be written to it)', 13, 10
         db '  VMHD D: /E      takes the partition out of D: again', 13, 10, '$'
msg_novm db 'VMHD: not running under vmdos', 13, 10, '$'
msg_e1   db 'VMHD: that drive letter is not one of VMHD', 39, 's. Type VMHD to see which', 13, 10
         db 'letters it has (none? add DEVICE=C:\VMDOS\VMHD.SYS to C:\FDCONFIG.SYS', 13, 10
         db 'and restart).', 13, 10, '$'
msg_e2   db 'VMHD: there is no partition with that number. Type VMHD to see the', 13, 10
         db 'partitions and their numbers (in [brackets]).', 13, 10, '$'
msg_e3   db 'VMHD: that partition is drive C: already.', 13, 10, '$'
msg_e4   db 'VMHD: that partition is in another VMHD drive already. Type VMHD to', 13, 10
         db 'see which, and take it out there first (VMHD x: /E).', 13, 10, '$'
msg_e5   db 'VMHD: that partition has no FAT file system and is not a DOS', 13, 10
         db 'partition, so DOS can', 39, 't use it. Nothing was done.', 13, 10, '$'
msg_e7   db 'VMHD: the disk could not be read. Nothing was done.', 13, 10, '$'
msg_e8   db 'VMHD: that drive holds another partition already. Take it out first', 13, 10
         db '(VMHD x: /E, with its letter), or use another VMHD letter. Type VMHD', 13, 10
         db 'to see what is where.', 13, 10, '$'
msg_w1   db 'Please check first:', 13, 10, '$'
msg_w2   db 13, 10, 'Put it in anyway (Y/N)? $'
msg_yes  db 'Y', 13, 10, '$'
msg_no   db 'N', 13, 10, 'Nothing was done.', 13, 10, '$'
buf:
