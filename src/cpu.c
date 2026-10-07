/* Machine setup: multiboot info, physical memory, paging, GDT/TSS/IDT,
   the real PIC and PIT, and kmain(). */
#include "kernel.h"

extern u8 __kernel_start[], __kernel_end[];
extern u64 gdt[];
extern u32 isr_table[48];
extern u8 v86_stack_top[];
void reload_gdt(void);

volatile u32 ticks;
int usb_ready;
u8 *disk_image;
u32 disk_size;
char cmdline[256];
int a20_on = 1;

/* ---------------- physical memory ---------------- */
static u32 alloc_next, alloc_end, ram_top;

void *phys_alloc(u32 bytes)
{
    bytes = (bytes + 4095) & ~4095u;
    if (alloc_next + bytes > alloc_end || alloc_next + bytes < alloc_next)
        panic("out of memory (wanted %u KiB, %u KiB left)", bytes >> 10, (alloc_end - alloc_next) >> 10);
    void *p = (void *)(uintptr_t)alloc_next;
    alloc_next += bytes;
    memset(p, 0, bytes);
    return p;
}

u32 phys_free(void) { return alloc_end - alloc_next; }

void *phys_try_alloc(u32 bytes)
{
    bytes = (bytes + 4095) & ~4095u;
    if (!bytes || bytes > alloc_end - alloc_next) return 0;
    return phys_alloc(bytes);
}

#define LOW_END 0x1000000u
#define ISA_DMA_LEN 0x10000u           /* linear below this: guest + DPMI window, not identity */

/* ---------------- paging ----------------
   Linear 0..0x10FFFF: the guest's 1 MiB + HMA (user pages, backed by guest_ram).
   Linear 0x110000 up to the top of RAM: identity (kernel, heap, boot modules).
   The framebuffer: identity if below 4 GiB, else a window under LOW_ALIAS.
   LOW_ALIAS: physical 0..1 MiB (real VGA text).
   PAE (Pentium Pro and later) when available, so a framebuffer above 4 GiB
   can be mapped; plain 2-level paging on a 386/486. */
#define LOW_ALIAS 0xFF000000u
static u32 *page_dir;                 /* non-PAE: the page directory */
static u64 *pdpt;                     /* PAE: 4 entries, then 4 page directories */
static u64 *pae_pd[4];
static int pae;
static u32 mmio_window = LOW_ALIAS;   /* MMIO above 4 GiB gets linear space below here */
static u8 *guest_ram;
static int paging_on;

static void *pt_for(u32 lin)
{
    if (pae) {
        u64 *pde = &pae_pd[lin >> 30][(lin >> 21) & 511];
        if (!(*pde & 1)) *pde = (u32)(uintptr_t)phys_alloc(4096) | 7;
        return (void *)(uintptr_t)(u32)(*pde & ~0xFFFull);
    }
    u32 *pde = &page_dir[lin >> 22];
    if (!(*pde & 1)) *pde = (u32)(uintptr_t)phys_alloc(4096) | 7;
    return (void *)(uintptr_t)(*pde & ~0xFFFu);
}

static void map_page64(u32 lin, u64 phys, u32 flags)
{
    if (pae) ((u64 *)pt_for(lin))[(lin >> 12) & 511] = (phys & ~0xFFFull) | flags;
    else ((u32 *)pt_for(lin))[(lin >> 12) & 1023] = ((u32)phys & ~0xFFFu) | flags;
}

void map_page(u32 lin, u32 phys, u32 flags) { map_page64(lin, phys, flags); }

static void map_range(u32 lin, u64 phys, u32 len, u32 flags)
{
    u32 off = lin & 0xFFF;
    lin -= off; phys -= off; len += off;
    for (u32 i = 0; i < len; i += 4096) {
        map_page64(lin + i, phys + i, flags);
        if (lin + i + 4096 == 0) break;
    }
}

static void flush_tlb(void)
{
    if (paging_on) __asm__ volatile("mov %%cr3,%%eax; mov %%eax,%%cr3" ::: "eax", "memory");
}

void set_a20(int on)
{
    a20_on = on;
    for (u32 i = 0; i < 16; i++)
        map_page(0x100000 + i * 4096, (u32)(uintptr_t)guest_ram + (on ? 0x100000 : 0) + i * 4096, 7);
    flush_tlb();
}

void *phys_low(u32 phys) { return (void *)(uintptr_t)(LOW_ALIAS + phys); }

/* Where the guest's own page for linear address lin is. */
u32 guest_phys(u32 lin) { return (u32)(uintptr_t)guest_ram + lin; }

void tlb_flush(void) { flush_tlb(); }

/* The page table's dirty bit for lin; cleared if clear. */
int page_dirty(u32 lin, int clear)
{
    int d;
    if (pae) { u64 *e = &((u64 *)pt_for(lin))[(lin >> 12) & 511]; d = (*e >> 6) & 1; if (clear) *e &= ~0x40ull; }
    else { u32 *e = &((u32 *)pt_for(lin))[(lin >> 12) & 1023]; d = (*e >> 6) & 1; if (clear) *e &= ~0x40u; }
    return d;
}

static int cpu_has_pae(void)
{
    u32 a, b;
    __asm__ volatile("pushfl; pop %0; mov %0,%1; xor $0x200000,%0; push %0; popfl; pushfl; pop %0; push %1; popfl"
                     : "=&r"(a), "=&r"(b));
    if (!((a ^ b) & 0x200000)) return 0;          /* no CPUID: 386 or early 486 */
    u32 eax = 1, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "+a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx));
    return (edx >> 6) & 1;
}

static u64 fb_phys;
static u32 fb_len, fb_lin;
static u32 mod_lo, mod_hi;
static u32 isa_dma_phys;              /* 64 KiB of RAM below 16 MiB for ISA DMA (a real Sound Blaster) */

/* The ISA DMA buffer, mapped through LOW_ALIAS (which reaches 16 MiB). */
void *isa_dma_buffer(u32 *phys)
{
    *phys = isa_dma_phys;
    return isa_dma_phys ? phys_low(isa_dma_phys) : 0;
}

static void paging_init(void)
{
    pae = cpu_has_pae() && !strstr(cmdline, "nopae");
    if (pae) {
        pdpt = phys_alloc(4096);
        for (int i = 0; i < 4; i++) {
            pae_pd[i] = phys_alloc(4096);
            pdpt[i] = (u32)(uintptr_t)pae_pd[i] | 1;
        }
    } else page_dir = phys_alloc(4096);
    guest_ram = phys_alloc(GUEST_TOP);
    for (u32 a = 0; a < 0x100000; a += 4096) map_page(a, (u32)(uintptr_t)guest_ram + a, 7);
    set_a20(1);
    map_range(LOW_END, LOW_END, ram_top - LOW_END, 3);
    if (mod_hi > ram_top) map_range(mod_lo, mod_lo, mod_hi - mod_lo, 3);
    map_range(LOW_ALIAS, 0, GUEST_TOP, 3);
    if (isa_dma_phys >= GUEST_TOP) map_range(LOW_ALIAS + isa_dma_phys, isa_dma_phys, ISA_DMA_LEN, 3);
    if (fb_len) {
        if (fb_phys + fb_len <= 0x100000000ull && (u32)fb_phys >= GUEST_TOP) fb_lin = (u32)fb_phys;
        else {
            fb_lin = (LOW_ALIAS - fb_len - 0x400000u) & ~0x3FFFFFu;
            if (fb_lin < ram_top || (fb_phys >> 32 && !pae)) { fb_lin = 0; fb_len = 0; }
            else mmio_window = fb_lin;
        }
        if (fb_len) map_range(fb_lin, fb_phys, fb_len, 3);
    }
    u32 cr0;
    if (pae) {
        u32 cr4;
        __asm__ volatile("mov %%cr4,%0" : "=r"(cr4));
        __asm__ volatile("mov %0,%%cr4" ::"r"(cr4 | 0x20));
        __asm__ volatile("mov %0,%%cr3" ::"r"(pdpt));
    } else __asm__ volatile("mov %0,%%cr3" ::"r"(page_dir));
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile("mov %0,%%cr0; jmp 1f; 1:" ::"r"(cr0) : "memory");
    paging_on = 1;
    kprintf("paging on (%s)\n", pae ? "PAE" : "32-bit");
}

/* Map more MMIO later (e.g. a Bochs VBE framebuffer found by PCI scan). */
void map_mmio(u32 phys, u32 len)
{
    map_range(phys, phys, len, 3 | 0x10);         /* PCD: uncached */
    flush_tlb();
}

/* Let ring 3 (DPMI clients) at these pages, or not. */
void set_user(u32 lin, u32 len, int user)
{
    for (u32 a = lin & ~0xFFFu; a < lin + len; a += 4096) {
        if (pae) { u64 *e = &((u64 *)pt_for(a))[(a >> 12) & 511]; *e = user ? *e | 4 : *e & ~4ull; }
        else { u32 *e = &((u32 *)pt_for(a))[(a >> 12) & 1023]; *e = user ? *e | 4 : *e & ~4u; }
    }
    flush_tlb();
}

void *map_mmio64_user(u64 phys, u32 len)
{
    void *p = map_mmio64(phys, len);
    if (p) set_user((u32)(uintptr_t)p, len, 1);
    return p;
}

/* MMIO anywhere: identity below 4 GiB, else a window below the framebuffer's. */
void *map_mmio64(u64 phys, u32 len)
{
    u32 off = (u32)phys & 0xFFF;
    len = (len + off + 4095) & ~4095u;
    if (phys + len <= 0x100000000ull && (u32)phys >= GUEST_TOP) {
        map_mmio((u32)phys, len);
        return (void *)(uintptr_t)(u32)phys;
    }
    if (!pae) return 0;
    u32 lin = (mmio_window - len) & ~0xFFFu;
    if (lin < ram_top + 0x1000000u) return 0;
    mmio_window = lin;
    map_range(lin, phys - off, len, 3 | 0x10);
    flush_tlb();
    return (void *)(uintptr_t)(lin + off);
}

/* ---------------- GDT, TSS, IDT ---------------- */
struct __attribute__((packed)) tss {
    u32 prev, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs, ldt;
    u16 trap, iomap;
    u8 intredir[32];
    u8 iobitmap[8192 + 1];
};
static struct tss tss __attribute__((aligned(16)));
static u64 idt[48];

static void tables_init(void)
{
    u32 base = (u32)(uintptr_t)&tss, limit = sizeof tss - 1;
    tss.ss0 = 0x10;
    tss.esp0 = (u32)(uintptr_t)v86_stack_top;
    tss.iomap = offsetof(struct tss, iobitmap);
    memset(tss.iobitmap, 0xFF, sizeof tss.iobitmap);   /* trap every port */
    gdt[3] = (limit & 0xFFFF) | ((u64)(base & 0xFFFFFF) << 16) | ((u64)0x89 << 40) |
             ((u64)((limit >> 16) & 0xF) << 48) | ((u64)(base >> 24) << 56);
    reload_gdt();
    __asm__ volatile("ltr %w0" ::"r"(0x18));

    for (int i = 0; i < 48; i++) {
        u32 h = isr_table[i];
        idt[i] = (h & 0xFFFF) | ((u64)0x08 << 16) | ((u64)0x8E << 40) | ((u64)(h >> 16) << 48);
    }
    struct __attribute__((packed)) { u16 lim; u32 base; } idtr = { sizeof idt - 1, (u32)(uintptr_t)idt };
    __asm__ volatile("lidt %0" ::"m"(idtr));
}

static void pic_init(void)
{
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 4);    outb(0xA1, 2);
    outb(0x21, 1);    outb(0xA1, 1);
    outb(0x21, 0xF8); /* timer, keyboard, cascade */
    outb(0xA1, 0xFF);
}

static void pit_init(void)
{
    outb(0x43, 0x34);
    outb(0x40, PIT_PER_TICK & 0xFF);
    outb(0x40, PIT_PER_TICK >> 8);
}

/* A 1.193182 MHz clock built from the tick count and the PIT's counter. */
u32 pit_clock(void)
{
    static u32 last;
    u32 fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl));
    outb(0x43, 0x00);
    u32 c = inb(0x40);
    c |= inb(0x40) << 8;
    u32 t = ticks;
    outb(0x20, 0x0A);
    if ((inb(0x20) & 1) && c > PIT_PER_TICK / 2) t++;   /* wrapped, IRQ not yet taken */
    if (c > PIT_PER_TICK) c = PIT_PER_TICK;
    u32 now = t * PIT_PER_TICK + (PIT_PER_TICK - c);
    if ((int32_t)(now - last) < 0) now = last;
    last = now;
    if (fl & EFL_IF) sti();
    return now;
}

void idle_wait(void) { __asm__ volatile("sti; hlt; cli" ::: "memory"); }

void reboot(void)
{
    cli();
    for (int i = 0; i < 10000 && (inb(0x64) & 2); i++) ;
    outb(0x64, 0xFE);
    outb(0xCF9, 0x0E);
    struct __attribute__((packed)) { u16 lim; u32 base; } z = { 0, 0 };
    __asm__ volatile("lidt %0; int3" ::"m"(z));
    for (;;) __asm__ volatile("hlt");
}

/* ---------------- multiboot ---------------- */
struct mb_info {
    u32 flags, mem_lower, mem_upper, boot_device, cmdline, mods_count, mods_addr;
    u32 syms[4], mmap_length, mmap_addr, drives_length, drives_addr, config_table;
    u32 boot_loader_name, apm_table, vbe_control_info, vbe_mode_info;
    u16 vbe_mode, vbe_seg, vbe_off, vbe_len;
    u64 fb_addr;
    u32 fb_pitch, fb_width, fb_height;
    u8 fb_bpp, fb_type, pad[2];           /* GRUB aligns the colour info to 112 */
    u8 rpos, rsz, gpos, gsz, bpos, bsz;
} __attribute__((packed));

struct mb_mod { u32 start, end, string, reserved; };
static struct { u32 start, end; char name[64]; } cdmod[8];
static int n_cdmod;

void kmain(u32 magic, struct mb_info *mb)
{
    serial_init();
    kprintf("\nvmdos: 32-bit v86 monitor\n");
    if (magic != 0x2BADB002) panic("not started by a Multiboot loader (magic %x)", magic);

    if ((mb->flags & 4) && mb->cmdline) {
        const char *c = (const char *)(uintptr_t)mb->cmdline;
        size_t n = strlen(c);
        if (n >= sizeof cmdline) n = sizeof cmdline - 1;
        memcpy(cmdline, c, n);
    }
    char *d = strstr(cmdline, "debug=");
    if (d) debug_level = d[6] - '0';
    kprintf("cmdline: %s\n", cmdline);

    /* Ranges the heap must avoid: the kernel, the modules, the boot information. */
    struct { u32 lo, hi; } ex[16];
    int nex = 0;
#define EXCL(a, b) do { if (nex < 16 && (b) > (a)) { ex[nex].lo = (a) & ~4095u; ex[nex].hi = ((b) + 4095) & ~4095u; nex++; } } while (0)
    EXCL((u32)(uintptr_t)__kernel_start, (u32)(uintptr_t)__kernel_end);
    EXCL((u32)(uintptr_t)mb, (u32)(uintptr_t)mb + sizeof *mb);
    if (mb->flags & 64) EXCL(mb->mmap_addr, mb->mmap_addr + mb->mmap_length);
    u32 mod_start = 0, mod_end = 0;
    if ((mb->flags & 8) && mb->mods_count) {
        struct mb_mod *m = (struct mb_mod *)(uintptr_t)mb->mods_addr;
        mod_start = m[0].start; mod_end = m[0].end;
        EXCL(mb->mods_addr, mb->mods_addr + mb->mods_count * 16);
        for (u32 i = 0; i < mb->mods_count && i < 8; i++) EXCL(m[i].start, m[i].end);
        /* the others are CD images: note them (names too) before anything is allocated */
        for (u32 i = 1; i < mb->mods_count && i < 8; i++) {
            cdmod[n_cdmod].start = m[i].start; cdmod[n_cdmod].end = m[i].end;
            const char *nm = m[i].string ? (const char *)(uintptr_t)m[i].string : "image.iso";
            int j = 0;
            for (; nm[j] && j < 63; j++) cdmod[n_cdmod].name[j] = nm[j];
            cdmod[n_cdmod].name[j] = 0;
            n_cdmod++;
        }
        kprintf("module: %x-%x (%u KiB)\n", mod_start, mod_end, (mod_end - mod_start) >> 10);
        mod_lo = mod_start & ~4095u; mod_hi = (mod_end + 4095) & ~4095u;
    }
    for (int i = 0; i < nex; i++)                 /* sort by start */
        for (int j = i + 1; j < nex; j++)
            if (ex[j].lo < ex[i].lo) { u32 t = ex[i].lo; ex[i].lo = ex[j].lo; ex[j].lo = t;
                                       t = ex[i].hi; ex[i].hi = ex[j].hi; ex[j].hi = t; }

    /* The heap: the largest free stretch of RAM between 1 MiB+64K and 4 GiB. */
    struct { u32 b, t; } rgn[128];
    int nr = 0;
    if (mb->flags & 64) {
        u8 *p = (u8 *)(uintptr_t)mb->mmap_addr, *e = p + mb->mmap_length;
        for (; p < e && nr < 128; p += *(u32 *)p + 4) {
            u64 b = *(u64 *)(p + 4), l = *(u64 *)(p + 12);
            if (*(u32 *)(p + 20) != 1 || b >= 0xFFFFF000ull) continue;
            u64 top = b + l;
            if (top > 0xFFFFF000ull) top = 0xFFFFF000ull;
            rgn[nr].b = (u32)b; rgn[nr].t = (u32)top; nr++;
        }
    } else {
        rgn[0].b = 0x100000; rgn[0].t = 0x100000 + mb->mem_upper * 1024; nr = 1;
    }
    for (int r = 0; r < nr; r++) {
        u32 lo = (rgn[r].b + 4095) & ~4095u, hi = rgn[r].t & ~4095u;
        if (hi > ram_top) ram_top = hi;
        if (lo < LOW_END) lo = LOW_END;              /* below 16 MiB: the guest and DPMI window */
        for (int i = 0; i <= nex && lo < hi; i++) {
            u32 end = i < nex && ex[i].lo < hi ? ex[i].lo : hi;
            if (end > lo && end - lo > alloc_end - alloc_next) { alloc_next = lo; alloc_end = end; }
            if (i < nex && ex[i].hi > lo) lo = ex[i].hi;
        }
    }
    /* 64 KiB below 16 MiB for ISA DMA: the highest free one in 1-16 MiB (out
       of the guest's way: nothing else uses physical 1-16 MiB once the
       module is moved), else in conventional memory above 128 KiB. */
    for (int pass = 0; pass < 2 && !isa_dma_phys; pass++) {
        u32 floor = pass ? 0x20000 : 0x100000, ceil = pass ? 0x90000 : LOW_END;
        for (int r = 0; r < nr && !isa_dma_phys; r++) {
            u32 lo = rgn[r].b < floor ? floor : rgn[r].b, hi = rgn[r].t > ceil ? ceil : rgn[r].t;
            if (hi < lo + ISA_DMA_LEN) continue;
            for (u32 a = (hi - ISA_DMA_LEN) & ~(ISA_DMA_LEN - 1); a >= lo && a + ISA_DMA_LEN <= hi; a -= ISA_DMA_LEN) {
                int clash = 0;
                for (int i = 0; i < nex; i++) if (a < ex[i].hi && ex[i].lo < a + ISA_DMA_LEN) clash = 1;
                if (!clash) { isa_dma_phys = a; break; }
                if (a < ISA_DMA_LEN) break;
            }
        }
    }
    dbg(1, "ISA DMA buffer at %x\n", isa_dma_phys);
    kprintf("RAM top %u MiB, heap %x-%x (%u MiB)\n", ram_top >> 20, alloc_next, alloc_end,
            (alloc_end - alloc_next) >> 20);
    if (alloc_end <= alloc_next + (2u << 20)) panic("not enough memory");

    int have_fb = 0;
    struct mb_info fbi = *mb;
    if ((mb->flags & (1 << 12)) && mb->fb_type == 1) {
        fb_phys = mb->fb_addr;
        fb_len = mb->fb_pitch * mb->fb_height;
        have_fb = 1;
    } else if (mb->flags & (1 << 12)) {
        kprintf("framebuffer type %u (not a linear RGB framebuffer)\n", mb->fb_type);
    }

    if (mod_start) {
        if (mod_start < LOW_END) {        /* below 16 MiB: move it out of the guest's / DPMI's way */
            u8 *p = phys_alloc(mod_end - mod_start);
            memcpy(p, (void *)(uintptr_t)mod_start, mod_end - mod_start);
            mod_end = (u32)(uintptr_t)p + (mod_end - mod_start);
            mod_start = (u32)(uintptr_t)p;
        }
        disk_image = (u8 *)(uintptr_t)mod_start;
        for (int i = 0; i < n_cdmod; i++) {
            u32 a = cdmod[i].start, n = cdmod[i].end - cdmod[i].start;
            if (a < LOW_END) {                   /* below 16 MiB: move it, as dos.img */
                u8 *p = phys_try_alloc(n);
                if (!p) { kprintf("cd: no room to move %s\n", cdmod[i].name); continue; }
                memcpy(p, (void *)(uintptr_t)a, n);
                a = (u32)(uintptr_t)p;
            }
            cd_add((u8 *)(uintptr_t)a, n, cdmod[i].name);
        }
        disk_size = mod_end - mod_start;
    }

    paging_init();
    tables_init();
    pic_init();
    pit_init();

    if (have_fb && !fb_len)
        kprintf("framebuffer at %x:%08x can't be mapped (needs PAE)\n", (u32)(fbi.fb_addr >> 32), (u32)fbi.fb_addr);
    else if (have_fb)
        video_framebuffer(fb_lin, fbi.fb_pitch, fbi.fb_width, fbi.fb_height, fbi.fb_bpp,
                          fbi.rpos, fbi.rsz, fbi.gpos, fbi.gsz, fbi.bpos, fbi.bsz);
    video_init();

    if (!disk_image)
        panic("No disk image. Load dos.img as a boot module "
              "(GRUB: module /boot/dos.img dos.img; QEMU: -initrd dos.img).");

    vdev_init();
    speed_init();
    bios_init();
    if (!strstr(cmdline, "usb=off")) usb_start(cmdline);
    sound_init();
    xms_init();
    mouse_ps2_init();
    usb_ready = 1;
    guest_start();
}
