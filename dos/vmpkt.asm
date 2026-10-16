; VMPKT.COM: a packet driver (FTP Packet Driver Specification 1.11) for the
; PC's Ethernet card, which vmdos drives (src/net.cpp). Anything written for
; a packet driver runs on it: mTCP, WATTCP programs, an IPX driver on top
; (VMIPX.COM), NCSA Telnet, Arachne...
;   VMPKT                  install on INT 60h, frames arrive on IRQ 10
;   VMPKT 0x61             another vector (60h-80h)
;   VMPKT 0x60 11          another IRQ for frames (3-15; one nothing else uses)
;   VMPKT /U               uninstall (if nothing has hooked over it)
; The IRQ is only the monitor's virtual one: vmdos raises it while received
; frames wait (INT 2Fh AX=564Eh, dos/vmapi.inc), the card itself stays out
; of DOS's sight. Basic and extended functions (receive modes, statistics);
; no multicast lists.
        cpu 286
        org 100h
        jmp install

; ---------------- resident ----------------
pkt_entry:
        jmp short pkt_go
        nop
        db 'PKT DRVR', 0

MAXH    equ 8
H_USED  equ 0                           ; byte
H_TLEN  equ 1                           ; byte: type length (0: everything)
H_TYPE  equ 2                           ; 8 bytes
H_RECV  equ 10                          ; dword: the receiver
H_SIZE  equ 14

; frame on the stack after push ds / push es / pusha / mov bp, sp
F_DI    equ 0
F_SI    equ 2
F_BX    equ 8
F_DX    equ 10
F_CX    equ 12
F_AX    equ 14
F_ES    equ 16
F_DS    equ 18
F_FL    equ 24

pkt_go: push ds
        push es
        pusha
        mov bp, sp
        cld
        push cs
        pop ds
        mov bl, ah
        xor bh, bh
        cmp bx, 25
        ja bad_command
        shl bx, 1
        jmp [cs:ftab + bx]

ftab:   dw bad_command, f_info, f_access, f_release, f_send, f_terminate, f_getaddr, f_reset
        dw bad_command, bad_command, f_params, bad_command, bad_command, bad_command, bad_command, bad_command
        dw bad_command, bad_command, bad_command, bad_command, f_setmode, f_getmode, no_multicast, no_multicast
        dw f_stats, cant_set

ok:     and byte [bp + F_FL], 0FEh
        popa
        pop es
        pop ds
        iret

err:    mov [bp + F_DX + 1], dh         ; DH = error code
        or byte [bp + F_FL], 1
        popa
        pop es
        pop ds
        iret

bad_handle:   mov dh, 1
              jmp err
no_class:     mov dh, 2
              jmp err
no_number:    mov dh, 4
              jmp err
bad_type:     mov dh, 5
              jmp err
no_multicast: mov dh, 6
              jmp err
bad_mode:     mov dh, 8
              jmp err
no_space:     mov dh, 9
              jmp err
type_inuse:   mov dh, 10
              jmp err
bad_command:  mov dh, 11
              jmp err
cant_send:    mov dh, 12
              jmp err
cant_set:     mov dh, 13
              jmp err

; AH=1 driver_info
f_info: mov word [bp + F_BX], 1         ; version
        mov word [bp + F_CX], 0100h     ; class 1 (DIX Ethernet), number 0
        mov word [bp + F_DX], 0FFFFh    ; type: generic
        mov byte [bp + F_AX], 2         ; basic + extended
        mov word [bp + F_SI], drv_name
        mov [bp + F_DS], cs
        jmp ok

; BX = handle -> SI = its slot (CF: no such handle)
handle_slot:
        dec bx
        cmp bx, MAXH
        jae .no
        mov si, bx
        imul si, si, H_SIZE
        add si, handles
        cmp byte [si + H_USED], 0
        je .no
        clc
        ret
.no:    stc
        ret

; AH=2 access_type: AL class, BX type, DL number, DS:SI template, CX its length, ES:DI receiver
f_access:
        mov ax, [bp + F_AX]
        cmp al, 1
        jne no_class
        cmp byte [bp + F_DX], 0
        jne no_number
        mov cx, [bp + F_CX]
        cmp cx, 8
        ja bad_type
        ; already taken (same template)?
        mov di, handles
        mov bx, MAXH
.dup:   cmp byte [di + H_USED], 0
        je .dnext
        cmp [di + H_TLEN], cl
        jne .dnext
        push cx
        push di
        push ds
        mov ds, [bp + F_DS]
        mov si, [bp + F_SI]
        add di, H_TYPE
        push cs
        pop es
        repe cmpsb
        pop ds
        pop di
        pop cx
        je type_inuse
.dnext: add di, H_SIZE
        dec bx
        jnz .dup
        ; a free slot
        mov di, handles
        xor bx, bx
.free:  cmp byte [di + H_USED], 0
        je .got
        add di, H_SIZE
        inc bx
        cmp bx, MAXH
        jb .free
        jmp no_space
.got:   mov byte [di + H_USED], 1
        mov [di + H_TLEN], cl
        mov ax, [bp + F_DI]
        mov [di + H_RECV], ax
        mov ax, [bp + F_ES]
        mov [di + H_RECV + 2], ax
        push cs
        pop es
        add di, H_TYPE
        push ds
        mov ds, [bp + F_DS]
        mov si, [bp + F_SI]
        rep movsb
        pop ds
        inc bx
        mov [bp + F_AX], bx             ; the handle
        jmp ok

; AH=3 release_type, AH=5 terminate: BX = handle
f_release:
f_terminate:
        mov bx, [bp + F_BX]
        call handle_slot
        jc bad_handle
        mov byte [si + H_USED], 0
        jmp ok

; AH=4 send_pkt: DS:SI, CX bytes
f_send: mov ax, 564Eh
        mov bl, 2
        mov cx, [bp + F_CX]
        mov si, [bp + F_SI]
        mov ds, [bp + F_DS]
        call vmapi
        push cs
        pop ds
        or ax, ax
        jnz cant_send
        jmp ok

; AH=6 get_address: BX handle, ES:DI buffer, CX its length
f_getaddr:
        mov bx, [bp + F_BX]
        call handle_slot
        jc bad_handle
        cmp word [bp + F_CX], 6
        jb no_space
        mov es, [bp + F_ES]
        mov di, [bp + F_DI]
        mov si, mac
        mov cx, 6
        rep movsb
        mov word [bp + F_CX], 6
        jmp ok

; AH=7 reset_interface
f_reset:
        jmp ok

; AH=0Ah get_parameters: ES:DI <- the table
f_params:
        mov word [bp + F_DI], params
        mov [bp + F_ES], cs
        jmp ok

; AH=14h set_rcv_mode: BX handle, CX mode
f_setmode:
        mov bx, [bp + F_BX]
        call handle_slot
        jc bad_handle
        mov cx, [bp + F_CX]
        cmp cx, 1
        jb bad_mode
        cmp cx, 6
        ja bad_mode
        mov ax, 564Eh
        mov bl, 4
        call vmapi
        or ax, ax
        jnz bad_mode
        mov [rcv_mode], cx
        jmp ok

; AH=15h get_rcv_mode: AX = mode
f_getmode:
        mov bx, [bp + F_BX]
        call handle_slot
        jc bad_handle
        mov ax, [rcv_mode]
        mov [bp + F_AX], ax
        jmp ok

; AH=18h get_statistics: DS:SI <- the counters
f_stats:
        mov bx, [bp + F_BX]
        call handle_slot
        jc bad_handle
        mov ax, 564Eh
        mov bl, 5
        push cs
        pop es
        mov di, stats
        call vmapi
        mov word [bp + F_SI], stats
        mov [bp + F_DS], cs
        jmp ok

; ---- frames in: the IRQ vmdos raises while some wait ----
irq_entry:
        push ax
        push ds
        push cs
        pop ds
        mov al, 20h                     ; EOI first: the busy flag keeps us from nesting
        cmp byte [irq_no], 8
        jb .m
        out 0A0h, al
.m:     out 20h, al
        cmp byte [busy], 0
        jne .out
        mov byte [busy], 1
        mov [old_ss], ss
        mov [old_sp], sp
        cli
        mov ax, cs
        mov ss, ax
        mov sp, stack_top
        push es
        pusha
        cld
        mov byte [budget], 32
.next:  push cs
        pop ds
        push cs
        pop es
        mov ax, 564Eh
        mov bl, 3
        mov di, rxbuf
        mov cx, 1514
        call vmapi
        jcxz .done
        cmp cx, 1514
        jbe .len
        mov cx, 1514
.len:   mov [rxlen], cx
        call deliver
        dec byte [budget]
        jnz .next
.done:  popa
        pop es
        cli
        mov ss, [cs:old_ss]
        mov sp, [cs:old_sp]
        mov byte [cs:busy], 0
.out:   pop ds
        pop ax
        iret

; the frame in rxbuf (rxlen bytes) to the handle whose type matches:
; a specific type first, then one that takes everything
deliver:
        mov si, handles
        mov dx, MAXH
.spec:  cmp byte [si + H_USED], 0
        je .snext
        mov cl, [si + H_TLEN]
        xor ch, ch
        jcxz .snext
        push si
        mov di, rxbuf + 12
        add si, H_TYPE
        repe cmpsb
        pop si
        je upcall
.snext: add si, H_SIZE
        dec dx
        jnz .spec
        mov si, handles
        mov dx, MAXH
.any:   cmp byte [si + H_USED], 0
        je .anext
        cmp byte [si + H_TLEN], 0
        je upcall
.anext: add si, H_SIZE
        dec dx
        jnz .any
        ret                             ; nobody wants it

; SI = the handle's slot
upcall: mov bx, si
        sub bx, handles
        mov ax, bx
        mov bl, H_SIZE
        div bl
        xor ah, ah
        inc ax
        mov [cur_handle], ax
        mov ax, [si + H_RECV]
        mov [recv], ax
        mov ax, [si + H_RECV + 2]
        mov [recv + 2], ax
        xor ax, ax                      ; first call: a buffer, please
        mov bx, [cur_handle]
        mov cx, [rxlen]
        call far [cs:recv]
        mov ax, es
        or ax, di
        jz .drop
        push di
        push es
        push cs
        pop ds
        mov si, rxbuf
        mov cx, [rxlen]
        cld
        rep movsb
        pop ds                          ; second call: DS:SI = their copy
        pop si
        mov ax, 1
        mov bx, [cs:cur_handle]
        mov cx, [cs:rxlen]
        call far [cs:recv]
.drop:  push cs
        pop ds
        cld
        ret

%include "vmapi.inc"

drv_name:   db 'VMPKT (vmdos)', 0
params:     db 1, 11                    ; spec 1.11
            db 14, 6                    ; table length, address length
            dw 1514, 0                  ; MTU, multicast entries
            dw 8, 4                     ; receive, send buffers
irq_vec:    dw 0                        ; interrupt number
mac:        times 6 db 0
rcv_mode:   dw 3
stats:      times 7 dd 0
handles:    times MAXH * H_SIZE db 0
irq_no:     db 10
busy:       db 0
budget:     db 0
old_ss:     dw 0
old_sp:     dw 0
cur_handle: dw 0
rxlen:      dw 0
recv:       dd 0
old_pkt:    dd 0
old_irq:    dd 0
pkt_vec:    db 60h
rxbuf:      times 1514 db 0
            align 2
            times 2048 db 0
stack_top:
resident_end:

; ---------------- install ----------------
install:
        mov si, 81h
        call skipsp
        cmp al, '/'
        jne .args
        mov al, [si + 1]
        or al, 20h
        cmp al, 'u'
        je uninstall
        jmp usage
.args:  cmp al, 13
        je .query
        call hexnum                     ; the vector
        jc usage
        cmp ax, 60h
        jb usage
        cmp ax, 80h
        ja usage
        mov [pkt_vec], al
        call skipsp
        cmp al, 13
        je .query
        call decnum                     ; the IRQ
        jc usage
        cmp ax, 3
        jb usage
        cmp ax, 15
        ja usage
        mov [irq_no], al

.query: mov ax, 564Eh
        xor bl, bl
        push cs
        pop es
        mov di, mac
        call vmapi
        cmp bx, 564Dh
        jne .novm
        or ax, ax
        jnz .card
        mov dx, msg_nocard
        jmp fail
.novm:  mov dx, msg_novm
        jmp fail
.card:  mov [link], cl

        ; a packet driver there already?
        mov al, [pkt_vec]
        call has_sig
        jnc .free
        mov dx, msg_already
        jmp fail

.free:  ; hook the vectors
        mov al, [pkt_vec]
        mov ah, 35h
        int 21h
        mov [old_pkt], bx
        mov [old_pkt + 2], es
        mov al, [irq_no]
        call irq_vector
        mov [irq_vec], ax
        mov ah, 35h
        int 21h
        mov [old_irq], bx
        mov [old_irq + 2], es
        mov al, [pkt_vec]
        mov ah, 25h
        mov dx, pkt_entry
        int 21h
        mov ax, [irq_vec]
        mov ah, 25h
        mov dx, irq_entry
        int 21h
        ; unmask the IRQ (and the cascade)
        cli
        mov cl, [irq_no]
        cmp cl, 8
        jb .lo
        sub cl, 8
        mov ah, 1
        shl ah, cl
        not ah
        in al, 0A1h
        and al, ah
        out 0A1h, al
        in al, 21h
        and al, 0FBh
        out 21h, al
        jmp .tell
.lo:    mov ah, 1
        shl ah, cl
        not ah
        in al, 21h
        and al, ah
        out 21h, al
.tell:  sti
        mov ax, 564Eh
        mov bl, 1
        mov cl, [irq_no]
        call vmapi

        ; report
        mov dx, msg_ok1
        call puts
        mov al, [pkt_vec]
        call puthex2
        mov dx, msg_ok2
        call puts
        mov al, [irq_no]
        xor ah, ah
        call putdec
        mov dx, msg_ok3
        call puts
        mov si, mac
        mov cx, 6
.mac:   lodsb
        call puthex2
        dec cx
        jz .macd
        mov dl, ':'
        mov ah, 2
        int 21h
        jmp .mac
.macd:  mov dx, msg_up
        cmp byte [link], 0
        jne .l
        mov dx, msg_down
.l:     call puts

        mov es, [2Ch]                   ; free the environment
        mov ah, 49h
        int 21h
        mov dx, (resident_end - $$ + 100h + 15) / 16
        mov ax, 3100h
        int 21h

; ---- VMPKT /U ----
uninstall:
        mov al, [pkt_vec]               ; on 60h-80h: the first VMPKT found
.scan:  call has_sig
        jnc .nxt
        ; ES = its segment: is it ours?
        mov di, drv_name
        mov si, di
        mov cx, 13
        repe cmpsb
        je .found
.nxt:   inc al
        cmp al, 80h
        jbe .scan
        mov dx, msg_notin
        jmp fail
.found: mov [pkt_vec], al
        mov bx, es
        mov [tsr_seg], bx
        ; both vectors still ours?
        mov ah, 35h
        int 21h
        mov ax, es
        cmp ax, [tsr_seg]
        jne .hooked
        mov es, [tsr_seg]
        mov ax, [es:irq_vec]
        mov ah, 35h
        int 21h
        mov ax, es
        cmp ax, [tsr_seg]
        jne .hooked
        mov ax, 564Eh                   ; no more IRQ
        mov bl, 1
        mov cl, 0FFh
        call vmapi
        mov es, [tsr_seg]
        push ds
        lds dx, [es:old_pkt]
        mov al, [cs:pkt_vec]
        mov ah, 25h
        int 21h
        pop ds
        push ds
        mov ax, [es:irq_vec]
        lds dx, [es:old_irq]
        mov ah, 25h
        int 21h
        pop ds
        mov ah, 49h                     ; ES = the TSR
        int 21h
        mov dx, msg_gone
        call puts
        mov ax, 4C00h
        int 21h
.hooked:
        mov dx, msg_hooked
        jmp fail

usage:  mov dx, msg_usage
fail:   call puts
        mov ax, 4C01h
        int 21h

; AL = vector: CF if a packet driver is there (ES = its segment)
has_sig:
        push ax
        push si
        push di
        push cx
        mov ah, 35h
        int 21h
        lea di, [bx + 3]
        mov si, sig
        mov cx, 8
        repe cmpsb
        pop cx
        pop di
        pop si
        pop ax
        je .yes
        clc
        ret
.yes:   stc
        ret

; AL = IRQ -> AX = its interrupt
irq_vector:
        xor ah, ah
        cmp al, 8
        jb .m
        add al, 70h - 8
        ret
.m:     add al, 8
        ret

skipsp: lodsb
        cmp al, ' '
        je skipsp
        cmp al, 9
        je skipsp
        dec si
        ret

; [SI]: "0x60", "60h" or "60" (hex) -> AX, CF if none
hexnum: xor dx, dx
        xor cx, cx
        cmp word [si], 'x0'
        je .x
        cmp word [si], 'X0'
        jne .d
.x:     add si, 2
.d:     lodsb
        mov ah, al
        sub al, '0'
        cmp al, 9
        jbe .add
        mov al, ah
        or al, 20h
        sub al, 'a'
        cmp al, 5
        ja .end
        add al, 10
.add:   xor ah, ah
        shl dx, 4
        add dx, ax
        inc cx
        jmp .d
.end:   or ah, 20h
        cmp ah, 'h'
        je .h
        dec si
.h:     mov ax, dx
        cmp cx, 1
        ret                             ; CF if no digits

; [SI] decimal -> AX, CF if none
decnum: xor dx, dx
        xor cx, cx
.d:     lodsb
        sub al, '0'
        cmp al, 9
        ja .end
        xor ah, ah
        imul dx, dx, 10
        add dx, ax
        inc cx
        jmp .d
.end:   dec si
        mov ax, dx
        cmp cx, 1
        ret

puts:   mov ah, 9
        int 21h
        ret

puthex2:
        push ax
        shr al, 4
        call .nib
        pop ax
        and al, 0Fh
.nib:   push ax
        add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     mov dl, al
        mov ah, 2
        int 21h
        pop ax
        ret

putdec: mov bx, 10
        xor cx, cx
.d:     xor dx, dx
        div bx
        push dx
        inc cx
        or ax, ax
        jnz .d
.p:     pop dx
        add dl, '0'
        mov ah, 2
        int 21h
        loop .p
        ret

sig:        db 'PKT DRVR'
link:       db 0
tsr_seg:    dw 0
msg_ok1:    db 'VMPKT: packet driver on INT $'
msg_ok2:    db 'h, IRQ $'
msg_ok3:    db ', MAC $'
msg_up:     db ', link up', 13, 10, '$'
msg_down:   db ', link down', 13, 10, '$'
msg_novm:   db 'VMPKT: not running under vmdos', 13, 10, '$'
msg_nocard: db 'VMPKT: vmdos found no Ethernet card it can drive (see DMESG)', 13, 10, '$'
msg_already: db 'VMPKT: there is a packet driver on that vector already', 13, 10, '$'
msg_notin:  db 'VMPKT: not installed', 13, 10, '$'
msg_hooked: db 'VMPKT: something has hooked over it; not removed', 13, 10, '$'
msg_gone:   db 'VMPKT: removed', 13, 10, '$'
msg_usage:  db 'usage: VMPKT [vector [irq]]   e.g. VMPKT 0x60 10', 13, 10
            db '       VMPKT /U               uninstall', 13, 10, '$'
