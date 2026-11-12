/*
   EDIT: vmdos's text editor for DOS.

     EDIT [file]

   A full-screen editor in the spirit of MS-DOS EDIT, small and plain: the
   text in memory as lines, drawn straight into text-mode video memory, keys
   from the BIOS (INT 16h), files through DOS. File names may be long ones
   (INT 21h AX=716Ch, with DOSLFN or on a VMDOSNET drive); without the
   long-name API it falls back to DOS's 8.3 calls.

   Keys: arrows, Home/End, PgUp/PgDn, Ctrl+Home/End (top/bottom), Ctrl+Left/
   Right (word), Ins (insert/overwrite), Del, Backspace, Enter, Tab.
   Ctrl+Y cut the line (several in a row are kept together), Ctrl+K copy the
   line, Ctrl+U paste. F2/Ctrl+S save, Shift+F2 save as, Ctrl+O open,
   Ctrl+F find, F3 find again, Ctrl+R replace, Ctrl+G go to line, F1 help,
   Esc/F10/Alt+X quit.

   Built with Open Watcom, large model, 8086 code (see Makefile in vmdos:
   make, or wcl -q -bt=dos -ml -0 -os -s edit.c). GPL-2.0-or-later, as vmdos.
*/
#include <dos.h>
#include <i86.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#define MAXLINES  16000u          /* line table: 16000 far pointers in one 64 KB block */
#define MAXLEN    32000u          /* longest line */
#define TABW      8

typedef unsigned char uchar;
typedef unsigned short ushort;

/* ---------------------------------------------------------------- text */
static char **text;               /* the lines (no line ends) */
static ushort *len, *cap;
static unsigned nlines;
static int modified, ends_nl = 1, insmode = 1, readonly;
static char fname[262];           /* as typed (may be a long name) */

static char **clip;               /* Ctrl+Y / Ctrl+K / Ctrl+U */
static ushort *cliplen;
static unsigned nclip;

static char findstr[80], replstr[80];

/* cursor: line and position in it; window: first line, first column on screen */
static unsigned cy, cx, top, left, wantcol;

/* ---------------------------------------------------------------- screen */
static ushort far *vid;
static int rows, cols, trows;     /* trows: text rows (rows minus title and status) */
static int mono;
static uchar a_text = 0x1F, a_bar = 0x70, a_hi = 0x3F, a_msg = 0x4F;
static char msg[160];             /* one-shot message for the status line */

static void bios_video(union REGS *r) { int86(0x10, r, r); }

static void screen_init(void)
{
    union REGS r;
    uchar far *bda = MK_FP(0x40, 0);
    r.h.ah = 0x0F; bios_video(&r);
    int mode = r.h.al & 0x7F;
    if (mode != 2 && mode != 3 && mode != 7) {          /* not a text mode: the usual one */
        r.x.ax = 0x0003; bios_video(&r);
        mode = 3;
    }
    cols = bda[0x4A] ? bda[0x4A] : 80;
    rows = bda[0x84] ? bda[0x84] + 1 : 25;
    if (rows < 5) rows = 25;
    trows = rows - 2;
    vid = MK_FP(mode == 7 ? 0xB000 : 0xB800, *(ushort far *)(bda + 0x4E));
    mono = mode == 7;
    if (mode == 7 || mode == 2) { a_text = 0x07; a_bar = 0x70; a_hi = 0x70; a_msg = 0x70; }
}

static void put(int y, int x, uchar c, uchar a)
{
    if (y >= 0 && y < rows && x >= 0 && x < cols) vid[y * cols + x] = (ushort)a << 8 | c;
}

static void puts_at(int y, int x, const char *s, uchar a, int width)
{
    int i = 0;
    for (; s[i] && i < width; i++) put(y, x + i, (uchar)s[i], a);
    for (; i < width; i++) put(y, x + i, ' ', a);
}

static void gotoxy(int y, int x)
{
    union REGS r;
    r.h.ah = 2; r.h.bh = *(uchar far *)MK_FP(0x40, 0x62); r.h.dh = (uchar)y; r.h.dl = (uchar)x;
    bios_video(&r);
}

static void cursor_shape(int big)
{
    union REGS r;
    r.h.ah = 1; r.h.ch = big ? 0 : 6; r.h.cl = 7;
    if (mono) { r.h.ch = big ? 0 : 11; r.h.cl = 12; }
    bios_video(&r);
}

/* ---------------------------------------------------------------- keys */
#define K_UP     0x4800
#define K_DOWN   0x5000
#define K_LEFT   0x4B00
#define K_RIGHT  0x4D00
#define K_HOME   0x4700
#define K_END    0x4F00
#define K_PGUP   0x4900
#define K_PGDN   0x5100
#define K_INS    0x5200
#define K_DEL    0x5300
#define K_CHOME  0x7700
#define K_CEND   0x7500
#define K_CLEFT  0x7300
#define K_CRIGHT 0x7400
#define K_F1     0x3B00
#define K_F2     0x3C00
#define K_F3     0x3D00
#define K_SF2    0x5500
#define K_F10    0x4400
#define K_ALTX   0x2D00

static int ext16 = -1;            /* the BIOS has the extended keyboard functions */

static unsigned getkey(void)
{
    union REGS r;
    if (ext16 < 0) {               /* AH=12h answers with the shift flags only on an enhanced BIOS */
        r.x.ax = 0x12FF; int86(0x16, &r, &r);
        ext16 = r.h.al != 0xFF || (*(uchar far *)MK_FP(0x40, 0x96) & 0x10);
    }
    r.h.ah = ext16 ? 0x10 : 0x00;
    int86(0x16, &r, &r);
    unsigned k = r.x.ax;
    if ((k & 0xFF) == 0xE0 && (k >> 8)) return k & 0xFF00;   /* gray keys: as the white ones */
    if (k & 0xFF) return k & 0xFF;                            /* a character (Ctrl+letters: 01h..1Ah) */
    return k & 0xFF00;
}

/* ---------------------------------------------------------------- memory */
static void *xalloc(unsigned n)
{
    void *p = malloc(n ? n : 1);
    return p;
}

static int ensure(unsigned y, unsigned need)       /* room for need chars in line y */
{
    if (need <= cap[y]) return 1;
    if (need > MAXLEN) { strcpy(msg, "Line too long"); return 0; }
    unsigned nc = need < 16 ? 16 : need + need / 2;
    if (nc > MAXLEN) nc = MAXLEN;
    char *p = realloc(text[y], nc);
    if (!p) { strcpy(msg, "Out of memory"); return 0; }
    text[y] = p; cap[y] = (ushort)nc;
    return 1;
}

static int insert_lines(unsigned at, unsigned n)    /* n empty lines before line at */
{
    if (nlines + n > MAXLINES) { strcpy(msg, "Too many lines (16000 at most)"); return 0; }
    memmove(&text[at + n], &text[at], (nlines - at) * sizeof text[0]);
    memmove(&len[at + n], &len[at], (nlines - at) * sizeof len[0]);
    memmove(&cap[at + n], &cap[at], (nlines - at) * sizeof cap[0]);
    for (unsigned i = 0; i < n; i++) { text[at + i] = 0; len[at + i] = cap[at + i] = 0; }
    nlines += n;
    return 1;
}

static void delete_line(unsigned at)
{
    free(text[at]);
    memmove(&text[at], &text[at + 1], (nlines - at - 1) * sizeof text[0]);
    memmove(&len[at], &len[at + 1], (nlines - at - 1) * sizeof len[0]);
    memmove(&cap[at], &cap[at + 1], (nlines - at - 1) * sizeof cap[0]);
    nlines--;
    if (!nlines) { insert_lines(0, 1); }
}

static void clear_text(void)
{
    for (unsigned i = 0; i < nlines; i++) free(text[i]);
    nlines = 0;
    insert_lines(0, 1);
    cy = cx = top = left = wantcol = 0;
    modified = 0; ends_nl = 1;
}

/* ---------------------------------------------------------------- files */
static int lfn_ok = -1;

/* DOS open/create: mode 0 read, 1 create/truncate for writing. Long names
   through AX=716Ch; when that isn't there, the 8.3 calls. Handle or -1. */
static int dos_open(const char *name, int create, unsigned *err)
{
    union REGS r; struct SREGS s;
    segread(&s);
    if (lfn_ok != 0) {
        r.x.ax = 0x716C;
        r.x.bx = create ? 0x0002 : 0x0000;          /* read/write, read-only */
        r.x.cx = 0;                                  /* attributes */
        r.x.dx = create ? 0x0012 : 0x0001;           /* create/truncate, open existing */
        r.x.si = FP_OFF(name); s.ds = FP_SEG(name);
        r.x.di = 1;
        intdosx(&r, &r, &s);
        if (!r.x.cflag) { lfn_ok = 1; return r.x.ax; }
        if (r.x.ax != 0x7100 && lfn_ok == 1) { *err = r.x.ax; return -1; }
        if (r.x.ax == 0x7100) lfn_ok = 0;
    }
    r.h.ah = create ? 0x3C : 0x3D;
    r.h.al = create ? 0 : 0;                         /* 3Dh: read; 3Ch: CX attributes */
    r.x.cx = 0;
    r.x.dx = FP_OFF(name); s.ds = FP_SEG(name);
    intdosx(&r, &r, &s);
    if (r.x.cflag) { *err = r.x.ax; return -1; }
    return r.x.ax;
}

static int dos_rw(int h, void *buf, unsigned n, int write)
{
    union REGS r; struct SREGS s;
    segread(&s);
    r.h.ah = write ? 0x40 : 0x3F; r.x.bx = h; r.x.cx = n;
    r.x.dx = FP_OFF(buf); s.ds = FP_SEG(buf);
    intdosx(&r, &r, &s);
    return r.x.cflag ? -1 : (int)r.x.ax;
}

static void dos_close(int h) { union REGS r; r.h.ah = 0x3E; r.x.bx = h; intdos(&r, &r); }

static const char *errtext(unsigned e)
{
    switch (e) {
    case 2: return "File not found";
    case 3: return "Path not found";
    case 4: return "Too many open files";
    case 5: return "Access denied (read-only?)";
    case 0x0F: return "Invalid drive";
    case 0x1D: case 0x27: case 0x70: return "Disk full";
    default: return "DOS error";
    }
}

static int append_char(unsigned y, char c)
{
    if (len[y] >= MAXLEN) return 1;                  /* overlong: the rest is cut */
    if (!ensure(y, len[y] + 1)) return 0;
    text[y][len[y]++] = c;
    return 1;
}

/* 1 loaded, 0 not there (new file), -1 error (msg says) */
static int load(const char *name)
{
    unsigned err = 0;
    int h = dos_open(name, 0, &err);
    clear_text();
    if (h < 0) {
        if (err == 2) return 0;
        sprintf(msg, "%s: %s", name, errtext(err));
        return -1;
    }
    static char buf[4096];
    unsigned y = 0;
    int n, eof = 0, last_nl = 1;
    while (!eof && (n = dos_rw(h, buf, sizeof buf, 0)) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == 0x1A) { eof = 1; break; }        /* ^Z: DOS end of text */
            if (c == '\r') continue;
            if (c == '\n') {
                if (!insert_lines(y + 1, 1)) { eof = 1; break; }
                y++; last_nl = 1;
                continue;
            }
            if (!append_char(y, c)) { eof = 1; break; }
            last_nl = 0;
        }
    }
    dos_close(h);
    /* a file that ends with a line end: the empty line after it isn't one */
    if (nlines > 1 && last_nl && len[nlines - 1] == 0) { free(text[nlines - 1]); nlines--; ends_nl = 1; }
    else ends_nl = last_nl;
    if (nlines == 0) insert_lines(0, 1);
    return 1;
}

static int save(const char *name)
{
    unsigned err = 0;
    int h = dos_open(name, 1, &err);
    if (h < 0) { sprintf(msg, "Can't save %s: %s", name, errtext(err)); return 0; }
    static char buf[4096];
    unsigned b = 0;
    int ok = 1;
    for (unsigned y = 0; y < nlines && ok; y++) {
        unsigned i = 0;
        for (;;) {
            unsigned k = len[y] - i;
            if (k > sizeof buf - b) k = sizeof buf - b;
            memcpy(buf + b, text[y] + i, k); b += k; i += k;
            if (b == sizeof buf) { if (dos_rw(h, buf, b, 1) != (int)b) ok = 0; b = 0; }
            if (i >= len[y] || !ok) break;
        }
        if (y + 1 < nlines || ends_nl) {
            if (b + 2 > sizeof buf) { if (dos_rw(h, buf, b, 1) != (int)b) ok = 0; b = 0; }
            buf[b++] = '\r'; buf[b++] = '\n';
        }
    }
    if (ok && b && dos_rw(h, buf, b, 1) != (int)b) ok = 0;
    dos_close(h);
    if (!ok) { sprintf(msg, "Can't save %s: disk full?", name); return 0; }
    modified = 0;
    sprintf(msg, "Saved %s (%u lines)", name, nlines);
    return 1;
}

/* ---------------------------------------------------------------- display */
static unsigned vcol(unsigned y, unsigned x)     /* screen column of position x in line y */
{
    unsigned v = 0;
    for (unsigned i = 0; i < x && i < len[y]; i++) v = text[y][i] == '\t' ? (v / TABW + 1) * TABW : v + 1;
    return v + (x > len[y] ? x - len[y] : 0);
}

static unsigned pos_of_vcol(unsigned y, unsigned want)   /* position whose column is nearest want */
{
    unsigned v = 0, i;
    for (i = 0; i < len[y]; i++) {
        unsigned nv = text[y][i] == '\t' ? (v / TABW + 1) * TABW : v + 1;
        if (nv > want) break;
        v = nv;
    }
    return i;
}

static void draw(void)
{
    char t[200];
    /* title */
    sprintf(t, " EDIT  %s%s%s", fname[0] ? fname : "(untitled)", modified ? "  *" : "", readonly ? "  [read-only]" : "");
    puts_at(0, 0, t, a_bar, cols);
    /* text */
    for (int r = 0; r < trows; r++) {
        unsigned y = top + r;
        int sx = 0;
        if (y < nlines) {
            unsigned v = 0;
            const char *p = text[y];
            for (unsigned i = 0; i < len[y] && sx < cols; i++) {
                if (p[i] == '\t') {
                    unsigned nv = (v / TABW + 1) * TABW;
                    for (; v < nv; v++) if (v >= left && sx < cols) put(r + 1, sx++, ' ', a_text);
                } else {
                    if (v >= left) put(r + 1, sx++, (uchar)p[i], a_text);
                    v++;
                }
            }
        }
        for (; sx < cols; sx++) put(r + 1, sx, ' ', a_text);
    }
    /* status */
    char right[60];
    sprintf(right, "Ln %u/%u Col %u %s ", cy + 1, nlines, vcol(cy, cx) + 1, insmode ? "INS" : "OVR");
    if (msg[0]) puts_at(rows - 1, 0, msg, a_msg, cols - (int)strlen(right));
    else puts_at(rows - 1, 0, " F1 Help  F2 Save  Ctrl+F Find  Esc Quit",
                 a_bar, cols - (int)strlen(right));
    puts_at(rows - 1, cols - (int)strlen(right), right, a_bar, (int)strlen(right));
    msg[0] = 0;
    gotoxy(1 + (int)(cy - top), (int)(vcol(cy, cx) - left));
}

static void scroll_to_cursor(void)
{
    if (cy >= nlines) cy = nlines - 1;
    if (cx > len[cy]) cx = len[cy];
    if (cy < top) top = cy;
    if (cy >= top + trows) top = cy - trows + 1;
    unsigned v = vcol(cy, cx);
    if (v < left) left = v;
    if (v >= left + cols) left = v - cols + 1;
}

/* one line of input on the status line; 1 entered, 0 Esc */
static int prompt(const char *q, char *buf, int max)
{
    int n = (int)strlen(buf);
    for (;;) {
        char t[200];
        int ql = (int)strlen(q);
        sprintf(t, "%s%s", q, buf);
        puts_at(rows - 1, 0, t, a_hi, cols);
        gotoxy(rows - 1, ql + n < cols ? ql + n : cols - 1);
        unsigned k = getkey();
        if (k == 27) return 0;
        if (k == 13) return 1;
        if (k == 8) { if (n) buf[--n] = 0; continue; }
        if (k == 0x19 || k == 0x15) { n = 0; buf[0] = 0; continue; }   /* Ctrl+Y / Ctrl+U: clear */
        if (k >= 32 && k < 256 && n < max - 1) { buf[n++] = (char)k; buf[n] = 0; }
    }
}

static int ask(const char *q, const char *keys)     /* one key of keys (lower case), 0 for Esc */
{
    puts_at(rows - 1, 0, q, a_hi, cols);
    gotoxy(rows - 1, (int)strlen(q) < cols ? (int)strlen(q) : cols - 1);
    for (;;) {
        unsigned k = getkey();
        if (k == 27) return 0;
        if (k < 256) {
            char c = (char)k;
            if (c >= 'A' && c <= 'Z') c += 32;
            if (strchr(keys, c)) return c;
        }
    }
}

static const char *help[] = {
    "EDIT - vmdos's text editor                            EDIT [/R] [file]",
    "",
    "  Arrows, Home, End, PgUp, PgDn  move     Ctrl+Home, Ctrl+End  top, end",
    "  Ctrl+Left, Ctrl+Right          a word   Ctrl+G               go to line",
    "  Ins     insert / overwrite              Enter                split line",
    "  Del, Backspace  delete",
    "",
    "  Ctrl+Y  cut the line (cut several in a row: they stay together)",
    "  Ctrl+K  copy the line                   Ctrl+U  paste them",
    "",
    "  F2, Ctrl+S  save                        Shift+F2  save as",
    "  Ctrl+O      open another file",
    "  Ctrl+F      find                        F3        find the next one",
    "  Ctrl+R      replace (asks at each one: Yes, No, All)",
    "",
    "  Esc, F10, Alt+X  quit (asks to save changes)",
    "",
    "  /R opens the file read-only. Long file names work where DOS has",
    "  the long-name API (DOSLFN, VMDOSNET drives).",
    "",
    "Press a key.",
    0
};

static void show_help(void)
{
    for (int r = 1; r < rows - 1; r++) puts_at(r, 0, "", a_text, cols);
    for (int i = 0; help[i] && i < trows; i++) puts_at(i + 1, 1, help[i], a_text, cols - 1);
    puts_at(rows - 1, 0, " Help", a_bar, cols);
    gotoxy(rows - 1, 5);
    getkey();
}

/* ---------------------------------------------------------------- editing */
static void set_modified(void) { modified = 1; }

static void ins_char(char c)
{
    if (readonly) { strcpy(msg, "Read-only"); return; }
    if (!insmode && cx < len[cy]) { text[cy][cx++] = c; set_modified(); return; }
    if (!ensure(cy, len[cy] + 1)) return;
    memmove(text[cy] + cx + 1, text[cy] + cx, len[cy] - cx);
    text[cy][cx++] = c; len[cy]++;
    set_modified();
}

static void split_line(void)
{
    if (readonly) { strcpy(msg, "Read-only"); return; }
    if (!insert_lines(cy + 1, 1)) return;
    unsigned rest = len[cy] - cx;
    if (rest) {
        if (!ensure(cy + 1, rest)) return;
        memcpy(text[cy + 1], text[cy] + cx, rest);
        len[cy + 1] = (ushort)rest; len[cy] = (ushort)cx;
    }
    cy++; cx = 0;
    set_modified();
}

static void join_next(void)        /* line cy and the next one: one line */
{
    if (cy + 1 >= nlines) return;
    if (!ensure(cy, len[cy] + len[cy + 1])) return;
    memcpy(text[cy] + len[cy], text[cy + 1], len[cy + 1]);
    len[cy] += len[cy + 1];
    delete_line(cy + 1);
    set_modified();
}

static void del_char(void)
{
    if (readonly) { strcpy(msg, "Read-only"); return; }
    if (cx < len[cy]) {
        memmove(text[cy] + cx, text[cy] + cx + 1, len[cy] - cx - 1);
        len[cy]--;
        set_modified();
    } else join_next();
}

static void backspace(void)
{
    if (readonly) { strcpy(msg, "Read-only"); return; }
    if (cx > 0) { cx--; del_char(); }
    else if (cy > 0) { cy--; cx = len[cy]; join_next(); }
}

static void clip_clear(void)
{
    for (unsigned i = 0; i < nclip; i++) free(clip[i]);
    nclip = 0;
}

static int clip_add(unsigned y)
{
    if (nclip >= MAXLINES) { strcpy(msg, "Clipboard full"); return 0; }
    char *p = xalloc(len[y]);
    if (!p) { strcpy(msg, "Out of memory"); return 0; }
    memcpy(p, text[y], len[y]);
    clip[nclip] = p; cliplen[nclip] = len[y]; nclip++;
    return 1;
}

static void cut_line(int wasCut)
{
    if (readonly) { strcpy(msg, "Read-only"); return; }
    if (!wasCut) clip_clear();
    if (!clip_add(cy)) return;
    if (nlines == 1) { len[0] = 0; ends_nl = 1; }
    else delete_line(cy);
    cx = 0;
    set_modified();
    sprintf(msg, "%u line%s cut", nclip, nclip == 1 ? "" : "s");
}

static void copy_line(int wasCopy)
{
    if (!wasCopy) clip_clear();
    if (clip_add(cy)) {
        sprintf(msg, "%u line%s copied", nclip, nclip == 1 ? "" : "s");
        if (cy + 1 < nlines) { cy++; cx = 0; }
    }
}

static void paste(void)
{
    if (readonly) { strcpy(msg, "Read-only"); return; }
    if (!nclip) { strcpy(msg, "Nothing to paste (Ctrl+Y cuts a line, Ctrl+K copies one)"); return; }
    if (!insert_lines(cy, nclip)) return;
    for (unsigned i = 0; i < nclip; i++) {
        if (!ensure(cy + i, cliplen[i])) break;
        memcpy(text[cy + i], clip[i], cliplen[i]); len[cy + i] = cliplen[i];
    }
    cy += nclip; cx = 0;
    set_modified();
}

static int match_at(unsigned y, unsigned x, const char *s, unsigned n)   /* case-insensitive */
{
    if (x + n > len[y]) return 0;
    for (unsigned i = 0; i < n; i++) {
        char a = text[y][x + i], b = s[i];
        if (a >= 'a' && a <= 'z') a -= 32;
        if (b >= 'a' && b <= 'z') b -= 32;
        if (a != b) return 0;
    }
    return 1;
}

static int find_next(int from_next)       /* from the cursor (or one past it): moves there */
{
    unsigned n = (unsigned)strlen(findstr);
    if (!n) return 0;
    unsigned y = cy, x = cx + (from_next ? 1 : 0);
    for (unsigned k = 0; k <= nlines; k++) {
        for (; x + n <= len[y]; x++)
            if (match_at(y, x, findstr, n)) { cy = y; cx = x; return 1; }
        y = y + 1 < nlines ? y + 1 : 0;
        x = 0;
    }
    return 0;
}

static void replace_all_prompt(void)
{
    if (readonly) { strcpy(msg, "Read-only"); return; }
    if (!prompt("Replace: ", findstr, sizeof findstr) || !findstr[0]) return;
    if (!prompt("With: ", replstr, sizeof replstr)) return;
    unsigned n = (unsigned)strlen(findstr), m = (unsigned)strlen(replstr), count = 0;
    unsigned sy = cy, sx = cx;
    int all = 0;
    unsigned y = cy, x = cx;
    for (unsigned lines = 0; lines <= nlines; ) {
        if (x + n <= len[y] && match_at(y, x, findstr, n)) {
            cy = y; cx = x;
            int a = 'y';
            if (!all) {
                scroll_to_cursor(); draw();
                a = ask("Replace this one? (Yes, No, All, Esc) ", "yna");
                if (!a) break;
                if (a == 'a') all = 1;
            }
            if (a != 'n') {
                if (!ensure(y, len[y] - n + m)) break;
                memmove(text[y] + x + m, text[y] + x + n, len[y] - x - n);
                memcpy(text[y] + x, replstr, m);
                len[y] = (ushort)(len[y] - n + m);
                x += m; count++;
                set_modified();
            } else x++;
            continue;
        }
        if (x < len[y]) { x++; continue; }
        y++; x = 0; lines++;
        if (y >= nlines) break;
    }
    if (!count) { cy = sy; cx = sx; }
    sprintf(msg, "%u replaced", count);
}

static int confirm_discard(void)     /* 1: go on (saved or not wanted), 0: stay */
{
    if (!modified) return 1;
    int a = ask("Save the changes first? (Yes, No, Esc) ", "yn");
    if (!a) return 0;
    if (a == 'n') return 1;
    if (!fname[0]) {
        char b[262] = "";
        if (!prompt("Save as: ", b, sizeof b) || !b[0]) return 0;
        strcpy(fname, b);
    }
    return save(fname);
}

static void open_file(const char *name)
{
    strncpy(fname, name, sizeof fname - 1);
    int r = load(fname);
    if (r == 0) sprintf(msg, "New file: %s", fname);
    else if (r < 0) { fname[0] = 0; }
}

/* ---------------------------------------------------------------- main */
int main(int argc, char **argv)
{
    signal(SIGINT, SIG_IGN);
    text = malloc(MAXLINES * sizeof text[0]);
    len = malloc(MAXLINES * sizeof len[0]);
    cap = malloc(MAXLINES * sizeof cap[0]);
    clip = malloc(MAXLINES * sizeof clip[0]);
    cliplen = malloc(MAXLINES * sizeof cliplen[0]);
    if (!text || !len || !cap || !clip || !cliplen) { puts("EDIT: not enough memory"); return 1; }
    nlines = 0;
    insert_lines(0, 1);

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '/' && (argv[i][1] == '?' || argv[i][1] == 'h' || argv[i][1] == 'H')) {
            puts("EDIT [/R] [file]: vmdos's text editor. /R: read-only. F1 in EDIT lists the keys.");
            return 0;
        }
        if (argv[i][0] == '/' && (argv[i][1] == 'r' || argv[i][1] == 'R')) { readonly = 1; continue; }
        if (!fname[0]) {
            /* a long name with spaces comes in quotes, split by the C library: put it back together */
            char b[262] = "";
            if (argv[i][0] == '"') {
                strncat(b, argv[i] + 1, sizeof b - 1);
                while (b[0] && b[strlen(b) - 1] != '"' && i + 1 < argc) {
                    strncat(b, " ", sizeof b - strlen(b) - 1);
                    strncat(b, argv[++i], sizeof b - strlen(b) - 1);
                }
                if (b[0] && b[strlen(b) - 1] == '"') b[strlen(b) - 1] = 0;
            } else strncpy(b, argv[i], sizeof b - 1);
            open_file(b);
        }
    }

    screen_init();
    cursor_shape(!insmode);
    int lastkey = 0;
    for (;;) {
        scroll_to_cursor();
        draw();
        unsigned k = getkey();
        int was = lastkey;
        lastkey = k;
        switch (k) {
        case K_UP:    if (cy) { cy--; cx = pos_of_vcol(cy, wantcol); } break;
        case K_DOWN:  if (cy + 1 < nlines) { cy++; cx = pos_of_vcol(cy, wantcol); } break;
        case K_LEFT:  if (cx) cx--; else if (cy) { cy--; cx = len[cy]; } wantcol = vcol(cy, cx); break;
        case K_RIGHT: if (cx < len[cy]) cx++; else if (cy + 1 < nlines) { cy++; cx = 0; } wantcol = vcol(cy, cx); break;
        case K_HOME: {                       /* to the first non-blank, then the start */
            unsigned f = 0;
            while (f < len[cy] && (text[cy][f] == ' ' || text[cy][f] == '\t')) f++;
            cx = cx == f ? 0 : f; wantcol = vcol(cy, cx); break; }
        case K_END:   cx = len[cy]; wantcol = vcol(cy, cx); break;
        case K_PGUP:  cy = cy > (unsigned)trows - 1 ? cy - (trows - 1) : 0;
                      top = top > (unsigned)trows - 1 ? top - (trows - 1) : 0; cx = pos_of_vcol(cy, wantcol); break;
        case K_PGDN:  cy = cy + trows - 1 < nlines ? cy + trows - 1 : nlines - 1;
                      if (top + trows - 1 < nlines) top += trows - 1; cx = pos_of_vcol(cy, wantcol); break;
        case K_CHOME: cy = cx = 0; wantcol = 0; break;
        case K_CEND:  cy = nlines - 1; cx = len[cy]; wantcol = vcol(cy, cx); break;
        case K_CLEFT:
            if (cx == 0) { if (cy) { cy--; cx = len[cy]; } }
            else { while (cx && text[cy][cx - 1] == ' ') cx--; while (cx && text[cy][cx - 1] != ' ') cx--; }
            wantcol = vcol(cy, cx); break;
        case K_CRIGHT:
            if (cx >= len[cy]) { if (cy + 1 < nlines) { cy++; cx = 0; } }
            else { while (cx < len[cy] && text[cy][cx] != ' ') cx++; while (cx < len[cy] && text[cy][cx] == ' ') cx++; }
            wantcol = vcol(cy, cx); break;
        case K_INS:   insmode = !insmode; cursor_shape(!insmode); break;
        case K_DEL:   del_char(); wantcol = vcol(cy, cx); break;
        case 8:       backspace(); wantcol = vcol(cy, cx); break;
        case 13:      split_line(); wantcol = 0; break;
        case 0x19:    cut_line(was == 0x19); break;                 /* Ctrl+Y */
        case 0x0B:    copy_line(was == 0x0B); break;                /* Ctrl+K */
        case 0x15:    paste(); break;                               /* Ctrl+U */
        case 0x13: case K_F2:                                        /* Ctrl+S, F2 */
            if (readonly) { strcpy(msg, "Read-only: Shift+F2 saves under another name"); break; }
            if (!fname[0]) {
                char b[262] = "";
                if (!prompt("Save as: ", b, sizeof b) || !b[0]) break;
                strcpy(fname, b);
            }
            save(fname);
            break;
        case K_SF2: {
            char b[262];
            strcpy(b, fname);
            if (!prompt("Save as: ", b, sizeof b) || !b[0]) break;
            if (save(b)) { strcpy(fname, b); readonly = 0; }
            break; }
        case 0x0F: {                                                 /* Ctrl+O */
            if (!confirm_discard()) break;
            char b[262] = "";
            if (!prompt("Open: ", b, sizeof b) || !b[0]) break;
            open_file(b);
            break; }
        case 0x06:                                                   /* Ctrl+F */
            if (!prompt("Find: ", findstr, sizeof findstr)) break;
            /* fall through */
        case K_F3:
            if (!findstr[0]) { strcpy(msg, "Ctrl+F: what to find"); break; }
            if (!find_next(k == K_F3)) sprintf(msg, "\"%s\" not found", findstr);
            wantcol = vcol(cy, cx);
            break;
        case 0x12: replace_all_prompt(); break;                     /* Ctrl+R */
        case 0x07: {                                                 /* Ctrl+G */
            char b[12] = "";
            if (!prompt("Go to line: ", b, sizeof b)) break;
            long n = atol(b);
            if (n >= 1) { cy = (unsigned)(n - 1 < (long)nlines ? n - 1 : nlines - 1); cx = 0; wantcol = 0; }
            break; }
        case K_F1: show_help(); break;
        case 27: case K_F10: case K_ALTX:
            if (confirm_discard()) {
                for (int r = 0; r < rows; r++) puts_at(r, 0, "", 0x07, cols);
                gotoxy(0, 0);
                cursor_shape(0);
                return 0;
            }
            break;
        case 9: ins_char('\t'); wantcol = vcol(cy, cx); break;
        default:
            if (k >= 32 && k < 256) { ins_char((char)k); wantcol = vcol(cy, cx); }
            break;
        }
    }
}
