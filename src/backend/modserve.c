/* modserve.c -- see modserve.h. The index, the open path, the registration
 * plan and the queries. No engine calls: those live in modserve_engine.c
 * behind sh_modserve_engine_ops, so this file builds into the native tests.
 *
 * Table shape: one open-addressing array of {hash, key, file} sized to a
 * power of two at least twice the key count. Keys are lowercase with '/'.
 * A lookup folds the request while hashing (FNV-1a 64), probes linearly and
 * compares folded bytes, so the provider path never copies or allocates.
 * Keys per file: the request path; for a decl also its decltree/ spelling
 * (DeclRegisterFile reads the source through that name); and the engine's
 * sanitised spelling when it differs (the resource name builder turns every
 * character outside a-z 0-9 $ . / into '_', lane R1 section 1i).
 */
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shlobj.h>
#include <ctype.h>
#include "modserve.h"
#include "modbundle.h"
#include "config_json.h"
#include "backend_log.h"

#define MS_NAME_MAX      1024u
#define MS_JSON_DEPTH    16u
#define MS_CAUSE_MAX     200
#define MS_CONFLICT_LOGS 32
#define MS_CACHE_SLOTS   64

/* ------------------------------------------------------------------ types */

typedef struct ms_need { const char *type, *name; int file; } ms_need;   /* file: the served provider, or -1 */

typedef struct ms_file {
    int bundle;
    const char *request, *disk, *blob;
    uint64_t size;
    const char *ptype, *pname, *reg;
    int is_decl, is_shadow, is_sedef, rank;
    int is_restore;                       /* a stock pointer restoring installed content under its own name */
    int need_first, need_count;
    int served;
    const unsigned char *body;            /* composed bytes served instead of the disk file (arena), or NULL */
    size_t body_len;
    const char *stock;                    /* a stock pointer's installed source (blob is its SHA-256), or NULL */
    volatile LONG opens, fail_logged, shadow_logged;
    volatile LONG reg_state;              /* 0 none, 1 ready, -1 failed (main thread) */
    volatile LONG withdrawn;              /* a shadow taken back by a registration pass: lookups miss it */
    volatile LONG reparse_tried;          /* the early-decl pass re-parsed it (main thread) */
    volatile LONG opened_ungated;         /* first served while one of its bundle's requirement gates was not live */
    volatile LONG refresh;                /* 1: its bytes changed at a rescan and the engine may hold the old ones */
    volatile LONG refresh_gen;            /* 0 changed; N: inherits (N levels up) from a changed or gone decl */
    volatile LONG refresh_new;            /* a new gameplay decl of a mod this session registered: register it now */
} ms_file;

/* One path several bundles provide with different bytes, and what serves it. */
typedef struct ms_conflict {
    const char *request;
    const char *providers;                /* "a@1, b@2" in index order */
    const char *reason;                   /* why the winner was chosen, or how the text was composed */
    int winner;                           /* the file that serves the path */
    int count;                            /* providers */
    int composed;                         /* 1: the list entries of every provider were merged */
} ms_conflict;

typedef struct ms_bundle {
    const char *dir, *folder, *id, *version, *hash, *name;
    const char *extra;                    /* the author block's other keys (and a product-block legacy) as JSON, or NULL */
    int legacy, admitted, first_file, file_count;   /* legacy: a package a map delivered */
    int grandfathered;                    /* a "legacy" object (author or product block): shadows may be new names */
    uint64_t total;                       /* sum of file sizes (the manifest row's size) */
    unsigned req;                         /* SH_MODSERVE_REQ_* bits from mod.json requirements */
    double installed_at;
    char cause[MS_CAUSE_MAX];
} ms_bundle;

typedef struct ms_slot { uint64_t h; const char *key; int file; } ms_slot;

typedef struct ms_chunk { struct ms_chunk *next; size_t used, cap; char data[1]; } ms_chunk;

/* A request the replaced table served and this one does not (refresh). */
struct ms_index;
typedef struct ms_gone { const struct ms_index *from; int file; volatile LONG pending; } ms_gone;

typedef struct ms_index {
    char root[MAX_PATH];
    ms_chunk *arena;
    ms_bundle *bundles; int nbundles, cbundles;
    ms_file *files; int nfiles, cfiles;
    ms_need *needs; int nneeds, cneeds;
    ms_slot *slots; uint32_t mask; int nkeys;
    ms_slot *pslots; uint32_t pmask;
    ms_conflict *conflict_list; int nconflict_list, cconflict_list;   /* sorted by request */
    struct { const char *from, *to; } *aliases; int naliases;         /* dropped copy's hash -> kept bundle's hash */
    int admitted, served_files, conflicts, composed, shadows;
    double build_ms;
    struct ms_index *prev;                /* the table this one replaced (retired, never freed) */
    ms_gone *gone; int ngone;             /* refresh: requests no longer served (pending gone shadows carried) */
    sh_modserve_diff diff;                /* refresh: this table against prev */
    volatile LONG expanded;               /* refresh: the inheritance closure was marked */
} ms_index;

static ms_index *volatile g_index;
static int ms_bundle_by_hash(const ms_index *x, const char *hash, int admitted_only);
static unsigned char *ms_file_raw(const ms_file *f, size_t *len);
/* Gates this layer changed and the values it displaced: a gate is restored
 * only when no admitted bundle needs it any more and nobody else has changed
 * it since. Main thread only, like every registration pass. */
static unsigned g_gates_owned;
static int g_gates_original[SH_MODSERVE_REQ_COUNT];
static SRWLOCK g_build_lock = SRWLOCK_INIT;
static char g_last_root[MAX_PATH];
static volatile LONG g_opens_served, g_open_failures, g_probe_claims, g_cache_hits;
static volatile LONG g_renderprog_logged;   /* render-program serve lines written */
static volatile LONG g_gates_live;          /* requirement gates the last pass read back as live (SH_MODSERVE_REQ_* bits) */
volatile LONG g_modserve_editor_ok, g_modserve_editor_failed;   /* written by modserve_engine.c */
#define MS_LISTENERS 4
static sh_modserve_index_fn volatile g_index_listener[MS_LISTENERS];   /* modvt.c, modsound.c, modserve_engine.c */

/* ------------------------------------------------------------------ logging */

static void ms_log(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    memcpy(line, "MODS: ", 6);
    va_start(ap, fmt);
    _vsnprintf_s(line + 6, sizeof line - 6, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static double ms_now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* ------------------------------------------------------------------ arena */

static void *ms_alloc(ms_index *x, size_t n)
{
    ms_chunk *c = x->arena;
    n = (n + 7u) & ~(size_t)7u;
    if (!c || c->cap - c->used < n) {
        size_t cap = n > 60000u ? n : 60000u;
        ms_chunk *k = (ms_chunk *)malloc(sizeof(ms_chunk) + cap);
        if (!k) return NULL;
        k->next = c; k->used = 0; k->cap = cap;
        x->arena = c = k;
    }
    c->used += n;
    return c->data + c->used - n;
}

static char *ms_strndup(ms_index *x, const char *s, size_t n)
{
    char *p = (char *)ms_alloc(x, n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static char *ms_strdup(ms_index *x, const char *s) { return ms_strndup(x, s, strlen(s)); }

static void ms_index_free(ms_index *x)
{
    ms_chunk *c, *n;
    if (!x) return;
    for (c = x->arena; c; c = n) { n = c->next; free(c); }
    free(x->bundles); free(x->files); free(x->needs); free(x->slots); free(x->pslots); free(x->conflict_list);
    free(x->aliases); free(x->gone);
    free(x);
}

/* ------------------------------------------------------------------ names */

static __inline char ms_fold(char c)
{
    if (c == '\\') return '/';
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

static void ms_lower(char *s) { for (; *s; ++s) *s = ms_fold(*s); }

static uint64_t ms_hash_folded(const char *s, size_t *out_len)
{
    uint64_t h = 1469598103934665603ULL;
    size_t n = 0;
    for (; s[n] && n < MS_NAME_MAX; ++n) {
        h ^= (unsigned char)ms_fold(s[n]);
        h *= 1099511628211ULL;
    }
    if (out_len) *out_len = s[n] ? MS_NAME_MAX : n;
    return h;
}

static int ms_folded_eq(const char *key, const char *s)
{
    for (;; ++key, ++s) {
        char a = *key, b = ms_fold(*s);
        if (a != b) return 0;
        if (!a) return 1;
    }
}

/* The engine's sanitised resource name: every character outside a-z 0-9 $ . /
 * becomes '_'. Input is already folded. */
static int ms_sanitize(const char *in, char *out, size_t cap)
{
    size_t i;
    int changed = 0;
    for (i = 0; in[i] && i + 1 < cap; ++i) {
        char c = in[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '$' || c == '.' || c == '/')) {
            c = '_';
            changed = 1;
        }
        out[i] = c;
    }
    out[i] = 0;
    return changed && !in[i];
}

static int ms_valid_id(const char *id)
{
    size_t i, n = strlen(id);
    if (!n || n > 64) return 0;
    if (!((id[0] >= 'a' && id[0] <= 'z') || (id[0] >= '0' && id[0] <= '9'))) return 0;
    for (i = 0; i < n; ++i) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
        if (c == '.' && id[i + 1] == '.') return 0;
    }
    return 1;
}

static int ms_valid_hash(char *h)
{
    size_t i;
    if (strlen(h) != SH_MODSERVE_HASH_HEX) return 0;
    for (i = 0; i < SH_MODSERVE_HASH_HEX; ++i) {
        h[i] = ms_fold(h[i]);
        if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f'))) return 0;
    }
    return 1;
}

/* A bundle-relative file path: assets/..., '/' separators, no traversal. */
static int ms_valid_rel(const char *p)
{
    size_t i, n = strlen(p);
    if (n < 8 || n >= 512 || strncmp(p, "assets/", 7)) return 0;
    if (strstr(p, "//") || strstr(p, "/./") || strstr(p, "/../") || p[n - 1] == '/' || p[n - 1] == '.') return 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)p[i];
        if (c < 0x20 || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|') return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ JSON reader
 * The document has passed sh_json_validate, so the reader only walks. */

typedef struct ms_json { const char *p, *e; } ms_json;

static const char *ms_jws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    return p;
}

static const char *ms_jskip_string(const char *p, const char *e)
{
    for (++p; p < e && *p != '"'; ++p) if (*p == '\\') ++p;
    return p < e ? p + 1 : e;
}

static const char *ms_jskip(const char *p, const char *e)
{
    int depth = 0;
    p = ms_jws(p, e);
    if (p >= e) return e;
    if (*p == '"') return ms_jskip_string(p, e);
    if (*p != '{' && *p != '[') {
        while (p < e && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' &&
               *p != '\n' && *p != '\r') ++p;
        return p;
    }
    while (p < e) {
        if (*p == '"') { p = ms_jskip_string(p, e); continue; }
        if (*p == '{' || *p == '[') ++depth;
        else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
        ++p;
    }
    return e;
}

/* Enter the container at c->p ('{' or '['). */
static int ms_jenter(ms_json *c, char open)
{
    c->p = ms_jws(c->p, c->e);
    if (c->p >= c->e || *c->p != open) return 0;
    ++c->p;
    return 1;
}

/* Advance to the next element; 0 at the container's end (consumed). */
static int ms_jnext(ms_json *c, char close)
{
    c->p = ms_jws(c->p, c->e);
    if (c->p < c->e && *c->p == ',') c->p = ms_jws(c->p + 1, c->e);
    if (c->p >= c->e) return 0;
    if (*c->p == close) { ++c->p; return 0; }
    return 1;
}

/* Read an object member: the decoded key into `key` and the value span. */
static int ms_jmember(ms_json *c, char *key, size_t cap, const char **v, const char **ve)
{
    const char *ks = c->p, *ke;
    size_t n = 0;
    if (*ks != '"') return 0;
    ke = ms_jskip_string(ks, c->e);
    if (!sh_json_decode_string(ks, (size_t)(ke - ks), key, cap, &n) || n >= cap) key[0] = 0;
    c->p = ms_jws(ke, c->e);
    if (c->p >= c->e || *c->p != ':') return 0;
    c->p = ms_jws(c->p + 1, c->e);
    *v = c->p;
    *ve = ms_jskip(c->p, c->e);
    c->p = *ve;
    return 1;
}

/* A string value decoded into the arena; NULL when it is not a string, holds
 * a NUL or exceeds the bound. */
static char *ms_jstr(ms_index *x, const char *v, const char *ve, size_t max)
{
    size_t n = 0, cap = (size_t)(ve - v) + 1;
    char *out;
    if (v >= ve || *v != '"' || cap > max + 3) return NULL;
    out = (char *)ms_alloc(x, cap);
    if (!out || !sh_json_decode_string(v, (size_t)(ve - v), out, cap, &n) || n >= cap || strlen(out) != n)
        return NULL;
    return out;
}

static int ms_jnum(const char *v, const char *ve, double *out)
{
    char buf[64];
    size_t n = (size_t)(ve - v);
    char *end;
    if (!n || n >= sizeof buf || !((*v >= '0' && *v <= '9') || *v == '-')) return 0;
    memcpy(buf, v, n);
    buf[n] = 0;
    *out = strtod(buf, &end);
    return end == buf + n;
}

/* ------------------------------------------------------------------ files */

/* Bundle trees hold engine paths deep enough to pass MAX_PATH below
 * %LOCALAPPDATA% (a converted first-generation package does), so files are
 * opened through the wide API with the \\?\ prefix, as the legacy converter
 * writes them. `path` is ANSI, absolute, with '\' separators. */
#define MS_PATH_MAX 1024
static int ms_wide(const char *path, wchar_t *out, size_t cap)
{
    size_t at = 0;
    if (path[0] && path[1] == ':' && path[2] == '\\') {
        if (cap < 5) return 0;
        wcscpy_s(out, cap, L"\\\\?\\");
        at = 4;
    }
    return MultiByteToWideChar(CP_ACP, 0, path, -1, out + at, (int)(cap - at)) > 0;
}

static unsigned char *ms_read_whole(const char *path, size_t max, size_t *len)
{
    HANDLE h;
    LARGE_INTEGER size;
    unsigned char *buf = NULL;
    DWORD got = 0;
    wchar_t wide[MS_PATH_MAX + 8];
    *len = 0;
    if (!ms_wide(path, wide, sizeof wide / sizeof wide[0])) return NULL;
    h = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && (uint64_t)size.QuadPart <= max) {
        buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, size.QuadPart ? (size_t)size.QuadPart : 1u);
        if (buf && size.QuadPart &&
            (!ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart)) {
            HeapFree(GetProcessHeap(), 0, buf);
            buf = NULL;
        }
        if (buf) *len = (size_t)size.QuadPart;
    }
    CloseHandle(h);
    return buf;
}

/* File size, or -1 when the path is not a regular file. */
static long long ms_file_size(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    wchar_t wide[MS_PATH_MAX + 8];
    if (!ms_wide(path, wide, sizeof wide / sizeof wide[0]) || !GetFileAttributesExW(wide, GetFileExInfoStandard, &a))
        return -1;
    if (a.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) return -1;
    return ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

/* ------------------------------------------------------------------ bundle parse */

static int ms_grow(void **arr, int *cap, int need, size_t elem)
{
    void *n;
    int c;
    if (need <= *cap) return 1;
    c = *cap ? *cap * 2 : 16;
    while (c < need) c *= 2;
    n = realloc(*arr, (size_t)c * elem);
    if (!n) return 0;
    *arr = n;
    *cap = c;
    return 1;
}

static void ms_fail(ms_bundle *b, const char *fmt, ...)
{
    va_list ap;
    if (b->cause[0]) return;   /* the first cause only */
    va_start(ap, fmt);
    _vsnprintf_s(b->cause, sizeof b->cause, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static int ms_rank(const char *type)
{
    if (!_stricmp(type, "md6Def")) return 1;
    if (!_stricmp(type, "entityDef")) return 2;
    if (!_stricmp(type, "snapEditorEntityDef")) return 3;
    return 0;
}

/* One files[] element. */
static int ms_str_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* One entry of the product block's "files" (pointer 0) or "stock" (pointer 1). */
static void ms_parse_file(ms_index *x, ms_bundle *b, ms_json *c, char **shadows, int nshadows, int pointer)
{
    char key[64];
    const char *v, *ve;
    char *path = NULL, *blob = NULL, *request = NULL, *ptype = NULL, *pname = NULL;
    const char *stock = NULL;
    double size = -1;
    int need_first = x->nneeds, need_count = 0;
    ms_file *f;
    if (!ms_jenter(c, '{')) { ms_fail(b, "a files[] entry is not an object"); return; }
    while (ms_jnext(c, '}')) {
        if (!ms_jmember(c, key, sizeof key, &v, &ve)) { ms_fail(b, "malformed files[] entry"); return; }
        if (!strcmp(key, "path")) path = ms_jstr(x, v, ve, 511);
        else if (!strcmp(key, "sha256") || !strcmp(key, "blob")) blob = ms_jstr(x, v, ve, 64);
        else if (pointer && !strcmp(key, "source")) stock = ms_jstr(x, v, ve, 511);
        else if (!strcmp(key, "stock")) {
            /* A stock pointer (modbundle.h): installed content, no blob, no size. */
            ms_json p = { v, ve };
            stock = "";
            if (ms_jenter(&p, '{')) while (ms_jnext(&p, '}')) {
                const char *pv, *pve;
                if (!ms_jmember(&p, key, sizeof key, &pv, &pve)) break;
                if (!strcmp(key, "source")) stock = ms_jstr(x, pv, pve, 511);
                else if (!strcmp(key, "sha256")) blob = ms_jstr(x, pv, pve, 64);
            }
        }
        else if (!strcmp(key, "request")) request = ms_jstr(x, v, ve, 511);
        else if (!strcmp(key, "size")) { if (!ms_jnum(v, ve, &size)) size = -1; }
        else if (!strcmp(key, "provides")) {
            ms_json p = { v, ve };
            if (ms_jenter(&p, '{')) while (ms_jnext(&p, '}')) {
                const char *pv, *pve;
                if (!ms_jmember(&p, key, sizeof key, &pv, &pve)) break;
                if (!strcmp(key, "type")) ptype = ms_jstr(x, pv, pve, 63);
                else if (!strcmp(key, "name")) pname = ms_jstr(x, pv, pve, 511);
            }
        } else if (!strcmp(key, "needs")) {
            ms_json a = { v, ve };
            if (ms_jenter(&a, '[')) while (ms_jnext(&a, ']')) {
                ms_json o = { a.p, ms_jskip(a.p, a.e) };
                char *nt = NULL, *nn = NULL;
                a.p = o.e;
                if (ms_jenter(&o, '{')) while (ms_jnext(&o, '}')) {
                    const char *nv, *nve;
                    if (!ms_jmember(&o, key, sizeof key, &nv, &nve)) break;
                    if (!strcmp(key, "type")) nt = ms_jstr(x, nv, nve, 63);
                    else if (!strcmp(key, "name")) nn = ms_jstr(x, nv, nve, 511);
                }
                if (!nt || !nn) continue;
                ms_lower(nn);
                if (!ms_grow((void **)&x->needs, &x->cneeds, x->nneeds + 1, sizeof(ms_need))) {
                    ms_fail(b, "out of memory");
                    return;
                }
                x->needs[x->nneeds].type = nt;
                x->needs[x->nneeds].name = nn;
                x->needs[x->nneeds].file = -1;
                x->nneeds++;
                need_count++;
            }
        }
    }
    if (pointer && !stock) stock = "";
    if (!path || !ms_valid_rel(path)) { ms_fail(b, "files[] path '%s' is not a plain assets/ path", path ? path : "?"); return; }
    if (stock) {
        if (!stock[0] || !blob || !ms_valid_hash(blob)) { ms_fail(b, "'%s' is a stock pointer without a source and hash", path); return; }
        size = 0;
    } else if (size < 0 || size > (double)SH_MODSERVE_MAX_FILE || size != (double)(uint64_t)size) {
        ms_fail(b, "'%s' has no valid size", path);
        return;
    }
    {
        char *derived = ms_strdup(x, path + 7);
        if (!derived) { ms_fail(b, "out of memory"); return; }
        ms_lower(derived);
        if (request) {
            ms_lower(request);
            if (strcmp(request, derived)) { ms_fail(b, "'%s' declares request '%s'", path, request); return; }
        }
        request = derived;
    }
    if (blob && !ms_valid_hash(blob)) blob = NULL;
    if (!ms_grow((void **)&x->files, &x->cfiles, x->nfiles + 1, sizeof(ms_file))) { ms_fail(b, "out of memory"); return; }
    f = &x->files[x->nfiles];
    memset(f, 0, sizeof *f);
    f->bundle = (int)(b - x->bundles);
    f->request = request;
    f->blob = blob;
    f->stock = stock;
    f->size = (uint64_t)size;
    f->need_first = need_first;
    f->need_count = need_count;
    /* shadows[] is sorted (ms_parse_bundle). */
    f->is_shadow = nshadows > 0 && bsearch(&request, shadows, (size_t)nshadows, sizeof(char *), ms_str_cmp) != NULL;
    f->is_restore = stock && stock[0] && sh_mb_stock_restore(request, stock);
    if (!strncmp(request, "generated/decls/", 16)) {
        const char *rest = request + 16, *slash = strchr(rest, '/');
        size_t n = strlen(rest);
        if (!slash || slash == rest || n < 6 || strcmp(rest + n - 5, ".decl") || (size_t)(slash - rest) + 6 > n) {
            ms_fail(b, "'%s' is not generated/decls/<type>/<name>.decl", request);
            return;
        }
        {
            char *derived_name = ms_strndup(x, slash + 1, n - (size_t)(slash - rest) - 6);
            if (!derived_name) { ms_fail(b, "out of memory"); return; }
            if (pname) {
                ms_lower(pname);
                if (strcmp(pname, derived_name)) {
                    ms_fail(b, "'%s' provides '%s', not the name its path gives", request, pname);
                    return;
                }
            }
            pname = derived_name;
        }
        if (!ptype) ptype = ms_strndup(x, rest, (size_t)(slash - rest));
        else if (_strnicmp(ptype, rest, (size_t)(slash - rest)) || ptype[slash - rest]) {
            ms_fail(b, "'%s' provides type '%s', not its folder", request, ptype);
            return;
        }
        f->is_decl = 1;
        f->reg = rest;
        f->rank = ms_rank(ptype);
        f->is_sedef = f->rank == 3;
    } else {
        if (!ptype) ptype = ms_strdup(x, "file");
        if (pname) ms_lower(pname);
        else pname = (char *)request;
    }
    if (!ptype || !pname) { ms_fail(b, "out of memory"); return; }
    f->ptype = ptype;
    f->pname = pname;
    /* Namespace: new names live under mods/<id>/. Converted legacy packages
     * keep their first-generation names, so they are exempt. Raw files (and
     * skins decls, which are named by their mesh path) may spell the id the
     * way the engine's name sanitiser derives it ('-' becomes '_'), as the
     * bundle builder accepts. */
    if (!b->legacy && !f->is_shadow && !f->is_restore) {
        char ns[96], ns_s[96], sid[80];
        int inside;
        _snprintf_s(ns, sizeof ns, _TRUNCATE, "mods/%s/", b->id);
        ms_sanitize(b->id, sid, sizeof sid);
        _snprintf_s(ns_s, sizeof ns_s, _TRUNCATE, "mods/%s/", sid);
        if (f->is_decl && _stricmp(ptype, "skins")) inside = !strncmp(f->pname, ns, strlen(ns));
        else if (!_stricmp(ptype, "soundBank")) {
            /* A bank lives in the one folder Wwise lists, so its namespace is
             * its stem: <id with '.' and '-' as '_'>_<rest>, as the builder
             * requires (sh_mb_bank_stem). */
            char stem[96], bid[80], *q;
            size_t k;
            _snprintf_s(bid, sizeof bid, _TRUNCATE, "%s_", b->id);
            for (q = bid; *q; ++q) if (*q == '.' || *q == '-') *q = '_';
            k = strlen(bid);
            inside = sh_mb_bank_stem(request, stem, sizeof stem) && !_strnicmp(stem, bid, k);
        } else {
            const char *subject = f->is_decl ? f->pname : request;
            char mid[100], mid_s[100];
            _snprintf_s(mid, sizeof mid, _TRUNCATE, "/%s", ns);
            _snprintf_s(mid_s, sizeof mid_s, _TRUNCATE, "/%s", ns_s);
            inside = !strncmp(subject, ns, strlen(ns)) || strstr(subject, mid) != NULL ||
                     !strncmp(subject, ns_s, strlen(ns_s)) || strstr(subject, mid_s) != NULL;
        }
        if (!inside) {
            ms_fail(b, "'%s' is outside mods/%s/ and not listed in shadows", request, b->id);
            return;
        }
    }
    {
        char disk[MS_PATH_MAX];
        char *p;
        _snprintf_s(disk, sizeof disk, _TRUNCATE, "%s\\%s", b->dir, path);
        for (p = disk; *p; ++p) if (*p == '/') *p = '\\';
        f->disk = ms_strdup(x, disk);
        if (!f->disk) { ms_fail(b, "out of memory"); return; }
    }
    x->nfiles++;
}

/* ------------------------------------------------------------------ requirements */

/* The only engine settings a bundle may require. Bit i of a requirement mask
 * is MS_GATES[i] = "0". Widening this is a product decision, not a format one. */
static const char *const MS_GATES[SH_MODSERVE_REQ_COUNT] = { "g_useImageBlackList", "g_useResourceBlackList" };

const char *sh_modserve_requirement_cvar(int index)
{
    return index >= 0 && index < SH_MODSERVE_REQ_COUNT ? MS_GATES[index] : NULL;
}

/* requirements: {"cvars": {<gate>: "0" | 0, ...}}. Anything else is a first
 * cause: the bundle needs a setting this build will not change for it. */
static void ms_parse_requirements(ms_bundle *b, const char *v, const char *ve)
{
    ms_json o = { v, ve };
    char key[64];
    const char *cv, *cve;
    if (!ms_jenter(&o, '{')) { ms_fail(b, "requirements must be an object"); return; }
    while (!b->cause[0] && ms_jnext(&o, '}')) {
        ms_json c;
        if (!ms_jmember(&o, key, sizeof key, &cv, &cve)) { ms_fail(b, "requirements is not a valid object"); return; }
        if (strcmp(key, "cvars")) { ms_fail(b, "requirement '%.60s' is refused (only cvars are supported)", key); return; }
        c.p = cv;
        c.e = cve;
        if (!ms_jenter(&c, '{')) { ms_fail(b, "requirements.cvars must be an object"); return; }
        while (ms_jnext(&c, '}')) {
            const char *xv, *xve;
            int g, zero;
            size_t n;
            if (!ms_jmember(&c, key, sizeof key, &xv, &xve)) { ms_fail(b, "requirements.cvars is not a valid object"); return; }
            n = (size_t)(xve - xv);
            zero = (n == 3 && !memcmp(xv, "\"0\"", 3)) || (n == 1 && *xv == '0');
            for (g = 0; g < SH_MODSERVE_REQ_COUNT; ++g)
                if (!strcmp(key, MS_GATES[g])) break;
            if (g == SH_MODSERVE_REQ_COUNT || !zero) {
                ms_fail(b, "requirement %.60s=%.*s is refused (this build applies only g_useImageBlackList=0 and "
                        "g_useResourceBlackList=0)", key, (int)(n > 24 ? 24 : n), xv);
                return;
            }
            b->req |= 1u << g;
        }
    }
}

/* A growable text for the "extra" object. */
typedef struct ms_text { char *p; size_t n, cap; int oom; } ms_text;

static void ms_text_put(ms_text *t, const char *s, size_t n)
{
    if (t->oom) return;
    if (t->n + n + 1 > t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 1024;
        char *grown;
        while (cap < t->n + n + 1) cap *= 2;
        grown = (char *)realloc(t->p, cap);
        if (!grown) { t->oom = 1; return; }
        t->p = grown;
        t->cap = cap;
    }
    memcpy(t->p + t->n, s, n);
    t->n += n;
    t->p[t->n] = 0;
}

/* Trim ASCII spaces in place (arena text). */
static char *ms_trim(char *s)
{
    char *e;
    if (!s) return s;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    return s;
}

/* A "legacy" object whose "source" is not "overrides" (the retired package folder): a package a map delivered. */
static int ms_delivered(const char *v, const char *ve)
{
    ms_json o = { v, ve };
    char key[64];
    const char *sv, *se;
    if (!ms_jenter(&o, '{')) return 0;
    while (ms_jnext(&o, '}')) {
        if (!ms_jmember(&o, key, sizeof key, &sv, &se)) break;
        if (!strcmp(key, "source")) return !(se - sv == 11 && !memcmp(sv, "\"overrides\"", 11));
    }
    return 1;
}

/* Parse <dir>\mod.json into the index; leaves b->cause set on failure. The
 * author block gives the identity, the requirements and "extra" (every key the
 * product does not read itself: strings, hud, description, ...); the product
 * block ("snapmap-plus") gives the bundle hash, the files, the shadows and the
 * verification state. Sizes only: hashes were verified when it was built. */
static void ms_parse_bundle(ms_index *x, ms_bundle *b)
{
    char path[MAX_PATH], key[64];
    unsigned char *text;
    size_t len = 0;
    sh_json_kind kind;
    sh_json_error err = { 0, NULL };
    ms_json c;
    const char *v, *ve, *files_v = NULL, *files_e = NULL, *stock_v = NULL, *stock_e = NULL, *req_v = NULL, *req_e = NULL;
    const char *block_v = NULL, *block_e = NULL, *legacy_v = NULL, *legacy_e = NULL, *shadows_v = NULL, *shadows_e = NULL;
    const char *author_legacy_v = NULL, *author_legacy_e = NULL;
    /* A converted first-generation package lists every name it carries, so
     * shadows run up to the file cap. */
    char **shadows = NULL, *id = NULL, *version = NULL, *name = NULL, *state_cause = NULL;
    int nshadows = 0, cshadows = 0, i, validated = 1, members = 0;
    double format = -1, built = 0;
    ms_text extra = { NULL, 0, 0, 0 };
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", b->dir);
    text = ms_read_whole(path, SH_MODSERVE_MAX_JSON, &len);
    if (!text) {
        char pkg[MAX_PATH];
        _snprintf_s(pkg, sizeof pkg, _TRUNCATE, "%s\\package.json", b->dir);
        if (ms_file_size(path) >= 0) ms_fail(b, "mod.json unreadable or over %u MiB", SH_MODSERVE_MAX_JSON >> 20);
        else if (ms_file_size(pkg) >= 0) ms_fail(b, "not built yet (package.json only; a rescan of the Mods tab builds it)");
        else ms_fail(b, "no mod.json");
        return;
    }
    if (!sh_json_validate_ex((const char *)text, len, MS_JSON_DEPTH, &kind, &err) || kind != SH_JSON_OBJECT) {
        ms_fail(b, "mod.json is not a valid JSON object (%s at byte %zu)", err.reason ? err.reason : "not an object",
                err.offset);
        HeapFree(GetProcessHeap(), 0, text);
        return;
    }
    c.p = (const char *)text;
    c.e = c.p + len;
    if (len >= 3 && text[0] == 0xEF && text[1] == 0xBB && text[2] == 0xBF) c.p += 3;
    ms_jenter(&c, '{');
    while (ms_jnext(&c, '}')) {
        const char *ks = c.p;
        if (!ms_jmember(&c, key, sizeof key, &v, &ve)) break;
        if (!strcmp(key, "id")) id = ms_jstr(x, v, ve, 96);
        else if (!strcmp(key, "version")) version = ms_jstr(x, v, ve, 160);
        else if (!strcmp(key, "name")) name = ms_jstr(x, v, ve, 400);
        else if (!strcmp(key, "requirements")) { req_v = v; req_e = ve; }
        else if (!strcmp(key, SH_MB_BLOCK_KEY)) { block_v = v; block_e = ve; }
        else if (!strcmp(key, "format") || !strcmp(key, "author") || !strcmp(key, "homepage") ||
                 !strcmp(key, "shadows") || !strcmp(key, "stock")) continue;
        else {
            /* Everything else is the author's, kept for the readers of
             * "extra" (strids.c, modhud.c); a "legacy" object grandfathers. */
            if (!strcmp(key, "legacy") && v < ve && *v == '{') { author_legacy_v = v; author_legacy_e = ve; }
            ms_text_put(&extra, members++ ? "," : "{", 1);
            ms_text_put(&extra, ks, (size_t)(ve - ks));
        }
    }
    if (block_v) {
        ms_json o = { block_v, block_e };
        if (ms_jenter(&o, '{')) while (ms_jnext(&o, '}')) {
            if (!ms_jmember(&o, key, sizeof key, &v, &ve)) break;
            if (!strcmp(key, "format")) { if (!ms_jnum(v, ve, &format)) format = -1; }
            else if (!strcmp(key, "bundle")) b->hash = ms_jstr(x, v, ve, 64);
            else if (!strcmp(key, "files")) { files_v = v; files_e = ve; }
            else if (!strcmp(key, "stock")) { stock_v = v; stock_e = ve; }
            else if (!strcmp(key, "shadows")) { shadows_v = v; shadows_e = ve; }
            else if (!strcmp(key, "legacy") && v < ve && *v == '{') { legacy_v = v; legacy_e = ve; }
            else if (!strcmp(key, "validated")) validated = !(ve - v == 5 && !memcmp(v, "false", 5));
            else if (!strcmp(key, "cause")) state_cause = ms_jstr(x, v, ve, 160);
            else if (!strcmp(key, "built")) { if (!ms_jnum(v, ve, &built)) built = 0; }
        }
    }
    if (author_legacy_v) {
        b->grandfathered = 1;
        b->legacy = ms_delivered(author_legacy_v, author_legacy_e);
    } else if (legacy_v) {
        /* A converted package keeps its legacy object in the product block;
         * readers of "extra" see it where the manifest has it. */
        b->grandfathered = 1;
        b->legacy = ms_delivered(legacy_v, legacy_e);
        ms_text_put(&extra, members++ ? "," : "{", 1);
        ms_text_put(&extra, "\"legacy\":", 9);
        ms_text_put(&extra, legacy_v, (size_t)(legacy_e - legacy_v));
    }
    if (members) {
        ms_text_put(&extra, "}", 1);
        if (!extra.oom) b->extra = ms_strndup(x, extra.p, extra.n);
    }
    free(extra.p);
    if (shadows_v) {
        ms_json a = { shadows_v, shadows_e };
        if (ms_jenter(&a, '[')) while (ms_jnext(&a, ']')) {
            const char *sv = a.p, *se = ms_jskip(a.p, a.e);
            char *s = ms_jstr(x, sv, se, 511);
            a.p = se;
            if (!s) continue;
            if (nshadows == (int)SH_MODSERVE_MAX_FILES) { ms_fail(b, "more than %u shadows", SH_MODSERVE_MAX_FILES); break; }
            if (!ms_grow((void **)&shadows, &cshadows, nshadows + 1, sizeof(char *))) { ms_fail(b, "out of memory"); break; }
            ms_lower(s);
            shadows[nshadows++] = s;
        }
    }
    id = ms_trim(id);
    if (id) ms_lower(id);
    b->id = id;
    b->version = ms_trim(version);
    b->name = ms_trim(name);
    b->installed_at = built;
    if (!block_v) ms_fail(b, "not built yet (mod.json has no \"" SH_MB_BLOCK_KEY "\" block; a rescan of the Mods tab builds it)");
    else if (format != 1) ms_fail(b, "mod.json product block format %g is not supported (this build reads format 1)", format);
    if (!b->id || !ms_valid_id(b->id)) ms_fail(b, "bundle id '%s' is not valid", b->id ? b->id : "");
    if (!b->version) b->version = "";
    if (!b->hash || !ms_valid_hash((char *)b->hash)) ms_fail(b, "mod.json has no bundle hash");
    if (!files_v && !stock_v) ms_fail(b, "mod.json lists no files");
    if (req_v && !b->cause[0]) ms_parse_requirements(b, req_v, req_e);
    if (nshadows > 1) qsort(shadows, (size_t)nshadows, sizeof(char *), ms_str_cmp);
    if (!b->cause[0]) {
        int pass;
        b->first_file = x->nfiles;
        for (pass = 0; pass < 2 && !b->cause[0]; ++pass) {
            ms_json a;
            a.p = pass ? stock_v : files_v;
            a.e = pass ? stock_e : files_e;
            if (!a.p) continue;
            if (ms_jenter(&a, '[')) while (!b->cause[0] && ms_jnext(&a, ']')) {
                ms_json one = { a.p, ms_jskip(a.p, a.e) };
                a.p = one.e;
                if (x->nfiles - b->first_file >= (int)SH_MODSERVE_MAX_FILES) { ms_fail(b, "more than %u files", SH_MODSERVE_MAX_FILES); break; }
                ms_parse_file(x, b, &one, shadows, nshadows, pass);
            }
        }
        b->file_count = x->nfiles - b->first_file;
        for (i = b->first_file; i < b->first_file + b->file_count; ++i) b->total += x->files[i].size;   /* a pointer's is 0 */
    }
    free(shadows);
    HeapFree(GetProcessHeap(), 0, text);
    if (!b->cause[0] && !validated)
        ms_fail(b, "not validated: %s", state_cause && *state_cause ? state_cause : "(no cause recorded)");
    /* Sizes only; hashes were verified at the build and install. */
    for (i = b->first_file; !b->cause[0] && i < b->first_file + b->file_count; ++i) {
        ms_file *f = &x->files[i];
        long long got;
        if (f->stock) continue;   /* resolved against the installed game at install, read when served */
        got = ms_file_size(f->disk);
        if (got == (long long)f->size) continue;
        if (got < 0) ms_fail(b, "'%s' is missing", f->request);
        else ms_fail(b, "size mismatch: '%s' is %lld bytes, mod.json says %llu", f->request, got,
                     (unsigned long long)f->size);
    }
}

/* Every folder under the store root that holds mod.json (or a package.json
 * not built yet). The retired blobs\ and legacy\ folders, hidden folders and
 * install staging are not mods. */
static void ms_scan_folder(ms_index *x, const char *dir)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        ms_bundle *b;
        char sub[MAX_PATH], probe[MAX_PATH];
        int mark_files = x->nfiles, mark_needs = x->nneeds;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        if (fd.cFileName[0] == '.') continue;
        if (!_stricmp(fd.cFileName, "blobs") || !_stricmp(fd.cFileName, "legacy")) continue;
        if (strstr(fd.cFileName, ".staging-") || strstr(fd.cFileName, ".old-")) continue;
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        _snprintf_s(probe, sizeof probe, _TRUNCATE, "%s\\mod.json", sub);
        if (ms_file_size(probe) < 0) {
            _snprintf_s(probe, sizeof probe, _TRUNCATE, "%s\\package.json", sub);
            if (ms_file_size(probe) < 0) continue;   /* not a mod folder */
        }
        if (!ms_grow((void **)&x->bundles, &x->cbundles, x->nbundles + 1, sizeof(ms_bundle))) break;
        b = &x->bundles[x->nbundles++];
        memset(b, 0, sizeof *b);
        b->dir = ms_strdup(x, sub);
        b->folder = ms_strdup(x, fd.cFileName);
        b->first_file = x->nfiles;
        if (!b->dir || !b->folder) { ms_fail(b, "out of memory"); }
        else ms_parse_bundle(x, b);
        if (b->cause[0]) {
            /* Failure isolation: drop this bundle's rows, keep everything else. */
            x->nfiles = mark_files;
            x->nneeds = mark_needs;
            b->file_count = 0;
            ms_log("bundle '%s' skipped: %s", fd.cFileName, b->cause);
        } else {
            b->admitted = 1;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* The record's aliases: a delivered copy the store upgrade dropped because an
 * installed mod serves the same content answers by the kept bundle's hash. */
static void ms_load_aliases(ms_index *x, const char *mods_root)
{
    char *text = NULL, key[80];
    ms_json o;
    const char *v, *ve;
    int cap = 0;
    if (sh_mb_record_get(mods_root, "aliases", &text) != 1 || !text) { free(text); return; }
    o.p = text;
    o.e = text + strlen(text);
    if (ms_jenter(&o, '{')) while (ms_jnext(&o, '}')) {
        char *to;
        if (!ms_jmember(&o, key, sizeof key, &v, &ve)) break;
        to = ms_jstr(x, v, ve, 64);
        if (strlen(key) != 64 || !to || !ms_valid_hash(to)) continue;
        if (!ms_grow((void **)&x->aliases, &cap, x->naliases + 1, sizeof *x->aliases)) break;
        x->aliases[x->naliases].from = ms_strdup(x, key);
        x->aliases[x->naliases].to = to;
        if (x->aliases[x->naliases].from) x->naliases++;
    }
    free(text);
}

/* ------------------------------------------------------------------ conflicts and table */

typedef struct ms_key { const char *key; int file; int alias; } ms_key;

static int ms_key_cmp(const void *a, const void *b)
{
    const ms_key *x = (const ms_key *)a, *y = (const ms_key *)b;
    int c = strcmp(x->key, y->key);
    if (c) return c;
    if (x->alias != y->alias) return x->alias - y->alias;
    return x->file - y->file;
}

static int ms_newer(const ms_index *x, int a, int b)   /* bundle a newer than bundle b */
{
    const ms_bundle *ba = &x->bundles[a], *bb = &x->bundles[b];
    if (ba->installed_at != bb->installed_at) return ba->installed_at > bb->installed_at;
    return strcmp(ba->folder, bb->folder) > 0;
}

/* ------------------------------------------------------------------ decl list composition
 *
 * Several mods often shadow one stock list decl to append their own entries:
 * every boss pack adds its demon to the encounter conductor's aiTypeList and
 * its encounter to the whitelist inspectors' validEncounters. Serving one
 * pack's copy drops the others' entries; serving none drops them all. When
 * every provider's text parses in the strict assignment grammar
 *
 *   text  := '{' stmt* '}'
 *   stmt  := key '=' value [';'] | key ['='] '{' stmt* '}' [';']
 *   key   := word | "string"     value := word | "string" | ( group )
 *
 * and the texts agree everywhere except inside lists (a block holding
 * `num = N;` and only `item[i]` members), the served text is the winner's
 * with every other provider's list items appended that the winner lacks
 * (matched by value, or by a block item's first scalar member such as
 * aiType), renumbered, and `num` set to the count. Any other difference, a
 * provider that lists an existing item with different content, or a text
 * outside the grammar leaves the winner's bytes as they are. Removal cannot
 * be expressed (no stock text is read here), so composition is a union. */

#define MC_DEPTH 32

typedef struct mc_node {
    const char *key; size_t key_n;
    const char *val; size_t val_n;        /* scalar value text, quotes included; NULL for a block */
    struct mc_node **kids; int nkids, ckids;
    int block, eq;
} mc_node;

typedef struct mc_arena { struct mc_arena *next; size_t used; char data[16384]; } mc_arena;
typedef struct mc_ctx { mc_arena *arena; int failed; } mc_ctx;

static void *mc_alloc(mc_ctx *c, size_t n)
{
    mc_arena *a = c->arena;
    n = (n + 7u) & ~(size_t)7u;
    if (n > sizeof a->data) { c->failed = 1; return NULL; }
    if (!a || sizeof a->data - a->used < n) {
        mc_arena *k = (mc_arena *)malloc(sizeof *k);
        if (!k) { c->failed = 1; return NULL; }
        k->next = a; k->used = 0;
        c->arena = a = k;
    }
    a->used += n;
    return a->data + a->used - n;
}

static void mc_free(mc_ctx *c)
{
    mc_arena *a, *n;
    for (a = c->arena; a; a = n) { n = a->next; free(a); }
    c->arena = NULL;
}

static int mc_add_kid(mc_ctx *c, mc_node *parent, mc_node *kid)
{
    if (parent->nkids == parent->ckids) {
        int cap = parent->ckids ? parent->ckids * 2 : 8;
        mc_node **k = (mc_node **)mc_alloc(c, sizeof(mc_node *) * (size_t)cap);
        if (!k) return 0;
        if (parent->nkids) memcpy(k, parent->kids, sizeof(mc_node *) * (size_t)parent->nkids);
        parent->kids = k;
        parent->ckids = cap;
    }
    parent->kids[parent->nkids++] = kid;
    return 1;
}

enum { MC_END, MC_WORD, MC_STR, MC_LB, MC_RB, MC_EQ, MC_SEMI, MC_GROUP, MC_BAD };
typedef struct mc_tok { int kind; const char *p; size_t n; } mc_tok;
typedef struct mc_lex { const char *t; size_t len, pos; } mc_lex;

static mc_tok mc_next(mc_lex *l)
{
    mc_tok k = { MC_END, NULL, 0 };
    const char *t = l->t;
    size_t i = l->pos, len = l->len;
    for (;;) {
        while (i < len && (t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) i++;
        if (i + 1 < len && t[i] == '/' && t[i + 1] == '/') { while (i < len && t[i] != '\n') i++; continue; }
        if (i + 1 < len && t[i] == '/' && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < len && !(t[i] == '*' && t[i + 1] == '/')) i++;
            i = i + 2 <= len ? i + 2 : len;
            continue;
        }
        break;
    }
    if (i >= len) { l->pos = len; return k; }
    k.p = t + i;
    switch (t[i]) {
    case '"':
        for (++i; i < len && t[i] != '"' && t[i] != '\n'; ++i) {}
        if (i >= len || t[i] != '"') { k.kind = MC_BAD; break; }
        ++i;
        k.kind = MC_STR;
        break;
    case '{': k.kind = MC_LB; ++i; break;
    case '}': k.kind = MC_RB; ++i; break;
    case '=': k.kind = MC_EQ; ++i; break;
    case ';': k.kind = MC_SEMI; ++i; break;
    case '(': {
        int depth = 0;
        for (; i < len; ++i) {
            if (t[i] == '(') depth++;
            else if (t[i] == ')' && --depth == 0) { ++i; break; }
            else if (t[i] == '{' || t[i] == '}' || t[i] == ';') { depth = -1; break; }
        }
        k.kind = depth == 0 ? MC_GROUP : MC_BAD;
        break;
    }
    default: {
        size_t s = i;
        while (i < len && !strchr(" \t\r\n{}=;\"()", t[i])) i++;
        k.kind = i > s ? MC_WORD : MC_BAD;
        if (i == s) i++;
        break;
    }
    }
    k.n = (size_t)(t + i - k.p);
    l->pos = i;
    return k;
}

static mc_tok mc_peek(mc_lex *l)
{
    size_t at = l->pos;
    mc_tok k = mc_next(l);
    l->pos = at;
    return k;
}

/* Statements up to the closing brace (consumed). */
static int mc_parse_block(mc_ctx *c, mc_lex *l, mc_node *parent, int depth)
{
    if (depth > MC_DEPTH) return 0;
    for (;;) {
        mc_tok key = mc_next(l), t;
        mc_node *n;
        if (key.kind == MC_RB) return 1;
        if (key.kind != MC_WORD && key.kind != MC_STR) return 0;
        n = (mc_node *)mc_alloc(c, sizeof *n);
        if (!n) return 0;
        memset(n, 0, sizeof *n);
        n->key = key.p;
        n->key_n = key.n;
        t = mc_next(l);
        if (t.kind == MC_EQ) { n->eq = 1; t = mc_next(l); }
        if (t.kind == MC_LB) {
            n->block = 1;
            if (!mc_parse_block(c, l, n, depth + 1)) return 0;
        } else if (n->eq && (t.kind == MC_WORD || t.kind == MC_STR || t.kind == MC_GROUP)) {
            n->val = t.p;
            n->val_n = t.n;
        } else return 0;
        if (mc_peek(l).kind == MC_SEMI) mc_next(l);
        if (!mc_add_kid(c, parent, n)) return 0;
    }
}

static mc_node *mc_parse(mc_ctx *c, const char *text, size_t len)
{
    mc_lex l = { text, len, 0 };
    mc_node *root = (mc_node *)mc_alloc(c, sizeof *root);
    if (!root) return NULL;
    memset(root, 0, sizeof *root);
    root->block = 1;
    if (mc_next(&l).kind != MC_LB || !mc_parse_block(c, &l, root, 0) || mc_next(&l).kind != MC_END) return NULL;
    return root;
}

typedef struct mc_buf { char *p; size_t n, cap; int failed; } mc_buf;

static void mc_put(mc_buf *b, const char *s, size_t n)
{
    if (b->failed) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        char *g;
        while (cap < b->n + n + 1) cap *= 2;
        g = (char *)realloc(b->p, cap);
        if (!g) { b->failed = 1; return; }
        b->p = g;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static int mc_key_is(const mc_node *n, const char *word)
{
    size_t w = strlen(word);
    return n->key_n == w && !_strnicmp(n->key, word, w);
}

/* item[<digits>] */
static int mc_is_item(const mc_node *n)
{
    size_t i;
    if (n->key_n < 7 || _strnicmp(n->key, "item[", 5) || n->key[n->key_n - 1] != ']') return 0;
    for (i = 5; i + 1 < n->key_n; ++i) if (n->key[i] < '0' || n->key[i] > '9') return 0;
    return 1;
}

static int mc_is_list(const mc_node *n)
{
    int i, num = 0;
    if (!n->block) return 0;
    for (i = 0; i < n->nkids; ++i) {
        const mc_node *k = n->kids[i];
        if (mc_key_is(k, "num") && !k->block) { num++; continue; }
        if (!mc_is_item(k)) return 0;
    }
    return num == 1;
}

static void mc_indent(mc_buf *b, int depth) { int i; for (i = 0; i < depth; ++i) mc_put(b, "\t", 1); }

/* Serialise `n`'s members at `depth`; lists are renumbered. */
static void mc_write_members(mc_buf *b, const mc_node *n, int depth)
{
    int i, item = 0, list = mc_is_list(n);
    char tmp[48];
    if (list) {
        int count = 0;
        for (i = 0; i < n->nkids; ++i) if (!mc_key_is(n->kids[i], "num")) count++;
        mc_indent(b, depth);
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "num = %d;\n", count);
        mc_put(b, tmp, strlen(tmp));
    }
    for (i = 0; i < n->nkids; ++i) {
        const mc_node *k = n->kids[i];
        if (list && mc_key_is(k, "num")) continue;
        mc_indent(b, depth);
        if (list) {
            _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "item[%d]", item++);
            mc_put(b, tmp, strlen(tmp));
        } else mc_put(b, k->key, k->key_n);
        if (k->block) {
            mc_put(b, k->eq ? " = {\n" : " {\n", k->eq ? 5 : 3);
            mc_write_members(b, k, depth + 1);
            mc_indent(b, depth);
            mc_put(b, "}\n", 2);
        } else {
            mc_put(b, " = ", 3);
            mc_put(b, k->val, k->val_n);
            mc_put(b, ";\n", 2);
        }
    }
}

/* The canonical text of one node (for equality and identity). Malloc'd. */
static char *mc_text(const mc_node *n, size_t *len)
{
    mc_buf b = { NULL, 0, 0, 0 };
    if (n->block) { mc_put(&b, "{\n", 2); mc_write_members(&b, n, 1); mc_put(&b, "}", 1); }
    else mc_put(&b, n->val, n->val_n);
    if (b.failed) { free(b.p); return NULL; }
    if (len) *len = b.n;
    return b.p ? b.p : _strdup("");
}

static int mc_equal(const mc_node *a, const mc_node *b)
{
    char *x, *y;
    int eq;
    if (a->block != b->block) return 0;
    if (!a->block) return a->val_n == b->val_n && !memcmp(a->val, b->val, a->val_n);
    x = mc_text(a, NULL);
    y = mc_text(b, NULL);
    eq = x && y && !strcmp(x, y);
    free(x);
    free(y);
    return eq;
}

/* A list item's identity: a scalar's value, or a block's first scalar member. */
static char *mc_identity(const mc_node *n)
{
    int i;
    if (n->block)
        for (i = 0; i < n->nkids; ++i)
            if (!n->kids[i]->block) {
                const mc_node *k = n->kids[i];
                char *s = (char *)malloc(k->key_n + k->val_n + 2);
                if (!s) return NULL;
                memcpy(s, k->key, k->key_n);
                s[k->key_n] = '=';
                memcpy(s + k->key_n + 1, k->val, k->val_n);
                s[k->key_n + k->val_n + 1] = 0;
                for (char *p = s; p < s + k->key_n; ++p) *p = ms_fold(*p);
                return s;
            }
    return mc_text(n, NULL);
}

static int mc_count_key(const mc_node *n, const mc_node *like)
{
    int i, c = 0;
    for (i = 0; i < n->nkids; ++i)
        if (n->kids[i]->key_n == like->key_n && !_strnicmp(n->kids[i]->key, like->key, like->key_n)) c++;
    return c;
}

/* Merge `other` into `base`; 0 when they disagree outside lists. *added
 * counts the list items appended. */
static int mc_merge(mc_ctx *c, mc_node *base, const mc_node *other, int *added)
{
    int i, k;
    if (mc_is_list(base) && mc_is_list(other)) {
        for (i = 0; i < other->nkids; ++i) {
            const mc_node *it = other->kids[i];
            char *id;
            int found = -1;
            if (mc_key_is(it, "num")) continue;
            id = mc_identity(it);
            if (!id) return 0;
            for (k = 0; k < base->nkids && found < 0; ++k) {
                char *bid;
                if (mc_key_is(base->kids[k], "num")) continue;
                bid = mc_identity(base->kids[k]);
                if (!bid) { free(id); return 0; }
                if (!strcmp(id, bid)) found = k;
                free(bid);
            }
            free(id);
            if (found >= 0) {
                if (!mc_equal(base->kids[found], it)) return 0;   /* one entry, two meanings */
                continue;
            }
            if (!mc_add_kid(c, base, (mc_node *)it)) return 0;
            (*added)++;
        }
        return 1;
    }
    if (mc_is_list(base) || mc_is_list(other)) return 0;
    if (mc_equal(base, other)) return 1;
    for (i = 0; i < other->nkids; ++i) {
        const mc_node *o = other->kids[i];
        mc_node *b = NULL;
        if (mc_count_key(other, o) != 1 || mc_count_key(base, o) > 1) return 0;
        for (k = 0; k < base->nkids; ++k)
            if (base->kids[k]->key_n == o->key_n && !_strnicmp(base->kids[k]->key, o->key, o->key_n)) { b = base->kids[k]; break; }
        if (!b) return 0;   /* a member only one text has: an edit this cannot place without the stock text */
        if (b->block && o->block) { if (!mc_merge(c, b, o, added)) return 0; }
        else if (!mc_equal(b, o)) return 0;
    }
    /* Members only the base has are another edit of the same kind. */
    for (k = 0; k < base->nkids; ++k) {
        int found = 0;
        for (i = 0; i < other->nkids && !found; ++i)
            found = base->kids[k]->key_n == other->kids[i]->key_n &&
                    !_strnicmp(base->kids[k]->key, other->kids[i]->key, other->kids[i]->key_n);
        if (!found) return 0;
    }
    return 1;
}

/* Compose texts[0] (the winner) with the others. Returns the composed text
 * (malloc'd, *len set) with *added list items appended, or NULL when the
 * texts cannot be composed. */
static char *ms_compose(const unsigned char *const *texts, const size_t *lens, int n, size_t *len, int *added)
{
    mc_ctx c = { NULL, 0 };
    mc_node *base;
    mc_buf out = { NULL, 0, 0, 0 };
    int i;
    *added = 0;
    *len = 0;
    base = mc_parse(&c, (const char *)texts[0], lens[0]);
    for (i = 1; base && i < n; ++i) {
        mc_node *o = mc_parse(&c, (const char *)texts[i], lens[i]);
        if (!o || !mc_merge(&c, base, o, added)) base = NULL;
    }
    if (base && !c.failed) {
        mc_put(&out, "{\n", 2);
        mc_write_members(&out, base, 1);
        mc_put(&out, "}\n", 2);
    }
    mc_free(&c);
    if (!base || c.failed || out.failed) { free(out.p); return NULL; }
    *len = out.n;
    return out.p;
}

#ifdef SH_MODSERVE_TESTING
char *sh_modserve_test_compose(const char *const *texts, int n, int *added)
{
    const unsigned char *t[8];
    size_t l[8], len;
    int i;
    if (n < 1 || n > 8) return NULL;
    for (i = 0; i < n; ++i) { t[i] = (const unsigned char *)texts[i]; l[i] = strlen(texts[i]); }
    return ms_compose(t, l, n, &len, added);
}
#endif

/* ------------------------------------------------------------------ conflict winner
 *
 * When several bundles of different mods provide one path with different
 * bytes, one of them serves it (the path is never left to the engine: a pack's
 * other files are served and name it, and for campaign content the game has no
 * SnapMap copy to fall back to; lane MG2). In order:
 *   1. the provider with the fewest needs no admitted bundle provides (needs
 *      the game satisfies count alike for every provider, so the difference
 *      is what one copy names that nothing installed supplies);
 *   2. the bytes most providers carry;
 *   3. an authored or migrated mod over a copy a map delivered (legacy\);
 *   4. the most recently installed, then the folder name. */

typedef struct ms_avail { char **keys; int n; } ms_avail;   /* sorted "type\x01name" and request keys of admitted files */

static int ms_pstr_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static int ms_avail_build(ms_index *x, ms_avail *av)
{
    int i, n = 0;
    av->n = 0;
    av->keys = (char **)malloc(sizeof(char *) * (size_t)(x->nfiles * 2 + 1));
    if (!av->keys) return 0;
    for (i = 0; i < x->nfiles; ++i) {
        const ms_file *f = &x->files[i];
        char buf[MS_NAME_MAX];
        if (!x->bundles[f->bundle].admitted) continue;
        _snprintf_s(buf, sizeof buf, _TRUNCATE, "%s\x01%s", f->ptype, f->pname);
        ms_lower(buf);
        av->keys[n] = ms_strdup(x, buf);
        if (av->keys[n]) n++;
        av->keys[n++] = (char *)f->request;
    }
    qsort(av->keys, (size_t)n, sizeof(char *), ms_pstr_cmp);
    av->n = n;
    return 1;
}

static int ms_avail_has(const ms_avail *av, const char *key)
{
    return av->n && bsearch(&key, av->keys, (size_t)av->n, sizeof(char *), ms_pstr_cmp) != NULL;
}

static int ms_unresolved(const ms_index *x, const ms_avail *av, const ms_file *f)
{
    int k, missing = 0;
    for (k = 0; k < f->need_count; ++k) {
        const ms_need *nd = &x->needs[f->need_first + k];
        char buf[MS_NAME_MAX], cand[2][SH_MB_REQUEST_MAX + 1];
        int c, nc, ok;
        _snprintf_s(buf, sizeof buf, _TRUNCATE, "%s\x01%s", nd->type, nd->name);
        ms_lower(buf);
        ok = ms_avail_has(av, buf);
        nc = ok ? 0 : sh_mb_need_requests(nd->type, nd->name, cand, 2);
        for (c = 0; c < nc && !ok; ++c) ok = ms_avail_has(av, cand[c]);
        if (!ok) missing++;
    }
    return missing;
}

/* Index of the winning key in keys[i..j), and the reason. */
static int ms_pick(const ms_index *x, const ms_avail *av, const ms_key *keys, int i, int j, const char **reason)
{
    int k, best = i, unres[64], share[64], m = j - i > 64 ? 64 : j - i, tie_un = 1, tie_sh = 1, tie_leg = 1;
    for (k = 0; k < m; ++k) {
        const ms_file *f = &x->files[keys[i + k].file];
        int q;
        unres[k] = ms_unresolved(x, av, f);
        share[k] = 0;
        for (q = 0; q < m; ++q) {
            const ms_file *g = &x->files[keys[i + q].file];
            if (f->blob && g->blob && !strcmp(f->blob, g->blob)) share[k]++;
        }
    }
    for (k = 1; k < m; ++k) {
        const ms_file *f = &x->files[keys[i + k].file], *b = &x->files[keys[best].file];
        int fl = x->bundles[f->bundle].legacy, bl = x->bundles[b->bundle].legacy, kb = best - i;
        if (unres[k] != unres[kb]) { if (unres[k] < unres[kb]) best = i + k; continue; }
        if (share[k] != share[kb]) { if (share[k] > share[kb]) best = i + k; continue; }
        if (fl != bl) { if (!fl) best = i + k; continue; }
        if (ms_newer(x, f->bundle, b->bundle)) best = i + k;
    }
    for (k = 0; k < m; ++k) {
        const ms_file *f = &x->files[keys[i + k].file], *b = &x->files[keys[best].file];
        if (unres[k] != unres[best - i]) tie_un = 0;
        if (share[k] != share[best - i]) tie_sh = 0;
        if (x->bundles[f->bundle].legacy != x->bundles[b->bundle].legacy) tie_leg = 0;
    }
    if (!tie_un) *reason = "its needs resolve against the installed bundles where the others' do not";
    else if (!tie_sh) *reason = "most providers carry these bytes";
    else if (!tie_leg) *reason = "an installed mod is preferred over a copy a map delivered";
    else *reason = "installed most recently";
    return best;
}

static int ms_conflict_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, ((const ms_conflict *)b)->request);
}

/* Decide which files serve their request path. Returns 0 on allocation failure. */
static int ms_resolve(ms_index *x)
{
    ms_key *keys;
    int i, n = 0, logged = 0, avail_built = 0;
    int *superseded_by;
    ms_avail avail = { NULL, 0 };
    x->conflicts = x->composed = x->nconflict_list = 0;
    keys = (ms_key *)malloc(sizeof(ms_key) * (size_t)(x->nfiles ? x->nfiles : 1));
    superseded_by = (int *)malloc(sizeof(int) * (size_t)(x->nbundles ? x->nbundles : 1));
    if (!keys || !superseded_by) { free(keys); free(superseded_by); return 0; }
    for (i = 0; i < x->nbundles; ++i) superseded_by[i] = -1;
    for (i = 0; i < x->nfiles; ++i) {
        x->files[i].served = 0;
        x->files[i].body = NULL;
        x->files[i].body_len = 0;
        if (x->bundles[x->files[i].bundle].admitted) { keys[n].key = x->files[i].request; keys[n].file = i; keys[n].alias = 0; n++; }
    }
    qsort(keys, (size_t)n, sizeof *keys, ms_key_cmp);
    for (i = 0; i < n;) {
        int j = i + 1, k, winner = keys[i].file, same_blob = 1, same_id = 1, any_shadow = 0, dup_in_bundle = 0;
        while (j < n && !strcmp(keys[j].key, keys[i].key)) ++j;
        for (k = i; k < j; ++k) {
            const ms_file *f = &x->files[keys[k].file], *w = &x->files[keys[i].file];
            any_shadow |= f->is_shadow;
            if (!f->blob || !w->blob || strcmp(f->blob, w->blob)) same_blob = 0;
            if (strcmp(x->bundles[f->bundle].id, x->bundles[w->bundle].id)) same_id = 0;
            if (k > i && f->bundle == x->files[keys[k - 1].file].bundle) dup_in_bundle = 1;
        }
        if (dup_in_bundle) {
            /* Not a conflict between bundles: the manifest itself is wrong. */
            for (k = i + 1; k < j; ++k)
                if (x->files[keys[k].file].bundle == x->files[keys[k - 1].file].bundle) {
                    ms_bundle *b = &x->bundles[x->files[keys[k].file].bundle];
                    ms_fail(b, "'%s' is listed twice", keys[k].key);
                    b->admitted = 0;
                    ms_log("bundle '%s' skipped: %s", b->folder, b->cause);
                }
            free(keys); free(superseded_by);
            if (avail_built) free(avail.keys);
            return ms_resolve(x);   /* rare; restart without that bundle */
        }
        if (j - i == 1 || same_blob) {
            x->files[winner].served = 1;
        } else if (same_id) {
            /* Two installed versions of one mod: the most recently installed serves. */
            for (k = i + 1; k < j; ++k)
                if (ms_newer(x, x->files[keys[k].file].bundle, x->files[winner].bundle)) winner = keys[k].file;
            x->files[winner].served = 1;
            for (k = i; k < j; ++k)
                if (keys[k].file != winner) superseded_by[x->files[keys[k].file].bundle] = x->files[winner].bundle;
        } else {
            char who[1024], how[200];
            size_t at = 0;
            const char *reason = NULL;
            int pick, composed = 0, added = 0;
            ms_conflict *c;
            x->conflicts++;
            if (!avail_built) { if (!ms_avail_build(x, &avail)) { free(keys); free(superseded_by); return 0; } avail_built = 1; }
            pick = ms_pick(x, &avail, keys, i, j, &reason);
            winner = keys[pick].file;
            who[0] = 0;
            for (k = i; k < j && at + 2 < sizeof who; ++k) {
                const ms_bundle *b = &x->bundles[x->files[keys[k].file].bundle];
                int w = _snprintf_s(who + at, sizeof who - at, _TRUNCATE, "%s%s@%s", k > i ? ", " : "", b->id, b->version);
                if (w < 0) break;
                at += (size_t)w;
            }
            /* Decls whose providers only add list entries are composed. */
            {
                const unsigned char *texts[64];
                size_t lens[64];
                unsigned char *owned[64];
                int nt = 0, ok = 1, q;
                memset(owned, 0, sizeof owned);
                for (k = -1; k < j - i && ok && nt < 64; ++k) {
                    int file = k < 0 ? winner : keys[i + k].file, dup = 0;
                    const ms_file *f = &x->files[file];
                    if (k >= 0 && file == winner) continue;
                    if (!f->is_decl || f->size > SH_MODSERVE_CACHE_FILE) { ok = 0; break; }
                    /* Identical bytes add nothing. */
                    for (q = i; q < i + k && !dup; ++q) {
                        const ms_file *g = &x->files[keys[q].file];
                        if (g->blob && f->blob && !strcmp(g->blob, f->blob)) dup = 1;
                    }
                    if (k >= 0 && f->blob && x->files[winner].blob && !strcmp(f->blob, x->files[winner].blob)) dup = 1;
                    if (dup) continue;
                    /* A stock pointer's text is the installed decl's. */
                    owned[nt] = ms_file_raw(f, &lens[nt]);
                    if (!owned[nt] || lens[nt] > SH_MODSERVE_CACHE_FILE) { ok = 0; break; }
                    texts[nt] = owned[nt];
                    nt++;
                }
                if (ok && nt > 1) {
                    size_t len = 0;
                    char *text = ms_compose(texts, lens, nt, &len, &added);
                    if (text) {
                        unsigned char *body = (unsigned char *)ms_alloc(x, len ? len : 1);
                        if (body) {
                            memcpy(body, text, len);
                            x->files[winner].body = body;
                            x->files[winner].body_len = len;
                            composed = 1;
                        }
                        free(text);
                    }
                }
                for (q = 0; q < nt; ++q) if (owned[q]) HeapFree(GetProcessHeap(), 0, owned[q]);
            }
            x->files[winner].served = 1;
            if (composed) {
                x->composed++;
                _snprintf_s(how, sizeof how, _TRUNCATE, "composed: %d list entr%s of the other providers added to %s@%s's text",
                            added, added == 1 ? "y" : "ies", x->bundles[x->files[winner].bundle].id,
                            x->bundles[x->files[winner].bundle].version);
            } else _snprintf_s(how, sizeof how, _TRUNCATE, "%s", reason);
            if (ms_grow((void **)&x->conflict_list, &x->cconflict_list, x->nconflict_list + 1, sizeof(ms_conflict))) {
                c = &x->conflict_list[x->nconflict_list++];
                c->request = x->files[winner].request;
                c->providers = ms_strdup(x, who);
                c->reason = ms_strdup(x, how);
                c->winner = winner;
                c->count = j - i;
                c->composed = composed;
            }
            if (logged++ < MS_CONFLICT_LOGS) {
                if (composed)
                    ms_log("conflict: %s '%s' is provided by %s; serving %s@%s's text with the others' list entries "
                           "(%d added)", any_shadow ? "shadow" : "path", keys[i].key, who,
                           x->bundles[x->files[winner].bundle].id, x->bundles[x->files[winner].bundle].version, added);
                else
                    ms_log("conflict: %s '%s' is provided by %s; serving %s@%s (%s)", any_shadow ? "shadow" : "path",
                           keys[i].key, who, x->bundles[x->files[winner].bundle].id,
                           x->bundles[x->files[winner].bundle].version, reason);
            }
        }
        i = j;
    }
    if (avail_built) free(avail.keys);
    if (logged > MS_CONFLICT_LOGS) ms_log("%d more conflicts not listed", logged - MS_CONFLICT_LOGS);
    for (i = 0; i < x->nbundles; ++i)
        if (superseded_by[i] >= 0)
            ms_log("bundle %s@%s: superseded by %s@%s (installed later) for the paths both provide",
                   x->bundles[i].id, x->bundles[i].version, x->bundles[superseded_by[i]].id,
                   x->bundles[superseded_by[i]].version);
    free(keys);
    free(superseded_by);
    return 1;
}

static uint32_t ms_pow2(uint32_t n)
{
    uint32_t c = 16;
    while (c < n * 2u) c <<= 1;
    return c;
}

static void ms_insert(ms_slot *slots, uint32_t mask, const char *key, int file)
{
    uint64_t h = ms_hash_folded(key, NULL);
    uint32_t i = (uint32_t)h & mask;
    while (slots[i].key) i = (i + 1) & mask;
    slots[i].h = h;
    slots[i].key = key;
    slots[i].file = file;
}

static int ms_find(const ms_slot *slots, uint32_t mask, const char *name)
{
    size_t len;
    uint64_t h;
    uint32_t i;
    if (!slots) return -1;
    h = ms_hash_folded(name, &len);
    if (len >= MS_NAME_MAX) return -1;
    for (i = (uint32_t)h & mask; slots[i].key; i = (i + 1) & mask)
        if (slots[i].h == h && ms_folded_eq(slots[i].key, name)) return slots[i].file;
    return -1;
}

/* Request table (primary keys plus unambiguous aliases) and provides table. */
static int ms_build_tables(ms_index *x)
{
    ms_key *keys;
    int i, n = 0, cap, dropped = 0;
    uint32_t nslots;
    cap = x->nfiles * 3 + 1;
    keys = (ms_key *)malloc(sizeof(ms_key) * (size_t)cap);
    if (!keys) return 0;
    for (i = 0; i < x->nfiles; ++i) {
        ms_file *f = &x->files[i];
        char buf[MS_NAME_MAX];
        if (!f->served) continue;
        x->served_files++;
        if (f->is_shadow) x->shadows++;
        keys[n].key = f->request; keys[n].file = i; keys[n].alias = 0; n++;
        if (f->is_decl) {
            char *alias;
            _snprintf_s(buf, sizeof buf, _TRUNCATE, "decltree/%s", f->reg);
            alias = ms_strdup(x, buf);
            if (alias) { keys[n].key = alias; keys[n].file = i; keys[n].alias = 1; n++; }
        }
        if (ms_sanitize(f->request, buf, sizeof buf)) {
            char *alias = ms_strdup(x, buf);
            if (alias) { keys[n].key = alias; keys[n].file = i; keys[n].alias = 1; n++; }
        }
    }
    qsort(keys, (size_t)n, sizeof *keys, ms_key_cmp);
    /* A primary key always wins its group (keys sort primaries first); an
     * alias survives only when every key in its group names the same file. */
    {
        int w = 0;
        for (i = 0; i < n;) {
            int j = i + 1, k, one_file = 1;
            while (j < n && !strcmp(keys[j].key, keys[i].key)) ++j;
            for (k = i + 1; k < j; ++k) if (keys[k].file != keys[i].file) one_file = 0;
            if (!keys[i].alias || one_file) keys[w++] = keys[i];
            if (!one_file) dropped += keys[i].alias ? j - i : j - i - 1;
            i = j;
        }
        n = w;
    }
    if (dropped) ms_log("%d alias spelling(s) dropped because two files share them", dropped);
    nslots = ms_pow2((uint32_t)(n ? n : 1));
    x->slots = (ms_slot *)calloc(nslots, sizeof(ms_slot));
    if (!x->slots) { free(keys); return 0; }
    x->mask = nslots - 1;
    for (i = 0; i < n; ++i) ms_insert(x->slots, x->mask, keys[i].key, keys[i].file);
    x->nkeys = n;
    free(keys);
    /* Provides: "<type>\x01<name>" folded -> file. */
    nslots = ms_pow2((uint32_t)(x->served_files ? x->served_files : 1));
    x->pslots = (ms_slot *)calloc(nslots, sizeof(ms_slot));
    if (!x->pslots) return 0;
    x->pmask = nslots - 1;
    for (i = 0; i < x->nfiles; ++i) {
        ms_file *f = &x->files[i];
        char buf[MS_NAME_MAX];
        char *key;
        if (!f->served) continue;
        _snprintf_s(buf, sizeof buf, _TRUNCATE, "%s\x01%s", f->ptype, f->pname);
        ms_lower(buf);
        if (ms_find(x->pslots, x->pmask, buf) >= 0) continue;
        key = ms_strdup(x, buf);
        if (key) ms_insert(x->pslots, x->pmask, key, i);
    }
    return 1;
}

static int ms_provider(const ms_index *x, const char *type, const char *name)
{
    char buf[MS_NAME_MAX];
    _snprintf_s(buf, sizeof buf, _TRUNCATE, "%s\x01%s", type, name);
    return ms_find(x->pslots, x->pmask, buf);
}

/* A need's served provider: the file whose provides (type, name) match, else
 * the file at one of the request paths the engine would ask for that name
 * (the bundle builder's own resolution: decl text spells the mod id with
 * '-', the engine derives binary names with '_', and image names carry a
 * source extension). Resolved once per index build. */
static int ms_need_provider(const ms_index *x, const char *type, const char *name)
{
    char cand[2][SH_MB_REQUEST_MAX + 1];
    int p = ms_provider(x, type, name), n, i;
    if (p >= 0) return p;
    n = sh_mb_need_requests(type, name, cand, 2);
    for (i = 0; i < n; ++i) {
        p = ms_find(x->slots, x->mask, cand[i]);
        if (p >= 0 && x->files[p].served) return p;
    }
    return -1;
}

static void ms_resolve_needs(ms_index *x)
{
    int i;
    for (i = 0; i < x->nneeds; ++i) x->needs[i].file = ms_need_provider(x, x->needs[i].type, x->needs[i].name);
}

/* ------------------------------------------------------------------ build and publish */

/* Two files of successive tables that serve the same bytes. */
static int ms_same_bytes(const ms_file *a, const ms_file *b)
{
    if (a->body || b->body)
        return a->body && b->body && a->body_len == b->body_len && !memcmp(a->body, b->body, a->body_len);
    return a->blob && b->blob && !strcmp(a->blob, b->blob);
}

/* Refresh: what the engine may hold from the replaced table. */
static int ms_kind(const ms_file *f)
{
    if (f->is_decl) return SH_MODSERVE_RF_DECL;
    if (f->ptype && !_stricmp(f->ptype, "baseModel")) return SH_MODSERVE_RF_MODEL;
    if (f->ptype && !_stricmp(f->ptype, "image")) return SH_MODSERVE_RF_IMAGE;
    return 0;
}

static void ms_diff_note(char *out, size_t cap, const char *request)
{
    size_t at = strlen(out);
    if (at + strlen(request) + 8 >= cap) {
        if (at + 6 < cap && (at < 5 || strcmp(out + at - 5, ", ...")))
            _snprintf_s(out + at, cap - at, _TRUNCATE, ", ...");
        return;
    }
    _snprintf_s(out + at, cap - at, _TRUNCATE, "%s%s", at ? ", " : "", request);
}

static int ms_add_gone(ms_index *x, int *cap, const ms_index *from, int file, int pending)
{
    if (!ms_grow((void **)&x->gone, cap, x->ngone + 1, sizeof *x->gone)) return 0;
    x->gone[x->ngone].from = from;
    x->gone[x->ngone].file = file;
    x->gone[x->ngone].pending = pending;
    x->ngone++;
    return 1;
}

/* A registration pass registered a decl of mod `id` from this table (or an
 * earlier one): the mod is in use this session. */
static int ms_id_registered(const ms_index *t, const char *id)
{
    int i;
    for (; t; t = t->prev)
        for (i = 0; i < t->nfiles; ++i)
            if (t->files[i].reg_state == 1 && !t->files[i].is_sedef && !strcmp(t->bundles[t->files[i].bundle].id, id))
                return 1;
    return 0;
}

/* Per served request path, the new table against the one it replaces. Marks
 * of unchanged bytes carry over; changed decls, models and images become
 * pending; a gone shadow decl is pending (the engine re-reads the game's
 * bytes); a gone new name stays registered until DOOM restarts. */
static void ms_diff(ms_index *x, const ms_index *old)
{
    sh_modserve_diff *d = &x->diff;
    int i, cap = 0;
    for (i = 0; i < x->nfiles; ++i) {
        ms_file *f = &x->files[i];
        int o, kind;
        if (!f->served || !x->bundles[f->bundle].admitted) continue;
        o = old->nkeys ? ms_find(old->slots, old->mask, f->request) : -1;
        if (o >= 0 && !old->files[o].served) o = -1;
        if (o < 0) {
            d->added++;
            if (f->is_decl && !f->is_shadow) {
                d->added_decls++;
                if (f->is_sedef) d->added_rows++;
                else if (ms_id_registered(old, x->bundles[f->bundle].id)) { f->refresh_new = 1; d->added_active++; }
            }
            continue;
        }
        if (ms_same_bytes(f, &old->files[o])) {
            f->refresh = old->files[o].refresh;
            f->refresh_gen = old->files[o].refresh_gen;
            f->refresh_new = old->files[o].refresh_new && f->reg_state != 1;
            continue;
        }
        d->changed++;
        if (f->is_sedef) d->changed_rows++;
        kind = ms_kind(f);
        if (kind && f->ptype && f->pname) f->refresh = 1;
        if (kind == SH_MODSERVE_RF_DECL) d->changed_decls++;
        else if (kind == SH_MODSERVE_RF_MODEL) d->changed_models++;
        else if (kind == SH_MODSERVE_RF_IMAGE) d->changed_images++;
        else if (!strncmp(f->request, "generated/swf/", 14)) d->changed_map++;
        else { d->changed_restart++; ms_diff_note(d->restart_first, sizeof d->restart_first, f->request); }
    }
    for (i = 0; i < old->nfiles; ++i) {
        const ms_file *of = &old->files[i];
        int n;
        if (!of->served || !old->bundles[of->bundle].admitted) continue;
        n = x->nkeys ? ms_find(x->slots, x->mask, of->request) : -1;
        if (n >= 0 && x->files[n].served) continue;
        d->gone++;
        if (of->is_decl && of->is_shadow && !of->is_restore && of->ptype && of->pname) {
            d->gone_shadows++;
            if (!ms_add_gone(x, &cap, old, i, 1)) ms_log("refresh: out of memory; a removed shadow keeps its bundle's text");
        } else if (of->is_sedef && of->ptype && of->pname) {
            /* A removed editor row: re-parsed without a source it resolves no
             * entityDef, so the next palette build leaves it out. */
            d->gone_rows++;
            if (!ms_add_gone(x, &cap, old, i, 1)) ms_log("refresh: out of memory; a removed editor row stays in the palette");
        } else if (of->is_decl) d->gone_names++;
    }
    /* Gone shadows an earlier rescan left pending, still not served. */
    for (i = 0; i < old->ngone; ++i) {
        const ms_gone *g = &old->gone[i];
        int n;
        if (!InterlockedCompareExchange((volatile LONG *)&g->pending, 0, 0)) continue;
        n = x->nkeys ? ms_find(x->slots, x->mask, g->from->files[g->file].request) : -1;
        if (n >= 0 && x->files[n].served) continue;
        (void)ms_add_gone(x, &cap, g->from, g->file, 1);
    }
}

int sh_modserve_index_build(const char *mods_root)
{
    ms_index *x, *old;
    double t0 = ms_now_ms();
    int i;
    if (!mods_root) return -1;
    AcquireSRWLockExclusive(&g_build_lock);
    x = (ms_index *)calloc(1, sizeof *x);
    if (!x) { ReleaseSRWLockExclusive(&g_build_lock); ms_log("index refused: out of memory"); return -1; }
    strncpy_s(x->root, sizeof x->root, mods_root, _TRUNCATE);
    strncpy_s(g_last_root, sizeof g_last_root, mods_root, _TRUNCATE);
    ms_scan_folder(x, mods_root);
    ms_load_aliases(x, mods_root);
    if (!ms_resolve(x) || !ms_build_tables(x)) {
        ReleaseSRWLockExclusive(&g_build_lock);
        ms_log("index refused: out of memory; the previous table stays");
        ms_index_free(x);
        return -1;
    }
    ms_resolve_needs(x);
    for (i = 0; i < x->nbundles; ++i) x->admitted += x->bundles[i].admitted;
    x->build_ms = ms_now_ms() - t0;
    /* Previously registered decls keep their state across a rescan. */
    old = g_index;
    /* One line per bundle and requirement, the first time an index carries
     * that bundle with it (a rescan does not repeat it). */
    for (i = 0; i < x->nbundles; ++i) {
        const ms_bundle *b = &x->bundles[i];
        int g, seen = 0;
        if (!b->admitted || !b->req) continue;
        if (old) {
            int o = ms_bundle_by_hash(old, b->hash, 1);
            seen = o >= 0 && old->bundles[o].req == b->req;
        }
        if (seen) continue;
        for (g = 0; g < SH_MODSERVE_REQ_COUNT; ++g)
            if (b->req & (1u << g)) ms_log("requirements: %s=0 for %s@%s", MS_GATES[g], b->id, b->version);
    }
    if (old) {
        for (i = 0; i < x->nfiles; ++i) {
            ms_file *f = &x->files[i];
            int o;
            if (!f->is_decl || !f->served) continue;
            o = ms_find(old->slots, old->mask, f->request);
            if (o >= 0 && old->files[o].reg_state) f->reg_state = old->files[o].reg_state;
            /* A shadow a registration pass withdrew stays withdrawn while the
             * same bytes serve the path (the pass that withdrew it marked it
             * done and does not look at it again). */
            if (o >= 0 && old->files[o].withdrawn && f->blob && old->files[o].blob && !strcmp(f->blob, old->files[o].blob) &&
                !old->files[o].body && !f->body)
                f->withdrawn = 1;
            /* The engine holds what an earlier table served: a shadow whose
             * bytes did not change is not offered to the early-decl pass again. */
            if (o >= 0 && ms_same_bytes(f, &old->files[o])) {
                f->opens = old->files[o].opens;
                f->reparse_tried = old->files[o].reparse_tried;
                f->opened_ungated = old->files[o].opened_ungated;
            }
        }
    }
    if (old)
        for (i = 0; i < x->nfiles; ++i) {
            ms_file *f = &x->files[i];
            int o;
            if (f->is_decl || !f->served) continue;
            o = ms_find(old->slots, old->mask, f->request);
            if (o >= 0 && ms_same_bytes(f, &old->files[o])) {
                f->opens = old->files[o].opens;
                f->reparse_tried = old->files[o].reparse_tried;
                f->opened_ungated = old->files[o].opened_ungated;
            }
        }
    if (old) ms_diff(x, old);
    x->prev = old;
    /* Publish the complete table; the old one is retired, never freed, since
     * a provider open on another thread may be inside it. */
    InterlockedExchangePointer((void *volatile *)&g_index, x);
    ReleaseSRWLockExclusive(&g_build_lock);
    ms_log("index built in %.1f ms (%d bundles, %d files)%s%s", x->build_ms, x->admitted, x->served_files,
           x->nbundles > x->admitted ? "; skipped bundles are listed above" : "",
           x->conflicts ? "; conflicts are listed above" : "");
    { int i; for (i = 0; i < MS_LISTENERS; i++) if (g_index_listener[i]) g_index_listener[i](); }
    return x->admitted;
}

void sh_modserve_set_index_listener(sh_modserve_index_fn fn)
{
    int i;
    if (!fn) { for (i = 0; i < MS_LISTENERS; i++) g_index_listener[i] = NULL; return; }
    for (i = 0; i < MS_LISTENERS; i++) if (g_index_listener[i] == fn) return;
    for (i = 0; i < MS_LISTENERS; i++) if (!g_index_listener[i]) { g_index_listener[i] = fn; return; }
}

int sh_modserve_index_boot(void)
{
    char base[MAX_PATH], root[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
        ms_log("index empty: the user-data folder is unavailable");
        return 0;
    }
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\snapmap-plus\\mods", base);
    return sh_modserve_index_build(root);
}

int sh_modserve_index_rescan(void)
{
    char root[MAX_PATH];
    AcquireSRWLockShared(&g_build_lock);
    strncpy_s(root, sizeof root, g_last_root, _TRUNCATE);
    ReleaseSRWLockShared(&g_build_lock);
    if (!root[0]) return sh_modserve_index_boot();
    return sh_modserve_index_build(root);
}

int sh_modserve_active(void)
{
    const ms_index *x = g_index;
    return x && x->nkeys > 0;
}

/* ------------------------------------------------------------------ serve */

static const ms_file *ms_lookup_file(const ms_index **out_index, const char *request)
{
    const ms_index *x = g_index;
    int f;
    if (!x || !x->nkeys || !request) return NULL;
    __try { f = ms_find(x->slots, x->mask, request); }
    __except (EXCEPTION_EXECUTE_HANDLER) { f = -1; }
    if (f < 0 || x->files[f].withdrawn) return NULL;
    if (out_index) *out_index = x;
    return &x->files[f];
}

int sh_modserve_module_served(const char *module_name)
{
    const ms_index *x = NULL;
    const ms_file *f;
    char prefix[14];
    int i;
    if (!module_name) return 0;
    for (i = 0; i < 13 && module_name[i]; ++i) prefix[i] = module_name[i] == '\\' ? '/' : module_name[i];
    prefix[i] = 0;
    if (_stricmp(prefix, "maps/modules/") != 0) return 0;
    f = ms_lookup_file(&x, module_name);
    return f && f->ptype && !strcmp(f->ptype, "snapModule");
}

int sh_modserve_lookup(const char *request, sh_modserve_hit *out)
{
    const ms_index *x = NULL;
    const ms_file *f = ms_lookup_file(&x, request);
    if (!f) return 0;
    if (out) {
        out->request = f->request;
        out->disk_path = f->disk;
        out->size = f->body ? (uint64_t)f->body_len : f->size;
        out->bundle_id = x->bundles[f->bundle].id;
        out->bundle_hash = x->bundles[f->bundle].hash;
        out->is_decl = f->is_decl;
        out->is_shadow = f->is_shadow;
        out->is_stock = f->stock != NULL;
        out->stock_source = f->stock;
    }
    return 1;
}

/* The bytes of one indexed file (process heap), or NULL: a stock pointer's
 * installed bytes, else the file at its indexed size. A pointer the reader
 * cannot answer falls back to a copy in the bundle folder with the same
 * hash. */
static unsigned char *ms_file_raw(const ms_file *f, size_t *len)
{
    unsigned char *buf = NULL;
    *len = 0;
    if (f->stock) {
        char cause[SH_MB_CAUSE_MAX], hex[SH_MB_HASH_HEX + 1];
        if (sh_mb_stock_read(f->stock, f->blob, &buf, len, f->request, cause, sizeof cause) == SH_MB_STOCK_OK) return buf;
        buf = ms_read_whole(f->disk, SH_MODSERVE_MAX_FILE, len);
        if (buf) {
            sh_mb_sha256_hex(buf, *len, hex);
            if (strcmp(hex, f->blob) != 0) { HeapFree(GetProcessHeap(), 0, buf); buf = NULL; *len = 0; }
        }
        return buf;
    }
    buf = ms_read_whole(f->disk, SH_MODSERVE_MAX_FILE, len);
    if (buf && (uint64_t)*len != f->size) { HeapFree(GetProcessHeap(), 0, buf); buf = NULL; *len = 0; }
    return buf;
}

/* Bounded cache of small files: decls are opened three times while they are
 * registered (source read, probe-gated open, load). Every open gets its own
 * copy, so eviction never frees bytes a stream holds. */
typedef struct ms_cached { const ms_file *file; unsigned char *bytes; size_t len; uint64_t tick; } ms_cached;
static ms_cached g_cache[MS_CACHE_SLOTS];
static size_t g_cache_bytes;
static uint64_t g_cache_tick;
static SRWLOCK g_cache_lock = SRWLOCK_INIT;
static volatile LONG g_cache_enabled = 1;

static unsigned char *ms_cache_copy(const ms_file *f, size_t *len)
{
    unsigned char *copy = NULL;
    int i;
    AcquireSRWLockExclusive(&g_cache_lock);
    for (i = 0; i < MS_CACHE_SLOTS; ++i) {
        if (g_cache[i].file != f) continue;
        copy = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, g_cache[i].len ? g_cache[i].len : 1u);
        if (copy) {
            memcpy(copy, g_cache[i].bytes, g_cache[i].len);
            *len = g_cache[i].len;
            g_cache[i].tick = ++g_cache_tick;
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_cache_lock);
    return copy;
}

static void ms_cache_put(const ms_file *f, const unsigned char *bytes, size_t len)
{
    unsigned char *mine;
    int i, slot = -1;
    if (len >= SH_MODSERVE_CACHE_FILE) return;
    mine = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, len ? len : 1u);
    if (!mine) return;
    memcpy(mine, bytes, len);
    AcquireSRWLockExclusive(&g_cache_lock);
    for (i = 0; i < MS_CACHE_SLOTS; ++i) if (g_cache[i].file == f) break;
    if (i == MS_CACHE_SLOTS) {
        for (;;) {
            int lru = -1, used = 0;
            slot = -1;
            for (i = 0; i < MS_CACHE_SLOTS; ++i) {
                if (!g_cache[i].file) { if (slot < 0) slot = i; continue; }
                used++;
                if (lru < 0 || g_cache[i].tick < g_cache[lru].tick) lru = i;
            }
            if (slot >= 0 && g_cache_bytes + len <= SH_MODSERVE_CACHE_TOTAL) break;
            if (lru < 0) { slot = -1; break; }
            /* Evict the least recently used file; streams own their copies. */
            g_cache_bytes -= g_cache[lru].len;
            HeapFree(GetProcessHeap(), 0, g_cache[lru].bytes);
            memset(&g_cache[lru], 0, sizeof g_cache[lru]);
            (void)used;
        }
        if (slot >= 0) {
            g_cache[slot].file = f;
            g_cache[slot].bytes = mine;
            g_cache[slot].len = len;
            g_cache[slot].tick = ++g_cache_tick;
            g_cache_bytes += len;
            mine = NULL;
        }
    }
    ReleaseSRWLockExclusive(&g_cache_lock);
    if (mine) HeapFree(GetProcessHeap(), 0, mine);
}

int sh_modserve_open(const char *request, unsigned char **bytes, size_t *length)
{
    const ms_index *x = NULL;
    const ms_file *f;
    unsigned char *buf = NULL;
    size_t len = 0;
    int first;
    if (bytes) *bytes = NULL;
    if (length) *length = 0;
    if (!bytes || !length) return 0;
    f = ms_lookup_file(&x, request);
    if (!f) return 0;
    if (f->body) {
        /* A composed text (ms_resolve): the index holds the bytes. */
        len = f->body_len;
        buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, len ? len : 1u);
        if (!buf) return -1;
        memcpy(buf, f->body, len);
    } else if (InterlockedCompareExchange(&g_cache_enabled, 0, 0)) buf = ms_cache_copy(f, &len);
    if (buf && !f->body) InterlockedIncrement(&g_cache_hits);
    else if (!buf) {
        buf = ms_file_raw(f, &len);
        if (!buf) {
            InterlockedIncrement(&g_open_failures);
            if (!InterlockedExchange((volatile LONG *)&f->fail_logged, 1)) {
                if (f->stock)
                    ms_log("'%s' from %s@%s points at installed content '%s' (sha256 %.16s), which could not be read "
                           "with that hash; the engine's own resolution is used", f->request, x->bundles[f->bundle].id,
                           x->bundles[f->bundle].version, f->stock, f->blob);
                else
                    ms_log("'%s' from %s@%s could not be read at its indexed size (%llu bytes) from '%s'; "
                           "the engine's own resolution is used", f->request, x->bundles[f->bundle].id,
                           x->bundles[f->bundle].version, (unsigned long long)f->size, f->disk);
            }
            return -1;
        }
        if (InterlockedCompareExchange(&g_cache_enabled, 0, 0)) ms_cache_put(f, buf, len);
    }
    /* A render program's files are named once each (the first 64): which of
     * them the engine opened shows which variant this GPU binds. A file first
     * served before its bundle's requirement gates were live was resolved by
     * the engine against the cut-content blacklists (modserve.h). */
    first = InterlockedIncrement((volatile LONG *)&f->opens) == 1;
    if (first && (x->bundles[f->bundle].req & ~(unsigned)InterlockedCompareExchange(&g_gates_live, 0, 0)))
        InterlockedExchange((volatile LONG *)&f->opened_ungated, 1);
    if (first &&
        (!strncmp(f->request, "generated/renderprogs/", 22) || !strncmp(f->request, "generated/spirv/", 16) ||
         !strncmp(f->request, "generated/decls/renderprog/", 27)) &&
        InterlockedIncrement(&g_renderprog_logged) <= 64)
        ms_log("renderprog: served %s (%s@%s, %zu bytes%s)", f->request, x->bundles[f->bundle].id,
               x->bundles[f->bundle].version, len, f->stock ? ", installed content" : "");
    InterlockedIncrement(&g_opens_served);
    if (f->is_shadow && !InterlockedExchange((volatile LONG *)&f->shadow_logged, 1))
        ms_log("shadow '%s' served from %s@%s (%zu bytes%s)", f->request, x->bundles[f->bundle].id,
               x->bundles[f->bundle].version, len, f->body ? ", composed with the other providers' list entries" : "");
    *bytes = buf;
    *length = len;
    return 1;
}

int sh_modserve_probe_claims(const char *path)
{
    const ms_file *f = ms_lookup_file(NULL, path);
    if (!f || !f->is_decl) return 0;
    InterlockedIncrement(&g_probe_claims);
    return 1;
}

#ifdef SH_MODSERVE_TESTING
void sh_modserve_test_cache(int enabled)
{
    int i;
    InterlockedExchange(&g_cache_enabled, enabled ? 1 : 0);
    AcquireSRWLockExclusive(&g_cache_lock);
    for (i = 0; i < MS_CACHE_SLOTS; ++i) if (g_cache[i].bytes) HeapFree(GetProcessHeap(), 0, g_cache[i].bytes);
    memset(g_cache, 0, sizeof g_cache);
    g_cache_bytes = 0;
    ReleaseSRWLockExclusive(&g_cache_lock);
}
size_t sh_modserve_test_cache_bytes(void) { return g_cache_bytes; }
void sh_modserve_test_reset_registration(void)
{
    ms_index *x = g_index;
    int i;
    if (x) for (i = 0; i < x->nfiles; ++i) x->files[i].reg_state = 0;
}
void sh_modserve_test_reset_gates(void)
{
    g_gates_owned = 0;
    memset(g_gates_original, 0, sizeof g_gates_original);
}
unsigned sh_modserve_test_gates_owned(void) { return g_gates_owned; }
#endif

/* ------------------------------------------------------------------ plan */

static int ms_need_blocks(const char *name)
{
    return !strncmp(name, "mods/", 5) || strstr(name, "/mods/") != NULL;
}

/* A decl a plan may register: every decl a bundle provides except shadows.
 * A grandfathered bundle's shadows are planned too, because a converted
 * first-generation package lists every name it carries as a shadow, new
 * names included; plan_run tells the two apart by whether the engine already
 * has the source (a stock decl: served by the provider, never reloaded). */
static int ms_plannable(const ms_index *x, const ms_file *f)
{
    return f->is_decl && (!f->is_shadow || x->bundles[f->bundle].grandfathered);
}

/* Collect `roots` and the closure of bundle-provided decls they need into an
 * ordered plan. */
static int ms_plan_from(const ms_index *x, const unsigned char *root_mark, int persist_sedefs,
                        sh_modserve_plan *out)
{
    int *in_plan = NULL, *queue = NULL, *order = NULL, *indeg = NULL, *step_of = NULL;
    const char **cause = NULL;
    int nq = 0, qh = 0, i, k, changed, n = 0, nf = x->nfiles;
    memset(out, 0, sizeof *out);
    in_plan = (int *)calloc((size_t)nf + 1, sizeof(int));
    queue = (int *)malloc(sizeof(int) * ((size_t)nf + 1));
    cause = (const char **)calloc((size_t)nf + 1, sizeof(char *));
    if (!in_plan || !queue || !cause) goto oom;
    for (i = 0; i < nf; ++i)
        if (root_mark[i]) { in_plan[i] = 1; queue[nq++] = i; }
    while (qh < nq) {
        const ms_file *f = &x->files[queue[qh++]];
        for (k = 0; k < f->need_count; ++k) {
            const ms_need *nd = &x->needs[f->need_first + k];
            int p = nd->file;
            if (p < 0) {
                if (ms_need_blocks(nd->name) && !cause[f - x->files]) {
                    char buf[MS_CAUSE_MAX];
                    _snprintf_s(buf, sizeof buf, _TRUNCATE, "needs %s '%s', which no admitted bundle provides",
                                nd->type, nd->name);
                    cause[f - x->files] = ms_strdup((ms_index *)x, buf);
                    if (!cause[f - x->files]) cause[f - x->files] = "needs a missing mod identity";
                }
                continue;
            }
            if (ms_plannable(x, &x->files[p]) && !in_plan[p]) { in_plan[p] = 1; queue[nq++] = p; }
        }
    }
    /* Blocked needs propagate to dependents. */
    do {
        changed = 0;
        for (i = 0; i < nf; ++i) {
            const ms_file *f = &x->files[i];
            if (!in_plan[i] || cause[i]) continue;
            for (k = 0; k < f->need_count; ++k) {
                const ms_need *nd = &x->needs[f->need_first + k];
                int p = nd->file;
                if (p >= 0 && in_plan[p] && cause[p]) {
                    char buf[MS_CAUSE_MAX];
                    _snprintf_s(buf, sizeof buf, _TRUNCATE, "needs %s '%s', which is blocked", nd->type, nd->name);
                    cause[i] = ms_strdup((ms_index *)x, buf);
                    if (!cause[i]) cause[i] = "needs a blocked decl";
                    changed = 1;
                    break;
                }
            }
        }
    } while (changed);
    /* Order: dependencies first, then rank, then manifest order. */
    order = (int *)malloc(sizeof(int) * ((size_t)nf + 1));
    indeg = (int *)calloc((size_t)nf + 1, sizeof(int));
    step_of = (int *)malloc(sizeof(int) * ((size_t)nf + 1));
    if (!order || !indeg || !step_of) goto oom;
    for (i = 0; i < nf; ++i) {
        const ms_file *f = &x->files[i];
        if (!in_plan[i]) continue;
        for (k = 0; k < f->need_count; ++k) {
            int p = x->needs[f->need_first + k].file;
            if (p >= 0 && p != i && in_plan[p]) indeg[i]++;
        }
    }
    for (;;) {
        int best = -1, best_cycle = -1;
        for (i = 0; i < nf; ++i) {
            if (in_plan[i] != 1) continue;
            if (best_cycle < 0 || x->files[i].rank < x->files[best_cycle].rank) best_cycle = i;
            if (indeg[i] == 0 && (best < 0 || x->files[i].rank < x->files[best].rank)) best = i;
        }
        if (best < 0) best = best_cycle;   /* a need cycle: fall back to rank order */
        if (best < 0) break;
        in_plan[best] = 2;
        order[n++] = best;
        /* Release the steps that needed this one. */
        for (i = 0; i < nf; ++i) {
            const ms_file *f = &x->files[i];
            if (in_plan[i] != 1) continue;
            for (k = 0; k < f->need_count; ++k) {
                int p = x->needs[f->need_first + k].file;
                if (p == best) indeg[i]--;
            }
        }
    }
    out->steps = (sh_modserve_step *)calloc((size_t)(n ? n : 1), sizeof(sh_modserve_step));
    if (!out->steps) goto oom;
    for (i = 0; i < n; ++i) {
        const ms_file *f = &x->files[order[i]];
        sh_modserve_step *s = &out->steps[i];
        s->type = f->ptype;
        s->name = f->pname;
        s->reg_path = f->reg;
        s->bundle_id = x->bundles[f->bundle].id;
        s->rank = f->rank;
        /* The editor plan keeps every step for the session: a row is kept, and
         * what its parse points at must outlive the purges around Play. */
        s->persist = persist_sedefs;
        /* A restored stock decl is planned like a shadow: left to the provider
         * when the engine knows its source, registered when it does not. */
        s->shadow = f->is_shadow || f->is_restore;
        s->blocked = cause[order[i]];
        s->file = order[i];
        if (s->blocked) out->blocked++;
    }
    out->count = n;
    free(in_plan); free(queue); free(order); free(indeg); free(step_of); free((void *)cause);
    return n;
oom:
    free(in_plan); free(queue); free(order); free(indeg); free(step_of); free((void *)cause);
    memset(out, 0, sizeof *out);
    return -1;
}

int sh_modserve_plan_editor(sh_modserve_plan *out)
{
    const ms_index *x = g_index;
    unsigned char *mark;
    int i, r;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!x) return 0;
    mark = (unsigned char *)calloc((size_t)x->nfiles + 1, 1);
    if (!mark) return -1;
    for (i = 0; i < x->nfiles; ++i)
        mark[i] = (unsigned char)(x->files[i].served && x->files[i].is_sedef && ms_plannable(x, &x->files[i]));
    r = ms_plan_from(x, mark, 1, out);
    free(mark);
    return r;
}

static int ms_bundle_by_hash(const ms_index *x, const char *hash, int admitted_only)
{
    int i, round;
    for (round = 0; round < 2 && hash; ++round) {
        for (i = 0; i < x->nbundles; ++i)
            if (x->bundles[i].hash && !_stricmp(x->bundles[i].hash, hash) && (!admitted_only || x->bundles[i].admitted))
                return i;
        /* A delivered copy the store upgrade dropped answers as the kept bundle. */
        for (i = 0; i < x->naliases; ++i)
            if (!_stricmp(x->aliases[i].from, hash)) break;
        hash = i < x->naliases ? x->aliases[i].to : NULL;
    }
    return -1;
}

int sh_modserve_plan_bundles(const char *const *bundle_hashes, int n, sh_modserve_plan *out, int *unknown)
{
    const ms_index *x = g_index;
    unsigned char *mark;
    int i, r, missing = 0;
    if (unknown) *unknown = 0;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!x || n <= 0 || !bundle_hashes) { if (unknown && n > 0) *unknown = n; return 0; }
    mark = (unsigned char *)calloc((size_t)x->nfiles + 1, 1);
    if (!mark) return -1;
    for (i = 0; i < n; ++i) {
        int b = ms_bundle_by_hash(x, bundle_hashes[i], 1), k, superseded = 0;
        if (b < 0) { missing++; continue; }
        for (k = x->bundles[b].first_file; k < x->bundles[b].first_file + x->bundles[b].file_count; ++k) {
            const ms_file *f = &x->files[k];
            int use = k;
            if (!f->served) {
                /* Another installed build of the same mod serves this path:
                 * the map gets the newer files (manager decision, lane M2). */
                int w = ms_find(x->slots, x->mask, f->request);
                if (w < 0 || strcmp(x->bundles[x->files[w].bundle].id, x->bundles[b].id)) continue;
                use = w;
                superseded = 1;
            }
            f = &x->files[use];
            if (f->served && ms_plannable(x, f) && !f->is_sedef) mark[use] = 1;
        }
        if (superseded)
            ms_log("map bundle %s@%s (%.16s) is superseded; its paths are served by the newest installed build",
                   x->bundles[b].id, x->bundles[b].version, x->bundles[b].hash);
    }
    r = ms_plan_from(x, mark, 0, out);
    free(mark);
    if (unknown) *unknown = missing;
    return r;
}

/* The editor rows of the bundles a map names, checked against the built
 * palette at load (lane M6). A row counts as missing when the palette lacks
 * it and no editor pass has visited it yet (reg_state 0): a row registered
 * before an earlier build that is still absent was rejected by the builder's
 * admission, and another rebuild would not admit it. Bundles are selected as
 * sh_modserve_plan_bundles selects them, superseded builds included. */
int sh_modserve_map_rows_sync(const char *const *bundle_hashes, int n, const sh_modserve_palette_ops *ops,
                              sh_modserve_rows_sync *out)
{
    const ms_index *x = g_index;
    unsigned char *cand;
    int i, k, r = 0;
    sh_modserve_rows_sync res;
    memset(&res, 0, sizeof res);
    res.rows_before = res.rows_after = -1;
    if (out) *out = res;
    if (n <= 0 || !bundle_hashes || !x || !x->nfiles) return 0;
    if (!ops || !ops->palette_ready || !ops->in_palette || !ops->rebuild) return -1;
    cand = (unsigned char *)calloc((size_t)x->nfiles + 1, 1);
    if (!cand) return -1;
    for (i = 0; i < n; ++i) {
        int b = ms_bundle_by_hash(x, bundle_hashes[i], 1);
        if (b < 0) continue;
        for (k = x->bundles[b].first_file; k < x->bundles[b].first_file + x->bundles[b].file_count; ++k) {
            const ms_file *f = &x->files[k];
            int use = k;
            if (!f->served) {
                int w = ms_find(x->slots, x->mask, f->request);
                if (w < 0 || strcmp(x->bundles[x->files[w].bundle].id, x->bundles[b].id)) continue;
                use = w;
            }
            f = &x->files[use];
            if (f->served && f->is_sedef && ms_plannable(x, f) && !cand[use]) { cand[use] = 1; res.rows++; }
        }
    }
    /* No palette yet: the first editor entry's build registers every row. */
    if (!res.rows || ops->palette_ready(ops->context) != 1) goto done;
    for (i = 0; i < x->nfiles; ++i) {
        if (!cand[i]) continue;
        if (x->files[i].reg_state != 0 || ops->in_palette(ops->context, x->files[i].pname) == 1) { cand[i] = 0; continue; }
        cand[i] = 2;
        res.missing++;
    }
    if (!res.missing) goto done;
    /* Once per call: the rebuild's editor pass registers the rows (and the
     * closure they need) before the builder runs. */
    res.rebuild = ops->rebuild(ops->context, &res.rows_before, &res.rows_after);
    r = 1;
    if (res.rebuild == 1)
        for (i = 0; i < x->nfiles; ++i) {
            if (cand[i] != 2) continue;
            if (x->files[i].reg_state == 1) res.registered++;
            if (ops->in_palette(ops->context, x->files[i].pname) == 1) res.admitted++;
        }
done:
    free(cand);
    if (out) *out = res;
    return r;
}

void sh_modserve_plan_free(sh_modserve_plan *plan)
{
    if (!plan) return;
    free(plan->steps);
    memset(plan, 0, sizeof *plan);
}

/* The bytes a file serves (HeapAlloc'd), composed or from disk. */
static unsigned char *ms_file_bytes(const ms_file *f, size_t *len)
{
    unsigned char *b;
    *len = 0;
    if (f->body) {
        b = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, f->body_len ? f->body_len : 1u);
        if (b) { memcpy(b, f->body, f->body_len); *len = f->body_len; }
        return b;
    }
    return ms_file_raw(f, len);
}

/* 1 with the first model an animWeb's modelInfos block names that no served
 * bundle file provides as an md6Def, the engine has no md6Def source for and
 * the game archives do not hold (the registry does not know a stock decl
 * whose source is not registered yet: the player hands' md6Def at map
 * activation); 0 when every model resolves (or the text cannot be read, or
 * the engine cannot tell). */
static int ms_animweb_unresolved(const ms_index *x, const ms_file *f, const sh_modserve_engine_ops *ops,
                                 char *model, size_t cap)
{
    size_t len = 0;
    unsigned char *text = ms_file_bytes(f, &len);
    mc_lex l;
    mc_tok t;
    int found = 0;
    model[0] = 0;
    if (!text) return 0;
    l.t = (const char *)text;
    l.len = len;
    l.pos = 0;
    while (!found && (t = mc_next(&l)).kind != MC_END) {
        if (t.kind != MC_WORD || t.n != 10 || _strnicmp(t.p, "modelInfos", 10)) continue;
        if (mc_next(&l).kind != MC_LB) continue;
        for (;;) {
            mc_tok m = mc_next(&l), v;
            char name[MS_NAME_MAX], key[MS_NAME_MAX + 8];
            int p;
            if (m.kind == MC_RB || m.kind == MC_END || m.kind == MC_BAD) break;
            if (m.kind != MC_STR) continue;
            v = mc_peek(&l);
            if (v.kind != MC_STR) continue;
            mc_next(&l);
            if (m.n < 3 || m.n - 2 >= sizeof name) continue;
            memcpy(name, m.p + 1, m.n - 2);
            name[m.n - 2] = 0;
            ms_lower(name);
            _snprintf_s(key, sizeof key, _TRUNCATE, "md6def\x01%s", name);
            p = ms_find(x->pslots, x->pmask, key);
            if (p >= 0 && x->files[p].served && !x->files[p].withdrawn) continue;
            if (ops->source_registered(ops->context, "md6Def", name) != 0) continue;
            if (ops->stock_exists) {
                char request[MS_NAME_MAX + 32];
                _snprintf_s(request, sizeof request, _TRUNCATE, "generated/decls/md6def/%s.decl", name);
                if (ops->stock_exists(ops->context, request) != 0) continue;
            }
            strncpy_s(model, cap, name, _TRUNCATE);
            found = 1;
            break;
        }
    }
    HeapFree(GetProcessHeap(), 0, text);
    return found;
}

/* ------------------------------------------------------------------ resource watermark */

#define MS_RL_NAME    0x10
#define MS_RL_NEXT    0x18
#define MS_RL_ENTRIES 0x20
#define MS_RL_COUNT   0x28
#define MS_RES_LEVEL  0x28
#define MS_RL_MAX_LISTS   4096
#define MS_RL_MAX_ENTRIES (1 << 22)

static int ms_uptr_cmp(const void *a, const void *b)
{
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

/* Visit every resource: fn(context, list, resource). 1 when the walk completed. */
typedef void (*ms_res_fn)(void *context, const unsigned char *list, unsigned char *res);
static int ms_res_walk(const void *head_slot, ms_res_fn fn, void *context, unsigned *lists)
{
    int ok = 0;
    unsigned nl = 0;
    if (lists) *lists = 0;
    if (!head_slot) return 0;
    __try {
        const unsigned char *l = *(const unsigned char *const *)head_slot;
        size_t total = 0;
        for (; l && nl < MS_RL_MAX_LISTS; l = *(const unsigned char *const *)(l + MS_RL_NEXT), nl++) {
            unsigned char *const *entries = *(unsigned char *const *const *)(l + MS_RL_ENTRIES);
            int count = *(const int *)(l + MS_RL_COUNT), i;
            if (count < 0 || count > MS_RL_MAX_ENTRIES || (count && !entries)) break;
            total += (size_t)count;
            if (total > MS_RL_MAX_ENTRIES) break;
            for (i = 0; i < count; ++i) if (entries[i]) fn(context, l, entries[i]);
        }
        ok = !l;
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    if (lists) *lists = nl;
    return ok;
}

typedef struct ms_take { sh_modserve_watermark *w; size_t cap; int oom; } ms_take;
static void ms_take_one(void *context, const unsigned char *list, unsigned char *res)
{
    ms_take *t = (ms_take *)context;
    (void)list;
    if (t->oom) return;
    if (t->w->n == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 65536;
        uintptr_t *g = (uintptr_t *)realloc(t->w->res, cap * sizeof(uintptr_t));
        if (!g) { t->oom = 1; return; }
        t->w->res = g;
        t->cap = cap;
    }
    t->w->res[t->w->n++] = (uintptr_t)res;
}

int sh_modserve_watermark_take(const void *head_slot, sh_modserve_watermark *w)
{
    ms_take t;
    if (!w) return 0;
    memset(w, 0, sizeof *w);
    t.w = w;
    t.cap = 0;
    t.oom = 0;
    w->ok = ms_res_walk(head_slot, ms_take_one, &t, NULL) && !t.oom;
    if (w->ok && w->n > 1) qsort(w->res, w->n, sizeof(uintptr_t), ms_uptr_cmp);
    return w->ok;
}

typedef struct ms_raise_cls { const unsigned char *list; unsigned n; } ms_raise_cls;
typedef struct ms_raise { const sh_modserve_watermark *w; sh_modserve_raised *out; ms_raise_cls cls[64]; int ncls; } ms_raise;
static void ms_raise_one(void *context, const unsigned char *list, unsigned char *res)
{
    ms_raise *r = (ms_raise *)context;
    uintptr_t key = (uintptr_t)res;
    int c;
    if (r->w->n && bsearch(&key, r->w->res, r->w->n, sizeof(uintptr_t), ms_uptr_cmp)) return;
    r->out->created++;
    if (*(unsigned *)(res + MS_RES_LEVEL) != 1) return;
    *(unsigned *)(res + MS_RES_LEVEL) = 4;
    r->out->raised++;
    for (c = 0; c < r->ncls && r->cls[c].list != list; ++c) {}
    if (c == r->ncls && r->ncls < 64) { r->cls[c].list = list; r->cls[c].n = 0; r->ncls++; }
    if (c < r->ncls) r->cls[c].n++;
}

int sh_modserve_watermark_raise(const void *head_slot, const sh_modserve_watermark *w, sh_modserve_raised *out)
{
    ms_raise r;
    sh_modserve_raised local;
    int ok, i, j;
    size_t at = 0;
    if (!out) out = &local;
    memset(out, 0, sizeof *out);
    if (!w || !w->ok) return 0;
    memset(&r, 0, sizeof r);
    r.w = w;
    r.out = out;
    ok = ms_res_walk(head_slot, ms_raise_one, &r, &out->lists);
    /* The classes, largest first, for the log. */
    for (i = 0; i < r.ncls; ++i)
        for (j = i + 1; j < r.ncls; ++j)
            if (r.cls[j].n > r.cls[i].n) { ms_raise_cls t = r.cls[i]; r.cls[i] = r.cls[j]; r.cls[j] = t; }
    for (i = 0; i < r.ncls && at + 40 < sizeof out->summary; ++i) {
        char name[32] = "?";
        int w2;
        __try {
            const char *s = *(const char *const *)(r.cls[i].list + MS_RL_NAME);
            if (s) { strncpy_s(name, sizeof name, s, _TRUNCATE); }
        } __except (EXCEPTION_EXECUTE_HANDLER) { strcpy_s(name, sizeof name, "?"); }
        w2 = _snprintf_s(out->summary + at, sizeof out->summary - at, _TRUNCATE, "%s%s %u", i ? ", " : "", name, r.cls[i].n);
        if (w2 < 0) break;
        at += (size_t)w2;
    }
    return ok;
}

/* Every resource the engine's lists hold: fn(context, list class, resource, name). */
typedef struct ms_each_res { sh_modserve_resource_fn fn; void *context; } ms_each_res;
static void ms_each_res_one(void *context, const unsigned char *list, unsigned char *res)
{
    ms_each_res *e = (ms_each_res *)context;
    const char *ln = *(const char *const *)(list + MS_RL_NAME), *rn = *(const char *const *)(res + 8);
    if (rn) e->fn(e->context, ln, res, rn);
}

int sh_modserve_resource_each(const void *head_slot, sh_modserve_resource_fn fn, void *context)
{
    ms_each_res e;
    if (!fn) return 0;
    e.fn = fn;
    e.context = context;
    return ms_res_walk(head_slot, ms_each_res_one, &e, NULL);
}

void sh_modserve_watermark_free(sh_modserve_watermark *w)
{
    if (!w) return;
    free(w->res);
    memset(w, 0, sizeof *w);
}

int sh_modserve_editor_refs(const sh_modserve_plan *plan, sh_modserve_ref_fn fn, void *context)
{
    const ms_index *x = g_index;
    int i, calls = 0;
    if (!plan || !fn || !x) return 0;
    for (i = 0; i < plan->count; ++i) {
        const sh_modserve_step *s = &plan->steps[i];
        size_t len = 0;
        unsigned char *text;
        mc_lex l;
        mc_tok t;
        if (s->rank != 3 || s->ready != 1 || s->file < 0 || s->file >= x->nfiles) continue;
        text = ms_file_bytes(&x->files[s->file], &len);
        if (!text) continue;
        l.t = (const char *)text;
        l.len = len;
        l.pos = 0;
        while ((t = mc_next(&l)).kind != MC_END) {
            const char *type = NULL;
            mc_tok v;
            char name[MS_NAME_MAX];
            if (t.kind != MC_WORD) continue;
            if (t.n == 11 && !_strnicmp(t.p, "animWebDecl", 11)) type = "animWeb";
            else if (t.n == 15 && !_strnicmp(t.p, "renderModelName", 15)) type = "md6Def";
            else if (t.n == 12 && !_strnicmp(t.p, "previewImage", 12)) type = "material";
            else continue;
            if (mc_peek(&l).kind != MC_EQ) continue;
            mc_next(&l);
            v = mc_next(&l);
            if (v.kind != MC_STR || v.n < 3 || v.n - 2 >= sizeof name) continue;
            memcpy(name, v.p + 1, v.n - 2);
            name[v.n - 2] = 0;
            /* A render model that is not an md6 is not a decl. */
            if (!strcmp(type, "md6Def") && (strlen(name) < 4 || _stricmp(name + strlen(name) - 4, ".md6"))) continue;
            fn(context, type, name, s->name);
            calls++;
        }
        HeapFree(GetProcessHeap(), 0, text);
    }
    return calls;
}

int sh_modserve_plan_run(sh_modserve_plan *plan, const sh_modserve_engine_ops *ops, const char *phase)
{
    ms_index *x = g_index;
    int i, k, ready = 0;
    unsigned char *failed, *stock;
    if (!plan || !ops || !ops->source_registered || !ops->register_file || !ops->load || !x) return -1;
    if (!phase) phase = "plan";
    failed = (unsigned char *)calloc((size_t)plan->count + 1, 1);
    stock = (unsigned char *)calloc((size_t)plan->count + 1, 1);
    if (!failed || !stock) { free(failed); free(stock); return -1; }
    /* 1: publish every source, so a parse that names another mod decl finds it. */
    for (i = 0; i < plan->count; ++i) {
        sh_modserve_step *s = &plan->steps[i];
        ms_file *f = &x->files[s->file];
        s->ready = 0;
        if (s->blocked) {
            failed[i] = 1;
            ms_log("%s: %s '%s' (%s) BLOCKED: %s", phase, s->type, s->name, s->bundle_id, s->blocked);
            continue;
        }
        if (f->reg_state == 1) continue;
        {
            int known = ops->source_registered(ops->context, s->type, s->name) == 1;
            char model[MS_NAME_MAX];
            /* An animWeb whose models block names a model nothing provides is
             * refused by the engine's parse (a C++ throw per load). Its file
             * is withdrawn from serving: a stock decl it shadowed serves
             * again, and a new name is never registered. */
            if (!_stricmp(s->type, "animWeb") && ms_animweb_unresolved(x, f, ops, model, sizeof model)) {
                InterlockedExchange(&f->withdrawn, 1);
                if (known && s->shadow) {
                    InterlockedExchange(&f->reg_state, 1);
                    stock[i] = 2;
                    ms_log("%s: animWeb '%s' (%s) shadow WITHDRAWN: it names model '%s', which neither a bundle nor "
                           "the game provides; the game's own animWeb is served", phase, s->name, s->bundle_id, model);
                } else {
                    failed[i] = 1;
                    InterlockedExchange(&f->reg_state, -1);
                    ms_log("%s: animWeb '%s' (%s) NOT READY: it names model '%s', which neither a bundle nor the game "
                           "provides; withdrawn and not registered (the engine would refuse it)", phase, s->name,
                           s->bundle_id, model);
                }
                continue;
            }
            if (known) {
                /* A shadow the engine already knows is a stock decl: the provider
                 * serves its bytes, and it is never reloaded here. */
                if (s->shadow) stock[i] = 1;
                continue;
            }
        }
        if (!ops->register_file(ops->context, s->type, s->name, s->reg_path)) {
            failed[i] = 1;
            InterlockedExchange(&f->reg_state, -1);
            ms_log("%s: %s '%s' (%s) NOT READY: DeclRegisterFile('%s') refused", phase, s->type, s->name,
                   s->bundle_id, s->reg_path);
        }
    }
    if (ops->sources_published) ops->sources_published(ops->context);
    /* 2: load in dependency order; a step whose needed decl failed is skipped. */
    for (i = 0; i < plan->count; ++i) {
        sh_modserve_step *s = &plan->steps[i];
        ms_file *f = &x->files[s->file];
        unsigned state = 0, lifetime = 0;
        int dep_failed = -1;
        if (failed[i]) { s->ready = -1; continue; }
        if (f->reg_state == 1) { s->ready = 1; ready++; continue; }
        if (stock[i] == 2) { s->ready = 1; ready++; continue; }   /* withdrawn: the stock decl serves */
        if (stock[i]) {
            /* Remembered as ready, so later loads of the map skip it quietly. */
            InterlockedExchange(&f->reg_state, 1);
            s->ready = 1;
            ready++;
            ms_log("%s: %s '%s' (%s) shadows a stock decl; served by the provider", phase, s->type, s->name,
                   s->bundle_id);
            continue;
        }
        for (k = 0; k < i && dep_failed < 0; ++k) {
            int n;
            if (!failed[k]) continue;
            for (n = 0; n < f->need_count; ++n)
                if (x->needs[f->need_first + n].file ==
                    plan->steps[k].file) { dep_failed = k; break; }
        }
        if (dep_failed >= 0) {
            failed[i] = 1;
            s->ready = -1;
            ms_log("%s: %s '%s' (%s) NOT READY: needs %s '%s', which failed", phase, s->type, s->name,
                   s->bundle_id, plan->steps[dep_failed].type, plan->steps[dep_failed].name);
            continue;
        }
        if (ops->load(ops->context, s->type, s->name, s->persist, &state, &lifetime)) {
            InterlockedExchange(&f->reg_state, 1);
            s->ready = 1;
            ready++;
            ms_log("%s: %s '%s' (%s) READY: state 0x%02x lifetime %u", phase, s->type, s->name, s->bundle_id,
                   state & 0xff, lifetime);
        } else {
            failed[i] = 1;
            s->ready = -1;
            InterlockedExchange(&f->reg_state, -1);
            ms_log("%s: %s '%s' (%s) NOT READY: state 0x%02x lifetime %u", phase, s->type, s->name,
                   s->bundle_id, state & 0xff, lifetime);
        }
    }
    free(failed);
    free(stock);
    return ready;
}


/* ------------------------------------------------------------------ requirements (apply) */

unsigned sh_modserve_requirements(void)
{
    const ms_index *x = g_index;
    unsigned all = 0;
    int i;
    if (!x) return 0;
    for (i = 0; i < x->nbundles; ++i)
        if (x->bundles[i].admitted) all |= x->bundles[i].req;
    return all;
}

static void ms_gate_names(unsigned mask, char *out, size_t cap)
{
    int g;
    size_t at = 0;
    out[0] = 0;
    for (g = 0; g < SH_MODSERVE_REQ_COUNT; ++g)
        if (mask & (1u << g))
            at += (size_t)_snprintf_s(out + at, cap - at, _TRUNCATE, "%s%s=0", at ? ", " : "", MS_GATES[g]);
}

int sh_modserve_requirements_apply(const sh_modserve_gate_ops *ops, const char *phase, sh_modserve_gates *out)
{
    sh_modserve_gates r;
    unsigned needed = sh_modserve_requirements(), restore = g_gates_owned & ~needed, touch, refused = 0;
    int g, before[SH_MODSERVE_REQ_COUNT] = { 0 }, want[SH_MODSERVE_REQ_COUNT] = { 0 };
    char names[160];
    memset(&r, 0, sizeof r);
    r.needed = needed;
    if (!phase) phase = "plan";
    /* No admitted bundle needs a gate and none is held: nothing is read or set. */
    if (!needed && !restore) { if (out) *out = r; return 0; }
    if (!ops || !ops->read || !ops->set) {
        ms_gate_names(needed, names, sizeof names);
        ms_log("%s: requirements %s NOT applied: the cvar system is unavailable", phase, names);
        r.not_live = needed;
        if (out) *out = r;
        return needed ? -1 : 0;
    }
    touch = needed | restore;
    /* Phase 1, disarm: read every affected gate, then set what differs. */
    for (g = 0; g < SH_MODSERVE_REQ_COUNT; ++g) {
        unsigned bit = 1u << g;
        int set;
        char text[16];
        if (!(touch & bit)) continue;
        if (ops->read(ops->context, MS_GATES[g], &before[g]) != 1) { refused |= bit; continue; }
        if (needed & bit) want[g] = 0;
        else want[g] = before[g] == 0 ? g_gates_original[g] : before[g];   /* changed by someone else: theirs */
        if (before[g] == want[g]) continue;
        _snprintf_s(text, sizeof text, _TRUNCATE, "%d", want[g]);
        set = ops->set(ops->context, MS_GATES[g], text);
        if (set != SH_MODSERVE_GATE_SET && set != SH_MODSERVE_GATE_QUEUED) { refused |= bit; continue; }
        r.changed |= bit;
        if (set == SH_MODSERVE_GATE_QUEUED) r.queued |= bit;
        if ((needed & bit) && !(g_gates_owned & bit)) { g_gates_original[g] = before[g]; g_gates_owned |= bit; }
    }
    /* A queued set reaches the gate only when the command buffer runs: drain
     * it here, between the phases, so the registration that follows is never
     * asked to parse a name the matcher still refuses. */
    if (r.queued) {
        if (ops->drain && ops->drain(ops->context) == 1) r.drained = 1;
        else {
            ms_gate_names(r.queued & needed, names, sizeof names);
            ms_log("%s: requirement set%s %s queued but the command buffer could not be drained", phase,
                   (r.queued & (r.queued - 1)) ? "s" : "", names[0] ? names : "(restore)");
        }
    }
    /* Phase 2, verify: the registration pass runs only against read-back values. */
    for (g = 0; g < SH_MODSERVE_REQ_COUNT; ++g) {
        unsigned bit = 1u << g;
        int now = -1;
        if (!(touch & bit) || (refused & bit)) continue;
        if (ops->read(ops->context, MS_GATES[g], &now) != 1) { refused |= bit; continue; }
        if (needed & bit) { if (now == 0) r.live |= bit; }
        else if (now == want[g]) { r.restored |= bit; g_gates_owned &= ~bit; }
    }
    if (r.changed & needed & r.live) {
        ms_gate_names(r.changed & needed & r.live, names, sizeof names);
        ms_log("%s: requirements applied: %s (%s)", phase, names,
               r.drained ? "queued, live after a command-buffer drain" : "set directly, live immediately");
    }
    for (g = 0; g < SH_MODSERVE_REQ_COUNT; ++g)
        if (r.restored & (1u << g) && (r.changed & (1u << g)))
            ms_log("%s: requirement released: %s restored to %d (no active bundle needs it)", phase, MS_GATES[g], want[g]);
    InterlockedExchange(&g_gates_live, (LONG)r.live);
    r.not_live = needed & ~r.live;
    if (r.not_live) {
        ms_gate_names(r.not_live, names, sizeof names);
        ms_log("%s: requirements %s are NOT live; the steps of bundles that need them are blocked", phase, names);
    }
    if (out) *out = r;
    if (!needed) return 0;
    return r.not_live ? -1 : 1;
}

int sh_modserve_requirements_check(const sh_modserve_gate_ops *ops, const char *phase, sh_modserve_gates *out)
{
    sh_modserve_gates r;
    int result = sh_modserve_requirements_apply(ops, phase, &r);
    if (result == 1 && !(r.changed & r.needed)) {
        char names[160];
        ms_gate_names(r.live, names, sizeof names);
        ms_log("%s: requirements live: %s (unchanged since the last pass)", phase ? phase : "plan", names);
    }
    if (out) *out = r;
    return result;
}

static const char *ms_unmet_cause(unsigned unmet)
{
    if ((unmet & 3u) == 3u) return "requirements g_useImageBlackList=0, g_useResourceBlackList=0 are not live";
    if (unmet & 2u) return "requirement g_useResourceBlackList=0 is not live";
    return "requirement g_useImageBlackList=0 is not live";
}

int sh_modserve_plan_block_unmet(sh_modserve_plan *plan, const sh_modserve_gates *gates)
{
    const ms_index *x = g_index;
    int i, n = 0;
    if (!plan || !gates || !gates->not_live || !x) return 0;
    for (i = 0; i < plan->count; ++i) {
        sh_modserve_step *s = &plan->steps[i];
        unsigned unmet = x->bundles[x->files[s->file].bundle].req & gates->not_live;
        /* A step registered while its gates were live stays ready. */
        if (unmet && !s->blocked && x->files[s->file].reg_state != 1) {
            s->blocked = ms_unmet_cause(unmet);
            plan->blocked++;
            n++;
        }
    }
    return n;
}

int sh_modserve_plan_run_gated(sh_modserve_plan *plan, const sh_modserve_engine_ops *ops,
                               const sh_modserve_gate_ops *gates, const char *phase, sh_modserve_gates *out)
{
    sh_modserve_gates r;
    if (out) memset(out, 0, sizeof *out);
    if (!plan || !g_index) return -1;
    if (!plan->count) return 0;
    /* Phases 1 and 2 (disarm, then verify after any drain) finish before the
     * plan's first DeclRegisterFile. */
    sh_modserve_requirements_apply(gates, phase, &r);
    sh_modserve_plan_block_unmet(plan, &r);
    if (out) *out = r;
    return sh_modserve_plan_run(plan, ops, phase);
}

/* ------------------------------------------------------------------ queries */

int sh_modserve_bundle_state(const char *bundle_hash, char *cause, size_t cause_cap)
{
    const ms_index *x = g_index;
    int b;
    if (cause && cause_cap) cause[0] = 0;
    if (!x) return 0;
    b = ms_bundle_by_hash(x, bundle_hash, 1);
    if (b >= 0) return 1;
    b = ms_bundle_by_hash(x, bundle_hash, 0);
    if (b < 0) return 0;
    if (cause && cause_cap) strncpy_s(cause, cause_cap, x->bundles[b].cause, _TRUNCATE);
    return -1;
}

int modserve_bundles_for_names(const char *const *names, int n_names,
                               char (*out_hashes)[SH_MODSERVE_HASH_HEX + 1], int cap)
{
    const ms_index *x = g_index;
    unsigned char *seen_file, *seen_bundle;
    int *queue, nq = 0, qh = 0, i, k, found = 0;
    if (!x || !names || n_names <= 0) return 0;
    seen_file = (unsigned char *)calloc((size_t)x->nfiles + 1, 1);
    seen_bundle = (unsigned char *)calloc((size_t)x->nbundles + 1, 1);
    queue = (int *)malloc(sizeof(int) * ((size_t)x->nfiles + 1));
    if (!seen_file || !seen_bundle || !queue) { free(seen_file); free(seen_bundle); free(queue); return -1; }
    for (i = 0; i < n_names; ++i) {
        char lower[MS_NAME_MAX];
        int f;
        if (!names[i] || strlen(names[i]) >= sizeof lower) continue;
        strcpy_s(lower, sizeof lower, names[i]);
        ms_lower(lower);
        f = ms_provider(x, "entitydef", lower);
        /* A placed decl name, or a module instance's moduleName (a snapModule
         * decl is served as a raw file named by its own path). */
        if (f < 0)
            for (k = 0; k < x->nfiles && f < 0; ++k)
                if (x->files[k].served && !strcmp(x->files[k].pname, lower) &&
                    (x->files[k].is_decl || !strcmp(x->files[k].ptype, "snapModule"))) f = k;
        if (f >= 0 && !seen_file[f]) { seen_file[f] = 1; queue[nq++] = f; }
    }
    while (qh < nq) {
        const ms_file *f = &x->files[queue[qh++]];
        if (!seen_bundle[f->bundle]) {
            seen_bundle[f->bundle] = 1;
            if (found < cap && out_hashes) strcpy_s(out_hashes[found], SH_MODSERVE_HASH_HEX + 1, x->bundles[f->bundle].hash);
            found++;
        }
        for (k = 0; k < f->need_count; ++k) {
            int p = x->needs[f->need_first + k].file;
            if (p >= 0 && !seen_file[p]) { seen_file[p] = 1; queue[nq++] = p; }
        }
    }
    free(seen_file); free(seen_bundle); free(queue);
    return found;
}

size_t modserve_bundles_for_names_visit(const char *const *names, size_t count,
                                        sh_modserve_closure_fn visit, void *context)
{
    const ms_index *x = g_index;
    unsigned char *seen_file, *seen_bundle;
    int *queue, nq = 0, qh = 0, k;
    size_t i, found = 0;
    if (!x || !names || !count || !visit) return 0;
    seen_file = (unsigned char *)calloc((size_t)x->nfiles + 1, 1);
    seen_bundle = (unsigned char *)calloc((size_t)x->nbundles + 1, 1);
    queue = (int *)malloc(sizeof(int) * ((size_t)x->nfiles + 1));
    if (!seen_file || !seen_bundle || !queue) { free(seen_file); free(seen_bundle); free(queue); return 0; }
    /* The placed names, each reported with the bundle that provides it. */
    for (i = 0; i < count; ++i) {
        char lower[MS_NAME_MAX];
        int f;
        const ms_bundle *b;
        if (!names[i] || strlen(names[i]) >= sizeof lower) continue;
        strcpy_s(lower, sizeof lower, names[i]);
        ms_lower(lower);
        f = ms_provider(x, "entitydef", lower);
        /* A placed decl name, or a module instance's moduleName (a snapModule
         * decl is served as a raw file named by its own path). */
        if (f < 0)
            for (k = 0; k < x->nfiles && f < 0; ++k)
                if (x->files[k].served && !strcmp(x->files[k].pname, lower) &&
                    (x->files[k].is_decl || !strcmp(x->files[k].ptype, "snapModule"))) f = k;
        if (f < 0) continue;
        b = &x->bundles[x->files[f].bundle];
        visit(context, b->id, b->version, b->hash, (unsigned long long)b->total, names[i]);
        if (!seen_bundle[x->files[f].bundle]) { seen_bundle[x->files[f].bundle] = 1; found++; }
        if (!seen_file[f]) { seen_file[f] = 1; queue[nq++] = f; }
    }
    /* Bundles the closure of their needs reaches, reported without a name. */
    while (qh < nq) {
        const ms_file *f = &x->files[queue[qh++]];
        if (!seen_bundle[f->bundle]) {
            const ms_bundle *b = &x->bundles[f->bundle];
            seen_bundle[f->bundle] = 1;
            found++;
            visit(context, b->id, b->version, b->hash, (unsigned long long)b->total, NULL);
        }
        for (k = 0; k < f->need_count; ++k) {
            int p = x->needs[f->need_first + k].file;
            if (p >= 0 && !seen_file[p]) { seen_file[p] = 1; queue[nq++] = p; }
        }
    }
    free(seen_file); free(seen_bundle); free(queue);
    return found;
}

void sh_modserve_get_stats(sh_modserve_stats *out)
{

    const ms_index *x = g_index;
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (x) {
        out->bundles_seen = x->nbundles;
        out->bundles_admitted = x->admitted;
        out->files = x->served_files;
        out->keys = x->nkeys;
        out->conflicts = x->conflicts;
        out->conflicts_composed = x->composed;
        out->shadows = x->shadows;
        out->build_ms = x->build_ms;
    }
    out->opens_served = (unsigned long)g_opens_served;
    out->open_failures = (unsigned long)g_open_failures;
    out->probe_claims = (unsigned long)g_probe_claims;
    out->cache_hits = (unsigned long)g_cache_hits;
    out->editor_rows_registered = (int)g_modserve_editor_ok;
    out->editor_rows_failed = (int)g_modserve_editor_failed;
}

int sh_modserve_conflict_info(const char *request, sh_modserve_conflict *out)
{
    const ms_index *x = g_index;
    char key[MS_NAME_MAX];
    const char *k = key;
    const ms_conflict *c;
    const ms_file *f;
    if (out) memset(out, 0, sizeof *out);
    if (!x || !request || !x->nconflict_list || strlen(request) >= sizeof key) return 0;
    strcpy_s(key, sizeof key, request);
    ms_lower(key);
    c = (const ms_conflict *)bsearch(&k, x->conflict_list, (size_t)x->nconflict_list, sizeof *c, ms_conflict_cmp);
    if (!c) return 0;
    if (out) {
        f = &x->files[c->winner];
        out->request = c->request;
        out->providers = c->providers ? c->providers : "";
        out->reason = c->reason ? c->reason : "";
        out->served_id = x->bundles[f->bundle].id;
        out->served_version = x->bundles[f->bundle].version;
        out->served_hash = x->bundles[f->bundle].hash;
        out->count = c->count;
        out->composed = c->composed;
        out->withdrawn = f->withdrawn != 0;
    }
    return 1;
}

void sh_modserve_each_bundle(sh_modserve_bundle_fn fn, void *context)
{
    const ms_index *x = g_index;
    int i;
    if (!x || !fn) return;
    for (i = 0; i < x->nbundles; ++i) {
        const ms_bundle *b = &x->bundles[i];
        fn(context, b->id ? b->id : b->folder, b->version ? b->version : "", b->hash ? b->hash : "",
           b->admitted, b->cause, b->file_count, b->legacy);
    }
}

/* Installed mods before bundles a map delivered, then id, then version. */
static const ms_index *g_extra_order_index;
static int ms_extra_order(const void *a, const void *b)
{
    const ms_bundle *x = &g_extra_order_index->bundles[*(const int *)a];
    const ms_bundle *y = &g_extra_order_index->bundles[*(const int *)b];
    int c;
    if (x->legacy != y->legacy) return x->legacy ? 1 : -1;
    c = _stricmp(x->id ? x->id : "", y->id ? y->id : "");
    if (!c) c = strcmp(x->version ? x->version : "", y->version ? y->version : "");
    if (!c) c = strcmp(x->hash ? x->hash : "", y->hash ? y->hash : "");
    return c;
}

int sh_modserve_each_bundle_extra(sh_modserve_extra_fn fn, void *context)
{
    static SRWLOCK order_lock = SRWLOCK_INIT;
    const ms_index *x = g_index;
    int *order, n = 0, i;
    if (!x || !fn) return 0;
    order = (int *)malloc(sizeof(int) * (size_t)(x->nbundles ? x->nbundles : 1));
    if (!order) return -1;
    for (i = 0; i < x->nbundles; ++i)
        if (x->bundles[i].admitted && x->bundles[i].extra) order[n++] = i;
    /* qsort has no context argument; the lock keeps the comparator's index
     * pointer to one caller at a time. */
    AcquireSRWLockExclusive(&order_lock);
    g_extra_order_index = x;
    if (n > 1) qsort(order, (size_t)n, sizeof(int), ms_extra_order);
    g_extra_order_index = NULL;
    ReleaseSRWLockExclusive(&order_lock);
    for (i = 0; i < n; ++i) {
        const ms_bundle *b = &x->bundles[order[i]];
        fn(context, b->id ? b->id : b->folder, b->version ? b->version : "", b->legacy, b->extra);
    }
    free(order);
    return n;
}

void sh_modserve_each_served(sh_modserve_file_fn fn, void *context)
{
    const ms_index *x = g_index;
    int i;
    if (!x || !fn) return;
    for (i = 0; i < x->nfiles; ++i) {
        const ms_file *f = &x->files[i];
        const ms_bundle *b = &x->bundles[f->bundle];
        if (!f->served || f->withdrawn || !b->admitted) continue;
        fn(context, f->request, f->size, b->id ? b->id : b->folder, b->version ? b->version : "", b->hash ? b->hash : "");
    }
}

/* Early decls: the provider is published seconds into the process, after
 * the signature scan, while the engine is already parsing its boot decls on
 * the main thread. A shadow it parsed before then holds the game's bytes
 * until something re-parses it; which shadows, and how many, depends on the
 * race (the boss packs' aiFSMManager states were served in 3 of 18 boots).
 * Offered: winners of admitted bundles that shadow a stock decl, never
 * opened through the provider, never tried, not withdrawn. Restored stock
 * decls are the game's own bytes and are not offered. */
/* An earlier table served these bytes or re-parsed them: an open on the
 * provider thread can land in the table a rescan is replacing, after the
 * rescan copied the marks. */
static int ms_settled_before(const ms_index *x, const ms_file *f);

/* The bundle's requirement gates are all live now. */
static int ms_gates_live_for(const ms_index *x, const ms_file *f)
{
    unsigned req = x->bundles[f->bundle].req;
    return req && !(req & ~(unsigned)InterlockedCompareExchange(&g_gates_live, 0, 0));
}

/* What the engine holds for this file may not be the bundle's: never opened
 * through the provider (and not settled by an earlier table), or first opened
 * before the bundle's requirement gates were live and live now. */
static int ms_stale(const ms_index *x, const ms_file *f)
{
    if (!InterlockedCompareExchange((volatile LONG *)&f->opens, 0, 0)) return !ms_settled_before(x, f);
    return InterlockedCompareExchange((volatile LONG *)&f->opened_ungated, 0, 0) && ms_gates_live_for(x, f);
}

static int ms_settled_before(const ms_index *x, const ms_file *f)
{
    const ms_index *p;
    for (p = x->prev; p; p = p->prev) {
        int o = p->nkeys ? ms_find(p->slots, p->mask, f->request) : -1;
        if (o >= 0 && ms_same_bytes(f, &p->files[o]) &&
            (InterlockedCompareExchange((volatile LONG *)&p->files[o].opens, 0, 0) ||
             InterlockedCompareExchange((volatile LONG *)&p->files[o].reparse_tried, 0, 0)))
            return 1;
    }
    return 0;
}

int sh_modserve_each_unopened_shadow(sh_modserve_unopened_fn fn, void *context)
{
    const ms_index *x = g_index;
    int rank, i, visited = 0;
    if (!x || !fn) return 0;
    for (rank = 0; rank <= 3; ++rank)
        for (i = 0; i < x->nfiles; ++i) {
            ms_file *f = (ms_file *)&x->files[i];
            if (f->rank != rank || !f->is_decl || !f->is_shadow || f->is_restore || !f->served || f->withdrawn ||
                !x->bundles[f->bundle].admitted || !f->ptype || !f->pname)
                continue;
            if (InterlockedCompareExchange(&f->reparse_tried, 0, 0) || !ms_stale(x, f)) continue;
            visited++;
            if (fn(context, f->ptype, f->pname, f->request, x->bundles[f->bundle].id) == 1)
                InterlockedExchange(&f->reparse_tried, 1);
        }
    return visited;
}

/* Raw resources first served before their bundle's gates were live: the
 * models and images whose load resolved names against the blacklists. */
int sh_modserve_each_ungated_raw(sh_modserve_unopened_fn fn, void *context)
{
    const ms_index *x = g_index;
    int i, visited = 0;
    if (!x || !fn) return 0;
    for (i = 0; i < x->nfiles; ++i) {
        ms_file *f = (ms_file *)&x->files[i];
        if (f->is_decl || !f->served || f->withdrawn || !x->bundles[f->bundle].admitted || !f->ptype || !f->pname ||
            (_stricmp(f->ptype, "baseModel") && _stricmp(f->ptype, "image")))
            continue;
        if (InterlockedCompareExchange(&f->reparse_tried, 0, 0) || !InterlockedCompareExchange(&f->opened_ungated, 0, 0) ||
            !ms_gates_live_for(x, f))
            continue;
        visited++;
        if (fn(context, f->ptype, f->pname, f->request, x->bundles[f->bundle].id) == 1)
            InterlockedExchange(&f->reparse_tried, 1);
    }
    return visited;
}

/* ------------------------------------------------------------------ refresh */

/* The parent a decl's text names at its top level (`inherit = "name"`), else
 * 0. Comments and strings are skipped; only depth 1 of the outer block. */
static int ms_top_inherit(const unsigned char *t, size_t n, char *out, size_t cap)
{
    size_t i = 0;
    int depth = 0;
    while (i < n) {
        unsigned char c = t[i];
        if (c == '/' && i + 1 < n && t[i + 1] == '/') { while (i < n && t[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < n && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(t[i] == '*' && t[i + 1] == '/')) i++;
            i += 2;
            continue;
        }
        if (c == '"') { i++; while (i < n && t[i] != '"') { if (t[i] == '\\') i++; i++; } i++; continue; }
        if (c == '{') { depth++; i++; continue; }
        if (c == '}') { if (--depth <= 0 && i > 0) return 0; i++; continue; }
        if (depth == 1 && (c == 'i' || c == 'I') && i + 7 <= n && !_strnicmp((const char *)t + i, "inherit", 7) &&
            (i == 0 || !(isalnum(t[i - 1]) || t[i - 1] == '_')) && (i + 7 == n || !(isalnum(t[i + 7]) || t[i + 7] == '_'))) {
            size_t j = i + 7, k = 0;
            while (j < n && (t[j] == ' ' || t[j] == '\t' || t[j] == '\r' || t[j] == '\n' || t[j] == '=')) j++;
            if (j >= n || t[j] != '"') return 0;
            for (++j; j < n && t[j] != '"' && k + 1 < cap; ++j) out[k++] = (char)t[j];
            out[k] = 0;
            return k > 0;
        }
        i++;
    }
    return 0;
}

/* Mark every served decl that inherits, at any depth, from a decl whose
 * served bytes changed or that is gone: the engine resolves a decl's parent
 * when the child is parsed, so a parent re-parsed alone leaves its parsed
 * children with the old values. Served decls of the parents' types are read
 * once (a stock pointer from the installed game); a stock decl no bundle
 * serves cannot be found here and keeps its parse until a map reload purges
 * it or DOOM restarts. Main thread, once per table; returns the decls marked. */
int sh_modserve_refresh_expand(void)
{
    ms_index *x = (ms_index *)g_index;
    char (*types)[64] = NULL;
    char **parent = NULL;
    int ntypes = 0, i, gen, marked = 0;
    if (!x || InterlockedExchange(&x->expanded, 1)) return 0;
    types = (char (*)[64])calloc(64, sizeof *types);
    if (!types) return 0;
    /* The types and names of the changed and gone decls (generation 0). */
    for (i = 0; i < x->nfiles + x->ngone; ++i) {
        const ms_file *f = i < x->nfiles ? &x->files[i] : &x->gone[i - x->nfiles].from->files[x->gone[i - x->nfiles].file];
        int t, seen = 0;
        if (i < x->nfiles ? (!f->refresh || !f->is_decl) : !x->gone[i - x->nfiles].pending) continue;
        if (!f->ptype || !f->pname) continue;
        for (t = 0; t < ntypes; ++t) if (!_stricmp(types[t], f->ptype)) seen = 1;
        if (!seen && ntypes < 64) strncpy_s(types[ntypes++], 64, f->ptype, _TRUNCATE);
    }
    if (!ntypes) { free(types); return 0; }
    /* Each served decl of those types: its top-level parent. */
    parent = (char **)calloc((size_t)x->nfiles, sizeof *parent);
    if (!parent) { free(types); return 0; }
    for (i = 0; i < x->nfiles; ++i) {
        ms_file *f = &x->files[i];
        unsigned char *b;
        size_t len = 0;
        char name[MS_NAME_MAX];
        int t, ok = 0;
        if (!f->is_decl || !f->served || f->withdrawn || !f->ptype || !f->pname || !x->bundles[f->bundle].admitted) continue;
        for (t = 0; t < ntypes && !ok; ++t) ok = !_stricmp(types[t], f->ptype);
        if (!ok) continue;
        b = ms_file_bytes(f, &len);
        if (!b) continue;
        if (ms_top_inherit(b, len, name, sizeof name)) parent[i] = ms_strdup(x, name);
        HeapFree(GetProcessHeap(), 0, b);
    }
    /* Generations: a child of generation N-1 is marked N, up to 8 levels. */
    for (gen = 1; gen <= 8; ++gen) {
        int added = 0;
        for (i = 0; i < x->nfiles; ++i) {
            ms_file *f = &x->files[i];
            int j;
            if (!parent[i] || f->refresh) continue;
            for (j = 0; j < x->nfiles + x->ngone; ++j) {
                const ms_file *p = j < x->nfiles ? &x->files[j] : &x->gone[j - x->nfiles].from->files[x->gone[j - x->nfiles].file];
                if (j < x->nfiles) {
                    if (!p->refresh || !p->is_decl || (int)p->refresh_gen != gen - 1) continue;
                } else if (gen != 1 || !x->gone[j - x->nfiles].pending) continue;
                if (!p->ptype || !p->pname || _stricmp(p->ptype, f->ptype) || _stricmp(p->pname, parent[i])) continue;
                InterlockedExchange(&f->refresh_gen, gen);
                InterlockedExchange(&f->refresh, 1);
                x->diff.dependents++;
                added++;
                marked++;
                break;
            }
        }
        if (!added) break;
    }
    free(parent);
    free(types);
    return marked;
}

static int ms_gates_ok(const ms_index *x, const ms_file *f)
{
    return !x->bundles[f->bundle].req || ms_gates_live_for(x, f);
}

static int ms_refresh_one(const ms_index *x, ms_file *f, int kind, sh_modserve_refresh_fn fn, void *context)
{
    sh_modserve_refresh_item it;
    it.type = f->ptype; it.name = f->pname; it.request = f->request; it.bundle_id = x->bundles[f->bundle].id;
    it.kind = kind; it.gone = 0; it.shadow = f->is_shadow; it.gates_live = ms_gates_ok(x, f);
    if (fn(context, &it) == 1) {
        InterlockedExchange(&f->refresh, 0);
        /* What the engine holds is settled: not an early decl. */
        if (kind == SH_MODSERVE_RF_DECL) InterlockedExchange(&f->reparse_tried, 1);
    }
    return 1;
}

int sh_modserve_each_refresh(sh_modserve_refresh_fn fn, void *context)
{
    const ms_index *x = g_index;
    int pass, i, visited = 0;
    if (!x || !fn) return 0;
    /* Changed decls in plan rank order, then gone shadows, then the decls
     * that inherit from them by generation and rank, then models and images. */
    for (pass = 0; pass <= 6; ++pass) {
        if (pass == 4) {
            for (i = 0; i < x->ngone; ++i) {
                ms_gone *g = (ms_gone *)&x->gone[i];
                const ms_file *of = &g->from->files[g->file];
                sh_modserve_refresh_item it;
                if (!InterlockedCompareExchange(&g->pending, 0, 0)) continue;
                it.type = of->ptype; it.name = of->pname; it.request = of->request;
                it.bundle_id = g->from->bundles[of->bundle].id;
                it.kind = SH_MODSERVE_RF_DECL; it.gone = 1; it.shadow = of->is_shadow; it.gates_live = 1;
                visited++;
                if (fn(context, &it) == 1) InterlockedExchange(&g->pending, 0);
            }
            continue;
        }
        if (pass == 5) {
            int gen, rank;
            for (gen = 1; gen <= 8; ++gen)
                for (rank = 0; rank <= 3; ++rank)
                    for (i = 0; i < x->nfiles; ++i) {
                        ms_file *f = (ms_file *)&x->files[i];
                        if (!InterlockedCompareExchange(&f->refresh, 0, 0) || !f->served || f->withdrawn ||
                            !x->bundles[f->bundle].admitted || !f->ptype || !f->pname || !f->is_decl ||
                            f->refresh_gen != gen || f->rank != rank)
                            continue;
                        visited += ms_refresh_one(x, f, SH_MODSERVE_RF_DECL, fn, context);
                    }
            continue;
        }
        for (i = 0; i < x->nfiles; ++i) {
            ms_file *f = (ms_file *)&x->files[i];
            int kind = ms_kind(f);
            if (!InterlockedCompareExchange(&f->refresh, 0, 0) || !f->served || f->withdrawn ||
                !x->bundles[f->bundle].admitted || !f->ptype || !f->pname)
                continue;
            if (pass < 4 ? (kind != SH_MODSERVE_RF_DECL || f->rank != pass || f->refresh_gen) : kind == SH_MODSERVE_RF_DECL)
                continue;
            visited += ms_refresh_one(x, f, kind, fn, context);
        }
    }
    return visited;
}

int sh_modserve_plan_refresh(sh_modserve_plan *out)
{
    const ms_index *x = g_index;
    unsigned char *mark;
    int i, r, any = 0;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!x) return 0;
    mark = (unsigned char *)calloc((size_t)x->nfiles + 1, 1);
    if (!mark) return -1;
    for (i = 0; i < x->nfiles; ++i) {
        ms_file *f = (ms_file *)&x->files[i];
        if (!InterlockedExchange(&f->refresh_new, 0)) continue;
        if (f->served && !f->withdrawn && x->bundles[f->bundle].admitted && ms_plannable(x, f) && !f->is_sedef) {
            mark[i] = 1;
            any = 1;
        }
    }
    r = any ? ms_plan_from(x, mark, 0, out) : 0;
    free(mark);
    return r;
}

void sh_modserve_last_diff(sh_modserve_diff *out)
{
    const ms_index *x = g_index;
    int i;
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!x) return;
    *out = x->diff;
    out->pending = 0;
    for (i = 0; i < x->nfiles; ++i)
        if (InterlockedCompareExchange((volatile LONG *)&x->files[i].refresh, 0, 0) && x->files[i].served &&
            !x->files[i].withdrawn)
            out->pending++;
    for (i = 0; i < x->ngone; ++i)
        if (InterlockedCompareExchange((volatile LONG *)&x->gone[i].pending, 0, 0)) out->pending++;
}

long sh_modserve_open_count(const char *request)
{
    const ms_file *f = ms_lookup_file(NULL, request);
    return f ? InterlockedCompareExchange((volatile LONG *)&f->opens, 0, 0) : 0;
}

int sh_modserve_is_mod_sedef(const char *name)
{
    const ms_index *x = g_index;
    char lower[MS_NAME_MAX];
    if (!x || !name || strlen(name) >= sizeof lower) return 0;
    strcpy_s(lower, sizeof lower, name);
    ms_lower(lower);
    return ms_provider(x, "snapeditorentitydef", lower) >= 0;
}
