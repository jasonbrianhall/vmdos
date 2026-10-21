; VMDOSNET.COM: a folder the host shares over virtio-9p (QEMU -virtfs) as a
; DOS drive. vmdos does the work (src/fs9p.cpp); nothing stays resident.
;   VMDOSNET                         list the shares and their drives
;   VMDOSNET V: [tag] [options]      share "tag" (default: the first) in V:
;     /LFN     long names for programs that ask for them (INT 21h 71xxh)
;     /WT      write straight through (no gathering of writes)
;     /RO      read-only
;     /C:kb    file cache size, KiB (default 4096; the first drive sets it)
;     /T:ms    how long listings are trusted before checking (default 1000)
;   VMDOSNET V: /D                   take the share out of V:
;   VMDOSNET /F                      write out what is gathered now
; (INT 2Fh AX=5652h; BX=0 list to ES:DI, 1 connect (ES:DI the block below),
;  2 disconnect DL, 3 flush; returns AX = 0 or an error, BX = 'VM'.)
        org 100h

        mov si, 81h
        call skipsp
        cmp al, 13
        je list
        cmp al, '/'
        jne .drv
        mov al, [si + 1]
        or al, 20h
        cmp al, 'f'
        jne usage
        mov ax, 5652h
        mov bx, 3
        call vmapi
        cmp bx, 564Dh
        jne novm
        mov dx, msg_flushed
        jmp say
.drv:   or al, 20h                      ; "V:"
        cmp al, 'a'
        jb usage
        cmp al, 'z'
        ja usage
        cmp byte [si + 1], ':'
        jne usage
        sub al, 'a'
        mov [blk_drive], al
        add si, 2
.tok:   call skipsp
        cmp al, 13
        je connect
        cmp al, '/'
        je .opt
        mov di, blk_tag                 ; the tag
        mov cx, 31
.tg:    lodsb
        cmp al, ' '
        jbe .tge
        cmp al, '/'
        je .tge
        stosb
        loop .tg
.tge:   mov byte [di], 0
        dec si
        jmp .tok
.opt:   inc si
        lodsb
        or al, 20h
        cmp al, 'd'
        je disconnect
        cmp al, 'w'
        je .wt
        cmp al, 'r'
        je .ro
        cmp al, 'l'
        je .lfn
        cmp al, 'c'
        je .cache
        cmp al, 't'
        je .ttl
        jmp usage
.wt:    or byte [blk_flags], 2
        jmp .skip
.ro:    or byte [blk_flags], 4
        jmp .skip
.lfn:   or byte [blk_flags], 1
        jmp .skip
.cache: call number
        mov [blk_cache], ax
        jmp .tok
.ttl:   call number
        mov [blk_ttl], ax
        jmp .tok
.skip:  lodsb                           ; the rest of the option
        cmp al, ' '
        ja .skip
        dec si
        jmp .tok

connect:
        push ds
        mov ax, 5D06h                   ; DOS's swappable data area: DS:SI
        int 21h
        mov ax, ds
        pop ds
        jc .nosda
        mov [blk_sda], si
        mov [blk_sda + 2], ax
        mov ah, 52h                     ; list of lists: ES:BX
        int 21h
        mov [blk_lol], bx
        mov [blk_lol + 2], es
        push cs
        pop es
        mov di, blk
        mov ax, 5652h
        mov bx, 1
        call vmapi
        cmp bx, 564Dh
        jne novm
        or ax, ax
        jnz error
        jmp list
.nosda: mov dx, msg_dos
        jmp say

disconnect:
        mov ax, 5652h
        mov bx, 2
        mov dl, [blk_drive]
        call vmapi
        cmp bx, 564Dh
        jne novm
        or ax, ax
        jnz error
        mov dx, msg_gone
        jmp say

list:   push cs
        pop es
        mov di, buf
        mov ax, 5652h
        xor bx, bx
        call vmapi
        cmp bx, 564Dh
        jne novm
        mov dx, msg_head
        mov ah, 9
        int 21h
        mov dx, buf
        jmp say

error:  mov bx, ax
        cmp bx, 9
        ja .gen
        shl bx, 1
        mov dx, [errs + bx]
        jmp say
.gen:   mov dx, msg_e0
        jmp say
usage:  mov dx, msg_use
        jmp say
novm:   mov dx, msg_novm
say:    mov ah, 9
        int 21h
        mov ax, 4C00h
        int 21h

; ":1234" -> AX (SI past it)
number: xor ax, ax
        cmp byte [si], ':'
        jne .end
        inc si
        mov bx, 10
.dig:   mov cl, [si]
        sub cl, '0'
        cmp cl, 9
        ja .end
        mul bx
        add al, cl
        adc ah, 0
        inc si
        jmp .dig
.end:   ret

skipsp: lodsb
        cmp al, ' '
        je skipsp
        cmp al, 9
        je skipsp
        dec si
        ret

%include "vmapi.inc"

; the block for BX=1
blk:
blk_sda   dd 0
blk_lol   dd 0
blk_drive db 0
blk_flags db 0
blk_cache dw 0
blk_ttl   dw 0
blk_tag   times 32 db 0

errs    dw msg_e0, msg_e1, msg_e2, msg_e3, msg_e4, msg_e5, msg_e0, msg_e7, msg_e8, msg_e9
msg_use  db 'Usage: VMDOSNET [V: [tag] [/LFN] [/WT] [/RO] [/C:kb] [/T:ms] | V: /D | /F]', 13, 10
         db '  VMDOSNET V: share /LFN   the host folder QEMU shares as "share" in V:,', 13, 10
         db '                           with long names. No arguments: list.', 13, 10, '$'
msg_head db 'Host folders (virtio-9p):', 13, 10, '$'
msg_novm db 'VMDOSNET: not running under vmdos', 13, 10, '$'
msg_dos  db 'VMDOSNET: this DOS has no swappable data area (INT 21h 5D06h)', 13, 10, '$'
msg_gone db 'Disconnected.', 13, 10, '$'
msg_flushed db 'Written out.', 13, 10, '$'
msg_e0   db 'VMDOSNET: failed', 13, 10, '$'
msg_e1   db 'VMDOSNET: no virtio-9p device (QEMU: -virtfs local,path=DIR,mount_tag=share,security_model=none)', 13, 10, '$'
msg_e2   db 'VMDOSNET: no share with that tag (VMDOSNET lists them)', 13, 10, '$'
msg_e3   db 'VMDOSNET: that drive letter is in use or past LASTDRIVE', 13, 10, '$'
msg_e4   db 'VMDOSNET: the host did not answer the 9p handshake', 13, 10, '$'
msg_e5   db 'VMDOSNET: out of memory', 13, 10, '$'
msg_e7   db 'VMDOSNET: that drive is not a share', 13, 10, '$'
msg_e8   db 'VMDOSNET: files are open on that drive', 13, 10, '$'
msg_e9   db 'VMDOSNET: not in VMHV', 39, 's virtual machine (boot menu choice 1)', 13, 10, '$'

buf:
