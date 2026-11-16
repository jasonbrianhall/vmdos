/*
 * dos-term.c: the terminal for ssh on DOS (vmdos). DOS has no terminal to
 * speak of (ANSI.SYS at best), so ssh's console reads and writes come here
 * (dos-compat.h renames read, write and ioctl):
 *
 *  - output: a Linux-console / xterm subset written straight to the text
 *    screen: cursor movement, erase, insert/delete characters and lines,
 *    scroll regions, tabs, 16 colors (256 and RGB colors mapped to them;
 *    8 background colors), reverse, DEC line
 *    drawing, the alternate screen, UTF-8 shown in code page 437, and
 *    answers to device status / attribute queries;
 *  - input: BIOS keys turned into the sequences TERM says (Linux console
 *    by default, xterm when TERM is xterm*), cursor keys in normal or
 *    application mode, Alt as ESC, Backspace as DEL;
 *  - TIOCGWINSZ: the screen's rows and columns.
 *
 * ssh sets TERM=linux on the remote side unless TERM is set in DOS.
 */
#include <sys/types.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <dpmi.h>
#include <go32.h>
#include <pc.h>
#include <sys/farptr.h>
#include <sys/fsext.h>
#include <io.h>
#include <fcntl.h>

#undef read
#undef write
#undef ioctl
#undef isatty
#undef close

#define MAXP 16

/*
 * The console, by DOS's device bits (isatty() isn't enough: Watt-32's
 * sockets are NUL device handles, which DOS calls character devices too).
 */
static int
is_con(int fd, int bit)
{
	int info;

	if (fd < 0 || __FSEXT_get_function(fd) != NULL)
		return 0;
	info = _get_dev_info(fd);
	return info != -1 && (info & 0x80) && (info & bit);
}
#define CON_IN(fd)  is_con(fd, 0x01)
#define CON_OUT(fd) is_con(fd, 0x02)

static int inited, rows, cols;
static int cx, cy;			/* cursor */
static int wrap_pending;		/* written to the last column */
static int top, bot;			/* scroll region, inclusive */
static int fg, bg, bold, rev, invis, dim;
static int attr;			/* VGA attribute in use */
static int autowrap = 1, insert, origin, ckm, cur_visible = 1, lnm;
static int g0_dec, g1_dec, shift_out;
static unsigned char tabs[256];
static int sv_x, sv_y, sv_fg, sv_bg, sv_bold, sv_rev, sv_g0, sv_origin;
static unsigned short *alt_save;	/* the main screen while on the alternate */
static int alt_x, alt_y;
static int state;			/* parser */
static int par[MAXP], npar, priv, inter;
static unsigned utf_cp, utf_left;
static int xterm_keys;
static int raw_in, opost = 1;
static unsigned char inq[256];		/* input queued: replies, multi-byte keys */
static int inq_len;
static int orig_cursor_shape = 0x0607;
static int last_ch = ' ';

enum { S_GROUND, S_ESC, S_CSI, S_OSC, S_OSC_ESC, S_CHARSET, S_STR, S_STR_ESC, S_HASH };

/* ANSI color number to VGA. */
static const unsigned char ansi2vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

/* Code page 437's characters 128-255 in Unicode, to show UTF-8. */
static const unsigned short cp437[128] = {
	0x00c7,0x00fc,0x00e9,0x00e2,0x00e4,0x00e0,0x00e5,0x00e7,0x00ea,0x00eb,0x00e8,0x00ef,0x00ee,0x00ec,0x00c4,0x00c5,
	0x00c9,0x00e6,0x00c6,0x00f4,0x00f6,0x00f2,0x00fb,0x00f9,0x00ff,0x00d6,0x00dc,0x00a2,0x00a3,0x00a5,0x20a7,0x0192,
	0x00e1,0x00ed,0x00f3,0x00fa,0x00f1,0x00d1,0x00aa,0x00ba,0x00bf,0x2310,0x00ac,0x00bd,0x00bc,0x00a1,0x00ab,0x00bb,
	0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255d,0x255c,0x255b,0x2510,
	0x2514,0x2534,0x252c,0x251c,0x2500,0x253c,0x255e,0x255f,0x255a,0x2554,0x2569,0x2566,0x2560,0x2550,0x256c,0x2567,
	0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256b,0x256a,0x2518,0x250c,0x2588,0x2584,0x258c,0x2590,0x2580,
	0x03b1,0x00df,0x0393,0x03c0,0x03a3,0x03c3,0x00b5,0x03c4,0x03a6,0x0398,0x03a9,0x03b4,0x221e,0x03c6,0x03b5,0x2229,
	0x2261,0x00b1,0x2265,0x2264,0x2320,0x2321,0x00f7,0x2248,0x00b0,0x2219,0x00b7,0x221a,0x207f,0x00b2,0x25a0,0x00a0,
};

/* Other characters servers send often, to their nearest in code page 437. */
static const struct { unsigned short u; unsigned char c; } extra[] = {
	{0x2018,'\''},{0x2019,'\''},{0x201a,','},{0x201c,'"'},{0x201d,'"'},{0x201e,'"'},
	{0x2013,'-'},{0x2014,'-'},{0x2212,'-'},{0x2026,'.'},{0x2022,0xf9},{0x25cf,0xf9},
	{0x2190,0x1b},{0x2192,0x1a},{0x2191,0x18},{0x2193,0x19},{0x2194,0x1d},{0x2195,0x12},
	{0x25b2,0x1e},{0x25bc,0x1f},{0x25ba,0x10},{0x25b6,0x10},{0x25c4,0x11},{0x25c0,0x11},
	{0x2713,0xfb},{0x2714,0xfb},{0x2717,'x'},{0x2718,'x'},{0x00d7,'x'},{0x2302,0x7f},
	{0x256d,0xda},{0x256e,0xbf},{0x256f,0xd9},{0x2570,0xc0},	/* rounded corners */
	{0x2501,0xc4},{0x2503,0xb3},{0x250f,0xda},{0x2513,0xbf},{0x2517,0xc0},{0x251b,0xd9},
	{0x2523,0xc3},{0x252b,0xb4},{0x2533,0xc2},{0x253b,0xc1},{0x254b,0xc5},
	{0x2574,0xc4},{0x2576,0xc4},{0x2575,0xb3},{0x2577,0xb3},{0x2578,0xc4},{0x257a,0xc4},
	{0x2581,0xdc},{0x2582,0xdc},{0x2583,0xdc},{0x2585,0xdc},{0x2586,0xdb},{0x2587,0xdb},
	{0x2589,0xdb},{0x258a,0xdb},{0x258b,0xdd},{0x258d,0xdd},{0x258e,0xdd},{0x258f,0xdd},
	{0x2594,0xdf},{0x2595,0xde},{0x25a1,0xfe},{0x25aa,0xfe},{0x25ab,0xfe},{0x00a6,0xb3},
	{0x00a9,'c'},{0x00ae,'r'},{0x2122,'t'},{0x20ac,'E'},{0x00a8,'"'},{0x00b4,'\''},
	{0x00c0,'A'},{0x00c1,'A'},{0x00c2,'A'},{0x00c3,'A'},{0x00c8,'E'},{0x00ca,'E'},{0x00cb,'E'},
	{0x00cc,'I'},{0x00cd,'I'},{0x00ce,'I'},{0x00cf,'I'},{0x00d2,'O'},{0x00d3,'O'},{0x00d4,'O'},
	{0x00d5,'O'},{0x00d8,'O'},{0x00d9,'U'},{0x00da,'U'},{0x00db,'U'},{0x00dd,'Y'},{0x00e3,'a'},
	{0x00f5,'o'},{0x00f8,'o'},{0x00fd,'y'},{0x0153,'o'},{0x0152,'O'},{0x00a0,' '},{0x200b,0},
	{0xfeff,0},{0x00b9,'1'},{0x00b3,'3'},
};

/* DEC special graphics ('0' set) for 0x5f-0x7e, in code page 437. */
static const unsigned char decgfx[32] = {
	' ', 0x04, 0xb1, 0x1a, 0x17, 0x1b, 0x19, 0xf8, 0xf1, 0x15, 0x12, 0xd9, 0xbf, 0xda, 0xc0, 0xc5,
	0xc4, 0xc4, 0xc4, 0xc4, 0xc4, 0xc3, 0xb4, 0xc1, 0xc2, 0xb3, 0xf3, 0xf2, 0xe3, 0xd8, 0x9c, 0xfa,
};

static unsigned char
uni2cp437(unsigned u)
{
	size_t i;

	if (u < 0x80)
		return u;
	for (i = 0; i < 128; i++)
		if (cp437[i] == u)
			return 128 + i;
	for (i = 0; i < sizeof extra / sizeof extra[0]; i++)
		if (extra[i].u == u)
			return extra[i].c;
	if (u >= 0x2500 && u <= 0x257f)		/* other box drawing */
		return 0xc5;
	if (u >= 0x2800 && u <= 0x28ff)		/* braille: dots */
		return u == 0x2800 ? ' ' : 0xb0;
	return '?';
}

static void
queue_input(const char *s, int n)
{
	if (n > (int)sizeof inq - inq_len)
		n = sizeof inq - inq_len;
	memcpy(inq + inq_len, s, n);
	inq_len += n;
}

static void
queue_str(const char *s)
{
	queue_input(s, strlen(s));
}

/* --- the screen --- */

static inline unsigned long
cell_addr(int y, int x)
{
	return ScreenPrimary + 2 * (y * cols + x);
}

static inline unsigned short
blank(void)
{
	return (unsigned short)((attr & 0xf0) | (attr & 0x0f)) << 8 | ' ';
}

static void
fill(int y, int x0, int x1, unsigned short cell)
{
	int x;

	for (x = x0; x < x1; x++)
		_farpokew(_dos_ds, cell_addr(y, x), cell);
}

static void
copy_line(int dst, int src)
{
	unsigned short buf[256];

	dosmemget(cell_addr(src, 0), cols * 2, buf);
	dosmemput(buf, cols * 2, cell_addr(dst, 0));
}

/* Scroll lines t..b up by n, clearing at the bottom. */
static void
scroll_up(int t, int b, int n)
{
	int y;

	if (n > b - t + 1)
		n = b - t + 1;
	for (y = t; y + n <= b; y++)
		copy_line(y, y + n);
	for (y = b - n + 1; y <= b; y++)
		fill(y, 0, cols, blank());
}

static void
scroll_down(int t, int b, int n)
{
	int y;

	if (n > b - t + 1)
		n = b - t + 1;
	for (y = b; y - n >= t; y--)
		copy_line(y, y - n);
	for (y = t; y < t + n; y++)
		fill(y, 0, cols, blank());
}

static void
set_cursor_shape(int visible)
{
	__dpmi_regs r;

	memset(&r, 0, sizeof r);
	r.h.ah = 0x01;
	r.x.cx = visible ? orig_cursor_shape : 0x2000;
	__dpmi_int(0x10, &r);
}

static void
compute_attr(void)
{
	int f = fg, b = bg;

	if (bold && f < 8)
		f |= 8;
	if (dim && f >= 8)
		f &= 7;
	if (rev) {
		int t = f;

		f = b;
		b = t;
	}
	if (invis)
		f = b;
	/* bright backgrounds would blink: VGA's default, left alone */
	attr = (b & 0x07) << 4 | (f & 0x0f);
}

static void
reset_attrs(void)
{
	fg = 7;
	bg = 0;
	bold = rev = invis = dim = 0;
	compute_attr();
}

static void term_restore(void);

static void
term_init(void)
{
	__dpmi_regs r;
	const char *t;
	int i;

	if (inited)
		return;
	inited = 1;
	rows = ScreenRows();
	cols = ScreenCols();
	if (cols > 256)
		cols = 256;
	ScreenGetCursor(&cy, &cx);
	top = 0;
	bot = rows - 1;
	for (i = 0; i < 256; i++)
		tabs[i] = (i % 8) == 0;
	memset(&r, 0, sizeof r);
	r.h.ah = 0x03;		/* the cursor's shape, to put back */
	__dpmi_int(0x10, &r);
	if (r.x.cx != 0 && !(r.x.cx & 0x2000))
		orig_cursor_shape = r.x.cx;
	reset_attrs();
	t = getenv("TERM");
	xterm_keys = t != NULL && (strncmp(t, "xterm", 5) == 0 ||
	    strncmp(t, "vt", 2) == 0 || strncmp(t, "screen", 6) == 0 ||
	    strncmp(t, "tmux", 4) == 0);
	atexit(term_restore);
}

/* At exit: the main screen back if on the alternate, the cursor shown.
   (Others may have written since: the cursor stays where it is.) */
static void
term_restore(void)
{
	if (alt_save != NULL) {
		dosmemput(alt_save, rows * cols * 2, ScreenPrimary);
		free(alt_save);
		alt_save = NULL;
		ScreenSetCursor(alt_y, alt_x);
	}
	if (!cur_visible)
		set_cursor_shape(1);
}

static void
clamp(void)
{
	int t = origin ? top : 0, b = origin ? bot : rows - 1;

	if (cx < 0) cx = 0;
	if (cx >= cols) cx = cols - 1;
	if (cy < t) cy = t;
	if (cy > b) cy = b;
}

static void
index_down(void)
{
	if (cy == bot)
		scroll_up(top, bot, 1);
	else if (cy < rows - 1)
		cy++;
}

static void
put_char(unsigned char c)
{
	if (wrap_pending) {
		wrap_pending = 0;
		if (autowrap) {
			cx = 0;
			index_down();
		}
	}
	if (insert) {
		int x;

		for (x = cols - 1; x > cx; x--)
			_farpokew(_dos_ds, cell_addr(cy, x),
			    _farpeekw(_dos_ds, cell_addr(cy, x - 1)));
	}
	_farpokew(_dos_ds, cell_addr(cy, cx), (unsigned short)attr << 8 | c);
	last_ch = c;
	if (cx == cols - 1)
		wrap_pending = 1;
	else
		cx++;
}

static void
put_glyph(unsigned u)
{
	unsigned char c;
	int dec = shift_out ? g1_dec : g0_dec;

	if (dec && u >= 0x5f && u <= 0x7e)
		c = decgfx[u - 0x5f];
	else
		c = uni2cp437(u);
	if (u >= 0x80 && c == 0)
		return;			/* zero width */
	put_char(c);
}

static void
erase_display(int mode)
{
	int y;

	if (mode == 0) {
		fill(cy, cx, cols, blank());
		for (y = cy + 1; y < rows; y++)
			fill(y, 0, cols, blank());
	} else if (mode == 1) {
		for (y = 0; y < cy; y++)
			fill(y, 0, cols, blank());
		fill(cy, 0, cx + 1, blank());
	} else if (mode == 2 || mode == 3) {
		for (y = 0; y < rows; y++)
			fill(y, 0, cols, blank());
	}
}

static void
erase_line(int mode)
{
	if (mode == 0)
		fill(cy, cx, cols, blank());
	else if (mode == 1)
		fill(cy, 0, cx + 1, blank());
	else
		fill(cy, 0, cols, blank());
}

static void
save_cursor(void)
{
	sv_x = cx; sv_y = cy; sv_fg = fg; sv_bg = bg; sv_bold = bold;
	sv_rev = rev; sv_g0 = g0_dec; sv_origin = origin;
}

static void
restore_cursor(void)
{
	cx = sv_x; cy = sv_y; fg = sv_fg; bg = sv_bg; bold = sv_bold;
	rev = sv_rev; g0_dec = sv_g0; origin = sv_origin;
	wrap_pending = 0;
	compute_attr();
	clamp();
}

static void
alt_screen(int on, int save)
{
	if (on && alt_save == NULL) {
		alt_save = malloc(rows * cols * 2);
		if (alt_save == NULL)
			return;
		dosmemget(ScreenPrimary, rows * cols * 2, alt_save);
		alt_x = cx;
		alt_y = cy;
		if (save)
			save_cursor();
		erase_display(2);
	} else if (!on && alt_save != NULL) {
		dosmemput(alt_save, rows * cols * 2, ScreenPrimary);
		free(alt_save);
		alt_save = NULL;
		cx = alt_x;
		cy = alt_y;
		if (save)
			restore_cursor();
	}
}

/* xterm 256 colors and RGB to the nearest of the 16. */
static int
rgb16(int r, int g, int b)
{
	static const unsigned char pal[16][3] = {
		{0,0,0},{0,0,170},{0,170,0},{0,170,170},{170,0,0},{170,0,170},{170,85,0},{170,170,170},
		{85,85,85},{85,85,255},{85,255,85},{85,255,255},{255,85,85},{255,85,255},{255,255,85},{255,255,255}};
	int i, best = 0, bd = 1 << 30;

	for (i = 0; i < 16; i++) {
		int dr = r - pal[i][0], dg = g - pal[i][1], db = b - pal[i][2];
		int d = dr * dr + dg * dg + db * db;

		if (d < bd) {
			bd = d;
			best = i;
		}
	}
	return best;
}

static int
color256(int n)
{
	static const unsigned char lv[6] = { 0, 95, 135, 175, 215, 255 };

	if (n < 8)
		return ansi2vga[n];
	if (n < 16)
		return ansi2vga[n - 8] | 8;
	if (n < 232) {
		n -= 16;
		return rgb16(lv[n / 36], lv[(n / 6) % 6], lv[n % 6]);
	}
	n = 8 + (n - 232) * 10;
	return rgb16(n, n, n);
}

static void
sgr(void)
{
	int i;

	if (npar == 0)
		par[npar++] = 0;
	for (i = 0; i < npar; i++) {
		int p = par[i];

		if (p == 0) reset_attrs();
		else if (p == 1) bold = 1;
		else if (p == 2) dim = 1;
		else if (p == 7) rev = 1;
		else if (p == 8) invis = 1;
		else if (p == 21 || p == 22) bold = dim = 0;
		else if (p == 27) rev = 0;
		else if (p == 28) invis = 0;
		else if (p >= 30 && p <= 37) fg = ansi2vga[p - 30];
		else if (p == 39) fg = 7;
		else if (p >= 40 && p <= 47) bg = ansi2vga[p - 40];
		else if (p == 49) bg = 0;
		else if (p >= 90 && p <= 97) fg = ansi2vga[p - 90] | 8;
		else if (p >= 100 && p <= 107) bg = ansi2vga[p - 100] | 8;
		else if ((p == 38 || p == 48) && i + 1 < npar) {
			int c = -1;

			if (par[i + 1] == 5 && i + 2 < npar) {
				c = color256(par[i + 2]);
				i += 2;
			} else if (par[i + 1] == 2 && i + 4 < npar) {
				c = rgb16(par[i + 2], par[i + 3], par[i + 4]);
				i += 4;
			}
			if (c >= 0) {
				if (p == 38)
					fg = c;
				else
					bg = c;
			}
		}
		/* 3 italic, 4 underline, 5 blink, 9 strike: no way to show */
	}
	compute_attr();
}

static void
set_mode(int on)
{
	int i;

	for (i = 0; i < npar; i++) {
		int p = par[i];

		if (priv == '?') {
			switch (p) {
			case 1: ckm = on; break;
			case 6: origin = on; cx = 0; cy = on ? top : 0; break;
			case 7: autowrap = on; break;
			case 25: cur_visible = on; set_cursor_shape(on); break;
			case 47: case 1047: alt_screen(on, 0); break;
			case 1048: if (on) save_cursor(); else restore_cursor(); break;
			case 1049: alt_screen(on, 1); break;
			}
		} else if (priv == 0) {
			if (p == 4)
				insert = on;
			else if (p == 20)
				lnm = on;
		}
	}
}

#define P(i, d) ((npar > (i) && par[i] > 0) ? par[i] : (d))

static void
csi(int f)
{
	char buf[32];
	int n = P(0, 1), x, y;

	if (priv == '>' || priv == '=') {	/* secondary DA etc. */
		if (f == 'c' && priv == '>')
			queue_str("\033[>0;0;0c");
		return;
	}
	if (inter != 0 && f != 'q' && f != 'p')
		return;
	if (f != 'b')
		wrap_pending = 0;
	switch (f) {
	case '@':
		for (x = cols - 1; x >= cx + n; x--)
			_farpokew(_dos_ds, cell_addr(cy, x),
			    _farpeekw(_dos_ds, cell_addr(cy, x - n)));
		fill(cy, cx, cx + n < cols ? cx + n : cols, blank());
		break;
	case 'A': cy -= n; if (cy < top && cy + n >= top) cy = top; clamp(); break;
	case 'B': case 'e':
		cy += n; if (cy > bot && cy - n <= bot) cy = bot; clamp(); break;
	case 'C': case 'a': cx += n; clamp(); break;
	case 'D': cx -= n; clamp(); break;
	case 'E': cy += n; cx = 0; if (cy > bot) cy = bot; clamp(); break;
	case 'F': cy -= n; cx = 0; if (cy < top) cy = top; clamp(); break;
	case 'G': case '`': cx = n - 1; clamp(); break;
	case 'H': case 'f':
		cy = P(0, 1) - 1 + (origin ? top : 0);
		cx = P(1, 1) - 1;
		clamp();
		break;
	case 'I':
		while (n-- > 0) {
			do cx++; while (cx < cols - 1 && !tabs[cx]);
		}
		clamp();
		break;
	case 'J': erase_display(npar ? par[0] : 0); break;
	case 'K': erase_line(npar ? par[0] : 0); break;
	case 'L':
		if (cy >= top && cy <= bot)
			scroll_down(cy, bot, n);
		cx = 0;
		break;
	case 'M':
		if (cy >= top && cy <= bot)
			scroll_up(cy, bot, n);
		cx = 0;
		break;
	case 'P':
		if (n > cols - cx)
			n = cols - cx;
		for (x = cx; x + n < cols; x++)
			_farpokew(_dos_ds, cell_addr(cy, x),
			    _farpeekw(_dos_ds, cell_addr(cy, x + n)));
		fill(cy, cols - n, cols, blank());
		break;
	case 'S': scroll_up(top, bot, n); break;
	case 'T': if (priv == 0 && npar <= 1) scroll_down(top, bot, n); break;
	case 'X':
		fill(cy, cx, cx + n < cols ? cx + n : cols, blank());
		break;
	case 'Z':
		while (n-- > 0) {
			do cx--; while (cx > 0 && !tabs[cx]);
		}
		clamp();
		break;
	case 'b':
		while (n-- > 0)
			put_char(last_ch);
		break;
	case 'c':
		/* ESC[?Nc is the Linux console's cursor shape: no answer */
		if (priv == 0 && P(0, 0) == 0)
			queue_str("\033[?6c");	/* a VT102 */
		break;
	case 'd':
		cy = n - 1 + (origin ? top : 0);
		clamp();
		break;
	case 'g':
		if (P(0, 0) == 0)
			tabs[cx] = 0;
		else if (par[0] == 3)
			memset(tabs, 0, sizeof tabs);
		break;
	case 'h': set_mode(1); break;
	case 'l': set_mode(0); break;
	case 'm': if (priv == 0) sgr(); break;
	case 'n':
		if (P(0, 0) == 5)
			queue_str("\033[0n");
		else if (P(0, 0) == 6) {
			y = cy + 1 - (origin ? top : 0);
			snprintf(buf, sizeof buf, "\033[%s%d;%dR",
			    priv == '?' ? "?" : "", y, cx + 1);
			queue_str(buf);
		}
		break;
	case 'r':
		if (priv)
			break;
		y = P(0, 1) - 1;
		x = P(1, rows) - 1;
		if (x >= rows)
			x = rows - 1;
		if (y < x) {
			top = y;
			bot = x;
		}
		cx = 0;
		cy = origin ? top : 0;
		break;
	case 's': if (priv == 0) save_cursor(); break;
	case 'u': if (priv == 0) restore_cursor(); break;
	}
}

static void
reset_terminal(void)
{
	top = 0;
	bot = rows - 1;
	cx = cy = 0;
	autowrap = 1;
	insert = origin = ckm = lnm = 0;
	g0_dec = g1_dec = shift_out = 0;
	reset_attrs();
	set_cursor_shape(cur_visible = 1);
	erase_display(2);
}

static void
esc(int c)
{
	state = S_GROUND;
	switch (c) {
	case '[': state = S_CSI; npar = 0; priv = 0; inter = 0;
		memset(par, 0, sizeof par); break;
	case ']': state = S_OSC; break;
	case 'P': case '_': case '^': case 'X': state = S_STR; break;
	case '(': case ')': case '*': case '+': inter = c; state = S_CHARSET; break;
	case '#': state = S_HASH; break;
	case '7': save_cursor(); break;
	case '8': restore_cursor(); break;
	case 'D': wrap_pending = 0; index_down(); break;
	case 'E': wrap_pending = 0; cx = 0; index_down(); break;
	case 'M':
		wrap_pending = 0;
		if (cy == top)
			scroll_down(top, bot, 1);
		else if (cy > 0)
			cy--;
		break;
	case 'H': tabs[cx] = 1; break;
	case 'c': reset_terminal(); break;
	case 'Z': queue_str("\033[?6c"); break;
	/* = > keypad modes, \ string end: nothing to do */
	}
}

static void
term_byte(unsigned char c)
{
	/* Controls act anywhere, except inside strings. */
	if (c < 0x20 && state != S_OSC && state != S_STR && state != S_OSC_ESC &&
	    state != S_STR_ESC) {
		switch (c) {
		case 0x07: return;			/* BEL: quiet */
		case 0x08:
			if (wrap_pending)
				wrap_pending = 0;
			else if (cx > 0)
				cx--;
			return;
		case 0x09:
			wrap_pending = 0;
			do cx++; while (cx < cols - 1 && !tabs[cx]);
			if (cx >= cols)
				cx = cols - 1;
			return;
		case 0x0a: case 0x0b: case 0x0c:
			wrap_pending = 0;
			if (lnm)
				cx = 0;
			index_down();
			return;
		case 0x0d: wrap_pending = 0; cx = 0; return;
		case 0x0e: shift_out = 1; return;
		case 0x0f: shift_out = 0; return;
		case 0x18: case 0x1a: state = S_GROUND; return;
		case 0x1b: state = S_ESC; utf_left = 0; return;
		default: return;
		}
	}
	switch (state) {
	case S_GROUND:
		if (utf_left) {
			if ((c & 0xc0) == 0x80) {
				utf_cp = utf_cp << 6 | (c & 0x3f);
				if (--utf_left == 0)
					put_glyph(utf_cp);
				return;
			}
			utf_left = 0;
			put_glyph(0xfffd);
		}
		if (c < 0x80) {
			if (c != 0x7f)
				put_glyph(c);
		} else if ((c & 0xe0) == 0xc0) {
			utf_cp = c & 0x1f; utf_left = 1;
		} else if ((c & 0xf0) == 0xe0) {
			utf_cp = c & 0x0f; utf_left = 2;
		} else if ((c & 0xf8) == 0xf0) {
			utf_cp = c & 0x07; utf_left = 3;
		} else
			put_glyph(0xfffd);
		break;
	case S_ESC:
		esc(c);
		break;
	case S_CSI:
		if (c >= '0' && c <= '9') {
			if (npar == 0)
				npar = 1;
			if (par[npar - 1] < 10000)
				par[npar - 1] = par[npar - 1] * 10 + (c - '0');
		} else if (c == ';' || c == ':') {
			if (npar == 0)
				npar = 1;
			if (npar < MAXP)
				npar++;
		} else if (c >= '<' && c <= '?') {
			priv = c;
		} else if (c >= 0x20 && c <= 0x2f) {
			inter = c;
		} else if (c >= 0x40 && c <= 0x7e) {
			state = S_GROUND;
			csi(c);
		} else
			state = S_GROUND;
		break;
	case S_CHARSET:
		state = S_GROUND;
		if (inter == '(')
			g0_dec = c == '0';
		else if (inter == ')')
			g1_dec = c == '0';
		break;
	case S_HASH:
		state = S_GROUND;
		break;
	case S_OSC:
		if (c == 0x07)
			state = S_GROUND;
		else if (c == 0x1b)
			state = S_OSC_ESC;
		break;
	case S_OSC_ESC:
		state = c == '\\' ? S_GROUND : S_OSC;
		break;
	case S_STR:
		if (c == 0x1b)
			state = S_STR_ESC;
		break;
	case S_STR_ESC:
		state = c == '\\' ? S_GROUND : S_STR;
		break;
	}
}

static int out_x = -1, out_y = -1;

static void
place_cursor(void)
{
	ScreenSetCursor(cy, cx);
	out_x = cx;
	out_y = cy;
}

/* --- what dos-compat.h sends here --- */

void
dos_term_mode(const struct termios *t)
{
	raw_in = !(t->c_lflag & ICANON);
	opost = (t->c_oflag & OPOST) != 0;
}

ssize_t
dos_write(int fd, const void *buf, size_t len)
{
	const unsigned char *p = buf;
	size_t i;
	int y, x;

	if (dos_is_pipe(fd))
		return dos_pipe_write(fd, buf, len);
	if (len == 0 || !CON_OUT(fd))
		return write(fd, buf, len);
	term_init();
	ScreenGetCursor(&y, &x);
	if (y != out_y || x != out_x) {
		cx = x;
		cy = y;
		wrap_pending = 0;
		clamp();
	}
	for (i = 0; i < len; i++) {
		if (p[i] == '\n' && opost)
			term_byte('\r');
		term_byte(p[i]);
	}
	place_cursor();
	return len;
}

/* A key from the BIOS (INT 16h AH=11h/10h), or -1. */
static int
bios_key(int wait)
{
	__dpmi_regs r;
	memset(&r, 0, sizeof r);
	if (!wait) {
		r.h.ah = 0x11;
		__dpmi_int(0x16, &r);
		if (r.x.flags & 0x40)	/* ZF: none */
			return -1;
	}
	memset(&r, 0, sizeof r);
	r.h.ah = 0x10;
	__dpmi_int(0x16, &r);
	return r.x.ax;
}

static void
key_seq(const char *linux_s, const char *xterm_s)
{
	queue_str(xterm_keys ? xterm_s : linux_s);
}

static void
cursor_key(char c)
{
	char s[4] = { 0x1b, ckm ? 'O' : '[', c, 0 };

	queue_str(s);
}

/* Alt+letter keys' scan codes, as letters. */
static const char alt_letters[] =
	"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0\0zxcvbnm";

static void
translate(int k)
{
	int scan = (k >> 8) & 0xff, ch = k & 0xff;
	char s[8];

	if (ch == 0xe0 && scan != 0)
		ch = 0;			/* the grey keys */
	if (ch != 0) {
		if (scan == 0x0e && ch == 0x08)
			ch = 0x7f;	/* Backspace */
		if (scan == 0x0e && ch == 0x7f)
			ch = 0x08;	/* Ctrl+Backspace */
		s[0] = ch;
		queue_input(s, 1);
		return;
	}
	switch (scan) {
	case 0x48: cursor_key('A'); return;
	case 0x50: cursor_key('B'); return;
	case 0x4d: cursor_key('C'); return;
	case 0x4b: cursor_key('D'); return;
	case 0x47: key_seq("\033[1~", ckm ? "\033OH" : "\033[H"); return;
	case 0x4f: key_seq("\033[4~", ckm ? "\033OF" : "\033[F"); return;
	case 0x52: queue_str("\033[2~"); return;
	case 0x53: queue_str("\033[3~"); return;
	case 0x49: queue_str("\033[5~"); return;
	case 0x51: queue_str("\033[6~"); return;
	case 0x3b: key_seq("\033[[A", "\033OP"); return;
	case 0x3c: key_seq("\033[[B", "\033OQ"); return;
	case 0x3d: key_seq("\033[[C", "\033OR"); return;
	case 0x3e: key_seq("\033[[D", "\033OS"); return;
	case 0x3f: key_seq("\033[[E", "\033[15~"); return;
	case 0x40: queue_str("\033[17~"); return;
	case 0x41: queue_str("\033[18~"); return;
	case 0x42: queue_str("\033[19~"); return;
	case 0x43: queue_str("\033[20~"); return;
	case 0x44: queue_str("\033[21~"); return;
	case 0x85: queue_str("\033[23~"); return;
	case 0x86: queue_str("\033[24~"); return;
	case 0x0f: queue_str("\033[Z"); return;			/* Shift+Tab */
	case 0x73: queue_str("\033[1;5D"); return;		/* Ctrl+arrows */
	case 0x74: queue_str("\033[1;5C"); return;
	case 0x8d: queue_str("\033[1;5A"); return;
	case 0x91: queue_str("\033[1;5B"); return;
	case 0x03: s[0] = 0; queue_input(s, 1); return;	/* Ctrl+@ */
	}
	if (scan < sizeof alt_letters && alt_letters[scan]) {
		s[0] = 0x1b;
		s[1] = alt_letters[scan];
		queue_input(s, 2);
	} else if (scan >= 0x78 && scan <= 0x81) {		/* Alt+1..0 */
		s[0] = 0x1b;
		s[1] = scan == 0x81 ? '0' : '1' + (scan - 0x78);
		queue_input(s, 2);
	} else if (scan == 0x01) {				/* Alt+Esc */
		queue_str("\033\033");
	} else if (scan == 0x0e) {				/* Alt+Backspace */
		queue_str("\033\177");
	} else if (scan == 0x1c) {				/* Alt+Enter */
		queue_str("\033\r");
	}
}

/*
 * Keys waiting? The BIOS keyboard buffer's head and tail (40:1A, 40:1C)
 * first: asking INT 16h on every turn of ssh's poll loop (through DPMI,
 * thousands of times a second) starves vmdos's network delivery. INT 16h
 * itself once a timer tick (55 ms, 40:6C) in case the buffer is elsewhere.
 * (Not uclock(): it reprograms the 8254.)
 */
static int
key_waiting(void)
{
	static unsigned long last;
	unsigned long now;

	if (_farpeekw(_dos_ds, 0x41a) != _farpeekw(_dos_ds, 0x41c))
		return 1;
	now = _farpeekl(_dos_ds, 0x46c);
	if (now == last)
		return 0;
	last = now;
	return -1;	/* ask the BIOS */
}

static void
pump_keys(void)
{
	int k, w;

	while (inq_len < (int)sizeof inq - 8 && (w = key_waiting()) != 0) {
		if ((k = bios_key(0)) == -1)
			break;
		translate(k);
		(void)w;
	}
}

/* Is fd the console's output? (Always writable.) */
int
dos_term_con_out(int fd)
{
	return CON_OUT(fd);
}

/* Is there something to read on the console? (raw mode) */
int
dos_term_readable(int fd)
{
	if (!raw_in || !CON_IN(fd))
		return -1;	/* ask select() */
	term_init();
	pump_keys();
	return inq_len > 0;
}

ssize_t
dos_read(int fd, void *buf, size_t len)
{
	int n;
	if (dos_is_pipe(fd))
		return dos_pipe_read(fd, buf, len);
	if (!raw_in || len == 0 || !CON_IN(fd))
		return read(fd, buf, len);
	term_init();
	pump_keys();
	if (inq_len == 0) {
		/* Blocking console read: wait for a key. */
		translate(bios_key(1));
	}
	n = inq_len < (int)len ? inq_len : (int)len;
	memcpy(buf, inq, n);
	memmove(inq, inq + n, inq_len - n);
	inq_len -= n;
	return n;
}

int
dos_ioctl(int fd, int cmd, ...)
{
	va_list ap;
	void *arg;

	va_start(ap, cmd);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (cmd == TIOCGWINSZ) {
		struct winsize *ws = arg;

		if (!CON_IN(fd) && !CON_OUT(fd)) {
			errno = ENOTTY;
			return -1;
		}
		memset(ws, 0, sizeof *ws);
		ws->ws_row = ScreenRows();
		ws->ws_col = ScreenCols();
		return 0;
	}
	return ioctl(fd, cmd, arg);
}
