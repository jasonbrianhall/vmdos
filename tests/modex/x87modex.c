/* X87MODEX.EXE: unchained 256-color ("mode X") written and read 8 bytes at
   a time, the way games clear and copy their screens: FST QWORD (Need for
   Speed SE), FISTP QWORD, MMX MOVQ, and FLD/FILD QWORD reads, with all four
   planes enabled so vmdos traps and emulates each one (mememu.c). Then
   every byte is read back plane by plane with plain MOVs and checked.
   Exits 0 if all of it matched, 1 if not (and prints where, in text mode).
   Built with DJGPP: tests/modex/modextest.sh. */
#include <stdio.h>
#include <string.h>
#include <dpmi.h>
#include <pc.h>
#include <sys/nearptr.h>

#define ROW 80                                  /* bytes per row per plane */
static volatile unsigned char *vga;

static void setmode(int m) { __dpmi_regs r; memset(&r, 0, sizeof r); r.x.ax = m; __dpmi_int(0x10, &r); }
static void mapmask(int m) { outportb(0x3C4, 2); outportb(0x3C5, m); }
static void readmap(int p) { outportb(0x3CE, 4); outportb(0x3CF, p); }

static int has_mmx(void)
{
    unsigned a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (d >> 23) & 1;
}

int main(void)
{
    static const unsigned long long fill[3] = {
        0x2A2A2A2A2A2A2A2AULL, 0x3434343434343434ULL, 0x2C2C2C2C2C2C2C2CULL };
    int mmx = has_mmx(), bad = 0, i, p;
    char msg[200] = "";

    if (!__djgpp_nearptr_enable()) { printf("no near pointers\n"); return 1; }
    vga = (volatile unsigned char *)(0xA0000 + __djgpp_conventional_base);

    setmode(0x13);
    outportb(0x3C4, 4); outportb(0x3C5, 0x06);                 /* unchain */
    outportb(0x3D4, 0x14); outportb(0x3D5, 0x00);
    outportb(0x3D4, 0x17); outportb(0x3D5, 0xE3);
    mapmask(0x0F);                                             /* all planes: trapped */

    /* rows 0-63: FST QWORD [EDX] (a double whose bytes are all 2Ah) */
    for (i = 0; i < 64 * ROW; i += 8)
        __asm__ volatile("fldl %1\n\tfstl (%0)\n\tfstp %%st(0)" :: "r"(vga + i), "m"(fill[0]) : "memory");
    /* rows 64-127: FILD/FISTP QWORD */
    for (i = 64 * ROW; i < 128 * ROW; i += 8)
        __asm__ volatile("fildll %1\n\tfistpll (%0)" :: "r"(vga + i), "m"(fill[1]) : "memory");
    /* rows 128-199: MOVQ (FST QWORD when there's no MMX) */
    for (i = 128 * ROW; i < 200 * ROW; i += 8)
        if (mmx) __asm__ volatile("movq %1, %%mm0\n\tmovq %%mm0, (%0)" :: "r"(vga + i), "m"(fill[2]) : "memory");
        else __asm__ volatile("fldl %1\n\tfstl (%0)\n\tfstp %%st(0)" :: "r"(vga + i), "m"(fill[2]) : "memory");
    if (mmx) __asm__ volatile("emms");

    /* every byte of every plane, read back with plain MOVs */
    for (p = 0; p < 4 && !bad; p++) {
        readmap(p);
        for (i = 0; i < 200 * ROW; i++) {
            unsigned char want = i < 64 * ROW ? 0x2A : i < 128 * ROW ? 0x34 : 0x2C;
            unsigned char got = vga[i];
            if (got != want) {
                sprintf(msg, "plane %d offset %d: %02X, wanted %02X\n", p, i, got, want);
                bad = 1; break;
            }
        }
    }
    /* 8-byte reads: FLD QWORD and FILD QWORD from plane 1 */
    if (!bad) {
        unsigned long long a = 0, b = 0;
        readmap(1);
        __asm__ volatile("fldl (%1)\n\tfstpl %0" : "=m"(a) : "r"(vga + 8) : "memory");
        __asm__ volatile("fildll (%1)\n\tfistpll %0" : "=m"(b) : "r"(vga + 64 * ROW + 8) : "memory");
        if (a != fill[0] || b != fill[1]) {
            sprintf(msg, "8-byte reads: %08lX%08lX %08lX%08lX\n", (unsigned long)(a >> 32), (unsigned long)a,
                    (unsigned long)(b >> 32), (unsigned long)b);
            bad = 1;
        }
    }
    setmode(0x03);
    printf("x87/MMX writes to unchained 256-color memory (%s): %s", mmx ? "MMX" : "no MMX", bad ? "FAILED\n" : "ok\n");
    if (bad) printf("%s", msg);
    return bad;
}
