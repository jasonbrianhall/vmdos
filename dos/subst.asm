; SUBST.COM: a folder as a drive letter, by changing DOS's current directory
; structure (CDS) as MS-DOS's SUBST does; FreeDOS's kernel follows a
; substituted drive itself. Modeled on Steffen Kaiser's SWSUBST (GPL), which
; FreeDOS ships for SUBST and JOIN.
;   SUBST                 list the substituted drives
;   SUBST X: C:\FOLDER    X: shows C:\FOLDER (X: an unused letter up to
;                         LASTDRIVE, or one SUBST made)
;   SUBST X: /D           undo
; nasm -DJOIN builds JOIN.COM from the same source (JOIN X: C:\FOLDER puts
; drive X: in an existing folder of another local drive). It isn't built:
; FreeDOS's kernel (2043) resolves a joined path to the drive but then finds
; no DPB for it (get_dpb() goes through get_cds(), which refuses joined
; drives), so every access fails with "File not found".
%ifndef JOIN
%define FLAG 1000h
%else
%define FLAG 2000h
%endif
CDS_FLAGS equ 43h
CDS_DPB   equ 45h
CDS_CLUS  equ 49h
CDS_BS    equ 4Fh
F_PHYS    equ 4000h
F_NET     equ 8000h
F_JOIN    equ 2000h
F_SUBST   equ 1000h

        org 100h
        cld
        mov ah, 30h                     ; DOS 4 or later (the CDS is 58h bytes)
        int 21h
        cmp al, 4
        jb err_dos
        mov ah, 52h                     ; list of lists
        int 21h
        mov [lol], bx
        mov [lol + 2], es
        mov ax, [es:bx + 16h]           ; the CDS array
        mov [cdsp], ax
        mov ax, [es:bx + 18h]
        mov [cdsp + 2], ax
        mov al, [es:bx + 21h]           ; LASTDRIVE
        mov [lastdr], al
        push ds
        pop es

        mov si, 81h
        call skip
        cmp al, 13
        je list
        and al, 0DFh                    ; X:
        cmp byte [si + 1], ':'
        jne usage
        sub al, 'A'
        jb usage
        cmp al, [lastdr]
        jae err_letter
        mov [drv], al
        add si, 2
        call skip
        cmp al, 13
        je usage
        cmp al, '/'
        jne .path
        mov al, [si + 1]
        and al, 0DFh
        cmp al, 'D'
        jne usage
        jmp undo
.path:  mov di, arg                     ; the folder, to its end
.cp:    lodsb
        cmp al, ' '
        jbe .ec
        stosb
        jmp .cp
.ec:    mov byte [di], 0
        jmp make

; ---- list: every drive with our flag
list:   xor bx, bx
.l:     cmp bl, [lastdr]
        jae .done
        push bx
        call cdsof
        test word [es:di + CDS_FLAGS], FLAG
        jz .n
        mov al, bl
        add al, 'A'
        mov [line], al
        mov dx, line
        mov cx, 6
        call write
        push ds                         ; its path
        push es
        pop ds
        mov dx, di
        call wrz
        pop ds
        mov dx, crlf
        mov cx, 2
        call write
        pop bx
        inc bx
        jmp .l
.n:     pop bx
        inc bx
        jmp .l
.done:  push ds
        pop es
        mov ax, 4C00h
        int 21h

; ---- X: /D
undo:   mov bl, [drv]
        call cdsof
        mov ax, [es:di + CDS_FLAGS]
        test ax, FLAG
        jz err_notours
        mov [oldflags], ax
        mov al, bl                      ; path X:\, backslash at 2
        add al, 'A'
        mov [es:di], al
        mov byte [es:di + 1], ':'
        mov byte [es:di + 2], '\'
        mov byte [es:di + 3], 0
        mov word [es:di + CDS_BS], 2
        mov word [es:di + CDS_CLUS], 0FFFFh
        mov al, bl
        call dpbof                      ; its own DPB: then a drive of its own again
        mov [es:di + CDS_DPB], ax
        mov [es:di + CDS_DPB + 2], dx
        mov cx, [es:di + CDS_FLAGS]
        and cx, ~(F_JOIN | F_SUBST)
        or ax, dx
        jz .nodpb
        or cx, F_PHYS
        jmp .f
.nodpb: and cx, ~F_PHYS
.f:     mov [es:di + CDS_FLAGS], cx
%ifdef JOIN
        les bx, [lol]
        dec byte [es:bx + 34h]          ; number of joined drives
%endif
        jmp ok

; ---- X: folder
make:   mov si, arg                     ; the canonical name (TRUENAME)
        mov di, path
        mov ah, 60h
        int 21h
        jc err_path
        mov si, path                    ; an existing folder?
        cmp byte [si + 1], ':'
        jne err_local
        cmp byte [si + 2], '\'
        jne err_local
        mov dx, path
        mov ax, 4300h
        int 21h
        jc .root
        test cl, 10h
        jz err_path
        jmp .isdir
.root:  cmp byte [path + 3], 0          ; X:\ itself has no attributes
        jne err_path
.isdir: mov al, [path]                  ; its drive: a local, physical one
        sub al, 'A'
        mov [tdrv], al
        cmp al, [drv]
        je err_same
        mov bl, al
        call cdsof
        mov ax, [es:di + CDS_FLAGS]
        test ax, F_NET
        jnz err_local
        test ax, F_PHYS
        jz err_local
        test ax, F_JOIN | F_SUBST
        jnz err_local
        mov bl, [drv]                   ; the drive letter
        call cdsof
        mov ax, [es:di + CDS_FLAGS]
        test ax, (F_JOIN | F_SUBST) & ~FLAG
        jnz err_other
%ifndef JOIN
        test ax, F_SUBST                ; a drive of its own can't be one
        jnz .free
        test ax, F_PHYS | F_NET
        jnz err_inuse
.free:
%else
        test ax, F_JOIN
        jnz err_inuse
        test ax, F_NET
        jnz err_local
        test ax, F_PHYS
        jz err_letter
        mov ah, 19h                     ; not the current drive
        int 21h
        cmp al, [drv]
        je err_current
%endif
        mov si, path                    ; the length
        xor cx, cx
.len:   cmp byte [si], 0
        je .gotlen
        inc si
        inc cx
        jmp .len
.gotlen:
        cmp cx, 66
        ja err_long
        mov [plen], cx
        mov bl, [drv]
        call cdsof
        push di
        mov si, path
        inc cx
        rep movsb
        pop di
        mov ax, [es:di + CDS_FLAGS]
        mov [oldflags], ax
%ifndef JOIN
        mov cx, [plen]                  ; backslash offset: the path's length
        cmp cx, 3                       ; (the root's \ for X:\)
        jne .bs
        dec cx
.bs:    mov [es:di + CDS_BS], cx
        mov al, [tdrv]
%else
        mov word [es:di + CDS_BS], 2
        push es
        push di
        mov bl, [tdrv]                  ; the folder's drive reads its folders again
        call cdsof
        mov word [es:di + CDS_CLUS], 0FFFFh
        pop di
        pop es
        mov al, [drv]
%endif
        mov word [es:di + CDS_CLUS], 0FFFFh
        push es
        push di
        call dpbof
        pop di
        pop es
        mov [es:di + CDS_DPB], ax
        mov [es:di + CDS_DPB + 2], dx
        mov ax, [es:di + CDS_FLAGS]
        and ax, ~(F_JOIN | F_SUBST)
        or ax, FLAG | F_PHYS
        mov [es:di + CDS_FLAGS], ax
%ifdef JOIN
        test word [oldflags], F_JOIN
        jnz ok
        les bx, [lol]
        inc byte [es:bx + 34h]
%endif
ok:     push ds
        pop es
        mov ax, 4C00h
        int 21h

; ---- helpers
cdsof:  les di, [cdsp]                  ; ES:DI = the CDS of drive BL
        mov al, 58h
        mul bl
        add di, ax
        ret
dpbof:  push es                         ; DX:AX = the DPB of drive AL (0 = A:), 0:0 if none
        push bx
        push di
        les di, [lol]
        les di, [es:di]                 ; the first DPB
.d:     cmp di, 0FFFFh
        je .none
        cmp [es:di], al
        je .got
        les di, [es:di + 19h]
        jmp .d
.got:   mov ax, di
        mov dx, es
        jmp .r
.none:  xor ax, ax
        xor dx, dx
.r:     pop di
        pop bx
        pop es
        ret
skip:   lodsb                           ; AL = the next non-blank, SI at it
        cmp al, ' '
        je skip
        cmp al, 9
        je skip
        dec si
        ret
write:  mov bx, 1                       ; CX bytes at DS:DX
        mov ah, 40h
        int 21h
        ret
wrz:    push si                         ; ASCIIZ at DS:DX
        mov si, dx
        xor cx, cx
.z:     cmp byte [si + 0], 0
        je .w
        inc si
        inc cx
        jmp .z
.w:     call write
        pop si
        ret

%macro fail 2
%1:     mov dx, %%m
        jmp die
%%m:    db %2, 13, 10, '$'
%endmacro
die:    push ds
        pop es
        mov ah, 9
        int 21h
        mov ax, 4C01h
        int 21h
usage:  mov dx, help
        jmp die
        fail err_dos, 'Needs DOS 4 or later'
        fail err_letter, 'Invalid drive letter (beyond LASTDRIVE, or no such drive)'
        fail err_path, 'Path not found (it has to be an existing folder)'
        fail err_local, 'The folder has to be on a local drive (not a network, CD, JOINed or SUBSTed one)'
        fail err_same, 'The folder is on that same drive'
        fail err_long, 'Path too long'
        fail err_current, 'Not the current drive'
%ifndef JOIN
        fail err_notours, 'That drive is not SUBSTed'
        fail err_inuse, 'That drive letter is in use'
        fail err_other, 'That drive is JOINed'
help:   db 'SUBST: a folder as a drive letter', 13, 10
        db '  SUBST            list the substituted drives', 13, 10
        db '  SUBST X: path    X: shows the folder (X: an unused letter)', 13, 10
        db '  SUBST X: /D      undo', 13, 10, '$'
line:   db '?: => '
%else
        fail err_notours, 'That drive is not JOINed'
        fail err_inuse, 'That drive is JOINed already'
        fail err_other, 'That drive is SUBSTed'
help:   db 'JOIN: a drive as a folder of another drive', 13, 10
        db '  JOIN             list the joined drives', 13, 10
        db '  JOIN X: path     X: appears as that (existing) folder', 13, 10
        db '  JOIN X: /D       undo', 13, 10, '$'
line:   db '?: => '
%endif
crlf:   db 13, 10

section .bss
lol     resd 1
cdsp    resd 1
lastdr  resb 1
drv     resb 1
tdrv    resb 1
oldflags resw 1
plen    resw 1
arg     resb 128
path    resb 128
