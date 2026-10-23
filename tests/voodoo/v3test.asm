; V3TEST.COM: draws on the emulated Voodoo 3 (vmdos voodoo3) as a Glide
; program would, from DOS: finds the card through the PCI BIOS, enters
; protected mode through DPMI, maps the card's registers and frame buffer
; (DPMI 0800h) and writes them: a 640x480 16-bit screen, a fill, a Gouraud
; triangle through the setup unit (its vertices stored with FSTP, as
; compiled Glide code does), then the command FIFO in the frame buffer
; filling the back buffer, shown through the overlay after a swap. Waits
; about 10 seconds (time for a screenshot), sets text mode and exits.
;   nasm -f bin -o V3TEST.COM v3test.asm
        cpu 686
        bits 16
        org 100h

W       equ 640
H       equ 480
STRIDE  equ W*2
FRONT   equ 0
BACK    equ 100000h
FIFO    equ 400000h

start:  mov bx, (progend - start + 100h + 15) / 16 + 1
        mov ah, 4Ah                     ; give back what the .COM doesn't use
        int 21h

        mov ax, 0B101h                  ; PCI BIOS?
        int 1Ah
        jc .nopci
        cmp edx, 20494350h
        jne .nopci
        mov ax, 0B102h                  ; find 121Ah:0005h
        mov cx, 0005h
        mov dx, 121Ah
        xor si, si
        int 1Ah
        jc .nocard
        mov [devfn], bx
        mov ax, 0B10Ah
        mov di, 10h
        int 1Ah
        and ecx, 0FFFFFFF0h
        mov [bar0], ecx
        mov ax, 0B10Ah
        mov bx, [devfn]
        mov di, 14h
        int 1Ah
        and ecx, 0FFFFFFF0h
        mov [bar1], ecx
        mov dx, msg_found
        mov ah, 9
        int 21h

        mov ax, 1687h                   ; DPMI
        int 2Fh
        test ax, ax
        jnz .nodpmi
        mov [entry], di
        mov [entry + 2], es
        mov bx, si
        mov ah, 48h
        int 21h
        jc .nodpmi
        mov es, ax
        mov ax, 1                       ; a 32-bit client
        call far [entry]
        jc .nodpmi
        jmp pm

.nopci: mov dx, msg_nopci
        jmp short .fail
.nocard: mov dx, msg_nocard
        jmp short .fail
.nodpmi: mov dx, msg_nodpmi
.fail:  mov ah, 9
        int 21h
        mov ax, 4C01h
        int 21h

; ---------------- protected mode (16-bit code, 32-bit addresses through FS) ----------------
pm:     xor ax, ax                      ; a flat 4 GB data selector
        mov cx, 1
        int 31h
        jc die
        mov [flat], ax
        mov bx, ax
        mov ax, 0007h
        xor cx, cx
        xor dx, dx
        int 31h
        mov ax, 0008h
        mov cx, 0FFFFh
        mov dx, 0FFFFh
        int 31h
        mov ax, 0009h
        mov cx, 0C0F2h                  ; 4 KB granular, 32-bit, data
        int 31h
        mov ebx, [bar0]                 ; map the registers, 32 MB
        call map
        mov [lin0], ebx
        mov ebx, [bar1]                 ; and the frame buffer
        call map
        mov [lin1], ebx
        mov fs, [flat]

        mov esi, [lin0]                 ; ESI: the I/O remap (BAR0 + 0)
        lea edi, [esi + 200000h]        ; EDI: the 3D registers
        mov dword [fs:esi + 28h], 1000h ; vgaInit0: extended shift out
        mov dword [fs:esi + 98h], W | H << 12   ; vidScreenSize
        mov dword [fs:esi + 0E4h], FRONT        ; vidDesktopStartAddr
        mov dword [fs:esi + 0E8h], STRIDE | STRIDE << 16
        mov dword [fs:esi + 5Ch], 1 | 1 << 18 | 1 << 10 | 1 << 11   ; vidProcCfg: on, 565, no CLUT

        mov dword [fs:edi + 1ECh], FRONT        ; colBufferAddr
        mov dword [fs:edi + 1F0h], STRIDE       ; colBufferStride
        mov dword [fs:edi + 1F4h], 200000h      ; auxBufferAddr
        mov dword [fs:edi + 1F8h], STRIDE
        mov dword [fs:edi + 118h], W            ; clipLeftRight
        mov dword [fs:edi + 11Ch], H            ; clipLowYHighY
        mov dword [fs:edi + 110h], 1 | 1 << 9   ; fbzMode: clip, RGB
        mov dword [fs:edi + 148h], 80h          ; color1: dark blue
        mov dword [fs:edi + 124h], 0            ; fastfillCMD

        mov dword [fs:edi + 104h], 1 << 8 | 1 << 14   ; fbzColorPath: iterated RGB
        mov dword [fs:edi + 260h], 1            ; sSetupMode: RGB
        mov bx, vtx                             ; three vertices: x, y (floats), ARGB
        mov cx, 3
.vtx:   fld dword [bx]                          ; as compiled Glide: FSTP into the card
        fstp dword [fs:edi + 264h]              ; sVx
        fld dword [bx + 4]
        fstp dword [fs:edi + 268h]              ; sVy
        mov eax, [bx + 8]
        mov [fs:edi + 26Ch], eax                ; sARGB
        cmp cx, 3
        je .begin
        mov dword [fs:edi + 2A0h], 0            ; sDrawTriCMD
        jmp short .next
.begin: mov dword [fs:edi + 2A4h], 0            ; sBeginTriCMD
.next:  add bx, 12
        loop .vtx
        call idle
        mov ecx, 55                             ; about 3 s to look at it
        call delay

        ; the command FIFO at FIFO in the frame buffer, hole counting
        lea edx, [esi + 80000h]                 ; EDX: the command FIFO registers
        mov dword [fs:edx + 24h], 0             ; cmdBaseSize0: off
        mov dword [fs:edx + 20h], FIFO >> 12    ; cmdBaseAddr0
        mov dword [fs:edx + 2Ch], FIFO          ; cmdRdPtrL0
        mov dword [fs:edx + 34h], FIFO - 4      ; cmdAMin0
        mov dword [fs:edx + 3Ch], FIFO - 4      ; cmdAMax0
        mov dword [fs:edx + 44h], 0             ; cmdFifoDepth0
        mov dword [fs:edx + 48h], 0             ; cmdHoleCnt0
        mov dword [fs:edx + 24h], 15 | 1 << 8   ; 64 KB, on
        mov ebx, [lin1]
        add ebx, FIFO
        mov dword [fs:ebx], 1 | (1ECh >> 2) << 3 | 1 << 15 | 2 << 16   ; packet 1: colBuffer, 2 words
        mov dword [fs:ebx + 4], BACK
        mov dword [fs:ebx + 8], STRIDE
        mov dword [fs:ebx + 12], 1 | (148h >> 2) << 3 | 1 << 16       ; color1
        mov dword [fs:ebx + 16], 0000FF00h
        mov dword [fs:ebx + 20], 1 | (124h >> 2) << 3 | 1 << 16       ; fastfillCMD
        mov dword [fs:ebx + 24], 0
        call idle

        mov dword [fs:esi + 9Ch], 0                      ; overlay: the whole screen
        mov dword [fs:esi + 0A0h], (W - 1) | (H - 1) << 12
        mov dword [fs:esi + 5Ch], 1 | 1 << 18 | 1 << 8 | 1 << 21 | 1 << 10 | 1 << 11
        mov dword [fs:edi + 250h], BACK                  ; leftOverlayBuf
        mov dword [fs:edi + 128h], 1                     ; swapbufferCMD, at a retrace
        call idle

        mov ecx, 182                            ; about 10 s
        call delay

        mov ax, 3                               ; text mode: the VGA shows again
        int 10h
        mov ax, 4C00h
        int 21h

; Until the card's status says idle: no FIFO entries, not busy (as grSstIdle).
idle:   push eax
.l:     mov eax, [fs:edi]                       ; status
        test eax, 7A0h
        jnz .l
        pop eax
        ret

; ECX BIOS ticks (18.2 a second)
delay:  push eax
        push ebx
        mov ebx, 46Ch
        mov eax, [fs:ebx]
        add eax, ecx
.w:     cmp eax, [fs:ebx]
        jns .w
        pop ebx
        pop eax
        ret

; DPMI 0800h: physical EBX, 32 MB -> linear EBX
map:    mov cx, bx
        shr ebx, 16
        xor si, si
        mov si, 200h
        xor di, di
        mov ax, 0800h
        int 31h
        jc die
        shl ebx, 16
        mov bx, cx
        ret

die:    mov ax, 4C02h
        int 21h

        align 4
vtx:    dd 320.0, 40.0, 0FF0000h
        dd 560.0, 440.0, 000FF00h
        dd 80.0, 440.0, 00000FFh
bar0    dd 0
bar1    dd 0
lin0    dd 0
lin1    dd 0
entry   dd 0
devfn   dw 0
flat    dw 0
msg_found  db 'V3TEST: Voodoo 3 found, drawing', 13, 10, '$'
msg_nopci  db 'V3TEST: no PCI BIOS (vmdos voodoo3?)', 13, 10, '$'
msg_nocard db 'V3TEST: no Voodoo 3 on the PCI bus', 13, 10, '$'
msg_nodpmi db 'V3TEST: no DPMI', 13, 10, '$'
progend:
