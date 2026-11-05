/* mememu.c's MMX and SSE loads and stores into trapped memory, as a Linux
   program: the instructions a Direct3D driver or a DirectDraw surface copy
   runs on the Voodoo 3's command FIFO, tiled memory or texture port. The
   "card" is an array whose accesses go through v3_mmio_read/write (logged)
   instead of faulting; the guest's MMX/XMM registers are this program's.

   make -C tests/voodoo emutest && tests/voodoo/emutest */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../../src/mememu.c"

/* ---------------- the kernel, stubbed ---------------- */
int hv_active, v3_on = 1, host_osfxsr = 1;
static u32 cr0;
u32 hv_guest_cr0(void) { return cr0; }
u32 hv_guest_cr4(void) { return 0x200; }
int hv_emu_check(u32 lin, int write) { (void)lin; (void)write; return 1; }
int hv_emu_gpa(u32 lin, u32 *gpa) { *gpa = lin; return 0; }
u8 hv_emu_rd8(u32 lin) { return rd8(lin); }
void hv_emu_wr8(u32 lin, u8 v) { wr8(lin, v); }
int page_user(u32 lin, int write) { (void)lin; (void)write; return 1; }
int vga16_window(u32 lin) { (void)lin; return 0; }
u8 vga16_read(u32 lin) { (void)lin; return 0; }
void vga16_write(u32 lin, u8 v) { (void)lin; (void)v; }

static u8 card[4096] __attribute__((aligned(64)));   /* its address range: trapped */
static u8 back[4096];                                /* what's "on the card" */
static struct { u32 a, v; int sz, wr; } acc[256];
static int nacc;
int v3_window(u32 a) { return a - (u32)(uintptr_t)card < sizeof card; }
static void log_acc(u32 a, u32 v, int sz, int wr) { if (nacc < 256) { acc[nacc].a = a - (u32)(uintptr_t)card; acc[nacc].v = v; acc[nacc].sz = sz; acc[nacc].wr = wr; nacc++; } }
u32 v3_mmio_read(u32 a, int sz) { u32 v = 0; memcpy(&v, back + (a - (u32)(uintptr_t)card), sz); log_acc(a, v, sz, 0); return v; }
void v3_mmio_write(u32 a, u32 v, int sz) { memcpy(back + (a - (u32)(uintptr_t)card), &v, sz); log_acc(a, v, sz, 1); }

/* ---------------- the guest ---------------- */
static u32 r[8], rip, fl = 2;
static struct emu_cpu cpu = { { &r[0], &r[1], &r[2], &r[3], &r[4], &r[5], &r[6], &r[7] }, &rip, &fl, { 0 }, 1 };
enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };
static u8 code[256];
static int fails;

static int run(const u8 *c, int n, int more)
{
    memset(code, 0x90, sizeof code);
    memcpy(code, c, n);
    rip = (u32)(uintptr_t)code;
    nacc = 0;
    int ok = mem_emulate(&cpu);
    if (ok && more) mem_emulate_more(&cpu);
    return ok;
}
static void check(const char *what, int ok)
{
    printf("%-50s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) fails++;
}

static u64 q(u32 lo, u32 hi) { return (u64)hi << 32 | lo; }
static void set_mm0(u64 v) { __asm__ volatile("movq %0,%%mm0" :: "m"(v)); }
static void set_mm2(u64 v) { __asm__ volatile("movq %0,%%mm2" :: "m"(v)); }
static u64 get_mm1(void) { u64 v; __asm__ volatile("movq %%mm1,%0" : "=m"(v)); return v; }
static u64 get_mm0(void) { u64 v; __asm__ volatile("movq %%mm0,%0" : "=m"(v)); return v; }
static void set_xmm0(const u32 *v) { __asm__ volatile("movups %0,%%xmm0" :: "m"(*(const u32 (*)[4])v)); }
static void get_xmm1(u32 *v) { __asm__ volatile("movups %%xmm1,%0" : "=m"(*(u32 (*)[4])v)); }

int main(void)
{
    long pg = 4096;
    mprotect((void *)((uintptr_t)x87_code & ~(pg - 1)), 2 * pg, PROT_READ | PROT_WRITE | PROT_EXEC);
    u32 C = (u32)(uintptr_t)card;

    /* MOVQ [EDI],MM0 */
    set_mm0(q(0x11223344, 0x55667788));
    r[EDI] = C + 0x40;
    check("movq [edi],mm0: two dword writes", run((u8[]){ 0x0F, 0x7F, 0x07 }, 3, 0) && nacc == 2 &&
          acc[0].wr && acc[0].a == 0x40 && acc[0].v == 0x11223344 && acc[0].sz == 4 && acc[1].a == 0x44 && acc[1].v == 0x55667788 &&
          rip == (u32)(uintptr_t)code + 3);

    /* MOVQ MM1,[ESI] */
    memcpy(back + 0x80, (u32[]){ 0xCAFEBABE, 0xDEADBEEF }, 8);
    r[ESI] = C + 0x80;
    check("movq mm1,[esi]: two dword reads", run((u8[]){ 0x0F, 0x6F, 0x0E }, 3, 0) && nacc == 2 && !acc[0].wr &&
          get_mm1() == q(0xCAFEBABE, 0xDEADBEEF));

    /* MOVNTQ [EDI+8],MM2 */
    set_mm2(q(1, 2));
    r[EDI] = C + 0x40;
    check("movntq [edi+8],mm2", run((u8[]){ 0x0F, 0xE7, 0x57, 0x08 }, 4, 0) && nacc == 2 && acc[0].a == 0x48 && acc[0].v == 1 && acc[1].v == 2);

    /* MOVD [EDI],MM0 */
    set_mm0(q(0xA5A5A5A5, 0x12345678));
    check("movd [edi],mm0: one dword", run((u8[]){ 0x0F, 0x7E, 0x07 }, 3, 0) && nacc == 1 && acc[0].v == 0xA5A5A5A5);

    /* MOVDQA [EDI],XMM0 */
    u32 x[4] = { 0x01010101, 0x02020202, 0x03030303, 0x04040404 }, y[4];
    set_xmm0(x);
    r[EDI] = C + 0x100;
    check("movdqa [edi],xmm0: four dwords", run((u8[]){ 0x66, 0x0F, 0x7F, 0x07 }, 4, 0) && nacc == 4 &&
          acc[0].a == 0x100 && acc[3].a == 0x10C && acc[3].v == 0x04040404 && !memcmp(back + 0x100, x, 16));

    /* MOVAPS XMM1,[ESI] */
    memcpy(back + 0x200, (u32[]){ 9, 8, 7, 6 }, 16);
    r[ESI] = C + 0x200;
    check("movaps xmm1,[esi]", run((u8[]){ 0x0F, 0x28, 0x0E }, 3, 0) && nacc == 4 && (get_xmm1(y), y[0] == 9 && y[3] == 6));

    /* MOVAPS misaligned: #GP in the guest, not emulated */
    r[ESI] = C + 0x204;
    check("movaps xmm1,[esi+4 misaligned]: refused", !run((u8[]){ 0x0F, 0x28, 0x0E }, 3, 0));

    /* MOVSS [EDI+4],XMM0 (printf may have used XMM0: again) */
    set_xmm0(x);
    r[EDI] = C + 0x300;
    check("movss [edi+4],xmm0: one dword", run((u8[]){ 0xF3, 0x0F, 0x11, 0x47, 0x04 }, 5, 0) && nacc == 1 && acc[0].a == 0x304 && acc[0].v == 0x01010101);

    /* MOVQ [EDI],XMM0 (66 0F D6) */
    set_xmm0(x);
    check("movq [edi],xmm0 (66 0f d6)", run((u8[]){ 0x66, 0x0F, 0xD6, 0x07 }, 4, 0) && nacc == 2 && acc[1].v == 0x02020202);

    /* MOVNTI [EDI],EAX */
    r[EAX] = 0x87654321;
    check("movnti [edi],eax", run((u8[]){ 0x0F, 0xC3, 0x07 }, 3, 0) && nacc == 1 && acc[0].v == 0x87654321);

    /* PADDW MM0,[ESI] (an MMX op reading the card) */
    set_mm0(q(0x00010001, 0x00010001));
    memcpy(back + 0x80, (u32[]){ 0x00020003, 0x00040005 }, 8);
    r[ESI] = C + 0x80;
    check("paddw mm0,[esi]", run((u8[]){ 0x0F, 0xFD, 0x06 }, 3, 0) && get_mm0() == q(0x00030004, 0x00050006));

    /* Under the guest's CR0.TS: the guest's #NM, not run here */
    cr0 = 8;
    check("movq [edi],mm0 with CR0.TS: refused", !run((u8[]){ 0x0F, 0x7F, 0x07 }, 3, 0));
    cr0 = 0;

    /* A copy loop, batched (mem_emulate_more):
         l: movq mm0,[esi]; movq [edi],mm0; add esi,8; add edi,8; dec ecx; jnz l; emms */
    static u64 src[16];
    for (int i = 0; i < 16; i++) src[i] = q(0x1000 + i, 0x2000 + i);
    memset(back + 0x400, 0, 128);
    r[ESI] = (u32)(uintptr_t)src; r[EDI] = C + 0x400; r[ECX] = 16;
    u8 loop[] = { 0x0F, 0x6F, 0x06, 0x0F, 0x7F, 0x07, 0x83, 0xC6, 0x08, 0x83, 0xC7, 0x08, 0x49, 0x75, 0xF1, 0x0F, 0x77 };
    /* the guest ran the load and faulted on the store: start there */
    __asm__ volatile("movq %0,%%mm0" :: "m"(src[0]));
    memset(code, 0xF4, sizeof code);                 /* HLT after it: where the batch stops */
    memcpy(code, loop, sizeof loop);
    rip = (u32)(uintptr_t)code + 3;
    nacc = 0;
    int ok = mem_emulate(&cpu);
    if (ok) mem_emulate_more(&cpu);
    if (getenv("V")) printf("ok %d ecx %u rip +%u nacc %d\n", ok, r[ECX], rip - (u32)(uintptr_t)code, nacc);
    check("MMX copy loop, batched: all 16 qwords", ok && !memcmp(back + 0x400, src, 128) && r[ECX] == 0 &&
          rip == (u32)(uintptr_t)code + sizeof loop && nacc == 32);

    /* MOVD MM0,EAX in a batch (register form, the GPR through the buffer) */
    r[EAX] = 0x13572468;
    r[EDI] = C + 0x40;
    check("movd mm0,eax then movd [edi],mm0", run((u8[]){ 0x0F, 0x6E, 0xC0 }, 3, 0) && run((u8[]){ 0x0F, 0x7E, 0x07 }, 3, 0) &&
          acc[0].v == 0x13572468);

    __asm__ volatile("emms");
    printf(fails ? "%d failed\n" : "all good\n", fails);
    return fails != 0;
}
