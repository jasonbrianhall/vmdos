/*
   ZCOPY: XCOPY with long file names and a progress bar.

     ZCOPY source [destination] [/A /C /D[:date] /E /F /H /I /L /M /N /P
                                 /Q /R /S /T /V /W /Y /-Y /NP /EXCLUDE:list]

   The switches are FreeDOS XCOPY's (and COPYCMD=/Y, /N or /-Y presets the
   overwrite prompt the same way). On top of them:
     - long names: where DOS has the long-name API (INT 21h 71xxh: DOSLFN, a
       VMDOSNET drive) files and folders are found, created and shown by
       their long names, and quoted names with spaces work; 8.3 otherwise.
     - progress: the size of the whole copy is counted first, then one line
       shows the current file's percentage, a bar for the whole copy, the
       files done, the speed and the time left. /NP turns it off (as does
       output to a file or a pipe).
     - /EXCLUDE:file1[+file2...]: files listing strings, one per line; a
       source path containing any of them (any case) is skipped.
   Ctrl-C stops between blocks and removes the half-written file.

   Exit codes as XCOPY: 0 copied, 1 no files found, 2 stopped with Ctrl-C,
   4 bad parameters or not enough memory, 5 a disk error.

   Part of vmdos (GPL-2.0-or-later); built with Open Watcom:
     wcl -q -bt=dos -ml -0 -os -s -za99 -k16384 zcopy.c
*/
#include <dos.h>
#include <i86.h>
#include <io.h>
#include <malloc.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define PATHMAX 300                     /* 260 for a long path, and room */

typedef unsigned long long u64;

/* ------------------------------------------------------------------ */
/* switches                                                           */
static int sw_archive, sw_continue, sw_empty, sw_full, sw_hidden, sw_intodir,
           sw_list, sw_reset, sw_prompt, sw_quiet, sw_readonly, sw_subdir,
           sw_tree, sw_verify, sw_wait, sw_noprog;
static int confirm_mode = 2;            /* 0 overwrite, 1 skip, 2 ask */
static int date_mode;                   /* 0 any, -1 newer than dest, 1 on/after date_min */
static unsigned long date_min;          /* DOS date << 16 */
static char **excl; static int nexcl;

/* state */
static int progress;                    /* drawing the progress line */
static int scanning;                    /* first pass: counting */
static int file_found;
static unsigned long files_copied, files_total, files_done;
static u64 bytes_total, bytes_done;
static unsigned long t_start;
static int drawn;                       /* a progress line is on screen */
static unsigned cols = 80;
static volatile int stop;
static unsigned char old_verify;
static char __far *buf; static unsigned bufsize;
static char dest_drive;

static unsigned long ticks(void) { return *(unsigned long __far *)MK_FP(0x40, 0x6C); }

/* ------------------------------------------------------------------ */
/* DOS calls, long-name API first, the 8.3 one when the drive has none  */
/* (AX=7100h, carry set). Every call tries again: drives differ.        */

static union REGS r; static struct SREGS s;

static int nolfn(void) { return r.x.cflag && r.x.ax == 0x7100; }

struct w32find {
    unsigned long attr;
    unsigned long ctime[2], atime[2], mtime[2];
    unsigned long sizehi, sizelo;
    char res[8];
    char lname[260];
    char sname[14];
};

struct zfind {
    int lfn; unsigned handle;
    struct w32find w;
    struct find_t f;
    /* the result */
    char *name; unsigned attr; unsigned long size; unsigned long stamp; /* date<<16|time */
};

static void zf_fill(struct zfind *z)
{
    if (z->lfn) {
        z->name = z->w.lname[0] ? z->w.lname : z->w.sname;
        z->attr = (unsigned)z->w.attr;
        z->size = z->w.sizelo;
        z->stamp = z->w.mtime[0];       /* SI=1: DOS time in the low word, date in the high */
    } else {
        z->name = z->f.name;
        z->attr = z->f.attrib;
        z->size = z->f.size;
        z->stamp = ((unsigned long)z->f.wr_date << 16) | z->f.wr_time;
    }
}

/* 0: found one */
static int zfindfirst(const char *spec, unsigned attr, struct zfind *z)
{
    segread(&s);
    r.x.ax = 0x714E; r.x.cx = attr; r.x.si = 1;
    r.x.dx = FP_OFF(spec); s.ds = FP_SEG(spec);
    r.x.di = FP_OFF(&z->w); s.es = FP_SEG(&z->w);
    intdosx(&r, &r, &s);
    if (!r.x.cflag) { z->lfn = 1; z->handle = r.x.ax; zf_fill(z); return 0; }
    if (!nolfn()) return 1;
    z->lfn = 0;
    if (_dos_findfirst(spec, attr, &z->f)) return 1;
    zf_fill(z);
    return 0;
}

static int zfindnext(struct zfind *z)
{
    if (z->lfn) {
        segread(&s);
        r.x.ax = 0x714F; r.x.bx = z->handle; r.x.si = 1;
        r.x.di = FP_OFF(&z->w); s.es = FP_SEG(&z->w);
        intdosx(&r, &r, &s);
        if (r.x.cflag) return 1;
    } else if (_dos_findnext(&z->f)) return 1;
    zf_fill(z);
    return 0;
}

static void zfindclose(struct zfind *z)
{
    if (z->lfn) { r.x.ax = 0x71A1; r.x.bx = z->handle; intdos(&r, &r); z->lfn = 0; }
}

/* the attributes, or -1 */
static int zgetattr(const char *p)
{
    segread(&s);
    r.x.ax = 0x7143; r.h.bl = 0; r.x.dx = FP_OFF(p); s.ds = FP_SEG(p);
    intdosx(&r, &r, &s);
    if (nolfn()) { r.x.ax = 0x4300; r.x.dx = FP_OFF(p); s.ds = FP_SEG(p); intdosx(&r, &r, &s); }
    return r.x.cflag ? -1 : (int)(r.x.cx & 0xFF);
}

static int zsetattr(const char *p, unsigned a)
{
    segread(&s);
    r.x.ax = 0x7143; r.h.bl = 1; r.x.cx = a; r.x.dx = FP_OFF(p); s.ds = FP_SEG(p);
    intdosx(&r, &r, &s);
    if (nolfn()) { r.x.ax = 0x4301; r.x.cx = a; r.x.dx = FP_OFF(p); s.ds = FP_SEG(p); intdosx(&r, &r, &s); }
    return r.x.cflag ? -1 : 0;
}

static int zmkdir(const char *p)
{
    segread(&s);
    r.x.ax = 0x7139; r.x.dx = FP_OFF(p); s.ds = FP_SEG(p);
    intdosx(&r, &r, &s);
    if (nolfn()) { r.h.ah = 0x39; r.x.dx = FP_OFF(p); s.ds = FP_SEG(p); intdosx(&r, &r, &s); }
    return r.x.cflag ? -1 : 0;
}

static void zunlink(const char *p)
{
    segread(&s);
    r.x.ax = 0x7141; r.x.si = 0; r.x.dx = FP_OFF(p); s.ds = FP_SEG(p);
    intdosx(&r, &r, &s);
    if (nolfn()) { r.h.ah = 0x41; r.x.dx = FP_OFF(p); s.ds = FP_SEG(p); intdosx(&r, &r, &s); }
}

/* a handle, or -1. create: make or truncate, for writing */
static int zopen(const char *p, int create)
{
    segread(&s);
    r.x.ax = 0x716C; r.x.bx = create ? 0x0001 : 0x0000; r.x.cx = 0;
    r.x.dx = create ? 0x0012 : 0x0001;
    r.x.si = FP_OFF(p); s.ds = FP_SEG(p);
    intdosx(&r, &r, &s);
    if (nolfn()) {
        r.x.ax = create ? 0x3C00 : 0x3D00; r.x.cx = 0;
        r.x.dx = FP_OFF(p); s.ds = FP_SEG(p);
        intdosx(&r, &r, &s);
    }
    return r.x.cflag ? -1 : (int)r.x.ax;
}

/* bytes moved, or -1 */
static long zio(int h, char __far *b, unsigned n, int write)
{
    segread(&s);
    r.h.ah = write ? 0x40 : 0x3F; r.x.bx = h; r.x.cx = n;
    r.x.dx = FP_OFF(b); s.ds = FP_SEG(b);
    intdosx(&r, &r, &s);
    return r.x.cflag ? -1L : (long)r.x.ax;
}

static void zclose(int h) { r.h.ah = 0x3E; r.x.bx = h; intdos(&r, &r); }

static void zsettime(int h, unsigned long stamp)
{
    r.x.ax = 0x5701; r.x.bx = h; r.x.cx = (unsigned)stamp; r.x.dx = (unsigned)(stamp >> 16);
    intdos(&r, &r);
}

/* the full path of in: long form if the API is there; "" if not valid */
static void ztruename(const char *in, char *out)
{
    segread(&s);
    for (int cl = 2; cl >= 0; cl -= 2) {             /* long form (exists), then full (may not) */
        r.x.ax = 0x7160; r.x.cx = cl;
        r.x.si = FP_OFF(in); s.ds = FP_SEG(in);
        r.x.di = FP_OFF(out); s.es = FP_SEG(out);
        intdosx(&r, &r, &s);
        if (!r.x.cflag) return;
        if (nolfn()) break;
    }
    r.h.ah = 0x60;
    r.x.si = FP_OFF(in); s.ds = FP_SEG(in);
    r.x.di = FP_OFF(out); s.es = FP_SEG(out);
    intdosx(&r, &r, &s);
    if (r.x.cflag) out[0] = 0;
}

/* free space on drive d (1 = A:) as clusters of *csize bytes */
static void zfree(int d, unsigned long *csize, unsigned long *nfree)
{
    static struct { unsigned size, ver; unsigned long spc, bps, avail, total, x[5]; } f;
    static char root[4] = "?:\\";
    root[0] = (char)('A' + d - 1);
    segread(&s);
    memset(&f, 0, sizeof f);
    r.x.ax = 0x7303; r.x.cx = sizeof f;
    r.x.dx = FP_OFF(root); s.ds = FP_SEG(root);
    r.x.di = FP_OFF(&f); s.es = FP_SEG(&f);
    intdosx(&r, &r, &s);
    if (!r.x.cflag && f.spc && f.bps) { *csize = f.spc * f.bps; *nfree = f.avail; return; }
    r.h.ah = 0x36; r.h.dl = (unsigned char)d; intdos(&r, &r);
    if (r.x.ax == 0xFFFF) { *csize = 1; *nfree = 0xFFFFFFFFUL; return; }
    *csize = (unsigned long)r.x.ax * r.x.cx; *nfree = r.x.bx;
}

/* ------------------------------------------------------------------ */
/* output                                                             */

static void fmt_size(char *o, u64 n)
{
    static const char *u[] = { "KB", "MB", "GB" };
    if (n < 1024) { sprintf(o, "%u B", (unsigned)n); return; }
    int i = 0; u64 d = 1024;
    while (i < 2 && n >= d * 1024) { d *= 1024; i++; }
    unsigned long whole = (unsigned long)(n / d), tenth = (unsigned long)(n % d * 10 / d);
    if (whole >= 100) sprintf(o, "%lu %s", whole, u[i]);
    else sprintf(o, "%lu.%lu %s", whole, tenth, u[i]);
}

static void clear_line(void)
{
    if (!drawn) return;
    printf("\r%*s\r", cols - 1, "");
    fflush(stdout);
    drawn = 0;
}

/* file: bytes of the current file done of fsize */
static void draw(unsigned long fdone, unsigned long fsize)
{
    static char line[200], tail[80], a[16], spd[16];
    unsigned fp = fsize ? (unsigned)((u64)fdone * 100 / fsize) : 100;
    u64 all = bytes_done + fdone;
    unsigned tp = bytes_total ? (unsigned)(all * 100 / bytes_total) : 100;
    if (tp > 100) tp = 100;
    unsigned long el = ticks() - t_start;              /* 18.2 a second */
    spd[0] = 0; a[0] = 0;
    if (el > 9 && all) {
        u64 bps = all * 182 / (el * 10);
        fmt_size(spd, bps); strcat(spd, "/s");
        if (bps && bytes_total > all) {
            unsigned long left = (unsigned long)((bytes_total - all) / bps);
            if (left >= 3600) sprintf(a, "%lu:%02lu:%02lu", left / 3600, left / 60 % 60, left % 60);
            else sprintf(a, "%lu:%02lu", left / 60, left % 60);
        }
    }
    sprintf(tail, " %3u%%  %lu/%lu  %s%s%s", tp, files_done < files_total ? files_done + 1 : files_total, files_total, spd, a[0] ? "  " : "", a);
    int bar = (int)cols - 1 - 6 - 2 - (int)strlen(tail);        /* "100% [" ... "]" */
    if (bar < 10) bar = 10;
    int fill = bar * tp / 100;
    char *p = line;
    p += sprintf(p, "\r%3u%% [", fp);
    for (int i = 0; i < bar; i++) *p++ = i < fill ? '\xDB' : '\xB0';
    *p++ = ']';
    strcpy(p, tail);
    fputs(line, stdout);
    fflush(stdout);
    drawn = 1;
}

/* a key from the keyboard, or a character of redirected input (-1 at its
   end), upper case; Ctrl-C stops */
static int getkey(void)
{
    if (!isatty(0)) {
        static char c;
        do {
            segread(&s);
            r.h.ah = 0x3F; r.x.bx = 0; r.x.cx = 1;
            r.x.dx = FP_OFF(&c); s.ds = FP_SEG(&c);
            intdosx(&r, &r, &s);
            if (r.x.cflag || r.x.ax == 0 || c == 0x1A) return -1;
        } while (c == '\r' || c == '\n' || c == ' ');
        return toupper((unsigned char)c);
    }
    r.h.ah = 0x08; intdos(&r, &r);
    int c = r.h.al;
    if (c == 0) { r.h.ah = 0x08; intdos(&r, &r); c = 0; }
    if (stop) return 0;
    return toupper(c);
}

/* returns 1..4: which of the choices (by first letter); 0: Ctrl-C or no input left */
static int ask(const char *msg, const char *a, const char *b, const char *c, const char *d)
{
    clear_line();
    for (;;) {
        printf("%s (%s/%s%s%s%s%s)? ", msg, a, b, c ? "/" : "", c ? c : "", d ? "/" : "", d ? d : "");
        fflush(stdout);
        int k = getkey();
        if (stop) { printf("^C\n"); return 0; }
        if (k < 0) { printf("\n"); return 0; }               /* input ran out: no answer */
        printf("%c\n", k > ' ' ? k : ' ');
        if (k == a[0]) return 1;
        if (k == b[0]) return 2;
        if (c && k == c[0]) return 3;
        if (d && k == d[0]) return 4;
    }
}

static void finish(void);

static void quit(int code)
{
    clear_line();
    finish();
    exit(code);
}

static void on_break(int sig) { stop = 1; signal(sig, on_break); }

/* ------------------------------------------------------------------ */
/* names                                                              */

static int haswild(const char *p) { return strpbrk(p, "*?") != 0; }

static void addsep(char *p)
{
    size_t l = strlen(p);
    if (l && p[l - 1] != '\\' && l < PATHMAX - 1) { p[l] = '\\'; p[l + 1] = 0; }
}

static int isdir(const char *p)
{
    size_t l = strlen(p);
    if (l == 3 && p[1] == ':' && p[2] == '\\') return 1;   /* a root */
    static char t[PATHMAX];
    strcpy(t, p);
    if (l > 3 && t[l - 1] == '\\') t[l - 1] = 0;
    int a = zgetattr(t);
    return a >= 0 && (a & _A_SUBDIR);
}

/* one part (before or after the last dot) of the target name: '?' takes
   one character of the source, '*' the rest of it, others are themselves */
static char *build_part(char *o, const char *src, const char *pat)
{
    while (*pat) {
        if (*pat == '*') { while (*src) *o++ = *src++; pat++; continue; }
        if (*pat == '?') { if (*src) *o++ = *src++; pat++; continue; }
        *o++ = *pat++;
        if (*src) src++;
    }
    return o;
}

/* the target's name: src through the pattern (XCOPY's rules, any length) */
static void build_name(char *o, const char *src, const char *pat)
{
    if (!strcmp(pat, "*.*") || !strcmp(pat, "*")) { strcpy(o, src); return; }
    static char sb[260], pb[260];
    strcpy(sb, src); strcpy(pb, pat);
    char *sd = strrchr(sb, '.'), *pd = strrchr(pb, '.');
    if (sd) *sd++ = 0;
    if (pd) *pd++ = 0;
    char *e = build_part(o, sb, pb);
    if (pd) {
        if (*pd || (sd && *pd)) {
            *e++ = '.';
            e = build_part(e, sd ? sd : "", pd);
            if (e[-1] == '.') e--;          /* "name." -> "name" */
        }
    }
    *e = 0;
}

static int excluded(const char *path)
{
    for (int i = 0; i < nexcl; i++) {
        const char *x = excl[i];
        size_t xl = strlen(x);
        for (const char *p = path; *p; p++)
            if (!strnicmp(p, x, xl)) return 1;
    }
    return 0;
}

static void read_exclude(char *list)
{
    for (char *f = strtok(list, "+"); f; f = strtok(0, "+")) {
        FILE *fp = fopen(f, "r");                        /* 8.3 or what the C library takes */
        if (!fp) { printf("Can't read exclude file - %s\n", f); exit(4); }
        static char ln[260];
        while (fgets(ln, sizeof ln, fp)) {
            ln[strcspn(ln, "\r\n")] = 0;
            if (!ln[0]) continue;
            excl = realloc(excl, (nexcl + 1) * sizeof *excl);
            if (!excl || !(excl[nexcl] = strdup(ln))) { puts("Insufficient memory"); exit(4); }
            nexcl++;
        }
        fclose(fp);
    }
}

/* make every missing folder of path (ends with \); 0 when there */
static int make_path(char *path)
{
    size_t l = strlen(path);
    for (size_t i = 3; i < l; i++) {
        if (path[i] != '\\') continue;
        path[i] = 0;
        int a = zgetattr(path);
        if (a < 0 ? zmkdir(path) : !(a & _A_SUBDIR)) { return -1; }
        path[i] = '\\';
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* copying                                                            */

static int copy_data(const char *src, const char *dst, unsigned long size, unsigned long stamp, unsigned attr)
{
    int in = zopen(src, 0);
    if (in < 0) { clear_line(); printf("Cannot open source file - %s\n", src); return 30; }
    int out = zopen(dst, 1);
    if (out < 0) { zclose(in); clear_line(); printf("Cannot create destination file - %s\n", dst); return 29; }
    unsigned long done = 0, last = ticks() - 99;
    int err = 0;
    for (;;) {
        if (stop) { err = 2; break; }
        long n = zio(in, buf, bufsize, 0);
        if (n < 0) { clear_line(); printf("Read error - %s\n", src); err = 30; break; }
        if (n == 0) break;
        long w = zio(out, buf, (unsigned)n, 1);
        if (w != n) { clear_line(); printf("Write error on destination file (disk full?) - %s\n", dst); err = 29; break; }
        done += (unsigned)n;
        if (progress && ticks() != last) { last = ticks(); draw(done, size); }
    }
    if (!err) zsettime(out, stamp);
    zclose(out); zclose(in);
    if (err) { zunlink(dst); return err; }
    zsetattr(dst, attr & (_A_RDONLY | _A_HIDDEN | _A_SYSTEM | _A_ARCH));
    if (progress) draw(size, size);
    return 0;
}

/* one file: the checks XCOPY makes, then the copy (or, scanning, the count) */
static void do_file(const char *src, const char *dst, struct zfind *sf)
{
    static struct zfind df;
    unsigned long size = sf->size, stamp = sf->stamp;
    unsigned attr = sf->attr;

    if (date_mode > 0 && stamp < date_min) return;
    if (excluded(src)) return;
    int exists = !zfindfirst(dst, _A_HIDDEN | _A_SYSTEM | _A_RDONLY | _A_ARCH | _A_SUBDIR, &df);
    unsigned long dsize = exists ? df.size : 0, dstamp = exists ? df.stamp : 0;
    unsigned dattr = exists ? df.attr : 0;
    zfindclose(&df);
    if (exists && (dattr & _A_SUBDIR)) {
        if (scanning) return;
        clear_line(); printf("Cannot overwrite a directory with a file - %s\n", dst);
        if (!sw_continue) quit(4);
        return;
    }
    if (exists && date_mode < 0 && stamp <= dstamp) return;
    if (exists && confirm_mode == 1) return;

    if (scanning) { files_total++; bytes_total += size; return; }

    if (sw_prompt) {
        int k = ask(src, "Yes", "No", 0, 0);
        if (stop) quit(2);
        if (k != 1) { bytes_total -= size; files_total--; return; }
    }
    if (!stricmp(src, dst)) {
        clear_line(); printf("File cannot be copied onto itself - %s\n", src);
        quit(4);
    }
    if (exists && confirm_mode == 2) {
        static char m[PATHMAX + 16];
        sprintf(m, "Overwrite %s", dst);
        int k = ask(m, "Yes", "No", "Overwrite all", "Skip all");
        if (stop) quit(2);
        if (k == 3) confirm_mode = 0;
        if (k == 0 || k == 2 || k == 4) {
            if (k == 4) confirm_mode = 1;
            bytes_total -= size; files_total--; return;
        }
    }

    unsigned long csize, nfree;
    zfree(dest_drive, &csize, &nfree);
    unsigned long need = (size + csize - 1) / csize, have = exists ? (dsize + csize - 1) / csize : 0;
    if (need > have && need - have > nfree) {
        clear_line(); printf("Insufficient disk space in destination path - %s\n", dst);
        quit(39);
    }

    if (exists && (dattr & (_A_RDONLY | _A_HIDDEN | _A_SYSTEM))) {
        if ((dattr & _A_RDONLY) && !sw_readonly) {
            clear_line(); printf("Write access denied - %s\n", dst);
            if (!sw_continue) quit(5);
            bytes_total -= size; files_total--; return;
        }
        if (!sw_list) zsetattr(dst, dattr & ~(_A_RDONLY | _A_HIDDEN | _A_SYSTEM));
    }

    if (!sw_quiet) {
        clear_line();
        if (sw_full) printf("%s -> %s\n", src, dst);
        else printf("%s\n", src);
    }
    if (!sw_list) {
        int e = copy_data(src, dst, size, stamp, attr);
        if (e == 2) { clear_line(); printf("^C\n"); quit(2); }
        if (e) {
            if (!sw_continue) quit(5);
            bytes_total -= size; files_total--; return;
        }
        if (sw_reset) zsetattr(src, attr & ~_A_ARCH);
    }
    bytes_done += size;
    files_done++;
    files_copied++;
}

/* src and dst end with \ and are grown and cut back as the walk goes down */
static void walk(char *src, char *dst, const char *spat, const char *dpat)
{
    static struct zfind f;              /* the file loop is done before recursing */
    size_t sl = strlen(src), dl = strlen(dst);
    unsigned fattr = _A_RDONLY | _A_ARCH | (sw_hidden ? _A_HIDDEN | _A_SYSTEM : 0);
    int any = 0;

    /* the files here */
    strcpy(src + sl, spat);
    int done = zfindfirst(src, fattr, &f);
    src[sl] = 0;
    for (; !done && !stop; done = zfindnext(&f)) {
        if (f.attr & (_A_SUBDIR | _A_VOLID)) continue;
        if (!sw_hidden && (f.attr & (_A_HIDDEN | _A_SYSTEM))) continue;
        if ((sw_archive || sw_reset) && !(f.attr & _A_ARCH)) continue;
        file_found = 1;
        if (!any) {
            any = 1;
            if (!scanning && !sw_list && make_path(dst)) {
                clear_line(); printf("Unable to create directory %s\n", dst);
                zfindclose(&f);
                if (!sw_continue) quit(4);
                return;
            }
            if (sw_tree) break;
        }
        if (sl + strlen(f.name) >= PATHMAX - 1 || dl + strlen(f.name) >= PATHMAX - 1) {
            clear_line(); printf("Path too long - %s%s\n", src, f.name);
            continue;
        }
        strcpy(src + sl, f.name);
        build_name(dst + dl, f.name, dpat);
        do_file(src, dst, &f);
        src[sl] = 0; dst[dl] = 0;
    }
    zfindclose(&f);
    if (stop) { clear_line(); printf("^C\n"); quit(2); }
    if (!any && sw_empty && !scanning && !sw_list && make_path(dst)) {
        clear_line(); printf("Unable to create directory %s\n", dst);
        if (!sw_continue) quit(4);
    }
    if (!(sw_subdir || sw_empty)) return;

    /* the folders here: their names first (the find handle is closed while recursing) */
    char **names = 0; int n = 0;
    strcpy(src + sl, "*.*");
    struct zfind *d = malloc(sizeof *d);
    if (!d) { puts("Insufficient memory"); quit(4); }
    done = zfindfirst(src, _A_SUBDIR | (sw_hidden ? _A_HIDDEN | _A_SYSTEM : 0) | _A_RDONLY | _A_ARCH, d);
    src[sl] = 0;
    for (; !done; done = zfindnext(d)) {
        if (!(d->attr & _A_SUBDIR) || !strcmp(d->name, ".") || !strcmp(d->name, "..")) continue;
        if (!sw_hidden && (d->attr & (_A_HIDDEN | _A_SYSTEM))) continue;
        char **nn = realloc(names, (n + 1) * sizeof *names);
        if (!nn || !(nn[n] = strdup(d->name))) { puts("Insufficient memory"); quit(4); }
        names = nn; n++;
    }
    zfindclose(d);
    free(d);
    for (int i = 0; i < n; i++) {
        size_t nl = strlen(names[i]);
        if (sl + nl + 2 < PATHMAX - 1 && dl + nl + 2 < PATHMAX - 1) {
            sprintf(src + sl, "%s\\", names[i]);
            sprintf(dst + dl, "%s\\", names[i]);
            walk(src, dst, spat, dpat);
            src[sl] = 0; dst[dl] = 0;
        } else { clear_line(); printf("Path too long - %s%s\n", src, names[i]); }
        free(names[i]);
    }
    free(names);
}

static void finish(void)
{
    static int once;
    if (once++) return;
    if (sw_verify) { r.h.ah = 0x2E; r.h.al = old_verify; r.h.dl = 0; intdos(&r, &r); }
    printf("%lu file(s) copied", files_copied);
    if (progress && files_copied) {
        static char sz[16], sp[16];
        unsigned long el = ticks() - t_start;
        fmt_size(sz, bytes_done);
        unsigned long secs = el * 10 / 182;
        printf(", %s in %lu:%02lu", sz, secs / 60, secs % 60);
        if (el > 9) { fmt_size(sp, bytes_done * 182 / (el * 10)); printf(" (%s/s)", sp); }
    }
    printf("\n");
}

/* ------------------------------------------------------------------ */

static void help(void)
{
    static const char *t[] = {
        "ZCOPY: XCOPY with long file names and a progress bar.",
        "",
        "ZCOPY source [destination] [/switches]",
        "  source       the folder and/or file(s) to copy (\"quote names with spaces\")",
        "  destination  where to, and/or the new name(s); default the current folder",
        "  /A           only files with the archive attribute; it is left set",
        "  /C           go on after errors",
        "  /D[:M-D-Y]   only files changed on or after the date; with no date, only",
        "               files newer than the destination's",
        "  /E           folders too, empty ones as well",
        "  /F           show source and destination names",
        "  /H           hidden and system files too",
        "  /I           destination missing and several files: it's a folder",
        "  /L           list the files without copying",
        "  /M           as /A, and clear the source's archive attribute after",
        "  /N           don't ask about existing files: skip them",
        "  /P           ask before each file",
        "  /Q           don't show the names",
        "  /R           overwrite read-only files too",
        "  /S           folders too, except empty ones",
        "  /T           the folder tree only, no files (add /E for empty folders)",
        "  /V           verify writes",
        "  /W           wait for a key first",
        "  /Y           don't ask about existing files: overwrite them",
        "  /-Y          ask before overwriting (the default)",
        "  /NP          no progress bar",
        "  /EXCLUDE:f1[+f2]  skip source paths containing a line of these files",
        "COPYCMD=/Y, /N or /-Y presets the overwrite question. Long names where",
        "DOS has the long-name API (DOSLFN); 8.3 names otherwise.",
        0
    };
    int tty = isatty(1);
    for (int i = 0; t[i]; i++) {
        if (tty && i == 22) {
            printf("-- more --"); fflush(stdout);
            r.h.ah = 0x08; intdos(&r, &r);
            printf("\r          \r");
        }
        puts(t[i]);
    }
}

/* "M-D-Y" or "M/D/Y" (or "M.D.Y"); year 80-99 -> 19xx, 0-79 -> 20xx */
static int parse_date(const char *p, unsigned long *out)
{
    unsigned m, d, y; char c1, c2;
    if (sscanf(p, "%u%c%u%c%u", &m, &c1, &d, &c2, &y) != 5) return -1;
    if (!strchr("-/.", c1) || !strchr("-/.", c2)) return -1;
    if (y < 80) y += 2000; else if (y < 100) y += 1900;
    static const unsigned char md[] = { 31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (y < 1980 || y > 2107 || m < 1 || m > 12 || d < 1 || d > md[m - 1]) return -1;
    if (m == 2 && d == 29 && (y % 4 || (y % 100 == 0 && y % 400))) return -1;
    *out = (unsigned long)(((y - 1980) << 9) | (m << 5) | d) << 16;
    return 0;
}

static void set_switch(const char *a)
{
    static char u[64];
    strncpy(u, a, sizeof u - 1);
    strupr(u);
    if (u[0] && !u[1]) switch (u[0]) {
        case 'A': sw_archive = 1; return;
        case 'C': sw_continue = 1; return;
        case 'D': date_mode = -1; return;
        case 'E': sw_empty = 1; return;
        case 'F': sw_full = 1; return;
        case 'H': sw_hidden = 1; return;
        case 'I': sw_intodir = 1; return;
        case 'L': sw_list = 1; return;
        case 'M': sw_reset = 1; return;
        case 'N': confirm_mode = 1; return;
        case 'P': sw_prompt = 1; return;
        case 'Q': sw_quiet = 1; return;
        case 'R': sw_readonly = 1; return;
        case 'S': sw_subdir = 1; return;
        case 'T': sw_tree = 1; return;
        case 'V': sw_verify = 1; return;
        case 'W': sw_wait = 1; return;
        case 'Y': confirm_mode = 0; return;
        case '?': help(); exit(0);
    }
    if (!strcmp(u, "-Y")) { confirm_mode = 2; return; }
    if (!strcmp(u, "NP")) { sw_noprog = 1; return; }
    if (!strncmp(u, "D:", 2)) {
        if (parse_date(a + 2, &date_min)) { printf("Invalid date - %s\n", a + 2); exit(4); }
        date_mode = 1; return;
    }
    if (!strncmp(u, "EXCLUDE:", 8)) { char *l = strdup(a + 8); if (l) read_exclude(l); return; }
    printf("Invalid switch - /%s\n", a);
    exit(4);
}

int main(int argc, char **argv)
{
    static char sarg[PATHMAX], src[PATHMAX], dst[PATHMAX], spat[260], dpat[260] = "*.*";
    char *files[2]; int nf = 0;

    char *cc = getenv("COPYCMD");
    if (cc) {
        if (!stricmp(cc, "/Y")) confirm_mode = 0;
        else if (!stricmp(cc, "/N")) confirm_mode = 1;
    }
    for (int i = 1; i < argc; i++) {
        char *a = argv[i];
        /* a name with switches stuck to it: "dir/s/e", and "/s/e" */
        char *sl = strchr(a, '/');
        if (sl && sl != a) {
            *sl = 0;
            if (nf == 2) { puts("Invalid number of parameters"); return 4; }
            files[nf++] = a;
            a = sl; *a = '/';
        }
        if (a[0] == '/') {
            /* /EXCLUDE:a+b keeps its slashes-free list whole; others split at '/' */
            char *p = a + 1;
            while (p && *p) {
                if (!strnicmp(p, "EXCLUDE:", 8) || !strnicmp(p, "D:", 2)) { set_switch(p); break; }
                char *q = strchr(p, '/');
                if (q) *q = 0;
                if (*p) set_switch(p);
                p = q ? q + 1 : 0;
            }
            continue;
        }
        if (nf == 2) { puts("Invalid number of parameters"); return 4; }
        files[nf++] = a;
    }
    if (nf == 0) { help(); return 4; }
    if (sw_reset) sw_archive = 1;

    /* the source: a folder, or a folder and a name or pattern */
    strncpy(sarg, files[0], PATHMAX - 1);
    size_t l = strlen(sarg);
    if (l > 1 && sarg[l - 1] == '\\' && sarg[l - 2] != ':') sarg[--l] = 0;
    if (l == 2 && sarg[1] == ':') strcat(sarg, ".");
    ztruename(sarg, src);
    if (!src[0]) { printf("Invalid source drive specification - %s\n", files[0]); return 4; }
    if (!haswild(src) && isdir(src)) strcpy(spat, "*.*");
    else {
        char *b = strrchr(src, '\\');
        strcpy(spat, b + 1);
        b[1] = 0;
        if (!isdir(src)) { printf("Source path not found - %s\n", src); return 4; }
    }
    addsep(src);

    /* the destination: a folder, or a folder and a name or pattern */
    if (nf < 2) ztruename(".", dst);
    else {
        static char darg[PATHMAX];
        strncpy(darg, files[1], PATHMAX - 1);
        l = strlen(darg);
        int trailing = l > 0 && darg[l - 1] == '\\';
        if (trailing && l > 1 && darg[l - 2] != ':') darg[--l] = 0;
        if (l == 2 && darg[1] == ':') strcat(darg, ".");
        ztruename(darg, dst);
        if (!dst[0]) { printf("Invalid destination drive specification - %s\n", files[1]); return 4; }
        if (!trailing && !isdir(dst)) {
            char *b = strrchr(dst, '\\');
            static char last[260];
            strcpy(last, b + 1);
            if (haswild(last)) { strcpy(dpat, last); b[1] = 0; }
            else {
                int many = haswild(spat) || sw_subdir || sw_empty;
                int folder = many && sw_intodir;
                if (!folder) {
                    printf("Does %s specify a file name\n", last);
                    int k = ask("or directory name on the target", "File", "Directory", 0, 0);
                    if (stop) return 2;
                    if (!k) return 4;
                    folder = k == 2;
                }
                if (!folder) { strcpy(dpat, last); b[1] = 0; }
            }
        }
    }
    addsep(dst);
    if (strlen(src) >= PATHMAX - 14 || strlen(dst) >= PATHMAX - 14) { puts("Path too long"); return 4; }

    if ((sw_subdir || sw_empty) && !strnicmp(src, dst, strlen(src))) {
        puts("Cannot perform a cyclic copy");
        return 4;
    }
    dest_drive = (char)(toupper(dst[0]) - 'A' + 1);

    for (bufsize = 0xF000; bufsize >= 0x1000; bufsize >>= 1)
        if ((buf = _fmalloc(bufsize)) != 0) break;
    if (!buf) { puts("Insufficient memory"); return 4; }

    signal(SIGINT, on_break);
    signal(SIGBREAK, on_break);

    if (sw_wait) {
        printf("Press any key to begin copying file(s)"); fflush(stdout);
        getkey(); printf("\n");
        if (stop) return 2;
    }
    if (sw_verify) {
        r.h.ah = 0x54; intdos(&r, &r); old_verify = r.h.al;
        r.h.ah = 0x2E; r.h.al = 1; r.h.dl = 0; intdos(&r, &r);
    }
    {
        r.h.ah = 0x0F; int86(0x10, &r, &r);           /* the screen's width */
        if (r.h.ah >= 40) cols = r.h.ah;
    }

    static char s2[PATHMAX], d2[PATHMAX];
    if (!sw_noprog && !sw_list && !sw_tree && isatty(1)) {
        /* first the size of the whole copy */
        scanning = 1;
        strcpy(s2, src); strcpy(d2, dst);
        walk(s2, d2, spat, dpat);
        scanning = 0;
        progress = files_total > 0;
        file_found = 0;
    }
    t_start = ticks();
    strcpy(s2, src); strcpy(d2, dst);
    walk(s2, d2, spat, dpat);
    clear_line();
    if (!file_found) {
        printf("File not found - %s\n", spat);
        finish();
        return 1;
    }
    finish();
    return 0;
}
