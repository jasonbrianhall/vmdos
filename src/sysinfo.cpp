// LSPCI.COM and LSUSB.COM: what's in the PC, as vmdos sees it. DOS can't
// reach the PCI configuration ports (vmdos keeps them; a guest's reads give
// FFh) or the USB controllers (vmdos drives them), so the listing is made
// here, as text.
//
// INT 2Fh AX=5649h ('I'), BX = 564Dh ('VM') on return:
//   BL = 0 the PCI devices, 1 the USB controllers and devices
//   BH = bit 0 verbose (LSPCI: subsystem, command, BARs, IRQ, capabilities;
//        LSUSB: descriptor classes), bit 1 (LSPCI) the first 64 bytes of
//        each function's configuration space in hex
//   EDX = position in the text (0: make it anew), ES:DI = buffer, CX = its size
//   returns CX = bytes copied (CR LF lines; 0: the end), EDX = next position
#include "hw.hpp"
#include "pci.hpp"

extern "C" int usb_list(char* buf, int max, int verbose);

namespace {

const int TEXT_MAX = 64 * 1024;
char* text;
int text_len;

struct Out {
    char* p; int max, n;
    void add(const char* fmt, ...) {
        if (n >= max - 1) return;
        va_list ap;
        va_start(ap, fmt);
        int w = vsnprintf(p + n, (size_t)(max - n), fmt, ap);
        va_end(ap);
        n += w < 0 ? 0 : w;
        if (n > max - 1) n = max - 1;
    }
};

struct ClassName { uint8_t cls, sub; const char* name; };
const ClassName classes[] = {
    { 0x00, 0x00, "Non-VGA unclassified device" }, { 0x00, 0x01, "VGA compatible unclassified device" },
    { 0x01, 0x00, "SCSI storage controller" }, { 0x01, 0x01, "IDE interface" }, { 0x01, 0x02, "Floppy disk controller" },
    { 0x01, 0x04, "RAID bus controller" }, { 0x01, 0x05, "ATA controller" }, { 0x01, 0x06, "SATA controller" },
    { 0x01, 0x07, "Serial Attached SCSI controller" }, { 0x01, 0x08, "Non-Volatile memory controller" },
    { 0x01, 0xFF, "Mass storage controller" },
    { 0x02, 0x00, "Ethernet controller" }, { 0x02, 0x80, "Network controller" }, { 0x02, 0xFF, "Network controller" },
    { 0x03, 0x00, "VGA compatible controller" }, { 0x03, 0x01, "XGA compatible controller" },
    { 0x03, 0x02, "3D controller" }, { 0x03, 0xFF, "Display controller" },
    { 0x04, 0x00, "Multimedia video controller" }, { 0x04, 0x01, "Multimedia audio controller" },
    { 0x04, 0x03, "Audio device" }, { 0x04, 0xFF, "Multimedia controller" },
    { 0x05, 0x00, "RAM memory" }, { 0x05, 0xFF, "Memory controller" },
    { 0x06, 0x00, "Host bridge" }, { 0x06, 0x01, "ISA bridge" }, { 0x06, 0x04, "PCI bridge" },
    { 0x06, 0x05, "PCMCIA bridge" }, { 0x06, 0x07, "CardBus bridge" }, { 0x06, 0x09, "PCI bridge" },
    { 0x06, 0xFF, "Bridge" },
    { 0x07, 0x00, "Serial controller" }, { 0x07, 0x01, "Parallel controller" },
    { 0x07, 0x80, "Communication controller" }, { 0x07, 0xFF, "Communication controller" },
    { 0x08, 0x00, "PIC" }, { 0x08, 0x01, "DMA controller" }, { 0x08, 0x02, "Timer" }, { 0x08, 0x03, "RTC" },
    { 0x08, 0x05, "SD Host controller" }, { 0x08, 0x06, "IOMMU" }, { 0x08, 0xFF, "System peripheral" },
    { 0x09, 0x00, "Keyboard controller" }, { 0x09, 0x02, "Mouse controller" }, { 0x09, 0xFF, "Input device controller" },
    { 0x0C, 0x00, "FireWire (IEEE 1394)" }, { 0x0C, 0x03, "USB controller" }, { 0x0C, 0x05, "SMBus" },
    { 0x0C, 0x07, "IPMI interface" }, { 0x0C, 0x80, "Serial bus controller" }, { 0x0C, 0xFF, "Serial bus controller" },
    { 0x0D, 0x11, "Bluetooth" }, { 0x0D, 0x80, "Wireless controller" }, { 0x0D, 0xFF, "Wireless controller" },
    { 0x10, 0xFF, "Encryption controller" },
    { 0x11, 0x80, "Signal processing controller" }, { 0x11, 0xFF, "Signal processing controller" },
    { 0x12, 0xFF, "Processing accelerators" },
    { 0x13, 0xFF, "Non-Essential Instrumentation" },
};

const char* class_name(int cls, int sub)
{
    for (const ClassName& c : classes) if (c.cls == cls && c.sub == sub) return c.name;
    for (const ClassName& c : classes) if (c.cls == cls && c.sub == 0xFF) return c.name;
    return cls == 0xFF ? "Unassigned class" : "Class";
}

const char* usb_progif(int p)
{
    switch (p) {
    case 0x00: return "UHCI"; case 0x10: return "OHCI"; case 0x20: return "EHCI"; case 0x30: return "xHCI";
    case 0x40: return "USB4"; case 0xFE: return "USB device";
    }
    return 0;
}

struct VendorName { uint16_t id; const char* name; };
const VendorName vendors[] = {
    { 0x8086, "Intel" }, { 0x1022, "AMD" }, { 0x1002, "AMD/ATI" }, { 0x10DE, "NVIDIA" }, { 0x10EC, "Realtek" },
    { 0x14E4, "Broadcom" }, { 0x1969, "Qualcomm Atheros" }, { 0x168C, "Qualcomm Atheros" }, { 0x1106, "VIA" },
    { 0x1039, "SiS" }, { 0x10B9, "ALi" }, { 0x1B21, "ASMedia" }, { 0x1B4B, "Marvell" }, { 0x11AB, "Marvell" },
    { 0x197B, "JMicron" }, { 0x1912, "Renesas" }, { 0x1033, "NEC" }, { 0x104C, "Texas Instruments" },
    { 0x1180, "Ricoh" }, { 0x1217, "O2 Micro" }, { 0x10EE, "Xilinx" }, { 0x144D, "Samsung" }, { 0x15B7, "SanDisk/WD" },
    { 0x1C5C, "SK hynix" }, { 0x126F, "Silicon Motion" }, { 0x1987, "Phison" }, { 0x1179, "Toshiba/Kioxia" },
    { 0x1344, "Micron" }, { 0x8087, "Intel" }, { 0x1AF4, "Red Hat (virtio)" }, { 0x1B36, "Red Hat (QEMU)" },
    { 0x1234, "QEMU" }, { 0x15AD, "VMware" }, { 0x80EE, "VirtualBox" }, { 0x1013, "Cirrus Logic" },
    { 0x5333, "S3" }, { 0x102B, "Matrox" }, { 0x1274, "Ensoniq" }, { 0x1102, "Creative" }, { 0x13F6, "C-Media" },
    { 0x1283, "ITE" }, { 0x1095, "Silicon Image" }, { 0x105A, "Promise" }, { 0x9005, "Adaptec" }, { 0x1000, "LSI/Broadcom" },
    { 0x17AA, "Lenovo" }, { 0x103C, "HP" }, { 0x1028, "Dell" }, { 0x1043, "ASUS" }, { 0x1458, "Gigabyte" },
    { 0x1462, "MSI" }, { 0x1025, "Acer" }, { 0x104D, "Sony" }, { 0x1849, "ASRock" }, { 0x15D9, "Supermicro" },
};

const char* vendor_name(uint16_t id)
{
    for (const VendorName& v : vendors) if (v.id == id) return v.name;
    return 0;
}

const char* cap_name(int id)
{
    switch (id) {
    case 0x01: return "Power Management"; case 0x02: return "AGP"; case 0x03: return "VPD";
    case 0x04: return "Slot ID"; case 0x05: return "MSI"; case 0x06: return "CompactPCI hot swap";
    case 0x07: return "PCI-X"; case 0x08: return "HyperTransport"; case 0x09: return "Vendor Specific";
    case 0x0A: return "Debug port"; case 0x0C: return "Hot-plug"; case 0x0D: return "Subsystem";
    case 0x0E: return "AGP 8x"; case 0x0F: return "Secure device"; case 0x10: return "Express";
    case 0x11: return "MSI-X"; case 0x12: return "SATA"; case 0x13: return "PCI Advanced Features";
    case 0x14: return "Enhanced Allocation"; case 0x15: return "Flattening Portal Bridge";
    }
    return "?";
}

const char* pcie_type(int t)
{
    switch (t) {
    case 0: return "Endpoint"; case 1: return "Legacy Endpoint"; case 4: return "Root Port";
    case 5: return "Upstream Port"; case 6: return "Downstream Port"; case 7: return "PCI-Express to PCI/PCI-X Bridge";
    case 8: return "PCI/PCI-X to PCI-Express Bridge"; case 9: return "Root Complex Integrated Endpoint";
    case 10: return "Root Complex Event Collector";
    }
    return "?";
}

// The BARs' addresses as the firmware set them (not their sizes: sizing a
// BAR means writing it, and vmdos is using some of these devices).
void bars(Out& o, const PciDevice& d, int nbars)
{
    for (int b = 0; b < nbars; b++) {
        uint32_t v = pci_read(d, 0x10 + 4 * b);
        if (!v) continue;
        if (v & 1) { o.add("\tI/O ports at %x\r\n", v & ~3u); continue; }
        bool is64 = (v & 6) == 4;
        uint32_t hi = is64 && b + 1 < nbars ? pci_read(d, 0x14 + 4 * b) : 0;
        if (hi) o.add("\tMemory at %x%08x", hi, v & ~0xFu);
        else o.add("\tMemory at %x", v & ~0xFu);
        o.add(" (%s, %sprefetchable)\r\n", is64 ? "64-bit" : (v & 6) == 2 ? "below 1M" : "32-bit", (v & 8) ? "" : "non-");
        if (is64) b++;
    }
}

void one(Out& o, const PciDevice& d, int flags)
{
    uint32_t id = pci_read(d, 0), cr = pci_read(d, 8), hdr = pci_read(d, 0x0C);
    uint16_t ven = (uint16_t)id, dev = (uint16_t)(id >> 16);
    int cls = cr >> 24, sub = (cr >> 16) & 0xFF, pif = (cr >> 8) & 0xFF, rev = cr & 0xFF, type = (hdr >> 16) & 0x7F;
    const char* vn = vendor_name(ven);
    o.add("%02x:%02x.%u %s [%02x%02x]: ", d.bus, d.dev, d.fn, class_name(cls, sub), cls, sub);
    if (vn) o.add("%s ", vn);
    o.add("[%04x:%04x]", ven, dev);
    if (rev) o.add(" (rev %02x)", rev);
    const char* up = cls == 0x0C && sub == 0x03 ? usb_progif(pif) : 0;
    if (up) o.add(" (%s)", up);
    else if (pif) o.add(" (prog-if %02x)", pif);
    o.add("\r\n");

    if (flags & 1) {
        uint32_t cmd = pci_read(d, 4), intr = pci_read(d, 0x3C);
        if (type == 0) {
            uint32_t ss = pci_read(d, 0x2C);
            if (ss && ss != 0xFFFFFFFFu) {
                const char* sv = vendor_name((uint16_t)ss);
                o.add("\tSubsystem: %s%s[%04x:%04x]\r\n", sv ? sv : "", sv ? " " : "", ss & 0xFFFF, ss >> 16);
            }
        }
        o.add("\tFlags:");
        bool first = true;
        auto flag = [&](const char* s) { o.add("%s %s", first ? "" : ",", s); first = false; };
        if (cmd & 4) flag("bus master");
        if (cmd & 2) flag("memory");
        if (cmd & 1) flag("I/O");
        if (cmd & (1u << 10)) flag("INTx off");
        if ((cmd >> 16) & (1u << 4)) flag("capabilities");
        int pin = (intr >> 8) & 0xFF, line = intr & 0xFF;
        if (pin) o.add("%s IRQ %u (pin %c)", first ? "" : ",", line == 0xFF ? 0u : (unsigned)line, 'A' + pin - 1);
        else if (first) o.add(" none");
        o.add("\r\n");
        if (type == 0) bars(o, d, 6);
        else if (type == 1) {
            bars(o, d, 2);
            uint32_t b = pci_read(d, 0x18);
            o.add("\tBus: primary %02x, secondary %02x, subordinate %02x\r\n", b & 0xFF, (b >> 8) & 0xFF, (b >> 16) & 0xFF);
        }
        if ((cmd >> 16) & (1u << 4)) {                // one a line, as lspci -v
            int cap = pci_read(d, type == 2 ? 0x14 : 0x34) & 0xFC;
            for (int n = 0; cap >= 0x40 && n < 48; n++) {
                uint32_t h = pci_read(d, cap);
                o.add("\tCapabilities: [%02x] %s", cap, cap_name(h & 0xFF));
                if ((h & 0xFF) == 0x10) o.add(" (%s)", pcie_type((h >> 20) & 0xF));
                if ((h & 0xFF) == 0x01) o.add(" (now D%u)", pci_read(d, cap + 4) & 3);
                if ((h & 0xFF) == 0x05) o.add(" (%s)", (h >> 16) & 1 ? "on" : "off");
                if ((h & 0xFF) == 0x11) o.add(" (%s)", (h >> 31) & 1 ? "on" : "off");
                o.add("\r\n");
                cap = (h >> 8) & 0xFC;
            }
        }
    }
    if (flags & 2) {
        for (int r = 0; r < 64; r += 16) {
            o.add("%02x:", r);
            for (int i = 0; i < 16; i += 4) {
                uint32_t v = pci_read(d, r + i);
                for (int k = 0; k < 4; k++) o.add(" %02x", (v >> (8 * k)) & 0xFF);
            }
            o.add("\r\n");
        }
    }
    if (flags & 3) o.add("\r\n");
}

int lspci(char* buf, int max, int flags)
{
    Out o{buf, max, 0};
    int n = 0;
    for (int bus = 0; bus < 256; bus++)
        for (int dv = 0; dv < 32; dv++) {
            PciDevice d{(uint8_t)bus, (uint8_t)dv, 0};
            uint32_t id = pci_read(d, 0);
            if ((id & 0xFFFF) == 0xFFFF || (id & 0xFFFF) == 0) continue;
            int fns = (pci_read(d, 0x0C) >> 16) & 0x80 ? 8 : 1;
            for (int fn = 0; fn < fns; fn++) {
                d.fn = (uint8_t)fn;
                if (fn && ((pci_read(d, 0) & 0xFFFF) == 0xFFFF || (pci_read(d, 0) & 0xFFFF) == 0)) continue;
                one(o, d, flags);
                n++;
            }
        }
    if (!n) o.add("No PCI devices found\r\n");
    return o.n;
}

} // namespace

extern "C" void info_api(struct regs* r)
{
    int fn = BL(r), flags = r->ebx >> 8 & 0xFF;
    BX(r) = 0x564D;
    if (fn > 1) { CX(r) = 0; AX(r) = 1; return; }
    if (!text) text = (char*)phys_alloc(TEXT_MAX);
    if (r->edx == 0)
        text_len = fn == 0 ? lspci(text, TEXT_MAX, flags) : usb_list(text, TEXT_MAX, flags & 1);
    uint32_t pos = r->edx, room = CX(r);
    uint32_t n = pos < (uint32_t)text_len ? (uint32_t)text_len - pos : 0;
    if (n > room) n = room;
    uint8_t* to = (uint8_t*)gptr(LIN(r->v86_es, DI(r)));
    memcpy(to, text + pos, n);
    CX(r) = (uint16_t)n;
    r->edx = pos + n;
    AX(r) = 0;
}
