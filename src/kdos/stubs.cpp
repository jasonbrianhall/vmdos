/* kdos: the parts of DOSBox-X its DOS and shell call that vmdos has no use
   for, or does its own way: the GUI and menus, capture, keyboard layouts
   and DBCS code pages, host folders (localDrive, overlays), its own XMS,
   EMS and mouse (vmdos's are in the monitor). Each does nothing, or the
   least that keeps the caller right. */
#include "dosbox.h"
#include "regs.h"
#include "mem.h"
#include "cross.h"
#include "dos_inc.h"
#include "drives.h"
#include "bios_disk.h"
#include "shiftjis.h"
#include <typeinfo>
#include <cxxabi.h>

extern "C" int kh_a20(void);
extern "C" void kh_set_a20(int on);

/* ---- variables DOSBox-X's other modules own ---- */
bool Mouse_Drv = false;
Bitu VGA_BIOS_SEG_END = 0xC800;
uint8_t ZDRIVE_NUM = 25;
bool addovl, addipx, addne2k, enableime, showdbcs;
uint16_t altcp_to_unicode[256], customcp_to_unicode[256];
bool chinasea, gbk;
uint8_t *clipAscii;
uint32_t clipSize;
bool clipboard_dosapi, swapad;
std::string config_run_var_device, config_run_var_devparm;
bool ctrlbrk, int15_wait_force_unmask_irq = true;
bool dbg_zero_on_dos_allocmem;
uint16_t desired_ems_segment;
uint8_t device_nextdrive;
bool dos_clear_tf_on_int01;
bool dos_con_use_int16_to_detect_input = true;
bool dos_kernel_disabled = true;
bool dos_shell_running_program;
unsigned int dosbox_shell_env_size;
bool j3100_start;
int log_dev_con;
bool mountwarning;
int msgcodepage;
bool pcibus_enable;
std::string prefix_local = ".DBLOCALFILE", prefix_overlay = ".DBOVERLAY";
int result_errorcode;
bool startcmd, startquiet, starttranspath, startwait, winrun;
std::string strPasteBuffer;
bool use_quick_reboot;
Bitu vm86_fake_io_off, vm86_fake_io_seg;
bool wpcolon;

/* ---- functions ---- */
void Add_VFiles(bool) {}
void CAPTURE_StartCapture() {} void CAPTURE_StopCapture() {}
void CAPTURE_StartMTWave() {} void CAPTURE_StopMTWave() {}
void CAPTURE_StartOPL() {} void CAPTURE_StopOPL() {}
void CAPTURE_StartWave() {} void CAPTURE_StopWave() {}
bool CheckDBCSCP(int32_t) { return false; }
bool isDBCSCP() { return false; }
bool InitCodePage() { return true; }
void initcodepagefont(void) {}
void makestdcp950table() {} void makeseacp951table() {}
void InitFontHandle(void) {} void ShutFontHandle(void) {} void JFONT_Init(void) {}
/* code page 437 both sides: names pass through unchanged */
static char cnv[CROSS_LEN * 4];
host_cnv_char_t *CodePageGuestToHost(const char *s) { strncpy(cnv, s, sizeof cnv - 1); return cnv; }
char *CodePageHostToGuest(const host_cnv_char_t *s) { strncpy(cnv, s, sizeof cnv - 1); return cnv; }
bool CodePageGuestToHostUTF8_Core(char *d, const char *s) { strcpy(d, s); return true; }
bool CodePageHostToGuestUTF8_Core(char *d, const char *s) { strcpy(d, s); return true; }
bool CodePageGuestToHostUTF16_Core(uint16_t *d, const char *s) { while ((*d++ = (uint8_t)*s++)) ; return true; }
bool CodePageHostToGuestUTF16_Core(char *d, const uint16_t *s) { while ((*d++ = (char)*s++)) ; return true; }
void Cross::ResolveHomedir(std::string &) {}
void DOSBox_SetSysMenu(void) {}
Bitu DOS_ChangeCodepage(int32_t, const char *) { return 1; }
Bitu DOS_ChangeKeyboardLayout(const char *, int32_t) { return 1; }
Bitu DOS_LoadKeyboardLayout(const char *, int32_t, const char *) { return 1; }
const char *DOS_GetLoadedLayout(void) { return nullptr; }
bool DOS_ClipboardGetUTF8(std::string &) { return false; }
bool DOS_ClipboardSetUTF8(const std::string &) { return false; }
bool getClipboard() { return false; }
void PasteClipboard(bool) {}
std::string GetDOSBoxXPath(bool) { return ""; }
Bitu GetEMSPageFrameSegment(void) { return 0xE000; }
RealPt Get_EMS_vm86control() { return 0; }
void LOADFIX_OnDOSShutdown() {}
void Load_Language(std::string) {}
bool SwitchLanguage(int, int, bool) { return false; }
void RDTSC_rebase() {}
void ResolvePath(std::string &) {}
void SetVal(const std::string &, const std::string &, const std::string &) {}
void Update_Get_Desired_Segment(void) {}
/* A20: vmdos's own (the monitor's XMS driver keeps it) */
static bool local_a20;
Bitu XMS_EnableA20(bool on) { kh_set_a20(on); return 1; }
Bitu XMS_GetEnabledA20(void) { return kh_a20() ? 1 : 0; }
bool XMS_HMA_EXISTS() { return false; }
bool XMS_IS_ACTIVE() { return false; }
void XMS_DOS_LocalA20EnableIfNotEnabled(void) { local_a20 = !kh_a20(); if (local_a20) kh_set_a20(1); }
void XMS_DOS_LocalA20DisableIfNotEnabled(void) { if (local_a20) { kh_set_a20(0); local_a20 = false; } }
void XMS_DOS_LocalA20EnableIfNotEnabled_XMSCALL(void) { XMS_DOS_LocalA20EnableIfNotEnabled(); }
void XMS_DOS_LocalA20DisableIfNotEnabled_XMSCALL(void) { XMS_DOS_LocalA20DisableIfNotEnabled(); }
/* DOSBox-X decides whether long names are on in its menu code (menu_callback.cpp) */
extern int enablelfn;
void dos_ver_menu(bool start)
{
    if (start || enablelfn != -2) uselfn = enablelfn == 1 || ((enablelfn == -1 || enablelfn == -2) && (dos.version.major > 6 || winrun));
}
void menu_update_autocycle(void) {}
void update_dos_ems_menu(void) {}
void pc98_set_char_mode(bool) {}
void runMount(const char *) {}
void runRescan(const char *) {}
bool systemmessagebox(char const *, char const *, char const *, char const *, int) { return false; }
void updateDPT(void) {}
std::string localDrive::GetHostName(const char *) { return ""; }
std::string Overlay_Drive::GetHostName(const char *) { return ""; }
ShiftJISDecoder::ShiftJISDecoder() {}

/* DOSBox-X's dynamic_casts name these drive and disk classes, which kdos
   leaves out: they get a type_info of their own, so a cast to one of them
   never matches */
#define KDOS_TYPEINFO(mangled, tag) \
    extern "C" { __cxxabiv1::__class_type_info kdos_ti_##tag __asm__(mangled)("kdos_none_" #tag); }
KDOS_TYPEINFO("_ZTI10localDrive", localDrive)
KDOS_TYPEINFO("_ZTI13Overlay_Drive", Overlay_Drive)
KDOS_TYPEINFO("_ZTI15imageDiskMemory", imageDiskMemory)
KDOS_TYPEINFO("_ZTI12imageDiskVHD", imageDiskVHD)

/* The programs on Z: (DOSBox-X's are in dos_programs.cpp, not here): just
   COMMAND.COM, this shell, for COMMAND /C and programs that run COMSPEC.
   vmdos's VM* tools are on C:. */
#include "programs.h"
void SHELL_ProgramStart(Program **make);
void DOS_SetupPrograms(void)
{
    PROGRAMS_MakeFile("COMMAND.COM", SHELL_ProgramStart);
}

/* for DOSBox-X's MSCDEX and ISO drive (dos_mscdex.cpp, drive_iso.cpp) */
bool getSwapRequest(void) { return false; }                 /* DOSBox-X's disc-swap hotkey */
void IDE_ATAPI_MediaChangeNotify(unsigned char, bool) {}   /* no emulated IDE CD drives here */
