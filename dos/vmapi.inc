; vmapi: vmdos's own INT 2Fh calls (AX=56xxh), made straight to the INT 2Fh
; stub of vmdos's BIOS (F000h: "HLT; db 2Fh", its offset in the BIOS's
; table at F000:005Eh) rather than through the INT 2Fh chain. DOS's own
; INT 2Fh handler (FreeDOS) ends the chain for calls it doesn't know: in
; virtual-8086 mode the monitor takes these off INT 2Fh before the chain
; runs, but in VMHV's virtual machine the stub at its end is the only way
; out, and they never got there. The stub works the same in both. Not
; under vmdos (no such stub): a plain INT 2Fh. Registers as for INT 2Fh.
vmapi:  push ds
        push si
        push ax
        mov ax, 0F000h
        mov ds, ax
        mov si, [2Fh * 2]               ; INT 2Fh's entry in vmdos's BIOS
        cmp si, 8000h                   ; (its stubs are all below 8000h)
        jae .chain
        cmp word [si], 2FF4h            ; HLT; db 2Fh
        jne .chain
        mov [cs:vmapi_ptr], si
        mov word [cs:vmapi_ptr + 2], 0F000h
        pop ax
        pop si
        pop ds
        pushf                           ; as INT does
        cli
        call far [cs:vmapi_ptr]
        ret
.chain: pop ax
        pop si
        pop ds
        int 2Fh
        ret
vmapi_ptr: dd 0
