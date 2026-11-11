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
#include <ctype.h>
#include "modserve.h"
#include "modmanifest.h"
#include "modbundle.h"
#include "modsalt.h"
#include "modstore.h"
#include "modregistry.h"
#include "decl_tree.h"
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
    volatile LONG plan_seen;              /* a plan run settled it ready (registered, or left to the game) */
    volatile LONG withdrawn;              /* a shadow taken back by a registration pass: lookups miss it */
    volatile LONG reparse_tried;          /* the early-decl pass re-parsed it (main thread) */
    volatile LONG opened_ungated;         /* first served while its bundle's cvar requirements were not live */
    volatile LONG refresh;                /* 1: its bytes changed at a rescan and the engine may hold the old ones */
    volatile LONG refresh_gen;            /* 0 changed; N: inherits (N levels up) from a changed or gone decl */
    volatile LONG refresh_new;            /* a new gameplay decl of a mod this session registered: register it now */
    int editor_closure;                   /* an editor row, or a served file a row needs at any depth (ms_mark_editor_closure) */
    /* Map scope: the other providers of a path several admitted bundles
     * provide, in winner order (x->alts), so the file that serves the path is
     * chosen per map from the bundles the open map declares (ms_scope_compute). */
    int alt_first, alt_count;             /* a winner's alternates */
    int alt_of;                           /* an alternate's winner, or -1 */
    volatile LONG scope_alt;              /* a winner's path: the file serving it for the listing, or -1 (the game's) */
    volatile LONG scope_last;             /* a winner's path: the last file that served it under scope, or -1 */
    struct ms_body *volatile scope_body;  /* the picked file: the text composed from the declared providers, or NULL */
    struct ms_body *scope_composed;       /* a winner: the texts composed for earlier listings (memo, build lock) */
    int scope_why;                        /* a winner: why its path is served as it is (MS_WHY_*) */
    /* Registries (modregistry.h, ms_registry_files): a synthetic file per
     * registry path, of the product's pseudo-bundle, never "served" itself;
     * it is reached from the winner of its path, or is the path's key when
     * no bundle carries a file there. */
    int registry;                         /* a registry's synthetic file: 1 + its position among them, else 0 */
    int registry_keyed;                   /* that file is the key of its path */
    int registry_of;                      /* a winner at a registry's path: 1 + that registry's file, else 0 */
    /* A decl the bundle carries at a game name with other bytes: an edited
     * copy whose derived edits the product block records ("edits"). Never
     * served, keyed, planned or refreshed; the game file is generated. */
    int edit;
} ms_file;

/* A text composed for one listing (arena; memoized per winner by the set of declared providers). */
typedef struct ms_body {
    const unsigned char *p;
    size_t len;
    uint64_t set;                         /* which of the winner's candidates (bit 0 the winner) it was composed from */
    uint64_t order;                       /* a hash of their order (the pick first, then the map's rows) */
    int added;
    struct ms_body *next;
} ms_body;

/* One path several bundles provide with different bytes, and what serves it. */
typedef struct ms_conflict {
    const char *request;
    const char *providers;                /* "a@1, b@2" in index order */
    const char *reason;                   /* why the winner was chosen, or how the text was composed */
    int winner;                           /* the file that serves the path */
    int count;                            /* providers */
    int composed;                         /* 1: the list entries of every provider were merged */
} ms_conflict;

typedef struct ms_req_cvar { const char *name, *value; } ms_req_cvar;   /* canonical value text */

/* One mod folder. Its identity is its salt (`id`, the builder block's
 * "salt", ONE-SHAPE rule 1); where it sits is its area: the root of the
 * store (place NULL) or a map's place, mods\maps\<segment> (place
 * "maps\<segment>"). The index holds one build per salt per area. */
typedef struct ms_bundle {
    const char *dir, *folder, *id, *hash, *name;
    const char *extra;                    /* the author block's other keys as JSON, or NULL */
    const char *place;                    /* "maps\<segment>" for a folder in a map's place; NULL at the root */
    int admitted, first_file, file_count;
    uint64_t total;                       /* sum of file sizes (the manifest row's size) */
    const char *req_text;                 /* canonical requirements (sh_mb_requirements_canonical), or NULL */
    ms_req_cvar *req_cvars; int req_ncvars;
    const char **req_act; int req_nact;   /* console lines run when its requirements go live */
    const char **req_deact; int req_ndeact;   /* and when they stop */
    const char *req_lost;                 /* a cvar an earlier install asks another value of, or NULL */
    volatile LONG req_live;               /* every cvar requirement read back as asked (read on any thread) */
    char req_cause[MS_CAUSE_MAX];         /* why they are not live (main thread) */
    double installed_at;
    char cause[MS_CAUSE_MAX];
    /* Map scope (modserve.h). */
    const char *scope_editor;             /* the first editor-time shadow (the editor-tweak class: always), or NULL */
    int has_rows, has_shadows;            /* adds Toybox rows; serves shadow files */
    volatile LONG shadows_off;            /* its shadows are withdrawn: the current map does not declare it */
    volatile LONG req_off;                /* its requirements are not accepted for the current map */
    volatile LONG sv;                     /* MS_SV_*: what it serves for the current listing */
    volatile LONG rank;                   /* its row's position among the bundles the open map declares, or -1 */
    volatile LONG inert;                  /* the open map declares another build of its salt */
    int root_build;                       /* the root folder's admitted build of its salt: it serves with no map open */
    int reg_relevant;                     /* registries: it states intent, carries a registry file or an encounter */
    const char *registries;               /* the product block's "registries" object (JSON), or NULL */
    const char *edits;                    /* the product block's "edits" array (JSON, derived edits), or NULL */
    size_t edits_len;
} ms_bundle;

/* A mod as a line names it: the author's name, else its folder, else its salt. */
static const char *ms_label(const ms_bundle *b)
{
    if (b->name && b->name[0]) return b->name;
    if (b->folder && b->folder[0]) return b->folder;
    return b->id ? b->id : "";
}

#define MS_SV_NEW     1                   /* names under its salt */
#define MS_SV_SHADOW  2                   /* files at game names that differ from the game's */
#define MS_SV_RESTORE 4                   /* surfaces: the game's own bytes under the game's name */
#define MS_SV_EDITOR  8                   /* editor tweaks: editor-only types at game names (DESIGN-additive 2.6) */
#define MS_SV_ALL     (MS_SV_NEW | MS_SV_SHADOW | MS_SV_RESTORE | MS_SV_EDITOR)

typedef struct ms_slot { uint64_t h; const char *key; int file; } ms_slot;

#define MS_PATH_MAX 1024                  /* a UTF-8 store path (ms_wide) */

typedef struct ms_chunk { struct ms_chunk *next; size_t used, cap; char data[1]; } ms_chunk;

/* A request the replaced table served and this one does not (refresh). */
struct ms_index;
typedef struct ms_gone { const struct ms_index *from; int file; volatile LONG pending; } ms_gone;

typedef struct ms_index {
    char root[MS_PATH_MAX];
    ms_chunk *arena;
    ms_bundle *bundles; int nbundles, cbundles;
    ms_file *files; int nfiles, cfiles;
    ms_need *needs; int nneeds, cneeds;
    ms_slot *slots; uint32_t mask; int nkeys;
    ms_slot *pslots; uint32_t pmask;
    ms_conflict *conflict_list; int nconflict_list, cconflict_list;   /* sorted by request */
    int *alts; int nalts, calts;          /* shadow paths' other providers (ms_file.alt_first) */
    const char **editor_only; int neditor_only;                        /* record "editorOnly": bundles a map named that change only the editor */
    int admitted, served_files, conflicts, composed, shadows;
    double build_ms;
    struct ms_index *prev;                /* the table this one replaced (retired, never freed) */
    ms_gone *gone; int ngone;             /* refresh: requests no longer served (pending gone shadows carried) */
    sh_modserve_diff diff;                /* refresh: this table against prev */
    volatile LONG expanded;               /* refresh: the inheritance closure was marked */
    /* Registries: the product's pseudo-bundle at bundles[nbundles] (one past
     * the count, so no bundle walk sees it), its files reg_first.., the
     * game's texts of the fixed registries (read once), and the texts
     * generated per serving set (memo, arena) with the one the listing uses. */
    int reg_bundle, reg_first, reg_count;
    const char **reg_xtext;               /* per registry file: the game's text (read once) */
    size_t *reg_xlen;
    unsigned char *reg_xread;
    struct ms_reg_memo *reg_memo;
    struct ms_reg_memo *volatile reg_cur;
} ms_index;

static ms_index *volatile g_index;
static volatile LONG g_scope_on;      /* map scope (ms_scope_compute) */
static int ms_bundle_by_hash(const ms_index *x, const char *hash, int admitted_only);
static unsigned char *ms_file_raw(const ms_file *f, size_t *len);
static void ms_req_index(ms_index *x, const ms_index *old);
static SRWLOCK g_build_lock = SRWLOCK_INIT;
static char g_last_root[MS_PATH_MAX];
static volatile LONG g_opens_served, g_open_failures, g_probe_claims, g_cache_hits;
static volatile LONG g_renderprog_logged;   /* render-program serve lines written */
volatile LONG g_modserve_editor_ok, g_modserve_editor_failed;   /* written by modserve_engine.c */
#define MS_LISTENERS 4
static sh_modserve_index_fn volatile g_index_listener[MS_LISTENERS];   /* modvt.c, modsound.c, modserve_engine.c, nav_bake.c */

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

/* The lines a player should see (sh_modserve_set_console). */
static sh_modserve_console_fn volatile g_console;
void sh_modserve_set_console(sh_modserve_console_fn fn) { g_console = fn; }
static sh_modserve_build_cause_fn volatile g_build_cause;
void sh_modserve_set_build_cause(sh_modserve_build_cause_fn fn) { g_build_cause = fn; }

static void ms_console(const char *fmt, ...)
{
    char line[600];
    va_list ap;
    sh_modserve_console_fn fn = g_console;
    if (!fn) return;
    memcpy(line, "Snapmap+ mods: ", 15);
    va_start(ap, fmt);
    _vsnprintf_s(line + 15, sizeof line - 15, _TRUNCATE, fmt, ap);
    va_end(ap);
    fn(line);
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
    free(x->editor_only); free(x->gone); free(x->alts);
    free(x);
}

/* ------------------------------------------------------------------ names */

/* A surface: a stock pointer at the game's own path (the game's bytes
 * under the game's name, modbundle.h sh_mb_stock_restore). No pass
 * registers, re-parses or refreshes one (ADD-L11); the gated reload alone
 * loads a surface model or image again when the game loaded it before its
 * bundle's requirements were live (ADD-L13). */
static int ms_surface(const ms_file *f) { return f->is_restore != 0; }

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
 * writes them. `path` is UTF-8, absolute, with '\' separators: every store
 * path in this file is, from the root sh_ms_root resolves (modstore.c) to
 * the folder names the scan reads, as the store and the record spell them. */
static int ms_wide(const char *path, wchar_t *out, size_t cap)
{
    size_t at = 0;
    if (path[0] && path[1] == ':' && path[2] == '\\') {
        if (cap < 5) return 0;
        wcscpy_s(out, cap, L"\\\\?\\");
        at = 4;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out + at, (int)(cap - at)) > 0;
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

/* A bundle whose content waits for its requirements: it sets cvars, or an
 * earlier install holds one of them at another value. Lines alone never wait. */
static int ms_req_needs(const ms_bundle *b) { return b->req_ncvars > 0 || b->req_lost != NULL; }

static int ms_rank(const char *type)
{
    if (!_stricmp(type, "renderParm")) return SH_MODSERVE_RANK_RENDERPARM;
    if (!_stricmp(type, "md6Def")) return SH_MODSERVE_RANK_MD6;
    if (!_stricmp(type, "entityDef")) return SH_MODSERVE_RANK_ENTITYDEF;
    if (!_stricmp(type, "snapEditorEntityDef")) return SH_MODSERVE_RANK_SEDEF;
    return SH_MODSERVE_RANK_OTHER;
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
        f->is_sedef = f->rank == SH_MODSERVE_RANK_SEDEF;
    } else {
        if (!ptype) ptype = ms_strdup(x, "file");
        if (pname) ms_lower(pname);
        else pname = (char *)request;
    }
    if (!ptype || !pname) { ms_fail(b, "out of memory"); return; }
    f->ptype = ptype;
    f->pname = pname;
    /* Namespace: new names live under mods/<salt>/ (ONE-SHAPE rule 1, the
     * builder's sh_mb_salt_paths_check). Raw files (and skins decls, which
     * are named by their mesh path) may sit deeper, below a folder of their
     * own; the sanitised spelling is the salt itself (a salt has no '-'). */
    if (!f->is_shadow && !f->is_restore) {
        char ns[96], ns_s[96], sid[80];
        int inside;
        _snprintf_s(ns, sizeof ns, _TRUNCATE, "mods/%s/", b->id);
        ms_sanitize(b->id, sid, sizeof sid);
        _snprintf_s(ns_s, sizeof ns_s, _TRUNCATE, "mods/%s/", sid);
        if (f->is_decl && _stricmp(ptype, "skins")) inside = !strncmp(f->pname, ns, strlen(ns));
        else if (!_stricmp(ptype, "soundBank")) {
            /* A bank lives in the one folder Wwise lists, so its namespace is
             * its stem: <salt>_<rest>, as the builder requires
             * (sh_mb_bank_stem). */
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
            ms_fail(b, "'%s' is outside mods/%s/ (its salt) and not listed in shadows", request, b->id);
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

/* requirements: the builder's canonical text (sh_mb_requirements_canonical)
 * is the only reading of the author's value, so the index and the bundle hash
 * see the same cvars and lines. No cvar or command is known here by name. */
static void ms_parse_requirements(ms_index *x, ms_bundle *b, const char *v, const char *ve)
{
    char cause[SH_MB_CAUSE_MAX], key[SH_MB_REQ_NAME_MAX + 16];
    char *canon = sh_mb_requirements_canonical(v, (size_t)(ve - v), cause, sizeof cause);
    ms_json o;
    const char *mv, *mve;
    if (!canon) { ms_fail(b, "%s", cause[0] ? cause : "requirements could not be read (out of memory)"); return; }
    b->req_text = ms_strdup(x, canon);
    free(canon);
    if (!b->req_text) { ms_fail(b, "out of memory"); return; }
    o.p = b->req_text;
    o.e = b->req_text + strlen(b->req_text);
    if (!ms_jenter(&o, '{')) { ms_fail(b, "requirements could not be read"); return; }
    while (!b->cause[0] && ms_jnext(&o, '}')) {
        ms_json c;
        if (!ms_jmember(&o, key, sizeof key, &mv, &mve)) { ms_fail(b, "requirements could not be read"); return; }
        c.p = mv;
        c.e = mve;
        if (!strcmp(key, "cvars")) {
            int n = 0;
            ms_json k = c;
            if (ms_jenter(&k, '{')) while (ms_jnext(&k, '}')) {
                if (!ms_jmember(&k, key, sizeof key, &mv, &mve)) break;
                n++;
            }
            if (!n) continue;
            b->req_cvars = (ms_req_cvar *)ms_alloc(x, sizeof(ms_req_cvar) * (size_t)n);
            if (!b->req_cvars) { ms_fail(b, "out of memory"); return; }
            if (ms_jenter(&c, '{')) while (b->req_ncvars < n && ms_jnext(&c, '}')) {
                ms_req_cvar *r = &b->req_cvars[b->req_ncvars];
                if (!ms_jmember(&c, key, sizeof key, &mv, &mve)) break;
                r->name = ms_strdup(x, key);
                r->value = ms_jstr(x, mv, mve, SH_MB_REQ_VALUE_MAX);
                if (!r->name || !r->value) { ms_fail(b, "out of memory"); return; }
                b->req_ncvars++;
            }
        } else if (!strcmp(key, "commands")) {
            if (!ms_jenter(&c, '{')) continue;
            while (ms_jnext(&c, '}')) {
                const char **lines;
                int *count, n = 0;
                ms_json a, k;
                if (!ms_jmember(&c, key, sizeof key, &mv, &mve)) break;
                if (!strcmp(key, "activate")) { count = &b->req_nact; }
                else if (!strcmp(key, "deactivate")) { count = &b->req_ndeact; }
                else continue;
                a.p = k.p = mv;
                a.e = k.e = mve;
                if (ms_jenter(&k, '[')) while (ms_jnext(&k, ']')) { k.p = ms_jskip(k.p, k.e); n++; }
                if (!n) continue;
                lines = (const char **)ms_alloc(x, sizeof(char *) * (size_t)n);
                if (!lines) { ms_fail(b, "out of memory"); return; }
                if (ms_jenter(&a, '[')) while (*count < n && ms_jnext(&a, ']')) {
                    const char *sv = a.p, *se = ms_jskip(a.p, a.e);
                    a.p = se;
                    lines[*count] = ms_jstr(x, sv, se, SH_MB_REQ_LINE_MAX);
                    if (!lines[*count]) { ms_fail(b, "out of memory"); return; }
                    (*count)++;
                }
                if (count == &b->req_nact) b->req_act = lines;
                else b->req_deact = lines;
            }
        }
    }
}

/* Map scope: a shadow the editor itself needs before any map is open. The
 * editor's decl types (snapEditorEntityDef, snapEditorSettings, the asset and
 * entity banks, the property inspectors, the palette and its groups) and the
 * editor's own movies; a bundle shadowing one is "always" unless its mod.json
 * says otherwise. */
static int ms_editor_time(const char *request)
{
    return sh_mb_editor_time(request);
}

/* A shadow of editor-time content: it changes what the author can set, not
 * what plays, so a placed name it resolves names no bundle (the save closure
 * and the map listing skip it). */
static int ms_editor_shadow(const ms_file *f)
{
    return f->is_shadow && ms_editor_time(f->request);
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

/* The retired author keys (ONE-SHAPE rule 6; modbundle.c mb_retired_key
 * refuses the same ones). */
static const char *const g_ms_retired[] = { "id", "key", "shadows", "stock", "scope", "player", "extends", "replaces",
                                            "legacy", "composes", "schema", NULL };

/* Parse <dir>\mod.json into the index; leaves b->cause set on failure. The
 * author block gives the name, the requirements and "extra"
 * (every key the product does not read itself: strings, hud, description,
 * ...); the builder block ("snapmap-plus", format 2) gives the salt, the
 * build hash, the files and the shadows. A folder in an earlier shape, one
 * the store has not built, and one whose build failed serve nothing; the
 * cause says which (ONE-SHAPE rules 2 and 6). Sizes only: hashes were
 * verified when it was built. */
static void ms_parse_bundle(ms_index *x, ms_bundle *b)
{
    char path[MS_PATH_MAX], key[64];
    unsigned char *text;
    size_t len = 0;
    sh_json_kind kind;
    sh_json_error err = { 0, NULL };
    ms_json c;
    const char *v, *ve, *files_v = NULL, *files_e = NULL, *stock_v = NULL, *stock_e = NULL, *req_v = NULL, *req_e = NULL;
    const char *block_v = NULL, *block_e = NULL, *shadows_v = NULL, *shadows_e = NULL, *retired = NULL;
    char **shadows = NULL, *salt = NULL, *name = NULL, *state_cause = NULL;
    int nshadows = 0, cshadows = 0, i, k, members = 0, block_object = 0;
    double format = -1, built = 0;
    ms_text extra = { NULL, 0, 0, 0 };
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mod.json", b->dir);
    text = ms_read_whole(path, SH_MODSERVE_MAX_JSON, &len);
    if (!text) {
        char pkg[MS_PATH_MAX];
        _snprintf_s(pkg, sizeof pkg, _TRUNCATE, "%s\\package.json", b->dir);
        if (ms_file_size(path) >= 0) ms_fail(b, "mod.json unreadable or over %u MiB", SH_MODSERVE_MAX_JSON >> 20);
        else if (ms_file_size(pkg) >= 0) ms_fail(b, SH_MB_SHAPE "its descriptor is package.json, which this version does not read");
        else ms_fail(b, "no mod.json");
        return;
    }
    if (!sh_json_validate_ex((const char *)text, len, MS_JSON_DEPTH, &kind, &err) || kind != SH_JSON_OBJECT) {
        /* The store remembers why it refused the folder (sh_ms_build_cause). */
        char last[SH_MB_CAUSE_MAX] = "";
        sh_modserve_build_cause_fn cause_fn = g_build_cause;
        if (cause_fn && b->dir) cause_fn(b->dir, last, sizeof last);
        if (last[0]) ms_fail(b, "not built: %s", last);
        else ms_fail(b, "mod.json is not a valid JSON object (%s at byte %zu)", err.reason ? err.reason : "not an object",
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
        for (k = 0; !retired && g_ms_retired[k]; ++k) if (!strcmp(key, g_ms_retired[k])) retired = g_ms_retired[k];
        if (!strcmp(key, "name")) name = ms_jstr(x, v, ve, 400);
        else if (!strcmp(key, "requirements")) { req_v = v; req_e = ve; }
        else if (!strcmp(key, SH_MB_BLOCK_KEY)) { block_v = v; block_e = ve; }
        else if (!strcmp(key, "author") || !strcmp(key, "homepage")) continue;
        else {
            /* Everything else is the author's, kept for the readers of
             * "extra" (strids.c, modhud.c, companions, scenes). */
            if (!strcmp(key, "hud") && v < ve && *v == '{') {
                ms_json h = { v, ve };
                char hk[64];
                const char *hv, *hve;
                if (ms_jenter(&h, '{')) while (ms_jnext(&h, '}')) {
                    if (!ms_jmember(&h, hk, sizeof hk, &hv, &hve)) break;
                    if (!strcmp(hk, "schema") && !retired) retired = "hud.schema";
                }
            }
            ms_text_put(&extra, members++ ? "," : "{", 1);
            ms_text_put(&extra, ks, (size_t)(ve - ks));
        }
    }
    if (block_v) {
        ms_json o = { block_v, block_e };
        if (ms_jenter(&o, '{')) {
            block_object = 1;
            while (ms_jnext(&o, '}')) {
                if (!ms_jmember(&o, key, sizeof key, &v, &ve)) break;
                if (!strcmp(key, "format")) { if (!ms_jnum(v, ve, &format)) format = -1; }
                else if (!strcmp(key, "salt")) salt = ms_jstr(x, v, ve, 32);
                else if (!strcmp(key, "build")) b->hash = ms_jstr(x, v, ve, 64);
                else if (!strcmp(key, "files")) { files_v = v; files_e = ve; }
                else if (!strcmp(key, "stock")) { stock_v = v; stock_e = ve; }
                else if (!strcmp(key, "shadows")) { shadows_v = v; shadows_e = ve; }
                else if (!strcmp(key, "cause")) {
                    /* Any cause stops the build serving, even one too long to show whole. */
                    state_cause = ms_jstr(x, v, ve, 4 * SH_MB_CAUSE_MAX);
                    if (!state_cause) state_cause = (char *)"its mod.json records a cause that cannot be read";
                }
                else if (!strcmp(key, "built")) { if (!ms_jnum(v, ve, &built)) built = 0; }
                else if (!strcmp(key, "registries") && v < ve && *v == '{') b->registries = ms_strndup(x, v, (size_t)(ve - v));
                else if (!strcmp(key, "edits") && v < ve && *v == '[') {
                    b->edits = ms_strndup(x, v, (size_t)(ve - v));
                    b->edits_len = b->edits ? (size_t)(ve - v) : 0;
                }
            }
        }
    }
    if (members) {
        ms_text_put(&extra, "}", 1);
        if (!extra.oom) b->extra = ms_strndup(x, extra.p, extra.n);
    }
    free(extra.p);
    salt = ms_trim(salt);
    b->id = salt;
    b->name = ms_trim(name);
    b->installed_at = built;
    /* The shape (ONE-SHAPE rule 6), in the order the builder's
     * sh_mb_descriptor_state_why words it. */
    if (block_v && (!block_object || format != (double)SH_MB_FORMAT))
        ms_fail(b, SH_MB_SHAPE "it was built by an earlier version of Snapmap+");
    else if (retired) ms_fail(b, SH_MB_SHAPE "mod.json uses \"%s\", which this version does not read", retired);
    else if (block_v && (!salt || !sh_salt_valid(salt, strlen(salt))))
        ms_fail(b, SH_MB_SHAPE "its \"" SH_MB_BLOCK_KEY "\" block has no valid salt");
    else if (!block_v) {
        /* A fresh folder: the store's scan salts and builds a root folder; a
         * map's place holds only builds the product received. */
        char last[SH_MB_CAUSE_MAX] = "";
        sh_modserve_build_cause_fn cause_fn = g_build_cause;
        if (b->place) ms_fail(b, SH_MB_SHAPE "it was not built by the mod store");
        else {
            if (cause_fn && b->dir) cause_fn(b->dir, last, sizeof last);
            if (last[0]) ms_fail(b, "not built: %s", last);
            else ms_fail(b, "not built yet (Refresh in the Mods tab builds it)");
        }
    }
    else if (state_cause && state_cause[0]) ms_fail(b, "not built: %s", state_cause);
    else if (!b->hash || !ms_valid_hash((char *)b->hash)) ms_fail(b, "not built yet (Refresh in the Mods tab builds it)");
    if (!b->id) b->id = "";
    if (!b->cause[0] && !files_v && !stock_v) ms_fail(b, "mod.json lists no files");
    if (shadows_v && !b->cause[0]) {
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
    if (req_v && !b->cause[0]) ms_parse_requirements(x, b, req_v, req_e);
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
        for (i = b->first_file; i < b->first_file + b->file_count; ++i) {
            const ms_file *f = &x->files[i];
            b->total += f->size;   /* a pointer's is 0 */
            if (f->is_sedef && !f->is_shadow) b->has_rows = 1;
            if (f->is_shadow && !f->is_restore) {
                b->has_shadows = 1;
                if (!b->scope_editor && ms_editor_time(f->request)) b->scope_editor = f->request;
            }
        }
    }
    free(shadows);
    HeapFree(GetProcessHeap(), 0, text);
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

/* "<name>.staging-<pid>" or "<name>.old-<pid>", the suffix last (modstore.c's
 * work folders); a mod named "doom.old-school" is not one. */
static int ms_work_suffix(const char *name, const char *tag)
{
    const char *at = NULL, *p;
    for (p = strstr(name, tag); p; p = strstr(p + 1, tag)) at = p;
    if (!at) return 0;
    at += strlen(tag);
    if (!*at) return 0;
    for (; *at; at++) if (*at < '0' || *at > '9') return 0;
    return 1;
}

/* A child folder of `dir` the walk looks at, in UTF-8; 0 to skip it (a file,
 * a reparse point, a hidden folder, a name that is not valid Unicode, install
 * staging). */
static int ms_walk_child(const WIN32_FIND_DATAW *fd, const char *rel, char *name, size_t cap)
{
    if (!(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        return 0;
    if (fd->cFileName[0] == L'.') return 0;
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, fd->cFileName, -1, name, (int)cap, NULL, NULL)) {
        ms_log("a folder in %s skipped: its name is not valid Unicode; rename it", rel && rel[0] ? rel : "the store");
        return 0;
    }
    return !ms_work_suffix(name, ".staging-") && !ms_work_suffix(name, ".old-");
}

/* The serving side of the store's walk (ONE-SHAPE rule 4, modstore.c): every
 * folder under the store root that holds mod.json (or a package.json, which
 * is refused), at any depth: a folder without one is an organizing folder and
 * is entered, to eight levels (modstore.h SH_MS_NEST_MAX); a mod folder is not
 * entered. At the top, "maps" is the places area: each folder in it is a
 * map's place, mods\maps\<segment>, whose mods are walked the same way.
 * `rel` is the folder's path below the root, a bundle's name; `place` its
 * area (NULL: the root). This scan only reads: salting and building are the
 * store's (ms_sync). */
static void ms_scan_folder(ms_index *x, const char *dir, const char *rel, int depth, const char *place)
{
    char pattern[MS_PATH_MAX];
    wchar_t wpattern[MS_PATH_MAX + 8];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    if (!ms_wide(pattern, wpattern, sizeof wpattern / sizeof wpattern[0])) return;
    h = FindFirstFileW(wpattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        ms_bundle *b;
        char name[MS_PATH_MAX], sub[MS_PATH_MAX], probe[MS_PATH_MAX], subrel[MS_PATH_MAX];
        int mark_files = x->nfiles, mark_needs = x->nneeds;
        if (!ms_walk_child(&fd, rel, name, sizeof name)) continue;
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", dir, name);
        if (rel && rel[0]) _snprintf_s(subrel, sizeof subrel, _TRUNCATE, "%s\\%s", rel, name);
        else strncpy_s(subrel, sizeof subrel, name, _TRUNCATE);
        if (!place && depth == 0 && !_stricmp(name, SH_MODSERVE_PLACES)) {
            /* The places area: each child folder is a map's place. */
            char inner[MS_PATH_MAX];
            wchar_t winner[MS_PATH_MAX + 8];
            WIN32_FIND_DATAW pd;
            HANDLE ph;
            _snprintf_s(inner, sizeof inner, _TRUNCATE, "%s\\*", sub);
            if (!ms_wide(inner, winner, sizeof winner / sizeof winner[0])) continue;
            ph = FindFirstFileW(winner, &pd);
            if (ph == INVALID_HANDLE_VALUE) continue;
            do {
                char seg[MS_PATH_MAX], prel[MS_PATH_MAX], pdir[MS_PATH_MAX];
                const char *pl;
                if (!ms_walk_child(&pd, subrel, seg, sizeof seg)) continue;
                _snprintf_s(prel, sizeof prel, _TRUNCATE, "%s\\%s", subrel, seg);
                _snprintf_s(pdir, sizeof pdir, _TRUNCATE, "%s\\%s", sub, seg);
                pl = ms_strdup(x, prel);
                if (pl) ms_scan_folder(x, pdir, prel, 1, pl);
            } while (FindNextFileW(ph, &pd));
            FindClose(ph);
            continue;
        }
        _snprintf_s(probe, sizeof probe, _TRUNCATE, "%s\\mod.json", sub);
        if (ms_file_size(probe) < 0) {
            _snprintf_s(probe, sizeof probe, _TRUNCATE, "%s\\package.json", sub);
            if (ms_file_size(probe) < 0) {
                /* Not a mod folder: an organizing folder, looked into. */
                if (depth + 1 < 8) ms_scan_folder(x, sub, subrel, depth + 1, place);
                continue;
            }
        }
        if (!ms_grow((void **)&x->bundles, &x->cbundles, x->nbundles + 1, sizeof(ms_bundle))) break;
        b = &x->bundles[x->nbundles++];
        memset(b, 0, sizeof *b);
        b->dir = ms_strdup(x, sub);
        b->folder = ms_strdup(x, subrel);
        b->place = place;
        b->first_file = x->nfiles;
        if (!b->dir || !b->folder) { ms_fail(b, "out of memory"); }
        else ms_parse_bundle(x, b);
        if (b->cause[0]) {
            /* Failure isolation: drop this bundle's rows, keep everything else. */
            x->nfiles = mark_files;
            x->nneeds = mark_needs;
            b->file_count = 0;
            ms_log("bundle '%s' skipped: %s", subrel, b->cause);
        } else b->admitted = 1;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* 1 when bundles a and b sit in the same area (both at the root, or both in
 * one map's place). */
static int ms_same_area(const ms_bundle *a, const ms_bundle *b)
{
    if (!a->place || !b->place) return !a->place && !b->place;
    return !_stricmp(a->place, b->place);
}

/* One build per salt per area (ONE-SHAPE rules 1 and 4). Of two admitted
 * folders of one salt in one area the one built first keeps it (built time,
 * then folder path); the other serves nothing until the store's next scan
 * makes it a new mod (at the root) or refuses it (in a place). Runs before
 * ms_resolve, which drops a refused bundle's files. */
static void ms_one_per_area(ms_index *x)
{
    int i, k;
    for (i = 0; i < x->nbundles; ++i) {
        ms_bundle *b = &x->bundles[i];
        if (!b->admitted) continue;
        for (k = 0; k < x->nbundles; ++k) {
            const ms_bundle *o = &x->bundles[k];
            int first;
            if (k == i || !o->admitted || strcmp(o->id, b->id) || !ms_same_area(o, b)) continue;
            first = o->installed_at != b->installed_at ? o->installed_at < b->installed_at : _stricmp(o->folder, b->folder) < 0;
            if (!first) continue;
            if (b->place)
                ms_fail(b, SH_MB_SHAPE "another folder of this map has the same mod: %s", o->folder);
            else
                ms_fail(b, "it is a copy of mods\\%s (the same salt %s); Refresh in the Mods tab makes it a new mod",
                        o->folder, b->id);
            b->admitted = 0;
            b->file_count = 0;
            ms_log("bundle '%s' skipped: %s", b->folder, b->cause);
            break;
        }
    }
}

/* The record's "editorOnly" section: bundles a map named whose fetch found
 * only editor-time content (modsync.c). Their rows are served without a
 * folder: the map plays without them. Keyed by bundle hash; the values (id,
 * name, when) are for the player reading the record. */
static void ms_load_editor_only(ms_index *x, const char *mods_root)
{
    char *text = NULL, key[80];
    ms_json o;
    const char *v, *ve;
    int cap = 0;
    if (sh_mb_record_get(mods_root, "editorOnly", &text) != 1 || !text) { free(text); return; }
    o.p = text;
    o.e = text + strlen(text);
    if (ms_jenter(&o, '{')) while (ms_jnext(&o, '}')) {
        const char *hash;
        if (!ms_jmember(&o, key, sizeof key, &v, &ve)) break;
        ms_lower(key);
        if (strlen(key) != 64 || !ms_valid_hash(key)) continue;
        if (!ms_grow((void **)&x->editor_only, &cap, x->neditor_only + 1, sizeof *x->editor_only)) break;
        hash = ms_strdup(x, key);
        if (hash) x->editor_only[x->neditor_only++] = hash;
    }
    free(text);
}

static int ms_editor_only_recorded(const ms_index *x, const char *bundle_hash)
{
    char lower[SH_MODSERVE_HASH_HEX + 1];
    int i;
    if (!x || !bundle_hash || strlen(bundle_hash) != SH_MODSERVE_HASH_HEX) return 0;
    strcpy_s(lower, sizeof lower, bundle_hash);
    ms_lower(lower);
    for (i = 0; i < x->neditor_only; ++i) if (!strcmp(x->editor_only[i], lower)) return 1;
    return 0;
}

int sh_modserve_editor_only_recorded(const char *bundle_hash)
{
    return ms_editor_only_recorded(g_index, bundle_hash);
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

/* Of two builds of one salt, the one that serves with no map declaring it
 * (ONE-SHAPE rule 4): the root folder's over a map's place, else the newer.
 * A place build never serves over the folder its author edits. */
static int ms_preferred(const ms_index *x, int a, int b)
{
    const ms_bundle *ba = &x->bundles[a], *bb = &x->bundles[b];
    if ((ba->place == NULL) != (bb->place == NULL) && ba->id && bb->id && !strcmp(ba->id, bb->id)) return ba->place == NULL;
    return ms_newer(x, a, b);
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
 * or in the bare statement grammar of md6Def and animWeb texts
 *
 *   stmt  := key value | key [name] '{' stmt* '}'     (no '=' or ';')
 *
 * and the texts agree everywhere except inside lists (a block holding
 * `num = N;` and only `item[i]` members) or bags (a block whose members
 * repeat a key, such as the `anim "clip" { ... }` blocks of an md6Def's
 * events or its `alias { ... }` rows), the served text is the winner's with
 * every other provider's list items or bag entries appended that the winner
 * lacks (matched by value, by a named block's key and name, or by a block
 * item's first scalar member such as aiType), lists renumbered and `num` set
 * to the count. Any other difference, a provider that lists an existing item
 * with different content, or a text outside the grammar leaves the winner's
 * bytes as they are. Removal cannot be expressed (no stock text is read
 * here), so composition is a union.
 *
 * A `key = value` statement keeps its `;` terminator only when its source
 * statement had one: an md6Def's meshKits pairs (`"doors" = ""`) have none,
 * and the engine's md6Def parser refuses the text when one is added
 * (snapmap-plus #160). An appended item takes the terminator convention of
 * the block it joins, and a renumbered list's `num` keeps the winner's. The
 * canonical text used for equality and identity ignores terminators. */

#define MC_DEPTH 32

typedef struct mc_node {
    const char *key; size_t key_n;
    const char *val; size_t val_n;        /* scalar value text, quotes included; NULL for a block */
    struct mc_node **kids; int nkids, ckids;
    int block, eq;
    int semi;                             /* the source statement ended in ';' */
} mc_node;

typedef struct mc_arena { struct mc_arena *next; size_t used; char data[16384]; } mc_arena;
typedef struct mc_ctx { mc_arena *arena; int failed; int conv; } mc_ctx;   /* conv: the winner's ';' convention */

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
        if (!n->eq && (t.kind == MC_WORD || t.kind == MC_STR) && mc_peek(l).kind == MC_LB) {
            /* A named block: `anim "clip" { ... }`. The name is part of the key. */
            n->key_n = (size_t)(t.p + t.n - n->key);
            t = mc_next(l);
        }
        if (t.kind == MC_LB) {
            n->block = 1;
            if (!mc_parse_block(c, l, n, depth + 1)) return 0;
        } else if (t.kind == MC_WORD || t.kind == MC_STR || t.kind == MC_GROUP) {
            n->val = t.p;
            n->val_n = t.n;
        } else if (t.kind == MC_RB && !n->eq) {
            /* A bare member before the closing brace: `{ "MD6_WEIGHTGROUP_ALL" }`. */
            n->val = t.p;
            n->val_n = 0;
            l->pos = (size_t)(t.p - l->t);
        } else return 0;
        if (mc_peek(l).kind == MC_SEMI) { mc_next(l); n->semi = 1; }
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

/* Serialise `n`'s members at `depth`; lists are renumbered. A `key = value`
 * statement ends in ';' when its source did, and always in the canonical text. */
static void mc_write_members(mc_buf *b, const mc_node *n, int depth, int canon)
{
    int i, item = 0, list = mc_is_list(n);
    char tmp[48];
    if (list) {
        int count = 0, semi = canon;
        for (i = 0; i < n->nkids; ++i)
            if (!mc_key_is(n->kids[i], "num")) count++;
            else semi |= n->kids[i]->semi;
        mc_indent(b, depth);
        _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "num = %d%s\n", count, semi ? ";" : "");
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
            mc_write_members(b, k, depth + 1, canon);
            mc_indent(b, depth);
            mc_put(b, "}\n", 2);
        } else if (k->eq) {
            mc_put(b, " = ", 3);
            mc_put(b, k->val, k->val_n);
            if (canon || k->semi) mc_put(b, ";\n", 2);
            else mc_put(b, "\n", 1);
        } else {
            if (k->val_n) { mc_put(b, " ", 1); mc_put(b, k->val, k->val_n); }
            mc_put(b, "\n", 1);
        }
    }
}

/* The canonical text of one node (for equality and identity). Malloc'd. */
static char *mc_text(const mc_node *n, size_t *len)
{
    mc_buf b = { NULL, 0, 0, 0 };
    if (n->block) { mc_put(&b, "{\n", 2); mc_write_members(&b, n, 1, 1); mc_put(&b, "}", 1); }
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

/* The key word alone (`anim` of `anim "clip"`). */
static size_t mc_word_n(const mc_node *n)
{
    size_t i = 0;
    while (i < n->key_n && n->key[i] != ' ' && n->key[i] != '\t') i++;
    return i;
}

static int mc_same_word(const mc_node *a, const mc_node *b)
{
    size_t x = mc_word_n(a), y = mc_word_n(b);
    return x == y && !_strnicmp(a->key, b->key, x);
}

/* A list item's identity: a scalar's value, or a block's first scalar member. */
static char *mc_identity(const mc_node *n)
{
    int i;
    if (n->block && mc_word_n(n) != n->key_n) {
        /* A named block is its key and name. */
        char *s = (char *)malloc(n->key_n + 1);
        if (!s) return NULL;
        memcpy(s, n->key, n->key_n);
        s[n->key_n] = 0;
        for (char *p = s; p < s + mc_word_n(n); ++p) *p = ms_fold(*p);
        return s;
    }
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

/* The ';' convention of the first `key = value` statement under `n`: 1 with
 * a terminator, 0 without, -1 when `n` holds none. */
static int mc_find_conv(const mc_node *n)
{
    int i, v;
    for (i = 0; i < n->nkids; ++i)
        if (!n->kids[i]->block && n->kids[i]->eq) return n->kids[i]->semi;
    for (i = 0; i < n->nkids; ++i)
        if (n->kids[i]->block && (v = mc_find_conv(n->kids[i])) >= 0) return v;
    return -1;
}

static void mc_set_conv(mc_node *n, int conv)
{
    int i;
    if (!n->block) { if (n->eq) n->semi = conv; return; }
    for (i = 0; i < n->nkids; ++i) mc_set_conv(n->kids[i], conv);
}

/* Append `item` to `parent` in the terminator convention of the block it
 * joins, or the winner's when that block holds no `key = value`. */
static int mc_append(mc_ctx *c, mc_node *parent, mc_node *item)
{
    int conv = mc_find_conv(parent);
    mc_set_conv(item, conv >= 0 ? conv : c->conv);
    return mc_add_kid(c, parent, item);
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
            if (!mc_append(c, base, (mc_node *)it)) return 0;
            (*added)++;
        }
        return 1;
    }
    if (mc_is_list(base) || mc_is_list(other)) return 0;
    if (mc_equal(base, other)) return 1;
    for (i = 0; i < other->nkids; ++i) {
        const mc_node *o = other->kids[i];
        mc_node *b = NULL;
        int bag = o->block && !o->eq && (mc_count_key(other, o) != 1 || mc_count_key(base, o) != 1 ||
                                         mc_word_n(o) != o->key_n);
        if (bag) {
            /* A bag entry (`anim "clip" { }`, `alias { }`): matched by its
             * identity among the base entries of the same key word. */
            /* A key that repeats within one text (`event "ae_sound"` rows,
             * `alias { }` rows) is matched by its whole text; a unique named
             * block (`anim "clip"`) by its key and name. A key the winner
             * lacks is new, not a repeat: two new clips with the same rows
             * (a door's kick and its reverse) are two entries, not one. */
            int exact = mc_count_key(other, o) > 1 || mc_count_key(base, o) > 1;
            char *id = exact ? mc_text(o, NULL) : mc_identity(o);
            int found = -1, same_word = 0;
            if (!id) return 0;
            for (k = 0; k < base->nkids && found < 0; ++k) {
                char *bid;
                if (!base->kids[k]->block || base->kids[k]->eq || !mc_same_word(base->kids[k], o)) continue;
                same_word++;
                bid = exact ? mc_text(base->kids[k], NULL) : mc_identity(base->kids[k]);
                if (!bid) { free(id); return 0; }
                if (!strcmp(id, bid)) found = k;
                free(bid);
            }
            free(id);
            if (found >= 0) {
                if (base->kids[found]->key_n != o->key_n || _strnicmp(base->kids[found]->key, o->key, o->key_n)) return 0;
                if (!mc_merge(c, base->kids[found], o, added)) return 0;
                continue;
            }
            if (!same_word && base->nkids) return 0;   /* nothing of this kind in the winner: not a bag it holds */
            if (!mc_append(c, base, (mc_node *)o)) return 0;
            (*added)++;
            continue;
        }
        if (mc_count_key(other, o) != 1 || mc_count_key(base, o) > 1) return 0;
        for (k = 0; k < base->nkids; ++k)
            if (base->kids[k]->key_n == o->key_n && !_strnicmp(base->kids[k]->key, o->key, o->key_n)) { b = base->kids[k]; break; }
        if (!b) return 0;   /* a member only one text has: an edit this cannot place without the stock text */
        if (b->block && o->block) { if (!mc_merge(c, b, o, added)) return 0; }
        else if (!mc_equal(b, o)) return 0;
    }
    /* Members only the base has are another edit of the same kind, unless
     * they are bag entries of a key word the other text also holds, or the
     * other's block is empty (the stock `aliases { }`): an empty bag holds
     * nothing to disagree with, as it does when the winner's is the empty one. */
    for (k = 0; k < base->nkids; ++k) {
        int found = 0;
        const mc_node *bk = base->kids[k];
        for (i = 0; i < other->nkids && !found; ++i)
            found = bk->key_n == other->kids[i]->key_n && !_strnicmp(bk->key, other->kids[i]->key, other->kids[i]->key_n);
        if (found) continue;
        if (bk->block && !bk->eq) {
            if (!other->nkids) continue;
            for (i = 0; i < other->nkids && !found; ++i)
                found = other->kids[i]->block && !other->kids[i]->eq && mc_same_word(bk, other->kids[i]);
        }
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
    if (base && (c.conv = mc_find_conv(base)) < 0) c.conv = 0;
    for (i = 1; base && i < n; ++i) {
        mc_node *o = mc_parse(&c, (const char *)texts[i], lens[i]);
        if (!o || !mc_merge(&c, base, o, added)) base = NULL;
    }
    if (base && !c.failed) {
        mc_put(&out, "{\n", 2);
        mc_write_members(&out, base, 1, 0);
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
 * When several bundles provide one path with different bytes, which one
 * serves is decided per map among the bundles the map declares
 * (ms_scope_compute, DESIGN-additive 2.3): no installed mod takes precedence
 * over another, and nothing about the install (time, folder, who delivered it)
 * counts. In order:
 *   1. the provider with the fewest needs the declared bundles do not provide
 *      (needs the game satisfies count alike for every provider, so the
 *      difference is what one copy names that nothing declared supplies);
 *   2. the bytes most declared providers carry;
 *   3. the order of the map's rows.
 * The static winner below (the same rules over every admitted bundle, then the
 * root folder's build of one salt, then the salt and the hash) serves only with map
 * scope off and is the first candidate for a path no declared bundle provides.
 * Two builds of one salt are never a conflict: each map declares its own. */

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

/* The static tie-break: of two builds of one salt the root folder's (ms_preferred), else the salt, then the hash. */
static int ms_static_before(const ms_index *x, int a, int b)
{
    const ms_bundle *ba = &x->bundles[a], *bb = &x->bundles[b];
    int c;
    if (a == b) return 0;
    if (!strcmp(ba->id, bb->id)) return ms_preferred(x, a, b);
    c = strcmp(ba->id, bb->id);
    if (c) return c < 0;
    return strcmp(ba->hash, bb->hash) < 0;
}

/* Index of the winning key in keys[i..j), and the reason. */
static int ms_pick(const ms_index *x, const ms_avail *av, const ms_key *keys, int i, int j, const char **reason)
{
    int k, best = i, unres[64], share[64], m = j - i > 64 ? 64 : j - i, tie_un = 1, tie_sh = 1, tie_key = 1;
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
        int kb = best - i;
        if (unres[k] != unres[kb]) { if (unres[k] < unres[kb]) best = i + k; continue; }
        if (share[k] != share[kb]) { if (share[k] > share[kb]) best = i + k; continue; }
        if (ms_static_before(x, f->bundle, b->bundle)) best = i + k;
    }
    for (k = 0; k < m; ++k) {
        const ms_file *f = &x->files[keys[i + k].file], *b = &x->files[keys[best].file];
        if (unres[k] != unres[best - i]) tie_un = 0;
        if (share[k] != share[best - i]) tie_sh = 0;
        if (strcmp(x->bundles[f->bundle].id, x->bundles[b->bundle].id)) tie_key = 0;
    }
    if (!tie_un) *reason = "its needs resolve against the installed bundles where the others' do not";
    else if (!tie_sh) *reason = "most providers carry these bytes";
    else if (tie_key) *reason = "the root folder's build of its salt";
    else *reason = "no rule separates them; the first salt in name order (a map's rows decide while it is open)";
    return best;
}

static int ms_conflict_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, ((const ms_conflict *)b)->request);
}

/* Map scope: the other providers of a path, recorded on its winner in the
 * order the static winner rules rank them (`ordered` 1, different bytes), the
 * root folder's build first (`ordered` 2, builds of one salt) or in index order
 * (identical bytes), so that the provider the open map declares serves the
 * path (ms_scope_compute). Up to 63 alternates; without memory the path keeps
 * only its winner. */
static void ms_record_alts(ms_index *x, const ms_avail *av, const ms_key *keys, int i, int j, int winner, int ordered)
{
    ms_key rest[64];
    int k, m = 0;
    for (k = i; k < j && m < 63; ++k) if (keys[k].file != winner) rest[m++] = keys[k];
    if (!m || !ms_grow((void **)&x->alts, &x->calts, x->nalts + m, sizeof(int))) return;
    x->files[winner].alt_first = x->nalts;
    x->files[winner].alt_count = m;
    while (m > 0) {
        const char *why = NULL;
        int b = 0, q;
        if (ordered == 1 && av && m > 1) b = ms_pick(x, av, rest, 0, m, &why);
        else if (ordered == 2)
            for (q = 1; q < m; ++q) if (ms_preferred(x, x->files[rest[q].file].bundle, x->files[rest[b].file].bundle)) b = q;
        x->alts[x->nalts++] = rest[b].file;
        x->files[rest[b].file].alt_of = winner;
        memmove(&rest[b], &rest[b + 1], sizeof *rest * (size_t)(m - b - 1));
        m--;
    }
}

/* Decide which files serve their request path. Returns 0 on allocation failure. */
static int ms_resolve(ms_index *x)
{
    ms_key *keys;
    int i, n = 0, logged = 0, avail_built = 0;
    int *superseded_by;
    ms_avail avail = { NULL, 0 };
    x->conflicts = x->composed = x->nconflict_list = x->nalts = 0;
    keys = (ms_key *)malloc(sizeof(ms_key) * (size_t)(x->nfiles ? x->nfiles : 1));
    superseded_by = (int *)malloc(sizeof(int) * (size_t)(x->nbundles ? x->nbundles : 1));
    if (!keys || !superseded_by) { free(keys); free(superseded_by); return 0; }
    for (i = 0; i < x->nbundles; ++i) superseded_by[i] = -1;
    for (i = 0; i < x->nfiles; ++i) {
        x->files[i].served = 0;
        x->files[i].body = NULL;
        x->files[i].body_len = 0;
        x->files[i].alt_first = x->files[i].alt_count = 0;
        x->files[i].alt_of = -1;
        x->files[i].scope_alt = -1;
        x->files[i].scope_last = -1;
        x->files[i].scope_body = NULL;
        x->files[i].scope_composed = NULL;
        x->files[i].scope_why = 0;
        if (x->bundles[x->files[i].bundle].admitted && !x->files[i].edit) { keys[n].key = x->files[i].request; keys[n].file = i; keys[n].alias = 0; n++; }
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
            if (j - i > 1) ms_record_alts(x, NULL, keys, i, j, winner, 0);
        } else if (same_id) {
            /* Two builds of one salt (the root folder and a map's place):
             * never a conflict. Each map serves the build it declares; the
             * root folder's is the static winner (no map open; ms_preferred). */
            for (k = i + 1; k < j; ++k)
                if (ms_preferred(x, x->files[keys[k].file].bundle, x->files[winner].bundle)) winner = keys[k].file;
            x->files[winner].served = 1;
            ms_record_alts(x, NULL, keys, i, j, winner, 2);
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
                int w = _snprintf_s(who + at, sizeof who - at, _TRUNCATE, "%s%s", k > i ? ", " : "", ms_label(b));
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
            if (any_shadow) ms_record_alts(x, &avail, keys, i, j, winner, 1);
            if (composed) {
                x->composed++;
                _snprintf_s(how, sizeof how, _TRUNCATE, "composed: %d list entr%s of the other providers added to %s's text",
                            added, added == 1 ? "y" : "ies", ms_label(&x->bundles[x->files[winner].bundle]));
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
                    ms_log("conflict: %s '%s' is provided by %s; serving %s's text with the others' list entries "
                           "(%d added)", any_shadow ? "shadow" : "path", keys[i].key, who,
                           ms_label(&x->bundles[x->files[winner].bundle]), added);
                else
                    ms_log("conflict: %s '%s' is provided by %s; serving %s (%s)", any_shadow ? "shadow" : "path",
                           keys[i].key, who, ms_label(&x->bundles[x->files[winner].bundle]), reason);
            }
        }
        i = j;
    }
    if (avail_built) free(avail.keys);
    if (logged > MS_CONFLICT_LOGS) ms_log("%d more conflicts not listed", logged - MS_CONFLICT_LOGS);
    for (i = 0; i < x->nbundles; ++i)
        if (superseded_by[i] >= 0)
            ms_log("mod %s: builds %.16s (%s) and %.16s (%s) both provide paths with other bytes; each map serves the "
                   "build it declares, and with no map open the root folder's (%.16s)", x->bundles[i].id, x->bundles[i].hash,
                   x->bundles[i].folder, x->bundles[superseded_by[i]].hash, x->bundles[superseded_by[i]].folder,
                   x->bundles[superseded_by[i]].hash);
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
        if (!f->served && !f->registry_keyed) continue;
        if (f->served) x->served_files++;
        if (f->is_shadow && f->served) x->shadows++;
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

/* The editor closure: every served file of an admitted bundle that adds
 * editor rows, and every served file one of those needs, at any depth.
 * Whatever the engine creates while it registers, parses or reloads one of
 * these is kept for the session, as a boot-time editor pass keeps its rows'
 * closure, so every pass treats them the same way (sh_modserve_step.persist,
 * the closure part of sh_modserve_each_refresh_part). The whole bundle counts,
 * not only what its rows need today: a later build's row can name a decl a
 * map pass already registered (R315 live: an open map's activation had made
 * the keycard md6Def a new row attaches, at level 1, and the keeper could
 * only raise the decl, not its model). Needs are closed under this mark, so a
 * closure decl's parents and models are in it too. */
static void ms_mark_editor_closure(ms_index *x)
{
    int *queue, nq = 0, qh = 0, i, k;
    if (!x->nfiles) return;
    queue = (int *)malloc(sizeof(int) * (size_t)x->nfiles);
    if (!queue) return;
    for (i = 0; i < x->nfiles; ++i) {
        ms_file *f = &x->files[i];
        if (f->served && x->bundles[f->bundle].admitted && (f->is_sedef || x->bundles[f->bundle].has_rows)) {
            f->editor_closure = 1;
            queue[nq++] = i;
        }
    }
    while (qh < nq) {
        const ms_file *f = &x->files[queue[qh++]];
        for (k = 0; k < f->need_count; ++k) {
            int p = x->needs[f->need_first + k].file;
            if (p < 0 || x->files[p].editor_closure || !x->files[p].served) continue;
            x->files[p].editor_closure = 1;
            queue[nq++] = p;
        }
    }
    free(queue);
}

/* ------------------------------------------------------------------ build and publish */

/* Map scope: the file that serves winner `f`'s path for the current listing
 * (ms_scope_compute: a bundle the open map declares, else what the editor or
 * the hub may serve, else nobody, -1, and the game's bytes answer). With map
 * scope off the static winner serves. Read lock-free. */
static int ms_scoped(const ms_index *x, int f)
{
    if (!InterlockedCompareExchange((volatile LONG *)&g_scope_on, 0, 0)) return f;
    return (int)InterlockedCompareExchange((volatile LONG *)&x->files[f].scope_alt, 0, 0);
}

/* The composed text a file serves instead of its own bytes, or NULL: the
 * listing's composition from the declared providers with map scope on, the
 * static composition from every provider with it off. */
static const unsigned char *ms_body_of(const ms_file *f, size_t *len)
{
    if (InterlockedCompareExchange((volatile LONG *)&g_scope_on, 0, 0)) {
        const ms_body *b = (const ms_body *)InterlockedCompareExchangePointer((PVOID volatile *)&f->scope_body, NULL, NULL);
        *len = b ? b->len : 0;
        return b ? b->p : NULL;
    }
    *len = f->body_len;
    return f->body;
}

/* Two files of successive tables that serve the same bytes. */
static int ms_same_bytes(const ms_file *a, const ms_file *b)
{
    size_t la, lb;
    const unsigned char *ba = ms_body_of(a, &la), *bb = ms_body_of(b, &lb);
    if (ba || bb) return ba && bb && la == lb && !memcmp(ba, bb, la);
    return a->blob && b->blob && !strcmp(a->blob, b->blob);
}

/* Refresh: what the engine may hold from the replaced table. */
static int ms_kind(const ms_file *f)
{
    if (f->is_decl) return SH_MODSERVE_RF_DECL;
    if (f->ptype && !_stricmp(f->ptype, "baseModel")) return SH_MODSERVE_RF_MODEL;
    if (f->ptype && !_stricmp(f->ptype, "image")) return SH_MODSERVE_RF_IMAGE;
    /* Held anims (level 4) are reloaded in place at a map pass (report ADD-G1);
     * a skeleton has no in-place route and stays a restart. */
    if (f->ptype && !_stricmp(f->ptype, "anim") && !strncmp(f->request, "cooked/anim/", 12)) return SH_MODSERVE_RF_ANIM;
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
static ms_body *ms_reg_body(const ms_index *x, int file);
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
            /* A new editor row that shadows a stock one is a palette change too:
             * the refresh below re-parses it, and the palette must be built
             * again to show the mod's row (the removal side counts it as well). */
            if (f->is_sedef && f->is_shadow && f->ptype && f->pname) d->changed_rows++;   /* it replaces the stock row */
            if (f->is_decl && !f->is_shadow) {
                d->added_decls++;
                if (f->is_sedef) d->added_rows++;
                else if (!ms_surface(f) && ms_id_registered(old, x->bundles[f->bundle].id)) { f->refresh_new = 1; d->added_active++; }
            } else if (f->is_shadow && ms_scoped(x, i) >= 0) {
                /* A shadow the previous table did not serve replaces a stock
                 * file the engine may already hold parsed from its own bytes
                 * (a mod installed after boot, or reinstalled after an
                 * uninstall): pending like a changed file, so the refresh
                 * re-parses or reloads it now instead of at the next restart.
                 * A shadow the map scope withdraws changes nothing yet. */
                kind = ms_kind(f);
                if (kind && f->ptype && f->pname && !ms_surface(f)) f->refresh = 1;
                if (kind == SH_MODSERVE_RF_DECL) d->changed_decls++;
                else if (kind == SH_MODSERVE_RF_MODEL) d->changed_models++;
                else if (kind == SH_MODSERVE_RF_IMAGE) d->changed_images++;
                else if (kind == SH_MODSERVE_RF_ANIM) d->changed_anims++;
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
        /* A withdrawn shadow's new bytes are read when a map that uses it opens. */
        if (kind && f->ptype && f->pname && !ms_surface(f) && !(f->is_shadow && ms_scoped(x, i) < 0)) f->refresh = 1;
        if (kind == SH_MODSERVE_RF_DECL) d->changed_decls++;
        else if (kind == SH_MODSERVE_RF_MODEL) d->changed_models++;
        else if (kind == SH_MODSERVE_RF_IMAGE) d->changed_images++;
        else if (kind == SH_MODSERVE_RF_ANIM) d->changed_anims++;
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
        /* A shadow the map scope had withdrawn: the engine holds the game's bytes already. */
        if (of->is_shadow && !of->is_restore && ms_scoped(old, i) < 0) continue;
        /* A removed editor row, the mod's own or a shadow of a stock one: re-parsed
         * without the mod's bytes it resolves no entityDef, or the game's own row,
         * so the palette must be built again without it. */
        if (of->is_sedef && of->ptype && of->pname) d->gone_rows++;
        if (of->is_decl && of->is_shadow && !of->is_restore && of->ptype && of->pname) {
            d->gone_shadows++;
            if (!ms_add_gone(x, &cap, old, i, 1)) ms_log("refresh: out of memory; a removed shadow keeps its bundle's text");
        } else if (of->is_sedef && !of->is_restore && of->ptype && of->pname) {
            if (!ms_add_gone(x, &cap, old, i, 1)) ms_log("refresh: out of memory; a removed editor row stays in the palette");
        } else if (of->is_decl) d->gone_names++;
    }
    /* Registries (L2b deviation 7): a rescan that changes the text a registry
     * generates for the listing (a mod rebuilt with other edits) marks the path that serves it, as a changed
     * decl is marked. */
    for (i = 0; i < x->reg_count; ++i) {
        int nf = x->reg_first + i, of = -1, j, target = -1;
        const ms_body *nb = ms_reg_body(x, nf), *ob = NULL;
        for (j = 0; j < old->reg_count && of < 0; ++j)
            if (!strcmp(old->files[old->reg_first + j].request, x->files[nf].request)) of = old->reg_first + j;
        if (of >= 0) ob = ms_reg_body(old, of);
        if (!nb && !ob) continue;
        if (nb && ob && nb->len == ob->len && !memcmp(nb->p, ob->p, nb->len)) continue;
        if (x->files[nf].registry_keyed) target = nf;
        else
            for (j = 0; j < x->nfiles && target < 0; ++j) if (x->files[j].registry_of == nf + 1) target = j;
        if (target < 0 || !x->files[target].ptype || !x->files[target].pname) continue;
        InterlockedExchange(&x->files[target].refresh_gen, 0);
        InterlockedExchange(&x->files[target].refresh, 1);
        d->changed++;
        d->changed_decls++;
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

/* ------------------------------------------------------------------ map scope */

/* The current map's listing (DESIGN-additive 2.3, 2.5): the bundle hashes it
 * declares, in the order of its rows, written by sh_modserve_scope_set under
 * g_build_lock and g_scope_lock and read by the index build under the same
 * build lock. The per-bundle and per-path results it gives are read
 * lock-free by every lookup. */
static SRWLOCK g_scope_lock = SRWLOCK_INIT;
static char (*g_scope_hashes)[SH_MODSERVE_HASH_HEX + 1];
static int g_scope_n, g_scope_cap;
static volatile LONG g_scope_open;
static volatile LONG g_scope_play;     /* SH_MODSERVE_MODE_PLAY: Play, or a map launched outside the editor */
static sh_modserve_scope_source_fn volatile g_scope_source;

/* The open map's context (sh_modserve_scope_set_place): its place, a
 * "maps\<segment>" folder below the store root ("" for none), and whether it
 * is the author's own map, whose rows always follow the root folders. Its own
 * lock: ms_bundle_by_hash reads it under the scope lock and without it. */
static SRWLOCK g_place_lock = SRWLOCK_INIT;
static char g_place[SH_MODSERVE_PLACE_CAP];
static int g_place_own;

static void ms_scope_place(char *out, size_t cap, int *own)
{
    AcquireSRWLockShared(&g_place_lock);
    strncpy_s(out, cap, g_place, _TRUNCATE);
    if (own) *own = g_place_own;
    ReleaseSRWLockShared(&g_place_lock);
}

/* Why a path is served as it is (sh_modserve_layer_report). */
#define MS_WHY_ONE       0   /* the only provider the listing admits */
#define MS_WHY_SAME      1   /* the declared providers carry the same bytes */
#define MS_WHY_NEEDS     2   /* fewest needs the declared bundles do not provide */
#define MS_WHY_SHARE     3   /* the bytes most declared providers carry */
#define MS_WHY_ROWS      4   /* the order of the map's rows */
#define MS_WHY_COMPOSED  5   /* the declared providers' list entries composed */
#define MS_WHY_EDITOR    6   /* not declared: a name the editor or the hub serves for the Toybox */
#define MS_WHY_ALWAYS    7   /* an editor-time bundle (editor tweaks) */
#define MS_WHY_NONE      8   /* nothing serves it: the game's bytes answer */
#define MS_WHY_SURFACE   9   /* not declared: a surface (the game's own bytes), served outside Play */
#define MS_WHY_REGISTRY  10  /* a registry the product generates from the serving bundles' intent */
static const char *const g_why_text[] = {
    "the only provider", "identical bytes", "its needs resolve against the declared bundles",
    "most declared providers carry these bytes", "the first of the map's rows", "composed from the declared providers",
    "not declared; offered for the Toybox", "editor-time content, served in every map", "no declared bundle provides it",
    "not declared; a surface (the game's own bytes)",
    "generated by the product from the serving bundles (a registry)",
};

/* Editor-time content: a bundle whose shadows include the editor's own decls
 * or movies. Those files are editor tweaks (DESIGN-additive 2.6): client-side,
 * they serve in every map this editor opens, a downloaded map included, and
 * nothing else of the bundle follows them: its names, surfaces, other shadows,
 * edits and requirements obey the declared rule like any bundle's. */
static int ms_scope_always(const ms_bundle *b) { return b->scope_editor != NULL; }

/* The file class a bundle's serve flags admit. */
static int ms_sv_class(const ms_file *f)
{
    if (f->is_restore) return MS_SV_RESTORE;
    if (f->is_shadow) return ms_editor_time(f->request) ? MS_SV_EDITOR : MS_SV_SHADOW;
    return MS_SV_NEW;
}

static int ms_sv_allows(const ms_index *x, int file)
{
    const ms_file *f = &x->files[file];
    return (InterlockedCompareExchange((volatile LONG *)&x->bundles[f->bundle].sv, 0, 0) & ms_sv_class(f)) != 0;
}

/* The bytes a pick serves: two picks serve the same bytes when both are the
 * game's (-1), or carry the same composed text, or the same file hash. */
static int ms_pick_same(const ms_index *x, int a, const ms_body *ba, int b, const ms_body *bb)
{
    const ms_file *fa, *fb;
    if (a < 0 || b < 0) return a == b;
    if (ba || bb) return ba && bb && (ba == bb || (ba->len == bb->len && !memcmp(ba->p, bb->p, ba->len)));
    fa = &x->files[a];
    fb = &x->files[b];
    return a == b || (fa->blob && fb->blob && !strcmp(fa->blob, fb->blob));
}

/* The admitted files of the declared bundles, as ms_avail_build lists them. */
static int ms_avail_declared(ms_index *x, ms_avail *av)
{
    int i, n = 0;
    av->n = 0;
    av->keys = (char **)malloc(sizeof(char *) * (size_t)(x->nfiles * 2 + 1));
    if (!av->keys) return 0;
    for (i = 0; i < x->nfiles; ++i) {
        const ms_file *f = &x->files[i];
        char buf[MS_NAME_MAX];
        if (!x->bundles[f->bundle].admitted || InterlockedCompareExchange(&x->bundles[f->bundle].rank, 0, 0) < 0) continue;
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

/* The text the declared providers `d[0..nd)` compose to (d[0] the pick),
 * memoized on the winner by the set of candidates; NULL when they do not
 * compose (not all decls, too large, a parse the list grammar refuses). */
static ms_body *ms_scope_compose(ms_index *x, int winner, const int *cand, int ncand, const int *d, int nd)
{
    const unsigned char *texts[64];
    size_t lens[64], len = 0;
    unsigned char *owned[64];
    uint64_t set = 0;
    int nt = 0, ok = 1, q, k, added = 0;
    ms_body *m;
    char *text;
    uint64_t order = 1469598103934665603ull;
    for (k = 0; k < nd; ++k) {
        for (q = 0; q < ncand && q < 64; ++q) if (cand[q] == d[k]) set |= 1ull << q;
        order = (order ^ (uint64_t)(unsigned)d[k]) * 1099511628211ull;
    }
    for (m = (ms_body *)x->files[winner].scope_composed; m; m = m->next)
        if (m->set == set && m->order == order) return m->p ? m : NULL;
    memset(owned, 0, sizeof owned);
    for (k = 0; k < nd && ok && nt < 64; ++k) {
        const ms_file *f = &x->files[d[k]];
        int dup = 0;
        if (!f->is_decl || f->size > SH_MODSERVE_CACHE_FILE) { ok = 0; break; }
        for (q = 0; q < k && !dup; ++q) {
            const ms_file *g = &x->files[d[q]];
            if (g->blob && f->blob && !strcmp(g->blob, f->blob)) dup = 1;
        }
        if (dup) continue;
        owned[nt] = ms_file_raw(f, &lens[nt]);
        if (!owned[nt] || lens[nt] > SH_MODSERVE_CACHE_FILE) { ok = 0; break; }
        texts[nt] = owned[nt];
        nt++;
    }
    text = ok && nt > 1 ? ms_compose(texts, lens, nt, &len, &added) : NULL;
    for (q = 0; q < nt; ++q) if (owned[q]) HeapFree(GetProcessHeap(), 0, owned[q]);
    m = (ms_body *)ms_alloc(x, sizeof *m);
    if (!m) { free(text); return NULL; }
    memset(m, 0, sizeof *m);
    m->set = set;
    m->order = order;
    if (text) {
        unsigned char *body = (unsigned char *)ms_alloc(x, len ? len : 1);
        if (body) { memcpy(body, text, len); m->p = body; m->len = len; m->added = added; }
        free(text);
    }
    m->next = (ms_body *)x->files[winner].scope_composed;
    x->files[winner].scope_composed = m;
    return m->p ? m : NULL;
}

/* ------------------------------------------------------------------ registries
 *
 * DESIGN-additive 2.4 (modregistry.h): derived edits. The index carries one
 * synthetic file per game file an admitted bundle's product block "edits"
 * names (the conductor, the player body and entity, weapons, FSM states,
 * whitelists, ...). Each belongs to the product's pseudo-bundle
 * (bundles[nbundles], never counted, never admitted), is never "served" (no
 * bundle walk, need, closure or name lookup sees it) and has no disk file:
 * its bytes are the text generated for the current listing (ms_reg_memo.body,
 * read through scope_body like a composed text): the game's text with the
 * serving bundles' edits applied. A bundle's edited copy itself (ms_file.edit)
 * is never served. Where a bundle serves its own file at
 * that name (a shadow), that winner points at the synthetic file
 * (registry_of), the shadow's diff against the game's text is that bundle's
 * edits, and the scope pick moves to the synthetic file; elsewhere the
 * synthetic file is the path's own key (registry_keyed). With no text for the
 * listing the game's file answers. */

typedef struct ms_reg_memo {
    uint64_t sig;                         /* the serving set and the choices it was generated for */
    ms_body **body;                       /* per registry file: the text it serves, or NULL (the game's) */
    int nrw;                              /* encounter and score decls served with the assigned bits */
    int *rw_file;
    ms_body **rw_body;
    const char **lines;                   /* sh_modserve_registry_report */
    int nlines, nsources;
    int *gate;                            /* per registry file: the source bundle whose requirement gates gate its text, or -1 */
    struct ms_reg_memo *next;
} ms_reg_memo;

#define MS_REG_SOURCE_MAX 256
static struct { char slot[SH_MR_SLOT_MAX]; char key[SH_MR_KEY_MAX]; } g_scope_choice[SH_MR_CHOICES_MAX];
static int g_scope_nchoice;           /* the open map's choices between mods' edits (g_scope_lock) */

/* The bundle whose requirement gates gate what `f` serves: its own, or, for
 * a generated text, the first source with requirements whose edits changed
 * it (L2b deviation 8). */
static const ms_bundle *ms_gate_of(const ms_index *x, const ms_file *f)
{
    if (f->registry) {
        const ms_reg_memo *m = (const ms_reg_memo *)InterlockedCompareExchangePointer((PVOID volatile *)&x->reg_cur, NULL, NULL);
        int k = (int)(f - x->files) - x->reg_first;
        if (m && m->gate && k >= 0 && k < x->reg_count && m->gate[k] >= 0) return &x->bundles[m->gate[k]];
    }
    return &x->bundles[f->bundle];
}

static int ms_live(const ms_index *x, const ms_file *f)
{
    return ((f->served && x->bundles[f->bundle].admitted) || f->registry_keyed) && !f->edit;
}

static int ms_is_type(const char *t, const char *want) { return t && !_stricmp(t, want); }

typedef struct ms_req_list { char (*reqs)[SH_MB_REQUEST_MAX + 1]; int n, cap; int oom; } ms_req_list;

static void ms_req_list_add(void *context, const char *file, const sh_dt_counts *counts)
{
    ms_req_list *l = (ms_req_list *)context;
    char req[SH_MB_REQUEST_MAX + 1];
    int k;
    (void)counts;
    if (!file || strlen(file) >= sizeof req) return;
    strcpy_s(req, sizeof req, file);
    ms_lower(req);
    for (k = 0; k < l->n; ++k) if (!strcmp(l->reqs[k], req)) return;
    if (!ms_grow((void **)&l->reqs, &l->cap, l->n + 1, sizeof *l->reqs)) { l->oom = 1; return; }
    strcpy_s(l->reqs[l->n++], SH_MB_REQUEST_MAX + 1, req);
}

static int ms_req_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* The edited copies of each admitted bundle (ms_file.edit): never served. */
static void ms_mark_edits(ms_index *x)
{
    int i, k, j;
    for (i = 0; i < x->nbundles; ++i) {
        ms_bundle *b = &x->bundles[i];
        ms_req_list l = { NULL, 0, 0, 0 };
        if (!b->edits || !b->edits_len) continue;
        if (sh_dt_edits_files(b->edits, b->edits_len, ms_req_list_add, &l) < 0)
            ms_log("bundle '%s': its product block's edits do not read; nothing it edits is generated", b->folder);
        for (k = b->first_file; k < b->first_file + b->file_count; ++k)
            for (j = 0; j < l.n; ++j)
                if (!strcmp(x->files[k].request, l.reqs[j])) { x->files[k].edit = 1; break; }
        free(l.reqs);
    }
}

/* The synthetic files, after ms_resolve (winners known), before the tables. */
static int ms_registry_files(ms_index *x)
{
    ms_req_list l = { NULL, 0, 0, 0 };
    int i, k;
    ms_bundle *pb;
    x->reg_count = 0;
    for (i = 0; i < x->nbundles; ++i)
        if (x->bundles[i].admitted && x->bundles[i].edits)
            (void)sh_dt_edits_files(x->bundles[i].edits, x->bundles[i].edits_len, ms_req_list_add, &l);
    if (l.oom) goto oom;
    if (l.n > 1) qsort(l.reqs, (size_t)l.n, sizeof *l.reqs, ms_req_cmp);
    /* The pseudo-bundle one past the count. */
    if (!ms_grow((void **)&x->bundles, &x->cbundles, x->nbundles + 1, sizeof(ms_bundle))) goto oom;
    pb = &x->bundles[x->nbundles];
    memset(pb, 0, sizeof *pb);
    pb->dir = pb->folder = "";
    pb->id = "snapmap-plus.registries";
    pb->hash = "";
    pb->name = "game files generated by the product from the declared mods' edits";
    pb->rank = -1;
    strcpy_s(pb->cause, sizeof pb->cause, "the product's generated game files, per map");
    pb->first_file = x->nfiles;
    x->reg_bundle = x->nbundles;
    x->reg_first = x->nfiles;
    for (k = 0; k < l.n; ++k) {
        ms_file *f;
        int w = -1, j;
        const char *rest, *slash, *type;
        size_t n;
        if (strncmp(l.reqs[k], "generated/decls/", 16)) continue;
        rest = l.reqs[k] + 16;
        slash = strchr(rest, '/');
        n = strlen(rest);
        if (!slash || n < 6 || strcmp(rest + n - 5, ".decl")) continue;
        type = sh_mb_decl_type(rest, (size_t)(slash - rest));
        if (!ms_grow((void **)&x->files, &x->cfiles, x->nfiles + 1, sizeof(ms_file))) goto oom;
        f = &x->files[x->nfiles];
        memset(f, 0, sizeof *f);
        f->bundle = x->reg_bundle;
        f->request = ms_strdup(x, l.reqs[k]);
        f->disk = "";
        if (!f->request) goto oom;
        f->ptype = type ? type : ms_strndup(x, rest, (size_t)(slash - rest));
        f->pname = ms_strndup(x, slash + 1, strlen(slash + 1) - 5);
        if (!f->pname || !f->ptype) goto oom;
        f->reg = f->request + 16;
        f->is_decl = 1;
        f->is_shadow = 1;                 /* a game name: with nothing generated the game's bytes come back */
        f->rank = ms_rank(f->ptype);
        f->registry = x->reg_count + 1;
        f->alt_of = -1;
        f->scope_alt = -1;
        f->scope_last = -1;
        for (j = 0; j < x->nfiles && w < 0; ++j)
            if (x->files[j].served && x->bundles[x->files[j].bundle].admitted && !strcmp(x->files[j].request, f->request)) w = j;
        if (w >= 0) x->files[w].registry_of = x->nfiles + 1;
        else f->registry_keyed = 1;
        x->nfiles++;
        x->reg_count++;
    }
    pb->file_count = x->reg_count;
    x->reg_xtext = (const char **)ms_alloc(x, sizeof(char *) * (size_t)(x->reg_count + 1));
    x->reg_xlen = (size_t *)ms_alloc(x, sizeof(size_t) * (size_t)(x->reg_count + 1));
    x->reg_xread = (unsigned char *)ms_alloc(x, (size_t)(x->reg_count + 1));
    if (!x->reg_xtext || !x->reg_xlen || !x->reg_xread) goto oom;
    memset((void *)x->reg_xtext, 0, sizeof(char *) * (size_t)(x->reg_count + 1));
    memset(x->reg_xlen, 0, sizeof(size_t) * (size_t)(x->reg_count + 1));
    memset(x->reg_xread, 0, (size_t)(x->reg_count + 1));
    free(l.reqs);
    return 1;
oom:
    free(l.reqs);
    return 0;
}

/* Which bundles the generator reads: edits, a served file at a registry's
 * name, or an encounter (the conductor's bits). */
static void ms_registry_relevant(ms_index *x)
{
    int i, k, r;
    for (i = 0; i < x->nbundles; ++i) {
        ms_bundle *b = &x->bundles[i];
        b->reg_relevant = 0;
        if (!b->admitted) continue;
        b->reg_relevant = b->edits != NULL;
        for (k = b->first_file; !b->reg_relevant && k < b->first_file + b->file_count; ++k) {
            const ms_file *f = &x->files[k];
            b->reg_relevant = ms_is_type(f->ptype, "snapAiEncounter");
            for (r = 0; r < x->reg_count && !b->reg_relevant; ++r)
                b->reg_relevant = f->served && !strcmp(f->request, x->files[x->reg_first + r].request);
        }
    }
}

static ms_body *ms_reg_body_new(ms_index *x, const char *p, size_t len)
{
    ms_body *m = (ms_body *)ms_alloc(x, sizeof *m);
    unsigned char *b = (unsigned char *)ms_alloc(x, len ? len : 1);
    if (!m || !b) return NULL;
    memset(m, 0, sizeof *m);
    memcpy(b, p, len);
    m->p = b;
    m->len = len;
    return m;
}

/* A file's bytes as a NUL-terminated process-heap text, or NULL. */
static char *ms_reg_text(const ms_file *f, size_t *len)
{
    size_t n = 0;
    unsigned char *raw = ms_file_raw(f, &n), *t;
    *len = 0;
    if (!raw) return NULL;
    t = (unsigned char *)HeapReAlloc(GetProcessHeap(), 0, raw, n + 1);
    if (!t) { HeapFree(GetProcessHeap(), 0, raw); return NULL; }
    t[n] = 0;
    *len = n;
    return (char *)t;
}

/* The game's text of registry file k, read once per index through the
 * installed-content reader (nothing is generated for one it cannot read). */
static void ms_registry_stock(ms_index *x, int k)
{
    const ms_file *f = &x->files[x->reg_first + k];
    char sha[SH_MB_HASH_HEX + 1], cause[SH_MB_CAUSE_MAX];
    unsigned long long size = 0;
    unsigned char *bytes = NULL;
    size_t len = 0;
    if (x->reg_xread[k]) return;
    x->reg_xread[k] = 1;
    if (sh_mb_stock_identify(f->request, sha, &size) == SH_MB_STOCK_OK &&
        sh_mb_stock_read(f->request, sha, &bytes, &len, f->request, cause, sizeof cause) == SH_MB_STOCK_OK) {
        char *t = (char *)ms_alloc(x, len + 1);
        if (t) { memcpy(t, bytes, len); t[len] = 0; x->reg_xtext[k] = t; x->reg_xlen[k] = len; }
        HeapFree(GetProcessHeap(), 0, bytes);
    } else ms_log("registry: the game's %s could not be read; nothing is generated for it", f->request);
}

static int ms_reg_order(const void *a, const void *b)
{
    const ms_bundle *x = *(const ms_bundle *const *)a, *y = *(const ms_bundle *const *)b;
    int c;
    if (x->rank >= 0 || y->rank >= 0) {
        if (x->rank < 0) return 1;
        if (y->rank < 0) return -1;
        return x->rank < y->rank ? -1 : x->rank > y->rank;
    }
    c = strcmp(x->id, y->id);
    return c ? c : strcmp(x->hash, y->hash);
}

static int ms_reg_own(char ***owned, int *n, int *cap, char *text)
{
    if (!text) return 0;
    if (!ms_grow((void **)owned, cap, *n + 1, sizeof(char *))) { HeapFree(GetProcessHeap(), 0, text); return 0; }
    (*owned)[(*n)++] = text;
    return 1;
}

/* Generate the registry files for the bundles in `set` (serving order). */
static ms_reg_memo *ms_registry_generate(ms_index *x, ms_bundle **set, int n, uint64_t sig)
{
    sh_mr_source *src = (sh_mr_source *)calloc((size_t)(n ? n : 1), sizeof *src);
    sh_mr_decl **decls = (sh_mr_decl **)calloc((size_t)(n ? n : 1), sizeof *decls);
    char **derived = (char **)calloc((size_t)(n ? n : 1), sizeof *derived);   /* a shadow's edits (malloc'd) */
    sh_mr_target *tg = (sh_mr_target *)calloc((size_t)(x->reg_count + 1), sizeof *tg);
    char **owned = NULL;
    int nowned = 0, cowned = 0, i, k;
    sh_mr_choice choices[SH_MR_CHOICES_MAX];
    sh_mr_result r;
    ms_reg_memo *m = NULL;
    memset(&r, 0, sizeof r);
    if (!src || !decls || !derived || !tg) goto done;
    for (k = 0; k < x->reg_count; ++k) {
        ms_registry_stock(x, k);
        tg[k].request = x->files[x->reg_first + k].request;
        tg[k].text = x->reg_xtext[k];
        tg[k].len = x->reg_xlen[k];
    }
    for (i = 0; i < g_scope_nchoice; ++i) { choices[i].slot = g_scope_choice[i].slot; choices[i].key = g_scope_choice[i].key; }
    for (i = 0; i < n; ++i) {
        ms_bundle *b = set[i];
        sh_mr_source *s = &src[i];
        char who[200];
        int nd = 0, want_decls = b->edits && strstr(b->edits, SH_MB_REGISTRY_CONDUCTOR) != NULL;
        _snprintf_s(who, sizeof who, _TRUNCATE, "%s", ms_label(b));
        s->key = b->id;
        s->who = ms_strdup(x, who);
        s->edits = b->edits;
        s->edits_len = b->edits_len;
        /* A shadow at a registry's name: its diff against the
         * game's text is its edits. */
        for (k = b->first_file; k < b->first_file + b->file_count; ++k) {
            const ms_file *f = &x->files[k];
            int q;
            if (!f->served || f->edit || !f->registry_of) continue;
            q = f->registry_of - 1 - x->reg_first;
            if (q < 0 || q >= x->reg_count || !tg[q].text) continue;
            {
                size_t len = 0;
                char *t = ms_reg_text(f, &len), cause[300], *one;
                if (!ms_reg_own(&owned, &nowned, &cowned, t)) continue;
                one = sh_dt_diff(f->request, tg[q].text, tg[q].len, t, len, NULL, cause, sizeof cause);
                if (!one) {
                    ms_log("registry: %s: %s's copy of %s does not parse (%s); it adds nothing", who, b->id, f->request, cause);
                    continue;
                }
                {
                    size_t ol = derived[i] ? strlen(derived[i]) : 0, al = strlen(one);
                    char *g = (char *)realloc(derived[i], ol + al + 3);
                    if (g) {
                        if (!ol) { g[0] = '['; memcpy(g + 1, one, al); g[al + 1] = ']'; g[al + 2] = 0; }
                        else { g[ol - 1] = ','; memcpy(g + ol, one, al); g[ol + al] = ']'; g[ol + al + 1] = 0; }
                        derived[i] = g;
                    }
                }
                free(one);
                if (!_stricmp(f->request, SH_MB_REGISTRY_CONDUCTOR)) want_decls = 1;
                ms_log("registry: %s carries its own %s; its edits against the game's text are applied", who, f->request);
            }
        }
        if (derived[i]) { s->edits = derived[i]; s->edits_len = strlen(derived[i]); }
        if (want_decls) {
            decls[i] = (sh_mr_decl *)calloc((size_t)(b->file_count ? b->file_count : 1), sizeof **decls);
            if (!decls[i]) goto done;
            for (k = b->first_file; k < b->first_file + b->file_count; ++k) {
                const ms_file *f = &x->files[k];
                int score = ms_is_type(f->ptype, "snapAiScore");
                size_t len = 0;
                char *t;
                if (!score && !ms_is_type(f->ptype, "snapAiEncounter")) continue;
                t = ms_reg_text(f, &len);
                if (!ms_reg_own(&owned, &nowned, &cowned, t)) continue;
                decls[i][nd].text = t;
                decls[i][nd].len = len;
                decls[i][nd].name = f->pname;
                decls[i][nd].score = score;
                decls[i][nd].id = k;
                nd++;
            }
            s->decls = decls[i];
            s->ndecls = nd;
        }
    }
    if (!sh_modregistry_generate(src, n, tg, x->reg_count, choices, g_scope_nchoice, &r)) goto done;
    m = (ms_reg_memo *)ms_alloc(x, sizeof *m);
    if (!m) goto done;
    memset(m, 0, sizeof *m);
    m->sig = sig;
    m->nsources = n;
    m->body = (ms_body **)ms_alloc(x, sizeof(ms_body *) * (size_t)(x->reg_count ? x->reg_count : 1));
    m->gate = (int *)ms_alloc(x, sizeof(int) * (size_t)(x->reg_count ? x->reg_count : 1));
    if (!m->body || !m->gate) { m = NULL; goto done; }
    memset(m->body, 0, sizeof(ms_body *) * (size_t)(x->reg_count ? x->reg_count : 1));
    for (k = 0; k < x->reg_count; ++k) {
        m->gate[k] = -1;
        if (k < r.ntargets && r.text[k]) m->body[k] = ms_reg_body_new(x, r.text[k], r.len[k]);
        /* The gates of the generated texts: the first contributing source with requirements. */
        for (i = 0; k < r.ntargets && i < n && m->gate[k] < 0; ++i)
            if (r.used[k][i] && ms_req_needs(set[i])) m->gate[k] = (int)(set[i] - x->bundles);
    }
    if (r.nrewrites) {
        m->rw_file = (int *)ms_alloc(x, sizeof(int) * (size_t)r.nrewrites);
        m->rw_body = (ms_body **)ms_alloc(x, sizeof(ms_body *) * (size_t)r.nrewrites);
        if (m->rw_file && m->rw_body)
            for (i = 0; i < r.nrewrites; ++i) {
                m->rw_file[m->nrw] = r.rewrites[i].id;
                m->rw_body[m->nrw] = ms_reg_body_new(x, r.rewrites[i].text, r.rewrites[i].len);
                if (m->rw_body[m->nrw]) m->nrw++;
            }
    }
    if (r.nlines) {
        m->lines = (const char **)ms_alloc(x, sizeof(char *) * (size_t)r.nlines);
        if (m->lines)
            for (i = 0; i < r.nlines; ++i) {
                m->lines[m->nlines] = ms_strdup(x, r.lines[i]);
                if (m->lines[m->nlines]) m->nlines++;
            }
    }
    for (i = 0; i < m->nlines; ++i) ms_log("registry: %s", m->lines[i]);
    m->next = x->reg_memo;
    x->reg_memo = m;
done:
    sh_modregistry_result_free(&r);
    for (i = 0; i < nowned; ++i) HeapFree(GetProcessHeap(), 0, owned[i]);
    free(owned);
    for (i = 0; i < n; ++i) {
        if (decls) free(decls[i]);
        if (derived) free(derived[i]);
    }
    free(src); free(decls); free(derived); free(tg);
    return m;
}

/* Two listings' registries serve the same bytes: per registry file the same
 * text, and the same rewritten decls. */
static int ms_reg_same(const ms_index *x, const ms_reg_memo *a, const ms_reg_memo *b)
{
    int k, i;
    if (a == b) return 1;
    for (k = 0; k < x->reg_count; ++k) {
        const ms_body *ba = a ? a->body[k] : NULL;
        const ms_body *bb = b ? b->body[k] : NULL;
        if (ba || bb) {
            if (!ba || !bb || ba->len != bb->len || memcmp(ba->p, bb->p, ba->len)) return 0;
        }
    }
    if ((a ? a->nrw : 0) != (b ? b->nrw : 0)) return 0;
    for (i = 0; a && b && i < a->nrw; ++i)
        if (a->rw_file[i] != b->rw_file[i] || a->rw_body[i]->len != b->rw_body[i]->len ||
            memcmp(a->rw_body[i]->p, b->rw_body[i]->p, a->rw_body[i]->len)) return 0;
    return 1;
}

/* The registries of the current listing (caller holds the scope and build
 * locks): the bundles that serve and edit a game file, in row order
 * (editor-time bundles after the rows, by key), memoized. With map scope off
 * nothing is generated. Returns 1 when the generated texts changed. */
static int ms_registry_compute(ms_index *x, int on)
{
    ms_bundle **set;
    ms_reg_memo *m = NULL, *was = x->reg_cur;
    int n = 0, i;
    uint64_t sig = 1469598103934665603ull;
    if (!x->reg_count) return 0;
    if (on) {
        set = (ms_bundle **)malloc(sizeof(ms_bundle *) * (size_t)(x->nbundles ? x->nbundles : 1));
        if (!set) return 0;
        for (i = 0; i < x->nbundles; ++i) {
            ms_bundle *b = &x->bundles[i];
            if (!b->admitted || !b->reg_relevant || b->inert) continue;
            if (b->rank < 0) continue;
            if (n < MS_REG_SOURCE_MAX) set[n++] = b;
        }
        qsort(set, (size_t)n, sizeof *set, ms_reg_order);
        for (i = 0; i < n; ++i) {
            const char *h = set[i]->hash;
            for (; *h; ++h) sig = (sig ^ (unsigned char)*h) * 1099511628211ull;
            sig = (sig ^ '|') * 1099511628211ull;
        }
        for (i = 0; i < g_scope_nchoice; ++i) {
            const char *p;
            for (p = g_scope_choice[i].slot; *p; ++p) sig = (sig ^ (unsigned char)*p) * 1099511628211ull;
            sig = (sig ^ '=') * 1099511628211ull;
            for (p = g_scope_choice[i].key; *p; ++p) sig = (sig ^ (unsigned char)*p) * 1099511628211ull;
            sig = (sig ^ ';') * 1099511628211ull;
        }
        if (n) {
            for (m = x->reg_memo; m && m->sig != sig; m = m->next) {}
            if (!m) m = ms_registry_generate(x, set, n, sig);
        }
        free(set);
    }
    InterlockedExchangePointer((PVOID volatile *)&x->reg_cur, m);
    return !ms_reg_same(x, was, m);
}

/* The text registry file `file` serves for the listing, or NULL. */
static ms_body *ms_reg_body(const ms_index *x, int file)
{
    const ms_reg_memo *m = (const ms_reg_memo *)InterlockedCompareExchangePointer((PVOID volatile *)&x->reg_cur, NULL, NULL);
    int k = file - x->reg_first;
    return m && k >= 0 && k < x->reg_count ? m->body[k] : NULL;
}

/* An encounter or score decl served with the assigned spawn-type bits, or NULL. */
static ms_body *ms_reg_rewrite(const ms_index *x, int file)
{
    const ms_reg_memo *m = (const ms_reg_memo *)InterlockedCompareExchangePointer((PVOID volatile *)&x->reg_cur, NULL, NULL);
    int i;
    for (i = 0; m && i < m->nrw; ++i) if (m->rw_file[i] == file) return m->rw_body[i];
    return NULL;
}

/* Set every admitted bundle's serve flags from the listing and the mode, then
 * choose the file serving each path (caller holds g_scope_lock and the build
 * lock). Declared bundles serve everything they carry. A build of a salt the
 * map declares at another hash is inert. With no map open, or in the editor,
 * an undeclared bundle serves the names under its salt for the Toybox (only
 * the root folder's build) and its surfaces, never its shadows; in Play and
 * in a map launched outside the editor it serves nothing. An editor-time
 * bundle (ms_scope_always) serves as before. With `mark`, every path whose
 * served bytes changed is marked pending as at a rescan (shadows: also when
 * the game's bytes come back; names: only when another build's bytes replace
 * the ones last served). Returns the bundles whose serving changed. */
static int ms_scope_compute(ms_index *x, int mark)
{
    int i, k, changed = 0, marked = 0, on = InterlockedCompareExchange(&g_scope_on, 0, 0) != 0;
    int open = InterlockedCompareExchange(&g_scope_open, 0, 0) != 0;
    int play = open && InterlockedCompareExchange(&g_scope_play, 0, 0) != 0, next = 0, av_built = 0;
    ms_avail av = { NULL, 0 };
    if (!x) return 0;
    for (i = 0; i < x->nbundles; ++i) x->bundles[i].inert = 0;
    /* The declared bundles, by hash, in row order (ms_bundle_by_hash picks the
     * folder of the open map's place, else the root's); a later row (or the
     * placed closure) naming another build of a salt already declared is not
     * one. */
    {
        LONG *rank = (LONG *)malloc(sizeof(LONG) * ((size_t)x->nbundles + 1));
        if (!rank) return 0;
        for (i = 0; i < x->nbundles; ++i) rank[i] = -1;
        for (k = 0; k < g_scope_n; ++k) {
            int b = ms_bundle_by_hash(x, g_scope_hashes[k], 1), dup = 0;
            if (b < 0 || rank[b] >= 0) continue;
            for (i = 0; i < x->nbundles && !dup; ++i) dup = rank[i] >= 0 && !strcmp(x->bundles[i].id, x->bundles[b].id);
            if (!dup) rank[b] = next++;
        }
        for (i = 0; i < x->nbundles; ++i) {
            ms_bundle *b = &x->bundles[i];
            LONG sv, roff, was_sv, was_r, was_rank;
            int inert = 0;
            if (!b->admitted) continue;
            for (k = 0; k < x->nbundles && !inert; ++k)
                inert = k != i && rank[k] >= 0 && rank[i] < 0 && !strcmp(x->bundles[k].id, b->id);
            if (!on || rank[i] >= 0) sv = MS_SV_ALL;
            else if (inert) sv = 0;
            else if (play || !b->root_build) sv = ms_scope_always(b) ? MS_SV_EDITOR : 0;
            else sv = (ms_scope_always(b) ? MS_SV_EDITOR : 0) | MS_SV_NEW | MS_SV_RESTORE;
            /* Requirements are cvars: a declared bundle's apply; a root build
             * with Toybox rows keeps them outside Play, because the palette
             * registers its rows in every editor map and with none open; in
             * Play no undeclared bundle's apply; an inert build never. A
             * place build serves only for its map (ONE-SHAPE rule 4). */
            roff = on && rank[i] < 0 && (inert || play || !(b->has_rows && b->root_build));
            b->inert = inert;
            was_sv = InterlockedExchange(&b->sv, sv);
            was_rank = InterlockedExchange(&b->rank, on ? rank[i] : -1);
            (void)InterlockedExchange(&b->shadows_off, (sv & (MS_SV_SHADOW | MS_SV_EDITOR)) ? 0 : 1);
            was_r = InterlockedExchange(&b->req_off, roff);
            if (was_sv != sv || was_r != roff || was_rank != (on ? rank[i] : -1)) changed++;
        }
        free(rank);
    }
    /* Registries from the bundles that now serve (ms_registry_compute). */
    if (ms_registry_compute(x, on)) changed++;
    /* Each path: the file that serves it now. */
    for (i = 0; i < x->nfiles; ++i) {
        ms_file *f = &x->files[i];
        int cand[64], nc = 0, d[64], nd = 0, best = -1, why = MS_WHY_NONE, q, declared = 0;
        long ok[64];   /* per candidate: its row (declared), past every row (editor-time bundles), or -1 */
        LONG was;
        ms_body *body = NULL, *was_body;
        if (!f->served || !x->bundles[f->bundle].admitted) continue;
        cand[nc++] = i;
        for (k = 0; k < f->alt_count && nc < 64; ++k) cand[nc++] = x->alts[f->alt_first + k];
        if (!on) best = i, why = MS_WHY_ONE;
        else {
            /* The providers that serve for the listing: the declared ones,
             * and editor-time bundles (they compose with the others after
             * every row). */
            for (k = 0; k < nc; ++k) {
                const ms_bundle *cb = &x->bundles[x->files[cand[k]].bundle];
                ok[k] = -1;
                if (!ms_sv_allows(x, cand[k])) continue;
                if (cb->rank >= 0) ok[k] = cb->rank, declared++;
                else if (ms_scope_always(cb)) ok[k] = 1000000L + k;
                else continue;
                d[nd] = cand[k];
                ok[nd] = ok[k];
                nd++;
            }
            if (nd == 1) best = d[0], why = declared ? MS_WHY_ONE : MS_WHY_ALWAYS;
            else if (nd > 1 && !declared) {
                /* Only editor-time bundles: the static order and composition, as before phase 2. */
                best = d[0];
                why = MS_WHY_ALWAYS;
                for (k = 1; k < nd; ++k) if (!ms_pick_same(x, d[0], NULL, d[k], NULL)) break;
                if (k < nd) {
                    body = ms_scope_compose(x, i, cand, nc, d, nd);
                    if (body) why = MS_WHY_COMPOSED;
                }
            } else if (nd > 1) {
                int same = 1, unres[64], share[64], tie_un = 1, tie_sh = 1, bk = 0;
                for (k = 1; k < nd && same; ++k)
                    same = ms_pick_same(x, d[0], NULL, d[k], NULL);
                if (same) {
                    for (k = 1; k < nd; ++k) if (ok[k] < ok[bk]) bk = k;
                    best = d[bk], why = MS_WHY_SAME;
                } else {
                    if (!av_built) av_built = ms_avail_declared(x, &av);
                    for (k = 0; k < nd; ++k) {
                        unres[k] = av_built ? ms_unresolved(x, &av, &x->files[d[k]]) : 0;
                        share[k] = 0;
                        for (q = 0; q < nd; ++q) share[k] += ms_pick_same(x, d[k], NULL, d[q], NULL);
                    }
                    for (k = 1; k < nd; ++k) {
                        if (unres[k] != unres[bk]) { if (unres[k] < unres[bk]) bk = k; continue; }
                        if (share[k] != share[bk]) { if (share[k] > share[bk]) bk = k; continue; }
                        if (ok[k] < ok[bk]) bk = k;
                    }
                    for (k = 0; k < nd; ++k) {
                        if (unres[k] != unres[bk]) tie_un = 0;
                        if (share[k] != share[bk]) tie_sh = 0;
                    }
                    best = d[bk];
                    why = !tie_un ? MS_WHY_NEEDS : !tie_sh ? MS_WHY_SHARE : MS_WHY_ROWS;
                    /* Lists and bags of the declared providers' texts compose
                     * (never two builds of one salt: only one is declared). */
                    {
                        int order[64], no = 0;
                        long ook[64];
                        order[no] = best; ook[no++] = ok[bk];
                        for (k = 0; k < nd; ++k) if (d[k] != best) { order[no] = d[k]; ook[no++] = ok[k]; }
                        for (k = 1; k < no; ++k)   /* the rest in row order */
                            for (q = k; q > 1 && ook[q] < ook[q - 1]; --q) {
                                int t = order[q]; long tk = ook[q];
                                order[q] = order[q - 1]; ook[q] = ook[q - 1];
                                order[q - 1] = t; ook[q - 1] = tk;
                            }
                        body = ms_scope_compose(x, i, cand, nc, order, no);
                        if (body) why = MS_WHY_COMPOSED;
                    }
                }
            } else {
                for (k = 0; k < nc && best < 0; ++k) if (ms_sv_allows(x, cand[k])) best = cand[k];
                if (best >= 0)
                    why = ms_scope_always(&x->bundles[x->files[best].bundle]) ? MS_WHY_ALWAYS :
                          x->files[best].is_restore ? MS_WHY_SURFACE : MS_WHY_EDITOR;
            }
        }
        /* A registry's path: the text generated for the listing, else the
         * game's; any other pick: a decl served with the spawn-type bits the
         * conductor assigned. */
        if (on && f->registry_of) {
            body = ms_reg_body(x, f->registry_of - 1);
            best = body ? f->registry_of - 1 : -1;
            why = body ? MS_WHY_REGISTRY : MS_WHY_NONE;
        } else if (on && best >= 0 && !body && x->reg_cur) {
            ms_body *rw = ms_reg_rewrite(x, best);
            if (rw) body = rw;
        }
        was = InterlockedCompareExchange(&f->scope_alt, 0, 0);
        was_body = was >= 0 && was < x->nfiles ?
            (ms_body *)InterlockedCompareExchangePointer((PVOID volatile *)&x->files[was].scope_body, NULL, NULL) : NULL;
        /* The text first, then the pick: a reader sees the new pick with its own text. */
        if (best >= 0) (void)InterlockedExchangePointer((PVOID volatile *)&x->files[best].scope_body, body);
        if (was >= 0 && was < x->nfiles && was != best)
            (void)InterlockedExchangePointer((PVOID volatile *)&x->files[was].scope_body, NULL);
        (void)InterlockedExchange(&f->scope_alt, best);
        f->scope_why = why;
        if (!mark) {
            if (best >= 0) (void)InterlockedExchange(&f->scope_last, best);
            continue;
        }
        if (!ms_kind(f) || !f->ptype || !f->pname || f->is_restore) {
            if (best >= 0) (void)InterlockedExchange(&f->scope_last, best);
            continue;
        }
        if (f->is_shadow) {
            if (ms_pick_same(x, (int)was, was_body, best, body)) continue;
        } else {
            /* A name: withdrawn, the engine keeps what it parsed; served again,
             * it is read again only when other bytes than the last served
             * ones serve it now (another build of its salt). */
            LONG last = InterlockedCompareExchange(&f->scope_last, 0, 0);
            if (best < 0) continue;
            (void)InterlockedExchange(&f->scope_last, best);
            if (last < 0 || ms_pick_same(x, (int)last, last == was ? was_body : NULL, best, body)) continue;
        }
        if (best >= 0) (void)InterlockedExchange(&f->scope_last, best);
        InterlockedExchange(&f->refresh_gen, 0);
        InterlockedExchange(&f->refresh, 1);
        marked++;
    }
    /* A registry path no bundle carries: its synthetic file is the key. */
    for (i = x->reg_first; x->reg_count && i < x->reg_first + x->reg_count; ++i) {
        ms_file *f = &x->files[i];
        ms_body *body, *was_body;
        LONG was;
        int best;
        if (!f->registry_keyed) continue;
        body = on ? ms_reg_body(x, i) : NULL;
        best = body ? i : -1;
        was = InterlockedCompareExchange(&f->scope_alt, 0, 0);
        was_body = was >= 0 ? (ms_body *)InterlockedCompareExchangePointer((PVOID volatile *)&f->scope_body, NULL, NULL) : NULL;
        (void)InterlockedExchangePointer((PVOID volatile *)&f->scope_body, body);
        (void)InterlockedExchange(&f->scope_alt, best);
        f->scope_why = body ? MS_WHY_REGISTRY : MS_WHY_NONE;
        if (best >= 0) (void)InterlockedExchange(&f->scope_last, best);
        if (!mark || ms_pick_same(x, (int)was, was_body, best, body)) continue;
        InterlockedExchange(&f->refresh_gen, 0);
        InterlockedExchange(&f->refresh, 1);
        marked++;
    }
    if (av_built) free(av.keys);
    if (marked) InterlockedExchange(&x->expanded, 0);
    return changed;
}

/* Once per bundle and hash: an editor-time bundle. */
static void ms_scope_log(const ms_index *x, const ms_index *old)
{
    int i;
    for (i = 0; i < x->nbundles; ++i) {
        const ms_bundle *b = &x->bundles[i];
        int o;
        if (!b->admitted || !b->scope_editor) continue;
        o = old ? ms_bundle_by_hash(old, b->hash, 1) : -1;
        if (o >= 0) continue;
        ms_log("scope: %s shadows editor-time content (%s); its editor tweaks serve in every map this editor "
               "opens, the rest only in maps that declare it", ms_label(b), b->scope_editor);
    }
}

void sh_modserve_scope_enable(int on)
{
    ms_index *x;
    InterlockedExchange(&g_scope_on, on ? 1 : 0);
    AcquireSRWLockExclusive(&g_build_lock);
    x = (ms_index *)g_index;
    AcquireSRWLockShared(&g_scope_lock);
    (void)ms_scope_compute(x, 1);
    ReleaseSRWLockShared(&g_scope_lock);
    ReleaseSRWLockExclusive(&g_build_lock);
}

int sh_modserve_scope_enabled(void) { return InterlockedCompareExchange(&g_scope_on, 0, 0) != 0; }
int sh_modserve_scope_map_open(void) { return InterlockedCompareExchange(&g_scope_open, 0, 0) != 0; }

/* Caller holds g_scope_lock exclusively. */
static void ms_scope_add_hash(const char *hash)
{
    char lower[SH_MODSERVE_HASH_HEX + 1];
    int i;
    if (!hash || strlen(hash) != SH_MODSERVE_HASH_HEX) return;
    strcpy_s(lower, sizeof lower, hash);
    if (!ms_valid_hash(lower)) return;
    for (i = 0; i < g_scope_n; ++i) if (!strcmp(g_scope_hashes[i], lower)) return;
    if (!ms_grow((void **)&g_scope_hashes, &g_scope_cap, g_scope_n + 1, sizeof *g_scope_hashes)) return;
    strcpy_s(g_scope_hashes[g_scope_n++], SH_MODSERVE_HASH_HEX + 1, lower);
}

int sh_modserve_scope_hashes(char (*out)[SH_MODSERVE_HASH_HEX + 1], int cap)
{
    int i, n;
    AcquireSRWLockShared(&g_scope_lock);
    n = g_scope_n;
    for (i = 0; out && i < n && i < cap; ++i) strcpy_s(out[i], SH_MODSERVE_HASH_HEX + 1, g_scope_hashes[i]);
    ReleaseSRWLockShared(&g_scope_lock);
    return n;
}

int sh_modserve_scope_set(const char *const *hashes, int nhashes, const char *const *ids, int nids, int map_open)
{
    ms_index *x;
    int i, changed;
    (void)ids; (void)nids;   /* a row names its build by hash; a key alone declares nothing (DESIGN-additive 2.5) */
    AcquireSRWLockExclusive(&g_build_lock);
    x = (ms_index *)g_index;
    AcquireSRWLockExclusive(&g_scope_lock);
    g_scope_n = 0;
    for (i = 0; hashes && i < nhashes; ++i) ms_scope_add_hash(hashes[i]);
    InterlockedExchange(&g_scope_open, map_open ? 1 : 0);
    if (!map_open) InterlockedExchange(&g_scope_play, 0);
    if (!map_open) g_scope_nchoice = 0;   /* registry choices belong to the open map */
    changed = ms_scope_compute(x, 1);
    ReleaseSRWLockExclusive(&g_scope_lock);
    ReleaseSRWLockExclusive(&g_build_lock);
    return changed;
}

int sh_modserve_scope_set_place(const char *place, int own)
{
    ms_index *x;
    char norm[SH_MODSERVE_PLACE_CAP];
    int changed = 0, same;
    size_t n;
    char *p;
    norm[0] = 0;
    if (place && place[0]) {
        /* "maps\<segment>", or the segment alone; '/' is '\'. */
        if (_strnicmp(place, SH_MODSERVE_PLACES "\\", sizeof SH_MODSERVE_PLACES) &&
            _strnicmp(place, SH_MODSERVE_PLACES "/", sizeof SH_MODSERVE_PLACES))
            n = (size_t)_snprintf_s(norm, sizeof norm, _TRUNCATE, "%s\\%s", SH_MODSERVE_PLACES, place);
        else n = (size_t)_snprintf_s(norm, sizeof norm, _TRUNCATE, "%s", place);
        if (n == (size_t)-1) return 0;   /* too long for a place */
        for (p = norm; *p; ++p) if (*p == '/') *p = '\\';
        while ((n = strlen(norm)) > 0 && norm[n - 1] == '\\') norm[n - 1] = 0;
    }
    AcquireSRWLockExclusive(&g_build_lock);
    x = (ms_index *)g_index;
    AcquireSRWLockExclusive(&g_scope_lock);
    AcquireSRWLockExclusive(&g_place_lock);
    same = !_stricmp(g_place, norm) && g_place_own == (own ? 1 : 0);
    strcpy_s(g_place, sizeof g_place, norm);
    g_place_own = own ? 1 : 0;
    ReleaseSRWLockExclusive(&g_place_lock);
    /* The listing resolves its hashes in this context: a change recomputes it. */
    if (!same && x && g_scope_n) changed = ms_scope_compute(x, 1);
    ReleaseSRWLockExclusive(&g_scope_lock);
    ReleaseSRWLockExclusive(&g_build_lock);
    return changed;
}

int sh_modserve_scope_set_choices(const char *const *slots, const char *const *keys, int n, int recompute)
{
    ms_index *x;
    int i, changed = 0;
    AcquireSRWLockExclusive(&g_build_lock);
    x = (ms_index *)g_index;
    AcquireSRWLockExclusive(&g_scope_lock);
    g_scope_nchoice = 0;
    for (i = 0; slots && keys && i < n && g_scope_nchoice < SH_MR_CHOICES_MAX; ++i) {
        if (!slots[i] || !keys[i] || !slots[i][0] || !keys[i][0]) continue;
        strncpy_s(g_scope_choice[g_scope_nchoice].slot, sizeof g_scope_choice[0].slot, slots[i], _TRUNCATE);
        strncpy_s(g_scope_choice[g_scope_nchoice].key, sizeof g_scope_choice[0].key, keys[i], _TRUNCATE);
        g_scope_nchoice++;
    }
    if (recompute && x) changed = ms_scope_compute(x, 1);
    ReleaseSRWLockExclusive(&g_scope_lock);
    ReleaseSRWLockExclusive(&g_build_lock);
    return changed;
}

int sh_modserve_registry_report(sh_modserve_line_fn fn, void *context)
{
    const ms_index *x = g_index;
    const ms_reg_memo *m;
    char line[800];
    int i, n = 0;
    if (!fn) return 0;
    AcquireSRWLockShared(&g_scope_lock);
    m = x ? (const ms_reg_memo *)x->reg_cur : NULL;
    if (!x || !x->reg_count) fn(context, "registries: no index");
    else {
        for (i = 0; i < x->reg_count; ++i) {
            const ms_file *f = &x->files[x->reg_first + i];
            const ms_body *b = m ? m->body[i] : NULL;
            if (b)
                _snprintf_s(line, sizeof line, _TRUNCATE, "registry %s: generated, %zu bytes%s", f->request, b->len,
                            f->registry_keyed ? "" : " (a bundle still carries this file; the generated text serves)");
            else
                _snprintf_s(line, sizeof line, _TRUNCATE, "registry %s: the game's own file", f->request);
            fn(context, line);
            n++;
        }
        for (i = 0; m && i < m->nlines; ++i) {
            _snprintf_s(line, sizeof line, _TRUNCATE, "registry: %s", m->lines[i]);
            fn(context, line);
            n++;
        }
        {
            int k;
            for (k = 0; k < x->nfiles; ++k) {
                const ms_file *f = &x->files[k];
                const ms_bundle *b;
                if (!f->edit || !x->bundles[f->bundle].admitted) continue;
                b = &x->bundles[f->bundle];
                _snprintf_s(line, sizeof line, _TRUNCATE, "registry edits %s: %s edits it; its copy is never "
                            "served, its edits apply in maps that declare it", f->request, ms_label(b));
                fn(context, line);
                n++;
            }
        }
        for (i = 0; i < g_scope_nchoice; ++i) {
            _snprintf_s(line, sizeof line, _TRUNCATE, "registry choice: %s -> %s", g_scope_choice[i].slot,
                        g_scope_choice[i].key);
            fn(context, line);
            n++;
        }
    }
    ReleaseSRWLockShared(&g_scope_lock);
    return n;
}

int sh_modserve_scope_mode(int mode)
{
    ms_index *x;
    int changed = 0;
    LONG want = mode == SH_MODSERVE_MODE_PLAY ? 1 : 0;
    AcquireSRWLockExclusive(&g_build_lock);
    x = (ms_index *)g_index;
    AcquireSRWLockShared(&g_scope_lock);
    if (InterlockedExchange(&g_scope_play, want) != want) changed = ms_scope_compute(x, 1);
    ReleaseSRWLockShared(&g_scope_lock);
    ReleaseSRWLockExclusive(&g_build_lock);
    return changed;
}

int sh_modserve_scope_mode_get(void)
{
    return InterlockedCompareExchange(&g_scope_play, 0, 0) ? SH_MODSERVE_MODE_PLAY : SH_MODSERVE_MODE_EDITOR;
}

int sh_modserve_scope_relevant(void)
{
    const ms_index *x = g_index;
    int any;
    if (!InterlockedCompareExchange(&g_scope_on, 0, 0)) return 0;
    AcquireSRWLockShared(&g_scope_lock);
    any = g_scope_n > 0;
    ReleaseSRWLockShared(&g_scope_lock);
    /* Every admitted bundle is scoped now: a map serves only what it declares. */
    return any || (x && x->admitted > 0);
}

int sh_modserve_bundle_edits_live(const char *bundle_hash)
{
    sh_modserve_scope_info si;
    if (!sh_modserve_scope_enabled()) return 1;
    if (!bundle_hash || !sh_modserve_bundle_scope(bundle_hash, &si)) return 0;
    return si.listed || si.scope == SH_MODSERVE_SCOPE_ALWAYS;
}

int sh_modserve_bundle_scope(const char *bundle_hash, sh_modserve_scope_info *out)
{
    const ms_index *x = g_index;
    const ms_bundle *b;
    int at, k;
    LONG sv;
    if (out) memset(out, 0, sizeof *out);
    if (!x || !bundle_hash || (at = ms_bundle_by_hash(x, bundle_hash, 1)) < 0) return 0;
    b = &x->bundles[at];
    if (!out) return 1;
    out->scope = ms_scope_always(b) ? SH_MODSERVE_SCOPE_ALWAYS : SH_MODSERVE_SCOPE_MAP;
    out->automatic = b->scope_editor != NULL;
    out->rank = (int)InterlockedCompareExchange((volatile LONG *)&b->rank, 0, 0);
    out->listed = out->rank >= 0;
    out->inert = InterlockedCompareExchange((volatile LONG *)&b->inert, 0, 0) != 0;
    out->map_open = sh_modserve_scope_map_open();
    out->play = out->map_open && sh_modserve_scope_mode_get() == SH_MODSERVE_MODE_PLAY;
    out->newest = b->root_build;
    out->in_place = b->place != NULL;
    for (k = b->first_file; k < b->first_file + b->file_count; ++k) {
        const ms_file *f = &x->files[k];
        if (f->is_shadow && !f->is_restore && (f->served || f->alt_of >= 0)) out->shadows++;
        if (!f->is_shadow && !f->is_restore) out->names++;
        if (f->is_restore) out->surfaces++;
    }
    sv = InterlockedCompareExchange((volatile LONG *)&b->sv, 0, 0);
    out->shadows_live = (sv & (MS_SV_SHADOW | MS_SV_EDITOR)) != 0;
    out->names_live = (sv & MS_SV_NEW) != 0;
    out->surfaces_live = (sv & MS_SV_RESTORE) != 0;
    out->requirements = b->req_text && (b->req_ncvars || b->req_nact || b->req_ndeact);
    out->requirements_live = out->requirements && !b->req_lost &&
                             !InterlockedCompareExchange((volatile LONG *)&b->req_off, 0, 0);
    if (b->scope_editor) strncpy_s(out->editor_path, sizeof out->editor_path, b->scope_editor, _TRUNCATE);
    return 1;
}

/* A mod in a report a player reads (sh_mods map, doctor): its name, never
 * its salt or a hash. */
static void ms_who(const ms_bundle *b, char *out, size_t cap)
{
    _snprintf_s(out, cap, _TRUNCATE, "%s", ms_label(b));
}

/* 1 when the providers of winner `i`'s path carry more than one set of bytes. */
static int ms_path_differs(const ms_index *x, int i)
{
    const ms_file *f = &x->files[i];
    int k;
    for (k = 0; k < f->alt_count; ++k) {
        const ms_file *g = &x->files[x->alts[f->alt_first + k]];
        if (!f->blob || !g->blob || strcmp(f->blob, g->blob)) return 1;
    }
    return 0;
}

/* Per path several bundles provide with different bytes: which layer serves
 * it for the open map and why (at most 40 lines); paths several bundles carry
 * with the same bytes are counted. */
static int ms_layer_lines(const ms_index *x, sh_modserve_line_fn fn, void *context)
{
    char line[900];
    int i, lines = 0, shown = 0, declared = 0, editor = 0, game = 0, multi = 0, same = 0;
    for (i = 0; i < x->nfiles; ++i) {
        const ms_file *f = &x->files[i];
        int p, k, why;
        if (!f->served || !x->bundles[f->bundle].admitted) continue;
        p = ms_scoped(x, i);
        if (p < 0) game++;
        else if (x->bundles[x->files[p].bundle].rank >= 0) declared++;
        else editor++;
        if (!f->alt_count) continue;
        if (!ms_path_differs(x, i)) { same++; continue; }
        multi++;
        if (shown >= 40) continue;
        why = InterlockedCompareExchange((volatile LONG *)&f->scope_alt, 0, 0) == -1 ? MS_WHY_NONE : f->scope_why;
        if (!sh_modserve_scope_enabled()) why = -1;
        {
            char who[200], others[400], merged[400], tail[860];
            size_t at = 0, mt = 0;
            size_t blen = 0;
            int composed = p >= 0 && ms_body_of(&x->files[p], &blen) != NULL;
            others[0] = merged[0] = 0;
            for (k = -1; k < f->alt_count; ++k) {
                int c = k < 0 ? i : x->alts[f->alt_first + k], w;
                const ms_bundle *cb = &x->bundles[x->files[c].bundle];
                char one[200];
                if (c == p) continue;
                ms_who(cb, one, sizeof one);
                /* A composed text carries the entries of the other providers that serve for the listing. */
                if (composed && (why < 0 || ((cb->sv & ms_sv_class(&x->files[c])) && (cb->rank >= 0 || ms_scope_always(cb))))) {
                    w = _snprintf_s(merged + mt, sizeof merged - mt, _TRUNCATE, "%s%s", mt ? ", " : "", one);
                    if (w > 0) mt += (size_t)w;
                    continue;
                }
                w = _snprintf_s(others + at, sizeof others - at, _TRUNCATE, "%s%s", at ? ", " : "", one);
                if (w < 0) break;
                at += (size_t)w;
            }
            _snprintf_s(tail, sizeof tail, _TRUNCATE, "%s%s%s%s", mt ? "; with the entries of: " : "", merged,
                        at ? "; not served: " : "", others);
            if (p >= 0) {
                const ms_bundle *b = &x->bundles[x->files[p].bundle];
                const char *text = why < 0 ? "map scope is off" :
                                   composed && b->rank < 0 ? "editor-time content, composed with the other editor-time providers" :
                                   g_why_text[why];
                ms_who(b, who, sizeof who);
                if (b->rank >= 0)
                    _snprintf_s(line, sizeof line, _TRUNCATE, "serving: %s: %s, declared (row %ld): %s%s",
                                f->request, who, (long)b->rank + 1, text, tail);
                else
                    _snprintf_s(line, sizeof line, _TRUNCATE, "serving: %s: %s: %s%s", f->request, who, text, tail);
            } else
                _snprintf_s(line, sizeof line, _TRUNCATE, "serving: %s: the game (%s); not served: %s", f->request,
                            g_why_text[MS_WHY_NONE], others);
        }
        fn(context, line);
        lines++;
        shown++;
    }
    if (multi > shown) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "serving: %d more path(s) several bundles provide with different bytes",
                    multi - shown);
        fn(context, line);
        lines++;
    }
    if (same) {
        _snprintf_s(line, sizeof line, _TRUNCATE, "serving: %d path(s) several bundles carry with the same bytes; one copy "
                    "serves when any provider may", same);
        fn(context, line);
        lines++;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "serving: %d path(s) from bundles the open map declares, %d from undeclared bundles (Toybox names, "
                "surfaces, editor-time content), %d left to the game", declared, editor, game);
    fn(context, line);
    return lines + 1;
}

int sh_modserve_scope_report(sh_modserve_line_fn fn, void *context)
{
    const ms_index *x = g_index;
    char line[900];
    int i, k, lines = 0, on = sh_modserve_scope_enabled();
    if (!fn) return 0;
    for (i = 0; x && i < x->nbundles; ++i) {
        const ms_bundle *b = &x->bundles[i];
        sh_modserve_scope_info si;
        char what[160], who[300], state[400];
        int builds = 0, first = 1;
        if (!b->admitted || !sh_modserve_bundle_scope(b->hash, &si)) continue;
        /* One row per build of a salt: a header once, before its first build. */
        for (k = 0; k < x->nbundles; ++k) {
            if (!x->bundles[k].admitted || strcmp(x->bundles[k].id, b->id)) continue;
            builds++;
            if (k < i) first = 0;
        }
        if (builds > 1 && first) {
            _snprintf_s(line, sizeof line, _TRUNCATE, "scope: mod %s: %d builds installed (the root folder and maps' places); "
                        "each map serves the build it declares, and with no map open the root folder's", ms_label(b), builds);
            fn(context, line);
            lines++;
        }
        ms_who(b, who, sizeof who);
        _snprintf_s(what, sizeof what, _TRUNCATE, " (%d name%s, %d shadow%s, %d surface%s%s)", si.names, si.names == 1 ? "" : "s",
                    si.shadows, si.shadows == 1 ? "" : "s", si.surfaces, si.surfaces == 1 ? "" : "s",
                    si.requirements ? ", requirements" : "");
        if (!on) strcpy_s(state, sizeof state, "map scope is off: serves everything it carries");
        else if (si.listed) _snprintf_s(state, sizeof state, _TRUNCATE, "declared by the open map (row %d): serves everything it carries", si.rank + 1);
        else if (si.inert) strcpy_s(state, sizeof state, "inert: the open map declares another build of its salt");
        else if (si.scope == SH_MODSERVE_SCOPE_ALWAYS)
            _snprintf_s(state, sizeof state, _TRUNCATE, "editor-time content (%s): its editor tweaks serve in every map; "
                        "the rest only in maps that declare it", si.editor_path);
        else if (si.play) strcpy_s(state, sizeof state, "not declared by the map being played: serves nothing");
        else if (!si.newest)
            _snprintf_s(state, sizeof state, _TRUNCATE, "not declared%s: a build in %s; it serves only for that map",
                        si.map_open ? " by the open map" : " (no map is open)", b->place ? b->place : "a map's place");
        else
            _snprintf_s(state, sizeof state, _TRUNCATE, "not declared%s: its names serve for the Toybox and its surfaces "
                        "serve; its shadows do not", si.map_open ? " by the open map" : " (no map is open)");
        _snprintf_s(line, sizeof line, _TRUNCATE, "scope: %s%s: %s%s", who, what, state,
                    si.requirements && !si.listed && si.requirements_live ? "; its requirements stay live because it adds Toybox rows" : "");
        fn(context, line);
        lines++;
    }
    if (!on) { fn(context, "scope: map scope is off; every bundle serves in every map"); lines++; }
    if (x) lines += ms_layer_lines(x, fn, context);
    return lines;
}

void sh_modserve_set_scope_source(sh_modserve_scope_source_fn fn) { g_scope_source = fn; }

int sh_modserve_scope_source(int when)
{
    sh_modserve_scope_source_fn fn = g_scope_source;
    int moved = 0, r;
    /* The Play entry: from here undeclared bundles serve nothing. */
    if (when == SH_MODSERVE_AT_PLAY && sh_modserve_scope_mode_get() != SH_MODSERVE_MODE_PLAY) {
        InterlockedExchange(&g_scope_play, 1);
        moved = 1;
    }
    r = fn ? fn(when) : 0;
    if (moved && r <= 0) {
        ms_index *x;
        AcquireSRWLockExclusive(&g_build_lock);
        x = (ms_index *)g_index;
        AcquireSRWLockShared(&g_scope_lock);
        r = ms_scope_compute(x, 1);
        ReleaseSRWLockShared(&g_scope_lock);
        ReleaseSRWLockExclusive(&g_build_lock);
    }
    return r;
}

/* ------------------------------------------------------------------ per-build bank stems */

static int ms_ci_eq(const char *a, const char *b, size_t n)
{
    return _strnicmp(a, b, n) == 0;
}

char *sh_modserve_bank_rename_text(const char *text, size_t len, const char *stem, const char *renamed, size_t *out_len)
{
    size_t sn = stem ? strlen(stem) : 0, rn = renamed ? strlen(renamed) : 0, i = 0, out_n = 0, cap;
    char *out = NULL;
    int changed = 0;
    if (out_len) *out_len = 0;
    if (!text || !sn || !rn) return NULL;
    cap = len + 64;
    out = (char *)malloc(cap + 1);
    if (!out) return NULL;
    while (i < len) {
        if (text[i] == '"') {
            size_t e = i + 1, body, k;
            while (e < len && text[e] != '"' && text[e] != '\n') { if (text[e] == '\\' && e + 1 < len) e++; e++; }
            body = e - (i + 1);
            /* the stem is the whole string, the string less ".bnk", or its last part before ".bnk" */
            for (k = 0; e < len && text[e] == '"' && k < 1; k++) {
                const char *s = text + i + 1;
                size_t at = (size_t)-1;
                if (body == sn && ms_ci_eq(s, stem, sn)) at = 0;
                else if (body == sn + 4 && ms_ci_eq(s, stem, sn) && ms_ci_eq(s + sn, ".bnk", 4)) at = 0;
                else if (body > sn + 4 && ms_ci_eq(s + body - 4, ".bnk", 4) && ms_ci_eq(s + body - 4 - sn, stem, sn) &&
                         s[body - 5 - sn] == '/')
                    at = body - 4 - sn;
                if (at != (size_t)-1) {
                    size_t need = out_n + body + rn + 2;
                    if (need + (len - e) > cap) {
                        char *g;
                        cap = need + (len - e) + 64;
                        g = (char *)realloc(out, cap + 1);
                        if (!g) { free(out); return NULL; }
                        out = g;
                    }
                    out[out_n++] = '"';
                    memcpy(out + out_n, s, at); out_n += at;
                    memcpy(out + out_n, renamed, rn); out_n += rn;
                    memcpy(out + out_n, s + at + sn, body - at - sn); out_n += body - at - sn;
                    out[out_n++] = '"';
                    i = e + 1;
                    changed = 1;
                    goto next;
                }
            }
            if (out_n + (e + 1 - i) > cap) {
                char *g;
                cap = out_n + (e + 1 - i) + (len - e) + 64;
                g = (char *)realloc(out, cap + 1);
                if (!g) { free(out); return NULL; }
                out = g;
            }
            k = e < len ? e + 1 : e;
            memcpy(out + out_n, text + i, k - i);
            out_n += k - i;
            i = k;
            continue;
        }
        if (out_n + 1 > cap) {
            char *g;
            cap = cap * 2 + 64;
            g = (char *)realloc(out, cap + 1);
            if (!g) { free(out); return NULL; }
            out = g;
        }
        out[out_n++] = text[i++];
next:   ;
    }
    if (!changed) { free(out); return NULL; }
    out[out_n] = 0;
    if (out_len) *out_len = out_n;
    return out;
}

int sh_modserve_bank_patch_id(unsigned char *bytes, size_t len, const char *stem)
{
    uint32_t id;
    if (!bytes || len < 16 || memcmp(bytes, "BKHD", 4) || !stem) return 0;
    id = sh_mb_fnv1_lower(stem, strlen(stem));
    memcpy(bytes + 12, &id, 4);
    return 1;
}

/* Create every directory of `path` (a file path) below the mods root. */
static int ms_mkdirs_for(const char *path)
{
    char dir[MS_PATH_MAX];
    wchar_t wide[MS_PATH_MAX + 8];
    char *p;
    strncpy_s(dir, sizeof dir, path, _TRUNCATE);
    p = strrchr(dir, '\\');
    if (!p) return 0;
    *p = 0;
    for (p = dir + 3; *p; ++p) {
        if (*p != '\\') continue;
        *p = 0;
        if (ms_wide(dir, wide, sizeof wide / sizeof wide[0])) CreateDirectoryW(wide, NULL);
        *p = '\\';
    }
    return ms_wide(dir, wide, sizeof wide / sizeof wide[0]) && (CreateDirectoryW(wide, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
}

/* Write `bytes` at `path` unless it already holds exactly them. 1 the file holds them. */
static int ms_write_served(const char *path, const unsigned char *bytes, size_t len)
{
    size_t have_len = 0;
    unsigned char *have = ms_read_whole(path, (size_t)SH_MODSERVE_MAX_FILE, &have_len);
    wchar_t wide[MS_PATH_MAX + 8];
    HANDLE h;
    DWORD put = 0;
    int same = have && have_len == len && !memcmp(have, bytes, len), ok;
    if (have) HeapFree(GetProcessHeap(), 0, have);
    if (same) return 1;
    if (!ms_mkdirs_for(path) || !ms_wide(path, wide, sizeof wide / sizeof wide[0])) return 0;
    h = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(h, bytes, (DWORD)len, &put, NULL) && put == (DWORD)len;
    CloseHandle(h);
    return ok;
}

/* Serve `f` from rewritten bytes, written under mods\.served\ by their own
 * hash (ONE-SHAPE rule 4: no folder is named by a build): a bank as
 * banks\<renamed stem>.bnk (its stem already carries the bank's hash), any
 * other file as files\<sha16>.<extension>. Size and blob follow. */
static int ms_serve_rewritten(ms_index *x, ms_file *f, const unsigned char *bytes, size_t len, const char *bank_stem)
{
    char path[MS_PATH_MAX], blob[SH_MB_HASH_HEX + 1], *p;
    const char *dot = strrchr(f->request, '.'), *slash = strrchr(f->request, '/');
    sh_mb_sha256_hex(bytes, len, blob);
    if (bank_stem) _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.served\\banks\\%s.bnk", x->root, bank_stem);
    else _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.served\\files\\%.16s%s", x->root, blob,
                     dot && (!slash || dot > slash) ? dot : "");
    for (p = path; *p; ++p) if (*p == '/') *p = '\\';
    if (!ms_write_served(path, bytes, len)) return 0;
    f->disk = ms_strdup(x, path);
    f->blob = ms_strdup(x, blob);
    f->size = (uint64_t)len;
    return f->disk && f->blob;
}

#define MS_BANKS_MAX 16
static void ms_bank_stems(ms_index *x)
{
    int bi;
    for (bi = 0; bi < x->nbundles; ++bi) {
        ms_bundle *b = &x->bundles[bi];
        char stems[MS_BANKS_MAX][SH_MB_BANK_STEM_MAX + 1], renamed[MS_BANKS_MAX][SH_MB_BANK_STEM_MAX + 20];
        int n = 0, i, k;
        if (!b->admitted || !b->hash || strlen(b->hash) != 64 || !b->id) continue;
        for (i = b->first_file; i < b->first_file + b->file_count && i < x->nfiles; ++i) {
            ms_file *f = &x->files[i];
            char stem[SH_MB_BANK_STEM_MAX + 1], req[SH_MB_REQUEST_MAX + 1];
            unsigned char *bytes;
            size_t len = 0;
            if (f->stock || !sh_mb_bank_stem(f->request, stem, sizeof stem)) continue;
            if (n >= MS_BANKS_MAX) { ms_log("bundle '%s': more than %d banks; the rest keep their names", b->id, MS_BANKS_MAX); break; }
            if (!f->blob || strlen(f->blob) != 64) {
                ms_log("bundle '%s': %s has no recorded hash; it keeps its name", b->id, f->request);
                continue;
            }
            /* The bank's own hash names it (ONE-SHAPE plan R8): two builds
             * with one bank's bytes share it, and Wwise loads it once. */
            _snprintf_s(renamed[n], sizeof renamed[0], _TRUNCATE, "%s.%.16s", stem, f->blob);
            bytes = ms_read_whole(f->disk, (size_t)SH_MODSERVE_MAX_FILE, &len);
            if (!bytes || !sh_modserve_bank_patch_id(bytes, len, renamed[n])) {
                if (bytes) HeapFree(GetProcessHeap(), 0, bytes);
                ms_log("bundle '%s': %s cannot be served under a stem of its own (unreadable or not a BKHD bank); it keeps "
                       "its name", b->id, f->request);
                continue;
            }
            _snprintf_s(req, sizeof req, _TRUNCATE, "sound/soundbanks/pc/%s.bnk", renamed[n]);
            if (!ms_serve_rewritten(x, f, bytes, len, renamed[n])) {
                HeapFree(GetProcessHeap(), 0, bytes);
                ms_log("bundle '%s': %s could not be written under .served; it keeps its name", b->id, f->request);
                continue;
            }
            HeapFree(GetProcessHeap(), 0, bytes);
            f->request = ms_strdup(x, req);
            if (!f->request) continue;
            if (!f->is_decl) f->pname = (char *)f->request;
            strcpy_s(stems[n], sizeof stems[0], stem);
            ms_log("bundle '%s': bank %s.bnk served as %s.bnk (a stem of its own to Wwise)", b->id, stem, renamed[n]);
            n++;
        }
        if (!n) continue;
        /* The bundle's served decls naming a bank follow the rename. */
        for (i = b->first_file; i < b->first_file + b->file_count && i < x->nfiles; ++i) {
            ms_file *f = &x->files[i];
            unsigned char *bytes;
            char *text = NULL;
            size_t len = 0, tl = 0;
            int changed = 0;
            if (!f->is_decl || f->stock) continue;
            bytes = ms_read_whole(f->disk, 8u << 20, &len);
            if (!bytes) continue;
            text = (char *)malloc(len + 1);
            if (text) { memcpy(text, bytes, len); text[len] = 0; tl = len; }
            HeapFree(GetProcessHeap(), 0, bytes);
            for (k = 0; text && k < n; ++k) {
                size_t nl = 0;
                char *next = sh_modserve_bank_rename_text(text, tl, stems[k], renamed[k], &nl);
                if (next) { free(text); text = next; tl = nl; changed = 1; }
            }
            if (changed && text) {
                if (ms_serve_rewritten(x, f, (const unsigned char *)text, tl, NULL))
                    ms_log("bundle '%s': %s names its bank; served with the bank's own stem", b->id, f->request);
                else ms_log("bundle '%s': %s names its bank but could not be rewritten", b->id, f->request);
            }
            free(text);
        }
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
    ms_scan_folder(x, mods_root, "", 0, NULL);
    ms_one_per_area(x);
    ms_load_editor_only(x, mods_root);
    ms_bank_stems(x);
    ms_mark_edits(x);
    if (!ms_resolve(x) || !ms_registry_files(x) || !ms_build_tables(x)) {
        ReleaseSRWLockExclusive(&g_build_lock);
        ms_log("index refused: out of memory; the previous table stays");
        ms_index_free(x);
        return -1;
    }
    ms_resolve_needs(x);
    ms_mark_editor_closure(x);
    for (i = 0; i < x->nbundles; ++i) x->admitted += x->bundles[i].admitted;
    ms_registry_relevant(x);
    /* The build of each salt that serves for the Toybox with no map
     * declaring it: the root folder's (ONE-SHAPE rule 4; ms_one_per_area left
     * one per salt). A salt held only in maps' places offers no rows. */
    for (i = 0; i < x->nbundles; ++i) {
        ms_bundle *b = &x->bundles[i];
        b->root_build = b->admitted && !b->place;
        b->rank = -1;
    }
    x->build_ms = ms_now_ms() - t0;
    /* Previously registered decls keep their state across a rescan. */
    old = g_index;
    /* Requirement conflicts, the log lines, and what is live already. */
    ms_req_index(x, old);
    if (old) {
        for (i = 0; i < x->nfiles; ++i) {
            ms_file *f = &x->files[i];
            int o;
            if (!f->is_decl || !f->served) continue;
            o = ms_find(old->slots, old->mask, f->request);
            if (o >= 0 && old->files[o].reg_state) f->reg_state = old->files[o].reg_state;
            if (o >= 0) f->plan_seen = old->files[o].plan_seen;
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
    /* The new table's map scope, from the current listing, before the diff
     * decides what the engine must read again. */
    AcquireSRWLockShared(&g_scope_lock);
    (void)ms_scope_compute(x, 0);
    ReleaseSRWLockShared(&g_scope_lock);
    ms_scope_log(x, old);
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
    char root[MS_PATH_MAX];
    /* The store's own root (modstore.c), in UTF-8 as the store writes it:
     * the ANSI code page cannot spell every profile path. */
    if (!sh_ms_root(root, sizeof root)) {
        ms_log("index empty: the user-data folder is unavailable");
        return 0;
    }
    /* The product's index is map-scoped from its first table: no map is open
     * at boot, so no map-scoped shadow serves before a map lists it. */
    InterlockedExchange(&g_scope_on, 1);
    return sh_modserve_index_build(root);
}

int sh_modserve_index_rescan(void)
{
    char root[MS_PATH_MAX];
    AcquireSRWLockShared(&g_build_lock);
    strncpy_s(root, sizeof root, g_last_root, _TRUNCATE);
    ReleaseSRWLockShared(&g_build_lock);
    if (!root[0]) return sh_modserve_index_boot();
    return sh_modserve_index_build(root);
}

int sh_modserve_active(void)
{
    const ms_index *x = g_index;
    return x && x->served_files > 0;   /* a registry's synthetic file is not a mod's */
}

/* ------------------------------------------------------------------ serve */

static const ms_file *ms_lookup_file_ex(const ms_index **out_index, const char *request, int scoped)
{
    const ms_index *x = g_index;
    int f;
    if (!x || !x->nkeys || !request) return NULL;
    __try { f = ms_find(x->slots, x->mask, request); }
    __except (EXCEPTION_EXECUTE_HANDLER) { f = -1; }
    if (f < 0 || x->files[f].withdrawn) return NULL;
    /* A registry's synthetic file has bytes only for a listing. */
    if (x->files[f].registry && (!scoped || !InterlockedCompareExchange(&g_scope_on, 0, 0))) return NULL;
    /* Map scope: a shadow path whose providers the current map does not list
     * is withdrawn; one it lists serves when the winner's bundle is not. */
    if (scoped && (f = ms_scoped(x, f)) < 0) return NULL;
    if (out_index) *out_index = x;
    return &x->files[f];
}

static const ms_file *ms_lookup_file(const ms_index **out_index, const char *request)
{
    return ms_lookup_file_ex(out_index, request, 1);
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

static int ms_lookup_hit(const char *request, sh_modserve_hit *out, int scoped)
{
    const ms_index *x = NULL;
    const ms_file *f = ms_lookup_file_ex(&x, request, scoped);
    if (!f) return 0;
    if (out) {
        out->request = f->request;
        out->disk_path = f->disk;
        size_t blen = 0;
        out->size = ms_body_of(f, &blen) ? (uint64_t)blen : f->size;
        out->bundle_id = x->bundles[f->bundle].id;
        out->bundle_hash = x->bundles[f->bundle].hash;
        out->is_decl = f->is_decl;
        out->is_shadow = f->is_shadow;
        out->is_stock = f->stock != NULL;
        out->stock_source = f->stock;
    }
    return 1;
}

int sh_modserve_lookup(const char *request, sh_modserve_hit *out) { return ms_lookup_hit(request, out, 1); }
int sh_modserve_lookup_indexed(const char *request, sh_modserve_hit *out) { return ms_lookup_hit(request, out, 0); }

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

static sh_modserve_serve_fn volatile g_serve_listener;

void sh_modserve_set_serve_listener(sh_modserve_serve_fn fn)
{
    InterlockedExchangePointer((PVOID volatile *)&g_serve_listener, (PVOID)fn);
}

int sh_modserve_serve_identity(const char *request, const char **bundle_hash, const char **file_hash,
                               const unsigned char **body, size_t *body_len, uint64_t *size)
{
    const ms_index *x = NULL;
    const ms_file *f = ms_lookup_file(&x, request);
    const unsigned char *b;
    size_t bl = 0;
    if (bundle_hash) *bundle_hash = NULL;
    if (file_hash) *file_hash = NULL;
    if (body) *body = NULL;
    if (body_len) *body_len = 0;
    if (size) *size = 0;
    if (!f) return 0;
    b = ms_body_of(f, &bl);
    if (bundle_hash) *bundle_hash = x->bundles[f->bundle].hash;
    if (b) { if (body) *body = b; if (body_len) *body_len = bl; if (size) *size = bl; }
    else { if (file_hash) *file_hash = f->blob; if (size) *size = f->size; }
    return 1;
}

int sh_modserve_open(const char *request, unsigned char **bytes, size_t *length)
{
    sh_modserve_serve_fn listen;
    const ms_index *x = NULL;
    const ms_file *f;
    unsigned char *buf = NULL;
    const unsigned char *body;
    size_t len = 0, body_len = 0;
    int first;
    if (bytes) *bytes = NULL;
    if (length) *length = 0;
    if (!bytes || !length) return 0;
    f = ms_lookup_file(&x, request);
    if (!f) return 0;
    body = ms_body_of(f, &body_len);
    if (body) {
        /* A composed text (ms_resolve, ms_scope_compute): the index holds the bytes. */
        len = body_len;
        buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, len ? len : 1u);
        if (!buf) return -1;
        memcpy(buf, body, len);
    } else if (InterlockedCompareExchange(&g_cache_enabled, 0, 0)) buf = ms_cache_copy(f, &len);
    if (buf && !body) InterlockedIncrement(&g_cache_hits);
    else if (!buf) {
        buf = ms_file_raw(f, &len);
        if (!buf) {
            InterlockedIncrement(&g_open_failures);
            if (!InterlockedExchange((volatile LONG *)&f->fail_logged, 1)) {
                if (f->stock)
                    ms_log("'%s' from %s points at installed content '%s' (sha256 %.16s), which could not be read "
                           "with that hash; the engine's own resolution is used", f->request, ms_label(&x->bundles[f->bundle]), f->stock, f->blob);
                else
                    ms_log("'%s' from %s could not be read at its indexed size (%llu bytes) from '%s'; "
                           "the engine's own resolution is used", f->request, ms_label(&x->bundles[f->bundle]), (unsigned long long)f->size, f->disk);
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
    if (first && ms_req_needs(ms_gate_of(x, f)) &&
        !InterlockedCompareExchange((volatile LONG *)&ms_gate_of(x, f)->req_live, 0, 0))
        InterlockedExchange((volatile LONG *)&f->opened_ungated, 1);
    if (first &&
        (!strncmp(f->request, "generated/renderprogs/", 22) || !strncmp(f->request, "generated/spirv/", 16) ||
         !strncmp(f->request, "generated/decls/renderprog/", 27)) &&
        InterlockedIncrement(&g_renderprog_logged) <= 64)
        ms_log("renderprog: served %s (%s, %zu bytes%s)", f->request, ms_label(&x->bundles[f->bundle]), len, f->stock ? ", installed content" : "");
    InterlockedIncrement(&g_opens_served);
    if (f->is_shadow && !InterlockedExchange((volatile LONG *)&f->shadow_logged, 1))
        ms_log("shadow '%s' served from %s (%zu bytes%s)", f->request, ms_label(&x->bundles[f->bundle]), len, body ? ", composed with the other providers' list entries" : "");
    listen = (sh_modserve_serve_fn)InterlockedCompareExchangePointer((PVOID volatile *)&g_serve_listener, NULL, NULL);
    if (listen) listen(f->request, x->bundles[f->bundle].hash, body ? NULL : f->blob, buf, len);
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
    if (x) for (i = 0; i < x->nfiles; ++i) { x->files[i].reg_state = 0; x->files[i].plan_seen = 0; }
}
#endif

/* ------------------------------------------------------------------ plan */

static int ms_need_blocks(const char *name)
{
    return !strncmp(name, "mods/", 5) || strstr(name, "/mods/") != NULL;
}

/* A decl a plan may register: every decl a bundle provides except shadows,
 * edited copies and surfaces (a shadow is served by the provider when the
 * game parses it, never reloaded by a plan). A surface (a stock pointer at the game's own path: the game's
 * bytes under the game's name, sh_mb_stock_restore) is never planned by any
 * pass: the provider serves it whenever the game parses it, at the game's
 * own time and level, so no pass registers or re-parses it (ADD-L11). */
static int ms_plannable(const ms_index *x, const ms_file *f)
{
    (void)x;
    return f->is_decl && !f->edit && !ms_surface(f) && !f->is_shadow;
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
         * what its parse points at must outlive the purges around Play. A
         * decl an editor row needs is kept by whichever pass registers it
         * first (a map pass, or a refresh after an in-session install), as
         * the boot editor pass would have kept it. */
        s->persist = persist_sedefs ? 1 : f->editor_closure ? 2 : 0;
        s->shadow = f->is_shadow;   /* surfaces are never planned (ms_plannable) */
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
    /* With map scope on, the rows whose path the listing serves: the root
     * folder's build of each undeclared salt, the declared build of a
     * declared one (a place build serves its names only for its map). */
    for (i = 0; i < x->nfiles; ++i)
        mark[i] = (unsigned char)(x->files[i].served && x->files[i].is_sedef && ms_plannable(x, &x->files[i]) &&
                                  ms_scoped(x, i) >= 0);
    r = ms_plan_from(x, mark, 1, out);
    free(mark);
    return r;
}

/* The bundle at `hash`. One build can sit in several areas (the root and a
 * map's place, or two places): the open map's place first unless the map
 * is the author's own (sh_modserve_scope_set_place), then the root, then
 * any place, in index order; the same hash is the same files, so this only
 * decides which folder's copy answers. */
static int ms_bundle_by_hash(const ms_index *x, const char *hash, int admitted_only)
{
    int i, round, own;
    char place[SH_MODSERVE_PLACE_CAP];
    if (!x || !hash) return -1;
    ms_scope_place(place, sizeof place, &own);
    for (round = own || !place[0] ? 1 : 0; round < 3; ++round)
        for (i = 0; i < x->nbundles; ++i) {
            const ms_bundle *b = &x->bundles[i];
            if (!b->hash || _stricmp(b->hash, hash) || (admitted_only && !b->admitted)) continue;
            if (round == 0 && (!b->place || _stricmp(b->place, place))) continue;
            if (round == 1 && b->place) continue;
            return i;
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
        int b = ms_bundle_by_hash(x, bundle_hashes[i], 1), k;
        if (b < 0) { missing++; continue; }
        for (k = x->bundles[b].first_file; k < x->bundles[b].first_file + x->bundles[b].file_count; ++k) {
            const ms_file *f = &x->files[k];
            int use = k;
            if (!f->served) {
                /* Another build of the same key is the path's static winner:
                 * the step registers the name, and the provider serves it the
                 * bytes of the build this map declares (ms_scope_compute). */
                int w = ms_find(x->slots, x->mask, f->request);
                if (w < 0 || strcmp(x->bundles[x->files[w].bundle].id, x->bundles[b].id)) continue;
                use = w;
            }
            f = &x->files[use];
            if (f->served && ms_plannable(x, f) && !f->is_sedef) mark[use] = 1;
        }
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
    const unsigned char *body;
    size_t blen = 0;
    *len = 0;
    if ((body = ms_body_of(f, &blen)) != NULL) {
        b = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, blen ? blen : 1u);
        if (b) { memcpy(b, body, blen); *len = blen; }
        return b;
    }
    return ms_file_raw(f, len);
}

/* 1 with the first model an animWeb's models or modelInfos block names that
 * no served bundle file provides as an md6Def and the engine cannot find; 0
 * when every model resolves (or the text cannot be read, or the engine cannot
 * tell). The engine's parse looks each model up with DeclFind and no
 * makeDefault, and a miss throws an idLexerException that the parse catches
 * itself: the web keeps what it had parsed so far (nothing past the props
 * block), is not marked failed, and the load reports success (13954591, both
 * renderers). `strict` is a load now: the engine's own lookup (decl_found)
 * decides, since the game archives also hold campaign md6Defs SnapMap cannot
 * load (issue 132: designsystems/med_station naming the campaign
 * wallhealthstation.md6). Otherwise (a stock shadow the engine parses later,
 * on demand) a model the registry or the game archives know counts as
 * resolved, since the registry does not know a stock decl whose source is not
 * registered yet (the player hands' md6Def at map activation). */
static int ms_animweb_unresolved(const ms_index *x, const ms_file *f, const sh_modserve_engine_ops *ops,
                                 int strict, char *model, size_t cap)
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
        /* modelInfos { "<md6>" "<alias>" ... } and models { "<md6>" ... } */
        int pairs;
        if (t.kind != MC_WORD) continue;
        if (t.n == 10 && !_strnicmp(t.p, "modelInfos", 10)) pairs = 1;
        else if (t.n == 6 && !_strnicmp(t.p, "models", 6)) pairs = 0;
        else continue;
        if (mc_next(&l).kind != MC_LB) continue;
        for (;;) {
            mc_tok m = mc_next(&l), v;
            char name[MS_NAME_MAX], key[MS_NAME_MAX + 8];
            int p, engine;
            if (m.kind == MC_RB || m.kind == MC_END || m.kind == MC_BAD) break;
            if (m.kind != MC_STR) continue;
            if (pairs) {
                v = mc_peek(&l);
                if (v.kind != MC_STR) continue;
                mc_next(&l);
            }
            if (m.n < 3 || m.n - 2 >= sizeof name) continue;
            memcpy(name, m.p + 1, m.n - 2);
            name[m.n - 2] = 0;
            ms_lower(name);
            _snprintf_s(key, sizeof key, _TRUNCATE, "md6def\x01%s", name);
            p = ms_find(x->pslots, x->pmask, key);
            if (p >= 0 && x->files[p].served && !x->files[p].withdrawn) continue;
            engine = ops->decl_found ? ops->decl_found(ops->context, "md6Def", name) : -1;
            if (engine == 1) continue;
            if (engine != 0 || !strict) {
                if (ops->source_registered(ops->context, "md6Def", name) != 0) continue;
                if (ops->stock_exists) {
                    char request[MS_NAME_MAX + 32];
                    _snprintf_s(request, sizeof request, _TRUNCATE, "generated/decls/md6def/%s.decl", name);
                    if (ops->stock_exists(ops->context, request) != 0) continue;
                }
            }
            strncpy_s(model, cap, name, _TRUNCATE);
            found = 1;
            break;
        }
    }
    HeapFree(GetProcessHeap(), 0, text);
    return found;
}

int sh_modserve_animweb_check(const char *request, const sh_modserve_engine_ops *ops, char *model, size_t cap)
{
    const ms_index *x = NULL;
    const ms_file *f;
    if (!model || !cap) return -1;
    model[0] = 0;
    if (!ops || !ops->source_registered) return -1;
    f = ms_lookup_file(&x, request);
    if (!f || !f->is_decl || !f->ptype || _stricmp(f->ptype, "animWeb")) return -1;
    return ms_animweb_unresolved(x, f, ops, 1, model, cap);
}

int sh_modserve_withdraw(const char *request, const char *type, const char *name, const char *why)
{
    const ms_index *x = NULL;
    ms_file *f = (ms_file *)ms_lookup_file(&x, request);
    const char *bundle;
    if (!f) return 0;
    bundle = x->bundles[f->bundle].id;
    InterlockedExchange(&f->withdrawn, 1);
    ms_log("withdrawn: %s '%s' (%s): %s", type ? type : "?", name ? name : request, bundle, why ? why : "");
    ms_console("%s: %s '%s' not loaded: %s", bundle, type ? type : "decl", name ? name : request, why ? why : "");
    return 1;
}

int sh_modserve_error_names_decl(const char *text, const char *type, const char *name)
{
    char hay[1024], needle[MS_NAME_MAX + 48];
    size_t i;
    if (!text || !type || !name || !text[0]) return 0;
    if (_snprintf_s(needle, sizeof needle, _TRUNCATE, "decls/%s/%s.decl", type, name) < 0) return 0;
    for (i = 0; needle[i]; ++i) needle[i] = needle[i] == '\\' ? '/' : (char)ms_fold(needle[i]);
    for (i = 0; i + 1 < sizeof hay && text[i]; ++i) hay[i] = text[i] == '\\' ? '/' : (char)ms_fold(text[i]);
    hay[i] = 0;
    return strstr(hay, needle) != NULL;
}

/* ------------------------------------------------------------------ resource watermark */

#define MS_RL_NAME    0x10
#define MS_RL_NEXT    0x18
#define MS_RL_ENTRIES 0x20
#define MS_RL_COUNT   0x28
#define MS_RL_TYPE    0x08
#define MS_RES_LEVEL  0x28
#define MS_RES_NAME   0x08
#define MS_RES_STATE  0x2c
#define MS_RES_FAILED  0x01   /* +0x2c bit 0: the load failed */
#define MS_RES_DEFAULT 0x04   /* +0x2c bit 2: the engine made a default (a failed read, GenericLoad) */
#define MS_RL_MAX_LISTS   4096
#define MS_RL_MAX_ENTRIES (1 << 22)

static int ms_uptr_cmp(const void *a, const void *b)
{
    uintptr_t x = *(const uintptr_t *)a, y = *(const uintptr_t *)b;
    return x < y ? -1 : x > y;
}

/* Visit every resource: fn(context, list, resource). 1 when the walk completed. */
typedef void (*ms_res_fn)(void *context, const unsigned char *list, unsigned char *res);
/* The engine's resource lock (report ADD-G3): list add, remove and find run
 * under one CRITICAL_SECTION, and list add swap-removes and grows the entry
 * array, so a walk takes it. `g_res_lock` is the address of the static struct
 * whose first qword is the CRITICAL_SECTION pointer (VK RVA 0x5D79238, GL
 * 0x467E8B8 on 13954591); the section is recursive on one thread. */
static void *volatile g_res_lock;

void sh_modserve_set_resource_lock(void *lock) { InterlockedExchangePointer((PVOID volatile *)&g_res_lock, lock); }

static CRITICAL_SECTION *ms_res_lock(void)
{
    void *lk = InterlockedCompareExchangePointer((PVOID volatile *)&g_res_lock, NULL, NULL);
    CRITICAL_SECTION *cs = NULL;
    if (!lk) return NULL;
    __try { cs = *(CRITICAL_SECTION *const *)lk; } __except (EXCEPTION_EXECUTE_HANDLER) { cs = NULL; }
    return cs;
}

static int ms_res_walk(const void *head_slot, ms_res_fn fn, void *context, unsigned *lists)
{
    int ok = 0;
    unsigned nl = 0;
    CRITICAL_SECTION *cs;
    if (lists) *lists = 0;
    if (!head_slot) return 0;
    cs = ms_res_lock();
    if (cs) EnterCriticalSection(cs);
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
    if (cs) LeaveCriticalSection(cs);
    if (lists) *lists = nl;
    return ok;
}

#define MS_KEPT_MAX  (1u << 20)
#define MS_PAIRS_MAX (1u << 20)
typedef struct ms_aset { uintptr_t *slot; size_t cap, n; } ms_aset;
typedef struct ms_pair { uintptr_t parent, child; } ms_pair;
static ms_aset g_kept;
static ms_pair *g_pairs;
static size_t g_pairs_cap, g_pairs_n;
static int g_attrib_full;
static unsigned g_attrib_raised_at_load;

static size_t ms_aset_at(const uintptr_t *slot, size_t cap, uintptr_t key)
{
    size_t i = (size_t)((key >> 4) * 0x9E3779B97F4A7C15ull) & (cap - 1);
    while (slot[i] && slot[i] != key) i = (i + 1) & (cap - 1);
    return i;
}

static int ms_aset_has(const ms_aset *s, uintptr_t key)
{
    return key && s->cap && s->slot[ms_aset_at(s->slot, s->cap, key)] == key;
}

/* 1 added, 0 already there, -1 the bound or memory refused it. */
static int ms_aset_add(ms_aset *s, uintptr_t key, size_t max)
{
    size_t i;
    if (!key) return 0;
    if ((s->n + 1) * 2 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 4096, j;
        uintptr_t *slot;
        if (s->n >= max) return ms_aset_has(s, key) ? 0 : -1;
        slot = (uintptr_t *)calloc(cap, sizeof *slot);
        if (!slot) return ms_aset_has(s, key) ? 0 : -1;
        for (j = 0; j < s->cap; ++j)
            if (s->slot[j]) slot[ms_aset_at(slot, cap, s->slot[j])] = s->slot[j];
        free(s->slot);
        s->slot = slot;
        s->cap = cap;
    }
    i = ms_aset_at(s->slot, s->cap, key);
    if (s->slot[i]) return 0;
    s->slot[i] = key;
    s->n++;
    return 1;
}

static void ms_aset_free(ms_aset *s)
{
    free(s->slot);
    memset(s, 0, sizeof *s);
}

/* Resources whose last load ran with a map heap selected (sh_modserve_attrib_enter
 * with heap != 0): what the load allocated with the current heap (an MD6
 * skeleton's joints, VK 0x1416d0770 at +0x40) belongs to that heap, which the
 * next map load releases whatever the resource's level (report F9: the
 * chainsaw gore skeleton raised by name kept its object and lost its joints,
 * G1 at 0x16f7f0b). No keep raises one; the purge and the next load recreate
 * it whole. A load in process heap 0 clears the mark. */
static ms_aset g_mapheap;
static unsigned g_left_n;
static char g_left_first[300];

/* Linear-probing delete: shift the run back over the hole. */
static void ms_aset_del(ms_aset *s, uintptr_t key)
{
    size_t i, j, k;
    if (!key || !s->cap) return;
    i = ms_aset_at(s->slot, s->cap, key);
    if (s->slot[i] != key) return;
    s->slot[i] = 0;
    s->n--;
    for (j = (i + 1) & (s->cap - 1); s->slot[j]; j = (j + 1) & (s->cap - 1)) {
        k = (size_t)((s->slot[j] >> 4) * 0x9E3779B97F4A7C15ull) & (s->cap - 1);
        if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) {
            s->slot[i] = s->slot[j];
            s->slot[j] = 0;
            i = j;
        }
    }
}

/* 1 when a keep may raise `res`; 0 (counted, named) when the map's heap owns it. */
static int ms_keepable(const unsigned char *res)
{
    const char *name;
    size_t at;
    if (!ms_aset_has(&g_mapheap, (uintptr_t)res)) return 1;
    g_left_n++;
    name = *(const char *const *)(res + MS_RES_NAME);
    at = strlen(g_left_first);
    if (name && at + strlen(name) + 3 < sizeof g_left_first)
        _snprintf_s(g_left_first + at, sizeof g_left_first - at, _TRUNCATE, "%s%s", at ? ", " : "", name);
    return 0;
}

unsigned sh_modserve_left_to_map(char *names, size_t cap)
{
    unsigned n = g_left_n;
    if (names && cap) strncpy_s(names, cap, g_left_first, _TRUNCATE);
    g_left_n = 0;
    g_left_first[0] = 0;
    return n;
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
    if (*(unsigned *)(res + MS_RES_LEVEL) == 4) sh_modserve_kept_add(res);
    if (*(unsigned *)(res + MS_RES_LEVEL) != 1 || !ms_keepable(res)) return;
    *(unsigned *)(res + MS_RES_LEVEL) = 4;
    sh_modserve_kept_add(res);
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
/* Collected under the lock; the callback (a reload, a re-parse) runs after it
 * is released, since GenericLoad under the lock was not analysed (ADD-G3). */
typedef struct ms_res_item { const char *list; void *res; const char *name; } ms_res_item;
typedef struct ms_res_items { ms_res_item *v; size_t n, cap; int oom; } ms_res_items;
static void ms_each_res_one(void *context, const unsigned char *list, unsigned char *res)
{
    ms_res_items *c = (ms_res_items *)context;
    const char *ln = *(const char *const *)(list + MS_RL_NAME), *rn = *(const char *const *)(res + 8);
    if (!rn || c->oom) return;
    if (c->n == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 1024;
        ms_res_item *v = (ms_res_item *)realloc(c->v, cap * sizeof *v);
        if (!v) { c->oom = 1; return; }
        c->v = v;
        c->cap = cap;
    }
    c->v[c->n].list = ln;
    c->v[c->n].res = res;
    c->v[c->n].name = rn;
    c->n++;
}

int sh_modserve_resource_each(const void *head_slot, sh_modserve_resource_fn fn, void *context)
{
    ms_res_items c;
    size_t i;
    int ok;
    if (!fn) return 0;
    memset(&c, 0, sizeof c);
    ok = ms_res_walk(head_slot, ms_each_res_one, &c, NULL) && !c.oom;
    __try {
        for (i = 0; i < c.n; ++i) fn(context, c.v[i].list, c.v[i].res, c.v[i].name);
    } __except (EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
    free(c.v);
    return ok;
}

int sh_modserve_watermark_has(const sh_modserve_watermark *w, const void *resource)
{
    uintptr_t key = (uintptr_t)resource;
    return w && w->ok && w->n && bsearch(&key, w->res, w->n, sizeof(uintptr_t), ms_uptr_cmp) != NULL;
}

void sh_modserve_watermark_free(sh_modserve_watermark *w)
{
    if (!w) return;
    free(w->res);
    memset(w, 0, sizeof *w);
}

/* What a kept pass looks up (sh_modserve_lookups_*). An open-addressing set
 * of resource addresses, filled by the engine side's ResourceLookup detour
 * while a pass is open and emptied when it closes. */
#define MS_LOOKUPS_MAX (1u << 20)
static uintptr_t *g_lookups;
static size_t g_lookups_cap, g_lookups_n;
static int g_lookups_open, g_lookups_full;

static size_t ms_lookup_slot(const uintptr_t *set, size_t cap, uintptr_t key)
{
    size_t i = (size_t)((key >> 4) * 0x9E3779B97F4A7C15ull) & (cap - 1);
    while (set[i] && set[i] != key) i = (i + 1) & (cap - 1);
    return i;
}

static int ms_lookups_grow(void)
{
    size_t cap = g_lookups_cap ? g_lookups_cap * 2 : 4096, i;
    uintptr_t *set;
    if (cap > MS_LOOKUPS_MAX * 2) return 0;
    set = (uintptr_t *)calloc(cap, sizeof *set);
    if (!set) return 0;
    for (i = 0; i < g_lookups_cap; ++i)
        if (g_lookups[i]) set[ms_lookup_slot(set, cap, g_lookups[i])] = g_lookups[i];
    free(g_lookups);
    g_lookups = set;
    g_lookups_cap = cap;
    return 1;
}

void sh_modserve_lookups_open(void)
{
    if (g_lookups && g_lookups_cap) memset(g_lookups, 0, g_lookups_cap * sizeof *g_lookups);
    g_lookups_n = 0;
    g_lookups_full = 0;
    g_lookups_open = 1;
}

int sh_modserve_lookups_recording(void)
{
    return g_lookups_open;
}

void sh_modserve_lookups_note(const void *resource)
{
    uintptr_t key = (uintptr_t)resource;
    size_t i;
    if (!g_lookups_open || !key || g_lookups_full) return;
    if ((g_lookups_n + 1) * 2 > g_lookups_cap && !ms_lookups_grow()) { g_lookups_full = 1; return; }
    i = ms_lookup_slot(g_lookups, g_lookups_cap, key);
    if (g_lookups[i]) return;
    g_lookups[i] = key;
    g_lookups_n++;
}

typedef struct ms_lookups_raise { sh_modserve_lookups *out; } ms_lookups_raise;
static void ms_lookups_raise_one(void *context, const unsigned char *list, unsigned char *res)
{
    sh_modserve_lookups *out = ((ms_lookups_raise *)context)->out;
    const char *name;
    size_t at;
    (void)list;
    if (g_lookups[ms_lookup_slot(g_lookups, g_lookups_cap, (uintptr_t)res)] != (uintptr_t)res) return;
    out->live++;
    if (*(unsigned *)(res + MS_RES_LEVEL) != 1 || !ms_keepable(res)) return;
    *(unsigned *)(res + MS_RES_LEVEL) = 4;
    sh_modserve_kept_add(res);
    out->raised++;
    name = *(const char *const *)(res + MS_RES_NAME);
    at = strlen(out->first);
    if (name && at + strlen(name) + 3 < sizeof out->first)
        _snprintf_s(out->first + at, sizeof out->first - at, _TRUNCATE, "%s%s", at ? ", " : "", name);
}

/* Close the pass: every resource it looked up that the lists still hold at
 * level 1 is raised to 4, under the resource lock as the watermark raise
 * does. The set is emptied whether or not the walk completes. */
int sh_modserve_lookups_close(const void *head_slot, sh_modserve_lookups *out)
{
    sh_modserve_lookups local;
    ms_lookups_raise r;
    int ok = 1;
    if (!out) out = &local;
    memset(out, 0, sizeof *out);
    if (!g_lookups_open) return 0;
    g_lookups_open = 0;
    out->looked_up = (unsigned)g_lookups_n;
    out->overflow = g_lookups_full;
    if (g_lookups_n) {
        r.out = out;
        ok = ms_res_walk(head_slot, ms_lookups_raise_one, &r, &out->lists);
        memset(g_lookups, 0, g_lookups_cap * sizeof *g_lookups);
    }
    g_lookups_n = 0;
    g_lookups_full = 0;
    return ok;
}

/* What the engine looks up on behalf of a kept object (sh_modserve_attrib_*).
 * Two open-addressing tables owned by the main thread: the kept set, by
 * address, and the recorded (parent, child) pairs. */

static size_t ms_pair_at(const ms_pair *t, size_t cap, uintptr_t parent, uintptr_t child)
{
    size_t i = (size_t)(((parent >> 4) * 0x9E3779B97F4A7C15ull) ^ ((child >> 4) * 0xC2B2AE3D27D4EB4Full)) & (cap - 1);
    while (t[i].parent && (t[i].parent != parent || t[i].child != child)) i = (i + 1) & (cap - 1);
    return i;
}

/* 1 added, 0 already there, -1 the bound or memory refused it. */
static int ms_pair_add(uintptr_t parent, uintptr_t child)
{
    size_t i;
    if ((g_pairs_n + 1) * 2 > g_pairs_cap) {
        size_t cap = g_pairs_cap ? g_pairs_cap * 2 : 8192, j;
        ms_pair *t;
        if (g_pairs_n >= MS_PAIRS_MAX) return -1;
        t = (ms_pair *)calloc(cap, sizeof *t);
        if (!t) return -1;
        for (j = 0; j < g_pairs_cap; ++j)
            if (g_pairs[j].parent) t[ms_pair_at(t, cap, g_pairs[j].parent, g_pairs[j].child)] = g_pairs[j];
        free(g_pairs);
        g_pairs = t;
        g_pairs_cap = cap;
    }
    i = ms_pair_at(g_pairs, g_pairs_cap, parent, child);
    if (g_pairs[i].parent) return 0;
    g_pairs[i].parent = parent;
    g_pairs[i].child = child;
    g_pairs_n++;
    return 1;
}

void sh_modserve_kept_add(const void *resource)
{
    if (ms_aset_add(&g_kept, (uintptr_t)resource, MS_KEPT_MAX) < 0) g_attrib_full = 1;
}

int sh_modserve_kept_has(const void *resource)
{
    return ms_aset_has(&g_kept, (uintptr_t)resource);
}

int sh_modserve_attrib_note(const void *parent, const void *child)
{
    uintptr_t p = (uintptr_t)parent, c = (uintptr_t)child;
    if (!p || !c || p == c) return 0;
    if (!g_attrib_full && ms_pair_add(p, c) < 0) g_attrib_full = 1;
    if (!ms_aset_has(&g_kept, p)) return 0;
    sh_modserve_kept_add(child);
    return ms_aset_has(&g_kept, c);
}

int sh_modserve_attrib_keep(void *resource)
{
    unsigned char *r = (unsigned char *)resource;
    int raised = 0;
    if (!r || !ms_aset_has(&g_kept, (uintptr_t)r)) return 0;
    __try {
        if (*(unsigned *)(r + MS_RES_LEVEL) == 1 && ms_keepable(r)) { *(unsigned *)(r + MS_RES_LEVEL) = 4; raised = 1; }
    } __except (EXCEPTION_EXECUTE_HANDLER) { raised = 0; }
    g_attrib_raised_at_load += (unsigned)raised;
    return raised;
}

static void *g_nest[SH_MODSERVE_NEST_MAX];
static int g_nest_depth;

int sh_modserve_attrib_enter(void *resource, int heap, int *kept)
{
    int depth = g_nest_depth, k = 0;
    if (heap > 0) { if (ms_aset_add(&g_mapheap, (uintptr_t)resource, MS_KEPT_MAX) < 0) g_attrib_full = 1; }
    else if (heap == 0) ms_aset_del(&g_mapheap, (uintptr_t)resource);
    if (depth > 0 && depth <= SH_MODSERVE_NEST_MAX) k = sh_modserve_attrib_note(g_nest[depth - 1], resource);
    if (depth < SH_MODSERVE_NEST_MAX) g_nest[depth] = resource;
    g_nest_depth = depth + 1;
    if (kept) *kept = k;
    return depth;
}

void sh_modserve_attrib_leave(int depth)
{
    g_nest_depth = depth < 0 ? 0 : depth;
}

int sh_modserve_attrib_depth(void)
{
    return g_nest_depth;
}

void sh_modserve_attrib_found(void *resource)
{
    int depth = g_nest_depth;
    if (resource && depth > 0 && depth <= SH_MODSERVE_NEST_MAX && sh_modserve_attrib_note(g_nest[depth - 1], resource))
        (void)sh_modserve_attrib_keep(resource);
}

void sh_modserve_attrib_reset(void)
{
    g_nest_depth = 0;
    ms_aset_free(&g_kept);
    ms_aset_free(&g_mapheap);
    g_left_n = 0;
    g_left_first[0] = 0;
    free(g_pairs);
    g_pairs = NULL;
    g_pairs_cap = g_pairs_n = 0;
    g_attrib_full = 0;
    g_attrib_raised_at_load = 0;
}

static int ms_pair_parent_cmp(const void *a, const void *b)
{
    uintptr_t x = ((const ms_pair *)a)->parent, y = ((const ms_pair *)b)->parent;
    return x < y ? -1 : x > y;
}

typedef struct ms_attrib_walk { ms_aset live, permanent; int oom; sh_modserve_attrib *out; } ms_attrib_walk;
static void ms_attrib_live_one(void *context, const unsigned char *list, unsigned char *res)
{
    ms_attrib_walk *w = (ms_attrib_walk *)context;
    (void)list;
    if (w->oom) return;
    if (ms_aset_add(&w->live, (uintptr_t)res, MS_RL_MAX_ENTRIES) < 0) w->oom = 1;
    else if (*(const unsigned *)(res + MS_RES_LEVEL) == 4 && ms_aset_add(&w->permanent, (uintptr_t)res, MS_RL_MAX_ENTRIES) < 0)
        w->oom = 1;
}

static void ms_attrib_raise_one(void *context, const unsigned char *list, unsigned char *res)
{
    sh_modserve_attrib *out = ((ms_attrib_walk *)context)->out;
    const char *name;
    size_t at;
    (void)list;
    if (*(unsigned *)(res + MS_RES_LEVEL) != 1 || !ms_aset_has(&g_kept, (uintptr_t)res) || !ms_keepable(res)) return;
    *(unsigned *)(res + MS_RES_LEVEL) = 4;
    out->raised++;
    name = *(const char *const *)(res + MS_RES_NAME);
    at = strlen(out->first);
    if (name && at + strlen(name) + 3 < sizeof out->first)
        _snprintf_s(out->first + at, sizeof out->first - at, _TRUNCATE, "%s%s", at ? ", " : "", name);
}

int sh_modserve_attrib_close(const void *head_slot, int forget, sh_modserve_attrib *out)
{
    sh_modserve_attrib local;
    ms_attrib_walk w;
    ms_aset kept;
    ms_pair *edges = NULL;
    uintptr_t *queue = NULL;
    size_t ne = 0, nq = 0, qcap = 0, i;
    int ok;
    if (!out) out = &local;
    memset(out, 0, sizeof *out);
    memset(&w, 0, sizeof w);
    memset(&kept, 0, sizeof kept);
    w.out = out;
    out->pairs = (unsigned)g_pairs_n;
    out->raised_at_load = g_attrib_raised_at_load;
    out->overflow = g_attrib_full;
    g_attrib_raised_at_load = 0;
    ok = ms_res_walk(head_slot, ms_attrib_live_one, &w, &out->lists) && !w.oom;
    if (!ok) goto done;
    /* Only what a list still holds stays kept: an address the engine frees
     * and reuses must not inherit the keep. */
    for (i = 0; i < g_kept.cap; ++i)
        if (g_kept.slot[i] && ms_aset_has(&w.live, g_kept.slot[i]) && ms_aset_add(&kept, g_kept.slot[i], MS_KEPT_MAX) < 0)
            { ok = 0; goto done; }
    ms_aset_free(&g_kept);
    g_kept = kept;
    memset(&kept, 0, sizeof kept);
    for (i = 0; i < g_mapheap.cap; ++i)
        if (g_mapheap.slot[i] && ms_aset_has(&w.live, g_mapheap.slot[i]) && ms_aset_add(&kept, g_mapheap.slot[i], MS_KEPT_MAX) < 0)
            { ok = 0; goto done; }
    ms_aset_free(&g_mapheap);
    g_mapheap = kept;
    memset(&kept, 0, sizeof kept);
    /* The pairs whose ends are both listed, sorted by parent; then breadth
     * first from every kept resource. */
    if (g_pairs_n) {
        edges = (ms_pair *)malloc(g_pairs_n * sizeof *edges);
        if (!edges) { ok = 0; goto done; }
        for (i = 0; i < g_pairs_cap; ++i)
            if (g_pairs[i].parent && ms_aset_has(&w.live, g_pairs[i].parent) && ms_aset_has(&w.live, g_pairs[i].child))
                edges[ne++] = g_pairs[i];
        qsort(edges, ne, sizeof *edges, ms_pair_parent_cmp);
    }
    if (ne && g_kept.n) {
        qcap = g_kept.n + 64;
        queue = (uintptr_t *)malloc(qcap * sizeof *queue);
        if (!queue) { ok = 0; goto done; }
        for (i = 0; i < g_kept.cap; ++i) if (g_kept.slot[i]) queue[nq++] = g_kept.slot[i];
        while (nq) {
            uintptr_t parent = queue[--nq];
            size_t lo = 0, hi = ne;
            while (lo < hi) { size_t mid = (lo + hi) / 2; if (edges[mid].parent < parent) lo = mid + 1; else hi = mid; }
            for (; lo < ne && edges[lo].parent == parent; ++lo) {
                int added = ms_aset_add(&g_kept, edges[lo].child, MS_KEPT_MAX);
                if (added < 0) { out->overflow = 1; continue; }
                if (!added) continue;
                out->followed++;
                if (nq == qcap) {
                    uintptr_t *g = (uintptr_t *)realloc(queue, qcap * 2 * sizeof *queue);
                    if (!g) { ok = 0; goto done; }
                    queue = g;
                    qcap *= 2;
                }
                queue[nq++] = edges[lo].child;
            }
        }
    }
    ok = ms_res_walk(head_slot, ms_attrib_raise_one, &w, NULL);
    /* The pairs: all forgotten before a purge. Otherwise only those a later
     * keep can still follow stay: a parent not kept yet and not permanent
     * (the engine's own level-4 objects, most of what the boot loads, are
     * never raised, and a kept parent's pairs were followed just now). */
    free(g_pairs);
    g_pairs = NULL;
    g_pairs_cap = g_pairs_n = 0;
    g_attrib_full = 0;
    if (!forget)
        for (i = 0; i < ne; ++i) {
            if (ms_aset_has(&g_kept, edges[i].parent) || ms_aset_has(&w.permanent, edges[i].parent)) continue;
            if (ms_pair_add(edges[i].parent, edges[i].child) < 0) { g_attrib_full = 1; break; }
        }
done:
    out->kept = (unsigned)g_kept.n;
    free(edges);
    free(queue);
    ms_aset_free(&w.live);
    ms_aset_free(&w.permanent);
    ms_aset_free(&kept);
    return ok;
}


/* What kept decls hold (sh_modserve_holds_raise). A (type, name) key: the
 * type case-insensitive, the name folded as the raw-name match folds it
 * (case, a backslash as '/', '-' as '_'). */
typedef struct ms_hold { const char *type, *name; int file; } ms_hold;

static char ms_hold_fold(char c)
{
    if (c == '-') return '_';
    return ms_fold(c);
}

static int ms_hold_cmp(const void *pa, const void *pb)
{
    const ms_hold *a = (const ms_hold *)pa, *b = (const ms_hold *)pb;
    const char *s = a->name, *t = b->name;
    int c = _stricmp(a->type, b->type);
    if (c) return c;
    for (; *s && *t; ++s, ++t) {
        char x = ms_hold_fold(*s), y = ms_hold_fold(*t);
        if (x != y) return (unsigned char)x < (unsigned char)y ? -1 : 1;
    }
    return *s ? 1 : *t ? -1 : 0;
}

typedef struct ms_holds_walk {
    const ms_hold *set; size_t n;
    unsigned char *root;                  /* pass 1: per index file, held at level 4 */
    sh_modserve_holds *out;               /* pass 2: raised */
} ms_holds_walk;

static const ms_hold *ms_holds_find(const ms_holds_walk *w, const unsigned char *list, const unsigned char *res)
{
    ms_hold key;
    key.type = *(const char *const *)(list + MS_RL_TYPE);
    key.name = *(const char *const *)(res + MS_RES_NAME);
    key.file = -1;
    if (!key.type || !key.name || !w->n) return NULL;
    return (const ms_hold *)bsearch(&key, w->set, w->n, sizeof *w->set, ms_hold_cmp);
}

static void ms_holds_roots_one(void *context, const unsigned char *list, unsigned char *res)
{
    ms_holds_walk *w = (ms_holds_walk *)context;
    const ms_hold *h;
    if (*(const unsigned *)(res + MS_RES_LEVEL) != 4) return;
    h = ms_holds_find(w, list, res);
    if (h) { w->root[h->file] = 1; sh_modserve_kept_add(res); }
}

static void ms_holds_raise_one(void *context, const unsigned char *list, unsigned char *res)
{
    ms_holds_walk *w = (ms_holds_walk *)context;
    const char *name;
    size_t at;
    if (*(const unsigned *)(res + MS_RES_LEVEL) != 1 || !ms_holds_find(w, list, res) || !ms_keepable(res)) return;
    *(unsigned *)(res + MS_RES_LEVEL) = 4;
    sh_modserve_kept_add(res);
    w->out->raised++;
    name = *(const char *const *)(res + MS_RES_NAME);
    at = strlen(w->out->first);
    if (at + strlen(name) + 3 < sizeof w->out->first)
        _snprintf_s(w->out->first + at, sizeof w->out->first - at, _TRUNCATE, "%s%s", at ? ", " : "", name);
}

/* The closure of what level-4 decls of served bundles hold (the comment on
 * sh_modserve_holds_raise): `*set_out` the distinct (type, name) needs, sorted
 * by ms_hold_cmp, each with the index file that provides it (-1: none). */
static int ms_holds_closure(const ms_index *x, const void *head_slot, ms_hold **set_out, size_t *ns_out,
                            sh_modserve_holds *out)
{
    ms_holds_walk w;
    ms_hold *cands = NULL, *set = NULL;
    int *queue = NULL, nq = 0, qh = 0, i, k, ok = 0;
    size_t nc = 0, ns = 0, cs = 0;
    *set_out = NULL;
    *ns_out = 0;
    memset(&w, 0, sizeof w);
    cands = (ms_hold *)malloc(sizeof *cands * (size_t)x->nfiles);
    w.root = (unsigned char *)calloc((size_t)x->nfiles, 1);
    queue = (int *)malloc(sizeof *queue * (size_t)x->nfiles);
    if (!cands || !w.root || !queue) goto done;
    /* The roots: decls of admitted bundles the product serves (a surface is
     * the game's own decl, kept at the game's level). */
    for (i = 0; i < x->nfiles; ++i) {
        const ms_file *f = &x->files[i];
        if (!f->is_decl || !f->served || ms_surface(f) || !f->ptype || !f->pname || !x->bundles[f->bundle].admitted) continue;
        cands[nc].type = f->ptype;
        cands[nc].name = f->pname;
        cands[nc].file = i;
        nc++;
    }
    if (nc > 1) qsort(cands, nc, sizeof *cands, ms_hold_cmp);
    w.set = cands;
    w.n = nc;
    if (!ms_res_walk(head_slot, ms_holds_roots_one, &w, &out->lists)) goto done;
    for (i = 0; i < x->nfiles; ++i) if (w.root[i]) { queue[nq++] = i; out->roots++; }
    /* Their needs, closed through the files that provide them. */
    while (qh < nq) {
        const ms_file *f = &x->files[queue[qh++]];
        for (k = 0; k < f->need_count; ++k) {
            const ms_need *nd = &x->needs[f->need_first + k];
            if (!nd->type || !nd->name) continue;
            if (ns == cs) {
                size_t c = cs ? cs * 2 : 256;
                ms_hold *g = (ms_hold *)realloc(set, sizeof *set * c);
                if (!g) goto done;
                set = g;
                cs = c;
            }
            set[ns].type = nd->type;
            set[ns].name = nd->name;
            set[ns].file = nd->file;
            ns++;
            if (nd->file >= 0 && nd->file < x->nfiles && !w.root[nd->file]) {
                w.root[nd->file] = 1;
                queue[nq++] = nd->file;
            }
        }
    }
    if (ns > 1) qsort(set, ns, sizeof *set, ms_hold_cmp);
    for (i = 0, k = 0; (size_t)i < ns; ++i)
        if (!k || ms_hold_cmp(&set[k - 1], &set[i])) set[k++] = set[i];
    ns = (size_t)k;
    out->names = (unsigned)ns;
    *set_out = set;
    *ns_out = ns;
    set = NULL;
    ok = 1;
done:
    free(cands);
    free(set);
    free(queue);
    free(w.root);
    return ok;
}

/* Everything a level-4 decl of a served bundle holds, by name: the needs the
 * builder recorded for its file and, through every need another file of the
 * index provides (a served decl or model, a closure pointer), that file's
 * needs, at any depth. A decl the pass keeps may have been parsed before its
 * watermark (an open map's own load created the chainsaw pickup's web at level
 * 1 and its parse created the clip; the pass then raised the web, and the
 * clip, older than the watermark, stayed at 1 and was freed under the web by
 * the Play purge, report X1), so the watermark alone cannot keep what it
 * holds. Each named resource found at level 1 is raised to 4; one the engine
 * does not hold yet is raised by the first pass after it is created. */
int sh_modserve_holds_raise(const void *head_slot, sh_modserve_holds *out)
{
    const ms_index *x = g_index;
    sh_modserve_holds local;
    ms_holds_walk w;
    ms_hold *set = NULL;
    size_t ns = 0;
    int ok;
    if (!out) out = &local;
    memset(out, 0, sizeof *out);
    if (!x || !head_slot || !x->nfiles) return 0;
    if (!ms_holds_closure(x, head_slot, &set, &ns, out)) return 0;
    memset(&w, 0, sizeof w);
    w.set = set;
    w.n = ns;
    w.out = out;
    ok = ns ? ms_res_walk(head_slot, ms_holds_raise_one, &w, &out->lists) : 1;
    free(set);
    return ok;
}

/* The resource list of a type (its +0x08 type name), as the walks find it. */
void *sh_modserve_resource_list(const void *head_slot, const char *type)
{
    void *found = NULL;
    unsigned nl = 0;
    CRITICAL_SECTION *cs;
    if (!head_slot || !type) return NULL;
    cs = ms_res_lock();
    if (cs) EnterCriticalSection(cs);
    __try {
        const unsigned char *l = *(const unsigned char *const *)head_slot;
        for (; l && nl < MS_RL_MAX_LISTS && !found; l = *(const unsigned char *const *)(l + MS_RL_NEXT), nl++) {
            const char *t = *(const char *const *)(l + MS_RL_TYPE);
            if (t && !_stricmp(t, type)) found = (void *)l;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) { found = NULL; }
    if (cs) LeaveCriticalSection(cs);
    return found;
}

/* The render models kept decls hold (sh_modserve_held_models). */
typedef struct ms_held_model_walk {
    const ms_hold *set; size_t n;
    void **res;                 /* per entry: the resource the lists hold, or NULL */
    unsigned char *load;        /* per entry: 1 when it is to be loaded */
} ms_held_model_walk;

static void ms_held_model_one(void *context, const unsigned char *list, unsigned char *res)
{
    ms_held_model_walk *w = (ms_held_model_walk *)context;
    ms_hold key;
    const ms_hold *h;
    size_t i;
    unsigned state;
    key.type = *(const char *const *)(list + MS_RL_TYPE);
    key.name = *(const char *const *)(res + MS_RES_NAME);
    key.file = -1;
    if (!key.type || !key.name || _stricmp(key.type, SH_MODSERVE_HELD_MODEL_TYPE)) return;
    h = (const ms_hold *)bsearch(&key, w->set, w->n, sizeof *w->set, ms_hold_cmp);
    if (!h) return;
    i = (size_t)(h - w->set);
    w->res[i] = res;
    state = *(const unsigned char *)(res + MS_RES_STATE);
    /* Held for the session and whole: nothing to do. Loaded at level 1 in
     * process heap 0: the holds raise keeps it. The default box (the engine's
     * model for a file it could not read: loaded where the file was not
     * served, report X2), or one whose load allocated from a map heap
     * (ms_keepable), is loaded again. */
    if (state & (MS_RES_FAILED | MS_RES_DEFAULT)) w->load[i] = 1;
    else if (*(const unsigned *)(res + MS_RES_LEVEL) == 4) w->load[i] = 0;
    else w->load[i] = ms_aset_has(&g_mapheap, (uintptr_t)res) ? 1 : 0;
}

int sh_modserve_held_models(const void *head_slot, sh_modserve_held_model_fn fn, void *context,
                            sh_modserve_holds *out)
{
    const ms_index *x = g_index;
    sh_modserve_holds local;
    ms_held_model_walk w;
    ms_hold *set = NULL, *models = NULL;
    size_t ns = 0, nm = 0, i;
    int visited = -1;
    if (!out) out = &local;
    memset(out, 0, sizeof *out);
    memset(&w, 0, sizeof w);
    if (!x || !head_slot || !x->nfiles || !fn) return -1;
    if (!ms_holds_closure(x, head_slot, &set, &ns, out)) return -1;
    /* Only render models whose own file the index serves in this scope: one
     * loaded where its file is not served comes out as the default box, and
     * a stock model (no provider) is the game's bytes in every scope. */
    models = (ms_hold *)malloc(sizeof *models * (ns ? ns : 1));
    if (!models) goto done;
    for (i = 0; i < ns; ++i)
        if (!_stricmp(set[i].type, SH_MODSERVE_HELD_MODEL_TYPE) && set[i].file >= 0 && set[i].file < x->nfiles &&
            x->files[set[i].file].served && x->bundles[x->files[set[i].file].bundle].admitted &&
            ms_scoped(x, set[i].file) >= 0)
            models[nm++] = set[i];
    visited = 0;
    if (!nm) goto done;
    w.set = models;
    w.n = nm;
    w.res = (void **)calloc(nm, sizeof *w.res);
    w.load = (unsigned char *)malloc(nm);
    if (!w.res || !w.load) { visited = -1; goto done; }
    memset(w.load, 1, nm);   /* not held at all: loaded */
    if (!ms_res_walk(head_slot, ms_held_model_one, &w, &out->lists)) { visited = -1; goto done; }
    /* Outside the resource lock: the callback loads. */
    for (i = 0; i < nm; ++i) {
        if (!w.load[i]) continue;
        visited++;
        if (!fn(context, models[i].name, w.res[i])) break;
    }
done:
    free(w.res);
    free(w.load);
    free(models);
    free(set);
    return visited;
}

unsigned sh_modserve_load_keep_lifetime(void *resource, void (*load)(void *), unsigned *was)
{
    unsigned char *r = (unsigned char *)resource;
    unsigned before, after;
    before = *(unsigned *)(r + MS_RES_LEVEL);
    if (was) *was = before;
    load(resource);
    after = *(unsigned *)(r + MS_RES_LEVEL);
    if (after != before) *(unsigned *)(r + MS_RES_LEVEL) = before;
    return after;
}

int sh_modserve_inplace_keeper(int mark_depth, int work)
{
    return mark_depth <= 0 && work > 0;
}

int sh_modserve_load_left_to_engine(unsigned lifetime, unsigned state, int persist)
{
    return persist != 1 && lifetime == 4 && !(state & 1);
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
        if (s->rank != SH_MODSERVE_RANK_SEDEF || s->ready != 1 || s->file < 0 || s->file >= x->nfiles) continue;
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
            /* A surface is the game's own decl: the game parses and keeps it. */
            {
                char rq[MS_NAME_MAX + 48];
                if (_snprintf_s(rq, sizeof rq, _TRUNCATE, "generated/decls/%s/%s.decl", type, name) > 0 &&
                    sh_modserve_is_surface(rq))
                    continue;
            }
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
        if (InterlockedCompareExchange(&f->withdrawn, 0, 0)) {
            /* Withdrawn by an earlier pass (a model SnapMap cannot load, or a
             * parse the engine refused): its bytes are not parsed again. */
            failed[i] = 1;
            InterlockedExchange(&f->reg_state, -1);
            ms_log("%s: %s '%s' (%s) NOT READY: withdrawn earlier in this session", phase, s->type, s->name,
                   s->bundle_id);
            continue;
        }
        {
            int known = ops->source_registered(ops->context, s->type, s->name) == 1;
            char model[MS_NAME_MAX];
            /* An animWeb whose models block names a model nothing provides is
             * refused by the engine's parse (a C++ throw per load). Its file
             * is withdrawn from serving: a stock decl it shadowed serves
             * again, and a new name is never registered. */
            if (!_stricmp(s->type, "animWeb") && ms_animweb_unresolved(x, f, ops, !known, model, sizeof model)) {
                InterlockedExchange(&f->withdrawn, 1);
                if (known && s->shadow) {
                    InterlockedExchange(&f->reg_state, 1);
                    stock[i] = 2;
                    ms_log("%s: animWeb '%s' (%s) shadow WITHDRAWN: it names model '%s', which neither a bundle nor "
                           "the game provides; the game's own animWeb is served", phase, s->name, s->bundle_id, model);
                    ms_console("%s: animWeb '%s' not applied: it names model '%s', which SnapMap cannot load; the "
                               "game's own animWeb is used", s->bundle_id, s->name, model);
                } else {
                    failed[i] = 1;
                    InterlockedExchange(&f->reg_state, -1);
                    ms_log("%s: animWeb '%s' (%s) NOT READY: it names model '%s', which neither a bundle nor the game "
                           "provides; withdrawn and not registered (the engine would refuse it)", phase, s->name,
                           s->bundle_id, model);
                    ms_console("%s: animWeb '%s' not loaded: it names model '%s', which SnapMap cannot load "
                               "(campaign-only or missing)", s->bundle_id, s->name, model);
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
        if (ops->held) {
            /* Registering a file re-parses a resource the engine already holds
             * under that name; a permanent one is left to the game as `load`
             * leaves it, so this pass never binds it to its level-1 objects. */
            unsigned hstate = 0, hlife = 0;
            if (ops->held(ops->context, s->type, s->name, &hstate, &hlife) == 1 &&
                sh_modserve_load_left_to_engine(hlife, hstate, s->persist)) {
                stock[i] = 3;
                ms_log("%s: %s '%s' (%s) left to the game: permanent, %s (state 0x%02x lifetime %u); not registered "
                       "by this pass, whose registration would re-parse it in place; the provider serves these bytes "
                       "whenever the game parses it", phase, s->type, s->name, s->bundle_id,
                       (hstate & 3) == 2 ? "not parsed yet" : "already parsed", hstate & 0xff, hlife);
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
        int dep_failed = -1, loaded;
        if (failed[i]) { s->ready = -1; continue; }
        if (f->reg_state == 1) { s->ready = 1; ready++; continue; }
        if (stock[i] == 2) { s->ready = 1; ready++; continue; }   /* withdrawn: the stock decl serves */
        if (stock[i] == 3) {
            /* Held permanent: left unregistered and unloaded (logged above). Not
             * remembered as registered, so a pass that keeps its decls still can. */
            s->ready = 1;
            ready++;
            continue;
        }
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
        loaded = ops->load(ops->context, s->type, s->name, s->persist, &state, &lifetime);
        if (loaded == SH_MODSERVE_LOAD_LEFT) {
            /* A permanent decl: this pass neither parses nor re-parses it
             * (sh_modserve_load_left_to_engine). The line carries the state and
             * lifetime the decl had before the load. */
            InterlockedExchange(&f->reg_state, 1);
            s->ready = 1;
            ready++;
            ms_log("%s: %s '%s' (%s) left to the game: permanent, %s (state 0x%02x lifetime %u); the provider "
                   "serves these bytes whenever the game parses it", phase, s->type, s->name, s->bundle_id,
                   (state & 3) == 2 ? "not parsed yet" : "already parsed", state & 0xff, lifetime);
        } else if (loaded == SH_MODSERVE_LOAD_REFUSED) {
            /* The parse failed although the load reports success: withdrawn,
             * so no later pass or demand load parses these bytes again. */
            char why[300];
            why[0] = 0;
            if (ops->refusal) ops->refusal(ops->context, why, sizeof why);
            failed[i] = 1;
            s->ready = -1;
            InterlockedExchange(&f->reg_state, -1);
            InterlockedExchange(&f->withdrawn, 1);
            ms_log("%s: %s '%s' (%s) NOT READY: the engine refused its parse%s%s; withdrawn, not loaded again", phase,
                   s->type, s->name, s->bundle_id, why[0] ? ": " : "", why);
            ms_console("%s: %s '%s' not loaded: the game refused it%s%s", s->bundle_id, s->type, s->name,
                       why[0] ? ": " : "", why);
        } else if (loaded) {
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
    /* What this run settled ready is not visited again by a later pass of
     * the same steps (sh_modserve_plan_unvisited). */
    for (i = 0; i < plan->count; ++i)
        if (plan->steps[i].ready == 1) InterlockedExchange(&x->files[plan->steps[i].file].plan_seen, 1);
    free(failed);
    free(stock);
    return ready;
}

int sh_modserve_plan_unvisited(const sh_modserve_plan *plan)
{
    const ms_index *x = g_index;
    int i, n = 0;
    if (!plan || !x) return 0;
    for (i = 0; i < plan->count; ++i) {
        const ms_file *f;
        if (plan->steps[i].file < 0 || plan->steps[i].file >= x->nfiles) continue;
        f = &x->files[plan->steps[i].file];
        if (InterlockedCompareExchange((volatile LONG *)&f->reg_state, 0, 0) == -1 ||
            InterlockedCompareExchange((volatile LONG *)&f->withdrawn, 0, 0))
            continue;   /* failed or withdrawn: no pass parses it again */
        if (!InterlockedCompareExchange((volatile LONG *)&f->plan_seen, 0, 0)) n++;
    }
    return n;
}


/* ------------------------------------------------------------------ requirements (apply) */

/* The cvars the requirements set and the bundles whose lines ran, across
 * tables (a rescan replaces the index, not this state). A cvar entry exists
 * while an accepted bundle needs it or while this layer holds the value it
 * displaced; a cvar is restored only when no bundle needs it any more and
 * nobody else changed it since. Written by the pass (main thread) under
 * g_req_lock; the reports and an index build read it shared. */
#define MS_REQ_ORIGINAL_MAX 512
#define MS_WHO_MAX          200
typedef struct ms_held {
    char name[SH_MB_REQ_NAME_MAX + 1];
    char want[SH_MB_REQ_VALUE_MAX + 1];   /* the value the accepted bundles ask */
    char original[MS_REQ_ORIGINAL_MAX];   /* what this layer displaced (owned only) */
    char owner[MS_WHO_MAX];               /* the name of the earliest install asking it */
    int needed;                           /* an accepted bundle asks it (this pass) */
    int owned;                            /* this layer changed it and restores `original` */
    int state;                            /* 1 live, 0 not applied yet, -1 not live */
    char why[120];                        /* not live: the cause */
} ms_held;

typedef struct ms_active {
    char id[100], who[MS_WHO_MAX];
    char *deact[SH_MB_REQ_COMMANDS_MAX];  /* its deactivate lines, run when it stops */
    int ndeact, act_ran, act_total;
} ms_active;

static SRWLOCK g_req_lock = SRWLOCK_INIT;
static ms_held *g_held;
static int g_nheld, g_cheld;
static ms_active *g_active;
static int g_nactive, g_cactive;
/* Ids whose requirements ran and then stopped (the map scope withdrew them,
 * or they were removed), until they start again: the report says "restored"
 * instead of "not applied yet". */
static char (*g_stopped)[100];
static int g_nstopped, g_cstopped;

static int ms_stopped_find(const char *id)
{
    int i;
    for (i = 0; i < g_nstopped; ++i) if (!strcmp(g_stopped[i], id)) return i;
    return -1;
}

static void ms_stopped_set(const char *id, int stopped)
{
    int i = ms_stopped_find(id);
    if (!stopped) {
        if (i >= 0) { memmove(&g_stopped[i], &g_stopped[i + 1], sizeof *g_stopped * (size_t)(g_nstopped - i - 1)); g_nstopped--; }
        return;
    }
    if (i >= 0 || !ms_grow((void **)&g_stopped, &g_cstopped, g_nstopped + 1, sizeof *g_stopped)) return;
    strncpy_s(g_stopped[g_nstopped++], sizeof *g_stopped, id, _TRUNCATE);
}

/* Accepted: admitted, not lost to an earlier install, and in scope for the
 * current map (modserve.h, map scope). */
static int ms_req_accepted(const ms_bundle *b)
{
    return b->admitted && b->req_text && !b->req_lost && (b->req_ncvars || b->req_nact || b->req_ndeact) &&
           !InterlockedCompareExchange((volatile LONG *)&b->req_off, 0, 0);
}

/* Two values the engine would hold alike: the same text, or the same number. */
static int ms_req_same(const char *a, const char *b)
{
    char *ea, *eb;
    double x, y;
    if (!strcmp(a, b)) return 1;
    if (!*a || !*b) return 0;
    x = strtod(a, &ea);
    y = strtod(b, &eb);
    return !*ea && !*eb && x == y;
}

static ms_held *ms_held_find(const char *name)
{
    int i;
    for (i = 0; i < g_nheld; ++i) if (!_stricmp(g_held[i].name, name)) return &g_held[i];
    return NULL;
}

static ms_active *ms_active_find(const char *id)
{
    int i;
    for (i = 0; i < g_nactive; ++i) if (!strcmp(g_active[i].id, id)) return &g_active[i];
    return NULL;
}

static void ms_active_drop(int i)
{
    int k;
    for (k = 0; k < g_active[i].ndeact; ++k) free(g_active[i].deact[k]);
    memmove(&g_active[i], &g_active[i + 1], sizeof *g_active * (size_t)(g_nactive - i - 1));
    g_nactive--;
}

/* A bundle's cvars are live when each one's entry is live at its value.
 * Caller holds g_req_lock (shared is enough: it writes only the bundle). */
static void ms_req_mark_bundle(ms_bundle *b)
{
    int k, unmet = 0;
    size_t at = 0;
    char names[MS_CAUSE_MAX];
    names[0] = 0;
    if (b->req_lost) {
        _snprintf_s(b->req_cause, sizeof b->req_cause, _TRUNCATE, "requirement %s", b->req_lost);
        InterlockedExchange(&b->req_live, 0);
        return;
    }
    if (InterlockedCompareExchange(&b->req_off, 0, 0)) {
        strcpy_s(b->req_cause, sizeof b->req_cause, "its requirements apply only in maps that use it");
        InterlockedExchange(&b->req_live, 0);
        return;
    }
    for (k = 0; k < b->req_ncvars; ++k) {
        const ms_held *h = ms_held_find(b->req_cvars[k].name);
        int w;
        if (h && h->state == 1 && ms_req_same(h->want, b->req_cvars[k].value)) continue;
        w = _snprintf_s(names + at, sizeof names - at, _TRUNCATE, "%s%s=%s", at ? ", " : "", b->req_cvars[k].name,
                        b->req_cvars[k].value);
        if (w > 0) at += (size_t)w;
        unmet++;
    }
    if (unmet) _snprintf_s(b->req_cause, sizeof b->req_cause, _TRUNCATE, "requirement%s %s %s not live", unmet > 1 ? "s" : "",
                           names, unmet > 1 ? "are" : "is");
    else b->req_cause[0] = 0;
    InterlockedExchange(&b->req_live, unmet ? 0 : 1);
}

/* At index build: bundles in install order (ms_newer); one asking a cvar an
 * earlier accepted bundle asks at another value loses. Then the log lines
 * (once per bundle and requirements text) and the liveness the held state
 * already gives, so a bundle whose cvars a pass made live serves as live. */
static void ms_req_index(ms_index *x, const ms_index *old)
{
    int *order, n = 0, i, j, k, m;
    order = (int *)malloc(sizeof(int) * (size_t)(x->nbundles ? x->nbundles : 1));
    if (!order) return;
    for (i = 0; i < x->nbundles; ++i)
        if (x->bundles[i].admitted && x->bundles[i].req_text) order[n++] = i;
    for (i = 1; i < n; ++i)   /* oldest install first */
        for (j = i; j > 0 && ms_newer(x, order[j - 1], order[j]); --j) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
    for (i = 0; i < n; ++i) {
        ms_bundle *b = &x->bundles[order[i]];
        for (k = 0; k < b->req_ncvars && !b->req_lost; ++k)
            for (j = 0; j < i && !b->req_lost; ++j) {
                const ms_bundle *e = &x->bundles[order[j]];
                if (e->req_lost) continue;
                for (m = 0; m < e->req_ncvars; ++m) {
                    char text[MS_CAUSE_MAX];
                    if (_stricmp(e->req_cvars[m].name, b->req_cvars[k].name) ||
                        ms_req_same(e->req_cvars[m].value, b->req_cvars[k].value))
                        continue;
                    _snprintf_s(text, sizeof text, _TRUNCATE, "%s=%s lost to %s, installed earlier, which asks \"%s\"",
                                b->req_cvars[k].name, b->req_cvars[k].value, ms_label(e), e->req_cvars[m].value);
                    b->req_lost = ms_strdup(x, text);
                    if (!b->req_lost) b->req_lost = "a cvar lost to an earlier install";
                    ms_log("requirements: conflict: %s: %s asks \"%s\", %s asks \"%s\"; %s, the later install, loses "
                           "and its requirements are not applied", b->req_cvars[k].name, ms_label(b),
                           b->req_cvars[k].value, ms_label(e), e->req_cvars[m].value, ms_label(b));
                    break;
                }
            }
    }
    for (i = 0; i < n; ++i) {
        const ms_bundle *b = &x->bundles[order[i]];
        int seen = 0;
        if (old) {
            int o = ms_bundle_by_hash(old, b->hash, 1);
            seen = o >= 0 && old->bundles[o].req_text && !strcmp(old->bundles[o].req_text, b->req_text) &&
                   !old->bundles[o].req_lost == !b->req_lost;
        }
        if (seen || b->req_lost) continue;
        for (k = 0; k < b->req_ncvars; ++k)
            ms_log("requirements: %s=%s for %s", b->req_cvars[k].name, b->req_cvars[k].value, ms_label(b));
        if (b->req_nact || b->req_ndeact)
            ms_log("requirements: %d activate and %d deactivate command(s) for %s", b->req_nact, b->req_ndeact, ms_label(b));
    }
    free(order);
    AcquireSRWLockShared(&g_req_lock);
    for (i = 0; i < x->nbundles; ++i)
        if (x->bundles[i].admitted && ms_req_needs(&x->bundles[i])) ms_req_mark_bundle(&x->bundles[i]);
    ReleaseSRWLockShared(&g_req_lock);
}

int sh_modserve_requirements(void)
{
    const ms_index *x = g_index;
    int i, k, n = 0, j, dup;
    if (!x) return 0;
    for (i = 0; i < x->nbundles; ++i) {
        const ms_bundle *b = &x->bundles[i];
        if (!ms_req_accepted(b)) continue;
        for (k = 0; k < b->req_ncvars; ++k) {
            dup = 0;
            for (j = 0; j < i && !dup; ++j) {
                const ms_bundle *e = &x->bundles[j];
                int m;
                if (!ms_req_accepted(e)) continue;
                for (m = 0; m < e->req_ncvars && !dup; ++m) dup = !_stricmp(e->req_cvars[m].name, b->req_cvars[k].name);
            }
            for (j = 0; j < k && !dup; ++j) dup = !_stricmp(b->req_cvars[j].name, b->req_cvars[k].name);
            n += !dup;
        }
    }
    return n;
}

int sh_modserve_requirements_held(void)
{
    const ms_index *x = g_index;
    int i, any;
    AcquireSRWLockShared(&g_req_lock);
    any = g_nheld > 0 || g_nactive > 0;
    ReleaseSRWLockShared(&g_req_lock);
    for (i = 0; !any && x && i < x->nbundles; ++i) any = ms_req_accepted(&x->bundles[i]);
    return any;
}

/* "a=0, b=1" of the entries selected by `pick` (1 needed, 2 not live, 3 changed and live). */
static void ms_held_names(int pick, const unsigned char *flag, char *out, size_t cap)
{
    int i;
    size_t at = 0;
    out[0] = 0;
    for (i = 0; i < g_nheld; ++i) {
        const ms_held *h = &g_held[i];
        int w;
        if (!h->needed) continue;
        if (pick == 2 && h->state == 1) continue;
        if (pick == 3 && !(flag && flag[i] && h->state == 1)) continue;
        w = _snprintf_s(out + at, cap - at, _TRUNCATE, "%s%s=%s", at ? ", " : "", h->name, h->want);
        if (w < 0) break;
        at += (size_t)w;
    }
}

static int ms_req_line(const sh_modserve_gate_ops *ops, const char *phase, const char *id, const char *line)
{
    int ok = 0;
    if (ops && ops->command) ok = ops->command(ops->context, line) == 1;
    if (ok) ms_log("requirements: %s: command \"%s\"", id, line);
    else ms_log("%s: requirements: %s: command \"%s\" NOT run (the command buffer is unavailable)", phase, id, line);
    return ok;
}

static const char *ms_refusal(int code)
{
    if (code == SH_MODSERVE_GATE_READ_ONLY) return "the engine marks it read-only";
    if (code == SH_MODSERVE_GATE_INIT_ONLY) return "the engine allows it only on the command line";
    return "the engine refused the value";
}

int sh_modserve_requirements_apply(const sh_modserve_gate_ops *ops, const char *phase, sh_modserve_gates *out)
{
    ms_index *x = g_index;
    sh_modserve_gates r;
    unsigned char *changed = NULL, *queued = NULL, *restoring = NULL, *refused = NULL;
    char (*target)[MS_REQ_ORIGINAL_MAX] = NULL;
    char names[600];
    int i, k, result;
    memset(&r, 0, sizeof r);
    if (!phase) phase = "plan";
    AcquireSRWLockExclusive(&g_req_lock);
    for (i = 0; i < g_nheld; ++i) g_held[i].needed = 0;
    /* What the accepted bundles ask, earliest install naming each cvar. */
    if (x) {
        int *order = (int *)malloc(sizeof(int) * (size_t)(x->nbundles ? x->nbundles : 1)), n = 0, j;
        if (order) {
            for (i = 0; i < x->nbundles; ++i) if (ms_req_accepted(&x->bundles[i])) order[n++] = i;
            for (i = 1; i < n; ++i)
                for (j = i; j > 0 && ms_newer(x, order[j - 1], order[j]); --j) { int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t; }
            for (i = 0; i < n; ++i) {
                const ms_bundle *b = &x->bundles[order[i]];
                for (k = 0; k < b->req_ncvars; ++k) {
                    ms_held *h = ms_held_find(b->req_cvars[k].name);
                    if (h && h->needed) continue;
                    if (!h) {
                        if (!ms_grow((void **)&g_held, &g_cheld, g_nheld + 1, sizeof *g_held)) continue;
                        h = &g_held[g_nheld++];
                        memset(h, 0, sizeof *h);
                        strncpy_s(h->name, sizeof h->name, b->req_cvars[k].name, _TRUNCATE);
                    }
                    if (strcmp(h->want, b->req_cvars[k].value)) h->state = 0;
                    strncpy_s(h->want, sizeof h->want, b->req_cvars[k].value, _TRUNCATE);
                    _snprintf_s(h->owner, sizeof h->owner, _TRUNCATE, "%s", ms_label(b));
                    h->needed = 1;
                    r.needed++;
                }
            }
            free(order);
        }
    }
    /* Nothing asked and nothing held: no cvar is read or set, no line runs. */
    if (!r.needed && !g_nheld && !g_nactive) {
        for (i = 0; x && i < x->nbundles; ++i) if (x->bundles[i].admitted && ms_req_needs(&x->bundles[i])) ms_req_mark_bundle(&x->bundles[i]);
        ReleaseSRWLockExclusive(&g_req_lock);
        if (out) *out = r;
        return 0;
    }
    /* Lines of bundles whose requirements stopped (uninstalled, removed, dropped
     * by a rescan, lost a conflict) run first; their cvars are released below. */
    for (i = g_nactive - 1; i >= 0; --i) {
        int still = 0, b;
        for (b = 0; x && b < x->nbundles && !still; ++b)
            still = ms_req_accepted(&x->bundles[b]) && !strcmp(x->bundles[b].id, g_active[i].id);
        if (still) continue;
        if (g_active[i].ndeact && (!ops || !ops->command)) continue;   /* kept until a pass can run its lines */
        ms_log("%s: requirements of %s stopped%s", phase, g_active[i].who,
               g_active[i].ndeact ? "; running its deactivate commands" : "");
        for (k = 0; k < g_active[i].ndeact; ++k)
            r.commands += ms_req_line(ops, phase, g_active[i].id, g_active[i].deact[k]);
        ms_stopped_set(g_active[i].id, 1);
        ms_active_drop(i);
    }
    if (!ops || !ops->read || !ops->set) {
        ms_held_names(1, NULL, names, sizeof names);
        if (r.needed) ms_log("%s: requirements %s NOT applied: the cvar system is unavailable", phase, names);
        for (i = 0; i < g_nheld; ++i)
            if (g_held[i].needed) {
                g_held[i].state = -1;
                strcpy_s(g_held[i].why, sizeof g_held[i].why, "the cvar system is unavailable");
            }
        r.not_live = r.needed;
        goto finish;
    }
    changed = (unsigned char *)calloc((size_t)g_nheld + 1, 1);
    queued = (unsigned char *)calloc((size_t)g_nheld + 1, 1);
    restoring = (unsigned char *)calloc((size_t)g_nheld + 1, 1);
    refused = (unsigned char *)calloc((size_t)g_nheld + 1, 1);
    target = (char (*)[MS_REQ_ORIGINAL_MAX])calloc((size_t)g_nheld + 1, MS_REQ_ORIGINAL_MAX);
    if (!changed || !queued || !restoring || !refused || !target) {
        ms_log("%s: requirements NOT applied: out of memory", phase);
        for (i = 0; i < g_nheld; ++i) if (g_held[i].needed) { g_held[i].state = -1; strcpy_s(g_held[i].why, sizeof g_held[i].why, "out of memory"); }
        r.not_live = r.needed;
        goto finish;
    }
    /* Phase 1: read every affected cvar, then set what differs. */
    for (i = 0; i < g_nheld; ++i) {
        ms_held *h = &g_held[i];
        char now[MS_REQ_ORIGINAL_MAX];
        int set;
        if (!h->needed && !h->owned) continue;
        now[0] = 0;
        if (ops->read(ops->context, h->name, now, sizeof now) != 1) {
            refused[i] = 1;
            if (h->needed) { h->state = -1; strcpy_s(h->why, sizeof h->why, "the engine has no such cvar"); }
            else h->owned = 0;   /* gone: nothing to restore */
            continue;
        }
        if (h->needed) {
            strcpy_s(target[i], MS_REQ_ORIGINAL_MAX, h->want);
            if (ms_req_same(now, h->want)) continue;
            if (!h->owned && strlen(now) + 1 >= sizeof now) {
                refused[i] = 1;
                h->state = -1;
                strcpy_s(h->why, sizeof h->why, "its current value is too long to restore later");
                continue;
            }
        } else {
            /* Changed by someone else since: theirs. */
            restoring[i] = 1;
            strcpy_s(target[i], MS_REQ_ORIGINAL_MAX, ms_req_same(now, h->want) ? h->original : now);
            if (ms_req_same(now, target[i])) continue;
        }
        set = ops->set(ops->context, h->name, target[i]);
        if (set != SH_MODSERVE_GATE_SET && set != SH_MODSERVE_GATE_QUEUED) {
            refused[i] = 1;
            if (h->needed) { h->state = -1; strcpy_s(h->why, sizeof h->why, ms_refusal(set)); }
            else ms_log("%s: requirement release: %s could not be restored to \"%s\" (%s)", phase, h->name, target[i],
                        ms_refusal(set));
            continue;
        }
        changed[i] = 1;
        r.changed++;
        if (set == SH_MODSERVE_GATE_QUEUED) { queued[i] = 1; r.queued++; }
        if (h->needed && !h->owned) { strcpy_s(h->original, sizeof h->original, now); h->owned = 1; }
    }
    /* A queued set reaches the cvar only when the command buffer runs: drain
     * it here, between the phases, so the registration that follows is never
     * asked to parse a name the engine still resolves the old way. */
    if (r.queued) {
        if (ops->drain && ops->drain(ops->context) == 1) r.drained = 1;
        else {
            size_t at = 0;
            names[0] = 0;
            for (i = 0; i < g_nheld; ++i)
                if (queued[i] && g_held[i].needed) {
                    int w = _snprintf_s(names + at, sizeof names - at, _TRUNCATE, "%s%s=%s", at ? ", " : "", g_held[i].name,
                                        g_held[i].want);
                    if (w > 0) at += (size_t)w;
                }
            ms_log("%s: requirement set%s %s queued but the command buffer could not be drained", phase,
                   r.queued > 1 ? "s" : "", names[0] ? names : "(restore)");
        }
    }
    /* Phase 2: the registration pass runs only against read-back values. */
    for (i = 0; i < g_nheld; ++i) {
        ms_held *h = &g_held[i];
        char now[MS_REQ_ORIGINAL_MAX];
        if (refused[i] || (!h->needed && !restoring[i])) continue;
        now[0] = 0;
        if (ops->read(ops->context, h->name, now, sizeof now) != 1) {
            if (h->needed) { h->state = -1; strcpy_s(h->why, sizeof h->why, "the engine has no such cvar"); }
            continue;
        }
        if (h->needed) {
            if (ms_req_same(now, h->want)) { h->state = 1; h->why[0] = 0; }
            else {
                h->state = -1;
                _snprintf_s(h->why, sizeof h->why, _TRUNCATE, "the engine kept \"%.60s\"", now);
            }
        } else if (ms_req_same(now, target[i])) {
            if (changed[i]) ms_log("%s: requirement released: %s restored to %s (no active bundle needs it)", phase, h->name,
                                   target[i]);
            r.restored++;
            h->owned = 0;
        }
    }
    for (i = 0; i < g_nheld; ++i) if (g_held[i].needed && g_held[i].state == 1) r.live++;
    r.not_live = r.needed - r.live;
    ms_held_names(3, changed, names, sizeof names);
    if (names[0])
        ms_log("%s: requirements applied: %s (%s)", phase, names,
               r.drained ? "queued, live after a command-buffer drain" : "set directly, live immediately");
    if (r.not_live) {
        ms_held_names(2, NULL, names, sizeof names);
        ms_log("%s: requirements %s are NOT live; the steps of bundles that need them are blocked", phase, names);
        for (i = 0; i < g_nheld; ++i)
            if (g_held[i].needed && g_held[i].state != 1)
                ms_log("%s: requirement %s=%s (%s) is not live: %s", phase, g_held[i].name, g_held[i].want, g_held[i].owner,
                       g_held[i].why[0] ? g_held[i].why : "not read back");
    }
finish:
    /* Entries neither asked nor holding a displaced value are done. */
    for (i = g_nheld - 1; i >= 0; --i)
        if (!g_held[i].needed && !g_held[i].owned) {
            memmove(&g_held[i], &g_held[i + 1], sizeof *g_held * (size_t)(g_nheld - i - 1));
            g_nheld--;
        }
    /* Lines of bundles whose requirements start now, after their cvars. */
    for (i = 0; x && i < x->nbundles; ++i) {
        ms_bundle *b = &x->bundles[i];
        ms_active *a;
        if (!ms_req_accepted(b)) continue;
        a = ms_active_find(b->id);
        if (!a) {
            int ran = 0;
            if (b->req_nact && (!ops || !ops->command)) {
                ms_log("%s: requirements: %s: %d activate command(s) wait for the command buffer", phase, ms_label(b),
                       b->req_nact);
                continue;
            }
            if (!ms_grow((void **)&g_active, &g_cactive, g_nactive + 1, sizeof *g_active)) continue;
            a = &g_active[g_nactive++];
            memset(a, 0, sizeof *a);
            strncpy_s(a->id, sizeof a->id, b->id, _TRUNCATE);
            ms_stopped_set(b->id, 0);
            if (b->req_nact) ms_log("%s: requirements of %s start; running its activate commands", phase, ms_label(b));
            for (k = 0; k < b->req_nact; ++k) ran += ms_req_line(ops, phase, b->id, b->req_act[k]);
            r.commands += ran;
            a->act_ran = ran;
            a->act_total = b->req_nact;
        }
        /* The current build's lines run when it stops. */
        _snprintf_s(a->who, sizeof a->who, _TRUNCATE, "%s", ms_label(b));
        for (k = 0; k < a->ndeact; ++k) free(a->deact[k]);
        a->ndeact = 0;
        for (k = 0; k < b->req_ndeact && k < (int)SH_MB_REQ_COMMANDS_MAX; ++k) {
            a->deact[a->ndeact] = _strdup(b->req_deact[k]);
            if (a->deact[a->ndeact]) a->ndeact++;
        }
    }
    for (i = 0; x && i < x->nbundles; ++i)
        if (x->bundles[i].admitted && ms_req_needs(&x->bundles[i])) ms_req_mark_bundle(&x->bundles[i]);
    ReleaseSRWLockExclusive(&g_req_lock);
    free(changed);
    free(queued);
    free(restoring);
    free(refused);
    free(target);
    if (out) *out = r;
    result = !r.needed ? 0 : r.not_live ? -1 : 1;
    return result;
}

int sh_modserve_requirements_check(const sh_modserve_gate_ops *ops, const char *phase, sh_modserve_gates *out)
{
    sh_modserve_gates r;
    int result = sh_modserve_requirements_apply(ops, phase, &r);
    if (result == 1 && !r.changed) {
        char names[600];
        AcquireSRWLockShared(&g_req_lock);
        ms_held_names(1, NULL, names, sizeof names);
        ReleaseSRWLockShared(&g_req_lock);
        ms_log("%s: requirements live: %s (unchanged since the last pass)", phase ? phase : "plan", names);
    }
    if (out) *out = r;
    return result;
}

int sh_modserve_plan_block_unmet(sh_modserve_plan *plan, const sh_modserve_gates *gates)
{
    const ms_index *x = g_index;
    int i, n = 0;
    (void)gates;
    if (!plan || !x) return 0;
    for (i = 0; i < plan->count; ++i) {
        sh_modserve_step *s = &plan->steps[i];
        /* The gates of the build that serves the step's path for the open
         * map (ms_scoped), not of the path's static winner: with two builds
         * of one key installed the winner can be the build the map does not
         * use, whose requirements are off (req_off). */
        int g = ms_scoped(x, s->file);
        const ms_bundle *b = ms_gate_of(x, &x->files[g >= 0 ? g : s->file]);
        /* A step registered while its requirements were live stays ready. */
        if (ms_req_needs(b) && !InterlockedCompareExchange((volatile LONG *)&b->req_live, 0, 0) && !s->blocked &&
            x->files[s->file].reg_state != 1) {
            s->blocked = b->req_cause[0] ? b->req_cause : "requirements are not live";
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
    /* Phases 1 and 2 (set, then verify after any drain) finish before the
     * plan's first DeclRegisterFile. */
    sh_modserve_requirements_apply(gates, phase, &r);
    sh_modserve_plan_block_unmet(plan, &r);
    if (out) *out = r;
    return sh_modserve_plan_run(plan, ops, phase);
}

int sh_modserve_requirements_report(sh_modserve_line_fn fn, void *context)
{
    const ms_index *x = g_index;
    char line[900];
    int i, k, lines = 0, any = 0;
    if (!fn) return 0;
    AcquireSRWLockShared(&g_req_lock);
    for (i = 0; x && i < x->nbundles; ++i) {
        const ms_bundle *b = &x->bundles[i];
        const ms_active *a;
        int off, stopped;
        const char *scope_why;
        if (!b->admitted || !b->req_text || (!b->req_ncvars && !b->req_nact && !b->req_ndeact)) continue;
        any = 1;
        a = ms_active_find(b->id);
        /* Each line is this bundle's own state for the open map: another
         * mod holding the same value does not make an inactive one live. */
        off = !b->req_lost && InterlockedCompareExchange((volatile LONG *)&b->req_off, 0, 0);
        stopped = !a && ms_stopped_find(b->id) >= 0;
        scope_why = sh_modserve_scope_map_open() ? "the open map does not use it" : "no map is open";
        for (k = 0; k < b->req_ncvars; ++k) {
            const ms_held *h = ms_held_find(b->req_cvars[k].name);
            const char *state;
            char why[300];
            if (b->req_lost) state = "refused", _snprintf_s(why, sizeof why, _TRUNCATE, " (%s)", b->req_lost);
            else if (off) {
                state = "not active for this map";
                if (h && h->needed && h->state == 1 && ms_req_same(h->want, b->req_cvars[k].value))
                    _snprintf_s(why, sizeof why, _TRUNCATE, " (%s; the cvar has this value because %s asks it)", scope_why,
                                h->owner);
                else if (h && h->owned && !h->needed)
                    _snprintf_s(why, sizeof why, _TRUNCATE, " (%s; restored to \"%.120s\" at the next editor pass or map load)",
                                scope_why, h->original);
                else if (stopped)
                    _snprintf_s(why, sizeof why, _TRUNCATE, " (%s; restored: the cvar is back at the value it displaced)",
                                scope_why);
                else _snprintf_s(why, sizeof why, _TRUNCATE, " (%s)", scope_why);
            } else if (!h || !h->needed || !ms_req_same(h->want, b->req_cvars[k].value) || h->state == 0)
                state = "not applied yet", strcpy_s(why, sizeof why, " (the next editor pass or map load applies it)");
            else if (h->state == 1) state = "live", why[0] = 0;
            else state = "NOT live", _snprintf_s(why, sizeof why, _TRUNCATE, " (%s; its steps are blocked)", h->why);
            _snprintf_s(line, sizeof line, _TRUNCATE, "requirements: %s: cvar %s=%s: %s%s", ms_label(b),
                        b->req_cvars[k].name, b->req_cvars[k].value, state, why);
            fn(context, line);
            lines++;
        }
        for (k = 0; k < b->req_nact; ++k) {
            const char *state = b->req_lost ? "not run (a cvar lost a conflict)"
                              : a ? (k < a->act_ran ? "ran" : "NOT run")
                              : off ? (stopped ? "ran, then stopped (not active for this map)" : "not run (not active for this map)")
                              : "runs at the next editor pass or map load";
            _snprintf_s(line, sizeof line, _TRUNCATE, "requirements: %s: activate command \"%s\": %s", ms_label(b),
                        b->req_act[k], state);
            fn(context, line);
            lines++;
        }
        for (k = 0; k < b->req_ndeact; ++k) {
            _snprintf_s(line, sizeof line, _TRUNCATE, "requirements: %s: deactivate command \"%s\": %s", ms_label(b),
                        b->req_deact[k], stopped ? "ran when it stopped" : "runs when the mod stops");
            fn(context, line);
            lines++;
        }
    }
    for (i = 0; i < g_nheld; ++i)
        if (!g_held[i].needed && g_held[i].owned) {
            _snprintf_s(line, sizeof line, _TRUNCATE, "requirements: %s is restored to \"%.200s\" at the next editor pass or "
                        "map load (no installed mod needs it)", g_held[i].name, g_held[i].original);
            fn(context, line);
            lines++;
        }
    for (i = 0; i < g_nactive; ++i) {
        int still = 0, b;
        for (b = 0; x && b < x->nbundles && !still; ++b)
            still = ms_req_accepted(&x->bundles[b]) && !strcmp(x->bundles[b].id, g_active[i].id);
        if (still) continue;
        _snprintf_s(line, sizeof line, _TRUNCATE, "requirements: %s stopped; its %d deactivate command(s) run at the next "
                    "editor pass or map load", g_active[i].who, g_active[i].ndeact);
        fn(context, line);
        lines++;
    }
    ReleaseSRWLockShared(&g_req_lock);
    if (!any && !lines) { fn(context, "requirements: none (no installed mod sets a cvar or runs a command)"); lines++; }
    return lines;
}

#ifdef SH_MODSERVE_TESTING
/* Forget the held cvars and the lines that ran, without restoring anything. */
void sh_modserve_test_reset_gates(void)
{
    int i;
    AcquireSRWLockExclusive(&g_req_lock);
    for (i = g_nactive - 1; i >= 0; --i) ms_active_drop(i);
    g_nheld = 0;
    g_nstopped = 0;
    ReleaseSRWLockExclusive(&g_req_lock);
}
unsigned sh_modserve_test_gates_owned(void)
{
    int i;
    unsigned n = 0;
    for (i = 0; i < g_nheld; ++i) n += g_held[i].owned ? 1u : 0u;
    return n;
}
#endif

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

/* The served file a placed name (lowercase) resolves to: an entityDef of
 * that name, else any served decl of that name, else a module instance's
 * moduleName (a snapModule decl is served as a raw file named by its own
 * path). -1 when no admitted bundle provides it. */
static int ms_name_file(const ms_index *x, const char *lower)
{
    int f = ms_provider(x, "entitydef", lower), k, m, nm;
    char keys[2][SH_MB_REQUEST_MAX + 1];
    for (k = 0; f < 0 && k < x->nfiles; ++k)
        if (x->files[k].served && x->files[k].pname && !strcmp(x->files[k].pname, lower) &&
            (x->files[k].is_decl || (x->files[k].ptype && !strcmp(x->files[k].ptype, "snapModule")))) f = k;
    /* A model an entity's edit state names (renderModelInfo.model, a .lwo or
     * .bmodel path) is provided under its cooked request, or under
     * generated/model/ for an md6 collision model (sh_mb_model_keys): a
     * bundle that carries or unlocks that model is one the map depends on. */
    nm = f < 0 ? sh_mb_model_keys(lower, keys, 2) : 0;
    for (m = 0; f < 0 && m < nm; ++m) {
        ms_lower(keys[m]);
        f = ms_provider(x, "model", keys[m]);
        for (k = 0; f < 0 && k < x->nfiles; ++k)
            if (x->files[k].served && !strcmp(x->files[k].request, keys[m])) f = k;
    }
    return f;
}

/* A name a manifest row lists resolves when the placed-name rule finds it
 * (ms_name_file), or any served file provides it under any type, or it is a
 * served request path itself. Rows written by a save hold placed names; rows
 * read from a mod.json hold every provided name. */
static int ms_row_name_resolves(const ms_index *x, const char *lower)
{
    int k;
    if (ms_name_file(x, lower) >= 0) return 1;
    for (k = 0; k < x->nfiles; ++k)
        if (x->files[k].served && x->files[k].pname && !strcmp(x->files[k].pname, lower)) return 1;
    return x->slots && ms_find(x->slots, x->mask, lower) >= 0;
}

/* 1 when bundle `b` carries `lower` (a placed name or a request path). */
static int ms_bundle_carries(const ms_index *x, int b, const char *lower)
{
    int k, m;
    char keys[2][SH_MB_REQUEST_MAX + 1];
    const ms_bundle *bd = &x->bundles[b];
    int model = sh_mb_model_keys(lower, keys, 2);
    for (m = 0; m < model; ++m) ms_lower(keys[m]);
    for (k = bd->first_file; k < bd->first_file + bd->file_count; ++k) {
        const ms_file *f = &x->files[k];
        if ((f->pname && !strcmp(f->pname, lower)) || !strcmp(f->request, lower)) return 1;
        for (m = 0; m < model; ++m) if (!strcmp(f->request, keys[m])) return 1;
    }
    return 0;
}

/* The admitted bundle of `salt` at `hash` (NULL: any hash) in the area
 * `place` ("" the root, NULL any map's place), or -1. */
static int ms_salt_in(const ms_index *x, const char *salt, const char *hash, const char *place)
{
    int i;
    for (i = 0; x && i < x->nbundles; ++i) {
        const ms_bundle *b = &x->bundles[i];
        if (!b->admitted || !b->id || strcmp(b->id, salt) || !b->hash) continue;
        if (hash && _stricmp(b->hash, hash)) continue;
        if (!place) { if (!b->place) continue; }
        else if (!place[0]) { if (b->place) continue; }
        else if (!b->place || _stricmp(b->place, place)) continue;
        return i;
    }
    return -1;
}

/* The build that serves a row (ONE-SHAPE rule 4 and the owner's decision of
 * 2026-11-10), in the open map's context (sh_modserve_scope_set_place), the
 * same presence rule the fetch uses (modsync.c):
 *   a stranger's map: its own place's folder of the salt at the row's hash,
 *     else the root folder of the salt when it is at that hash;
 *   the author's own map: the root folder of the salt at any hash, else its
 *     own place's folder at the row's hash.
 * Never another map's place: each map keeps its own copy. -1 when none: the
 * row is missing and the fetch brings its build into the map's place. */
static int ms_row_build(const ms_index *x, const sh_modmanifest_row *row)
{
    char salt[SH_MODMANIFEST_SALT_CAP], place[SH_MODSERVE_PLACE_CAP];
    int own, at = -1;
    if (!x || !row || !sh_salt_valid(row->salt, strlen(row->salt))) return -1;
    strcpy_s(salt, sizeof salt, row->salt);
    ms_lower(salt);
    ms_scope_place(place, sizeof place, &own);
    if (!own && place[0] && row->build[0]) at = ms_salt_in(x, salt, row->build, place);
    if (at < 0) at = ms_salt_in(x, salt, own ? NULL : row->build, "");
    if (at < 0 && own && place[0] && row->build[0]) at = ms_salt_in(x, salt, row->build, place);
    return at;
}

static void ms_ref_fill(const ms_bundle *b, sh_modserve_bundle_ref *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    strncpy_s(out->id, sizeof out->id, b->id, _TRUNCATE);
    strncpy_s(out->hash, sizeof out->hash, b->hash, _TRUNCATE);
    strncpy_s(out->name, sizeof out->name, b->name ? b->name : "", _TRUNCATE);
    strncpy_s(out->folder, sizeof out->folder, b->folder ? b->folder : "", _TRUNCATE);
    strncpy_s(out->place, sizeof out->place, b->place ? b->place : "", _TRUNCATE);
    out->size = (unsigned long long)b->total;
}

int sh_modserve_row_build(const sh_modmanifest_row *row, sh_modserve_bundle_ref *out)
{
    const ms_index *x = g_index;
    int at = ms_row_build(x, row);
    if (out) memset(out, 0, sizeof *out);
    if (at < 0) return 0;
    ms_ref_fill(&x->bundles[at], out);
    return 1;
}

/* The asset rule (modserve.h): a row is served by the build sh_modserve_row_build
 * names when every name it lists resolves, in that build or through the
 * serving index. */
int sh_modserve_row_served_for(const sh_modmanifest_row *row, const char *subject, char *cause, size_t cap)
{
    const ms_index *x = g_index;
    char salt[SH_MODMANIFEST_SALT_CAP], title[300], missing[600];
    const char *what = subject && subject[0] ? subject : "map";
    int i, declared, skipped = -1, absent = 0;
    size_t k, w = 0;
    if (cause && cap) cause[0] = 0;
    if (!row || !sh_salt_valid(row->salt, strlen(row->salt))) {
        if (cause && cap) _snprintf_s(cause, cap, _TRUNCATE, "This %s names a mod this version does not read.", what);
        return SH_MODSERVE_ROW_MISSING;
    }
    /* A build the record knows changes only the editor: the map plays
     * without it, whatever folders are installed. */
    if (ms_editor_only_recorded(x, row->build)) return SH_MODSERVE_ROW_SERVED;
    strcpy_s(salt, sizeof salt, row->salt);
    ms_lower(salt);
    declared = ms_row_build(x, row);
    /* A folder of the salt that was skipped: the one at the row's hash, else,
     * when no folder of the salt serves at all, any. */
    if (declared < 0 && x) {
        int at_hash = -1, any = -1, serving = 0;
        for (i = 0; i < x->nbundles; ++i) {
            const ms_bundle *b = &x->bundles[i];
            if (!b->id || strcmp(b->id, salt)) continue;
            if (b->admitted) serving = 1;
            else if (at_hash < 0 && b->hash && !_stricmp(b->hash, row->build)) at_hash = i;
            else if (any < 0) any = i;
        }
        skipped = at_hash >= 0 ? at_hash : serving ? -1 : any;
    }
    {
        const ms_bundle *b = declared >= 0 ? &x->bundles[declared] : skipped >= 0 ? &x->bundles[skipped] : NULL;
        const char *n;
        for (i = 0; !b && x && i < x->nbundles; ++i)   /* another build of the salt names it */
            if (x->bundles[i].id && !strcmp(x->bundles[i].id, salt) && x->bundles[i].name && x->bundles[i].name[0]) b = &x->bundles[i];
        n = b && b->name && b->name[0] ? b->name : row->name[0] ? row->name : salt;
        strncpy_s(title, sizeof title, n, _TRUNCATE);
    }
    if (declared < 0) {
        if (cause && cap) {
            if (skipped >= 0)
                _snprintf_s(cause, cap, _TRUNCATE, "This %s uses %s, which is installed but cannot be used: %s.", what,
                            title, x->bundles[skipped].cause[0] ? x->bundles[skipped].cause : "its folder was skipped");
            else _snprintf_s(cause, cap, _TRUNCATE, "This %s uses %s, which is not installed.", what, title);
        }
        return SH_MODSERVE_ROW_MISSING;
    }
    missing[0] = 0;
    for (k = 0; k < row->name_count; ++k) {
        char lower[MS_NAME_MAX];
        if (!row->names || !row->names[k] || !row->names[k][0] || strlen(row->names[k]) >= sizeof lower) continue;
        strcpy_s(lower, sizeof lower, row->names[k]);
        ms_lower(lower);
        if (ms_bundle_carries(x, declared, lower) || ms_row_name_resolves(x, lower)) continue;
        if (absent < 3) {
            int n = _snprintf_s(missing + w, sizeof missing - w, _TRUNCATE, "%s%s", absent ? ", " : "", row->names[k]);
            if (n > 0) w += (size_t)n;
        }
        absent++;
    }
    if (!absent) return SH_MODSERVE_ROW_SERVED;
    if (cause && cap) {
        char more[48] = "";
        if (absent > 3) _snprintf_s(more, sizeof more, _TRUNCATE, " and %d more", absent - 3);
        _snprintf_s(cause, cap, _TRUNCATE, "%s is installed but does not provide %s%s, which this %s uses.", title, missing,
                    more, what);
    }
    return SH_MODSERVE_ROW_INCOMPLETE;
}

int sh_modserve_row_served(const sh_modmanifest_row *row, char *cause, size_t cap)
{
    return sh_modserve_row_served_for(row, "map", cause, cap);
}

/* A served file, as the open map serves its path: the build the map declares
 * among several of one salt (ms_scope_compute), else the static winner. */
static int ms_scoped_or(const ms_index *x, int f)
{
    int g;
    if (f < 0) return f;
    g = ms_scoped(x, f);
    return g >= 0 ? g : f;
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
        f = ms_scoped_or(x, ms_name_file(x, lower));
        if (f >= 0 && ms_editor_shadow(&x->files[f])) continue;
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
            int p = ms_scoped_or(x, x->needs[f->need_first + k].file);
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
        f = ms_scoped_or(x, ms_name_file(x, lower));
        if (f < 0 || ms_editor_shadow(&x->files[f])) continue;
        b = &x->bundles[x->files[f].bundle];
        visit(context, b->id, b->hash, (unsigned long long)b->total, names[i]);
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
            visit(context, b->id, b->hash, (unsigned long long)b->total, NULL);
        }
        for (k = 0; k < f->need_count; ++k) {
            int p = ms_scoped_or(x, x->needs[f->need_first + k].file);
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
        int p = ms_scoped(x, c->winner);
        f = &x->files[p >= 0 ? p : c->winner];
        out->game = p < 0;
        out->request = c->request;
        out->providers = c->providers ? c->providers : "";
        out->reason = c->reason ? c->reason : "";
        out->served_id = x->bundles[f->bundle].id;
        out->served_hash = x->bundles[f->bundle].hash;
        out->count = c->count;
        if (sh_modserve_scope_enabled()) {
            size_t blen = 0;
            out->composed = p >= 0 && ms_body_of(f, &blen) != NULL;
            if (p >= 0) out->reason = g_why_text[x->files[c->winner].scope_why];
        } else out->composed = c->composed;
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
        fn(context, b->id && b->id[0] ? b->id : b->folder, b->hash ? b->hash : "",
           b->admitted, b->cause, b->file_count, b->place != NULL);
    }
}

/* Root folders before maps' places, then salt, then folder. */
static const ms_index *g_extra_order_index;
static int ms_extra_order(const void *a, const void *b)
{
    const ms_bundle *x = &g_extra_order_index->bundles[*(const int *)a];
    const ms_bundle *y = &g_extra_order_index->bundles[*(const int *)b];
    int c;
    if ((x->place == NULL) != (y->place == NULL)) return x->place ? 1 : -1;
    c = strcmp(x->id ? x->id : "", y->id ? y->id : "");
    if (!c) c = _stricmp(x->folder ? x->folder : "", y->folder ? y->folder : "");
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
        fn(context, b->id ? b->id : b->folder, b->hash ? b->hash : "", b->place != NULL, b->extra);
    }
    free(order);
    return n;
}

int sh_modserve_each_bundle_registries(sh_modserve_registries_fn fn, void *context)
{
    const ms_index *x = g_index;
    int i, n = 0;
    if (!x || !fn) return 0;
    for (i = 0; i < x->nbundles; ++i) {
        const ms_bundle *b = &x->bundles[i];
        if (!b->admitted || !b->hash) continue;
        fn(context, b->id ? b->id : b->folder, b->hash, b->registries);
        n++;
    }
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
        fn(context, f->request, f->size, b->id ? b->id : b->folder, b->hash ? b->hash : "");
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

/* The bundle has cvar requirements and they are all live now. */
static int ms_gates_live_for(const ms_index *x, const ms_file *f)
{
    const ms_bundle *b = ms_gate_of(x, f);
    return ms_req_needs(b) && InterlockedCompareExchange((volatile LONG *)&b->req_live, 0, 0);
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
    for (rank = 0; rank <= SH_MODSERVE_RANK_MAX; ++rank)
        for (i = 0; i < x->nfiles; ++i) {
            ms_file *f = (ms_file *)&x->files[i], *ef;
            int eff;
            if (f->rank != rank || !f->is_decl || !f->is_shadow || f->is_restore || !f->served || f->withdrawn ||
                !x->bundles[f->bundle].admitted || !f->ptype || !f->pname || (eff = ms_scoped(x, i)) < 0)
                continue;
            ef = (ms_file *)&x->files[eff];   /* the provider that serves the path for the open map */
            if (InterlockedCompareExchange(&ef->reparse_tried, 0, 0) || !ms_stale(x, ef)) continue;
            visited++;
            if (fn(context, f->ptype, f->pname, f->request, x->bundles[ef->bundle].id) == 1)
                InterlockedExchange(&ef->reparse_tried, 1);
        }
    return visited;
}

/* Raw resources first served before their bundle's gates were live: the
 * models and images whose load resolved names against the blacklists,
 * surfaces included (ADD-L13). */
int sh_modserve_each_ungated_raw(sh_modserve_unopened_fn fn, void *context)
{
    const ms_index *x = g_index;
    int i, visited = 0;
    if (!x || !fn) return 0;
    for (i = 0; i < x->nfiles; ++i) {
        ms_file *f = (ms_file *)&x->files[i], *ef;
        int eff;
        if (f->is_decl || !f->served || f->withdrawn || !x->bundles[f->bundle].admitted || !f->ptype || !f->pname ||
            (_stricmp(f->ptype, "baseModel") && _stricmp(f->ptype, "image")) || (eff = ms_scoped(x, i)) < 0)
            continue;
        ef = (ms_file *)&x->files[eff];
        /* A surface (the game's own model or image) is offered too: one the
         * game loaded at boot with the blacklists on holds no materials for
         * its blacklisted names until it is loaded again with them off (the
         * stock spider mesh, ADD-V7a D1). This is the one pass that touches
         * a surface; no plan registers or re-parses one (ADD-L11). A surface
         * never opened through the provider is offered as well, as ms_stale
         * offers a decl: on a boot that builds the mod folders the game loads
         * the mesh while the index is still empty, so the open never reaches
         * the provider and is never marked ungated (ADD-V7b D1). */
        if (InterlockedCompareExchange(&ef->reparse_tried, 0, 0) || !ms_gates_live_for(x, ef) ||
            !(InterlockedCompareExchange(&ef->opened_ungated, 0, 0) ||
              (ms_surface(ef) && !InterlockedCompareExchange(&ef->opens, 0, 0) && !ms_settled_before(x, ef))))
            continue;
        visited++;
        if (fn(context, f->ptype, f->pname, f->request, x->bundles[ef->bundle].id) == 1)
            InterlockedExchange(&ef->reparse_tried, 1);
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
            if (!parent[i] || f->refresh || ms_surface(f)) continue;
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
    return !ms_req_needs(ms_gate_of(x, f)) || ms_gates_live_for(x, f);
}

static int ms_refresh_one(const ms_index *x, ms_file *f, int kind, sh_modserve_refresh_fn fn, void *context)
{
    sh_modserve_refresh_item it;
    int eff = ms_scoped(x, (int)(f - x->files));
    const ms_file *ef = eff >= 0 ? &x->files[eff] : f;   /* the provider serving the path for the open map */
    /* A surface holds the game's own bytes: nothing to refresh, ever. */
    if (ms_surface(f) || (eff >= 0 && ms_surface(ef))) { InterlockedExchange(&f->refresh, 0); return 0; }
    /* A name no bundle serves for the open map has no game bytes to go back
     * to: the engine keeps what it parsed until a map serves the name again. */
    if (eff < 0 && !f->is_shadow) return 0;
    it.type = f->ptype; it.name = f->pname; it.request = f->request; it.bundle_id = x->bundles[ef->bundle].id;
    /* A shadow the map scope withdrew goes back to the game's bytes, which need no gate. */
    it.kind = kind; it.shadow = f->is_shadow;
    it.gone = eff < 0;
    it.gates_live = it.gone ? 1 : ms_gates_ok(x, ef);
    if (fn(context, &it) == 1) {
        InterlockedExchange(&f->refresh, 0);
        /* What the engine holds is settled: not an early decl. */
        if (kind == SH_MODSERVE_RF_DECL) {
            InterlockedExchange(&f->reparse_tried, 1);
            if (eff >= 0) InterlockedExchange((volatile LONG *)&x->files[eff].reparse_tried, 1);
        }
    }
    return 1;
}

/* Whether a file belongs to the part of a refresh asked for. A renderParm
 * goes with the editor part, which runs first, so it is parsed before any
 * material of either part resolves it (SH_MODSERVE_RANK_RENDERPARM). */
static int ms_in_part(const ms_file *f, int part)
{
    int editor = f->editor_closure != 0 || (f->is_decl && f->rank == SH_MODSERVE_RANK_RENDERPARM);
    return part == SH_MODSERVE_PART_ALL || (part == SH_MODSERVE_PART_EDITOR) == editor;
}

int sh_modserve_each_refresh(sh_modserve_refresh_fn fn, void *context)
{
    return sh_modserve_each_refresh_part(fn, context, SH_MODSERVE_PART_ALL);
}

int sh_modserve_each_refresh_part(sh_modserve_refresh_fn fn, void *context, int part)
{
    const ms_index *x = g_index;
    int pass, i, visited = 0;
    if (!x || !fn) return 0;
    /* Changed decls in plan rank order (passes 0 to SH_MODSERVE_RANK_MAX, a
     * renderParm first), then gone shadows, then the decls that inherit from
     * them by generation and rank, then models and images. */
    for (pass = 0; pass <= SH_MODSERVE_RANK_MAX + 3; ++pass) {
        if (pass == SH_MODSERVE_RANK_MAX + 1) {
            /* Gone shadows return to the game's bytes: never the editor part. */
            if (part == SH_MODSERVE_PART_EDITOR) continue;
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
        if (pass == SH_MODSERVE_RANK_MAX + 2) {
            int gen, rank;
            for (gen = 1; gen <= 8; ++gen)
                for (rank = 0; rank <= SH_MODSERVE_RANK_MAX; ++rank)
                    for (i = 0; i < x->nfiles; ++i) {
                        ms_file *f = (ms_file *)&x->files[i];
                        if (!InterlockedCompareExchange(&f->refresh, 0, 0) || !ms_live(x, f) || f->withdrawn ||
                            !f->ptype || !f->pname || !f->is_decl ||
                            f->refresh_gen != gen || f->rank != rank || !ms_in_part(f, part))
                            continue;
                        visited += ms_refresh_one(x, f, SH_MODSERVE_RF_DECL, fn, context);
                    }
            continue;
        }
        for (i = 0; i < x->nfiles; ++i) {
            ms_file *f = (ms_file *)&x->files[i];
            int kind = ms_kind(f);
            if (!InterlockedCompareExchange(&f->refresh, 0, 0) || !ms_live(x, f) || f->withdrawn ||
                !f->ptype || !f->pname || !ms_in_part(f, part))
                continue;
            if (pass <= SH_MODSERVE_RANK_MAX ? (kind != SH_MODSERVE_RF_DECL || f->rank != pass || f->refresh_gen)
                                             : kind == SH_MODSERVE_RF_DECL)
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

int sh_modserve_refresh_editor_pending(void)
{
    const ms_index *x = g_index;
    int i, n = 0;
    if (!x) return 0;
    for (i = 0; i < x->nfiles; ++i) {
        const ms_file *f = &x->files[i];
        int eff;
        if (!f->editor_closure || !ms_live(x, f) || f->withdrawn || !f->ptype || !f->pname)
            continue;
        if (!InterlockedCompareExchange((volatile LONG *)&f->refresh, 0, 0)) continue;
        /* An item waiting for its bundle's gates is not applied now either. */
        eff = ms_scoped(x, i);
        if (eff >= 0 && !ms_gates_ok(x, &x->files[eff])) continue;
        if (eff < 0 && !f->is_shadow) continue;   /* a withdrawn name waits for a map that serves it */
        n++;
    }
    return n;
}

int sh_modserve_refresh_pending_anims(void)
{
    const ms_index *x = g_index;
    int i, n = 0;
    if (!x) return 0;
    for (i = 0; i < x->nfiles; ++i) {
        const ms_file *f = &x->files[i];
        if (!InterlockedCompareExchange((volatile LONG *)&f->refresh, 0, 0) || !ms_live(x, f) || f->withdrawn ||
            !f->ptype || !f->pname || ms_kind(f) != SH_MODSERVE_RF_ANIM)
            continue;
        n++;
    }
    return n;
}

/* A step the kept part runs: one that keeps its decls, and every renderParm
 * (the kept part runs first, and a renderParm is parsed before any material
 * that resolves it: SH_MODSERVE_RANK_RENDERPARM); not one already registered. */
static int ms_split_keeps(const ms_index *x, const sh_modserve_step *s)
{
    if (!s->persist && s->rank != SH_MODSERVE_RANK_RENDERPARM) return 0;
    return !(x && s->file >= 0 && s->file < x->nfiles && x->files[s->file].reg_state == 1);
}

int sh_modserve_plan_split(const sh_modserve_plan *plan, sh_modserve_plan *kept, sh_modserve_plan *rest)
{
    const ms_index *x = g_index;
    int i, nk = 0;
    if (kept) memset(kept, 0, sizeof *kept);
    if (rest) memset(rest, 0, sizeof *rest);
    if (!plan || !kept || !rest) return -1;
    for (i = 0; i < plan->count; ++i) {
        const sh_modserve_step *s = &plan->steps[i];
        /* A step already registered needs no watermark: plan_run skips it. */
        if (ms_split_keeps(x, s)) nk++;
    }
    if (!nk) return 0;
    kept->steps = (sh_modserve_step *)calloc((size_t)nk, sizeof(sh_modserve_step));
    rest->steps = (sh_modserve_step *)calloc((size_t)(plan->count - nk ? plan->count - nk : 1), sizeof(sh_modserve_step));
    if (!kept->steps || !rest->steps) { sh_modserve_plan_free(kept); sh_modserve_plan_free(rest); return -1; }
    for (i = 0; i < plan->count; ++i) {
        const sh_modserve_step *s = &plan->steps[i];
        int keep = ms_split_keeps(x, s);
        sh_modserve_plan *to = keep ? kept : rest;
        to->steps[to->count++] = *s;
        if (s->blocked) to->blocked++;
    }
    return kept->count;
}

void sh_modserve_plan_merge_ready(sh_modserve_plan *plan, const sh_modserve_plan *part)
{
    int i, j;
    if (!plan || !part) return;
    for (j = 0; j < part->count; ++j)
        for (i = 0; i < plan->count; ++i)
            if (plan->steps[i].file == part->steps[j].file) { plan->steps[i].ready = part->steps[j].ready; break; }
}

int sh_modserve_plan_block_failed_needs(sh_modserve_plan *plan)
{
    const ms_index *x = g_index;
    int i, k, n = 0;
    if (!plan || !x) return 0;
    for (i = 0; i < plan->count; ++i) {
        sh_modserve_step *s = &plan->steps[i];
        const ms_file *f;
        if (s->blocked || s->file < 0 || s->file >= x->nfiles) continue;
        f = &x->files[s->file];
        for (k = 0; k < f->need_count; ++k) {
            const ms_need *nd = &x->needs[f->need_first + k];
            if (nd->file >= 0 && nd->file != s->file && x->files[nd->file].reg_state == -1) {
                s->blocked = "needs a decl an editor row keeps, which failed";
                plan->blocked++;
                n++;
                break;
            }
        }
    }
    return n;
}

void sh_modserve_plan_mark_registered(sh_modserve_plan *plan)
{
    const ms_index *x = g_index;
    int i;
    if (!plan || !x) return;
    for (i = 0; i < plan->count; ++i) {
        sh_modserve_step *s = &plan->steps[i];
        s->ready = s->file >= 0 && s->file < x->nfiles && x->files[s->file].reg_state == 1 ? 1 : 0;
    }
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
    /* What the refresh walks visit: a served file, or a registry's synthetic
     * file (player/coop, the conductor, a weapon slot), which serves the
     * generated text under the game's name and is never "served" itself. */
    for (i = 0; i < x->nfiles; ++i)
        if (InterlockedCompareExchange((volatile LONG *)&x->files[i].refresh, 0, 0) && ms_live(x, &x->files[i]) &&
            !x->files[i].withdrawn)
            out->pending++;
    for (i = 0; i < x->ngone; ++i)
        if (InterlockedCompareExchange((volatile LONG *)&x->gone[i].pending, 0, 0)) out->pending++;
}

int sh_modserve_is_restore(const char *request)
{
    const ms_file *f = ms_lookup_file(NULL, request);
    return f && f->is_restore;
}

int sh_modserve_is_surface(const char *request)
{
    const ms_index *x = g_index;
    char lower[MS_NAME_MAX];
    size_t i;
    int f;
    if (!x || !x->nkeys || !request || strlen(request) >= sizeof lower) return 0;
    for (i = 0; request[i]; ++i) lower[i] = ms_fold(request[i]);
    lower[i] = 0;
    __try { f = ms_find(x->slots, x->mask, lower); }
    __except (EXCEPTION_EXECUTE_HANDLER) { f = -1; }
    /* Any build at the path: a surface is the game's file whoever carries it. */
    return f >= 0 && ms_surface(&x->files[f]);
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
