/* serial=dos: the PC's own COM1 and COM2 for DOS.

   By default COM1 carries vmdos's log and DOS sees no serial port. With
   serial=dos on the command line the log stays off the port, and each
   UART that's there (3F8h with IRQ 4, 2F8h with IRQ 3) is DOS's: its
   eight ports go straight to the chip, its interrupt is raised on the
   guest's PIC, the BIOS data area lists it and INT 14h drives it. So
   null-modem links, terminal programs and serial mice work as on a PC.
   (serial=off: just no log on COM1.) */
#include "kernel.h"

static const u16 com_base[2] = { 0x3F8, 0x2F8 };
static const u8 com_irq[2] = { 4, 3 };
static int com_on[2];

static int uart_present(u16 b)
{
    if (inb(b + 5) == 0xFF) return 0;              /* nothing decodes the port */
    outb(b + 7, 0x5A);                             /* the scratch register keeps what's written */
    if (inb(b + 7) != 0x5A) return 0;
    outb(b + 7, 0xA5);
    return inb(b + 7) == 0xA5;
}

void serial_dos_init(void)
{
    if (!serial_mode_dos()) return;
    int n = 0;
    for (int i = 0; i < 2; i++) {
        u16 b = com_base[i];
        if (!uart_present(b)) { kprintf("serial: COM%d (%xh) not there\n", i + 1, b); continue; }
        outb(b + 1, 0);                            /* no interrupts until DOS asks */
        outb(b + 4, 0);                            /* OUT2 off: the IRQ line stays quiet */
        (void)inb(b + 5); (void)inb(b + 6); (void)inb(b + 2); (void)inb(b);
        com_on[i] = 1;
        outb(0x21, inb(0x21) & ~(1u << com_irq[i]));   /* the real IRQ, raised on the guest's PIC */
        kprintf("serial: COM%d at %xh, IRQ %d, for DOS\n", i + 1, b, com_irq[i]);
        n++;
    }
    if (!n) kprintf("serial: serial=dos, but no COM1 or COM2 on this PC\n");
}

/* The base of DOS's n-th serial port (BDA order: those present, packed), or 0. */
u16 serial_dos_port(int n)
{
    for (int i = 0; i < 2; i++)
        if (com_on[i] && n-- == 0) return com_base[i];
    return 0;
}

/* A guest port access: 1 if it's a DOS-owned UART's (done on the chip). */
int serial_port(u16 port, int write, u8 *v)
{
    for (int i = 0; i < 2; i++) {
        if (!com_on[i] || port < com_base[i] || port > com_base[i] + 7) continue;
        if (write) outb(port, *v); else *v = inb(port);
        return 1;
    }
    return 0;
}

/* The real IRQ 3 or 4 (the PIC is acknowledged by the caller). The UART
   keeps its line up until the guest's handler reads it, so the PIC's
   edge comes again only for a new event, as on a PC. */
void serial_irq(int irq)
{
    for (int i = 0; i < 2; i++)
        if (com_on[i] && com_irq[i] == irq) vpic_raise(irq);
}

/* ---------------- INT 14h ---------------- */
static int wait_lsr(u16 b, u8 bit)                 /* about a second, as a PC's BIOS */
{
    u32 t0 = pit_clock();
    while (!(inb(b + 5) & bit)) {
        if (pit_clock() - t0 > PIT_HZ) return 0;
        sti(); __asm__ volatile("pause"); __asm__ volatile("cli");
    }
    return 1;
}

void serial_int14(struct regs *r)
{
    u16 b = DX(r) < 4 ? rd16(BDA + DX(r) * 2) : 0;
    int ours = 0;
    for (int i = 0; i < 2; i++) ours |= com_on[i] && b == com_base[i];
    if (!ours) { AH(r) = 0x80; return; }           /* no such port (or one DOS moved elsewhere): time-out */
    switch (AH(r)) {
    case 0x00: {                                   /* initialise: AL = baud (7-5), parity (4-3), stop (2), length (1-0) */
        static const u16 div[8] = { 1047, 768, 384, 192, 96, 48, 24, 12 };   /* 110 ... 9600 */
        u16 d = div[AL(r) >> 5];
        u8 lcr = (AL(r) & 3) | (AL(r) & 4) | ((AL(r) & 0x18) == 0x08 ? 0x08 : (AL(r) & 0x18) == 0x18 ? 0x18 : 0);
        outb(b + 3, 0x80);
        outb(b, (u8)d); outb(b + 1, d >> 8);
        outb(b + 3, lcr);
        outb(b + 1, 0);
        AH(r) = inb(b + 5); AL(r) = inb(b + 6);
        return; }
    case 0x01: {                                   /* send AL */
        outb(b + 4, inb(b + 4) | 3);               /* DTR, RTS */
        if (!wait_lsr(b, 0x20)) { AH(r) = inb(b + 5) | 0x80; return; }
        outb(b, AL(r));
        AH(r) = inb(b + 5);
        return; }
    case 0x02: {                                   /* receive into AL */
        outb(b + 4, inb(b + 4) | 1);               /* DTR */
        if (!wait_lsr(b, 0x01)) { AH(r) = (inb(b + 5) & 0x1E) | 0x80; return; }
        AH(r) = inb(b + 5) & 0x1E;
        AL(r) = inb(b);
        return; }
    case 0x03:                                     /* status: AH = line, AL = modem */
        AH(r) = inb(b + 5); AL(r) = inb(b + 6);
        return;
    }
    AH(r) = 0x80;
}
