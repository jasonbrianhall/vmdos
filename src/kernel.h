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
struct regs;
void log_api(struct regs *r);                 /* INT 2Fh AX=564Ch: DMESG.COM reads the log */
const char *log_save(void);                   /* the log into C:\VMDOS\VMDOS.LOG (0: done) */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strncmp(const char *a, const char *b, size_t n);
char *strstr(const char *h, const char *n);
int vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);
int snprintf(char *buf, size_t n, const char *fmt, ...);
void serial_init(const char *cmd);            /* the log on COM1 (or not: serial=off / serial=dos) */
extern volatile int log_save_req;             /* Ctrl+Shift+S with the log open: log_save at the next tick */
int log_tail(char *out, int rows, int scroll);   /* the log's last lines, for the overlay (lib.c) */
void video_log_toggle(void);                  /* Ctrl+Shift+F10: the log over the running game (video.c) */
int video_log_on(void);
void video_log_scroll(int lines);             /* + up (older), - down; big numbers: to the top / the end */
void tlb_flush_range(u32 lin, u32 len);       /* INVLPG those pages (a full flush on a 386) */
void kdelay_us(u32 us);                       /* busy-wait, TSC-timed (any CPU, interrupts on or off) */
u64 tsc_read(void);
u32 tsc_khz(void);                            /* 0: no TSC */
/* perf on the command line: where the boot CPU's time goes, every 5 s (v86.c) */
extern int perf_on;
void perf_io_probe(void);
extern u32 perf_ports[1024];                  /* guest port accesses by port (vdev.c) */
int serial_mode_dos(void);
void serial_dos_init(void);                   /* serial.c: serial=dos, COM1/COM2 for DOS */
u16 serial_dos_port(int n);                   /* DOS's n-th serial port's base, or 0 */
int serial_port(u16 port, int write, u8 *v);  /* a guest access to a DOS-owned UART */
void serial_irq(int irq);                     /* the real IRQ 3 or 4 */
struct regs;
void serial_int14(struct regs *r);
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
void tick_poll_begin(void);                   /* around long work with interrupts off: */
void tick_poll(void);                         /*   call well under a tick apart, so no tick is lost */
void tick_poll_end(void);
void *phys_alloc(u32 bytes);                  /* page-aligned, zeroed */
u32 phys_free(void);                          /* bytes left in the heap */
void *phys_try_alloc(u32 bytes);              /* like phys_alloc, NULL when out */
void phys_release(void *p, u32 bytes);        /* give back (what phys_alloc gave, or a part) */
u32 phys_largest(void);                       /* the biggest piece phys_alloc could give */
#define HEAP_RESERVE (4u << 20)                /* kept for the kernel: EMS, XMS, DPMI stop short of it */
u32 phys_spare(void);                         /* phys_free less HEAP_RESERVE */
u32 ram_top_addr(void);                       /* the end of RAM below 4 GiB */
void set_user(u32 lin, u32 len, int user);    /* ring 3 access to these pages */
void *map_mmio64_user(u64 phys, u32 len);
u32 guest_phys(u32 lin);                      /* physical page behind guest linear lin */
void tlb_flush(void);
int page_dirty(u32 lin, int clear);           /* PTE dirty bit (cleared if clear) */
void smp_tick(void);                          /* boot CPU, each timer tick: wake the helper if it's halted */
void video_tlb_tick(void);                    /* boot CPU, each timer tick: the TLB flush the VESA redraw asked for */
void map_page(u32 lin, u32 phys, u32 flags);
u32 page_phys(u32 lin);                       /* physical page behind lin, 0: not mapped */
u32 linear_hole(u32 len, u32 align);          /* unmapped linear space for an emulated device (cpu.c) */
int page_user(u32 lin, int write);            /* ring 3 may access it without a fault */
void set_a20(int on);
extern int a20_on;
extern int usb_ready;
void *phys_low(u32 phys);                     /* pointer to physical memory below 1 MiB */
void *isa_dma_buffer(u32 *phys);              /* 64 KiB of RAM below 16 MiB, 64 KiB aligned (0: none) */
void idle_wait(void);                         /* sti; hlt; cli */
void reboot(void) __attribute__((noreturn));
void map_mmio(u32 phys, u32 len);
void io_allow(u16 port, u32 n);               /* ports straight to the hardware for the guest (no trap) */
void map_fb(u32 phys, u32 len);               /* write-combining (PAT) when the CPU has it */
void *map_mmio64(u64 phys, u32 len);          /* any address; NULL if it can't be mapped */
void usb_start(const char *cmdline);
void usb_shutdown(void);                      /* usb.cpp: before a reboot */
void usb_tick(void);
extern u32 kernel_stack_top;
void v86_enter(struct regs *r) __attribute__((noreturn));
extern u8 *disk_image;                        /* the RAM disk (boot module) */
extern u32 disk_size;
extern char cmdline[512];

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
int vpic_base0(void);                         /* the master PIC's vector base (IRQ 0) */
int chosen_irq_of(int vec);
void vpic_lower(int irq);                     /* take back a request not yet delivered */                   /* the IRQ behind a delivered vector, -1: none */
void vpic_ack(int vec);                       /* mark in service */
void vpic_eoi_master(void);
void vpic_eoi_irq(int irq);
u32 port_in(u16 port, int size);
void port_out(u16 port, u32 val, int size);
void vdev_tick(void);                         /* from the real timer IRQ */
void vkbd_real_scancode(u8 sc);
int vkbd_take(void);                          /* raw byte from the keyboard queue, -1 if none */               /* from the real keyboard IRQ */
void vaux_motion(int dx, int dy, int b);      /* the virtual PS/2 mouse port */
void vaux_bios_enable(int on);                /* INT 15h C2xx on that port */
void vaux_bios_reset(void);
int vaux_bios_byte(void);                     /* mouse byte from the 8042, or -1 */
void speed_init(void);                        /* speed= (slowdown) */
/* ---- fat.c: read-only FAT reader ---- */
struct fatvol {
    int (*rd)(u32 lba, u32 n, void *buf);
    u32 base, total, spc, fat_start, root_start, root_secs, data_start, root_clus, nclus, fat_cached;
    int type;
    u8 sec[512], fat[1024];
};
struct extent { u32 lba, count; };
int fat_mount(struct fatvol *v, int (*rd)(u32 lba, u32 n, void *buf), u32 base);
const char *dos_kind(struct fatvol *v);       /* disk.c: "FreeDOS", "MS-DOS", ... or NULL */
int fat_lookup(struct fatvol *v, const char *path, u32 *clus, u32 *size);
int fat_extents(struct fatvol *v, u32 clus, u32 bytes, struct extent *ext, int max);

/* ---- disk.c: drive C: (AHCI partition or RAM disk) ---- */
extern u32 disk_sectors;
void disk_init(void);
int disk_read(u32 lba, u32 n, void *buf);     /* 0 or a BIOS INT 13h status */
int disk_write(u32 lba, u32 n, const void *buf);
int disk_volume(struct fatvol *v);            /* mount C:'s FAT partition */
void disk_flush(void);                        /* gathered writes out to the disk */
void disk_tick(void);                         /* timer: flush writes older than 50 ms */
const char *disk_kind(void);
int disk_dev_count(void);                      /* every real disk, for hd.c */
u64 disk_dev_sectors(int d);
const char *disk_dev_name(int d);
const char *disk_dev_kind(int d);
u32 disk_dev_gen(int d);
int disk_dev_index(int d);
int disk_dev_rw(int d, u32 lba, u32 n, void *buf, int write);
int disk_c_dev(u32 *start, u32 *size);
extern int disk_on_a;                          /* no C:: the RAM disk is in A: and vmdos boots from it */

/* ---- joy.c: game port, USB gamepads ---- */
void joy_set(int pad, int on, int x, int y, int buttons);
int joy_present(void);
u8 joy_port_in(void);
void joy_port_out(void);
int joy_bios(struct regs *r);

/* ---- hd.c: VMHD, other disks' partitions as DOS drives ---- */
void hd_api(struct regs *r);
int hd_drive_part(int drive, int *dev, u32 *start);   /* a VMHD drive's disk and partition start: 0, or -1 */
int hd_bios_disks(void);                       /* BIOS hard disks: C:'s, then the others (once DOS is up) */
void hd_dos_up(void);
int usb_quiet(void);                           /* usb.cpp: no transfer or poll under way */
void hd_int13(struct regs *r);

void cd_add(u8 *data, u32 size, const char *name);
void ems_init(void);                          /* ems.c: LIM 4.0 expanded memory */
void ems_int67(struct regs *r);
void ems_query(struct regs *r);
int ems_present(void);   /* cd.c: a CD-ROM image */
void cd_api(struct regs *r);                  /* INT 2Fh AX=5644h */
void guest_idle_poll(void);                   /* v86.c: a "nothing to do" poll; sleeps when spinning */
int fd_int13(struct regs *r);                 /* floppy.c: INT 13h, DL < 80h */
int fd_ram(int d, u8 *image, u32 size);       /* a floppy-sized RAM disk in drive d: 0, or 3 not a floppy */
int fd_is_dos(int d);
void fd_api(struct regs *r);                  /* INT 2Fh AX=5646h (VMFD.COM) */
void speed_throttle(void);                    /* after each timer tick */
u32 speed_api(u32 v, int fine);               /* INT 2Fh AX=5653h (fine: CX = 'HP', hundredths of a percent) */
void video_osd(const char *msg);              /* a note at the top right for 2 s */
void video_vbe_window(struct regs *r);        /* VESA bank switch (4F05h / WinFuncPtr) */
void video_vbe_pm(int id, u32 ebx, u32 ecx, u32 edx, u32 pal_lin);   /* VBE PM interface call */
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
extern int xms_hidden;                         /* noxms: no XMS driver */
void xms_call(struct regs *r);
void xms_exec_a20_off(void);                  /* A20 off for a program DOS just loaded, as FreeDOS's EXEC */

/* ---- mouse.c: PS/2 and USB mice, INT 33h ---- */
void mouse_ps2_init(void);
void mouse_ps2_byte(u8 b);
void mouse_input(int dx, int dy, int buttons);
void mouse_absolute(int x, int y, int buttons);  /* a tablet: 0..32767 across the screen */
void mouse_usb_attached(void);
void mouse_update(void);
void mouse_int33(struct regs *r);
int mouse_callback_due(void);
void mouse_start_callback(struct regs *r);
u16 mouse_begin_callback(void);               /* for a client in PM: the stub's offset */
int mouse_log(void);                          /* "mouselog" on the command line */
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
int dpmi_hw_active(int vec);                 /* its PM handler is still running */
struct regs *dpmi_hw_interrupt(struct regs *r, int vec);
struct regs *dpmi_reflect_irq(struct regs *r, int vec);
void wait_for_irq(void);
void dpmi_espfix(struct regs *r);

/* ---- hv.c: VMHV, DOS in an AMD-V virtual machine ---- */
extern int hv_active;
u8 *hv_hole_page(void);
void bios_disklog_fault(u32 lin, int write);  /* disklog: a disk buffer page faulted to the guest */
u32 hv_lfb(void);                             /* VMHV: the VESA framebuffer's guest-physical address */
u32 hv_vm_top(void);                          /* VMHV: the top of the VM's RAM */
u8 *hv_gpa_ptr(u32 gpa);
int hv_guest_byte(u32 lin, u32 *gpa);          /* disklog: guest byte via its page tables, -1 unmapped */                      /* VMHV: host pointer for guest-physical memory (0: none) */                       /* VMHV: the read-only FFh page of the empty bus (0: not in VMHV) */
void video_b000_map(void);
void video_vgalog_giveup(void);              /* vgalog: dump the VGA VxD's code now (Windows quitting) */                    /* video.c: B0000h-B7FFFh as the VGA decodes it */
int hv_guest_pm(u16 *cs, u32 *eip);
u32 hv_guest_cr0(void);                       /* the VM's CR0 (0 outside VMHV) */
void hv_dump_here(const char *why);
void hv_dump_lin(u32 a, u32 len);           /* VMHV: code and stack at the guest's CS:EIP, for vgalog */           /* VMHV: the guest is in protected mode, at CS:EIP */
int hv_start(int key);                        /* only returns on failure: an error code */
void hv_hotkey(void);                         /* Ctrl+Alt+End */
u32 hv_ext_kb(void);                          /* the VM's extended memory (0: no VM) */
int hv_e820(struct regs *r);
int v86_wrap_string(struct regs *r);           /* v86.c: 8086 segment wrap of a string instruction */
int hv_emu_check(u32 lin, int write);          /* 0 unmapped, 1 memory, 2 the trapped VGA window */
u8 hv_emu_rd8(u32 lin);                       /* mememu.c in VMHV: guest linear memory */
int hv_emu_gpa(u32 lin, u32 *gpa);            /* VMHV: guest linear to guest physical, 0 if mapped */
void hv_emu_wr8(u32 lin, u8 v);

/* ---- fs9p.cpp: host folders over virtio-9p as DOS drives (VMDOSNET.COM) ---- */
void fs9p_api(struct regs *r);                /* INT 2Fh AX=5652h */
int fs9p_redir(struct regs *r);               /* INT 2Fh AH=11h: 1 handled (ours) */
int fs9p_lfn(struct regs *r);                 /* INT 21h AH=71h: 1 handled */
void fs9p_lfn_done(struct regs *r);           /* BIOS trap 5Ah: after DOS's 6C00h for a 716Ch */
void fs9p_flush_all(void);                    /* gathered writes out */
int fs9p_kopen(const char *path, u32 *size);  /* "X:\\FILE" on a share, for reading: a handle, or -2 not found, -4 not a share */
int fs9p_kread(int h, u32 off, u32 len, void *buf);   /* 0, or -1 */
void fs9p_kclose(int h);
void fs9p_idle(void);                         /* DOS idle: gathered writes out */

/* ---- net.cpp: the PC's Ethernet card for VMPKT.COM (INT 2Fh AX=564Eh) ---- */
extern int net_ready;
void net_init(void);
void net_tick(void);                          /* every timer tick: poll the card */
void net_api(struct regs *r);
void info_api(struct regs *r);                /* INT 2Fh AX=5649h: LSPCI.COM / LSUSB.COM (sysinfo.cpp) */
int ne2k_port(u16 port, int size, int write, u32 *val);   /* ne2k.c: the emulated ISA NE2000 */

/* ---- smp.c: a second CPU does the screen and the sound ---- */
struct rlock { volatile u32 held; volatile int owner; int depth; u32 fl; };   /* the CPU holding it may take it again */
void rlock_take(struct rlock *l);
int rlock_try(struct rlock *l);
void rlock_drop(struct rlock *l);
extern struct rlock video_lock;               /* video.c's state: the helper's redraw against mode and register changes */
extern struct rlock sound_lock;               /* the Sound Blaster core, the CD audio ring */
extern struct rlock log_lock;                 /* kprintf */
extern volatile int smp_active;               /* the helper CPU redraws and renders (not the timer tick) */
int smp_on_ap(void);                          /* running on the helper (or a render helper) */
int smp_cpu(void);                            /* 0 boot CPU, 1 helper, 2.. render helpers */
#define SMP_SLOTS 4                           /* the helper and up to three render helpers */
int smp_slot(void);                           /* 0..SMP_SLOTS-1: whose per-CPU scratch to use */
int par_for(u32 n, u32 chunk, void (*fn)(u32 i, void *arg), void *arg);   /* the helper's rows, shared; 0: some undrawn */
void smp_start(u32 tramp);                    /* tramp: a free physical page below 1 MiB */
void smp_go(void);
void smp_park(void);                          /* VMHV: everything back on the boot CPU, for good */
void smp_ap_fault(struct regs *r) __attribute__((noreturn));
/* The boot CPU's local APIC timer, one-shot, for the guest's PIT channel 0
   above the 1 kHz tick (vdev.c). Vector FAST_TIMER_VEC (boot.S). */
#define FAST_TIMER_VEC 0x31
void fast_timer_arm(u32 pit_clocks);          /* interrupt in that many PIT clocks; 0: off */
void fast_timer_eoi(void);
void fast_timer_tick(void);                   /* each host tick: its calibration */
int fast_timer_ok(void);                      /* calibrated and usable */
void vdev_fast_tick(void);                    /* vdev.c: from the APIC timer */

/* ---- sound.c ---- */
void sound_init(void);
void sound_tick(void);                        /* every timer tick (on the helper CPU if there is one) */
void sound_irq_poll(void);                    /* every timer tick when the helper renders: the SB's IRQ */
int sound_port(u16 port, int write, u8 *v);   /* 1 if it's a sound port */

/* ---- video.c ---- */
void video_init(void);
void video_shot_api(struct regs *r);           /* INT 2Fh AX=5650h: VMSHOT */
void video_refresh(void);                     /* called at ~60 Hz */
void video_refresh_late(void);                /* the helper CPU, after video_refresh: scale=xbrz's pass */
void video_set_mode(int mode, int clear);
u32 video_port_in(u16 port);
void video_port_out(u16 port, u8 v);
void video_console(const char *msg);          /* panic screen */
void video_console_attr(const char *title, const char *msg, u16 attr);
void video_console_save(void);
void video_console_restore(void);
/* ---- bootcon.c: the log on screen while vmdos starts ---- */
extern int bootcon_on;
int bootcon_start(u64 addr, u32 pitch, u32 w, u32 h, u32 bpp, u8 rpos, u8 gpos, u8 bpos);
void log_to_bootcon(void);
void bootcon_remap(u32 lin);
void bootcon_stop(void);
void bootcon_dos_started(void);               /* the log screen (video=vbe on another card): new title, black till Ctrl+Shift+F10 */
void bootcon_show(int on);                    /* the log screen: Ctrl+Shift+F10 shows it (1) or blacks it out (0) */
u64 boot_fb_phys(void);                       /* cpu.c: the boot framebuffer's physical address, 0: none */
void bootcon_putc(char c);
int video_fb_ready(void);                      /* video.c has a framebuffer to draw on */
extern volatile int log_view_req;
extern int video_mode;
extern u8 vga_dac[256][3];
extern u8 crtc[64];
extern int palette_dirty;
void video_framebuffer(u64 addr, u32 pitch, u32 w, u32 h, u32 bpp,
                       u8 rpos, u8 rsz, u8 gpos, u8 gsz, u8 bpos, u8 bsz);
void video_text_fallback(void);
int video_int10(struct regs *r);              /* 1: continue the INT 10h stub (register writes to replay) */
/* mememu.c: emulate the instruction that touched trapped video memory */
struct emu_cpu {
    u32 *gpr[8];                              /* eax ecx edx ebx esp ebp esi edi */
    u32 *eip, *eflags;
    u32 seg_base[6];                          /* es cs ss ds fs gs */
    int code32;                               /* 32-bit code segment */
};
int mem_emulate(struct emu_cpu *e);
int mem_emulate_more(struct emu_cpu *e);         /* go on emulating a window loop (VMHV) */
int vga16_window(u32 lin);                    /* video.c: 16-color planar window */
u8 vga16_read(u32 lin);
void vga16_write(u32 lin, u8 v);
int vga16_fault(struct emu_cpu *e, u32 cr2);  /* 1: emulated */
int video_gfx_height(void);                   /* graphics mode height (200 in text modes) */
void video_puts(const char *s);
extern const u8 *vga_font16, *vga_font8;

/* ---- vgabios.c: video=vbe, the video card's own VGA/VESA BIOS (legacy BIOS / CSM boots) ---- */
extern int vga_native;                        /* the guest has the real card: its ROM, ports, memory */
void vga_native_init(void);                   /* video_init: decide, find the card, give the guest its ports */
void vga_native_bios(void);                   /* bios_init: the ROM at C000h, its vectors and BIOS data */
u16 vga_native_rom_end(void);                 /* the paragraph after the card's ROM (UMBs start there) */
u32 vga_native_lfb(u32 phys, u32 len);        /* [phys, +len) in the card's memory: the bytes to map (DPMI 0800h), else 0 */
void vga_native_rom_module(u8 *p, u32 len);   /* vmdos.efi's vgarom module: the card's legacy BIOS (video=vbe on UEFI) */
int vga_native_logscreen(void);                /* video=vbe: the boot log stays live on another card's framebuffer */
int vga_native_post(void);                    /* display=: AX for the chosen card's BIOS init (bus, devfn), -1: none
                                              (the PC's BIOS started the card) */
int vga_native_pci(int bus, int devfn);       /* 1: the card's own PCI function (its config writes go through) */
void video_native_sync(void);                 /* video.c: video_mode from the BIOS data area */
void video_native_pointer(void);              /* video.c: the INT 33h text-mode pointer, drawn in video memory */
#include "voodoo3.h"
