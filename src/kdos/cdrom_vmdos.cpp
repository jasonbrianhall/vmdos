/* kdos: vmdos's CD drives (cd.c's units: ISO/CUE images that VMCD and the
   media console put in, boot modules, real ATAPI/USB drives) as DOSBox-X's
   CDROM_Interface. DOSBox-X's MSCDEX gives each one a drive letter and its
   isoDrive reads the file system, the way DOSBox-X's IMGMOUNT -t iso does:
   no VMCD.SYS or SHSUCDX in this DOS. */
#include "dosbox.h"
#include "cdrom.h"
#include "dos_inc.h"
#include "drives.h"
#include "mem.h"
#include <stdlib.h>
#include <string.h>

extern "C" {
int cd_unit_count(void);
uint32_t cd_unit_gen(int unit);
int cd_unit_present(int unit);
uint32_t cd_unit_sectors(int unit);
int cd_unit_toc(int unit, int *first, int *last, uint32_t *start, uint8_t *ctl);
int cd_unit_read(int unit, uint32_t lba, uint32_t n, uint8_t *buf);
int cd_unit_play(int unit, uint32_t start, uint32_t end);
void cd_unit_pause(int unit);
void cd_unit_resume(int unit);
void cd_unit_stop(int unit);
int cd_unit_audio(int unit, uint32_t *pos);
void cd_unit_letter(int unit, char letter);
}
extern bool kdos_cd_empty_ok;
bool MSCDEX_HasMediaChanged(uint8_t subUnit);
uint8_t MSCDEX_GetSubUnit(char driveLetter);

static void lba_msf(uint32_t lba, TMSF &m)      /* with the 2 s before LBA 0 */
{
	int M, S, F;
	FRAMES_TO_MSF((int)(lba + 150), &M, &S, &F);
	m.min = (unsigned char)M; m.sec = (unsigned char)S; m.fr = (unsigned char)F;
}

class CDROM_Interface_Vmdos : public CDROM_Interface {
public:
	int unit = 0;
	uint32_t seen = 0;          /* cd.c's change count, last asked */
	uint8_t *bounce = nullptr;

	bool SetDevice(const char *path, int) override {
		unit = atoi(path + 6);                  /* "vmdos:N" */
		seen = cd_unit_gen(unit);
		return unit >= 0 && unit < cd_unit_count();
	}
	bool toc(int &first, int &last, uint32_t *start, uint8_t *ctl) {
		return cd_unit_toc(unit, &first, &last, start, ctl) == 0;
	}
	bool GetUPC(unsigned char &attr, char *upc) override { attr = 0; strcpy(upc, "UPC"); return true; }
	bool GetAudioTracks(int &stTrack, int &end, TMSF &leadOut) override {
		uint32_t st[101]; uint8_t ctl[101]; int f, l;
		if (!toc(f, l, st, ctl)) return false;
		stTrack = f; end = l; lba_msf(st[l + 1], leadOut);
		return true;
	}
	bool GetAudioTrackInfo(int track, TMSF &start, unsigned char &attr) override {
		uint32_t st[101]; uint8_t ctl[101]; int f, l;
		if (!toc(f, l, st, ctl) || track < f || track > l) return false;
		lba_msf(st[track], start);
		attr = (unsigned char)(ctl[track] << 4 | ctl[track] >> 4);  /* CONTROL high, ADR low */
		return true;
	}
	bool GetAudioSub(unsigned char &attr, unsigned char &track, unsigned char &index, TMSF &relPos, TMSF &absPos) override {
		uint32_t st[101]; uint8_t ctl[101]; int f, l, t;
		if (!toc(f, l, st, ctl)) return false;
		uint32_t p;
		cd_unit_audio(unit, &p);
		for (t = f; t < l && st[t + 1] <= p; t++) ;
		attr = (unsigned char)(ctl[t] << 4 | ctl[t] >> 4);
		track = (unsigned char)t; index = 1;
		lba_msf(p, absPos);
		{ int M, S, F; FRAMES_TO_MSF((int)(p >= st[t] ? p - st[t] : 0), &M, &S, &F);
		  relPos.min = (unsigned char)M; relPos.sec = (unsigned char)S; relPos.fr = (unsigned char)F; }
		return true;
	}
	bool GetAudioStatus(bool &playing, bool &pause) override {
		uint32_t p;
		int s = cd_unit_audio(unit, &p);
		playing = s == 1; pause = s == 2;
		return true;
	}
	bool GetMediaTrayStatus(bool &mediaPresent, bool &mediaChanged, bool &trayOpen) override {
		mediaPresent = cd_unit_present(unit) != 0;
		uint32_t g = cd_unit_gen(unit);
		mediaChanged = g != seen;
		seen = g;
		trayOpen = !mediaPresent;
		return true;
	}
	bool PlayAudioSector(unsigned long start, unsigned long len) override {
		return cd_unit_play(unit, (uint32_t)start, (uint32_t)(start + len)) == 0;
	}
	bool PauseAudio(bool resume) override {
		if (resume) cd_unit_resume(unit); else cd_unit_pause(unit);
		return true;
	}
	bool StopAudio(void) override { cd_unit_stop(unit); return true; }
	void ChannelControl(TCtrl) override {}
	bool ReadSectorsHost(void *buffer, bool raw, unsigned long sector, unsigned long num) override {
		if (raw) return false;                  /* cooked sectors only */
		return cd_unit_read(unit, (uint32_t)sector, (uint32_t)num, (uint8_t *)buffer) == 0;
	}
	bool ReadSectors(PhysPt buffer, bool raw, unsigned long sector, unsigned long num) override {
		if (raw) return false;
		if (!bounce) bounce = (uint8_t *)malloc(16 * COOKED_SECTOR_SIZE);
		while (num) {
			unsigned long n = num > 16 ? 16 : num;
			if (cd_unit_read(unit, (uint32_t)sector, (uint32_t)n, bounce)) return false;
			MEM_BlockWrite(buffer, bounce, n * COOKED_SECTOR_SIZE);
			buffer += (PhysPt)(n * COOKED_SECTOR_SIZE); sector += n; num -= n;
		}
		return true;
	}
	bool LoadUnloadMedia(bool) override { return true; }
	~CDROM_Interface_Vmdos() { free(bounce); }
};

/* for DOSBox-X's MSCDEX (CDROM_AllocateInterface, dos_mscdex.cpp) */
int kdos_cd_alloc(const char *path, CDROM_Interface **cdrom)
{
	if (strncmp(path, "vmdos:", 6)) return 6;
	CDROM_Interface_Vmdos *cd = new CDROM_Interface_Vmdos;
	(*cdrom = cd)->Addref();
	if (!cd->SetDevice(path, 0)) { cd->Release(); *cdrom = nullptr; return 3; }
	return 0;
}

static int n_cd;
static char cd_letter[16];
static uint32_t cd_seen[16];

/* each of vmdos's CD units a letter, from D: (or the first free one) up */
void kdos_mount_cds(void)
{
	n_cd = cd_unit_count();
	if (n_cd > 16) n_cd = 16;
	int l = 3;
	while (l < 26 && Drives[l]) l++;
	kdos_cd_empty_ok = true;
	for (int u = 0; u < n_cd && l < 26; u++, l++) {
		if (Drives[l]) break;                   /* MSCDEX wants them in a row */
		char path[16];
		snprintf(path, sizeof path, "vmdos:%d", u);
		cd_seen[u] = cd_unit_gen(u);
		int err = 0;
		std::vector<std::string> opts;
		isoDrive *d = new isoDrive((char)('A' + l), path, 0xF8, err, opts);
		if (err) { LOG_MSG("CD drive %d: no drive letter (MSCDEX: %d)", u + 1, err); delete d; break; }
		Drives[l] = d;
		mem_writeb(Real2Phys(dos.tables.mediaid) + l * 9, 0xF8);
		cd_letter[u] = (char)('A' + l);
		cd_unit_letter(u, (char)('A' + l));
		LOG_MSG("%c: is vmdos's CD drive %d", 'A' + l, u + 1);
	}
	kdos_cd_empty_ok = false;
}

/* a disc changed (VMCD, the media console, a real drive): its file system
   again. From the trap, before each INT 21h. */
extern "C" uint32_t kh_ticks(void);
void kdos_cd_check(void)
{
	static uint32_t polled;
	if (n_cd && kh_ticks() - polled >= 2000) {   /* a real drive's tray: asked every 2 s */
		polled = kh_ticks();
		for (int u = 0; u < n_cd; u++) cd_unit_present(u);
	}
	for (int u = 0; u < n_cd; u++) {
		if (!cd_letter[u]) continue;
		uint32_t g = cd_unit_gen(u);
		if (g == cd_seen[u]) continue;
		cd_seen[u] = g;
		isoDrive *d = dynamic_cast<isoDrive *>(Drives[cd_letter[u] - 'A']);
		if (!d) continue;
		uint8_t su = MSCDEX_GetSubUnit(cd_letter[u]);
		MSCDEX_HasMediaChanged(su);            /* MSCDEX's idea of the disc: the new one */
		d->kdosRefresh();
	}
}
