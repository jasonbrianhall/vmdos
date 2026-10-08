/* A small read-only FAT12/16/32 reader for the monitor: finds a file by its
   DOS path (8.3 names, as INT 21h AH=60h gives them) and lists the disk
   sectors it occupies. Used to pick C:'s partition (it must hold
   KERNEL.SYS) and to read CD images straight from files on C: (cd.c). */
#include "kernel.h"

static int vol_read(struct fatvol *v, u32 lba, void *buf)
{
    return v->rd(v->base + lba, 1, buf);
}

int fat_mount(struct fatvol *v, int (*rd)(u32 lba, u32 n, void *buf), u32 base)
{
    u8 *b = v->sec;
    v->rd = rd; v->base = base; v->fat_cached = (u32)-1;
    if (rd(base, 1, b)) return -1;
    if (b[510] != 0x55 || b[511] != 0xAA || *(u16 *)(b + 11) != 512) return -1;
    u32 spc = b[13], resv = *(u16 *)(b + 14), nfat = b[16], rootent = *(u16 *)(b + 17);
    u32 tot = *(u16 *)(b + 19), fatsz = *(u16 *)(b + 22);
    if (!tot) tot = *(u32 *)(b + 32);
    if (!fatsz) fatsz = *(u32 *)(b + 36);
    if (!spc || (spc & (spc - 1)) || !nfat || !fatsz || !resv) return -1;
    v->spc = spc;
    v->fat_start = resv;
    v->root_start = resv + nfat * fatsz;
    v->root_secs = (rootent * 32 + 511) / 512;
    v->data_start = v->root_start + v->root_secs;
    if (tot <= v->data_start) return -1;
    v->nclus = (tot - v->data_start) / spc;
    v->type = v->nclus < 4085 ? 12 : v->nclus < 65525 ? 16 : 32;
    v->root_clus = v->type == 32 ? *(u32 *)(b + 44) : 0;
    v->total = tot;
    return 0;
}

static u32 fat_next(struct fatvol *v, u32 c)
{
    u32 off = v->type == 12 ? c + c / 2 : v->type == 16 ? c * 2 : c * 4;
    u32 s = v->fat_start + off / 512, o = off % 512;
    if (v->fat_cached != s) {
        if (vol_read(v, s, v->fat) || vol_read(v, s + 1, v->fat + 512)) return 0x0FFFFFFF;
        v->fat_cached = s;
    }
    u32 e;
    if (v->type == 12) { e = v->fat[o] | v->fat[o + 1] << 8; e = (c & 1) ? e >> 4 : e & 0xFFF; if (e >= 0xFF7) e = 0x0FFFFFFF; }
    else if (v->type == 16) { e = v->fat[o] | v->fat[o + 1] << 8; if (e >= 0xFFF7) e = 0x0FFFFFFF; }
    else { e = *(u32 *)(v->fat + o) & 0x0FFFFFFF; if (e >= 0x0FFFFFF7) e = 0x0FFFFFFF; }
    if (e < 2 || (e != 0x0FFFFFFF && e >= v->nclus + 2)) e = 0x0FFFFFFF;
    return e;
}

static u32 clus_lba(struct fatvol *v, u32 c) { return v->data_start + (c - 2) * v->spc; }

/* "NAME.EXT" (one path component) -> 11-byte directory name; -1 if not 8.3. */
static int name83(const char *p, int n, u8 out[11])
{
    memset(out, ' ', 11);
    int i = 0, k = 0;
    for (; i < n && p[i] != '.'; i++) { if (k == 8) return -1; char c = p[i]; out[k++] = (u8)(c >= 'a' && c <= 'z' ? c - 32 : c); }
    if (i < n) {
        i++;
        for (k = 8; i < n; i++) { if (k == 11 || p[i] == '.') return -1; char c = p[i]; out[k++] = (u8)(c >= 'a' && c <= 'z' ? c - 32 : c); }
    }
    return k || out[0] != ' ' ? 0 : -1;
}

/* Search directory (cluster c, 0 = root) for name; fills entry fields. */
static int dir_find(struct fatvol *v, u32 c, const u8 name[11], u32 *clus, u32 *size, int *isdir)
{
    u32 nsec = (c == 0 && v->type != 32) ? v->root_secs : v->spc;
    if (c == 0 && v->type == 32) c = v->root_clus;
    for (int guard = 0; guard < 65536; guard++) {
        u32 lba = (c == 0) ? v->root_start : clus_lba(v, c);
        for (u32 s = 0; s < nsec; s++) {
            if (vol_read(v, lba + s, v->sec)) return -1;
            for (int e = 0; e < 512; e += 32) {
                u8 *d = v->sec + e;
                if (d[0] == 0) return -1;
                if (d[0] == 0xE5 || d[11] == 0x0F || (d[11] & 8)) continue;
                if (memcmp(d, name, 11)) continue;
                *clus = *(u16 *)(d + 26) | (v->type == 32 ? (u32)*(u16 *)(d + 20) << 16 : 0);
                *size = *(u32 *)(d + 28);
                *isdir = (d[11] & 0x10) != 0;
                return 0;
            }
        }
        if (c == 0) return -1;                                   /* FAT12/16 root: fixed size */
        c = fat_next(v, c);
        if (c == 0x0FFFFFFF) return -1;
    }
    return -1;
}

/* Path like "\ISOS\WAR2.ISO" (or "C:\..." or with '/'); a file, not a folder. */
int fat_lookup(struct fatvol *v, const char *path, u32 *clus, u32 *size)
{
    if (path[0] && path[1] == ':') path += 2;
    u32 c = 0;
    int isdir = 1;
    *size = 0;
    while (*path) {
        while (*path == '\\' || *path == '/') path++;
        if (!*path) break;
        int n = 0;
        while (path[n] && path[n] != '\\' && path[n] != '/') n++;
        u8 nm[11];
        if (!isdir || name83(path, n, nm)) return -1;
        if (dir_find(v, c, nm, &c, size, &isdir)) return -1;
        path += n;
    }
    if (isdir) return -1;
    *clus = c;
    return 0;
}

/* The disk sectors (absolute LBAs) of a file's first 'bytes' bytes as runs
   (ext NULL: just count them). */
int fat_extents(struct fatvol *v, u32 clus, u32 bytes, struct extent *ext, int max)
{
    u32 need = (bytes + v->spc * 512 - 1) / (v->spc * 512);
    int n = 0;
    while (need) {
        if (clus < 2 || clus == 0x0FFFFFFF) return -1;
        u32 lba = v->base + clus_lba(v, clus);
        static u32 last;                                          /* end of the previous run (counting) */
        if (n && (ext ? ext[n - 1].lba + ext[n - 1].count : last) == lba) { if (ext) ext[n - 1].count += v->spc; }
        else { if (n == max) return -2; if (ext) { ext[n].lba = lba; ext[n].count = v->spc; } n++; }
        last = lba + v->spc;
        need--;
        if (need) clus = fat_next(v, clus);
    }
    return n;
}
