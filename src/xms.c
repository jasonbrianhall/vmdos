/* XMS 3.0, provided by the monitor (no HIMEM needed): the HMA, extended
   memory blocks held in kernel memory, and upper memory blocks in the
   guest's own C800h-EFFFh. INT 2Fh AX=4300h/4310h (bios.c) finds it.

   Extended memory isn't in the v86 guest's address space, so it's reached
   through function 0Bh (move), as real-mode programs do anyway. Lock (0Ch)
   reports the block's physical address. */
#include "kernel.h"

#define MAX_HANDLES 64
#define UMB_START 0xC800                    /* paragraphs */
extern u16 umb_end;                         /* ems.c: E000h with an EMS page frame */
#define UMB_END   umb_end

static u8 *pool;                            /* extended memory */
static u32 pool_kb;
static struct { u32 off_kb, size_kb; u8 used, locks; } h[MAX_HANDLES + 1];   /* handle = index, 1.. */
static int hma_used, a20_global, a20_local;
static u16 umb_next = UMB_START;
static struct { u16 seg, paras; } umbs[8];

void xms_init(void)
{
    /* MB; "xms=N" on the command line. By default 1 GB, as DOS memory
       managers give a big machine, but at most half of the kernel's free
       RAM, so DPMI programs (taken as they ask) still find plenty. */
    u32 want = 1024;
    char *p = strstr(cmdline, "xms=");
    if (p) { want = 0; for (p += 4; *p >= '0' && *p <= '9'; p++) want = want * 10 + (*p - '0'); }
    u32 avail = phys_free();
    avail = avail > (16u << 20) ? avail - (16u << 20) : 0;    /* leave the kernel some room */
    if (!p) avail /= 2;
    if (want > 3072) want = 3072;
    u32 bytes = want << 20;
    if (bytes > avail) bytes = avail & ~0xFFFFFu;
    if (bytes) {
        pool = phys_alloc(bytes);
        set_user((u32)(uintptr_t)pool, bytes, 1);   /* DPMI clients use locked blocks by address */
    }
    pool_kb = bytes >> 10;
    kprintf("XMS: %u KiB extended memory, HMA, %u KiB of UMBs\n", pool_kb, (UMB_END - UMB_START) / 64);
}

/* First fit over the free gaps between allocated blocks. */
static int block_overlaps(u32 off, u32 size, int except)
{
    for (int i = 1; i <= MAX_HANDLES; i++)
        if (h[i].used && i != except && h[i].size_kb &&
            off < h[i].off_kb + h[i].size_kb && h[i].off_kb < off + size) return i;
    return 0;
}

static u32 find_gap(u32 size_kb, int except)
{
    if (!size_kb) return 0;
    u32 off = 0;
    for (;;) {
        if (off + size_kb > pool_kb) return 0xFFFFFFFF;
        int o = block_overlaps(off, size_kb, except);
        if (!o) return off;
        off = h[o].off_kb + h[o].size_kb;
    }
}

static void free_space(u32 *largest, u32 *total)
{
    *largest = *total = 0;
    u32 off = 0;
    while (off < pool_kb) {
        u32 next = pool_kb;                  /* start of the nearest block at or after off */
        int inside = 0;
        for (int i = 1; i <= MAX_HANDLES; i++) {
            if (!h[i].used || !h[i].size_kb) continue;
            if (h[i].off_kb <= off && off < h[i].off_kb + h[i].size_kb) { off = h[i].off_kb + h[i].size_kb; inside = 1; break; }
            if (h[i].off_kb > off && h[i].off_kb < next) next = h[i].off_kb;
        }
        if (inside) continue;
        u32 gap = next - off;
        *total += gap;
        if (gap > *largest) *largest = gap;
        off = next;
    }
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
    return pool + h[hd].off_kb * 1024 + off;
}

static void alloc_kb(struct regs *r, u32 kb, int wide)
{
    int i;
    for (i = 1; i <= MAX_HANDLES && h[i].used; i++) ;
    if (i > MAX_HANDLES) { fail(r, 0xA1); return; }
    u32 off = find_gap(kb, 0);
    if (off == 0xFFFFFFFF) { fail(r, 0xA0); return; }
    h[i].used = 1; h[i].off_kb = off; h[i].size_kb = kb; h[i].locks = 0;
    memset(pool + off * 1024, 0, kb * 1024);
    AX(r) = 1;
    DX(r) = (u16)i;
    (void)wide;
}

static void realloc_kb(struct regs *r, u16 hd, u32 kb)
{
    if (!valid(hd)) { fail(r, 0xA2); return; }
    if (h[hd].locks) { fail(r, 0xAB); return; }
    if (kb <= h[hd].size_kb || !block_overlaps(h[hd].off_kb, kb, hd)) {
        if (h[hd].off_kb + kb > pool_kb) { fail(r, 0xA0); return; }
        h[hd].size_kb = kb;
        AX(r) = 1;
        return;
    }
    u32 off = find_gap(kb, hd);
    if (off == 0xFFFFFFFF) { fail(r, 0xA0); return; }
    memmove(pool + off * 1024, pool + h[hd].off_kb * 1024, h[hd].size_kb * 1024);
    h[hd].off_kb = off; h[hd].size_kb = kb;
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
        if (fn == 0x88) { r->eax = largest; r->edx = total; r->ecx = 0x110000 + pool_kb * 1024 - 1; BL(r) = 0; return; }
        AX(r) = (u16)(largest > 0xFFFF ? 0xFFFF : largest);
        DX(r) = (u16)(total > 0xFFFF ? 0xFFFF : total);
        if (!total) BL(r) = 0xA0;
        return; }
    case 0x09: alloc_kb(r, DX(r), 0); return;
    case 0x89: alloc_kb(r, r->edx, 1); return;
    case 0x0A:
        if (!valid(DX(r))) { fail(r, 0xA2); return; }
        if (h[DX(r)].locks) { fail(r, 0xAB); return; }
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
        u32 a = (u32)(uintptr_t)pool + h[hd].off_kb * 1024;
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

u8 *xms_pool_range(u32 *len) { *len = pool_kb * 1024; return pool; }
