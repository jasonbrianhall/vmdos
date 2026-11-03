/* serial=dos: the PC's own serial ports for DOS.

   By default COM1 carries vmdos's log and DOS sees no serial port. With
   serial=dos on the command line the log stays off the port, and each
   UART that's there is DOS's: COM1 (3F8h, IRQ 4), COM2 (2F8h, IRQ 3),
   COM3 (3E8h, IRQ 4), COM4 (2E8h, IRQ 3), then the 16550 UARTs of PCI and
   PCIe serial cards (at their own I/O addresses, with the IRQ their
   firmware routed), up to four in all. comN=base[,irq] (hex base, decimal
   IRQ, 0: none) puts a UART in slot N by hand and implies serial=dos.

   A port of DOS's goes straight to the chip: the TSS's I/O bitmap (and
   VMHV's) lets the guest's IN and OUT through, so a program that bit-bangs
   the modem lines (a PIC or EPROM programmer driving DTR/RTS/TxD and
   reading CTS/DSR), polls the LSR at 115200 baud or reprograms the FIFOs
   sees the hardware with no trap in between, as on a PC. serial=dos,trap
   keeps the old path: every access trapped and done by the kernel.

   Its interrupt is raised on the guest's PIC and the BIOS data area lists
   it; INT 14h drives it. (serial=off: just no log on COM1.) */
#include "kernel.h"

#define NSLOT 4
struct com { u16 base; u8 irq; u8 pci; };          /* irq 0: none (polled) */
static struct com slot[NSLOT];
static int trap_mode;
static u16 level_masked;                           /* level-triggered IRQs masked on the PIC until the guest's EOI */
static u16 elcr;                                   /* the chipset's edge/level register (4D0h/4D1h) */

static const u16 legacy_base[4] = { 0x3F8, 0x2F8, 0x3E8, 0x2E8 };
static const u8 legacy_irq[4] = { 4, 3, 4, 3 };

static int uart_present(u16 b)
{
    if (inb(b + 5) == 0xFF) return 0;              /* nothing decodes the port */
    outb(b + 7, 0x5A);                             /* the scratch register keeps what's written */
    if (inb(b + 7) != 0x5A) return 0;
    outb(b + 7, 0xA5);
    return inb(b + 7) == 0xA5;
}

static int irq_usable(int irq)                     /* not the kernel's own (timer, keyboard, cascade, RTC, mouse, FPU) */
{
    return irq >= 3 && irq <= 15 && irq != 8 && irq != 12 && irq != 13;
}

static void real_irq_mask(int irq, int on)
{
    u16 p = irq < 8 ? 0x21 : 0xA1;
    u8 bit = 1u << (irq & 7);
    outb(p, on ? inb(p) | bit : inb(p) & ~bit);
}

static int slot_of(u16 base)
{
    for (int i = 0; i < NSLOT; i++) if (slot[i].base == base) return i;
    return -1;
}

static int add_uart(int want, u16 base, int irq, int pci)   /* want: a slot, or -1 for the first free */
{
    if (slot_of(base) >= 0) return 0;
    if (want < 0) for (int i = 0; i < NSLOT && want < 0; i++) if (!slot[i].base) want = i;
    if (want < 0 || slot[want].base) return 0;
    slot[want].base = base; slot[want].irq = irq_usable(irq) ? irq : 0; slot[want].pci = pci;
    return 1;
}

/* ---- PCI and PCIe serial cards (class 07h, subclass 00h: 16450/16550 and up) ---- */
static u32 pcfg(int bus, int dev, int fn, int off)
{
    outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fn << 8 | (off & 0xFC));
    return inl(0xCFC);
}
static void pcfg_w(int bus, int dev, int fn, int off, u32 v)
{
    outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fn << 8 | (off & 0xFC));
    outl(0xCFC, v);
}

static void pci_card(int bus, int dev, int fn)
{
    u32 id = pcfg(bus, dev, fn, 0), cmd = pcfg(bus, dev, fn, 4);
    u32 intr = pcfg(bus, dev, fn, 0x3C);
    int irq = (intr >> 8) & 0xFF ? (int)(intr & 0xFF) : 0;          /* no INTx pin: no IRQ */
    u16 bar[6], len[6];
    int nio = 0;
    pcfg_w(bus, dev, fn, 4, cmd & ~3u);                              /* decoding off while sizing */
    for (int i = 0; i < 6; i++) {
        u32 v = pcfg(bus, dev, fn, 0x10 + i * 4);
        bar[i] = len[i] = 0;
        if (!(v & 1)) { if ((v & 6) == 4) i++; continue; }          /* memory BAR (a 64-bit one takes two) */
        pcfg_w(bus, dev, fn, 0x10 + i * 4, 0xFFFFFFFF);
        u32 m = pcfg(bus, dev, fn, 0x10 + i * 4);
        pcfg_w(bus, dev, fn, 0x10 + i * 4, v);
        if ((v & 0xFFFC) && (m & 0xFFFC)) { bar[i] = v & 0xFFFC; len[i] = (u16)(~(m & 0xFFFC) + 1); nio++; }
    }
    /* I/O on, INTx enabled (bit 10 off); UEFI leaves a card no driver claimed undecoded */
    pcfg_w(bus, dev, fn, 4, (cmd | 1) & ~0x400u);
    kprintf("serial: PCI %02x:%02x.%x (%04x:%04x), %d I/O range(s), IRQ %d\n",
            bus, dev, fn, id & 0xFFFF, id >> 16, nio, irq);
    if (!nio) { kprintf("serial:   only memory-mapped: DOS can't use it\n"); return; }

    u16 cand[8]; int nc = 0;
    u16 ven = id & 0xFFFF, did = id >> 16;
    if (ven == 0x1C00 && bar[0] && len[0] >= 0x100) {               /* WCH CH382/CH384: UARTs from BAR0 + C0h */
        int n = did == 0x3470 ? 4 : did == 0x3253 || did == 0x3250 ? 2 : 1;
        for (int k = 0; k < n; k++) cand[nc++] = bar[0] + 0xC0 + k * 8;
    } else {
        for (int i = 0; i < 6 && nc < 8; i++) {
            if (!bar[i]) continue;
            if (len[i] == 8) cand[nc++] = bar[i];                    /* one UART a range (MosChip, ASIX, most) */
            else if (len[i] >= 16 && len[i] <= 64 && nio == 1)       /* one range, UARTs 8 apart */
                for (int k = 0; k < len[i] / 8 && nc < 8; k++) cand[nc++] = bar[i] + k * 8;
        }
    }
    for (int k = 0; k < nc; k++) {
        if (slot_of(cand[k]) >= 0 || !uart_present(cand[k])) continue;   /* (comN= took it already) */
        if (!add_uart(-1, cand[k], irq, 1)) { kprintf("serial:   UART at %xh: no COM slot left\n", cand[k]); continue; }
        if (!irq_usable(irq)) kprintf("serial:   UART at %xh: no legacy IRQ routed, polled only\n", cand[k]);
    }
}

static void pci_scan(void)
{
    for (int bus = 0; bus < 256; bus++)
        for (int dev = 0; dev < 32; dev++)
            for (int fn = 0; fn < 8; fn++) {
                u32 id = pcfg(bus, dev, fn, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (!fn) break; continue; }
                u32 cls = pcfg(bus, dev, fn, 8) >> 8;
                if ((cls >> 8) == 0x0700 && (cls & 0xFF) >= 1 && (cls & 0xFF) <= 6) pci_card(bus, dev, fn);
                if (!fn && !(pcfg(bus, dev, fn, 0x0C) & 0x800000)) break;
            }
}

static u32 hexnum(const char **p)
{
    u32 v = 0; const char *s = *p;
    for (;; s++) {
        char c = *s;
        if (c >= '0' && c <= '9') v = v * 16 + c - '0';
        else if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f') v = v * 16 + (c | 0x20) - 'a' + 10;
        else break;
    }
    *p = s; return v;
}

void serial_dos_init(void)
{
    if (!serial_mode_dos()) return;
    const char *s = strstr(cmdline, "serial=dos");
    trap_mode = s && !strncmp(s + 10, ",trap", 5);

    /* comN=base[,irq] first: they take their slot */
    for (int n = 0; n < NSLOT; n++) {
        char key[6] = { 'c', 'o', 'm', (char)('1' + n), '=', 0 };
        const char *p = strstr(cmdline, key);
        if (!p || (p != cmdline && p[-1] != ' ')) continue;
        p += 5;
        u16 b = (u16)hexnum(&p);
        int irq = 0;
        if (*p == ',') { p++; while (*p >= '0' && *p <= '9') irq = irq * 10 + *p++ - '0'; }
        if (b < 0x100 || !uart_present(b)) { kprintf("serial: com%d=%xh: no UART there\n", n + 1, b); continue; }
        add_uart(n, b, irq, 0);
    }
    for (int i = 0; i < 4; i++)
        if (slot_of(legacy_base[i]) < 0 && uart_present(legacy_base[i])) add_uart(-1, legacy_base[i], legacy_irq[i], 0);
    pci_scan();

    elcr = inb(0x4D0) | inb(0x4D1) << 8;
    if ((elcr & 0x0107) || elcr == 0xFFFF) elcr = 0;    /* IRQ 0-2 and 8 are always edge: no ELCR here */

    int n = 0;
    for (int i = 0; i < NSLOT; i++) {
        u16 b = slot[i].base;
        if (!b) continue;
        outb(b + 1, 0);                            /* no interrupts until DOS asks */
        outb(b + 4, 0);                            /* OUT2 off: the IRQ line stays quiet */
        /* FIFOs off, as after a PC's reset: UEFI firmware (and GRUB's serial
           console) leave them on, often with a 14-byte trigger. Then a byte
           interrupts only after 13 more or a timeout, and the IIR reads C4h
           or CCh where DOS-era code written for an 8250/16450 compares
           with 04h: it sees no received byte at all. */
        outb(b + 2, 0x06); outb(b + 2, 0);
        (void)inb(b + 5); (void)inb(b + 6); (void)inb(b + 2); (void)inb(b);
        if (!trap_mode) io_allow(b, 8);            /* straight to the chip: no trap */
        if (slot[i].irq) real_irq_mask(slot[i].irq, 0);   /* the real IRQ, raised on the guest's PIC */
        kprintf("serial: COM%d at %xh, IRQ %d%s, for DOS%s\n", i + 1, b, slot[i].irq,
                slot[i].irq && (elcr >> slot[i].irq & 1) ? " (level)" : "", trap_mode ? " (trapped)" : "");
        n++;
    }
    if (!n) kprintf("serial: serial=dos, but no serial port on this PC\n");
}

/* The base of DOS's n-th serial port (BDA slot n), or 0. */
u16 serial_dos_port(int n) { return n >= 0 && n < NSLOT ? slot[n].base : 0; }

/* A guest port access: 1 if it's a DOS-owned UART's (done on the chip).
   Only serial=dos,trap (or an instruction the kernel emulates) gets here. */
int serial_port(u16 port, int write, u8 *v)
{
    for (int i = 0; i < NSLOT; i++) {
        if (!slot[i].base || port < slot[i].base || port > slot[i].base + 7) continue;
        if (write) outb(port, *v); else *v = inb(port);
        return 1;
    }
    return 0;
}

/* A real IRQ (the PIC is acknowledged by the caller): 1 if it's a UART's.
   An edge-triggered line: the UART keeps it up until the guest's handler
   reads it, so the PIC's edge comes again only for a new event, as on a
   PC. A level-triggered one (a PCI card's, ELCR set): masked on the PIC
   until the guest's EOI, else it would fire again before the guest's
   handler ran. */
int serial_irq(int irq)
{
    int ours = 0;
    for (int i = 0; i < NSLOT; i++) ours |= slot[i].base && slot[i].irq == irq;
    if (!ours) return 0;
    if (elcr >> irq & 1) { real_irq_mask(irq, 1); level_masked |= 1u << irq; }
    vpic_raise(irq);
    return 1;
}

/* The guest wrote an EOI to its PIC: open the level-triggered lines it's done with. */
void serial_guest_eoi(void)
{
    if (!level_masked) return;
    for (int irq = 3; irq < 16; irq++)
        if ((level_masked >> irq & 1) && !vpic_busy(irq)) {
            level_masked &= ~(1u << irq);
            real_irq_mask(irq, 0);
        }
}

/* FreeDOS's COM driver (kernel/serial.asm) answers a non-destructive read
   by ending in the console's exit, CommonNdRdExit, which asks INT 16h about
   the keyboard: a byte from the port is fetched and kept, but the request
   says "busy" unless a key is waiting too. DOS polls stdin that way, so
   after CTTY COM1 the first byte typed is swallowed and DOS waits on the
   keyboard for good. Its jump is pointed one step on, at ConNdRd2, which
   hands back the byte kept (in AL) and reports it ready. The driver's code
   lies in segment 70h; nothing is changed unless all of it matches. */
static void freedos_comndread_fix(void)
{
    static int tries;
    if (tries >= 8) return;                          /* the kernel is long in place by then */
    tries++;
    static const short pat[] = {                       /* ComNdRead; -1: any byte */
        0xE8, -1, -1, 0x8A, 0x07, -2, 0xC0, 0x75, -1,     /* call GetComStat; mov al,[bx]; or al,al; jnz ComNdRd1 */
        0xE8, -1, -1, 0xF6, 0xC4, 0x01, 0x74, -1,         /* call ComRdStatus; test ah,1; jz ComNdRtn */
        0xE8, -1, -1, 0xE8, -1, -1, 0x88, 0x07,           /* call BiosRdCom; call GetComStat; mov [bx],al */
        0xE9, -1, -1 };                                   /* ComNdRd1: jmp CommonNdRdExit */
    const u32 seg = 0x700, n = sizeof pat / sizeof pat[0];
    for (u32 o = 0; o + n < 0x8000; o++) {
        u32 k;
        for (k = 0; k < n; k++) {
            u8 b = rd8(seg + o + k);
            if (pat[k] == -2 ? (b != 0x08 && b != 0x0A) : (pat[k] >= 0 && b != pat[k])) break;
        }
        if (k < n) continue;
        u16 jend = (u16)(o + n);                      /* offset after the jmp */
        u16 t = jend + rd16(seg + jend - 2);          /* CommonNdRdExit */
        /* mov al,[cs:uScanCode]; or al,al; jnz ConNdRd2 -> lds bx,[cs:_ReqPktPtr] */
        u8 o2 = rd8(seg + t + 4);
        if (rd8(seg + t) != 0x2E || rd8(seg + t + 1) != 0xA0 || (o2 != 0x08 && o2 != 0x0A) ||
            rd8(seg + t + 5) != 0xC0 || rd8(seg + t + 6) != 0x75) return;
        u16 c = t + 8 + (signed char)rd8(seg + t + 7);
        if (rd8(seg + c) != 0x2E || rd8(seg + c + 1) != 0xC5 || rd8(seg + c + 2) != 0x1E) return;
        wr16(seg + jend - 2, (u16)(c - jend));
        tries = 8;
        kprintf("serial: FreeDOS COM driver: non-destructive read fixed (0070:%04x)\n", jend - 3);
        return;
    }
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
    if (!b || slot_of(b) < 0) { AH(r) = 0x80; return; }   /* no such port (or one DOS moved elsewhere): time-out */
    freedos_comndread_fix();
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
    case 0x04: {                                   /* extended initialise (PS/2): up to 19200 baud */
        /* AL break, BH parity (0 none, 1 odd, 2 even, 3 mark, 4 space),
           BL stop bits (0: 1, 1: 2), CH length (0-3: 5-8 bits), CL baud (0-8: 110 ... 19200) */
        static const u16 div[9] = { 1047, 768, 384, 192, 96, 48, 24, 12, 6 };
        static const u8 par[5] = { 0x00, 0x08, 0x18, 0x28, 0x38 };
        u8 bh = (u8)(r->ebx >> 8), bl = (u8)r->ebx, ch = (u8)(r->ecx >> 8), cl = (u8)r->ecx;
        if (bh > 4 || bl > 1 || ch > 3 || cl > 8) { AH(r) = inb(b + 5) | 0x80; AL(r) = inb(b + 6); return; }
        u8 lcr = ch | (bl << 2) | par[bh] | (AL(r) ? 0x40 : 0);
        outb(b + 3, 0x80);
        outb(b, (u8)div[cl]); outb(b + 1, div[cl] >> 8);
        outb(b + 3, lcr);
        outb(b + 1, 0);
        AH(r) = inb(b + 5); AL(r) = inb(b + 6);
        return; }
    case 0x05:                                     /* extended control: AL 0 = read MCR into BL, 1 = write BL */
        if (AL(r) == 1) outb(b + 4, (u8)r->ebx & 0x1F);
        else if (AL(r) == 0) r->ebx = (r->ebx & ~0xFFu) | inb(b + 4);
        AH(r) = inb(b + 5); AL(r) = inb(b + 6);
        return;
    }
    AH(r) = 0x80;
}
