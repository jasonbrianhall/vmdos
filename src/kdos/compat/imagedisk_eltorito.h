#pragma once
/* kdos: DOSBox-X's El Torito floppy emulation isn't here; the class is
   kept as a type so dynamic_cast checks for it compile (and never match) */
#include "bios_disk.h"
class imageDiskElToritoFloppy : public imageDisk {
public:
    imageDiskElToritoFloppy() : imageDisk(ID_EL_TORITO_FLOPPY) {}
    unsigned char CDROM_drive = 0;
    unsigned long cdrom_sector_offset = 0;
    unsigned char floppy_type = 0;
};
