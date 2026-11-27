/* kdos: the DOS kernel and shell of DOSBox-X, running in the monitor
   (shell=kernel on the kernel command line) instead of FreeDOS in the
   guest. This file is its side of the monitor, in the monitor's C; the
   DOSBox-X code and its glue are C++ (src/kdos/).

   How it runs: DOSBox-X marks its callbacks in guest code with the
   4-byte opcode FE 38 nn nn, which a real CPU doesn't have. In virtual-8086
   mode that is an invalid-opcode fault, so the monitor's #UD handler
   (v86.c) calls kdos_ud, which runs callback nn's C++ handler (glue.cpp).
   Everything else the guest runs natively, as under FreeDOS: programs,
   the BIOS, interrupt handlers. */
#include "kernel.h"

int kdos_on;                                   /* shell=kernel: no FreeDOS */
struct regs;
struct regs *kdos_trap(struct regs *r);        /* glue.cpp */

/* VMDOSNET (INT 2Fh AX=5652h): fs9p puts a share in a drive letter, which
   for DOSBox-X's DOS also needs a drive of its own (redir_drive.cpp) */
void fs9p_api(struct regs *r);
void kdos_net_mounted(int dr);
void kdos_net_unmounted(int dr);
void kdos_fs9p_api(struct regs *r)
{
    u16 fn = BX(r);
    int dr = fn == 1 ? rd8(LIN(r->v86_es, DI(r)) + 8) : DL(r);
    fs9p_api(r);
    if (!kdos_on || AX(r)) return;
    if (fn == 1) kdos_net_mounted(dr);
    if (fn == 2) kdos_net_unmounted(dr);
}

/* #UD in the guest: one of DOSBox-X's callbacks? (1: handled) */
int kdos_ud(struct regs *r)
{
    if (!kdos_on) return 0;
    u32 a = LIN(r->cs, IP(r));
    if (rd8(a) != 0xFE || rd8(a + 1) != 0x38) return 0;
    kdos_trap(r);
    return 1;
}

int kdos_wanted(void)
{
    const char *s = strstr(cmdline, "shell=kernel");
    return s && (s == cmdline || s[-1] == ' ');
}

/* ---- what glue.cpp needs from the monitor, as plain C ---- */
void kh_log(const char *s) { kprintf("%s", s); }
void kh_panic(const char *s) { panic("kdos: %s", s); }

/* kdos started again (VMHV's VM, a restart there): its memory as at its
   first start, with no more taken from the monitor. Its initialized data
   as loaded; the memory it had (2 MB heap pieces, stacks), handed out
   again in the order it asks, which is the same each start. */
static struct { void *p; u32 n; } kh_mem[64];
static int kh_nmem, kh_reuse;
static u8 *kh_data0;
void kh_restart(char *data, char *data_end)
{
    u32 n = (u32)(data_end - data);
    if (!kh_data0) { kh_data0 = phys_alloc(n ? n : 1); memcpy(kh_data0, data, n); return; }
    memcpy(data, kh_data0, n);
    kh_reuse = 0;
}
void *kh_alloc(u32 bytes)
{
    while (kh_reuse < kh_nmem)
        if (kh_mem[kh_reuse].n >= bytes) return kh_mem[kh_reuse++].p;
        else kh_reuse++;
    void *p = phys_alloc(bytes);
    if (kh_nmem < 64) { kh_mem[kh_nmem].p = p; kh_mem[kh_nmem++].n = bytes; kh_reuse = kh_nmem; }
    return p;
}
u32 kh_ticks(void) { return ticks; }

/* The guest's interrupt flag: in V86 mode the monitor's virtual one (vif);
   in VMHV's VM (real mode) the guest's own, in the trap's registers */
static struct regs *hv_trap_r;
int kh_vif_get(void) { extern int vif; return hv_trap_r ? (hv_trap_r->eflags & EFL_IF) != 0 : vif; }
void kh_vif_set(int v)
{
    extern int vif;
    if (!hv_trap_r) { vif = v; return; }
    if (v) hv_trap_r->eflags |= EFL_IF; else hv_trap_r->eflags &= ~EFL_IF;
}

/* #UD in VMHV's VM, in real or virtual-8086 mode (hv.c): one of kdos's
   callbacks? (1: handled, r updated) */
int kdos_hv_ud(struct regs *r)
{
    if (!kdos_on) return 0;
    u32 a = LIN(r->cs, IP(r));
    if (rd8(a) != 0xFE || rd8(a + 1) != 0x38) return 0;
    hv_trap_r = r;
    kdos_trap(r);
    hv_trap_r = 0;
    return 1;
}
int kh_a20(void) { return a20_on; }
void kh_set_a20(int on) { guest_a20(on); }
const char *kh_cmdline(void) { return cmdline; }

int kh_disk_read(u32 lba, u32 n, void *buf) { return disk_read(lba, n, buf); }
int kh_disk_write(u32 lba, u32 n, const void *buf) { return disk_write(lba, n, buf); }
u32 kh_disk_sectors(void) { return disk_sectors; }
/* C:'s FAT partition: its first sector (0: none) */
int kh_disk_part(u32 *base, u32 *count)
{
    static struct fatvol v;
    if (!disk_sectors || disk_volume(&v)) return -1;
    *base = v.base;
    *count = v.total;
    return 0;
}

/* INT 10h through vmdos's video BIOS, straight from the monitor (as
   DOSBox-X's CON device calls its own INT 10h code directly) */
void kh_int10(u32 *eax, u32 *ebx, u32 *ecx, u32 *edx, u16 es, u32 *ebp)
{
    static struct regs t;
    memset(&t, 0, sizeof t);
    t.eax = *eax; t.ebx = *ebx; t.ecx = *ecx; t.edx = *edx; t.ebp = *ebp;
    t.v86_es = es; t.eflags = EFL_VM | 2;
    bios_service(&t, 0x10, 0);
    *eax = t.eax; *ebx = t.ebx; *ecx = t.ecx; *edx = t.edx; *ebp = t.ebp;
}

/* floppies (floppy.c): A: and B: for DOSBox-X's FAT driver */
u32 fd_kdos_sectors(int d);
int fd_kdos_rw(int d, u32 lba, u32 n, void *buf, int write);
u32 fd_kdos_gen(int d);
u32 kh_fd_sectors(int d) { return fd_kdos_sectors(d); }
int kh_fd_rw(int d, u32 lba, u32 n, void *buf, int write) { return fd_kdos_rw(d, lba, n, buf, write); }
u32 kh_fd_gen(int d) { return fd_kdos_gen(d); }

/* the CMOS clock, for DOS's date and time */
static u8 cmos(u8 reg) { outb(0x70, reg); return inb(0x71); }
static int bcd(u8 v, int is_bcd) { return is_bcd ? (v >> 4) * 10 + (v & 15) : v; }
void kh_rtc(int *y, int *mo, int *d, int *h, int *mi, int *s)
{
    while (cmos(0x0A) & 0x80) ;                         /* update in progress */
    int isbcd = !(cmos(0x0B) & 4);
    *s = bcd(cmos(0), isbcd); *mi = bcd(cmos(2), isbcd); *h = bcd(cmos(4) & 0x7F, isbcd);
    *d = bcd(cmos(7), isbcd); *mo = bcd(cmos(8), isbcd); *y = 2000 + bcd(cmos(9), isbcd);
}
