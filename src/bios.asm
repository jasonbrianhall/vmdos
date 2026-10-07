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
