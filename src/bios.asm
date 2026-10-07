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

; ---- stubs ----
dummy:  iret

%assign v 0x10
%rep 8
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

int33:  TRAP 0x33                ; mouse driver (in the monitor)
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

int09:  TRAP 0x09               ; keyboard: the monitor reads port 60h, fills the buffer, EOIs
        iret

int18:  TRAP 0x18
        iret
int19:  TRAP 0x19
        iret

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

config_table:                    ; INT 15h AH=C0h
        dw 8
        db 0xFC, 0x01, 0x00      ; model AT, submodel, revision
        db 0x70                  ; 2nd 8259, RTC, INT 15h/4Fh called by INT 9
        db 0x40                  ; INT 16h/09h extended keyboard functions
        db 0, 0, 0

diskette_table:                  ; INT 1Eh (1.44 MB values; there are no floppies)
        db 0xAF, 0x02, 0x25, 0x02, 0x12, 0x1B, 0xFF, 0x6C, 0xF6, 0x0F, 0x08

stubs_end:

        times 0xFFF0 - ($ - $$) db 0
reset:  jmp 0xF000:int19         ; FFF0
        db "10/07/26"            ; FFF5
        db 0                     ; FFFD
        db 0xFC                  ; FFFE model: AT
        db 0
