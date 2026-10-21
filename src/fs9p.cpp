// A folder of the host as a DOS drive: virtio-9p (QEMU: -virtfs, or
// -fsdev local,id=fs0,path=DIR,security_model=none -device
// virtio-9p-pci,fsdev=fs0,mount_tag=share) with 9P2000.L, served to DOS as a
// network drive through the redirector interface (INT 2Fh AH=11h), which DOS
// calls for every file operation on a drive whose CDS says "network". The
// monitor takes those calls off INT 2Fh in do_int (v86.c) before the chain
// runs, so nothing is resident in DOS. VMDOSNET.COM (INT 2Fh AX=5652h) puts
// a share in a drive letter.
//
// Names: the host's names are shown to DOS as 8.3 names: a name that is a
// valid 8.3 name already is that name in capitals, any other gets a short
// name made from it and a hash of the whole name (SOME~K3Q.TXT), which stays
// the same whatever else is in the folder. Files and folders made from DOS
// are made in lower case. With /LFN the long-name API (INT 21h AX=71xxh) is
// answered for the drive too, so programs that use it see the real names.
//
// Caching: directory listings are kept (names checked against the folder's
// times after ttl ms, sizes and dates read again after ttl ms); file data is
// kept in 32 KiB blocks (an LRU, checked against the file's time and size
// when it is opened), read ahead when reading straight through; writes are
// gathered per file and sent on close, commit, a seek elsewhere, after
// 500 ms or when DOS is idle (/WT: written straight through).
#include "virtio.hpp"
#include "tcp9p.hpp"

extern "C" {
void fs9p_api(struct regs* r);
int fs9p_redir(struct regs* r);
int fs9p_lfn(struct regs* r);
int fs9p_kopen(const char* path, u32* size);
int fs9p_kread(int h, u32 off, u32 len, void* buf);
void fs9p_kclose(int h);
void fs9p_lfn_done(struct regs* r);
void fs9p_flush_all(void);
void fs9p_idle(void);
}

namespace {

// ---- DOS error codes
enum { E_FNF = 2, E_PNF = 3, E_TOOMANY = 4, E_ACCESS = 5, E_HANDLE = 6, E_NOMEM = 8, E_NOMORE = 18,
       E_GENERAL = 31, E_EXISTS = 80 };

// Linux errno (Rlerror) -> DOS error; nf: what "no such file" is here
int dos_err(int e, int nf)
{
    switch (e) {
    case 0: return 0;
    case 2: return nf;                         // ENOENT
    case 20: return E_PNF;                     // ENOTDIR
    case 17: return E_EXISTS;                  // EEXIST
    case 23: case 24: return E_TOOMANY;        // ENFILE, EMFILE
    case 12: return E_NOMEM;
    default: return E_ACCESS;                  // EACCES, EPERM, EROFS, ENOTEMPTY, EISDIR, EBUSY...
    }
}

// ---- 9P2000.L
enum { Rlerror = 7, Tstatfs = 8, Rstatfs, Tlopen = 12, Rlopen, Tlcreate = 14, Rlcreate, Tgetattr = 24, Rgetattr,
       Tsetattr = 26, Rsetattr, Treaddir = 40, Rreaddir, Tfsync = 50, Rfsync, Tmkdir = 72, Rmkdir, Trenameat = 74,
       Rrenameat, Tunlinkat = 76, Runlinkat, Tversion = 100, Rversion, Tattach = 104, Rattach, Twalk = 110, Rwalk,
       Tread = 116, Rread, Twrite = 118, Rwrite, Tclunk = 120, Rclunk, Tremove = 122, Rremove,
       Trename = 20, Rrename };
enum { L_RDONLY = 0, L_WRONLY = 1, L_RDWR = 2, L_CREAT = 0x40, L_EXCL = 0x80, L_TRUNC = 0x200, L_DIRECTORY = 0x10000 };
enum { SA_MODE = 1, SA_SIZE = 8, SA_ATIME = 0x10, SA_MTIME = 0x20, SA_ATIME_SET = 0x80, SA_MTIME_SET = 0x100 };
const u16 NOTAG = 0xFFFF;
const u32 NOFID = 0xFFFFFFFFu;

inline u16 g16(const u8* p) { return (u16)(p[0] | p[1] << 8); }
inline u32 g32(const u8* p) { return g16(p) | (u32)g16(p + 2) << 16; }
inline u64 g64(const u8* p) { return g32(p) | (u64)g32(p + 4) << 32; }
inline void p32(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }

struct W {                                     // a message being built
    u8* b;
    u32 n;
    W(u8* buf, u8 type, u16 tag) : b(buf), n(7) { b[4] = type; b[5] = (u8)tag; b[6] = (u8)(tag >> 8); }
    W& w8(u8 v) { b[n++] = v; return *this; }
    W& w16(u16 v) { b[n++] = (u8)v; b[n++] = (u8)(v >> 8); return *this; }
    W& w32(u32 v) { p32(b + n, v); n += 4; return *this; }
    W& w64(u64 v) { w32((u32)v); return w32((u32)(v >> 32)); }
    W& ws(const char* s, u32 len) { w16((u16)len); memcpy(b + n, s, len); n += len; return *this; }
    W& ws(const char* s) { return ws(s, (u32)strlen(s)); }
    void end() { p32(b, n); }
};

// ---- the devices: one virtqueue, a big request slot (reads, writes,
// listings) and NSLOT small ones sent together (attributes of a listing)
const int MAXDEV = 8, NSLOT = 16, NFIDFREE = 128;
const u32 SLOT = 8192;
const u32 MSIZE = 135168;                      // 128 KiB of data per read or write, and headers

struct Dev {
    VirtioDev v;
    Virtq q;
    char tag[64];                              // virtio: the mount tag; TCP: host:port, as shown
    u32 msize;
    u8 *btx, *brx, *stx, *srx;
    u32 root, fid_next;
    u32 fids[NFIDFREE];
    int nfids;
    bool up, broken;
    PciDevice pci;
    u32 gen;                                   // attaches so far: fids of an older one are gone
    // TCP (VMDOSNET V: host[:port]): the connection, what to attach to, the requests sent
    bool tcp, gone, reviving;
    int conn;
    Tcp9pCfg net;
    char aname[128];
    u32 uid, last_try;
    int npend;
    u16 pend_tag[NSLOT + 1];
    int pend_slot[NSLOT + 1];
};
Dev devs[MAXDEV];
int ndev;
bool probed;

inline u8* txp(Dev& d, int s) { return s < 0 ? d.btx : d.stx + s * SLOT; }
inline u8* rxp(Dev& d, int s) { return s < 0 ? d.brx : d.srx + s * SLOT; }

bool dev_revive(Dev& d);

void post(Dev& d, int s)
{
    int di = s < 0 ? 2 * NSLOT : 2 * s;
    u8 *tx = txp(d, s), *rx = rxp(d, s);
    rx[4] = 0;
    if (d.tcp) {
        if (d.broken) return;
        if (d.npend <= NSLOT) { d.pend_tag[d.npend] = g16(tx + 5); d.pend_slot[d.npend++] = s; }
        if (!tcp9p_send(d.conn, tx, g32(tx))) d.broken = true;
        return;
    }
    volatile VirtqDesc* ds = d.q.desc();
    ds[di].addr = (u32)(uintptr_t)tx; ds[di].len = g32(tx); ds[di].flags = VQ_NEXT; ds[di].next = (u16)(di + 1);
    ds[di + 1].addr = (u32)(uintptr_t)rx; ds[di + 1].len = s < 0 ? d.msize : SLOT;
    ds[di + 1].flags = VQ_WRITE; ds[di + 1].next = 0;
    d.q.push((u16)di);
}

// TCP: the answers to what was posted come in on the stream, matched by tag
bool run_tcp(Dev& d, int n)
{
    int got = 0;
    u32 last = ticks;
    while (got < n && !d.broken) {
        if (!tcp9p_pump(d.conn)) {
            kprintf("9p: %s: the connection is gone\n", d.tag);
            d.broken = true;
            break;
        }
        u32 have;
        u8* p = tcp9p_data(d.conn, &have);
        while (have >= 7) {
            u32 size = g32(p);
            if (size < 7 || size > MSIZE + 4096) { kprintf("9p: %s: a bad message\n", d.tag); d.broken = true; break; }
            if (have < size) break;
            u16 tag = g16(p + 5);
            for (int i = 0; i < d.npend; i++) {
                if (d.pend_tag[i] != tag) continue;
                int s = d.pend_slot[i];
                u32 cap = s < 0 ? d.msize : SLOT;
                if (size <= cap) memcpy(rxp(d, s), p, size);
                d.pend_tag[i] = 0xFFFE;
                got++;
                break;
            }
            tcp9p_consume(d.conn, size);
            p = tcp9p_data(d.conn, &have);
            last = ticks;
        }
        if ((u32)(ticks - last) > 30000) {
            kprintf("9p: %s: no answer from the server in 30 s\n", d.tag);
            d.broken = true;
        }
        kdelay_us(2);
    }
    d.npend = 0;
    return got == n;
}

bool run(Dev& d, int n)                       // the n requests posted: send, wait for all
{
    if (d.tcp) {
        if (d.broken) { d.npend = 0; return false; }
        return run_tcp(d, n);
    }
    if (d.broken) return false;
    virtio_notify(d.v, 0, d.q);
    int got = 0;
    for (u32 t = 0; got < n; ) {
        if (d.q.has_used()) { d.q.take(); got++; continue; }
        if (++t > 30000000) {                  // ~30 s
            kprintf("9p: %s: no answer from the host, drive given up\n", d.tag);
            d.broken = true;
            return false;
        }
        if (!(t & 63)) tick_poll();
        kdelay_us(1);
    }
    return true;
}

int reply(Dev& d, int s, u8 want)              // 0, or the errno of the answer in slot s
{
    u8* rx = rxp(d, s);
    if (rx[4] == want) return 0;
    if (rx[4] == Rlerror) { int e = (int)g32(rx + 7); return e ? e : 5; }
    return 5;                                  // EIO
}

int rpc(Dev& d, u8 want)                       // the big slot
{
    if (d.broken && d.tcp && !d.reviving) dev_revive(d);
    if (d.broken) return 5;
    post(d, -1);
    if (!run(d, 1)) return 5;
    return reply(d, -1, want);
}

u32 fid_get(Dev& d) { return d.nfids ? d.fids[--d.nfids] : d.fid_next++; }
void fid_put(Dev& d, u32 f) { if (d.nfids < NFIDFREE) d.fids[d.nfids++] = f; }

int p_clunk(Dev& d, u32 fid)
{
    W w(d.btx, Tclunk, 1); w.w32(fid).end();
    int e = rpc(d, Rclunk);
    fid_put(d, fid);
    return e;
}

// *out: a new fid for path ("a/b", "": a clone) from fid. 0 or errno.
int p_walk(Dev& d, u32 fid, const char* path, u32* out)
{
    u32 nf = fid_get(d);
    const char* p = path;
    bool first = true;
    do {
        W w(d.btx, Twalk, 1);
        w.w32(first ? fid : nf).w32(nf);
        u32 at = w.n;
        w.w16(0);
        int k = 0;
        while (*p && k < 16) {
            const char* e = p;
            while (*e && *e != '/') e++;
            w.ws(p, (u32)(e - p));
            k++;
            p = *e ? e + 1 : e;
        }
        w.b[at] = (u8)k;
        w.end();
        int err = rpc(d, Rwalk);
        if (!err && g16(d.brx + 7) < k) err = 2;    // a name not there: ENOENT
        if (err) {
            if (!first) p_clunk(d, nf);
            else fid_put(d, nf);
            return err;
        }
        first = false;
    } while (*p);
    *out = nf;
    return 0;
}

struct Attr { u8 qtype; u64 qpath; u32 mode; u64 size, mt, mtn, ct, ctn, at; };
void parse_attr(const u8* rx, Attr& a)
{
    a.qtype = rx[15]; a.qpath = g64(rx + 20); a.mode = g32(rx + 28); a.size = g64(rx + 56);
    a.at = g64(rx + 80); a.mt = g64(rx + 96); a.mtn = g64(rx + 104); a.ct = g64(rx + 112); a.ctn = g64(rx + 120);
}
void m_getattr(u8* tx, u16 tag, u32 fid) { W w(tx, Tgetattr, tag); w.w32(fid).w64(0x7FF).end(); }
int p_getattr(Dev& d, u32 fid, Attr& a)
{
    m_getattr(d.btx, 1, fid);
    int e = rpc(d, Rgetattr);
    if (!e) parse_attr(d.brx, a);
    return e;
}
int p_lopen(Dev& d, u32 fid, u32 flags) { W w(d.btx, Tlopen, 1); w.w32(fid).w32(flags).end(); return rpc(d, Rlopen); }
int p_setattr(Dev& d, u32 fid, u32 valid, u32 mode, u64 size, u64 mtime)
{
    W w(d.btx, Tsetattr, 1);
    w.w32(fid).w32(valid).w32(mode).w32(0).w32(0).w64(size).w64(0).w64(0).w64(mtime).w64(0).end();
    return rpc(d, Rsetattr);
}
// bytes read (data at d.brx + 11), or -errno
long p_read(Dev& d, u32 fid, u64 off, u32 n)
{
    if (n > d.msize - 24) n = d.msize - 24;
    W w(d.btx, Tread, 1); w.w32(fid).w64(off).w32(n).end();
    int e = rpc(d, Rread);
    if (e) return -e;
    u32 c = g32(d.brx + 7);
    return c > n ? n : c;
}
// n bytes at d.btx + 23 written: bytes written, or -errno
long p_write(Dev& d, u32 fid, u64 off, u32 n)
{
    W w(d.btx, Twrite, 1); w.w32(fid).w64(off).w32(n);
    w.n += n;
    w.end();
    int e = rpc(d, Rwrite);
    return e ? -e : (long)g32(d.brx + 7);
}
int p_path_fid(Dev& d, const char* hp, u32* fid) { return p_walk(d, d.root, hp, fid); }

// ---- the device's setup
bool dev_attach(Dev& d)
{
    W w(d.btx, Tversion, NOTAG); w.w32(MSIZE).ws("9P2000.L").end();
    d.msize = 8192;                            // until the answer: what it may write back
    int e = rpc(d, Rversion);
    if (e) { kprintf("9p: %s: Tversion failed (%d)\n", d.tag, e); return false; }
    d.msize = g32(d.brx + 7);
    if (d.msize > MSIZE) d.msize = MSIZE;
    if (d.msize < 8192 || g16(d.brx + 11) != 8 || memcmp(d.brx + 13, "9P2000.L", 8)) {
        kprintf("9p: %s: the host doesn't speak 9P2000.L\n", d.tag);
        return false;
    }
    d.root = 1;
    d.fid_next = 2;
    d.nfids = 0;
    d.gen++;                                   // the fids open files had are gone
    W a(d.btx, Tattach, 1); a.w32(d.root).w32(NOFID).ws("root").ws(d.aname).w32(d.uid).end();
    e = rpc(d, Rattach);
    if (e) { kprintf("9p: %s: Tattach failed (%d)\n", d.tag, e); return false; }
    d.up = true;
    return true;
}

// TCP: the connection again (the server restarted, the network went away
// for a while); at most every 2 s. Open files are lost; paths work again.
bool dev_revive(Dev& d)
{
    if (!d.tcp || d.gone || (u32)(ticks - d.last_try) < 2000) return !d.broken;
    d.last_try = ticks;
    d.reviving = true;
    tcp9p_close(d.conn);
    d.net.local_port = (u16)(0xC000 | ((d.net.local_port + 1) & 0x3FFF));
    d.conn = tcp9p_open(d.net);
    bool ok = d.conn >= 0;
    if (ok) {
        d.broken = false;
        ok = dev_attach(d);
        if (!ok) { tcp9p_close(d.conn); d.conn = -1; }
    }
    d.broken = !ok;
    d.reviving = false;
    kprintf("9p: %s: %s\n", d.tag, ok ? "connected again (open files are lost)" : "still no connection");
    return ok;
}

void dev_probe(const PciDevice& pd)
{
    if (ndev >= MAXDEV) return;
    Dev& d = devs[ndev];
    memset(&d, 0, sizeof d);
    d.pci = pd;
    if (!virtio_open(d.v, pd)) return;
    u32 got;
    if (!virtio_features(d.v, 1, &got)) { virtio_fail(d.v); return; }   // VIRTIO_9P_MOUNT_TAG
    if (!virtio_queue(d.v, 0, d.q, 64)) { kprintf("9p: no request queue\n"); virtio_fail(d.v); return; }
    if (got & 1) {
        int len = virtio_cfg8(d.v, 0) | virtio_cfg8(d.v, 1) << 8;
        for (int i = 0; i < len && i < 63; i++) d.tag[i] = (char)virtio_cfg8(d.v, 2 + i);
    }
    if (!d.tag[0]) snprintf(d.tag, sizeof d.tag, "9p%d", ndev);
    d.btx = (u8*)phys_try_alloc(MSIZE); d.brx = (u8*)phys_try_alloc(MSIZE);
    d.stx = (u8*)phys_try_alloc(NSLOT * SLOT); d.srx = (u8*)phys_try_alloc(NSLOT * SLOT);
    if (!d.btx || !d.brx || !d.stx || !d.srx) { kprintf("9p: out of memory\n"); virtio_fail(d.v); return; }
    virtio_ready(d.v);
    tick_poll_begin();
    dev_attach(d);
    tick_poll_end();
    kprintf("9p %02x:%02x.%u%s: tag \"%s\", msize %u%s\n", pd.bus, pd.dev, pd.fn, d.v.modern ? "" : " (legacy)",
            d.tag, d.msize, d.up ? "" : ", not usable");
    ndev++;
}

void probe_all()
{
    if (probed) return;
    probed = true;
    for (int bus = 0; bus < 256; bus++)
        for (int dv = 0; dv < 32; dv++) {
            PciDevice pd{(uint8_t)bus, (uint8_t)dv, 0};
            uint32_t id0 = pci_read(pd, 0);
            if ((id0 & 0xFFFF) == 0xFFFF) continue;
            int fns = (pci_read(pd, 0x0C) >> 16) & 0x80 ? 8 : 1;
            for (int fn = 0; fn < fns; fn++) {
                pd.fn = (uint8_t)fn;
                uint32_t id = pci_read(pd, 0);
                if ((id & 0xFFFF) == 0x1AF4 && ((id >> 16) == 0x1009 || (id >> 16) == 0x1049)) dev_probe(pd);
            }
        }
}

// ---- the drives
enum { MF_LFN = 1, MF_WT = 2, MF_RO = 4 };
struct Mount {
    bool used;
    u8 dev, flags;
    u32 ttl;                                   // ms
    u32 sda, lol, cds;                         // linear: DOS's swappable data area, list of lists, this drive's CDS
};
Mount mnts[26];
int nmnt;

// 64-bit by 32-bit division without libgcc's helpers (the kernel links none)
u64 div64(u64 n, u32 d, u32* rem = 0)
{
    u32 hi = (u32)(n >> 32), lo_ = (u32)n, qh = hi / d, r = hi % d, ql;
    __asm__("divl %4" : "=a"(ql), "=d"(r) : "a"(lo_), "d"(r), "rm"(d));
    if (rem) *rem = r;
    return (u64)qh << 32 | ql;
}

// ---- time: the host's seconds since 1970 (taken as the PC's clock time,
// as QEMU's RTC is UTC by default) <-> DOS date and time
void to_dos(u64 s, u16* t, u16* dt)
{
    if ((s >> 63) || s < 315532800ull) s = 315532800ull;      // before 1980: 1980-01-01
    u32 sec;
    u32 days = (u32)div64(s, 86400, &sec);
    // days -> civil (Howard Hinnant)
    u32 z = days + 719468, era = z / 146097, doe = z - era * 146097;
    u32 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    u32 y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    u32 mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    if (y > 2107) { y = 2107; m = 12; d = 31; }
    *dt = (u16)((y - 1980) << 9 | m << 5 | d);
    *t = (u16)((sec / 3600) << 11 | ((sec / 60) % 60) << 5 | (sec % 60) / 2);
}
u64 from_dos(u16 t, u16 dt)
{
    u32 y = (dt >> 9) + 1980, m = (dt >> 5) & 15, d = dt & 31;
    if (m < 1) m = 1;
    if (d < 1) d = 1;
    if (m <= 2) y--;
    u32 era = y / 400, yoe = y - era * 400;
    u32 doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    u64 days = (u64)era * 146097 + doe - 719468;
    return days * 86400 + (t >> 11) * 3600 + ((t >> 5) & 63) * 60 + (t & 31) * 2;
}
u64 filetime(u64 s) { return (s + 11644473600ull) * 10000000ull; }

// ---- 8.3 names
inline char up(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
inline char lo(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
bool ok83(char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return true;
    for (const char* q = "!#$%&'()-@^_`{}~"; *q; q++) if (*q == c) return true;
    return false;
}
bool is_device(const char* sn)               // CON, NUL, COM1... (as an 11-byte FCB name)
{
    static const char* const dev[] = { "CON", "PRN", "AUX", "NUL", "CLOCK$", "COM1", "COM2", "COM3", "COM4",
                                       "LPT1", "LPT2", "LPT3", 0 };
    for (int i = 0; dev[i]; i++) {
        int n = (int)strlen(dev[i]), k = 0;
        while (k < n && sn[k] == dev[i][k]) k++;
        if (k == n && (n == 8 || sn[n] == ' ')) return true;
    }
    return false;
}
// A valid 8.3 name (any case): its FCB form, upper case
bool valid83(const char* s, int n, char sn[11])
{
    memset(sn, ' ', 11);
    int i = 0, k = 0;
    if (n < 1 || s[0] == '.') return false;
    for (; i < n && s[i] != '.'; i++) {
        if (k >= 8 || !ok83(s[i])) return false;
        sn[k++] = up(s[i]);
    }
    if (i < n) {                               // the extension
        i++;
        if (i == n) return false;              // "NAME."
        for (k = 8; i < n; i++) {
            if (k >= 11 || !ok83(s[i])) return false;
            sn[k++] = up(s[i]);
        }
    }
    return !is_device(sn);
}
u32 fnv(const char* s, int n, u32 salt)
{
    u32 h = 2166136261u ^ (salt * 0x9E3779B9u);
    for (int i = 0; i < n; i++) { h ^= (u8)s[i]; h *= 16777619u; }
    return h;
}
// A short name for a long one: up to 4 characters of it, ~, 3 of a hash
void mangle(const char* s, int n, u32 salt, char sn[11])
{
    memset(sn, ' ', 11);
    int dot = -1;
    for (int i = n - 1; i > 0; i--) if (s[i] == '.') { dot = i; break; }
    int k = 0;
    for (int i = 0; i < (dot < 0 ? n : dot) && k < 4; i++) {
        char c = s[i];
        if (c == ' ' || c == '.') continue;
        sn[k++] = ok83(c) ? up(c) : '_';
    }
    if (!k) sn[k++] = '_';
    sn[k++] = '~';
    u32 h = fnv(s, n, salt) % 46656;
    static const char b36[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    sn[k++] = b36[h / 1296]; sn[k++] = b36[(h / 36) % 36]; sn[k++] = b36[h % 36];
    if (dot >= 0)
        for (int i = dot + 1, e = 8; i < n && e < 11; i++) {
            char c = s[i];
            if (c == ' ' || c == '.') continue;
            sn[e++] = ok83(c) ? up(c) : '_';
        }
}
// FCB form of a path component ("NAME.EXT", "*.TXT"): false if it isn't one
bool to_fcb(const char* s, int n, char out[11])
{
    memset(out, ' ', 11);
    if (n == 1 && s[0] == '.') { out[0] = '.'; return true; }
    if (n == 2 && s[0] == '.' && s[1] == '.') { out[0] = out[1] = '.'; return true; }
    int i = 0, k = 0;
    for (; i < n && s[i] != '.'; i++) {
        if (s[i] == '*') { while (k < 8) out[k++] = '?'; continue; }
        if (k >= 8) return false;
        out[k++] = up(s[i]);
    }
    if (i < n) {
        for (i++, k = 8; i < n; i++) {
            if (s[i] == '.') return false;
            if (s[i] == '*') { while (k < 11) out[k++] = '?'; continue; }
            if (k >= 11) return false;
            out[k++] = up(s[i]);
        }
    }
    return true;
}
int fcb_to_str(const char* sn, char* out)     // "NAME.EXT"
{
    int k = 0;
    for (int i = 0; i < 8 && sn[i] != ' '; i++) out[k++] = sn[i];
    if (sn[8] != ' ') {
        out[k++] = '.';
        for (int i = 8; i < 11 && sn[i] != ' '; i++) out[k++] = sn[i];
    }
    out[k] = 0;
    return k;
}
bool fcb_match(const char* pat, const char* sn)
{
    for (int i = 0; i < 11; i++)
        if (pat[i] != '?' && pat[i] != sn[i]) return false;
    return true;
}
bool ieq(const char* a, int n, const char* b, int m)
{
    if (n != m) return false;
    for (int i = 0; i < n; i++) if (up(a[i]) != up(b[i])) return false;
    return true;
}
// Wildcards on long names, as Windows: * any run, ? one character, any case;
// "*.*" is everything
bool glob(const char* p, const char* s)
{
    if (!strncmp(p, "*.*", 4)) return true;
    const char *bp = 0, *bs = 0;
    while (*s) {
        if (*p == '*') { bp = ++p; bs = s; continue; }
        if (*p && (*p == '?' || up(*p) == up(*s))) { p++; s++; continue; }
        if (!bp) return false;
        p = bp; s = ++bs;
    }
    while (*p == '*' || (*p == '.' && !p[1])) p++;
    return !*p;
}

// ---- directory listings
const int NDIR = 48, HPMAX = 512, MAXENT = 8192;
const u32 NAMEBUF = 512 * 1024;
struct Ent {
    u32 noff;
    u16 nlen;
    u8 attr, qtype;
    char sn[11];
    u32 size;
    u16 dtime, ddate;
    u64 qpath;
    u32 mode;
    u64 mt, at;
};
struct Dir {
    bool used;
    u8 dev;
    u32 gen, hash, lru;
    char path[HPMAX];
    Ent* ents;
    char* names;
    u32 n, bytes;
    u64 mt, mtn, ct, ctn;
    u32 attrs_at, names_at;
    u16 dtime, ddate;
};
Dir dirs[NDIR];
u32 dir_gen, lru_ctr;
Ent* sents;                                    // scratch for a listing being read
char* snames;
u16* sidx;
u16* shash;                                    // short names taken: open hashing, 16384 slots

u32 path_hash(int dev, const char* p) { return fnv(p, (int)strlen(p), (u32)dev + 1); }
Dir* dir_find(int dev, const char* hp)
{
    u32 h = path_hash(dev, hp);
    for (int i = 0; i < NDIR; i++)
        if (dirs[i].used && dirs[i].hash == h && dirs[i].dev == dev && !strncmp(dirs[i].path, hp, HPMAX))
            return &dirs[i];
    return 0;
}
void dir_drop(Dir* D)
{
    if (D->ents) phys_release(D->ents, D->bytes);
    D->ents = 0;
    D->used = false;
}
void dir_forget(int dev, const char* hp) { Dir* D = dir_find(dev, hp); if (D) dir_drop(D); }
const char* ename(Dir* D, const Ent& e) { return D->names + e.noff; }

void ent_attrs(Ent& e, const char* name)
{
    bool dir = (e.qtype & 0x80) || (e.mode & 0xF000) == 0x4000;
    e.attr = dir ? 0x10 : 0x20;
    if (!(e.mode & 0200) && !dir) e.attr |= 0x01;
    if (name[0] == '.') e.attr |= 0x02;
    to_dos(e.mt, &e.dtime, &e.ddate);
}

int name_cmp(const char* a, const char* b)
{
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}
u32 sn_hash(const char* sn) { return fnv(sn, 11, 0) & 16383; }
bool sn_taken(const char* sn, const Ent* list)
{
    for (u32 h = sn_hash(sn); shash[h] != 0xFFFF; h = (h + 1) & 16383)
        if (!memcmp(list[shash[h]].sn, sn, 11)) return true;
    return false;
}
void sn_take(u16 i, const Ent* list)
{
    u32 h = sn_hash(list[i].sn);
    while (shash[h] != 0xFFFF) h = (h + 1) & 16383;
    shash[h] = i;
}

int dir_fetch(Mount& m, const char* hp, Dir* D)
{
    Dev& d = devs[m.dev];
    u32 dfid, ofid;
    int e = p_path_fid(d, hp, &dfid);
    if (e) return e;
    Attr da;
    if ((e = p_getattr(d, dfid, da)) || !(da.qtype & 0x80)) { p_clunk(d, dfid); return e ? e : 20; }
    if ((e = p_walk(d, dfid, "", &ofid))) { p_clunk(d, dfid); return e; }
    if ((e = p_lopen(d, ofid, L_RDONLY | L_DIRECTORY))) { p_clunk(d, ofid); p_clunk(d, dfid); return e; }
    u32 n = 0, nb = 0;
    u64 off = 0;
    bool over = false;
    for (;;) {
        W w(d.btx, Treaddir, 1); w.w32(ofid).w64(off).w32(d.msize - 24).end();
        if ((e = rpc(d, Rreaddir))) break;
        u32 cnt = g32(d.brx + 7);
        if (!cnt) break;
        const u8 *p = d.brx + 11, *end = p + cnt;
        while (p + 24 <= end) {
            u8 qt = p[0];
            u64 qpath = g64(p + 5);
            off = g64(p + 13);
            u8 type = p[21];
            u16 nl = g16(p + 22);
            const char* nm = (const char*)p + 24;
            p += 24 + nl;
            if ((nl == 1 && nm[0] == '.') || (nl == 2 && nm[0] == '.' && nm[1] == '.')) continue;
            if (type == 10 || nl > 255 || !nl) continue;              // symlinks: not shown
            if (n >= (u32)MAXENT || nb + nl + 1 > NAMEBUF) { over = true; continue; }
            Ent& x = sents[n++];
            memset(&x, 0, sizeof x);
            x.noff = nb; x.nlen = nl;
            memcpy(snames + nb, nm, nl);
            snames[nb + nl] = 0;
            nb += nl + 1;
            x.qtype = qt; x.qpath = qpath;
        }
    }
    p_clunk(d, ofid);
    if (over) kprintf("9p: %s: only the first %u names shown\n", hp[0] ? hp : "/", n);
    // attributes: walks, then getattrs, then clunks, NSLOT at a time
    for (u32 i = 0; i < n && !d.broken; i += NSLOT) {
        int k = (int)(n - i < (u32)NSLOT ? n - i : NSLOT);
        u32 f[NSLOT];
        bool ok[NSLOT];
        for (int j = 0; j < k; j++) {
            f[j] = fid_get(d);
            W w(txp(d, j), Twalk, (u16)j);
            w.w32(dfid).w32(f[j]).w16(1).ws(snames + sents[i + j].noff, sents[i + j].nlen).end();
            post(d, j);
        }
        run(d, k);
        int c = 0;
        for (int j = 0; j < k; j++) {
            ok[j] = !reply(d, j, Rwalk) && g16(rxp(d, j) + 7) == 1;
            if (ok[j]) { m_getattr(txp(d, c), (u16)c, f[j]); post(d, c); c++; }
        }
        run(d, c);
        c = 0;
        for (int j = 0; j < k; j++) {
            Ent& x = sents[i + j];
            x.qtype = 0xFF;                    // gone, unless its attributes came
            if (!ok[j]) continue;
            if (!reply(d, c, Rgetattr)) {
                Attr a;
                parse_attr(rxp(d, c), a);
                x.qtype = a.qtype; x.mode = a.mode; x.mt = a.mt; x.at = a.at;
                x.size = a.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)a.size;
                if ((a.mode & 0xF000) == 0xA000) x.qtype = 0xFF;          // a symlink
            }
            c++;
        }
        c = 0;
        for (int j = 0; j < k; j++)
            if (ok[j]) { W w(txp(d, c), Tclunk, (u16)c); w.w32(f[j]).end(); post(d, c); c++; }
        run(d, c);
        for (int j = 0; j < k; j++) fid_put(d, f[j]);
    }
    p_clunk(d, dfid);
    if (d.broken) return 5;
    // in name order (shell sort), the gone ones out
    u32 m2 = 0;
    for (u32 i = 0; i < n; i++) if (sents[i].qtype != 0xFF) sidx[m2++] = (u16)i;
    n = m2;
    for (u32 gap = n / 2; gap; gap /= 2)
        for (u32 i = gap; i < n; i++) {
            u16 t = sidx[i];
            u32 j = i;
            for (; j >= gap && name_cmp(snames + sents[sidx[j - gap]].noff, snames + sents[t].noff) > 0; j -= gap)
                sidx[j] = sidx[j - gap];
            sidx[j] = t;
        }
    u32 bytes = (n * sizeof(Ent) + nb + 4095) & ~4095u;
    if (!bytes) bytes = 4096;
    Ent* ents = (Ent*)phys_try_alloc(bytes);
    if (!ents) return 12;
    char* names = (char*)(ents + n);
    // short names: the valid 8.3 ones first (the first of two the same in
    // capitals keeps it), then the others made up
    for (int i = 0; i < 16384; i++) shash[i] = 0xFFFF;
    u32 nn = 0;
    for (u32 i = 0; i < n; i++) {
        Ent& x = ents[i];
        x = sents[sidx[i]];
        memcpy(names + nn, snames + x.noff, x.nlen + 1);
        x.noff = nn;
        nn += x.nlen + 1;
        x.sn[0] = 0;
        char sn[11];
        if (valid83(names + x.noff, x.nlen, sn)) {
            memcpy(x.sn, sn, 11);
            if (!sn_taken(sn, ents)) sn_take((u16)i, ents);
            else x.sn[0] = 0;
        }
        ent_attrs(x, names + x.noff);
    }
    for (u32 i = 0; i < n; i++) {
        Ent& x = ents[i];
        if (x.sn[0]) continue;
        for (u32 salt = 0; ; salt++) {
            mangle(names + x.noff, x.nlen, salt, x.sn);
            if (!sn_taken(x.sn, ents)) break;
        }
        sn_take((u16)i, ents);
    }
    D->used = true;
    D->dev = m.dev;
    D->gen = ++dir_gen;
    D->hash = path_hash(m.dev, hp);
    D->lru = ++lru_ctr;
    {
        size_t l = strlen(hp);
        if (l >= (size_t)HPMAX) l = HPMAX - 1;
        memcpy(D->path, hp, l);
        D->path[l] = 0;
    }
    D->ents = ents; D->names = names; D->n = n; D->bytes = bytes;
    D->mt = da.mt; D->mtn = da.mtn; D->ct = da.ct; D->ctn = da.ctn;
    to_dos(da.mt, &D->dtime, &D->ddate);
    D->attrs_at = D->names_at = ticks;
    return 0;
}

// The listing of host directory hp; attrs: sizes and dates wanted fresh
// (else only the names). 0 and *err set when there is none.
Dir* dir_get(Mount& m, const char* hp, bool attrs, int* err)
{
    Dir* D = dir_find(m.dev, hp);
    if (D) {
        if ((u32)(ticks - (attrs ? D->attrs_at : D->names_at)) < m.ttl) { D->lru = ++lru_ctr; return D; }
        if (!attrs) {                          // names: unchanged while the folder's times are
            Dev& d = devs[m.dev];
            u32 f;
            Attr a;
            int e = p_path_fid(d, hp, &f);
            if (!e) { e = p_getattr(d, f, a); p_clunk(d, f); }
            if (!e && a.mt == D->mt && a.mtn == D->mtn && a.ct == D->ct && a.ctn == D->ctn) {
                D->names_at = ticks;
                D->lru = ++lru_ctr;
                return D;
            }
        }
        dir_drop(D);
    }
    D = 0;
    for (int i = 0; i < NDIR; i++) if (!dirs[i].used) { D = &dirs[i]; break; }
    if (!D) {
        D = &dirs[0];
        for (int i = 1; i < NDIR; i++) if (dirs[i].lru < D->lru) D = &dirs[i];
        dir_drop(D);
    }
    int e = dir_fetch(m, hp, D);
    if (e) { *err = e; return 0; }
    return D;
}

// ---- paths: DOS (V:\DIR\FILE.TXT, or long names with /LFN) to the host's
struct Res {
    char hp[HPMAX];                            // the host path ("dir/file"; "" the root)
    char parent[HPMAX];                        // the folder it is in
    char last[256];                            // the last component as asked for
    char name[256];                            // ... and the host's name for it (found)
    Ent e;
    bool root;
    u16 dtime, ddate;                          // the folder's (for . and ..)
};
bool path_cat(char* hp, const char* name, int n)
{
    size_t l = strlen(hp);
    if (l + (l ? 1 : 0) + n + 1 > (size_t)HPMAX) return false;
    if (l) hp[l++] = '/';
    memcpy(hp + l, name, n);
    hp[l + n] = 0;
    return true;
}
// p: the path after "X:" ("\DIR\FILE"); 0: found (R.e), E_FNF: all but the
// last there (R.parent, R.last), E_PNF: a folder on the way missing
int resolve(Mount& m, const char* p, Res& R)
{
    R.hp[0] = R.parent[0] = R.last[0] = R.name[0] = 0;
    R.root = true;
    while (*p == '\\' || *p == '/') p++;
    while (*p) {
        const char* e = p;
        while (*e && *e != '\\' && *e != '/') e++;
        int n = (int)(e - p);
        const char* nx = e;
        while (*nx == '\\' || *nx == '/') nx++;
        bool last = !*nx;
        if (n > 255) return E_PNF;
        int err = 0;
        Dir* D = dir_get(m, R.hp, false, &err);
        if (!D) return E_PNF;
        R.dtime = D->dtime; R.ddate = D->ddate;
        char fcb[11];
        bool f = to_fcb(p, n, fcb);
        Ent* hit = 0;
        for (u32 i = 0; i < D->n; i++) {
            Ent& x = D->ents[i];
            if ((f && !memcmp(x.sn, fcb, 11)) || ieq(ename(D, x), x.nlen, p, n)) {
                hit = &x;
                if (f && !memcmp(x.sn, fcb, 11)) break;
            }
        }
        memcpy(R.parent, R.hp, HPMAX);
        memcpy(R.last, p, n);
        R.last[n] = 0;
        if (!hit) return last ? E_FNF : E_PNF;
        if (!last && !(hit->attr & 0x10)) return E_PNF;
        R.e = *hit;
        memcpy(R.name, ename(D, *hit), hit->nlen + 1);
        if (!path_cat(R.hp, R.name, hit->nlen)) return E_PNF;
        R.root = false;
        p = nx;
    }
    return 0;
}

// ---- file data cache
const u32 BLK = 32768;
struct Blk { bool used; u8 dev; u64 qpath; u32 ver, no, len, lru; };
Blk* blks;
u8* bdata;
u32 nblk;
u32 ver_of(const Attr& a) { return fnv((const char*)&a.mt, 16, (u32)a.size) ^ (u32)(a.size >> 32); }
Blk* blk_find(u8 dev, u64 qpath, u32 ver, u32 no)
{
    for (u32 i = 0; i < nblk; i++) {
        Blk& b = blks[i];
        if (b.used && b.no == no && b.qpath == qpath && b.dev == dev) {
            if (b.ver != ver) { b.used = false; return 0; }
            b.lru = ++lru_ctr;
            return &b;
        }
    }
    return 0;
}
Blk* blk_new()
{
    Blk* v = &blks[0];
    for (u32 i = 0; i < nblk; i++) {
        if (!blks[i].used) { v = &blks[i]; break; }
        if (blks[i].lru < v->lru) v = &blks[i];
    }
    v->used = true;
    v->lru = ++lru_ctr;
    return v;
}
u8* blk_data(Blk* b) { return bdata + (u32)(b - blks) * BLK; }
void blk_forget(u8 dev, u64 qpath)
{
    for (u32 i = 0; i < nblk; i++) if (blks[i].used && blks[i].qpath == qpath && blks[i].dev == dev) blks[i].used = false;
}

// ---- open files: the SFT holds 9000h + the index at 0Bh and MAGIC at 1Bh
const int NOF = 64, NWB = 8;
const u32 WBSZ = 65536, MAGIC = 0x50394D56u;  // "VM9P"
struct OFile {
    bool used, wrote;
    u8 drive, acc;
    u32 fid, ver, size, last_end, gen;         // gen: the device's attach it was opened in
    u64 qpath;
    char hp[HPMAX];
    u8* wb;                                    // writes gathered: wb_len bytes for wb_off
    u32 wb_off, wb_len, wb_at;
};
OFile ofs[NOF];
u8* wbufs[NWB];
bool wb_busy[NWB];

int sft_file(u32 sft)                          // index of an SFT's open file, -1 not ours
{
    if ((rd16(sft + 5) & 0x8000) == 0 || rd32(sft + 0x1B) != MAGIC) return -1;
    u16 h = rd16(sft + 0x0B);
    if ((h & 0xFF00) != 0x9000 || (h & 0xFF) >= NOF || !ofs[h & 0xFF].used) return -1;
    if (ofs[h & 0xFF].drive != (rd16(sft + 5) & 0x3F)) return -1;
    return h & 0xFF;
}

bool stale(const OFile& f) { return f.gen != devs[mnts[f.drive].dev].gen; }   // opened before a reconnect

int wb_flush(OFile& f)
{
    if (!f.wb_len) return 0;
    if (stale(f)) { kprintf("9p: %u bytes for %s lost (the connection was lost)\n", f.wb_len, f.hp); f.wb_len = 0; return 5; }
    Dev& d = devs[mnts[f.drive].dev];
    u32 done = 0;
    int err = 0;
    while (done < f.wb_len) {
        u32 c = f.wb_len - done;
        if (c > d.msize - 23) c = d.msize - 23;
        memcpy(d.btx + 23, f.wb + done, c);
        long w = p_write(d, f.fid, f.wb_off + done, c);
        if (w <= 0) { err = w < 0 ? (int)-w : 28; break; }
        done += (u32)w;
    }
    if (err) kprintf("9p: write to %s failed (%d)\n", f.hp, err);
    f.wb_len = 0;
    return err;
}
void wb_release(OFile& f)
{
    for (int i = 0; i < NWB; i++) if (wbufs[i] && wbufs[i] == f.wb) wb_busy[i] = false;
    f.wb = 0;
}
u8* wb_get()
{
    for (int i = 0; i < NWB; i++) {
        if (wb_busy[i]) continue;
        if (!wbufs[i]) wbufs[i] = (u8*)phys_try_alloc(WBSZ);
        if (!wbufs[i]) return 0;
        wb_busy[i] = true;
        return wbufs[i];
    }
    return 0;
}

// n bytes at pos of an open file (fid) into dst, through the block cache:
// bytes read, or -errno. *last_end: where the previous read ended (read-ahead).
long cread(u8 dv, u32 fid, u64 qpath, u32 ver, u32* last_end, u32 pos, u32 n, u8* dst)
{
    Dev& d = devs[dv];
    u32 done = 0;
    while (done < n) {
        u32 p = pos + done, no = p / BLK, in = p % BLK;
        Blk* b = nblk ? blk_find(dv, qpath, ver, no) : 0;
        if (!b && nblk < 8) {                  // no cache: straight in
            long r = p_read(d, fid, p, n - done);
            if (r < 0) return done ? (long)done : r;
            memcpy(dst + done, d.brx + 11, (u32)r);
            done += (u32)r;
            if (!r) break;
            continue;
        }
        if (!b) {
            u32 ra = p == *last_end || in ? 4 : 1;     // straight through: read ahead
            if (ra * BLK > d.msize - 24) ra = (d.msize - 24) / BLK;
            if (!ra) ra = 1;
            long r = p_read(d, fid, (u64)no * BLK, ra * BLK);
            if (r < 0) return done ? (long)done : r;
            for (u32 k = 0; k < ra; k++) {
                long c = r - (long)(k * BLK);
                if (c <= 0 && k) break;
                Blk* nb = blk_new();
                nb->dev = dv; nb->qpath = qpath; nb->ver = ver; nb->no = no + k;
                nb->len = c <= 0 ? 0 : c > (long)BLK ? BLK : (u32)c;
                memcpy(blk_data(nb), d.brx + 11 + k * BLK, nb->len);
                if (!k) b = nb;
            }
        }
        if (in >= b->len) break;               // the end of the file
        u32 c = b->len - in;
        if (c > n - done) c = n - done;
        memcpy(dst + done, blk_data(b) + in, c);
        done += c;
        if (b->len < BLK) break;
    }
    *last_end = pos + done;
    return done;
}

// n bytes at pos from the host into guest memory at lin: bytes read
long file_read(OFile& f, u32 pos, u32 n, u32 lin)
{
    if (f.wb_len) wb_flush(f);
    return cread(mnts[f.drive].dev, f.fid, f.qpath, f.ver, &f.last_end, pos, n, gptr(lin));
}

// n bytes from guest memory at lin to pos: bytes written, or -errno
long file_write(OFile& f, u32 pos, u32 n, u32 lin)
{
    Mount& m = mnts[f.drive];
    Dev& d = devs[m.dev];
    blk_forget(m.dev, f.qpath);
    f.wrote = true;
    if (!(m.flags & MF_WT) && n < WBSZ) {
        if (!f.wb) f.wb = wb_get();
        if (f.wb) {
            if (f.wb_len && (pos != f.wb_off + f.wb_len || f.wb_len + n > WBSZ)) wb_flush(f);
            if (!f.wb_len) { f.wb_off = pos; f.wb_at = ticks; }
            memcpy(f.wb + f.wb_len, gptr(lin), n);
            f.wb_len += n;
            if (pos + n > f.size) f.size = pos + n;
            return n;
        }
    }
    wb_flush(f);
    u32 done = 0;
    while (done < n) {
        u32 c = n - done;
        if (c > d.msize - 23) c = d.msize - 23;
        memcpy(d.btx + 23, gptr(lin + done), c);
        long w = p_write(d, f.fid, pos + done, c);
        if (w < 0) { if (!done) return w; break; }
        if (!w) break;
        done += (u32)w;
    }
    if (pos + done > f.size) f.size = pos + done;
    return done;
}

// After writing: the size and date in the folder's listing
void ent_update(OFile& f, const Attr& a)
{
    char par[HPMAX];
    memcpy(par, f.hp, HPMAX);
    char* s = par;
    for (char* q = par; *q; q++) if (*q == '/') s = q;
    const char* nm = s == par && *s != '/' ? f.hp : f.hp + (s - par) + 1;
    if (s == par && *s != '/') par[0] = 0; else *s = 0;
    Dir* D = dir_find(mnts[f.drive].dev, par);
    if (!D) return;
    for (u32 i = 0; i < D->n; i++) {
        Ent& x = D->ents[i];
        if (!strncmp(ename(D, x), nm, 256)) {
            x.size = a.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)a.size;
            x.mt = a.mt; x.mode = a.mode;
            ent_attrs(x, ename(D, x));
            return;
        }
    }
}

void file_close(OFile& f, u32 sft)
{
    Mount& m = mnts[f.drive];
    Dev& d = devs[m.dev];
    wb_flush(f);
    wb_release(f);
    bool dated = sft && (rd16(sft + 5) & 0x4000);
    if (dated && !(m.flags & MF_RO))
        p_setattr(d, f.fid, SA_MTIME | SA_MTIME_SET | SA_ATIME, 0, 0, from_dos(rd16(sft + 0x0D), rd16(sft + 0x0F)));
    if (f.wrote || dated) {
        Attr a;
        if (!p_getattr(d, f.fid, a)) ent_update(f, a);
    }
    p_clunk(d, f.fid);
    f.used = false;
}

// ---- searches (FINDFIRST/FINDNEXT, and the long-name ones)
const int NSRCH = 64;
const u32 SMAGIC = 0x53394D56u;
struct Search {
    bool used, lfn, started;
    u8 drive, attr, need, stage;               // stage 0, 1: "." and ".." (not in the root), 2: the names
    u16 seq;
    u32 at;
    char dir[HPMAX];
    char last[256];                            // the last name given
    char pat[11];                              // FCB pattern, or ...
    char lpat[260];                            // ... long one
};
Search srch[NSRCH];
u16 srch_seq;

Search* srch_new()
{
    Search* s = &srch[0];
    for (int i = 0; i < NSRCH; i++) {
        if (!srch[i].used) { s = &srch[i]; break; }
        if ((u32)(ticks - srch[i].at) > (u32)(ticks - s->at)) s = &srch[i];
    }
    memset(s, 0, sizeof *s);
    s->used = true;
    s->seq = ++srch_seq;
    s->at = ticks;
    return s;
}

struct Found { char sn[11]; const char* name; Ent e; };
// The next name of a search: 0, or E_NOMORE (and the search is over)
int srch_next(Search& S, Found& F, char* namebuf)
{
    Mount& m = mnts[S.drive];
    S.at = ticks;
    int err = 0;
    // the first call wants the sizes and dates fresh; the next ones go on
    // through the same listing while it is kept
    Dir* D = S.started ? dir_find(m.dev, S.dir) : 0;
    if (D) D->lru = ++lru_ctr;
    else D = dir_get(m, S.dir, true, &err);
    S.started = true;
    if (!D) { S.used = false; return E_NOMORE; }
    while (S.stage < 2) {
        int st = S.stage++;
        if (!S.dir[0]) continue;               // the root has no . and ..
        memset(&F.e, 0, sizeof F.e);
        memset(F.sn, ' ', 11);
        F.sn[0] = '.';
        if (st) F.sn[1] = '.';
        F.e.attr = 0x10; F.e.dtime = D->dtime; F.e.ddate = D->ddate; F.e.mt = D->mt;
        memcpy(F.e.sn, F.sn, 11);
        namebuf[0] = '.'; namebuf[1] = st ? '.' : 0; namebuf[2] = 0;
        F.name = namebuf;
        bool hit = S.lfn ? glob(S.lpat, namebuf) : fcb_match(S.pat, F.sn);
        if (hit && (S.attr & 0x10) && (0x10 & S.need) == S.need) return 0;
    }
    // the first name after the last one given (sorted: new and gone names don't upset it)
    u32 lo_ = 0, hi = D->n;
    if (S.last[0])
        while (lo_ < hi) {
            u32 mid = (lo_ + hi) / 2;
            if (name_cmp(ename(D, D->ents[mid]), S.last) <= 0) lo_ = mid + 1; else hi = mid;
        }
    for (u32 i = lo_; i < D->n; i++) {
        Ent& x = D->ents[i];
        if (x.attr & 0x16 & ~S.attr) continue;
        if ((x.attr & S.need) != S.need) continue;
        bool hit;
        if (S.lfn) {
            char s83[13];
            fcb_to_str(x.sn, s83);
            hit = glob(S.lpat, ename(D, x)) || glob(S.lpat, s83);
        } else hit = fcb_match(S.pat, x.sn);
        if (!hit) continue;
        memcpy(S.last, ename(D, x), x.nlen + 1);
        memcpy(namebuf, ename(D, x), x.nlen + 1);
        F.e = x;
        memcpy(F.sn, x.sn, 11);
        F.name = namebuf;
        return 0;
    }
    S.used = false;
    return E_NOMORE;
}

// ---- helpers for the calls
inline void ok(struct regs* r) { r->eflags &= ~EFL_CF; }
inline void fail(struct regs* r, int e) { r->eflags |= EFL_CF; AX(r) = (u16)e; }
void gstr(u32 lin, char* out, int max)
{
    int i = 0;
    for (; i < max - 1; i++) if (!(out[i] = (char)rd8(lin + i))) break;
    out[i] = 0;
}
void pstr(u32 lin, const char* s) { do wr8(lin++, (u8)*s); while (*s++); }
bool has_wild(const char* s) { for (; *s; s++) if (*s == '?' || *s == '*') return true; return false; }
// split "\A\B\PAT" into "\A\B" and "PAT"
const char* last_comp(char* p)
{
    char* s = 0;
    for (char* q = p; *q; q++) if (*q == '\\') s = q;
    if (!s) return p;
    *s = 0;
    return s + 1;
}
int drive_of(const char* p) { return p[1] == ':' ? up(p[0]) - 'A' : -1; }
Mount* mount_of(int dr) { return dr >= 0 && dr < 26 && mnts[dr].used ? &mnts[dr] : 0; }
void lower_name(char* dst, const char* s) { while ((*dst++ = lo(*s++))) ; }
// The host's name for a name given through the long-name API: as given, but
// a plain 8.3 name in capitals (as DOS programs write them) in lower case
void host_name(char* dst, const char* s)
{
    char sn[11];
    bool lower = false;
    for (const char* q = s; *q; q++) if (*q >= 'a' && *q <= 'z') lower = true;
    if (!lower && valid83(s, (int)strlen(s), sn)) lower_name(dst, s);
    else memcpy(dst, s, strlen(s) + 1);
}

int of_new()
{
    for (int i = 0; i < NOF; i++) if (!ofs[i].used) { memset(&ofs[i], 0, sizeof ofs[i]); return i; }
    return -1;
}

void sft_fill(u32 sft, int drive, int idx, const OFile& f, u8 attr, u16 t, u16 dt, const char* sn)
{
    wr8(sft + 4, attr);
    wr16(sft + 5, (u16)(0x8040 | drive));      // remote, not written to, drive
    wr32(sft + 7, 0);
    wr16(sft + 0x0B, (u16)(0x9000 | idx));
    wr16(sft + 0x0D, t);
    wr16(sft + 0x0F, dt);
    wr32(sft + 0x11, f.size);
    wr32(sft + 0x15, 0);
    wr16(sft + 0x19, 0xFFFF);
    wr32(sft + 0x1B, MAGIC);
    wr8(sft + 0x1F, 0xFF);
    for (int i = 0; i < 11; i++) wr8(sft + 0x20 + i, (u8)sn[i]);
}

// A long name for the file DOS is about to create (716Ch -> 6C00h with its short name)
struct { bool on; u8 drive; char parent[HPMAX]; char sn[11]; char name[256]; } pending;

// Open or create path (after "X:") for an SFT. action: low nibble 0 fail if
// there, 1 open, 2 truncate; 10h: create if not there. 0 and *taken 1
// opened, 2 created, 3 truncated; or a DOS error.
int do_open(Mount& m, int drive, u32 sft, const char* path, u16 mode, u16 attr, u16 action, u16* taken)
{
    Dev& d = devs[m.dev];
    Res R;
    int e = resolve(m, path, R);
    if (e == E_PNF || (e == 0 && R.root)) return E_PNF;
    u32 fid;
    Attr a;
    u8 acc = mode & 3;
    if (acc > 2) return 12;                    // invalid access code
    char hp[HPMAX];
    if (!e) {
        if (R.e.attr & 0x10) return E_ACCESS;
        if (!(action & 0x0F)) return E_EXISTS;
        bool trunc = (action & 0x0F) == 2;
        if ((acc || trunc) && ((R.e.attr & 1) || (m.flags & MF_RO))) return E_ACCESS;
        if ((e = p_path_fid(d, R.hp, &fid))) return dos_err(e, E_FNF);
        u32 fl = trunc ? (L_RDWR | L_TRUNC) : acc == 0 ? L_RDONLY : acc == 1 ? L_WRONLY : L_RDWR;
        if ((e = p_lopen(d, fid, fl)) || (e = p_getattr(d, fid, a))) { p_clunk(d, fid); return dos_err(e, E_FNF); }
        *taken = trunc ? 3 : 1;
        memcpy(hp, R.hp, HPMAX);
    } else {
        if (!(action & 0x10)) return E_FNF;
        if (m.flags & MF_RO) return E_ACCESS;
        if (has_wild(R.last)) return E_PNF;
        char name[256];
        char sn[11];
        to_fcb(R.last, (int)strlen(R.last), sn);
        if (pending.on && pending.drive == drive && !strncmp(pending.parent, R.parent, HPMAX) && !memcmp(pending.sn, sn, 11))
            memcpy(name, pending.name, 256);
        else lower_name(name, R.last);
        pending.on = false;
        if ((e = p_path_fid(d, R.parent, &fid))) return dos_err(e, E_PNF);
        W w(d.btx, Tlcreate, 1);
        w.w32(fid).ws(name).w32(L_RDWR | L_CREAT | L_EXCL).w32((attr & 1) ? 0444 : 0644).w32(0).end();
        if ((e = rpc(d, Rlcreate)) || (e = p_getattr(d, fid, a))) { p_clunk(d, fid); return dos_err(e, E_PNF); }
        dir_forget(m.dev, R.parent);
        *taken = 2;
        memcpy(hp, R.parent, HPMAX);
        path_cat(hp, name, (int)strlen(name));
        memset(&R.e, 0, sizeof R.e);
        R.e.mode = a.mode; R.e.qtype = a.qtype;
        ent_attrs(R.e, name);
        memcpy(R.e.sn, sn, 11);
    }
    int i = of_new();
    if (i < 0) { p_clunk(d, fid); return E_TOOMANY; }
    OFile& f = ofs[i];
    f.used = true;
    f.gen = devs[m.dev].gen;
    f.drive = (u8)drive;
    f.acc = acc;
    f.fid = fid;
    f.qpath = a.qpath;
    f.ver = ver_of(a);
    f.size = a.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)a.size;
    f.last_end = 0xFFFFFFFFu;
    f.wrote = *taken != 1;
    memcpy(f.hp, hp, HPMAX);
    u16 t, dt;
    to_dos(a.mt, &t, &dt);
    u8 at = (u8)(R.e.attr & ~0x10);
    if (*taken == 2) at = (u8)(0x20 | (attr & 0x07));
    sft_fill(sft, drive, i, f, at, t, dt, R.e.sn);
    return 0;
}

int do_unlink(Mount& m, const char* parent, const char* name, bool dir)
{
    Dev& d = devs[m.dev];
    u32 fid;
    int e = p_path_fid(d, parent, &fid);
    if (e) return e;
    W w(d.btx, Tunlinkat, 1); w.w32(fid).ws(name).w32(dir ? 0x200 : 0).end();
    e = rpc(d, Runlinkat);
    if (e == 95) {                             // EOPNOTSUPP (diod): Tremove of the file's own fid
        u32 f;
        char nm[256];
        size_t l = strlen(name);
        memcpy(nm, name, l + 1);
        if (!(e = p_walk(d, fid, nm, &f))) {
            W x(d.btx, Tremove, 1); x.w32(f).end();
            e = rpc(d, Rremove);               // (the fid is gone either way)
            fid_put(d, f);
        }
    }
    p_clunk(d, fid);
    dir_forget(m.dev, parent);
    return e;
}
int do_rename(Mount& m, const char* p1, const char* n1, const char* p2, const char* n2)
{
    Dev& d = devs[m.dev];
    u32 f1, f2;
    int e = p_path_fid(d, p1, &f1);
    if (e) return e;
    if ((e = p_path_fid(d, p2, &f2))) { p_clunk(d, f1); return e; }
    W w(d.btx, Trenameat, 1); w.w32(f1).ws(n1).w32(f2).ws(n2).end();
    e = rpc(d, Rrenameat);
    if (e == 95) {                             // EOPNOTSUPP (diod): Trename of the file's own fid
        u32 f;
        if (!(e = p_walk(d, f1, n1, &f))) {
            W x(d.btx, Trename, 1); x.w32(f).w32(f2).ws(n2).end();
            e = rpc(d, Rrename);
            p_clunk(d, f);
        }
    }
    p_clunk(d, f1);
    p_clunk(d, f2);
    dir_forget(m.dev, p1);
    dir_forget(m.dev, p2);
    return e;
}
int do_mkdir(Mount& m, const char* parent, const char* name)
{
    Dev& d = devs[m.dev];
    u32 fid;
    int e = p_path_fid(d, parent, &fid);
    if (e) return e;
    W w(d.btx, Tmkdir, 1); w.w32(fid).ws(name).w32(0755).w32(0).end();
    e = rpc(d, Rmkdir);
    p_clunk(d, fid);
    dir_forget(m.dev, parent);
    return e;
}
int do_chmod(Mount& m, Res& R, u16 attr)
{
    if (R.e.attr & 0x10) return 0;
    Dev& d = devs[m.dev];
    u32 mode = R.e.mode & 07777;
    if (attr & 1) mode &= ~0222u; else mode |= 0200;
    if (mode == (R.e.mode & 07777)) return 0;
    u32 fid;
    int e = p_path_fid(d, R.hp, &fid);
    if (e) return e;
    e = p_setattr(d, fid, SA_MODE, mode, 0, 0);
    p_clunk(d, fid);
    dir_forget(m.dev, R.parent);
    return e;
}
// FCB-style rename pattern: NEW with ? taking OLD's character
void fcb_subst(const char* pat, const char* old, char* out)
{
    for (int i = 0; i < 11; i++) out[i] = pat[i] == '?' ? old[i] : pat[i];
}

// Delete or rename every match of a wildcard; rename: the FCB pattern for the new names
int wild_files(Mount& m, Res& P, const char* pat, const char* newpat, const char* newparent)
{
    int err = 0;
    Dir* D = dir_get(m, P.hp, true, &err);
    if (!D) return E_PNF;
    char fcb[11];
    to_fcb(pat, (int)strlen(pat), fcb);
    // the names first: the listing goes away with the first change
    static char names[64][256];
    static char sns[64][11];
    int done = 0, found = 0, denied = 0;
    for (;;) {
        D = dir_get(m, P.hp, true, &err);
        if (!D) break;
        int k = 0;
        for (u32 i = 0; i < D->n && k < 64; i++) {
            Ent& x = D->ents[i];
            if ((x.attr & 0x16) || !fcb_match(fcb, x.sn)) continue;
            if (x.attr & 1) { denied++; continue; }
            memcpy(names[k], ename(D, x), x.nlen + 1);
            memcpy(sns[k], x.sn, 11);
            k++;
        }
        if (!k) break;
        int now = 0;
        for (int j = 0; j < k; j++) {
            int e;
            found++;
            if (newpat) {
                char nsn[11], ns[13], nm[13];
                fcb_subst(newpat, sns[j], nsn);
                fcb_to_str(nsn, ns);
                lower_name(nm, ns);
                if (!memcmp(nsn, sns[j], 11)) continue;
                e = do_rename(m, P.hp, names[j], newparent, nm);
            } else e = do_unlink(m, P.hp, names[j], false);
            if (e) { err = dos_err(e, E_FNF); denied++; } else { done++; now++; }
        }
        if (!now || newpat || found > 100000) break;   // deleted ones: look again for more than 64
    }
    if (!found && !denied) return E_FNF;
    if (denied && !done) return err ? err : E_ACCESS;
    return 0;
}

int path_of(u32 sda, int which, char* out)     // the SDA's first or second file name
{
    gstr(sda + (which ? 0x11E : 0x9E), out, 128);
    return drive_of(out);
}

bool flush_due(u32 age)
{
    bool any = false;
    for (int i = 0; i < NOF; i++)
        if (ofs[i].used && ofs[i].wb_len && (u32)(ticks - ofs[i].wb_at) >= age) { wb_flush(ofs[i]); any = true; }
    return any;
}

int redir(struct regs* r, Mount& m, int drive, u8 fn)
{
    u32 sda = m.sda;
    char p1[128], p2[128];
    Res R;
    int e;
    switch (fn) {
    case 0x01:                                 // rmdir
    case 0x03:                                 // mkdir
    case 0x05: {                               // chdir
        path_of(sda, 0, p1);
        e = resolve(m, p1 + 2, R);
        if (fn == 0x05) {
            if (e || !(R.root || (R.e.attr & 0x10))) return E_PNF;
            return 0;
        }
        if (m.flags & MF_RO) return E_ACCESS;
        if (fn == 0x01) {
            if (e) return E_PNF;
            if (R.root || !(R.e.attr & 0x10)) return E_ACCESS;
            e = do_unlink(m, R.parent, R.name, true);
            dir_forget(m.dev, R.hp);
            return dos_err(e, E_PNF);
        }
        if (!e) return E_ACCESS;
        if (e != E_FNF || has_wild(R.last)) return E_PNF;
        char nm[256];
        lower_name(nm, R.last);
        return dos_err(do_mkdir(m, R.parent, nm), E_PNF);
    }
    case 0x0E:                                 // set attributes: [SS:SP]
    case 0x0F: {                               // get attributes
        path_of(sda, 0, p1);
        if ((e = resolve(m, p1 + 2, R))) return e;
        if (fn == 0x0E) {
            if (m.flags & MF_RO) return E_ACCESS;
            return dos_err(do_chmod(m, R, rd16(LIN(r->ss, SP(r)))), E_FNF);
        }
        if (R.root) { AX(r) = 0x10; return 0; }
        AX(r) = R.e.attr;
        BX(r) = (u16)(R.e.size >> 16); DI(r) = (u16)R.e.size;
        CX(r) = R.e.dtime; DX(r) = R.e.ddate;
        return 0;
    }
    case 0x11: {                               // rename
        if (m.flags & MF_RO) return E_ACCESS;
        path_of(sda, 0, p1);
        if (path_of(sda, 1, p2) != drive) return 17;   // not the same device
        const char* l1 = last_comp(p1 + 2);
        const char* l2 = last_comp(p2 + 2);
        Res P1, P2;
        if (resolve(m, p1 + 2, P1) || !(P1.root || (P1.e.attr & 0x10))) return E_PNF;
        if (resolve(m, p2 + 2, P2) || !(P2.root || (P2.e.attr & 0x10))) return E_PNF;
        if (has_wild(l1) || has_wild(l2)) {
            char np[11];
            to_fcb(l2, (int)strlen(l2), np);
            return wild_files(m, P1, l1, np, P2.hp);
        }
        char q1[300], q2[300];
        snprintf(q1, sizeof q1, "%s\\%s", p1 + 2, l1);
        snprintf(q2, sizeof q2, "%s\\%s", p2 + 2, l2);
        if ((e = resolve(m, q1, R))) return e;
        Res T;
        int e2 = resolve(m, q2, T);
        if (!e2) {
            if (T.e.qpath == R.e.qpath && !name_cmp(T.parent, R.parent)) return 0;   // itself
            return E_ACCESS;
        }
        if (e2 != E_FNF) return E_PNF;
        char nm[256];
        lower_name(nm, T.last);
        return dos_err(do_rename(m, R.parent, R.name, T.parent, nm), E_FNF);
    }
    case 0x13: {                               // delete (wildcards too)
        if (m.flags & MF_RO) return E_ACCESS;
        path_of(sda, 0, p1);
        const char* l = last_comp(p1 + 2);
        Res P;
        if (resolve(m, p1 + 2, P) || !(P.root || (P.e.attr & 0x10))) return E_PNF;
        if (has_wild(l)) return wild_files(m, P, l, 0, 0);
        char q[300];
        snprintf(q, sizeof q, "%s\\%s", p1 + 2, l);
        if ((e = resolve(m, q, R))) return e;
        if (R.e.attr & 0x11) return E_ACCESS;
        return dos_err(do_unlink(m, R.parent, R.name, false), E_FNF);
    }
    case 0x16:                                 // open: [SS:SP] mode
    case 0x17:                                 // create / truncate: [SS:SP] attribute
    case 0x2E: {                               // extended open / create (SDA 2DDh...)
        path_of(sda, 0, p1);
        u32 sft = LIN(r->v86_es, DI(r));
        u16 sw = rd16(LIN(r->ss, SP(r))), mode, attr, action, taken = 0;
        if (fn == 0x16) { mode = sw & 0xFF; attr = 0; action = 0x01; }
        else if (fn == 0x17) { mode = 2; attr = sw & 0xFF; action = (sw & 0x100) ? 0x10 : 0x12; }
        else { action = rd16(sda + 0x2DD); attr = rd16(sda + 0x2DF); mode = rd16(sda + 0x2E1); }
        if ((e = do_open(m, drive, sft, p1 + 2, mode, attr, action, &taken))) return e;
        if (fn == 0x2E) CX(r) = taken;
        return 0;
    }
    case 0x1B: {                               // find first: DTA: the search block, SDA 1B3h: the entry
        path_of(sda, 0, p1);
        u8 attr = rd8(sda + 0x24D);
        const char* l = last_comp(p1 + 2);
        Res P;
        if (resolve(m, p1 + 2, P) || !(P.root || (P.e.attr & 0x10))) return E_PNF;
        u32 dta = LIN(rd16(sda + 0x0E), rd16(sda + 0x0C));
        Search* S = srch_new();
        S->drive = (u8)drive;
        S->attr = attr;
        memcpy(S->dir, P.hp, HPMAX);
        to_fcb(l, (int)strlen(l), S->pat);
        Found F;
        char nb[256];
        if (attr == 0x08) {                    // the volume label: the share's tag
            S->used = false;
            memset(&F, 0, sizeof F);
            memset(F.sn, ' ', 11);
            Dev& dv = devs[m.dev];
            const char* t = dv.tag;                     // TCP: the export's last name ("9P" for none)
            if (dv.tcp) {
                t = "9P";
                for (const char* q = dv.aname; *q; q++) if (*q == '/' && q[1]) t = q + 1;
            }
            for (int i = 0, k = 0; k < 11 && t[i] && t[i] != '/'; i++)
                if (ok83(t[i]) || t[i] == ' ') F.sn[k++] = up(t[i]);
            F.e.attr = 0x08;
            F.e.dtime = P.dtime; F.e.ddate = P.ddate;
        } else if (srch_next(*S, F, nb)) return E_FNF;
        // the search block
        wr8(dta, (u8)(0x80 | drive));
        for (int i = 0; i < 11; i++) wr8(dta + 1 + i, (u8)S->pat[i]);
        wr8(dta + 12, attr);
        wr16(dta + 13, (u16)(S - srch));
        wr16(dta + 15, S->seq);
        wr32(dta + 17, SMAGIC);
        // the entry found
        u32 de = sda + 0x1B3;
        for (int i = 0; i < 32; i++) wr8(de + i, 0);
        for (int i = 0; i < 11; i++) wr8(de + i, (u8)F.sn[i]);
        wr8(de + 11, F.e.attr);
        wr16(de + 0x16, F.e.dtime); wr16(de + 0x18, F.e.ddate);
        wr32(de + 0x1C, F.e.size);
        return 0;
    }
    case 0x1C: {                               // find next: ES:DI the search block
        u32 sb = LIN(r->v86_es, DI(r));
        u16 si = rd16(sb + 13);
        if (si >= NSRCH || rd32(sb + 17) != SMAGIC) return E_NOMORE;
        Search& S = srch[si];
        if (!S.used || S.seq != rd16(sb + 15) || S.lfn) return E_NOMORE;
        Found F;
        char nb[256];
        if (srch_next(S, F, nb)) return E_NOMORE;
        u32 de = sda + 0x1B3;
        for (int i = 0; i < 32; i++) wr8(de + i, 0);
        for (int i = 0; i < 11; i++) wr8(de + i, (u8)F.sn[i]);
        wr8(de + 11, F.e.attr);
        wr16(de + 0x16, F.e.dtime); wr16(de + 0x18, F.e.ddate);
        wr32(de + 0x1C, F.e.size);
        return 0;
    }
    case 0x0C:                                 // disk space: ES:DI the CDS
    case 0xA3: {                               // (FreeDOS) 32-bit disk space
        Dev& d = devs[m.dev];
        W w(d.btx, Tstatfs, 1); w.w32(d.root).end();
        if ((e = rpc(d, Rstatfs))) return dos_err(e, E_ACCESS);
        u64 bs = g32(d.brx + 4 + 3 + 4), blocks = g64(d.brx + 15), avail = g64(d.brx + 31);
        if (!bs) bs = 4096;
        if (fn == 0xA3) {
            u32 unit = bs > 32768 ? 4096 : (u32)bs;
            u64 tot = div64(blocks * bs, unit), fr = div64(avail * bs, unit);
            if (tot > 0xFFFFFFFFull) tot = 0xFFFFFFFFull;
            if (fr > 0xFFFFFFFFull) fr = 0xFFFFFFFFull;
            AX(r) = (u16)(tot >> 16); BX(r) = (u16)tot;
            CX(r) = (u16)(fr >> 16); DX(r) = (u16)fr;
            SI(r) = (u16)unit;
            return 0;
        }
        u64 tot = (blocks * bs) >> 15, fr = (avail * bs) >> 15;   // 32 KiB clusters, as much as 2 GiB shows
        AX(r) = 64; CX(r) = 512;
        BX(r) = (u16)(tot > 0xFFFE ? 0xFFFE : tot);
        DX(r) = (u16)(fr > 0xFFFE ? 0xFFFE : fr);
        return 0;
    }
    }
    return -1;
}

int redir_sft(struct regs* r, int idx, u8 fn)
{
    OFile& f = ofs[idx];
    Mount& m = mnts[f.drive];
    u32 sft = LIN(r->v86_es, DI(r));
    if (stale(f)) {                            // its connection was lost: only closing works
        if (fn != 0x06) return fn == 0x0A || fn == 0x0B ? 0 : E_ACCESS;
        u16 c = rd16(sft);
        if (c) wr16(sft, --c);
        if (!c) { wb_flush(f); wb_release(f); f.used = false; }
        return 0;
    }
    switch (fn) {
    case 0x06: {                               // close
        u16 c = rd16(sft);
        if (c) wr16(sft, --c);
        if (!c) file_close(f, sft);
        else wb_flush(f);
        return 0;
    }
    case 0x07:                                 // commit
        wb_flush(f);
        return 0;
    case 0x08: {                               // read CX bytes to the DTA
        u32 pos = rd32(sft + 0x15), dta = LIN(rd16(m.sda + 0x0E), rd16(m.sda + 0x0C));
        long n = file_read(f, pos, CX(r), dta);
        if (n < 0) return dos_err((int)-n, E_ACCESS);
        wr32(sft + 0x15, pos + (u32)n);
        CX(r) = (u16)n;
        return 0;
    }
    case 0x09: {                               // write CX bytes from the DTA (0: the size is the position)
        if (m.flags & MF_RO) return E_ACCESS;
        u32 pos = rd32(sft + 0x15), dta = LIN(rd16(m.sda + 0x0E), rd16(m.sda + 0x0C));
        Dev& d = devs[m.dev];
        if (!CX(r)) {
            wb_flush(f);
            int e = p_setattr(d, f.fid, SA_SIZE, 0, pos, 0);
            if (e) return dos_err(e, E_ACCESS);
            blk_forget(m.dev, f.qpath);
            f.size = pos;
            f.wrote = true;
        } else {
            long n = file_write(f, pos, CX(r), dta);
            if (n < 0) return dos_err((int)-n, E_ACCESS);
            wr32(sft + 0x15, pos + (u32)n);
            CX(r) = (u16)n;
        }
        wr32(sft + 0x11, f.size);
        wr16(sft + 5, rd16(sft + 5) & ~0x40);
        return 0;
    }
    case 0x0A: case 0x0B:                      // lock, unlock: fine
        return 0;
    case 0x21: {                               // seek from the end: CX:DX
        wb_flush(f);
        Attr a;
        if (!p_getattr(devs[m.dev], f.fid, a)) f.size = a.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)a.size;
        long long np = (long long)f.size + (int)((u32)CX(r) << 16 | DX(r));
        if (np < 0) np = 0;
        wr32(sft + 0x15, (u32)np);
        wr32(sft + 0x11, f.size);
        DX(r) = (u16)(np >> 16); AX(r) = (u16)np;
        return 0;
    }
    }
    return -1;
}

// ---- long names: INT 21h AX=71xxh
u32 lfn_sda()
{
    for (int i = 0; i < 26; i++) if (mnts[i].used) return mnts[i].sda;
    return 0;
}
// A DOS character device's name (NUL, CON, a driver's): DOS's own, on any drive
bool dos_device(u32 lol, const char* comp, int n)
{
    char nm[8];
    int k = 0;
    for (; k < n && comp[k] != '.' && k < 8; k++) nm[k] = up(comp[k]);
    if (k < n && comp[k] != '.') return false;
    for (int i = k; i < 8; i++) nm[i] = ' ';
    u32 dev = lol + 0x22;                      // the NUL device, the start of the chain
    for (int i = 0; i < 64; i++) {
        if (rd16(dev + 4) & 0x8000) {
            int j = 0;
            while (j < 8 && rd8(dev + 10 + j) == (u8)nm[j]) j++;
            if (j == 8) return true;
        }
        u16 off = rd16(dev), seg = rd16(dev + 2);
        if (off == 0xFFFF) break;
        dev = LIN(seg, off);
    }
    return false;
}

// A long-name path argument to the drive and the path after "X:" (absolute,
// "\"-separated, . and .. gone): the drive, or -1 when it isn't ours.
int lfn_path(u32 lin, char* out)
{
    char s[300];
    gstr(lin, s, sizeof s);
    if ((s[0] == '\\' || s[0] == '/') && (s[1] == '\\' || s[1] == '/')) return -1;   // UNC
    int dr;
    const char* p = s;
    u32 sda = lfn_sda();
    if (s[0] && s[1] == ':') { dr = up(s[0]) - 'A'; p = s + 2; }
    else dr = rd8(sda + 0x16);
    Mount* m = mount_of(dr);
    if (!m || !(m->flags & MF_LFN)) return -1;
    char w[300];
    int n = 0;
    if (*p != '\\' && *p != '/') {             // relative: from the drive's current folder
        char c[70];
        gstr(m->cds, c, 67);
        int bs = rd16(m->cds + 0x4F);
        const char* q = c + (bs < 67 ? bs : 2);
        while (*q && n < 290) w[n++] = *q++;
        if (n && w[n - 1] != '\\') w[n++] = '\\';
    }
    while (*p && n < 298) { w[n++] = *p == '/' ? '\\' : *p; p++; }
    w[n] = 0;
    // components: . and .. out, empty ones out
    int o = 0;
    out[0] = 0;
    for (char* q = w; *q; ) {
        while (*q == '\\') q++;
        char* e = q;
        while (*e && *e != '\\') e++;
        int len = (int)(e - q);
        if (len == 1 && q[0] == '.') ;
        else if (len == 2 && q[0] == '.' && q[1] == '.') {
            while (o > 0 && out[o - 1] != '\\') o--;
            if (o > 0) o--;
        } else if (len) {
            // trailing spaces (and dots, but in patterns) are dropped, as Windows does
            bool wild = false;
            for (int k = 0; k < len; k++) if (q[k] == '*' || q[k] == '?') wild = true;
            while (len > 1 && (q[len - 1] == ' ' || (q[len - 1] == '.' && !wild))) len--;
            if (o + len + 2 >= 270) return -2;
            out[o++] = '\\';
            memcpy(out + o, q, len);
            o += len;
        }
        out[o] = 0;
        q = e;
    }
    if (!o) { out[0] = '\\'; out[1] = 0; }
    const char* l = out;
    for (const char* q = out; *q; q++) if (*q == '\\') l = q + 1;
    if (*l && dos_device(m->lol, l, (int)strlen(l))) return -1;
    return dr;
}
// The short form of a resolved path ("\SHORT~1\FILE.TXT"), from the host path
int short_path(Mount& m, const char* hp, char* out, int max)
{
    int o = 0;
    char cur[HPMAX] = "";
    const char* p = hp;
    while (*p) {
        const char* e = p;
        while (*e && *e != '/') e++;
        int err = 0;
        Dir* D = dir_get(m, cur, false, &err);
        if (!D) return -1;
        const Ent* hit = 0;
        for (u32 i = 0; i < D->n; i++)
            if (D->ents[i].nlen == e - p && !memcmp(ename(D, D->ents[i]), p, e - p)) { hit = &D->ents[i]; break; }
        if (!hit) return -1;
        char s[13];
        int k = fcb_to_str(hit->sn, s);
        if (o + k + 2 >= max) return -1;
        out[o++] = '\\';
        memcpy(out + o, s, k);
        o += k;
        path_cat(cur, p, (int)(e - p));
        p = *e ? e + 1 : e;
    }
    if (!o) out[o++] = '\\';
    out[o] = 0;
    return o;
}
void long_path(const char* hp, char* out)      // "\long dir\file"
{
    int o = 0;
    out[o++] = '\\';
    for (const char* p = hp; *p && o < 259; p++) out[o++] = *p == '/' ? '\\' : *p;
    out[o] = 0;
}

void lfn_fill(u32 rec, const Found& F, int dosfmt)
{
    for (int i = 0; i < 318; i++) wr8(rec + i, 0);
    wr32(rec, F.e.attr);
    u64 mt = F.e.mt, at = F.e.at ? F.e.at : F.e.mt;
    if (dosfmt) {
        u16 t, d;
        to_dos(mt, &t, &d);
        wr16(rec + 4, t); wr16(rec + 6, d);
        wr16(rec + 20, t); wr16(rec + 22, d);
        to_dos(at, &t, &d);
        wr16(rec + 12, t); wr16(rec + 14, d);
    } else {
        u64 ft = filetime(mt), fa = filetime(at);
        wr32(rec + 4, (u32)ft); wr32(rec + 8, (u32)(ft >> 32));
        wr32(rec + 12, (u32)fa); wr32(rec + 16, (u32)(fa >> 32));
        wr32(rec + 20, (u32)ft); wr32(rec + 24, (u32)(ft >> 32));
    }
    wr32(rec + 32, F.e.size);
    pstr(rec + 44, F.name);
    char s[13];
    fcb_to_str(F.sn, s);
    if (name_cmp(s, F.name)) pstr(rec + 304, s);
}

struct { bool on; u16 ds, si; } lfn_saved;

int lfn(struct regs* r)
{
    u8 fn = AL(r);
    char p[300];
    Res R;
    int e, dr;
    switch (fn) {
    case 0x39: case 0x3A: case 0x3B: case 0x41: case 0x43: case 0x4E: case 0x56: case 0x60: case 0x6C: case 0xA0:
        break;
    case 0x47: {                               // current folder of drive DL (0: the current one) to DS:SI
        u32 sda = lfn_sda();
        dr = DL(r) ? DL(r) - 1 : rd8(sda + 0x16);
        Mount* m = mount_of(dr);
        if (!m || !(m->flags & MF_LFN)) return -1;
        char c[70];
        gstr(m->cds, c, 67);
        int bs = rd16(m->cds + 0x4F);
        if ((e = resolve(*m, c + (bs < 67 ? bs : 2), R))) {
            pstr(LIN(r->v86_ds, SI(r)), "");
            return 0;
        }
        char lp[300];
        long_path(R.hp, lp);
        pstr(LIN(r->v86_ds, SI(r)), lp + 1);
        return 0;
    }
    case 0x4F: case 0xA1: {                    // find next / close: BX
        u16 h = BX(r);
        if ((h & 0xFF00) != 0x9F00 || (h & 0xFF) >= NSRCH) return -1;
        Search& S = srch[h & 0xFF];
        if (fn == 0xA1) { S.used = false; return 0; }
        if (!S.used || !S.lfn) return E_NOMORE;
        Found F;
        char nb[256];
        if (srch_next(S, F, nb)) { S.used = true; S.lfn = true; return E_NOMORE; }   // (the handle stays till A1h)
        lfn_fill(LIN(r->v86_es, DI(r)), F, SI(r) & 1);
        CX(r) = 0;
        return 0;
    }
    case 0xA6: {                               // file information by handle BX to DS:DX
        u32 sda = lfn_sda(), psp = (u32)rd16(sda + 0x10) << 4;
        if (BX(r) >= rd16(psp + 0x32)) return -1;
        u8 si = rd8(LIN(rd16(psp + 0x36), rd16(psp + 0x34)) + BX(r));
        if (si == 0xFF) return -1;
        u32 lol = 0;
        for (int i = 0; i < 26; i++) if (mnts[i].used) { lol = mnts[i].lol; break; }
        u32 t = LIN(rd16(lol + 6), rd16(lol + 4)), sft = 0;
        for (int k = 0; k < 64 && rd16(t) != 0xFFFF; k++) {
            u16 cnt = rd16(t + 4);
            if (si < cnt) { sft = t + 6 + si * 0x3B; break; }
            si -= cnt;
            t = LIN(rd16(t + 2), rd16(t));
        }
        int idx = sft ? sft_file(sft) : -1;
        if (idx < 0) return -1;
        OFile& f = ofs[idx];
        if (!(mnts[f.drive].flags & MF_LFN)) return -1;
        if (stale(f)) return E_HANDLE;
        wb_flush(f);
        Attr a;
        if ((e = p_getattr(devs[mnts[f.drive].dev], f.fid, a))) return dos_err(e, E_HANDLE);
        u32 o = LIN(r->v86_ds, DX(r));
        for (int i = 0; i < 52; i++) wr8(o + i, 0);
        wr32(o, rd8(sft + 4));
        u64 ft = filetime(a.mt), fa = filetime(a.at);
        wr32(o + 4, (u32)ft); wr32(o + 8, (u32)(ft >> 32));
        wr32(o + 12, (u32)fa); wr32(o + 16, (u32)(fa >> 32));
        wr32(o + 20, (u32)ft); wr32(o + 24, (u32)(ft >> 32));
        wr32(o + 28, 0x9F9F0000u | f.drive);
        wr32(o + 32, (u32)(a.size >> 32)); wr32(o + 36, (u32)a.size);
        wr32(o + 40, 1);
        wr32(o + 44, (u32)(a.qpath >> 32)); wr32(o + 48, (u32)a.qpath);
        return 0;
    }
    default:
        return -1;
    }
    // the calls with a path at DS:DX (DS:SI for 60h and 6Ch)
    u32 src = (fn == 0x60 || fn == 0x6C) ? LIN(r->v86_ds, SI(r)) : LIN(r->v86_ds, DX(r));
    if ((dr = lfn_path(src, p)) < 0) return dr == -2 ? E_PNF : -1;
    Mount& m = mnts[dr];
    bool ro = m.flags & MF_RO;
    switch (fn) {
    case 0xA0: {                               // volume information: ES:DI the file system's name
        if (p[1]) return -1;
        BX(r) = 0x4002;                        // case kept, long names
        if (CX(r) >= 3) pstr(LIN(r->v86_es, DI(r)), "9P");
        CX(r) = 255; DX(r) = 260;
        return 0;
    }
    case 0x39:                                 // mkdir
        e = resolve(m, p, R);
        if (!e) return E_ACCESS;
        if (e != E_FNF || has_wild(R.last)) return E_PNF;
        if (ro) return E_ACCESS;
        {
            char nm[256];
            host_name(nm, R.last);
            return dos_err(do_mkdir(m, R.parent, nm), E_PNF);
        }
    case 0x3A:                                 // rmdir
        if ((e = resolve(m, p, R))) return E_PNF;
        if (ro || R.root || !(R.e.attr & 0x10)) return E_ACCESS;
        e = do_unlink(m, R.parent, R.name, true);
        dir_forget(m.dev, R.hp);
        return dos_err(e, E_PNF);
    case 0x3B: {                               // chdir: the CDS gets the short path
        if ((e = resolve(m, p, R)) || !(R.root || (R.e.attr & 0x10))) return E_PNF;
        char sp[300];
        if (short_path(m, R.hp, sp, 64) < 0) return E_PNF;
        char cds[70];
        snprintf(cds, sizeof cds, "%c:%s", 'A' + dr, sp);
        for (int i = 0; i < 67; i++) wr8(m.cds + i, 0);
        pstr(m.cds, cds);
        return 0;
    }
    case 0x41: {                               // delete: SI=1 wildcards, CL may / CH must attributes
        char dirp[300];
        memcpy(dirp, p, 300);
        char* l = (char*)last_comp(dirp);
        Res P;
        if (resolve(m, dirp, P) || !(P.root || (P.e.attr & 0x10))) return E_PNF;
        if (ro) return E_ACCESS;
        if (!has_wild(l) || !SI(r)) {
            if ((e = resolve(m, p, R))) return e;
            if (R.e.attr & 0x11) return E_ACCESS;
            return dos_err(do_unlink(m, R.parent, R.name, false), E_FNF);
        }
        int err = 0, found = 0;
        for (int round = 0; round < 1000; round++) {
            Dir* D = dir_get(m, P.hp, true, &err);
            if (!D) break;
            char nm[256];
            bool any = false;
            for (u32 i = 0; i < D->n; i++) {
                Ent& x = D->ents[i];
                char s83[13];
                fcb_to_str(x.sn, s83);
                if ((x.attr & 0x17 & ~CL(r)) || (x.attr & CH(r)) != CH(r)) continue;
                if (!glob(l, ename(D, x)) && !glob(l, s83)) continue;
                memcpy(nm, ename(D, x), x.nlen + 1);
                any = true;
                break;
            }
            if (!any) break;
            if ((e = do_unlink(m, P.hp, nm, false))) return dos_err(e, E_ACCESS);
            found++;
        }
        return found ? 0 : E_FNF;
    }
    case 0x43: {                               // attributes and times: BL
        if ((e = resolve(m, p, R))) return e;
        u8 bl = BL(r);
        if (bl == 0) { CX(r) = R.root ? 0x10 : R.e.attr; return 0; }
        if (bl == 1) { if (ro) return E_ACCESS; return dos_err(do_chmod(m, R, CX(r)), E_FNF); }
        if (bl == 2) { AX(r) = (u16)R.e.size; DX(r) = (u16)(R.e.size >> 16); return 0; }
        if (bl == 4 || bl == 8) { CX(r) = R.e.dtime; DI(r) = R.e.ddate; if (bl == 8) SI(r) = 0; return 0; }
        if (bl == 6) { u16 t, d; to_dos(R.e.at, &t, &d); DI(r) = d; return 0; }
        if (bl == 3 || bl == 5 || bl == 7) {
            if (ro) return E_ACCESS;
            if (bl != 3) return 0;
            Dev& d = devs[m.dev];
            u32 fid;
            if ((e = p_path_fid(d, R.hp, &fid))) return dos_err(e, E_FNF);
            e = p_setattr(d, fid, SA_MTIME | SA_MTIME_SET, 0, 0, from_dos(CX(r), DI(r)));
            p_clunk(d, fid);
            dir_forget(m.dev, R.parent);
            return dos_err(e, E_ACCESS);
        }
        return 1;                              // invalid function
    }
    case 0x4E: {                               // find first: CL may, CH must, SI 1 DOS times, ES:DI the record
        char dirp[300];
        memcpy(dirp, p, 300);
        char* l = (char*)last_comp(dirp);
        Res P;
        if (resolve(m, dirp, P) || !(P.root || (P.e.attr & 0x10))) return E_PNF;
        Search* S = srch_new();
        S->lfn = true;
        S->drive = (u8)dr;
        S->attr = CL(r);
        S->need = CH(r);
        memcpy(S->dir, P.hp, HPMAX);
        {
            size_t n = strlen(l);
            if (n > 259) n = 259;
            memcpy(S->lpat, l, n);
            S->lpat[n] = 0;
        }
        Found F;
        char nb[256];
        if (srch_next(*S, F, nb)) return E_FNF;
        lfn_fill(LIN(r->v86_es, DI(r)), F, SI(r) & 1);
        AX(r) = (u16)(0x9F00 | (S - srch));
        CX(r) = 0;
        return 0;
    }
    case 0x56: {                               // rename DS:DX to ES:DI
        char q[300];
        if (lfn_path(LIN(r->v86_es, DI(r)), q) != dr) return 17;
        if ((e = resolve(m, p, R))) return e;
        if (ro) return E_ACCESS;
        Res T;
        int e2 = resolve(m, q, T);
        if (e2 == E_PNF) return E_PNF;
        if (!e2 && !(T.e.qpath == R.e.qpath && !name_cmp(T.parent, R.parent))) return E_ACCESS;
        if (has_wild(T.last)) return E_PNF;
        if (!e2 && !name_cmp(T.last, R.name)) return 0;
        char nm[256];
        host_name(nm, T.last);
        return dos_err(do_rename(m, R.parent, R.name, T.parent, nm), E_FNF);
    }
    case 0x60: {                               // the canonical name: CL 0 full, 1 short, 2 long; to ES:DI
        e = resolve(m, p, R);
        char out[300];
        u8 cl = CL(r);
        if (e == E_PNF) return E_PNF;
        if (cl == 1 || cl == 2) {
            const char* hp = e ? R.parent : R.hp;
            char sp[300];
            if (cl == 1) { if (short_path(m, hp, sp, 260) < 0) return E_PNF; }
            else long_path(hp, sp);
            if (e) {
                size_t l = strlen(sp);
                if (l > 1) sp[l++] = '\\';
                snprintf(sp + l, sizeof sp - l, "%s", R.last);
            }
            snprintf(out, sizeof out, "%c:%s", 'A' + dr, sp);
        } else snprintf(out, sizeof out, "%c:%s", 'A' + dr, p);
        pstr(LIN(r->v86_es, DI(r)), out);
        return 0;
    }
    case 0x6C: {                               // extended open / create: DOS's 6C00h with the short name
        char par[300];
        memcpy(par, p, 300);
        last_comp(par);
        Res P;
        e = resolve(m, p, R);
        if (e == E_PNF) return E_PNF;
        char sp[300];
        if (!e) {
            if (short_path(m, R.hp, sp, 120) < 0) return E_PNF;
        } else {
            if (has_wild(R.last)) return E_PNF;
            if (resolve(m, par, P)) return E_PNF;
            if (short_path(m, P.hp, sp, 100) < 0) return E_PNF;
            // the short name the new file will have
            char sn[11];
            int ln = (int)strlen(R.last);
            if (!valid83(R.last, ln, sn)) {
                int err = 0;
                Dir* D = dir_get(m, P.hp, false, &err);
                for (u32 salt = 0; ; salt++) {
                    mangle(R.last, ln, salt, sn);
                    bool taken = false;
                    for (u32 i = 0; D && i < D->n; i++) if (!memcmp(D->ents[i].sn, sn, 11)) { taken = true; break; }
                    if (!taken) break;
                }
            }
            pending.on = true;
            pending.drive = (u8)dr;
            memcpy(pending.parent, P.hp, HPMAX);
            memcpy(pending.sn, sn, 11);
            host_name(pending.name, R.last);
            size_t l = strlen(sp);
            if (l > 1) sp[l++] = '\\';
            fcb_to_str(sn, sp + l);
        }
        char full[140];
        snprintf(full, sizeof full, "%c:%s", 'A' + dr, sp);
        u32 sda = m.sda;
        pstr(sda + 0x11E, full);
        // through DOS from vmdos's BIOS: INT 21h AX=6C00h there, then the
        // caller's DS:SI back (fs9p_lfn_done)
        lfn_saved.on = true;
        lfn_saved.ds = (u16)r->v86_ds;
        lfn_saved.si = SI(r);
        AX(r) = 0x6C00;
        r->v86_ds = (sda >> 4);
        SI(r) = (u16)((sda & 15) + 0x11E);
        return -3;
    }
    }
    return -1;
}

void list(u32 out)
{
    char b[1100];
    int n = 0;
    probe_all();
    int shown = 0;
    for (int i = 0; i < ndev; i++) if (!devs[i].gone) shown++;
    if (!shown) n += snprintf(b + n, sizeof b - n, "None (QEMU: -virtfs local,path=DIR,mount_tag=share,security_model=none;\r\n"
                                                    "  or a 9P server on the network: VMDOSNET V: host[:port][/export])\r\n");
    for (int i = 0; i < ndev; i++) {
        if (devs[i].gone) continue;
        n += snprintf(b + n, sizeof b - n, "  %-12s", devs[i].tag);
        if (devs[i].tcp && devs[i].aname[0]) n += snprintf(b + n, sizeof b - n, " %s", devs[i].aname);
        int k = 0;
        for (int dr = 0; dr < 26; dr++)
            if (mnts[dr].used && mnts[dr].dev == i) {
                Mount& m = mnts[dr];
                n += snprintf(b + n, sizeof b - n, "%s%c:%s%s%s", k++ ? ", " : " in ", 'A' + dr,
                              m.flags & MF_LFN ? " long names" : "", m.flags & MF_WT ? " write-through" : "",
                              m.flags & MF_RO ? " read-only" : "");
            }
        if (!k) n += snprintf(b + n, sizeof b - n, " (not in a drive)");
        if (!devs[i].up || devs[i].broken) n += snprintf(b + n, sizeof b - n, " - not working");
        n += snprintf(b + n, sizeof b - n, "\r\n");
        if (n > 900) break;
    }
    if (nblk) n += snprintf(b + n, sizeof b - n, "File cache %u KiB\r\n", nblk * BLK / 1024);
    b[n++] = '$';
    for (int i = 0; i < n; i++) wr8(out + i, (u8)b[i]);
}

bool alloc_caches(u32 kb)
{
    if (!sents) {
        sents = (Ent*)phys_try_alloc(MAXENT * sizeof(Ent));
        snames = (char*)phys_try_alloc(NAMEBUF);
        sidx = (u16*)phys_try_alloc(MAXENT * 2);
        shash = (u16*)phys_try_alloc(16384 * 2);
        if (!sents || !snames || !sidx || !shash) return false;
    }
    if (!nblk && kb) {
        u32 n = kb / (BLK / 1024);
        while (n >= 8) {
            if (phys_spare() > n * BLK + (2u << 20)) {
                bdata = (u8*)phys_try_alloc(n * BLK);
                if (bdata) break;
            }
            n /= 2;
        }
        if (n >= 8 && bdata) {
            blks = (Blk*)phys_try_alloc((n * sizeof(Blk) + 4095) & ~4095u);
            if (blks) nblk = n;
            else { phys_release(bdata, n * BLK); bdata = 0; }
        }
    }
    return true;
}

// A host share over TCP (kind 1): an existing connection to the same place,
// or a new one. The device, or -(an error as mount's).
int tcp_dev(const Tcp9pCfg& net, const char* name, const char* aname, u32 uid)
{
    for (int i = 0; i < ndev; i++) {
        Dev& d = devs[i];
        if (d.tcp && !d.gone && d.net.family == net.family && d.net.port == net.port &&
            !memcmp(d.net.server, net.server, 16) && !strncmp(d.aname, aname, 128) && d.uid == uid) return i;
    }
    int dv = -1;
    for (int i = 0; i < ndev; i++) if (devs[i].tcp && devs[i].gone) { dv = i; break; }
    if (dv < 0) { if (ndev == MAXDEV) return -12; dv = ndev; }
    Dev& d = devs[dv];
    u8 *btx = d.btx, *brx = d.brx, *stx = d.stx, *srx = d.srx;
    memset(&d, 0, sizeof d);
    d.btx = btx ? btx : (u8*)phys_try_alloc(MSIZE); d.brx = brx ? brx : (u8*)phys_try_alloc(MSIZE);
    d.stx = stx ? stx : (u8*)phys_try_alloc(NSLOT * SLOT); d.srx = srx ? srx : (u8*)phys_try_alloc(NSLOT * SLOT);
    if (!d.btx || !d.brx || !d.stx || !d.srx) return -5;
    d.tcp = true;
    d.net = net;
    d.uid = uid;
    memcpy(d.aname, aname, 128);
    d.aname[127] = 0;
    memcpy(d.tag, name, 32);
    d.tag[31] = 0;
    d.conn = tcp9p_open(net);
    if (d.conn < 0) { d.gone = true; if (dv == ndev) ndev++; return d.conn == -3 ? -11 : d.conn == -2 ? -10 : -5; }
    if (dv == ndev) ndev++;
    if (!dev_attach(d)) {
        tcp9p_close(d.conn);
        d.gone = true;
        return -4;
    }
    kprintf("9p: connected to %s%s%s, msize %u\n", d.tag, aname[0] ? " " : "", aname, d.msize);
    return dv;
}

bool dev_in_use(int dv)
{
    for (int i = 0; i < 26; i++) if (mnts[i].used && mnts[i].dev == dv) return true;
    return false;
}

// INT 2Fh AX=5652h BX=1, ES:DI:
//   00 dw SDA off, seg; 04 dw list of lists off, seg;
//   08 db drive (0 = A:), flags (1 long names, 2 write-through, 4 read-only);
//   0A dw cache KiB (0: 4096), ttl ms (0: 1000);
//   0E db kind: 0 a virtio-9p share, 1 a 9P server over TCP; 0F dd uid to attach as
//   13 db tag (32, ASCIIZ: virtio: the mount tag, "": the first; TCP: the name shown)
//   33 Tcp9pCfg (TCP: the addresses VMDOSNET.EXE found); then db aname (128: the export)
const u32 MB_KIND = 0x0E, MB_UID = 0x0F, MB_TAG = 0x13, MB_NET = 0x33, MB_ANAME = MB_NET + sizeof(Tcp9pCfg);
int mount(struct regs* r)
{
    u32 pb = LIN(r->v86_es, DI(r));
    u32 sda = LIN(rd16(pb + 2), rd16(pb)), lol = LIN(rd16(pb + 6), rd16(pb + 4));
    int dr = rd8(pb + 8);
    u8 flags = rd8(pb + 9);
    u32 kb = rd16(pb + 10), ttl = rd16(pb + 12);
    char tag[64];
    gstr(pb + MB_TAG, tag, 32);
    if (hv_active) return 9;                   // VMHV: DOS's INT 2Fh calls don't come here
    if (dr < 2 || dr >= 26 || dr >= rd8(lol + 0x21)) return 3;
    u32 cds = LIN(rd16(lol + 0x18), rd16(lol + 0x16)) + dr * 0x58;
    u16 cf = rd16(cds + 0x43);
    if ((cf & 0xC000) && !mnts[dr].used) return 3;     // a drive already
    if (mnts[dr].used) for (int i = 0; i < NOF; i++) if (ofs[i].used && ofs[i].drive == dr) return 8;
    if (!alloc_caches(kb ? kb : 4096)) return 5;
    int dv = -1;
    if (rd8(pb + MB_KIND) == 1) {
        Tcp9pCfg net;
        memcpy(&net, gptr(pb + MB_NET), sizeof net);
        char aname[128];
        gstr(pb + MB_ANAME, aname, 128);
        dv = tcp_dev(net, tag, aname, rd32(pb + MB_UID));
        if (dv < 0) return -dv;
    } else {
        probe_all();
        for (int i = 0; i < ndev; i++)
            if (!devs[i].tcp && (!tag[0] || ieq(tag, (int)strlen(tag), devs[i].tag, (int)strlen(devs[i].tag)))) { dv = i; break; }
        if (dv < 0) {
            for (int i = 0; i < ndev; i++) if (!devs[i].tcp) return 2;
            return 1;
        }
    }
    int old = mnts[dr].used ? mnts[dr].dev : -1;
    Dev& d = devs[dv];
    if (!d.up || d.broken) {
        tick_poll_begin();
        d.broken = false;
        bool ok_ = dev_attach(d);
        tick_poll_end();
        if (!ok_) return 4;
    }
    Mount& m = mnts[dr];
    if (!m.used) nmnt++;
    m.used = true;
    m.dev = (u8)dv;
    m.flags = flags;
    m.ttl = ttl ? ttl : 1000;
    m.sda = sda;
    m.lol = lol;
    m.cds = cds;
    for (int i = 0; i < 67; i++) wr8(cds + i, 0);
    wr8(cds, (u8)('A' + dr)); wr8(cds + 1, ':'); wr8(cds + 2, '\\');
    wr16(cds + 0x43, 0xC000);                  // network + physical: the redirector's
    wr32(cds + 0x45, 0);
    wr32(cds + 0x49, 0);
    wr16(cds + 0x4D, 0xFFFF);
    wr16(cds + 0x4F, 2);
    for (int i = 0; i < NDIR; i++) if (dirs[i].used && dirs[i].dev == dv) dir_drop(&dirs[i]);
    if (old >= 0 && old != dv && devs[old].tcp && !dev_in_use(old)) { tcp9p_close(devs[old].conn); devs[old].gone = true; }
    kprintf("9p: \"%s\" in %c:%s%s%s, cache %u KiB\n", d.tag, 'A' + dr, flags & MF_LFN ? ", long names" : "",
            flags & MF_WT ? ", write-through" : "", flags & MF_RO ? ", read-only" : "", nblk * BLK / 1024);
    return 0;
}

// ---- files for the monitor itself (VMCD's images on a share)
const int NKF = 32;
struct KFile { bool used; u8 dev; u32 fid, ver, size, last_end, gen; u64 qpath; };
KFile kfs[NKF];
int in9p;                                      // a call is under way (CD audio's timer reads wait)

int unmount(int dr)
{
    Mount* m = mount_of(dr);
    if (!m) return 7;
    for (int i = 0; i < NOF; i++) if (ofs[i].used && ofs[i].drive == dr) return 8;
    wr16(m->cds + 0x43, 0);
    m->used = false;
    nmnt--;
    int dv = m->dev;
    if (devs[dv].tcp && !dev_in_use(dv)) {     // the last drive on that connection
        for (int i = 0; i < NKF; i++) if (kfs[i].used && kfs[i].dev == dv) return 0;   // VMCD still reads from it
        tcp9p_close(devs[dv].conn);
        devs[dv].gone = true;
    }
    return 0;
}


} // namespace

// "X:\PATH\FILE" on a share opened for reading: a handle (0...), or -2 not
// found, -3 path not found, -4 not a share, -6 no room, -7 the host failed
int fs9p_kopen(const char* path, u32* size)
{
    Mount* m = path[0] && path[1] == ':' ? mount_of(up(path[0]) - 'A') : 0;
    if (!m) return -4;
    if (in9p) return -7;
    in9p++;
    tick_poll_begin();
    Res R;
    int e = resolve(*m, path + 2, R), h = -6;
    Dev& d = devs[m->dev];
    u32 fid;
    Attr a;
    if (e) h = e == E_FNF ? -2 : -3;
    else if (R.root || (R.e.attr & 0x10)) h = -2;
    else if (p_path_fid(d, R.hp, &fid)) h = -2;
    else if (p_lopen(d, fid, L_RDONLY) || p_getattr(d, fid, a)) { p_clunk(d, fid); h = -7; }
    else {
        for (int i = 0; i < NKF; i++) if (!kfs[i].used) { h = i; break; }
        if (h < 0) p_clunk(d, fid);
        else {
            KFile& k = kfs[h];
            k.used = true; k.dev = m->dev; k.fid = fid; k.gen = d.gen; k.qpath = a.qpath; k.ver = ver_of(a);
            k.size = a.size > 0xFFFFFFFFull ? 0xFFFFFFFFu : (u32)a.size;
            k.last_end = 0xFFFFFFFFu;
            *size = k.size;
        }
    }
    tick_poll_end();
    in9p--;
    return h;
}

// len bytes at off: 0, or -1 (short, or the host failed, or busy)
int fs9p_kread(int h, u32 off, u32 len, void* buf)
{
    if (h < 0 || h >= NKF || !kfs[h].used || in9p) return -1;
    KFile& k = kfs[h];
    if (k.gen != devs[k.dev].gen) return -1;
    in9p++;
    tick_poll_begin();
    long n = cread(k.dev, k.fid, k.qpath, k.ver, &k.last_end, off, len, (u8*)buf);
    tick_poll_end();
    in9p--;
    return n == (long)len ? 0 : -1;
}

void fs9p_kclose(int h)
{
    if (h < 0 || h >= NKF || !kfs[h].used) return;
    in9p++;
    tick_poll_begin();
    if (kfs[h].gen == devs[kfs[h].dev].gen) p_clunk(devs[kfs[h].dev], kfs[h].fid);
    tick_poll_end();
    in9p--;
    kfs[h].used = false;
}

// INT 2Fh AX=5652h (VMDOSNET.COM). BX=0: the shares to ES:DI ($-terminated);
// 1: mount (above); 2: drive DL out; 3: write out what is gathered.
// AX = 0 or an error; BX = 564Dh.
void fs9p_api(struct regs* r)
{
    in9p++;
    tick_poll_begin();
    switch (BX(r)) {
    case 0: list(LIN(r->v86_es, DI(r))); AX(r) = 0; break;
    case 1: AX(r) = (u16)mount(r); break;
    case 2: fs9p_flush_all(); AX(r) = (u16)unmount(DL(r)); break;
    case 3: fs9p_flush_all(); AX(r) = 0; break;
    default: AX(r) = 1;
    }
    tick_poll_end();
    in9p--;
    BX(r) = 0x564D;
}

// INT 2Fh AH=11h from DOS: 1 handled (CF, AX set), 0 not ours: the chain's
int fs9p_redir(struct regs* r)
{
    if (!nmnt) return 0;
    u8 fn = AL(r);
    int e = -1;
    in9p++;
    tick_poll_begin();
    switch (fn) {
    case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x21: {
        int i = sft_file(LIN(r->v86_es, DI(r)));
        if (i >= 0) e = redir_sft(r, i, fn);
        break;
    }
    case 0x0C: case 0xA3: {
        u32 cds = LIN(r->v86_es, DI(r));
        int dr = up((char)rd8(cds)) - 'A';
        Mount* m = mount_of(dr);
        if (m && rd8(cds + 1) == ':') e = redir(r, *m, dr, fn);
        break;
    }
    case 0x1C: {
        u32 sb = LIN(r->v86_es, DI(r));
        u8 b = rd8(sb);
        Mount* m = (b & 0x80) ? mount_of(b & 0x3F) : 0;
        if (m && rd32(sb + 17) == SMAGIC) e = redir(r, *m, b & 0x3F, fn);
        break;
    }
    case 0x01: case 0x03: case 0x05: case 0x0E: case 0x0F: case 0x11: case 0x13: case 0x16: case 0x17:
    case 0x1B: case 0x2E: {
        u32 sda = lfn_sda();
        char p[128];
        int dr = path_of(sda, 0, p);
        Mount* m = mount_of(dr);
        if (m) e = redir(r, *m, dr, fn);
        break;
    }
    case 0x1D: case 0x20: case 0x22:           // close all, flush all, a program ends: then the others'
        fs9p_flush_all();
        break;
    }
    if (fn != 0x07 && fn != 0x06) flush_due(500);
    tick_poll_end();
    in9p--;
    if (debug_level >= (fn == 0x08 || fn == 0x09 || fn == 0x1C ? 3 : 2) && e != -1) {
        char p[128];
        u32 sda = lfn_sda();
        gstr(sda + 0x9E, p, 80);
        kprintf("9p: 11%02x %s -> %d\n", fn, p, e);
    }
    if (e < 0) return 0;
    if (e) fail(r, e); else ok(r);
    return 1;
}

// INT 21h AH=71h: 1 handled, 0 not ours (to DOS)
int fs9p_lfn(struct regs* r)
{
    if (!nmnt) return 0;
    in9p++;
    tick_poll_begin();
    u8 fn = AL(r);
    int e = lfn(r);
    tick_poll_end();
    in9p--;
    if (debug_level >= (fn == 0x4F || fn == 0x60 ? 3 : 2) && (e != -1 || debug_level >= 4)) {
        char p[128];
        gstr(LIN(r->v86_ds, (fn == 0x60 || fn == 0x6C) ? SI(r) : DX(r)), p, 80);
        kprintf("9p: 71%02x %s -> %d\n", fn, p, e);
    }
    if (e == -1) return 0;
    if (e == -3) {                             // via vmdos's BIOS: INT 21h there, then back
        v86_reflect(r, 0x21);
        r->cs = 0xF000;
        r->eip = rd16(0xF0000u + 0x232);
        return 1;
    }
    if (e) fail(r, e); else ok(r);
    return 1;
}

// After DOS's 6C00h for a 716Ch: the caller's DS:SI back
void fs9p_lfn_done(struct regs* r)
{
    if (lfn_saved.on) {
        r->v86_ds = lfn_saved.ds;
        SI(r) = lfn_saved.si;
        lfn_saved.on = false;
    }
    pending.on = false;
}

void fs9p_flush_all(void)
{
    if (!nmnt) return;
    flush_due(0);
}

void fs9p_idle(void)
{
    if (!nmnt) return;
    bool any = false;
    for (int i = 0; i < NOF; i++) if (ofs[i].used && ofs[i].wb_len) any = true;
    if (!any || in9p) return;
    in9p++;
    tick_poll_begin();
    flush_due(200);
    tick_poll_end();
    in9p--;
}
