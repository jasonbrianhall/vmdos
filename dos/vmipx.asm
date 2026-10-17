; VMIPX.COM: an IPX driver (the Novell IPX API: INT 2Fh AX=7A00h entry
; point and INT 7Ah) on top of a packet driver -- VMPKT.COM under vmdos,
; or any other on a real PC. Network 00000000h, the card's MAC as the
; node, so DOS games find each other on the LAN. It takes IPX in all four
; Ethernet framings (Ethernet II 8137h, Novell raw 802.3, 802.2, SNAP) and
; sends Ethernet II until it hears IPX only in another framing (as from
; NetWare ODI drivers, raw 802.3 by default), then sends in that.
;   VMIPX          load (finds the packet driver on 60h-80h)
;   VMIPX 0x61     use the packet driver on that vector
;   VMIPX 802.3    always send in that framing (also 802.2, SNAP, II)
; IPX only (no SPX); one network, no routers: a packet's immediate
; address is its destination node. Sockets, listen ECBs with
; scatter/gather fragments, ESRs (at interrupt time, as Novell's), IPX
; and AES timed events, cancel, interval marker.
        cpu 286
        org 100h
        jmp install

MAXS    equ 32                          ; open sockets
S_SOCK  equ 0                           ; word, network order (0: free)
S_HEAD  equ 2                           ; dword: first listening ECB
S_SIZE  equ 6
MAXE    equ 32                          ; timed events waiting
MAXD    equ 32                          ; send completions waiting for their ESR

; the ECB
E_LINK  equ 0
E_ESR   equ 4
E_INUSE equ 8
E_CC    equ 9
E_SOCK  equ 10
E_WORK  equ 12                          ; IPX workspace: event ticks (word), kind (byte)
E_IMM   equ 28
E_FRAGN equ 34
E_FRAG  equ 36

; frame on the stack after push ds / push es / pusha / mov bp, sp
F_DI    equ 0
F_SI    equ 2
F_BX    equ 8
F_DX    equ 10
F_CX    equ 12
F_AX    equ 14
F_ES    equ 16
F_DS    equ 18

; ---------------- resident ----------------
far_entry:                              ; what INT 2Fh AX=7A00h hands out
        call ipx_main
        retf
int7a_entry:
        call ipx_main
        iret

int2f_entry:
        cmp ax, 7A00h
        jne .chain
        mov al, 0FFh
        push cs
        pop es
        mov di, far_entry
        iret
.chain: jmp far [cs:old2f]

ipx_main:
        pushf
        cli
        push ds
        push es
        pusha
        mov bp, sp
        cld
        mov ax, cs
        mov ds, ax
        mov bx, [bp + F_BX]
        cmp bx, 1Bh
        ja .bad
        shl bx, 1
        call [cs:ftab + bx]
        jmp .out
.bad:   mov byte [bp + F_AX], 0FFh
.out:   popa
        pop es
        pop ds
        popf
        ret

ftab:   dw f_open, f_close, f_target, f_send, f_listen, f_sched, f_cancel, f_aes
        dw f_marker, f_addr, f_nop, f_nop, f_none, f_none, f_none, f_none
        dw f_nospx, f_none, f_none, f_none, f_none, f_none, f_none, f_none
        dw f_none, f_none, f_maxsize, f_maxsize

f_nop:  ret
f_none: mov byte [bp + F_AX], 0FFh
        ret
f_nospx:
        mov byte [bp + F_AX], 0         ; SPX not installed
        ret
f_maxsize:
        mov word [bp + F_AX], 1500 - 30 + 30
        mov word [bp + F_CX], 0
        ret

; DX = socket (network order) -> SI = its entry (CF: not open)
find_sock:
        mov si, sockets
        mov cx, MAXS
.l:     cmp [si + S_SOCK], dx
        je .y
        add si, S_SIZE
        loop .l
        stc
        ret
.y:     clc
        ret

; BX=0 open socket: AL longevity, DX socket (0: any) -> AL 0 / FFh open already / FEh full, DX
f_open: mov dx, [bp + F_DX]
        or dx, dx
        jnz .given
.dyn:   mov ax, [next_dyn]              ; dynamic: 4000h-7FFFh
        inc ax
        and ax, 3FFFh
        or ax, 4000h
        mov [next_dyn], ax
        xchg al, ah                     ; to network order
        mov dx, ax
        call find_sock
        jnc .dyn
        jmp .take
.given: call find_sock
        jc .take
        mov byte [bp + F_AX], 0FFh
        ret
.take:  push dx
        xor dx, dx
        call find_sock                  ; a free entry
        pop dx
        jnc .ok
        mov byte [bp + F_AX], 0FEh
        ret
.ok:    mov [si + S_SOCK], dx
        mov word [si + S_HEAD], 0
        mov word [si + S_HEAD + 2], 0
        mov [bp + F_DX], dx
        mov byte [bp + F_AX], 0
        ret

; BX=1 close socket DX: its listening ECBs are cancelled
f_close:
        mov dx, [bp + F_DX]
        or dx, dx
        jz .x
        call find_sock
        jc .x
        les di, [si + S_HEAD]
.c:     mov ax, es
        or ax, di
        jz .free
        mov byte [es:di + E_INUSE], 0
        mov byte [es:di + E_CC], 0FCh
        les di, [es:di + E_LINK]
        jmp .c
.free:  mov word [si + S_SOCK], 0
        ; and its IPX events
        mov bx, events
        mov cx, MAXE
.e:     les di, [bx]
        mov ax, es
        or ax, di
        jz .en
        cmp [es:di + E_SOCK], dx
        jne .en
        cmp byte [es:di + E_WORK + 2], 0FFh
        jne .en
        mov byte [es:di + E_INUSE], 0
        mov byte [es:di + E_CC], 0FCh
        mov word [bx], 0
        mov word [bx + 2], 0
.en:    add bx, 4
        loop .e
.x:     ret

; BX=2 get local target: ES:SI address (net, node, socket) -> ES:DI immediate address, CX ticks
f_target:
        push ds
        mov ds, [bp + F_ES]
        mov si, [bp + F_SI]
        add si, 4
        mov es, [bp + F_ES]
        mov di, [bp + F_DI]
        movsw
        movsw
        movsw
        pop ds
        mov word [bp + F_CX], 1
        mov byte [bp + F_AX], 0
        ret

; BX=3 send: ES:SI = ECB -> AL 0 (the result is in the ECB; Novell's IPX
; clears AL and Doom's IPXSETUP fails on anything else)
f_send: mov byte [bp + F_AX], 0
        mov di, [bp + F_SI]
        mov es, [bp + F_ES]
        mov byte [es:di + E_INUSE], 0FFh
        ; the fragments, to tx_ipx
        mov cx, [es:di + E_FRAGN]
        lea bx, [di + E_FRAG]
        mov dx, 0                       ; bytes so far
        jcxz .bad
.frag:  push cx
        mov cx, [es:bx + 4]
        mov ax, dx
        add ax, cx
        jc .big
        cmp ax, 1492
        ja .big
        push ds
        push es
        push di
        lds si, [es:bx]
        push cs
        pop es
        mov di, tx_ipx
        add di, dx
        rep movsb
        pop di
        pop es
        pop ds
        mov dx, ax
        add bx, 6
        pop cx
        loop .frag
        jmp .built
.big:   pop cx
.bad:   mov al, 0FDh                    ; malformed
        jmp .done
.built: cmp dx, 30
        jb .bad
        ; the IPX header: checksum, length, transport control, the source address
        mov word [tx_ipx + 0], 0FFFFh
        mov ax, dx
        xchg al, ah
        mov [tx_ipx + 2], ax
        mov byte [tx_ipx + 4], 0
        mov ax, [net]
        mov [tx_ipx + 18], ax
        mov ax, [net + 2]
        mov [tx_ipx + 20], ax
        mov ax, [mac]
        mov [tx_ipx + 22], ax
        mov ax, [mac + 2]
        mov [tx_ipx + 24], ax
        mov ax, [mac + 4]
        mov [tx_ipx + 26], ax
        mov ax, [es:di + E_SOCK]
        mov [tx_ipx + 28], ax
        ; the link header in front of it -> SI
        mov si, tx_ipx
        mov al, [tx_fmt]
        cmp al, 2
        je .llc
        cmp al, 3
        je .snap
        sub si, 2
        mov ax, 3781h                   ; Ethernet II: 8137h
        cmp byte [tx_fmt], 0
        je .tl
        mov ax, dx                      ; raw 802.3: the length
        xchg al, ah
.tl:    mov [si], ax
        jmp .macs
.llc:   sub si, 3                       ; 802.2: DSAP, SSAP E0h, UI
        mov word [si], 0E0E0h
        mov byte [si + 2], 3
        jmp .len
.snap:  sub si, 8                       ; SNAP: AA AA 03, 00 00 00, 8137h
        mov word [si], 0AAAAh
        mov word [si + 2], 0003h
        mov word [si + 4], 0
        mov word [si + 6], 3781h
.len:   mov ax, tx_ipx
        sub ax, si
        add ax, dx
        xchg al, ah
        sub si, 2
        mov [si], ax
.macs:  sub si, 12
        push si
        push ds
        mov ax, cs
        mov es, ax
        mov di, si
        mov si, [bp + F_SI]
        mov ds, [bp + F_ES]
        add si, E_IMM
        movsw                           ; destination: the immediate address
        movsw
        movsw
        pop ds
        mov si, mac
        movsw                           ; source
        movsw
        movsw
        pop si
        mov cx, tx_ipx                  ; the frame's length
        add cx, dx
        sub cx, si
        cmp cx, 60
        jae .send
        mov di, si
        add di, cx
        neg cx
        add cx, 60
        xor al, al
        rep stosb
        mov cx, 60
.send:  mov ah, 4
        int 60h                         ; patched to the packet driver's vector
pkt_int1 equ $ - 1
        mov al, 0
        jnc .done
        mov al, 0FFh                    ; hardware failure
.done:  mov es, [bp + F_ES]
        mov di, [bp + F_SI]
        mov [es:di + E_CC], al
        mov byte [es:di + E_INUSE], 0
        mov ax, [es:di + E_ESR]
        or ax, [es:di + E_ESR + 2]
        jz .x
        ; its ESR runs from the timer, not inside this call
        mov bx, [done_tail]
        mov ax, bx
        add ax, 4
        cmp ax, done_end
        jb .w
        mov ax, done_q
.w:     cmp ax, [done_head]
        je .x                           ; full: no ESR
        mov [bx], di
        mov [bx + 2], es
        mov [done_tail], ax
.x:     ret

; BX=4 listen: ES:SI = ECB -> AL 0 / FFh socket not open
f_listen:
        mov es, [bp + F_ES]
        mov di, [bp + F_SI]
        mov dx, [es:di + E_SOCK]
        call find_sock
        jnc .open
        mov byte [es:di + E_INUSE], 0
        mov byte [es:di + E_CC], 0FFh
        mov byte [bp + F_AX], 0FFh
        ret
.open:  mov byte [es:di + E_INUSE], 0FEh
        mov word [es:di + E_LINK], 0
        mov word [es:di + E_LINK + 2], 0
        ; append to the socket's list
        lea bx, [si + S_HEAD]
        push ds
.walk:  mov ax, [bx]
        or ax, [bx + 2]
        jz .put
        lds bx, [bx]                    ; DS:BX = that ECB (its link is at 0)
        jmp .walk
.put:   mov [bx], di
        mov [bx + 2], es
        pop ds
        mov byte [bp + F_AX], 0
        ret

; BX=5 IPX event, BX=7 AES event: AX ticks, ES:SI = ECB
f_sched:
        mov dl, 0FFh
        jmp sched
f_aes:  mov dl, 0
sched:  mov es, [bp + F_ES]
        mov di, [bp + F_SI]
        mov ax, [bp + F_AX]
        or ax, ax
        jnz .t
        inc ax
.t:     mov [es:di + E_WORK], ax
        mov [es:di + E_WORK + 2], dl
        mov byte [es:di + E_INUSE], 0FDh
        mov bx, events
        mov cx, MAXE
.f:     mov ax, [bx]
        or ax, [bx + 2]
        jz .put
        add bx, 4
        loop .f
        mov byte [es:di + E_INUSE], 0   ; no room: done at once
        ret
.put:   mov [bx], di
        mov [bx + 2], es
        ret

; BX=6 cancel: ES:SI = ECB -> AL 0 / F9h can't / FFh not in use
f_cancel:
        mov es, [bp + F_ES]
        mov di, [bp + F_SI]
        cmp byte [es:di + E_INUSE], 0
        jne .busy
        mov byte [bp + F_AX], 0FFh
        ret
.busy:  ; a timed event?
        mov bx, events
        mov cx, MAXE
.e:     cmp [bx], di
        jne .en
        mov ax, es
        cmp [bx + 2], ax
        jne .en
        mov word [bx], 0
        mov word [bx + 2], 0
        jmp .gone
.en:    add bx, 4
        loop .e
        ; listening on a socket?
        mov si, sockets
        mov cx, MAXS
.s:     cmp word [si + S_SOCK], 0
        je .sn
        lea bx, [si + S_HEAD]
        push ds
.walk:  mov ax, [bx]
        or ax, [bx + 2]
        jz .wend
        mov ax, [bx]
        cmp ax, di
        jne .wn
        mov ax, [bx + 2]
        mov dx, es
        cmp ax, dx
        jne .wn
        mov ax, [es:di + E_LINK]        ; unlink
        mov [bx], ax
        mov ax, [es:di + E_LINK + 2]
        mov [bx + 2], ax
        pop ds
        jmp .gone
.wn:    lds bx, [bx]
        jmp .walk
.wend:  pop ds
.sn:    add si, S_SIZE
        loop .s
        mov byte [bp + F_AX], 0F9h      ; a send under way
        ret
.gone:  mov byte [es:di + E_INUSE], 0
        mov byte [es:di + E_CC], 0FCh
        mov byte [bp + F_AX], 0
        ret

; BX=8 interval marker: AX = the BIOS tick count
f_marker:
        push ds
        mov ax, 40h
        mov ds, ax
        mov ax, [6Ch]
        pop ds
        mov [bp + F_AX], ax
        ret

; BX=9 internetwork address: ES:SI <- network (4), node (6)
f_addr: mov es, [bp + F_ES]
        mov di, [bp + F_SI]
        mov si, net
        mov cx, 5
        rep movsw
        ret

; ---- the packet driver's receiver ----
receiver:
        or ax, ax
        jnz .full
        cmp cx, RXMAX
        ja .none
        cmp byte [cs:rx_busy], 0
        jne .none
        mov byte [cs:rx_busy], 1
        mov [cs:rx_len], cx
        push cs
        pop es
        mov di, rxbuf
        retf
.none:  xor di, di
        mov es, di
        retf
.full:  push ds
        push es
        pusha
        mov ax, cs
        mov ds, ax
        cld
        call got_frame
        mov byte [cs:rx_busy], 0
        popa
        pop es
        pop ds
        retf

; the frame in rxbuf: to the first ECB listening on its socket
got_frame:
        mov cx, [rx_len]
        cmp cx, 14 + 30
        jb .x
        mov si, rxbuf + 14
        mov ax, [rxbuf + 12]
        xchg al, ah
        mov bl, 0
        cmp ax, 8137h                   ; Ethernet II
        je .ipx
        cmp ax, 1500
        ja .x
        add ax, 14                      ; 802.3: the frame without padding
        cmp ax, cx
        ja .x
        mov cx, ax
        mov bl, 1
        cmp word [si], 0FFFFh           ; raw 802.3: IPX's checksum right away
        je .ipx
        mov bl, 2
        cmp word [si], 0E0E0h           ; 802.2
        jne .snap
        cmp byte [si + 2], 3
        jne .x
        add si, 3
        jmp .ipx
.snap:  mov bl, 3
        cmp word [si], 0AAAAh
        jne .x
        cmp word [si + 2], 0003h
        jne .x
        cmp word [si + 4], 0
        jne .x
        cmp word [si + 6], 3781h
        jne .x
        add si, 8
.ipx:   mov [rx_ipx], si
        add cx, rxbuf                   ; CX = bytes from the IPX header on
        sub cx, si
        mov ax, [si + 2]                ; the IPX length
        xchg al, ah
        cmp ax, 30
        jb .x
        cmp ax, cx
        ja .x
        mov [ipx_len], ax
        ; for this node (or everyone)?
        mov bp, si
        add si, 10
        mov di, mac
        push cs
        pop es
        mov cx, 3
        repe cmpsw
        je .mine
        cmp word [ds:bp + 10], 0FFFFh
        jne .x
        cmp word [ds:bp + 12], 0FFFFh
        jne .x
        cmp word [ds:bp + 14], 0FFFFh
        jne .x
.mine:  ; the framing to send in: Ethernet II, unless IPX only comes in another
        cmp bl, 0
        jne .other
        mov byte [seen_ii], 1
        jmp .learnt
.other: cmp byte [fmt_fixed], 0
        jne .learnt
        cmp byte [seen_ii], 0
        jne .learnt
        mov [tx_fmt], bl
.learnt:
        mov dx, [ds:bp + 16]
        call find_sock
        jc .x
        les di, [si + S_HEAD]
        mov ax, es
        or ax, di
        jz .x
        mov ax, [es:di + E_LINK]        ; take it off the list
        mov [si + S_HEAD], ax
        mov ax, [es:di + E_LINK + 2]
        mov [si + S_HEAD + 2], ax
        ; the sender's MAC
        mov ax, [rxbuf + 6]
        mov [es:di + E_IMM], ax
        mov ax, [rxbuf + 8]
        mov [es:di + E_IMM + 2], ax
        mov ax, [rxbuf + 10]
        mov [es:di + E_IMM + 4], ax
        ; scatter into the fragments
        mov dx, [ipx_len]               ; left to copy
        mov si, [rx_ipx]
        mov cx, [es:di + E_FRAGN]
        lea bx, [di + E_FRAG]
        mov byte [cc], 0
        jcxz .over
.frag:  or dx, dx
        jz .copied
        push cx
        push di
        push es
        mov cx, [es:bx + 4]
        cmp cx, dx
        jbe .n
        mov cx, dx
.n:     sub dx, cx
        les di, [es:bx]
        rep movsb
        pop es
        pop di
        pop cx
        add bx, 6
        loop .frag
        or dx, dx
        jz .copied
.over:  mov byte [cc], 0FDh             ; it didn't fit
.copied:
        mov al, [cc]
        mov [es:di + E_CC], al
        mov byte [es:di + E_INUSE], 0
        mov al, 0FFh
        call run_esr
.x:     ret

; ES:DI = ECB, AL = FFh (IPX) / 0 (AES): call its ESR, if it has one
run_esr:
        mov dx, [es:di + E_ESR]
        or dx, [es:di + E_ESR + 2]
        jz .x
        push ds
        push es
        push bp
        push di
        mov si, di
        mov dx, [es:di + E_ESR]
        mov [esr_ptr], dx
        mov dx, [es:di + E_ESR + 2]
        mov [esr_ptr + 2], dx
        cli
        call far [cs:esr_ptr]
        cli
        pop di
        pop bp
        pop es
        pop ds
        cld
.x:     ret

; ---- the timer: timed events, ESRs of finished sends ----
int8_entry:
        pushf
        call far [cs:old8]
        push ds
        push es
        pusha
        mov ax, cs
        mov ds, ax
        cmp byte [t_busy], 0
        jne .out
        mov byte [t_busy], 1
        mov [t_ss], ss
        mov [t_sp], sp
        mov ss, ax
        mov sp, t_stack_top
        cld
        ; timed events
        mov bx, events
        mov cx, MAXE
.e:     les di, [bx]
        mov ax, es
        or ax, di
        jz .en
        dec word [es:di + E_WORK]
        jnz .en
        mov word [bx], 0
        mov word [bx + 2], 0
        mov byte [es:di + E_CC], 0
        mov byte [es:di + E_INUSE], 0
        mov al, [es:di + E_WORK + 2]
        push bx
        push cx
        call run_esr
        pop cx
        pop bx
        mov ax, cs
        mov ds, ax
.en:    add bx, 4
        loop .e
        ; finished sends
.d:     mov bx, [done_head]
        cmp bx, [done_tail]
        je .dd
        les di, [bx]
        add bx, 4
        cmp bx, done_end
        jb .w
        mov bx, done_q
.w:     mov [done_head], bx
        mov al, 0FFh
        call run_esr
        mov ax, cs
        mov ds, ax
        jmp .d
.dd:    cli
        mov ss, [t_ss]
        mov sp, [t_sp]
        mov byte [t_busy], 0
.out:   popa
        pop es
        pop ds
        iret

RXMAX   equ 1514
old2f:      dd 0
old7a:      dd 0
old8:       dd 0
esr_ptr:    dd 0
net:        dd 0                        ; network 00000000h
mac:        times 6 db 0                ; (follows net: GetInternetworkAddress copies both)
next_dyn:   dw 3FFFh
handle:     dw 0
rx_busy:    db 0
t_busy:     db 0
cc:         db 0
rx_len:     dw 0
ipx_len:    dw 0
t_ss:       dw 0
t_sp:       dw 0
done_head:  dw done_q
done_tail:  dw done_q
done_q:     times MAXD dd 0
done_end:
events:     times MAXE dd 0
sockets:    times MAXS * S_SIZE db 0
tx_fmt:     db 0                        ; 0 Ethernet II, 1 raw 802.3, 2 802.2, 3 SNAP
fmt_fixed:  db 0
seen_ii:    db 0
rx_ipx:     dw 0
txbuf:      times 22 db 0               ; the link header goes in front of
tx_ipx:     times 1500 db 0             ; the IPX packet
rxbuf:      times RXMAX db 0
            align 2
            times 1024 db 0
t_stack_top:
resident_end:

; ---------------- install ----------------
install:
        mov ax, 7A00h                   ; an IPX driver already?
        int 2Fh
        cmp al, 0FFh
        jne .noipx
        mov dx, msg_already
        jmp fail
.noipx: mov si, 81h
.arg:   call skipsp
        cmp byte [si], 13
        je .args
        mov di, kw_8023
        mov bl, 1
        call keyword
        je .fmt
        mov di, kw_8022
        mov bl, 2
        call keyword
        je .fmt
        mov di, kw_snap
        mov bl, 3
        call keyword
        je .fmt
        mov di, kw_ii
        mov bl, 0
        call keyword
        je .fmt
        call hexnum
        jc usage
        dec si
        mov [want_vec], al
        jmp .arg
.fmt:   mov [tx_fmt], bl
        mov byte [fmt_fixed], 1
        jmp .arg
.args:  mov al, [want_vec]
        or al, al
        jz .scan
        call has_sig
        jnc .nopkt
        jmp .have
.scan:  mov al, 60h                     ; the first packet driver on 60h-80h
.s:     call has_sig
        jc .have
        inc al
        cmp al, 80h
        jbe .s
.nopkt: mov dx, msg_nopkt
        jmp fail
.have:  mov [pkt_int1], al
        mov [pkt_int2], al
        mov [pkt_int3], al
        mov [pkt_int4], al
        mov [pkt_vec], al

        mov ax, 0201h                   ; access_type: class 1, IPX's EtherType
        mov bx, 0FFFFh
        xor dl, dl
        mov si, ipx_type
        mov cx, 2
        push cs
        pop es
        mov di, receiver
        int 60h
pkt_int2 equ $ - 1
        jnc .acc
        mov dx, msg_type
        jmp fail
.acc:   mov [handle], ax
        mov ax, 0201h                   ; and everything else, for IPX in 802.3 frames
        mov bx, 0FFFFh                  ; (if nobody has asked for that)
        xor dl, dl
        mov si, ipx_type
        xor cx, cx
        push cs
        pop es
        mov di, receiver
        int 60h
pkt_int4 equ $ - 1
        mov ax, [handle]
        mov bx, ax
        mov ah, 6                       ; get_address
        push cs
        pop es
        mov di, mac
        mov cx, 6
        int 60h
pkt_int3 equ $ - 1
        jnc .addr
        mov dx, msg_addr
        jmp fail

.addr:  mov ax, 352Fh
        int 21h
        mov [old2f], bx
        mov [old2f + 2], es
        mov ax, 357Ah
        int 21h
        mov [old7a], bx
        mov [old7a + 2], es
        mov ax, 3508h
        int 21h
        mov [old8], bx
        mov [old8 + 2], es
        mov ax, 252Fh
        mov dx, int2f_entry
        int 21h
        mov ax, 257Ah
        mov dx, int7a_entry
        int 21h
        mov ax, 2508h
        mov dx, int8_entry
        int 21h

        mov dx, msg_ok1
        call puts
        mov al, [pkt_vec]
        call puthex2
        mov dx, msg_ok2
        call puts
        mov bl, [tx_fmt]
        xor bh, bh
        shl bx, 1
        mov dx, [fmt_names + bx]
        call puts
        cmp byte [fmt_fixed], 0
        jne .fixed
        mov dx, msg_auto
        call puts
.fixed: mov dx, msg_ok3
        call puts
        mov si, mac
        mov cx, 6
.mac:   lodsb
        call puthex2
        loop .mac
        mov dx, msg_crlf
        call puts

        mov es, [2Ch]                   ; free the environment
        mov ah, 49h
        int 21h
        mov dx, (resident_end - $$ + 100h + 15) / 16
        mov ax, 3100h
        int 21h

usage:  mov dx, msg_usage
fail:   call puts
        mov ax, 4C01h
        int 21h

; AL = vector: CF if a packet driver is there
has_sig:
        push ax
        push bx
        push es
        mov ah, 35h
        int 21h
        lea di, [bx + 3]
        mov si, sig
        mov cx, 8
        repe cmpsb
        pop es
        pop bx
        pop ax
        je .yes
        clc
        ret
.yes:   stc
        ret

; [SI] the keyword at DI (lower case, 0-terminated), whole word? ZF, SI past it
keyword:
        push si
.k:     mov al, [di]
        or al, al
        jz .end
        mov ah, [si]
        cmp ah, 'A'
        jb .c
        cmp ah, 'Z'
        ja .c
        or ah, 20h
.c:     cmp al, ah
        jne .no
        inc si
        inc di
        jmp .k
.end:   mov al, [si]
        cmp al, ' '
        je .yes
        cmp al, 9
        je .yes
        cmp al, 13
        je .yes
.no:    pop si
        or sp, sp                       ; NZ
        ret
.yes:   add sp, 2
        cmp al, al                      ; ZF
        ret

skipsp: lodsb
        cmp al, ' '
        je skipsp
        cmp al, 9
        je skipsp
        dec si
        ret

; [SI]: "0x61", "61h" or "61" (hex) -> AL, CF if none
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
.end:   mov ax, dx
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
.nib:   add al, '0'
        cmp al, '9'
        jbe .p
        add al, 7
.p:     mov dl, al
        mov ah, 2
        int 21h
        ret

sig:        db 'PKT DRVR'
ipx_type:   db 81h, 37h
pkt_vec:    db 0
msg_ok1:    db 'VMIPX: IPX on the packet driver at INT $'
msg_ok2:    db 'h, $'
msg_auto:   db ' (auto)$'
msg_ok3:    db ', network 00000000, node $'
fmt_names:  dw f_ii, f_8023, f_8022, f_snap
f_ii:       db 'Ethernet II$'
f_8023:     db '802.3$'
f_8022:     db '802.2$'
f_snap:     db 'SNAP$'
kw_8023:    db '802.3', 0
kw_8022:    db '802.2', 0
kw_snap:    db 'snap', 0
kw_ii:      db 'ii', 0
want_vec:   db 0
msg_crlf:   db 13, 10, '$'
msg_already: db 'VMIPX: an IPX driver is loaded already', 13, 10, '$'
msg_nopkt:  db 'VMIPX: no packet driver (load VMPKT first)', 13, 10, '$'
msg_type:   db 'VMIPX: the packet driver refused IPX', 27h, 's frame type', 13, 10, '$'
msg_addr:   db 'VMIPX: the packet driver gave no address', 13, 10, '$'
msg_usage:  db 'usage: VMIPX [packet driver vector, e.g. 0x60] [802.3 | 802.2 | SNAP | II]', 13, 10, '$'
