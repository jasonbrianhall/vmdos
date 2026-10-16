/* An ISA NE2000 (National DP8390 + 32 KB of buffer RAM) for drivers that
   want real hardware: Windows 95/98/ME's "NE2000 Compatible", NE2000
   packet drivers, ODI/NDIS drivers for Windows 3.x. Its frames come from
   and go to the PC's real card (net.cpp), with the address DOS has there.

   Ports base..base+0Fh are the 8390's registers (three pages), base+10h-17h
   the data port for remote DMA, base+18h-1Fh reset. Kernel option
   ne2k=300,11 (hex base, IRQ; these are the default), ne2k=off.

   Modelled on QEMU's ne2000.c, which drivers have long been tested
   against: the same memory map (PROM at 0, RAM at 4000h-BFFFh), PROM
   contents and register behaviour. The interrupt line is the 8390's INT
   (ISR & IMR) and raises the IRQ on its rising edge, as on the ISA bus. */
#include "kernel.h"

/* net.cpp */
int net_peek_len(void);
int net_take(u8 *buf, int max);
int net_send(const u8 *f, unsigned len);
int net_dos_mac(u8 *mac, int fix);
void net_rx_mode(int m);
extern int net_ready;

#define CR_STP 0x01
#define CR_STA 0x02
#define CR_TXP 0x04
#define CR_RREAD 0x08
#define CR_RWRITE 0x10
#define CR_PAGE 0xC0

#define ISR_PRX 0x01
#define ISR_PTX 0x02
#define ISR_OVW 0x10
#define ISR_RDC 0x40
#define ISR_RST 0x80

#define RCR_AB 0x04
#define RCR_AM 0x08
#define RCR_PRO 0x10

#define PMEM_START 0x4000
#define PMEM_END 0xC000
#define MEM_SIZE PMEM_END

static u8 mem[MEM_SIZE];
static u16 base = 0x300;
static int irq = 11, enabled;
static u8 cmd, isr, imr, rcr, tcr, dcr, tsr, rsr, boundary, curpag, tpsr;
static u16 start, stop, tcnt, rsar, rcnt;
static u8 par[6], mult[8];
static int line;                                 /* INT out of the 8390 */
static int rx_seen;

static void update_irq(void)
{
    int now = (isr & imr & 0x7F) != 0;
    if (now && !line) vpic_raise(irq);
    line = now;
}

static void reset(int fix)
{
    u8 mac[6];
    isr = ISR_RST;
    cmd = CR_STP | 0x20;
    imr = 0;
    line = 0;
    if (!net_dos_mac(mac, fix)) memset(mac, 0, 6);
    u8 prom[16] = { 0 };                         /* the PROM: the MAC, ..., 'WW' (a 16-bit NE2000) */
    memcpy(prom, mac, 6);
    prom[14] = prom[15] = 0x57;
    for (int i = 0; i < 16; i++) mem[2 * i] = mem[2 * i + 1] = prom[i];   /* byte-wide, each byte doubled */
}

/* Room for a full frame between CURR and BNRY? */
static int buffer_full(void)
{
    u32 index = curpag << 8, bnd = boundary << 8, avail;
    if (start >= stop || stop > PMEM_END) return 1;
    if (index < bnd) avail = bnd - index;
    else avail = (stop - start) - (index - bnd);
    return avail < 1514 + 4 + 256;
}

static int want(const u8 *f)
{
    if (rcr & RCR_PRO) return 1;
    if (f[0] == 0xFF && f[1] == 0xFF && f[2] == 0xFF && f[3] == 0xFF && f[4] == 0xFF && f[5] == 0xFF)
        return (rcr & RCR_AB) != 0;
    if (f[0] & 1) return (rcr & RCR_AM) != 0;    /* multicast: no hash check, the filter is open */
    return memcmp(f, par, 6) == 0;
}

static void receive(const u8 *buf, int size)
{
    u8 pad[60];
    if (size < 60) { memcpy(pad, buf, size); memset(pad + size, 0, 60 - size); buf = pad; size = 60; }
    u32 index = curpag << 8;
    if (index >= PMEM_END || index < start || index >= stop) index = start;
    u32 total = size + 4;
    u32 next = index + ((total + 4 + 255) & ~0xFFu);
    if (next >= stop) next -= stop - start;
    rsr = 0x01 | ((buf[0] & 1) ? 0x20 : 0);      /* PRX, PHY (multicast/broadcast) */
    u8 *p = mem + index;
    p[0] = rsr;
    p[1] = (u8)(next >> 8);
    p[2] = (u8)total;
    p[3] = (u8)(total >> 8);
    index += 4;
    while (size > 0) {
        u32 avail = index <= stop ? stop - index : 0;
        u32 len = (u32)size < avail ? (u32)size : avail;
        memcpy(mem + index, buf, len);
        buf += len; index += len; size -= len;
        if (index == stop) index = start;
    }
    curpag = (u8)(next >> 8);
    isr |= ISR_PRX;
    update_irq();
}

int ne2k_running(void) { return enabled && !(cmd & CR_STP) && (cmd & CR_STA); }

/* From the timer tick: frames waiting in net.cpp into the ring. */
void ne2k_tick(void)
{
    static u8 f[1536];
    if (!ne2k_running() || (tcr & 0x06)) return;          /* loopback: nothing from the wire */
    while (net_peek_len() && !buffer_full()) {
        int n = net_take(f, sizeof f);
        if (n >= 14 && want(f)) { receive(f, n); rx_seen++; }
    }
}

static void real_mode(void)                     /* the real card's filter, to match */
{
    if (!ne2k_running()) return;
    net_rx_mode(rcr & RCR_PRO ? 6 : rcr & RCR_AM ? 5 : rcr & RCR_AB ? 3 : 2);
}

static u8 mem_rd8(u32 a) { return a < 32 || (a >= PMEM_START && a < PMEM_END) ? mem[a] : 0xFF; }
static void mem_wr8(u32 a, u8 v) { if (a >= PMEM_START && a < PMEM_END) mem[a] = v; }

static void dma_step(int len)
{
    rsar += len;
    if (rsar == stop) rsar = start;
    if (rcnt <= len) { rcnt = 0; isr |= ISR_RDC; update_irq(); }
    else rcnt -= len;
}

static u32 data_in(int size)
{
    u32 v = 0;
    int n = size == 4 ? 4 : (dcr & 1) ? 2 : 1;
    for (int i = 0; i < n; i += (dcr & 1) ? 2 : 1) {
        u32 a = rsar;
        u32 w;
        if (dcr & 1) { a &= ~1u; w = mem_rd8(a) | mem_rd8(a + 1) << 8; v |= w << (8 * i); dma_step(2); }
        else { w = mem_rd8(a); v |= w << (8 * i); dma_step(1); }
    }
    return v;
}

static void data_out(u32 v, int size)
{
    int n = size == 4 ? 4 : (dcr & 1) ? 2 : 1;
    for (int i = 0; i < n; i += (dcr & 1) ? 2 : 1) {
        u32 a = rsar;
        if (dcr & 1) { a &= ~1u; mem_wr8(a, (u8)(v >> (8 * i))); mem_wr8(a + 1, (u8)(v >> (8 * i + 8))); dma_step(2); }
        else { mem_wr8(a, (u8)(v >> (8 * i))); dma_step(1); }
    }
}

static void write_cr(u8 v)
{
    cmd = v;
    if (v & CR_STP) return;
    isr &= ~ISR_RST;
    if ((v & (CR_RREAD | CR_RWRITE)) && rcnt == 0) { isr |= ISR_RDC; update_irq(); }
    if (v & CR_TXP) {
        u32 index = tpsr << 8;
        if (index >= PMEM_END) index -= PMEM_END - PMEM_START;
        if (index + tcnt <= PMEM_END && tcnt >= 14) {
            unsigned len = tcnt > 1514 ? 1514 : tcnt;
            if (tcr & 0x06) {                       /* loopback: back to ourselves */
                if (want(mem + index)) receive(mem + index, len);
            } else net_send(mem + index, len);
        }
        tsr = 0x01;                                  /* PTX */
        isr |= ISR_PTX;
        cmd &= ~CR_TXP;
        update_irq();
    }
    real_mode();
}

static void reg_write(int r, u8 v)
{
    int page = cmd >> 6;
    if (r == 0) { write_cr(v); return; }
    if (page == 0) switch (r) {
        case 0x01: start = v << 8; break;
        case 0x02: stop = v << 8; break;
        case 0x03: boundary = v; break;
        case 0x04: tpsr = v; break;
        case 0x05: tcnt = (tcnt & 0xFF00) | v; break;
        case 0x06: tcnt = (tcnt & 0x00FF) | v << 8; break;
        case 0x07: isr &= ~(v & 0x7F); update_irq(); break;
        case 0x08: rsar = (rsar & 0xFF00) | v; break;
        case 0x09: rsar = (rsar & 0x00FF) | v << 8; break;
        case 0x0A: rcnt = (rcnt & 0xFF00) | v; break;
        case 0x0B: rcnt = (rcnt & 0x00FF) | v << 8; break;
        case 0x0C: rcr = v; real_mode(); break;
        case 0x0D: tcr = v; break;
        case 0x0E: dcr = v; break;
        case 0x0F: imr = v; update_irq(); break;
    } else if (page == 1) {
        if (r <= 6) par[r - 1] = v;
        else if (r == 7) curpag = v;
        else mult[r - 8] = v;
    }
}

static u8 reg_read(int r)
{
    int page = cmd >> 6;
    if (r == 0) return cmd;
    if (page == 0) switch (r) {
        case 0x03: return boundary;
        case 0x04: return tsr;
        case 0x07: return isr;
        case 0x08: return (u8)rsar;
        case 0x09: return (u8)(rsar >> 8);
        case 0x0A: return 0x50;                   /* RTL8019/8029 ID 'P' 'C', as QEMU's */
        case 0x0B: return 0x43;
        case 0x0C: return rsr;
        default: return 0;
    }
    if (page == 1) {
        if (r <= 6) return par[r - 1];
        if (r == 7) return curpag;
        return mult[r - 8];
    }
    if (page == 2) switch (r) {
        case 0x01: return start >> 8;
        case 0x02: return stop >> 8;
        case 0x04: return tpsr;
        case 0x0C: return rcr;
        case 0x0D: return tcr;
        case 0x0E: return dcr;
        case 0x0F: return imr;
        default: return 0;
    }
    if (r == 0x05) return 0x40;                  /* page 3 CONFIG2: 10BaseT */
    return 0;
}

/* 1 if port is the NE2000's (val in/out). */
int ne2k_port(u16 port, int size, int write, u32 *val)
{
    if (!enabled || port < base || port >= base + 0x20) return 0;
    int off = port - base;
    if (off < 0x10) {                            /* registers: a byte at a time */
        for (int i = 0; i < size && off + i < 0x10; i++) {
            if (write) reg_write(off + i, (u8)(*val >> (8 * i)));
            else *val = (i ? *val : 0) | (u32)reg_read(off + i) << (8 * i);
        }
        return 1;
    }
    if (off < 0x18) {
        if (write) data_out(*val, size);
        else *val = data_in(size);
        return 1;
    }
    if (!write) { reset(1); *val = 0; }           /* reading the reset port resets */
    return 1;
}

void ne2k_init(void)
{
    const char *p = strstr(cmdline, "ne2k=");
    if (!net_ready) return;
    if (p && !strncmp(p + 5, "off", 3)) return;
    if (p) {
        u32 b = 0, n = 0;
        p += 5;
        for (; (*p >= '0' && *p <= '9') || ((*p | 0x20) >= 'a' && (*p | 0x20) <= 'f'); p++)
            b = b * 16 + (*p <= '9' ? *p - '0' : (*p | 0x20) - 'a' + 10);
        if (b >= 0x200 && b <= 0x3E0) base = (u16)b;
        if (*p == ',') { for (p++; *p >= '0' && *p <= '9'; p++) n = n * 10 + (*p - '0'); if (n >= 2 && n <= 15) irq = n; }
    }
    enabled = 1;
    reset(0);
    kprintf("net: NE2000 at %xh, IRQ %d\n", base, irq);
}
