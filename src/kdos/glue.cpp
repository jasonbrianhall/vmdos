/* kdos glue: runs DOSBox-X's DOS kernel and shell (the other files in
   src/kdos, taken from DOSBox-X) inside vmdos's monitor.

   DOSBox-X emulates the CPU; vmdos runs the guest natively in V86 mode.
   The two meet at DOSBox-X's callbacks: guest code (INT 21h's handler,
   the shell's INT 2Eh, the "stop" after a nested INT ...) holds the
   opcode FE 38 nn nn, which faults (#UD) under V86, and kdos_host.c
   sends the fault here. kdos_trap copies the guest's registers into
   DOSBox-X's cpu_regs/Segs, runs handler nn and copies them back.

   DOSBox-X's code also runs guest code from C++ and waits for it to come
   back: CALLBACK_RunRealInt (INT 16h for a key, INT 21h AH=4Bh from the
   shell, ...) ends in DOSBOX_RunMachine(), which in DOSBox-X runs the
   emulated CPU until a callback answers CBRET_STOP. Here every handler
   runs on a coroutine of its own (a stack from a small pool); RunMachine
   puts the running coroutine on a wait stack and switches back to the
   trap, which returns to the guest. When the guest reaches a callback
   whose handler answers CBRET_STOP, the trap resumes the coroutine on top
   of the wait stack, inside its RunMachine, and it carries on. Waits nest
   as DOSBox-X's RunMachine calls nest. */
#include "dosbox.h"
#include "logging.h"
#include "regs.h"
#include "callback.h"
#include "mem.h"
#include "setup.h"
#include "control.h"
#include "dos_inc.h"
#include "drives.h"
#include "bios_disk.h"
#include "shell.h"
#include "ints/int10.h"
#include <map>

extern "C" {
void kh_log(const char *s);
void kh_panic(const char *s) __attribute__((noreturn));
void *kh_alloc(uint32_t bytes);
uint32_t kh_ticks(void);
int kh_vif_get(void);
void kh_vif_set(int v);
int kh_a20(void);
void kh_set_a20(int on);
const char *kh_cmdline(void);
int kh_disk_read(uint32_t lba, uint32_t n, void *buf);
int kh_disk_write(uint32_t lba, uint32_t n, const void *buf);
uint32_t kh_disk_sectors(void);
int kh_disk_part(uint32_t *base, uint32_t *count);
void kh_int10(uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx, uint16_t es, uint32_t *ebp);
}

/* the monitor's interrupt frame (kernel.h struct regs) */
struct kregs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_k, ebx, edx, ecx, eax;
    uint32_t vec, err;
    uint32_t eip, cs, eflags;
    uint32_t esp, ss;
    uint32_t v86_es, v86_ds, v86_fs, v86_gs;
};

/* ---------------- DOSBox-X's CPU state, as its DOS code sees it ---------------- */
CPU_Regs cpu_regs;
Segments Segs;
CPUBlock_kdos cpu;
HostPt MemBase = nullptr;
size_t MemSize = 0x110000;
MachineType machine = MCH_VGA;
SVGACards svgaCard = SVGA_None;
bool mono_cga = false;
Config *control;

#define USER_FLAGS (FLAG_CF | FLAG_PF | FLAG_AF | FLAG_ZF | FLAG_SF | FLAG_OF | FLAG_DF | FLAG_TF)

static void load_regs(const kregs *r)
{
    reg_eax = r->eax; reg_ebx = r->ebx; reg_ecx = r->ecx; reg_edx = r->edx;
    reg_esi = r->esi; reg_edi = r->edi; reg_ebp = r->ebp; reg_esp = r->esp & 0xFFFF;
    reg_eip = r->eip & 0xFFFF;
    reg_flags = (r->eflags & USER_FLAGS) | 2 | (kh_vif_get() ? FLAG_IF : 0);
    SegSet16(cs, (uint16_t)r->cs); SegSet16(ss, (uint16_t)r->ss);
    SegSet16(ds, (uint16_t)r->v86_ds); SegSet16(es, (uint16_t)r->v86_es);
    SegSet16(fs, (uint16_t)r->v86_fs); SegSet16(gs, (uint16_t)r->v86_gs);
}
static void store_regs(kregs *r)
{
    r->eax = reg_eax; r->ebx = reg_ebx; r->ecx = reg_ecx; r->edx = reg_edx;
    r->esi = reg_esi; r->edi = reg_edi; r->ebp = reg_ebp;
    r->esp = reg_esp & 0xFFFF; r->eip = reg_eip & 0xFFFF;
    r->eflags = (r->eflags & ~USER_FLAGS) | (reg_flags & USER_FLAGS);
    kh_vif_set((reg_flags & FLAG_IF) != 0);
    r->cs = SegValue(cs); r->ss = SegValue(ss);
    r->v86_ds = SegValue(ds); r->v86_es = SegValue(es);
    r->v86_fs = SegValue(fs); r->v86_gs = SegValue(gs);
}

/* ---------------- coroutines ---------------- */
extern "C" void kco_switch(uint32_t *save_sp, uint32_t new_sp);
__asm__(".text\n.globl kco_switch\nkco_switch:\n"
        "  push %ebp\n  push %ebx\n  push %esi\n  push %edi\n"
        "  mov 20(%esp),%eax\n  mov %esp,(%eax)\n"
        "  mov 24(%esp),%esp\n"
        "  pop %edi\n  pop %esi\n  pop %ebx\n  pop %ebp\n  ret\n");

#define CO_STACK (128u << 10)
#define CO_MAX 24
struct kco {
    uint32_t sp;
    uint8_t *stack;
    CallBack_Handler fn;
    void (*vfn)(void);
    Bitu ret;
    int done, used;
};
static kco cos[CO_MAX];
static kco *cur;                       /* the coroutine running, NULL: the trap itself */
static uint32_t trap_sp;               /* the trap's stack while a coroutine runs */
static kco *waiting[CO_MAX];           /* in DOSBOX_RunMachine, innermost last */
static int nwait;

static void co_entry(void)
{
    kco *c = cur;
    if (c->vfn) { c->vfn(); c->ret = CBRET_STOP; }
    else c->ret = c->fn();
    c->done = 1;
    kco_switch(&c->sp, trap_sp);       /* never comes back */
    kh_panic("finished coroutine resumed");
}
static kco *co_new(CallBack_Handler fn, void (*vfn)(void))
{
    for (int i = 0; i < CO_MAX; i++) {
        kco *c = &cos[i];
        if (c->used) continue;
        if (!c->stack) c->stack = (uint8_t *)kh_alloc(CO_STACK);
        c->used = 1; c->done = 0; c->fn = fn; c->vfn = vfn; c->ret = CBRET_NONE;
        uint32_t *sp = (uint32_t *)(c->stack + CO_STACK);
        *--sp = 0;                             /* co_entry's return address: none */
        *--sp = (uint32_t)(uintptr_t)co_entry; /* kco_switch's ret */
        *--sp = 0; *--sp = 0; *--sp = 0; *--sp = 0;   /* ebp ebx esi edi */
        c->sp = (uint32_t)(uintptr_t)sp;
        return c;
    }
    kh_panic("out of DOS coroutines (calls nested too deep)");
}
static void co_resume(kco *c)
{
    kco *prev = cur;
    cur = c;
    kco_switch(&trap_sp, c->sp);
    cur = prev;
}

/* DOSBox-X: run the CPU until a callback stops it. Here: give the guest
   the CPU (back to the trap, which returns to V86) until a stop. */
void DOSBOX_RunMachine(void)
{
    kco *c = cur;
    if (!c) kh_panic("RunMachine outside a DOS call");
    if (nwait >= CO_MAX) kh_panic("RunMachine nested too deep");
    waiting[nwait++] = c;
    kco_switch(&c->sp, trap_sp);       /* resumed by the trap that saw the stop (trap_sp is its stack now) */
}

/* a coroutine ended with CBRET_STOP: resume the innermost RunMachine,
   and on, while those end with a stop too */
static void finish(kco *c)
{
    while (c && c->done) {
        Bitu ret = c->ret;
        c->used = 0;
        c = nullptr;
        if (ret == CBRET_STOP) {
            if (!nwait) { kh_log("kdos: stop with nothing waiting\n"); return; }
            kco *w = waiting[--nwait];
            co_resume(w);
            c = w;
        }
    }
}

extern CallBack_Handler CallBack_Handlers[];
extern "C" void kdos_floppy_check(void);
static uint8_t fpu_guest[108];

extern "C" kregs *kdos_trap(kregs *r)
{
    __asm__ volatile("fnsave %0\n\tfninit" : "=m"(fpu_guest));
    uint32_t a = (r->cs << 4) + (r->eip & 0xFFFF);
    uint16_t nr = mem_readw(a + 2);
    load_regs(r);
    reg_eip = (reg_eip + 4) & 0xFFFF;                       /* past FE 38 nn nn, as the CPU would be */
    if (nr >= CB_MAX || !CallBack_Handlers[nr]) {
        char b[80]; snprintf(b, sizeof b, "callback %u at %04x:%04x has no handler", nr, (unsigned)r->cs, (unsigned)(r->eip & 0xFFFF));
        kh_panic(b);
    }
    kdos_floppy_check();                  /* VMFD changed a floppy: its drive again (start.cpp) */
    kco *c = co_new(CallBack_Handlers[nr], nullptr);
    co_resume(c);
    finish(c);
    store_regs(r);
    __asm__ volatile("frstor %0" : : "m"(fpu_guest));
    return r;
}

/* ---------------- configuration: [dos], [dosbox], [config] ---------------- */
static const struct { const char *name, *value; } cfg_table[] = {
    { "ver", "7.10" }, { "lfn", "true" }, { "xms", "false" }, { "ems", "false" }, { "umb", "false" },
    { "files", "127" }, { "fcbs", "100" }, { "lastdrive", "z" }, { "break", "off" },
    { "hma free space", "34560" }, { "dos sda size", "0" }, { "kernel allocation in umb", "false" },
    { "private area in umb", "false" }, { "private area write protect", "false" },
    { "keep private area on boot", "auto" }, { "minimum dos initial private segment", "0" },
    { "minimum mcb segment", "0" }, { "minimum mcb free", "0" }, { "enable dummy device mcb", "true" },
    { "mcb corruption becomes application free memory", "true" },
    { "resized free memory block becomes allocated", "true" },
    { "shellhigh", "false" }, { "shell permanent", "false" }, { "startbanner", "false" },
    { "automount", "false" }, { "automountall", "false" }, { "mountwarning", "false" },
    { "dos clipboard api", "false" }, { "dos clipboard device enable", "false" }, { "dos clipboard device name", "CLIP$" },
    { "network redirector", "false" }, { "int 13 extensions", "true" }, { "int 13 disk change detect", "true" },
    { "keyboardlayout", "none" }, { "country", "" }, { "customcodepage", "" },
    { "con device use int 16h to detect keyboard input", "true" },
    { "zero memory on int 21h memory allocation", "false" }, { "exepack", "auto" },
    { "drive z expand path", "true" }, { "hidenonrepresentable", "false" }, { "utf8 file names", "false" },
    { "file access tries", "0" }, { "pipe temporary device", "true" }, { "special operation file prefix", "" },
    { "filenamechar", "true" }, { "collating and uppercase", "true" }, { "dbcs", "false" },
    { "ext dev read limit", "0" }, { "ext dev write limit", "0" },
    { "additional environment block size on exec", "-1" }, { "maximum environment block size on exec", "-1" },
    { "quick reboot", "false" }, { "share", "true" }, { "rem", "true" }, { "dos idle api", "true" },
    { "fat32setversion", "auto" }, { "autofixwarning", "false" }, { "autoa20fix", "true" }, { "autoloadfix", "true" },
    { "startcmd", "false" }, { "startincon", "" }, { "startwait", "false" }, { "startquiet", "false" },
    { "startnopause", "false" }, { "starttranspath", "false" }, { "cpm compatibility mode", "auto" },
    { "command shell flush keyboard buffer", "true" }, { "shell configuration as commands", "false" },
    { "turn off a20 gate on load if loadfix needed", "true" }, { "unmask timer on disk io", "false" },
    { "int15 wait force unmask irq", "true" }, { "floppy drive data rate limit", "0" },
    { "hard drive data rate limit", "0" },   /* 0: no emulated disk speed (vmdos disks are real) */ { "break on int3", "false" }, { "badcommandhandler", "" },
    { "shell", "" }, { "dos", "true" }, { "output", "surface" }, { "font", "" }, { "colors", "" },
    { "ansi.sys", "true" }, { "bannercolortheme", "default" }, { "numlock", "" }, { "cols", "0" }, { "lins", "0" },
};
static const char *cfg_find(std::string const &p)
{
    for (auto &e : cfg_table) if (p == e.name) return e.value;
    return nullptr;
}
std::string Section_prop::GetPropValue(std::string const &p) const { const char *v = cfg_find(p); return v ? v : NO_SUCH_PROPERTY; }
const char *Section_prop::Get_string(std::string const &p) const { const char *v = cfg_find(p); return v ? v : ""; }
bool Section_prop::Get_bool(std::string const &p) const
{
    const char *v = cfg_find(p);
    return v && (!strcasecmp(v, "true") || !strcmp(v, "1") || !strcasecmp(v, "on") || !strcasecmp(v, "yes"));
}
int Section_prop::Get_int(std::string const &p) const { const char *v = cfg_find(p); return v ? atoi(v) : 0; }
Hex Section_prop::Get_hex(std::string const &p) const { const char *v = cfg_find(p); return Hex(v ? (int)strtol(v, nullptr, 16) : 0); }
double Section_prop::Get_double(std::string const &p) const { const char *v = cfg_find(p); return v ? atoi(v) : 0; }
Section *Config::GetSection(std::string const &name) const
{
    static std::map<std::string, Section_prop *> secs;
    auto it = secs.find(name);
    if (it != secs.end()) return it->second;
    Section_prop *s = new Section_prop(name.c_str());
    secs[name] = s;
    return s;
}

/* ---------------- messages, logging, exits ---------------- */
static std::map<std::string, std::string> *msgs;
void MSG_Add(const char *name, const char *text)
{
    if (!msgs) msgs = new std::map<std::string, std::string>;
    if (msgs->find(name) == msgs->end()) (*msgs)[name] = text;
}
void MSG_Replace(const char *name, const char *text)
{
    if (!msgs) msgs = new std::map<std::string, std::string>;
    (*msgs)[name] = text;
}
/* every message: DOSBox-X's name for this shell becomes vmdos's */
void MSG_Rename(const char *from, const char *to)
{
    if (!msgs) return;
    for (auto &m : *msgs)
        for (size_t at; (at = m.second.find(from)) != std::string::npos; ) m.second.replace(at, strlen(from), to);
}
const char *MSG_Get(char const *name)
{
    if (msgs) { auto it = msgs->find(name); if (it != msgs->end()) return it->second.c_str(); }
    return name;
}
void MSG_Init(void) {}
std::string formatString(const char *format, ...)
{
    char b[1024]; va_list a; va_start(a, format); vsnprintf(b, sizeof b, format, a); va_end(a);
    return b;
}
void kdos_log(const char *fmt, ...)
{
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b - 2, fmt, a); va_end(a);
    size_t n = strlen(b);
    if (!n || b[n - 1] != '\n') { b[n] = '\n'; b[n + 1] = 0; }
    kh_log("kdos: "); kh_log(b);
}
void E_Exit(const char *fmt, ...)
{
    char b[512]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    kh_panic(b);
}


/* ---------------- INT 10h: vmdos's video BIOS ---------------- */
static void int10(uint16_t ax, uint16_t bx, uint16_t cx, uint16_t dx, uint16_t *oax = nullptr, uint16_t *obx = nullptr,
                  uint16_t *ocx = nullptr, uint16_t *odx = nullptr)
{
    uint32_t a = ax, b = bx, c = cx, d = dx, bp = 0;
    kh_int10(&a, &b, &c, &d, 0, &bp);
    if (oax) *oax = (uint16_t)a;
    if (obx) *obx = (uint16_t)b;
    if (ocx) *ocx = (uint16_t)c;
    if (odx) *odx = (uint16_t)d;
}
static uint8_t cur_page(void) { return real_readb(BIOSMEM_SEG, BIOSMEM_CURRENT_PAGE); }
void INT10_TeletypeOutputAttr(uint8_t chr, uint8_t attr, bool useattr)
{
    if (useattr && chr >= 32) int10(0x0900 | chr, (uint16_t)(cur_page() << 8 | attr), 1, 0);
    int10(0x0E00 | chr, (uint16_t)(cur_page() << 8 | attr), 0, 0);
}
void INT10_TeletypeOutput(uint8_t chr, uint8_t attr) { int10(0x0E00 | chr, (uint16_t)(cur_page() << 8 | attr), 0, 0); }
void INT10_SetCursorPos(uint8_t row, uint8_t col, uint8_t page) { int10(0x0200, (uint16_t)(page << 8), 0, (uint16_t)(row << 8 | col)); }
void INT10_SetCursorPos_viaRealInt(uint8_t row, uint8_t col, uint8_t page) { INT10_SetCursorPos(row, col, page); }
void INT10_GetCursorPos(uint8_t *row, uint8_t *col, uint8_t page) { *col = CURSOR_POS_COL(page); *row = CURSOR_POS_ROW(page); }
void INT10_ScrollWindow(uint8_t rul, uint8_t cul, uint8_t rlr, uint8_t clr, int8_t nlines, uint8_t attr, uint8_t page)
{
    (void)page;
    /* DOSBox-X counts lines scrolled up as negative (its INT 10h AH=06h passes -AL) */
    uint16_t n = (uint16_t)(nlines < 0 ? -nlines : nlines);
    int10((uint16_t)((nlines < 0 ? 0x0600 : 0x0700) | (n & 0xFF)), (uint16_t)(attr << 8), (uint16_t)(rul << 8 | cul), (uint16_t)(rlr << 8 | clr));
}
void INT10_ScrollWindow_viaRealInt(uint8_t rul, uint8_t cul, uint8_t rlr, uint8_t clr, int8_t nlines, uint8_t attr, uint8_t page)
{ INT10_ScrollWindow(rul, cul, rlr, clr, nlines, attr, page); }
void INT10_WriteChar(uint16_t chr, uint8_t attr, uint8_t page, uint16_t count, bool showattr)
{ int10((uint16_t)((showattr ? 0x0900 : 0x0A00) | (chr & 0xFF)), (uint16_t)(page << 8 | attr), count, 0); }
void INT10_WriteChar_viaRealInt(uint8_t chr, uint8_t attr, uint8_t page, uint16_t count, bool showattr)
{ INT10_WriteChar(chr, attr, page, count, showattr); }
void INT10_GetScreenColumns(uint16_t *cols) { *cols = real_readw(BIOSMEM_SEG, BIOSMEM_NB_COLS); }
bool INT10_SetVideoMode(uint16_t mode) { int10((uint16_t)(mode & 0xFF), 0, 0, 0); return true; }
bool INT10_SetCurMode(void) { return true; }
bool INT10_GetInsertState(void) { return (mem_readb(0x417) & 0x80) != 0; }
void INT10_SetCursorShape(uint8_t first, uint8_t last) { int10(0x0100, 0, (uint16_t)(first << 8 | last), 0); }
void INT10_ReadCharAttr(uint16_t *result, uint8_t page)
{
    uint16_t ax;
    int10(0x0800, (uint16_t)(page << 8), 0, 0, &ax);
    *result = ax;
}
void ReadCharAttr(uint16_t col, uint16_t row, uint8_t page, uint16_t *result)
{
    uint16_t cols = real_readw(BIOSMEM_SEG, BIOSMEM_NB_COLS);
    uint32_t a = 0xB8000 + page * real_readw(BIOSMEM_SEG, BIOSMEM_PAGE_SIZE) + (row * cols + col) * 2u;
    *result = mem_readw(a);
}
void WriteChar(uint16_t col, uint16_t row, uint8_t page, uint16_t chr, uint8_t attr, bool useattr)
{
    uint16_t cols = real_readw(BIOSMEM_SEG, BIOSMEM_NB_COLS);
    uint32_t a = 0xB8000 + page * real_readw(BIOSMEM_SEG, BIOSMEM_PAGE_SIZE) + (row * cols + col) * 2u;
    mem_writeb(a, (uint8_t)chr);
    if (useattr) mem_writeb(a + 1, attr);
}
static VideoModeBlock text_mode = { 3, M_TEXT, 720, 400, 80, 25, 9, 16, 8, 0xB8000, 0x1000, 100, 449, 80, 400, 0, 0 };
VideoModeBlock *CurMode = &text_mode;

/* ---------------- memory ---------------- */
void MEM_BlockRead(LinearPt pt, void *data, Bitu size) { memcpy(data, (void *)(uintptr_t)pt, size); }
void MEM_BlockWrite(LinearPt pt, const void *data, size_t size) { memcpy((void *)(uintptr_t)pt, data, size); }
void MEM_BlockCopy(LinearPt dest, LinearPt src, Bitu size) { memmove((void *)(uintptr_t)dest, (void *)(uintptr_t)src, size); }
void MEM_StrCopy(LinearPt pt, char *data, Bitu size)
{
    while (size--) { char c = (char)mem_readb(pt++); if (!c) break; *data++ = c; }
    *data = 0;
}
Bitu mem_strlen(LinearPt pt) { Bitu n = 0; while (mem_readb(pt + n)) n++; return n; }
void mem_strcpy(LinearPt d, LinearPt s) { while (uint8_t c = mem_readb(s++)) mem_writeb(d++, c); mem_writeb(d, 0); }
void mem_memcpy(LinearPt d, LinearPt s, Bitu n) { MEM_BlockCopy(d, s, n); }
void phys_writes(PhysPt addr, const char *s, Bitu len) { MEM_BlockWrite(addr, s, len); }
HostPt GetMemBase(void) { return nullptr; }
bool MEM_A20_Enabled(void) { return kh_a20() != 0; }
void MEM_A20_Enable(bool on) { kh_set_a20(on); }
Bitu MEM_ConventionalPages(void) { return 0xA0; }
Bitu MEM_TotalPages(void) { return 0x110; }
Bitu MEM_PageMask(void) { return 0xFFFFF; }
bool MEM_map_RAM_physmem(Bitu, Bitu) { return true; }
bool MEM_map_ROM_physmem(Bitu, Bitu) { return true; }
bool MEM_unmap_physmem(Bitu, Bitu) { return true; }

/* the ROM BIOS area DOSBox-X's callbacks and tables live in: F000:8000-BFFF
   (vmdos's BIOS stubs end below F000:1000, its VBE data is at FC00:0000) */
static Bitu rom_next = 0xF8000;
Bitu ROMBIOS_GetMemory(Bitu bytes, const char *who, Bitu alignment, Bitu must_be_at)
{
    (void)who;
    if (must_be_at) return 0;
    if (alignment > 1) rom_next = (rom_next + alignment - 1) & ~(alignment - 1);
    Bitu a = rom_next;
    rom_next += bytes;
    if (rom_next > 0xFC000) E_Exit("kdos: out of ROM BIOS space");
    return a;
}

/* ---------------- the rest of the machine DOSBox-X's DOS reaches for ---------------- */
Bitu CPU_AutoDetermineMode;
int32_t CPU_Cycles, CPU_CycleLeft, CPU_CycleMax = 3000, CPU_OldCycleMax;
bool CPU_CycleAutoAdjust;
void CPU_STI(void) { SETFLAGBIT(IF, true); }
void CPU_HLT(uint32_t) { CALLBACK_Idle(); }
void CPU_Push16(uint16_t v) { reg_sp -= 2; real_writew(SegValue(ss), reg_sp, v); }
uint16_t CPU_Pop16(void) { uint16_t v = real_readw(SegValue(ss), reg_sp); reg_sp += 2; return v; }
void CPU_SetSegGeneral(int seg, uint16_t v) { SegSet16((Bitu)seg, v); }
Bitu FillFlags(void) { return reg_flags; }
void IO_Write(Bitu port, uint8_t v) { __asm__ volatile("outb %0,%w1" ::"a"(v), "Nd"((uint16_t)port)); }
void IO_WriteB(Bitu port, uint8_t v) { IO_Write(port, v); }
uint8_t IO_Read(Bitu port) { uint8_t v; __asm__ volatile("inb %w1,%0" : "=a"(v) : "Nd"((uint16_t)port)); return v; }
double PIC_FullIndex(void) { return (double)kh_ticks(); }
void PIC_SetIRQMask(Bitu, bool) {}
uint32_t GetTicks(void) { return kh_ticks(); }
void SDL_Delay(uint32_t ms) { uint32_t t = kh_ticks(); while (kh_ticks() - t < ms) CALLBACK_Idle(); }
void GFX_SetTitle(int32_t, int32_t, int32_t, bool) {}
CParallel_kdos *parallelPortObjects[9];
CSerial_kdos *serialports[4];
DOSBoxMenu mainMenu;
uint16_t guest_msdos_mcb_chain;
uint32_t guest_msdos_LoL, guest_msdos_dev_chain;
int boothax;
Bitu CPU_ArchitectureType = CPU_ARCHTYPE_486;

/* DOSBox-X waits by calling CALLBACK_Idle a number of times, each a few
   emulated instructions there; here each waits for an interrupt (up to a
   55 ms timer tick). A wait in time instead. */
void kdos_idle_ms(uint32_t ms)
{
    uint32_t t = kh_ticks();
    while (kh_ticks() - t < ms) CALLBACK_Idle();
}
