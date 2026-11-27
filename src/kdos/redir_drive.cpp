/* kdos: a DOS drive that is a network redirector's (vmdos's 9P shares,
   VMDOSNET). vmdos's 9P code (fs9p.cpp) is a DOS network redirector: DOS
   calls it with INT 2Fh AX=11xxh, the file name in the swappable data
   area (SDA), an SFT entry at ES:DI, data at the SDA's DTA. MS-DOS and
   FreeDOS make those calls for a drive whose CDS says "network";
   DOSBox-X's DOS keeps its own drives instead. So this drive makes the
   calls itself, as MS-DOS would: it fills in DOSBox-X's SDA (laid out as
   MS-DOS's) and an SFT entry of its own, and runs INT 2Fh in the guest,
   where the monitor answers it. fs9p is used unchanged.

   Long names (a share VMDOSNET put in with long names, the default):
   programs' INT 21h AH=71h calls for the drive go straight to fs9p in the
   monitor (v86.c, before DOS sees them). DOSBox-X's DOS, with long names
   on, hands this drive long names itself (the shell's DIR, COPY, MD, CD,
   ...): those go to the same code, fs9p_lfn, called directly with the
   registers an INT 21h AX=71xxh would have; files are opened by the short
   name fs9p gives a long one (fs9p_short_path, which also notes the long
   name a new file is to have on the host). */
#include "dosbox.h"
#include "regs.h"
#include "callback.h"
#include "mem.h"
#include "dos_inc.h"
#include "drives.h"

/* SDA fields (MS-DOS 4+ layout; DOSBox-X's SDA has it) */
#define SDA_DTA 0x0C
#define SDA_FN1 0x9E
#define SDA_FN2 0x11E
#define SDA_ENTRY 0x1B3                        /* find first/next: the directory entry found */
#define SDA_SATTR 0x24D
#define SFT_SIZE 0x3B
#define NSFT 16
#define NSEARCH 32
#define BOUNCE 2048

static uint16_t scratch_seg;                   /* bounce buffer, SFTs, search blocks, a CDS */
static uint8_t sft_used[NSFT];
static uint8_t search_used[NSEARCH];

static PhysPt sda(void) { return PhysMake(DOS_SDA_SEG, DOS_SDA_OFS); }
static PhysPt bounce(void) { return PhysMake(scratch_seg, 0); }
static PhysPt sft_at(int i) { return PhysMake(scratch_seg, (uint16_t)(BOUNCE + i * SFT_SIZE)); }
static PhysPt search_at(int i) { return PhysMake(scratch_seg, (uint16_t)(BOUNCE + NSFT * SFT_SIZE + i * 22)); }
static PhysPt cds_at(void) { return PhysMake(scratch_seg, (uint16_t)(BOUNCE + NSFT * SFT_SIZE + NSEARCH * 22)); }

static void scratch_init(void)
{
    if (scratch_seg) return;
    unsigned bytes = BOUNCE + NSFT * SFT_SIZE + NSEARCH * 22 + 0x60;
    scratch_seg = DOS_GetMemory((uint16_t)((bytes + 15) / 16), "9P redirector");
}

static void set_fn(PhysPt at, uint8_t drive, const char *name)
{
    char b[DOS_PATHLENGTH + 8];
    snprintf(b, sizeof b, "%c:\\%s", 'A' + drive, name);
    for (char *p = b; *p; p++) if (*p == '/') *p = '\\';
    MEM_BlockWrite(at, b, strlen(b) + 1);
}

/* ---- long names: fs9p's INT 21h AX=71xxh, called directly ---- */
struct kregs {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp_k, ebx, edx, ecx, eax;
    uint32_t vec, err;
    uint32_t eip, cs, eflags;
    uint32_t esp, ss;
    uint32_t v86_es, v86_ds, v86_fs, v86_gs;
};
extern "C" int fs9p_lfn(kregs *r);
extern "C" int fs9p_short_path(uint32_t lin, char *out);
static PhysPt lp1(void) { return bounce(); }            /* first path */
static PhysPt lp2(void) { return bounce() + 320; }      /* second path (rename) */
static PhysPt lrec(void) { return bounce() + 640; }     /* find data record (318 bytes) */
/* AX=71xxh; 0 or the DOS error (0xFFFF: fs9p didn't take it) */
static uint16_t lfn(uint16_t ax, PhysPt dsdx, PhysPt esdi, uint16_t bx, uint16_t cx, uint16_t si, kregs *o = nullptr)
{
    kregs r;
    memset(&r, 0, sizeof r);
    r.eax = ax; r.ebx = bx; r.ecx = cx; r.esi = si;
    r.v86_ds = dsdx >> 4; r.edx = dsdx & 15;
    r.v86_es = esdi >> 4; r.edi = esdi & 15;
    r.eflags = 0x20202;
    if (!fs9p_lfn(&r)) return 0xFFFF;
    if (o) *o = r;
    return (r.eflags & 1) ? (uint16_t)r.eax : 0;
}

/* INT 2Fh AX=11xxh in the guest. word: pushed first (the redirector reads
   it at SS:SP); ES:DI, CX as given. Returns 0 or the DOS error; *out
   gets AX BX CX DX DI after the call. */
struct rres { uint16_t ax, bx, cx, dx, di; };
static uint16_t redir(uint8_t fn, PhysPt esdi, uint16_t cx, uint16_t word, rres *out = nullptr, uint16_t dx = 0)
{
    CPU_Regs save = cpu_regs;
    Segments ssave = Segs;
    uint16_t old_dta_off = mem_readw(sda() + SDA_DTA), old_dta_seg = mem_readw(sda() + SDA_DTA + 2);
    mem_writew(sda() + SDA_DTA, 0);
    mem_writew(sda() + SDA_DTA + 2, scratch_seg);
    reg_sp -= 2;
    real_writew(SegValue(ss), reg_sp, word);
    reg_ax = (uint16_t)(0x1100 | fn);
    reg_cx = cx;
    reg_dx = dx;
    SegSet16(es, (uint16_t)(esdi >> 4));
    reg_di = (uint16_t)(esdi & 15);
    CALLBACK_RunRealInt(0x2F);
    uint16_t err = (reg_flags & FLAG_CF) ? reg_ax : 0;
    if (out) { out->ax = reg_ax; out->bx = reg_bx; out->cx = reg_cx; out->dx = reg_dx; out->di = reg_di; }
    mem_writew(sda() + SDA_DTA, old_dta_off);
    mem_writew(sda() + SDA_DTA + 2, old_dta_seg);
    cpu_regs = save;
    Segs = ssave;
    return err;
}
static bool fail(uint16_t err) { DOS_SetError(err); return false; }

/* ---------------- files ---------------- */
class Redir_File : public DOS_File {
public:
    int slot;
    uint32_t pos = 0;
    uint8_t drive;
    Redir_File(int s, uint8_t d, uint32_t fl) : slot(s), drive(d)
    {
        PhysPt t = sft_at(slot);
        flags = fl;
        open = true;
        attr = mem_readb(t + 4);
        time = mem_readw(t + 0x0D);
        date = mem_readw(t + 0x0F);
        refCtr = 1;
    }
    bool Read(uint8_t *data, uint16_t *size) override
    {
        uint16_t want = *size, got = 0;
        while (got < want) {
            uint16_t n = (uint16_t)std::min<unsigned>(want - got, BOUNCE);
            mem_writed(sft_at(slot) + 0x15, pos);
            rres o;
            uint16_t e = redir(0x08, sft_at(slot), n, 0, &o);
            if (e) { *size = got; return got ? true : fail(e); }
            MEM_BlockRead(bounce(), data + got, o.cx);
            pos += o.cx; got += o.cx;
            if (o.cx < n) break;
        }
        *size = got;
        return true;
    }
    bool Write(const uint8_t *data, uint16_t *size) override
    {
        uint16_t want = *size, put = 0;
        if (!want) {                                       /* 0 bytes: the file ends here */
            mem_writed(sft_at(slot) + 0x15, pos);
            uint16_t e = redir(0x09, sft_at(slot), 0, 0);
            return e ? fail(e) : true;
        }
        while (put < want) {
            uint16_t n = (uint16_t)std::min<unsigned>(want - put, BOUNCE);
            MEM_BlockWrite(bounce(), data + put, n);
            mem_writed(sft_at(slot) + 0x15, pos);
            rres o;
            uint16_t e = redir(0x09, sft_at(slot), n, 0, &o);
            if (e) { *size = put; return put ? true : fail(e); }
            pos += o.cx; put += o.cx;
            if (o.cx < n) break;
        }
        *size = put;
        return true;
    }
    bool Seek(uint32_t *p, uint32_t type) override
    {
        int32_t off = (int32_t)*p;
        switch (type) {
        case DOS_SEEK_SET: pos = (uint32_t)off; break;
        case DOS_SEEK_CUR: pos += (uint32_t)off; break;
        case DOS_SEEK_END: {
            rres o;
            uint16_t e = redir(0x21, sft_at(slot), (uint16_t)((uint32_t)off >> 16), 0, &o, (uint16_t)off);   /* CX:DX */
            if (e) return fail(e);
            pos = (uint32_t)o.dx << 16 | o.ax;
            break; }
        default: return fail(DOSERR_FUNCTION_NUMBER_INVALID);
        }
        *p = pos;
        return true;
    }
    bool Close() override
    {
        if (refCtr == 1 && open) {
            mem_writew(sft_at(slot), 1);                     /* the last handle: the redirector closes it */
            redir(0x06, sft_at(slot), 0, 0);
            sft_used[slot] = 0;
            open = false;
        }
        return true;
    }
    uint16_t GetInformation(void) override { return (uint16_t)(0x8000 | 0x40 | drive); }   /* remote, not written */
    uint32_t GetSeekPos() override { return pos; }
};

/* ---------------- the drive ---------------- */
class Redir_Drive : public DOS_Drive {
public:
    uint8_t drive;
    uint16_t lfn_h[LFN_FILEFIND_MAX + 2];        /* fs9p's search handle per DOSBox-X LFN search */
    bool lfn_ok;                                  /* the share has long names (fs9p takes AX=71xxh for it) */
    /* name (as DOSBox-X gives it, no drive) to "V:\name" at a */
    void put_path(PhysPt a, const char *name) { set_fn(a, drive, name); }
    bool longnames(void) { return uselfn && lfn_ok; }
    /* the 8.3 path for name, into out (no drive): new names too (create) */
    int short_name(const char *name, char *out)
    {
        if (!longnames()) { strcpy(out, name); return 0; }
        put_path(lp1(), name);
        char full[160];
        int e = fs9p_short_path(lp1(), full);
        if (e == -1) { strcpy(out, name); return 0; }
        if (e) return e;
        strcpy(out, full[2] == '\\' ? full + 3 : full + 2);
        return 0;
    }
    Redir_Drive(uint8_t d) : drive(d)
    {
        for (auto &h : lfn_h) h = 0xFFFF;
        scratch_init();
        set_fn(lp1(), drive, "");
        lfn_ok = lfn(0x71A0, lp1(), lrec(), 0, 0, 0) == 0;   /* volume information: fs9p answers for long-name shares */
        snprintf(info, sizeof info, "9P share (VMDOSNET)");
        strcpy(curdir, "");
    }
    int new_sft(void)
    {
        for (int i = 0; i < NSFT; i++)
            if (!sft_used[i]) {
                sft_used[i] = 1;
                for (int k = 0; k < SFT_SIZE; k++) mem_writeb(sft_at(i) + k, 0);
                mem_writew(sft_at(i), 1);
                return i;
            }
        return -1;
    }
    bool FileOpen(DOS_File **file, const char *lname, uint32_t flags) override
    {
        char name[DOS_PATHLENGTH + 16];
        if (int e = short_name(lname, name)) return fail((uint16_t)e);
        int s = new_sft();
        if (s < 0) return fail(DOSERR_TOO_MANY_OPEN_FILES);
        set_fn(sda() + SDA_FN1, drive, name);
        mem_writew(sft_at(s) + 2, (uint16_t)(flags & 0xFF));
        uint16_t e = redir(0x16, sft_at(s), 0, (uint16_t)(flags & 0xFF));
        if (e) { sft_used[s] = 0; return fail(e); }
        *file = new Redir_File(s, drive, flags);
        (*file)->SetName(name);
        return true;
    }
    bool FileCreate(DOS_File **file, const char *lname, uint16_t attributes) override
    {
        char name[DOS_PATHLENGTH + 16];
        if (int e = short_name(lname, name)) return fail((uint16_t)e);   /* (fs9p notes the long name to give it) */
        int s = new_sft();
        if (s < 0) return fail(DOSERR_TOO_MANY_OPEN_FILES);
        set_fn(sda() + SDA_FN1, drive, name);
        mem_writew(sft_at(s) + 2, 2);
        uint16_t e = redir(0x17, sft_at(s), 0, (uint16_t)(attributes & 0xFF));
        if (e) { sft_used[s] = 0; return fail(e); }
        *file = new Redir_File(s, drive, OPEN_READWRITE);
        (*file)->SetName(name);
        return true;
    }
    bool simple(uint8_t fn, const char *name, uint16_t word = 0, rres *o = nullptr)
    {
        set_fn(sda() + SDA_FN1, drive, name);
        uint16_t e = redir(fn, 0, 0, word, o);
        return e ? fail(e) : true;
    }
    /* one of fs9p's AX=71xxh calls on a path: 0, or the error set */
    bool lcall(uint16_t ax, const char *name, uint16_t bx = 0, uint16_t cx = 0, uint16_t si = 0, kregs *o = nullptr,
               const char *name2 = nullptr)
    {
        put_path(lp1(), name);
        if (name2) put_path(lp2(), name2);
        uint16_t e = lfn(ax, lp1(), name2 ? lp2() : lrec(), bx, cx, si, o);
        return e ? fail(e == 0xFFFF ? DOSERR_ACCESS_DENIED : e) : true;
    }
    bool FileUnlink(const char *name) override { return longnames() ? lcall(0x7141, name) : simple(0x13, name); }
    bool RemoveDir(const char *dir) override { return longnames() ? lcall(0x713A, dir) : simple(0x01, dir); }
    bool MakeDir(const char *dir) override { return longnames() ? lcall(0x7139, dir) : simple(0x03, dir); }
    bool TestDir(const char *dir) override
    {
        if (longnames()) {
            if (!*dir) return true;
            kregs o;
            uint16_t err = dos.errorcode;
            bool ok = lcall(0x7143, dir, 0, 0, 0, &o) && (o.ecx & DOS_ATTR_DIRECTORY);
            dos.errorcode = err;
            return ok;
        }
        set_fn(sda() + SDA_FN1, drive, dir);
        return redir(0x05, 0, 0, 0) == 0;
    }
    bool Rename(const char *oldname, const char *newname) override
    {
        if (longnames()) return lcall(0x7156, oldname, 0, 0, 0, nullptr, newname);
        set_fn(sda() + SDA_FN2, drive, newname);
        return simple(0x11, oldname);
    }
    bool GetFileAttr(const char *name, uint16_t *attr) override
    {
        if (longnames()) {
            kregs o;
            if (!lcall(0x7143, name, 0, 0, 0, &o)) return false;
            *attr = (uint16_t)o.ecx;
            return true;
        }
        rres o;
        if (!simple(0x0F, name, 0, &o)) return false;
        *attr = o.ax;
        return true;
    }
    bool SetFileAttr(const char *name, uint16_t attr) override
    { return longnames() ? lcall(0x7143, name, 1, attr) : simple(0x0E, name, attr); }
    bool GetFileAttrEx(char *, struct stat *) override { return false; }
    unsigned long GetCompressedSize(char *) override { return 0; }
    bool FileExists(const char *name) override
    {
        uint16_t a;
        uint16_t olderr = dos.errorcode;
        bool r = GetFileAttr(name, &a) && !(a & DOS_ATTR_DIRECTORY);
        dos.errorcode = olderr;
        return r;
    }
    bool FileStat(const char *name, FileStat_Block *const st) override
    {
        if (longnames()) {
            kregs a, b, c;
            if (!lcall(0x7143, name, 0, 0, 0, &a) || !lcall(0x7143, name, 2, 0, 0, &b) || !lcall(0x7143, name, 4, 0, 0, &c)) return false;
            st->attr = (uint16_t)a.ecx; st->size = (uint32_t)(b.edx & 0xFFFF) << 16 | (b.eax & 0xFFFF);
            st->time = (uint16_t)c.ecx; st->date = (uint16_t)c.edi;
            return true;
        }
        rres o;
        if (!simple(0x0F, name, 0, &o)) return false;
        st->attr = o.ax; st->size = (uint32_t)o.bx << 16 | o.di; st->time = o.cx; st->date = o.dx;
        return true;
    }
    /* find first / next: the redirector's search block lives in our scratch
       memory (one per search, its number kept in the DTA's dirID) */
    bool result(DOS_DTA &dta)
    {
        PhysPt de = sda() + SDA_ENTRY;
        char fcb[11], name[13];
        for (int i = 0; i < 11; i++) fcb[i] = (char)mem_readb(de + i);
        int k = 0;
        for (int i = 0; i < 8 && fcb[i] != ' '; i++) name[k++] = fcb[i];
        if (fcb[8] != ' ') { name[k++] = '.'; for (int i = 8; i < 11 && fcb[i] != ' '; i++) name[k++] = fcb[i]; }
        name[k] = 0;
        dta.SetResult(name, name, mem_readd(de + 0x1C), 0, mem_readw(de + 0x18), mem_readw(de + 0x16), mem_readb(de + 11));
        return true;
    }
    /* a long-name search: fs9p's 714Eh/714Fh record (DOS times) into the DTA */
    bool lresult(DOS_DTA &dta)
    {
        PhysPt r = lrec();
        char lname[LFN_NAMELENGTH + 1], sname[14];
        MEM_StrCopy(r + 0x2C, lname, LFN_NAMELENGTH);
        MEM_StrCopy(r + 0x130, sname, 13);
        if (!sname[0]) strcpy(sname, lname);                 /* a valid 8.3 name is its own short name */
        dta.SetResult(sname, lname, mem_readd(r + 0x20), mem_readd(r + 0x1C), mem_readw(r + 0x16), mem_readw(r + 0x14),
                      (uint8_t)mem_readd(r));
        return true;
    }
    bool FindFirst(const char *_dir, DOS_DTA &dta, bool fcb_findfirst) override
    {
        uint8_t attr; char pattern[CROSS_LEN];
        dta.GetSearchParams(attr, pattern, false);
        int slot = lfn_filefind_handle;
        if (longnames() && !fcb_findfirst && slot < LFN_FILEFIND_NONE && attr != DOS_ATTR_VOLUME) {
            if (slot > LFN_FILEFIND_MAX) slot = LFN_FILEFIND_MAX + 1;
            if (lfn_h[slot] != 0xFFFF) { lfn(0x71A1, 0, 0, lfn_h[slot], 0, 0); lfn_h[slot] = 0xFFFF; }
            char full[DOS_PATHLENGTH + LFN_NAMELENGTH + 4];
            snprintf(full, sizeof full, "%s%s%s", _dir, *_dir ? "\\" : "", pattern);
            kregs o;
            if (!lcall(0x714E, full, 0, (uint16_t)(attr | DOS_ATTR_ARCHIVE | DOS_ATTR_READ_ONLY), 1, &o)) {
                if (dos.errorcode == DOSERR_FILE_NOT_FOUND) DOS_SetError(DOSERR_NO_MORE_FILES);
                return false;
            }
            lfn_h[slot] = (uint16_t)o.eax;
            return lresult(dta);
        }
        int s = -1;
        for (int i = 0; i < NSEARCH; i++) if (!search_used[i]) { s = i; break; }
        if (s < 0) for (int i = 0; i < NSEARCH; i++) search_used[i] = 0, s = 0;   /* all taken: forget them */
        char full[DOS_PATHLENGTH + 16];
        snprintf(full, sizeof full, "%s%s%s", _dir, *_dir ? "\\" : "", pattern);
        set_fn(sda() + SDA_FN1, drive, full);
        mem_writeb(sda() + SDA_SATTR, attr);
        uint16_t e = redir(0x1B, 0, 0, 0);
        if (e) return fail(e == DOSERR_FILE_NOT_FOUND || e == 0x12 ? DOSERR_NO_MORE_FILES : e);
        /* the redirector put its search block at the DTA (our bounce buffer) */
        MEM_BlockCopy(search_at(s), bounce(), 21);
        search_used[s] = 1;
        dta.SetDirID((uint16_t)s);
        return result(dta);
    }
    bool FindNext(DOS_DTA &dta) override
    {
        int slot = lfn_filefind_handle;
        if (longnames() && slot < LFN_FILEFIND_NONE) {
            if (slot > LFN_FILEFIND_MAX) slot = LFN_FILEFIND_MAX + 1;
            if (lfn_h[slot] != 0xFFFF) {
                if (lfn(0x714F, 0, lrec(), lfn_h[slot], 0, 1)) {
                    lfn(0x71A1, 0, 0, lfn_h[slot], 0, 0);
                    lfn_h[slot] = 0xFFFF;
                    return fail(DOSERR_NO_MORE_FILES);
                }
                return lresult(dta);
            }
        }
        int s = dta.GetDirID();
        if (s < 0 || s >= NSEARCH || !search_used[s]) return fail(DOSERR_NO_MORE_FILES);
        uint16_t e = redir(0x1C, search_at(s), 0, 0);
        if (e) { search_used[s] = 0; return fail(DOSERR_NO_MORE_FILES); }
        return result(dta);
    }
    bool AllocationInfo(uint16_t *bytes_sector, uint8_t *sectors_cluster, uint16_t *total_clusters, uint16_t *free_clusters) override
    {
        PhysPt c = cds_at();
        mem_writeb(c, (uint8_t)('A' + drive)); mem_writeb(c + 1, ':'); mem_writeb(c + 2, '\\'); mem_writeb(c + 3, 0);
        rres o;
        if (redir(0x0C, c, 0, 0, &o)) { *bytes_sector = 512; *sectors_cluster = 64; *total_clusters = 0xFFFE; *free_clusters = 0xFFFE; return true; }
        *sectors_cluster = (uint8_t)o.ax; *total_clusters = o.bx; *bytes_sector = o.cx; *free_clusters = o.dx;
        return true;
    }
    uint8_t GetMediaByte(void) override { return 0xF8; }
    bool isRemote(void) override { return true; }
    bool isRemovable(void) override { return false; }
    Bits UnMount(void) override { delete this; return 0; }
    char const *GetLabel(void) override { return "9P"; }
};

/* vmdos's monitor: VMDOSNET put a share in drive dr (fs9p_api), or took it out */
extern "C" void kdos_net_mounted(int dr)
{
    if (dr < 2 || dr >= 26) return;
    scratch_init();
    if (Drives[dr] && !dynamic_cast<Redir_Drive *>(Drives[dr])) return;   /* a drive of DOS's own: leave it */
    if (!Drives[dr]) Drives[dr] = new Redir_Drive((uint8_t)dr);
    mem_writeb(Real2Phys(dos.tables.mediaid) + dr * 9, 0xF8);
    LOG_MSG("%c: is a 9P share (VMDOSNET)", 'A' + dr);
}
extern "C" void kdos_net_unmounted(int dr)
{
    if (dr < 0 || dr >= 26) return;
    if (Redir_Drive *d = dynamic_cast<Redir_Drive *>(Drives[dr])) { Drives[dr] = nullptr; delete d; }
}
