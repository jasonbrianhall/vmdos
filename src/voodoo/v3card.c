/* 86Box's Voodoo Banshee / Voodoo 3 (vid_voodoo_banshee.c, as upstream)
   and what v3.c needs to see inside it: the card's struct is private to
   that file, so it is built from here with these few accessors after it. */
#include "vid_voodoo_banshee.c"

const device_t *v3_device(void) { return &voodoo_3_3000_device; }
voodoo_t *v3_card_voodoo(void *p) { return ((banshee_t *) p)->voodoo; }
svga_t *v3_card_svga(void *p) { return &((banshee_t *) p)->svga; }
uint32_t v3_card_bar(void *p, int n)
{
    const banshee_t *b = p;
    return n == 0 ? b->memBaseAddr0 : n == 1 ? b->memBaseAddr1 : b->ioBaseAddr;
}
int v3_card_mem_on(void *p) { return !!(((banshee_t *) p)->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_MEM); }
