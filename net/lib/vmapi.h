// vmapi: vmdos's own INT 2Fh calls (AX=56xxh), made straight to the INT 2Fh
// stub of vmdos's BIOS (F000h: "HLT; db 2Fh", its offset in the BIOS's table
// at F000:005Eh) rather than through the INT 2Fh chain, as dos/vmapi.inc
// does for the assembly tools. DOS's own INT 2Fh handler (FreeDOS) ends the
// chain for calls it doesn't know: in virtual-8086 mode the monitor takes
// these off INT 2Fh before the chain runs, but in VMHV's virtual machine the
// stub at its end is the only way out. Not under vmdos (no such stub): a
// plain INT 2Fh. Registers in and out as for int86x( 0x2f, ... ).
#ifndef VMAPI_H
#define VMAPI_H

#include <dos.h>
#include <stdint.h>

static void vmapi( union REGS *r, struct SREGS *sr ) {
  static uint16_t stub = 0xFFFF;               // 0xFFFF: not looked yet, 0: none
  if ( stub == 0xFFFF ) {
    uint16_t off = *(uint16_t __far *)MK_FP( 0xF000, 0x2F * 2 );
    stub = ( off < 0x8000 && *(uint16_t __far *)MK_FP( 0xF000, off ) == 0x2FF4 ) ? off : 0;
  }
  if ( !stub ) { int86x( 0x2f, r, r, sr ); return; }

  uint16_t a = r->x.ax, b = r->x.bx, c = r->x.cx, d = r->x.dx, s = r->x.si, i = r->x.di;
  uint16_t e = sr->es, ds_ = sr->ds, f;
  uint16_t sto = stub, sts = 0xF000;          // the far address, offset first
  _asm {
    push ds
    push bp
    mov  ax, e
    mov  es, ax
    mov  bx, b
    mov  cx, c
    mov  dx, d
    mov  si, s
    mov  di, i
    push sts                                   ; the stub's address on the stack
    push sto
    push ds_
    mov  ax, a
    pop  ds
    mov  bp, sp                                ; SS:BP -> the stub's address
    pushf                                      ; as INT does
    cli
    call dword ptr ss:[bp]
    pop  bp                                    ; (the far address)
    pop  bp
    pop  bp                                    ; the frame's own BP back
    push ds
    push ss
    pop  ds                                    ; small model: the locals' DS is SS
    pushf
    pop  f
    mov  a, ax
    mov  b, bx
    mov  c, cx
    mov  d, dx
    mov  s, si
    mov  i, di
    mov  ax, es
    mov  e, ax
    pop  ax
    mov  ds_, ax
    pop  ds
  }
  r->x.ax = a; r->x.bx = b; r->x.cx = c; r->x.dx = d; r->x.si = s; r->x.di = i;
  r->x.cflag = f & 1;
  sr->es = e; sr->ds = ds_;
}

#endif
