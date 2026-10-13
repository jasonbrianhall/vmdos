; VMHV.COM: restart DOS inside vmdos's AMD-V virtual machine (src/hv.c),
; where it has 640 KB + 8 MB of memory to manage itself: HIMEM, EMM386,
; Windows 3.x, DOS extenders' own kernels.
;
;   VMHV        restart in the VM, picking the FDCONFIG.SYS menu choice
;               whose MENU line mentions VMHV (vmdos's has "MENU 2 = ...")
;   VMHV n      ... picking choice n instead
;
; In the VM, Ctrl+Alt+End sends Ctrl+Alt+Del (DOS and SMARTDRV flush, then
; vmdos restarts); twice within 3 s asks to force a restart.
; Build: nasm -f bin vmhv.asm -o VMHV.COM

        org 0x100

        mov si, 0x81                    ; command line: first digit, if any
        xor bl, bl
.scan:  lodsb
        cmp al, 13
        je .cfg
        cmp al, '1'
        jb .scan
        cmp al, '9'
        ja .scan
        mov bl, al
        jmp .go
.cfg:   call find_choice                ; none given: look in FDCONFIG.SYS
.go:    push bx
        mov ah, 0x0D                    ; DOS: write out its buffers first
        int 0x21
        mov dx, msg_go
        mov ah, 9
        int 0x21
        pop bx
        mov ax, 0x5648                  ; vmdos: VMHV (doesn't come back if it starts)
        int 0x2F
        cmp bx, 0x564D
        jne .novm
        cmp ax, 6
        ja .unknown
        mov bx, ax
        shl bx, 1
        mov dx, [errs + bx]
        jmp .say
.unknown:
        mov dx, e_unk
        jmp .say
.novm:  mov dx, e_novm
.say:   mov ah, 9
        int 0x21
        mov ax, 0x4C01
        int 0x21

; BL = the digit of the first "MENU n = ..." line in C:\FDCONFIG.SYS that
; mentions VMHV (any case), or 0.
find_choice:
        xor bl, bl
        mov dx, cfgname
        mov ax, 0x3D00
        int 0x21
        jc .ret
        mov bx, ax
        mov dx, buf
        mov cx, BUFLEN - 1
        mov ah, 0x3F
        int 0x21
        jnc .read
        xor ax, ax
.read:  mov di, ax
        mov byte [buf + di], 0
        mov ah, 0x3E
        int 0x21
        xor bl, bl
        mov si, buf
.line:  cmp byte [si], 0
        je .ret
        mov ax, [si]                    ; "MENU " at the start of the line
        or ax, 0x2020
        cmp ax, 'me'
        jne .next
        mov ax, [si + 2]
        or ax, 0x2020
        cmp ax, 'nu'
        jne .next
        mov al, [si + 4]
        cmp al, ' '
        jne .next
        mov al, [si + 5]
        cmp al, '1'
        jb .next
        cmp al, '9'
        ja .next
        mov ah, al                      ; the digit; now VMHV before the line ends?
        push si
.find:  mov cl, [si]
        cmp cl, 0
        je .nf
        cmp cl, 10
        je .nf
        mov cx, [si]
        or cx, 0x2020
        cmp cx, 'vm'
        jne .adv
        mov cx, [si + 2]
        or cx, 0x2020
        cmp cx, 'hv'
        jne .adv
        pop si
        mov bl, ah
        ret
.adv:   inc si
        jmp .find
.nf:    pop si
.next:  lodsb                           ; skip to the next line
        cmp al, 0
        je .ret
        cmp al, 10
        jne .next
        jmp .line
.ret:   ret

cfgname: db 'C:\FDCONFIG.SYS', 0
msg_go: db 'VMHV: restarting DOS in a virtual machine...', 13, 10, '$'
errs:   dw e_unk, e1, e2, e3, e4, e5, e6
e1:     db 'VMHV: this CPU has no AMD-V (SVM).', 13, 10, '$'
e2:     db 'VMHV: AMD-V (SVM) is turned off in the firmware setup.', 13, 10, '$'
e3:     db 'VMHV: this CPU', 39, 's AMD-V has no nested paging.', 13, 10, '$'
e4:     db 'VMHV: not enough memory for the virtual machine.', 13, 10, '$'
e5:     db 'VMHV: Intel VT-x isn', 39, 't supported yet (AMD-V only).', 13, 10, '$'
e6:     db 'VMHV: vmdos was started with nopae; VMHV needs PAE paging.', 13, 10, '$'
e_unk:  db 'VMHV: the virtual machine could not start.', 13, 10, '$'
e_novm: db 'VMHV: needs vmdos.', 13, 10, '$'

BUFLEN  equ 4096
buf:
