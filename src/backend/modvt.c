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
 *                  (CMP QWORD [ext],0; CMP DWORD [cvar],0; MOV EAX,4) gives the two globals.
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
                            const char *version, const char *bundle_hash)
{
    mv_collect *c = (mv_collect *)context;
    char fam[SH_MODVT_NAME_MAX];
    int member = 0, i;
    if (!sh_mb_vt_member(request, fam, sizeof fam, &member)) return;
    for (i = 0; i < c->n && i < c->cap; ++i)
        if (!strcmp(c->out[i].name, fam)) {
            /* A second bundle serving the same family name: the first keeps it. */
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
        _snprintf_s(f->bundle, sizeof f->bundle, _TRUNCATE, "%s@%s", bundle_id, version);
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
    sh_modserve_each_served(mv_collect_file, &c);
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

enum { MV_TAKEN = 1, MV_RENDERER, MV_FULL, MV_SETSOURCE, MV_FAULT };

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
        if (flavor != 2 && flavor != 4) {
            if (mv_refusal_new(f, MV_RENDERER))
                mv_log("%s: family '%s' (%s) cannot be served on this renderer: OpenGL binds a private family only "
                       "through sparse page tables (GL_AMD_sparse_texture and its cvar); its materials draw with "
                       "default images", phase, f->name, f->bundle);
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
    *out = l;
    return 1;
}

#ifndef SH_MODVT_PURE
/* ================================================================== engine */
#include "signatures.h"
#include "commands.h"
#include "engine_globals.h"
#include "host_image.h"
#include "process_heap_scope.h"

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
    int32_t cvar = 0;
    if (g_vulkan) return 2;
    if (!g_sparse_ext || !g_sparse_cvar) return 2;
    __try { ext = *g_sparse_ext; cvar = *g_sparse_cvar; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 2; }
    return (ext && cvar) ? 4 : 2;
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

static int op_flavor(void *c)
{
    (void)c;
    if (g_vulkan) return 2;
    return sh_modvt_engine_flavor() == 4 ? 4 : 0;
}

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

void sh_modvt_before_pass(const char *phase)
{
    if (!g_bound) return;
    if (sh_modvt_engine_on_main() != 1) return;
    mv_sync_now(phase);
    sh_modvt_mark_exposed();
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
}

/* sh_vt sync: run the sync now from the console (main thread). */
void sh_modvt_console_sync(void)
{
    if (!g_bound) { sh_printf("VT: registration unavailable on this build\n"); return; }
    if (sh_modvt_engine_on_main() != 1) { sh_printf("VT: sync refused: not on the main thread\n"); return; }
    mv_sync_now("console");
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
        mv_log("mod families unavailable: the VT system, Release or the record table could not be decoded from the "
               "signed code");
        return 0;
    }
    g_sys = (uint8_t *)module_base + l.sys_rva;
    g_table = l.table_off;
    g_release = (mv_release_fn)(module_base + l.release_rva);
    g_setsource = (mv_setsource_fn)ss->addr;
    if (l.sparse_ext_rva && l.sparse_cvar_rva) {
        g_sparse_ext = (const uint64_t *)(module_base + l.sparse_ext_rva);
        g_sparse_cvar = (const int32_t *)(module_base + l.sparse_cvar_rva);
    }
    g_bound = 1;
    mv_log("mod families bound on %s: SetSource rva 0x%X, Release rva 0x%X, VirtualTextureForMaterial rva 0x%X, VT "
           "system rva 0x%X, record table sys+0x%X%s", g_vulkan == 1 ? "Vulkan" : g_vulkan == 0 ? "OpenGL" : "an unknown image",
           ss->rva, l.release_rva, vf->rva, l.sys_rva, l.table_off,
           g_vulkan ? "" : (g_sparse_ext ? "; sparse test decoded" : "; sparse test NOT decoded (families are not served on OpenGL)"));
    sh_modserve_set_index_listener(mv_index_changed);
    n = sh_modvt_collect(NULL, 0);
    if (n) {
        InterlockedExchange(&g_pending, 1);
        mv_log("%d famil%s indexed at boot; registered at the first editor entry or map load", n, n == 1 ? "y" : "ies");
    }
    return 1;
}
#endif /* SH_MODVT_PURE */
