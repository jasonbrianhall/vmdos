/* ACPI power: the power button, and turning the machine off (S5).

   The power button is ACPI's fixed one: PWRBTN_EN set in PM1_EN, then the
   timer looks at PWRBTN_STS in PM1_STS (no SCI: IRQ 9 stays masked).
   QEMU's system_powerdown (virt-manager's Shut Down, virsh shutdown) is that
   button. A press asks for power-off (power_req); v86.c does it when the
   disks are quiet: vmdos's write caches out, then S5.

   S5: SLP_TYP from the DSDT's \_S5_ package, with SLP_EN, into PM1a_CNT
   (and PM1b_CNT). The firmware isn't asked to switch to ACPI mode (SMI_CMD):
   in legacy mode the power button stays the firmware's on real machines,
   and QEMU takes SLP_EN either way. "acpi=off": none of it. */
#include "kernel.h"

static u16 evt_a, evt_b, cnt_a, cnt_b, evt_len;
static u8 typ_a, typ_b;
static int have_s5, have_button;
volatile int power_req;

/* One element of \_S5_'s package: a ZeroOp, OneOp, BytePrefix or bare byte. */
static const u8 *aml_byte(const u8 *p, u8 *v)
{
    if (*p == 0x0A) { *v = p[1]; return p + 2; }
    *v = *p == 0x01 ? 1 : *p == 0xFF ? 0xFF : *p;   /* ZeroOp 00, OneOp 01 */
    return p + 1;
}

static int find_s5(const u8 *t)
{
    u32 len = *(const u32 *)(t + 4);
    for (u32 i = 36; i + 12 < len; i++) {
        const u8 *p = t + i;
        if (memcmp(p, "_S5_", 4)) continue;
        if (!(p[-1] == 0x08 || (p[-2] == 0x08 && p[-1] == '\\'))) continue;   /* NameOp (\)_S5_ */
        p += 4;
        if (*p != 0x12) continue;                     /* PackageOp */
        p += 1 + ((p[1] & 0xC0) >> 6) + 1 + 1;        /* PackageOp, PkgLength, NumElements */
        p = aml_byte(p, &typ_a);
        aml_byte(p, &typ_b);
        return 1;
    }
    return 0;
}

void acpi_init(void)
{
    if (strstr(cmdline, "acpi=off")) return;
    const u8 *f = acpi_find("FACP");
    if (!f) { kprintf("acpi: no FADT: no power button or power-off\n"); return; }
    u32 flen = *(const u32 *)(f + 4);
    evt_a = (u16)*(const u32 *)(f + 56); evt_b = (u16)*(const u32 *)(f + 60);
    cnt_a = (u16)*(const u32 *)(f + 64); cnt_b = (u16)*(const u32 *)(f + 68);
    evt_len = f[88];
    u32 flags = flen >= 116 ? *(const u32 *)(f + 112) : 0;
    const u8 *dsdt = acpi_table_at(*(const u32 *)(f + 40));
    have_s5 = cnt_a && dsdt && find_s5(dsdt);
    if (!have_s5 && cnt_a) {                          /* an SSDT may hold it */
        const u8 *s = acpi_find("SSDT");
        have_s5 = s && find_s5(s);
    }
    /* PWR_BUTTON (flags bit 4) set: the button is a control-method device,
       which needs an AML interpreter: not here. */
    if (evt_a && evt_len >= 4 && !(flags & (1u << 4))) {
        u16 en = evt_a + evt_len / 2;
        outw(evt_a, 1u << 8);                         /* PWRBTN_STS: clear (write 1) */
        outw(en, inw(en) | 1u << 8);                  /* PWRBTN_EN */
        if (evt_b) { outw(evt_b, 1u << 8); outw(evt_b + evt_len / 2, inw(evt_b + evt_len / 2) | 1u << 8); }
        have_button = 1;
    }
    kprintf("acpi: power button %s, power-off %s (S5 SLP_TYP %u/%u, PM1a_CNT %x)\n",
            have_button ? "on" : "not fixed (control method)", have_s5 ? "on" : "unknown", typ_a, typ_b, cnt_a);
}

/* From the timer: the button pressed. */
void acpi_tick(void)
{
    if (!have_button) return;
    u16 st = inw(evt_a) | (evt_b ? inw(evt_b) : 0);
    if (!(st & 1u << 8)) return;
    outw(evt_a, 1u << 8);
    if (evt_b) outw(evt_b, 1u << 8);
    if (!power_req) {
        kprintf("acpi: power button: turning off when the disks are quiet\n");
        video_osd("Power button: turning off");
        power_req = 1;
    }
}

static int hypervisor(void)
{
    u32 a = 1, b, c, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
    return (c >> 31) & 1;
}

/* Everything out to the disks, then off. Returns only if the machine stayed on. */
int acpi_poweroff(void)
{
    kprintf("acpi: powering off\n");
    fs9p_flush_all();
    disk_flush();
    u32 fl;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(fl) :: "memory");
    if (have_s5) {
        outw(cnt_a, (inw(cnt_a) & ~(7u << 10)) | (u16)(typ_a & 7) << 10 | 1u << 13);
        if (cnt_b) outw(cnt_b, (inw(cnt_b) & ~(7u << 10)) | (u16)(typ_b & 7) << 10 | 1u << 13);
        kdelay_us(200000);
    }
    if (hypervisor()) {                               /* QEMU (ICH9, PIIX), older QEMU/Bochs, VirtualBox */
        outw(0x604, 0x2000);
        outw(0xB004, 0x2000);
        outw(0x4004, 0x3400);
        kdelay_us(200000);
    }
    if (fl & 0x200) sti();
    kprintf("acpi: the machine didn't turn off\n");
    return -1;
}
