// vmdos.efi: UEFI loader for the 32-bit vmdos kernel.
//
// Reads dos.img, or else floppy.img (the boot floppy vmdos falls back to
// as A: when it finds no C:), and any *.ISO files (CD-ROM images) from the
// folder vmdos.efi was started from, takes the
// framebuffer from GOP, places the embedded kernel at its link address
// (16 MiB), exits boot services, leaves long mode (efi/tramp.S) and enters
// the kernel with Multiboot information, as GRUB would.
//
// "debug" in the load options waits for a key before handing over; other
// options (e.g. debug=2, nopae) go to the kernel's command line.
#include <efi.h>
#include <efilib.h>

extern const UINT8 kernel_image[], kernel_image_end[];
extern const UINT8 tramp_start[], tramp_end[];
#include "kernel_layout.h"      // KERNEL_BASE, KERNEL_END, KERNEL_ENTRY
#include "version.h"            // VMDOS_VERSION

struct __attribute__((packed)) MultibootInfo {
    UINT32 flags, mem_lower, mem_upper, boot_device, cmdline;
    UINT32 mods_count, mods_addr;
    UINT32 syms[4];
    UINT32 mmap_length, mmap_addr, drives_length, drives_addr;
    UINT32 config_table, boot_loader_name, apm_table;
    UINT32 vbe_control_info, vbe_mode_info;
    UINT16 vbe_mode, vbe_interface_seg, vbe_interface_off, vbe_interface_len;
    UINT64 fb_addr;
    UINT32 fb_pitch, fb_width, fb_height;
    UINT8 fb_bpp, fb_type;
    UINT8 fb_pad[2];            // GRUB aligns what follows to 4 bytes
    UINT8 fb_color[6];          // red, green, blue (position, size) pairs
};
struct __attribute__((packed)) MultibootModule { UINT32 mod_start, mod_end, string, reserved; };
struct __attribute__((packed)) MultibootMmap { UINT32 size; UINT64 addr, len; UINT32 type; };
struct Handoff { UINT32 src, dst, copy_len, zero_len, mbi, entry; };
#define MAX_MMAP 256
#define KERNEL_MEM_SIZE (KERNEL_END - KERNEL_BASE)

static EFI_SYSTEM_TABLE* ST_;

// Plain loops (gnu-efi's CopyMem/SetMem calling convention varies by version).
static void fill(void* p, UINTN n, UINT8 v) { volatile UINT8* d = p; while (n--) *d++ = v; }
static void copy(void* to, const void* from, UINTN n) {
    volatile UINT8* d = to;
    const UINT8* s = from;
    while (n--) *d++ = *s++;
}

static void wait_key(void) {
    UINTN idx;
    uefi_call_wrapper(ST_->ConIn->Reset, 2, ST_->ConIn, FALSE);
    uefi_call_wrapper(ST_->BootServices->WaitForEvent, 3, 1, &ST_->ConIn->WaitForKey, &idx);
}

// A key, or secs seconds with none.
static void wait_key_secs(int secs) {
    uefi_call_wrapper(ST_->ConIn->Reset, 2, ST_->ConIn, FALSE);
    for (int i = 0; i < secs * 100; i++) {
        EFI_INPUT_KEY k;
        if (!EFI_ERROR(uefi_call_wrapper(ST_->ConIn->ReadKeyStroke, 2, ST_->ConIn, &k))) return;
        uefi_call_wrapper(ST_->BootServices->Stall, 1, 10000);
    }
}

static void fail(CHAR16* msg) {
    Print(L"\r\nvmdos.efi: %s\r\nPress any key to return.\r\n", msg);
    wait_key();
}

static void* alloc_low_type(UINTN bytes, EFI_MEMORY_TYPE type) {
    EFI_PHYSICAL_ADDRESS addr = 0xFFFFFFFF;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->AllocatePages, 4, AllocateMaxAddress,
                                    type, EFI_SIZE_TO_PAGES(bytes), &addr)))
        return NULL;
    return (void*)(UINTN)addr;
}
static void* alloc_low(UINTN bytes) { return alloc_low_type(bytes, EfiLoaderData); }

static int gop_format_ok(EFI_GRAPHICS_PIXEL_FORMAT f) {
    return f == PixelBlueGreenRedReserved8BitPerColor || f == PixelRedGreenBlueReserved8BitPerColor;
}

// Keep the current mode if it's 32-bit and at least 640x400; else the
// smallest 32-bit mode that is (text at 1:1 is crisp and fast to draw).
static EFI_GRAPHICS_OUTPUT_PROTOCOL* fit_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop) {
    if (!gop || !gop->Mode || !gop->Mode->Info) return NULL;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* cur = gop->Mode->Info;
    if (gop_format_ok(cur->PixelFormat) && cur->HorizontalResolution >= 640 && cur->VerticalResolution >= 400)
        return gop;
    UINT32 best = (UINT32)-1, best_px = 0xFFFFFFFF;
    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info;
        UINTN size;
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, m, &size, &info))) continue;
        UINT32 px = info->HorizontalResolution * info->VerticalResolution;
        if (gop_format_ok(info->PixelFormat) && info->HorizontalResolution >= 640 &&
            info->VerticalResolution >= 400 && px < best_px) { best = m; best_px = px; }
    }
    if (best == (UINT32)-1) return gop_format_ok(cur->PixelFormat) ? gop : NULL;
    uefi_call_wrapper(gop->SetMode, 2, gop, best);
    return gop;
}

// The firmware's own choice: the GOP the console draws on.
static EFI_GRAPHICS_OUTPUT_PROTOCOL* setup_gop(void) {
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->LocateProtocol, 3, &guid, NULL, (void**)&gop)) || !gop)
        return NULL;
    return fit_mode(gop);
}

static void* read_file(EFI_HANDLE image, CHAR16* name, UINTN* size) {
    EFI_GUID lip = LOADED_IMAGE_PROTOCOL, sfs = SIMPLE_FILE_SYSTEM_PROTOCOL;
    EFI_LOADED_IMAGE* li;
    EFI_FILE_IO_INTERFACE* fs;
    EFI_FILE_HANDLE root, f;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->HandleProtocol, 3, image, &lip, (void**)&li))) return NULL;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->HandleProtocol, 3, li->DeviceHandle, &sfs, (void**)&fs))) return NULL;
    if (EFI_ERROR(uefi_call_wrapper(fs->OpenVolume, 2, fs, &root))) return NULL;
    if (EFI_ERROR(uefi_call_wrapper(root->Open, 5, root, &f, name, EFI_FILE_MODE_READ, 0))) return NULL;
    EFI_FILE_INFO* info = LibFileInfo(f);
    if (!info) return NULL;
    *size = info->FileSize;
    FreePool(info);
    void* buf = alloc_low(*size + 1);
    if (!buf) return NULL;
    UINTN n = *size;
    if (EFI_ERROR(uefi_call_wrapper(f->Read, 3, f, &n, buf)) || n != *size) return NULL;
    uefi_call_wrapper(f->Close, 1, f);
    return buf;
}

// The *.ISO files in folder dir (path with a trailing backslash).
#define MAX_ISO 7
static struct { UINT8* data; UINTN size; char name[32]; } isos[MAX_ISO];
static int n_iso;

static void load_isos(EFI_HANDLE image, CHAR16* dir) {
    EFI_GUID lip = LOADED_IMAGE_PROTOCOL, sfs = SIMPLE_FILE_SYSTEM_PROTOCOL;
    EFI_LOADED_IMAGE* li;
    EFI_FILE_IO_INTERFACE* fs;
    EFI_FILE_HANDLE root, d;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->HandleProtocol, 3, image, &lip, (void**)&li))) return;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->HandleProtocol, 3, li->DeviceHandle, &sfs, (void**)&fs))) return;
    if (EFI_ERROR(uefi_call_wrapper(fs->OpenVolume, 2, fs, &root))) return;
    static CHAR16 dpath[512], fpath[600];
    StrCpy(dpath, dir);
    UINTN dl = StrLen(dpath);
    if (dl > 1 && dpath[dl - 1] == L'\\') dpath[dl - 1] = 0;
    if (dl <= 1) d = root;
    else if (EFI_ERROR(uefi_call_wrapper(root->Open, 5, root, &d, dpath, EFI_FILE_MODE_READ, 0))) return;
    static UINT8 buf[1024];
    for (;;) {
        UINTN n = sizeof buf;
        if (EFI_ERROR(uefi_call_wrapper(d->Read, 3, d, &n, buf)) || n == 0) break;
        EFI_FILE_INFO* fi = (EFI_FILE_INFO*)buf;
        if (fi->Attribute & EFI_FILE_DIRECTORY) continue;
        UINTN ln = StrLen(fi->FileName);
        if (ln < 5) continue;
        CHAR16* e = fi->FileName + ln - 4;
        if (!(e[0] == L'.' && (e[1] | 32) == L'i' && (e[2] | 32) == L's' && (e[3] | 32) == L'o')) continue;
        if (n_iso == MAX_ISO) { Print(L"  (more than %d ISO files: %s skipped)\r\n", MAX_ISO, fi->FileName); continue; }
        StrCpy(fpath, dir);
        StrCat(fpath, fi->FileName);
        Print(L"  CD image:     %s\r\n", fpath);
        UINTN size = 0;
        UINT8* data = read_file(image, fpath, &size);
        if (!data) { Print(L"    can't read it (out of memory below 4 GiB?)\r\n"); continue; }
        isos[n_iso].data = data; isos[n_iso].size = size;
        UINTN k = 0;
        for (; k < ln && k < 31; k++) isos[n_iso].name[k] = fi->FileName[k] < 127 ? (char)fi->FileName[k] : '_';
        isos[n_iso].name[k] = 0;
        n_iso++;
    }
}

static int boot_usb;

// The partition vmdos.efi was loaded from: its start (LBA, 0 if unknown)
// and signature (MBR disk signature or GPT partition GUID, sig_len 4 / 16).
// The kernel prefers it for C: (esp=LBA espsig=HEX on the command line).
static UINT64 boot_partition(EFI_HANDLE image, UINT8* sig, int* sig_len) {
    EFI_GUID lip = LOADED_IMAGE_PROTOCOL;
    EFI_LOADED_IMAGE* li;
    *sig_len = 0;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->HandleProtocol, 3, image, &lip, (void**)&li))) return 0;
    EFI_DEVICE_PATH* dp = DevicePathFromHandle(li->DeviceHandle);
    for (int guard = 0; dp && !IsDevicePathEnd(dp) && guard < 64; guard++, dp = NextDevicePathNode(dp)) {
        if (DevicePathType(dp) == MESSAGING_DEVICE_PATH &&
            (DevicePathSubType(dp) == MSG_USB_DP || DevicePathSubType(dp) == MSG_USB_CLASS_DP))
            boot_usb = 1;                               // on a USB stick: the kernel waits for it
        if (DevicePathType(dp) == MEDIA_DEVICE_PATH && DevicePathSubType(dp) == MEDIA_HARDDRIVE_DP) {
            HARDDRIVE_DEVICE_PATH* hd = (HARDDRIVE_DEVICE_PATH*)dp;
            int n = hd->SignatureType == 1 ? 4 : hd->SignatureType == 2 ? 16 : 0;
            for (int i = 0; i < n; i++) sig[i] = hd->Signature[i];
            *sig_len = n;
            return hd->PartitionStart;
        }
    }
    return 0;
}

static int file_exists(EFI_HANDLE image, CHAR16* name) {
    EFI_GUID lip = LOADED_IMAGE_PROTOCOL, sfs = SIMPLE_FILE_SYSTEM_PROTOCOL;
    EFI_LOADED_IMAGE* li;
    EFI_FILE_IO_INTERFACE* fs;
    EFI_FILE_HANDLE root, f;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->HandleProtocol, 3, image, &lip, (void**)&li))) return 0;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->HandleProtocol, 3, li->DeviceHandle, &sfs, (void**)&fs))) return 0;
    if (EFI_ERROR(uefi_call_wrapper(fs->OpenVolume, 2, fs, &root))) return 0;
    if (EFI_ERROR(uefi_call_wrapper(root->Open, 5, root, &f, name, EFI_FILE_MODE_READ, 0))) return 0;
    uefi_call_wrapper(f->Close, 1, f);
    return 1;
}

static int has_word(const char* s, const char* w) {
    for (; *s; s++) {
        int i = 0;
        while (w[i] && s[i] == w[i]) i++;
        if (!w[i] && (s[i] == 0 || s[i] == ' ')) return 1;
    }
    return 0;
}

// vga= (kernel options): the screen mode to boot in. vmdos can't change it
// once the kernel runs (GOP is a boot service; the README says why), so it
// is picked here: vga=ask lists the modes and asks, vga=N takes mode N from
// that list, vga=WxH the mode of that size, vga=max the largest (usually the
// panel's own). The first vga= given wins: GRUB's menu entry, then KARGS,
// then vmdos.cfg.
static const char* opt_value(const char* s, const char* key) {
    UINTN kl = 0;
    while (key[kl]) kl++;
    for (const char* p = s; *p; p++) {
        if (p != s && p[-1] != ' ') continue;
        UINTN i = 0;
        while (i < kl && p[i] == key[i]) i++;
        if (i == kl) return p + kl;
    }
    return NULL;
}

static int mode_ok(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info) {
    return gop_format_ok(info->PixelFormat) && info->HorizontalResolution >= 640 && info->VerticalResolution >= 400;
}

static void list_modes(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop) {
    int col = 0;
    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info;
        UINTN size;
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, m, &size, &info)) || !mode_ok(info)) continue;
        UINT32 h = info->VerticalResolution;
        Print(L"  %2d: %4dx%d%c%s", m, info->HorizontalResolution, h, m == gop->Mode->Mode ? L'*' : L' ',
              h >= 1000 ? L"" : h >= 100 ? L" " : L"  ");          // heights padded to 4
        if (++col == 4) { Print(L"\r\n"); col = 0; }
    }
    if (col) Print(L"\r\n");
}

static int find_mode(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, UINT32 w, UINT32 h, int largest) {
    int best = -1;
    UINT64 best_px = 0;
    for (UINT32 m = 0; m < gop->Mode->MaxMode; m++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info;
        UINTN size;
        if (EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, m, &size, &info)) || !mode_ok(info)) continue;
        UINT64 px = (UINT64)info->HorizontalResolution * info->VerticalResolution;
        if (largest ? px > best_px : (info->HorizontalResolution == w && info->VerticalResolution == h)) {
            best = (int)m; best_px = px;
            if (!largest) break;
        }
    }
    return best;
}

// A mode number typed at the keyboard; Enter alone, Esc or 15 s with no key: -1.
static int read_mode_number(void) {
    int v = -1, waited = 0, typed = 0;
    uefi_call_wrapper(ST_->ConIn->Reset, 2, ST_->ConIn, FALSE);
    for (;;) {
        EFI_INPUT_KEY k;
        if (EFI_ERROR(uefi_call_wrapper(ST_->ConIn->ReadKeyStroke, 2, ST_->ConIn, &k))) {
            uefi_call_wrapper(ST_->BootServices->Stall, 1, 10000);
            if (!typed && ++waited > 1500) { Print(L"\r\n"); return -1; }
            continue;
        }
        typed = 1;
        if (k.UnicodeChar == L'\r') { Print(L"\r\n"); return v; }
        if (k.ScanCode == 0x17) { Print(L"\r\n"); return -1; }          // Esc
        if ((k.UnicodeChar == 8 || k.ScanCode == 0x08) && v >= 0) {   // backspace
            v = v >= 10 ? v / 10 : -1;
            Print(L"\b \b");
        } else if (k.UnicodeChar >= L'0' && k.UnicodeChar <= L'9' && v < 1000) {
            v = (v < 0 ? 0 : v * 10) + (k.UnicodeChar - L'0');
            Print(L"%c", k.UnicodeChar);
        }
    }
}

static void apply_vga(EFI_GRAPHICS_OUTPUT_PROTOCOL* gop, const char* opts) {
    const char* v = opt_value(opts, "vga=");
    if (!v) return;
    int m = -1;
    if (v[0] == 'a' && v[1] == 's' && v[2] == 'k') {
        Print(L"\r\nScreen modes (* = now):\r\n");
        list_modes(gop);
        Print(L"Mode number, then Enter (Enter alone or 15 s: keep %dx%d): ",
              gop->Mode->Info->HorizontalResolution, gop->Mode->Info->VerticalResolution);
        m = read_mode_number();
        if (m < 0) return;
    } else if (v[0] == 'm' && v[1] == 'a' && v[2] == 'x') {
        m = find_mode(gop, 0, 0, 1);
    } else {
        UINT32 a = 0, b = 0;
        const char* p = v;
        while (*p >= '0' && *p <= '9') a = a * 10 + (UINT32)(*p++ - '0');
        if (*p == 'x' || *p == 'X') {
            p++;
            while (*p >= '0' && *p <= '9') b = b * 10 + (UINT32)(*p++ - '0');
            m = find_mode(gop, a, b, 0);
            if (m < 0) Print(L"  vga=%dx%d: no such mode here (vga=ask lists them)\r\n", a, b);
        } else if (p != v) m = (int)a;
    }
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info;
    UINTN size;
    if (m < 0 || (UINT32)m >= gop->Mode->MaxMode ||
        EFI_ERROR(uefi_call_wrapper(gop->QueryMode, 4, gop, (UINT32)m, &size, &info)) || !mode_ok(info)) {
        if (m >= 0) Print(L"  vga=%d: not a usable mode (vga=ask lists them)\r\n", m);
        return;
    }
    if ((UINT32)m != gop->Mode->Mode && EFI_ERROR(uefi_call_wrapper(gop->SetMode, 2, gop, (UINT32)m))) {
        Print(L"  mode %d (%dx%d) would not set\r\n", m, info->HorizontalResolution, info->VerticalResolution);
        return;
    }
    Print(L"  screen:       %dx%d (mode %d; vga=%dx%d keeps it)\r\n", gop->Mode->Info->HorizontalResolution,
          gop->Mode->Info->VerticalResolution, m, gop->Mode->Info->HorizontalResolution, gop->Mode->Info->VerticalResolution);
}

// display= (kernel options): which screen vmdos draws on, when the firmware
// has more than one display output (a laptop's panel and its HDMI port, two
// graphics cards). Like vga=, only possible here: the kernel draws on the
// framebuffer of the output picked now. display=ask lists them and asks,
// display=N takes output N, display=external the first external one
// (HDMI, DisplayPort, DVI, VGA), display=internal the built-in panel.
// Outputs are the GOP handles with a device path (the console splitter
// has none); an ACPI _ADR node in it, where the firmware gives one, says
// what kind of connector it is (ACPI _DOD display type, bits 11-8).
#define MAX_OUTS 8
struct Output { EFI_GRAPHICS_OUTPUT_PROTOCOL* gop; CHAR16* path; int type; };
enum { DT_OTHER = 0, DT_CRT = 1, DT_TV = 2, DT_EXTERNAL = 3, DT_PANEL = 4, DT_NONE = -1 };

static int output_type(EFI_DEVICE_PATH* dp) {
    int t = DT_NONE;
    for (int k = 0; dp && !IsDevicePathEnd(dp) && k < 64; k++, dp = NextDevicePathNode(dp)) {
        if (DevicePathNodeLength(dp) < 4) break;
        if (DevicePathType(dp) == ACPI_DEVICE_PATH && DevicePathSubType(dp) == ACPI_ADR_DP && DevicePathNodeLength(dp) >= 8) {
            UINT32 adr;
            copy(&adr, (UINT8*)dp + 4, 4);
            if (adr & 0x80000000u) t = (int)((adr >> 8) & 0xF);    // the _DOD layout
            else t = DT_OTHER;
        }
    }
    return t;
}

static const CHAR16* type_name(int t) {
    switch (t) {
    case DT_CRT: return L"external (VGA)";
    case DT_TV: return L"TV out";
    case DT_EXTERNAL: return L"external (HDMI/DP/DVI)";
    case DT_PANEL: return L"built-in panel";
    case DT_NONE: return L"display adapter";
    default: return L"display output";
    }
}

static int list_outputs(struct Output* o) {
    EFI_GUID gg = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID, dg = DEVICE_PATH_PROTOCOL;
    EFI_HANDLE* hb = NULL;
    UINTN hn = 0;
    int n = 0;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->LocateHandleBuffer, 5, ByProtocol, &gg, NULL, &hn, &hb)) || !hb)
        return 0;
    for (UINTN i = 0; i < hn && n < MAX_OUTS; i++) {
        EFI_DEVICE_PATH* dp = NULL;
        EFI_GRAPHICS_OUTPUT_PROTOCOL* g = NULL;
        if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->HandleProtocol, 3, hb[i], &dg, (void**)&dp)) || !dp) continue;
        if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->HandleProtocol, 3, hb[i], &gg, (void**)&g)) || !g || !g->Mode) continue;
        o[n].gop = g;
        o[n].path = DevicePathToStr(dp);
        o[n].type = output_type(dp);
        n++;
    }
    uefi_call_wrapper(ST_->BootServices->FreePool, 1, hb);
    return n;
}

static EFI_GRAPHICS_OUTPUT_PROTOCOL* choose_display(EFI_GRAPHICS_OUTPUT_PROTOCOL* cur, const char* opts) {
    const char* v = opt_value(opts, "display=");
    if (!v) return cur;
    static struct Output o[MAX_OUTS];
    int n = list_outputs(o), pick = -1;
    int ask = v[0] == 'a' && v[1] == 's' && v[2] == 'k';
    if (n == 0) {
        Print(L"  display=: the firmware lists no display outputs; keeping this screen\r\n");
        if (ask) { Print(L"Press a key to go on (10 s).\r\n"); wait_key_secs(10); }
        return cur;
    }
    if (ask) {
        Print(L"\r\nDisplay outputs (* = this screen):\r\n");
        for (int i = 0; i < n; i++) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* in = o[i].gop->Mode->Info;
            Print(L" %c%d: %s, %dx%d\r\n      %s\r\n", o[i].gop == cur ? L'*' : L' ', i + 1, type_name(o[i].type),
                  in ? in->HorizontalResolution : 0, in ? in->VerticalResolution : 0, o[i].path ? o[i].path : L"?");
        }
        if (n == 1) {
            Print(L"  only one output: nothing to choose (the firmware may drive the others itself)\r\n"
                  L"Press a key to go on (10 s).\r\n");
            wait_key_secs(10);
            return cur;
        }
        Print(L"Number, then Enter (Enter alone or 15 s: keep this screen): ");
        int k = read_mode_number();
        if (k < 1 || k > n) return cur;
        pick = k - 1;
    } else if (v[0] >= '0' && v[0] <= '9') {
        int k = 0;
        for (const char* p = v; *p >= '0' && *p <= '9' && k < 100; p++) k = k * 10 + (*p - '0');
        if (k < 1 || k > n) { Print(L"  display=%d: there are %d outputs (display=ask lists them)\r\n", k, n); return cur; }
        pick = k - 1;
    } else {
        int ext = v[0] == 'e' && v[1] == 'x' && v[2] == 't', in = v[0] == 'i' && v[1] == 'n' && v[2] == 't';
        if (!ext && !in) { Print(L"  display=: ask, a number, external or internal\r\n"); return cur; }
        for (int i = 0; i < n && pick < 0; i++)
            if (ext ? (o[i].type == DT_EXTERNAL || o[i].type == DT_CRT) : o[i].type == DT_PANEL) pick = i;
        if (pick < 0) {
            Print(L"  display=%a: no %s output found (display=ask lists them); keeping this screen\r\n",
                  ext ? "external" : "internal", ext ? L"external" : L"built-in panel");
            return cur;
        }
    }
    EFI_GRAPHICS_OUTPUT_PROTOCOL* g = o[pick].gop;
    if (g == cur) return cur;
    // Light it up: an output the firmware wasn't showing may have no mode set
    // yet (QueryMode says EFI_NOT_STARTED); setting one starts it.
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION* info;
    UINTN size;
    if (uefi_call_wrapper(g->QueryMode, 4, g, g->Mode->Mode, &size, &info) == EFI_NOT_STARTED || !g->Mode->Info ||
        !g->Mode->Info->HorizontalResolution)
        uefi_call_wrapper(g->SetMode, 2, g, g->Mode->Mode < g->Mode->MaxMode ? g->Mode->Mode : 0);
    if (!fit_mode(g)) {
        Print(L"  output %d has no 32-bit mode of 640x400 or more; keeping this screen\r\n", pick + 1);
        return cur;
    }
    Print(L"  display:      output %d, %s, %dx%d\r\n", pick + 1, type_name(o[pick].type),
          g->Mode->Info->HorizontalResolution, g->Mode->Info->VerticalResolution);
    return g;
}

static int usable_after_exit(UINT32 t) {
    return t == EfiConventionalMemory || t == EfiBootServicesCode || t == EfiBootServicesData ||
           t == EfiLoaderCode || t == EfiLoaderData;
}

static int overlaps(UINT64 a, UINT64 alen, UINT64 b, UINT64 blen) { return a < b + blen && b < a + alen; }

// The ACPI RSDP from the firmware's configuration table (2.0 preferred),
// for the kernel's rsdp= (it finds the CPUs in the MADT): 0 if none or
// above 4 GiB. Without the firmware it can't be found later.
static UINT32 acpi_rsdp(EFI_SYSTEM_TABLE* st) {
    static const UINT8 acpi20[16] = { 0x71, 0xe8, 0x68, 0x88, 0xf1, 0xe4, 0xd3, 0x11, 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 };
    static const UINT8 acpi10[16] = { 0x30, 0x2d, 0x9d, 0xeb, 0x88, 0x2d, 0xd3, 0x11, 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d };
    UINT64 found = 0;
    for (int want = 0; want < 2 && !found; want++)
        for (UINTN i = 0; i < st->NumberOfTableEntries && !found; i++) {
            const UINT8* g = (const UINT8*)&st->ConfigurationTable[i].VendorGuid;
            const UINT8* w = want ? acpi10 : acpi20;
            int same = 1;
            for (int k = 0; k < 16; k++) if (g[k] != w[k]) same = 0;
            if (same) found = (UINT64)(UINTN)st->ConfigurationTable[i].VendorTable;
        }
    return found >> 32 ? 0 : (UINT32)found;
}

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE* st) {
    InitializeLib(image, st);
    ST_ = st;
    uefi_call_wrapper(st->BootServices->SetWatchdogTimer, 4, 0, 0, 0, NULL);

    // The graphics mode first (switching it may clear the screen), then
    // say who we are before anything slow happens.
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = setup_gop();
    Print(L"vmdos %a (UEFI loader)\r\n", VMDOS_VERSION);
    if (!gop) { fail(L"no 32-bit graphics mode (GOP) available"); return EFI_UNSUPPORTED; }

    EFI_GUID lip = LOADED_IMAGE_PROTOCOL;
    EFI_LOADED_IMAGE* li;
    uefi_call_wrapper(st->BootServices->HandleProtocol, 3, image, &lip, (void**)&li);
    CHAR16* self = DevicePathToStr(li->FilePath);
    static CHAR16 img_path[512];
    // The folder vmdos.efi is in. FilePath is usually just "\EFI\vmdos\vmdos.efi",
    // but a loader that chainloads us (some GRUB builds) may leave device
    // nodes in front ("HD(1,MBR,...)/\EFI\...") or use '/': keep what follows
    // the last ')', with '\' separators and no doubled ones.
    UINTN len = self ? StrLen(self) : 0, from = 0, cut = 0, n_p = 0;
    for (UINTN i = 0; i < len; i++) if (self[i] == L')') from = i + 1;
    for (UINTN i = from; i < len && n_p < 400; i++) {
        CHAR16 c = self[i] == L'/' ? L'\\' : self[i];
        if (c == L'\\' && n_p && img_path[n_p - 1] == L'\\') continue;
        if (!n_p && c != L'\\') img_path[n_p++] = L'\\';
        img_path[n_p++] = c;
    }
    for (UINTN i = 0; i < n_p; i++) if (img_path[i] == L'\\') cut = i + 1;
    StrCpy(img_path + cut, L"dos.img");

    // Load options -> command line; "debug" alone pauses here.
    static char opts[512];
    int debug = 0;
    {
        CHAR16* o = li->LoadOptions;
        UINTN on = li->LoadOptionsSize / 2, n = 0;
        for (UINTN i = 0; o && i < on && o[i] && n < sizeof opts - 1; i++)
            opts[n++] = (o[i] >= 32 && o[i] < 127) ? (char)o[i] : ' ';
        opts[n] = 0;
        for (UINTN i = 0; i + 5 <= n; i++)
            if (opts[i] == 'd' && opts[i+1] == 'e' && opts[i+2] == 'b' && opts[i+3] == 'u' && opts[i+4] == 'g' &&
                (i + 5 == n || opts[i+5] == ' ')) debug = 1;
    }

    // vmdos.cfg next to vmdos.efi: more kernel options (one line).
    {
        static CHAR16 cfg_path[512];
        UINTN i = 0;
        for (; i < cut; i++) cfg_path[i] = img_path[i];
        StrCpy(cfg_path + cut, L"vmdos.cfg");
        UINTN cs = 0;
        char* cfg = read_file(image, cfg_path, &cs);
        UINTN n = 0;
        while (opts[n]) n++;
        // Word by word, leaving out those already given (GRUB's menu entry
        // passes the same options).
        static char word[128];
        UINTN wn = 0;
        for (UINTN k = 0; cfg && k <= cs; k++) {
            char c = k < cs ? cfg[k] : ' ';
            if (c == '\r' || c == '\n' || c == '\t') c = ' ';
            if (c < 32 || c > 126) continue;
            if (c != ' ') { if (wn < sizeof word - 1) word[wn++] = c; continue; }
            word[wn] = 0;
            if (wn && !has_word(opts, word) && n + wn + 2 < sizeof opts) {
                if (n) opts[n++] = ' ';
                for (UINTN j = 0; j < wn; j++) opts[n++] = word[j];
                opts[n] = 0;
            }
            wn = 0;
        }
        opts[n] = 0;
        if (cfg) Print(L"  options from  %s: %a\r\n", cfg_path, opts);
        else Print(L"  no %s (no options from it)\r\n", cfg_path);
    }
    gop = choose_display(gop, opts);
    apply_vga(gop, opts);
    // C: is the partition vmdos.efi came from when it holds KERNEL.SYS
    // (the kernel's SATA or USB driver reads it); dos.img only for c=ram or else.
    UINT8 esp_sig[16];
    int esp_sig_len;
    UINT64 esp = boot_partition(image, esp_sig, &esp_sig_len);
    int disk_c = file_exists(image, L"\\KERNEL.SYS") && !has_word(opts, "c=ram");
    UINTN disk_size = 0;
    UINT8* disk = NULL;
    // dos.img too when it's there: the RAM disk is the fallback if the
    // kernel can't drive this disk (not SATA, IDE or USB). Without it,
    // floppy.img: booted as A: when the kernel finds no C:.
    int floppy = 0;
    if (disk_c) Print(L"  C: drive:     this partition (LBA %ld), if the kernel's SATA/IDE/USB driver finds it\r\n", esp);
    Print(L"  loading       %s ...\r\n", img_path);
    disk = read_file(image, img_path, &disk_size);
    if (!disk) {
        StrCpy(img_path + cut, L"floppy.img");
        Print(L"  loading       %s ...\r\n", img_path);
        disk = read_file(image, img_path, &disk_size);
        floppy = disk != NULL;
    }
    if (!disk && !disk_c) { fail(L"no KERNEL.SYS on this partition, and neither dos.img nor floppy.img next to vmdos.efi"); return EFI_NOT_FOUND; }
    if (disk) Print(L"  %s %s, %d KiB\r\n", floppy ? L"boot floppy: " : L"disk image:  ", img_path, disk_size >> 10);
    else Print(L"  (no dos.img or floppy.img: nothing to fall back to without C:)\r\n");
    if (esp) {
        UINTN n = 0;
        while (opts[n]) n++;
        char num[24]; int k = 0;
        UINT64 v = esp;
        do { num[k++] = (char)('0' + v % 10); v /= 10; } while (v && k < 20);
        const char* pre = " esp=";
        for (int i = 0; pre[i] && n < sizeof opts - 1; i++) opts[n++] = pre[i];
        while (k && n < sizeof opts - 1) opts[n++] = num[--k];
        if (esp_sig_len && n + 9 + 2 * esp_sig_len < sizeof opts) {
            const char* p2 = " espsig=";
            for (int i = 0; p2[i]; i++) opts[n++] = p2[i];
            for (int i = 0; i < esp_sig_len; i++) {
                opts[n++] = "0123456789abcdef"[esp_sig[i] >> 4];
                opts[n++] = "0123456789abcdef"[esp_sig[i] & 15];
            }
        }
        if (boot_usb && n + 12 < sizeof opts) {
            const char* p3 = " bootdev=usb";
            for (int i = 0; p3[i]; i++) opts[n++] = p3[i];
        }
        opts[n] = 0;
    }

    {
        static CHAR16 dir[512];
        UINTN i = 0;
        for (; i < cut; i++) dir[i] = img_path[i];
        dir[i] = 0;
        load_isos(image, dir);
    }
    Print(L"  graphics:     %dx%d, framebuffer 0x%lx\r\n", gop->Mode->Info->HorizontalResolution,
          gop->Mode->Info->VerticalResolution, (UINT64)gop->Mode->FrameBufferBase);

    // The kernel goes at its link address. If the firmware is using that
    // memory for its own boot-time data, copy it there after ExitBootServices.
    UINTN image_size = kernel_image_end - kernel_image;
    struct MultibootInfo* mbi = alloc_low(4096);
    struct MultibootMmap* mmap = alloc_low(MAX_MMAP * sizeof(struct MultibootMmap));
    struct Handoff* ho = alloc_low(4096);
    UINT8* tramp = alloc_low_type(4096, EfiLoaderCode);
    if (!mbi || !mmap || !ho || !tramp) { fail(L"out of memory below 4 GiB"); return EFI_OUT_OF_RESOURCES; }
    fill(ho, sizeof *ho, 0);
    copy(tramp, tramp_start, tramp_end - tramp_start);

    EFI_PHYSICAL_ADDRESS kaddr = KERNEL_BASE;
    if (!EFI_ERROR(uefi_call_wrapper(st->BootServices->AllocatePages, 4, AllocateAddress, EfiLoaderData,
                                     EFI_SIZE_TO_PAGES(KERNEL_MEM_SIZE), &kaddr))) {
        copy((void*)(UINTN)KERNEL_BASE, kernel_image, image_size);
        fill((UINT8*)(UINTN)KERNEL_BASE + image_size, KERNEL_MEM_SIZE - image_size, 0);
        Print(L"  kernel:       %d KiB at 0x%x\r\n", KERNEL_MEM_SIZE >> 10, KERNEL_BASE);
    } else {
        UINT8* src = alloc_low(image_size);
        if (!src) { fail(L"out of memory below 4 GiB"); return EFI_OUT_OF_RESOURCES; }
        copy(src, kernel_image, image_size);
        ho->src = (UINT32)(UINTN)src;
        ho->dst = KERNEL_BASE;
        ho->copy_len = (UINT32)image_size;
        ho->zero_len = (UINT32)(KERNEL_MEM_SIZE - image_size);
        // Everything that must survive the copy has to be elsewhere.
        UINT64 ours[][2] = { { (UINTN)disk, disk_size }, { (UINTN)mbi, 4096 }, { (UINTN)ho, 4096 },
                             { (UINTN)tramp, 4096 }, { (UINTN)src, image_size },
                             { (UINTN)mmap, MAX_MMAP * sizeof(struct MultibootMmap) } };
        for (UINTN i = 0; i < sizeof ours / sizeof ours[0]; i++)
            if (overlaps(ours[i][0], ours[i][1], KERNEL_BASE, KERNEL_MEM_SIZE)) {
                fail(L"the firmware put the loader's data where the kernel goes (16 MiB)");
                return EFI_OUT_OF_RESOURCES;
            }
        for (int i = 0; i < n_iso; i++)
            if (overlaps((UINTN)isos[i].data, isos[i].size, KERNEL_BASE, KERNEL_MEM_SIZE)) {
                fail(L"the firmware put a CD image where the kernel goes (16 MiB)");
                return EFI_OUT_OF_RESOURCES;
            }
        UINTN ms = 0, k, ds; UINT32 dv;
        uefi_call_wrapper(st->BootServices->GetMemoryMap, 5, &ms, NULL, &k, &ds, &dv);
        ms += 16 * ds;
        UINT8* m = AllocatePool(ms);
        if (m && !EFI_ERROR(uefi_call_wrapper(st->BootServices->GetMemoryMap, 5, &ms, (EFI_MEMORY_DESCRIPTOR*)m, &k, &ds, &dv)))
            for (UINTN off = 0; off < ms; off += ds) {
                EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)(m + off);
                if (overlaps(d->PhysicalStart, d->NumberOfPages * 4096, KERNEL_BASE, KERNEL_MEM_SIZE) &&
                    !usable_after_exit(d->Type)) {
                    Print(L"  memory at 0x%lx is type %d\r\n", d->PhysicalStart, d->Type);
                    fail(L"the memory at 16 MiB, where the kernel goes, belongs to the firmware");
                    return EFI_OUT_OF_RESOURCES;
                }
            }
        Print(L"  kernel:       %d KiB, copied to 0x%x after leaving the firmware\r\n",
              KERNEL_MEM_SIZE >> 10, KERNEL_BASE);
    }
    ho->mbi = (UINT32)(UINTN)mbi;
    ho->entry = KERNEL_ENTRY;

    // Multiboot information.
    fill(mbi, 4096, 0);
    struct MultibootModule* mod = (struct MultibootModule*)((UINT8*)mbi + 512);
    char* cmdline = (char*)mbi + 1024;
    char* modname = (char*)mbi + 3072;
    copy(modname, !disk ? "none" : floppy ? "floppy.img" : "dos.img", 11);
    mod->mod_start = (UINT32)(UINTN)disk;
    mod->mod_end = (UINT32)(UINTN)disk + (UINT32)disk_size;
    mod->string = (UINT32)(UINTN)modname;
    copy(cmdline, "vmdos.efi ", 10);
    copy(cmdline + 10, opts, sizeof opts);
    UINT32 rsdp = acpi_rsdp(st);
    if (rsdp) {
        char* e = cmdline;
        while (*e) e++;
        copy(e, " rsdp=", 6);
        for (int k = 0; k < 8; k++) e[6 + k] = "0123456789abcdef"[(rsdp >> (28 - 4 * k)) & 15];
        e[14] = 0;
    }
    mbi->flags = (1 << 2) | (1 << 3) | (1 << 12);
    mbi->cmdline = (UINT32)(UINTN)cmdline;
    for (int i = 0; i < n_iso; i++) {
        char* nm = modname + 32 * (i + 1);
        copy(nm, isos[i].name, 32);
        mod[i + 1].mod_start = (UINT32)(UINTN)isos[i].data;
        mod[i + 1].mod_end = (UINT32)(UINTN)isos[i].data + (UINT32)isos[i].size;
        mod[i + 1].string = (UINT32)(UINTN)nm;
    }
    mbi->mods_count = 1 + n_iso;
    mbi->mods_addr = (UINT32)(UINTN)mod;
    mbi->fb_addr = gop->Mode->FrameBufferBase;
    mbi->fb_width = gop->Mode->Info->HorizontalResolution;
    mbi->fb_height = gop->Mode->Info->VerticalResolution;
    mbi->fb_pitch = gop->Mode->Info->PixelsPerScanLine * 4;
    mbi->fb_bpp = 32;
    mbi->fb_type = 1;
    int bgr = gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor;
    mbi->fb_color[0] = bgr ? 16 : 0;  mbi->fb_color[1] = 8;
    mbi->fb_color[2] = 8;             mbi->fb_color[3] = 8;
    mbi->fb_color[4] = bgr ? 0 : 16;  mbi->fb_color[5] = 8;

    Print(L"  command line: %a\r\n", cmdline);
    Print(L"Starting the vmdos kernel...\r\n");
    if (debug) { Print(L"debug: press any key to start the kernel.\r\n"); wait_key(); }

    // Leave boot services. After a failed ExitBootServices only GetMemoryMap
    // and ExitBootServices may be called, so the buffer is allocated first.
    UINTN map_size = 0, map_cap, key, desc_size;
    UINT32 desc_ver;
    uefi_call_wrapper(st->BootServices->GetMemoryMap, 5, &map_size, NULL, &key, &desc_size, &desc_ver);
    map_cap = map_size + 64 * (desc_size ? desc_size : 48);
    EFI_MEMORY_DESCRIPTOR* map = AllocatePool(map_cap);
    if (!map) { fail(L"out of memory for the memory map"); return EFI_OUT_OF_RESOURCES; }
    for (int attempt = 0; attempt < 8; attempt++) {
        map_size = map_cap;
        if (EFI_ERROR(uefi_call_wrapper(st->BootServices->GetMemoryMap, 5, &map_size, map, &key, &desc_size, &desc_ver)))
            continue;
        if (!EFI_ERROR(uefi_call_wrapper(st->BootServices->ExitBootServices, 2, image, key)))
            goto exited;
    }
    for (;;) __asm__ volatile("hlt");

exited:
    __asm__ volatile("cli");
    // Memory the kernel may use: free RAM plus what the firmware and this
    // loader used while booting (the kernel keeps clear of the module and
    // the Multiboot structures). Adjacent ranges are merged.
    {
        UINTN count = 0;
        for (UINTN off = 0; off < map_size; off += desc_size) {
            EFI_MEMORY_DESCRIPTOR* d = (EFI_MEMORY_DESCRIPTOR*)((UINT8*)map + off);
            if (!usable_after_exit(d->Type)) continue;
            UINT64 a = d->PhysicalStart, l = d->NumberOfPages * 4096;
            if (count && mmap[count - 1].addr + mmap[count - 1].len == a) { mmap[count - 1].len += l; continue; }
            if (count == MAX_MMAP) break;
            mmap[count].size = sizeof(struct MultibootMmap) - 4;
            mmap[count].addr = a;
            mmap[count].len = l;
            mmap[count].type = 1;
            count++;
        }
        mbi->mmap_addr = (UINT32)(UINTN)mmap;
        mbi->mmap_length = (UINT32)(count * sizeof(struct MultibootMmap));
        mbi->flags |= 1 << 6;
    }
    ((void (__attribute__((sysv_abi)) *)(struct Handoff*))(UINTN)tramp)(ho);
    for (;;) __asm__ volatile("hlt");
}
