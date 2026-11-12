/* The media console: Ctrl+Shift+F6 opens a command line over whatever is
   running (the framebuffer, as the Ctrl+Shift+F10 log), to change CD
   images, floppy images and VMHD's partitions without leaving the game:
   "disc 2" when a game asks for it, a floppy for an installer.

   While it is open it has the keyboard (the guest is told the modifiers
   it saw go down are up again, and gets nothing until it closes); the
   game keeps running underneath. Esc or Ctrl+Shift+F6 closes it.

   Keys come in from the keyboard interrupt (vdev.c) and only edit the
   line; Enter queues the command, and the timer tick runs it (media_run)
   once no disk transfer, write-back or USB transfer is under way (v86.c),
   as Ctrl+Shift+S's log_save does. The commands call what VMCD, VMFD and
   VMHD reach through INT 2Fh (cd.c, floppy.c, hd.c), so a change is the
   same media change to DOS (and to the ATAPI drives Windows uses).

   Paths are 8.3, on C: unless they start with another drive letter (a
   VMHD drive for CD images and DIR; a VMDOSNET share for CD images). CD
   changes the current folder on C:, which paths without a leading
   backslash start from. */
#include "kernel.h"

int cd_con_ready(void);
int cd_con_units(void);
int cd_con_unit_of(char letter);
char cd_con_letter(int u);
int cd_con_modules(void);
const char *cd_con_module_name(int i);
void cd_con_describe(int u, char *b, int n);
int cd_con_insert(int u, const char *path);
int cd_con_module(int u, int i);
int cd_con_eject(int u);
int fd_con_insert(int d, const char *path, int ro);
int fd_con_eject(int d);
void fd_con_describe(int d, char *b, int n);
int hd_con_units(int *first);
int hd_con_scan(void);
void hd_con_describe_unit(int u, char *b, int n);
void hd_con_describe_part(int k, char *b, int n);
int hd_con_attach(int drive, int k, int ro, int confirmed, char *msg, int msgn);
int hd_con_eject(int drive);
int hd_drive_part(int drive, int *dev, u32 *start);
int video_overlay_ok(void);

#define MC_W 76                                 /* video.c's LOGOV_W */
#define MC_LINES 256                            /* output kept, a ring */
#define IN_MAX (MC_W - 3)
#define HIST 16

static struct rlock mc_lock;                    /* the text: the keyboard IRQ and the tick write, the drawing CPU reads */
static volatile int on;
static char mc_out[MC_LINES][MC_W + 1];
static u32 out_n;                               /* lines ever written */
static int scroll;                              /* lines up from the newest */
static char in[IN_MAX + 1];
static int in_len;
static char hist[HIST][IN_MAX + 1];
static int hist_n, hist_at;
static char cmd[IN_MAX + 1];
static volatile int cmd_req;
static int greeted;
static char cwd[64] = "\\";                    /* the current folder on C: ("\\ISOS"), for relative paths */

/* ---------------- output ---------------- */
static void add_line(const char *s, int n)
{
    char *d = mc_out[out_n % MC_LINES];
    int k = 0;
    for (; k < n && k < MC_W; k++) d[k] = s[k];
    d[k] = 0;
    out_n++;
}

/* Lines (\n or \r\n), wrapped at MC_W. */
static void pr(const char *fmt, ...)
{
    char b[640];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    rlock_take(&mc_lock);
    const char *p = b;
    for (;;) {
        int n = 0;
        while (p[n] && p[n] != '\n' && p[n] != '\r') n++;
        const char *q = p;
        int m = n;
        for (;;) {                                                  /* wrapped at a space when there is one */
            if (m <= MC_W) { add_line(q, m); break; }
            int cut = MC_W;
            while (cut > MC_W / 2 && q[cut] != ' ') cut--;
            if (q[cut] != ' ') cut = MC_W;
            add_line(q, cut);
            q += cut; m -= cut;
            while (m > 0 && *q == ' ') q++, m--;
            if (!m) break;
        }
        p += n;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
        if (!*p) break;
    }
    scroll = 0;
    rlock_drop(&mc_lock);
}

static void cls(void)
{
    rlock_take(&mc_lock);
    out_n = 0; scroll = 0;
    rlock_drop(&mc_lock);
}

/* For video.c: rows lines of w + 1 chars (w <= MC_W): the title, the
   newest output, the prompt. */
void media_text(char *out, int rows, int w, int cursor)
{
    if (w > MC_W) w = MC_W;
    rlock_take(&mc_lock);
    int body = rows - 2;
    u32 have = out_n < MC_LINES ? out_n : MC_LINES;
    int max = (int)have > body ? (int)have - body : 0;
    if (scroll > max) scroll = max;
    snprintf(out, w + 1, scroll ? "vmdos media console (scrolled back: PgDn)" :
             "vmdos media console - help for commands, Esc closes");
    for (int r = 0; r < body; r++) {
        char *d = out + (r + 1) * (w + 1);
        long l = (long)out_n - scroll - body + r;
        if (l < 0 || l < (long)out_n - (long)have) { d[0] = 0; continue; }
        const char *s = mc_out[l % MC_LINES];
        int c = 0;
        for (; c < w && s[c]; c++) d[c] = s[c];
        d[c] = 0;
    }
    char *d = out + (rows - 1) * (w + 1);
    char pfx[72];
    int pl = snprintf(pfx, sizeof pfx, "C:%s> ", cwd);
    if (pl > w / 2) pl = snprintf(pfx, sizeof pfx, "C:...> ");
    int start = in_len + pl + 1 > w ? in_len + pl + 1 - w : 0;   /* a long line: its end shows */
    int c = 0;
    for (; pfx[c]; c++) d[c] = pfx[c];
    for (int i = start; i < in_len && c < w - 1; i++) d[c++] = in[i];
    d[c++] = cmd_req ? ' ' : cursor ? '_' : ' ';
    d[c] = 0;
    rlock_drop(&mc_lock);
}

/* ---------------- the keyboard (from vdev.c, in the keyboard interrupt) ---------------- */
int media_on(void) { return on; }
int media_pending(void) { return cmd_req; }

static void queue(const char *c)
{
    int i = 0;
    for (; c[i] && i < IN_MAX; i++) cmd[i] = c[i];
    cmd[i] = 0;
    cmd_req = 1;
}

void media_toggle(void)
{
    if (on) { on = 0; return; }
    if (!video_overlay_ok()) { video_osd("Media console: not on this screen"); return; }
    on = 1;
    scroll = 0;
    if (!greeted) {
        greeted = 1;
        pr("Change CDs, floppies and VMHD drives while a program runs. Type help.");
    }
    if (!cmd_req) queue("status");
}

static const char key_lo[0x3A] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0, 'a', 's',
    'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\', 'z', 'x', 'c', 'v',
    'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' ',
};
static const char key_hi[0x3A] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' ',
};

static void set_input(const char *s)
{
    int i = 0;
    for (; s[i] && i < IN_MAX; i++) in[i] = s[i];
    in[i] = 0;
    in_len = i;
}

/* A set-1 key (k without the break bit), up or down. */
void media_key(u8 k, int up, int shift, int ctrl)
{
    if (up) return;
    rlock_take(&mc_lock);
    switch (k) {
    case 0x01: on = 0; break;                                       /* Esc */
    case 0x1C:                                                      /* Enter */
        if (cmd_req) break;                                         /* the last one hasn't run yet */
        in[in_len] = 0;
        rlock_drop(&mc_lock);
        pr("C:%s> %s", cwd, in);
        rlock_take(&mc_lock);
        if (in_len) {
            if (!hist_n || strncmp(hist[(hist_n - 1) % HIST], in, IN_MAX + 1)) {
                memcpy(hist[hist_n % HIST], in, IN_MAX + 1);
                hist_n++;
            }
            queue(in);
        }
        hist_at = hist_n;
        in_len = 0; in[0] = 0;
        break;
    case 0x0E: if (in_len) in[--in_len] = 0; break;               /* Backspace */
    case 0x48:                                                      /* Up: history */
        if (hist_at > 0 && hist_at > hist_n - HIST) { hist_at--; set_input(hist[hist_at % HIST]); }
        break;
    case 0x50:                                                      /* Down */
        if (hist_at < hist_n) { hist_at++; set_input(hist_at < hist_n ? hist[hist_at % HIST] : ""); }
        break;
    case 0x49: scroll += 8; break;                                  /* PgUp */
    case 0x51: scroll = scroll > 8 ? scroll - 8 : 0; break;         /* PgDn */
    default:
        if (ctrl && k == 0x16) { in_len = 0; in[0] = 0; break; }   /* Ctrl+U: clear the line */
        if (ctrl && k == 0x26) { rlock_drop(&mc_lock); cls(); return; }   /* Ctrl+L: clear the screen */
        if (ctrl || k >= 0x3A) break;
        char ch = shift ? key_hi[k] : key_lo[k];
        if (ch && in_len < IN_MAX) { in[in_len++] = ch; in[in_len] = 0; }
    }
    rlock_drop(&mc_lock);
}

/* ---------------- the commands (from the timer tick) ---------------- */
static char lc(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int is(const char *a, const char *w)                        /* a == w, any case */
{
    while (*a && *w && lc(*a) == *w) a++, w++;
    return !*a && !*w;
}
static int all_digits(const char *s) { if (!*s) return 0; for (; *s; s++) if (*s < '0' || *s > '9') return 0; return 1; }
static int num(const char *s) { int n = 0; for (; *s >= '0' && *s <= '9'; s++) n = n * 10 + *s - '0'; return n; }
/* "D:" or "D" -> 0-based DOS drive, else -1. */
static int letter(const char *s)
{
    char c = (char)(s[0] & ~32);
    if (c < 'A' || c > 'Z' || (s[1] && !(s[1] == ':' && !s[2]))) return -1;
    return c - 'A';
}
static int is_eject(const char *s) { return is(s, "eject") || is(s, "/e") || is(s, "empty") || is(s, "-e"); }

/* A DOS path from what was typed: upper case, backslashes, "." and ".."
   worked out. On C: a path without a leading backslash is relative to the
   current folder (cd); "D:..." is from D:'s root. Out: "C:\ISOS\GAME.ISO",
   "C:\" for the root. */
static void norm(const char *p, char *out, int n)
{
    char raw[160];
    char drv = 'C';
    if (p[0] && p[1] == ':') { drv = (char)(p[0] & ~32); p += 2; }
    if (*p == '\\' || *p == '/' || drv != 'C') snprintf(raw, sizeof raw, "%s", p);
    else snprintf(raw, sizeof raw, "%s\\%s", cwd, p);
    int j = 0;
    out[j++] = drv; out[j++] = ':';
    const char *q = raw;
    for (;;) {
        while (*q == '\\' || *q == '/') q++;
        if (!*q) break;
        int k = 0;
        while (q[k] && q[k] != '\\' && q[k] != '/') k++;
        if (k == 1 && q[0] == '.') ;                                   /* "." */
        else if (k == 2 && q[0] == '.' && q[1] == '.') {               /* "..": up one */
            while (j > 2 && out[j - 1] != '\\') j--;
            if (j > 2) j--;
        } else if (j + 1 + k < n) {
            out[j++] = '\\';
            for (int i = 0; i < k; i++) { char c = q[i]; out[j++] = c >= 'a' && c <= 'z' ? (char)(c - 32) : c; }
        }
        q += k;
    }
    if (j == 2) out[j++] = '\\';
    out[j] = 0;
}

static const char *cd_err(int e)
{
    switch (e) {
    case 2: return "not found (8.3 names: DIR shows them)";
    case 3: return "not an ISO 9660 image";
    case 4: return "not on C:, a VMHD drive or a VMDOSNET share";
    case 5: return "not an image drive";
    case 6: return "out of memory (too many images?)";
    case 7: return "disk error";
    case 8: return "a CUE sheet vmdos can't use (see the log)";
    }
    return "failed";
}
static const char *fd_err(int e)
{
    switch (e) {
    case 2: return "not found (8.3 names: DIR shows them)";
    case 3: return "not a floppy image (160K-2.88M, or a boot sector to go by)";
    case 4: return "floppy images have to be on C:";
    case 6: return "out of memory";
    case 7: return "disk error";
    }
    return "failed";
}

/* "1 (F:)" */
static void cd_name(int u, char *b, int n)
{
    char l = cd_con_letter(u);
    if (l) snprintf(b, n, "%d (%c:)", u + 1, l); else snprintf(b, n, "%d", u + 1);
}

static void show_cd(void)
{
    if (!cd_con_ready()) { pr("CD:      no drives yet (VMCD.SYS sets them up when DOS starts)"); return; }
    char b[120], nm[12];
    for (int u = 0; u < cd_con_units(); u++) {
        cd_name(u, nm, sizeof nm);
        cd_con_describe(u, b, sizeof b);
        pr("%s %-6s %s", u ? "        " : "CD:     ", nm, b);
    }
    if (cd_con_modules() > 1) {
        pr("         images loaded with vmdos (vmcd N #M):");
        for (int i = 0; i < cd_con_modules(); i++) pr("           #%d %s", i + 1, cd_con_module_name(i));
    }
}
static void show_fd(void)
{
    char a[100], b[100];
    fd_con_describe(0, a, sizeof a);
    fd_con_describe(1, b, sizeof b);
    pr("Floppy:  A: %s", a);
    pr("         B: %s", b);
}
static void show_hd(void)
{
    int first, n = hd_con_units(&first);
    if (!n) { pr("VMHD:    no drive letters (DEVICE=C:\\VMDOS\\VMHD.SYS in FDCONFIG.SYS)"); return; }
    char b[120];
    for (int u = 0; u < n; u++) {
        hd_con_describe_unit(u, b, sizeof b);
        pr("%s %c: %s", u ? "        " : "VMHD:   ", 'A' + first + u, b);
    }
}

static void help(void)
{
    pr("Paths are 8.3 names, on C: unless another drive is given.");
    pr("  status                    what is in every drive");
    pr("  cd \\ISOS   cd ..   cd     change folder on C: / show it (paths are relative)");
    pr("  dir [\\ISOS]               files and folders (C:, or a VMHD drive: dir D:\\)");
    pr("  vmcd                      the CD drives");
    pr("  vmcd 1 GAME.ISO           ISO or CUE sheet in CD drive 1 (F: works once");
    pr("                            VMCD has run in DOS and told vmdos the letters)");
    pr("  vmcd 1 eject              empty it (a real drive: open the tray)");
    pr("  vmcd 1 #2                 image 2 of the ones loaded with vmdos");
    pr("  vmfd A: DISK2.IMG         floppy image in A: (/R after it: read-only)");
    pr("  vmfd A: eject             empty A:");
    pr("  vmhd                      VMHD's drives, the partitions of other disks");
    pr("  vmhd D: 3                 partition 3 in D: (/R read-only, /Y: confirm)");
    pr("  vmhd D: eject             take it out of D:");
    pr("  cls                       clear; Up/Down: earlier lines; PgUp/PgDn: scroll");
    pr("  Esc or Ctrl+Shift+F6 closes; the program runs on underneath.");
    pr("Short names: cdrom for vmcd, fd for vmfd, hd for vmhd.");
}

static void do_cdrom(int argc, char **argv)
{
    if (argc < 2) { show_cd(); return; }
    if (!cd_con_ready()) { pr("No CD drives yet: VMCD.SYS sets them up when DOS starts."); return; }
    int u = -1;
    if (all_digits(argv[1])) u = num(argv[1]) - 1;
    else if (letter(argv[1]) >= 0) {
        u = cd_con_unit_of((char)('A' + letter(argv[1])));
        if (u < 0) { pr("%s isn't known as a CD drive (run VMCD once in DOS, or use the drive's number: vmcd)", argv[1]); return; }
    }
    if (u < 0 || u >= cd_con_units()) { pr("No CD drive %s (vmcd lists them).", argv[1]); return; }
    char nm[12];
    cd_name(u, nm, sizeof nm);
    if (argc < 3) { char b[120]; cd_con_describe(u, b, sizeof b); pr("CD %s: %s", nm, b); return; }
    int e;
    if (is_eject(argv[2])) {
        e = cd_con_eject(u);
        if (!e) pr("CD %s: ejected.", nm);
    } else if (argv[2][0] == '#' || all_digits(argv[2])) {
        int i = num(argv[2] + (argv[2][0] == '#')) - 1;
        if (!cd_con_modules()) { pr("No images were loaded with vmdos (give a path instead)."); return; }
        e = cd_con_module(u, i);
        if (e == 5 && (i < 0 || i >= cd_con_modules())) { pr("No image #%d (vmcd lists them).", i + 1); return; }
        if (!e) pr("CD %s now holds %s.", nm, cd_con_module_name(i));
    } else {
        char path[80];
        norm(argv[2], path, sizeof path);
        e = cd_con_insert(u, path);
        if (!e) { char b[120]; cd_con_describe(u, b, sizeof b); pr("CD %s now holds %s.", nm, b); }
        else { pr("%s: %s", path, cd_err(e)); return; }
    }
    if (e == 5) pr("CD %s is a real drive: put a disc in it (or eject it).", nm);
}

static void do_fd(int argc, char **argv)
{
    if (argc < 2) { show_fd(); return; }
    int d = letter(argv[1]);
    if (d < 0 || d > 1) { pr("Floppy drives are A: and B:."); return; }
    if (argc < 3) { char b[100]; fd_con_describe(d, b, sizeof b); pr("%c: %s", 'A' + d, b); return; }
    if (is_eject(argv[2])) { fd_con_eject(d); pr("%c: is empty.", 'A' + d); return; }
    int ro = argc > 3 && is(argv[3], "/r");
    char path[80];
    norm(argv[2], path, sizeof path);
    int e = fd_con_insert(d, path, ro);
    if (e) { pr("%s: %s", path, fd_err(e)); return; }
    char b[100];
    fd_con_describe(d, b, sizeof b);
    pr("%c: now holds %s.", 'A' + d, b);
}

static void do_hd(int argc, char **argv)
{
    int first, n = hd_con_units(&first);
    if (argc < 2) {
        show_hd();
        int np = hd_con_scan();
        char b[140];
        if (np) pr("Partitions:");
        else pr("No other disks with partitions.");
        for (int k = 0; k < np; k++) { hd_con_describe_part(k, b, sizeof b); pr("  [%d] %s", k + 1, b); }
        if (n && np) pr("vmhd %c: N puts partition N in %c: (/R read-only).", 'A' + first, 'A' + first);
        return;
    }
    if (!n) { pr("VMHD has no drive letters: add DEVICE=C:\\VMDOS\\VMHD.SYS to C:\\FDCONFIG.SYS."); return; }
    int d = letter(argv[1]);
    if (d < first || d >= first + n) { pr("VMHD's drives are %c: to %c:.", 'A' + first, 'A' + first + n - 1); return; }
    if (argc < 3) { char b[120]; hd_con_describe_unit(d - first, b, sizeof b); pr("%c: %s", 'A' + d, b); return; }
    if (is_eject(argv[2])) { hd_con_eject(d); pr("%c: is empty now (the partition itself is untouched).", 'A' + d); return; }
    if (!all_digits(argv[2])) { pr("vmhd %c: N, with N a partition number from  vmhd", 'A' + d); return; }
    int ro = 0, yes = 0;
    for (int i = 3; i < argc; i++) { if (is(argv[i], "/r")) ro = 1; if (is(argv[i], "/y")) yes = 1; }
    static char msg[600];
    int e = hd_con_attach(d, num(argv[2]) - 1, ro, yes, msg, sizeof msg);
    switch (e) {
    case 0: case 6: pr("%s", msg); if (e == 6) pr("To use it anyway: vmhd %c: %s /Y", 'A' + d, argv[2]); break;
    case 2: pr("No partition %s (vmhd lists them).", argv[2]); break;
    case 3: pr("That is C:."); break;
    case 4: pr("That partition is in another drive already."); break;
    case 5: pr("DOS can't use it: no FAT file system, and not a DOS partition."); break;
    case 7: pr("Disk error."); break;
    case 8: pr("%c: holds another partition: vmhd %c: eject first.", 'A' + d, 'A' + d); break;
    default: pr("Failed (%d).", e);
    }
}

/* DIR */
struct dirctx { int n, col; char row[MC_W + 1]; };
static void dir_flush(struct dirctx *c) { if (c->col) { pr("%s", c->row); c->col = 0; } }
static int dir_one(void *ctx, const char *name, u32 clus, u32 size)
{
    struct dirctx *c = ctx;
    char e[26];
    int dir = name[0] && name[strlen(name) - 1] == '\\';
    if (dir) snprintf(e, sizeof e, "%-13s   <DIR>", name);
    else if (size >= 10u << 20) snprintf(e, sizeof e, "%-13s %4u MB", name, size >> 20);
    else if (size >= 10u << 10) snprintf(e, sizeof e, "%-13s %4u KB", name, size >> 10);
    else snprintf(e, sizeof e, "%-13s %4u  B", name, size);
    int at = c->col * 25;
    snprintf(c->row + at, sizeof c->row - at, "%-25s", e);
    if (++c->col == 3) dir_flush(c);
    return ++c->n >= 600;
}
static int dir_dev;
static int dir_rd(u32 lba, u32 n, void *buf) { return disk_dev_rw(dir_dev, lba, n, buf, 0); }
static void do_dir(int argc, char **argv)
{
    static struct fatvol v;
    char path[80];
    norm(argc > 1 ? argv[1] : ".", path, sizeof path);
    int d = path[0] - 'A';
    if (d == 2) { if (disk_volume(&v)) { pr("C: has no FAT partition vmdos can read."); return; } }
    else {
        u32 start;
        if (hd_drive_part(d, &dir_dev, &start)) { pr("%c: isn't C: or a VMHD drive with a partition in it.", path[0]); return; }
        if (fat_mount(&v, dir_rd, start)) { pr("%c: has no FAT file system vmdos can read.", path[0]); return; }
    }
    struct dirctx c = { 0, 0, "" };
    pr("Folder %s", path);
    if (fat_dir_all(&v, path, dir_one, &c)) { pr("No such folder."); return; }
    dir_flush(&c);
    pr(c.n >= 600 ? "(first 600 shown)" : c.n > 1 ? "%d entries" : c.n ? "1 entry" : "Empty.", c.n);
}

/* CD: change the current folder (on C: only). */
static int stop_at_first(void *ctx, const char *name, u32 clus, u32 size) { return 1; }
static void do_chdir(const char *arg)
{
    if (!arg || !*arg) { pr("C:%s", cwd); return; }
    char path[80];
    norm(arg, path, sizeof path);
    if (path[0] != 'C') { pr("Only C: here: the media console's folders are on C:."); return; }
    if (path[3]) {                                                    /* not the root: is it a folder? */
        static struct fatvol v;
        if (disk_volume(&v)) { pr("C: has no FAT partition vmdos can read."); return; }
        if (fat_dir_all(&v, path, stop_at_first, 0)) {
            pr("No folder %s%s", path, all_digits(arg) ? " (CD drives: vmcd)" : "");
            return;
        }
    }
    snprintf(cwd, sizeof cwd, "%s", path + 2);
}

/* The queued command, at a timer tick with the disk and USB quiet. */
void media_run(void)
{
    if (!cmd_req) return;
    char line[IN_MAX + 1];
    memcpy(line, cmd, sizeof line);
    char *argv[8];
    int argc = 0;
    for (char *p = line; *p && argc < 8; ) {
        while (*p == ' ' || *p == '\t') *p++ = 0;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (argc) {
        const char *c = argv[0];
        if (is(c, "help") || is(c, "?") || is(c, "h")) help();
        else if (is(c, "status") || is(c, "st")) { show_cd(); show_fd(); show_hd(); }
        else if (is(c, "cd") || is(c, "chdir")) do_chdir(argc > 1 ? argv[1] : 0);
        else if ((c[0] | 32) == 'c' && (c[1] | 32) == 'd' && (c[2] == '.' || c[2] == '\\')) do_chdir(c + 2);   /* cd.. cd\ */
        else if (is(c, "cdrom") || is(c, "vmcd")) do_cdrom(argc, argv);
        else if (is(c, "fd") || is(c, "vmfd") || is(c, "floppy")) do_fd(argc, argv);
        else if (is(c, "hd") || is(c, "vmhd")) do_hd(argc, argv);
        else if (is(c, "dir") || is(c, "ls")) do_dir(argc, argv);
        else if (is(c, "cls") || is(c, "clear")) cls();
        else if (is(c, "exit") || is(c, "close") || is(c, "quit")) on = 0;
        else pr("Unknown command %s (type help).", c);
    }
    cmd_req = 0;
}
