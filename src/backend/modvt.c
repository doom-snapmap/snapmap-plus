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
#include "modvt.h"
#include "modserve.h"
#include "modbundle.h"
#include "backend_log.h"

/* ------------------------------------------------------------------ log */

static void (*g_echo)(const char *line);   /* the console, once commands exist */

static void mv_log(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    memcpy(line, "VT: ", 4);
    va_start(ap, fmt);
    _vsnprintf_s(line + 4, sizeof line - 4, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
    if (g_echo) g_echo(line);
}

/* ------------------------------------------------------------------ boards served */

typedef struct mv_collect { sh_modvt_family *out; int n, cap, oom; } mv_collect;

static void mv_collect_file(void *context, const char *request, uint64_t size, const char *bundle_id,
                            const char *bundle_hash)
{
    mv_collect *c = (mv_collect *)context;
    char fam[SH_MODVT_NAME_MAX];
    int member = 0, i;
    sh_modvt_family *f;
    if (!sh_mb_vt_member(request, fam, sizeof fam, &member)) return;
    for (i = 0; i < c->n; ++i)
        if (!strcmp(c->out[i].name, fam)) {
            /* Two salts serving one board name: the first keeps it. */
            if (!strcmp(c->out[i].bundle_hash, bundle_hash)) {
                c->out[i].members |= 1u << (member - 1);
                c->out[i].bytes += size;
            }
            return;
        }
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 8;
        sh_modvt_family *grown = (sh_modvt_family *)realloc(c->out, sizeof *grown * (size_t)cap);
        if (!grown) { c->oom = 1; return; }
        c->out = grown;
        c->cap = cap;
    }
    f = &c->out[c->n++];
    memset(f, 0, sizeof *f);
    strcpy_s(f->name, sizeof f->name, fam);
    _snprintf_s(f->path, sizeof f->path, _TRUNCATE, "virtualtextures/%s.pages", fam);
    _snprintf_s(f->bundle, sizeof f->bundle, _TRUNCATE, "%s", bundle_id);
    strncpy_s(f->bundle_hash, sizeof f->bundle_hash, bundle_hash, _TRUNCATE);
    f->members = 1u << (member - 1);
    f->bytes = size;
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
    else free(c.out);
    return c.n;
}

/* ------------------------------------------------------------------ boards held */

typedef struct mv_owned {
    int used;             /* 1 held; 0 free, or (name set) a record released or taken back */
    int record;
    void *rec;            /* the record's address, while held */
    char name[SH_MODVT_NAME_MAX];
    char path[SH_MODVT_PATH_MAX];
    char bundle[SH_MODVT_NAME_MAX];
    char bundle_hash[65];
} mv_owned;

/* The detours read the boards and write the material list on whatever
 * thread binds materials; the sync changes them on the main thread. The
 * lock is never held across an engine call. */
static SRWLOCK g_lock = SRWLOCK_INIT;
static mv_owned g_owned[SH_MODVT_SLOTS];
static volatile LONG g_nowned;   /* boards held: the detours' idle test */

/* Materials bound to a board, by board name, so a board registered again
 * binds them again. Kept across releases. */
typedef struct mv_material { void *material; char name[SH_MODVT_NAME_MAX]; } mv_material;
static mv_material *g_mats;
static int g_nmats, g_capmats;

/* Refusals already logged, so a pass does not repeat them: (name, build, cause). */
typedef struct mv_refusal { char name[SH_MODVT_NAME_MAX]; char hash[65]; int cause; } mv_refusal;
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
    strcpy_s(g_refused[g_nrefused].name, sizeof g_refused[0].name, f->name);
    strcpy_s(g_refused[g_nrefused].hash, sizeof g_refused[0].hash, f->bundle_hash);
    g_refused[g_nrefused].cause = cause;
    g_nrefused++;
    return 1;
}

static void mv_refusal_clear(const char *name)
{
    int i, w = 0;
    for (i = 0; i < g_nrefused; ++i)
        if (strcmp(g_refused[i].name, name)) g_refused[w++] = g_refused[i];
    g_nrefused = w;
}

static int mv_owned_by_name(const char *name)
{
    int i;
    for (i = 0; i < SH_MODVT_SLOTS; ++i)
        if (g_owned[i].used && !strcmp(g_owned[i].name, name)) return i;
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
        if (!g_owned[i].used && g_owned[i].record && !strcmp(g_owned[i].name, name)) return i;
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
        mv_set_used(o, 0, NULL);   /* name and record kept: the record is preferred again */
        res.lost++;
    }

    /* 2. Boards no longer served, or served from another build. */
    for (i = 0; i < SH_MODVT_SLOTS; ++i) {
        mv_owned *o = &g_owned[i];
        int w;
        if (!o->used) continue;
        w = mv_wanted_by_name(wanted, n, o->name);
        if (w >= 0 && !strcmp(wanted[w].bundle_hash, o->bundle_hash)) continue;
        if (!release_now) { res.pending++; continue; }
        if (ops->release(ops->context, o->record) == 1) {
            mv_log("%s: released record %d, board '%s' (%s %s)", phase, o->record, o->name, o->bundle,
                   w < 0 ? "is no longer served here" : "is now served from another build");
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
        int o = mv_owned_by_name(f->name), slot = -1, r, idx;
        if (o >= 0) {
            if (!strcmp(g_owned[o].bundle_hash, f->bundle_hash)) res.kept++;
            continue;   /* else its release is pending */
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
        if (idx >= 0 && g_owned[idx].record >= 1 && g_owned[idx].record <= SH_MODVT_LAST_USED &&
            !strcmp(g_owned[idx].name, f->name) && ops->is_free(ops->context, g_owned[idx].record) == 1)
            slot = g_owned[idx].record;
        for (k = 1; k <= SH_MODVT_LAST_USED && slot < 0; ++k)
            if (ops->is_free(ops->context, k) == 1) slot = k;
        if (slot < 0 || idx < 0) {
            if (mv_refusal_new(f, MV_FULL)) {
                mv_log("%s: board '%s' (%s) not registered: records 1 to %d are all in use (the game's own board, the "
                       "map's own families and other boards; record 31 is the engine's); its materials draw with plain "
                       "textures", phase, f->name, f->bundle, SH_MODVT_LAST_USED);
                if (ops->notice) {
                    char text[256];
                    sh_modvt_full_notice(text, sizeof text, f->bundle);
                    ops->notice(ops->context, text);
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
            int again;
            m->record = slot;
            strcpy_s(m->name, sizeof m->name, f->name);
            strcpy_s(m->path, sizeof m->path, f->path);
            strcpy_s(m->bundle, sizeof m->bundle, f->bundle);
            strcpy_s(m->bundle_hash, sizeof m->bundle_hash, f->bundle_hash);
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

int sh_modvt_owned_count(void)
{
    return (int)InterlockedCompareExchange(&g_nowned, 0, 0);
}

void sh_modvt_reset(void)
{
    AcquireSRWLockExclusive(&g_lock);
    memset(g_owned, 0, sizeof g_owned);
    free(g_mats);
    g_mats = NULL;
    g_nmats = g_capmats = 0;
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_nowned, 0);
    free(g_refused);
    g_refused = NULL;
    g_nrefused = g_caprefused = 0;
}

void sh_modvt_full_notice(char *out, size_t cap, const char *mod)
{
    char name[96];
    if (!out || !cap) return;
    strncpy_s(name, sizeof name, mod && *mod ? mod : "a mod", _TRUNCATE);
    _snprintf_s(out, cap, _TRUNCATE,
                "DOOM has no room for another mod texture board on this map, so %s draws with plain textures; "
                "a map with fewer mods that carry their own textures avoids this.", name);
}

/* ------------------------------------------------------------------ the two detours */

void *sh_modvt_board_record(const char *name)
{
    void *rec = NULL;
    int i;
    if (!name || !*name || !InterlockedCompareExchange(&g_nowned, 0, 0)) return NULL;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < SH_MODVT_SLOTS && !rec; ++i)
        if (g_owned[i].used && g_owned[i].rec && !strcmp(g_owned[i].name, name)) rec = g_owned[i].rec;
    ReleaseSRWLockShared(&g_lock);
    return rec;
}

static void mv_remember(void *material, const char *name)
{
    int i;
    for (i = 0; i < g_nmats; ++i)
        if (g_mats[i].material == material) {
            if (strcmp(g_mats[i].name, name)) strcpy_s(g_mats[i].name, sizeof g_mats[i].name, name);
            return;
        }
    if (g_nmats == g_capmats) {
        int cap = g_capmats ? g_capmats * 2 : 16;
        mv_material *grown = (mv_material *)realloc(g_mats, sizeof *grown * (size_t)cap);
        if (!grown) return;   /* this material is not bound again after a release; logged by the caller's count */
        g_mats = grown;
        g_capmats = cap;
    }
    g_mats[g_nmats].material = material;
    strcpy_s(g_mats[g_nmats].name, sizeof g_mats[0].name, name);
    g_nmats++;
}

int sh_modvt_bind_board(void *parms, void *record, const sh_modvt_bind_ops *ops)
{
    char name[SH_MODVT_NAME_MAX];
    void *image;
    float word[4];
    const float *fb;
    int i, held = 0;
    if (!parms || !record || !InterlockedCompareExchange(&g_nowned, 0, 0)) return 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < SH_MODVT_SLOTS && !held; ++i)
        if (g_owned[i].used && g_owned[i].rec == record) {
            strcpy_s(name, sizeof name, g_owned[i].name);
            held = 1;
        }
    ReleaseSRWLockShared(&g_lock);
    if (!held || !ops || !ops->feedback || !ops->set_vec4 || !ops->set_texture || !ops->feedback_handle ||
        !ops->pagetable_handle)
        return 0;
    image = *(void *const *)((const uint8_t *)record + SH_VT_REC_PTIMAGE);
    if (!image) return 0;   /* no page table: the view's values stay, as for any record without one */
    fb = ops->feedback(record, word);
    if (fb != word) memcpy(word, fb, sizeof word);
    ops->set_vec4(parms, ops->feedback_handle, word);
    ops->set_texture(parms, ops->pagetable_handle, image);
    AcquireSRWLockExclusive(&g_lock);
    mv_remember((uint8_t *)parms - SH_VT_MAT_PARMS, name);
    ReleaseSRWLockExclusive(&g_lock);
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

#ifndef SH_MODVT_PURE
/* ================================================================== engine */
#include "signatures.h"
#include "commands.h"
#include "engine_globals.h"
#include "host_image.h"
#include "hook.h"
#include "modpublished.h"
#include "process_heap_scope.h"

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

static mv_setsource_fn g_setsource;
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

static void mv_rd_cstr(const void *slot, char *out, size_t cap)
{
    const char *s = NULL;
    out[0] = 0;
    if (!mv_rd_ptr(slot, (void **)&s) || !s) return;
    __try { strncpy_s(out, cap, s, _TRUNCATE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { out[0] = 0; }
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
    char name[64];
    int type = 0;
    if (!r) return 0;
    mv_rd_cstr(r + SH_VT_REC_NAME, name, sizeof name);
    mv_rd_i32(r + SH_VT_REC_TYPE, &type);
    return type == 2 && strstr(name, "_vmtr") != NULL;
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
    InterlockedExchange(&g_handles, 1);
    return 1;
}

/* ------------------------------------------------------------------ detours */

static void *mv_vf_detour(void *sys, void *material)
{
    char name[SH_MODVT_NAME_MAX];
    void *rec;
    if (material && InterlockedCompareExchange(&g_nowned, 0, 0)) {
        mv_rd_cstr((const uint8_t *)material + SH_VT_MAT_LANDPAGE, name, sizeof name);
        if (name[0] && (rec = sh_modvt_board_record(name)) != NULL) return rec;
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
    char n[SH_MODVT_NAME_MAX + 8];
    (void)c;
    for (k = 1; k < SH_MODVT_SLOTS; ++k) {
        uint8_t *r = sh_modvt_engine_record(k);
        if (!r) continue;
        mv_rd_cstr(r + SH_VT_REC_NAME, n, sizeof n);
        if (n[0] && !strcmp(n, name)) return k;
    }
    return -1;
}

static int op_is_free(void *c, int k)
{
    uint8_t *r = sh_modvt_engine_record(k);
    char path[8], name[8];
    (void)c;
    if (!r) return 0;
    mv_rd_cstr(r + SH_VT_REC_PATH, path, sizeof path);
    mv_rd_cstr(r + SH_VT_REC_NAME, name, sizeof name);
    return !path[0] && !name[0];
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
    char landpage[SH_MODVT_NAME_MAX];
    int type = 0, ok = 0;
    (void)c;
    if (!m || !r || sh_modvt_engine_on_main() != 1) return 0;
    mv_rd_cstr(m + SH_VT_MAT_LANDPAGE, landpage, sizeof landpage);
    if (strcmp(landpage, name) || !mv_rd_i32(r + SH_VT_REC_TYPE, &type)) return 0;
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

static const sh_modvt_ops g_ops = { NULL, op_ready, op_find, op_is_free, op_set_source, op_adopt_cache, op_release,
                                    op_record, op_rebind, op_notice };

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
    if (!n && !sh_modvt_owned_count()) { free(fams); InterlockedExchange(&g_pending, 0); return; }
    if (!mv_handles()) {
        /* Not set yet (the renderer is still starting), or unavailable for the session. */
        InterlockedExchange(&g_pending, InterlockedCompareExchange(&g_handles, 0, 0) == 0);
        free(fams);
        return;
    }
    if (sh_modvt_sync(fams, n, &g_ops, phase, release_now, &res) < 0) InterlockedExchange(&g_pending, 1);
    else InterlockedExchange(&g_pending, res.pending ? 1 : 0);
    free(fams);
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
}

/* The VT frame update, in EndFrame right after the render-thread sync: the
 * point the engine changes VT state itself. A queued sync runs first. */
static void mv_frame_detour(void *sys)
{
    if (g_bound && InterlockedCompareExchange(&g_pending, 0, 0) && sh_modvt_engine_on_main() == 1)
        mv_sync_now("frame", 1);
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
    sh_printf("VT: boards: %d served, %d registered%s%s%s, %d material%s bound; %s, %s%s\n", n, sh_modvt_owned_count(),
              held[0] ? " (records " : "", held, held[0] ? ")" : "", sh_modvt_material_count(NULL),
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

/* ------------------------------------------------------------------ install */

static const sig_result *mv_clean(const sig_result *r, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; ++i)
        if (r[i].name && !strcmp(r[i].name, name)) return r[i].status == SIG_OK ? &r[i] : NULL;
    return NULL;
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
        if ((s[i] = mv_clean(results, count, names[i])) == NULL) {
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
    mv_log("boards bound on %s: SetSource rva 0x%X, VirtualTextureForMaterial rva 0x%X and binder rva 0x%X (detoured), "
           "rebind rva 0x%X, VT system rva 0x%X, record table sys+0x%X",
           g_vulkan == 1 ? "Vulkan" : g_vulkan == 0 ? "OpenGL" : "an unknown image", s[0]->rva, s[1]->rva, s[2]->rva,
           s[3]->rva, l.sys_rva, l.table_off);
    sh_modserve_set_index_listener(mv_index_changed);
    if (sh_modvt_collect(NULL)) InterlockedExchange(&g_pending, 1);
    return 1;
}
#endif /* SH_MODVT_PURE */
