/*
   TREE: the folders of a drive or path, as a tree.

     TREE [drive:][path] [/F] [/A]
       /F  the files in each folder too
       /A  plain ASCII lines (+---, \---, |) instead of the line-drawing ones

   Long names where DOS has the long-name API (INT 21h 714Eh: DOSLFN, a
   VMDOSNET drive), 8.3 names otherwise. Part of vmdos (GPL-2.0-or-later);
   built with Open Watcom: wcl -q -bt=dos -ms -0 -os -s -za99 tree.c
*/
#include <dos.h>
#include <i86.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int show_files, ascii, lfn = -1;
static unsigned long ndirs, nfiles;

/* one folder's entries: names, and which are folders */
struct ent { char *name; int dir; };

struct w32find {                  /* INT 21h 714Eh's record */
    unsigned long attr;
    unsigned long ctime[2], atime[2], mtime[2];
    unsigned long sizehi, sizelo;
    char res[8];
    char lname[260];
    char sname[14];
};

/* the entries of folder path (no trailing \), folders and (with /F) files */
static struct ent *list(const char *path, int *count)
{
    static char spec[300];
    struct ent *v = 0;
    int n = 0, cap = 0;
    sprintf(spec, "%s\\*.*", path);
    union REGS r; struct SREGS s;
    segread(&s);
    if (lfn != 0) {
        static struct w32find f;
        r.x.ax = 0x714E; r.x.cx = 0x0016; r.x.si = 1;          /* hidden, system, folders; DOS times */
        r.x.dx = FP_OFF(spec); s.ds = FP_SEG(spec);
        r.x.di = FP_OFF(&f); s.es = FP_SEG(&f);
        intdosx(&r, &r, &s);
        if (!r.x.cflag) {
            lfn = 1;
            unsigned h = r.x.ax;
            for (;;) {
                if (strcmp(f.lname, ".") && strcmp(f.lname, "..")) {
                    int d = (f.attr & 0x10) != 0;
                    if (d || show_files) {
                        if (n == cap) { cap = cap ? cap * 2 : 16; v = realloc(v, cap * sizeof *v); if (!v) break; }
                        v[n].name = strdup(f.lname[0] ? f.lname : f.sname);
                        v[n].dir = d; n++;
                    }
                }
                r.x.ax = 0x714F; r.x.bx = h; r.x.si = 1;
                r.x.di = FP_OFF(&f); s.es = FP_SEG(&f);
                intdosx(&r, &r, &s);
                if (r.x.cflag) break;
            }
            r.x.ax = 0x71A1; r.x.bx = h; intdos(&r, &r);
            *count = n;
            return v;
        }
        if (r.x.ax == 0x7100) lfn = 0;
        else if (lfn == 1) { *count = 0; return 0; }          /* the API is there: not found */
    }
    struct find_t f;
    unsigned rc = _dos_findfirst(spec, _A_SUBDIR | _A_HIDDEN | _A_SYSTEM | _A_RDONLY | _A_ARCH, &f);
    while (!rc) {
        if (strcmp(f.name, ".") && strcmp(f.name, "..")) {
            int d = (f.attrib & _A_SUBDIR) != 0;
            if (d || show_files) {
                if (n == cap) { cap = cap ? cap * 2 : 16; v = realloc(v, cap * sizeof *v); if (!v) break; }
                v[n].name = strdup(f.name);
                v[n].dir = d; n++;
            }
        }
        rc = _dos_findnext(&f);
    }
    *count = n;
    return v;
}

static void walk(char *path, char *prefix)
{
    int n;
    struct ent *v = list(path, &n);
    int lastdir = -1;
    for (int i = 0; i < n; i++) if (v[i].dir) lastdir = i;
    size_t pl = strlen(path), xl = strlen(prefix);
    /* files first (with /F), under a line that goes on if folders follow */
    int anyfile = 0;
    for (int i = 0; i < n; i++) {
        if (v[i].dir) continue;
        printf("%s%s   %s\n", prefix, lastdir >= 0 ? (ascii ? "|" : "\xB3") : " ", v[i].name);
        nfiles++; anyfile = 1;
    }
    if (anyfile) printf("%s%s\n", prefix, lastdir >= 0 ? (ascii ? "|" : "\xB3") : "");
    for (int i = 0; i < n; i++) {
        if (!v[i].dir) continue;
        int last = i == lastdir;
        printf("%s%s%s\n", prefix,
               ascii ? (last ? "\\---" : "+---") : (last ? "\xC0\xC4\xC4\xC4" : "\xC3\xC4\xC4\xC4"), v[i].name);
        ndirs++;
        if (pl + 1 + strlen(v[i].name) < 255 && xl + 4 < 250) {
            sprintf(path + pl, "\\%s", v[i].name);
            strcpy(prefix + xl, last ? "    " : (ascii ? "|   " : "\xB3   "));
            walk(path, prefix);
            path[pl] = 0; prefix[xl] = 0;
        }
    }
    for (int i = 0; i < n; i++) free(v[i].name);
    free(v);
}

int main(int argc, char **argv)
{
    static char path[300], prefix[260];
    char *arg = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '/') {
            char c = argv[i][1] | 0x20;
            if (c == 'f') show_files = 1;
            else if (c == 'a') ascii = 1;
            else {
                puts("TREE [drive:][path] [/F] [/A]: the folders as a tree\n"
                     "  /F  the files in each folder too\n"
                     "  /A  ASCII lines instead of line-drawing characters");
                return argv[i][1] == '?' ? 0 : 1;
            }
        } else arg = argv[i];
    }
    /* the full path: TRUENAME (long form when the API is there) */
    union REGS r; struct SREGS s;
    static char in[300];
    strcpy(in, arg ? arg : ".");
    segread(&s);
    r.x.ax = 0x7160; r.x.cx = 0x0002;                          /* the long name of the path */
    r.x.si = FP_OFF(in); s.ds = FP_SEG(in);
    r.x.di = FP_OFF(path); s.es = FP_SEG(path);
    intdosx(&r, &r, &s);
    if (r.x.cflag) {
        r.h.ah = 0x60;
        r.x.si = FP_OFF(in); r.x.di = FP_OFF(path);
        intdosx(&r, &r, &s);
        if (r.x.cflag) { printf("Invalid path - %s\n", in); return 1; }
    }
    size_t l = strlen(path);
    if (l > 3 && path[l - 1] == '\\') path[--l] = 0;
    /* the volume, as DOS's TREE says it */
    struct find_t f;
    char vspec[8] = "?:\\*.*";
    vspec[0] = path[0];
    if (!_dos_findfirst(vspec, _A_VOLID, &f)) {
        char *dot = strchr(f.name, '.');
        if (dot) memmove(dot, dot + 1, strlen(dot));
        printf("Folder PATH listing for volume %s\n", f.name);
    } else printf("Folder PATH listing\n");
    {
        static struct { unsigned info; unsigned long serial; char label[11], fs[8]; } mi;
        r.x.ax = 0x6900; r.h.bl = (unsigned char)((path[0] | 0x20) - 'a' + 1);
        r.x.dx = FP_OFF(&mi); s.ds = FP_SEG(&mi);
        intdosx(&r, &r, &s);
        if (!r.x.cflag && mi.serial)
            printf("Volume serial number is %04X-%04X\n", (unsigned)(mi.serial >> 16), (unsigned)mi.serial);
    }
    printf("%s\n", path);
    if (l == 3) path[2] = 0;                                   /* "C:" + "\name" below */
    walk(path, prefix);
    if (!ndirs) printf("No subfolders exist\n");
    return 0;
}
