/* kdos: declarations DOSBox-X's DOS and shell use from parts of DOSBox-X
   that vmdos doesn't have (or has its own way); defined in glue.cpp */
#pragma once
#include "dosbox.h"
#include "mem.h"
#define WRITE_POD(a, b) do{}while(0)
#define READ_POD(a, b) do{}while(0)
extern uint16_t guest_msdos_mcb_chain;
/* cpu.h */
#define CPU_AUTODETERMINE_NONE 0x00
#define CPU_AUTODETERMINE_CORE 0x01
#define CPU_AUTODETERMINE_CYCLES 0x02
#define CPU_AUTODETERMINE_SHIFT 0x02
#define CPU_AUTODETERMINE_MASK 0x03
extern Bitu CPU_AutoDetermineMode;
extern int32_t CPU_Cycles, CPU_CycleLeft, CPU_CycleMax, CPU_OldCycleMax;
extern bool CPU_CycleAutoAdjust;
struct CPUBlock_kdos { struct { bool big; } code, stack; Bitu pmode; Bitu cpl; };
#define CPU_ARCHTYPE_8086 0x05
#define CPU_ARCHTYPE_80186 0x15
#define CPU_ARCHTYPE_286 0x25
#define CPU_ARCHTYPE_386 0x35
#define CPU_ARCHTYPE_486 0x45
extern Bitu CPU_ArchitectureType;
extern CPUBlock_kdos cpu;
void CPU_Push16(uint16_t value);
uint16_t CPU_Pop16(void);
void CPU_SetSegGeneral(int seg, uint16_t value);
void CPU_STI(void);
void CPU_HLT(uint32_t oldeip);
/* timer, pic, io */
#define PIT_TICK_RATE 1193182u
void IO_Write(Bitu port, uint8_t val);
uint8_t IO_Read(Bitu port);
void IO_WriteB(Bitu port, uint8_t val);
double PIC_FullIndex(void);
typedef double pic_tickindex_t;
uint32_t GetTicks(void);
/* printers: none */
struct CParallel_kdos { bool Putchar(uint8_t) { return false; } };
extern CParallel_kdos *parallelPortObjects[9];
/* DBCS: never (no PC-98, no DOS/V) */
static inline bool isKanji1(uint8_t) { return false; }
static inline bool isKanji2(uint8_t) { return false; }
static inline bool isKanji1_gbk(uint8_t) { return false; }
static inline bool isJEGAEnabled(void) { return false; }
static inline bool J3_IsJapanese(void) { return false; }
/* IDE: vmdos has its own ATAPI emulation (atapi.c), none of DOSBox-X's */
class imageDisk;
static inline void IDE_CDROM_Attach(signed char, bool, unsigned char) {}
static inline bool IDE_CDROM_Detach_Ret(signed char &, bool &, char) { return false; }
static inline void IDE_CDROM_Detach(char) {}
static inline void IDE_Hard_Disk_Detach(unsigned char) {}
#define MAX_IDE_CONTROLLERS 0
#define WRITE_POD_SIZE(a, b) do{}while(0)
#define READ_POD_SIZE(a, b) do{}while(0)
/* DOSBox-X's GUI menu: there is none; calls on it do nothing */
class DOSBoxMenu {
public:
    class item {
    public:
        item &check(bool) { return *this; }
        item &enable(bool) { return *this; }
        item &hide(bool) { return *this; }
        item &set_text(const std::string &) { return *this; }
        std::string get_text() const { return ""; }
        item &refresh_item(DOSBoxMenu &) { return *this; }
        bool is_checked() const { return false; }
    };
    item &get_item(const std::string &) { static item i; return i; }
    void unbuild() {} void rebuild() {}
};
extern DOSBoxMenu mainMenu;
/* serial ports: DOS's AUX device has none here */
struct CSerial_kdos {
    bool Getchar(uint8_t *, uint8_t *, bool, uint32_t) { return false; }
    bool Putchar(uint8_t, bool, bool, uint32_t) { return false; }
};
extern CSerial_kdos *serialports[4];
/* DOS/V, AX, J-3100: none */
#define DOSV_FEP_CTRL_IAS 0x01
#define DOSV_FEP_CTRL_MSKANJI 0x02
#define DOSV_MSKANJI_API 0
static inline uint8_t DOSV_GetFepCtrl(void) { return 0; }
static inline uint16_t DOSV_GetFontHandlerOffset(int) { return 0; }
static inline bool CheckAnotherDisplayDriver(void) { return false; }
static inline void ttf_reset(void) {}
void PIC_SetIRQMask(Bitu irq, bool masked);
static inline void mem_unalignedwrited(LinearPt a, uint32_t v) { mem_writed(a, v); }
/* DOSBox-X's modules */
class Module_base {
protected:
    Section *m_configuration;
public:
    Module_base(Section *c) : m_configuration(c) {}
    virtual ~Module_base() {}
};
#define DOSBOXMENU_NULL 0
#define DOSBOXMENU_HMENU 1
#define DOSBOXMENU_SDLDRAW 2
#define DOSBOXMENU_NSMENU 3
#define DOSBOXMENU_TYPE DOSBOXMENU_NULL
static inline void INT10_AX_SetCRTBIOSMode(Bitu) {}
static inline void INT16_AX_SetKBDBIOSMode(Bitu) {}
static inline void INT60_J3_Setup(void) {}
static inline void DOSV_Setup(void) {}
static inline void SetTrueVideoMode(Bitu) {}
static inline void INT10_DOSV_SetCRTBIOSMode(Bitu) {}
#define UPDATED_STR "vmdos kdos"
#define OS_PLATFORM_LONG "vmdos"
#define OS_BIT "32"
#define OS_BIT_INT 32
static inline std::string mapper_event_keybind_string(const std::string &) { return ""; }
static inline bool TTF_using(void) { return false; }
/* std::stringstream would bring in all of iostreams and locales; DOSBox-X's
   DOS code only splits strings with it, which this does */
namespace kdos {
class sstream {
    std::string s; size_t pos = 0; bool ok = true;
public:
    sstream() {}
    sstream(const std::string &v) : s(v) {}
    explicit operator bool() const { return ok; }
    bool getline(std::string &out, char delim) {
        if (pos >= s.size()) { ok = false; return false; }
        size_t e = s.find(delim, pos);
        if (e == std::string::npos) e = s.size();
        out = s.substr(pos, e - pos); pos = e + 1; return true;
    }
    sstream &operator>>(std::string &out) {
        while (pos < s.size() && isspace((unsigned char)s[pos])) pos++;
        if (pos >= s.size()) { ok = false; return *this; }
        size_t b = pos; while (pos < s.size() && !isspace((unsigned char)s[pos])) pos++;
        out = s.substr(b, pos - b); return *this;
    }
    std::string str() const { return s; }
};
}
static inline kdos::sstream &kdos_getline(kdos::sstream &ss, std::string &out, char d = '\n') { ss.getline(out, d); return ss; }
#define NO_SUCH_PROPERTY "PROP_NOT_EXIST"
#define MEM_PAGE_SIZE 4096
#define GIT_COMMIT_HASH ""
static inline bool DOSV_CheckCJKVideoMode(void) { return false; }
static inline int GetTrueVideoMode(void) { return 3; }
void SDL_Delay(uint32_t ms);
void DOSBOX_RunMachine(void);
void kdos_cds_sync(uint8_t drive);
