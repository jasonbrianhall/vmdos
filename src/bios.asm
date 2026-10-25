; The guest's BIOS segment (F000:0000, 64 KiB). The services themselves run in
; the monitor: "HLT; db id" traps into it (HLT is privileged in v86 mode).
; Assemble: nasm -f bin bios.asm

        bits 16
        org 0

%macro TRAP 1
        hlt
        db %1
%endmacro

; ---- 0000: default IVT offsets for vectors 0..255 ----
vectors:
%assign v 0
%rep 256
  %if v == 0x06
        dw int06
  %elif v == 0x08
        dw int08
  %elif v == 0x09
        dw int09
  %elif v >= 0x0A && v <= 0x0F
        dw irq_master
  %elif v == 0x74
        dw int74
  %elif v >= 0x70 && v <= 0x77
        dw irq_slave
  %elif (v >= 0x10 && v <= 0x17) || v == 0x1A
        dw svc_ %+ v
  %elif v == 0x2F
        dw int2f
  %elif v == 0x33
        dw int33
  %elif v == 0x18
        dw int18
  %elif v == 0x19
        dw int19
  %else
        dw dummy
  %endif
  %assign v v+1
%endrep

; ---- 0200: pointers for the monitor ----
        dw config_table
        dw diskette_table
        dw stubs_end
        dw xms_entry                     ; 0206
        dw mouse_cb                      ; 0208
mouse_handler: dd 0                      ; 020A: the program's INT 33h/0Ch handler
        dw dpmi_entry                    ; 020E  DPMI: real-to-protected switch entry
        dw dpmi_rmret                    ; 0210  return from a real-mode excursion
        dw dpmi_raw_rm2pm                ; 0212  raw switch, real -> protected
        dw dpmi_cb0                      ; 0214  real-mode callbacks, 4 bytes apart
        dw pm_hwret                      ; 0216  protected-mode stubs (HOST_CS = F000h base)
        dw pm_excret                     ; 0218
        dw pm_cbret                      ; 021A
        dw pm_raw_pm2rm                  ; 021C
        dw pm_retf                       ; 021E  state save/restore: nothing to do
        dw rm_retf                       ; 0220
        dw pm_defint                     ; 0222  default PM interrupt handlers, 4 bytes apart
        dw dpmi_intstub                  ; 0224  "INT n" for reflected interrupts (n patched)
        dw vbe_bank                      ; 0226  VESA window function (far call)
        dw vbe_pmi                       ; 0228  VESA protected-mode interface table (4F0Ah)
        dw vbe_pmi_end - vbe_pmi         ; 022A  its length
        dw ps2_handler                   ; 022C  INT 15h C207h handler (far pointer)
        dw int67                         ; 022E  EMS (VMEMS.SYS points INT 67h here)
        dw vga_regtab_n                  ; 0230  INT 10h: register writes to replay (count, then port/value triples)
        dw lfn_open                      ; 0232  long-name open on a 9p drive (fs9p.cpp)
        dw bios32_dir                    ; 0234  BIOS32 service directory (vpci.c turns it on)
        dw rom_post                      ; 0236  voodoo3: the card's BIOS initialised, then the boot sector
        dw vga_post                      ; 0238  video=vbe: text mode by the card's BIOS, then the boot sector
        dw dpmi_mousecb                  ; 023A  INT 33h 0Ch/14h from PM: the driver calls this, it calls the PM handler

; ---- stubs ----
dummy:  iret

; voodoo3: as a PC's BIOS before booting, the video card's own BIOS is run
; (its init entry, C000:0003, AX = its bus and device/function: 00h, 08h);
; it takes INT 10h. Then the boot sector, with the registers it was given.
rom_post:
        pushad
        push ds
        push es
        mov ax, 0x0008
        call 0xC000:0x0003
        pop es
        pop ds
        popad
        jmp 0x0000:0x7C00

; video=vbe: the PC's card has its own BIOS (initialised by the PC's); a
; clean text mode from it, as a BIOS leaves the screen at boot, then the
; boot sector.
vga_post:
        pushad
        push ds
        push es
        mov ax, 0x0003
        int 0x10
        pop es
        pop ds
        popad
        jmp 0x0000:0x7C00

; INT 10h: the monitor does the work; after a mode set in VMHV it returns
; here (instead of IRETing for us) with a list of the register writes the
; mode set made, and they are done again as real OUTs from the guest, as a
; VGA BIOS does them: Windows' display VxD traps the VGA ports while the BIOS
; runs and keeps its copy of the registers from what it sees.
svc_16: TRAP 0x10
        push ax
        push cx
        push dx
        push si
        mov si, vga_regtab
        mov cx, [cs:vga_regtab_n]
        jcxz .done
.next:  mov dx, [cs:si]
        mov al, [cs:si + 2]
        add si, 3
        test dh, 80h                    ; bit 15: a read (3DAh: the attribute flip-flop)
        jnz .in
        out dx, al
        loop .next
        jmp short .done
.in:    and dh, 7Fh
        in al, dx
        loop .next
.done:  pop si
        pop dx
        pop cx
        pop ax
        iret

%assign v 0x11
%rep 4
svc_ %+ v:
        TRAP v
        iret
  %assign v v+1
%endrep

; INT 15h: the monitor does the work. After C200h (PS/2 mouse on) it
; returns here instead, and IRQ 12 (and the cascade, IRQ 2) is unmasked with
; real port I/O, as a BIOS does it: Windows' VPICD traps the PIC's mask
; registers and only lets IRQ 12 through to a VM that unmasked it.
; Success: CF clear in the flags the IRET returns.
svc_21: TRAP 0x15
        push ax
        in al, 0A1h
        and al, 0EFh
        out 0A1h, al
        in al, 21h
        and al, 0FBh
        out 21h, al
        pop ax
        push bp
        mov bp, sp
        and byte [bp + 6], 0FEh         ; the caller's CF: success
        pop bp
        iret

%assign v 0x16
%rep 2
svc_ %+ v:
        TRAP v
        iret
  %assign v v+1
%endrep
svc_26: TRAP 0x1A
        iret

int2f:  TRAP 0x2F                ; multiplex: XMS installation check / entry point
        iret

xms_entry:                       ; XMS driver entry (far call); starts with a short
        jmp short .go            ; jump so other drivers can hook it, as HIMEM's does
        nop
        nop
        nop
.go:    TRAP 0x43
        retf

; VBE 2.0 protected-mode interface (4F0Ah): offsets of 32-bit routines a
; program copies and calls near. Each traps to the monitor (HLT is
; privileged; the monitor knows the "HLT id RET" shape wherever it runs).
vbe_pmi:
        dw .win - vbe_pmi                ; set window: BL = 0, DX = bank
        dw .start - vbe_pmi              ; set display start: CX:DX = address / 4
        dw .pal - vbe_pmi                ; set palette: CX, DX, ES:EDI
        dw 0                             ; no ports / memory to grant
.win:   TRAP 0x47
        ret
.start: TRAP 0x48
        ret
.pal:   TRAP 0x49
        ret
vbe_pmi_end:

vbe_bank:                        ; VESA WinFuncPtr: BH=0 set / 1 get, DX = bank
        TRAP 0x46
        retf

; INT 21h AX=716Ch on a 9p drive (fs9p.cpp): the monitor turns it into DOS's
; own 6C00h with the short name, made from here; then the caller's DS:SI go
; back and DOS's CF and ZF are the caller's.
lfn_open:
        int 21h
        TRAP 0x5A
        iret

int33:  TRAP 0x33                ; mouse driver (in the monitor)
        iret

int67:  TRAP 0x67                ; EMS (in the monitor, ems.c)
        iret

mouse_cb:                        ; entered like an interrupt when a mouse event is due
        pusha
        push ds
        push es
        TRAP 0x34                ; event registers
        call far [cs:mouse_handler]
        TRAP 0x35                ; handler finished
        pop es
        pop ds
        popa
        iret

; ---- DPMI host stubs (the host itself is in the monitor, dpmi.c) ----
dpmi_entry:                      ; far-called by the client to enter protected mode
        TRAP 0x54                ; save the caller's state
        mov ah, 0x62             ; current PSP -> BX
        int 0x21
        TRAP 0x55                ; build the client, continue in protected mode
dpmi_rmret:
        TRAP 0x50
dpmi_intstub:                    ; a real INT instruction, so the monitor's shortcuts apply
        int 0x00                 ; the byte after CDh is patched before each use
        TRAP 0x50
dpmi_raw_rm2pm:
        TRAP 0x56
rm_retf:
        retf
align 4
dpmi_cb0:
%assign i 0
%rep 16
        TRAP 0x60 + i
        nop
        nop
  %assign i i+1
%endrep
dpmi_mousecb:                    ; far-called by the mouse driver with the event registers
        TRAP 0x5B                ; the client's protected-mode handler, then on here
        retf
pm_hwret:
        TRAP 0x51
pm_excret:
        TRAP 0x52
pm_cbret:
        TRAP 0x53
pm_raw_pm2rm:
        TRAP 0x57
pm_retf:
        retf                     ; the host code segment matches the client's bitness
align 4
pm_defint:
%assign i 0
%rep 256
        TRAP 0x58
        db i
        nop
  %assign i i+1
%endrep

int06:  TRAP 0x06               ; invalid opcode nobody handles: stop with a report
        iret

int08:  TRAP 0x08               ; tick count, then the user hook, then EOI
        int 0x1C
        push ax
        mov al, 0x20
        out 0x20, al
        pop ax
        iret

; Keyboard: the scan code read and the EOI sent with real port I/O, as a
; BIOS does: in a DOS window under Windows the keyboard VxD hands the VM its
; keys through a virtual port 60h, and VPICD sends the next IRQ 1 only after
; the VM's own EOI.
int09:  push ax
        in al, 0x60
        TRAP 0x09               ; AL: the scan code; the monitor fills the buffer
        mov al, 0x20
        out 0x20, al
        pop ax
        iret

int18:  TRAP 0x18
        iret
int19:  TRAP 0x19
        iret

int74:                           ; IRQ 12: PS/2 mouse byte -> INT 15h C207h handler
        pusha
        push ds
        push es
        in al, 0x64              ; the byte from the 8042 with real port reads, as a BIOS
        mov ah, al               ; does it: under Windows the keyboard VxD has taken it
        in al, 0x60              ; already and hands it to the VM through port 60h
        TRAP 0x74                ; AH status (bit 0: AL is a byte); DX=1: packet done, AX status, BX X, CX Y
        test dx, dx
        jz .eoi
        push ax
        push bx
        push cx
        xor dx, dx
        push dx
        call far [cs:ps2_handler]
        add sp, 8
.eoi:   mov al, 0x20
        out 0xA0, al
        out 0x20, al
        pop es
        pop ds
        popa
        iret
ps2_handler: dd 0

irq_master:
        push ax
        mov al, 0x20
        out 0x20, al
        pop ax
        iret
irq_slave:
        push ax
        mov al, 0x20
        out 0xA0, al
        out 0x20, al
        pop ax
        iret

; ---- the PCI BIOS for 32-bit callers (Windows' PCI.VXD) ----
; A BIOS32 service directory with the "$PCI" service. Its signature is
; "_3?_" here: vpci.c makes it "_32_" (and the checksum right) only when
; there is a PCI bus to show (voodoo3), so without one nothing finds it.
; Both entry points are 32-bit, called far; they don't touch memory, so
; they work whatever base the caller's code selector has. Configuration
; space through ports CF8h/CFCh (the monitor's virtual bus, vpci.c).
        align 16
bios32_dir:
        db '_3?_'
        dd 0F0000h + bios32_entry        ; physical
        db 0                             ; revision
        db 1                             ; length, paragraphs
        db 0                             ; checksum (vpci.c)
        times 5 db 0

        bits 32
bios32_entry:                            ; EAX = service ID, EBX = 0
        cmp eax, '$PCI'
        jne .none
        mov ebx, 0F0000h                 ; the service: base, length, entry
        mov ecx, 10000h
        mov edx, pci32
        xor al, al
        retf
.none:  mov al, 80h
        retf

; AX = B1xxh, as INT 1Ah. CF and AH: the result.
pci32:  cmp ah, 0B1h
        jne .bad
        cmp al, 01h
        je .present
        cmp al, 02h
        je .finddev
        cmp al, 03h
        je .findclass
        cmp al, 08h
        jb .bad
        cmp al, 0Dh
        ja .bad
        push eax
        push edx
        call .addr                       ; EAX: CF8h value, DX: data port
        mov dx, 0CF8h
        out dx, eax
        pop edx
        pop eax
        push edx
        mov dx, di
        and dx, 3
        add dx, 0CFCh
        cmp al, 08h
        je .rb
        cmp al, 09h
        je .rw
        cmp al, 0Ah
        je .rd
        cmp al, 0Bh
        je .wb
        cmp al, 0Ch
        je .ww
        mov eax, ecx                     ; 0Dh: write dword
        out dx, eax
        jmp .okpop
.wb:    mov al, cl
        out dx, al
        jmp .okpop
.ww:    mov ax, cx
        out dx, ax
        jmp .okpop
.rb:    in al, dx
        mov cl, al
        jmp .okpop
.rw:    in ax, dx
        mov cx, ax
        jmp .okpop
.rd:    in eax, dx
        mov ecx, eax
.okpop: pop edx
        jmp .ok

.addr:  movzx eax, bx                    ; BH bus, BL device/function
        shl eax, 8
        or eax, 80000000h
        push edi
        and edi, 0FCh
        or eax, edi
        pop edi
        ret

.present:
        mov edx, 20494350h               ; "PCI "
        mov al, 01h                      ; configuration mechanism 1
        mov bx, 0210h                    ; version 2.10
        xor cl, cl                       ; last bus: 0
        xor ah, ah
        clc
        retf

.finddev:                                ; device CX of vendor DX, the SI-th
        push eax
        push edx
        push esi
        push ebp
        mov ebp, ecx
        shl ebp, 16
        mov bp, dx                       ; the ID dword wanted
        xor ebx, ebx                     ; BH = 0, BL = devfn
.fdloop:
        mov eax, ebx
        shl eax, 8
        or eax, 80000000h
        mov dx, 0CF8h
        out dx, eax
        mov dx, 0CFCh
        in eax, dx
        cmp eax, ebp
        jne .fdnext
        test si, si
        jz .found
        dec si
.fdnext:
        inc bl
        jnz .fdloop
        pop ebp
        pop esi
        pop edx
        pop eax
        jmp .notfound
.found: pop ebp
        pop esi
        pop edx
        pop eax
        jmp .ok

.findclass:                              ; class code ECX (24 bits), the SI-th
        push eax
        push edx
        push esi
        xor ebx, ebx
.fcloop:
        mov eax, ebx
        shl eax, 8
        or eax, 80000008h
        mov dx, 0CF8h
        out dx, eax
        mov dx, 0CFCh
        in eax, dx
        shr eax, 8
        push ecx
        and ecx, 0FFFFFFh
        cmp eax, ecx
        pop ecx
        jne .fcnext
        test si, si
        jz .fcfound
        dec si
.fcnext:
        inc bl
        jnz .fcloop
        pop esi
        pop edx
        pop eax
        jmp .notfound
.fcfound:
        pop esi
        pop edx
        pop eax
        jmp .ok

.ok:    xor ah, ah
        clc
        retf
.notfound:
        mov ah, 86h
        stc
        retf
.bad:   mov ah, 81h
        stc
        retf
        bits 16

config_table:                    ; INT 15h AH=C0h
        dw 8
        db 0xFC, 0x01, 0x00      ; model AT, submodel, revision
        db 0x70                  ; 2nd 8259, RTC, INT 15h/4Fh called by INT 9
        db 0x40                  ; INT 16h/09h extended keyboard functions
        db 0, 0, 0

diskette_table:                  ; INT 1Eh (1.44 MB values; there are no floppies)
        db 0xAF, 0x02, 0x25, 0x02, 0x12, 0x1B, 0xFF, 0x6C, 0xF6, 0x0F, 0x08

vga_regtab_n: dw 0
vga_regtab:   times 3 * 160 db 0
stubs_end:

        times 0xFFF0 - ($ - $$) db 0
reset:  jmp 0xF000:int19         ; FFF0
        db "10/07/26"            ; FFF5
        db 0                     ; FFFD
        db 0xFC                  ; FFFE model: AT
        db 0
