// The remote DOS prompt: remote=PORT on the command line (remote=on: 23)
// has vmdos take telnet connections to DOS's IPv4 address there
// (tcp9p_listen) and show the DOS text screen in them, keys going back to
// DOS as if typed. One session at a time; remotepw=WORD asks for it first.
// Telnet is plain text on the network: a LAN, or a tunnel (ssh -L), not
// the internet.
//
// The screen goes as ANSI (colors, cursor) and UTF-8 (code page 437's
// glyphs), only the cells that changed, a few times a second; a graphics
// mode shows a note. Keys come back as scan codes into the keyboard queue
// (vkbd_inject), so programs with their own keyboard handler get them as
// well as DOS; the terminal's escape sequences are the arrows, Home/End,
// PgUp/PgDn, Ins/Del and F1-F12. Characters with no key (437's others) go
// into the BIOS buffer. It needs DOS's address: STARTNET (DHCP) or VMTCP
// before, which vmdos sees go by; nothing stays resident in DOS.
#include "hw.hpp"
#include "tcp9p.hpp"

extern "C" int bios_kbuf_put(u16 w);
extern "C" void video_osd(const char* msg);
extern "C" int net_guest_addr(uint8_t* mac, uint8_t* ip, int* prefix);

namespace {

int port = -1;                                  // -1: not read yet, 0: off
char pw[32];
int sess = -1;
bool authed, closing;
u32 closing_since;
int tries;
char pwbuf[32];
int pwn;

const int MAXC = 240 * 75;                      // video.c's TEXT_CELLS
u16 cur[MAXC], sent[MAXC];
int s_cols, s_rows, s_crow = -2, s_ccol, s_mode;    // what the terminal shows (s_mode 0: text, 1: the graphics note)
bool dirty_all = true;
u32 last_draw;

// ---------------- out ----------------
char ob[16384];
int on_;
void out(const char* s, int n) { if (on_ + n > (int)sizeof ob) return; memcpy(ob + on_, s, n); on_ += n; }
void outs(const char* s) { out(s, (int)strlen(s)); }
void outf(const char* fmt, ...)
{
    char b[64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > 0) out(b, n < (int)sizeof b ? n : (int)sizeof b - 1);
}
bool flush()
{
    if (!on_) return true;
    bool ok = tcp9p_send(sess, (const u8*)ob, (u32)on_);
    on_ = 0;
    return ok;
}

void utf8(u32 u)
{
    char b[3];
    if (u < 0x80) { b[0] = (char)u; out(b, 1); }
    else if (u < 0x800) { b[0] = (char)(0xC0 | u >> 6); b[1] = (char)(0x80 | (u & 0x3F)); out(b, 2); }
    else { b[0] = (char)(0xE0 | u >> 12); b[1] = (char)(0x80 | (u >> 6 & 0x3F)); b[2] = (char)(0x80 | (u & 0x3F)); out(b, 3); }
}

const u8 ansi[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };  // VGA's color order to ANSI's
int last_attr = -1;
void attr(u8 a)
{
    if (a == last_attr) return;
    last_attr = a;
    int fg = a & 15, bg = a >> 4 & 7;
    outf("\x1b[0;%d;%dm", fg & 8 ? 90 + ansi[fg & 7] : 30 + ansi[fg], 40 + ansi[bg]);
}

// The screen: what changed since the last time (all of it after dirty_all).
void draw()
{
    int C, R, crow, ccol;
    if (!video_text_snapshot(cur, MAXC, &C, &R, &crow, &ccol)) {
        if (s_mode != 1) {
            outs("\x1b[0m\x1b[2J\x1b[H[vmdos: DOS is in a graphics mode; the prompt shows again in text mode]");
            s_mode = 1; last_attr = -1;
        }
        return;
    }
    if (s_mode != 0 || C != s_cols || R != s_rows) { dirty_all = true; s_mode = 0; s_cols = C; s_rows = R; }
    if (dirty_all) { outs("\x1b[0m\x1b[2J"); last_attr = -1; }
    bool moved = false;
    for (int r = 0; r < R; r++) {
        int c = 0;
        while (c < C) {
            int i = r * C + c;
            if (!dirty_all && cur[i] == sent[i]) { c++; continue; }
            outf("\x1b[%d;%dH", r + 1, c + 1);
            moved = true;
            while (c < C && (dirty_all || cur[r * C + c] != sent[r * C + c] ||
                             (c + 1 < C && cur[r * C + c + 1] != sent[r * C + c + 1]))) {   // short gaps: written through
                u16 v = cur[r * C + c];
                attr((u8)(v >> 8));
                utf8(cp437_uni((u8)v));
                sent[r * C + c] = v;
                c++;
                if (on_ > (int)sizeof ob - 64) { if (!flush()) { dirty_all = true; return; } }
            }
        }
    }
    dirty_all = false;
    if (moved || crow != s_crow || ccol != s_ccol) {
        if (crow < 0) outs("\x1b[?25l");
        else outf("\x1b[?25h\x1b[%d;%dH", crow + 1, ccol + 1);
        s_crow = crow; s_ccol = ccol;
    }
}

// ---------------- in: telnet, then keys ----------------
u8 kq[256];                                     // scan codes waiting for room in the keyboard queue
u8 kh, kt;                                      // (wrap at 256)
void sc(u8 s) { if ((u8)(kt + 1) != kh) kq[kt++] = s; }
void key(u8 make, bool e0 = false, bool shift = false, bool ctrl = false)
{
    if (ctrl) sc(0x1D);
    if (shift) sc(0x2A);
    if (e0) sc(0xE0);
    sc(make);
    if (e0) sc(0xE0);
    sc((u8)(make | 0x80));
    if (shift) sc(0xAA);
    if (ctrl) sc(0x9D);
}
u16 bios_q[64];                                 // characters with no key: into the BIOS buffer
int bh, bt;

void type_char(u8 c)
{
    if (c == '\r' || c == '\n') { key(0x1C); return; }
    if (c == '\t') { key(0x0F); return; }
    if (c == 8 || c == 0x7F) { key(0x0E); return; }
    if (c == 0x1B) { key(0x01); return; }
    if (c >= 1 && c <= 26) { int k = us_key((u8)(c + 0x60)); if (k >= 0) key((u8)(k & 0x7F), false, false, true); return; }
    int k = us_key(c);
    if (k >= 0) { key((u8)(k & 0x7F), false, (k & 0x80) != 0); return; }
    if (c >= 0x80 && (u8)(bt + 1) % 64 != bh) { bios_q[bt] = c; bt = (bt + 1) % 64; }
}

enum { T_DATA, T_IAC, T_OPT, T_SB, T_SB_IAC };
int tstate;
u8 esc[16];
int escn;
u32 esc_since;
bool cr_seen;
u8 utf[4];
int utfn, utfneed;

const u8 fkeys[] = { 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58 };

// An escape sequence, whole: its key.
void esc_key()
{
    int n = escn;
    escn = 0;
    if (n == 1) { key(0x01); return; }          // Esc alone
    u8 f = esc[n - 1];
    if (esc[1] == 'O' && n == 3) {
        if (f >= 'P' && f <= 'S') { key(fkeys[f - 'P']); return; }
    }
    if (esc[1] != '[' && esc[1] != 'O') { for (int i = 1; i < n; i++) type_char(esc[i]); return; }   // Alt+x: just x
    int p = 0;
    for (int i = 2; i < n - 1; i++) if (esc[i] >= '0' && esc[i] <= '9') p = p * 10 + (esc[i] - '0'); else break;
    switch (f) {
    case 'A': key(0x48, true); return;
    case 'B': key(0x50, true); return;
    case 'C': key(0x4D, true); return;
    case 'D': key(0x4B, true); return;
    case 'H': key(0x47, true); return;
    case 'F': key(0x4F, true); return;
    case 'Z': key(0x0F, false, true); return;   // Shift+Tab
    case '~':
        switch (p) {
        case 1: case 7: key(0x47, true); return;
        case 2: key(0x52, true); return;
        case 3: key(0x53, true); return;
        case 4: case 8: key(0x4F, true); return;
        case 5: key(0x49, true); return;
        case 6: key(0x51, true); return;
        case 11: case 12: case 13: case 14: case 15: key(fkeys[p - 11]); return;
        case 17: case 18: case 19: case 20: case 21: key(fkeys[p - 12]); return;
        case 23: case 24: key(fkeys[p - 13]); return;
        }
    }
}

void login_char(u8 c)
{
    if (c == '\r' || c == '\n') {
        if (!pwn && c == '\n') return;
        pwbuf[pwn] = 0;
        if ((int)strlen(pwbuf) == (int)strlen(pw) && !strncmp(pwbuf, pw, sizeof pw)) {
            authed = true;
            dirty_all = true; s_crow = -2; s_mode = -1;
            outs("\r\n");
        } else if (++tries >= 3) {
            outs("\r\nNo.\r\n");
            closing = true; closing_since = ticks;
        } else outs("\r\nWrong. Password: ");
        pwn = 0;
        return;
    }
    if ((c == 8 || c == 0x7F) && pwn) { pwn--; return; }
    if (c >= 0x20 && pwn < (int)sizeof pwbuf - 1) pwbuf[pwn++] = (char)c;
}

void data_byte(u8 c)
{
    if (!authed) { if (c) login_char(c); return; }
    if (cr_seen) { cr_seen = false; if (c == 0 || c == '\n') return; }   // CR NUL, CR LF: one Enter
    if (escn) {
        if (escn < (int)sizeof esc) esc[escn++] = c;
        bool done = escn == 2 ? (c != '[' && c != 'O') : (c >= 0x40 && c <= 0x7E);
        if (done || escn == (int)sizeof esc) esc_key();
        return;
    }
    if (c == 0x1B) { esc[0] = c; escn = 1; esc_since = ticks; return; }
    if (c == '\r') cr_seen = true;
    if (utfneed) {                              // a UTF-8 sequence: its 437 character
        utf[utfn++] = c;
        if (utfn < utfneed) return;
        u32 u = utfneed == 2 ? (u32)(utf[0] & 0x1F) << 6 | (utf[1] & 0x3F)
              : utfneed == 3 ? (u32)(utf[0] & 0x0F) << 12 | (u32)(utf[1] & 0x3F) << 6 | (utf[2] & 0x3F)
              : '?';
        utfneed = 0;
        type_char(uni_cp437(u));
        return;
    }
    if (c >= 0xC0) { utf[0] = c; utfn = 1; utfneed = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : 2; return; }
    if (c >= 0x80) return;
    if (c) type_char(c);
}

void input(const u8* p, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        u8 c = p[i];
        switch (tstate) {
        case T_DATA: if (c == 0xFF) tstate = T_IAC; else data_byte(c); break;
        case T_IAC:
            if (c == 0xFF) { data_byte(0xFF); tstate = T_DATA; }
            else if (c >= 0xFB && c <= 0xFE) tstate = T_OPT;
            else if (c == 0xFA) tstate = T_SB;
            else tstate = T_DATA;
            break;
        case T_OPT: tstate = T_DATA; break;     // WILL/WONT/DO/DONT x: we already said what we do
        case T_SB: if (c == 0xFF) tstate = T_SB_IAC; break;
        case T_SB_IAC: tstate = c == 0xF0 ? T_DATA : T_SB; break;
        }
    }
}

void feed_keys()
{
    // Paced by the BIOS's buffer (15 keys): a burst from the terminal (a
    // paste) would fill it faster than DOS reads. A program with its own
    // keyboard handler leaves it empty and gets them as fast as they come.
    u16 head = rd16(0x41A), tail = rd16(0x41C), size = (u16)(rd16(0x482) - rd16(0x480));
    int queued = size ? ((tail - head + size) % size) / 2 : 0;
    for (int n = 0; kh != kt && vkbd_free() > 0 && n < 8 && queued < 8; n++) {
        u8 b = kq[kh++];
        vkbd_inject(b);
        if (!(b & 0x80) && b != 0x1D && b != 0x2A) queued++;   // a key down: a key in the buffer soon
    }
    while (bh != bt && bios_kbuf_put(bios_q[bh])) bh = (bh + 1) % 64;
}

void end_session(const char* why)
{
    u8 ip[4];
    tcp9p_peer(sess, ip);
    char m[64];
    snprintf(m, sizeof m, "remote: %u.%u.%u.%u %s", ip[0], ip[1], ip[2], ip[3], why);
    kprintf("%s\n", m);
    video_osd(m);
    sess = -1;
}

} // namespace

extern "C" void remote_tick(void)
{
    if (port < 0) {
        const char* p = strstr(cmdline, "remote=");
        port = 0;
        if (p && (p == cmdline || p[-1] == ' ')) {
            p += 7;
            if (!strncmp(p, "on", 2)) port = 23;
            else while (*p >= '0' && *p <= '9') port = port * 10 + (*p++ - '0');
            if (port > 65535) port = 0;
        }
        const char* w = strstr(cmdline, "remotepw=");
        if (w) {
            w += 9;
            int n = 0;
            while (*w && *w != ' ' && n < (int)sizeof pw - 1) pw[n++] = *w++;
            pw[n] = 0;
        }
        if (port) {
            tcp9p_listen((u16)port);
            kprintf("remote: the DOS prompt on telnet port %d%s\n", port, pw[0] ? " (password)" : " (no password: anyone who can reach it)");
        }
    }
    if (!port) return;
    int a = tcp9p_accept();
    if (a >= 0) {
        if (sess >= 0) {
            static const char busy[] = "vmdos: another remote session has the prompt.\r\n";
            tcp9p_send(a, (const u8*)busy, sizeof busy - 1);
            tcp9p_service(a);
            tcp9p_close_fin(a);
        } else {
            sess = a;
            authed = !pw[0]; closing = false; tries = 0; pwn = 0;
            tstate = T_DATA; escn = 0; cr_seen = false; utfneed = 0; kh = kt = 0; bh = bt = 0;
            dirty_all = true; s_crow = -2; s_mode = -1; last_attr = -1; on_ = 0;
            static const u8 neg[] = { 0xFF, 0xFB, 0x01, 0xFF, 0xFB, 0x03, 0xFF, 0xFD, 0x03 };   // WILL ECHO, WILL SGA, DO SGA
            out((const char*)neg, sizeof neg);
            if (!authed) outs("vmdos remote prompt - password: ");
            flush();
            u8 ip[4];
            tcp9p_peer(sess, ip);
            char m[64];
            snprintf(m, sizeof m, "remote: %u.%u.%u.%u connected", ip[0], ip[1], ip[2], ip[3]);
            kprintf("%s\n", m);
            video_osd(m);
        }
    }
    if (sess < 0) return;
    if (!tcp9p_service(sess)) { tcp9p_close(sess); end_session("disconnected"); return; }
    if (closing) {
        if (!tcp9p_unacked(sess) || (u32)(ticks - closing_since) > 2000) { tcp9p_close_fin(sess); end_session("refused (password)"); }
        return;
    }
    u32 have;
    u8* d = tcp9p_data(sess, &have);
    if (have) { input(d, have); tcp9p_consume(sess, have); }
    if (escn == 1 && (u32)(ticks - esc_since) > 60) esc_key();   // Esc on its own
    feed_keys();
    if (authed && (u32)(ticks - last_draw) >= 40 && tcp9p_unacked(sess) < 32768) {
        last_draw = ticks;
        draw();
    }
    flush();
    tcp9p_service(sess);
}
