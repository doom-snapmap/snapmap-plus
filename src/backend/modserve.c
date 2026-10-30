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
    int need_first, need_count;
    int served;
    const unsigned char *body;            /* composed bytes served instead of the disk file (arena), or NULL */
    size_t body_len;
    volatile LONG opens, fail_logged, shadow_logged;
    volatile LONG reg_state;              /* 0 none, 1 ready, -1 failed (main thread) */
    volatile LONG withdrawn;              /* a shadow taken back by a registration pass: lookups miss it */
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
    int legacy, admitted, first_file, file_count;
    int grandfathered;                    /* under legacy\ or bundle.json extra.legacy: shadows may be new names */
    uint64_t total;                       /* sum of file sizes (the manifest row's size) */
    unsigned req;                         /* SH_MODSERVE_REQ_* bits from bundle.json requirements */
    double installed_at;
    char cause[MS_CAUSE_MAX];
} ms_bundle;

typedef struct ms_slot { uint64_t h; const char *key; int file; } ms_slot;

typedef struct ms_chunk { struct ms_chunk *next; size_t used, cap; char data[1]; } ms_chunk;

typedef struct ms_index {
    char root[MAX_PATH];
    ms_chunk *arena;
    ms_bundle *bundles; int nbundles, cbundles;
    ms_file *files; int nfiles, cfiles;
    ms_need *needs; int nneeds, cneeds;
    ms_slot *slots; uint32_t mask; int nkeys;
    ms_slot *pslots; uint32_t pmask;
    ms_conflict *conflict_list; int nconflict_list, cconflict_list;   /* sorted by request */
    int admitted, served_files, conflicts, composed, shadows;
    double build_ms;
} ms_index;

static ms_index *volatile g_index;
static int ms_bundle_by_hash(const ms_index *x, const char *hash, int admitted_only);
/* Gates this layer changed and the values it displaced: a gate is restored
 * only when no admitted bundle needs it any more and nobody else has changed
 * it since. Main thread only, like every registration pass. */
static unsigned g_gates_owned;
static int g_gates_original[SH_MODSERVE_REQ_COUNT];
static SRWLOCK g_build_lock = SRWLOCK_INIT;
static char g_last_root[MAX_PATH];
static volatile LONG g_opens_served, g_open_failures, g_probe_claims, g_cache_hits;
volatile LONG g_modserve_editor_ok, g_modserve_editor_failed;   /* written by modserve_engine.c */

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

static void ms_parse_file(ms_index *x, ms_bundle *b, ms_json *c, char **shadows, int nshadows)
{
    char key[64];
    const char *v, *ve;
    char *path = NULL, *blob = NULL, *request = NULL, *ptype = NULL, *pname = NULL;
    double size = -1;
    int need_first = x->nneeds, need_count = 0;
    ms_file *f;
    if (!ms_jenter(c, '{')) { ms_fail(b, "a files[] entry is not an object"); return; }
    while (ms_jnext(c, '}')) {
        if (!ms_jmember(c, key, sizeof key, &v, &ve)) { ms_fail(b, "malformed files[] entry"); return; }
        if (!strcmp(key, "path")) path = ms_jstr(x, v, ve, 511);
        else if (!strcmp(key, "blob")) blob = ms_jstr(x, v, ve, 64);
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
    if (!path || !ms_valid_rel(path)) { ms_fail(b, "files[] path '%s' is not a plain assets/ path", path ? path : "?"); return; }
    if (size < 0 || size > (double)SH_MODSERVE_MAX_FILE || size != (double)(uint64_t)size) {
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
    f->size = (uint64_t)size;
    f->need_first = need_first;
    f->need_count = need_count;
    /* shadows[] is sorted (ms_parse_bundle). */
    f->is_shadow = nshadows > 0 && bsearch(&request, shadows, (size_t)nshadows, sizeof(char *), ms_str_cmp) != NULL;
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
    if (!b->legacy && !f->is_shadow) {
        char ns[96], ns_s[96], sid[80];
        int inside;
        _snprintf_s(ns, sizeof ns, _TRUNCATE, "mods/%s/", b->id);
        ms_sanitize(b->id, sid, sizeof sid);
        _snprintf_s(ns_s, sizeof ns_s, _TRUNCATE, "mods/%s/", sid);
        if (f->is_decl && _stricmp(ptype, "skins")) inside = !strncmp(f->pname, ns, strlen(ns));
        else {
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

static void ms_read_state(ms_index *x, ms_bundle *b)
{
    char path[MAX_PATH], key[64];
    unsigned char *text;
    size_t len = 0;
    sh_json_kind kind;
    ms_json c;
    const char *v, *ve;
    int validated = 1;
    char *cause = NULL;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.state.json", b->dir);
    text = ms_read_whole(path, 65536, &len);
    if (!text) return;   /* no state: the builder's manifest is trusted */
    if (!sh_json_validate((const char *)text, len, MS_JSON_DEPTH, &kind) || kind != SH_JSON_OBJECT) {
        ms_fail(b, ".state.json is not a JSON object");
        HeapFree(GetProcessHeap(), 0, text);
        return;
    }
    c.p = (const char *)text;
    c.e = c.p + len;
    if (ms_jenter(&c, '{')) while (ms_jnext(&c, '}')) {
        if (!ms_jmember(&c, key, sizeof key, &v, &ve)) break;
        if (!strcmp(key, "validated")) validated = !(ve - v == 5 && !memcmp(v, "false", 5));
        else if (!strcmp(key, "firstCause")) cause = ms_jstr(x, v, ve, 160);
        else if (!strcmp(key, "installedAt")) {
            double t;
            if (ms_jnum(v, ve, &t)) b->installed_at = t;
        }
    }
    if (!validated) ms_fail(b, "not validated: %s", cause && *cause ? cause : "(no cause recorded)");
    HeapFree(GetProcessHeap(), 0, text);
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

/* Parse <dir>\bundle.json into the index; leaves b->cause set on failure. */
static void ms_parse_bundle(ms_index *x, ms_bundle *b, const char *blobs_dir)
{
    char path[MAX_PATH], key[64];
    unsigned char *text;
    size_t len = 0;
    sh_json_kind kind;
    sh_json_error err = { 0, NULL };
    ms_json c;
    const char *v, *ve, *files_v = NULL, *files_e = NULL, *req_v = NULL, *req_e = NULL;
    /* A converted first-generation package lists every name it carries, so
     * shadows run up to the file cap. */
    char **shadows = NULL;
    int nshadows = 0, cshadows = 0, i;
    double format = -1;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\bundle.json", b->dir);
    text = ms_read_whole(path, SH_MODSERVE_MAX_JSON, &len);
    if (!text) {
        ms_fail(b, ms_file_size(path) < 0 ? "no bundle.json (the folder is not built)" : "bundle.json unreadable or over 1 MiB");
        return;
    }
    if (!sh_json_validate_ex((const char *)text, len, MS_JSON_DEPTH, &kind, &err) || kind != SH_JSON_OBJECT) {
        ms_fail(b, "bundle.json is not a valid JSON object (%s at byte %zu)", err.reason ? err.reason : "not an object",
                err.offset);
        HeapFree(GetProcessHeap(), 0, text);
        return;
    }
    c.p = (const char *)text;
    c.e = c.p + len;
    ms_jenter(&c, '{');
    while (ms_jnext(&c, '}')) {
        if (!ms_jmember(&c, key, sizeof key, &v, &ve)) break;
        if (!strcmp(key, "format")) { if (!ms_jnum(v, ve, &format)) format = -1; }
        else if (!strcmp(key, "id")) b->id = ms_jstr(x, v, ve, 64);
        else if (!strcmp(key, "version")) b->version = ms_jstr(x, v, ve, 32);
        else if (!strcmp(key, "name")) b->name = ms_jstr(x, v, ve, 80);
        else if (!strcmp(key, "bundle")) b->hash = ms_jstr(x, v, ve, 64);
        else if (!strcmp(key, "files")) { files_v = v; files_e = ve; }
        else if (!strcmp(key, "requirements")) { req_v = v; req_e = ve; }
        else if (!strcmp(key, "extra")) {
            /* The builder preserves descriptor keys it does not own under
             * "extra"; a "legacy" object there marks a grandfathered bundle. */
            ms_json o = { v, ve };
            if (ms_jenter(&o, '{')) while (ms_jnext(&o, '}')) {
                const char *ev, *eve;
                char ek[64];
                if (!ms_jmember(&o, ek, sizeof ek, &ev, &eve)) break;
                if (!strcmp(ek, "legacy") && ev < eve && *ev == '{') b->grandfathered = 1;
            }
        }
        else if (!strcmp(key, "shadows")) {
            ms_json a = { v, ve };
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
    }
    if (format != 1) ms_fail(b, "bundle format %g is not supported (this build reads format 1)", format);
    if (!b->id || !ms_valid_id(b->id)) ms_fail(b, "bundle id '%s' is not valid", b->id ? b->id : "");
    if (!b->version) b->version = "";
    if (!b->hash || !ms_valid_hash((char *)b->hash)) ms_fail(b, "bundle.json has no bundle hash");
    if (b->legacy) b->grandfathered = 1;
    if (!files_v) ms_fail(b, "bundle.json has no files");
    if (req_v && !b->cause[0]) ms_parse_requirements(b, req_v, req_e);
    if (nshadows > 1) qsort(shadows, (size_t)nshadows, sizeof(char *), ms_str_cmp);
    if (!b->cause[0]) {
        ms_json a = { files_v, files_e };
        b->first_file = x->nfiles;
        if (ms_jenter(&a, '[')) while (!b->cause[0] && ms_jnext(&a, ']')) {
            ms_json one = { a.p, ms_jskip(a.p, a.e) };
            a.p = one.e;
            if (x->nfiles - b->first_file >= (int)SH_MODSERVE_MAX_FILES) { ms_fail(b, "more than %u files", SH_MODSERVE_MAX_FILES); break; }
            ms_parse_file(x, b, &one, shadows, nshadows);
        }
        b->file_count = x->nfiles - b->first_file;
        for (i = b->first_file; i < b->first_file + b->file_count; ++i) b->total += x->files[i].size;
    }
    free(shadows);
    HeapFree(GetProcessHeap(), 0, text);
    if (!b->cause[0]) ms_read_state(x, b);
    /* Sizes only; hashes were verified at install. A file missing from the
     * tree is taken from the blob store when the blob has the right size. */
    for (i = b->first_file; !b->cause[0] && i < b->first_file + b->file_count; ++i) {
        ms_file *f = &x->files[i];
        long long got = ms_file_size(f->disk);
        if (got == (long long)f->size) continue;
        if (f->blob && blobs_dir[0]) {
            char alt[MS_PATH_MAX];
            _snprintf_s(alt, sizeof alt, _TRUNCATE, "%s\\%s", blobs_dir, f->blob);
            if (ms_file_size(alt) == (long long)f->size) { f->disk = ms_strdup(x, alt); if (f->disk) continue; }
        }
        if (got < 0) ms_fail(b, "'%s' is missing", f->request);
        else ms_fail(b, "size mismatch: '%s' is %lld bytes, bundle.json says %llu", f->request, got,
                     (unsigned long long)f->size);
    }
}

static void ms_scan_folder(ms_index *x, const char *dir, int legacy, const char *blobs_dir)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        ms_bundle *b;
        char sub[MAX_PATH];
        int mark_files = x->nfiles, mark_needs = x->nneeds;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            continue;
        if (fd.cFileName[0] == '.') continue;
        if (!legacy && (!_stricmp(fd.cFileName, "blobs") || !_stricmp(fd.cFileName, "legacy"))) continue;
        if (!ms_grow((void **)&x->bundles, &x->cbundles, x->nbundles + 1, sizeof(ms_bundle))) break;
        b = &x->bundles[x->nbundles++];
        memset(b, 0, sizeof *b);
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        b->dir = ms_strdup(x, sub);
        b->folder = ms_strdup(x, fd.cFileName);
        b->legacy = legacy;
        b->first_file = x->nfiles;
        if (!b->dir || !b->folder) { ms_fail(b, "out of memory"); }
        else ms_parse_bundle(x, b, blobs_dir);
        if (b->cause[0]) {
            /* Failure isolation: drop this bundle's rows, keep everything else. */
            x->nfiles = mark_files;
            x->nneeds = mark_needs;
            b->file_count = 0;
            ms_log("bundle '%s%s' skipped: %s", legacy ? "legacy\\" : "", fd.cFileName, b->cause);
        } else {
            b->admitted = 1;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
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
                    owned[nt] = ms_read_whole(f->disk, SH_MODSERVE_CACHE_FILE, &lens[nt]);
                    if (!owned[nt] || lens[nt] != f->size) { ok = 0; break; }
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

int sh_modserve_index_build(const char *mods_root)
{
    ms_index *x, *old;
    char blobs[MAX_PATH], legacy[MAX_PATH];
    double t0 = ms_now_ms();
    int i;
    if (!mods_root) return -1;
    AcquireSRWLockExclusive(&g_build_lock);
    x = (ms_index *)calloc(1, sizeof *x);
    if (!x) { ReleaseSRWLockExclusive(&g_build_lock); ms_log("index refused: out of memory"); return -1; }
    strncpy_s(x->root, sizeof x->root, mods_root, _TRUNCATE);
    strncpy_s(g_last_root, sizeof g_last_root, mods_root, _TRUNCATE);
    _snprintf_s(blobs, sizeof blobs, _TRUNCATE, "%s\\blobs", mods_root);
    _snprintf_s(legacy, sizeof legacy, _TRUNCATE, "%s\\legacy", mods_root);
    ms_scan_folder(x, mods_root, 0, blobs);
    ms_scan_folder(x, legacy, 1, blobs);
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
        }
    }
    /* Publish the complete table; the old one is retired, never freed, since
     * a provider open on another thread may be inside it. */
    InterlockedExchangePointer((void *volatile *)&g_index, x);
    ReleaseSRWLockExclusive(&g_build_lock);
    ms_log("index built in %.1f ms (%d bundles, %d files)%s%s", x->build_ms, x->admitted, x->served_files,
           x->nbundles > x->admitted ? "; skipped bundles are listed above" : "",
           x->conflicts ? "; conflicts are listed above" : "");
    return x->admitted;
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
    }
    return 1;
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
        buf = ms_read_whole(f->disk, SH_MODSERVE_MAX_FILE, &len);
        if (buf && (uint64_t)len != f->size) { HeapFree(GetProcessHeap(), 0, buf); buf = NULL; }
        if (!buf) {
            InterlockedIncrement(&g_open_failures);
            if (!InterlockedExchange((volatile LONG *)&f->fail_logged, 1))
                ms_log("'%s' from %s@%s could not be read at its indexed size (%llu bytes) from '%s'; "
                       "the engine's own resolution is used", f->request, x->bundles[f->bundle].id,
                       x->bundles[f->bundle].version, (unsigned long long)f->size, f->disk);
            return -1;
        }
        if (InterlockedCompareExchange(&g_cache_enabled, 0, 0)) ms_cache_put(f, buf, len);
    }
    InterlockedIncrement((volatile LONG *)&f->opens);
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
        s->persist = persist_sedefs && f->is_sedef;
        s->shadow = f->is_shadow;
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
    int i;
    if (!hash) return -1;
    for (i = 0; i < x->nbundles; ++i)
        if (x->bundles[i].hash && !_stricmp(x->bundles[i].hash, hash) && (!admitted_only || x->bundles[i].admitted))
            return i;
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
    b = ms_read_whole(f->disk, SH_MODSERVE_MAX_FILE, len);
    if (b && *len != f->size) { HeapFree(GetProcessHeap(), 0, b); b = NULL; *len = 0; }
    return b;
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
        if (f < 0)
            for (k = 0; k < x->nfiles && f < 0; ++k)
                if (x->files[k].served && x->files[k].is_decl && !strcmp(x->files[k].pname, lower)) f = k;
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
        if (f < 0)
            for (k = 0; k < x->nfiles && f < 0; ++k)
                if (x->files[k].served && x->files[k].is_decl && !strcmp(x->files[k].pname, lower)) f = k;
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

int sh_modserve_is_mod_sedef(const char *name)
{
    const ms_index *x = g_index;
    char lower[MS_NAME_MAX];
    if (!x || !name || strlen(name) >= sizeof lower) return 0;
    strcpy_s(lower, sizeof lower, name);
    ms_lower(lower);
    return ms_provider(x, "snapeditorentitydef", lower) >= 0;
}
