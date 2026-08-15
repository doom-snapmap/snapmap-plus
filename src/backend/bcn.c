/* bcn.c -- see bcn.h. BC1 / BC3 / BC7 -> RGBA8.
 *
 * Straight implementations of the public block-compression formats; no engine dependency.
 * Two invariants everything here obeys, because a texture browser must never fault on a
 * malformed or truncated asset:
 *   - reads never go past `src_len` (a short block reads as zero);
 *   - every channel is clamped on the way out. */

#include <string.h>
#include "bcn.h"

typedef unsigned char      u8;
typedef unsigned int       u32;
typedef unsigned long long u64;

static u8 clamp8(int v) { return (u8)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

/* Safe fetch: past the end of the source reads as 0 rather than faulting. */
static u8 at(const u8 *s, size_t len, size_t i) { return i < len ? s[i] : 0u; }

/* ------------------------------------------------------------------ BC1 colour block ----------*/

static void c565(unsigned v, u8 *out)
{
    unsigned r = (v >> 11) & 31u, g = (v >> 5) & 63u, b = v & 31u;
    out[0] = (u8)((r * 255u + 15u) / 31u);
    out[1] = (u8)((g * 255u + 31u) / 63u);
    out[2] = (u8)((b * 255u + 15u) / 31u);
}

/* Fills pal[4][3]. `opaque_only` forces the 4-colour (no punch-through) interpretation, which
 * is what BC3's colour block always uses. */
static void bc1_palette(const u8 *s, size_t len, size_t off, u8 pal[4][3], int *punch, int opaque_only)
{
    unsigned c0 = (unsigned)at(s, len, off) | ((unsigned)at(s, len, off + 1) << 8);
    unsigned c1 = (unsigned)at(s, len, off + 2) | ((unsigned)at(s, len, off + 3) << 8);
    c565(c0, pal[0]);
    c565(c1, pal[1]);
    if (c0 > c1 || opaque_only) {
        for (int i = 0; i < 3; ++i) {
            pal[2][i] = (u8)((2 * pal[0][i] + pal[1][i]) / 3);
            pal[3][i] = (u8)((pal[0][i] + 2 * pal[1][i]) / 3);
        }
        *punch = 0;
    } else {
        for (int i = 0; i < 3; ++i) pal[2][i] = (u8)((pal[0][i] + pal[1][i]) / 2);
        pal[3][0] = pal[3][1] = pal[3][2] = 0;
        *punch = 1;                      /* index 3 is transparent black */
    }
}

/* ------------------------------------------------------------------ BC4 alpha block -----------*/

static void bc4_palette(const u8 *s, size_t len, size_t off, u8 pal[8])
{
    unsigned a0 = at(s, len, off), a1 = at(s, len, off + 1);
    pal[0] = (u8)a0; pal[1] = (u8)a1;
    if (a0 > a1) {
        for (int i = 0; i < 6; ++i) pal[2 + i] = (u8)(((6 - i) * a0 + (1 + i) * a1) / 7);
    } else {
        for (int i = 0; i < 4; ++i) pal[2 + i] = (u8)(((4 - i) * a0 + (1 + i) * a1) / 5);
        pal[6] = 0; pal[7] = 255;
    }
}

static int bcn_dimensions(unsigned w, unsigned h, unsigned *pw, unsigned *ph, size_t *rgba_size)
{
    unsigned x = BCN_PAD(w), y = BCN_PAD(h);
    if (!w || !h || !x || !y) return 0;
    if ((size_t)x > (size_t)-1 / (size_t)y) return 0;
    size_t pixels = (size_t)x * y;
    if (pixels > (size_t)-1 / 4u) return 0;
    if (pw) *pw = x;
    if (ph) *ph = y;
    if (rgba_size) *rgba_size = pixels * 4u;
    return 1;
}

size_t bcn_rgba_size(unsigned w, unsigned h)
{
    size_t size = 0;
    return bcn_dimensions(w, h, NULL, NULL, &size) ? size : 0;
}

int bcn_decode_bc1(const u8 *src, size_t src_len, unsigned w, unsigned h, u8 *dst)
{
    unsigned pw = 0, ph = 0;
    if (!src || !dst || !bcn_dimensions(w, h, &pw, &ph, NULL)) return 0;
    size_t bi = 0;
    for (unsigned by = 0; by < ph; by += 4) {
        for (unsigned bx = 0; bx < pw; bx += 4, bi += 8) {
            u8 pal[4][3]; int punch = 0;
            bc1_palette(src, src_len, bi, pal, &punch, 0);
            u32 idx = (u32)at(src, src_len, bi + 4)        | ((u32)at(src, src_len, bi + 5) << 8)
                    | ((u32)at(src, src_len, bi + 6) << 16) | ((u32)at(src, src_len, bi + 7) << 24);
            for (unsigned py = 0; py < 4; ++py)
                for (unsigned px = 0; px < 4; ++px) {
                    unsigned k = py * 4 + px, sel = (idx >> (2 * k)) & 3u;
                    u8 *o = dst + ((size_t)(by + py) * pw + (bx + px)) * 4;
                    o[0] = pal[sel][0]; o[1] = pal[sel][1]; o[2] = pal[sel][2];
                    o[3] = (punch && sel == 3) ? 0u : 255u;
                }
        }
    }
    return 1;
}

int bcn_decode_bc3(const u8 *src, size_t src_len, unsigned w, unsigned h, u8 *dst)
{
    unsigned pw = 0, ph = 0;
    if (!src || !dst || !bcn_dimensions(w, h, &pw, &ph, NULL)) return 0;
    size_t bi = 0;
    for (unsigned by = 0; by < ph; by += 4) {
        for (unsigned bx = 0; bx < pw; bx += 4, bi += 16) {
            u8 apal[8]; bc4_palette(src, src_len, bi, apal);
            u64 abits = 0;
            for (int i = 0; i < 6; ++i) abits |= (u64)at(src, src_len, bi + 2 + i) << (8 * i);
            u8 pal[4][3]; int punch = 0;
            bc1_palette(src, src_len, bi + 8, pal, &punch, 1);   /* BC3 colour is always 4-colour */
            u32 idx = (u32)at(src, src_len, bi + 12)        | ((u32)at(src, src_len, bi + 13) << 8)
                    | ((u32)at(src, src_len, bi + 14) << 16) | ((u32)at(src, src_len, bi + 15) << 24);
            for (unsigned py = 0; py < 4; ++py)
                for (unsigned px = 0; px < 4; ++px) {
                    unsigned k = py * 4 + px, sel = (idx >> (2 * k)) & 3u;
                    u8 *o = dst + ((size_t)(by + py) * pw + (bx + px)) * 4;
                    o[0] = pal[sel][0]; o[1] = pal[sel][1]; o[2] = pal[sel][2];
                    o[3] = apal[(abits >> (3 * k)) & 7u];
                }
        }
    }
    return 1;
}

/* ---------------------------------------------------------------------------- BC7 --------------
 * Eight modes over a 128-bit block. The tables below are the format's own partition and anchor
 * tables (shared with BC6H) and are reproduced verbatim from the specification. */

static const u8 P2[64][16] = {
{0,0,1,1,0,0,1,1,0,0,1,1,0,0,1,1},{0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,1},
{0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1},{0,0,0,1,0,0,1,1,0,0,1,1,0,1,1,1},
{0,0,0,0,0,0,0,1,0,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,1,0,1,1,1,1,1,1,1},
{0,0,0,1,0,0,1,1,0,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,1,0,0,1,1,0,1,1,1},
{0,0,0,0,0,0,0,0,0,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,1,1,1,1,1,1,1,1,1},
{0,0,0,0,0,0,0,1,0,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,1,0,1,1,1},
{0,0,0,1,0,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1},
{0,0,0,0,1,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,0,1,1,1,1},
{0,0,0,0,1,0,0,0,1,1,1,0,1,1,1,1},{0,1,1,1,0,0,0,1,0,0,0,0,0,0,0,0},
{0,0,0,0,0,0,0,0,1,0,0,0,1,1,1,0},{0,1,1,1,0,0,1,1,0,0,0,1,0,0,0,0},
{0,0,1,1,0,0,0,1,0,0,0,0,0,0,0,0},{0,0,0,0,1,0,0,0,1,1,0,0,1,1,1,0},
{0,0,0,0,0,0,0,0,1,0,0,0,1,1,0,0},{0,1,1,1,0,0,1,1,0,0,1,1,0,0,0,1},
{0,0,1,1,0,0,0,1,0,0,0,1,0,0,0,0},{0,0,0,0,1,0,0,0,1,0,0,0,1,1,0,0},
{0,1,1,0,0,1,1,0,0,1,1,0,0,1,1,0},{0,0,1,1,0,1,1,0,0,1,1,0,1,1,0,0},
{0,0,0,1,0,1,1,1,1,1,1,0,1,0,0,0},{0,0,0,0,1,1,1,1,1,1,1,1,0,0,0,0},
{0,1,1,1,0,0,0,1,1,0,0,0,1,1,1,0},{0,0,1,1,1,0,0,1,1,0,0,1,1,1,0,0},
{0,1,0,1,0,1,0,1,0,1,0,1,0,1,0,1},{0,0,0,0,1,1,1,1,0,0,0,0,1,1,1,1},
{0,1,0,1,1,0,1,0,0,1,0,1,1,0,1,0},{0,0,1,1,0,0,1,1,1,1,0,0,1,1,0,0},
{0,0,1,1,1,1,0,0,0,0,1,1,1,1,0,0},{0,1,0,1,0,1,0,1,1,0,1,0,1,0,1,0},
{0,1,1,0,1,0,0,1,0,1,1,0,1,0,0,1},{0,1,0,1,1,0,1,0,1,0,1,0,0,1,0,1},
{0,1,1,1,0,0,1,1,1,1,0,0,1,1,1,0},{0,0,0,1,0,0,1,1,1,1,0,0,1,0,0,0},
{0,0,1,1,0,0,1,0,0,1,0,0,1,1,0,0},{0,0,1,1,1,0,1,1,1,1,0,1,1,1,0,0},
{0,1,1,0,1,0,0,1,1,0,0,1,0,1,1,0},{0,0,1,1,1,1,0,0,1,1,0,0,0,0,1,1},
{0,1,1,0,0,1,1,0,1,0,0,1,1,0,0,1},{0,0,0,0,0,1,1,0,0,1,1,0,0,0,0,0},
{0,1,0,0,1,1,1,0,0,1,0,0,0,0,0,0},{0,0,1,0,0,1,1,1,0,0,1,0,0,0,0,0},
{0,0,0,0,0,0,1,0,0,1,1,1,0,0,1,0},{0,0,0,0,0,1,0,0,1,1,1,0,0,1,0,0},
{0,1,1,0,1,1,0,0,1,0,0,1,0,0,1,1},{0,0,1,1,0,1,1,0,1,1,0,0,1,0,0,1},
{0,1,1,0,0,0,1,1,1,0,0,1,1,1,0,0},{0,0,1,1,1,0,0,1,1,1,0,0,0,1,1,0},
{0,1,1,0,1,1,0,0,1,1,0,0,1,0,0,1},{0,1,1,0,0,0,1,1,0,0,1,1,1,0,0,1},
{0,1,1,1,1,1,1,0,1,0,0,0,0,0,0,1},{0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1},
{0,0,0,0,1,1,1,1,0,0,1,1,0,0,1,1},{0,0,1,1,0,0,1,1,1,1,1,1,0,0,0,0},
{0,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0},{0,1,0,0,0,1,0,0,0,1,1,1,0,1,1,1}
};

static const u8 P3[64][16] = {
{0,0,1,1,0,0,1,1,0,2,2,1,2,2,2,2},{0,0,0,1,0,0,1,1,2,2,1,1,2,2,2,1},
{0,0,0,0,2,0,0,1,2,2,1,1,2,2,1,1},{0,2,2,2,0,0,2,2,0,0,1,1,0,1,1,1},
{0,0,0,0,0,0,0,0,1,1,2,2,1,1,2,2},{0,0,1,1,0,0,1,1,0,0,2,2,0,0,2,2},
{0,0,2,2,0,0,2,2,1,1,1,1,1,1,1,1},{0,0,1,1,0,0,1,1,2,2,1,1,2,2,1,1},
{0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2},{0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2},
{0,0,0,0,1,1,1,1,2,2,2,2,2,2,2,2},{0,0,1,2,0,0,1,2,0,0,1,2,0,0,1,2},
{0,1,1,2,0,1,1,2,0,1,1,2,0,1,1,2},{0,1,2,2,0,1,2,2,0,1,2,2,0,1,2,2},
{0,0,1,1,0,1,1,2,1,1,2,2,1,2,2,2},{0,0,1,1,2,0,0,1,2,2,0,0,2,2,2,0},
{0,0,0,1,0,0,1,1,0,1,1,2,1,1,2,2},{0,1,1,1,0,0,1,1,2,0,0,1,2,2,0,0},
{0,0,0,0,1,1,2,2,1,1,2,2,1,1,2,2},{0,0,2,2,0,0,2,2,0,0,2,2,1,1,1,1},
{0,1,1,1,0,1,1,1,0,2,2,2,0,2,2,2},{0,0,0,1,0,0,0,1,2,2,2,1,2,2,2,1},
{0,0,0,0,0,0,1,1,0,1,2,2,0,1,2,2},{0,0,0,0,1,1,0,0,2,2,1,0,2,2,1,0},
{0,1,2,2,0,1,2,2,0,0,1,1,0,0,0,0},{0,0,1,2,0,0,1,2,1,1,2,2,2,2,2,2},
{0,1,1,0,1,2,2,1,1,2,2,1,0,1,1,0},{0,0,0,0,0,1,1,0,1,2,2,1,1,2,2,1},
{0,0,2,2,1,1,0,2,1,1,0,2,0,0,2,2},{0,1,1,0,0,1,1,0,2,0,0,2,2,2,2,2},
{0,0,1,1,0,1,2,2,0,1,2,2,0,0,1,1},{0,0,0,0,2,0,0,0,2,2,1,1,2,2,2,1},
{0,0,0,0,0,0,0,2,1,1,2,2,1,2,2,2},{0,2,2,2,0,0,2,2,0,0,1,2,0,0,1,1},
{0,0,1,1,0,0,1,2,0,0,2,2,0,2,2,2},{0,1,2,0,0,1,2,0,0,1,2,0,0,1,2,0},
{0,0,0,0,1,1,1,1,2,2,2,2,0,0,0,0},{0,1,2,0,1,2,0,1,2,0,1,2,0,1,2,0},
{0,1,2,0,2,0,1,2,1,2,0,1,0,1,2,0},{0,0,1,1,2,2,0,0,1,1,2,2,0,0,1,1},
{0,0,1,1,1,1,2,2,2,2,0,0,0,0,1,1},{0,1,0,1,0,1,0,1,2,2,2,2,2,2,2,2},
{0,0,0,0,0,0,0,0,2,1,2,1,2,1,2,1},{0,0,2,2,1,1,2,2,0,0,2,2,1,1,2,2},
{0,0,2,2,0,0,1,1,0,0,2,2,0,0,1,1},{0,2,2,0,1,2,2,1,0,2,2,0,1,2,2,1},
{0,1,0,1,2,2,2,2,2,2,2,2,0,1,0,1},{0,0,0,0,2,1,2,1,2,1,2,1,2,1,2,1},
{0,1,0,1,0,1,0,1,0,1,0,1,2,2,2,2},{0,2,2,2,0,1,1,1,0,2,2,2,0,1,1,1},
{0,0,0,2,1,1,1,2,0,0,0,2,1,1,1,2},{0,0,0,0,2,1,1,2,2,1,1,2,2,1,1,2},
{0,2,2,2,0,1,1,1,0,1,1,1,0,2,2,2},{0,0,0,2,1,1,1,2,1,1,1,2,0,0,0,2},
{0,1,1,0,0,1,1,0,0,1,1,0,2,2,2,2},{0,0,0,0,0,0,0,0,2,1,1,2,2,1,1,2},
{0,1,1,0,0,1,1,0,2,2,2,2,2,2,2,2},{0,0,2,2,0,0,1,1,0,0,1,1,0,0,2,2},
{0,0,2,2,1,1,2,2,1,1,2,2,0,0,2,2},{0,0,0,0,0,0,0,0,0,0,0,0,2,1,1,2},
{0,0,0,2,0,0,0,1,0,0,0,2,0,0,0,1},{0,2,2,2,1,2,2,2,0,2,2,2,1,2,2,2},
{0,1,0,1,2,2,2,2,2,2,2,2,2,2,2,2},{0,1,1,1,2,0,1,1,2,2,0,1,2,2,2,0}
};

static const u8 A2[64] = {
15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,15,
15, 2, 8, 2, 2, 8, 8,15, 2, 8, 2, 2, 8, 8, 2, 2,
15,15, 6, 8, 2, 8,15,15, 2, 8, 2, 2, 2,15,15, 6,
 6, 2, 6, 8,15,15, 2, 2,15,15,15,15,15, 2, 2,15 };
static const u8 A3a[64] = {
 3, 3,15,15, 8, 3,15,15, 8, 8, 6, 6, 6, 5, 3, 3,
 3, 3, 8,15, 3, 3, 6,10, 5, 8, 8, 6, 8, 5,15,15,
 8,15, 3, 5, 6,10, 8,15,15, 3,15, 5,15,15,15,15,
 3,15, 5, 5, 5, 8, 5,10, 5,10, 8,13,15,12, 3, 3 };
static const u8 A3b[64] = {
15, 8, 8, 3,15,15, 3, 8,15,15,15,15,15,15,15, 8,
15, 8,15, 3,15, 8,15, 8, 3,15, 6,10,15,15,10, 8,
15, 3,15,10,10, 8, 9,10, 6,15, 8,15, 3, 6, 6, 8,
15, 3,15,15,15,15,15,15,15,15,15,15, 3,15,15, 8 };

static const int W2[4]  = {0,21,43,64};
static const int W3[8]  = {0,9,18,27,37,46,55,64};
static const int W4[16] = {0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64};

/* mode: subsets, partition bits, rotation bits, index-select bits, colour bits, alpha bits,
 *       p-bit kind (0 none / 1 shared per subset / 2 per endpoint), index bits, index2 bits */
static const struct { u8 ns, pb, rb, isb, cb, ab, pk, ib, ib2; } MODE[8] = {
    {3,4,0,0,4,0,2,3,0}, {2,6,0,0,6,0,1,3,0}, {3,6,0,0,5,0,0,2,0}, {2,6,0,0,7,0,2,2,0},
    {1,0,2,1,5,6,0,2,3}, {1,0,2,0,7,8,0,2,2}, {1,0,0,0,7,7,2,4,0}, {2,6,0,0,5,5,2,2,0}
};

typedef struct { const u8 *b; size_t len; unsigned pos; } bits_t;

static u32 bget(bits_t *s, unsigned n)
{
    u32 v = 0;
    for (unsigned i = 0; i < n; ++i) {
        unsigned bit = s->pos + i;
        u8 byte = at(s->b, s->len, bit >> 3);
        v |= (u32)((byte >> (bit & 7)) & 1u) << i;
    }
    s->pos += n;
    return v;
}

/* Expand a `bits`-wide value to 8 bits by replicating the high bits, per the spec. */
static u8 unq(u32 v, unsigned bits)
{
    if (bits >= 8) return (u8)v;
    v <<= (8 - bits);
    return (u8)(v | (v >> bits));
}

static int interp(int a, int b, int wt) { return (a * (64 - wt) + b * wt + 32) >> 6; }

int bcn_decode_bc7(const u8 *src, size_t src_len, unsigned w, unsigned h, u8 *dst)
{
    unsigned pw = 0, ph = 0;
    if (!src || !dst || !bcn_dimensions(w, h, &pw, &ph, NULL)) return 0;

    for (unsigned by = 0; by < ph; by += 4) {
        for (unsigned bx = 0; bx < pw; bx += 4) {
            size_t bi = ((size_t)(by >> 2) * (pw >> 2) + (bx >> 2)) * 16u;
            bits_t s = { src + (bi <= src_len ? bi : src_len),
                         bi <= src_len ? src_len - bi : 0u, 0u };

            unsigned mode = 8u;
            for (unsigned m = 0; m < 8u; ++m) if (bget(&s, 1)) { mode = m; break; }
            if (mode == 8u) {                       /* invalid block -> transparent black */
                for (unsigned py = 0; py < 4; ++py)
                    for (unsigned px = 0; px < 4; ++px)
                        memset(dst + ((size_t)(by + py) * pw + (bx + px)) * 4, 0, 4);
                continue;
            }

            const u8 ns = MODE[mode].ns, cb = MODE[mode].cb, ab = MODE[mode].ab;
            const u8 ib = MODE[mode].ib, ib2 = MODE[mode].ib2, pk = MODE[mode].pk;

            u32 part = MODE[mode].pb ? bget(&s, MODE[mode].pb) : 0u;
            u32 rot  = MODE[mode].rb ? bget(&s, MODE[mode].rb) : 0u;
            u32 isel = MODE[mode].isb ? bget(&s, MODE[mode].isb) : 0u;

            u32 ep[6][4];                            /* [endpoint][RGBA], still quantised */
            unsigned nep = (unsigned)ns * 2u;
            for (unsigned c = 0; c < 3; ++c) for (unsigned e = 0; e < nep; ++e) ep[e][c] = bget(&s, cb);
            if (ab) for (unsigned e = 0; e < nep; ++e) ep[e][3] = bget(&s, ab);
            else    for (unsigned e = 0; e < nep; ++e) ep[e][3] = 255u;

            u32 pbit[6] = {0,0,0,0,0,0};
            if (pk == 1) { for (unsigned i = 0; i < ns; ++i) { u32 p = bget(&s, 1); pbit[i*2] = pbit[i*2+1] = p; } }
            else if (pk == 2) { for (unsigned e = 0; e < nep; ++e) pbit[e] = bget(&s, 1); }

            /* Unquantise endpoints (p-bit becomes the new LSB). */
            u8 col[6][4];
            for (unsigned e = 0; e < nep; ++e) {
                unsigned cbits = cb + (pk ? 1u : 0u), abits = ab ? ab + (pk ? 1u : 0u) : 0u;
                for (unsigned c = 0; c < 3; ++c) {
                    u32 v = pk ? ((ep[e][c] << 1) | pbit[e]) : ep[e][c];
                    col[e][c] = unq(v, cbits);
                }
                if (ab) {
                    u32 v = pk ? ((ep[e][3] << 1) | pbit[e]) : ep[e][3];
                    col[e][3] = unq(v, abits);
                } else col[e][3] = 255u;
            }

            /* Index bits. The anchor pixel of each subset stores one bit fewer. */
            const u8 *ptab = (ns == 2) ? P2[part] : (ns == 3 ? P3[part] : NULL);
            unsigned anchor[3] = {0,0,0};
            if (ns == 2) anchor[1] = A2[part];
            else if (ns == 3) { anchor[1] = A3a[part]; anchor[2] = A3b[part]; }

            u32 idx1[16], idx2[16];
            for (unsigned k = 0; k < 16; ++k) {
                unsigned sub = ptab ? ptab[k] : 0u;
                unsigned n = ib - ((k == anchor[sub]) ? 1u : 0u);
                idx1[k] = bget(&s, n);
            }
            if (ib2) for (unsigned k = 0; k < 16; ++k) idx2[k] = bget(&s, ib2 - (k == 0 ? 1u : 0u));

            const int *wc = (ib == 2) ? W2 : (ib == 3 ? W3 : W4);
            const int *wa = ib2 ? ((ib2 == 2) ? W2 : W3) : wc;

            for (unsigned k = 0; k < 16; ++k) {
                unsigned sub = ptab ? ptab[k] : 0u;
                const u8 *e0 = col[sub * 2], *e1 = col[sub * 2 + 1];
                /* Mode 4's index-select bit swaps which index set drives colour vs alpha. */
                u32 ci = idx1[k], ai = ib2 ? idx2[k] : idx1[k];
                if (ib2 && isel) { u32 t = ci; ci = ai; ai = t; }
                int cw = (ib2 && isel) ? wa[ci] : wc[ci];
                int aw = ib2 ? ((ib2 && isel) ? wc[ai] : wa[ai]) : wc[ai];

                u8 out[4];
                for (unsigned c = 0; c < 3; ++c) out[c] = clamp8(interp(e0[c], e1[c], cw));
                out[3] = clamp8(interp(e0[3], e1[3], aw));

                /* Rotation moves the alpha channel into one of R/G/B (modes 4 and 5). */
                if (rot == 1) { u8 t = out[0]; out[0] = out[3]; out[3] = t; }
                else if (rot == 2) { u8 t = out[1]; out[1] = out[3]; out[3] = t; }
                else if (rot == 3) { u8 t = out[2]; out[2] = out[3]; out[3] = t; }

                unsigned px = k & 3u, py = k >> 2;
                memcpy(dst + ((size_t)(by + py) * pw + (bx + px)) * 4, out, 4);
            }
        }
    }
    return 1;
}

int bcn_decode(unsigned format_code, const u8 *src, size_t src_len,
               unsigned w, unsigned h, u8 *dst)
{
    switch (format_code) {
        case 10u: return bcn_decode_bc1(src, src_len, w, h, dst);
        case 11u: return bcn_decode_bc3(src, src_len, w, h, dst);
        case 23u: return bcn_decode_bc7(src, src_len, w, h, dst);
        default:  return 0;
    }
}
