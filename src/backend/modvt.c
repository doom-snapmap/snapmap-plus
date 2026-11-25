/* modvt.c -- see modvt.h. The first half (boards, sync, the detours' logic,
 * decoding) makes no engine call and builds into the native tests with
 * SH_MODVT_PURE; the engine half binds the VT system, installs the detours
 * and runs the sync on DOOM's main thread.
 *
 * Engine surface (build 13954591), every address from a signature or decoded
 * from inside one with its bytes checked; any mismatch leaves every site as
 * shipped and no board is registered:
 *   VtSetSource       idVirtualTexture::SetSource(rec, name, path, flavor, type, direct, reader),
 *                     VK 0x185AC30, GL 0x185BC90. Its head is LEA RCX,[sys]; CALL sync;
 *                     MOV RCX,RSI; CALL Release: the VT system (VK 0x66E0940, GL 0x4FE6120),
 *                     its job synchronisation (VK 0x17E59F0) and Release (VK 0x1859480,
 *                     GL 0x185A460).
 *   VtForMaterial     VirtualTextureForMaterial(sys, material), VK 0x17E8230, GL 0x17E9100
 *                     (detoured): its MOV RAX,[R13+disp32] returns record 1 for "_vmtr", so
 *                     disp32 - 8 is the record pointer table (VK sys+0x8B00, GL sys+0xA400).
 *   VtMaterialBind    the material binder (parms, record), VK 0x16CCCF0, GL 0x16CD9A0
 *                     (detoured): its calls give the record's feedback word (VK 0x1859780), the
 *                     vec4 setter (VK 0x15B9560) and the texture setter (VK 0x15B93C0).
 *   VtMaterialRebind  the material rebind (material), VK 0x16CCC80, GL 0x16CD930: it calls
 *                     VtForMaterial and VtMaterialBind (checked against their signatures) and
 *                     ends in the parameter-block copy (VK 0x16C9BD0) behind a VT-off flag.
 *   VtViewPageTable   the per-view setter's vmtrPageTableMap site, VK 0x17B4578, GL 0x17B5808:
 *   VtViewFeedback    and its vmtrFeedbackFloat site, VK 0x17B4643, GL 0x17B58D3. They give the
 *                     globals holding those two renderParm handles (VK 0x3844FB8 / 0x38450D8), and
 *                     are checked to read record 1 and call the binder's feedback function.
 *   JobsWaitAll       idParallelJobManagerLocal's wait for every job list, VK 0x31BF50, GL 0x31BAE0,
 *                     and the job manager it is given (VK 0x5842040).
 *   VtFrameCall       the site in EndFrame, VK 0x1558A79, GL 0x1559E29, right after the render-thread
 *                     sync: LEA RCX,[sys] (checked); CALL the VT frame update (VK 0x17E7AE0,
 *                     GL 0x17E89B0), detoured: boards change there, before the engine's update.
 * SetSource and Release run the VT system's synchronisation and allocate from
 * the current heap, so they are called on the main thread inside process heap
 * 0 (the page tables live as long as the record).
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "../common/text.h"
#include "modvt.h"
#include "modserve.h"
#include "modbundle.h"
#include "backend_log.h"

/* ------------------------------------------------------------------ log */

static void (*g_echo)(const char *line);   /* the console, once commands exist */

static void mv_log(const char *fmt, ...)
{
    char *body, *line;
    va_list ap;
    va_start(ap, fmt);
    body = sh_vtextf(fmt, ap);
    va_end(ap);
    line = sh_textf("VT: %s", body ? body : "(out of memory)");
    free(body);
    backend_log(line ? line : "VT: (out of memory)");
    if (g_echo) g_echo(line ? line : "VT: (out of memory)");
    free(line);
}

/* ------------------------------------------------------------------ boards served */

typedef struct mv_collect { sh_modvt_family *out; int n, cap, oom; } mv_collect;

static void mv_collect_file(void *context, const char *request, uint64_t size, const char *bundle_id,
                            const char *bundle_hash)
{
    mv_collect *c = (mv_collect *)context;
    char *fam = NULL;
    int member = 0, i;
    sh_modvt_family *f;
    if (!sh_mb_vt_member(request, &fam, &member)) { free(fam); return; }
    for (i = 0; i < c->n; ++i)
        if (!strcmp(c->out[i].name, fam)) {
            /* Two salts serving one board name: the first keeps it. */
            if (!strcmp(c->out[i].bundle_hash, bundle_hash)) {
                c->out[i].members |= 1u << (member - 1);
                c->out[i].bytes += size;
            }
            free(fam);
            return;
        }
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 8;
        sh_modvt_family *grown = (sh_modvt_family *)realloc(c->out, sizeof *grown * (size_t)cap);
        if (!grown) { c->oom = 1; free(fam); return; }
        c->out = grown;
        c->cap = cap;
    }
    f = &c->out[c->n];
    memset(f, 0, sizeof *f);
    f->name = fam;
    f->path = sh_textf("virtualtextures/%s.pages", fam);
    f->bundle = sh_text(bundle_id ? bundle_id : "");
    if (!f->path || !f->bundle) { free(f->name); free(f->path); free(f->bundle); c->oom = 1; return; }
    c->n++;
    strncpy_s(f->bundle_hash, sizeof f->bundle_hash, bundle_hash, _TRUNCATE);
    f->members = 1u << (member - 1);
    f->bytes = size;
}

void sh_modvt_families_free(sh_modvt_family *f, int n)
{
    int i;
    for (i = 0; f && i < n; ++i) { free(f[i].name); free(f[i].path); free(f[i].bundle); }
    free(f);
}

int sh_modvt_collect(sh_modvt_family **out)
{
    mv_collect c;
    memset(&c, 0, sizeof c);
    /* From the build the loaded map serves (a map's own copy of a salt, not
     * the root folder's), as scenes and banks are. */
    sh_modserve_each_served_here(mv_collect_file, &c);
    if (c.oom) mv_log("collecting the boards ran out of memory; %d are considered", c.n);
    if (out) *out = c.out;
    else sh_modvt_families_free(c.out, c.n);
    return c.n;
}

/* ------------------------------------------------------------------ boards held */

typedef struct mv_owned {
    int used;             /* 1 held; 0 free, or (name set) a record released or taken back */
    int record;
    void *rec;            /* the record's address, while held */
    char *name;           /* malloc'd, NULL until a board took the entry */
    char *path;
    char *bundle;
    char bundle_hash[65];
    unsigned squares;     /* the board's own member squares (bit j: slot j) */
    unsigned served_squares; /* the host and guest squares still served on this map */
} mv_owned;

/* A board placed in a record another board holds (no record was free). It stays while that record is held: its
 * squares cannot be handed to another board, since pages of them may still be resident in the shared cache under
 * that record's coordinates. A later build of the same board goes to new squares; the old entry is kept, stale. */
typedef struct mv_guest {
    int used;             /* placed; 0 once its record was released or lost */
    int stale;            /* a later build of the board took other squares */
    int record;
    void *rec;
    char *name;           /* malloc'd */
    char *bundle;
    char bundle_hash[65];
    unsigned squares;     /* the squares it occupies in the record's family */
    unsigned char to[16]; /* its own square j -> to[j] - 1 */
} mv_guest;
static mv_guest *g_guests;
static int g_nguests, g_capguests;
static volatile LONG g_nguests_live;   /* placed and not stale: the detours' idle test */

/* The detours read the boards and write the material list on whatever
 * thread binds materials; the sync changes them on the main thread. The
 * lock is never held across an engine call. */
static SRWLOCK g_lock = SRWLOCK_INIT;
static mv_owned g_owned[SH_MODVT_SLOTS];
static volatile LONG g_source_version;
static volatile LONG g_nowned;   /* boards held: the detours' idle test */

/* Materials bound to a board, by board name, so a board registered again
 * binds them again. Kept across releases. */
typedef struct mv_material {
    void *material;
    char *name;           /* malloc'd */
    int moved;            /* a shared board's material: its mapping was moved from base to applied */
    float base[4], applied[4];
} mv_material;
static mv_material *g_mats;
static int g_nmats, g_capmats;

/* Refusals already logged, so a pass does not repeat them: (name, build, cause). */
typedef struct mv_refusal { char *name; char hash[65]; int cause; } mv_refusal;   /* name malloc'd */
static mv_refusal *g_refused;
static int g_nrefused, g_caprefused;

enum { MV_TAKEN = 1, MV_FULL, MV_SETSOURCE, MV_FAULT, MV_CACHE };

static int mv_refusal_new(const sh_modvt_family *f, int cause)
{
    int i;
    for (i = 0; i < g_nrefused; ++i)
        if (g_refused[i].cause == cause && !strcmp(g_refused[i].name, f->name) && !strcmp(g_refused[i].hash, f->bundle_hash))
            return 0;
    if (g_nrefused == g_caprefused) {
        int cap = g_caprefused ? g_caprefused * 2 : 8;
        mv_refusal *grown = (mv_refusal *)realloc(g_refused, sizeof *grown * (size_t)cap);
        if (!grown) return 1;   /* logged again next time, never dropped */
        g_refused = grown;
        g_caprefused = cap;
    }
    if (!(g_refused[g_nrefused].name = sh_text(f->name))) return 1;
    strcpy_s(g_refused[g_nrefused].hash, sizeof g_refused[0].hash, f->bundle_hash);
    g_refused[g_nrefused].cause = cause;
    g_nrefused++;
    return 1;
}

static void mv_refusal_clear(const char *name)
{
    int i, w = 0;
    for (i = 0; i < g_nrefused; ++i) {
        if (strcmp(g_refused[i].name, name)) g_refused[w++] = g_refused[i];
        else free(g_refused[i].name);
    }
    g_nrefused = w;
}

static int mv_owned_by_name(const char *name)
{
    int i;
    for (i = 0; i < SH_MODVT_SLOTS; ++i)
        if (g_owned[i].used && g_owned[i].name && !strcmp(g_owned[i].name, name)) return i;
    return -1;
}

static int mv_wanted_by_name(const sh_modvt_family *w, int n, const char *name)
{
    int i;
    for (i = 0; i < n; ++i)
        if (!strcmp(w[i].name, name)) return i;
    return -1;
}

/* A free entry: the one this board left (its record is preferred again),
 * else a clean one, else any free one. */
static int mv_free_owned(const char *name)
{
    int i, any = -1;
    for (i = 0; i < SH_MODVT_SLOTS; ++i)
        if (!g_owned[i].used && g_owned[i].record && g_owned[i].name && !strcmp(g_owned[i].name, name)) return i;
    for (i = 0; i < SH_MODVT_SLOTS; ++i) {
        if (g_owned[i].used) continue;
        if (!g_owned[i].record) return i;
        if (any < 0) any = i;
    }
    return any;
}

static int mv_members(unsigned m)
{
    int n = 0;
    while (m) { n += (int)(m & 1u); m >>= 1; }
    return n;
}

static void mv_set_used(mv_owned *o, int used, void *rec)
{
    AcquireSRWLockExclusive(&g_lock);
    o->used = used;
    o->rec = used ? rec : NULL;
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedIncrement(&g_source_version);
    InterlockedExchangeAdd(&g_nowned, used ? 1 : -1);
}

/* Bind every material remembered for `name` again to record k. */
static int mv_rebind(const sh_modvt_ops *ops, const char *name, int k)
{
    void **list = NULL;
    int i, n = 0, done = 0;
    if (!ops->rebind) return 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nmats; ++i) n += !strcmp(g_mats[i].name, name);
    if (n && (list = (void **)malloc(sizeof *list * (size_t)n)) != NULL)
        for (i = 0, n = 0; i < g_nmats; ++i)
            if (!strcmp(g_mats[i].name, name)) list[n++] = g_mats[i].material;
    ReleaseSRWLockShared(&g_lock);
    if (!list) return 0;
    for (i = 0; i < n; ++i) done += ops->rebind(ops->context, list[i], k, name) == 1;
    free(list);
    return done;
}

/* ------------------------------------------------------------------ shared records */

static void mv_guests_recount(void)
{
    int i, live = 0;
    for (i = 0; i < g_nguests; ++i) live += g_guests[i].used && !g_guests[i].stale;
    InterlockedExchange(&g_nguests_live, live);
}

/* The live guest of that name, else -1. */
static int mv_guest_by_name(const char *name)
{
    int i;
    for (i = 0; i < g_nguests; ++i)
        if (g_guests[i].used && !g_guests[i].stale && !strcmp(g_guests[i].name, name)) return i;
    return -1;
}

/* The squares record k's family has taken: its board's own and every guest's, stale ones included. */
static unsigned mv_record_used(const mv_owned *o)
{
    unsigned used = o->squares;
    int i;
    for (i = 0; i < g_nguests; ++i)
        if (g_guests[i].used && g_guests[i].record == o->record) used |= g_guests[i].squares;
    return used;
}

/* Forget every guest of record k (the record was released or lost). */
static void mv_guests_drop(int k)
{
    int i;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < g_nguests; ++i)
        if (g_guests[i].used && g_guests[i].record == k) { g_guests[i].used = 0; g_guests[i].rec = NULL; }
    ReleaseSRWLockExclusive(&g_lock);
    mv_guests_recount();
}

/* 1 when a guest of record k is still wanted (served here from the build it was placed from). */
static int mv_guests_wanted(int k, const sh_modvt_family *wanted, int n)
{
    int i, w;
    for (i = 0; i < g_nguests; ++i) {
        if (!g_guests[i].used || g_guests[i].stale || g_guests[i].record != k) continue;
        w = mv_wanted_by_name(wanted, n, g_guests[i].name);
        if (w >= 0 && !strcmp(wanted[w].bundle_hash, g_guests[i].bundle_hash)) return 1;
    }
    return 0;
}

int sh_modvt_square_map(unsigned used, unsigned members, unsigned char to[16])
{
    int j, s = 0;
    memset(to, 0, 16);
    for (j = 0; j < 16; ++j) {
        if (!(members & (1u << j))) continue;
        while (s < 16 && (used & (1u << s))) ++s;
        if (s >= 16) { memset(to, 0, 16); return 0; }
        to[j] = (unsigned char)(s + 1);
        used |= 1u << s;
    }
    return 1;
}

int sh_modvt_mapping_shift(const unsigned char to[16], const float in[4], float out[4])
{
    int col, row, from, dest;
    memcpy(out, in, sizeof(float) * 4);
    if (!(in[2] >= 0.0f && in[2] < 1.0f && in[3] >= 0.0f && in[3] < 1.0f)) return 0;
    col = (int)(in[2] * 4.0f + 1e-4f);
    row = (int)(in[3] * 4.0f + 1e-4f);
    if (col > 3) col = 3;
    if (row > 3) row = 3;
    from = row * 4 + col;
    if (!to[from]) return 0;
    dest = to[from] - 1;
    out[2] = in[2] + 0.25f * (float)(dest % 4 - col);
    out[3] = in[3] + 0.25f * (float)(dest / 4 - row);
    return 1;
}

int sh_modvt_mega_ok(const uint8_t *m)
{
    int log2, side, squares, levels, count;
    if (!m) return 0;
    memcpy(&log2, m + SH_VT_M2_LOG2, 4);
    memcpy(&side, m + SH_VT_M2_SIDE, 4);
    memcpy(&squares, m + SH_VT_M2_GEOMETRY, 4);
    memcpy(&levels, m + SH_VT_M2_GEOMETRY + 4, 4);
    memcpy(&count, m + SH_VT_M2_COUNT, 4);
    return log2 == 2 && side == 4 && squares == 16 && levels == 12 && count >= 0 && count <= 16;
}

static uint8_t *mv_slot(uint8_t *m, int j) { return m + SH_VT_M2_SLOTS + (size_t)j * SH_VT_M2_SLOT; }
static void *mv_slot_ptr(uint8_t *m, int j, size_t off) { void *p; memcpy(&p, mv_slot(m, j) + off, sizeof p); return p; }

int sh_modvt_mega_move(uint8_t *dst, uint8_t *src, const unsigned char to[16])
{
    int j, moved = 0, left = 0, count;
    uint8_t keep[16][SH_VT_M2_SLOT];
    if (!sh_modvt_mega_ok(dst) || !sh_modvt_mega_ok(src)) return -1;
    for (j = 0; j < 16; ++j) {
        uint8_t *d;
        if (!mv_slot_ptr(src, j, SH_VT_M2_PAGES)) continue;
        if (!to[j]) return -1;
        d = mv_slot(dst, to[j] - 1);
        if (mv_slot_ptr(dst, to[j] - 1, SH_VT_M2_PAGES) || mv_slot_ptr(dst, to[j] - 1, SH_VT_M2_CELLS) ||
            mv_slot_ptr(dst, to[j] - 1, SH_VT_M2_READER))
            return -1;
        (void)d;
    }
    memcpy(&count, dst + SH_VT_M2_COUNT, 4);
    for (j = 0; j < 16; ++j) {
        uint8_t *s = mv_slot(src, j), *d;
        void *pages = mv_slot_ptr(src, j, SH_VT_M2_PAGES);
        int n, col, row, l;
        if (!pages) continue;
        n = to[j] - 1;
        col = n % 4;
        row = n / 4;
        d = mv_slot(dst, n);
        /* Everything but the page table first: a page read takes a slot only once its page table is there. */
        memcpy(d, s, SH_VT_M2_PAGES);
        memcpy(d + SH_VT_M2_CELLS, s + SH_VT_M2_CELLS, SH_VT_M2_SLOT - SH_VT_M2_CELLS);
        for (l = 0; l < 12; ++l) {
            int32_t x = (int32_t)(((uint32_t)col * 512u) >> l), y = (int32_t)(((uint32_t)row * 512u) >> l);
            memcpy(d + SH_VT_M2_LEVELS + (size_t)l * 0x18, &x, 4);
            memcpy(d + SH_VT_M2_LEVELS + (size_t)l * 0x18 + 4, &y, 4);
        }
        MemoryBarrier();
        memcpy(d + SH_VT_M2_PAGES, &pages, sizeof pages);
        memset(s, 0, SH_VT_M2_SLOT);
        if (n + 1 > count) count = n + 1;
        moved++;
    }
    MemoryBarrier();
    memcpy(dst + SH_VT_M2_COUNT, &count, 4);
    /* What is left (nothing, when every member had a square) from slot 0, so src's destructor frees exactly it. */
    for (j = 0; j < 16; ++j)
        if (mv_slot_ptr(src, j, SH_VT_M2_PAGES) || mv_slot_ptr(src, j, SH_VT_M2_CELLS) || mv_slot_ptr(src, j, SH_VT_M2_READER))
            memcpy(keep[left++], mv_slot(src, j), SH_VT_M2_SLOT);
    memset(mv_slot(src, 0), 0, (size_t)16 * SH_VT_M2_SLOT);
    for (j = 0; j < left; ++j) memcpy(mv_slot(src, j), keep[j], SH_VT_M2_SLOT);
    memcpy(src + SH_VT_M2_COUNT, &left, 4);
    return moved;
}

int sh_modvt_parm_get(const void *parms, int index, float out[4])
{
    const uint8_t *p = (const uint8_t *)parms, *ops, *vals;
    int n, i;
    if (!p || index < 0) return 0;
    memcpy(&ops, p, sizeof ops);
    memcpy(&n, p + 8, 4);
    memcpy(&vals, p + 0x18, sizeof vals);
    if (!ops || !vals || n <= 0 || n > 4096) return 0;
    for (i = n - 1; i >= 0; --i) {
        uint16_t at;
        memcpy(&at, ops + (size_t)i * 8 + 2, 2);
        if ((int)(int16_t)at == index) { memcpy(out, vals + (size_t)i * 16, sizeof(float) * 4); return 1; }
    }
    return 0;
}

int sh_modvt_guest_count(void) { return (int)InterlockedCompareExchange(&g_nguests_live, 0, 0); }

void sh_modvt_each_guest(sh_modvt_guest_fn fn, void *context)
{
    typedef struct { char *name; char hash[65]; int record; } one;
    one *list = NULL;
    int i, n = 0;
    if (!fn) return;
    AcquireSRWLockShared(&g_lock);
    if (g_nguests && (list = (one *)calloc((size_t)g_nguests, sizeof *list)) != NULL)
        for (i = 0; i < g_nguests; ++i)
            if (g_guests[i].used && !g_guests[i].stale && (list[n].name = sh_text(g_guests[i].name)) != NULL) {
                strcpy_s(list[n].hash, sizeof list[n].hash, g_guests[i].bundle_hash);
                list[n++].record = g_guests[i].record;
            }
    ReleaseSRWLockShared(&g_lock);
    for (i = 0; i < n; ++i) { fn(context, list[i].name, list[i].hash, list[i].record); free(list[i].name); }
    free(list);
}

/* No free record: place the board in the squares a held record's family leaves free. 1 when it was handled (placed,
 * or refused with its own line), 0 when no held record has room. */
static int mv_share(const sh_modvt_family *f, const sh_modvt_ops *ops, const char *phase, sh_modvt_result *res)
{
    int i, r;
    unsigned char to[16];
    if (!ops->join || !f->members) return 0;
    for (i = 0; i < SH_MODVT_SLOTS; ++i) {
        mv_owned *o = &g_owned[i];
        mv_guest *g;
        char *name, *bundle;
        int j;
        unsigned squares = 0;
        if (o->used && ops->pool_conflict && ops->pool_conflict(ops->context, o->record)) continue;
        if (!o->used || !o->rec || !sh_modvt_square_map(mv_record_used(o), f->members, to)) continue;
        r = ops->join(ops->context, o->record, f->path, to);
        if (r != 1) {
            if (mv_refusal_new(f, r < 0 ? MV_FAULT : MV_SETSOURCE))
                mv_log("%s: board '%s' (%s) not placed in record %d: %s", phase, f->name, f->bundle, o->record,
                       r < 0 ? "FAULTED (see the line above)"
                             : "its members did not load (see the engine's 'Using mega2 chunk file' console lines)");
            res->refused++;
            return 1;
        }
        for (j = 0; j < 16; ++j)
            if (to[j]) squares |= 1u << (to[j] - 1);
        name = sh_text(f->name);
        bundle = sh_text(f->bundle);
        AcquireSRWLockExclusive(&g_lock);
        if (name && bundle && g_nguests == g_capguests) {
            int cap = g_capguests ? g_capguests * 2 : 8;
            mv_guest *grown = (mv_guest *)realloc(g_guests, sizeof *grown * (size_t)cap);
            if (grown) { g_guests = grown; g_capguests = cap; }
        }
        if (name && bundle && g_nguests < g_capguests) {
            g = &g_guests[g_nguests++];
            memset(g, 0, sizeof *g);
            g->used = 1;
            g->record = o->record;
            g->rec = o->rec;
            g->name = name;
            g->bundle = bundle;
            strcpy_s(g->bundle_hash, sizeof g->bundle_hash, f->bundle_hash);
            g->squares = squares;
            memcpy(g->to, to, sizeof g->to);
            name = bundle = NULL;
        }
        ReleaseSRWLockExclusive(&g_lock);
        if (name || bundle) {
            /* Out of memory recording it: its squares stay taken in the engine's family; it is not tried again. */
            free(name);
            free(bundle);
            mv_log("%s: board '%s' placed in record %d but out of memory recording it", phase, f->name, o->record);
            res->refused++;
            return 1;
        }
        mv_guests_recount();
        mv_refusal_clear(f->name);
        mv_log("%s: board '%s' (%s, %d member%s) placed in record %d beside board '%s' (no record was free); its "
               "materials are bound at the next frame", phase, f->name, f->bundle, mv_members(f->members),
               mv_members(f->members) == 1 ? "" : "s", o->record, o->name);
        InterlockedIncrement(&g_source_version);
        res->shared++;
        return 1;
    }
    return 0;
}

int sh_modvt_sync(const sh_modvt_family *wanted, int n, const sh_modvt_ops *ops, const char *phase, int release_now,
                  sh_modvt_result *out)
{
    sh_modvt_result res;
    int i, k;
    memset(&res, 0, sizeof res);
    if (n < 0) n = 0;
    res.wanted = n;
    if (!ops || !ops->ready || !ops->find || !ops->is_free || !ops->set_source || !ops->adopt_cache || !ops->release ||
        !ops->record || ops->ready(ops->context) != 1) {
        res.not_ready = 1;
        if (out) *out = res;
        return -1;
    }
    if (!phase) phase = "sync";

    /* 1. The records held: the engine may have released or reused one. */
    for (i = 0; i < SH_MODVT_SLOTS; ++i) {
        mv_owned *o = &g_owned[i];
        if (!o->used) continue;
        k = ops->find(ops->context, o->name);
        if (k == o->record) continue;
        mv_log("%s: record %d no longer holds board '%s' (%s); it is registered again while its mod is served", phase,
               o->record, o->name, k < 0 ? "the engine released it" : "the name moved to another record");
        mv_guests_drop(o->record);   /* the boards placed beside it are placed again too */
        mv_set_used(o, 0, NULL);   /* name and record kept: the record is preferred again */
        res.lost++;
    }

    /* 2. Boards no longer served, or served from another build. */
    for (i = 0; i < SH_MODVT_SLOTS; ++i) {
        mv_owned *o = &g_owned[i];
        int w;
        if (!o->used) continue;
        w = mv_wanted_by_name(wanted, n, o->name);
        int alias = ops->pool_conflict && ops->pool_conflict(ops->context, o->record);
        if (!alias && w >= 0 && !strcmp(wanted[w].bundle_hash, o->bundle_hash)) continue;
        /* No longer served, but boards placed beside it still draw from it: kept. A later build of it is registered
         * anew, and the boards beside it are placed again (their materials are bound again at the next frame). */
        if (!alias && w < 0 && mv_guests_wanted(o->record, wanted, n)) continue;
        if (!release_now) { res.pending++; continue; }
        if (ops->release(ops->context, o->record) == 1) {
            mv_log("%s: released record %d, board '%s' (%s %s)", phase, o->record, o->name, o->bundle,
                   alias ? "needs another material-cache record" :
                   w < 0 ? "is no longer served here" : "is now served from another build");
            mv_guests_drop(o->record);
            mv_set_used(o, 0, NULL);
            res.released++;
        } else {
            mv_log("%s: releasing record %d, board '%s', FAILED; it is tried again at the next frame", phase, o->record,
                   o->name);
            res.pending++;
        }
    }

    /* 3. Every board without a record. */
    for (i = 0; i < n; ++i) {
        const sh_modvt_family *f = &wanted[i];
        int o = mv_owned_by_name(f->name), slot = -1, r, idx, g;
        if (o >= 0) {
            if (!strcmp(g_owned[o].bundle_hash, f->bundle_hash)) res.kept++;
            continue;   /* else its release is pending */
        }
        g = mv_guest_by_name(f->name);
        if (g >= 0) {
            if (!strcmp(g_guests[g].bundle_hash, f->bundle_hash)) { res.kept++; continue; }
            AcquireSRWLockExclusive(&g_lock);
            g_guests[g].stale = 1;   /* a later build: its squares stay taken, the new build is placed anew */
            ReleaseSRWLockExclusive(&g_lock);
            mv_guests_recount();
        }
        k = ops->find(ops->context, f->name);
        if (k >= 0) {
            if (mv_refusal_new(f, MV_TAKEN))
                mv_log("%s: board '%s' (%s) not registered: record %d already holds that name and was not created by "
                       "Snapmap+ ('sh_vt register' made it); its materials keep that record", phase, f->name, f->bundle, k);
            res.refused++;
            continue;
        }
        idx = mv_free_owned(f->name);
        if (idx >= 0 && g_owned[idx].record >= 1 && g_owned[idx].record <= SH_MODVT_LAST_USED && g_owned[idx].name &&
            !strcmp(g_owned[idx].name, f->name) && ops->is_free(ops->context, g_owned[idx].record) == 1 &&
            (!ops->pool_conflict || !ops->pool_conflict(ops->context, g_owned[idx].record)))
            slot = g_owned[idx].record;
        for (k = 1; k <= SH_MODVT_LAST_USED && slot < 0; ++k)
            if (ops->is_free(ops->context, k) == 1 && (!ops->pool_conflict || !ops->pool_conflict(ops->context, k))) slot = k;
        if (slot < 0 || idx < 0) {
            if (mv_share(f, ops, phase, &res)) continue;
            if (mv_refusal_new(f, MV_FULL)) {
                mv_log("%s: board '%s' (%s) not registered: no independent material-cache slot among records 1 to %d "
                       "and no board's record has %d "
                       "free square%s; its materials draw with plain textures", phase, f->name, f->bundle,
                       SH_MODVT_LAST_USED, mv_members(f->members), mv_members(f->members) == 1 ? "" : "s");
                if (ops->notice) {
                    char *name = sh_modserve_bundle_name(f->bundle_hash);   /* never the salt */
                    char *text = sh_modvt_full_notice(name);
                    free(name);
                    if (text) ops->notice(ops->context, text);
                    free(text);
                }
            }
            res.refused++;
            res.full++;
            continue;
        }
        r = ops->set_source(ops->context, slot, f->name, f->path);
        if (r == 1 && ops->adopt_cache(ops->context, slot) != 1) {
            /* Nothing is bound to the record yet: release it at once. */
            if (mv_refusal_new(f, MV_CACHE))
                mv_log("%s: board '%s' (%s) not registered: record %d could not be pointed at the material-format page "
                       "cache", phase, f->name, f->bundle, slot);
            (void)ops->release(ops->context, slot);
            res.refused++;
            continue;
        }
        if (r == 1) {
            mv_owned *m = &g_owned[idx];
            char *name = sh_text(f->name), *path = sh_text(f->path), *bundle = sh_text(f->bundle), *old[3];
            int again;
            if (!name || !path || !bundle) {
                free(name); free(path); free(bundle);
                mv_log("%s: board '%s' registered as record %d but out of memory recording it; it is released", phase,
                       f->name, slot);
                (void)ops->release(ops->context, slot);
                res.refused++;
                continue;
            }
            m->record = slot;
            AcquireSRWLockExclusive(&g_lock);
            old[0] = m->name; old[1] = m->path; old[2] = m->bundle;
            m->name = name; m->path = path; m->bundle = bundle;
            ReleaseSRWLockExclusive(&g_lock);
            free(old[0]); free(old[1]); free(old[2]);
            strcpy_s(m->bundle_hash, sizeof m->bundle_hash, f->bundle_hash);
            m->squares = f->members;
            mv_set_used(m, 1, ops->record(ops->context, slot));
            mv_refusal_clear(f->name);
            mv_log("%s: registered board '%s' (%s, %d member%s, %llu bytes) as record %d on the material-format page "
                   "cache", phase, f->name, f->bundle, mv_members(f->members), mv_members(f->members) == 1 ? "" : "s",
                   (unsigned long long)f->bytes, slot);
            res.registered++;
            again = mv_rebind(ops, f->name, slot);
            if (again) mv_log("%s: %d material%s of board '%s' bound again to record %d", phase, again,
                              again == 1 ? "" : "s", f->name, slot);
            res.rebound += again;
        } else {
            if (mv_refusal_new(f, r < 0 ? MV_FAULT : MV_SETSOURCE))
                mv_log("%s: board '%s' (%s) not registered: SetSource on record %d %s", phase, f->name, f->bundle, slot,
                       r < 0 ? "FAULTED (see the line above)"
                             : "refused (no member opened, or a bad header: see the engine's 'Using mega2 chunk file' "
                               "and 'SetSource:' console lines)");
            res.refused++;
        }
    }

    for (i = 0; i < SH_MODVT_SLOTS; ++i) {
        mv_owned *o = &g_owned[i];
        if (!o->used) continue;
        int w = mv_wanted_by_name(wanted, n, o->name);
        unsigned active = w >= 0 && !strcmp(wanted[w].bundle_hash, o->bundle_hash) ? o->squares : 0;
        for (int j = 0; j < g_nguests; ++j) {
            mv_guest *g = &g_guests[j];
            if (!g->used || g->stale || g->record != o->record) continue;
            w = mv_wanted_by_name(wanted, n, g->name);
            if (w >= 0 && !strcmp(wanted[w].bundle_hash, g->bundle_hash)) active |= g->squares;
        }
        if (active != o->served_squares) {
            o->served_squares = active;
            InterlockedIncrement(&g_source_version);
        }
    }
    if (out) *out = res;
    return sh_modvt_owned_count();
}

const char *sh_modvt_record_owner(int k)
{
    int i;
    for (i = 0; i < SH_MODVT_SLOTS; ++i)
        if (g_owned[i].used && g_owned[i].record == k) return g_owned[i].name;
    return NULL;
}

unsigned sh_modvt_served_squares(int k)
{
    for (int i = 0; i < SH_MODVT_SLOTS; ++i)
        if (g_owned[i].used && g_owned[i].record == k) return g_owned[i].served_squares;
    return 0;
}

unsigned sh_modvt_source_version(void) { return (unsigned)InterlockedCompareExchange(&g_source_version, 0, 0); }

int sh_modvt_owned_count(void)
{
    return (int)InterlockedCompareExchange(&g_nowned, 0, 0);
}

void sh_modvt_reset(void)
{
    int i;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < SH_MODVT_SLOTS; ++i) { free(g_owned[i].name); free(g_owned[i].path); free(g_owned[i].bundle); }
    memset(g_owned, 0, sizeof g_owned);
    for (i = 0; i < g_nmats; ++i) free(g_mats[i].name);
    free(g_mats);
    g_mats = NULL;
    g_nmats = g_capmats = 0;
    for (i = 0; i < g_nguests; ++i) { free(g_guests[i].name); free(g_guests[i].bundle); }
    free(g_guests);
    g_guests = NULL;
    g_nguests = g_capguests = 0;
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_nowned, 0);
    InterlockedExchange(&g_nguests_live, 0);
    for (i = 0; i < g_nrefused; ++i) free(g_refused[i].name);
    free(g_refused);
    g_refused = NULL;
    g_nrefused = g_caprefused = 0;
}

char *sh_modvt_full_notice(const char *mod)
{
    return sh_textf("DOOM has no room for another mod texture board on this map, so %s draws with plain textures; "
                    "a map with fewer mods that carry their own textures avoids this.", mod && *mod ? mod : "a mod");
}

/* ------------------------------------------------------------------ the two detours */

void *sh_modvt_board_record(const char *name)
{
    void *rec = NULL;
    int i;
    if (!name || !*name || !InterlockedCompareExchange(&g_nowned, 0, 0)) return NULL;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < SH_MODVT_SLOTS && !rec; ++i)
        if (g_owned[i].used && g_owned[i].rec && g_owned[i].name && !strcmp(g_owned[i].name, name)) rec = g_owned[i].rec;
    for (i = 0; i < g_nguests && !rec; ++i)
        if (g_guests[i].used && !g_guests[i].stale && g_guests[i].rec && !strcmp(g_guests[i].name, name)) rec = g_guests[i].rec;
    ReleaseSRWLockShared(&g_lock);
    return rec;
}

/* Under g_lock (exclusive). The entry for `material`, made when new (NULL out of memory). */
static mv_material *mv_remember(void *material, const char *name)
{
    int i;
    char *copy;
    for (i = 0; i < g_nmats; ++i)
        if (g_mats[i].material == material) {
            if (strcmp(g_mats[i].name, name) && (copy = sh_text(name)) != NULL) {
                free(g_mats[i].name);
                g_mats[i].name = copy;
                g_mats[i].moved = 0;
            }
            return &g_mats[i];
        }
    if (g_nmats == g_capmats) {
        int cap = g_capmats ? g_capmats * 2 : 16;
        mv_material *grown = (mv_material *)realloc(g_mats, sizeof *grown * (size_t)cap);
        if (!grown) return NULL;   /* this material is not bound again after a release; logged by the caller's count */
        g_mats = grown;
        g_capmats = cap;
    }
    if (!(copy = sh_text(name))) return NULL;
    memset(&g_mats[g_nmats], 0, sizeof g_mats[0]);
    g_mats[g_nmats].material = material;
    g_mats[g_nmats].name = copy;
    return &g_mats[g_nmats++];
}

/* The landpagefile of the material whose parameter set is `parms` (malloc'd), or NULL. */
static char *mv_landpage(const void *parms)
{
    const char *s = NULL;
    char *out = NULL;
    __try {
        s = *(const char *const *)((const uint8_t *)parms - SH_VT_MAT_PARMS + SH_VT_MAT_LANDPAGE);
        if (s) out = sh_text(s);
    } __except (EXCEPTION_EXECUTE_HANDLER) { free(out); out = NULL; }
    return out;
}

int sh_modvt_bind_board(void *parms, void *record, const sh_modvt_bind_ops *ops)
{
    char *name = NULL, *land = NULL;
    void *image;
    float word[4];
    const float *fb;
    unsigned char to[16];
    int i, held = 0, guest = 0;
    if (!parms || !record || !InterlockedCompareExchange(&g_nowned, 0, 0)) return 0;
    if (InterlockedCompareExchange(&g_nguests_live, 0, 0)) land = mv_landpage(parms);
    AcquireSRWLockShared(&g_lock);
    /* A shared record serves several boards: the material's landpagefile says which. */
    for (i = 0; land && i < g_nguests && !held; ++i)
        if (g_guests[i].used && !g_guests[i].stale && g_guests[i].rec == record && !strcmp(g_guests[i].name, land)) {
            name = sh_text(g_guests[i].name);
            memcpy(to, g_guests[i].to, sizeof to);
            held = guest = 1;
        }
    for (i = 0; i < SH_MODVT_SLOTS && !held; ++i)
        if (g_owned[i].used && g_owned[i].rec == record) {
            name = sh_text(g_owned[i].name);
            held = 1;
        }
    ReleaseSRWLockShared(&g_lock);
    free(land);
    if (!held || !name || !ops || !ops->feedback || !ops->set_vec4 || !ops->set_texture || !ops->feedback_handle ||
        !ops->pagetable_handle) {
        free(name);
        return 0;
    }
    image = *(void *const *)((const uint8_t *)record + SH_VT_REC_PTIMAGE);
    if (!image) { free(name); return 0; }   /* no page table: the view's values stay, as for any record without one */
    fb = ops->feedback(record, word);
    if (fb != word) memcpy(word, fb, sizeof word);
    ops->set_vec4(parms, ops->feedback_handle, word);
    ops->set_texture(parms, ops->pagetable_handle, image);
    if (guest && ops->mapping_handle && ops->get_vec4) {
        /* The mapping its text gave names its own square; it is moved to the square the board was placed in. A
         * second bind finds the moved value and starts again from the text's. */
        float cur[4], base[4], moved[4];
        mv_material *m;
        if (ops->get_vec4(parms, ops->mapping_handle, cur) == 1) {
            AcquireSRWLockExclusive(&g_lock);
            m = mv_remember((uint8_t *)parms - SH_VT_MAT_PARMS, name);
            memcpy(base, m && m->moved && !memcmp(cur, m->applied, sizeof cur) ? m->base : cur, sizeof base);
            if (sh_modvt_mapping_shift(to, base, moved)) {
                ops->set_vec4(parms, ops->mapping_handle, moved);
                if (m) { m->moved = 1; memcpy(m->base, base, sizeof base); memcpy(m->applied, moved, sizeof moved); }
            }
            ReleaseSRWLockExclusive(&g_lock);
            free(name);
            return 1;
        }
    }
    AcquireSRWLockExclusive(&g_lock);
    (void)mv_remember((uint8_t *)parms - SH_VT_MAT_PARMS, name);
    ReleaseSRWLockExclusive(&g_lock);
    free(name);
    return 1;
}

int sh_modvt_material_count(const char *name)
{
    int i, n = 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nmats; ++i) n += !name || !strcmp(g_mats[i].name, name);
    ReleaseSRWLockShared(&g_lock);
    return n;
}

/* ------------------------------------------------------------------ decoding */

static uint32_t mv_target(uint32_t rva, size_t at, size_t len, const uint8_t *code, size_t disp_at)
{
    int32_t d;
    memcpy(&d, code + disp_at, 4);
    return (uint32_t)((int64_t)rva + (int64_t)at + (int64_t)len + d);
}

int sh_modvt_find_setsource_head(const uint8_t *code, size_t len)
{
    size_t i;
    if (!code || len < 20) return -1;
    for (i = 0; i + 20 <= len; ++i)
        if (code[i] == 0x48 && code[i + 1] == 0x8D && code[i + 2] == 0x0D && code[i + 7] == 0xE8 &&
            code[i + 12] == 0x48 && code[i + 13] == 0x8B && code[i + 14] == 0xCE && code[i + 15] == 0xE8)
            return (int)i;
    return -1;
}

uint32_t sh_modvt_find_table_disp(const uint8_t *code, size_t len)
{
    size_t i;
    uint32_t d;
    if (!code || len < 8) return 0;
    for (i = 0; i + 8 <= len; ++i)
        if (code[i] == 0x49 && code[i + 1] == 0x8B && code[i + 2] == 0x85 && code[i + 7] == 0xE9) {
            memcpy(&d, code + i + 3, 4);
            return d;
        }
    return 0;
}

int sh_modvt_release_head_ok(const uint8_t *code, size_t len)
{
    size_t i;
    if (!code || len < 11 || memcmp(code, "\x48\x8B\xC4\x55", 4)) return 0;
    for (i = 0; i + 7 <= len; ++i)
        if (!memcmp(code + i, "\x83\xB9\xC0\x02\x00\x00\x00", 7)) return 1;
    return 0;
}

int sh_modvt_decode_layout(const uint8_t *ss, size_t ss_len, uint32_t ss_rva, const uint8_t *vf, size_t vf_len,
                           uint32_t vf_rva, sh_modvt_layout *out)
{
    sh_modvt_layout l;
    int at;
    uint32_t d;
    memset(&l, 0, sizeof l);
    if (out) *out = l;
    if (!ss || !vf || !out) return 0;
    at = sh_modvt_find_setsource_head(ss, ss_len);
    if (at < 0x30 || at > 0x80) return 0;
    l.sys_rva = mv_target(ss_rva, (size_t)at, 7, ss, (size_t)at + 3);
    l.sync_rva = mv_target(ss_rva, (size_t)at + 7, 5, ss, (size_t)at + 8);
    l.release_rva = mv_target(ss_rva, (size_t)at + 15, 5, ss, (size_t)at + 16);
    d = sh_modvt_find_table_disp(vf, vf_len);
    if (d < 8 || d >= 0x20000) return 0;
    l.table_off = d - 8;
    *out = l;
    return 1;
}

/* Fixed offsets inside the signed binder: CALL feedback at +0x4E, CALL vec4
 * set at +0x66; the texture setter is the first MOV RDX,[rip]; MOV RCX,RSI;
 * CALL after them. */
int sh_modvt_decode_binder(const uint8_t *code, size_t len, uint32_t rva, sh_modvt_binder *out)
{
    sh_modvt_binder b;
    size_t i;
    memset(&b, 0, sizeof b);
    if (out) *out = b;
    if (!code || !out || len < 0xC0 || code[0x4E] != 0xE8 || code[0x66] != 0xE8) return 0;
    b.feedback_rva = mv_target(rva, 0x4E, 5, code, 0x4F);
    b.set_vec4_rva = mv_target(rva, 0x66, 5, code, 0x67);
    for (i = 0x6B; i + 15 <= len && !b.set_texture_rva; ++i)
        if (code[i] == 0x48 && code[i + 1] == 0x8B && code[i + 2] == 0x15 && code[i + 7] == 0x48 && code[i + 8] == 0x8B &&
            code[i + 9] == 0xCE && code[i + 10] == 0xE8)
            b.set_texture_rva = mv_target(rva, i + 10, 5, code, i + 11);
    if (!b.set_texture_rva) return 0;
    *out = b;
    return 1;
}

/* Fixed offsets inside the signed rebind: LEA RCX,[sys] at +0x16, CALL
 * VirtualTextureForMaterial at +0x1D, CMP DWORD [vt_off],0 at +0x45, CALL
 * binder at +0x55, JMP copy at +0x62. */
int sh_modvt_decode_rebind(const uint8_t *code, size_t len, uint32_t rva, sh_modvt_rebind *out)
{
    sh_modvt_rebind r;
    memset(&r, 0, sizeof r);
    if (out) *out = r;
    if (!code || !out || len < 0x67 || memcmp(code + 0x16, "\x48\x8D\x0D", 3) || code[0x1D] != 0xE8 ||
        memcmp(code + 0x45, "\x83\x3D", 2) || code[0x4B] != 0x00 || code[0x55] != 0xE8 || code[0x62] != 0xE9)
        return 0;
    r.sys_rva = mv_target(rva, 0x16, 7, code, 0x19);
    r.vf_rva = mv_target(rva, 0x1D, 5, code, 0x1E);
    r.vt_off_rva = mv_target(rva, 0x45, 7, code, 0x47);
    r.binder_rva = mv_target(rva, 0x55, 5, code, 0x56);
    r.copy_rva = mv_target(rva, 0x62, 5, code, 0x63);
    *out = r;
    return 1;
}

/* VtViewPageTable: MOV R8,[record 1 + 0x3E8]; MOV RCX,R15; MOV RDX,[handle]; CALL.
 * VtViewFeedback: MOV RBX,[handle]; LEA RDX,[RBP+0xE0]; LEA RCX,[record 1]; CALL feedback. */
int sh_modvt_decode_view(const uint8_t *pt, size_t pt_len, uint32_t pt_rva, const uint8_t *fb, size_t fb_len,
                         uint32_t fb_rva, sh_modvt_view *out)
{
    sh_modvt_view v;
    memset(&v, 0, sizeof v);
    if (out) *out = v;
    if (!pt || !fb || !out || pt_len < 22 || fb_len < 26 || memcmp(pt, "\x4C\x8B\x05", 3) ||
        memcmp(pt + 7, "\x49\x8B\xCF\x48\x8B\x15", 6) || pt[17] != 0xE8 || memcmp(fb, "\x48\x8B\x1D", 3) ||
        memcmp(fb + 7, "\x48\x8D\x95", 3) || memcmp(fb + 14, "\x48\x8D\x0D", 3) || fb[21] != 0xE8)
        return 0;
    v.pagetable_src_rva = mv_target(pt_rva, 0, 7, pt, 3);
    v.pagetable_handle_rva = mv_target(pt_rva, 10, 7, pt, 13);
    v.feedback_handle_rva = mv_target(fb_rva, 0, 7, fb, 3);
    v.feedback_rec_rva = mv_target(fb_rva, 14, 7, fb, 17);
    v.feedback_fn_rva = mv_target(fb_rva, 21, 5, fb, 22);
    if (v.pagetable_src_rva != v.feedback_rec_rva + SH_VT_REC_PTIMAGE) return 0;
    *out = v;
    return 1;
}

uint32_t sh_modvt_decode_jobs(const uint8_t *code, size_t len, uint32_t rva)
{
    if (!code || len < 0x3A || memcmp(code + 0x2E, "\x48\x8D\x0D", 3) || code[0x35] != 0xE8) return 0;
    return mv_target(rva, 0x2E, 7, code, 0x31);
}

/* MOVZX R8D,BPL; CALL [RAX+0x80]; LEA RCX,[sys]; CALL update. */
int sh_modvt_decode_frame_call(const uint8_t *code, size_t len, uint32_t rva, uint32_t *sys_rva, uint32_t *update_rva)
{
    if (sys_rva) *sys_rva = 0;
    if (update_rva) *update_rva = 0;
    if (!code || !sys_rva || !update_rva || len < 0x16 || memcmp(code + 4, "\xFF\x90\x80\x00\x00\x00\x48\x8D\x0D", 9) ||
        code[0x11] != 0xE8)
        return 0;
    *sys_rva = mv_target(rva, 0x0A, 7, code, 0x0D);
    *update_rva = mv_target(rva, 0x11, 5, code, 0x12);
    return 1;
}

int sh_modvt_frame_head_ok(const uint8_t *code, size_t len)
{
    return code && len >= 15 && !memcmp(code, "\x48\x89\x5C\x24\x08\x48\x89\x74\x24\x10\x57\x48\x83\xEC\x50", 15);
}

int sh_modvt_decode_loader(const uint8_t *ss, size_t len, uint32_t rva, sh_modvt_loader *out)
{
    sh_modvt_loader l;
    size_t i;
    memset(&l, 0, sizeof l);
    if (out) *out = l;
    if (!ss || !out) return 0;
    for (i = 20; i + 31 <= len; ++i) {
        if (ss[i] != 0xE8 || memcmp(ss + i + 5, "\x48\x89\x86\xF0\x02\x00\x00\x48\x8D\x8D", 10) || ss[i + 19] != 0xE8)
            continue;
        if (memcmp(ss + i - 20, "\x48\x8D\x8D", 3) || ss[i - 13] != 0xE8 || ss[i - 8] != 0x90 ||
            memcmp(ss + i - 7, "\x48\x8D\x8D", 3))
            continue;
        l.loader_rva = mv_target(rva, i, 5, ss, i + 1);
        l.str_ctor_rva = mv_target(rva, i - 13, 5, ss, i - 12);
        l.str_dtor_rva = mv_target(rva, i + 19, 5, ss, i + 20);
        *out = l;
        return 1;
    }
    return 0;
}

int sh_modvt_loader_body_ok(const uint8_t *code, size_t len)
{
    size_t i;
    if (!code || len < 0x400 || memcmp(code + 0x43, "\xB9\x58\x31\x00\x00", 5)) return 0;
    for (i = 0; i + 4 <= 0x400; ++i)
        if (!memcmp(code + i, "\x21\xBB\x3F\xA6", 4)) return 1;
    return 0;
}

#ifndef SH_MODVT_PURE
/* ================================================================== engine */
#include "signatures.h"
#include "commands.h"
#include "engine_globals.h"
#include "host_image.h"
#include "hook.h"
#include "modpublished.h"
#include "process_heap_scope.h"
#include "modvt_resident.h"

/* idResource fields (images are resources): +0x28 the lifetime a map purge
 * compares; 4 lives for the session (modserve_engine.c keeps its own the same way). */
#define MV_RES_LIFETIME  0x28
#define MV_RES_SESSION   4u

typedef char (*mv_setsource_fn)(void *rec, const char *name, const char *path, int flavor, int type, char direct,
                                void *reader);
typedef void (*mv_record_fn)(void *rec);
typedef void (*mv_sys_fn)(void *sys);
typedef void (*mv_waitall_fn)(void *jobs);
typedef void (*mv_frame_fn)(void *sys);
typedef void *(*mv_vf_fn)(void *sys, void *material);
typedef void (*mv_bind_fn)(void *parms, void *rec);
typedef void (*mv_copy_fn)(void *material);

typedef void (*mv_str_ctor_fn)(void *str, const char *text);
typedef void (*mv_str_dtor_fn)(void *str);
typedef uint8_t *(*mv_loader_fn)(void *path);
typedef void (*mv_vdelete_fn)(void *self, int flags);
typedef int (*mv_vheader_fn)(void *self, void *header);

static mv_setsource_fn g_setsource;
static mv_loader_fn g_loader;        /* the mega2 loader SetSource calls (NULL: no sharing) */
static mv_str_ctor_fn g_str_ctor;
static mv_str_dtor_fn g_str_dtor;
static volatile LONG g_scan;         /* materials of shared boards to bind at the next frame */
static mv_record_fn g_release;
static mv_sys_fn g_sync;
static mv_waitall_fn g_waitall;
static void *g_jobs;
static void *const *g_pagetable_handle;   /* the per-view setter's handle globals */
static void *const *g_feedback_handle;
static const uint8_t *g_view_record;      /* the record the per-view setter reads: record 1 */
static mv_bind_fn g_binder;          /* the binder's entry (its detour runs first) */
static mv_copy_fn g_copy;
static const int32_t *g_vt_off;
static mv_vf_fn g_vf_tramp;
static mv_bind_fn g_bind_tramp;
static mv_frame_fn g_frame_tramp;
static sh_modvt_bind_ops g_bind_ops;
static volatile LONG g_handles;      /* 0 not tried, 1 found, -1 missing */
static uint8_t *g_sys;
static uint32_t g_table;
static int g_vulkan = -1;
static int g_bound;
static sh_process_heap_api g_heap;
static const uint8_t *g_main_thread_at;
static volatile LONG g_pending;
static volatile LONG g_waiting_logged;

static int mv_rd_ptr(const void *p, void **v) { __try { *v = *(void *const *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { *v = NULL; return 0; } }
static int mv_rd_i32(const void *p, int *v) { __try { *v = *(const int *)p; return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { *v = 0; return 0; } }
static int mv_rd_code(const uint8_t *p, uint8_t *out, size_t n) { __try { memcpy(out, p, n); return 1; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

/* The engine's C string at `slot` (a char *), whole (malloc'd); NULL when
 * there is none or it does not read. */
static char *mv_rd_cstr(const void *slot)
{
    const char *s = NULL;
    char *out = NULL;
    if (!mv_rd_ptr(slot, (void **)&s) || !s) return NULL;
    __try {
        size_t n = strlen(s);
        if ((out = (char *)malloc(n + 1)) != NULL) memcpy(out, s, n + 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) { free(out); out = NULL; }
    return out;
}

/* 1 when the engine's C string at `slot` is exactly `name` (compared in
 * place, whole: the detours run per material bind). */
static int mv_cstr_is(const void *slot, const char *name)
{
    const char *s = NULL;
    int same = 0;
    if (!name || !mv_rd_ptr(slot, (void **)&s) || !s) return 0;
    __try { same = !strcmp(s, name); }
    __except (EXCEPTION_EXECUTE_HANDLER) { same = 0; }
    return same;
}

/* 1 when the engine's C string at `slot` is empty or absent. */
static int mv_cstr_empty(const void *slot)
{
    const char *s = NULL;
    int empty = 1;
    if (!mv_rd_ptr(slot, (void **)&s) || !s) return 1;
    __try { empty = !s[0]; }
    __except (EXCEPTION_EXECUTE_HANDLER) { empty = 1; }
    return empty;
}

/* The console only from the main thread; the log always. */
static void mv_echo(const char *line)
{
    if (sh_modvt_engine_on_main() == 1) sh_printf("%s\n", line);
}

uint8_t *sh_modvt_engine_record(int k)
{
    void *r = NULL;
    if (!g_bound || k < 0 || k >= SH_MODVT_SLOTS) return NULL;
    mv_rd_ptr(g_sys + g_table + 8 * k, &r);
    return (uint8_t *)r;
}

int sh_modvt_engine_bound(void) { return g_bound; }
int sh_modvt_engine_vulkan(void) { return g_vulkan; }
uint8_t *sh_modvt_engine_sys(void) { return g_sys; }
uint32_t sh_modvt_engine_table(void) { return g_table; }

int sh_modvt_engine_on_main(void)
{
    uint32_t id = 0;
    if (!g_main_thread_at) return -1;
    __try { id = *(const uint32_t *)g_main_thread_at; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    if (!id) return -1;
    return id == GetCurrentThreadId() ? 1 : 0;
}

/* Record 1 is the game's own board, built by the VT start-up; until then no
 * board may be registered. */
int sh_modvt_engine_ready(void)
{
    uint8_t *r = sh_modvt_engine_record(1);
    char *name;
    int type = 0, ready;
    if (!r) return 0;
    name = mv_rd_cstr(r + SH_VT_REC_NAME);
    mv_rd_i32(r + SH_VT_REC_TYPE, &type);
    ready = type == 2 && name && strstr(name, "_vmtr") != NULL;
    free(name);
    return ready;
}

static DWORD g_fault_code;
static int mv_filter(DWORD code) { g_fault_code = code; return EXCEPTION_EXECUTE_HANDLER; }

/* No job may read the record table while a record changes: wait for every
 * job list, as the engine does before it releases records at a map unload.
 * Main thread only; the main thread kicks no job until it returns. */
static int mv_quiesce(void)
{
    __try { g_waitall(g_jobs); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

int sh_modvt_engine_set_source(int k, const char *name, const char *path, int flavor, int *result, double *ms,
                               unsigned *fault)
{
    sh_process_heap_scope heap = { 0 };
    uint8_t *r = sh_modvt_engine_record(k);
    int ok = 0;
    char ret = 0;
    LARGE_INTEGER f, a, b;
    if (result) *result = 0;
    if (ms) *ms = 0;
    if (fault) *fault = 0;
    if (!g_bound || !r || k < 1 || k > SH_MODVT_LAST_USED || sh_modvt_engine_on_main() != 1) return 0;
    if (!mv_quiesce()) return 0;
    if (!sh_process_heap_enter(&g_heap, &heap)) return 0;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    g_fault_code = 0;
    __try {
        __try { ret = g_setsource(r, name, path, flavor, 2, 0, NULL); ok = 1; }
        __except (mv_filter(GetExceptionCode())) { ok = 0; }
    } __finally {
        if (!sh_process_heap_leave(&heap)) mv_log("SetSource: the process heap scope did not balance");
    }
    QueryPerformanceCounter(&b);
    if (ms) *ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    if (!ok) { if (fault) *fault = (unsigned)g_fault_code; return -1; }
    if (result) *result = ret ? 1 : 0;
    return 1;
}

/* SetSource's own head: synchronise the VT system's jobs, then Release. */
int sh_modvt_engine_release(int k, unsigned *fault)
{
    sh_process_heap_scope heap = { 0 };
    uint8_t *r = sh_modvt_engine_record(k);
    int ok = 0;
    if (fault) *fault = 0;
    if (!g_bound || !r || k < 1 || k > SH_MODVT_LAST_USED || sh_modvt_engine_on_main() != 1) return 0;
    if (!mv_quiesce()) return 0;
    if (!sh_process_heap_enter(&g_heap, &heap)) return 0;
    g_fault_code = 0;
    __try {
        __try { g_sync(g_sys); g_release(r); ok = 1; }
        __except (mv_filter(GetExceptionCode())) { ok = 0; }
    } __finally {
        if (!sh_process_heap_leave(&heap)) mv_log("Release: the process heap scope did not balance");
    }
    if (!ok) { if (fault) *fault = (unsigned)g_fault_code; return -1; }
    return 1;
}

/* The renderParm handles the lit programs read: the ones the per-view
 * setter fills from record 1, read from its own globals once the renderer
 * has set them; the record it reads must be the table's record 1. */
static int mv_handles(void)
{
    LONG state = InterlockedCompareExchange(&g_handles, 0, 0);
    void *fb = NULL, *pt = NULL;
    if (state) return state > 0;
    mv_rd_ptr(g_feedback_handle, &fb);
    mv_rd_ptr(g_pagetable_handle, &pt);
    if (!fb || !pt) return 0;   /* not set yet: asked again at the next sync */
    if (sh_modvt_engine_record(1) != g_view_record) {
        InterlockedExchange(&g_handles, -1);
        mv_log("boards unavailable: the per-view setter reads record %p, the table's record 1 is %p", (void *)g_view_record,
               (void *)sh_modvt_engine_record(1));
        return 0;
    }
    g_bind_ops.feedback_handle = fb;
    g_bind_ops.pagetable_handle = pt;
    {
        /* virtualMapping, for boards placed in a shared record: the renderParm decl the engine's lookup answers, which
         * is what a handle global holds; checked on the feedback handle, read from the per-view setter's global. */
        void *vm = g_loader ? sh_modserve_engine_decl("renderParm", "virtualMapping") : NULL;
        void *vfb = g_loader ? sh_modserve_engine_decl("renderParm", "vmtrFeedbackFloat") : NULL;
        if (vm && vfb == fb) g_bind_ops.mapping_handle = vm;
        else
            mv_log("boards that find no free record cannot share one this session: %s",
                   !g_loader ? "the mega2 loader did not decode"
                   : !vm     ? "renderParm virtualMapping was not found"
                             : "a renderParm lookup does not answer the handle the renderer holds");
    }
    InterlockedExchange(&g_handles, 1);
    return 1;
}

/* ------------------------------------------------------------------ detours */

static void *mv_vf_detour(void *sys, void *material)
{
    void *rec = NULL;
    int i;
    if (material && InterlockedCompareExchange(&g_nowned, 0, 0)) {
        /* The landpagefile name, compared whole in place with each board held. */
        const void *slot = (const uint8_t *)material + SH_VT_MAT_LANDPAGE;
        AcquireSRWLockShared(&g_lock);
        for (i = 0; i < SH_MODVT_SLOTS && !rec; ++i)
            if (g_owned[i].used && g_owned[i].rec && g_owned[i].name && mv_cstr_is(slot, g_owned[i].name))
                rec = g_owned[i].rec;
        for (i = 0; i < g_nguests && !rec; ++i)
            if (g_guests[i].used && !g_guests[i].stale && g_guests[i].rec && mv_cstr_is(slot, g_guests[i].name))
                rec = g_guests[i].rec;
        ReleaseSRWLockShared(&g_lock);
        if (rec) return rec;
    }
    return g_vf_tramp(sys, material);
}

static void mv_bind_detour(void *parms, void *rec)
{
    g_bind_tramp(parms, rec);
    if (InterlockedCompareExchange(&g_nowned, 0, 0)) (void)sh_modvt_bind_board(parms, rec, &g_bind_ops);
}

/* ------------------------------------------------------------------ ops */

static int op_ready(void *c) { (void)c; return sh_modvt_engine_ready(); }

static int op_find(void *c, const char *name)
{
    int k;
    (void)c;
    if (!name || !name[0]) return -1;
    for (k = 1; k < SH_MODVT_SLOTS; ++k) {
        uint8_t *r = sh_modvt_engine_record(k);
        if (r && mv_cstr_is(r + SH_VT_REC_NAME, name)) return k;
    }
    return -1;
}

static int op_is_free(void *c, int k)
{
    uint8_t *r = sh_modvt_engine_record(k);
    (void)c;
    if (!r) return 0;
    return mv_cstr_empty(r + SH_VT_REC_PATH) && mv_cstr_empty(r + SH_VT_REC_NAME);
}

/* Physical hash entries retain four of the feedback word's five source bits. Only records sharing
 * a physical pool can collide. Record 31 is the engine's scratch record. */
static int op_pool_conflict(void *c, int k)
{
    uint8_t *one = sh_modvt_engine_record(1), *mate = NULL;
    void *pool = NULL, *other = NULL, *current = NULL;
    uint8_t *r = sh_modvt_engine_record(k);
    (void)c;
    if (!one || !mv_rd_ptr(one + SH_VT_REC_CACHE, &pool) || !pool || !r || !mv_rd_ptr(r + SH_VT_REC_CACHE, &current))
        return 1;
    if (!mv_rd_ptr(g_sys + g_table + 8 * (k ^ 16), (void **)&mate) || (mate && !mv_rd_ptr(mate + SH_VT_REC_CACHE, &other)))
        return 1;
    if (!mv_cstr_empty(r + SH_VT_REC_NAME) && current != pool) return 1;
    return mate && other == pool && (!mv_cstr_empty(mate + SH_VT_REC_PATH) || !mv_cstr_empty(mate + SH_VT_REC_NAME));
}

void *sh_modvt_owned_reader(int k, const void *pool)
{
    uint8_t *r = sh_modvt_engine_record(k);
    void *found = NULL, *cache = NULL, *reader = NULL, *mega = NULL;
    if (!r || k < 2 || k > SH_MODVT_LAST_USED) return NULL;
    AcquireSRWLockShared(&g_lock);
    for (int i = 0; i < SH_MODVT_SLOTS; ++i) {
        mv_owned *o = &g_owned[i];
        if (!o->used || o->record != k || o->rec != r) continue;
        if (mv_rd_ptr(r + SH_VT_REC_CACHE, &cache) && cache == pool && mv_cstr_is(r + SH_VT_REC_NAME, o->name) &&
            mv_rd_ptr(r + SH_VT_REC_READER, &reader) && reader && mv_rd_ptr(r + SH_VT_REC_MEGA2, &mega) &&
            reader == mega && !op_pool_conflict(NULL, k))
            found = reader;
        break;
    }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

static int op_set_source(void *c, int k, const char *name, const char *path)
{
    int result = 0;
    double ms = 0;
    unsigned fault = 0;
    int r = sh_modvt_engine_set_source(k, name, path, SH_MODVT_FLAVOR, &result, &ms, &fault);
    (void)c;
    if (r < 0) { mv_log("SetSource(record %d, '%s') FAULTED (exception 0x%08X)", k, name, fault); return -1; }
    if (r == 0) { mv_log("SetSource(record %d, '%s') not called (heap scope or main thread)", k, name); return 0; }
    if (result) {
        short levels = 0;
        unsigned was = 0;
        uint8_t *image = NULL;
        /* The page-table image is a resource of the level it was made in: the
         * next map load purges it while the record, and every material bound
         * to the board, still points at it. A board lives for the session. */
        __try {
            levels = *(const short *)(sh_modvt_engine_record(k) + SH_VT_REC_LEVELS);
            image = *(uint8_t *const *)(sh_modvt_engine_record(k) + SH_VT_REC_PTIMAGE);
            if (image) {
                was = *(const unsigned *)(image + MV_RES_LIFETIME);
                *(unsigned *)(image + MV_RES_LIFETIME) = MV_RES_SESSION;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { levels = 0; image = NULL; }
        if (!image) { mv_log("SetSource(record %d, '%s') left no page-table image", k, name); return 0; }
        mv_log("SetSource(record %d, '%s', '%s', flavor %d, type 2) -> 1 in %.1f ms; %d levels; page-table image "
               "lifetime %u -> %u (the session)", k, name, path, SH_MODVT_FLAVOR, ms, (int)levels, was, MV_RES_SESSION);
    }
    return result ? 1 : 0;
}

/* Record 1's cache is the material-format one (flavor 0). The record has no
 * material and no request yet; the VT jobs are synchronised first so no job
 * holds the cache it had. */
static int op_adopt_cache(void *c, int k)
{
    uint8_t *one = sh_modvt_engine_record(1), *r = sh_modvt_engine_record(k);
    void *cache = NULL;
    int flavor = -1, ok = 0;
    (void)c;
    if (!one || !r || k < 2 || !mv_rd_ptr(one + SH_VT_REC_CACHE, &cache) || !cache || !mv_rd_i32(cache, &flavor) ||
        flavor != 0)
        return 0;
    if (!mv_quiesce()) return 0;
    __try { g_sync(g_sys); *(void **)(r + SH_VT_REC_CACHE) = cache; ok = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    return ok;
}

static int op_release(void *c, int k)
{
    unsigned fault = 0;
    int r = sh_modvt_engine_release(k, &fault);
    (void)c;
    if (r < 0) mv_log("Release(record %d) FAULTED (exception 0x%08X)", k, fault);
    return r == 1;
}

static void *op_record(void *c, int k) { (void)c; return sh_modvt_engine_record(k); }

/* The engine's own rebind without its lookup: index and type, the binder
 * (through its detour, which adds the board's parameters), the block copy. */
static int op_rebind(void *c, void *material, int k, const char *name)
{
    sh_process_heap_scope heap = { 0 };
    uint8_t *m = (uint8_t *)material, *r = sh_modvt_engine_record(k);
    int type = 0, ok = 0;
    (void)c;
    if (!m || !r || sh_modvt_engine_on_main() != 1) return 0;
    if (!mv_cstr_is(m + SH_VT_MAT_LANDPAGE, name) || !mv_rd_i32(r + SH_VT_REC_TYPE, &type)) return 0;
    if (!mv_quiesce()) return 0;
    if (!sh_process_heap_enter(&g_heap, &heap)) return 0;
    __try {
        __try {
            *(int *)(m + SH_VT_MAT_RECORD) = k;
            *(int *)(m + SH_VT_MAT_RECTYPE) = type;
            if (*g_vt_off == 0) {
                g_binder(m + SH_VT_MAT_PARMS, r);
                g_copy(m);
            }
            ok = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    } __finally {
        if (!sh_process_heap_leave(&heap)) mv_log("rebind: the process heap scope did not balance");
    }
    return ok;
}

static void op_notice(void *c, const char *text) { (void)c; sh_modpublished_notice(text); }

/* A board's members into free squares of the family record k holds: the engine's own loader reads them (through the
 * provider, as SetSource would), then they move into the record's page source and the temporary one is deleted with
 * nothing left to free. The record's header is filled again by the page source itself, as SetSource fills it. Main
 * thread, every job list finished, the VT system synchronised, inside process heap 0. */
static int op_join(void *c, int k, const char *path, const unsigned char to[16])
{
    sh_process_heap_scope heap = { 0 };
    uint8_t *r = sh_modvt_engine_record(k), *mega = NULL, *t = NULL;
    __declspec(align(16)) uint8_t str[0x100];
    int ok = 0, moved = 0, same = 0;
    (void)c;
    if (!g_loader || !g_bind_ops.mapping_handle || !r || !path || sh_modvt_engine_on_main() != 1) return 0;
    if (!mv_rd_ptr(r + SH_VT_REC_MEGA2, (void **)&mega) || !mega) return 0;
    __try { same = sh_modvt_mega_ok(mega); } __except (EXCEPTION_EXECUTE_HANDLER) { same = 0; }
    if (!same) return 0;
    if (!mv_quiesce()) return 0;
    if (!sh_process_heap_enter(&g_heap, &heap)) return 0;
    g_fault_code = 0;
    __try {
        __try {
            g_sync(g_sys);
            memset(str, 0, sizeof str);
            g_str_ctor(str, path);
            t = g_loader(str);
            g_str_dtor(str);
            if (t) {
                if (*(void **)t == *(void **)mega && sh_modvt_mega_ok(t)) moved = sh_modvt_mega_move(mega, t, to);
                /* What it still holds (nothing once moved) is freed by its own destructor. */
                ((mv_vdelete_fn)(*(void ***)t)[0])(t, 1);
                if (moved > 0) ((mv_vheader_fn)(*(void ***)mega)[1])(mega, r + SH_VT_REC_MAGIC);
            }
            ok = 1;
        } __except (mv_filter(GetExceptionCode())) { ok = 0; }
    } __finally {
        if (!sh_process_heap_leave(&heap)) mv_log("join: the process heap scope did not balance");
    }
    if (!ok) { mv_log("placing '%s' in record %d FAULTED (exception 0x%08X)", path, k, (unsigned)g_fault_code); return -1; }
    if (moved > 0) InterlockedExchange(&g_scan, 1);
    return moved > 0 ? 1 : 0;
}

static const sh_modvt_ops g_ops = { NULL, op_ready, op_find, op_is_free, op_set_source, op_adopt_cache, op_release,
                                    op_record, op_rebind, op_notice, op_join, op_pool_conflict };

/* The material's vec4 for a renderParm handle (its parm index at +0x68, as the setter reads it). */
static int mv_get_vec4(void *parms, void *handle, float *out4)
{
    int found = 0;
    __try { found = sh_modvt_parm_get(parms, *(const int *)((const uint8_t *)handle + 0x68), out4); }
    __except (EXCEPTION_EXECUTE_HANDLER) { found = 0; }
    return found;
}

/* ------------------------------------------------------------------ the shared boards' materials */

/* The material parser binds a material to the record whose name is its landpagefile; a board placed in a shared
 * record has none, so its materials parse unbound (no page table). At the frame after a placement or a decl pass,
 * each parsed material of every placed board whose landpagefile names it is bound to the record with the engine's
 * rebind: index and type, the binder (whose detour adds the record's parameters and moves the mapping), the copy. */
typedef struct mv_scan_guest { char *name; char hash[65]; int record; } mv_scan_guest;
typedef struct mv_scan { mv_scan_guest *g; int n, cap, bound, seen; } mv_scan;

static void mv_scan_add(void *ctx, const char *name, const char *hash, int record)
{
    mv_scan *s = (mv_scan *)ctx;
    if (s->n == s->cap) {
        int cap = s->cap ? s->cap * 2 : 8;
        mv_scan_guest *grown = (mv_scan_guest *)realloc(s->g, sizeof *grown * (size_t)cap);
        if (!grown) return;
        s->g = grown;
        s->cap = cap;
    }
    if (!(s->g[s->n].name = sh_text(name))) return;
    strcpy_s(s->g[s->n].hash, sizeof s->g[0].hash, hash);
    s->g[s->n++].record = record;
}

static void mv_scan_file(void *ctx, const char *request, uint64_t size, const char *bundle_id, const char *bundle_hash)
{
    mv_scan *s = (mv_scan *)ctx;
    char type[SH_MB_TYPE_CAP], *name = NULL;
    void *obj = NULL;
    unsigned state = 0, life = 0;
    int i;
    (void)size; (void)bundle_id;
    if (!request || !bundle_hash || _strnicmp(request, "generated/decls/material", 24)) return;
    for (i = 0; i < s->n && strcmp(s->g[i].hash, bundle_hash); ++i) {}
    if (i == s->n) return;
    if (!sh_mb_logical_name(request, type, &name) || !name ||
        (_stricmp(type, "material2") && _stricmp(type, "material"))) { free(name); return; }
    if (sh_modserve_engine_decl_held(type, name, &obj, &state, &life) == 1 && obj && !(state & 3)) {
        int j, k = -1;
        s->seen++;
        for (j = 0; j < s->n; ++j)
            if (!strcmp(s->g[j].hash, bundle_hash) && mv_cstr_is((uint8_t *)obj + SH_VT_MAT_LANDPAGE, s->g[j].name)) {
                k = j;
                break;
            }
        if (k >= 0) {
            int bound = -1;
            mv_rd_i32((uint8_t *)obj + SH_VT_MAT_RECORD, &bound);
            if (bound != s->g[k].record && op_rebind(NULL, obj, s->g[k].record, s->g[k].name) == 1) s->bound++;
        }
    }
    free(name);
}

static void mv_scan_guests(const char *phase)
{
    mv_scan s;
    int i;
    memset(&s, 0, sizeof s);
    sh_modvt_each_guest(mv_scan_add, &s);
    if (s.n) sh_modserve_each_served_here(mv_scan_file, &s);
    if (s.bound)
        mv_log("%s: %d material%s of boards in shared records bound to their records (%d parsed)", phase, s.bound,
               s.bound == 1 ? "" : "s", s.seen);
    for (i = 0; i < s.n; ++i) free(s.g[i].name);
    free(s.g);
}

/* ------------------------------------------------------------------ points */

static void mv_sync_now(const char *phase, int release_now)
{
    sh_modvt_family *fams = NULL;
    sh_modvt_result res;
    int n;
    /* Before the VT start-up builds record 1 nothing is read or called. */
    if (!sh_modvt_engine_ready()) {
        if (!InterlockedExchange(&g_waiting_logged, 1))
            mv_log("%s: the virtual-texture system is not up yet; boards are registered once it is", phase);
        InterlockedExchange(&g_pending, 1);
        return;
    }
    n = sh_modvt_collect(&fams);
    if (!n && !sh_modvt_owned_count()) { sh_modvt_families_free(fams, n); InterlockedExchange(&g_pending, 0); return; }
    if (!mv_handles()) {
        /* Not set yet (the renderer is still starting), or unavailable for the session. */
        InterlockedExchange(&g_pending, InterlockedCompareExchange(&g_handles, 0, 0) == 0);
        sh_modvt_families_free(fams, n);
        return;
    }
    if (sh_modvt_sync(fams, n, &g_ops, phase, release_now, &res) < 0) InterlockedExchange(&g_pending, 1);
    else InterlockedExchange(&g_pending, res.pending ? 1 : 0);
    sh_modvt_families_free(fams, n);
}

/* The serving index listener: any thread. The next frame boundary syncs. */
static void mv_index_changed(void)
{
    if (g_bound) InterlockedExchange(&g_pending, 1);
}

void sh_modvt_before_pass(const char *phase)
{
    if (!g_bound || sh_modvt_engine_on_main() != 1) return;
    mv_sync_now(phase, 0);
    /* The pass parses materials; those of boards in shared records are bound at the next frame. */
    if (sh_modvt_guest_count()) InterlockedExchange(&g_scan, 1);
}

/* The VT frame update, in EndFrame right after the render-thread sync: the
 * point the engine changes VT state itself. A queued sync runs first. */
static void mv_frame_detour(void *sys)
{
    static ULONGLONG next_check;
    ULONGLONG now = GetTickCount64();
    if (g_bound && sh_modvt_owned_count() && now >= next_check && sh_modvt_engine_on_main() == 1) {
        next_check = now + 1000;
        for (int i = 0; i < SH_MODVT_SLOTS; ++i) {
            mv_owned *o = &g_owned[i];
            if (!o->used) continue;
            uint8_t *r = sh_modvt_engine_record(o->record);
            if (r != o->rec || !r || !mv_cstr_is(r + SH_VT_REC_NAME, o->name) || op_pool_conflict(NULL, o->record)) {
                InterlockedExchange(&g_pending, 1);
                break;
            }
        }
    }
    if (g_bound && InterlockedCompareExchange(&g_pending, 0, 0) && sh_modvt_engine_on_main() == 1)
        mv_sync_now("frame", 1);
    if (g_bound && InterlockedCompareExchange(&g_scan, 0, 0) && sh_modvt_engine_on_main() == 1) {
        InterlockedExchange(&g_scan, 0);
        mv_scan_guests("frame");
    }
    if (g_bound && sh_modvt_engine_on_main() == 1) sh_modvt_resident_frame();
    g_frame_tramp(sys);
}

void sh_modvt_print_status(void)
{
    int k, n = sh_modvt_collect(NULL), first = 1;
    char held[160] = "";
    size_t used = 0;
    for (k = 1; k < SH_MODVT_SLOTS; ++k)
        if (sh_modvt_record_owner(k)) {
            int w = _snprintf_s(held + used, sizeof held - used, _TRUNCATE, "%s%d", first ? "" : ", ", k);
            if (w > 0) used += (size_t)w;
            first = 0;
        }
    sh_printf("VT: boards: %d served, %d registered%s%s%s, %d placed in shared records, %d material%s bound; %s, %s%s\n",
              n, sh_modvt_owned_count(), held[0] ? " (records " : "", held, held[0] ? ")" : "", sh_modvt_guest_count(),
              sh_modvt_material_count(NULL),
              sh_modvt_material_count(NULL) == 1 ? "" : "s", g_bound ? "registration bound" : "registration unavailable",
              g_vulkan == 1 ? "Vulkan" : g_vulkan == 0 ? "OpenGL" : "unknown renderer",
              g_pending ? "; a sync is pending" : "");
}

/* sh_vt sync: run the sync now from the console (main thread). */
void sh_modvt_console_sync(void)
{
    if (!g_bound) { sh_printf("VT: registration unavailable on this build\n"); return; }
    if (sh_modvt_engine_on_main() != 1) { sh_printf("VT: sync refused: not on the main thread\n"); return; }
    mv_sync_now("console", 1);
    sh_modvt_print_status();
}


int sh_modvt_install(const sig_result *results, size_t count, const uint8_t *module_base)
{
    static const char *const names[] = { "VtSetSource", "VtForMaterial", "VtMaterialBind", "VtMaterialRebind",
                                         "VtViewPageTable", "VtViewFeedback", "JobsWaitAll", "VtFrameCall" };
    const sig_result *s[8];
    const sh_build_identity *id = sh_host_identity();
    uint8_t sc[0xC0], vc[0xC0], bc[0xC0], rc[0x70], relc[0x50], pc[0x20], fc[0x20], jc[0x40], tc[0x20], uc[0x10];
    uint32_t frame_sys = 0, frame_update = 0;
    sh_modvt_layout l;
    sh_modvt_binder b;
    sh_modvt_rebind rb;
    sh_modvt_view v;
    uint32_t jobs;
    size_t i;
    g_echo = mv_echo;
    g_vulkan = id ? id->vulkan : -1;
    g_main_thread_at = (const uint8_t *)glb_resolve(module_base, "main_thread_id", NULL);
    for (i = 0; i < 8; ++i)
        if ((s[i] = sig_ok(results, count, names[i])) == NULL) {
            mv_log("boards unavailable: %s is not a clean signature match", names[i]);
            return 0;
        }
    if (!sh_process_heap_bind(&g_heap, results, count, module_base) || !g_main_thread_at) {
        mv_log("boards unavailable: %s", g_main_thread_at ? "the process heap scope is unbound" : "main_thread_id is unresolved");
        return 0;
    }
    if (!mv_rd_code((const uint8_t *)s[0]->addr, sc, sizeof sc) || !mv_rd_code((const uint8_t *)s[1]->addr, vc, sizeof vc) ||
        !mv_rd_code((const uint8_t *)s[2]->addr, bc, sizeof bc) || !mv_rd_code((const uint8_t *)s[3]->addr, rc, sizeof rc) ||
        !sh_modvt_decode_layout(sc, sizeof sc, s[0]->rva, vc, sizeof vc, s[1]->rva, &l) ||
        !mv_rd_code(module_base + l.release_rva, relc, sizeof relc) || !sh_modvt_release_head_ok(relc, sizeof relc) ||
        !sh_modvt_decode_binder(bc, sizeof bc, s[2]->rva, &b) || !sh_modvt_decode_rebind(rc, sizeof rc, s[3]->rva, &rb) ||
        !mv_rd_code((const uint8_t *)s[4]->addr, pc, sizeof pc) || !mv_rd_code((const uint8_t *)s[5]->addr, fc, sizeof fc) ||
        !mv_rd_code((const uint8_t *)s[6]->addr, jc, sizeof jc) || !mv_rd_code((const uint8_t *)s[7]->addr, tc, sizeof tc) ||
        !sh_modvt_decode_frame_call(tc, sizeof tc, s[7]->rva, &frame_sys, &frame_update) ||
        !mv_rd_code(module_base + frame_update, uc, sizeof uc) || !sh_modvt_frame_head_ok(uc, sizeof uc) ||
        frame_sys != l.sys_rva ||
        !sh_modvt_decode_view(pc, sizeof pc, s[4]->rva, fc, sizeof fc, s[5]->rva, &v) ||
        !(jobs = sh_modvt_decode_jobs(jc, sizeof jc, s[6]->rva)) || rb.sys_rva != l.sys_rva || rb.vf_rva != s[1]->rva ||
        rb.binder_rva != s[2]->rva || v.feedback_fn_rva != b.feedback_rva) {
        mv_log("boards unavailable: the VT system, its record table, Release, the binder's setters, the rebind, the "
               "per-view handles, the job manager or the VT frame update could not be decoded from the signed code");
        return 0;
    }
    g_sys = (uint8_t *)module_base + l.sys_rva;
    g_table = l.table_off;
    g_sync = (mv_sys_fn)(module_base + l.sync_rva);
    g_release = (mv_record_fn)(module_base + l.release_rva);
    g_setsource = (mv_setsource_fn)s[0]->addr;
    g_waitall = (mv_waitall_fn)s[6]->addr;
    g_jobs = (void *)(module_base + jobs);
    g_pagetable_handle = (void *const *)(module_base + v.pagetable_handle_rva);
    g_feedback_handle = (void *const *)(module_base + v.feedback_handle_rva);
    g_view_record = module_base + v.feedback_rec_rva;
    g_binder = (mv_bind_fn)s[2]->addr;
    g_copy = (mv_copy_fn)(module_base + rb.copy_rva);
    g_vt_off = (const int32_t *)(module_base + rb.vt_off_rva);
    g_bind_ops.feedback = (float *(*)(void *, float *))(module_base + b.feedback_rva);
    g_bind_ops.set_vec4 = (void (*)(void *, void *, const float *))(module_base + b.set_vec4_rva);
    g_bind_ops.set_texture = (void (*)(void *, void *, void *))(module_base + b.set_texture_rva);
    g_bind_ops.get_vec4 = mv_get_vec4;
    {
        /* Sharing a record: SetSource's own call of the mega2 loader, the idStr it builds for it, and the loader's
         * constants for the page-source layout. Without them a board that finds no record draws plain, as before. */
        static uint8_t ssc[0x600], ldc[0x400];
        sh_modvt_loader ld;
        if (mv_rd_code((const uint8_t *)s[0]->addr, ssc, sizeof ssc) && sh_modvt_decode_loader(ssc, sizeof ssc, s[0]->rva, &ld) &&
            mv_rd_code(module_base + ld.loader_rva, ldc, sizeof ldc) && sh_modvt_loader_body_ok(ldc, sizeof ldc)) {
            g_loader = (mv_loader_fn)(module_base + ld.loader_rva);
            g_str_ctor = (mv_str_ctor_fn)(module_base + ld.str_ctor_rva);
            g_str_dtor = (mv_str_dtor_fn)(module_base + ld.str_dtor_rva);
        } else
            mv_log("boards that find no free record cannot share one: the mega2 loader inside SetSource did not decode");
    }
    /* Both prologues: pushes and SUB RSP, then the security-cookie load
     * MOV RAX,[rip] (VirtualTextureForMaterial at +11, the binder at +10),
     * which the trampoline rewrites. */
    g_vf_tramp = (mv_vf_fn)hook_prepare_rip_load((void *)s[1]->addr, (void *)mv_vf_detour, 18, 11);
    g_bind_tramp = (mv_bind_fn)hook_prepare_rip_load((void *)s[2]->addr, (void *)mv_bind_detour, 17, 10);
    /* The frame update's 15-byte prologue is whole and position independent. */
    g_frame_tramp = (mv_frame_fn)hook_prepare((void *)(module_base + frame_update), (void *)mv_frame_detour, 15);
    if (!g_vf_tramp || !g_bind_tramp || !g_frame_tramp || hook_commit((void *)g_vf_tramp) != B2_PATCH_OK ||
        hook_commit((void *)g_bind_tramp) != B2_PATCH_OK || hook_commit((void *)g_frame_tramp) != B2_PATCH_OK) {
        mv_log("boards unavailable: the material lookup, binder and frame update detours could not be installed (%s)",
               hook_last_failure());
        if (g_frame_tramp && hook_unpatch((void *)g_frame_tramp)) g_frame_tramp = NULL;
        if (g_bind_tramp && hook_unpatch((void *)g_bind_tramp)) g_bind_tramp = NULL;
        if (g_vf_tramp && hook_unpatch((void *)g_vf_tramp)) g_vf_tramp = NULL;
        return 0;
    }
    g_bound = 1;
    sh_modvt_resident_install(sig_ok(results, count, "VtFrameFeedback"), sig_ok(results, count, "VtCoarseCells"), module_base);
    mv_log("boards bound on %s: SetSource rva 0x%X, VirtualTextureForMaterial rva 0x%X and binder rva 0x%X (detoured), "
           "rebind rva 0x%X, VT system rva 0x%X, record table sys+0x%X",
           g_vulkan == 1 ? "Vulkan" : g_vulkan == 0 ? "OpenGL" : "an unknown image", s[0]->rva, s[1]->rva, s[2]->rva,
           s[3]->rva, l.sys_rva, l.table_off);
    sh_modserve_set_index_listener(mv_index_changed);
    if (sh_modvt_collect(NULL)) InterlockedExchange(&g_pending, 1);
    return 1;
}
#endif /* SH_MODVT_PURE */
