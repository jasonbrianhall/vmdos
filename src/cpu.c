/* Machine setup: multiboot info, physical memory, paging, GDT/TSS/IDT,
   the real PIC and PIT, and kmain(). */
#include "kernel.h"

extern u8 __kernel_start[], __kernel_end[];
extern u64 gdt[];
extern u32 isr_table[48];
extern u8 v86_stack_top[];
void reload_gdt(void);

volatile u32 ticks;
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

/* ---------------- paging ----------------
   Linear 0..0x10FFFF: the guest's 1 MiB + HMA (user pages, backed by guest_ram).
   Linear 0x110000 up to the top of RAM: identity (kernel, heap, boot modules).
   The framebuffer: identity.  LOW_ALIAS: physical 0..1 MiB (real VGA text). */
#define LOW_ALIAS 0xFF000000u
static u32 *page_dir;
static u8 *guest_ram;
static int paging_on;

static u32 *pt_for(u32 lin)
{
    u32 *pde = &page_dir[lin >> 22];
    if (!(*pde & 1)) {
        u32 *pt = phys_alloc(4096);
        *pde = (u32)(uintptr_t)pt | 7;
    }
    return (u32 *)(uintptr_t)(*pde & ~0xFFFu);
}

void map_page(u32 lin, u32 phys, u32 flags)
{
    pt_for(lin)[(lin >> 12) & 1023] = (phys & ~0xFFFu) | flags;
}

static void map_range(u32 lin, u32 phys, u32 len, u32 flags)
{
    u32 off = lin & 0xFFF;
    lin -= off; phys -= off; len += off;
    for (u32 i = 0; i < len; i += 4096) {
        map_page(lin + i, phys + i, flags);
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

static u32 fb_phys, fb_len;

static void paging_init(void)
{
    page_dir = phys_alloc(4096);
    guest_ram = phys_alloc(GUEST_TOP);
    for (u32 a = 0; a < 0x100000; a += 4096) map_page(a, (u32)(uintptr_t)guest_ram + a, 7);
    set_a20(1);
    map_range(0x110000, 0x110000, ram_top - 0x110000, 3);
    map_range(LOW_ALIAS, 0, GUEST_TOP, 3);
    if (fb_len) map_range(fb_phys, fb_phys, fb_len, 3);
    __asm__ volatile("mov %0,%%cr3" ::"r"(page_dir));
    u32 cr0;
    __asm__ volatile("mov %%cr0,%0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ volatile("mov %0,%%cr0; jmp 1f; 1:" ::"r"(cr0) : "memory");
    paging_on = 1;
}

/* Map more MMIO later (e.g. a Bochs VBE framebuffer found by PCI scan). */
void map_mmio(u32 phys, u32 len)
{
    map_range(phys, phys, len, 3);
    flush_tlb();
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

    u32 used_end = (u32)(uintptr_t)__kernel_end;
    u32 mod_start = 0, mod_end = 0;
    if ((mb->flags & 8) && mb->mods_count) {
        struct mb_mod *m = (struct mb_mod *)(uintptr_t)mb->mods_addr;
        mod_start = m[0].start; mod_end = m[0].end;
        for (u32 i = 0; i < mb->mods_count; i++)
            if (m[i].end > used_end) used_end = m[i].end;
        kprintf("module: %x-%x (%u KiB)\n", mod_start, mod_end, (mod_end - mod_start) >> 10);
    }

    /* Choose the heap: the RAM region holding (or following) the kernel and modules. */
    used_end = (used_end + 4095) & ~4095u;
    if (mb->flags & 64) {
        u8 *p = (u8 *)(uintptr_t)mb->mmap_addr, *e = p + mb->mmap_length;
        for (; p < e; p += *(u32 *)p + 4) {
            u64 b = *(u64 *)(p + 4), l = *(u64 *)(p + 12);
            u32 type = *(u32 *)(p + 20);
            if (type != 1 || b >= 0x100000000ull) continue;
            u64 top = b + l;
            if (top > 0x100000000ull) top = 0x100000000ull - 4096;
            if (top > ram_top) ram_top = (u32)top;
            u32 s = b > used_end ? (u32)b : used_end;
            if (top > s && (u32)top - s > alloc_end - alloc_next) { alloc_next = s; alloc_end = (u32)top; }
        }
    } else {
        ram_top = 0x100000 + mb->mem_upper * 1024;
        alloc_next = used_end; alloc_end = ram_top;
    }
    alloc_end &= ~4095u;
    kprintf("RAM top %u MiB, heap %x-%x\n", ram_top >> 20, alloc_next, alloc_end);
    if (alloc_end <= alloc_next + (2u << 20)) panic("not enough memory");

    if (mod_start) {
        if (mod_start < GUEST_TOP) {      /* below 1 MiB+64K: move it out of the guest's way */
            u8 *p = phys_alloc(mod_end - mod_start);
            memcpy(p, (void *)(uintptr_t)mod_start, mod_end - mod_start);
            mod_end = (u32)(uintptr_t)p + (mod_end - mod_start);
            mod_start = (u32)(uintptr_t)p;
        }
        disk_image = (u8 *)(uintptr_t)mod_start;
        disk_size = mod_end - mod_start;
    }

    int have_fb = 0;
    struct mb_info fbi = *mb;
    if ((mb->flags & (1 << 12)) && mb->fb_type == 1 && mb->fb_addr < 0x100000000ull) {
        fb_phys = (u32)mb->fb_addr;
        fb_len = mb->fb_pitch * mb->fb_height;
        have_fb = 1;
    } else if (mb->flags & (1 << 12)) {
        kprintf("framebuffer type %u at %x:%x (unusable)\n", mb->fb_type,
                (u32)(mb->fb_addr >> 32), (u32)mb->fb_addr);
    }

    paging_init();
    tables_init();
    pic_init();
    pit_init();

    if (have_fb)
        video_framebuffer(fbi.fb_addr, fbi.fb_pitch, fbi.fb_width, fbi.fb_height, fbi.fb_bpp,
                          fbi.rpos, fbi.rsz, fbi.gpos, fbi.gsz, fbi.bpos, fbi.bsz);
    video_init();

    if (!disk_image)
        panic("No disk image. Load dos.img as a boot module "
              "(GRUB: module /boot/dos.img dos.img; QEMU: -initrd dos.img).");

    vdev_init();
    bios_init();
    guest_start();
}
