/* modvt.c -- see modvt.h. The first half (families, sync, decoding) makes no
 * engine call and builds into the native tests with SH_MODVT_PURE; the
 * engine half binds the VT system and runs the sync on DOOM's main thread.
 *
 * Engine surface (build 13954591), every address from a signature or decoded
 * from inside one with its bytes checked; any mismatch leaves registration
 * unavailable and the families unregistered:
 *   VtSetSource    idVirtualTexture::SetSource(rec, name, path, flavor, type, direct, reader),
 *                  VK 0x185AC30, GL 0x185BC90. Its head is LEA RCX,[sys]; CALL; MOV RCX,RSI;
 *                  CALL Release: the VT system (VK 0x66E0940, GL 0x4FE6120) and Release
 *                  (VK 0x1859480, GL 0x185A460).
 *   VtForMaterial  VirtualTextureForMaterial(sys, material), VK 0x17E8230, GL 0x17E9100: its
 *                  MOV RAX,[R13+disp32] returns record 1 for "_vmtr", so disp32 - 8 is the
 *                  record pointer table (VK sys+0x8B00, GL sys+0xA400). On OpenGL its sparse test
 *                  (CMP QWORD [ext],0; CMP DWORD [cvar],0; MOV EAX,4) gives the two globals,
 *                  __glewTexStorageSparseAMD and vt_enablePRT: with both set a material's flavor
 *                  1 or 2 becomes 4, so a family's record must carry the same flavor to match.
 * SetSource runs the VT system's synchronisation at entry and allocates from
 * the current heap, so it is called on the main thread inside process heap 0
 * (the page tables live as long as the record).
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
#include "vtpack.h"
#include "mp_json.h"

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

/* ------------------------------------------------------------------ families */

typedef struct mv_collect { sh_modvt_family *out; int cap, n; } mv_collect;

static void mv_collect_file(void *context, const char *request, uint64_t size, const char *bundle_id,
                            const char *bundle_hash)
{
    mv_collect *c = (mv_collect *)context;
    char fam[SH_MODVT_NAME_MAX];
    int member = 0, i;
    if (!sh_mb_vt_member(request, fam, sizeof fam, &member)) return;
    for (i = 0; i < c->n && i < c->cap; ++i)
        if (!strcmp(c->out[i].name, fam)) {
            /* Two salts serving one family name: the first keeps it. */
            if (!strcmp(c->out[i].bundle_hash, bundle_hash)) {
                c->out[i].members |= 1u << (member - 1);
                c->out[i].bytes += size;
            }
            return;
        }
    if (c->n < c->cap) {
        sh_modvt_family *f = &c->out[c->n];
        memset(f, 0, sizeof *f);
        strcpy_s(f->name, sizeof f->name, fam);
        _snprintf_s(f->path, sizeof f->path, _TRUNCATE, "virtualtextures/%s.pages", fam);
        _snprintf_s(f->bundle, sizeof f->bundle, _TRUNCATE, "%s", bundle_id);
        strncpy_s(f->bundle_hash, sizeof f->bundle_hash, bundle_hash, _TRUNCATE);
        f->members = 1u << (member - 1);
        f->bytes = size;
    }
    c->n++;
}

int sh_modvt_collect(sh_modvt_family *out, int cap)
{
    mv_collect c;
    sh_modvt_family *scratch = NULL;
    /* Counting alone still needs the names, to fold a family's members. */
    if (!out || cap <= 0) {
        scratch = (sh_modvt_family *)calloc(SH_MODVT_MAX, sizeof *scratch);
        if (!scratch) return 0;
        out = scratch;
        cap = SH_MODVT_MAX;
    }
    c.out = out;
    c.cap = cap;
    c.n = 0;
    /* From the build the loaded map serves (a map's own copy of a salt, not
     * the root folder's), as scenes and banks are. */
    sh_modserve_each_served_here(mv_collect_file, &c);
    free(scratch);
    return c.n;
}

/* ------------------------------------------------------------------ product records */

typedef struct mv_owned {
    int used;             /* 1 held; 0 free, or (record set) a record the engine took back */
    int record;
    int flavor;
    int exposed;          /* a decl registration pass ran while it was registered */
    int held_logged;      /* the "kept until DOOM exits" line was written */
    char name[SH_MODVT_NAME_MAX];
    char path[SH_MODVT_PATH_MAX];
    char bundle[SH_MODVT_NAME_MAX];
    char bundle_hash[65];
} mv_owned;

static mv_owned g_owned[SH_MODVT_SLOTS];

/* Refusals already logged, so a pass does not repeat them: (name, cause). */
#define MV_REFUSALS 64
static struct { char name[SH_MODVT_NAME_MAX]; char hash[65]; int cause; } g_refused[MV_REFUSALS];
static int g_nrefused;

enum { MV_TAKEN = 1, MV_FULL, MV_SETSOURCE, MV_FAULT };

static int mv_refusal_new(const sh_modvt_family *f, int cause)
{
    int i;
    for (i = 0; i < g_nrefused; ++i)
        if (g_refused[i].cause == cause && !strcmp(g_refused[i].name, f->name) && !strcmp(g_refused[i].hash, f->bundle_hash))
            return 0;
    if (g_nrefused < MV_REFUSALS) {
        strcpy_s(g_refused[g_nrefused].name, sizeof g_refused[0].name, f->name);
        strcpy_s(g_refused[g_nrefused].hash, sizeof g_refused[0].hash, f->bundle_hash);
        g_refused[g_nrefused].cause = cause;
        g_nrefused++;
    }
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

/* A free entry: the one a lost record of this family left (its record is
 * preferred again), else a clean one, else any free one. */
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

int sh_modvt_sync(const sh_modvt_family *wanted, int n, const sh_modvt_ops *ops, const char *phase,
                  sh_modvt_result *out)
{
    sh_modvt_result res;
    int i, k, count = 0;
    memset(&res, 0, sizeof res);
    if (n < 0) n = 0;
    res.wanted = n;
    if (!ops || !ops->ready || !ops->flavor || !ops->find || !ops->is_free || !ops->set_source || !ops->release ||
        ops->ready(ops->context) != 1) {
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
        mv_log("%s: record %d no longer holds family '%s' (%s); it is registered again if its bundle is still installed",
               phase, o->record, o->name, k < 0 ? "the engine released it" : "the name moved to another record");
        o->used = 0;          /* name and record kept: the record is preferred again */
        res.lost++;
    }

    /* 2. Families that left the index, or whose bundle changed. */
    for (i = 0; i < SH_MODVT_SLOTS; ++i) {
        mv_owned *o = &g_owned[i];
        int w;
        if (!o->used) continue;
        w = mv_wanted_by_name(wanted, n, o->name);
        if (w >= 0 && !strcmp(wanted[w].bundle_hash, o->bundle_hash)) continue;
        if (o->exposed) {
            if (!o->held_logged)
                mv_log("%s: family '%s' (%s) %s; record %d is kept until DOOM exits, because a registration pass ran "
                       "since it was registered and its materials may be bound to it%s", phase, o->name, o->bundle,
                       w < 0 ? "is no longer installed" : "now comes from another build", o->record,
                       w < 0 ? "" : " (the engine keeps reading the bytes it opened)");
            o->held_logged = 1;
            res.held++;
            continue;
        }
        {
            int r = ops->release(ops->context, o->record);
            if (r == 1) {
                mv_log("%s: released record %d, family '%s' (%s %s)", phase, o->record, o->name, o->bundle,
                       w < 0 ? "is no longer installed" : "was replaced by another build");
                memset(o, 0, sizeof *o);
                res.released++;
            } else {
                mv_log("%s: releasing record %d, family '%s', FAILED; the record stays", phase, o->record, o->name);
                o->held_logged = 1;
                o->exposed = 1;
                res.held++;
            }
        }
    }

    /* 3. Every family without a record. */
    for (i = 0; i < n; ++i) {
        const sh_modvt_family *f = &wanted[i];
        int o = mv_owned_by_name(f->name), slot = -1, flavor, r, idx;
        if (o >= 0) {
            if (!strcmp(g_owned[o].bundle_hash, f->bundle_hash)) res.kept++;
            continue;   /* else held above */
        }
        k = ops->find(ops->context, f->name);
        if (k >= 0) {
            if (mv_refusal_new(f, MV_TAKEN))
                mv_log("%s: family '%s' (%s) not registered: record %d already holds that name and was not created by "
                       "Snapmap+ (a material naming the family was parsed before the family was served, or 'sh_vt "
                       "register' made it); its materials keep that record", phase, f->name, f->bundle, k);
            res.refused++;
            continue;
        }
        flavor = ops->flavor(ops->context);
        idx = mv_free_owned(f->name);
        if (idx >= 0 && g_owned[idx].record >= 1 && g_owned[idx].record <= SH_MODVT_LAST_USED &&
            !strcmp(g_owned[idx].name, f->name) && ops->is_free(ops->context, g_owned[idx].record) == 1)
            slot = g_owned[idx].record;
        for (k = 1; k <= SH_MODVT_LAST_USED && slot < 0; ++k)
            if (ops->is_free(ops->context, k) == 1) slot = k;
        if (slot < 0 || idx < 0) {
            if (mv_refusal_new(f, MV_FULL))
                mv_log("%s: family '%s' (%s) not registered: records 1 to %d are all in use (stock and map families "
                       "are never evicted, and record 31 is the engine's); its materials draw with default images",
                       phase, f->name, f->bundle, SH_MODVT_LAST_USED);
            res.refused++;
            res.full++;
            continue;
        }
        r = ops->set_source(ops->context, slot, f->name, f->path, flavor);
        if (r == 1) {
            mv_owned *m = &g_owned[idx];
            /* Back in the record the engine took: materials bound to it see the family again. */
            int exposed = m->record == slot && !strcmp(m->name, f->name) ? m->exposed : 0;
            memset(m, 0, sizeof *m);
            m->exposed = exposed;
            m->used = 1;
            m->record = slot;
            m->flavor = flavor;
            strcpy_s(m->name, sizeof m->name, f->name);
            strcpy_s(m->path, sizeof m->path, f->path);
            strcpy_s(m->bundle, sizeof m->bundle, f->bundle);
            strcpy_s(m->bundle_hash, sizeof m->bundle_hash, f->bundle_hash);
            mv_refusal_clear(f->name);
            mv_log("%s: registered family '%s' (%s, %d member%s, %llu bytes) as record %d: SetSource('%s', flavor %d, "
                   "type 2)", phase, f->name, f->bundle, mv_members(f->members), mv_members(f->members) == 1 ? "" : "s",
                   (unsigned long long)f->bytes, slot, f->path, flavor);
            res.registered++;
        } else {
            if (mv_refusal_new(f, r < 0 ? MV_FAULT : MV_SETSOURCE))
                mv_log("%s: family '%s' (%s) not registered: SetSource on record %d %s", phase, f->name, f->bundle, slot,
                       r < 0 ? "FAULTED (see the line above)"
                             : "refused (no member opened, or a bad header: see the engine's 'Using mega2 chunk file' "
                               "and 'SetSource:' console lines)");
            res.refused++;
        }
    }

    for (i = 0; i < SH_MODVT_SLOTS; ++i) count += g_owned[i].used;
    if (out) *out = res;
    return count;
}

void sh_modvt_mark_exposed(void)
{
    int i;
    for (i = 0; i < SH_MODVT_SLOTS; ++i)
        if (g_owned[i].used) g_owned[i].exposed = 1;
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
    int i, n = 0;
    for (i = 0; i < SH_MODVT_SLOTS; ++i) n += g_owned[i].used;
    return n;
}

void sh_modvt_reset(void)
{
    memset(g_owned, 0, sizeof g_owned);
    g_nrefused = 0;
}

int sh_modvt_flavor(int vulkan, uint64_t sparse_ext, int32_t enable_prt)
{
    return !vulkan && sparse_ext && enable_prt ? 4 : 2;
}

/* ------------------------------------------------------------------ decoding */

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
                           uint32_t vf_rva, int opengl, sh_modvt_layout *out)
{
    sh_modvt_layout l;
    int at;
    uint32_t d;
    memset(&l, 0, sizeof l);
    if (out) *out = l;
    if (!ss || !vf || !out) return 0;
    at = sh_modvt_find_setsource_head(ss, ss_len);
    if (at < 0x30 || at > 0x80) return 0;
    {
        int32_t d_lea, d_rel;
        memcpy(&d_lea, ss + at + 3, 4);
        memcpy(&d_rel, ss + at + 16, 4);
        l.sys_rva = (uint32_t)((int64_t)ss_rva + at + 7 + d_lea);
        l.release_rva = (uint32_t)((int64_t)ss_rva + at + 20 + d_rel);
    }
    d = sh_modvt_find_table_disp(vf, vf_len);
    if (d < 8 || d >= 0x20000) return 0;
    l.table_off = d - 8;
    if (opengl) {
        size_t i, j, m;
        for (i = 0x30; i + 8 <= 0x90 && i + 8 <= vf_len && !l.sparse_ext_rva; ++i) {
            if (!(vf[i] == 0x48 && vf[i + 1] == 0x83 && vf[i + 2] == 0x3D && vf[i + 7] == 0x00)) continue;
            for (j = i + 8; j + 7 <= i + 0x30 && j + 7 <= vf_len; ++j) {
                int four = 0;
                if (!(vf[j] == 0x83 && vf[j + 1] == 0x3D && vf[j + 6] == 0x00)) continue;
                for (m = j + 7; m + 5 <= j + 0x18 && m + 5 <= vf_len; ++m)
                    if (!memcmp(vf + m, "\xB8\x04\x00\x00\x00", 5)) four = 1;
                if (four) {
                    int32_t d1, d2;
                    memcpy(&d1, vf + i + 3, 4);
                    memcpy(&d2, vf + j + 2, 4);
                    l.sparse_ext_rva = (uint32_t)((int64_t)vf_rva + (int64_t)i + 8 + d1);
                    l.sparse_cvar_rva = (uint32_t)((int64_t)vf_rva + (int64_t)j + 7 + d2);
                }
                break;
            }
        }
    }
    /* On OpenGL the record's flavor follows the material's, which this test decides. */
    if (opengl && !l.sparse_ext_rva) return 0;
    *out = l;
    return 1;
}

/* ================================================================== lit pages
 *
 * Mods' lit pages (vtpack.h): the mods the loaded map serves are packed into
 * the free area of the stock `_vmtr` family's squares 16, 8 and 7. A mod is placed once per session
 * (its place is kept while DOOM runs, so a material read once stays right);
 * the merged member holds the pages of the mods served now. */

#define MV_LIT_SQUARES  3

/* The stock squares the lit pages go into, in the order a tie prefers them:
 * the three whose installed member is nearly empty (no level-0 page, a few
 * page rows at levels 1 to 8). Every other square's member is 0.6 to 3 GB,
 * and a merged member is served from memory. */
static const int g_lit_square[MV_LIT_SQUARES] = { 16, 8, 7 };

typedef struct mv_lit_mod {
    char salt[SH_MB_ID_MAX + 1];
    char hash[65];
    char key[SH_MB_ID_MAX + 70];          /* "<salt>@<hash>": orders placement and the member */
    char request[SH_MB_ID_MAX + 40];      /* virtualtextures/mods/<salt>/vmtr.mega2 */
    int size;                             /* its node (the block's record) */
    int placed;
    sh_vtp_node node;
    int served;                           /* the loaded map serves it now */
    int told;                             /* the "does not fit" line was written for this set */
    char **mats;                          /* lower-cased material requests (the block's record) */
    int nmats;
} mv_lit_mod;

typedef struct mv_lit_stock {
    uint8_t *bytes;                       /* the installed member of that square */
    size_t len;
    sh_vtp_member m;
    int ok;
    uint64_t sig;                         /* the served set's signature there; 0 when no mod is placed in it */
} mv_lit_stock;

static mv_lit_mod *g_lit;                 /* every build seen this session, placed or not */
static int g_nlit, g_clit;
static SRWLOCK g_lit_lock = SRWLOCK_INIT;
static mv_lit_stock g_stock[MV_LIT_SQUARES];
static volatile LONG g_lit_known;         /* a refresh ran */

static int mv_lit_slot(int square)
{
    int s;
    for (s = 0; s < MV_LIT_SQUARES; ++s) if (g_lit_square[s] == square) return s;
    return -1;
}

int sh_modvt_lit_square(int k)
{
    return k >= 0 && k < MV_LIT_SQUARES ? g_lit_square[k] : 0;
}

int sh_modvt_lit_set_stock(int square, const uint8_t *bytes, size_t len)
{
    char why[200];
    int s = mv_lit_slot(square), ok = 0;
    uint8_t *copy;
    if (s < 0) return 0;
    copy = len ? (uint8_t *)malloc(len) : NULL;
    AcquireSRWLockExclusive(&g_lit_lock);
    free(g_stock[s].bytes);
    memset(&g_stock[s], 0, sizeof g_stock[s]);
    if (copy) {
        memcpy(copy, bytes, len);
        g_stock[s].bytes = copy;
        g_stock[s].len = len;
        g_stock[s].ok = sh_vtp_open(&g_stock[s].m, copy, len, why, sizeof why) && g_stock[s].m.square == square;
        if (!g_stock[s].ok)
            mv_log("lit pages: the game's virtualtextures/_vmtr_sq%d.mega2 cannot be read as square %d (%s); no lit pages "
                   "are placed there", square, square, g_stock[s].m.square && g_stock[s].m.square != square ? "another square" : why);
    }
    ok = g_stock[s].ok;
    ReleaseSRWLockExclusive(&g_lit_lock);
    return ok;
}

static void mv_lower(char *s) { for (; *s; ++s) { if (*s == '\\') *s = '/'; else if (*s >= 'A' && *s <= 'Z') *s += 32; } }

static void mv_lit_free(mv_lit_mod *m)
{
    int i;
    for (i = 0; i < m->nmats; ++i) free(m->mats[i]);
    free(m->mats);
    memset(m, 0, sizeof *m);
}

void sh_modvt_lit_reset(void)
{
    int i;
    AcquireSRWLockExclusive(&g_lit_lock);
    for (i = 0; i < g_nlit; ++i) mv_lit_free(&g_lit[i]);
    free(g_lit);
    g_lit = NULL;
    g_nlit = g_clit = 0;
    for (i = 0; i < MV_LIT_SQUARES; ++i) g_stock[i].sig = 0;
    InterlockedExchange(&g_lit_known, 0);
    ReleaseSRWLockExclusive(&g_lit_lock);
}

/* The served lit members, collected from the index. */
typedef struct mv_lit_seen { char salt[SH_MB_ID_MAX + 1]; char hash[65]; } mv_lit_seen;
typedef struct mv_lit_walk { mv_lit_seen *v; int n, cap, oom; } mv_lit_walk;

static void mv_lit_file(void *context, const char *request, uint64_t size, const char *bundle_id, const char *bundle_hash)
{
    mv_lit_walk *w = (mv_lit_walk *)context;
    char salt[SH_MB_ID_MAX + 1];
    (void)size;
    (void)bundle_id;
    if (!sh_mb_vt_lit_member(request, salt, sizeof salt)) return;
    if (w->n == w->cap) {
        int cap = w->cap ? w->cap * 2 : 16;
        mv_lit_seen *g = (mv_lit_seen *)realloc(w->v, sizeof *g * (size_t)cap);
        if (!g) { w->oom = 1; return; }
        w->v = g;
        w->cap = cap;
    }
    strcpy_s(w->v[w->n].salt, sizeof w->v[0].salt, salt);
    mv_lower(w->v[w->n].salt);
    strncpy_s(w->v[w->n].hash, sizeof w->v[0].hash, bundle_hash, _TRUNCATE);
    w->n++;
}

static int mv_lit_find(const char *salt, const char *hash)
{
    int i;
    for (i = 0; i < g_nlit; ++i) if (!strcmp(g_lit[i].salt, salt) && !strcmp(g_lit[i].hash, hash)) return i;
    return -1;
}

/* The block's record, registries "vt": the node and the materials. */
static void mv_lit_registry(void *context, const char *id, const char *hash, const char *registries)
{
    mpj *root, *vt, *node, *mats;
    char salt[SH_MB_ID_MAX + 1];
    int i, k;
    (void)context;
    if (!registries || !strstr(registries, "\"vt\"")) return;
    strncpy_s(salt, sizeof salt, id ? id : "", _TRUNCATE);
    mv_lower(salt);
    if (mv_lit_find(salt, hash) >= 0) return;            /* read once per build */
    if (g_nlit == g_clit) {
        int cap = g_clit ? g_clit * 2 : 16;
        mv_lit_mod *g = (mv_lit_mod *)realloc(g_lit, sizeof *g * (size_t)cap);
        if (!g) { mv_log("lit pages: out of memory reading %s's record", salt); return; }
        g_lit = g;
        g_clit = cap;
    }
    root = mpj_parse(registries, strlen(registries));
    vt = root ? mpj_get(root, "vt") : NULL;
    node = vt ? mpj_get(vt, "node") : NULL;
    mats = vt ? mpj_get(vt, "materials") : NULL;
    if (node && mpj_is_num(node)) {
        mv_lit_mod *m = &g_lit[g_nlit];
        memset(m, 0, sizeof *m);
        strcpy_s(m->salt, sizeof m->salt, salt);
        strncpy_s(m->hash, sizeof m->hash, hash, _TRUNCATE);
        _snprintf_s(m->key, sizeof m->key, _TRUNCATE, "%s@%s", m->salt, m->hash);
        _snprintf_s(m->request, sizeof m->request, _TRUNCATE, "virtualtextures/mods/%s/vmtr.mega2", m->salt);
        m->size = (int)mpj_int(node);
        if (mats && mats->type == MPJ_ARR && mats->n) {
            m->mats = (char **)calloc((size_t)mats->n, sizeof(char *));
            for (i = 0, k = 0; m->mats && i < mats->n; ++i)
                if (mats->items[i]->type == MPJ_STR && (m->mats[k] = _strdup(mats->items[i]->text)) != NULL)
                    mv_lower(m->mats[k++]);
            m->nmats = k;
        }
        g_nlit++;
    }
    mpj_free(root);
}

static uint64_t mv_fnv(uint64_t h, const char *s)
{
    for (; *s; ++s) h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return (h ^ '|') * 1099511628211ull;
}

static int mv_lit_key_cmp(const void *a, const void *b)
{
    return strcmp(g_lit[*(const int *)a].key, g_lit[*(const int *)b].key);
}

/* Who the loaded map serves now, every newcomer placed (held places kept).
 * Caller holds g_lit_lock exclusively. Returns how many are served. */
static int mv_lit_refresh_locked(void)
{
    mv_lit_walk w = { NULL, 0, 0, 0 };
    sh_vtp_req *reqs = NULL;
    int *order = NULL;
    const sh_vtp_member *stocks[MV_LIT_SQUARES];
    int i, k, s, n = 0, ns = 0, served = 0;
    sh_modserve_each_served_here(mv_lit_file, &w);
    if (w.n) (void)sh_modserve_each_bundle_registries(mv_lit_registry, NULL);
    for (i = 0; i < g_nlit; ++i) g_lit[i].served = 0;
    for (k = 0; k < w.n; ++k) {
        i = mv_lit_find(w.v[k].salt, w.v[k].hash);
        if (i >= 0) g_lit[i].served = 1;
    }
    free(w.v);
    if (w.oom) mv_log("lit pages: out of memory collecting the served mods");
    /* A rebuilt mod takes the place its earlier build held when that build is
     * not served and the new block fits in it, so its materials stay right. */
    for (i = 0; i < g_nlit; ++i) {
        mv_lit_mod *m = &g_lit[i];
        if (!m->served || m->placed) continue;
        for (k = 0; k < g_nlit; ++k) {
            mv_lit_mod *o = &g_lit[k];
            if (k == i || !o->placed || o->served || strcmp(o->salt, m->salt) || m->size > o->node.size) continue;
            m->node = o->node;
            m->node.size = m->size;
            m->placed = 1;
            o->placed = 0;
            break;
        }
    }
    for (s = 0; s < MV_LIT_SQUARES; ++s) if (g_stock[s].ok) stocks[ns++] = &g_stock[s].m;
    reqs = (sh_vtp_req *)calloc((size_t)(g_nlit ? g_nlit : 1), sizeof *reqs);
    order = (int *)calloc((size_t)(g_nlit ? g_nlit : 1), sizeof *order);
    if (!reqs || !order) {
        free(reqs);
        free(order);
        mv_log("lit pages: out of memory placing the served mods");
        return 0;
    }
    if (ns) {
        for (i = 0; i < g_nlit; ++i) {
            if (!g_lit[i].placed && !g_lit[i].served) continue;   /* a held place, or a newcomer to place */
            reqs[n].key = g_lit[i].key;
            reqs[n].size = g_lit[i].size;
            reqs[n].held = g_lit[i].placed;
            reqs[n].node = g_lit[i].node;
            order[n++] = i;
        }
        (void)sh_vtp_place(stocks, ns, reqs, n);
        for (k = 0; k < n; ++k)
            if (reqs[k].placed && !g_lit[order[k]].placed) { g_lit[order[k]].placed = 1; g_lit[order[k]].node = reqs[k].node; }
    }
    n = 0;
    for (i = 0; i < g_nlit; ++i) if (g_lit[i].served) { order[n++] = i; served++; }
    qsort(order, (size_t)n, sizeof order[0], mv_lit_key_cmp);
    /* Per square: the served mods placed there, in key order, name its member. */
    for (s = 0; s < MV_LIT_SQUARES; ++s) {
        uint64_t sig = 1469598103934665603ull;
        int any = 0;
        char num[64];
        _snprintf_s(num, sizeof num, _TRUNCATE, "sq%d:%zu", g_lit_square[s], g_stock[s].len);
        sig = mv_fnv(sig, num);
        for (k = 0; k < n && g_stock[s].ok; ++k) {
            const mv_lit_mod *m = &g_lit[order[k]];
            if (!m->placed || m->node.square != g_lit_square[s]) continue;
            any = 1;
            sig = mv_fnv(sig, m->key);
            _snprintf_s(num, sizeof num, _TRUNCATE, "%d,%d,%d", m->node.x, m->node.y, m->node.size);
            sig = mv_fnv(sig, num);
        }
        g_stock[s].sig = any ? (sig ? sig : 1) : 0;
    }
    free(reqs);
    free(order);
    InterlockedExchange(&g_lit_known, 1);
    return served;
}

int sh_modvt_lit_refresh(void)
{
    int n;
    AcquireSRWLockExclusive(&g_lit_lock);
    n = mv_lit_refresh_locked();
    ReleaseSRWLockExclusive(&g_lit_lock);
    return n;
}

uint64_t sh_modvt_lit_signature(int square)
{
    uint64_t sig = 0;
    int s = mv_lit_slot(square);
    if (s < 0) return 0;
    AcquireSRWLockShared(&g_lit_lock);
    sig = g_stock[s].sig;
    ReleaseSRWLockShared(&g_lit_lock);
    return sig;
}

int sh_modvt_lit_state(int k, sh_modvt_lit_info *out)
{
    int r = 0;
    AcquireSRWLockShared(&g_lit_lock);
    if (k >= 0 && k < g_nlit && out) {
        const mv_lit_mod *m = &g_lit[k];
        memset(out, 0, sizeof *out);
        strcpy_s(out->salt, sizeof out->salt, m->salt);
        strcpy_s(out->hash, sizeof out->hash, m->hash);
        out->size = m->size;
        out->placed = m->placed;
        out->served = m->served;
        out->square = m->placed ? m->node.square : 0;
        out->x = m->node.x;
        out->y = m->node.y;
        out->materials = m->nmats;
        r = 1;
    }
    ReleaseSRWLockShared(&g_lit_lock);
    return r;
}

static unsigned char *mv_read_disk(const char *utf8, size_t max, size_t *len)
{
    wchar_t wide[1024];
    HANDLE h;
    LARGE_INTEGER size;
    unsigned char *buf = NULL;
    size_t done = 0;
    *len = 0;
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wide, (int)(sizeof wide / sizeof wide[0]))) return NULL;
    h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && (uint64_t)size.QuadPart <= max &&
        (buf = (unsigned char *)malloc((size_t)size.QuadPart)) != NULL) {
        while (done < (size_t)size.QuadPart) {
            DWORD got = 0, want = (DWORD)((size_t)size.QuadPart - done > (1u << 24) ? (1u << 24) : (size_t)size.QuadPart - done);
            if (!ReadFile(h, buf + done, want, &got, NULL) || !got) break;
            done += got;
        }
        if (done != (size_t)size.QuadPart) { free(buf); buf = NULL; } else *len = done;
    }
    CloseHandle(h);
    return buf;
}

int sh_modvt_lit_compose(int square, uint8_t **out, size_t *out_len, char *why, size_t cap)
{
    sh_vtp_mod *mods;
    sh_vtp_member *mem;
    uint8_t **bytes;
    int i, n = 0, ok, s = mv_lit_slot(square);
    *out = NULL;
    *out_len = 0;
    AcquireSRWLockShared(&g_lit_lock);
    if (s < 0 || !g_stock[s].ok) {
        ReleaseSRWLockShared(&g_lit_lock);
        _snprintf_s(why, cap, _TRUNCATE, "the game's square %d member is not available", square);
        return 0;
    }
    mods = (sh_vtp_mod *)calloc((size_t)(g_nlit ? g_nlit : 1), sizeof *mods);
    mem = (sh_vtp_member *)calloc((size_t)(g_nlit ? g_nlit : 1), sizeof *mem);
    bytes = (uint8_t **)calloc((size_t)(g_nlit ? g_nlit : 1), sizeof *bytes);
    if (!mods || !mem || !bytes) {
        ReleaseSRWLockShared(&g_lit_lock);
        free(mods); free(mem); free(bytes);
        _snprintf_s(why, cap, _TRUNCATE, "out of memory");
        return 0;
    }
    for (i = 0; i < g_nlit; ++i) {
        mv_lit_mod *m = &g_lit[i];
        sh_modserve_hit hit;
        size_t len = 0;
        sh_vtp_lit lit;
        char w2[300];
        if (!m->served || !m->placed || m->node.square != square) continue;
        if (!sh_modserve_lookup(m->request, &hit) || strcmp(hit.bundle_hash, m->hash) ||
            !(bytes[n] = mv_read_disk(hit.disk_path, SH_MODSERVE_MAX_FILE, &len))) {
            mv_log("lit pages: %s of %s could not be read; its textures are left out", m->request, m->salt);
            continue;
        }
        memset(&lit, 0, sizeof lit);
        if (!sh_vtp_open(&mem[n], bytes[n], len, w2, sizeof w2) || !sh_vtp_lit_info(&mem[n], &lit, w2, sizeof w2) ||
            lit.node != m->size) {
            mv_log("lit pages: %s of %s is not what its build recorded (%s); its textures are left out", m->request,
                   m->salt, lit.node && lit.node != m->size ? "another block size" : w2);
            free(bytes[n]);
            bytes[n] = NULL;
            continue;
        }
        mods[n].key = m->key;
        mods[n].m = &mem[n];
        mods[n].node = m->node;
        n++;
    }
    ok = sh_vtp_compose(&g_stock[s].m, mods, n, out, out_len, why, cap);
    ReleaseSRWLockShared(&g_lit_lock);
    for (i = 0; i < n; ++i) free(bytes[i]);
    free(mods);
    free(mem);
    free(bytes);
    return ok;
}

int sh_modvt_lit_wanted(const char *request)
{
    char req[512];
    int i, k, r = 0;
    if (!request || !InterlockedCompareExchange(&g_lit_known, 0, 0)) return 0;
    strncpy_s(req, sizeof req, request, _TRUNCATE);
    mv_lower(req);
    if (strncmp(req, "generated/decls/material", 24)) return 0;
    AcquireSRWLockShared(&g_lit_lock);
    for (i = 0; i < g_nlit && !r; ++i)
        for (k = 0; g_lit[i].served && k < g_lit[i].nmats && !r; ++k) r = !strcmp(g_lit[i].mats[k], req);
    ReleaseSRWLockShared(&g_lit_lock);
    return r;
}

int sh_modvt_lit_material(const char *request, const char *text, size_t len, char **out, size_t *out_len)
{
    char req[512];
    int i, k, r = 0;
    *out = NULL;
    *out_len = 0;
    if (!request) return 0;
    strncpy_s(req, sizeof req, request, _TRUNCATE);
    mv_lower(req);
    if (strncmp(req, "generated/decls/material", 24)) return 0;
    AcquireSRWLockShared(&g_lit_lock);
    for (i = 0; i < g_nlit && !r; ++i) {
        const mv_lit_mod *m = &g_lit[i];
        if (!m->served) continue;
        for (k = 0; k < m->nmats && strcmp(m->mats[k], req); ++k) {}
        if (k == m->nmats || !sh_vtp_material_names(text, len, m->salt)) continue;
        *out = m->placed ? sh_vtp_material_moved(text, len, m->node, out_len) : sh_vtp_material_plain(text, len, out_len);
        r = *out ? 1 : -1;
    }
    ReleaseSRWLockShared(&g_lit_lock);
    return r;
}

#ifndef SH_MODVT_PURE
/* ================================================================== engine */
#include "signatures.h"
#include "commands.h"
#include "engine_globals.h"
#include "host_image.h"
#include "process_heap_scope.h"
#include "modstore.h"
#include "overrides.h"

#define MV_CODE 0xC0

typedef char (*mv_setsource_fn)(void *rec, const char *name, const char *path, int flavor, int type, char direct,
                                void *reader);
typedef void (*mv_release_fn)(void *rec);

static mv_setsource_fn g_setsource;
static mv_release_fn g_release;
static uint8_t *g_sys;
static uint32_t g_table;
static const uint64_t *g_sparse_ext;
static const int32_t *g_sparse_cvar;
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

/* Record 1 is the stock family, built by the VT start-up; until then no
 * family may be registered. */
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

int sh_modvt_engine_flavor(void)
{
    uint64_t ext = 0;
    int32_t prt = 0;
    if (!g_vulkan && g_sparse_ext) {
        __try { ext = *g_sparse_ext; prt = *g_sparse_cvar; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ext = 0; prt = 0; }
    }
    return sh_modvt_flavor(g_vulkan != 0, ext, prt);
}

static DWORD g_fault_code;
static int mv_filter(DWORD code) { g_fault_code = code; return EXCEPTION_EXECUTE_HANDLER; }

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

int sh_modvt_engine_release(int k, unsigned *fault)
{
    sh_process_heap_scope heap = { 0 };
    uint8_t *r = sh_modvt_engine_record(k);
    int ok = 0;
    if (fault) *fault = 0;
    if (!g_bound || !r || k < 1 || k > SH_MODVT_LAST_USED || sh_modvt_engine_on_main() != 1) return 0;
    if (!sh_process_heap_enter(&g_heap, &heap)) return 0;
    g_fault_code = 0;
    __try {
        __try { g_release(r); ok = 1; }
        __except (mv_filter(GetExceptionCode())) { ok = 0; }
    } __finally {
        if (!sh_process_heap_leave(&heap)) mv_log("Release: the process heap scope did not balance");
    }
    if (!ok) { if (fault) *fault = (unsigned)g_fault_code; return -1; }
    return 1;
}

/* ------------------------------------------------------------------ ops */

static int op_ready(void *c) { (void)c; return sh_modvt_engine_ready(); }

static int op_flavor(void *c) { (void)c; return sh_modvt_engine_flavor(); }

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

static int op_set_source(void *c, int k, const char *name, const char *path, int flavor)
{
    int result = 0;
    double ms = 0;
    unsigned fault = 0;
    int r = sh_modvt_engine_set_source(k, name, path, flavor, &result, &ms, &fault);
    (void)c;
    if (r < 0) { mv_log("SetSource(record %d, '%s') FAULTED (exception 0x%08X)", k, name, fault); return -1; }
    if (r == 0) { mv_log("SetSource(record %d, '%s') not called (heap scope or main thread)", k, name); return 0; }
    if (result) {
        short levels = 0;
        __try { levels = *(const short *)(sh_modvt_engine_record(k) + SH_VT_REC_LEVELS); }
        __except (EXCEPTION_EXECUTE_HANDLER) { levels = 0; }
        mv_log("SetSource(record %d, '%s', '%s', flavor %d, type 2) -> 1 in %.1f ms; %d levels", k, name, path, flavor,
               ms, (int)levels);
    }
    return result ? 1 : 0;
}

static int op_release(void *c, int k)
{
    unsigned fault = 0;
    int r = sh_modvt_engine_release(k, &fault);
    (void)c;
    if (r < 0) mv_log("Release(record %d) FAULTED (exception 0x%08X)", k, fault);
    return r == 1;
}

static const sh_modvt_ops g_ops = { NULL, op_ready, op_flavor, op_find, op_is_free, op_set_source, op_release };

/* ------------------------------------------------------------------ points */

static void mv_sync_now(const char *phase)
{
    sh_modvt_family *fams;
    sh_modvt_result res;
    int n;
    fams = (sh_modvt_family *)calloc(SH_MODVT_MAX, sizeof *fams);
    if (!fams) { mv_log("%s: sync refused (out of memory)", phase); return; }
    n = sh_modvt_collect(fams, SH_MODVT_MAX);
    if (n > SH_MODVT_MAX) {
        mv_log("%s: %d families indexed; only the first %d are considered", phase, n, SH_MODVT_MAX);
        n = SH_MODVT_MAX;
    }
    if (!n && !sh_modvt_owned_count()) { free(fams); InterlockedExchange(&g_pending, 0); return; }
    if (sh_modvt_sync(fams, n, &g_ops, phase, &res) < 0) {
        if (!InterlockedExchange(&g_waiting_logged, 1))
            mv_log("%s: %d famil%s indexed; the virtual-texture system is not up yet, so registration waits for the "
                   "next pass", phase, n, n == 1 ? "y" : "ies");
        InterlockedExchange(&g_pending, 1);
    } else {
        InterlockedExchange(&g_pending, 0);
    }
    free(fams);
}

/* The serving index listener: any thread. */
static void mv_index_changed(void)
{
    if (!g_bound) return;
    if (sh_modvt_engine_on_main() == 1) { mv_sync_now("index"); return; }
    InterlockedExchange(&g_pending, 1);
    {
        int n = sh_modvt_collect(NULL, 0);
        if (n || sh_modvt_owned_count())
            mv_log("index: rebuilt off the main thread with %d famil%s; registration runs at the next editor entry or "
                   "map load (or 'sh_vt sync')", n, n == 1 ? "y" : "ies");
    }
}

static void mv_lit_pass(const char *phase);   /* lit pages, below */

void sh_modvt_before_pass(const char *phase)
{
    if (!g_bound) return;
    if (sh_modvt_engine_on_main() != 1) return;
    mv_sync_now(phase);
    sh_modvt_mark_exposed();
    if (sh_modvt_engine_ready()) mv_lit_pass(phase);
}

void sh_modvt_print_status(void)
{
    int k, n = sh_modvt_collect(NULL, 0), first = 1;
    char held[160] = "";
    size_t used = 0;
    for (k = 1; k < SH_MODVT_SLOTS; ++k)
        if (sh_modvt_record_owner(k)) {
            int w = _snprintf_s(held + used, sizeof held - used, _TRUNCATE, "%s%d", first ? "" : ", ", k);
            if (w > 0) used += (size_t)w;
            first = 0;
        }
    sh_printf("VT: families: %d indexed, %d registered%s%s%s; %s, %s, matched flavor %d%s\n", n, sh_modvt_owned_count(),
              held[0] ? " (records " : "", held, held[0] ? ")" : "",
              g_bound ? "registration bound" : "registration unavailable",
              g_vulkan == 1 ? "Vulkan" : g_vulkan == 0 ? "OpenGL" : "unknown renderer",
              g_bound ? sh_modvt_engine_flavor() : -1,
              g_pending ? "; a sync is pending" : "");
    sh_modvt_lit_status();
}

/* sh_vt sync: run the sync now from the console (main thread). */
void sh_modvt_console_sync(void)
{
    if (!g_bound) { sh_printf("VT: registration unavailable on this build\n"); return; }
    if (sh_modvt_engine_on_main() != 1) { sh_printf("VT: sync refused: not on the main thread\n"); return; }
    mv_sync_now("console");
    sh_modvt_print_status();
}

/* ------------------------------------------------------------------ lit pages: serving */

typedef void (*mv_exec_fn)(void *cmdsys, const char *text);
static void *g_cmdsys;
static volatile LONG64 g_engine_sig[MV_LIT_SQUARES];   /* what record 1 reads for each square: 0 the game's file */
static volatile LONG g_lit_opens;
static char g_lit_cache[MV_LIT_SQUARES][MAX_PATH * 3]; /* each square's merged member served now (UTF-8) */
static uint64_t g_lit_cache_sig[MV_LIT_SQUARES];
static SRWLOCK g_cache_lock2 = SRWLOCK_INIT;

/* The stock family's member of a square the lit pages use, as the engine
 * names it: record 1's path is absolute (<DOOM>/virtualtextures/_vmtr.pages),
 * so its members are asked for at <DOOM>/virtualtextures/_vmtr_sq<N>.mega2.
 * Returns that square's slot, else -1. */
static int mv_lit_request_slot(const char *request)
{
    char r[1024], want[64];
    size_t n, k;
    int s;
    if (!request || (n = strlen(request)) >= sizeof r) return -1;
    strcpy_s(r, sizeof r, request);
    mv_lower(r);
    for (s = 0; s < MV_LIT_SQUARES; ++s) {
        k = (size_t)_snprintf_s(want, sizeof want, _TRUNCATE, "virtualtextures/_vmtr_sq%d.mega2", g_lit_square[s]);
        if (n >= k && !strcmp(r + n - k, want) && (n == k || r[n - k - 1] == '/')) return s;
    }
    return -1;
}

/* <mods>\.served\vt: the merged members, one per square and served set. */
static int mv_lit_dir(char *out, size_t cap)
{
    char root[MAX_PATH * 3];
    if (!sh_ms_root(root, sizeof root)) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\.served\\vt", root);
    return 1;
}

static int mv_mkdirs_utf8(const char *dir)
{
    wchar_t w[1024], *p;
    if (!MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, (int)(sizeof w / sizeof w[0]))) return 0;
    for (p = w + 3; *p; ++p)
        if (*p == L'\\') { *p = 0; CreateDirectoryW(w, NULL); *p = L'\\'; }
    return CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int mv_write_atomic(const char *path, const uint8_t *bytes, size_t len)
{
    wchar_t w[1024], t[1040];
    HANDLE h;
    size_t done = 0;
    int ok = 1;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, (int)(sizeof w / sizeof w[0]))) return 0;
    _snwprintf_s(t, sizeof t / sizeof t[0], _TRUNCATE, L"%s.%lu.tmp", w, GetCurrentProcessId());
    h = CreateFileW(t, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    while (ok && done < len) {
        DWORD got = 0, want = (DWORD)(len - done > (1u << 24) ? (1u << 24) : len - done);
        ok = WriteFile(h, bytes + done, want, &got, NULL) && got == want;
        done += got;
    }
    ok = CloseHandle(h) && ok;
    if (ok) ok = MoveFileExW(t, w, MOVEFILE_REPLACE_EXISTING) != 0;
    if (!ok) DeleteFileW(t);
    return ok;
}

/* Remove every other merged member of that square from the folder. */
static void mv_lit_prune(const char *dir, int square, const char *keep_name)
{
    wchar_t pat[1024], wkeep[128], path[1100], wdir[1024];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char pdir[MAX_PATH * 3];
    _snprintf_s(pdir, sizeof pdir, _TRUNCATE, "%s\\sq%d-*.mega2", dir, square);
    if (!MultiByteToWideChar(CP_UTF8, 0, pdir, -1, pat, (int)(sizeof pat / sizeof pat[0])) ||
        !MultiByteToWideChar(CP_UTF8, 0, keep_name ? keep_name : "", -1, wkeep, (int)(sizeof wkeep / sizeof wkeep[0])) ||
        !MultiByteToWideChar(CP_UTF8, 0, dir, -1, wdir, (int)(sizeof wdir / sizeof wdir[0])))
        return;
    h = FindFirstFileW(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!_wcsicmp(fd.cFileName, wkeep)) continue;
        _snwprintf_s(path, sizeof path / sizeof path[0], _TRUNCATE, L"%s\\%s", wdir, fd.cFileName);
        DeleteFileW(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Square slot s's merged member of the served set, written once under
 * .served\vt. 1 with its path, 0 when no served mod is placed there, -1 when
 * it could not be built. */
static int mv_lit_member(int s, char *path, size_t cap, uint64_t *sig_out)
{
    char dir[MAX_PATH * 3], name[64], why[300];
    int square = g_lit_square[s];
    uint64_t sig = sh_modvt_lit_signature(square);
    uint8_t *img = NULL;
    size_t len = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    wchar_t w[1024];
    *sig_out = sig;
    if (!sig) return 0;
    AcquireSRWLockExclusive(&g_cache_lock2);
    if (g_lit_cache_sig[s] == sig && g_lit_cache[s][0]) {
        strcpy_s(path, cap, g_lit_cache[s]);
        ReleaseSRWLockExclusive(&g_cache_lock2);
        return 1;
    }
    if (!mv_lit_dir(dir, sizeof dir) || !mv_mkdirs_utf8(dir)) {
        ReleaseSRWLockExclusive(&g_cache_lock2);
        mv_log("lit pages: the mods folder's .served\\vt folder could not be made; mods' lit textures are not served");
        return -1;
    }
    _snprintf_s(name, sizeof name, _TRUNCATE, "sq%d-%016llx.mega2", square, (unsigned long long)sig);
    _snprintf_s(path, cap, _TRUNCATE, "%s\\%s", dir, name);
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, (int)(sizeof w / sizeof w[0])) ||
        !GetFileAttributesExW(w, GetFileExInfoStandard, &fa)) {
        /* Not built for this set yet (a file of that name was built for the same set). */
        ULONGLONG t0 = GetTickCount64();
        if (!sh_modvt_lit_compose(square, &img, &len, why, sizeof why)) {
            ReleaseSRWLockExclusive(&g_cache_lock2);
            mv_log("lit pages: the merged square %d could not be built (%s); mods' lit textures there are not served",
                   square, why);
            return -1;
        }
        if (!mv_write_atomic(path, img, len)) {
            free(img);
            ReleaseSRWLockExclusive(&g_cache_lock2);
            mv_log("lit pages: %s could not be written; mods' lit textures there are not served", path);
            return -1;
        }
        free(img);
        mv_log("lit pages: built %s (%zu bytes) in %llu ms", name, len, (unsigned long long)(GetTickCount64() - t0));
    }
    strcpy_s(g_lit_cache[s], sizeof g_lit_cache[s], path);
    g_lit_cache_sig[s] = sig;
    ReleaseSRWLockExclusive(&g_cache_lock2);
    mv_lit_prune(dir, square, name);
    return 1;
}

int sh_modvt_open(const char *request, unsigned char **bytes, size_t *length)
{
    int s;
    *bytes = NULL;
    *length = 0;
    if (!request) return 0;
    if ((s = mv_lit_request_slot(request)) >= 0) {
        char path[MAX_PATH * 3];
        uint64_t sig = 0;
        size_t len = 0;
        unsigned char *raw, *heap;
        if (!InterlockedCompareExchange(&g_lit_known, 0, 0)) (void)sh_modvt_lit_refresh();
        if (mv_lit_member(s, path, sizeof path, &sig) != 1) { InterlockedExchange64(&g_engine_sig[s], 0); return 0; }
        raw = mv_read_disk(path, (size_t)-1 / 2, &len);
        heap = raw ? (unsigned char *)HeapAlloc(GetProcessHeap(), 0, len) : NULL;
        if (!heap) {
            free(raw);
            mv_log("lit pages: %s could not be read; the game's own square %d is used", path, g_lit_square[s]);
            InterlockedExchange64(&g_engine_sig[s], 0);
            return 0;
        }
        memcpy(heap, raw, len);
        free(raw);
        InterlockedExchange64(&g_engine_sig[s], (LONG64)sig);
        InterlockedIncrement(&g_lit_opens);
        *bytes = heap;
        *length = len;
        return 1;
    }
    if (sh_modvt_lit_wanted(request)) {
        unsigned char *orig = NULL;
        size_t olen = 0, nlen = 0;
        char *moved = NULL;
        int r;
        if (sh_modserve_open(request, &orig, &olen) != 1) return 0;
        r = sh_modvt_lit_material(request, (const char *)orig, olen, &moved, &nlen);
        if (r == 1) {
            unsigned char *heap = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, nlen ? nlen : 1);
            if (heap) {
                memcpy(heap, moved, nlen);
                free(moved);
                HeapFree(GetProcessHeap(), 0, orig);
                *bytes = heap;
                *length = nlen;
                return 1;
            }
        }
        free(moved);
        if (r < 0) mv_log("lit pages: %s names its mod's lit pages but has no virtualmapping or closing brace line; it is "
                          "served as the mod carries it", request);
        /* Not a lit material: the caller's own serving answers it. */
        HeapFree(GetProcessHeap(), 0, orig);
    }
    return 0;
}

static int mv_lit_engine_current(void)
{
    int s;
    for (s = 0; s < MV_LIT_SQUARES; ++s)
        if ((uint64_t)InterlockedCompareExchange64(&g_engine_sig[s], 0, 0) != sh_modvt_lit_signature(g_lit_square[s]))
            return 0;
    return 1;
}

/* Run the engine's own reload of the stock family's members inside process
 * heap 0: vt_reload (handler VK 0x1417E3BE0, GL 0x1417E4A80) calls SetSource
 * again for every record, so record 1's mega2 loader re-opens _vmtr_sq1 to
 * _vmtr_sq16 through the provider, and rebinds the materials.
 * vt_reloadMega2Vmtrs (VK 0x1417E3830, GL handler 0x1417E48D0) cannot be
 * used: it first counts each record's members through the file system's
 * exists slot (+0xC0), which answers false for record 1's absolute path, so it
 * always returns before reloading. */
static int mv_lit_reload(const char *phase)
{
    sh_process_heap_scope heap = { 0 };
    uint8_t *rec = sh_modvt_engine_record(1);
    void *before = NULL, *after = NULL;
    mv_exec_fn exec = NULL;
    int ok = 0;
    if (!g_cmdsys || !rec) {
        mv_log("%s: lit pages: the console command system is unbound; the texture files are not reloaded", phase);
        return 0;
    }
    __try { exec = *(mv_exec_fn *)(*(uint8_t **)g_cmdsys + 0x48); }
    __except (EXCEPTION_EXECUTE_HANDLER) { exec = NULL; }
    if (!exec) return 0;
    (void)mv_rd_ptr(rec + SH_VT_REC_MEGA2, &before);
    if (!sh_process_heap_enter(&g_heap, &heap)) return 0;
    __try {
        __try { exec(g_cmdsys, "vt_reload"); ok = 1; }
        __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    } __finally {
        if (!sh_process_heap_leave(&heap)) mv_log("vt_reload: the process heap scope did not balance");
    }
    (void)mv_rd_ptr(rec + SH_VT_REC_MEGA2, &after);
    if (!ok) { mv_log("%s: lit pages: vt_reload FAULTED", phase); return 0; }
    if (after == before) {
        mv_log("%s: lit pages: vt_reload left the stock family's reader as it was; the textures show after DOOM "
               "restarts", phase);
        return 0;
    }
    if (!mv_lit_engine_current()) {
        mv_log("%s: lit pages: the engine reloaded but did not read every merged member", phase);
        return 0;
    }
    return 1;
}

static void mv_lit_pass(const char *phase)
{
    int i, s, served, placed = 0, notfit = 0, built = 1;
    char path[MAX_PATH * 3];
    AcquireSRWLockExclusive(&g_lit_lock);
    served = mv_lit_refresh_locked();
    for (i = 0; i < g_nlit; ++i) {
        mv_lit_mod *m = &g_lit[i];
        if (!m->served) { m->told = 0; continue; }
        if (m->placed) { placed++; continue; }
        notfit++;
        if (!m->told) {
            char label[300];
            if (!sh_modserve_bundle_label(m->hash, label, sizeof label)) strcpy_s(label, sizeof label, "a mod");
            mv_log("%s: %s's lit textures do not fit in the game's free texture space beside the other mods this map "
                   "uses, so it draws with its plain images; remove a mod with lit textures from this map, or ask the "
                   "mod's author to bake its textures smaller", phase, label);
            m->told = 1;
        }
    }
    ReleaseSRWLockExclusive(&g_lit_lock);
    if (mv_lit_engine_current()) return;
    /* vt_reload faults in its GPU synchronisation when no map is loaded (the
     * boot editor pass at the hub: VK rip 0x1A49A15 under SetSource), so the
     * engine reads a changed set at the first pass of a loaded map. */
    if (!sh_modserve_scope_map_open()) return;
    for (s = 0; s < MV_LIT_SQUARES; ++s) {
        uint64_t sig;
        if (mv_lit_member(s, path, sizeof path, &sig) < 0) built = 0;
    }
    if (!built) return;
    if (mv_lit_reload(phase))
        mv_log("%s: lit pages: %d mod%s' pages placed (%d served, %d without room); the engine reloaded its texture files",
               phase, placed, placed == 1 ? "'s" : "s", served, notfit);
}

void sh_modvt_lit_status(void)
{
    int i, s, served = 0, placed = 0;
    AcquireSRWLockShared(&g_lit_lock);
    for (i = 0; i < g_nlit; ++i) {
        const mv_lit_mod *m = &g_lit[i];
        if (m->served) served++;
        if (m->served && m->placed) placed++;
        if (m->placed)
            sh_printf("VT: lit %s@%.12s: block %d at %d,%d of square %d, %d material%s%s\n", m->salt, m->hash, m->size,
                      m->node.x, m->node.y, m->node.square, m->nmats, m->nmats == 1 ? "" : "s",
                      m->served ? ", served" : "");
        else
            sh_printf("VT: lit %s@%.12s: block %d, no place%s\n", m->salt, m->hash, m->size, m->served ? ", served" : "");
    }
    ReleaseSRWLockShared(&g_lit_lock);
    for (s = 0; s < MV_LIT_SQUARES; ++s) {
        uint64_t have = (uint64_t)InterlockedCompareExchange64(&g_engine_sig[s], 0, 0);
        uint64_t want = sh_modvt_lit_signature(g_lit_square[s]);
        sh_printf("VT: lit pages: square %d the engine reads: %s%s\n", g_lit_square[s],
                  have ? "merged" : "the game's", want != have ? "; a reload is pending" : "");
    }
    sh_printf("VT: lit pages: %d served, %d placed, %d member opens\n", served, placed, (int)g_lit_opens);
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
    const sig_result *ss = mv_clean(results, count, "VtSetSource");
    const sig_result *vf = mv_clean(results, count, "VtForMaterial");
    const sh_build_identity *id = sh_host_identity();
    uint8_t sc[MV_CODE], vc[MV_CODE], rc[0x50];
    sh_modvt_layout l;
    int n;
    g_echo = mv_echo;
    g_vulkan = id ? id->vulkan : -1;
    g_main_thread_at = (const uint8_t *)glb_resolve(module_base, "main_thread_id", NULL);
    if (!ss || !vf) {
        mv_log("mod families unavailable: %s is not a clean signature match", !ss ? "VtSetSource" : "VtForMaterial");
        return 0;
    }
    if (!sh_process_heap_bind(&g_heap, results, count, module_base) || !g_main_thread_at) {
        mv_log("mod families unavailable: %s", g_main_thread_at ? "the process heap scope is unbound" : "main_thread_id is unresolved");
        return 0;
    }
    if (!mv_rd_code((const uint8_t *)ss->addr, sc, sizeof sc) || !mv_rd_code((const uint8_t *)vf->addr, vc, sizeof vc) ||
        !sh_modvt_decode_layout(sc, sizeof sc, ss->rva, vc, sizeof vc, vf->rva, g_vulkan == 0, &l) ||
        !mv_rd_code(module_base + l.release_rva, rc, sizeof rc) || !sh_modvt_release_head_ok(rc, sizeof rc)) {
        mv_log("mod families unavailable: the VT system, Release, the record table or (OpenGL) the sparse test could not "
               "be decoded from the signed code");
        return 0;
    }
    g_sys = (uint8_t *)module_base + l.sys_rva;
    g_table = l.table_off;
    g_release = (mv_release_fn)(module_base + l.release_rva);
    g_setsource = (mv_setsource_fn)ss->addr;
    if (l.sparse_ext_rva) {
        g_sparse_ext = (const uint64_t *)(module_base + l.sparse_ext_rva);
        g_sparse_cvar = (const int32_t *)(module_base + l.sparse_cvar_rva);
    }
    g_bound = 1;
    g_cmdsys = sh_resolve_cmdsys(results, count, module_base);
    {
        /* The game's own members of the squares the lit pages use, the base of every merged member. */
        const wchar_t *dir = sh_host_dir();
        int s;
        for (s = 0; s < MV_LIT_SQUARES; ++s) {
            wchar_t path[1024];
            char utf8[MAX_PATH * 3];
            size_t len = 0;
            unsigned char *stock = NULL;
            int square = sh_modvt_lit_square(s);
            if (dir) {
                _snwprintf_s(path, sizeof path / sizeof path[0], _TRUNCATE, L"%s\\virtualtextures\\_vmtr_sq%d.mega2", dir, square);
                if (WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, (int)sizeof utf8, NULL, NULL))
                    stock = mv_read_disk(utf8, 64u << 20, &len);
            }
            if (stock) (void)sh_modvt_lit_set_stock(square, stock, len);
            else mv_log("lit pages: the game's virtualtextures/_vmtr_sq%d.mega2 could not be read; no lit pages are placed there", square);
            free(stock);
        }
    }
    mv_log("mod families bound on %s: SetSource rva 0x%X, Release rva 0x%X, VirtualTextureForMaterial rva 0x%X, VT "
           "system rva 0x%X, record table sys+0x%X", g_vulkan == 1 ? "Vulkan" : g_vulkan == 0 ? "OpenGL" : "an unknown image",
           ss->rva, l.release_rva, vf->rva, l.sys_rva, l.table_off);
    sh_modserve_set_index_listener(mv_index_changed);
    sh_overrides_set_vt_open(sh_modvt_open);
    n = sh_modvt_collect(NULL, 0);
    if (n) {
        InterlockedExchange(&g_pending, 1);
        mv_log("%d famil%s indexed at boot; registered at the first editor entry or map load", n, n == 1 ? "y" : "ies");
    }
    return 1;
}
#endif /* SH_MODVT_PURE */
