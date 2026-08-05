/* megapreview.c -- see megapreview.h. Reads DOOM's megatexture atlas off disk and decodes a named
 * material's pages with the engine's own decoder, then hands the pixels to the preview transport.
 *
 * Nothing here hooks or mutates the engine. It reads three kinds of file the game ships
 * (`*.vmtr` tables and `_vmtr_sq*.mega2` shards, both under <game>\virtualtextures) and makes one
 * call into a pure decode function. Every constant is cited to the doom-re campaign evidence that
 * established it. */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "megapreview.h"
#include "imgpreview.h"   /* the fallback producer for materials with no atlas rect */
#include "preview.h"
#include "backend_log.h"

/* ------------------------------------------------------------------------ the decoder -----------
 * FUN_14196E140, RVA 0x196E140 in the pinned build. Signature:
 *
 *     void decode(const u8 *header16, const u8 *payload, void *unused, u8 *out);   out is 0x50000
 *
 * 5 planes of 128x128 RGBA at +0, +0x10000, +0x20000, +0x30000, +0x40000. Plane 0 is albedo, which
 * is the only one a browser needs (campaign evidence 04 SS9, confirmed by observation in 08 SS2).
 *
 * The RVA is build-specific, so it is NOT trusted on its own -- the prologue is checked against the
 * bytes below before the pointer is ever called. That prologue is whole, position-independent
 * instructions (push regs; sub rsp,0x130; lea rbp,[rsp+0x40]) with no RIP-relative operand, so it is
 * a safe signature. If DOOM updates and the function moves, this refuses to install. */
#define DECODE_RVA   0x196E140u
static const unsigned char DECODE_SIG[] = {
    0x40,0x55,0x56,0x57,0x41,0x54,0x41,0x55,0x41,0x56,0x41,0x57,
    0x48,0x81,0xEC,0x30,0x01,0x00,0x00,0x48,0x8D,0x6C,0x24,0x40
};
typedef void (*decode_fn)(const unsigned char *hdr, const unsigned char *payload,
                          void *unused, unsigned char *out);

#define OUT_SIZE     0x50000u    /* 5 planes x 128 x 128 x 4                                       */
#define PLANE_STRIDE 0x10000u
#define PAGE_FULL    128u        /* decoded page edge                                              */
#define PAGE_CORE    120u        /* usable pixels; the rest is a 4px border per side (evidence 07) */
#define PAGE_BORDER  4u

/* THE TRAP (evidence 08 SS3). The decoder reads a LONG way past the end of the page data -- measured
 * mean 73,172 B and max 167,220 B over 120 sampled pages. In the engine the page sits inside a much
 * larger staging allocation so this is invisible; a tight buffer faults inside the plane codec.
 * 256 KB is ~1.6x the measured worst case. The tail is zeroed so the read-ahead is deterministic
 * rather than whatever the heap happened to hold. */
#define PAGE_SLACK   0x40000u
#define PAGE_MAX     0x40000u    /* largest observed payload is ~50 KB; this is a sanity ceiling    */

/* ------------------------------------------------------------------------ the atlas --------------
 * All from campaign evidence 04 SS5 and 07. The atlas is 245760 x 245760 px = 2048 x 2048 pages of
 * 120 px, split into a 4x4 grid of shards each covering 512 x 512 pages. */
#define ATLAS_PAGE_PX 120u
#define SHARD_PAGES   512u
#define MAX_LEVELS    10         /* 512x512, 256x256, ... 1x1                                      */

/* Index cell layout: the mip chain concatenated, each level row-major, level 0 first. */
static unsigned g_levelAxis[MAX_LEVELS];
static unsigned g_levelBase[MAX_LEVELS];

typedef struct {
    char     name[192];
    unsigned x, y, w, h;
} vmtr_rect;

typedef struct {
    FILE               *f;
    unsigned           *index;      /* idxCount x u32: cell -> page id, 0xFFFFFFFF = absent        */
    unsigned            idxCount;
    unsigned long long *table;      /* pageCount x { u64 offset, u64 size }                        */
    unsigned            pageCount;
    int                 tried;      /* so a missing/corrupt shard is only reported once            */
} shard_t;

static const uint8_t *g_base;
static decode_fn      g_decode;
static vmtr_rect     *g_rects;
static int            g_rectCount;
static shard_t        g_shard[17];          /* 1-based, shards 1..16                              */
static unsigned char *g_out;                /* OUT_SIZE decode target                             */
static unsigned char *g_page;               /* PAGE_MAX + PAGE_SLACK, zero-tailed                 */
static unsigned char *g_levelTmp[MAX_LEVELS];  /* per-level 120x120 RGBA scratch for mip fallback */
static CRITICAL_SECTION g_lock;             /* serializes the shared scratch above                */
static LONG           g_installed;

static char           g_vtDir[MAX_PATH];

/* ---------------------------------------------------------------------- file plumbing -----------*/

static int megapreview_vt_dir(void)
{
    char exe[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(NULL, exe, MAX_PATH)) return 0;
    char *slash = strrchr(exe, '\\');
    if (!slash) return 0;
    *slash = '\0';
    _snprintf_s(g_vtDir, sizeof g_vtDir, _TRUNCATE, "%s\\virtualtextures", exe);
    DWORD a = GetFileAttributesA(g_vtDir);
    return (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

/* `.vmtr` rows are CRLF text: line 1 version, line 2 count, line 3 a column comment, then
 *     x y width height flags timeStamp mtrCheck "name"
 * with mtrCheck frequently negative. Anything that does not match that shape is skipped, which
 * covers the header lines without needing to count them. */
static void megapreview_load_one_vmtr(const char *path, int *cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        int x, y, w, h, flags; long long ts, chk;
        char name[192];
        if (sscanf(line, " %d %d %d %d %d %lld %lld \"%191[^\"]\"",
                   &x, &y, &w, &h, &flags, &ts, &chk, name) != 8) continue;
        if (w <= 0 || h <= 0 || x < 0 || y < 0) continue;
        if (g_rectCount == *cap) {
            int grown = *cap ? *cap * 2 : 1024;
            vmtr_rect *bigger = (vmtr_rect *)realloc(g_rects, (size_t)grown * sizeof *bigger);
            if (!bigger) break;
            g_rects = bigger; *cap = grown;
        }
        vmtr_rect *r = &g_rects[g_rectCount++];
        strncpy_s(r->name, sizeof r->name, name, _TRUNCATE);
        r->x = (unsigned)x; r->y = (unsigned)y; r->w = (unsigned)w; r->h = (unsigned)h;
    }
    fclose(f);
}

static int megapreview_load_rects(void)
{
    if (g_rectCount) return 1;
    char pattern[MAX_PATH];
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*.vmtr", g_vtDir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        backend_log("B2: megapreview -- no .vmtr tables found; previews unavailable");
        return 0;
    }
    int cap = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[MAX_PATH];
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_vtDir, fd.cFileName);
        megapreview_load_one_vmtr(path, &cap);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    char line[160];
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: megapreview -- loaded %d atlas rects from %s", g_rectCount, g_vtDir);
    backend_log(line);
    return g_rectCount > 0;
}

static const vmtr_rect *megapreview_find(const char *name)
{
    for (int i = 0; i < g_rectCount; ++i)
        if (_stricmp(g_rects[i].name, name) == 0) return &g_rects[i];
    return NULL;
}

/* A material's atlas rect in atlas pixels, or 0 if it has none. Two callers want this: the Assets
 * browser, to compute a `virtualmapping` renderParm value ((w,h,x,y)/245760), and the same browser
 * to know whether that carrier applies at all -- only VT-backed materials have a rect, so a null
 * answer is the honest reason to refuse the Virtual Mapping option rather than write a broken one.
 * Read-only against the already-parsed .vmtr table; takes the same lock the producer does. */
int sh_megapreview_rect(const char *name, int *out_xywh)
{
    if (!name || !out_xywh) return 0;
    int got = 0;
    EnterCriticalSection(&g_lock);
    if (megapreview_load_rects()) {
        const vmtr_rect *r = megapreview_find(name);
        if (r) {
            out_xywh[0] = (int)r->x; out_xywh[1] = (int)r->y;
            out_xywh[2] = (int)r->w; out_xywh[3] = (int)r->h;
            got = 1;
        }
    }
    LeaveCriticalSection(&g_lock);
    return got;
}

/* Open a shard and read its two tables. Layout, all DIRECT from idMegaTexture2::Load
 * (FUN_140e10bf0) and verified against every shipped shard -- campaign evidence 07 SS1:
 *
 *     0x000        0x170-byte header (magic 0xA63FBB21, version 2)
 *     0x170        page payloads, contiguous
 *     [hdr +0x40]  PAGE INDEX  idxCount(+0x4C) x u32
 *     [hdr +0x38]  PAGE TABLE  pageCount(+0x48) x { u64 offset, u64 size } */
static shard_t *megapreview_shard(int n)
{
    if (n < 1 || n > 16) return NULL;
    shard_t *s = &g_shard[n];
    if (s->index) return s;
    if (s->tried)  return NULL;
    s->tried = 1;

    char path[MAX_PATH];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\_vmtr_sq%d.mega2", g_vtDir, n);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    unsigned char hdr[0x170];
    if (fread(hdr, 1, sizeof hdr, f) != sizeof hdr) { fclose(f); return NULL; }
    unsigned magic = *(unsigned *)(hdr + 0x00), ver = *(unsigned *)(hdr + 0x04);
    if (magic != 0xA63FBB21u || ver != 2u) {
        char line[200];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B2: megapreview -- _vmtr_sq%d.mega2 bad magic/version (%08X/%u)", n, magic, ver);
        backend_log(line);
        fclose(f); return NULL;
    }
    unsigned long long tableOff = *(unsigned long long *)(hdr + 0x38);
    unsigned long long idxOff   = *(unsigned long long *)(hdr + 0x40);
    unsigned pageCount = *(unsigned *)(hdr + 0x48);
    unsigned idxCount  = *(unsigned *)(hdr + 0x4C);
    if (!pageCount || !idxCount || idxCount > (1u << 24) || pageCount > (1u << 24)) {
        fclose(f); return NULL;
    }

    unsigned           *index = (unsigned *)malloc((size_t)idxCount * 4);
    unsigned long long *table = (unsigned long long *)malloc((size_t)pageCount * 16);
    if (!index || !table) { free(index); free(table); fclose(f); return NULL; }

    int ok = (_fseeki64(f, (long long)idxOff, SEEK_SET) == 0) &&
             (fread(index, 4, idxCount, f) == idxCount) &&
             (_fseeki64(f, (long long)tableOff, SEEK_SET) == 0) &&
             (fread(table, 16, pageCount, f) == pageCount);
    if (!ok) { free(index); free(table); fclose(f); return NULL; }

    s->f = f; s->index = index; s->idxCount = idxCount;
    s->table = table; s->pageCount = pageCount;
    return s;
}

/* -------------------------------------------------------------------------- decoding ------------*/

/* Decode one page into g_out. Returns 1 on success, 0 if the cell has no page (absent) or the read
 * or decode failed. `level`/`px`/`py` are in that level's page grid. */
static int megapreview_decode_page(int level, unsigned px, unsigned py)
{
    unsigned axis = g_levelAxis[level];
    if (px >= axis * 4u || py >= axis * 4u) return 0;

    int n = 1 + (int)(px / axis) + 4 * (int)(py / axis);
    shard_t *s = megapreview_shard(n);
    if (!s) return 0;

    unsigned cell = g_levelBase[level] + (py % axis) * axis + (px % axis);
    if (cell >= s->idxCount) return 0;
    unsigned pid = s->index[cell];
    if (pid == 0xFFFFFFFFu || pid >= s->pageCount) return 0;

    unsigned long long off  = s->table[(size_t)pid * 2];
    unsigned long long size = s->table[(size_t)pid * 2 + 1];
    if (size < 17 || size > PAGE_MAX) return 0;

    if (_fseeki64(s->f, (long long)off, SEEK_SET) != 0) return 0;
    if (fread(g_page, 1, (size_t)size, s->f) != (size_t)size) return 0;
    memset(g_page + size, 0, PAGE_SLACK);        /* the read-ahead tail; see PAGE_SLACK */

    /* Pre-clear: skipped planes are NOT written by the decoder (evidence 08 SS2), so a stale
     * buffer would show the previous material's pixels in any plane this page omits. */
    memset(g_out, 0, OUT_SIZE);

    int ok = 1;
    __try {
        g_decode(g_page, g_page + 16, NULL, g_out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = 0;
        backend_log("B2: megapreview -- FAULTED inside the page decoder");
    }
    return ok;
}

/* One page's 120x120 albedo core, walking UP the mip chain when a page is absent.
 *
 * An absent page at a fine level is NORMAL virtual texturing: it means there is no extra detail
 * there, and the renderer samples the parent. So we upscale the parent's corresponding quadrant
 * rather than leaving a hole. Coarse levels are dense, so this always terminates. */
static int megapreview_page_core(int level, unsigned px, unsigned py, unsigned char *dst)
{
    if (level >= MAX_LEVELS) return 0;

    if (megapreview_decode_page(level, px, py)) {
        for (unsigned row = 0; row < PAGE_CORE; ++row) {
            const unsigned char *src =
                g_out + ((size_t)(row + PAGE_BORDER) * PAGE_FULL + PAGE_BORDER) * 4;
            memcpy(dst + (size_t)row * PAGE_CORE * 4, src, PAGE_CORE * 4);
        }
        return 1;
    }

    unsigned char *parent = g_levelTmp[level];
    if (!parent) return 0;
    if (!megapreview_page_core(level + 1, px / 2, py / 2, parent)) return 0;

    /* Point-sample the parent's quadrant. 60x60 -> 120x120, so every source pixel becomes a 2x2
     * block; this is a fallback for detail that does not exist, not a resampling problem. */
    unsigned qx = (px & 1u) * (PAGE_CORE / 2), qy = (py & 1u) * (PAGE_CORE / 2);
    for (unsigned row = 0; row < PAGE_CORE; ++row) {
        const unsigned char *src = parent + ((size_t)(qy + row / 2) * PAGE_CORE + qx) * 4;
        unsigned char       *out = dst + (size_t)row * PAGE_CORE * 4;
        for (unsigned col = 0; col < PAGE_CORE; ++col)
            memcpy(out + (size_t)col * 4, src + (size_t)(col / 2) * 4, 4);
    }
    return 1;
}

/* --------------------------------------------------------------------------- produce ------------*/

/* Preview budget. A preview of P pixels costs (P/120)^2 pages regardless of the material's native
 * size, so this is a straight quality/cost dial (campaign evidence 08 SS5b). 2x2 = 240x240 for
 * ~4 pages and ~85 KB is the measured sweet spot: a large visible gain over a single page, with
 * diminishing returns past it. */
#define PREVIEW_MAX_PAGES_PER_AXIS 2u

static int megapreview_produce(const char *name)
{
    const vmtr_rect *r = megapreview_find(name);
    if (!r) {
        char line[300];
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "B2: megapreview -- '%s' has no atlas rect (not a virtual-textured material)", name);
        backend_log(line);
        return 0;
    }

    /* Finest level whose page span fits the budget. Coarser levels always span fewer pages, so the
     * first match walking up from 0 is the most detailed one that fits. */
    int level = -1; unsigned px0 = 0, py0 = 0, nx = 0, ny = 0, ps = 0;
    for (int L = 0; L < MAX_LEVELS; ++L) {
        unsigned size = ATLAS_PAGE_PX << L;
        unsigned a = r->x / size, b = r->y / size;
        unsigned c = (r->x + r->w - 1) / size, d = (r->y + r->h - 1) / size;
        if (c - a + 1 <= PREVIEW_MAX_PAGES_PER_AXIS && d - b + 1 <= PREVIEW_MAX_PAGES_PER_AXIS) {
            level = L; px0 = a; py0 = b; nx = c - a + 1; ny = d - b + 1; ps = size;
            break;
        }
    }
    if (level < 0) return 0;

    unsigned cw = nx * PAGE_CORE, ch = ny * PAGE_CORE;
    unsigned char *canvas = (unsigned char *)malloc((size_t)cw * ch * 4);
    if (!canvas) return 0;
    memset(canvas, 0, (size_t)cw * ch * 4);

    unsigned char *tile = (unsigned char *)malloc(PAGE_CORE * PAGE_CORE * 4);
    if (!tile) { free(canvas); return 0; }

    int got = 0;
    for (unsigned j = 0; j < ny; ++j) {
        for (unsigned i = 0; i < nx; ++i) {
            if (!megapreview_page_core(level, px0 + i, py0 + j, tile)) continue;
            /* Plane 0's 4th byte is NOT a coverage alpha. The albedo plane carries no per-pixel
             * transparency, so whatever the codec leaves there is not one to honour. The old BMP
             * transport discarded the byte and so never showed the difference; PNG honours it,
             * which drew every opaque wall translucent over the checkerboard. Stamp opaque for
             * the pixels we actually decoded -- and only those, so pages that failed keep the
             * pre-cleared alpha=0 and still read as a hole rather than as black content. */
            for (unsigned p = 3; p < PAGE_CORE * PAGE_CORE * 4; p += 4) tile[p] = 0xFF;
            for (unsigned row = 0; row < PAGE_CORE; ++row)
                memcpy(canvas + (((size_t)(j * PAGE_CORE + row) * cw) + i * PAGE_CORE) * 4,
                       tile + (size_t)row * PAGE_CORE * 4, PAGE_CORE * 4);
            got++;
        }
    }
    free(tile);
    if (!got) { free(canvas); backend_log("B2: megapreview -- no pages decoded"); return 0; }

    /* The pages cover at least the material's rect and often more, because the rect need not be
     * page-aligned at the chosen level. Crop to the material itself so the preview never shows a
     * neighbour's pixels. Offsets are exact: the atlas is a plain grid. */
    unsigned sx = (r->x - px0 * ps) * PAGE_CORE / ps;
    unsigned sy = (r->y - py0 * ps) * PAGE_CORE / ps;
    unsigned sw = r->w * PAGE_CORE / ps, sh = r->h * PAGE_CORE / ps;
    if (sw == 0) sw = 1;
    if (sh == 0) sh = 1;
    if (sx + sw > cw) sw = cw - sx;
    if (sy + sh > ch) sh = ch - sy;

    unsigned char *crop = canvas;
    if (sx || sy || sw != cw || sh != ch) {
        crop = (unsigned char *)malloc((size_t)sw * sh * 4);
        if (!crop) { free(canvas); return 0; }
        for (unsigned row = 0; row < sh; ++row)
            memcpy(crop + (size_t)row * sw * 4,
                   canvas + (((size_t)(sy + row) * cw) + sx) * 4, (size_t)sw * 4);
    }

    sh_preview_publish(crop, sw, sh);

    char line[320];
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: megapreview -- '%s' %ux%u px: mip L%d, %u page(s), %d decoded -> %ux%u preview",
                name, r->w, r->h, level, nx * ny, got, sw, sh);
    backend_log(line);

    if (crop != canvas) free(crop);
    free(canvas);
    return 1;
}

/* ---------------------------------------------------------------------------- worker ------------*/

/* One serving thread, so decoding never runs on the UI or render thread and two requests can never
 * share the scratch buffers. Polling rather than an event because the transport is deliberately
 * ignorant of who produces for it; 100 ms is imperceptible next to the click that caused it. */
static DWORD WINAPI megapreview_worker(LPVOID unused)
{
    (void)unused;
    char want[512], failed[512] = { 0 };

    for (;;) {
        Sleep(100);
        if (!sh_preview_take_request(want, sizeof want)) continue;

        /* take_request keeps reporting the same request until something publishes, so a name that
         * CANNOT be produced would otherwise be retried ten times a second forever. Suppress only
         * that case -- keyed on the name that actually failed.
         *
         * It must not be keyed on "the last name we attempted": that also swallows a repeat request
         * for a name that SUCCEEDED, which is a thing users do constantly (click the same asset
         * again, or re-preview after changing something). Doing so leaves the UI polling a request
         * nobody is serving until it times out, while the pane still shows the previous image. */
        if (failed[0] && strcmp(want, failed) == 0) continue;

        EnterCriticalSection(&g_lock);
        int ok = megapreview_load_rects() && megapreview_produce(want);
        LeaveCriticalSection(&g_lock);

        /* Atlas route declined -> the material is not virtual-textured. Roughly half the catalog
         * is like that; those are backed by ordinary image assets, which imgpreview reads out of
         * the .index/.resources containers. Outside the lock: different scratch, different files. */
        if (!ok) ok = sh_imgpreview_produce(want);

        if (ok) failed[0] = '\0';
        else    strncpy_s(failed, sizeof failed, want, _TRUNCATE);
    }
}

int sh_megapreview_install(const uint8_t *module_base)
{
    if (!module_base) { backend_log("B2: megapreview -- no module base; not installed"); return 0; }
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;

    const unsigned char *fn = (const unsigned char *)(module_base + DECODE_RVA);
    int match = 0;
    __try { match = (memcmp(fn, DECODE_SIG, sizeof DECODE_SIG) == 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { match = 0; }
    if (!match) {
        backend_log("B2: megapreview -- decoder signature MISMATCH at RVA 0x196E140; "
                    "this DOOM build is not the pinned one. Previews disabled (nothing called).");
        return 0;
    }
    g_decode = (decode_fn)fn;
    g_base   = module_base;

    if (!megapreview_vt_dir()) {
        backend_log("B2: megapreview -- <game>\\virtualtextures not found; previews disabled");
        return 0;
    }

    for (int L = 0, axis = SHARD_PAGES, base = 0; L < MAX_LEVELS; ++L) {
        g_levelAxis[L] = (unsigned)axis;
        g_levelBase[L] = (unsigned)base;
        base += axis * axis;
        axis /= 2;
    }

    g_out  = (unsigned char *)malloc(OUT_SIZE);
    g_page = (unsigned char *)malloc(PAGE_MAX + PAGE_SLACK);
    if (!g_out || !g_page) { backend_log("B2: megapreview -- scratch alloc failed"); return 0; }
    for (int L = 0; L < MAX_LEVELS; ++L) {
        g_levelTmp[L] = (unsigned char *)malloc(PAGE_CORE * PAGE_CORE * 4);
        if (!g_levelTmp[L]) { backend_log("B2: megapreview -- scratch alloc failed"); return 0; }
    }
    InitializeCriticalSection(&g_lock);

    /* 8 MB reserved stack: the decoder's own frame is small but it recurses into the plane codec,
     * and reserve costs nothing until touched. */
    HANDLE t = CreateThread(NULL, 8u << 20, megapreview_worker, NULL, 0, NULL);
    if (!t) { backend_log("B2: megapreview -- worker thread failed to start"); return 0; }
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(t);

    backend_log("B2: megapreview -- installed; decoder verified at RVA 0x196E140, worker running");
    return 1;
}
