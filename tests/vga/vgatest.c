/* vga_show_colors.c
 * Shows every color the current DAC can produce (6-bit or 8-bit).
 * Compile: gcc -O2 -o vga_show_colors.exe vga_show_colors.c
 */

#include <stdio.h>
#include <string.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/farptr.h>
#include <sys/movedata.h>
#include <pc.h>
#include <conio.h>

#define VGA_SEGMENT 0xA0000

/* ------------------------------------------------------------------ */
/* VBE helpers                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char     VbeSignature[4];
    unsigned short VbeVersion;
    unsigned long  OemStringPtr;
    unsigned long  Capabilities;   /* bit 0 = DAC switchable */
} __attribute__((packed)) VbeInfoBlock;

static int vbe_dac_switchable(void)
{
    VbeInfoBlock info;
    __dpmi_regs r;
    unsigned long tb = __tb;

    memset(&info, 0, sizeof(info));
    memcpy(info.VbeSignature, "VBE2", 4);
    dosmemput(&info, sizeof(info), tb);

    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F00;
    r.x.es = tb >> 4;
    r.x.di = tb & 0x0F;
    __dpmi_int(0x10, &r);

    if (r.h.al != 0x4F || r.h.ah != 0)
        return 0;

    dosmemget(tb, sizeof(info), &info);
    if (memcmp(info.VbeSignature, "VESA", 4) != 0)
        return 0;

    return (info.Capabilities & 1) != 0;
}

static int set_dac_bits(int bits)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F08;
    r.h.bl = 0;          /* set */
    r.h.bh = bits;
    __dpmi_int(0x10, &r);
    if (r.h.al != 0x4F || r.h.ah != 0)
        return -1;
    return r.h.bh;       /* actual bits obtained */
}

static int get_dac_bits(void)
{
    __dpmi_regs r;
    memset(&r, 0, sizeof(r));
    r.x.ax = 0x4F08;
    r.h.bl = 1;          /* get */
    __dpmi_int(0x10, &r);
    if (r.h.al != 0x4F || r.h.ah != 0)
        return -1;
    return r.h.bh;
}

/* ------------------------------------------------------------------ */
/* Mode / palette                                                     */
/* ------------------------------------------------------------------ */

static void set_mode13(void)
{
    __dpmi_regs r;
    r.x.ax = 0x13;
    __dpmi_int(0x10, &r);
}

static void set_text_mode(void)
{
    __dpmi_regs r;
    r.x.ax = 0x03;
    __dpmi_int(0x10, &r);
}

/* Load a palette that uses the full range the DAC currently supports.
 * bits == 8 → values 0-255
 * bits == 6 → values 0-63 (scaled so the ramp still looks correct)
 */
static void load_palette(int bits)
{
    int i, max = (bits == 8) ? 255 : 63;

    outportb(0x3C8, 0);

    /* 0-63   : pure red ramp   */
    for (i = 0; i < 64; i++) {
        int v = (bits == 8) ? (i * 255 / 63) : i;
        outportb(0x3C9, v);
        outportb(0x3C9, 0);
        outportb(0x3C9, 0);
    }
    /* 64-127 : pure green ramp */
    for (i = 0; i < 64; i++) {
        int v = (bits == 8) ? (i * 255 / 63) : i;
        outportb(0x3C9, 0);
        outportb(0x3C9, v);
        outportb(0x3C9, 0);
    }
    /* 128-191: pure blue ramp  */
    for (i = 0; i < 64; i++) {
        int v = (bits == 8) ? (i * 255 / 63) : i;
        outportb(0x3C9, 0);
        outportb(0x3C9, 0);
        outportb(0x3C9, v);
    }
    /* 192-255: greyscale       */
    for (i = 0; i < 64; i++) {
        int v = (bits == 8) ? (i * 255 / 63) : i;
        outportb(0x3C9, v);
        outportb(0x3C9, v);
        outportb(0x3C9, v);
    }
}

/* Draw four vertical color bars that cover the whole screen */
static void draw_color_bars(void)
{
    int x, y, idx;

    for (y = 0; y < 200; y++) {
        for (x = 0; x < 320; x++) {
            if      (x <  80) idx = (x * 64) / 80;          /* red   0-63   */
            else if (x < 160) idx = 64 + ((x-80) * 64)/80; /* green 64-127 */
            else if (x < 240) idx = 128 + ((x-160)*64)/80;/* blue  128-191*/
            else              idx = 192 + ((x-240)*64)/80;/* grey  192-255*/

            _farpokeb(_dos_ds, VGA_SEGMENT + y*320 + x, idx);
        }
    }
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    int bits, got;

    printf("Checking for 8-bit DAC support...\n");

    if (vbe_dac_switchable()) {
        bits = get_dac_bits();
        printf("Current DAC width: %d bits\n", bits);

        got = set_dac_bits(8);
        if (got == 8) {
            bits = 8;
            printf("Successfully switched to 8-bit DAC.\n");
        } else {
            bits = (got > 0) ? got : 6;
            printf("Hardware stayed at %d-bit DAC.\n", bits);
        }
    } else {
        bits = 6;
        printf("No VBE DAC switching – using standard 6-bit DAC.\n");
    }

    printf("Press any key to show the color bars...\n");
    getch();

    set_mode13();
    load_palette(bits);
    draw_color_bars();

    /* stay until key press */
    while (!kbhit())
        ;
    getch();

    set_text_mode();
    printf("Done. DAC was %d-bit.\n", bits);
    return 0;
}
