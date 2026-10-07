// vmdos.efi: UEFI loader for the 32-bit vmdos kernel.
//
// Reads dos.img from the folder vmdos.efi was started from, takes the
// framebuffer from GOP, places the embedded kernel at its link address
// (4 MiB), exits boot services, leaves long mode (efi/tramp.S) and enters
// the kernel with Multiboot information, as GRUB would.
//
// "debug" in the load options waits for a key before handing over; other
// options (e.g. debug=2, nopae) go to the kernel's command line.
#include <efi.h>
#include <efilib.h>

extern const UINT8 kernel_image[], kernel_image_end[];
extern const UINT8 tramp_start[], tramp_end[];
#include "kernel_layout.h"      // KERNEL_BASE, KERNEL_END, KERNEL_ENTRY

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
static EFI_GRAPHICS_OUTPUT_PROTOCOL* setup_gop(void) {
    EFI_GUID guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = NULL;
    if (EFI_ERROR(uefi_call_wrapper(ST_->BootServices->LocateProtocol, 3, &guid, NULL, (void**)&gop)) || !gop)
        return NULL;
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

static int usable_after_exit(UINT32 t) {
    return t == EfiConventionalMemory || t == EfiBootServicesCode || t == EfiBootServicesData ||
           t == EfiLoaderCode || t == EfiLoaderData;
}

static int overlaps(UINT64 a, UINT64 alen, UINT64 b, UINT64 blen) { return a < b + blen && b < a + alen; }

EFI_STATUS efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE* st) {
    InitializeLib(image, st);
    ST_ = st;
    uefi_call_wrapper(st->BootServices->SetWatchdogTimer, 4, 0, 0, 0, NULL);

    EFI_GUID lip = LOADED_IMAGE_PROTOCOL;
    EFI_LOADED_IMAGE* li;
    uefi_call_wrapper(st->BootServices->HandleProtocol, 3, image, &lip, (void**)&li);
    CHAR16* self = DevicePathToStr(li->FilePath);
    static CHAR16 img_path[512];
    UINTN len = self ? StrLen(self) : 0, cut = 0;
    for (UINTN i = 0; i < len && i < 400; i++) if (self[i] == L'\\' || self[i] == L'/') cut = i + 1;
    for (UINTN i = 0; i < cut; i++) img_path[i] = self[i] == L'/' ? L'\\' : self[i];
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

    Print(L"vmdos UEFI loader\r\n  reading %s\r\n", img_path);
    UINTN disk_size = 0;
    UINT8* disk = read_file(image, img_path, &disk_size);
    if (!disk) { fail(L"can't read dos.img (it goes in the same folder as vmdos.efi)"); return EFI_NOT_FOUND; }

    EFI_GRAPHICS_OUTPUT_PROTOCOL* gop = setup_gop();
    if (!gop) { fail(L"no 32-bit graphics mode (GOP) available"); return EFI_UNSUPPORTED; }

    Print(L"vmdos UEFI loader\r\n");
    Print(L"  disk image:   %s, %d KiB at 0x%lx\r\n", img_path, disk_size >> 10, (UINT64)(UINTN)disk);
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
                fail(L"the firmware put the loader's data where the kernel goes (4 MiB)");
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
                    fail(L"the memory at 4 MiB, where the kernel goes, belongs to the firmware");
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
    copy(modname, "dos.img", 8);
    mod->mod_start = (UINT32)(UINTN)disk;
    mod->mod_end = (UINT32)(UINTN)disk + (UINT32)disk_size;
    mod->string = (UINT32)(UINTN)modname;
    copy(cmdline, "vmdos.efi ", 10);
    copy(cmdline + 10, opts, sizeof opts);
    mbi->flags = (1 << 2) | (1 << 3) | (1 << 12);
    mbi->cmdline = (UINT32)(UINTN)cmdline;
    mbi->mods_count = 1;
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
