/* kdos: DOSBox-X's cdrom.h, reduced to the CD-ROM interface its MSCDEX and
   ISO drive use. DOSBox-X's image, SDL and host-drive interfaces are left
   out; vmdos's CD drives (cd.c's units) are cdrom_vmdos.cpp's. */
#ifndef __CDROM_INTERFACE__
#define __CDROM_INTERFACE__
#include "dosbox.h"
#include "mem.h"
#include <string.h>
#include <type_traits>

#define CD_FPS	75
#define FRAMES_TO_MSF(f, M,S,F)	{ int value = f; *(F) = value%CD_FPS; value /= CD_FPS; *(S) = value%60; value /= 60; *(M) = value; }
#define MSF_TO_FRAMES(M, S, F)	((M)*60*CD_FPS+(S)*CD_FPS+(F))
#define RAW_SECTOR_SIZE		2352
#define COOKED_SECTOR_SIZE	2048
#define REDBOOK_FRAME_PADDING 150u

enum { CDROM_USE_SDL, CDROM_USE_ASPI, CDROM_USE_IOCTL_DIO, CDROM_USE_IOCTL_DX, CDROM_USE_IOCTL_MCI };

typedef struct SMSF {
	unsigned char min = 0;
	unsigned char sec = 0;
	unsigned char fr = 0;
} TMSF;

typedef struct SCtrl {
	uint8_t out[4];
	uint8_t vol[4];
} TCtrl;

template<typename T1, typename T2>
inline constexpr T1 ceil_udivide(const T1 x, const T2 y) noexcept {
	return (x != 0) ? 1 + ((x - 1) / y) : 0;
}

class CDROM_Interface
{
public:
	enum INTERFACE_TYPE { ID_BASE=0, ID_FAKE, ID_IMAGE };
	virtual ~CDROM_Interface(void) {}
	virtual bool SetDevice(const char* path, int forceCD) = 0;
	virtual bool GetUPC(unsigned char& attr, char* upc) = 0;
	virtual bool GetAudioTracks(int& stTrack, int& end, TMSF& leadOut) = 0;
	virtual bool GetAudioTrackInfo(int track, TMSF& start, unsigned char& attr) = 0;
	virtual bool GetAudioSub(unsigned char& attr, unsigned char& track, unsigned char& index, TMSF& relPos, TMSF& absPos) = 0;
	virtual bool GetAudioStatus(bool& playing, bool& pause) = 0;
	virtual bool GetMediaTrayStatus(bool& mediaPresent, bool& mediaChanged, bool& trayOpen) = 0;
	virtual bool PlayAudioSector(unsigned long start, unsigned long len) = 0;
	virtual bool PauseAudio(bool resume) = 0;
	virtual bool StopAudio(void) = 0;
	virtual void ChannelControl(TCtrl ctrl) = 0;
	virtual bool ReadSectors(PhysPt buffer, bool raw, unsigned long sector, unsigned long num) = 0;
	virtual bool ReadSectorsHost(void* buffer, bool raw, unsigned long sector, unsigned long num) = 0;
	virtual bool LoadUnloadMedia(bool unload) = 0;
	virtual void InitNewMedia(void) {}
	INTERFACE_TYPE class_id = ID_BASE;
	uint8_t subUnit = 0xFF;
private:
	volatile int refcount = 0;
public:
	int Addref() { return ++refcount; }
	int Release() { int ret = --refcount; if (ret == 0) delete this; return ret; }
};

/* only for drive_iso.cpp's type checks: kdos never makes these */
class CDROM_Interface_Fake : public CDROM_Interface {
public:
	bool isEmpty = false;
};
class CDROM_Interface_Image : public CDROM_Interface {
public:
	bool HasDataTrack(void) { return true; }
	bool HasAudioTrack(void) { return false; }
};

int CDROM_AllocateInterface(const char* physicalPath, int forceCD, uint16_t numDrive, CDROM_Interface **cdrom);
int kdos_cd_alloc(const char* physicalPath, CDROM_Interface **cdrom);    /* cdrom_vmdos.cpp */
#endif
