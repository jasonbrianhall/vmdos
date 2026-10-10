/* XMS 3.0, provided by the monitor (no HIMEM needed): the HMA, extended
   memory blocks held in kernel memory, and upper memory blocks in the
   guest's own C800h-EFFFh. INT 2Fh AX=4300h/4310h (bios.c) finds it.

   Extended memory isn't in the v86 guest's address space, so it's reached
   through function 0Bh (move), as real-mode programs do anyway. Lock (0Ch)
   reports the block's physical address. Each block is kernel RAM taken
   when it's allocated and given back when it's freed, so XMS, EMS and
   DPMI share the machine's memory. */
#include "kernel.h"

#define MAX_HANDLES 64
#define UMB_START 0xC800                    /* paragraphs */
extern u16 umb_end;                         /* ems.c: E000h with an EMS page frame */
#define UMB_END   umb_end

static u32 cap_kb, used_kb;                 /* the most there can be (xms=), allocated */
static struct { u8 *mem; u32 size_kb; u8 used, locks; } h[MAX_HANDLES + 1];  /* handle = index, 1.. */
static int hma_used, a20_global, a20_local;
static u16 umb_next = UMB_START;
static struct { u16 seg, paras; } umbs[8];

static int set_size(u16 hd, u32 kb);

/* A restart into a floppy: A20 off, the HMA free, the old DOS's blocks
   given back. */
void xms_restart(void)
{
    for (int i = 1; i <= MAX_HANDLES; i++) if (h[i].used) { set_size((u16)i, 0); h[i].used = 0; h[i].locks = 0; }
    hma_used = 0; a20_global = a20_local = 0;
    set_a20(0);
}

/* noxms on the command line: no XMS driver at all (the installation check
   says none, VMXMS.SYS doesn't stay), for software that wants its own,
   such as Windows 3.1 Setup with its HIMEM.SYS. */
int xms_hidden;

void xms_init(void)
{
    if (strstr(cmdline, "noxms")) {
        xms_hidden = 1;
        kprintf("XMS: hidden (noxms)\n");
        return;
    }
    /* MB; "xms=N" on the command line. By default 1 GB, as DOS memory
       managers give a big machine, but at most half of the RAM there is,
       so a program that takes all the XMS it can (and keeps it) leaves
       DPMI programs plenty. Nothing is set aside until it's allocated. */
    u32 want = 1024;
    char *p = strstr(cmdline, "xms=");
    if (p) { want = 0; for (p += 4; *p >= '0' && *p <= '9'; p++) want = want * 10 + (*p - '0'); }
    if (want > 3072) want = 3072;
    cap_kb = want << 10;
    u32 room = phys_spare() >> 10;
    if (!p) room /= 2;
    if (cap_kb > room) cap_kb = room & ~1023u;
    kprintf("XMS: up to %u KiB extended memory, HMA, %u KiB of UMBs\n", cap_kb, (UMB_END - UMB_START) / 64);
}

static u32 bytes_of(u32 kb) { return (kb * 1024 + 4095) & ~4095u; }

/* Free KiB: what's left of xms=, and of the RAM; the largest block the RAM can give. */
static void free_space(u32 *largest, u32 *total)
{
    u32 spare = phys_spare() >> 10, big = phys_largest() >> 10;
    *total = cap_kb - used_kb < spare ? cap_kb - used_kb : spare;
    *largest = big < *total ? big : *total;
}

/* Block hd's RAM becomes kb KiB, keeping what's in it. */
static int set_size(u16 hd, u32 kb)
{
    u32 nb = bytes_of(kb), ob = h[hd].mem ? bytes_of(h[hd].size_kb) : 0;
    if (kb > h[hd].size_kb) {
        u32 lg, tot;
        free_space(&lg, &tot);
        if (kb - h[hd].size_kb > tot) return 0;
    }
    if (nb != ob) {
        u8 *m = nb ? phys_try_alloc(nb) : 0;
        if (nb && !m) return 0;
        if (m && h[hd].mem) memcpy(m, h[hd].mem, ob < nb ? ob : nb);
        if (m) set_user((u32)(uintptr_t)m, nb, 1);          /* DPMI clients use locked blocks by address */
        if (h[hd].mem) { set_user((u32)(uintptr_t)h[hd].mem, ob, 0); phys_release(h[hd].mem, ob); }
        h[hd].mem = m;
    }
    used_kb = used_kb - h[hd].size_kb + kb;
    h[hd].size_kb = kb;
    return 1;
}

static int fail(struct regs *r, u8 code) { AX(r) = 0; BL(r) = code; return 0; }

static int valid(u16 hd) { return hd >= 1 && hd <= MAX_HANDLES && h[hd].used; }

/* Address for a move: handle 0 = real-mode seg:off; else a block offset. */
static u8 *move_ptr(u16 hd, u32 off, u32 len, int *err)
{
    if (!hd) {
        u32 lin = LIN(off >> 16, off & 0xFFFF);
        if (lin + len > GUEST_TOP) { *err = 0xA5; return 0; }
        return gptr(lin);
    }
    if (!valid(hd)) { *err = 0xA3; return 0; }
    if (off + len < off || off + len > h[hd].size_kb * 1024) { *err = 0xA7; return 0; }
    return h[hd].mem + off;
}

static void alloc_kb(struct regs *r, u32 kb, int wide)
{
    int i;
    for (i = 1; i <= MAX_HANDLES && h[i].used; i++) ;
    if (i > MAX_HANDLES) { fail(r, 0xA1); return; }
    h[i].mem = 0; h[i].size_kb = 0; h[i].locks = 0;
    if (!set_size((u16)i, kb)) { fail(r, 0xA0); return; }
    h[i].used = 1;
    AX(r) = 1;
    DX(r) = (u16)i;
    (void)wide;
}

static void realloc_kb(struct regs *r, u16 hd, u32 kb)
{
    if (!valid(hd)) { fail(r, 0xA2); return; }
    if (h[hd].locks) { fail(r, 0xAB); return; }
    if (!set_size(hd, kb)) { fail(r, 0xA0); return; }
    AX(r) = 1;
}

void xms_call(struct regs *r)
{
    u8 fn = AH(r);
    BL(r) = 0;
    dbg(2, "XMS %02x BX=%04x DX=%04x\n", fn, BX(r), DX(r));
    switch (fn) {
    case 0x00: AX(r) = 0x0300; BX(r) = 0x0001; DX(r) = 1; return;
    case 0x01:
        if (hma_used) { fail(r, 0x91); return; }
        hma_used = 1; AX(r) = 1; return;
    case 0x02:
        if (!hma_used) { fail(r, 0x93); return; }
        hma_used = 0; AX(r) = 1; return;
    case 0x03: a20_global = 1; set_a20(1); AX(r) = 1; return;
    case 0x04: a20_global = 0; if (!a20_local) set_a20(0); AX(r) = 1; return;
    case 0x05: a20_local++; set_a20(1); AX(r) = 1; return;
    case 0x06:
        if (a20_local) a20_local--;
        if (!a20_local && !a20_global) set_a20(0);
        AX(r) = 1; return;
    case 0x07: AX(r) = (u16)a20_on; return;
    case 0x08: case 0x88: {
        u32 largest, total;
        free_space(&largest, &total);
        if (fn == 0x88) { r->eax = largest; r->edx = total; r->ecx = ram_top_addr() - 1; BL(r) = 0; return; }
        AX(r) = (u16)(largest > 0xFFFF ? 0xFFFF : largest);
        DX(r) = (u16)(total > 0xFFFF ? 0xFFFF : total);
        if (!total) BL(r) = 0xA0;
        return; }
    case 0x09: alloc_kb(r, DX(r), 0); return;
    case 0x89: alloc_kb(r, r->edx, 1); return;
    case 0x0A:
        if (!valid(DX(r))) { fail(r, 0xA2); return; }
        if (h[DX(r)].locks) { fail(r, 0xAB); return; }
        set_size(DX(r), 0);
        h[DX(r)].used = 0; AX(r) = 1; return;
    case 0x0B: {
        u32 p = LIN(r->v86_ds, SI(r));
        u32 len = rd32(p);
        u16 sh = rd16(p + 4), dh = rd16(p + 10);
        u32 so = rd32(p + 6), doff = rd32(p + 12);
        int err = 0;
        u8 *src = move_ptr(sh, so, len, &err);
        if (!src) { fail(r, err == 0xA3 ? 0xA3 : err == 0xA7 ? 0xA4 : err); return; }
        u8 *dst = move_ptr(dh, doff, len, &err);
        if (!dst) { fail(r, err == 0xA3 ? 0xA5 : err == 0xA7 ? 0xA6 : err); return; }
        memmove(dst, src, len);
        AX(r) = 1;
        return; }
    case 0x0C: {
        u16 hd = DX(r);
        if (!valid(hd)) { fail(r, 0xA2); return; }
        h[hd].locks++;
        u32 a = (u32)(uintptr_t)h[hd].mem;
        DX(r) = (u16)(a >> 16); BX(r) = (u16)a; AX(r) = 1;
        return; }
    case 0x0D:
        if (!valid(DX(r))) { fail(r, 0xA2); return; }
        if (!h[DX(r)].locks) { fail(r, 0xAA); return; }
        h[DX(r)].locks--; AX(r) = 1; return;
    case 0x0E: case 0x8E: {
        u16 hd = DX(r);
        if (!valid(hd)) { fail(r, 0xA2); return; }
        int freeh = 0;
        for (int i = 1; i <= MAX_HANDLES; i++) freeh += !h[i].used;
        AX(r) = 1;
        BH(r) = h[hd].locks;
        if (fn == 0x8E) { CX(r) = (u16)freeh; r->edx = h[hd].size_kb; }
        else { BL(r) = (u8)freeh; DX(r) = (u16)(h[hd].size_kb > 0xFFFF ? 0xFFFF : h[hd].size_kb); }
        return; }
    case 0x0F: realloc_kb(r, DX(r), BX(r)); return;
    case 0x8F: realloc_kb(r, DX(r), r->ebx); return;
    case 0x10: {                                  /* request UMB: DX paragraphs */
        u16 left = UMB_END - umb_next;
        if (!left) { DX(r) = 0; fail(r, 0xB1); return; }
        if (DX(r) > left) { DX(r) = left; fail(r, 0xB0); return; }
        for (int i = 0; i < 8; i++)
            if (!umbs[i].paras) {
                umbs[i].seg = umb_next; umbs[i].paras = DX(r);
                BX(r) = umb_next;
                umb_next += DX(r);
                AX(r) = 1;
                return;
            }
        DX(r) = 0; fail(r, 0xB1);
        return; }
    case 0x11:                                    /* release UMB (only the last one is reused) */
        for (int i = 0; i < 8; i++)
            if (umbs[i].paras && umbs[i].seg == DX(r)) {
                if (umbs[i].seg + umbs[i].paras == umb_next) umb_next = umbs[i].seg;
                umbs[i].paras = 0;
                AX(r) = 1;
                return;
            }
        fail(r, 0xB2);
        return;
    }
    dbg(1, "XMS function %02x not implemented\n", fn);
    fail(r, 0x80);
}

/* Is [a, a+n) inside an XMS block? (DPMI clients hand locked blocks' addresses over) */
int xms_range_ok(u32 a, u32 n)
{
    for (int i = 1; i <= MAX_HANDLES; i++)
        if (h[i].used && h[i].mem && a >= (u32)(uintptr_t)h[i].mem && a + n <= (u32)(uintptr_t)h[i].mem + h[i].size_kb * 1024)
            return 1;
    return 0;
}
