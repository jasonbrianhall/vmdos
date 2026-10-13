; VMHV.COM: restart DOS inside vmdos's AMD-V virtual machine (src/hv.c),
; where it has 640 KB + 8 MB of memory to manage itself: HIMEM, EMM386,
; Windows 3.x, DOS extenders' own kernels.
;
;   VMHV        restart in the VM; at FDCONFIG.SYS's menu, pick a choice
;               yourself, or as the countdown ends vmdos picks the one
;               whose MENU line mentions VMHV (vmdos's has "MENU 2 = ...")
;   VMHV n      ... picking choice n at once
;
; In the VM, Ctrl+Alt+End sends Ctrl+Alt+Del (DOS and SMARTDRV flush, then
; vmdos restarts); twice within 3 s asks to force a restart.
; Build: nasm -f bin vmhv.asm -o VMHV.COM

        org 0x100

        mov si, 0x81                    ; command line: first digit, if any
        xor bl, bl
.scan:  lodsb
        cmp al, 13
        je .go
        cmp al, '1'
        jb .scan
        cmp al, '9'
        ja .scan
        mov bl, al
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

