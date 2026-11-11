/* vtpack.c -- see vtpack.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include "vtpack.h"

static void vp_why(char *why, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!why || !cap) return;
    va_start(ap, fmt);
    _vsnprintf_s(why, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void wr64(uint8_t *p, uint64_t v) { wr32(p, (uint32_t)v); wr32(p + 4, (uint32_t)(v >> 32)); }

static int vp_log2(int n) { int k = 0; while ((1 << (k + 1)) <= n) ++k; return k; }
static int vp_side(int level) { int w = SH_VTP_SQ_PAGES >> level; return w > 0 ? w : 1; }

/* Level L of square n: x, y (global grid of that level), w, h, first cell, count. */
static void vp_level(int square, int level, uint32_t e[6])
{
    int col = (square - 1) % 4, row = (square - 1) / 4, L;
    uint32_t first = 0;
    for (L = 0; L < level; ++L) first += (uint32_t)vp_side(L) * (uint32_t)vp_side(L);
    e[0] = (uint32_t)((col * SH_VTP_SQ_PAGES) >> level);
    e[1] = (uint32_t)((row * SH_VTP_SQ_PAGES) >> level);
    e[2] = e[3] = (uint32_t)vp_side(level);
    e[4] = first;
    e[5] = e[2] * e[3];
}

static uint32_t vp_first(int level)
{
    uint32_t e[6];
    vp_level(1, level, e);
    return e[4];
}

/* The header constants after the magic: version 2, 16 squares, 12 levels, the family sides. */
static const uint32_t g_consts[13] = { 2, 16, SH_VTP_LEVELS, SH_VTP_TEXELS, SH_VTP_TEXELS, SH_VTP_PAGES0, SH_VTP_PAGES0,
                                       SH_VTP_SQ_PAGES * SH_VTP_PAGE, SH_VTP_SQ_PAGES * SH_VTP_PAGE, SH_VTP_SQ_PAGES,
                                       SH_VTP_SQ_PAGES, 2, 12 };

typedef struct vp_span { uint64_t a, b; } vp_span;
static int vp_span_cmp(const void *x, const void *y)
{
    const vp_span *a = (const vp_span *)x, *b = (const vp_span *)y;
    return a->a < b->a ? -1 : a->a > b->a;
}

int sh_vtp_open(sh_vtp_member *m, const uint8_t *d, size_t len, char *why, size_t cap)
{
    int i, L, sq;
    uint64_t pay_end;
    vp_span *spans;
    memset(m, 0, sizeof *m);
    if (!d || len < SH_VTP_HDR) { vp_why(why, cap, "it is shorter than a member header"); return 0; }
    if (rd32(d) != SH_VTP_MAGIC) { vp_why(why, cap, "it is not a mega2 member (magic 0x%08X)", rd32(d)); return 0; }
    for (i = 0; i < 13; ++i)
        if (rd32(d + 4 + 4 * i) != g_consts[i]) {
            vp_why(why, cap, "its header constants are not the family geometry (at 0x%X)", 4 + 4 * i);
            return 0;
        }
    for (sq = 1; sq <= 16; ++sq) {
        for (L = 0; L < SH_VTP_LEVELS; ++L) {
            uint32_t e[6];
            int k;
            vp_level(sq, L, e);
            for (k = 0; k < 6; ++k) if (rd32(d + 0x50 + 0x18 * L + 4 * k) != e[k]) break;
            if (k < 6) break;
        }
        if (L == SH_VTP_LEVELS) break;
    }
    if (sq > 16) { vp_why(why, cap, "its level table is not one square's"); return 0; }
    m->data = d;
    m->len = len;
    m->table_off = rd64(d + 0x38);
    m->index_off = rd64(d + 0x40);
    m->page_count = rd32(d + 0x48);
    m->square = sq;
    if (rd32(d + 0x4C) != SH_VTP_CELLS) { vp_why(why, cap, "its cell count is %u, not %u", rd32(d + 0x4C), SH_VTP_CELLS); return 0; }
    if (m->index_off < SH_VTP_HDR || m->index_off > len || len - m->index_off < 4ull * SH_VTP_CELLS ||
        m->table_off < SH_VTP_HDR || m->table_off > len || (len - m->table_off) / 16 < m->page_count) {
        vp_why(why, cap, "its cell index or page table lies outside the file");
        return 0;
    }
    pay_end = m->index_off < m->table_off ? m->index_off : m->table_off;
    for (i = 0; i < (int)SH_VTP_CELLS; ++i) {
        uint32_t p = rd32(d + m->index_off + 4ull * (uint64_t)i);
        if (p != SH_VTP_EMPTY && p >= m->page_count) {
            vp_why(why, cap, "a cell names page %u of %u", p, m->page_count);
            return 0;
        }
    }
    spans = (vp_span *)malloc(sizeof *spans * (m->page_count ? m->page_count : 1));
    if (!spans) { vp_why(why, cap, "out of memory"); return 0; }
    for (i = 0; i < (int)m->page_count; ++i) {
        uint64_t off = rd64(d + m->table_off + 16ull * (uint64_t)i), size = rd64(d + m->table_off + 16ull * (uint64_t)i + 8);
        if (off < SH_VTP_HDR || off > pay_end || size > pay_end - off) {
            free(spans);
            vp_why(why, cap, "page %d lies outside the payload area", i);
            return 0;
        }
        spans[i].a = off;
        spans[i].b = off + size;
    }
    qsort(spans, m->page_count, sizeof *spans, vp_span_cmp);
    for (i = 1; i < (int)m->page_count; ++i)
        if (spans[i].a < spans[i - 1].b) {
            free(spans);
            vp_why(why, cap, "two pages overlap at 0x%llX", (unsigned long long)spans[i].a);
            return 0;
        }
    free(spans);
    return 1;
}

uint32_t sh_vtp_cell(const sh_vtp_member *m, int level, int x, int y)
{
    int w = vp_side(level);
    if (!m || !m->data || level < 0 || level >= SH_VTP_LEVELS || x < 0 || y < 0 || x >= w || y >= w) return SH_VTP_EMPTY;
    return rd32(m->data + m->index_off + 4ull * ((uint64_t)vp_first(level) + (uint64_t)y * (uint64_t)w + (uint64_t)x));
}

int sh_vtp_page(const sh_vtp_member *m, uint32_t id, const uint8_t **bytes, size_t *len)
{
    uint64_t off, size;
    if (!m || id >= m->page_count) return 0;
    off = rd64(m->data + m->table_off + 16ull * id);
    size = rd64(m->data + m->table_off + 16ull * id + 8);
    *bytes = m->data + off;
    *len = (size_t)size;
    return 1;
}

int sh_vtp_page_ok(const uint8_t *p, size_t n, char *why, size_t cap)
{
    unsigned l[5], i, absent, flags;
    size_t want;
    if (n < 16) { vp_why(why, cap, "a page is shorter than its header"); return 0; }
    for (i = 0; i < 5; ++i) l[i] = (unsigned)p[4 + 2 * i] << 8 | p[5 + 2 * i];
    flags = p[3];
    absent = p[14];
    want = 16u + l[0] + l[1] + l[2] + l[3] + l[4];
    if ((flags & 0x08) && !(absent & 0x20)) want += 0x800;
    if (absent & 0x80) { vp_why(why, cap, "a page sets the lightmap flag"); return 0; }
    if (want != n) { vp_why(why, cap, "a page stores %zu bytes, its header says %zu", n, want); return 0; }
    for (i = 0; i < 3; ++i) {
        if (((absent >> i) & 1) && l[i]) { vp_why(why, cap, "a page marks plane %u absent but stores it", i); return 0; }
        if (!((absent >> i) & 1) && !l[i]) { vp_why(why, cap, "a page has an empty plane %u", i); return 0; }
    }
    if ((absent & 0x40) && l[3]) { vp_why(why, cap, "a page marks plane 3 absent but stores it"); return 0; }
    return 1;
}

int sh_vtp_lit_info(const sh_vtp_member *m, sh_vtp_lit *out, char *why, size_t cap)
{
    int L, x, y, w = 0, h = 0, S = 1;
    uint32_t i;
    memset(out, 0, sizeof *out);
    if (m->square != SH_VTP_LIT_SQUARE) {
        vp_why(why, cap, "its level table is square %d's; the relative form is square 1 from the origin", m->square);
        return 0;
    }
    for (y = 0; y < SH_VTP_SQ_PAGES; ++y)
        for (x = 0; x < SH_VTP_SQ_PAGES; ++x)
            if (sh_vtp_cell(m, 0, x, y) != SH_VTP_EMPTY) {
                if (x + 1 > w) w = x + 1;
                if (y + 1 > h) h = y + 1;
            }
    if (!w) { vp_why(why, cap, "it has no level-0 page"); return 0; }
    while (S < w || S < h) S <<= 1;
    if (S > SH_VTP_NODE_MAX) {
        vp_why(why, cap, "its pages cover %d x %d level-0 pages, and at most %d x %d (%d texels square) fit in the game's "
               "texture square that Snapmap+ shares among mods", w, h, SH_VTP_NODE_MAX, SH_VTP_NODE_MAX,
               SH_VTP_NODE_MAX * SH_VTP_PAGE);
        return 0;
    }
    for (L = 0; L < SH_VTP_LEVELS; ++L) {
        int side = vp_side(L), lim = S >> L ? S >> L : 1;
        for (y = 0; y < side; ++y)
            for (x = 0; x < side; ++x) {
                if (sh_vtp_cell(m, L, x, y) == SH_VTP_EMPTY) continue;
                if (x >= lim || y >= lim) {
                    vp_why(why, cap, "its level %d cell %d,%d lies outside its %d-page block", L, x, y, S);
                    return 0;
                }
                if (L + 1 < SH_VTP_LEVELS && sh_vtp_cell(m, L + 1, x >> 1, y >> 1) == SH_VTP_EMPTY) {
                    vp_why(why, cap, "its level %d cell %d,%d has no level %d page above it", L, x, y, L + 1);
                    return 0;
                }
            }
    }
    for (i = 0; i < m->page_count; ++i) {
        const uint8_t *p;
        size_t n;
        char pw[160];
        (void)sh_vtp_page(m, i, &p, &n);
        if (!sh_vtp_page_ok(p, n, pw, sizeof pw)) { vp_why(why, cap, "%s (page %u)", pw, i); return 0; }
        out->page_bytes += n;
    }
    out->w = w;
    out->h = h;
    out->node = S;
    out->pages = m->page_count;
    return 1;
}

/* ------------------------------------------------------------------ packing */

static int vp_stock_empty(const sh_vtp_member *stock, int level, int x0, int y0, int n)
{
    int x, y;
    for (y = y0; y < y0 + n; ++y)
        for (x = x0; x < x0 + n; ++x)
            if (sh_vtp_cell(stock, level, x, y) != SH_VTP_EMPTY) return 0;
    return 1;
}

int sh_vtp_fits(const sh_vtp_member *stock, int x, int y, int size)
{
    int L, top = vp_log2(size);
    for (L = 0; L <= top; ++L)
        if (!vp_stock_empty(stock, L, x >> L, y >> L, size >> L ? size >> L : 1)) return 0;
    return 1;
}

static int vp_overlap(sh_vtp_node a, sh_vtp_node b)
{
    return a.square == b.square && a.x < b.x + b.size && b.x < a.x + a.size && a.y < b.y + b.size && b.y < a.y + a.size;
}

int sh_vtp_carry(const sh_vtp_member *stock, sh_vtp_node node, const sh_vtp_node *others, int n)
{
    int top = vp_log2(node.size), L, i;
    for (L = top + 1; L <= SH_VTP_CARRY_MAX; ++L) {
        sh_vtp_node block;
        int clash = 0;
        block.size = 1 << L;
        block.square = node.square;
        if (node.x % block.size || node.y % block.size) break;
        if (sh_vtp_cell(stock, L, node.x >> L, node.y >> L) != SH_VTP_EMPTY) break;
        block.x = node.x;
        block.y = node.y;
        for (i = 0; i < n && !clash; ++i) clash = vp_overlap(block, others[i]);
        if (clash) break;
        top = L;
    }
    return top;
}

static const sh_vtp_req *g_sort_reqs;
static int vp_req_cmp(const void *a, const void *b)
{
    const sh_vtp_req *x = &g_sort_reqs[*(const int *)a], *y = &g_sort_reqs[*(const int *)b];
    if (x->size != y->size) return x->size > y->size ? -1 : 1;
    return strcmp(x->key, y->key);
}

int sh_vtp_place(const sh_vtp_member *const *stocks, int nstocks, sh_vtp_req *reqs, int n)
{
    sh_vtp_node *placed = (sh_vtp_node *)malloc(sizeof *placed * (size_t)(n ? n : 1));
    int *order = (int *)malloc(sizeof *order * (size_t)(n ? n : 1));
    int i, k, np = 0, no = 0;
    if (!placed || !order) { free(placed); free(order); return 0; }
    for (i = 0; i < n; ++i) {
        reqs[i].placed = 0;
        if (reqs[i].held) { reqs[i].node.size = reqs[i].size; reqs[i].placed = 1; placed[np++] = reqs[i].node; }
        else order[no++] = i;
    }
    g_sort_reqs = reqs;
    qsort(order, (size_t)no, sizeof *order, vp_req_cmp);
    for (k = 0; k < no; ++k) {
        sh_vtp_req *r = &reqs[order[k]];
        int S = r->size, x, y, s, best = -1, bx = 0, by = 0, bs = 0;
        if (S < 1 || S > SH_VTP_NODE_MAX || (S & (S - 1))) continue;
        /* Every square in the given order; the first one wins a tie. */
        for (s = 0; s < nstocks; ++s)
            for (y = 0; y + S <= SH_VTP_SQ_PAGES; y += S)
                for (x = 0; x + S <= SH_VTP_SQ_PAGES; x += S) {
                    sh_vtp_node c;
                    int clash = 0, score;
                    c.square = stocks[s]->square; c.x = x; c.y = y; c.size = S;
                    for (i = 0; i < np && !clash; ++i) clash = vp_overlap(c, placed[i]);
                    if (clash || !sh_vtp_fits(stocks[s], x, y, S)) continue;
                    score = sh_vtp_carry(stocks[s], c, placed, np);
                    if (score > best) { best = score; bx = x; by = y; bs = stocks[s]->square; }
                }
        if (best < 0) continue;
        r->node.square = bs;
        r->node.x = bx;
        r->node.y = by;
        r->node.size = S;
        r->placed = 1;
        placed[np++] = r->node;
    }
    free(placed);
    free(order);
    for (i = 0, k = 0; i < n; ++i) k += reqs[i].placed;
    return k;
}

static const sh_vtp_mod *g_sort_mods;
static int vp_mod_cmp(const void *a, const void *b)
{
    return strcmp(g_sort_mods[*(const int *)a].key, g_sort_mods[*(const int *)b].key);
}

int sh_vtp_compose(const sh_vtp_member *stock, sh_vtp_mod *mods, int n, uint8_t **out, size_t *out_len, char *why,
                   size_t cap)
{
    uint32_t *index = NULL, **maps = NULL, npages, p;
    int *order = NULL, i, k, L, ok = 0, n_here = 0;
    sh_vtp_node *nodes = NULL;
    uint64_t bytes = 0, pos;
    uint8_t *img = NULL;
    *out = NULL;
    *out_len = 0;
    if (stock->square < 1 || stock->square > 16) { vp_why(why, cap, "the stock member is not one square's"); return 0; }
    index = (uint32_t *)malloc(sizeof *index * SH_VTP_CELLS);
    order = (int *)malloc(sizeof *order * (size_t)(n ? n : 1));
    maps = (uint32_t **)calloc((size_t)(n ? n : 1), sizeof *maps);
    nodes = (sh_vtp_node *)malloc(sizeof *nodes * (size_t)(n ? n : 1));
    if (!index || !order || !maps || !nodes) { vp_why(why, cap, "out of memory"); goto done; }
    for (i = 0; i < (int)SH_VTP_CELLS; ++i) index[i] = rd32(stock->data + stock->index_off + 4ull * (uint64_t)i);
    npages = stock->page_count;
    for (p = 0; p < stock->page_count; ++p) bytes += rd64(stock->data + stock->table_off + 16ull * p + 8);
    /* Only the mods placed in this square. */
    {
        int m = 0;
        for (i = 0; i < n; ++i) if (mods[i].node.square == stock->square) order[m++] = i;
        g_sort_mods = mods;
        qsort(order, (size_t)m, sizeof *order, vp_mod_cmp);
        n_here = m;
    }
    /* Cells and page ids, mods in key order. */
    for (k = 0; k < n_here; ++k) {
        sh_vtp_mod *md = &mods[order[k]];
        int top, j, no = 0;
        if (md->m->square != SH_VTP_LIT_SQUARE) { vp_why(why, cap, "%s: its member is not in the relative form", md->key); goto done; }
        for (j = 0; j < n; ++j) if (j != order[k]) nodes[no++] = mods[j].node;
        top = sh_vtp_carry(stock, md->node, nodes, no);
        maps[k] = (uint32_t *)malloc(sizeof(uint32_t) * (md->m->page_count ? md->m->page_count : 1));
        if (!maps[k]) { vp_why(why, cap, "out of memory"); goto done; }
        for (p = 0; p < md->m->page_count; ++p) maps[k][p] = SH_VTP_EMPTY;
        md->levels = top + 1;
        md->cells = md->pages = 0;
        for (L = 0; L <= top; ++L) {
            int lim = md->node.size >> L ? md->node.size >> L : 1, x, y, side = vp_side(L);
            int gx = md->node.x >> L, gy = md->node.y >> L;
            for (y = 0; y < lim; ++y)
                for (x = 0; x < lim; ++x) {
                    uint32_t own = sh_vtp_cell(md->m, L, x, y), cell;
                    if (own == SH_VTP_EMPTY) continue;
                    if (gx + x >= side || gy + y >= side) { vp_why(why, cap, "%s: its block leaves the square", md->key); goto done; }
                    cell = vp_first(L) + (uint32_t)(gy + y) * (uint32_t)side + (uint32_t)(gx + x);
                    if (index[cell] != SH_VTP_EMPTY) {
                        vp_why(why, cap, "%s: level %d cell %d,%d is already taken", md->key, L, gx + x, gy + y);
                        goto done;
                    }
                    if (maps[k][own] == SH_VTP_EMPTY) {
                        maps[k][own] = npages++;
                        bytes += rd64(md->m->data + md->m->table_off + 16ull * own + 8);
                        md->pages++;
                    }
                    index[cell] = maps[k][own];
                    md->cells++;
                }
        }
    }
    {
        uint64_t total = SH_VTP_HDR + bytes + 4ull * SH_VTP_CELLS + 16ull * npages;
        if (total > (uint64_t)(size_t)-1 / 2) { vp_why(why, cap, "the merged member would be too large"); goto done; }
        img = (uint8_t *)malloc((size_t)total);
        if (!img) { vp_why(why, cap, "out of memory (%llu bytes)", (unsigned long long)total); goto done; }
        memset(img, 0, SH_VTP_HDR);
        wr32(img, SH_VTP_MAGIC);
        for (i = 0; i < 13; ++i) wr32(img + 4 + 4 * i, g_consts[i]);
        wr64(img + 0x38, SH_VTP_HDR + bytes + 4ull * SH_VTP_CELLS);
        wr64(img + 0x40, SH_VTP_HDR + bytes);
        wr32(img + 0x48, npages);
        wr32(img + 0x4C, SH_VTP_CELLS);
        for (L = 0; L < SH_VTP_LEVELS; ++L) {
            uint32_t e[6];
            int q;
            vp_level(stock->square, L, e);
            for (q = 0; q < 6; ++q) wr32(img + 0x50 + 0x18 * L + 4 * q, e[q]);
        }
        /* Payloads: stock pages in stock order, then each mod's in its new id order. */
        pos = SH_VTP_HDR;
        {
            uint8_t *table = img + SH_VTP_HDR + bytes + 4ull * SH_VTP_CELLS;
            uint32_t id = 0;
            for (p = 0; p < stock->page_count; ++p, ++id) {
                const uint8_t *src;
                size_t len;
                (void)sh_vtp_page(stock, p, &src, &len);
                memcpy(img + pos, src, len);
                wr64(table + 16ull * id, pos);
                wr64(table + 16ull * id + 8, len);
                pos += len;
            }
            for (k = 0; k < n_here; ++k) {
                const sh_vtp_mod *md = &mods[order[k]];
                uint32_t base = id, j;
                /* maps[k] assigns consecutive ids from `base` in first-seen order. */
                for (j = 0; j < md->pages; ++j) {
                    for (p = 0; p < md->m->page_count && maps[k][p] != base + j; ++p) {}
                    {
                        const uint8_t *src;
                        size_t len;
                        (void)sh_vtp_page(md->m, p, &src, &len);
                        memcpy(img + pos, src, len);
                        wr64(table + 16ull * id, pos);
                        wr64(table + 16ull * id + 8, len);
                        pos += len;
                        ++id;
                    }
                }
            }
        }
        for (i = 0; i < (int)SH_VTP_CELLS; ++i) wr32(img + SH_VTP_HDR + bytes + 4ull * (uint64_t)i, index[i]);
        *out = img;
        *out_len = (size_t)total;
        img = NULL;
        ok = 1;
    }
done:
    for (i = 0; maps && i < n; ++i) free(maps[i]);
    free(maps);
    free(index);
    free(order);
    free(nodes);
    free(img);
    return ok;
}

/* ------------------------------------------------------------------ materials */

static const char *const g_tail[] = {
    "physicalfilterparms\tphysicalvmtrfilterparms",
    "minlodmap\t_black",
    "pagetablemap\t_vmtrpagetable",
    "physicalmappingsmap\t_physicalvmtrmappings",
    "physicalpagesmaparray0\t_physicalvmtrpages0",
    "physicalpagesmaparray1\t_physicalvmtrpages0",
    "physicalpagesmap4\t_black",
    "physicalpagesprtmap4\t_black",
};
#define VP_TAIL (sizeof g_tail / sizeof g_tail[0])

typedef struct vp_line { const char *p; size_t n; } vp_line;

/* Split into lines (LF, CRLF or CR end a line), as Python's splitlines does for them. */
static vp_line *vp_lines(const char *t, size_t len, int *count)
{
    size_t i = 0, start = 0, cap = 64;
    int n = 0;
    vp_line *v = (vp_line *)malloc(sizeof *v * cap);
    if (!v) return NULL;
    while (i < len) {
        if (t[i] == '\n' || t[i] == '\r') {
            if (n == (int)cap) { vp_line *g = (vp_line *)realloc(v, sizeof *v * (cap *= 2)); if (!g) { free(v); return NULL; } v = g; }
            v[n].p = t + start; v[n].n = i - start; n++;
            if (t[i] == '\r' && i + 1 < len && t[i + 1] == '\n') ++i;
            start = ++i;
            continue;
        }
        ++i;
    }
    if (start < len) {
        if (n == (int)cap) { vp_line *g = (vp_line *)realloc(v, sizeof *v * (cap + 1)); if (!g) { free(v); return NULL; } v = g; }
        v[n].p = t + start; v[n].n = len - start; n++;
    }
    *count = n;
    return v;
}

/* The line's first token, lower-cased into `key`. */
static void vp_key(vp_line l, char *key, size_t cap)
{
    size_t i = 0, k = 0;
    while (i < l.n && isspace((unsigned char)l.p[i])) ++i;
    while (i < l.n && !isspace((unsigned char)l.p[i]) && k + 1 < cap) key[k++] = (char)tolower((unsigned char)l.p[i++]);
    key[k] = 0;
}

static int vp_is_tail(const char *key)
{
    size_t i;
    for (i = 0; i < VP_TAIL; ++i) {
        size_t n = strcspn(g_tail[i], "\t");
        if (strlen(key) == n && !strncmp(key, g_tail[i], n)) return 1;
    }
    return 0;
}

/* "virtualmapping { a, b, c, d }" -> 1 with the values. */
static int vp_mapping_line(vp_line l, double v[4])
{
    char buf[256], key[32], *p, *e;
    int i;
    vp_key(l, key, sizeof key);
    if (strcmp(key, "virtualmapping") || l.n >= sizeof buf) return 0;
    memcpy(buf, l.p, l.n);
    buf[l.n] = 0;
    p = strchr(buf, '{');
    if (!p) return 0;
    ++p;
    for (i = 0; i < 4; ++i) {
        v[i] = strtod(p, &e);
        if (e == p) return 0;
        p = e;
        while (*p == ' ' || *p == '\t') ++p;
        if (i < 3) { if (*p != ',') return 0; ++p; }
    }
    if (*p != '}') return 0;
    for (++p; *p; ++p) if (!isspace((unsigned char)*p)) return 0;
    return 1;
}

typedef struct vp_out { char *p; size_t n, cap; int oom; } vp_out;
static void vp_put(vp_out *o, const char *s, size_t n)
{
    if (o->oom) return;
    if (o->n + n + 1 > o->cap) {
        size_t c = o->cap ? o->cap : 256;
        char *g;
        while (o->n + n + 1 > c) c *= 2;
        g = (char *)realloc(o->p, c);
        if (!g) { o->oom = 1; return; }
        o->p = g;
        o->cap = c;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}
static void vp_putl(vp_out *o, const char *s, size_t n) { vp_put(o, s, n); vp_put(o, "\n", 1); }

int sh_vtp_material_names(const char *text, size_t len, const char *salt)
{
    int n = 0, i, hit = 0;
    char want[96];
    vp_line *ls = vp_lines(text, len, &n);
    if (!ls || !salt) { free(ls); return 0; }
    _snprintf_s(want, sizeof want, _TRUNCATE, "\"mods/%s/vmtr\"", salt);
    for (i = 0; i < n && !hit; ++i) {
        char key[32];
        size_t k = 0, wl = strlen(want);
        vp_key(ls[i], key, sizeof key);
        if (strcmp(key, "landpagefile")) continue;
        while (k < ls[i].n && isspace((unsigned char)ls[i].p[k])) ++k;
        k += 12;
        while (k < ls[i].n && isspace((unsigned char)ls[i].p[k])) ++k;
        if (ls[i].n - k >= wl && !_strnicmp(ls[i].p + k, want, wl)) {
            size_t j;
            hit = 1;
            for (j = k + wl; j < ls[i].n; ++j) if (!isspace((unsigned char)ls[i].p[j])) hit = 0;
        }
    }
    free(ls);
    return hit;
}

int sh_vtp_material_mapping(const char *text, size_t len, double out[4])
{
    int n = 0, i, hit = 0;
    vp_line *ls = vp_lines(text, len, &n);
    if (!ls) return 0;
    for (i = 0; i < n && !hit; ++i) hit = vp_mapping_line(ls[i], out);
    free(ls);
    return hit;
}

static int vp_close_line(const vp_line *ls, int n)
{
    int i;
    for (i = n - 1; i >= 0; --i) if (ls[i].n == 1 && ls[i].p[0] == '}') return i;
    return -1;
}

char *sh_vtp_material_moved(const char *text, size_t len, sh_vtp_node node, size_t *out_len)
{
    int n = 0, i, close, mapped = 0;
    vp_line *ls = vp_lines(text, len, &n);
    vp_out o = { 0 };
    int col = (node.square - 1) % 4, row = (node.square - 1) / 4;
    double dx = (double)((col * SH_VTP_SQ_PAGES + node.x) * SH_VTP_PAGE) / SH_VTP_TEXELS;
    double dy = (double)((row * SH_VTP_SQ_PAGES + node.y) * SH_VTP_PAGE) / SH_VTP_TEXELS;
    if (out_len) *out_len = 0;
    if (!ls) return NULL;
    if (node.square < 1 || node.square > 16) { free(ls); return NULL; }
    close = vp_close_line(ls, n);
    for (i = 0; i < n && !mapped; ++i) { double v[4]; mapped = vp_mapping_line(ls[i], v); }
    if (close < 0 || !mapped) { free(ls); return NULL; }
    for (i = 0; i < n; ++i) {
        char key[40];
        double v[4];
        if (i == close) {
            size_t t;
            for (t = 0; t < VP_TAIL; ++t) vp_putl(&o, g_tail[t], strlen(g_tail[t]));
        }
        vp_key(ls[i], key, sizeof key);
        if (!strcmp(key, "landpagefile") || vp_is_tail(key)) continue;
        if (vp_mapping_line(ls[i], v)) {
            char line[160];
            int w = _snprintf_s(line, sizeof line, _TRUNCATE, "virtualmapping\t{ %.10f, %.10f, %.10f, %.10f }", v[0], v[1],
                                v[2] + dx, v[3] + dy);
            vp_putl(&o, line, (size_t)(w > 0 ? w : 0));
            continue;
        }
        vp_putl(&o, ls[i].p, ls[i].n);
    }
    free(ls);
    if (o.oom) { free(o.p); return NULL; }
    if (out_len) *out_len = o.n;
    return o.p;
}

char *sh_vtp_material_plain(const char *text, size_t len, size_t *out_len)
{
    int n = 0, i, close;
    vp_line *ls = vp_lines(text, len, &n);
    vp_out o = { 0 };
    if (out_len) *out_len = 0;
    if (!ls) return NULL;
    close = vp_close_line(ls, n);
    if (close < 0) { free(ls); return NULL; }
    for (i = 0; i < n; ++i) {
        char key[40];
        vp_key(ls[i], key, sizeof key);
        if (!strcmp(key, "landpagefile") || !strcmp(key, "usevirtualmapping") || !strcmp(key, "virtualmapping") ||
            !strcmp(key, "virtualtexturefeedbackfloat") || vp_is_tail(key))
            continue;
        if (!strcmp(key, "ambientprogram")) { vp_putl(&o, "ambientprogram\toutsidenonvmtr", 29); continue; }
        if (!strcmp(key, "shadowprogram")) { vp_putl(&o, "shadowprogram\tshadownonvmtr", 27); continue; }
        vp_putl(&o, ls[i].p, ls[i].n);
    }
    free(ls);
    if (o.oom) { free(o.p); return NULL; }
    if (out_len) *out_len = o.n;
    return o.p;
}
