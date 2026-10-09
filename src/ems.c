/* EMS: LIM 4.0 expanded memory (INT 67h), as EMM386 provides it.
   The pages are kernel RAM (ems=MB, default 32; ems=0: none); the 64 KiB
   page frame is E000h, four 16 KiB physical pages whose guest page-table
   entries point at whichever logical pages are mapped (or back at the
   guest's own RAM when nothing is). VMEMS.SYS (dos/vmems.asm) is the
   EMMXXXX0 device programs look for and points INT 67h at the BIOS stub
   that traps here. No VCPI (DE00h says so): DOS extenders use DPMI. */
#include "kernel.h"
typedef short s16;

#define FRAME_SEG 0xE000
#define FRAME_LIN 0xE0000u
#define NPHYS 4
#define PAGE (16u * 1024)
#define MAX_HANDLES 255
#define MAX_PAGES 2048                      /* 32 MiB, LIM 4.0's limit */

static u8 *pool;
static u32 total, nfree;
static u16 owner[MAX_PAGES];                /* 0 free, else handle + 1 */
static struct { u8 used, saved; u16 n; u16 *pages; char name[8]; s16 save[NPHYS][2]; } h[MAX_HANDLES];
static s16 cur[NPHYS][2];                   /* mapped: handle, logical page; -1 none */
u16 umb_end = 0xF000;                       /* xms.c: UMBs end below the page frame */

int ems_present(void) { return pool != 0; }

void ems_init(void)
{
    u32 mb = 32;
    const char *o = strstr(cmdline, "ems=");
    if (o) { mb = 0; for (o += 4; *o >= '0' && *o <= '9'; o++) mb = mb * 10 + (u32)(*o - '0'); }
    if (!mb) { kprintf("EMS: off\n"); return; }
    total = mb * 64;
    if (total > MAX_PAGES) total = MAX_PAGES;
    pool = phys_try_alloc(total * PAGE);
    if (!pool) { kprintf("EMS: no memory for %u MiB\n", mb); total = 0; return; }
    nfree = total;
    for (int i = 0; i < NPHYS; i++) cur[i][0] = cur[i][1] = -1;
    h[0].used = 1;                          /* handle 0: the OS handle, no pages */
    umb_end = FRAME_SEG;
    kprintf("EMS: %u KiB, page frame %04x\n", total * 16, FRAME_SEG);
}

static u8 *page_ptr(int hd, int lp) { return pool + (u32)h[hd].pages[lp] * PAGE; }

static void map_phys(int p, int hd, int lp)
{
    u32 lin = FRAME_LIN + (u32)p * PAGE;
    for (u32 k = 0; k < PAGE; k += 4096) {
        u32 phys = hd >= 0 ? (u32)(uintptr_t)page_ptr(hd, lp) + k : guest_phys(lin + k);
        map_page(lin + k, phys, 7);
    }
    cur[p][0] = (s16)hd; cur[p][1] = (s16)lp;
}
static void flush(void) { tlb_flush(); }

/* A restart into a floppy: the page frame is plain memory again. */
void ems_restart(void)
{
    if (!total) return;
    for (int p = 0; p < NPHYS; p++) if (cur[p][0] >= 0) map_phys(p, -1, 0);
    flush();
}

static int alloc_pages(int hd, u32 n)
{
    u16 *pg = n ? phys_try_alloc((n * 2 + 4095) & ~4095u) : 0;
    if (n && !pg) return 0x80;
    for (u32 i = 0, p = 0; i < n; i++) {
        while (owner[p]) p++;
        owner[p] = (u16)(hd + 1);
        pg[i] = (u16)p;
    }
    nfree -= n;
    h[hd].pages = pg; h[hd].n = (u16)n;
    return 0;
}

static void free_pages(int hd)
{
    for (u32 i = 0; i < h[hd].n; i++) owner[h[hd].pages[i]] = 0;
    nfree += h[hd].n;
    h[hd].n = 0;                            /* the page list's memory is not reused (kernel heap) */
    for (int p = 0; p < NPHYS; p++) if (cur[p][0] == hd) map_phys(p, -1, 0);
    flush();
}

static int new_handle(void)
{
    for (int i = 1; i < MAX_HANDLES; i++) if (!h[i].used) { memset(&h[i], 0, sizeof h[i]); h[i].used = 1; return i; }
    return -1;
}
static int valid(int hd) { return hd >= 0 && hd < MAX_HANDLES && h[hd].used; }

/* Map logical page lp of handle hd (0xFFFF: unmap) at physical page p. */
static int map(int hd, int p, u16 lp)
{
    if (!valid(hd)) return 0x83;
    if (p < 0 || p >= NPHYS) return 0x8B;
    if (lp == 0xFFFF) { map_phys(p, -1, 0); return 0; }
    if (lp >= h[hd].n) return 0x8A;
    map_phys(p, hd, lp);
    return 0;
}

static int seg_to_phys(u16 seg) { return seg >= FRAME_SEG && seg < FRAME_SEG + 0x1000 && !(seg & 0x3FF) ? (seg - FRAME_SEG) >> 10 : -1; }

/* Function 57h: a memory region, conventional or expanded, byte i of it. */
struct region { u8 type; u16 hd, off, seg; };
static u8 *reg_ptr(struct region *g, u32 i, u32 *left)
{
    if (g->type == 0) {                     /* conventional: through the guest's address space */
        u32 a = ((u32)g->seg << 4) + g->off + i;
        *left = 4096 - (a & 4095);
        return gptr(a);
    }
    u32 o = g->off + i, lp = g->seg + o / PAGE;
    if (lp >= h[g->hd].n) return 0;
    *left = PAGE - o % PAGE;
    return page_ptr(g->hd, (int)lp) + o % PAGE;
}

static int move_region(struct regs *r, int xchg)
{
    u32 a = LIN(r->v86_ds, SI(r));
    u32 len = rd32(a);
    struct region s = { rd8(a + 4), rd16(a + 5), rd16(a + 7), rd16(a + 9) };
    struct region d = { rd8(a + 11), rd16(a + 12), rd16(a + 14), rd16(a + 16) };
    if (len > 0x100000) return 0x96;
    for (int k = 0; k < 2; k++) {
        struct region *g = k ? &d : &s;
        if (g->type > 1) return 0x98;
        if (g->type == 1) {
            if (!valid(g->hd)) return 0x83;
            if (g->off >= PAGE) return 0x95;
            if (g->seg >= h[g->hd].n || (u32)g->seg * PAGE + g->off + len > (u32)h[g->hd].n * PAGE) return 0x93;
        } else if (((u32)g->seg << 4) + g->off + len > 0x100000) return 0xA2;
    }
    for (u32 i = 0; i < len;) {
        u32 l1, l2;
        u8 *ps = reg_ptr(&s, i, &l1), *pd = reg_ptr(&d, i, &l2);
        if (!ps || !pd) return 0x93;
        u32 n = len - i;
        if (n > l1) n = l1;
        if (n > l2) n = l2;
        if (xchg) for (u32 k = 0; k < n; k++) { u8 t = ps[k]; ps[k] = pd[k]; pd[k] = t; }
        else memmove(pd, ps, n);
        i += n;
    }
    return 0;
}

static void write_map(u32 a) { for (int p = 0; p < NPHYS; p++) { wr16(a + p * 4, (u16)cur[p][0]); wr16(a + p * 4 + 2, (u16)cur[p][1]); } }
static void read_map(u32 a)
{
    for (int p = 0; p < NPHYS; p++) {
        s16 hd = (s16)rd16(a + p * 4), lp = (s16)rd16(a + p * 4 + 2);
        if (hd >= 0 && valid(hd) && lp >= 0 && lp < h[hd].n) map_phys(p, hd, lp);
        else map_phys(p, -1, 0);
    }
    flush();
}

void ems_int67(struct regs *r)
{
    int st = 0;
    u8 fn = AH(r), sub = AL(r);
    if (!pool) { AH(r) = 0x84; return; }
    dbg(2, "EMS %02x%02x BX=%04x CX=%04x DX=%04x\n", fn, sub, BX(r), CX(r), DX(r));
    switch (fn) {
    case 0x40: break;                                         /* status */
    case 0x41: BX(r) = FRAME_SEG; break;                      /* page frame */
    case 0x42: BX(r) = (u16)nfree; DX(r) = (u16)total; break; /* unallocated / total pages */
    case 0x43: case 0x5A: {                                   /* allocate (5Ah: standard/raw, 0 pages allowed) */
        u32 n = BX(r);
        if (fn == 0x5A && sub > 1) { st = 0x8F; break; }
        if (!n && fn == 0x43) { st = 0x89; break; }
        if (n > total) { st = 0x87; break; }
        if (n > nfree) { st = 0x88; break; }
        int hd = new_handle();
        if (hd < 0) { st = 0x85; break; }
        st = alloc_pages(hd, n);
        if (st) { h[hd].used = 0; break; }
        DX(r) = (u16)hd;
        break; }
    case 0x44: st = map(DX(r), sub, BX(r)); flush(); break;   /* map */
    case 0x45: {                                              /* deallocate */
        int hd = DX(r);
        if (!valid(hd)) { st = 0x83; break; }
        if (h[hd].saved) { st = 0x86; break; }
        free_pages(hd);
        if (hd) h[hd].used = 0;
        break; }
    case 0x46: AL(r) = 0x40; break;                           /* version 4.0 */
    case 0x47: {                                              /* save page map */
        int hd = DX(r);
        if (!valid(hd)) { st = 0x83; break; }
        if (h[hd].saved) { st = 0x8D; break; }
        memcpy(h[hd].save, cur, sizeof cur); h[hd].saved = 1;
        break; }
    case 0x48: {                                              /* restore page map */
        int hd = DX(r);
        if (!valid(hd)) { st = 0x83; break; }
        if (!h[hd].saved) { st = 0x8E; break; }
        for (int p = 0; p < NPHYS; p++) {
            int sh = h[hd].save[p][0], sl = h[hd].save[p][1];
            if (sh >= 0 && valid(sh) && sl < h[sh].n) map_phys(p, sh, sl); else map_phys(p, -1, 0);
        }
        flush();
        h[hd].saved = 0;
        break; }
    case 0x4B: { int n = 0; for (int i = 0; i < MAX_HANDLES; i++) n += h[i].used; BX(r) = (u16)n; break; }
    case 0x4C: if (!valid(DX(r))) st = 0x83; else BX(r) = h[DX(r)].n; break;
    case 0x4D: {                                              /* all handles' pages into ES:DI */
        u32 a = LIN(r->v86_es, DI(r));
        int n = 0;
        for (int i = 0; i < MAX_HANDLES; i++) if (h[i].used) { wr16(a + n * 4, (u16)i); wr16(a + n * 4 + 2, h[i].n); n++; }
        BX(r) = (u16)n;
        break; }
    case 0x4E:                                                /* page map: get / set / both / size */
        if (sub == 0 || sub == 2) write_map(LIN(r->v86_es, DI(r)));
        if (sub == 1 || sub == 2) read_map(LIN(r->v86_ds, SI(r)));
        if (sub == 3) AL(r) = NPHYS * 4;
        if (sub > 3) st = 0x8F;
        break;
    case 0x4F: {                                              /* partial page map */
        if (sub == 2) { AL(r) = (u8)(2 + BX(r) * 6); break; }
        if (sub == 0) {                                       /* DS:SI = count + segments -> ES:DI */
            u32 l = LIN(r->v86_ds, SI(r)), o = LIN(r->v86_es, DI(r));
            u16 n = rd16(l);
            wr16(o, n);
            for (u16 i = 0; i < n; i++) {
                u16 seg = rd16(l + 2 + i * 2);
                int p = seg_to_phys(seg);
                if (p < 0) { st = 0x8B; break; }
                wr16(o + 2 + i * 6, seg); wr16(o + 4 + i * 6, (u16)cur[p][0]); wr16(o + 6 + i * 6, (u16)cur[p][1]);
            }
        } else if (sub == 1) {
            u32 l = LIN(r->v86_ds, SI(r));
            u16 n = rd16(l);
            for (u16 i = 0; i < n; i++) {
                int p = seg_to_phys(rd16(l + 2 + i * 6));
                s16 hd = (s16)rd16(l + 4 + i * 6), lp = (s16)rd16(l + 6 + i * 6);
                if (p < 0) { st = 0x8B; break; }
                if (hd >= 0 && valid(hd) && lp >= 0 && lp < h[hd].n) map_phys(p, hd, lp); else map_phys(p, -1, 0);
            }
            flush();
        } else st = 0x8F;
        break; }
    case 0x50: {                                              /* map multiple: DS:SI pairs (logical, physical) */
        if (sub > 1) { st = 0x8F; break; }
        u32 l = LIN(r->v86_ds, SI(r));
        for (u16 i = 0; i < CX(r) && !st; i++) {
            u16 lp = rd16(l + i * 4), ph = rd16(l + i * 4 + 2);
            int p = sub ? seg_to_phys(ph) : ph;
            st = map(DX(r), p, lp);
        }
        flush();
        break; }
    case 0x51: {                                              /* reallocate */
        int hd = DX(r);
        u32 n = BX(r), old = valid(hd) ? h[hd].n : 0;
        if (!valid(hd)) { st = 0x83; break; }
        if (n > total) { st = 0x87; break; }
        if (n > old && n - old > nfree) { st = 0x88; break; }
        u16 *pg = n ? phys_try_alloc((n * 2 + 4095) & ~4095u) : 0;
        if (n && !pg) { st = 0x80; break; }
        for (u32 i = 0; i < n && i < old; i++) pg[i] = h[hd].pages[i];
        for (u32 i = n; i < old; i++) owner[h[hd].pages[i]] = 0;
        for (u32 i = old, p = 0; i < n; i++) { while (owner[p]) p++; owner[p] = (u16)(hd + 1); pg[i] = (u16)p; }
        nfree = nfree + old - n;
        h[hd].pages = pg; h[hd].n = (u16)n;
        for (int p = 0; p < NPHYS; p++) if (cur[p][0] == hd && cur[p][1] >= (s16)n) map_phys(p, -1, 0);
        flush();
        BX(r) = (u16)n;
        break; }
    case 0x52:                                                /* attributes: volatile only */
        if (sub == 0) AL(r) = 0;
        else if (sub == 1) { if (BL(r)) st = 0x91; }
        else if (sub == 2) AL(r) = 0;
        else st = 0x8F;
        break;
    case 0x53:                                                /* handle name */
        if (!valid(DX(r))) { st = 0x83; break; }
        if (sub == 0) { u32 a = LIN(r->v86_es, DI(r)); for (int i = 0; i < 8; i++) wr8(a + i, (u8)h[DX(r)].name[i]); }
        else if (sub == 1) {
            u32 a = LIN(r->v86_ds, SI(r));
            char nm[8]; int blank = 1;
            for (int i = 0; i < 8; i++) { nm[i] = (char)rd8(a + i); if (nm[i]) blank = 0; }
            for (int i = 1; i < MAX_HANDLES && !blank; i++)
                if (h[i].used && i != DX(r) && !memcmp(h[i].name, nm, 8)) { st = 0xA1; break; }
            if (!st) memcpy(h[DX(r)].name, nm, 8);
        } else st = 0x8F;
        break;
    case 0x54:                                                /* handle directory */
        if (sub == 0) {
            u32 a = LIN(r->v86_es, DI(r));
            int n = 0;
            for (int i = 0; i < MAX_HANDLES; i++) if (h[i].used) {
                wr16(a + n * 10, (u16)i);
                for (int k = 0; k < 8; k++) wr8(a + n * 10 + 2 + k, (u8)h[i].name[k]);
                n++;
            }
            AL(r) = (u8)n;
        } else if (sub == 1) {
            u32 a = LIN(r->v86_ds, SI(r));
            char nm[8];
            for (int i = 0; i < 8; i++) nm[i] = (char)rd8(a + i);
            st = 0xA0;
            for (int i = 0; i < MAX_HANDLES; i++) if (h[i].used && !memcmp(h[i].name, nm, 8)) { DX(r) = (u16)i; st = 0; break; }
        } else if (sub == 2) BX(r) = MAX_HANDLES;
        else st = 0x8F;
        break;
    case 0x57:                                                /* move / exchange memory region */
        if (sub > 1) { st = 0x8F; break; }
        st = move_region(r, sub);
        break;
    case 0x58:                                                /* mappable physical pages */
        if (sub == 0) {
            u32 a = LIN(r->v86_es, DI(r));
            for (int p = 0; p < NPHYS; p++) { wr16(a + p * 4, (u16)(FRAME_SEG + p * 0x400)); wr16(a + p * 4 + 2, (u16)p); }
            CX(r) = NPHYS;
        } else if (sub == 1) CX(r) = NPHYS;
        else st = 0x8F;
        break;
    case 0x59:                                                /* hardware information */
        if (sub == 0) {
            u32 a = LIN(r->v86_es, DI(r));
            wr16(a, 0x400); wr16(a + 2, 0); wr16(a + 4, NPHYS * 4); wr16(a + 6, 0); wr16(a + 8, 0);
        } else if (sub == 1) { BX(r) = (u16)nfree; DX(r) = (u16)total; }
        else st = 0x8F;
        break;
    case 0x5B:                                                /* alternate map register sets: none */
        if (sub == 0) { BL(r) = 0; r->v86_es = 0; DI(r) = 0; }
        else if (sub == 1) { if (BL(r)) st = 0x9C; }
        else if (sub == 2) DX(r) = NPHYS * 4;
        else if (sub == 3) BL(r) = 0;
        else if (sub == 4) { if (BL(r)) st = 0x9C; }
        else st = 0x8F;
        break;
    case 0x5C: break;                                         /* prepare for warm boot */
    case 0xDE: st = 0x84; break;                              /* VCPI: not here */
    default: st = 0x84;
    }
    AH(r) = (u8)st;
}

/* For VMEMS.SYS (INT 2Fh AX=5645h): AX=0, BX=pages, DX=frame when present. */
void ems_query(struct regs *r)
{
    if (!pool) return;
    AX(r) = 0; BX(r) = (u16)total; DX(r) = FRAME_SEG;
}
