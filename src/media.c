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

   Paths are 8.3, on the current drive unless they start with another
   drive letter, as in DOS: "D:" alone makes D: the current drive, each
   drive has its own current folder (CD), and a path without a leading
   backslash starts from it. DIR and CD work on C:, the floppy images in
   A: and B:, VMHD's drives (looked at again each time, so a partition
   formatted since it went in shows its files) and the CD drives (ISO
   9660). CD images can be on C:, a VMHD drive or a VMDOSNET share;
   floppy images on C:.

   LSPCI and LSUSB show what LSPCI.COM and LSUSB.COM do (sysinfo.cpp);
   RESCAN looks at every disk again (USB disks plugged in since, new
   partitions, new file systems). */
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
int fd_con_read(int d, u32 lba, u32 n, void *buf);
int fd_con_in(int d);
int hd_con_refresh(int drive);
int cd_unit_read(int unit, u32 lba, u32 n, u8 *buf);
int sysinfo_text(int which, int flags, char *buf, int max);

#define MC_W 76                                 /* video.c's LOGOV_W */
#define MC_LINES 512                            /* output kept, a ring (LSPCI -V is long) */
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
static char cur = 'C';                          /* the current drive */
static char cwds[26][128];                       /* each drive's current folder ("\\ISOS"; "" is the root) */
static const char *cwd_of(char d)
{
    const char *c = cwds[(d & ~32) - 'A'];
    return c[0] ? c : "\\";
}
static void set_cwd(char d, const char *path)   /* path: "\\ISOS" or "\\" */
{
    snprintf(cwds[(d & ~32) - 'A'], sizeof cwds[0], "%s", path[0] && path[1] ? path : "");
}

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
    int pl = snprintf(pfx, sizeof pfx, "%c:%s> ", cur, cwd_of(cur));
    if (pl > w / 2) pl = snprintf(pfx, sizeof pfx, "%c:...> ", cur);
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
        if (disk_on_a) cur = 'A';                                   /* no C:: the RAM disk in A: */
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
        pr("%c:%s> %s", cur, cwd_of(cur), in);
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

/* A DOS path from what was typed: backslashes, "." and ".." worked out;
   long names are fine. On the current drive unless it starts with "D:"; without a
   leading backslash, from that drive's current folder (cd), as in DOS.
   Out: "C:\ISOS\GAME.ISO", "C:\" for the root. */
static void upcase(char *p) { for (; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32; }
static void norm(const char *p, char *out, int n)
{
    char raw[320];
    char drv = cur;
    if (p[0] && p[1] == ':' && ((p[0] | 32) >= 'a' && (p[0] | 32) <= 'z')) { drv = (char)(p[0] & ~32); p += 2; }
    if (*p == '\\' || *p == '/') snprintf(raw, sizeof raw, "%s", p);
    else snprintf(raw, sizeof raw, "%s\\%s", cwd_of(drv), p);
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
            for (int i = 0; i < k; i++) out[j++] = q[i];                /* the case as typed (cd puts the real one) */
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
    pr("Paths are 8.3 names, on the current drive unless another is given.");
    pr("  status                    what is in every drive");
    pr("  A:   D:                   make A: (D:) the current drive (looks at it again)");
    pr("  cd \\ISOS   cd ..   cd     change folder / show it (each drive has its own)");
    pr("  dir [\\ISOS]   dir A:      files and folders: C:, floppy images, VMHD, CDs");
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
    pr("  rescan                    look at every disk again (newly formatted, plugged in)");
    pr("  lspci [-v] [-x]           the PCI devices (as LSPCI.COM)");
    pr("  lsusb [-v]                the USB controllers and devices (as LSUSB.COM)");
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
    if (cd_con_letter(u)) set_cwd(cd_con_letter(u), "\\");          /* another disc: back to its root */
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
        char path[160];
        norm(argv[2], path, sizeof path);
        upcase(path);
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
    set_cwd((char)('A' + d), "\\");                                    /* another disk: back to its root */
    if (is_eject(argv[2])) { fd_con_eject(d); pr("%c: is empty.", 'A' + d); return; }
    int ro = argc > 3 && is(argv[3], "/r");
    char path[160];
    norm(argv[2], path, sizeof path);
    upcase(path);
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
    set_cwd((char)('A' + d), "\\");
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

/* ---------------- drives: what DIR and CD read ----------------
   A: and B: (floppy images), C:, VMHD's drives: FAT (fat.c). CD drives:
   ISO 9660, read here. */
struct vol { char drv; int iso, joliet; struct fatvol fv; u32 root_lba, root_len; };
static int joliet;                                                      /* the open disc's names: UCS-2 */
static int vol_dev, vol_fd, iso_u;
static u8 iso_sec[2048];
static int rd_hd(u32 lba, u32 n, void *buf) { return disk_dev_rw(vol_dev, lba, n, buf, 0); }
static int rd_fd(u32 lba, u32 n, void *buf) { return fd_con_read(vol_fd, lba, n, buf); }
static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* "A: B: C: D:-E: (VMHD) F: (CD)": the drives the console can read. */
static void known_drives(char *b, int n)
{
    int k = snprintf(b, n, "A: B:%s", disk_on_a ? "" : " C:");
    int first, nh = hd_con_units(&first);
    if (nh == 1) k += snprintf(b + k, n - k, " %c: (VMHD)", 'A' + first);
    else if (nh > 1) k += snprintf(b + k, n - k, " %c:-%c: (VMHD)", 'A' + first, 'A' + first + nh - 1);
    for (int u = 0; u < cd_con_units() && k < n - 12; u++)
        if (cd_con_letter(u)) k += snprintf(b + k, n - k, " %c: (CD)", cd_con_letter(u));
}

static int iso_open(int u, struct vol *v)
{
    iso_u = u;
    v->iso = 1;
    v->joliet = joliet = 0;
    v->root_len = 0;
    for (u32 s = 16; s < 32; s++) {                                     /* the volume descriptors */
        int e = cd_unit_read(u, s, 1, iso_sec);
        if (e == 2) { pr("%c: has no disc in it.", v->drv); return -1; }
        if (e || memcmp(iso_sec + 1, "CD001", 5) || iso_sec[0] == 255) break;
        int jol = iso_sec[0] == 2 && iso_sec[88] == '%' && iso_sec[89] == '/' &&
                  (iso_sec[90] == '@' || iso_sec[90] == 'C' || iso_sec[90] == 'E');
        if (iso_sec[0] != 1 && !jol) continue;                          /* a boot record */
        if (v->root_len && !jol) continue;
        v->root_lba = le32(iso_sec + 156 + 2);                          /* Joliet's (long names) over the primary one's */
        v->root_len = le32(iso_sec + 156 + 10);
        v->joliet = jol;
        if (jol) break;
    }
    if (v->root_len) { joliet = v->joliet; return 0; }
    pr("%c: the disc has no ISO 9660 file system (an audio CD?).", v->drv);
    return -1;
}

/* Every record of the directory at lba (len bytes) to fn, "." and ".."
   left out; 1 when fn stopped it, -1 a read error. */
static int iso_each(u32 lba, u32 len, int (*fn)(void *ctx, const u8 *rec), void *ctx)
{
    for (u32 off = 0; off < len; off += 2048) {
        if (cd_unit_read(iso_u, lba + off / 2048, 1, iso_sec)) return -1;
        for (u32 i = 0; i < 2048; ) {
            u32 l = iso_sec[i];
            if (!l || i + l > 2048) break;                              /* the rest of this sector is padding */
            const u8 *r = iso_sec + i;
            i += l;
            if (r[32] == 1 && r[33] <= 1) continue;
            if (fn(ctx, r)) return 1;
        }
    }
    return 0;
}
/* A record's name, without ";1" or a lone trailing dot (Joliet's UCS-2:
   '?' outside ASCII). */
static void iso_name(const u8 *r, char *b, int n)
{
    int k = 0;
    for (int i = 0; i < r[32] && k < n - 1; i += joliet ? 2 : 1) {
        u32 c = joliet ? (u32)r[33 + i] << 8 | r[34 + i] : r[33 + i];
        if (c == ';') break;
        b[k++] = c < 0x80 ? (char)c : '?';
    }
    if (k && b[k - 1] == '.') k--;
    b[k] = 0;
}
static int same_ci(const char *a, int n, const char *b)                   /* a (n chars) == b, case aside */
{
    for (int i = 0; i < n; i++) {
        char x = a[i], y = b[i];
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y || !y) return 0;
    }
    return !b[n];
}
struct iso_find { const char *want; int len; u32 lba, size; int found; };
static int iso_match(void *ctx, const u8 *r)
{
    struct iso_find *f = ctx;
    char nm[130];
    iso_name(r, nm, sizeof nm);
    if (!(r[25] & 2) || !same_ci(f->want, f->len, nm)) return 0;
    f->lba = le32(r + 2); f->size = le32(r + 10); f->found = 1;
    return 1;
}
struct iso_list { int (*fn)(void *, const char *, u32, u32); void *ctx; };
static int iso_lister(void *ctx, const u8 *r)
{
    struct iso_list *l = ctx;
    char nm[130];
    iso_name(r, nm, sizeof nm - 1);
    if (r[25] & 2) { int k = (int)strlen(nm); nm[k] = '\\'; nm[k + 1] = 0; }
    return l->fn(l->ctx, nm, 0, r[25] & 2 ? 0 : le32(r + 10));
}
static int iso_dir(struct vol *v, const char *path, int (*fn)(void *, const char *, u32, u32), void *ctx)
{
    u32 lba = v->root_lba, len = v->root_len;
    if (path[0] && path[1] == ':') path += 2;
    while (*path) {                                                     /* down to the folder */
        while (*path == '\\') path++;
        if (!*path) break;
        int k = 0;
        while (path[k] && path[k] != '\\') k++;
        struct iso_find f = { path, k, 0, 0, 0 };
        if (iso_each(lba, len, iso_match, &f) < 0 || !f.found) return -1;
        lba = f.lba; len = f.size;
        path += k;
    }
    struct iso_list l = { fn, ctx };
    return iso_each(lba, len, iso_lister, &l) < 0 ? -1 : 0;
}

/* Drive drv ready to read: 0, or -1 with why not printed. A VMHD drive's
   partition is looked at again first: one formatted since it went in has
   a file system now (and DOS is told to read its BPB again). */
static int vol_open(char drv, struct vol *v)
{
    int d = (drv & ~32) - 'A';
    char k[96];
    v->drv = (char)('A' + d);
    v->iso = 0;
    if (d < 0 || d >= 26) { pr("No such drive."); return -1; }
    if (d < 2) {
        if (!fd_con_in(d)) { pr("%c: is empty (vmfd %c: IMAGE puts a floppy image in it).", v->drv, v->drv); return -1; }
        vol_fd = d;
        if (fat_mount(&v->fv, rd_fd, 0)) { pr("%c: holds no FAT disk vmdos can read (a booter's own disk?).", v->drv); return -1; }
        return 0;
    }
    if (d == 2 && !disk_on_a) {
        if (disk_volume(&v->fv)) { pr("C: has no FAT partition vmdos can read."); return -1; }
        return 0;
    }
    int h = hd_con_refresh(d);
    if (h == 0) { pr("%c: is a VMHD drive with nothing in it: vmhd lists the partitions, vmhd %c: N puts one in.", v->drv, v->drv); return -1; }
    if (h > 0) {
        u32 start;
        if (hd_drive_part(d, &vol_dev, &start)) { pr("%c: its disk is gone (rescan).", v->drv); return -1; }
        if (fat_mount(&v->fv, rd_hd, start)) { pr("%c: has no FAT file system yet (FORMAT %c: in DOS makes one).", v->drv, v->drv); return -1; }
        return 0;
    }
    int u = cd_con_unit_of(v->drv);
    if (u >= 0) return iso_open(u, v);
    known_drives(k, sizeof k);
    pr("%c: isn't a drive the console can read. Drives: %s", v->drv, k);
    if (!cd_con_ready() || !cd_con_units()) ;
    else for (int i = 0; i < cd_con_units(); i++) if (!cd_con_letter(i)) { pr("(CD drives get their letters once VMCD has run in DOS.)"); break; }
    return -1;
}
static int vol_dir(struct vol *v, const char *path, int (*fn)(void *, const char *, u32, u32), void *ctx)
{
    return v->iso ? iso_dir(v, path, fn, ctx) : fat_dir_all(&v->fv, path, fn, ctx);
}

/* DIR: two passes, the first for the widest name, so the columns fit the
   long names (one column when one is too long for more). */
struct dirctx { int n, col, w, cols, widest; char row[MC_W + 1]; };
static void dir_flush(struct dirctx *c) { if (c->col) { pr("%s", c->row); c->col = 0; } }
static int dir_measure(void *ctx, const char *name, u32 clus, u32 size)
{
    struct dirctx *c = ctx;
    int l = (int)strlen(name);
    if (l > c->widest) c->widest = l;
    return ++c->n >= 600;
}
static int dir_one(void *ctx, const char *name, u32 clus, u32 size)
{
    struct dirctx *c = ctx;
    char e[MC_W + 1], sz[12];
    int dir = name[0] && name[strlen(name) - 1] == '\\';
    if (dir) snprintf(sz, sizeof sz, "  <DIR>");
    else if (size >= 10u << 20) snprintf(sz, sizeof sz, "%4u MB", size >> 20);
    else if (size >= 10u << 10) snprintf(sz, sizeof sz, "%4u KB", size >> 10);
    else snprintf(sz, sizeof sz, "%4u  B", size);
    int nw = c->w - 10;                                                 /* the name's part of a column */
    if ((int)strlen(name) > nw) {                                       /* too long even alone: its end shown */
        int l = (int)strlen(name);
        snprintf(e, sizeof e, "..%s %s", name + l - (nw - 2), sz);
    } else snprintf(e, sizeof e, "%-*s %s", nw, name, sz);
    int at = c->col * c->w;
    snprintf(c->row + at, sizeof c->row - at, "%-*s", c->w, e);
    if (++c->col == c->cols) dir_flush(c);
    return ++c->n >= 600;
}
static struct vol vol;
static void do_dir(int argc, char **argv)
{
    char path[160];
    norm(argc > 1 ? argv[1] : ".", path, sizeof path);
    if (vol_open(path[0], &vol)) return;
    struct dirctx c = { 0, 0, 0, 0, 12, "" };
    if (vol_dir(&vol, path, dir_measure, &c)) { pr("Folder %s", path); pr("No such folder."); return; }
    c.w = c.widest + 10 + 2;                                            /* name, size, a gap */
    c.cols = (MC_W - 2) / c.w;
    if (c.cols < 1) { c.cols = 1; c.w = MC_W - 2; }
    if (c.cols > 3) { c.cols = 3; c.w = (MC_W - 2) / 3; }
    c.n = 0;
    pr("Folder %s", path);
    vol_dir(&vol, path, dir_one, &c);
    dir_flush(&c);
    pr(c.n >= 600 ? "(first 600 shown)" : c.n > 1 ? "%d entries" : c.n ? "1 entry" : "Empty.", c.n);
}

/* CD: change a drive's current folder (the current drive's, or D:'s with "cd D:\X"), or show it. */
static int stop_at_first(void *ctx, const char *name, u32 clus, u32 size) { return 1; }
/* path's folders under the names they have on the disk ("c:\my games" ->
   "C:\My Games"): what cd keeps, so the prompt shows them. */
struct real { const char *want; int len; char got[130]; };
static int real_one(void *ctx, const char *name, u32 clus, u32 size)
{
    struct real *r = ctx;
    int l = (int)strlen(name);
    if (l != r->len + 1 || name[l - 1] != '\\') return 0;                /* folders: "NAME\\" */
    for (int i = 0; i < r->len; i++) {
        char x = r->want[i], y = name[i];
        if ((x >= 'a' && x <= 'z' ? x - 32 : x) != (y >= 'a' && y <= 'z' ? y - 32 : y)) return 0;
    }
    memcpy(r->got, name, (u32)r->len);
    r->got[r->len] = 0;
    return 1;
}
static void real_names(struct vol *v, char *path, int n)
{
    char out[160], up[160];
    int j = 3, k;
    memcpy(out, path, 3);                                               /* "C:\" */
    for (const char *p = path + 3; *p; ) {
        for (k = 0; p[k] && p[k] != '\\'; k++) ;
        struct real r = { p, k, "" };
        memcpy(up, out, (u32)j); up[j] = 0;
        if (vol_dir(v, up, real_one, &r) < 0 || !r.got[0]) { memcpy(r.got, p, (u32)k); r.got[k] = 0; upcase(r.got); }   /* an 8.3 alias: as DOS shows it */
        if (j > 3 && j < (int)sizeof out - 1) out[j++] = '\\';
        for (int i = 0; r.got[i] && j < (int)sizeof out - 1; i++) out[j++] = r.got[i];
        p += k;
        if (*p) p++;
    }
    out[j] = 0;
    snprintf(path, n, "%s", out);
}
static int is_folder(struct vol *v, const char *path) { return !path[3] || vol_dir(v, path, stop_at_first, 0) >= 0; }
static void do_chdir(const char *arg)
{
    if (!arg || !*arg) { pr("%c:%s", cur, cwd_of(cur)); return; }
    if (arg[1] == ':' && !arg[2]) { pr("%c:%s", arg[0] & ~32, cwd_of(arg[0])); return; }   /* cd D: */
    char path[160];
    norm(arg, path, sizeof path);
    if (vol_open(path[0], &vol)) return;
    if (!is_folder(&vol, path)) { pr("No folder %s%s", path, all_digits(arg) ? " (CD drives: vmcd)" : ""); return; }
    real_names(&vol, path, sizeof path);
    set_cwd(path[0], path + 2);
}

/* "D:": make D: the current drive. Its disk is looked at again (a
   partition formatted since, a new floppy image or disc); its current
   folder goes back to the root if it's gone. */
static void do_drive(char drv)
{
    drv = (char)(drv & ~32);
    if (vol_open(drv, &vol)) return;
    char path[160];
    snprintf(path, sizeof path, "%c:%s", drv, cwd_of(drv));
    if (!is_folder(&vol, path)) set_cwd(drv, "\\");
    cur = drv;
}

/* RESCAN: every disk looked at again, and what VMHD can use now. */
static void do_rescan(void)
{
    int np = hd_con_scan();
    char b[140];
    show_hd();
    if (np) pr("Partitions:");
    else pr("No other disks with partitions.");
    for (int k = 0; k < np; k++) { hd_con_describe_part(k, b, sizeof b); pr("  [%d] %s", k + 1, b); }
}

/* LSPCI [-v] [-x], LSUSB [-v]: as LSPCI.COM and LSUSB.COM. */
static char sys_text[48 * 1024];
static void do_sys(int which, int argc, char **argv)
{
    int flags = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-' && argv[i][0] != '/') { pr("%s [-v]%s", which ? "lsusb" : "lspci", which ? "" : " [-x]"); return; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if ((*p | 32) == 'v') flags |= 1;
            else if ((*p | 32) == 'x' && !which) flags |= 2;
            else { pr("%s [-v]%s", which ? "lsusb" : "lspci", which ? "" : " [-x]"); return; }
        }
    }
    int n = sysinfo_text(which, flags, sys_text, sizeof sys_text);
    sys_text[n < (int)sizeof sys_text ? n : (int)sizeof sys_text - 1] = 0;
    int lines = 0;
    for (const char *p = sys_text; *p; ) {
        char line[200];
        int k = 0;
        for (; *p && *p != '\r' && *p != '\n'; p++) {
            if (*p == '\t') { do line[k++] = ' '; while (k % 4 && k < (int)sizeof line - 1); }
            else if (k < (int)sizeof line - 1) line[k++] = *p;
            if (k >= (int)sizeof line - 1) k = sizeof line - 2;
        }
        line[k] = 0;
        if (*p == '\r') p++;
        if (*p == '\n') p++;
        pr("%s", line);
        lines++;
    }
    if (lines > 16) pr("(%d lines: PgUp/PgDn scroll)", lines);
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
        if (*p == '"') {                                              /* "a long name" */
            argv[argc++] = ++p;
            while (*p && *p != '"') p++;
            if (*p) *p++ = 0;
            continue;
        }
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
    }
    if (argc) {
        const char *c = argv[0];
        if (is(c, "help") || is(c, "?") || is(c, "h")) help();
        else if (is(c, "status") || is(c, "st")) { show_cd(); show_fd(); show_hd(); }
        else if (is(c, "cd") || is(c, "chdir")) {
            char all[IN_MAX + 1] = "";                                  /* cd my games: the words are one name */
            for (int i = 1, k = 0; i < argc; i++) k += snprintf(all + k, sizeof all - k, i > 1 ? " %s" : "%s", argv[i]);
            do_chdir(argc > 1 ? all : 0);
        }
        else if ((c[0] | 32) == 'c' && (c[1] | 32) == 'd' && (c[2] == '.' || c[2] == '\\')) do_chdir(c + 2);   /* cd.. cd\ */
        else if (is(c, "cdrom") || is(c, "vmcd")) do_cdrom(argc, argv);
        else if (is(c, "fd") || is(c, "vmfd") || is(c, "floppy")) do_fd(argc, argv);
        else if (is(c, "hd") || is(c, "vmhd")) do_hd(argc, argv);
        else if (is(c, "dir") || is(c, "ls")) do_dir(argc, argv);
        else if (letter(c) >= 0 && c[1] == ':' && argc == 1) do_drive(c[0]);
        else if (is(c, "rescan")) do_rescan();
        else if (is(c, "lspci")) do_sys(0, argc, argv);
        else if (is(c, "lsusb")) do_sys(1, argc, argv);
        else if (is(c, "cls") || is(c, "clear")) cls();
        else if (is(c, "exit") || is(c, "close") || is(c, "quit")) on = 0;
        else pr("Unknown command %s (type help).", c);
    }
    cmd_req = 0;
}
