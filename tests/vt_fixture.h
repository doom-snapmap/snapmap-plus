/* vt_fixture.h -- synthetic mega2 members for the lit-page tests (no game bytes):
 * a stock-like square 16 and mods in the relative form (vtpack.h). */
#ifndef VT_FIXTURE_H
#define VT_FIXTURE_H
#include <stdlib.h>
#include <string.h>
#include "../src/backend/vtpack.h"


typedef struct cellv { int L, x, y; uint32_t page; } cellv;
typedef struct build {
    cellv *cells; int ncells, ccells;
    uint8_t **pages; size_t *plen; int npages, cpages;
} build;

static void page_new(build *b, unsigned seed)
{
    unsigned n0 = 20 + seed % 37, n1 = 10 + seed % 13, n2 = 5 + seed % 7, i;
    size_t n = 16 + n0 + n1 + n2;
    uint8_t *p = (uint8_t *)malloc(n);
    p[0] = p[1] = p[2] = 0x1E;
    p[3] = 0x04;
    p[4] = (uint8_t)(n0 >> 8); p[5] = (uint8_t)n0;
    p[6] = (uint8_t)(n1 >> 8); p[7] = (uint8_t)n1;
    p[8] = (uint8_t)(n2 >> 8); p[9] = (uint8_t)n2;
    p[10] = p[11] = p[12] = p[13] = 0;
    p[14] = 0x60;
    p[15] = 0xFF;
    for (i = 16; i < n; ++i) p[i] = (uint8_t)(seed * 31u + i);
    if (b->npages == b->cpages) {
        b->cpages = b->cpages ? b->cpages * 2 : 64;
        b->pages = (uint8_t **)realloc(b->pages, sizeof *b->pages * (size_t)b->cpages);
        b->plen = (size_t *)realloc(b->plen, sizeof *b->plen * (size_t)b->cpages);
    }
    b->pages[b->npages] = p;
    b->plen[b->npages] = n;
    b->npages++;
}

static void cell_add(build *b, int L, int x, int y, uint32_t page)
{
    if (b->ncells == b->ccells) {
        b->ccells = b->ccells ? b->ccells * 2 : 256;
        b->cells = (cellv *)realloc(b->cells, sizeof *b->cells * (size_t)b->ccells);
    }
    b->cells[b->ncells].L = L; b->cells[b->ncells].x = x; b->cells[b->ncells].y = y; b->cells[b->ncells].page = page;
    b->ncells++;
}

static void w32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static void w64(uint8_t *p, uint64_t v) { memcpy(p, &v, 8); }

static int side(int L) { int w = 512 >> L; return w ? w : 1; }
static uint32_t first(int L) { uint32_t f = 0; int k; for (k = 0; k < L; ++k) f += (uint32_t)(side(k) * side(k)); return f; }

/* A member of `square` (tools/vt/mega2.py write_member's layout); cells are square-local. */
static uint8_t *member_write(const build *b, int square, size_t *len)
{
    static const uint32_t consts[13] = { 2, 16, 12, 245760, 245760, 2048, 2048, 61440, 61440, 512, 512, 2, 12 };
    uint64_t pay = 0, pos;
    size_t total;
    uint8_t *m;
    uint32_t *index = (uint32_t *)malloc(4u * SH_VTP_CELLS);
    int i, L, col = (square - 1) % 4, row = (square - 1) / 4;
    for (i = 0; i < (int)SH_VTP_CELLS; ++i) index[i] = SH_VTP_EMPTY;
    for (i = 0; i < b->ncells; ++i)
        index[first(b->cells[i].L) + (uint32_t)(b->cells[i].y * side(b->cells[i].L) + b->cells[i].x)] = b->cells[i].page;
    for (i = 0; i < b->npages; ++i) pay += b->plen[i];
    total = (size_t)(SH_VTP_HDR + pay + 4ull * SH_VTP_CELLS + 16ull * (uint64_t)b->npages);
    m = (uint8_t *)calloc(1, total);
    w32(m, SH_VTP_MAGIC);
    for (i = 0; i < 13; ++i) w32(m + 4 + 4 * i, consts[i]);
    w64(m + 0x38, SH_VTP_HDR + pay + 4ull * SH_VTP_CELLS);
    w64(m + 0x40, SH_VTP_HDR + pay);
    w32(m + 0x48, (uint32_t)b->npages);
    w32(m + 0x4C, SH_VTP_CELLS);
    for (L = 0; L < 12; ++L) {
        w32(m + 0x50 + 0x18 * L, (uint32_t)((col * 512) >> L));
        w32(m + 0x54 + 0x18 * L, (uint32_t)((row * 512) >> L));
        w32(m + 0x58 + 0x18 * L, (uint32_t)side(L));
        w32(m + 0x5C + 0x18 * L, (uint32_t)side(L));
        w32(m + 0x60 + 0x18 * L, first(L));
        w32(m + 0x64 + 0x18 * L, (uint32_t)(side(L) * side(L)));
    }
    pos = SH_VTP_HDR;
    for (i = 0; i < b->npages; ++i) {
        memcpy(m + pos, b->pages[i], b->plen[i]);
        w64(m + SH_VTP_HDR + pay + 4ull * SH_VTP_CELLS + 16ull * (uint64_t)i, pos);
        w64(m + SH_VTP_HDR + pay + 4ull * SH_VTP_CELLS + 16ull * (uint64_t)i + 8, b->plen[i]);
        pos += b->plen[i];
    }
    memcpy(m + SH_VTP_HDR + pay, index, 4u * SH_VTP_CELLS);
    free(index);
    *len = total;
    return m;
}

static void build_free(build *b)
{
    int i;
    for (i = 0; i < b->npages; ++i) free(b->pages[i]);
    free(b->pages); free(b->plen); free(b->cells);
    memset(b, 0, sizeof *b);
}

/* A nearly empty stock square: levels 1 to 8 one row at the top edge (square
 * 16 as the game ships it; square 8 also one at the bottom), 9 to 11 one cell. */
static uint8_t *stock_member_of(int square, size_t *len)
{
    build b = { 0 };
    int L, x;
    uint8_t *m;
    for (L = 1; L <= 8; ++L)
        for (x = 0; x < side(L); ++x) {
            cell_add(&b, L, x, 0, (uint32_t)b.npages);
            page_new(&b, 7000u + (unsigned)b.npages);
            if (square == 8) { cell_add(&b, L, x, side(L) - 1, (uint32_t)b.npages); page_new(&b, 7000u + (unsigned)b.npages); }
        }
    for (L = 9; L < 12; ++L) { cell_add(&b, L, 0, 0, (uint32_t)b.npages); page_new(&b, 7000u + (unsigned)b.npages); }
    m = member_write(&b, square, len);
    build_free(&b);
    return m;
}

static uint8_t *stock_member(size_t *len) { return stock_member_of(16, len); }

/* A relative-form member covering w x h level-0 pages with the whole mip chain. */
static uint8_t *lit_member(int w, int h, unsigned seed, int square, int hole, size_t *len)
{
    build b = { 0 };
    int L, x, y;
    uint8_t *m;
    for (L = 0; L < 12; ++L) {
        int cw = (w + (1 << L) - 1) >> L, ch = (h + (1 << L) - 1) >> L;
        for (y = 0; y < ch; ++y)
            for (x = 0; x < cw; ++x) {
                if (hole && L == 1 && x == 0 && y == 0) continue;
                cell_add(&b, L, x, y, (uint32_t)b.npages);
                page_new(&b, seed + (unsigned)b.npages);
            }
    }
    m = member_write(&b, square, len);
    build_free(&b);
    return m;
}

#endif
