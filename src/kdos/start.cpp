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
#include <time.h>
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

/* vmdos's floppy drive d (A: 0, B: 1): its image, whatever VMFD or the
   boot put in it (floppy.c) */
extern "C" {
uint32_t kh_fd_sectors(int d);
int kh_fd_rw(int d, uint32_t lba, uint32_t n, void *buf, int write);
uint32_t kh_fd_gen(int d);
}
class vmdosFloppy : public imageDisk {
public:
    int d;
    vmdosFloppy(int drive, uint32_t n) : imageDisk(ID_BASE), d(drive)
    {
        uint8_t bs[512];
        sector_size = 512;
        hardDrive = false;
        active = true;
        diskSizeK = n / 2;
        image_length = (uint64_t)n * 512;
        heads = 2; sectors = 18;
        if (!kh_fd_rw(d, 0, 1, bs, 0)) {                         /* the boot sector's geometry */
            uint16_t spt = (uint16_t)(bs[0x18] | bs[0x19] << 8), hd = (uint16_t)(bs[0x1A] | bs[0x1B] << 8);
            if (spt && spt <= 63 && hd && hd <= 255) { sectors = spt; heads = hd; }
        }
        cylinders = n / (heads * sectors);
        diskname = d ? "vmdos B:" : "vmdos A:";
    }
    Int13Status Read_AbsoluteSector(uint32_t s, void *data) override
    { return kh_fd_rw(d, s, 1, data, 0) ? Int13Status::SectorNotFound : Int13Status::NoError; }
    Int13Status Write_AbsoluteSector(uint32_t s, const void *data) override
    { return kh_fd_rw(d, s, 1, (void *)data, 1) ? Int13Status::WriteProtected : Int13Status::NoError; }
};
static uint32_t fd_gen_seen[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };

/* A: or B: as the floppy now in it (empty, or not a DOS disk: no letter).
   The drive it replaces isn't freed: files may still be open on it. */
static void mount_floppy(int d)
{
    fd_gen_seen[d] = kh_fd_gen(d);
    Drives[d] = nullptr;
    uint32_t n = kh_fd_sectors(d);
    if (!n) return;
    imageDisk *disk = new vmdosFloppy(d, n);
    disk->Addref();
    std::vector<std::string> opts;
    fatDrive *f = new fatDrive(disk, opts);
    if (!f->created_successfully) { LOG_MSG("%c: holds no FAT file system DOSBox-X can mount", 'A' + d); return; }
    Drives[d] = f;
    mem_writeb(Real2Phys(dos.tables.mediaid) + d * 9, f->GetMediaByte());
    LOG_MSG("%c: is vmdos's floppy (%u KiB)", 'A' + d, (unsigned)(n / 2));
}

/* A floppy changed (VMFD, the media console): its drive again. From the
   trap, before each INT 21h. */
void MSCDEX_Startup(Section *);
void kdos_mount_cds(void);

extern "C" void kdos_floppy_check(void)
{
    if (dos_kernel_disabled) return;
    for (int d = 0; d < 2; d++)
        if (kh_fd_gen(d) != fd_gen_seen[d]) mount_floppy(d);
}

/* Z:\AUTOEXEC.BAT: the shell starts on Z: and runs this, not FreeDOS's
   C:\AUTOEXEC.BAT (whose DOSLFN and SHSUCDX are FreeDOS's: this DOS has
   long names itself). Written for the drives there are: vmdos's tools are
   in C:\VMDOS, or on the boot floppy in A:\VMDOS when there's no C:.
   KDOSEXEC.BAT in the root of that drive, when there is one, is the user's. */
static char autoexec[1536];
static int autoexec_n;
static void add(const char *f, ...)
{
    va_list a;
    va_start(a, f);
    autoexec_n += vsnprintf(autoexec + autoexec_n, sizeof autoexec - (size_t)autoexec_n, f, a);
    va_end(a);
}
static void make_autoexec(void)
{
    char h = Drives[2] ? 'C' : Drives[0] ? 'A' : 0;     /* where vmdos's tools are */
    add("@ECHO OFF\r\nPROMPT $P$G\r\n");
    if (h) {
        add("PATH Z:\\;%c:\\;%c:\\VMDOS;%c:\\VMDOS\\NET%s\r\n", h, h, h, h == 'C' ? ";C:\\BIN" : "");
        add("SET MTCPCFG=%c:\\VMDOS\\NET\\VMTCP.CFG\r\n", h);
    } else add("PATH Z:\\\r\n");
    add("SET BLASTER=A220 I5 D1 T4 P330\r\n");
    if (h) add("IF EXIST %c:\\KDOSEXEC.BAT CALL %c:\\KDOSEXEC.BAT\r\n", h, h);
    add("ECHO.\r\nECHO Welcome to the VMDOS Command Shell\r\nECHO.\r\n");
    if (h == 'A')
        add("ECHO No C: drive: vmdos found no FAT disk.\r\n"
            "ECHO A: is the boot floppy, with vmdos's tools in A:\\VMDOS.\r\nECHO.\r\n");
    if (!h)
        add("ECHO No C: drive: vmdos found no FAT disk, and no boot floppy is in A:,\r\n"
            "ECHO so vmdos's tools (VMDOSNET, VMFD, ...) aren't here.\r\nECHO.\r\n");
    if (h) add("%c:\r\n", h);
}

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
                "Drive %c does not exist!\nVMDOSNET gives a host folder a drive letter; VMFD changes floppies in A: and B:.\n");
    MSG_Rename("DOSBox-X command shell", "vmdos command shell");
    MSG_Rename("DOSBox-X's", "vmdos's");
    MSG_Rename("DOSBox-X", "vmdos");
}

static void kdos_main(void)
{
    {   /* Z:'s files are dated now, not DOSBox-X's 2002-10-01 */
        extern uint16_t fztime, fzdate;
        time_t t = time(nullptr);
        struct tm *tm = localtime(&t);
        fzdate = DOS_PackDate((uint16_t)(tm->tm_year + 1900), (uint16_t)(tm->tm_mon + 1), (uint16_t)tm->tm_mday);
        fztime = DOS_PackTime((uint16_t)tm->tm_hour, (uint16_t)tm->tm_min, (uint16_t)tm->tm_sec);
    }
    dos_kernel_disabled = false;
    DOS_Startup(nullptr);
    DRIVES_Startup(nullptr);
    DOS_InitClock();
    mount_floppy(0);
    mount_floppy(1);
    mount_c();
    MSCDEX_Startup(nullptr);              /* DOSBox-X's MSCDEX, for vmdos's CD drives */
    kdos_mount_cds();
    SHELL_MessagesInit();
    brand_messages();
    make_autoexec();
    VFILE_Register("AUTOEXEC.BAT", (uint8_t *)autoexec, (uint32_t)strlen(autoexec));
    SHELL_Init();
    SHELL_Run();
    kh_log("kdos: the shell ended\n");
}
static Bitu boot_handler(void) { kdos_main(); return CBRET_NONE; }

/* from guest_start (v86.c), in place of booting FreeDOS: the guest starts
   at a callback that runs the DOS kernel's start and then the shell */
struct kregs_s;
extern "C" void kh_restart(char *data, char *data_end);
extern "C" void kdos_boot(uint32_t *cs, uint32_t *eip, uint32_t *ss, uint32_t *esp)
{
    /* kdos's zeroed data is after the kernel, not in its .bss (linker.ld) */
    extern char __kdos_bss_start[], __kdos_end[], __kdos_data_start[], __kdos_data_end[];
    kh_restart(__kdos_data_start, __kdos_data_end);   /* started before (VMHV): data as loaded */
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
