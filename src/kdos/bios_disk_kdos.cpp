/* kdos: the parts of DOSBox-X src/ints/bios_disk.cpp that its FAT driver uses:
   imageDisk (whose sectors vmdos's disk code gives: kdisk.cpp), the
   disk lists and partition tables. Lines as in DOSBox-X, the rest left out. */
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "dosbox.h"
#include "callback.h"
#include "bios.h"
#include "bios_disk.h"
#include "timer.h"
#include "regs.h"
#include "mem.h"
#include "dos_inc.h" /* for Drives[] */
#include "../dos/drives.h"
#include "ide.h"
#include "cpu.h"
#include "int13.h"
extern bool int13_disk_change_detect_enable;
diskGeo DiskGeometryList[] = {
    /* KB   S  H  C type SS  root clus ID  fatsz */
    { 160,  8, 1, 40, 0, 512,  64, 1, 0xFE, 1},      // IBM PC double density 5.25" single-sided 160KB
    { 180,  9, 1, 40, 0, 512,  64, 1, 0xFC, 2},      // IBM PC double density 5.25" single-sided 180KB
    { 200, 10, 1, 40, 0, 512,  64, 2, 0xFC, 2},      // DEC Rainbow double density 5.25" single-sided 200KB (I think...)
    { 320,  8, 2, 40, 1, 512, 112, 2, 0xFF, 1},      // IBM PC double density 5.25" double-sided 320KB
    { 360,  9, 2, 40, 1, 512, 112, 2, 0xFD, 2},      // IBM PC double density 5.25" double-sided 360KB
    { 400, 10, 2, 40, 1, 512, 112, 2, 0xFD, 2},      // DEC Rainbow double density 5.25" double-sided 400KB (I think...)
    { 640,  8, 2, 80, 3, 512, 112, 2, 0xFB, 2},      // IBM PC double density 3.5" double-sided 640KB
    { 720,  9, 2, 80, 3, 512, 112, 2, 0xF9, 3},      // IBM PC double density 3.5" double-sided 720KB
    {1200, 15, 2, 80, 2, 512, 224, 1, 0xF9, 7},      // IBM PC double density 5.25" double-sided 1.2MB
    {1440, 18, 2, 80, 4, 512, 224, 1, 0xF0, 9},      // IBM PC high density 3.5" double-sided 1.44MB
    {1680, 21, 2, 80, 4, 512,  16, 4, 0xF0, 3},      // IBM PC high density 3.5" double-sided 1.68MB (DMF)
    {2880, 36, 2, 80, 6, 512, 240, 2, 0xF0, 9},      // IBM PC high density 3.5" double-sided 2.88MB

    {1232,  8, 2, 77, 7, 1024,192, 1, 0xFE, 2},      // NEC PC-98 high density 3.5" double-sided 1.2MB "3-mode"
    {1520, 19, 2, 80, 2, 512, 224, 1, 0xF9, 10},      // IBM PC high density 5.25" double-sided 1.52MB (XDF)
    {1840, 23, 2, 80, 4, 512, 224, 1, 0xF0, 12},      // IBM PC high density 3.5" double-sided 1.84MB (XDF)

    {   0,  0, 0,  0, 0,    0,  0, 0,    0, 0}
};

Bitu diskparm0 = 0, diskparm1 = 0;
PhysPt floppyparm0 = 0;/* this is a physical memory address! */
uint16_t imgDTASeg;
RealPt imgDTAPtr;
DOS_DTA *imgDTA;
static bool swapping_requested;

void CMOS_SetRegister(Bitu regNr, uint8_t val); //For setting equipment word

/* 2 floppies and 2 harddrives, max */
bool imageDiskChange[MAX_DISK_IMAGES]={false};
imageDisk *imageDiskList[MAX_DISK_IMAGES]={NULL};
imageDisk *diskSwap[MAX_SWAPPABLE_DISKS]={NULL};
int32_t swapPosition;
int swapInDisksSpecificDrive = -1;
Int13Status imageDisk::Read_Sector(uint32_t head,uint32_t cylinder,uint32_t sector,void * data,unsigned int req_sector_size) {
    uint32_t sectnum;

    if (req_sector_size == 0)
        req_sector_size = sector_size;
    if (req_sector_size != sector_size)
        return Int13Status::SectorNotFound;

    if (sector == 0) {
        LOG_MSG("Attempted to read invalid sector 0.");
        return Int13Status::SectorNotFound;
    }

    sectnum = ( (cylinder * heads + head) * sectors ) + sector - 1L;

    return Read_AbsoluteSector(sectnum, data);
}

Int13Status imageDisk::Read_AbsoluteSector(uint32_t sectnum, void * data) {

    uint64_t bytenum,res;
    int got;

    bytenum = (uint64_t)sectnum * (uint64_t)sector_size;
    if ((bytenum + sector_size) > this->image_length) {
        LOG_MSG("Attempt to read invalid sector in Read_AbsoluteSector for sector %lu.\n", (unsigned long)sectnum);
        return Int13Status::SectorNotFound;
    }
    bytenum += image_base;

    //LOG_MSG("Reading sectors %ld at bytenum %I64d", sectnum, bytenum);

    fseeko64(diskimg,(fseek_ofs_t)bytenum,SEEK_SET);
    res = (uint64_t)ftello64(diskimg);
    if (res != bytenum) {
        LOG_MSG("fseek() failed in Read_AbsoluteSector for sector %lu. Want=%llu Got=%llu\n",
            (unsigned long)sectnum,(unsigned long long)bytenum,(unsigned long long)res);
        return Int13Status::SeekFailed;
    }

    got = (int)fread(data, 1, sector_size, diskimg);
    if ((unsigned int)got != sector_size) {
        LOG_MSG("fread() failed in Read_AbsoluteSector for sector %lu. Want=%u got=%d\n",
            (unsigned long)sectnum,sector_size,(unsigned int)got);
        return Int13Status::ControllerFailure;
    }

    return Int13Status::NoError;
}

Int13Status imageDisk::Write_Sector(uint32_t head,uint32_t cylinder,uint32_t sector,const void * data,unsigned int req_sector_size) {
    uint32_t sectnum;

    if (req_sector_size == 0)
        req_sector_size = sector_size;
    if (req_sector_size != sector_size)
        return Int13Status::SectorNotFound;

    sectnum = ( (cylinder * heads + head) * sectors ) + sector - 1L;

    return Write_AbsoluteSector(sectnum, data);
}


Int13Status imageDisk::Write_AbsoluteSector(uint32_t sectnum, const void *data) {

    uint64_t bytenum;

    bytenum = (uint64_t)sectnum * sector_size;
    if ((bytenum + sector_size) > this->image_length) {
        LOG_MSG("Attempt to read invalid sector in Write_AbsoluteSector for sector %lu.\n", (unsigned long)sectnum);
        return Int13Status::SectorNotFound;
    }
    bytenum += image_base;

    //LOG_MSG("Writing sectors to %ld at bytenum %d", sectnum, bytenum);

    fseeko64(diskimg,(fseek_ofs_t)bytenum,SEEK_SET);
    if ((uint64_t)ftello64(diskimg) != bytenum)
        LOG_MSG("WARNING: fseek() failed in Write_AbsoluteSector for sector %lu\n",(unsigned long)sectnum);

    size_t ret=fwrite(data, sector_size, 1, diskimg);

    return ((ret>0)?Int13Status::NoError:Int13Status::ControllerFailure);

}

void imageDisk::Set_Reserved_Cylinders(Bitu resCyl) {
    reserved_cylinders = resCyl;
}

uint32_t imageDisk::Get_Reserved_Cylinders() {
    return reserved_cylinders;
}

imageDisk::imageDisk(IMAGE_TYPE class_id) : class_id(class_id) {
}

void imageDisk::UpdateFloppyType(void) {
	uint8_t i=0;

	while (DiskGeometryList[i].ksize!=0x0) {
		if ((DiskGeometryList[i].ksize==diskSizeK) || (DiskGeometryList[i].ksize+1==diskSizeK)) {
			floppytype = i;
			LOG_MSG("Updating floppy type to %u BIOS type 0x%02x",floppytype,GetBiosType());
			break;
		}
		i++;
	}
}

imageDisk::~imageDisk()
{
    if(diskimg != NULL) {
        fclose(diskimg);
        diskimg=NULL;
    }
    if (ffdd)
        delete ffdd;
}

/* var_read() on the packed bootstrap fields below is unaligned-safe by design,
 * so -Waddress-of-packed-member is a false positive here. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress-of-packed-member"
#endif
void imageDisk::Set_GeometryForHardDisk()
{ Set_Geometry(255, (uint32_t)(diskSizeK * 2 / (255 * 63)) + 1, 63, 512); } /* kdos: stubbed */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

void imageDisk::Set_Geometry(uint32_t setHeads, uint32_t setCyl, uint32_t setSect, uint32_t setSectSize) {

    uint64_t total_sectors = (uint64_t)setHeads * (uint64_t)setCyl * (uint64_t)setSect;
    sectors = 63; // Default to 63 sectors per track.
    heads = 16;   // Default to 16 heads.

    if(((setSectSize & (setSectSize - 1)) != 0)) {
        LOG_MSG("bios_disk: Invalid sector size %u, must be a power of 2.", setSectSize);
        active = false;
        return;
    }

    if(total_sectors == 0) {
        LOG_MSG("bios_disk: Invalid disk geometry C, H, S = %u, %u, %u", setCyl, setHeads, setSect);
        active = false;
        return;
    }
    else if (total_sectors > 1024ULL * 255ULL * 63ULL) {
        LOG_MSG("bios_disk: Disk geometry C, H, S = %u, %u, %u exceeds 8.4GB disk size, setting to max limits", setCyl, setHeads, setSect);
        cylinders = 1024;
        heads = 255;
        sectors = 63;
        sector_size = setSectSize;
        active = true;
        return;
    }

    // Use the provided geometry, if it is valid
    if(setCyl > 0 && setHeads > 0 && setSect > 0 && (setCyl <= 1024 || IS_PC98_ARCH) && setHeads <= 255 && setSect <= 63) {
        cylinders = setCyl;
        heads = setHeads;
        sectors = setSect;
        sector_size = setSectSize;
        active = true;
        return;
    }

    if (IS_PC98_ARCH) {
        /* TODO: Some early PC-98 SCSI BIOS has its own 4096 cylinder (12-bit) limit */
    }

    if(total_sectors < 17ULL * 1024 * 16)
        sectors = 17;
    else if(total_sectors < 32ULL * 1024 * 16)
        sectors = 32;
    else
        sectors = 63;

    while(heads < 255) {
        uint64_t test_cyl =
            total_sectors / ((uint64_t)heads * sectors);
        if(test_cyl <= 1024)
            break;
        heads <<= 1;
        if(heads > 255) {
            heads = 255;
            break;
        }
    }
    cylinders = (uint32_t)(total_sectors / ((uint32_t)heads * sectors));
    sector_size = setSectSize;
    active = true;
}

void imageDisk::Get_Geometry(uint32_t * getHeads, uint32_t *getCyl, uint32_t *getSect, uint32_t *getSectSize) {
    *getHeads = heads;
    *getCyl = cylinders;
    *getSect = sectors;
    *getSectSize = sector_size;
}

uint8_t imageDisk::GetBiosType(void) {
    if(!hardDrive) {
        return (uint8_t)DiskGeometryList[floppytype].biosval;
    } else return 0;
}

uint32_t imageDisk::getSectSize(void) {
    return sector_size;
}

bool PartitionLoadMBR(std::vector<partTable::partentry_t> &parts,imageDisk *loadedDisk) {
	partTable smbr;

	parts.clear();

	if (loadedDisk->getSectSize() > sizeof(smbr)) return false;
	if (loadedDisk->Read_Sector(0,0,1,&smbr) != Int13Status::NoError) return false;
	if (smbr.magic1 != 0x55 || smbr.magic2 != 0xaa) return false; /* Must have signature */

	/* first copy the main partition table entries */
	for (size_t i=0;i < 4;i++)
		parts.push_back(smbr.pentry[i]);

	/* then, enumerate extended partitions and add the partitions within, doing it in a way that
	 * allows recursive extended partitions */
	{
		size_t i=0;

		do {
			if (parts[i].parttype == 0x05/*extended*/ || parts[i].parttype == 0x0F/*LBA extended*/) {
				unsigned long sect = parts[i].absSectStart;
				unsigned long send = sect + parts[i].partSize;

				/* partitions within an extended partition form a sort of linked list.
				 * first entry is the partition, sector start relative to parent partition.
				 * second entry points to next link in the list. */

				/* parts[i] is the parent partition in this loop.
				 * this loop will add to the parts vector, the parent
				 * loop will continue enumerating through the vector
				 * until all have processed. in this way, extended
				 * partitions will be expanded into the sub partitions
				 * until none is left to do. */

				/* FIXME: Extended partitions within extended partitions not yet tested,
				 *        MS-DOS FDISK.EXE won't generate that. */

				while (sect < send) {
					smbr.magic1 = smbr.magic2 = 0;
					loadedDisk->Read_AbsoluteSector(sect,&smbr);

					if (smbr.magic1 != 0x55 || smbr.magic2 != 0xAA)
						break;
					if (smbr.pentry[0].absSectStart == 0 || smbr.pentry[0].partSize == 0)
						break; // FIXME: Not sure what MS-DOS considers the end of the linked list

					const size_t idx = parts.size();

					/* Right, get this: absolute start sector in entry #0 is relative to this link in the linked list.
					 * The pointer to the next link in the linked list is relative to the parent partition. Blegh. */
					smbr.pentry[0].absSectStart += sect;
					if (smbr.pentry[1].absSectStart != 0)
						smbr.pentry[1].absSectStart += parts[i].absSectStart;

					/* if the partition extends past the parent, then stop */
					if ((smbr.pentry[0].absSectStart+smbr.pentry[0].partSize) > (parts[i].absSectStart+parts[i].partSize))
						break;

					parts.push_back(smbr.pentry[0]);

					/* Based on MS-DOS 5.0, the 2nd entry is a link to the next entry, but only if
					 * start sector is nonzero and type is 0x05/0x0F. I'm not certain if MS-DOS allows
					 * the linked list to go either direction, but for the sake of preventing infinite
					 * loops stop if the link points to a lower sector number. */
					if (idx < 256 && (smbr.pentry[1].parttype == 0x05 || smbr.pentry[1].parttype == 0x0F) &&
						smbr.pentry[1].absSectStart != 0 && smbr.pentry[1].absSectStart > sect) {
						sect = smbr.pentry[1].absSectStart;
					}
					else {
						break;
					}
				}
			}
			i++;
		} while (i < parts.size());
	}

	return true;
}

bool PartitionLoadIPL1(std::vector<_PC98RawPartition> &parts,imageDisk *loadedDisk) {
	unsigned char ipltable[SECTOR_SIZE_MAX];

	parts.clear();

	assert(sizeof(_PC98RawPartition) == 32);
	if (loadedDisk->getSectSize() > sizeof(ipltable)) return false;

	memset(ipltable,0,sizeof(ipltable));
	if (loadedDisk->Read_Sector(0,0,2,ipltable) != Int13Status::NoError) return false;

	const unsigned int max_entries = std::min(16UL,(unsigned long)(loadedDisk->getSectSize() / sizeof(_PC98RawPartition)));

	for (size_t i=0;i < max_entries;i++) {
		const _PC98RawPartition *pe = (_PC98RawPartition*)(ipltable+(i * 32));

		if (pe->mid == 0 && pe->sid == 0 &&
			pe->ipl_sect == 0 && pe->ipl_head == 0 && pe->ipl_cyl == 0 &&
			pe->sector == 0 && pe->head == 0 && pe->cyl == 0 &&
			pe->end_sector == 0 && pe->end_head == 0 && pe->end_cyl == 0)
			continue; /* unused */

		parts.push_back(*pe);
	}

	return true;
}

std::string PartitionIdentifyType(imageDisk *loadedDisk) {
	struct partTable mbrData;

	if (loadedDisk->getSectSize() > sizeof(mbrData))
		return std::string(); /* That would cause a buffer overrun */

	if (loadedDisk->Read_Sector(0,0,1,&mbrData) != Int13Status::NoError)
		return std::string();

	if (!memcmp(mbrData.booter+4,"IPL1",4))
		return "IPL1"; /* PC-98 IPL1 */

	if (mbrData.magic1 == 0x55 && mbrData.magic2 == 0xaa)
		return "MBR"; /* IBM PC MBR */

	return std::string();
}

void LogPrintPartitionTable(const std::vector<_PC98RawPartition> &parts) {
	for (size_t i=0;i < parts.size();i++) {
		const _PC98RawPartition &part = parts[i];

		LOG(LOG_DOSMISC,LOG_DEBUG)("IPL #%u: boot=%u active=%u startchs=%u/%u/%u endchs=%u/%u/%u '%s'",
        (unsigned int)i,(part.mid&0x80)?1:0,(part.sid&0x80)?1:0,
			part.cyl,part.head,part.sector,part.end_cyl,part.end_head,part.end_sector,
			std::string((char*)part.name,sizeof(part.name)).c_str());
	}
}

void LogPrintPartitionTable(const std::vector<partTable::partentry_t> &parts) {
	for (size_t i=0;i < parts.size();i++) {
		const partTable::partentry_t &part = parts[i];

		LOG(LOG_DOSMISC,LOG_DEBUG)("MBR #%u: bootflag=%u parttype=0x%02x beginchs=0x%02x%02x%02x endchs=0x%02x%02x%02x start=%llu size=%llu",
			(unsigned int)i,(part.bootflag&0x80)?1:0,part.parttype,
			part.beginchs[0],part.beginchs[1],part.beginchs[2],
			part.endchs[0],part.endchs[1],part.endchs[2],
			(unsigned long long)part.absSectStart,
			(unsigned long long)part.partSize);
	}
}

