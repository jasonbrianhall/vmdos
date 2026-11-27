/* kdos: starting DOSBox-X's DOS kernel and shell on vmdos (shell=kernel),
   and drive C: on the disk vmdos found. The order follows DOSBox-X's own
   boot of its DOS (sdlmain.cpp, after "DOS kernel init"). */
#include "dosbox.h"
#include "logging.h"
#include "regs.h"
#include "callback.h"
#include "mem.h"
#include "control.h"
#include "dos_inc.h"
#include "drives.h"
#include "bios_disk.h"
#include "shell.h"
#include "bios.h"

extern "C" {
void kh_log(const char *s);
int kh_disk_read(uint32_t lba, uint32_t n, void *buf);
int kh_disk_write(uint32_t lba, uint32_t n, const void *buf);
uint32_t kh_disk_sectors(void);
int kh_disk_part(uint32_t *base, uint32_t *count);
}

void CALLBACK_Init(void);
void PROGRAMS_Init(void);
void DOS_Startup(Section *sec);
void DRIVES_Startup(Section *s);
void DOS_InitClock(void);
void SHELL_MessagesInit(void);
void SHELL_Init(void);
void SHELL_Run(void);
void AUTOEXEC_Startup(Section *sec);
extern CallBack_Handler CallBack_Handlers[];
extern bool dos_kernel_disabled;

/* vmdos's C: as a DOSBox-X disk: its sectors come from vmdos's disk code
   (AHCI, NVMe, USB, ... or the RAM disk), the same as INT 13h's */
class vmdosDisk : public imageDisk {
public:
    vmdosDisk() : imageDisk(ID_BASE)
    {
        uint32_t n = kh_disk_sectors();
        sector_size = 512;
        hardDrive = true;
        active = true;
        diskSizeK = n / 2;
        image_length = (uint64_t)n * 512;
        heads = 255; sectors = 63; cylinders = n / (255 * 63) + 1;
        diskname = "vmdos C:";
    }
    Int13Status Read_AbsoluteSector(uint32_t s, void *data) override
    { return kh_disk_read(s, 1, data) ? Int13Status::SectorNotFound : Int13Status::NoError; }
    Int13Status Write_AbsoluteSector(uint32_t s, const void *data) override
    { return kh_disk_write(s, 1, data) ? Int13Status::WriteProtected : Int13Status::NoError; }
};

static void mount_c(void)
{
    uint32_t base, count;
    if (kh_disk_part(&base, &count)) { LOG_MSG("no C: (no FAT partition vmdos can read)"); return; }
    imageDisk *d = new vmdosDisk();
    d->Addref();
    std::vector<std::string> opts;
    char b[40];
    snprintf(b, sizeof b, "sectoff=%u", (unsigned)base); opts.push_back(b);
    snprintf(b, sizeof b, "sectlen=%u", (unsigned)count); opts.push_back(b);
    fatDrive *f = new fatDrive(d, opts);
    if (!f->created_successfully) { LOG_MSG("C: at sector %u: not a FAT file system DOSBox-X can mount", (unsigned)base); return; }
    DriveManager::AppendDisk(2, f);
    DriveManager::InitializeDrive(2);
    mem_writeb(Real2Phys(dos.tables.mediaid) + 2 * 9, f->GetMediaByte());
    LOG_MSG("C: is vmdos's disk, FAT partition at sector %u (%u sectors)", (unsigned)base, (unsigned)count);
}

/* Z:\AUTOEXEC.BAT: the shell starts on Z: and runs this, not FreeDOS's
   C:\AUTOEXEC.BAT (whose DOSLFN and SHSUCDX are FreeDOS's: this DOS has long
   names itself). C:\KDOSEXEC.BAT, when there is one, is the user's own. */
static char autoexec[] =
    "@ECHO OFF\r\n"
    "PROMPT $P$G\r\n"
    "PATH Z:\\;C:\\;C:\\VMDOS;C:\\VMDOS\\NET;C:\\BIN\r\n"
    "SET MTCPCFG=C:\\VMDOS\\NET\\VMTCP.CFG\r\n"
    "SET BLASTER=A220 I5 D1 T4 P330\r\n"
    "IF EXIST C:\\KDOSEXEC.BAT CALL C:\\KDOSEXEC.BAT\r\n"
    "ECHO.\r\n"
    "ECHO Welcome to the VMDOS Command Shell\r\n"
    "ECHO.\r\n"
    "C:\r\n";

void MSG_Replace(const char *name, const char *text);
void MSG_Rename(const char *from, const char *to);

/* The shell's name on screen is vmdos's: "vmdos command shell". Its
   messages are DOSBox-X's, with these changed (more can go here as the
   shell gains or loses commands). */
static void brand_messages(void)
{
    MSG_Replace("SHELL_CMD_VER_VER", "vmdos command shell version %s%s. Reported DOS version %d.%02d.\n");
    MSG_Replace("SHELL_CMD_VER_HELP", "Displays or sets the DOS version the shell reports.\n");
    MSG_Replace("SHELL_CMD_HELP_HELP", "Shows the vmdos command shell's commands.\n");
    MSG_Replace("SHELL_EXECUTE_DRIVE_NOT_FOUND",
                "Drive %c does not exist!\nVMHD gives other disks' partitions a letter, VMCD changes CDs, VMFD floppies.\n");
    MSG_Rename("DOSBox-X command shell", "vmdos command shell");
    MSG_Rename("DOSBox-X's", "vmdos's");
    MSG_Rename("DOSBox-X", "vmdos");
}

static void kdos_main(void)
{
    dos_kernel_disabled = false;
    DOS_Startup(nullptr);
    DRIVES_Startup(nullptr);
    DOS_InitClock();
    mount_c();
    SHELL_MessagesInit();
    brand_messages();
    VFILE_Register("AUTOEXEC.BAT", (uint8_t *)autoexec, (uint32_t)strlen(autoexec));
    SHELL_Init();
    SHELL_Run();
    kh_log("kdos: the shell ended\n");
}
static Bitu boot_handler(void) { kdos_main(); return CBRET_NONE; }

/* from guest_start (v86.c), in place of booting FreeDOS: the guest starts
   at a callback that runs the DOS kernel's start and then the shell */
struct kregs_s;
extern "C" void kdos_boot(uint32_t *cs, uint32_t *eip, uint32_t *ss, uint32_t *esp)
{
    /* kdos's zeroed data is after the kernel, not in its .bss (linker.ld) */
    extern char __kdos_bss_start[], __kdos_end[];
    memset(__kdos_bss_start, 0, (size_t)(__kdos_end - __kdos_bss_start));
    /* the C++ constructors of kdos's globals (the monitor runs none) */
    extern void (*__init_array_start[])(void), (*__init_array_end[])(void);
    for (void (**f)(void) = __init_array_start; f < __init_array_end; f++) (*f)();
    static Config cfg;
    control = &cfg;
    CALLBACK_Init();
    PROGRAMS_Init();
    Bitu cb = CALLBACK_Allocate();
    CallBack_Handlers[cb] = boot_handler;
    PhysPt at = ROMBIOS_GetMemory(8, "kdos boot", 1);
    phys_writeb(at + 0, 0xFE); phys_writeb(at + 1, 0x38); phys_writew(at + 2, (uint16_t)cb);
    phys_writeb(at + 4, 0xFB); phys_writeb(at + 5, 0xF4); phys_writeb(at + 6, 0xEB); phys_writeb(at + 7, 0xFC);   /* sti; hlt; jmp $-2 */
    *cs = at >> 4 & 0xF000; *eip = at & 0xFFFF;
    *ss = 0x9000; *esp = 0xFFFE;
    kh_log("kdos: starting the vmdos command shell (shell=kernel; DOS kernel and shell from DOSBox-X)\n");
}
