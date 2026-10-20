/* xBRZ (Zenju's pixel-art scaler, as in his xBRZ 1.8), 2x to 6x, for
   scale=xbrz (video.c). Integer only: the colour distance (YCbCr, BT.2020
   weights) is in 1/16 units with an integer square root, and the
   thresholds are scaled by 10. Distances between two colours are cached:
   a palette mode has few distinct pairs. Render helpers (smp.c) fill the
   cache at the same time, so an entry carries a check word: one read
   half from two writes fails it and is worked out again.

   Per source pixel F, the corner it shares with G, J and K (right, below,
   below right) is classified first (xbrz_corners, from a 4x4 window: rows
   y-1..y+2); then each pixel's block of k x k is filled with its colour and
   each of its four corners blended towards the neighbour colour where the
   corners say so (xbrz_row, from a 3x3 window and the corners of rows y-1
   and y). So a changed source row changes the scaled rows y-2..y+2. */
#include "kernel.h"

static u8 rp = 16, gp = 8, bp = 0;           /* where the 8-bit primaries are in a pixel */
static struct dc { u32 a, b, d, c; } dcache[65536];
#define DC_SALT 0x5BD1E995u

void xbrz_format(u8 rpos, u8 gpos, u8 bpos) { rp = rpos; gp = gpos; bp = bpos; memset(dcache, 0, sizeof dcache); }

static u32 isqrt(u32 v)
{
    u32 r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return r;
}

/* The YCbCr distance of two colours, 16 = one step of 8-bit luma. */
static u32 dist(u32 a, u32 b)
{
    if (a == b) return 0;
    if (a > b) { u32 t = a; a = b; b = t; }
    volatile struct dc *e = &dcache[(a * 0x9E3779B1u ^ b * 0x85EBCA77u) >> 16];
    u32 ea = e->a, eb = e->b, ed = e->d, ec = e->c;
    if (ea == a && eb == b && ec == (ea ^ eb ^ ed ^ DC_SALT)) return ed;
    int dr = (int)(a >> rp & 255) - (int)(b >> rp & 255);
    int dg = (int)(a >> gp & 255) - (int)(b >> gp & 255);
    int db = (int)(a >> bp & 255) - (int)(b >> bp & 255);
    int y = (269 * dr + 694 * dg + 61 * db) >> 6;            /* k_r 0.2627, k_g 0.6780, k_b 0.0593 */
    int cb = (544 * (db * 16 - y)) >> 10;                     /* 0.5 / (1 - k_b) */
    int cr = (694 * (dr * 16 - y)) >> 10;                     /* 0.5 / (1 - k_r) */
    u32 d = isqrt((u32)(y * y + cb * cb + cr * cr));
    e->c = 0; e->a = a; e->b = b; e->d = d;
    e->c = a ^ b ^ d ^ DC_SALT;
    return d;
}

static int eq(u32 a, u32 b) { return dist(a, b) < 30 * 16; }  /* equal colour tolerance 30 */

/* ---------------- corner classification ---------------- */
enum { NONE, NORMAL, DOMINANT };

/* Row y: per pixel F, the blend for the corner F G / J K, as
   bits 0-1 F, 2-3 G, 4-5 J, 6-7 K. */
void xbrz_corners(const u32 *src, u32 w, u32 h, u32 y, u8 *out)
{
    const u32 *r0 = src + (y ? y - 1 : 0) * w, *r1 = src + y * w;
    const u32 *r2 = src + (y + 1 < h ? y + 1 : h - 1) * w, *r3 = src + (y + 2 < h ? y + 2 : h - 1) * w;
    for (u32 x = 0; x < w; x++) {
        u32 xm = x ? x - 1 : 0, x1 = x + 1 < w ? x + 1 : w - 1, x2 = x + 2 < w ? x + 2 : w - 1;
        u32 b = r0[x], c = r0[x1];
        u32 e = r1[xm], f = r1[x], g = r1[x1], hh = r1[x2];
        u32 i = r2[xm], j = r2[x], k = r2[x1], l = r2[x2];
        u32 n = r3[x], o = r3[x1];
        u8 res = 0;
        if (!((f == g && j == k) || (f == j && g == k))) {
            u32 jg = dist(i, f) + dist(f, c) + dist(n, k) + dist(k, hh) + 4 * dist(j, g);
            u32 fk = dist(e, j) + dist(j, o) + dist(b, g) + dist(g, l) + 4 * dist(f, k);
            if (jg < fk) {
                u8 t = 36 * jg < 10 * fk ? DOMINANT : NORMAL;     /* dominant direction threshold 3.6 */
                if (f != g && f != j) res |= t;
                if (k != j && k != g) res |= t << 6;
            } else if (fk < jg) {
                u8 t = 36 * fk < 10 * jg ? DOMINANT : NORMAL;
                if (j != f && j != k) res |= t << 4;
                if (g != f && g != k) res |= t << 2;
            }
        }
        out[x] = res;
    }
}

/* ---------------- the scalers ----------------
   Each blend as cells (row, column) of the k x k block, for the bottom
   right corner, with the weight of the new colour (num/den, kept as
   1/256ths; 256: replace). Steep is shallow mirrored on the diagonal. */
struct cell { u8 i, j; u16 w; };
#define C(i, j, num, den) { i, j, ((num) * 256 + (den) / 2) / (den) }
struct scaler { const struct cell *shallow, *both, *diag, *corner; u8 n_shallow, n_both, n_diag, n_corner; };
#define N(a) a, (u8)(sizeof a / sizeof a[0])

static const struct cell s2_sh[] = { C(1,0,1,4), C(1,1,3,4) };
static const struct cell s2_bo[] = { C(1,0,1,4), C(0,1,1,4), C(1,1,5,6) };
static const struct cell s2_di[] = { C(1,1,1,2) };
static const struct cell s2_co[] = { C(1,1,21,100) };

static const struct cell s3_sh[] = { C(2,0,1,4), C(1,2,1,4), C(2,1,3,4), C(2,2,1,1) };
static const struct cell s3_bo[] = { C(2,0,1,4), C(0,2,1,4), C(2,1,3,4), C(1,2,3,4), C(2,2,1,1) };
static const struct cell s3_di[] = { C(1,2,1,8), C(2,1,1,8), C(2,2,7,8) };
static const struct cell s3_co[] = { C(2,2,45,100) };

static const struct cell s4_sh[] = { C(3,0,1,4), C(2,2,1,4), C(3,1,3,4), C(2,3,3,4), C(3,2,1,1), C(3,3,1,1) };
static const struct cell s4_bo[] = { C(3,1,3,4), C(1,3,3,4), C(3,0,1,4), C(0,3,1,4), C(2,2,1,3), C(3,3,1,1), C(3,2,1,1), C(2,3,1,1) };
static const struct cell s4_di[] = { C(3,2,1,2), C(2,3,1,2), C(3,3,1,1) };
static const struct cell s4_co[] = { C(3,3,68,100), C(3,2,9,100), C(2,3,9,100) };

static const struct cell s5_sh[] = { C(4,0,1,4), C(3,2,1,4), C(2,4,1,4), C(4,1,3,4), C(3,3,3,4),
                                     C(4,2,1,1), C(4,3,1,1), C(4,4,1,1), C(3,4,1,1) };
static const struct cell s5_bo[] = { C(0,4,1,4), C(2,3,1,4), C(1,4,3,4), C(4,0,1,4), C(3,2,1,4), C(4,1,3,4), C(3,3,2,3),
                                     C(2,4,1,1), C(3,4,1,1), C(4,4,1,1), C(4,2,1,1), C(4,3,1,1) };
static const struct cell s5_di[] = { C(4,2,1,8), C(3,3,1,8), C(2,4,1,8), C(4,3,7,8), C(3,4,7,8), C(4,4,1,1) };
static const struct cell s5_co[] = { C(4,4,86,100), C(4,3,23,100), C(3,4,23,100) };

static const struct cell s6_sh[] = { C(5,0,1,4), C(4,2,1,4), C(3,4,1,4), C(5,1,3,4), C(4,3,3,4), C(3,5,3,4),
                                     C(5,2,1,1), C(5,3,1,1), C(5,4,1,1), C(5,5,1,1), C(4,4,1,1), C(4,5,1,1) };
static const struct cell s6_bo[] = { C(0,5,1,4), C(2,4,1,4), C(1,5,3,4), C(3,4,3,4), C(5,0,1,4), C(4,2,1,4), C(5,1,3,4), C(4,3,3,4),
                                     C(2,5,1,1), C(3,5,1,1), C(4,5,1,1), C(5,5,1,1), C(4,4,1,1), C(5,4,1,1), C(5,2,1,1), C(5,3,1,1) };
static const struct cell s6_di[] = { C(5,3,1,2), C(4,4,1,2), C(3,5,1,2), C(4,5,1,1), C(5,5,1,1), C(5,4,1,1) };
static const struct cell s6_co[] = { C(5,5,97,100), C(4,5,42,100), C(5,4,42,100), C(5,3,6,100), C(3,5,6,100) };

static const struct scaler scalers[7] = {
    [2] = { s2_sh, s2_bo, s2_di, s2_co, 2, 3, 1, 1 },
    [3] = { s3_sh, s3_bo, s3_di, s3_co, 4, 5, 3, 1 },
    [4] = { s4_sh, s4_bo, s4_di, s4_co, 6, 8, 3, 3 },
    [5] = { s5_sh, s5_bo, s5_di, s5_co, 9, 12, 6, 3 },
    [6] = { s6_sh, s6_bo, s6_di, s6_co, 12, 16, 6, 5 },
};

static u32 mix(u32 back, u32 front, u32 f)                  /* f/256 of front over back */
{
    if (f == 256) return front;
    return (((back & 0xFF00FF) * (256 - f) + (front & 0xFF00FF) * f) >> 8 & 0xFF00FF) |
           (((back & 0x00FF00) * (256 - f) + (front & 0x00FF00) * f) >> 8 & 0x00FF00);
}

/* Cell (i, j) of a block turned by rot quarter turns: where it is unturned. */
static u32 *cell_at(u32 *blk, u32 pitch, u32 k, int rot, u32 i, u32 j)
{
    u32 oi = i, oj = j;
    if (rot == 1) { oi = k - 1 - j; oj = i; }
    else if (rot == 2) { oi = k - 1 - i; oj = k - 1 - j; }
    else if (rot == 3) { oi = j; oj = k - 1 - i; }
    return blk + oi * pitch + oj;
}

static void apply(const struct cell *c, int n, int transpose, u32 *blk, u32 pitch, u32 k, int rot, u32 col)
{
    for (int m = 0; m < n; m++) {
        u32 *p = transpose ? cell_at(blk, pitch, k, rot, c[m].j, c[m].i) : cell_at(blk, pitch, k, rot, c[m].i, c[m].j);
        *p = mix(*p, col, c[m].w);
    }
}

/* 3x3 window a..i (row by row) turned by a quarter turn, as indices. */
static const u8 turn[4][9] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8 },
    { 6, 3, 0, 7, 4, 1, 8, 5, 2 },
    { 8, 7, 6, 5, 4, 3, 2, 1, 0 },
    { 2, 5, 8, 1, 4, 7, 0, 3, 6 },
};

/* The bottom right corner of the block, in the window turned by rot. */
static void blend_corner(const u32 *ker, u8 blend, int rot, u32 *blk, u32 pitch, u32 k)
{
    const u8 *t = turn[rot];
    u32 b = ker[t[1]], c = ker[t[2]], d = ker[t[3]], e = ker[t[4]], f = ker[t[5]];
    u32 g = ker[t[6]], h = ker[t[7]], i = ker[t[8]];
    u8 bl = rot ? (u8)(blend << (2 * rot) | blend >> (8 - 2 * rot)) : blend;   /* turn the corners too */
    u32 br = bl >> 4 & 3, tr = bl >> 2 & 3, bot_l = bl >> 6 & 3;
    if (br < NORMAL) return;
    int line;
    if (br >= DOMINANT) line = 1;
    else if (tr != NONE && !eq(e, g)) line = 0;                   /* no second blend next to it (insular pixels) */
    else if (bot_l != NONE && !eq(e, c)) line = 0;
    else if (!eq(e, i) && eq(g, h) && eq(h, i) && eq(i, f) && eq(f, c)) line = 0;   /* L-shapes: the corner only */
    else line = 1;
    u32 px = dist(e, f) <= dist(e, h) ? f : h;                    /* the more similar neighbour */
    const struct scaler *s = &scalers[k];
    if (!line) { apply(s->corner, s->n_corner, 0, blk, pitch, k, rot, px); return; }
    u32 fg = dist(f, g), hc = dist(h, c);
    int shallow = 22 * fg <= 10 * hc && e != g && d != g;         /* steep direction threshold 2.2 */
    int steep = 22 * hc <= 10 * fg && e != c && b != c;
    if (shallow && steep) apply(s->both, s->n_both, 0, blk, pitch, k, rot, px);
    else if (shallow) apply(s->shallow, s->n_shallow, 0, blk, pitch, k, rot, px);
    else if (steep) apply(s->shallow, s->n_shallow, 1, blk, pitch, k, rot, px);
    else apply(s->diag, s->n_diag, 0, blk, pitch, k, rot, px);
}

/* Source row y scaled k times: k rows of w * k pixels at dst. cor: the
   corners of every row (xbrz_corners), rows y-1 and y current. */
void xbrz_row(const u32 *src, const u8 *cor, u32 w, u32 h, u32 y, u32 k, u32 *dst)
{
    u32 pitch = w * k;
    const u32 *pm = src + (y ? y - 1 : 0) * w, *p0 = src + y * w, *pp = src + (y + 1 < h ? y + 1 : y) * w;
    for (u32 x = 0; x < w; x++) {
        u32 xm = x ? x - 1 : 0, xp = x + 1 < w ? x + 1 : x;
        u32 ker[9] = { pm[xm], pm[x], pm[xp], p0[xm], p0[x], p0[xp], pp[xm], pp[x], pp[xp] };
        u32 *blk = dst + x * k;
        for (u32 r = 0; r < k; r++) for (u32 q = 0; q < k; q++) blk[r * pitch + q] = ker[4];
        u8 tl = y && x ? cor[(y - 1) * w + x - 1] >> 6 : 0;            /* K of the corner up left */
        u8 tr = y ? cor[(y - 1) * w + x] >> 4 & 3 : 0;                  /* J of the one above */
        u8 br = cor[y * w + x] & 3;                                     /* F of its own */
        u8 bl = x ? cor[y * w + x - 1] >> 2 & 3 : 0;                    /* G of the one to the left */
        u8 blend = (u8)(tl | tr << 2 | br << 4 | bl << 6);
        if (!blend) continue;
        for (int rot = 0; rot < 4; rot++) blend_corner(ker, blend, rot, blk, pitch, k);
    }
}
