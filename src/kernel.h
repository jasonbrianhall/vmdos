/* vmdos: a 32-bit protected-mode kernel that runs DOS in virtual-8086 mode. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

/* ---- ports ---- */
static inline void outb(u16 p, u8 v) { __asm__ volatile("outb %0,%1" ::"a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0,%1" ::"a"(v), "Nd"(p)); }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0,%1" ::"a"(v), "Nd"(p)); }
static inline u8 inb(u16 p) { u8 v; __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline u16 inw(u16 p) { u16 v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline u32 inl(u16 p) { u32 v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void cli(void) { __asm__ volatile("cli" ::: "memory"); }
static inline void sti(void) { __asm__ volatile("sti" ::: "memory"); }

/* ---- interrupt frame (boot.S builds it; see isr_common) ---- */
struct regs {
    u32 gs, fs, es, ds;                       /* zero when coming from v86 */
    u32 edi, esi, ebp, esp_k, ebx, edx, ecx, eax;
    u32 vec, err;
    u32 eip, cs, eflags;
    u32 esp, ss;                              /* only from ring 3 / v86 */
    u32 v86_es, v86_ds, v86_fs, v86_gs;       /* only from v86 */
};

#define EFL_CF 0x0001
#define EFL_PF 0x0004
#define EFL_AF 0x0010
#define EFL_ZF 0x0040
#define EFL_SF 0x0080
#define EFL_TF 0x0100
#define EFL_IF 0x0200
#define EFL_DF 0x0400
#define EFL_OF 0x0800
#define EFL_VM 0x20000

/* 16/8-bit register views of a frame */
#define AX(r) (*(u16 *)&(r)->eax)
#define BX(r) (*(u16 *)&(r)->ebx)
#define CX(r) (*(u16 *)&(r)->ecx)
#define DX(r) (*(u16 *)&(r)->edx)
#define SI(r) (*(u16 *)&(r)->esi)
#define DI(r) (*(u16 *)&(r)->edi)
#define BP(r) (*(u16 *)&(r)->ebp)
#define SP(r) (*(u16 *)&(r)->esp)
#define IP(r) (*(u16 *)&(r)->eip)
#define AL(r) (*(u8 *)&(r)->eax)
#define AH(r) (((u8 *)&(r)->eax)[1])
#define BL(r) (*(u8 *)&(r)->ebx)
#define BH(r) (((u8 *)&(r)->ebx)[1])
#define CL(r) (*(u8 *)&(r)->ecx)
#define CH(r) (((u8 *)&(r)->ecx)[1])
#define DL(r) (*(u8 *)&(r)->edx)
#define DH(r) (((u8 *)&(r)->edx)[1])

/* ---- guest memory: linear 0..GUEST_TOP is the v86 guest's address space ---- */
#define GUEST_TOP 0x110000u
static inline u8 *gptr(u32 lin) { return (u8 *)(uintptr_t)lin; }
static inline u8 rd8(u32 a) { return *(volatile u8 *)gptr(a); }
static inline u16 rd16(u32 a) { return *(volatile u16 *)gptr(a); }
static inline u32 rd32(u32 a) { return *(volatile u32 *)gptr(a); }
static inline void wr8(u32 a, u8 v) { *(volatile u8 *)gptr(a) = v; }
static inline void wr16(u32 a, u16 v) { *(volatile u16 *)gptr(a) = v; }
static inline void wr32(u32 a, u32 v) { *(volatile u32 *)gptr(a) = v; }
#define LIN(seg, off) ((((u32)(seg)) << 4) + (u16)(off))
#define BDA 0x400u

/* ---- lib.c ---- */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strncmp(const char *a, const char *b, size_t n);
char *strstr(const char *h, const char *n);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
void serial_init(void);
void kprintf(const char *fmt, ...);
extern int debug_level;
#define dbg(lvl, ...) do { if (debug_level >= (lvl)) kprintf(__VA_ARGS__); } while (0)
__attribute__((noreturn)) void panic(const char *fmt, ...);

/* ---- cpu.c ---- */
extern volatile u32 ticks;                    /* real timer ticks (TICK_HZ) */
#define TICK_HZ 1000
#define PIT_HZ 1193182u
#define PIT_PER_TICK 1193u
u32 pit_clock(void);                          /* 1.193182 MHz monotonic clock (wraps) */
void *phys_alloc(u32 bytes);                  /* page-aligned, zeroed */
u32 phys_free(void);                          /* bytes left in the heap */
void *phys_try_alloc(u32 bytes);              /* like phys_alloc, NULL when out */
void set_user(u32 lin, u32 len, int user);    /* ring 3 access to these pages */
void *map_mmio64_user(u64 phys, u32 len);
u32 guest_phys(u32 lin);                      /* physical page behind guest linear lin */
void tlb_flush(void);
int page_dirty(u32 lin, int clear);           /* PTE dirty bit (cleared if clear) */
void map_page(u32 lin, u32 phys, u32 flags);
void set_a20(int on);
extern int a20_on;
extern int usb_ready;
void *phys_low(u32 phys);                     /* pointer to physical memory below 1 MiB */
void *isa_dma_buffer(u32 *phys);              /* 64 KiB of RAM below 16 MiB, 64 KiB aligned (0: none) */
void idle_wait(void);                         /* sti; hlt; cli */
void reboot(void) __attribute__((noreturn));
void map_mmio(u32 phys, u32 len);
void *map_mmio64(u64 phys, u32 len);          /* any address; NULL if it can't be mapped */
void usb_start(const char *cmdline);
void usb_tick(void);
extern u32 kernel_stack_top;
void v86_enter(struct regs *r) __attribute__((noreturn));
extern u8 *disk_image;                        /* the RAM disk (boot module) */
extern u32 disk_size;
extern char cmdline[256];

/* ---- v86.c ---- */
extern int vif;                               /* the guest's virtual IF */
void v86_push16(struct regs *r, u16 v);
u16 v86_pop16(struct regs *r);
void v86_reflect(struct regs *r, int vec);    /* deliver INT vec into the guest */
struct regs *isr_dispatch(struct regs *r);    /* returns the frame to resume */
void guest_start(void) __attribute__((noreturn));

/* ---- vdev.c: virtual PIC, PIT, keyboard controller, misc ports ---- */
void vdev_init(void);
void vpic_raise(int irq);
int vpic_pending(void);                       /* vector to deliver now, or -1 */
void vpic_ack(int vec);                       /* mark in service */
void vpic_eoi_master(void);
void vpic_eoi_irq(int irq);
u32 port_in(u16 port, int size);
void port_out(u16 port, u32 val, int size);
void vdev_tick(void);                         /* from the real timer IRQ */
void vkbd_real_scancode(u8 sc);               /* from the real keyboard IRQ */
void speed_init(void);                        /* speed= (slowdown) */
void speed_throttle(void);                    /* after each timer tick */
u32 speed_api(u32 pm);                        /* INT 2Fh AX=5653h */
void video_osd(const char *msg);              /* a note at the top right for 2 s */
void video_vbe_window(struct regs *r);        /* VESA bank switch (4F05h / WinFuncPtr) */
int video_vram_range(u32 p, u32 n);           /* inside the VESA framebuffer? */
int vkbd_read_data(void);                     /* guest's port 60h read */
void vkbd_refill(void);
u32 vpit_clock(void);

/* ---- bios.c ---- */
void bios_init(void);
#define BIOS_DONE 0     /* service ran; return to caller (emulated IRET) */
#define BIOS_CONT 1     /* continue executing the stub after the trap */
#define BIOS_RETRY 2    /* nothing yet: wait for an interrupt and run again */
#define BIOS_DONEF 3    /* like BIOS_DONE, but CF and ZF are results for the caller */
#define BIOS_SWITCH 4   /* the guest changed mode (DPMI): leave the old frame alone */
int bios_service(struct regs *r, int id, int via_stub);
void bios_boot(struct regs *r);
u32 bios_stub_entry(int vec);                 /* linear address of IVT default for vec */
int bios_stub_is_direct(int vec);

/* ---- xms.c: the XMS 3.0 driver (HMA, extended memory blocks, UMBs) ---- */
void xms_init(void);
void xms_call(struct regs *r);

/* ---- mouse.c: PS/2 and USB mice, INT 33h ---- */
void mouse_ps2_init(void);
void mouse_ps2_byte(u8 b);
void mouse_input(int dx, int dy, int buttons);
void mouse_usb_attached(void);
void mouse_update(void);
void mouse_int33(struct regs *r);
int mouse_callback_due(void);
void mouse_start_callback(struct regs *r);
u16 mouse_begin_callback(void);               /* for a client in PM: the stub's offset */
struct regs *dpmi_rm_iret_call(struct regs *r, u16 cs, u16 ip);
int dpmi_reflected_buffer(struct regs *r, u32 *lin);   /* PM ES:EDI of a reflected INT */
void mouse_cb_regs(struct regs *r);
void mouse_cb_done(void);
int mouse_pointer(int *x, int *y, u16 *and_mask, u16 *xor_mask);

/* ---- dpmi.c: DPMI 0.9 host ---- */
void dpmi_detect(struct regs *r);             /* INT 2Fh AX=1687h */
int dpmi_rm_trap(struct regs *r, int id);     /* v86-side stubs; BIOS_* */
struct regs *dpmi_exception(struct regs *r);  /* any fault from a protected-mode client */
struct regs *dpmi_take_switch(void);          /* frame of a mode switch, or NULL */
int dpmi_pm_hooked(int vec);
struct regs *dpmi_hw_interrupt(struct regs *r, int vec);
struct regs *dpmi_reflect_irq(struct regs *r, int vec);
void wait_for_irq(void);
void dpmi_espfix(struct regs *r);

/* ---- sound.c ---- */
void sound_init(void);
void sound_tick(void);                        /* every timer tick */
int sound_port(u16 port, int write, u8 *v);   /* 1 if it's a sound port */

/* ---- video.c ---- */
void video_init(void);
void video_refresh(void);                     /* called at ~60 Hz */
void video_set_mode(int mode, int clear);
u32 video_port_in(u16 port);
void video_port_out(u16 port, u8 v);
void video_console(const char *msg);          /* panic screen */
extern int video_mode;
extern u8 vga_dac[256][3];
extern u8 crtc[32];
extern int palette_dirty;
void video_framebuffer(u64 addr, u32 pitch, u32 w, u32 h, u32 bpp,
                       u8 rpos, u8 rsz, u8 gpos, u8 gsz, u8 bpos, u8 bsz);
void video_text_fallback(void);
void video_int10(struct regs *r);
/* mememu.c: emulate the instruction that touched trapped video memory */
struct emu_cpu {
    u32 *gpr[8];                              /* eax ecx edx ebx esp ebp esi edi */
    u32 *eip, *eflags;
    u32 seg_base[6];                          /* es cs ss ds fs gs */
    int code32;                               /* 32-bit code segment */
};
int mem_emulate(struct emu_cpu *e);
int vga16_window(u32 lin);                    /* video.c: 16-colour planar window */
u8 vga16_read(u32 lin);
void vga16_write(u32 lin, u8 v);
int vga16_fault(struct emu_cpu *e, u32 cr2);  /* 1: emulated */
int video_gfx_height(void);                   /* graphics mode height (200 in text modes) */
void video_puts(const char *s);
extern const u8 *vga_font16, *vga_font8;
