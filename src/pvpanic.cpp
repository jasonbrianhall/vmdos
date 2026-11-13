/* pvpanic: telling QEMU/KVM that vmdos stopped (libvirt: <panic model='isa'/>
   or model='pvpanic' for the PCI one). The ISA device is port 505h (QEMU's
   default; read: the events it takes), the PCI one 1B36:0011 (a byte at BAR 0).

   vmdos sends CRASH_LOADED ("the guest is handling its crash"): the host
   logs it (QMP GUEST_CRASHLOADED, libvirt's "crashed" event) and leaves the
   VM running, so the panic screen stays up until Ctrl+Alt+Del. PANICKED
   instead would have libvirt act on <on_crash> (destroy by default):
   "pvpanic=panic" asks for that. A QEMU too old for CRASH_LOADED (before
   5.0) gets nothing unless pvpanic=panic. "pvpanic=off": none of it. */
#include "hw.hpp"
#include "pci.hpp"

enum { EV_PANICKED = 1, EV_CRASH_LOADED = 2 };

namespace {
u16 port;
volatile u8 *mmio;
u8 events;

int hypervisor(void)
{
    u32 a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (c >> 31) & 1;
}
} // namespace

extern "C" void pvpanic_init(void)
{
    if (strstr(cmdline, "pvpanic=off") || !hypervisor()) return;   /* port 505h is a PCH's GPIOs on real boards */
    u8 v = inb(0x505);
    if (v != 0xFF && v && !(v & ~7)) { port = 0x505; events = v; }
    PciDevice d;
    if (!port && pci_find_id(0x1B36, 0x0011, &d)) {
        u32 bar = pci_read(d, 0x10);
        if (!(bar & 1) && (bar & ~0xFu)) {
            pci_write(d, 0x04, pci_read(d, 0x04) | 2);   /* memory on */
            mmio = (volatile u8 *)map_mmio64(bar & ~0xFu, 4096);
            if (mmio) events = mmio[0];
        }
    }
    if (port || mmio)
        kprintf("pvpanic: %s, events %x%s\n", port ? "port 505h" : "PCI", events,
                events & EV_CRASH_LOADED ? "" : " (no CRASH_LOADED: QEMU 5.0+, or pvpanic=panic)");
}

extern "C" void pvpanic_signal(void)
{
    if (!port && !mmio) return;
    u8 ev = 0;
    if (strstr(cmdline, "pvpanic=panic")) ev = events & EV_PANICKED;
    else ev = events & EV_CRASH_LOADED;
    if (!ev) return;
    if (port) outb(port, ev); else mmio[0] = ev;
    kprintf("pvpanic: host told (%s)\n", ev == EV_PANICKED ? "panicked" : "crash handled here");
}
