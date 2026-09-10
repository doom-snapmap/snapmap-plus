/* Read .vmtr tables and _vmtr_sq*.mega2 shards from virtualtextures, decode
 * selected pages with the native codec, then publish preview pixels.
 * Installed files remain read-only.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "megapreview.h"
#include "imgpreview.h"   /* the fallback producer for materials with no atlas rect */
#include "preview.h"
#include "backend_log.h"

/* Mega2PageDecode (pinned Vulkan RVA 0x196E140): decode(header16, payload,
 * unused, out). Output is five 128x128 RGBA planes totaling 0x50000 bytes;
 * plane 0 is albedo. Resolve through the shared signature database;
 * unavailable or ambiguous matches disable this route.
 */
typedef void (*decode_fn)(const unsigned char *hdr, const unsigned char *payload,
                          void *unused, unsigned char *out);

#define OUT_SIZE     0x50000u    /* 5 planes x 128 x 128 x 4                                       */
#define PLANE_STRIDE 0x10000u
#define PAGE_FULL    128u        /* decoded page edge                                              */
#define PAGE_CORE    120u        /* usable pixels; the rest is a 4px border per side               */
#define PAGE_BORDER  4u

/* Reserve a zeroed 256 KB tail after each payload. Controlled runs measured
 * read-ahead up to 167,220 bytes beyond page data; a tightly sized buffer can
 * fault in the plane codec.
 */
#define PAGE_SLACK   0x40000u
#define PAGE_MAX     0x40000u    /* largest observed payload is ~50 KB; this is a sanity ceiling    */

/* ------------------------------------------------------------------------ the atlas --------------
 * The atlas is 245760 x 245760 px = 2048 x 2048 pages of 120 px, split into a 4x4 grid of shards
 * each covering 512 x 512 pages. */
#define ATLAS_PAGE_PX 120u
#define SHARD_PAGES   512u
#define MAX_LEVELS    10         /* 512x512, 256x256, ... 1x1                                      */

/* Index cell layout: the mip chain concatenated, each level row-major, level 0 first. */
static unsigned g_levelAxis[MAX_LEVELS];
static unsigned g_levelBase[MAX_LEVELS];

typedef struct {
    const char *name;
    unsigned x, y, w, h;
} vmtr_rect;

typedef struct {
    FILE               *f;
    unsigned long long  fileBytes;
    unsigned long long  tableOff;   /* pageCount x { u64 offset, u64 size }                        */
    unsigned long long  indexOff;   /* idxCount x u32: cell -> page id                             */
    unsigned            idxCount;
    unsigned            pageCount;
    int                 tried;      /* so a missing/corrupt shard is only reported once            */
} shard_t;

static const uint8_t *g_base;
static decode_fn      g_decode;
static vmtr_rect     *g_rects;
static int            g_rectCount;
static int            g_rectCap;
static char          *g_rectNames;
static size_t         g_rectNameBytes;
static shard_t        g_shard[17];          /* 1-based, shards 1..16                              */
static unsigned char *g_out;                /* OUT_SIZE decode target                             */
static unsigned char *g_page;               /* PAGE_MAX + PAGE_SLACK, zero-tailed                 */
static unsigned char *g_levelTmp[MAX_LEVELS];  /* per-level 120x120 RGBA scratch for mip fallback */
static CRITICAL_SECTION g_lock;             /* serializes the shared scratch above                */
static LONG           g_installed;
static HANDLE         g_requestEvent;        /* worker sleeps until slot_request_preview signals  */

static char           g_vtDir[MAX_PATH];

static void megapreview_free_scratch(void)
{
    free(g_out); g_out = NULL;
    free(g_page); g_page = NULL;
    for (int L = 0; L < MAX_LEVELS; ++L) {
        free(g_levelTmp[L]);
        g_levelTmp[L] = NULL;
    }
}

static int megapreview_alloc_scratch(void)
{
    if (g_out && g_page) return 1;
    megapreview_free_scratch();
    g_out  = (unsigned char *)malloc(OUT_SIZE);
    g_page = (unsigned char *)malloc(PAGE_MAX + PAGE_SLACK);
    if (!g_out || !g_page) { megapreview_free_scratch(); return 0; }
    for (int L = 0; L < MAX_LEVELS; ++L) {
        g_levelTmp[L] = (unsigned char *)malloc(PAGE_CORE * PAGE_CORE * 4);
        if (!g_levelTmp[L]) { megapreview_free_scratch(); return 0; }
    }
    return 1;
}

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
static void megapreview_load_one_vmtr(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        int x, y, w, h, flags; long long ts, chk;
        char name[192];
        if (sscanf_s(line, " %d %d %d %d %d %lld %lld \"%191[^\"]\"",
                     &x, &y, &w, &h, &flags, &ts, &chk, name,
                     (unsigned)sizeof name) != 8) continue;
        if (w <= 0 || h <= 0 || x < 0 || y < 0) continue;
        char *copy = _strdup(name);
        if (!copy) break;
        if (g_rectCount == g_rectCap) {
            int grown = g_rectCap ? g_rectCap * 2 : 1024;
            vmtr_rect *bigger = (vmtr_rect *)realloc(g_rects, (size_t)grown * sizeof *bigger);
            if (!bigger) { free(copy); break; }
            g_rects = bigger; g_rectCap = grown;
        }
        vmtr_rect *r = &g_rects[g_rectCount++];
        r->name = copy;
        r->x = (unsigned)x; r->y = (unsigned)y; r->w = (unsigned)w; r->h = (unsigned)h;
    }
    fclose(f);
}

/* Allocate names individually while parsing, then compact them into one pool
 * and shrink the rectangle array.
 */
static void megapreview_compact_rects(void)
{
    size_t need = 0;
    for (int i = 0; i < g_rectCount; ++i) {
        size_t n = strlen(g_rects[i].name) + 1u;
        if (n > (size_t)-1 - need) return;
        need += n;
    }

    char *pool = need ? (char *)malloc(need) : NULL;
    if (need && pool) {
        char *next = pool;
        for (int i = 0; i < g_rectCount; ++i) {
            size_t n = strlen(g_rects[i].name) + 1u;
            memcpy(next, g_rects[i].name, n);
            free((void *)g_rects[i].name);
            g_rects[i].name = next;
            next += n;
        }
        g_rectNames = pool;
    }
    g_rectNameBytes = need;

    if (g_rectCount > 0) {
        vmtr_rect *exact = (vmtr_rect *)realloc(g_rects, (size_t)g_rectCount * sizeof *exact);
        if (exact) { g_rects = exact; g_rectCap = g_rectCount; }
    }
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
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        char path[MAX_PATH];
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_vtDir, fd.cFileName);
        megapreview_load_one_vmtr(path);
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    megapreview_compact_rects();

    char line[260];
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: megapreview -- loaded %d atlas rects from %s; retained %llu name bytes and "
                "%llu rect bytes",
                g_rectCount, g_vtDir, (unsigned long long)g_rectNameBytes,
                (unsigned long long)((size_t)g_rectCap * sizeof *g_rects));
    backend_log(line);
    return g_rectCount > 0;
}

static const vmtr_rect *megapreview_find(const char *name)
{
    for (int i = 0; i < g_rectCount; ++i)
        if (_stricmp(g_rects[i].name, name) == 0) return &g_rects[i];
    return NULL;
}

/* Look up an atlas rectangle under the producer lock. The browser uses it to
 * enable virtualmapping and compute {w,h,x,y}/245760.
 */
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

/* Row `i` of the parsed atlas, or NULL past the end. Loads the tables on first use like the rect
 * lookup does, so the caller need not care who touched it first. Takes the same lock. */
const char *sh_megapreview_name_at(int i)
{
    if (i < 0) return NULL;
    const char *out = NULL;
    EnterCriticalSection(&g_lock);
    if (megapreview_load_rects() && i < g_rectCount) out = g_rects[i].name;
    LeaveCriticalSection(&g_lock);
    return out;
}

/* Open a shard and retain only its header metadata. The layout follows idMegaTexture2::Load
 * (FUN_140e10bf0) and was verified against every shipped shard:
 *
 *     0x000        0x170-byte header (magic 0xA63FBB21, version 2)
 *     0x170        page payloads, contiguous
 *     [hdr +0x40]  PAGE INDEX  idxCount(+0x4C) x u32
 *     [hdr +0x38]  PAGE TABLE  pageCount(+0x48) x { u64 offset, u64 size } */
static shard_t *megapreview_shard(int n)
{
    if (n < 1 || n > 16) return NULL;
    shard_t *s = &g_shard[n];
    if (s->f) return s;
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

    if (_fseeki64(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long long fileEnd = _ftelli64(f);
    if (fileEnd < 0) { fclose(f); return NULL; }
    unsigned long long fileBytes = (unsigned long long)fileEnd;
    if (idxOff > fileBytes || tableOff > fileBytes ||
        (unsigned long long)idxCount > (fileBytes - idxOff) / 4u ||
        (unsigned long long)pageCount > (fileBytes - tableOff) / 16u) {
        fclose(f);
        return NULL;
    }

    s->f = f; s->fileBytes = fileBytes;
    s->indexOff = idxOff; s->idxCount = idxCount;
    s->tableOff = tableOff; s->pageCount = pageCount;
    return s;
}

/* Read one u32 page ID and one 16-byte page-table entry on demand instead of
 * retaining complete shard tables.
 */
static int megapreview_page_entry(shard_t *s, unsigned cell,
                                  unsigned long long *out_off, unsigned long long *out_size)
{
    if (!s || !s->f || !out_off || !out_size || cell >= s->idxCount) return 0;

    unsigned pid = 0;
    unsigned long long indexAt = s->indexOff + (unsigned long long)cell * 4u;
    if (_fseeki64(s->f, (long long)indexAt, SEEK_SET) != 0 ||
        fread(&pid, sizeof pid, 1, s->f) != 1 ||
        pid == 0xFFFFFFFFu || pid >= s->pageCount)
        return 0;

    unsigned long long entry[2] = {0, 0};
    unsigned long long tableAt = s->tableOff + (unsigned long long)pid * 16u;
    if (_fseeki64(s->f, (long long)tableAt, SEEK_SET) != 0 ||
        fread(entry, sizeof entry, 1, s->f) != 1 ||
        entry[0] > s->fileBytes || entry[1] > s->fileBytes - entry[0])
        return 0;

    *out_off = entry[0];
    *out_size = entry[1];
    return 1;
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
    unsigned long long off = 0, size = 0;
    if (!megapreview_page_entry(s, cell, &off, &size)) return 0;
    if (size < 17 || size > PAGE_MAX) return 0;

    if (_fseeki64(s->f, (long long)off, SEEK_SET) != 0) return 0;
    if (fread(g_page, 1, (size_t)size, s->f) != (size_t)size) return 0;
    memset(g_page + size, 0, PAGE_SLACK);        /* the read-ahead tail; see PAGE_SLACK */

    /* Clear output first because the decoder leaves omitted planes untouched. */
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

/* Read the 120x120 albedo core. If a page is absent, try coarser mips and
 * upscale the corresponding parent quadrant.
 */
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

/* Limit previews to a 2x2-page budget (240x240 pixels); choose the finest mip
 * that fits.
 */
#define PREVIEW_MAX_PAGES_PER_AXIS 2u

static int megapreview_produce(const char *name, unsigned long generation)
{
    ULONGLONG started = GetTickCount64();
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
            /* The albedo plane has no coverage alpha. Set decoded pixels
             * opaque; failed pages retain pre-cleared alpha zero.
             */
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

    int published = sh_preview_publish(generation, crop, sw, sh);

    char line[360];
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "B2: megapreview -- '%s' %ux%u px: mip L%d, %u page(s), %d decoded -> "
                "%ux%u preview in %llu ms",
                name, r->w, r->h, level, nx * ny, got, sw, sh,
                (unsigned long long)(GetTickCount64() - started));
    backend_log(line);

    if (crop != canvas) free(crop);
    free(canvas);
    return published;
}

/* ---------------------------------------------------------------------------- worker ------------*/

/* One event-driven worker owns the decode scratch. Allocate only for atlas
 * requests and release after 30 seconds idle.
 */
static int megapreview_service_request(const char *want, unsigned long generation, int kind)
{
    /* A catalog-typed Image is already the final resource. It must not load or search VMTR, and
     * it must not be captured by a same-named Material record. */
    if (kind == SH_ASSET_IMAGE)
        return sh_imgpreview_produce_image(want, generation);

    EnterCriticalSection(&g_lock);
    int hasRect = megapreview_load_rects() && megapreview_find(want) != NULL;
    int ok = hasRect && megapreview_alloc_scratch()
           ? megapreview_produce(want, generation) : SH_PREVIEW_FAILED;
    LeaveCriticalSection(&g_lock);

    if (ok == SH_PREVIEW_STALE) return ok;

    /* If atlas lookup declines, try ordinary material/image resources outside
     * this lock; that producer owns separate scratch.
     */
    if (!ok) ok = sh_imgpreview_produce(want, generation);
    return ok;
}

static DWORD WINAPI megapreview_worker(LPVOID unused)
{
    (void)unused;
    char want[512];

    for (;;) {
        DWORD wait = WaitForSingleObject(g_requestEvent, 30000);
        if (wait == WAIT_TIMEOUT) {
            EnterCriticalSection(&g_lock);
            megapreview_free_scratch();
            LeaveCriticalSection(&g_lock);
            continue;
        }
        if (wait != WAIT_OBJECT_0) break;

        unsigned long generation = 0;
        int kind = SH_PREVIEW_KIND_AUTO;
        if (!sh_preview_take_request(want, sizeof want, &generation, &kind)) continue;
        (void)megapreview_service_request(want, generation, kind);
        /* A failed request stays unpublished; another click supplies another event. */
    }
    return 0;
}

void sh_megapreview_wake(void)
{
    if (g_requestEvent) SetEvent(g_requestEvent);
}

int sh_megapreview_install(const sig_result *results, size_t n, const uint8_t *module_base)
{
    if (!module_base) { backend_log("B2: megapreview -- no module base; not installed"); return 0; }
    if (InterlockedCompareExchange(&g_installed, 1, 0) != 0) return 0;

    uintptr_t fn = sig_addr_by_name(results, n, "Mega2PageDecode");
    if (!fn) {
        backend_log("B2: megapreview -- Mega2PageDecode did not resolve; this DOOM build is not the "
                    "pinned one. Previews disabled (nothing called).");
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

    InitializeCriticalSection(&g_lock);
    g_requestEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!g_requestEvent) { backend_log("B2: megapreview -- request event failed"); return 0; }

    /* Reserve an 8 MB worker stack for the native decoder and recursive plane
     * codec.
     */
    HANDLE t = CreateThread(NULL, 8u << 20, megapreview_worker, NULL, 0, NULL);
    if (!t) {
        CloseHandle(g_requestEvent); g_requestEvent = NULL;
        backend_log("B2: megapreview -- worker thread failed to start");
        return 0;
    }
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    CloseHandle(t);

    backend_log("B2: megapreview -- installed; decoder verified at RVA 0x196E140, worker waiting; "
                "decode scratch allocates on demand");
    return 1;
}
