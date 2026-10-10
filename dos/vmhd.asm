; VMHD.SYS: empty drive letters for partitions of vmdos's other disks
; (SATA, IDE, USB), filled with VMHD.COM ("VMHD D: 3"). Nothing is put in
; a drive by itself. Every request goes to the monitor (INT 2Fh AX=5647h).
;   DEVICE=C:\VMDOS\VMHD.SYS      (in FDCONFIG.SYS: 2 drives)
;   DEVICE=C:\VMDOS\VMHD.SYS 4    (4 drives, 1 to 8)
; Not under vmdos it doesn't stay in memory.
        org 0
hdr:    dd -1
        dw 2002h                        ; block device, 32-bit sector numbers, no FAT for BUILD BPB
        dw strategy
        dw interrupt
units:  db 0                            ; number of drives (set at init)
        db 'VMHD   '
rh:     dd 0

strategy:
        mov [cs:rh], bx
        mov [cs:rh + 2], es
        retf

interrupt:
        push ax
        push bx
        push cx
        push dx
        push si
        push di
        push es
        les di, [cs:rh]
        cmp byte [es:di + 2], 0
        je init
        mov ax, 5647h
        mov bx, 1
        mov dx, cs
        mov si, bpbs                    ; the monitor puts each drive's BPB here
        int 2Fh
done:   pop es
        pop di
        pop si
        pop dx
        pop cx
        pop bx
        pop ax
        retf

bpbptr: times 8 dw 0                    ; init's BPB array
bpbs:   times 8 * 64 db 0               ; a BPB a drive
resident_end:

; A BPB for DOS to set the drives up with at boot (a 1.44 MB diskette);
; each drive gets its partition's own when one is put in.
dflt:   dw 512
        db 1
        dw 1
        db 2
        dw 224, 2880
        db 0F0h
        dw 9, 18, 2
        dd 0, 0
DFLT_LEN equ $ - dflt

init:   push ds
        mov cx, 2                       ; drives wanted: a digit after the file name, else 2
        lds si, [es:di + 18]            ; DEVICE= line
.name:  lodsb
        cmp al, ' '
        ja .name
.arg:   cmp al, 13
        je .got
        cmp al, 10
        je .got
        or al, al
        jz .got
        cmp al, '1'
        jb .next
        cmp al, '8'
        ja .next
        sub al, '0'
        mov cl, al
        jmp short .got
.next:  lodsb
        jmp .arg
.got:   push cs
        pop ds
        mov dl, [es:di + 22]            ; our first drive (0 = A:)
        mov [first], dl
        mov ax, 5647h
        xor bx, bx
        int 2Fh
        cmp ax, 5647h                   ; unchanged: not vmdos
        je .none
        or ax, ax
        jz .none
        mov [units], al
        mov [es:di + 13], al
        mov cx, ax                      ; fill in each drive's BPB and its pointer
        push es
        push di
        push cs
        pop es
        mov bx, bpbptr
        mov di, bpbs
.fill:  mov [bx], di
        add bx, 2
        push cx
        push di
        mov si, dflt
        mov cx, DFLT_LEN
        rep movsb
        pop di
        pop cx
        add di, 64
        loop .fill
        pop di
        pop es
        mov word [es:di + 14], resident_end
        mov [es:di + 16], cs
        mov word [es:di + 18], bpbptr
        mov [es:di + 20], cs
        mov word [es:di + 3], 0100h
        mov al, [first]                 ; "VMHD: D: to E: ..."
        add al, 'A'
        mov [msg_a], al
        add al, [units]
        dec al
        mov [msg_b], al
        mov dx, msg_ok
        mov ah, 9
        int 21h
        jmp short .out
.none:  mov byte [es:di + 13], 0
        mov word [es:di + 14], 0        ; don't stay
        mov [es:di + 16], cs
        mov word [es:di + 3], 810Ch
.out:   pop ds
        jmp done

first   db 0
msg_ok  db 'VMHD: '
msg_a   db 'D: to '
msg_b   db 'E: for other disks', 39, ' partitions (VMHD.COM puts one in)', 13, 10, '$'
