/* The clipboard: text copied off the screen (video.c: Ctrl+Shift+C, or
   Ctrl+Shift+drag) or taken from the host's (agent.cpp: the SPICE agent's
   clipboard, QEMU's qemu-vdagent or virt-manager's SPICE channel), and
   Ctrl+Shift+V typing it into DOS. Kept in code page 437, rows ending in
   '\n'; UTF-8 to and from the host.

   Pasting goes through the BIOS keyboard buffer (40:1Eh), as typed keys
   with their US scan codes, as fast as DOS takes them: anything that
   reads keys through INT 16h (COMMAND.COM, ssh, EDIT) gets them; a game
   with its own keyboard handler doesn't. It waits until Ctrl, Shift and
   Alt are up (the guest saw Ctrl+Shift go down), and any key typed stops
   it. */
#include "kernel.h"

#define CLIP_MAX 65536
static u8 clip[CLIP_MAX];
static u32 clip_len;
static u32 paste_pos, paste_len;                 /* what's left to type: clip[paste_pos..paste_len) */
static int pasting;
volatile int clip_grab_req;                       /* agent.cpp: tell the host we have text */

/* Code page 437's 80h-FFh in Unicode. */
static const u16 cp437_hi[128] = {
    0x00C7,0x00FC,0x00E9,0x00E2,0x00E4,0x00E0,0x00E5,0x00E7,0x00EA,0x00EB,0x00E8,0x00EF,0x00EE,0x00EC,0x00C4,0x00C5,
    0x00C9,0x00E6,0x00C6,0x00F4,0x00F6,0x00F2,0x00FB,0x00F9,0x00FF,0x00D6,0x00DC,0x00A2,0x00A3,0x00A5,0x20A7,0x0192,
    0x00E1,0x00ED,0x00F3,0x00FA,0x00F1,0x00D1,0x00AA,0x00BA,0x00BF,0x2310,0x00AC,0x00BD,0x00BC,0x00A1,0x00AB,0x00BB,
    0x2591,0x2592,0x2593,0x2502,0x2524,0x2561,0x2562,0x2556,0x2555,0x2563,0x2551,0x2557,0x255D,0x255C,0x255B,0x2510,
    0x2514,0x2534,0x252C,0x251C,0x2500,0x253C,0x255E,0x255F,0x255A,0x2554,0x2569,0x2566,0x2560,0x2550,0x256C,0x2567,
    0x2568,0x2564,0x2565,0x2559,0x2558,0x2552,0x2553,0x256B,0x256A,0x2518,0x250C,0x2588,0x2584,0x258C,0x2590,0x2580,
    0x03B1,0x00DF,0x0393,0x03C0,0x03A3,0x03C3,0x00B5,0x03C4,0x03A6,0x0398,0x03A9,0x03B4,0x221E,0x03C6,0x03B5,0x2229,
    0x2261,0x00B1,0x2265,0x2264,0x2320,0x2321,0x00F7,0x2248,0x00B0,0x2219,0x00B7,0x221A,0x207F,0x00B2,0x25A0,0x00A0,
};

static u8 uni_to_437(u32 u)
{
    if (u < 0x80) return (u8)u;
    for (int i = 0; i < 128; i++) if (cp437_hi[i] == u) return (u8)(0x80 + i);
    switch (u) {                                  /* what has no 437 glyph: the nearest */
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2022: return 0x07;
    case 0x00D7: return 'x';
    case 0x2026: return '.';
    }
    return '?';
}

/* Text copied off the screen (437, '\n' between rows). */
void clip_set(const u8 *s, u32 n)
{
    if (n > CLIP_MAX) n = CLIP_MAX;
    memcpy(clip, s, n);
    clip_len = n;
    clip_grab_req = 1;
    kprintf("clip: %u bytes copied\n", n);
}

/* The host's clipboard (UTF-8): kept for Ctrl+Shift+V. CR LF and CR are rows. */
void clip_from_host(const u8 *s, u32 n)
{
    u32 o = 0;
    for (u32 i = 0; i < n && o < CLIP_MAX; ) {
        u32 c = s[i++];
        if (c >= 0x80) {                           /* a UTF-8 sequence */
            int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
            c &= more == 3 ? 0x07 : more == 2 ? 0x0F : 0x1F;
            for (; more && i < n && (s[i] & 0xC0) == 0x80; more--) c = c << 6 | (s[i++] & 0x3F);
        }
        if (c == '\r') { if (i < n && s[i] == '\n') i++; c = '\n'; }
        if (!c) continue;
        clip[o++] = uni_to_437(c);
    }
    clip_len = o;
    dbg(1, "clip: %u bytes from the host\n", o);
}

/* The clipboard as UTF-8 for the host (agent.cpp): its length; at most max bytes into out. */
u32 clip_to_utf8(u8 *out, u32 max)
{
    u32 o = 0;
    for (u32 i = 0; i < clip_len; i++) {
        u8 c = clip[i];
        u32 u = c >= 0x80 ? cp437_hi[c - 0x80] : c;
        if (u < 0x80) { if (o + 1 > max) break; out[o++] = (u8)u; }
        else if (u < 0x800) { if (o + 2 > max) break; out[o++] = (u8)(0xC0 | u >> 6); out[o++] = (u8)(0x80 | (u & 0x3F)); }
        else { if (o + 3 > max) break; out[o++] = (u8)(0xE0 | u >> 12); out[o++] = (u8)(0x80 | (u >> 6 & 0x3F)); out[o++] = (u8)(0x80 | (u & 0x3F)); }
    }
    return o;
}

u32 clip_size(void) { return clip_len; }

/* ---------------- Ctrl+Shift+V: type it ---------------- */
/* US scan codes of ASCII 20h-7Fh (bit 7: with Shift). */
static const u8 us_scan[96] = {
    0x39,0x82,0xA8,0x84,0x85,0x86,0x88,0x28,0x8A,0x8B,0x89,0x8D,0x33,0x0C,0x34,0x35,   /*  !"#$%&'()*+,-./ */
    0x0B,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0xA7,0x27,0xB3,0x0D,0xB4,0xB5,   /* 0-9 :;<=>? */
    0x83,0x9E,0xB0,0xAE,0xA0,0x92,0xA1,0xA2,0xA3,0x97,0xA4,0xA5,0xA6,0xB2,0xB1,0x98,   /* @A-O */
    0x99,0x90,0x93,0x9F,0x94,0x96,0xAF,0x91,0xAD,0x95,0xAC,0x1A,0x2B,0x1B,0x87,0x8C,   /* P-Z [\]^_ */
    0x29,0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,   /* `a-o */
    0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C,0x9A,0xAB,0x9B,0xA9,0x0E,   /* p-z {|}~ DEL */
};

/* For remote.cpp: a character's Unicode (437's glyphs for 01h-1Fh too) and its US key. */
static const u16 cp437_lo[32] = {
    0x0020,0x263A,0x263B,0x2665,0x2666,0x2663,0x2660,0x2022,0x25D8,0x25CB,0x25D9,0x2642,0x2640,0x266A,0x266B,0x263C,
    0x25BA,0x25C4,0x2195,0x203C,0x00B6,0x00A7,0x25AC,0x21A8,0x2191,0x2193,0x2192,0x2190,0x221F,0x2194,0x25B2,0x25BC,
};
u32 cp437_uni(u8 c) { return c < 0x20 ? cp437_lo[c] : c == 0x7F ? 0x2302 : c >= 0x80 ? cp437_hi[c - 0x80] : c; }
u8 uni_cp437(u32 u) { return uni_to_437(u); }
int us_key(u8 c) { return c >= 0x20 && c < 0x80 ? us_scan[c - 0x20] : -1; }   /* scan code | 80h with Shift */

static u16 key_word(u8 c)
{
    if (c == '\n') return 0x1C0D;                 /* Enter */
    if (c == '\t') return 0x0F09;
    if (c == 0x1B) return 0x011B;
    if (c == 0x08) return 0x0E08;
    if (c >= 0x20 && c < 0x80) return (u16)((us_scan[c - 0x20] & 0x7F) << 8 | c);
    return c;                                      /* 437's others (and controls): no key, as Alt+keypad gives */
}

/* 1 if there's something to paste (the key was the paste's). */
int clip_paste(void)
{
    if (!clip_len) return 0;
    paste_pos = 0; paste_len = clip_len;
    while (paste_len && clip[paste_len - 1] == '\n') paste_len--;   /* no Enter of its own at the end */
    pasting = paste_len > 0;
    return 1;
}
void clip_paste_stop(void) { pasting = 0; }

int bios_kbuf_put(u16 w);
/* From the timer: the next keys, while the buffer has room. */
void clip_tick(void)
{
    if (!pasting) return;
    if (rd8(0x417) & 0x0F) return;                 /* Shift, Ctrl or Alt still down */
    for (int i = 0; i < 4 && paste_pos < paste_len; i++) {
        if (!bios_kbuf_put(key_word(clip[paste_pos]))) return;   /* full: on the next tick */
        paste_pos++;
    }
    if (paste_pos >= paste_len) pasting = 0;
}
