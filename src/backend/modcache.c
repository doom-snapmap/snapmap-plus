/* modcache.c -- see modcache.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modcache.h"
#include "mod_paths.h"
#include "modmanifest.h"
#include "modunit.h"
#include "modbundle.h"
#include "modserve.h"   /* SH_MODSERVE_STANDIN_DIR: the stand-in rows' folder */
#include "modauthor.h"  /* a unit's root as the engine names it, for its label */
#include "modprog.h"
#include "modzip.h"
#include "map_carrier.h"
#include "mp_json.h"
#include "standin_rows.h"
#include "decl_tree.h"
#include "grid_room.h"
#include "backend_log.h"
#include "saved_maps.h"
#include "../common/text.h"

static SRWLOCK g_lock = SRWLOCK_INIT;     /* the rows, the notices, the local index, the settings */
static SRWLOCK g_blob_fetch = SRWLOCK_INIT;   /* a file fetched alone: one at a time (sh_cache_prepare's threads) */
static SRWLOCK g_disk = SRWLOCK_INIT;     /* the cache's folders: a composition put in place, the sweep */
static char *g_mods_root;                  /* override (tests, tools); NULL: the product's */
static char *g_saved_dir, *g_published_dir;
static sh_cache_local_fn g_local_fn;
static void *g_local_ctx;
static sh_mb_resolver g_resolver;
static int g_resolver_set;
static int (*g_rescan)(void);
static const char *(*g_store_root)(void);

static void mc_log(const char *line) { backend_log(line); }
static void mc_logf(const char *fmt, ...)
{
    va_list ap;
    char *line;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    if (line) { mc_log(line); free(line); }
}

void sh_cache_set_mods_root(const char *mods_root)
{
    AcquireSRWLockExclusive(&g_lock);
    free(g_mods_root);
    g_mods_root = mods_root ? sh_text(mods_root) : NULL;
    ReleaseSRWLockExclusive(&g_lock);
    sh_cache_local_changed();
}

void sh_cache_set_store_root(const char *(*fn)(void)) { g_store_root = fn; }

static char *mc_mods_root(void)
{
    char *out;
    AcquireSRWLockShared(&g_lock);
    out = g_mods_root ? sh_text(g_mods_root) : NULL;
    ReleaseSRWLockShared(&g_lock);
    if (!out && g_store_root && g_store_root()) out = sh_text(g_store_root());
    return out;
}

char *sh_cache_root(void)
{
    char *mods = mc_mods_root(), *cache = sh_mod_cache_dir(mods);
    char *out = cache ? sh_textf("%s\\content", cache) : NULL;
    free(mods);
    free(cache);
    return out;
}

void sh_cache_set_resolver(const sh_mb_resolver *resolver)
{
    if (resolver) { g_resolver = *resolver; g_resolver_set = 1; }
    else { memset(&g_resolver, 0, sizeof g_resolver); g_resolver_set = 0; }
}

void sh_cache_set_rescan(int (*rescan)(void)) { g_rescan = rescan; }

int sh_cache_composition_hash(const char *salt, const char *const *units, size_t count, char out[SH_CACHE_HASH_HEX + 1])
{
    return sh_modmanifest_composition_hash(salt, units, count, out);
}

static int mc_hex64(const char *s)
{
    size_t i;
    if (!s) return 0;
    for (i = 0; i < 64; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return s[64] == 0;
}

static void mc_slashes(char *p) { for (; p && *p; p++) if (*p == '/') *p = '\\'; }

static int mc_mkdirs(const char *path)
{
    wchar_t *w = sh_mb_wpath(path), *p;
    int ok = 1;
    if (!w) return 0;
    for (p = w + 1; *p; p++) {
        if (*p != L'\\') continue;
        if (p > w && p[-1] == L':') continue;
        if (p >= w + 4 && !wcsncmp(w, L"\\\\?\\", 4) && p == w + 3) continue;
        *p = 0;
        CreateDirectoryW(w, NULL);
        *p = L'\\';
    }
    if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ok = 0;
    free(w);
    return ok;
}

static int mc_exists(const char *path, unsigned long long *size)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    wchar_t *w = sh_mb_wpath(path);
    int ok = w && GetFileAttributesExW(w, GetFileExInfoStandard, &a) && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    if (ok && size) *size = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    free(w);
    return ok;
}

/* 1 when the file was last written more than `seconds` ago. */
static int mc_untouched_for(const char *path, unsigned long long seconds)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    FILETIME now;
    ULARGE_INTEGER t, n;
    wchar_t *w = sh_mb_wpath(path);
    int ok = w && GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    free(w);
    if (!ok) return 0;
    GetSystemTimeAsFileTime(&now);
    t.LowPart = a.ftLastWriteTime.dwLowDateTime; t.HighPart = a.ftLastWriteTime.dwHighDateTime;
    n.LowPart = now.dwLowDateTime; n.HighPart = now.dwHighDateTime;
    return n.QuadPart > t.QuadPart && (n.QuadPart - t.QuadPart) / 10000000ull > seconds;
}

static int mc_dir_exists(const char *path)
{
    wchar_t *w = sh_mb_wpath(path);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int mc_delete(const char *path)
{
    wchar_t *w = sh_mb_wpath(path);
    int ok = 0;
    if (w) { SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL); ok = DeleteFileW(w) != 0; }
    free(w);
    return ok;
}

static void mc_rmtree_w(const wchar_t *top)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = sh_wtextf(L"%s\\*", top);
    HANDLE h = pattern ? FindFirstFileW(pattern, &fd) : INVALID_HANDLE_VALUE;
    free(pattern);
    while (h != INVALID_HANDLE_VALUE) {
        if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
            wchar_t *child = sh_wtextf(L"%s\\%s", top, fd.cFileName);
            if (child) {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                    mc_rmtree_w(child);
                else { SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL); DeleteFileW(child); }
                free(child);
            }
        }
        if (!FindNextFileW(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
    }
    RemoveDirectoryW(top);
}

static int mc_rmtree(const char *path)
{
    wchar_t *w = sh_mb_wpath(path);
    int ok;
    if (!w) return 0;
    mc_rmtree_w(w);
    ok = GetFileAttributesW(w) == INVALID_FILE_ATTRIBUTES;
    free(w);
    return ok;
}

static int mc_move(const char *from, const char *to)
{
    wchar_t *a = sh_mb_wpath(from), *b = sh_mb_wpath(to);
    DWORD error = ERROR_NOT_ENOUGH_MEMORY;
    if (a && b) {
        for (unsigned attempt = 0; ; attempt++) {
            if (MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING)) { error = ERROR_SUCCESS; break; }
            error = GetLastError();
            if (attempt == 7 || (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION &&
                                 error != ERROR_ACCESS_DENIED)) break;
            Sleep(16u << attempt);
        }
    }
    free(a);
    free(b);
    SetLastError(error);
    return error == ERROR_SUCCESS;
}

static int mc_write(const char *path, const void *data, size_t len)
{
    char *tmp = sh_textf("%s.tmp-%lu", path, GetCurrentThreadId());
    wchar_t *w = tmp ? sh_mb_wpath(tmp) : NULL;
    HANDLE h;
    DWORD put = 0;
    int ok = 0;
    if (w) {
        h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            ok = len <= 0xFFFFFFFFu && WriteFile(h, data, (DWORD)len, &put, NULL) && put == (DWORD)len && FlushFileBuffers(h);
            CloseHandle(h);
        }
    }
    if (ok) ok = mc_move(tmp, path);
    if (!ok && tmp) mc_delete(tmp);
    free(w);
    free(tmp);
    return ok;
}

/* <cache>\<sub> (malloc'd). */
static char *mc_path(const char *fmt, ...)
{
    va_list ap;
    char *root = sh_cache_root(), *rel, *out = NULL;
    if (!root) return NULL;
    va_start(ap, fmt);
    rel = sh_vtextf(fmt, ap);
    va_end(ap);
    if (rel) out = sh_textf("%s\\%s", root, rel);
    free(rel);
    free(root);
    return out;
}

char *sh_cache_blob_path(const char *sha)
{
    char *p;
    if (!mc_hex64(sha)) return NULL;
    p = mc_path("files\\%.2s\\%s", sha, sha);
    if (p && !mc_exists(p, NULL)) { free(p); p = NULL; }
    return p;
}

int sh_cache_put_blob(const char *from, const char *sha, unsigned long long size)
{
    char have[SH_MB_HASH_HEX + 1], *dest, *dir;
    unsigned long long got = 0;
    wchar_t *w;
    int ok;
    if (!from || !mc_hex64(sha)) return 0;
    if (!sh_mb_sha256_file(from, &got, have) || strcmp(have, sha) || got != size) {
        mc_logf("MODS: content: a downloaded file was not the bytes its SHA-256 names (%.16s); it was discarded", sha);
        mc_delete(from);
        return 0;
    }
    dir = mc_path("files\\%.2s", sha);
    dest = mc_path("files\\%.2s\\%s", sha, sha);
    ok = dir && dest && mc_mkdirs(dir);
    if (ok && mc_exists(dest, NULL)) mc_delete(from);
    else if (ok && !(ok = mc_move(from, dest)) && mc_exists(dest, NULL)) {
        /* another fetch stored the same file meanwhile (units share files; four fetch at once) */
        mc_delete(from);
        ok = 1;
    }
    if (ok && (w = sh_mb_wpath(dest)) != NULL) { SetFileAttributesW(w, FILE_ATTRIBUTE_READONLY); free(w); }
    free(dir);
    free(dest);
    return ok;
}

int sh_cache_import_blob(const char *sha, const void *bytes, size_t len)
{
    char *dir, *path;
    int ok;
    if (!mc_hex64(sha)) return 0;
    dir = mc_path("partial");
    path = dir ? sh_textf("%s\\%s.import-%lu.part", dir, sha, GetCurrentThreadId()) : NULL;
    ok = path && mc_mkdirs(dir) && mc_write(path, bytes, len) && sh_cache_put_blob(path, sha, len);
    if (!ok && path) mc_delete(path);
    free(dir);
    free(path);
    return ok;
}

int sh_cache_put_manifest(const char *hash, const void *bytes, size_t len)
{
    char have[SH_MB_HASH_HEX + 1], *dir, *path;
    int ok;
    if (!mc_hex64(hash) || !bytes || !len) return 0;
    sh_mb_sha256_hex(bytes, len, have);
    if (strcmp(have, hash)) return 0;
    dir = mc_path("units");
    path = mc_path("units\\%s.json", hash);
    ok = dir && path && mc_mkdirs(dir) && (mc_exists(path, NULL) || mc_write(path, bytes, len));
    free(dir);
    free(path);
    return ok;
}

char *sh_cache_manifest(const char *hash, size_t *len)
{
    char *path = mc_hex64(hash) ? mc_path("units\\%s.json", hash) : NULL, have[SH_MB_HASH_HEX + 1];
    unsigned char *bytes = path ? sh_mb_read_file(path, 1u << 30, len) : NULL;
    free(path);
    if (!bytes) return NULL;
    sh_mb_sha256_hex(bytes, *len, have);
    if (strcmp(have, hash)) { free(bytes); return NULL; }   /* damaged: treated as absent */
    return (char *)bytes;
}

typedef struct mc_local {
    char sha[SH_MB_HASH_HEX + 1];
    unsigned long long size;
    char *path;
    unsigned long long checked_size;   /* the size and write time the file had when its SHA-256 was last checked */
    FILETIME checked_time;
    int checked;                       /* 1 the bytes were the recorded SHA-256 then, -1 they were not */
} mc_local;
static mc_local *g_local;
static size_t g_nlocal;
static int g_local_ready;
static FILETIME g_local_built_at;   /* when the list was read from the folders (system time) */

static int mc_local_cmp(const void *a, const void *b) { return strcmp(((const mc_local *)a)->sha, ((const mc_local *)b)->sha); }

void sh_cache_set_local(sh_cache_local_fn fn, void *context)
{
    AcquireSRWLockExclusive(&g_lock);
    g_local_fn = fn;
    g_local_ctx = context;
    ReleaseSRWLockExclusive(&g_lock);
}

void sh_cache_local_changed(void)
{
    size_t i;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < g_nlocal; i++) free(g_local[i].path);
    free(g_local);
    g_local = NULL;
    g_nlocal = 0;
    g_local_ready = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

/* Every built mod folder at the top of the mods root (never a dot folder or maps\): its carried files, by SHA-256.
 * Called with the lock held exclusively. */
static void mc_local_build_locked(void)
{
    char *root = g_mods_root ? sh_text(g_mods_root) : (g_store_root && g_store_root() ? sh_text(g_store_root()) : NULL);
    char *pattern = root ? sh_textf("%s\\*", root) : NULL;
    wchar_t *wp = pattern ? sh_mb_wpath(pattern) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h = wp ? FindFirstFileW(wp, &fd) : INVALID_HANDLE_VALUE;
    size_t cap = 0;
    g_local_ready = 1;
    GetSystemTimeAsFileTime(&g_local_built_at);
    while (h != INVALID_HANDLE_VALUE) {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && _wcsicmp(fd.cFileName, L"maps")) {
            char *leaf = sh_mb_utf8(fd.cFileName), *folder = leaf ? sh_textf("%s\\%s", root, leaf) : NULL;
            sh_mb_bundle b;
            memset(&b, 0, sizeof b);
            if (folder && sh_mb_load_folder(folder, &b) && b.validated) {
                size_t i;
                for (i = 0; i < b.file_count; i++) {
                    const sh_mb_file *f = &b.files[i];
                    if (f->stock || f->absent || !f->path || !mc_hex64(f->blob)) continue;
                    if (g_nlocal == cap) {
                        size_t next = cap ? cap * 2 : 1024;
                        mc_local *grown = (mc_local *)realloc(g_local, next * sizeof *grown);
                        if (!grown) break;
                        g_local = grown;
                        cap = next;
                    }
                    strcpy_s(g_local[g_nlocal].sha, sizeof g_local[g_nlocal].sha, f->blob);
                    g_local[g_nlocal].size = f->size;
                    memset(&g_local[g_nlocal], 0, sizeof g_local[g_nlocal]);
                    strcpy_s(g_local[g_nlocal].sha, sizeof g_local[g_nlocal].sha, f->blob);
                    g_local[g_nlocal].size = f->size;
                    g_local[g_nlocal].path = sh_textf("%s\\%s", folder, f->path);
                    mc_slashes(g_local[g_nlocal].path);
                    if (g_local[g_nlocal].path) g_nlocal++;
                }
            }
            sh_mb_free(&b);
            free(folder);
            free(leaf);
        }
        if (!FindNextFileW(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
    }
    if (g_nlocal) qsort(g_local, g_nlocal, sizeof *g_local, mc_local_cmp);
    free(wp);
    free(pattern);
    free(root);
}

/* A local mod's file serves a unit only with the exact SHA-256 the unit names, checked again whenever its size or
 * write time changed since it was last checked. Lock held exclusively. */
static int mc_local_verified(mc_local *l)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    wchar_t *w = sh_mb_wpath(l->path);
    unsigned long long size;
    char have[SH_MB_HASH_HEX + 1];
    int ok = w && GetFileAttributesExW(w, GetFileExInfoStandard, &a);
    free(w);
    if (!ok) return 0;
    size = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    if (l->checked && size == l->checked_size && !CompareFileTime(&a.ftLastWriteTime, &l->checked_time)) return l->checked > 0;
    l->checked_size = size;
    l->checked_time = a.ftLastWriteTime;
    l->checked = size == l->size && sh_mb_sha256_file(l->path, &size, have) && !strcmp(have, l->sha) ? 1 : -1;
    return l->checked > 0;
}

/* A new build manifest can make more local files available by hash. Lock held exclusively. */
static int mc_local_stale_locked(void)
{
    char *root = g_mods_root ? sh_text(g_mods_root) : (g_store_root && g_store_root() ? sh_text(g_store_root()) : NULL);
    char *pattern = root ? sh_textf("%s\\*", root) : NULL;
    wchar_t *wp = pattern ? sh_mb_wpath(pattern) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h = wp ? FindFirstFileW(wp, &fd) : INVALID_HANDLE_VALUE;
    int stale = 0;
    while (h != INVALID_HANDLE_VALUE && !stale) {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && _wcsicmp(fd.cFileName, L"maps")) {
            char *leaf = sh_mb_utf8(fd.cFileName), *json = leaf ? sh_textf("%s\\%s\\" SH_MB_BUILD_MANIFEST, root, leaf) : NULL;
            wchar_t *wj = json ? sh_mb_wpath(json) : NULL;
            WIN32_FILE_ATTRIBUTE_DATA a;
            if (wj && GetFileAttributesExW(wj, GetFileExInfoStandard, &a) &&
                CompareFileTime(&a.ftLastWriteTime, &g_local_built_at) >= 0)
                stale = 1;
            free(wj);
            free(json);
            free(leaf);
        }
        if (!FindNextFileW(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
    }
    if (h != INVALID_HANDLE_VALUE) FindClose(h);
    free(wp);
    free(pattern);
    free(root);
    return stale;
}

/* A file of the list by SHA-256 and size, its bytes checked (malloc'd path), or NULL. Lock held exclusively. */
static char *mc_local_lookup_locked(const char *sha, unsigned long long size)
{
    mc_local key;
    const mc_local *hit;
    strcpy_s(key.sha, sizeof key.sha, sha);
    hit = g_nlocal ? (const mc_local *)bsearch(&key, g_local, g_nlocal, sizeof *g_local, mc_local_cmp) : NULL;
    return hit && hit->size == size && mc_local_verified((mc_local *)hit) ? sh_text(hit->path) : NULL;
}

static char *mc_local_find(const char *sha, unsigned long long size)
{
    char *out = NULL;
    sh_cache_local_fn fn;
    void *ctx;
    AcquireSRWLockShared(&g_lock);
    fn = g_local_fn;
    ctx = g_local_ctx;
    ReleaseSRWLockShared(&g_lock);
    if (fn) return fn(ctx, sha, size);
    AcquireSRWLockExclusive(&g_lock);
    if (!g_local_ready) mc_local_build_locked();
    out = mc_local_lookup_locked(sha, size);
    if (!out && mc_local_stale_locked()) {
        /* a folder built since the list was made (a Refresh or `sh_mods rescan` after boot) carries files
         * the list does not know: read the folders again once, then look again */
        size_t i;
        for (i = 0; i < g_nlocal; i++) free(g_local[i].path);
        free(g_local);
        g_local = NULL;
        g_nlocal = 0;
        mc_local_build_locked();
        out = mc_local_lookup_locked(sha, size);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return out;
}

/* Where a file's bytes are here: a blob, else a local mod's file (malloc'd), else NULL. */
static char *mc_source_of(const char *sha, unsigned long long size)
{
    char *p = sh_cache_blob_path(sha);
    return p ? p : mc_local_find(sha, size);
}

char *sh_cache_file_source(const char *sha, unsigned long long size) { return mc_source_of(sha, size); }

typedef struct mc_file { char *path; char sha[SH_MB_HASH_HEX + 1]; unsigned long long size; } mc_file;
typedef struct mc_unit {
    char hash[SH_MB_HASH_HEX + 1];
    char *bytes; size_t len;          /* the manifest, when here */
    mc_file *files; size_t nfiles;
    char *root, *kind;
    int format;
    int state;                        /* 1 present, 0 files missing, -1 no manifest, -2 unknown format */
    int cause;                        /* SH_CACHE_ABSENT_* when not present */
    unsigned long long bytes_total, bytes_missing;
} mc_unit;

static void mc_unit_free(mc_unit *u)
{
    size_t i;
    if (!u) return;
    for (i = 0; i < u->nfiles; i++) free(u->files[i].path);
    free(u->files);
    free(u->bytes);
    free(u->root);
    free(u->kind);
    memset(u, 0, sizeof *u);
}

/* Parse a manifest's files and root; 0 when it is not a unit manifest. */
static int mc_unit_parse(mc_unit *u)
{
    mpj *m = mpj_parse(u->bytes, u->len);
    const mpj *files = m ? mpj_get(m, "files") : NULL, *root = m ? mpj_get(m, "root") : NULL, *fmt = m ? mpj_get(m, "format") : NULL;
    int i, ok = 0;
    if (!m || !files || files->type != MPJ_ARR || !fmt || !mpj_is_num(fmt)) goto done;
    u->format = (int)mpj_int(fmt);
    if (root && root->type == MPJ_OBJ) {
        const mpj *n = mpj_get(root, "name"), *k = mpj_get(root, "kind");
        if (n && n->type == MPJ_STR) u->root = sh_text(n->text);
        if (k && k->type == MPJ_STR) u->kind = sh_text(k->text);
    }
    if (files->n && !(u->files = (mc_file *)calloc((size_t)files->n, sizeof *u->files))) goto done;
    for (i = 0; i < files->n; i++) {
        const mpj *p = mpj_get(files->items[i], "path"), *s = mpj_get(files->items[i], "sha256"), *z = mpj_get(files->items[i], "size");
        if (!p || p->type != MPJ_STR || !s || s->type != MPJ_STR || !mc_hex64(s->text) || !z || !mpj_is_num(z)) goto done;
        u->files[u->nfiles].path = sh_text(p->text);
        strcpy_s(u->files[u->nfiles].sha, sizeof u->files[u->nfiles].sha, s->text);
        u->files[u->nfiles].size = (unsigned long long)mpj_int(z);
        u->bytes_total += u->files[u->nfiles].size;
        u->nfiles++;
    }
    ok = 1;
done:
    mpj_free(m);
    return ok;
}

/* The unit's manifest and whether each file is here. */
static void mc_unit_load(mc_unit *u, const char *hash)
{
    size_t i;
    memset(u, 0, sizeof *u);
    strcpy_s(u->hash, sizeof u->hash, hash);
    if (!(u->bytes = sh_cache_manifest(hash, &u->len))) { u->state = -1; return; }
    if (!mc_unit_parse(u)) { u->state = -2; return; }
    if (u->format != SH_UNIT_FORMAT) { u->state = -2; return; }
    u->state = 1;
    for (i = 0; i < u->nfiles; i++) {
        char *src = mc_source_of(u->files[i].sha, u->files[i].size);
        if (!src) { u->state = 0; u->bytes_missing += u->files[i].size; }
        free(src);
    }
}

int sh_cache_unit_state(const char *hash, size_t *missing, unsigned long long *missing_bytes, unsigned long long *bytes)
{
    mc_unit u;
    size_t i, n = 0;
    int state;
    if (!mc_hex64(hash)) return -1;
    mc_unit_load(&u, hash);
    for (i = 0; u.state == 0 && i < u.nfiles; i++) {
        char *src = mc_source_of(u.files[i].sha, u.files[i].size);
        if (!src) n++;
        free(src);
    }
    if (missing) *missing = n;
    if (missing_bytes) *missing_bytes = u.bytes_missing;
    if (bytes) *bytes = u.bytes_total;
    state = u.state;
    mc_unit_free(&u);
    return state;
}

typedef struct mc_absent { char *object, *mod; int cause; } mc_absent;
/* What the map saved of a placed name: its entity's class and its reference slot counts (a stand-in keeps them). */
typedef struct mc_placed_info { char *cls; int entrefs, varrefs, known; } mc_placed_info;
typedef struct mc_row {
    char chash[SH_CACHE_HASH_HEX + 1];
    char salt[SH_MODMANIFEST_SALT_CAP];
    char *name;
    size_t n;
    char (*units)[SH_CACHE_HASH_HEX + 1];
    char **roots;
    char **placed; size_t nplaced;     /* names the map places under mods/<salt>/ */
    mc_placed_info *pinfo;             /* per placed name, when the map text was read */
    char *modules;                     /* the row's module records, JSON, or NULL */
    char *labels;                      /* the row's labels: each placed object's name as the author sees it, or NULL */
    mc_absent *absent; size_t nabsent; /* what its last composition stood in for */
    int broken;                        /* a row of no units: the local folder is here and cannot be used */
    int notice;                        /* the absent objects wait for the notice */
} mc_row;

static mc_row *g_rows;
static size_t g_nrows;

static void mc_row_clear(mc_row *r)
{
    size_t i;
    free(r->name);
    if (r->roots) for (i = 0; i < r->n; i++) free(r->roots[i]);
    free(r->roots);
    free(r->units);
    for (i = 0; i < r->nplaced; i++) {
        free(r->placed[i]);
        if (r->pinfo) free(r->pinfo[i].cls);
    }
    free(r->placed);
    free(r->pinfo);
    free(r->modules);
    free(r->labels);
    for (i = 0; i < r->nabsent; i++) { free(r->absent[i].object); free(r->absent[i].mod); }
    free(r->absent);
    memset(r, 0, sizeof *r);
}

static mc_row *mc_row_find_locked(const char *chash)
{
    size_t i;
    for (i = 0; i < g_nrows; i++) if (!strcmp(g_rows[i].chash, chash)) return &g_rows[i];
    return NULL;
}

/* The map's entities under mods/: each one's inherit, class and reference slot counts (entityEntRefs and
 * entityVarRefs rows keyed by uniqueId, the counts the map validator compares with the entity's palette row). */
typedef struct mc_map_ent { const char *inherit, *cls; int entrefs, varrefs; } mc_map_ent;
typedef struct mc_map_ents { mpj *doc; mc_map_ent *v; size_t n; } mc_map_ents;

static int mc_csr_count(const mpj *mm, long long uid)
{
    const mpj *kv = mm ? mpj_get(mm, "keyValues") : NULL;
    long long b, e;
    if (!kv || kv->type != MPJ_ARR || uid < 0 || uid + 1 >= kv->n) return 0;
    b = mpj_int(kv->items[uid]);
    e = mpj_int(kv->items[uid + 1]);
    return e > b ? (int)(e - b) : 0;
}

static void mc_map_ents_read(mc_map_ents *out, const char *json, size_t len)
{
    const mpj *ents, *refs, *er, *vr;
    int i;
    memset(out, 0, sizeof *out);
    if (!json || !len || !(out->doc = mpj_parse(json, len))) return;
    ents = mpj_get(out->doc, "entities");
    refs = mpj_get(out->doc, "references");
    er = refs ? mpj_get(refs, "entityEntRefs") : NULL;
    vr = refs ? mpj_get(refs, "entityVarRefs") : NULL;
    if (!ents || ents->type != MPJ_ARR || !ents->n) return;
    if (!(out->v = (mc_map_ent *)calloc((size_t)ents->n, sizeof *out->v))) return;
    for (i = 0; i < ents->n; i++) {
        const mpj *def = mpj_get(ents->items[i], "entityDef"), *inh = def ? mpj_get(def, "inherit") : NULL,
                  *cls = def ? mpj_get(def, "className") : NULL, *uid = mpj_get(ents->items[i], "uniqueId");
        if (!inh || inh->type != MPJ_STR || _strnicmp(inh->text, "mods/", 5)) continue;
        out->v[out->n].inherit = inh->text;
        out->v[out->n].cls = cls && cls->type == MPJ_STR ? cls->text : "";
        if (uid && mpj_is_num(uid)) {
            out->v[out->n].entrefs = mc_csr_count(er, mpj_int(uid));
            out->v[out->n].varrefs = mc_csr_count(vr, mpj_int(uid));
        }
        out->n++;
    }
}

static void mc_map_ents_free(mc_map_ents *m)
{
    free(m->v);
    mpj_free(m->doc);
    memset(m, 0, sizeof *m);
}

void sh_cache_note_rows(const sh_modmanifest_list *rows, char *const *placed, size_t placed_count)
{
    size_t i, k;
    mc_map_ents ents;
    size_t text_len = 0;
    const char *text = sh_modmanifest_content_text(&text_len);
    if (!rows) return;
    /* Read once, outside the lock: what the map saved of its mod objects, for their stand-ins. */
    mc_map_ents_read(&ents, text, text_len);
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < rows->count; i++) {
        const sh_modmanifest_row *row = &rows->rows[i];
        mc_row *r;
        char prefix[SH_MODMANIFEST_SALT_CAP + 8];
        size_t pl;
        if (!row->pinned) continue;
        r = mc_row_find_locked(row->build);
        if (!r) {
            mc_row *grown = (mc_row *)realloc(g_rows, (g_nrows + 1) * sizeof *grown);
            if (!grown) break;
            g_rows = grown;
            r = &g_rows[g_nrows++];
            memset(r, 0, sizeof *r);
        } else {
            mc_absent *keep = r->absent;
            size_t nkeep = r->nabsent;
            int notice = r->notice;
            r->absent = NULL;
            r->nabsent = 0;
            mc_row_clear(r);
            r->absent = keep;
            r->nabsent = nkeep;
            r->notice = notice;
        }
        strcpy_s(r->chash, sizeof r->chash, row->build);
        strcpy_s(r->salt, sizeof r->salt, row->salt);
        r->name = sh_text(sh_modmanifest_row_name(row)[0] ? row->name : row->salt);
        r->modules = row->modules ? sh_text(row->modules) : NULL;
        r->labels = row->labels ? sh_text(row->labels) : NULL;
        r->broken = row->local_broken;
        r->n = row->unit_count;
        r->units = row->unit_count ? calloc(row->unit_count, sizeof *r->units) : NULL;
        r->roots = row->unit_count ? (char **)calloc(row->unit_count, sizeof *r->roots) : NULL;
        for (k = 0; r->units && r->roots && k < row->unit_count; k++) {
            strcpy_s(r->units[k], sizeof r->units[k], row->units[k]);
            r->roots[k] = sh_text(row->roots[k]);
        }
        _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "mods/%s/", row->salt);
        pl = strlen(prefix);
        for (k = 0; placed && k < placed_count; k++) {
            if (!placed[k] || _strnicmp(placed[k], prefix, pl)) continue;
            {
                char **grown = (char **)realloc(r->placed, (r->nplaced + 1) * sizeof *grown);
                if (!grown) break;
                r->placed = grown;
                r->placed[r->nplaced] = sh_text(placed[k]);
                if (r->placed[r->nplaced]) r->nplaced++;
            }
        }
        if (r->nplaced && ents.n && (r->pinfo = (mc_placed_info *)calloc(r->nplaced, sizeof *r->pinfo)) != NULL) {
            size_t p, e;
            for (p = 0; p < r->nplaced; p++)
                for (e = 0; e < ents.n; e++) {
                    if (_stricmp(ents.v[e].inherit, r->placed[p])) continue;
                    r->pinfo[p].cls = sh_text(ents.v[e].cls);
                    r->pinfo[p].entrefs = ents.v[e].entrefs;
                    r->pinfo[p].varrefs = ents.v[e].varrefs;
                    r->pinfo[p].known = 1;
                    break;
                }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    mc_map_ents_free(&ents);
}

int sh_cache_row_known(const char *chash)
{
    int known;
    if (!mc_hex64(chash)) return 0;
    AcquireSRWLockShared(&g_lock);
    known = mc_row_find_locked(chash) != NULL;
    ReleaseSRWLockShared(&g_lock);
    return known;
}

int sh_cache_absent_causes(const char *chash, unsigned *mask)
{
    char *dir = mc_hex64(chash) ? mc_path("served\\%s", chash) : NULL, *side = dir ? sh_textf("%s.json", dir) : NULL;
    int here = dir && mc_dir_exists(dir) && side && mc_exists(side, NULL);
    if (mask) *mask = 0;
    if (here && mask) {
        size_t len = 0;
        unsigned char *t = sh_mb_read_file(side, 1u << 24, &len);
        mpj *j = t ? mpj_parse((const char *)t, len) : NULL;
        const mpj *c = j ? mpj_get(j, "complete") : NULL, *a = j ? mpj_get(j, "absent") : NULL;
        int k;
        if (!(c && c->type == MPJ_BOOL && c->b)) {
            for (k = 0; a && a->type == MPJ_ARR && k < a->n; k++) {
                const mpj *why = mpj_get(a->items[k], "cause");
                long long cause = why && mpj_is_num(why) ? mpj_int(why) : 0;
                *mask |= 1u << (cause > 0 && cause < 32 ? (unsigned)cause : 0u);
            }
            if (!*mask) *mask = 1u;   /* stood in, no cause recorded */
        }
        mpj_free(j);
        free(t);
    }
    free(dir);
    free(side);
    return here;
}

int sh_cache_composed(const char *chash, int *complete)
{
    char *dir = mc_hex64(chash) ? mc_path("served\\%s", chash) : NULL, *side = dir ? sh_textf("%s.json", dir) : NULL;
    int here = dir && mc_dir_exists(dir) && side && mc_exists(side, NULL);
    if (complete) {
        *complete = 0;
        if (here) {
            size_t len = 0;
            unsigned char *t = sh_mb_read_file(side, 1u << 24, &len);
            mpj *j = t ? mpj_parse((const char *)t, len) : NULL;
            const mpj *c = j ? mpj_get(j, "complete") : NULL;
            *complete = c && c->type == MPJ_BOOL && c->b;
            mpj_free(j);
            free(t);
        }
    }
    free(dir);
    free(side);
    return here;
}

char *sh_cache_object_name(const char *root)
{
    const char *leaf = root ? strrchr(root, '/') : NULL;
    char *out;
    size_t i;
    int up = 1;
    leaf = leaf ? leaf + 1 : (root ? root : "");
    if (!strcmp(leaf, "*")) return sh_text("");   /* a whole-mod row: the mod's own name stands for it */
    if (!(out = sh_text(leaf))) return NULL;
    for (i = 0; out[i]; i++) {
        if (out[i] == '_' || out[i] == '-' || out[i] == '.') { out[i] = ' '; up = 1; continue; }
        if (up && out[i] >= 'a' && out[i] <= 'z') out[i] = (char)(out[i] - 'a' + 'A');
        up = 0;
    }
    return out;
}

/* The name a notice gives an object of row `r`: the name the author sees, carried by the row's labels under
 * the name the map places it by (a unit's root is first spelled as the engine names it), else one made from the root
 * or placed name. malloc'd. */
static char *mc_object_label(const mc_row *r, const char *name, int root)
{
    sh_modmanifest_row probe;
    char *engine = NULL, *label;
    if (!name) return sh_text("");
    if (!strcmp(name, "*")) return sh_text("");   /* a whole-mod row: the mod's own name stands for it */
    memset(&probe, 0, sizeof probe);
    probe.labels = r->labels;
    if (root) engine = sh_ma_engine_name(r->salt, name, strlen(name));
    label = sh_modmanifest_row_label(&probe, engine ? engine : name);
    free(engine);
    return label ? label : sh_cache_object_name(name);
}

/* A mod's name in a player sentence: a row with no name is kept under its salt, which is never shown. */
static const char *mc_mod_label(const char *name, const char *salt)
{
    return name && name[0] && (!salt || strcmp(name, salt)) ? name : "a mod";
}

/* The cause order: the most actionable first. */
static int mc_cause_rank(int cause)
{
    switch (cause) {
    case SH_CACHE_ABSENT_OFFLINE: return 0;
    case SH_CACHE_ABSENT_UNREACHABLE: return 1;
    case SH_CACHE_ABSENT_DISK: return 2;
    case SH_CACHE_ABSENT_NEWER: return 3;
    case SH_CACHE_ABSENT_GAME: return 4;
    case SH_CACHE_ABSENT_NOT_IN_STORE: return 5;
    case SH_CACHE_ABSENT_REMOVED: return 6;
    case SH_CACHE_ABSENT_BROKEN: return 7;
    case SH_CACHE_ABSENT_LOCAL: return 8;
    case SH_CACHE_ABSENT_LACKED: return 9;
    case SH_CACHE_ABSENT_REFUSED: return 10;
    default: return 10;
    }
}

/* How many bytes the game's dialog holds of a notice, with the end (modpublished.c). */
#define MC_NOTICE_MAX 250

/* The subject of an absence sentence. fit 0: every object named; 1: counted, the mods named; 2: counted alone. A whole-mod
 * row's object is empty: the mod's own name stands for it. */
static char *mc_subject(const sh_cache_absent *a, size_t n, int fit)
{
    char *subject = NULL;
    size_t i, k, mods = 0;
    const char *mod0 = NULL;
    int plural = n > 1;
    if (!a || !n) return NULL;
    for (i = 0; i < n; i++) {
        int seen = 0;
        for (k = 0; k < i && !seen; k++) seen = !strcmp(a[k].mod ? a[k].mod : "", a[i].mod ? a[i].mod : "");
        if (!seen) { mods++; if (!mod0) mod0 = a[i].mod; }
    }
    if (fit == 2) return plural ? sh_textf("%u objects", (unsigned)n) : sh_text("an object");
    if (!plural) {
        const char *mod = a[0].mod && a[0].mod[0] ? a[0].mod : "a mod";
        subject = a[0].object && a[0].object[0] ? sh_textf("the %s from %s", a[0].object, mod) : sh_text(mod);
    } else if (fit == 1) {
        subject = mods > 1 ? sh_textf("%u objects from %u mods", (unsigned)n, (unsigned)mods)
                           : sh_textf("%u objects from %s", (unsigned)n, mod0 && mod0[0] ? mod0 : "a mod");
    } else {
        char *list = NULL;
        size_t shown = n > 3 ? 3 : n;
        for (i = 0; i < shown; i++) {
            const char *sep = !i ? "" : (i + 1 == shown && n <= 3) ? " and " : ", ";
            if (a[i].object && a[i].object[0]) (void)sh_text_set(&list, "%s%sthe %s", list ? list : "", sep, a[i].object);
            else (void)sh_text_set(&list, "%s%s%s", list ? list : "", sep, a[i].mod && a[i].mod[0] ? a[i].mod : "a mod");
        }
        if (n > 3) (void)sh_text_set(&list, "%s and %u more", list ? list : "", (unsigned)(n - 3));
        subject = mods > 1 ? sh_textf("%u objects from %u mods (%s)", (unsigned)n, (unsigned)mods, list ? list : "")
                           : sh_textf("%u objects from %s (%s)", (unsigned)n, mod0 && mod0[0] ? mod0 : "a mod", list ? list : "");
        free(list);
    }
    return subject;
}

char *sh_cache_objects_subject(const sh_cache_absent *a, size_t n)
{
    return mc_subject(a, n, 0);
}

/* One cause's sentence around a subject. */
static char *mc_notice_sentence(const char *subject, int cause, int plural)
{
    char *out = NULL;
    switch (cause) {
    case SH_CACHE_ABSENT_OFFLINE:
        out = sh_textf("This map is playing without %s, which %s not on this computer. Connect to the internet and open the map "
                       "again to get %s.", subject, plural ? "are" : "is", plural ? "them" : "it");
        break;
    case SH_CACHE_ABSENT_UNREACHABLE:
        out = plural ? sh_textf("This map is playing without %s: the mod store could not be reached. Open it again later to get them.", subject)
                     : sh_textf("This map is playing without %s: the mod store could not be reached. Open the map again later to get it.", subject);
        break;
    case SH_CACHE_ABSENT_NEWER:
        out = sh_textf("This map is playing without %s, which need%s a newer Snapmap+; run the Snapmap+ installer to update.", subject,
                       plural ? "" : "s");
        break;
    case SH_CACHE_ABSENT_GAME:
        out = sh_textf("This map is playing without %s, which need%s a different version of DOOM's own files.", subject, plural ? "" : "s");
        break;
    case SH_CACHE_ABSENT_NOT_IN_STORE:
        out = sh_textf("This map is playing without %s, which %s not in the mod store yet.", subject, plural ? "are" : "is");
        break;
    case SH_CACHE_ABSENT_REMOVED:
        out = sh_textf("This map is playing without %s, which %s removed from the mod store.", subject, plural ? "were" : "was");
        break;
    case SH_CACHE_ABSENT_DISK:
        out = sh_textf("This map is playing without %s, because there is not enough free disk space.", subject);
        break;
    case SH_CACHE_ABSENT_LOCAL:
        out = sh_textf("This map is playing without %s, which %s not on this computer.", subject, plural ? "are" : "is");
        break;
    case SH_CACHE_ABSENT_BROKEN:
        out = sh_textf("This map is playing without %s: your folder of it cannot be used. The Mods tab says why.", subject);
        break;
    case SH_CACHE_ABSENT_LACKED:
        out = sh_textf("This map is playing without %s, which your folder of the mod no longer has.", subject);
        break;
    case SH_CACHE_ABSENT_REFUSED:
        out = sh_textf("This map is playing without %s because the game could not load it. Fix the file in the Mods tab and press Refresh.", subject);
        break;
    default:
        out = sh_textf("This map is playing without %s.", subject);
        break;
    }
    return out;
}

/* The sentence for the objects a map is playing without: never over what the game's dialog holds, so a long list of names gives
 * way to the count, the cause's tail kept. */
char *sh_cache_notice_text(const sh_cache_absent *a, size_t n)
{
    char *out = NULL;
    size_t i, best = 0;
    int fit;
    if (!a || !n) return NULL;
    for (i = 1; i < n; i++) if (mc_cause_rank(a[i].cause) < mc_cause_rank(a[best].cause)) best = i;
    for (fit = 0; fit <= 2; fit++) {
        char *subject = mc_subject(a, n, fit);
        if (!subject) return NULL;
        free(out);
        out = mc_notice_sentence(subject, a[best].cause, n > 1);
        free(subject);
        if (!out || strlen(out) <= MC_NOTICE_MAX) break;
    }
    return out;
}

int sh_cache_notice_pending(void)
{
    size_t i;
    int any = 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nrows && !any; i++) any = g_rows[i].notice && g_rows[i].nabsent;
    ReleaseSRWLockShared(&g_lock);
    return any;
}

void sh_cache_notice_drop(void)
{
    size_t i;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < g_nrows; i++) g_rows[i].notice = 0;
    ReleaseSRWLockExclusive(&g_lock);
}

char *sh_cache_take_notice(void)
{
    sh_cache_absent *list = NULL;
    size_t n = 0, i, k;
    char *out = NULL;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < g_nrows; i++) {
        mc_row *r = &g_rows[i];
        if (!r->notice) continue;
        r->notice = 0;
        for (k = 0; k < r->nabsent; k++) {
            sh_cache_absent *grown = (sh_cache_absent *)realloc(list, (n + 1) * sizeof *grown);
            if (!grown) break;
            list = grown;
            list[n].object = r->absent[k].object;
            list[n].mod = r->absent[k].mod;
            list[n].cause = r->absent[k].cause;
            n++;
        }
    }
    if (n) out = sh_cache_notice_text(list, n);
    ReleaseSRWLockExclusive(&g_lock);
    free(list);
    return out;
}

/* The source of a composition's files: path -> blob or local file. */
typedef struct mc_src { mc_unit *units; size_t n; } mc_src;

static char *mc_compose_source(void *context, const char *path)
{
    const mc_src *s = (const mc_src *)context;
    size_t u, f;
    for (u = 0; u < s->n; u++) {
        if (s->units[u].state != 1) continue;
        for (f = 0; f < s->units[u].nfiles; f++)
            if (!strcmp(s->units[u].files[f].path, path)) return mc_source_of(s->units[u].files[f].sha, s->units[u].files[f].size);
    }
    return NULL;
}

static const char MC_ENTITY_STANDIN[] =
    "{\n"
    "\tinherit = \"snapmaps/unknown\";\n"
    "}\n";

/* The folder of the stand-in rows: decls/snapeditorentitydef/<this>/<the placed name's path>.decl. The serving index
 * knows its names by it (modserve.h sh_modserve_standin_name): never a unit's miss, never read again once the row's
 * real composition serves. */
#define MC_STANDIN_ROWS SH_MODSERVE_STANDIN_DIR

/* 1 when `cls` reads as a class name (letters, digits, '_'), so it can be written into a decl. */
static int mc_class_name(const char *cls)
{
    const char *c;
    if (!cls || !*cls || strlen(cls) > 128) return 0;
    for (c = cls; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_')) return 0;
    return 1;
}

/* An absent placed object's entityDef: the stock snapmaps/unknown (idEntity, no model, no collision, no AI) with the
 * class the map saved for the entity, so it has that class's defaults. A class other than idEntity derives from it,
 * which is all the entityDef parse asks of an inherit. For an idPlaceableSnapAIEncounter the default encounterToSpawn
 * is NULL, and the engine then logs one level-4 line and never spawns. */
char *sh_cache_standin_def_text(const char *cls)
{
    if (!mc_class_name(cls) || !strcmp(cls, "idEntity")) return sh_text(MC_ENTITY_STANDIN);
    return sh_textf(SH_MODSERVE_STANDIN_CLASS("%s"), cls);
}

/* The stock row a stand-in row inherits for (cls, entrefs, varrefs): exactly that key, else the class's row with
 * no references, else its first; NULL when DOOM has no row of that class. *exact 1 for the exact key. */
static const char *mc_standin_base(const char *cls, int entrefs, int varrefs, int *exact)
{
    size_t i;
    const char *any = NULL, *bare = NULL;
    *exact = 0;
    if (!cls) return NULL;
    for (i = 0; i < SH_STANDIN_ROWS_N; i++) {
        const sh_standin_row_base *b = &SH_STANDIN_ROWS[i];
        if (strcmp(b->cls, cls)) continue;
        if (b->entrefs == entrefs && b->varrefs == varrefs) { *exact = 1; return b->row; }
        if (!any) any = b->row;
        if (!bare && !b->entrefs && !b->varrefs) bare = b->row;
    }
    return bare ? bare : any;
}

char *sh_cache_standin_row_text(const char *def_source, const char *cls, int entrefs, int varrefs)
{
    int exact = 0;
    const char *base = mc_standin_base(mc_class_name(cls) ? cls : NULL, entrefs, varrefs, &exact);
    char *inherit = base ? sh_textf("\tinherit = \"%s\";\n", base) : sh_text(""), *refs = sh_text(""), *out;
    /* Not an exact stock key: the counts the map saved are written over the base's, so the validator's comparison
     * holds; the slots then have the base's paths up to its count and none past it. */
    if (!exact) {
        free(refs);
        refs = sh_textf("\t\tentRefs = {\n\t\t\tnum = %d;\n\t\t}\n\t\tvarRefs = {\n\t\t\tnum = %d;\n\t\t}\n",
                        entrefs < 0 ? 0 : entrefs, varrefs < 0 ? 0 : varrefs);
    }
    out = inherit && refs ? sh_textf("{\n%s"
                                     "\tedit = {\n"
                                     "\t\tentityDef = \"%s\";\n"
                                     "\t\tdisplayNameTag = \"#str_sh_unknown\";\n"
                                     "\t\tdisplayDescriptionTag = \"#str_sh_unknown_desc\";\n"
                                     "\t\tflags = {\n"
                                     "\t\t\tplaceable = false;\n"
                                     "\t\t\teditable = true;\n"
                                     "\t\t\tisMoveable = true;\n"
                                     "\t\t\tisDeletable = true;\n"
                                     "\t\t}\n"
                                     "\t\trenderInfo = {\n"
                                     "\t\t\talwaysDrawBounds = true;\n"
                                     "\t\t\talwaysDrawLabel = true;\n"
                                     "\t\t}\n"
                                     "%s"
                                     "\t}\n"
                                     "}\n", inherit, def_source, refs)
                              : NULL;
    free(inherit);
    free(refs);
    return out;
}

/* A number member of a decl block (0 when absent, as the decl's own default). */
static double mc_dt_num(sh_dt_node *n, const char *key)
{
    sh_dt_node *c = n ? sh_dt_child(n, key) : NULL;
    char buf[64];
    if (!c || !c->val || !c->val_n || c->val_n >= sizeof buf) return 0;
    memcpy(buf, c->val, c->val_n);
    buf[c->val_n] = 0;
    return atof(buf);
}

/* "bounds = { b = { b[0] = {x y z} b[1] = {x y z} } }" as six numbers into the text. */
static void mc_bounds_json(char **t, sh_dt_node *bounds)
{
    sh_dt_node *b = bounds ? sh_dt_child(bounds, "b") : NULL, *lo = b ? sh_dt_child(b, "b[0]") : NULL,
               *hi = b ? sh_dt_child(b, "b[1]") : NULL;
    (void)sh_text_set(t, "%s[%g,%g,%g,%g,%g,%g]", *t ? *t : "", mc_dt_num(lo, "x"), mc_dt_num(lo, "y"), mc_dt_num(lo, "z"),
                      mc_dt_num(hi, "x"), mc_dt_num(hi, "y"), mc_dt_num(hi, "z"));
}

char *sh_cache_module_record(const char *name, const char *decl, size_t len)
{
    char cause[256], *t = NULL;
    sh_dt *tree;
    sh_dt_node *module, *portals;
    int i, first = 1;
    if (!name || !decl || !(tree = sh_dt_parse(decl, len, cause, sizeof cause))) return NULL;
    module = sh_dt_at(sh_dt_root(tree), "edit/module");
    if (!module) { sh_dt_free(tree); return NULL; }
    t = sh_text("{\"name\":\"");
    (void)sh_text_set(&t, "%s%s\",\"bounds\":", t ? t : "", name);
    mc_bounds_json(&t, sh_dt_child(module, "bounds"));
    (void)sh_text_set(&t, "%s,\"portals\":[", t ? t : "");
    portals = sh_dt_child(module, "portals");
    for (i = 0; portals && i < portals->nkids; i++) {
        sh_dt_node *item = portals->kids[i], *dir;
        char d[64];
        if (!item->block || item->key_n < 5 || _strnicmp(item->key, "item[", 5)) continue;
        dir = sh_dt_child(item, "dir");
        d[0] = 0;
        if (dir && dir->val && dir->val_n > 2 && dir->val_n < sizeof d) {
            memcpy(d, dir->val + 1, dir->val_n - 2);   /* the token's quotes off */
            d[dir->val_n - 2] = 0;
        }
        (void)sh_text_set(&t, "%s%s{\"bounds\":", t ? t : "", first ? "" : ",");
        mc_bounds_json(&t, sh_dt_child(item, "bounds"));
        (void)sh_text_set(&t, "%s,\"dir\":\"%s\"}", t ? t : "", d);
        first = 0;
    }
    (void)sh_text_set(&t, "%s]}", t ? t : "");
    sh_dt_free(tree);
    return t;
}

int sh_cache_row_record_modules(sh_modmanifest_row *row, const char *map_json, size_t len, sh_cache_read_fn read,
                                void *context)
{
    mpj *m = map_json && len ? mpj_parse(map_json, len) : NULL;
    const mpj *inst = m ? mpj_get(m, "instances") : NULL;
    char prefix[SH_MODMANIFEST_SALT_CAP + 32], *out = NULL;
    size_t pl;
    int i, k, n = 0;
    if (!row || !read) { mpj_free(m); return -1; }
    if (!inst || inst->type != MPJ_ARR) { mpj_free(m); return 0; }
    _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "maps/modules/mods/%s/", row->salt);
    pl = strlen(prefix);
    for (i = 0; i < inst->n; i++) {
        const mpj *name = mpj_get(inst->items[i], "moduleName");
        size_t dl = 0;
        char *decl, *rec;
        int seen = 0;
        if (!name || name->type != MPJ_STR || _strnicmp(name->text, prefix, pl)) continue;
        for (k = 0; k < i && !seen; k++) {
            const mpj *o = mpj_get(inst->items[k], "moduleName");
            seen = o && o->type == MPJ_STR && !_stricmp(o->text, name->text);
        }
        if (seen || !(decl = read(context, name->text, &dl))) continue;
        rec = sh_cache_module_record(name->text, decl, dl);
        free(decl);
        if (!rec) continue;
        (void)sh_text_set(&out, "%s%s%s", out ? out : "[", n ? "," : "", rec);
        free(rec);
        n++;
    }
    mpj_free(m);
    if (n) {
        (void)sh_text_set(&out, "%s]", out);
        if (!out) return -1;
        free(row->modules);
        row->modules = out;
    }
    return n;
}

/* The six numbers of a record's bounds array. */
static int mc_six(const mpj *a, double v[6])
{
    int i;
    if (!a || a->type != MPJ_ARR || a->n != 6) return 0;
    for (i = 0; i < 6; i++) {
        if (!mpj_is_num(a->items[i])) return 0;
        v[i] = mpj_num(a->items[i]);
    }
    return 1;
}

static void mc_bounds_decl(char **t, const char *indent, const double v[6])
{
    (void)sh_text_set(t, "%s%sbounds = {\n%s\tb = {\n%s\t\tb[0] = {\n%s\t\t\tx = %g;\n%s\t\t\ty = %g;\n%s\t\t\tz = %g;\n"
                      "%s\t\t}\n%s\t\tb[1] = {\n%s\t\t\tx = %g;\n%s\t\t\ty = %g;\n%s\t\t\tz = %g;\n%s\t\t}\n%s\t}\n%s}\n",
                      *t ? *t : "", indent, indent, indent, indent, v[0], indent, v[1], indent, v[2], indent, indent, indent,
                      v[3], indent, v[4], indent, v[5], indent, indent, indent);
}

/* The rebuilt blank room a module of these bounds stands in with: the product's grid room of the bounds' size
 * (modern; clamped to the grid room's limits), "maps/modules/smpgrid/v1/m/<x>_<y>_<z>.decl", and its size. */
static int mc_module_room(const double b[6], char *out, size_t cap, sh_grid_size *size)
{
    float req[3];
    int i;
    for (i = 0; i < 3; i++) req[i] = (float)(b[3 + i] - b[i] > 0 ? b[3 + i] - b[i] : 0);
    if (!sh_grid_clamp(SH_GRID_MODERN, req, size) || !sh_grid_name(size, out, cap)) return 0;
    return strcat_s(out, cap, ".decl") == 0;
}

/* The modules standing in now: a module's logical name ("mods/<salt>/rooms/hall") and the grid room size it
 * stands in as. A composition sets each of its row's module records here when no present unit carries the module, and
 * clears it when one does, so the asset layer answers a stand-in's derived files only while it stands in. */
typedef struct mc_standin_module { char *logical; sh_grid_size size; } mc_standin_module;
static mc_standin_module *g_standin_modules;
static size_t g_nstandin_modules;

static void mc_standin_module_set(const char *logical, const sh_grid_size *size)
{
    size_t i;
    AcquireSRWLockExclusive(&g_lock);
    for (i = 0; i < g_nstandin_modules && _stricmp(g_standin_modules[i].logical, logical); i++) {}
    if (i < g_nstandin_modules) {
        if (size) g_standin_modules[i].size = *size;
        else {
            free(g_standin_modules[i].logical);
            g_standin_modules[i] = g_standin_modules[--g_nstandin_modules];
        }
    } else if (size) {
        mc_standin_module *grown = (mc_standin_module *)realloc(g_standin_modules, (g_nstandin_modules + 1) * sizeof *grown);
        char *copy = sh_text(logical);
        if (grown && copy) {
            g_standin_modules = grown;
            g_standin_modules[g_nstandin_modules].logical = copy;
            g_standin_modules[g_nstandin_modules].size = *size;
            g_nstandin_modules++;
        } else {
            if (grown) g_standin_modules = grown;
            free(copy);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

int sh_cache_module_standin(const char *logical, sh_grid_size *size)
{
    size_t i;
    int found = 0;
    if (!logical) return 0;
    AcquireSRWLockShared(&g_lock);
    for (i = 0; i < g_nstandin_modules && !found; i++)
        if (!_stricmp(g_standin_modules[i].logical, logical)) {
            if (size) *size = g_standin_modules[i].size;
            found = 1;
        }
    ReleaseSRWLockShared(&g_lock);
    return found;
}

static char *mc_module_standin(const char *record_json, size_t len, char **room, sh_grid_size *size_out)
{
    mpj *r = mpj_parse(record_json, len);
    const mpj *portals = r ? mpj_get(r, "portals") : NULL;
    double b[6];
    char name[160], *t = NULL;
    sh_grid_size size;
    int i, n = 0;
    if (room) *room = NULL;
    if (!r || !mc_six(mpj_get(r, "bounds"), b) || !portals || portals->type != MPJ_ARR || !mc_module_room(b, name, sizeof name, &size)) {
        mpj_free(r);
        return NULL;
    }
    /* The room's own walls, floor and lights; the absent module's bounds and portals, so the map's instance, its
     * door slots (RepairAndMigrate's door pass compares their count with the module's portals) and the entities
     * inside keep their places. */
    t = sh_textf("{\n\tinherit = \"%s\";\n\tedit = {\n\t\tmodule = {\n", name);
    if (size_out) *size_out = size;
    mc_bounds_decl(&t, "\t\t\t", b);
    for (i = 0; i < portals->n; i++) if (mc_six(mpj_get(portals->items[i], "bounds"), b)) n++;
    (void)sh_text_set(&t, "%s\t\t\tportals = {\n\t\t\t\tnum = %d;\n", t ? t : "", n);
    for (i = 0, n = 0; i < portals->n; i++) {
        const mpj *dir = mpj_get(portals->items[i], "dir");
        char head[48];
        if (!mc_six(mpj_get(portals->items[i], "bounds"), b)) continue;
        _snprintf_s(head, sizeof head, _TRUNCATE, "\t\t\t\titem[%d] = {\n", n++);
        (void)sh_text_set(&t, "%s%s", t ? t : "", head);
        mc_bounds_decl(&t, "\t\t\t\t\t", b);
        if (dir && dir->type == MPJ_STR && mc_class_name(dir->text))
            (void)sh_text_set(&t, "%s\t\t\t\t\tdir = \"%s\";\n", t ? t : "", dir->text);
        (void)sh_text_set(&t, "%s\t\t\t\t}\n", t ? t : "");
    }
    (void)sh_text_set(&t, "%s\t\t\t}\n\t\t}\n\t}\n}\n", t ? t : "");
    if (room) *room = sh_text(name);
    mpj_free(r);
    return t;
}

char *sh_cache_module_standin_text(const char *record_json, size_t len, char **room)
{
    return mc_module_standin(record_json, len, room, NULL);
}

/* The stock resolver, with the product's grid rooms as the game's: a module stand-in inherits one, and the product
 * derives it from the installed blank room wherever it is asked for. */
static int mc_stock_or_grid(void *context, const char *request)
{
    const sh_mb_resolver *inner = (const sh_mb_resolver *)context;
    sh_grid_size size;
    if (request && sh_grid_parse_name(request, &size)) return 1;
    return inner && inner->stock ? inner->stock(inner->stock_context, request) : -1;   /* -1: not known, as no reader */
}

/* 1 when a present unit carries `path`. */
static int mc_carried(const mc_unit *units, size_t n, const char *path)
{
    size_t u, f;
    for (u = 0; u < n; u++) {
        if (units[u].state != 1) continue;
        for (f = 0; f < units[u].nfiles; f++) if (!_stricmp(units[u].files[f].path, path)) return 1;
    }
    return 0;
}

typedef struct mc_extras { sh_unit_extra *v; char **paths, **texts; size_t n; } mc_extras;

/* Add the stand-in file `path` with `text` (both taken; the bare entity stand-in when text is NULL). */
static void mc_extra_add_text(mc_extras *e, const mc_unit *units, size_t nunits, char *path, char *text)
{
    size_t i;
    if (!path) { free(text); return; }
    for (i = 0; i < e->n; i++) if (!_stricmp(e->paths[i], path)) { free(path); free(text); return; }
    if (mc_carried(units, nunits, path)) { free(path); free(text); return; }
    if (!text && !(text = sh_text(MC_ENTITY_STANDIN))) { free(path); return; }
    {
        sh_unit_extra *gv = (sh_unit_extra *)realloc(e->v, (e->n + 1) * sizeof *gv);
        char **gp = gv ? (char **)realloc(e->paths, (e->n + 1) * sizeof *gp) : NULL;
        char **gt = gp ? (char **)realloc(e->texts, (e->n + 1) * sizeof *gt) : NULL;
        if (gv) e->v = gv;
        if (gp) e->paths = gp;
        if (!gv || !gp || !gt) { free(path); free(text); return; }
        e->texts = gt;
        e->paths[e->n] = path;
        e->texts[e->n] = text;
        e->v[e->n].path = path;
        e->v[e->n].text = text;
        e->v[e->n].len = strlen(text);
        e->n++;
    }
}

static void mc_extra_add(mc_extras *e, const mc_unit *units, size_t nunits, char *path)
{
    mc_extra_add_text(e, units, nunits, path, NULL);
}

/* Every module record of the row whose module no present unit carries gets the rebuilt blank room of its
 * size with its bounds and portals (sh_cache_module_standin_text) at the module's own file, and a module info at its
 * info's name inheriting the blank room's, so the module palette holds it. Returns the stand-ins added. */
static int mc_module_standins(mc_extras *e, const mc_unit *units, size_t nunits, const char *salt, const char *modules)
{
    mpj *a = modules ? mpj_parse(modules, strlen(modules)) : NULL;
    char prefix[SH_MODMANIFEST_SALT_CAP + 32];
    size_t pl;
    int i, added = 0;
    if (!a || a->type != MPJ_ARR) { mpj_free(a); return 0; }
    _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "maps/modules/mods/%s/", salt);
    pl = strlen(prefix);
    for (i = 0; i < a->n; i++) {
        const mpj *name = mpj_get(a->items[i], "name");
        char *rest, *path, *info, *text, *room = NULL, *body, *c;
        size_t rl;
        if (!name || name->type != MPJ_STR || _strnicmp(name->text, prefix, pl) || !name->text[pl]) continue;
        rest = sh_text(name->text + pl);
        for (c = rest; c && *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        if (!rest) continue;
        rl = strlen(rest);
        if (rl > 5 && !strcmp(rest + rl - 5, ".decl")) rest[rl - 5] = 0;
        path = sh_textf("map-modules/%s.decl", rest);
        info = sh_textf("decls/snapmoduleinfo/%s.decl", rest);
        body = mpj_dump(a->items[i], NULL);
        text = body ? mc_module_standin(body, strlen(body), &room, NULL) : NULL;
        if (path && text && !mc_carried(units, nunits, path)) {
            char *stock_info = room ? sh_textf("{\n\tinherit = \"%s\";\n}\n", sh_grid_stock_name(SH_GRID_MODERN) + strlen("maps/modules/"))
                                    : NULL;
            mc_extra_add_text(e, units, nunits, path, text);
            text = NULL;
            path = NULL;
            if (info && stock_info) { mc_extra_add_text(e, units, nunits, info, stock_info); info = NULL; stock_info = NULL; }
            free(stock_info);
            added++;
        }
        free(text);
        free(body);
        free(room);
        free(path);
        free(info);
        free(rest);
    }
    mpj_free(a);
    return added;
}

/* Which of the row's recorded modules stand in now (sh_cache_module_standin): each record whose module no present unit
 * carries stands in as the grid room of its size, and one a present unit carries does not. Every composition of the
 * row marks them, a made one or one already on disk. */
static void mc_module_marks(const mc_unit *units, size_t nunits, const char *salt, const char *modules)
{
    mpj *a = modules ? mpj_parse(modules, strlen(modules)) : NULL;
    char prefix[SH_MODMANIFEST_SALT_CAP + 32];
    size_t pl;
    int i;
    if (!a || a->type != MPJ_ARR) { mpj_free(a); return; }
    _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "maps/modules/mods/%s/", salt);
    pl = strlen(prefix);
    for (i = 0; i < a->n; i++) {
        const mpj *name = mpj_get(a->items[i], "name");
        char logical[256], path[256], room[160];
        double b[6];
        sh_grid_size size;
        size_t n;
        if (!name || name->type != MPJ_STR || _strnicmp(name->text, prefix, pl) || !name->text[pl]) continue;
        n = strlen(name->text);
        if (n < 6 || _stricmp(name->text + n - 5, ".decl") || n - 13 - 5 >= sizeof logical) continue;
        memcpy(logical, name->text + 13, n - 13 - 5);
        logical[n - 13 - 5] = 0;
        _strlwr_s(logical, sizeof logical);
        _snprintf_s(path, sizeof path, _TRUNCATE, "map-modules/%s.decl", logical + strlen("mods/") + strlen(salt) + 1);
        if (mc_carried(units, nunits, path)) mc_standin_module_set(logical, NULL);
        else if (mc_six(mpj_get(a->items[i], "bounds"), b) && mc_module_room(b, room, sizeof room, &size))
            mc_standin_module_set(logical, &size);
    }
    mpj_free(a);
}

/* The sidecar beside a composition: what it holds. */
static int mc_sidecar(const char *path, const mc_row *r, const mc_unit *units, int complete)
{
    char *t = sh_textf("{\"complete\":%s,\"salt\":\"%s\",\"units\":[", complete ? "true" : "false", r->salt);
    size_t i;
    int first = 1, ok;
    for (i = 0; t && i < r->n; i++) {
        if (units[i].state != 1) continue;
        (void)sh_text_set(&t, "%s%s\"%s\"", t, first ? "" : ",", units[i].hash);
        first = 0;
    }
    (void)sh_text_set(&t, "%s],\"absent\":[", t ? t : "");
    first = 1;
    for (i = 0; t && i < r->n; i++) {
        if (units[i].state == 1) continue;
        (void)sh_text_set(&t, "%s%s{\"unit\":\"%s\",\"cause\":%d}", t, first ? "" : ",", units[i].hash, units[i].cause);
        first = 0;
    }
    (void)sh_text_set(&t, "%s]", t ? t : "");
    if (!r->n) {
        /* A row of no units stands in for what the map places: the names it was composed for. */
        (void)sh_text_set(&t, "%s,\"placed\":[", t ? t : "");
        for (i = 0; t && i < r->nplaced; i++) (void)sh_text_set(&t, "%s%s\"%s\"", t, i ? "," : "", r->placed[i]);
        (void)sh_text_set(&t, "%s]", t ? t : "");
    }
    (void)sh_text_set(&t, "%s}", t ? t : "");
    ok = t && mc_write(path, t, strlen(t));
    free(t);
    return ok;
}

/* 1 when the composition's sidecar names exactly the units that are present now (nothing to compose again). */
static int mc_side_same(const char *chash, const mc_unit *units, size_t n)
{
    char *side = mc_path("served\\%s.json", chash);
    size_t len = 0, i, present = 0;
    unsigned char *t = side ? sh_mb_read_file(side, 1u << 24, &len) : NULL;
    mpj *j = t ? mpj_parse((const char *)t, len) : NULL;
    const mpj *u = j ? mpj_get(j, "units") : NULL;
    int same = u && u->type == MPJ_ARR;
    for (i = 0; same && i < n; i++) {
        int k, found = 0;
        if (units[i].state != 1) continue;
        present++;
        for (k = 0; k < u->n && !found; k++) found = mpj_str_is(u->items[k], units[i].hash);
        same = found;
    }
    if (same && (size_t)u->n != present) same = 0;
    mpj_free(j);
    free(t);
    free(side);
    return same;
}

/* 1 when a row of no units' composition was made for exactly the names the map places now. */
static int mc_side_same_placed(const char *chash, const mc_row *r)
{
    char *side = mc_path("served\\%s.json", chash);
    size_t len = 0, i;
    unsigned char *t = side ? sh_mb_read_file(side, 1u << 24, &len) : NULL;
    mpj *j = t ? mpj_parse((const char *)t, len) : NULL;
    const mpj *p = j ? mpj_get(j, "placed") : NULL;
    int same = p && p->type == MPJ_ARR && (size_t)p->n == r->nplaced;
    for (i = 0; same && i < r->nplaced; i++) {
        int k, found = 0;
        for (k = 0; k < p->n && !found; k++)
            found = p->items[k]->type == MPJ_STR && p->items[k]->text && !_stricmp(p->items[k]->text, r->placed[i]);
        same = found;
    }
    mpj_free(j);
    free(t);
    free(side);
    return same;
}

/* The absence causes the composition's sidecar recorded, for the absent units whose cause is still 0: what the last
 * attempt to get each one learned (the store's answer at a fetch), which a later load that asks nothing repeats. */
static void mc_side_causes(const char *chash, mc_unit *units, size_t n)
{
    char *side = mc_path("served\\%s.json", chash);
    size_t len = 0, i;
    unsigned char *t = side ? sh_mb_read_file(side, 1u << 24, &len) : NULL;
    mpj *j = t ? mpj_parse((const char *)t, len) : NULL;
    const mpj *a = j ? mpj_get(j, "absent") : NULL;
    int k;
    for (k = 0; a && a->type == MPJ_ARR && k < a->n; k++) {
        const mpj *u = mpj_get(a->items[k], "unit"), *c = mpj_get(a->items[k], "cause");
        for (i = 0; u && c && mpj_is_num(c) && i < n; i++)
            if (units[i].state != 1 && !units[i].cause && mpj_str_is(u, units[i].hash)) units[i].cause = (int)mpj_int(c);
    }
    mpj_free(j);
    free(t);
    free(side);
}

/* Compose row `chash`. The absence cause of unit i when it is not here is causes[i] (a fetch's answer), else the one
 * the composition on disk recorded, else `cause` (SH_CACHE_ABSENT_OFFLINE: nothing asked). A composition that is not
 * made again keeps its sidecar, rewritten only when a fetch brought causes. */
static int mc_product_ok(const mc_unit *u);
static int mc_compose(const char *chash, const int *causes, int cause, char **why, int *built)
{
    mc_row snapshot;
    mc_unit *units = NULL;
    mc_extras extras = { 0 };
    const char **manifests = NULL;
    size_t *lens = NULL, nm = 0, i, k;
    char *dest = NULL, *staging = NULL, *side = NULL, *old = NULL, *reason = NULL;
    sh_mb_bundle out;
    sh_mb_resolver resolver, inner;
    int ok = 0, complete = 1, had_complete = 0, here, modules = 0;
    mc_src src;

    if (why) *why = NULL;
    if (built) *built = 0;
    memset(&snapshot, 0, sizeof snapshot);
    memset(&out, 0, sizeof out);
    AcquireSRWLockShared(&g_lock);
    {
        const mc_row *r = mc_row_find_locked(chash);
        if (r) {
            strcpy_s(snapshot.chash, sizeof snapshot.chash, r->chash);
            strcpy_s(snapshot.salt, sizeof snapshot.salt, r->salt);
            snapshot.name = sh_text(r->name ? r->name : r->salt);
            snapshot.modules = r->modules ? sh_text(r->modules) : NULL;
            snapshot.labels = r->labels ? sh_text(r->labels) : NULL;
            snapshot.broken = r->broken;
            snapshot.n = r->n;
            snapshot.units = r->n ? calloc(r->n, sizeof *snapshot.units) : NULL;
            snapshot.roots = r->n ? (char **)calloc(r->n, sizeof *snapshot.roots) : NULL;
            for (i = 0; snapshot.units && snapshot.roots && i < r->n; i++) {
                strcpy_s(snapshot.units[i], sizeof snapshot.units[i], r->units[i]);
                snapshot.roots[i] = sh_text(r->roots[i]);
            }
            snapshot.placed = r->nplaced ? (char **)calloc(r->nplaced, sizeof *snapshot.placed) : NULL;
            for (i = 0; snapshot.placed && i < r->nplaced; i++) snapshot.placed[i] = sh_text(r->placed[i]);
            snapshot.nplaced = snapshot.placed ? r->nplaced : 0;
            if (snapshot.nplaced && r->pinfo &&
                (snapshot.pinfo = (mc_placed_info *)calloc(snapshot.nplaced, sizeof *snapshot.pinfo)) != NULL) {
                for (i = 0; i < snapshot.nplaced; i++) {
                    snapshot.pinfo[i] = r->pinfo[i];
                    snapshot.pinfo[i].cls = r->pinfo[i].cls ? sh_text(r->pinfo[i].cls) : NULL;
                }
            }
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (!snapshot.chash[0]) { if (why) *why = sh_text("the map's row is not known"); return 0; }
    if (snapshot.n && (!snapshot.units || !snapshot.roots)) { reason = sh_text("out of memory"); goto done; }

    units = snapshot.n ? (mc_unit *)calloc(snapshot.n, sizeof *units) : NULL;
    manifests = snapshot.n ? (const char **)calloc(snapshot.n, sizeof *manifests) : NULL;
    lens = snapshot.n ? (size_t *)calloc(snapshot.n, sizeof *lens) : NULL;
    if (snapshot.n && (!units || !manifests || !lens)) { reason = sh_text("out of memory"); goto done; }
    for (i = 0; i < snapshot.n; i++) {
        mc_unit_load(&units[i], snapshot.units[i]);
        if (units[i].state == 1 && !mc_product_ok(&units[i])) units[i].state = -2;
        if (units[i].state == 1) { manifests[nm] = units[i].bytes; lens[nm] = units[i].len; nm++; continue; }
        complete = 0;
        units[i].cause = units[i].state == -2 ? SH_CACHE_ABSENT_NEWER : (causes && causes[i]) ? causes[i] : 0;
        if (!units[i].root) units[i].root = sh_text(snapshot.roots[i]);
    }
    /* A row of no units (a local row whose folder is not here): every object the map places under its namespace
     * stands in. */
    if (!snapshot.n) complete = 0;
    here = sh_cache_composed(chash, &had_complete);
    mc_module_marks(units, snapshot.n, snapshot.salt, snapshot.modules);
    if (!complete) {
        if (here) mc_side_causes(chash, units, snapshot.n);
        for (i = 0; i < snapshot.n; i++) if (units[i].state != 1 && !units[i].cause) units[i].cause = cause;
    }
    if (here && had_complete) { ok = 1; complete = 1; goto record; }   /* the same units compose the same bytes */
    if (here && !complete &&
        (snapshot.n ? mc_side_same(chash, units, snapshot.n) : mc_side_same_placed(chash, &snapshot))) {   /* nothing new */
        if (causes) {
            char *sp = mc_path("served\\%s.json", chash);
            AcquireSRWLockExclusive(&g_disk);
            if (sp) (void)mc_sidecar(sp, &snapshot, units, 0);
            ReleaseSRWLockExclusive(&g_disk);
            free(sp);
        }
        ok = 1;
        goto record;
    }

    /* Stand-ins for what is absent: every name the map places under the namespace that no present unit
     * provides (first, so a placed name keeps its class and row), then the absent roots that are entities. */
    if (!complete) {
        for (k = 0; k < snapshot.nplaced; k++) {
            const char *rest = snapshot.placed[k] + strlen("mods/") + strlen(snapshot.salt) + 1;
            const mc_placed_info *pi = snapshot.pinfo && snapshot.pinfo[k].known ? &snapshot.pinfo[k] : NULL;
            char *lower = sh_text(rest), *c, *def_path, *def_source, *row_path;
            for (c = lower; c && *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
            if (!lower) continue;
            def_path = sh_textf("entities/%s.decl", lower);
            def_source = sh_textf("entities/%s", lower);
            /* What the map saved of the entity decides both stand-ins: the definition keeps its class, and a palette
             * row names it with the reference slots the map saved, because the map validator (RepairAndMigrate)
             * refuses a placed entity whose name no palette row carries, or whose slot counts differ from its
             * row's. A map text not read: no row. */
            if (mc_carried(units, snapshot.n, def_path)) { free(def_path); def_path = NULL; }
            if (def_path) {
                mc_extra_add_text(&extras, units, snapshot.n, def_path, sh_cache_standin_def_text(pi ? pi->cls : NULL));
                if (pi && def_source) {
                    row_path = sh_textf("decls/snapeditorentitydef/" MC_STANDIN_ROWS "/%s.decl", lower);
                    mc_extra_add_text(&extras, units, snapshot.n, row_path,
                                      sh_cache_standin_row_text(def_source, pi->cls, pi->entrefs, pi->varrefs));
                }
            }
            free(def_source);
            free(lower);
        }
        for (i = 0; i < snapshot.n; i++) {
            if (units[i].state == 1 || !snapshot.roots[i]) continue;
            if (!strncmp(snapshot.roots[i], "entities/", 9)) mc_extra_add(&extras, units, snapshot.n, sh_textf("%s.decl", snapshot.roots[i]));
        }
        modules = mc_module_standins(&extras, units, snapshot.n, snapshot.salt, snapshot.modules);
    }

    dest = mc_path("served\\%s", chash);
    side = mc_path("served\\%s.json", chash);
    staging = mc_path("served\\%s.staging-%lu-%llu", chash, GetCurrentThreadId(), (unsigned long long)GetTickCount64());
    if (!dest || !side || !staging) { reason = sh_text("out of memory"); goto done; }
    {
        char *served = mc_path("served");
        int made = served && mc_mkdirs(served);
        free(served);
        if (!made) { reason = sh_text("the content cache's folder could not be made"); goto done; }
    }
    if (!nm && !extras.n) {
        /* Nothing of the row is here and nothing stands in (an absent module): the row serves nothing. */
        complete = 0;
        ok = mc_sidecar(side, &snapshot, units, 0) && mc_mkdirs(dest);
        if (!ok) reason = sh_text("the content cache could not be written");
        if (ok && built) *built = 1;
        goto record;
    }
    memset(&resolver, 0, sizeof resolver);
    if (g_resolver_set) resolver = g_resolver;
    if (modules) {
        /* A module stand-in inherits a grid room, which the game's archives do not list. */
        inner = resolver;
        resolver.stock = mc_stock_or_grid;
        resolver.stock_context = &inner;
    }
    src.units = units;
    src.n = snapshot.n;
    if (!sh_unit_compose_ex(manifests, lens, nm, mc_compose_source, &src, extras.v, extras.n, 1, snapshot.salt,
                            snapshot.name, &resolver, staging, &out, &reason)) {
        goto done;
    }
    sh_mb_free(&out);
    /* Keep the previous files until the new files and their sidecar are both in place. */
    AcquireSRWLockExclusive(&g_disk);
    if (sh_cache_composed(chash, &had_complete) && had_complete) {
        ReleaseSRWLockExclusive(&g_disk);
        mc_rmtree(staging);
        complete = ok = 1;
        goto record;
    }
    if (mc_dir_exists(dest)) {
        old = sh_textf("%s.old-%llu", dest, (unsigned long long)GetTickCount64());
        if (!old || !mc_move(dest, old)) {
            ReleaseSRWLockExclusive(&g_disk);
            mc_rmtree(staging);
            reason = sh_text("the earlier composition is in use; it is replaced at the next start");
            goto done;
        }
    }
    if (!mc_move(staging, dest)) {
        mc_logf("MODS: content: moving the prepared composition failed (Windows error %lu)", GetLastError());
    } else if (mc_sidecar(side, &snapshot, units, complete)) {
        ok = 1;
    } else {
        mc_logf("MODS: content: the composition's sidecar could not be replaced");
        if (!mc_move(dest, staging))
            mc_logf("MODS: content: moving the uncommitted composition aside failed (Windows error %lu)", GetLastError());
    }
    if (!ok && old && !mc_move(old, dest))
        mc_logf("MODS: content: the earlier composition is retained at '%s' (Windows error %lu)", old, GetLastError());
    ReleaseSRWLockExclusive(&g_disk);
    if (!ok) { reason = sh_text("the composition could not be put in place"); goto done; }
    if (old) mc_rmtree(old);
    mc_logf("MODS: content: %s composed from %zu unit(s)%s%s", snapshot.name, nm, complete ? "" : ", with stand-ins for ",
            complete ? "" : "the objects that are not here");
    if (built) *built = 1;

record:
    /* What the load's notice names: the objects of the units that are not here. */
    AcquireSRWLockExclusive(&g_lock);
    {
        mc_row *r = mc_row_find_locked(chash);
        if (r) {
            for (i = 0; i < r->nabsent; i++) { free(r->absent[i].object); free(r->absent[i].mod); }
            free(r->absent);
            r->absent = NULL;
            r->nabsent = 0;
            r->notice = 0;
            if (!complete && snapshot.n) {
                r->absent = (mc_absent *)calloc(snapshot.n, sizeof *r->absent);
                for (i = 0; r->absent && i < snapshot.n; i++) {
                    if (units[i].state == 1) continue;
                    r->absent[r->nabsent].object = mc_object_label(&snapshot, snapshot.roots[i], 1);
                    r->absent[r->nabsent].mod = sh_text(mc_mod_label(snapshot.name, snapshot.salt));
                    r->absent[r->nabsent].cause = units[i].cause;
                    r->nabsent++;
                }
                r->notice = r->nabsent > 0;
            } else if (!snapshot.n && snapshot.nplaced) {
                /* A local row whose folder is not here: each object the map places of it, an entity's inherit
                 * (a name only a value spells, a skin, is no object); every name when the map text was not read. */
                int read = 0;
                for (i = 0; snapshot.pinfo && i < snapshot.nplaced; i++) read |= snapshot.pinfo[i].known;
                r->absent = (mc_absent *)calloc(snapshot.nplaced, sizeof *r->absent);
                for (i = 0; r->absent && i < snapshot.nplaced; i++) {
                    if (read && !snapshot.pinfo[i].known) continue;
                    r->absent[r->nabsent].object = mc_object_label(&snapshot, snapshot.placed[i], 0);
                    r->absent[r->nabsent].mod = sh_text(mc_mod_label(snapshot.name, snapshot.salt));
                    r->absent[r->nabsent].cause = snapshot.broken == SH_MODMANIFEST_LOCAL_REFUSED ? SH_CACHE_ABSENT_REFUSED :
                                                  snapshot.broken == SH_MODMANIFEST_LOCAL_LACKS ? SH_CACHE_ABSENT_LACKED
                                                  : snapshot.broken ? SH_CACHE_ABSENT_BROKEN : SH_CACHE_ABSENT_LOCAL;
                    r->nabsent++;
                }
                r->notice = r->nabsent > 0;
            }
        }
    }
    ReleaseSRWLockExclusive(&g_lock);

done:
    if (!ok && staging && mc_dir_exists(staging)) mc_rmtree(staging);
    if (!ok && reason) mc_logf("MODS: content: %s could not be composed: %s", snapshot.name ? snapshot.name : snapshot.salt, reason);
    if (why) *why = reason;
    else free(reason);
    for (i = 0; units && i < snapshot.n; i++) mc_unit_free(&units[i]);
    free(units);
    free((void *)manifests);
    free(lens);
    for (i = 0; i < extras.n; i++) { free(extras.paths[i]); free(extras.texts[i]); }
    free(extras.paths);
    free(extras.texts);
    free(extras.v);
    free(dest);
    free(side);
    free(staging);
    free(old);
    mc_row_clear(&snapshot);
    return ok;
}

int sh_cache_compose_now(const char *chash, int cause, char **why)
{
    return mc_compose(chash, NULL, cause ? cause : SH_CACHE_ABSENT_OFFLINE, why, NULL);
}

/* A failure's absence cause. */
static int mc_net_cause(int code, const modnet_result *r)
{
    /* No name resolved: this computer is not connected, not a store that did not answer. */
    if (code == MODNET_E_NET && r && r->offline) return SH_CACHE_ABSENT_OFFLINE;
    if (code == MODNET_E_BLOCKED) return SH_CACHE_ABSENT_REMOVED;
    if (code == MODNET_E_RETIRED) return SH_CACHE_ABSENT_NEWER;   /* a 410: this Snapmap+ is too old */
    if (code == MODNET_E_NOT_FOUND) return SH_CACHE_ABSENT_NOT_IN_STORE;
    if (code == MODNET_E_ARG && r && r->message && strstr(r->message, "(error 112)")) return SH_CACHE_ABSENT_DISK;
    return SH_CACHE_ABSENT_UNREACHABLE;
}

int sh_cache_net_cause(int code)
{
    return mc_net_cause(code, NULL);
}

int sh_cache_net_retry_ms(int code, int attempt)
{
    if (attempt < 0 || attempt >= 4) return -1;
    switch (code) {
    case MODNET_E_NET: case MODNET_E_TIMEOUT: case MODNET_E_RATE: case MODNET_E_UNAVAILABLE:
        return 250 << attempt;   /* 250, 500, 1000, 2000 ms */
    default:
        return -1;
    }
}

/* One blob into files\, resumed from partial\, checked; one retry after a bad download, and up to four after a
 * failure in the network (sh_cache_net_retry_ms). 0, or a cause. */
static int mc_fetch_blob(const char *sha, unsigned long long size, const modnet_io *io)
{
    char *dir = mc_path("partial"), *part = mc_path("partial\\%s.part", sha);
    int tries, net = 0, cause = SH_CACHE_ABSENT_UNREACHABLE;
    if (!dir || !part || !mc_mkdirs(dir)) { free(dir); free(part); return SH_CACHE_ABSENT_DISK; }
    for (tries = 0; tries < 2; tries++) {
        unsigned long long have = 0;
        modnet_result r;
        int code, wait;
        if (!mc_exists(part, &have) || have > size) have = 0;
        code = modnet_get_blob(sha, part, have, io, &r);
        if (code != MODNET_OK && (wait = sh_cache_net_retry_ms(code, net)) >= 0) {
            mc_logf("MODS: content: a unit's file %.16s stopped (%s); tried again in %d ms", sha,
                    r.message ? r.message : modnet_code_text(code), wait);
            Sleep((DWORD)wait);
            net++;
            tries--;   /* a network retry is not one of the bad-download tries */
            continue;
        }
        if (code != MODNET_OK) {
            cause = mc_net_cause(code, &r);
            if (code == MODNET_E_CANCELLED) cause = -1;
            else mc_logf("MODS: content: a unit's file %.16s did not download (%s; HTTP %d, %llu of %llu bytes here)", sha,
                         r.message ? r.message : modnet_code_text(code), r.status, have + r.received, size);
            break;
        }
        if (sh_cache_put_blob(part, sha, size)) { cause = 0; break; }
        /* the bytes were not the blob's: discarded by put_blob; once more from the start */
    }
    free(dir);
    free(part);
    return cause;
}

/* The archive of a held unit (one request): unit.json first, then the files this computer lacks (every file
 * when its manifest is not here or it lacks all, else those a mask names, bit i being the manifest's file i). A
 * transfer the network broke is resumed; the manifest is checked against the unit's hash and each file against
 * its SHA-256. 0, a cause, or -1 cancelled. */
static int mc_fetch_archive(mc_unit *u, const char *hash, const modnet_io *io)
{
    unsigned char *mask = NULL;
    size_t mask_len = 0, f, lacking = 0;
    char *dir = mc_path("partial"), *part = NULL, *cause_text = NULL;
    unsigned long long have = 0, started = GetTickCount64();
    modnet_result r;
    sh_zip_reader z;
    size_t stored = 0;
    int code = MODNET_OK, cause = 0, net = 0;
    if (u->state == 0 && u->nfiles) {
        mask_len = (u->nfiles + 7) / 8;
        if ((mask = (unsigned char *)calloc(mask_len, 1)) == NULL) { free(dir); return SH_CACHE_ABSENT_DISK; }
        for (f = 0; f < u->nfiles; f++) {
            char *src = mc_source_of(u->files[f].sha, u->files[f].size);
            if (src) { free(src); continue; }
            mask[f >> 3] |= (unsigned char)(1u << (f & 7));
            lacking++;
        }
        if (lacking == u->nfiles) { free(mask); mask = NULL; mask_len = 0; }   /* every file: no mask */
    }
    if (mask) {
        char msha[SH_MB_HASH_HEX + 1];
        sh_mb_sha256_hex(mask, mask_len, msha);
        part = mc_path("partial\\%s.%.16s.zip.part", hash, msha);   /* another mask is another archive */
    } else part = mc_path("partial\\%s.zip.part", hash);
    if (!dir || !part || !mc_mkdirs(dir)) { cause = SH_CACHE_ABSENT_DISK; goto done; }
    for (;;) {
        int wait;
        if (!mc_exists(part, &have)) have = 0;
        code = modnet_get_unit_archive(hash, mask, mask_len, part, have, io, &r);
        if (code == MODNET_OK || (wait = sh_cache_net_retry_ms(code, net)) < 0) break;
        mc_logf("MODS: content: unit %.16s's archive stopped (%s); tried again in %d ms", hash,
                r.message ? r.message : modnet_code_text(code), wait);
        Sleep((DWORD)wait);
        net++;
    }
    if (code == MODNET_E_CANCELLED) { cause = -1; goto done; }
    if (code != MODNET_OK) {
        cause = mc_net_cause(code, &r);
        mc_logf("MODS: content: unit %.16s's archive did not download (%s; HTTP %d)", hash,
                r.message ? r.message : modnet_code_text(code), r.status);
        goto done;
    }
    memset(&z, 0, sizeof z);
    if (!sh_zip_reader_open(&z, part, &cause_text)) {
        mc_logf("MODS: content: unit %.16s's archive did not read (%s)", hash, cause_text ? cause_text : "?");
        free(cause_text);
        mc_delete(part);
        cause = SH_CACHE_ABSENT_UNREACHABLE;
        goto done;
    }
    if (u->state == -1) {
        /* The manifest arrives as the archive's first entry; it must be the unit's (its hash). */
        long idx = sh_zip_reader_find(&z, "unit.json");
        size_t len = 0;
        unsigned char *bytes = idx >= 0 ? sh_zip_reader_read(&z, idx, (size_t)1 << 28, &len, &cause_text) : NULL;
        free(cause_text);
        cause_text = NULL;
        if (!bytes || !sh_cache_put_manifest(hash, bytes, len)) {
            mc_logf("MODS: content: unit %.16s's archive carries no manifest of the unit", hash);
            cause = SH_CACHE_ABSENT_UNREACHABLE;
        }
        free(bytes);
        mc_unit_free(u);
        mc_unit_load(u, hash);
    }
    for (f = 0; !cause && u->state >= 0 && f < u->nfiles; f++) {
        long idx;
        unsigned char *bytes;
        size_t len = 0;
        char *tmp, *src = mc_source_of(u->files[f].sha, u->files[f].size);
        if (src) { free(src); continue; }
        if ((idx = sh_zip_reader_find(&z, u->files[f].path)) < 0) continue;
        bytes = sh_zip_reader_read(&z, idx, (size_t)u->files[f].size + 1, &len, &cause_text);
        free(cause_text);
        cause_text = NULL;
        if (!bytes) continue;
        tmp = mc_path("partial\\%s.%lu.part", u->files[f].sha, GetCurrentThreadId());
        if (!tmp || !mc_write(tmp, bytes, len)) cause = SH_CACHE_ABSENT_DISK;
        else if (sh_cache_put_blob(tmp, u->files[f].sha, u->files[f].size)) stored++;
        free(tmp);
        free(bytes);
    }
    /* What one archive brought, so a live check reads the requests and the time a unit took. */
    (void)mc_exists(part, &have);
    mc_logf("MODS: content: unit %.16s's archive: %zu file(s) stored of %zu in the unit, %llu bytes, %s, "
            "%d network retr%s, %.1f s", hash, stored, u->state >= 0 ? u->nfiles : 0, have,
            mask ? "the files this computer lacked (a mask)" : "every file", net, net == 1 ? "y" : "ies",
            (double)(GetTickCount64() - started) / 1000.0);
    sh_zip_reader_close(&z);
    mc_delete(part);
done:
    free(mask);
    free(dir);
    free(part);
    return cause;
}

/* The game's files a unit points to must be this game's bytes. 1 when they are. */
int sh_cache_pointer_checked(const char *source)
{
    /* The product's container (modprog.h) holds files the installed product derives from DOOM's and the mod's own; a
     * unit names them by the bytes the publishing product derived, which this product derives again where they serve,
     * so they are not checked against DOOM's files. */
    return !(source && (!strcmp(source, SH_MODPROG_CONTAINER) || !strcmp(source, "product")));
}

/* The unit's pointers into the product's own container name bytes this product derives (modprog.h); a product
 * that derives other bytes, or none, does not have the unit's content (a different product version). 1 ok. */
static int mc_product_ok(const mc_unit *u)
{
    mpj *m = mpj_parse(u->bytes, u->len);
    const mpj *game = m ? mpj_get(m, "game") : NULL;
    int i, ok = 1;
    for (i = 0; ok && game && game->type == MPJ_ARR && i < game->n; i++) {
        const mpj *p = mpj_get(game->items[i], "path"), *s = mpj_get(game->items[i], "sha256"), *src = mpj_get(game->items[i], "source");
        unsigned char *bytes = NULL;
        size_t len = 0;
        char have[SH_MB_HASH_HEX + 1];
        if (!p || p->type != MPJ_STR || !s || s->type != MPJ_STR || !src || src->type != MPJ_STR) continue;
        if (sh_cache_pointer_checked(src->text)) continue;   /* DOOM's own files: mc_game_ok */
        /* A file the product names by its bytes' hash (a look's image) exists here only once the unit's own material
         * has compiled, after this check: this product's compiler answers for it and serves the name its own compile
         * spells (modserve.h sh_modserve_set_product_reader), so a unit pointing at one is not refused for it. */
        if (sh_mb_product_hashed(p->text) && !sh_modprog_owns(p->text)) continue;
        if (!sh_modprog_owns(p->text) ||sh_modprog_read(p->text, &bytes, &len) != SH_MB_STOCK_OK || !bytes) {
            mc_logf("MODS: content: a unit names %s of Snapmap+'s own files, which this Snapmap+ does not make", p->text);
            ok = 0;
            continue;
        }
        sh_mb_sha256_hex(bytes, len, have);
        HeapFree(GetProcessHeap(), 0, bytes);
        if (strcmp(have, s->text)) {
            mc_logf("MODS: content: a unit names other bytes of Snapmap+'s own %s than this Snapmap+ makes", p->text);
            ok = 0;
        }
    }
    mpj_free(m);
    return ok;
}

static int mc_game_ok(const mc_unit *u)
{
    mpj *m = mpj_parse(u->bytes, u->len);
    const mpj *game = m ? mpj_get(m, "game") : NULL;
    int i, ok = 1;
    for (i = 0; ok && game && game->type == MPJ_ARR && i < game->n; i++) {
        const mpj *p = mpj_get(game->items[i], "path"), *s = mpj_get(game->items[i], "sha256"), *src = mpj_get(game->items[i], "source");
        unsigned long long size = 0;
        if (!p || p->type != MPJ_STR || !s || s->type != MPJ_STR) continue;
        if (src && src->type == MPJ_STR && !sh_cache_pointer_checked(src->text)) continue;   /* the product's own */
        {
            char *cause = NULL;
            int v = sh_mb_stock_verify(p->text, s->text, &size, p->text, &cause);
            free(cause);
            if (v == SH_MB_STOCK_ABSENT || v == SH_MB_STOCK_MISMATCH) ok = 0;   /* unreadable: the composition says */
        }
    }
    mpj_free(m);
    return ok;
}

/* The held units of a row fetched on up to four threads: each takes the next unit, fetches its archive (and
 * alone any file the archive lacked) and records its cause. Progress is the sum of every transfer, reported to the
 * caller's callback under one lock. */
#define MC_FETCH_AT_ONCE 4
typedef struct mc_fetch_pool {
    const char (*units)[SH_MB_HASH_HEX + 1];
    const size_t *todo;
    size_t ntodo;
    volatile LONG next;
    int *causes;
    const modnet_io *outer;
    CRITICAL_SECTION lock;
    unsigned long long done[MC_FETCH_AT_ONCE * 4], total[MC_FETCH_AT_ONCE * 4];   /* per unit slot, mod the table */
    volatile LONG cancelled;
} mc_fetch_pool;

typedef struct mc_fetch_slot { mc_fetch_pool *pool; size_t slot; } mc_fetch_slot;

static void mc_fetch_progress(void *context, unsigned long long done, unsigned long long total)
{
    mc_fetch_slot *s = (mc_fetch_slot *)context;
    mc_fetch_pool *p = s->pool;
    unsigned long long sum = 0, all = 0;
    size_t k;
    EnterCriticalSection(&p->lock);
    p->done[s->slot] = done;
    if (total > p->total[s->slot]) p->total[s->slot] = total;
    for (k = 0; k < sizeof p->done / sizeof p->done[0]; k++) { sum += p->done[k]; all += p->total[k]; }
    if (p->outer && p->outer->progress) p->outer->progress(p->outer->context, sum, all);
    LeaveCriticalSection(&p->lock);
}

static DWORD WINAPI mc_fetch_worker(LPVOID context)
{
    mc_fetch_pool *p = (mc_fetch_pool *)context;
    for (;;) {
        LONG n = InterlockedIncrement(&p->next) - 1;
        size_t i, f;
        mc_unit u;
        mc_fetch_slot slot;
        modnet_io io;
        int c;
        if (n < 0 || (size_t)n >= p->ntodo || InterlockedCompareExchange(&p->cancelled, 0, 0)) break;
        i = p->todo[n];
        slot.pool = p;
        slot.slot = (size_t)n % (sizeof p->done / sizeof p->done[0]);
        memset(&io, 0, sizeof io);
        if (p->outer) io.cancel = p->outer->cancel;
        io.progress = mc_fetch_progress;
        io.context = &slot;
        mc_unit_load(&u, p->units[i]);
        /* Its manifest's checks first when it is here; else the archive brings it and they follow. */
        if (u.state == -2) { p->causes[i] = SH_CACHE_ABSENT_NEWER; mc_unit_free(&u); continue; }
        if (u.state >= 0 && !mc_product_ok(&u)) { p->causes[i] = SH_CACHE_ABSENT_NEWER; mc_unit_free(&u); continue; }
        if (u.state >= 0 && !mc_game_ok(&u)) { p->causes[i] = SH_CACHE_ABSENT_GAME; mc_unit_free(&u); continue; }
        if (u.state != 1) {
            c = mc_fetch_archive(&u, p->units[i], &io);
            if (c < 0) { InterlockedExchange(&p->cancelled, 1); mc_unit_free(&u); break; }
            if (c) p->causes[i] = c;
            else if (u.state == -2) p->causes[i] = SH_CACHE_ABSENT_NEWER;
            else if (!mc_product_ok(&u)) p->causes[i] = SH_CACHE_ABSENT_NEWER;
            else if (!mc_game_ok(&u)) p->causes[i] = SH_CACHE_ABSENT_GAME;
            /* An entry the archive lacked or that did not check: fetched alone (a repair, a few files), one at a time
             * across the threads, since two units may lack the same file. */
            for (f = 0; !p->causes[i] && u.state >= 0 && f < u.nfiles; f++) {
                char *src = mc_source_of(u.files[f].sha, u.files[f].size);
                if (src) { free(src); continue; }
                AcquireSRWLockExclusive(&g_blob_fetch);
                src = mc_source_of(u.files[f].sha, u.files[f].size);
                c = src ? 0 : mc_fetch_blob(u.files[f].sha, u.files[f].size, &io);
                ReleaseSRWLockExclusive(&g_blob_fetch);
                free(src);
                if (c < 0) { InterlockedExchange(&p->cancelled, 1); break; }
                if (c) p->causes[i] = c;
            }
        }
        mc_unit_free(&u);
        if (InterlockedCompareExchange(&p->cancelled, 0, 0)) break;
    }
    return 0;
}

int sh_cache_prepare(const char *chash, const modnet_io *io)
{
    mc_row snapshot;
    const char **want = NULL;
    int *states = NULL, *causes = NULL;
    size_t i, nwant = 0;
    modnet_result r;
    int code = MODNET_OK, ok;
    char *why = NULL;

    memset(&snapshot, 0, sizeof snapshot);
    AcquireSRWLockShared(&g_lock);
    {
        const mc_row *row = mc_row_find_locked(chash);
        if (row) {
            snapshot.n = row->n;
            snapshot.units = row->n ? calloc(row->n, sizeof *snapshot.units) : NULL;
            for (i = 0; snapshot.units && i < row->n; i++) strcpy_s(snapshot.units[i], sizeof snapshot.units[i], row->units[i]);
            strcpy_s(snapshot.chash, sizeof snapshot.chash, row->chash);
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (!snapshot.chash[0]) return -1;
    causes = snapshot.n ? (int *)calloc(snapshot.n, sizeof *causes) : NULL;
    want = snapshot.n ? (const char **)calloc(snapshot.n, sizeof *want) : NULL;
    states = snapshot.n ? (int *)calloc(snapshot.n, sizeof *states) : NULL;
    if (snapshot.n && (!causes || !want || !states || !snapshot.units)) { code = MODNET_E_NOMEM; goto done; }
    for (i = 0; i < snapshot.n; i++)
        if (sh_cache_unit_state(snapshot.units[i], NULL, NULL, NULL) != 1) want[nwant++] = snapshot.units[i];
    if (nwant) {
        /* The store's state of each absent unit (held, missing, removed); then the held ones fetched, four at a time. */
        int st = modnet_unit_status(want, nwant, states, io, &r);
        size_t w = 0, *todo = (size_t *)calloc(snapshot.n ? snapshot.n : 1, sizeof *todo), ntodo = 0;
        if (!todo) { code = MODNET_E_NOMEM; goto done; }
        for (i = 0; i < snapshot.n; i++) {
            if (w < nwant && want[w] == snapshot.units[i]) {
                int state = states[w++];
                if (st != MODNET_OK) {
                    causes[i] = st == MODNET_E_CANCELLED ? SH_CACHE_ABSENT_UNREACHABLE : mc_net_cause(st, &r);
                    mc_logf("MODS: content: the store's state of unit %.16s was not read (%s; HTTP %d)", snapshot.units[i],
                            r.message ? r.message : modnet_code_text(st), r.status);
                    continue;
                }
                if (state == MODNET_UNIT_REMOVED) { causes[i] = SH_CACHE_ABSENT_REMOVED; continue; }
                if (state == MODNET_UNIT_MISSING) {
                    /* Not in the store: what is here may still compose it (a local mod's files and the manifest). */
                    causes[i] = SH_CACHE_ABSENT_NOT_IN_STORE;
                    continue;
                }
                todo[ntodo++] = i;
            }
        }
        if (ntodo) {
            mc_fetch_pool pool;
            HANDLE threads[MC_FETCH_AT_ONCE];
            DWORD started = 0, t;
            memset(&pool, 0, sizeof pool);
            pool.units = (const char (*)[SH_MB_HASH_HEX + 1])snapshot.units;
            pool.todo = todo;
            pool.ntodo = ntodo;
            pool.causes = causes;
            pool.outer = io;
            InitializeCriticalSection(&pool.lock);
            for (t = 0; t < MC_FETCH_AT_ONCE && t < ntodo; t++) {
                threads[started] = CreateThread(NULL, 0, mc_fetch_worker, &pool, 0, NULL);
                if (threads[started]) started++;
            }
            if (!started) (void)mc_fetch_worker(&pool);   /* no thread could start: this one fetches them */
            else WaitForMultipleObjects(started, threads, TRUE, INFINITE);
            for (t = 0; t < started; t++) CloseHandle(threads[t]);
            DeleteCriticalSection(&pool.lock);
            if (pool.cancelled) { free(todo); code = MODNET_E_CANCELLED; goto done; }
        }
        free(todo);
    }
    ok = mc_compose(chash, causes, SH_CACHE_ABSENT_UNREACHABLE, &why, NULL);
    if (!ok) { mc_logf("MODS: content: the map's row could not be composed: %s", why ? why : "?"); code = -1; }
done:
    free(why);
    free(causes);
    free((void *)want);
    free(states);
    free(snapshot.units);
    return code;
}

/* The manifest's content step (modmanifest.h sh_modmanifest_set_content): note the rows; compose each pinned row
 * whose composition is not on disk, or is a stand-in composition whose units are all here now. */
void sh_cache_on_load(const sh_modmanifest_list *rows, char *const *placed, size_t count)
{
    size_t i, made = 0;
    if (!rows) return;
    sh_cache_notice_drop();   /* only this load's rows say what it plays without */
    sh_cache_local_changed(); /* the local mods' files as they are now (a Refresh may have rebuilt them) */
    sh_cache_note_rows(rows, placed, count);
    for (i = 0; i < rows->count; i++) {
        char *why = NULL;
        int built = 0;
        if (!rows->rows[i].pinned) continue;
        /* Cheap when nothing changed; a stand-in composition whose units are all here now is composed in full. */
        if (mc_compose(rows->rows[i].build, NULL, SH_CACHE_ABSENT_OFFLINE, &why, &built) && built) made++;
        free(why);
    }
    if (made && g_rescan) (void)g_rescan();
}

void sh_cache_set_map_folders(const char *saved, const char *published)
{
    AcquireSRWLockExclusive(&g_lock);
    free(g_saved_dir);
    free(g_published_dir);
    g_saved_dir = saved ? sh_text(saved) : NULL;
    g_published_dir = published ? sh_text(published) : NULL;
    ReleaseSRWLockExclusive(&g_lock);
}

static char *mc_game_maps(const char *which)
{
    char local[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", local, (DWORD)sizeof local);
    if (!n || n >= sizeof local) return NULL;
    return sh_textf("%s\\id Software\\DOOM\\base\\doomsnapmaps\\%s", local, which);
}

/* A growing list of hashes. */
typedef struct mc_set { char (*v)[SH_CACHE_HASH_HEX + 1]; size_t n, cap; } mc_set;
static int mc_set_has(const mc_set *s, const char *h)
{
    size_t i;
    for (i = 0; i < s->n; i++) if (!strcmp(s->v[i], h)) return 1;
    return 0;
}
static void mc_set_add(mc_set *s, const char *h)
{
    if (!mc_hex64(h) || mc_set_has(s, h)) return;
    if (s->n == s->cap) {
        size_t next = s->cap ? s->cap * 2 : 64;
        void *g = realloc(s->v, next * sizeof *s->v);
        if (!g) return;
        s->v = g;
        s->cap = next;
    }
    strcpy_s(s->v[s->n++], SH_CACHE_HASH_HEX + 1, h);
}

/* What the kept maps use: every unit and composition their pinned rows name. A kept map whose text is
 * there and does not read (a file that cannot be opened, a slot that does not decode, a row that does not parse) is
 * counted in `unreadable`: what it uses is unknown, so the sweep removes nothing while there is one. */
typedef struct mc_kept { mc_set units, comps; size_t maps, unreadable; } mc_kept;

static void mc_kept_text(mc_kept *k, const char *text, size_t len)
{
    size_t i, u, pinned = 0;
    unsigned rejected = 0;
    sh_modmanifest_list rows;
    memset(&rows, 0, sizeof rows);
    {
        sh_carrier_view member;
        if (!sh_carrier_view_read(text, len, &member)) { k->unreadable++; return; }
        if (member.format == 1) return;   /* Earlier maps held only whole bundles. */
        if (member.state == SH_CARRIER_MALFORMED) k->unreadable++;
        if (member.state == SH_CARRIER_PRESENT && member.mods.present)
            sh_modmanifest_read(text, len, &rows, &rejected);
    }
    /* A row of an earlier shape names no unit and is not counted; only a row that does not parse leaves what the map
     * uses unknown. */
    if (rejected > rows.earlier) k->unreadable++;
    for (i = 0; i < rows.count; i++) {
        if (!rows.rows[i].pinned) continue;
        pinned++;
        mc_set_add(&k->comps, rows.rows[i].build);
        for (u = 0; u < rows.rows[i].unit_count; u++) mc_set_add(&k->units, rows.rows[i].units[u]);
    }
    if (pinned) k->maps++;
    sh_modmanifest_list_free(&rows);
}

/* The files of `dir` matching `pattern` (leaf names, malloc'd array of malloc'd). *failed (optional) is set when the
 * folder is there and cannot be listed. */
static char **mc_list_ex(const char *dir, const char *pattern, int dirs, size_t *count, int *failed)
{
    char *pat = sh_textf("%s\\%s", dir, pattern), **out = NULL;
    wchar_t *wp = pat ? sh_mb_wpath(pat) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h = wp ? FindFirstFileW(wp, &fd) : INVALID_HANDLE_VALUE;
    size_t n = 0;
    if (h == INVALID_HANDLE_VALUE && failed) {
        DWORD e = wp ? GetLastError() : ERROR_OUTOFMEMORY;
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND && e != ERROR_NO_MORE_FILES) *failed = 1;
    }
    while (h != INVALID_HANDLE_VALUE) {
        int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (fd.cFileName[0] != L'.' && isdir == dirs) {
            char **g = (char **)realloc(out, (n + 1) * sizeof *g);
            if (g) { out = g; if ((out[n] = sh_mb_utf8(fd.cFileName)) != NULL) n++; }
            else if (failed) *failed = 1;
        }
        if (!FindNextFileW(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
    }
    free(wp);
    free(pat);
    *count = n;
    return out;
}

static char **mc_list(const char *dir, const char *pattern, int dirs, size_t *count)
{
    return mc_list_ex(dir, pattern, dirs, count, NULL);
}

static void mc_list_free(char **v, size_t n) { size_t i; for (i = 0; i < n; i++) free(v[i]); free(v); }

/* Every map text (*.decl) of one of the game's map folders. */
static void mc_kept_dir(mc_kept *k, const char *dir)
{
    char **names;
    size_t n, i;
    int failed = 0;
    if (!dir) return;
    names = mc_list_ex(dir, "*.decl", 0, &n, &failed);
    if (failed) k->unreadable++;
    for (i = 0; names && i < n; i++) {
        size_t len = 0;
        char *path;
        unsigned char *text;
        path = sh_textf("%s\\%s", dir, names[i]);
        text = path ? sh_mb_read_file(path, (size_t)1 << 31, &len) : NULL;
        if (text) mc_kept_text(k, (const char *)text, len);
        else k->unreadable++;
        free(text);
        free(path);
    }
    mc_list_free(names, names ? n : 0);
}

static void mc_kept_slot(void *context, const char *local_id, const char *item_id, const char *text, size_t len)
{
    (void)local_id;
    (void)item_id;
    mc_kept_text((mc_kept *)context, text, len);
}

/* The kept maps: the player's local saves (My Maps slots, the only copy of a map saved offline), the cloud
 * copies of saved maps, and, unless `drop_played`, the published maps the game keeps. */
static void mc_kept_read(mc_kept *k, int drop_played)
{
    char *saved, *published;
    size_t bad = 0;
    AcquireSRWLockShared(&g_lock);
    saved = g_saved_dir ? sh_text(g_saved_dir) : mc_game_maps("saved-maps");
    published = g_published_dir ? sh_text(g_published_dir) : mc_game_maps("published-maps");
    ReleaseSRWLockShared(&g_lock);
    if (sh_saved_maps_each(mc_kept_slot, k, &bad) < 0) k->unreadable++;
    k->unreadable += bad;
    mc_kept_dir(k, saved);
    if (!drop_played) mc_kept_dir(k, published);
    free(saved);
    free(published);
}

#define MC_PARTIAL_IDLE_S 3600ull   /* a partial download nobody wrote for this long is not running */

int sh_cache_sweep(int drop_played, const char *const *held, size_t held_count, sh_cache_sweep_stats *out)
{
    mc_kept kept;
    mc_set blobs = { 0 };
    char *udir = mc_path("units"), *sdir = mc_path("served"), *fdir = mc_path("files"), *pdir = mc_path("partial");
    char **names;
    size_t n, i;
    sh_cache_sweep_stats st;
    sh_modmanifest_list cur;
    memset(&st, 0, sizeof st);
    memset(&kept, 0, sizeof kept);
    /* The references. */
    mc_kept_read(&kept, drop_played);
    memset(&cur, 0, sizeof cur);
    if (sh_modmanifest_current(&cur)) {
        for (i = 0; i < cur.count; i++) {
            size_t k;
            if (!cur.rows[i].pinned) {
                /* The open map's local row may be served by its stand-ins (a row of no units). */
                char none[SH_CACHE_HASH_HEX + 1];
                if (strcmp(cur.rows[i].salt, SH_MODMANIFEST_PRODUCT) && sh_modmanifest_composition_hash(cur.rows[i].salt, NULL, 0, none))
                    mc_set_add(&kept.comps, none);
                continue;
            }
            mc_set_add(&kept.comps, cur.rows[i].build);
            for (k = 0; k < cur.rows[i].unit_count; k++) mc_set_add(&kept.units, cur.rows[i].units[k]);
        }
    }
    sh_modmanifest_list_free(&cur);
    for (i = 0; held && i < held_count; i++) mc_set_add(&kept.units, held[i]);
    st.maps = kept.maps;
    if (kept.unreadable) {
        /* A kept map that does not read may use any unit here: nothing is removed until it reads. */
        mc_logf("MODS: content: the sweep removed nothing: %zu kept map(s) or map folder(s) could not be read, so what "
                "they use is not known", kept.unreadable);
        st.unreadable = kept.unreadable;
        goto done;
    }

    AcquireSRWLockExclusive(&g_disk);
    /* Manifests no reference names; the blobs every remaining manifest names. */
    names = udir ? mc_list(udir, "*.json", 0, &n) : NULL;
    for (i = 0; names && i < n; i++) {
        char hash[SH_CACHE_HASH_HEX + 1], *path = sh_textf("%s\\%s", udir, names[i]);
        size_t len = strlen(names[i]);
        if (!path) continue;
        if (len != 69 || strcmp(names[i] + 64, ".json")) { free(path); continue; }
        memcpy(hash, names[i], 64);
        hash[64] = 0;
        if (!mc_set_has(&kept.units, hash)) { if (mc_delete(path)) st.units_removed++; free(path); continue; }
        st.units_kept++;
        {
            mc_unit u;
            size_t f;
            memset(&u, 0, sizeof u);
            if ((u.bytes = sh_cache_manifest(hash, &u.len)) != NULL && mc_unit_parse(&u))
                for (f = 0; f < u.nfiles; f++) mc_set_add(&blobs, u.files[f].sha);
            mc_unit_free(&u);
        }
        free(path);
    }
    mc_list_free(names, names ? n : 0);
    /* Blobs no remaining manifest names. */
    names = fdir ? mc_list(fdir, "*", 1, &n) : NULL;
    for (i = 0; names && i < n; i++) {
        char *sub = sh_textf("%s\\%s", fdir, names[i]), **leaves;
        size_t m, j;
        leaves = sub ? mc_list(sub, "*", 0, &m) : NULL;
        for (j = 0; leaves && j < m; j++) {
            char *path = sh_textf("%s\\%s", sub, leaves[j]);
            unsigned long long size = 0;
            if (path && mc_hex64(leaves[j]) && !mc_set_has(&blobs, leaves[j])) {
                (void)mc_exists(path, &size);
                if (mc_delete(path)) { st.blobs_removed++; st.bytes_removed += size; }
            } else if (path && mc_exists(path, &size)) st.bytes_kept += size;
            free(path);
        }
        mc_list_free(leaves, leaves ? m : 0);
        free(sub);
    }
    mc_list_free(names, names ? n : 0);
    /* Compositions no kept map's row uses, and what a crash left of a composition being made or replaced. */
    names = sdir ? mc_list(sdir, "*", 1, &n) : NULL;
    for (i = 0; names && i < n; i++) {
        char hash[SH_CACHE_HASH_HEX + 1], *path = sh_textf("%s\\%s", sdir, names[i]);
        int stale = strlen(names[i]) != 64;
        if (!stale) { memcpy(hash, names[i], 65); stale = !mc_hex64(hash) || !mc_set_has(&kept.comps, hash); }
        /* A composition being made or replaced right now (built outside this lock) is left alone; only one nothing
         * has written for an hour is what a crash left. */
        else stale = path && mc_untouched_for(path, MC_PARTIAL_IDLE_S);
        if (path && stale && mc_rmtree(path)) {
            char *side = strlen(names[i]) == 64 ? sh_textf("%s.json", path) : NULL;
            if (side) mc_delete(side);
            free(side);
            st.served_removed++;
        }
        free(path);
    }
    mc_list_free(names, names ? n : 0);
    /* A download cut short leaves its partial file, which the next download of the same file resumes. Once no kept map
     * names its unit or file and nothing has written it for an hour (a running download writes all the time), it goes
     * too. */
    names = pdir ? mc_list(pdir, "*.part", 0, &n) : NULL;
    for (i = 0; names && i < n; i++) {
        char key[SH_CACHE_HASH_HEX + 1], *path;
        if (strlen(names[i]) < 64) continue;
        memcpy(key, names[i], 64);
        key[64] = 0;
        if (!mc_hex64(key) || mc_set_has(&kept.units, key) || mc_set_has(&blobs, key)) continue;
        path = sh_textf("%s\\%s", pdir, names[i]);
        if (path && mc_untouched_for(path, MC_PARTIAL_IDLE_S) && mc_delete(path)) st.partials_removed++;
        free(path);
    }
    mc_list_free(names, names ? n : 0);
    ReleaseSRWLockExclusive(&g_disk);
    mc_logf("MODS: content: the sweep kept %zu unit(s) of %zu map(s) and removed %zu unit(s), %zu file(s) (%llu bytes), "
            "%zu composition(s) and %zu partial download(s)%s", st.units_kept, st.maps, st.units_removed, st.blobs_removed,
            st.bytes_removed, st.served_removed, st.partials_removed,
            drop_played ? "; content of maps only played was removed" : "");
done:
    if (out) *out = st;
    free(kept.units.v);
    free(kept.comps.v);
    free(blobs.v);
    free(udir);
    free(sdir);
    free(fdir);
    free(pdir);
    return 1;
}

static void mc_size_text(char *out, size_t cap, unsigned long long bytes)
{
    double v = (double)bytes;
    if (v >= 1024.0 * 1024.0 * 1024.0) _snprintf_s(out, cap, _TRUNCATE, "%.1f GB", v / (1024.0 * 1024.0 * 1024.0));
    else if (v >= 1024.0 * 1024.0) _snprintf_s(out, cap, _TRUNCATE, "%.1f MB", v / (1024.0 * 1024.0));
    else _snprintf_s(out, cap, _TRUNCATE, "%.0f KB", v / 1024.0);
}

/* Each kept map's rows: one line per row, and the totals. A local save's cloud copy in saved-maps (its leaf begins
 * with the slot's cloud item id) is the same map and is not reported again. */
typedef struct mc_report {
    sh_cache_line_fn fn; void *ctx; mc_set files; unsigned long long total; size_t maps, lines;
    char **items; size_t nitems;      /* the local saves' cloud item ids */
    const char *want;                 /* sh_cache_stage_copy: the composition hash looked for */
    sh_modmanifest_list found;        /* its row, once found */
} mc_report;

static void mc_report_text(mc_report *rep, const char *text, size_t len, const char *kind)
{
    size_t i, k, f;
    sh_modmanifest_list rows;
    int counted = 0;
    memset(&rows, 0, sizeof rows);
    if (sh_carrier_part(text, len, "mods", NULL, NULL) == 1) sh_modmanifest_read(text, len, &rows, NULL);
    for (i = 0; i < rows.count; i++) {
        const sh_modmanifest_row *row = &rows.rows[i];
        unsigned long long bytes = 0, shared = 0;
        size_t objects = 0;
        char size[32], share[32], *line;
        if (!row->pinned) continue;
        if (rep->want) {
            if (!rep->found.count && !_stricmp(row->build, rep->want)) (void)sh_modmanifest_list_add(&rep->found, row);
            continue;
        }
        if (!counted) { counted = 1; rep->maps++; }
        for (k = 0; k < row->unit_count; k++) {
            mc_unit u;
            mc_unit_load(&u, row->units[k]);
            if (strcmp(row->roots[k], "*")) objects++;
            for (f = 0; f < u.nfiles; f++) {
                bytes += u.files[f].size;
                if (mc_set_has(&rep->files, u.files[f].sha)) shared += u.files[f].size;
                else { mc_set_add(&rep->files, u.files[f].sha); rep->total += u.files[f].size; }
            }
            mc_unit_free(&u);
        }
        mc_size_text(size, sizeof size, bytes);
        mc_size_text(share, sizeof share, shared);
        line = shared ? sh_textf("A map you %s: %s, %u object%s, %s (%s of it shared with other maps).", kind, sh_modmanifest_row_name(row)[0] ? row->name : "a mod",
                                 (unsigned)objects, objects == 1 ? "" : "s", size, share)
                      : sh_textf("A map you %s: %s, %u object%s, %s.", kind, sh_modmanifest_row_name(row)[0] ? row->name : "a mod",
                                 (unsigned)objects, objects == 1 ? "" : "s", size);
        if (line && rep->fn) { rep->fn(rep->ctx, line, row->build); rep->lines++; }
        free(line);
    }
    sh_modmanifest_list_free(&rows);
}

static void mc_report_slot(void *context, const char *local_id, const char *item_id, const char *text, size_t len)
{
    mc_report *rep = (mc_report *)context;
    (void)local_id;
    if (item_id && item_id[0]) {
        char **g = (char **)realloc(rep->items, (rep->nitems + 1) * sizeof *g);
        if (g) { rep->items = g; if ((g[rep->nitems] = sh_text(item_id)) != NULL) rep->nitems++; }
    }
    mc_report_text(rep, text, len, "kept");
}

static int mc_report_copy(const mc_report *rep, const char *leaf)
{
    size_t i, n;
    for (i = 0; i < rep->nitems; i++) {
        n = strlen(rep->items[i]);
        if (!_strnicmp(leaf, rep->items[i], n) && leaf[n] == '_') return 1;
    }
    return 0;
}

static void mc_report_dir(mc_report *rep, const char *dir, const char *kind, int copies)
{
    char **names;
    size_t n, i;
    if (!dir) return;
    names = mc_list(dir, "*.decl", 0, &n);
    for (i = 0; names && i < n; i++) {
        size_t len = 0;
        char *path;
        unsigned char *text;
        if (copies && mc_report_copy(rep, names[i])) continue;
        path = sh_textf("%s\\%s", dir, names[i]);
        text = path ? sh_mb_read_file(path, (size_t)1 << 31, &len) : NULL;
        if (text) mc_report_text(rep, (const char *)text, len, kind);
        free(text);
        free(path);
    }
    mc_list_free(names, names ? n : 0);
}

static void mc_report_run(mc_report *rep)
{
    char *saved, *published;
    size_t i;
    AcquireSRWLockShared(&g_lock);
    saved = g_saved_dir ? sh_text(g_saved_dir) : mc_game_maps("saved-maps");
    published = g_published_dir ? sh_text(g_published_dir) : mc_game_maps("published-maps");
    ReleaseSRWLockShared(&g_lock);
    (void)sh_saved_maps_each(mc_report_slot, rep, NULL);
    mc_report_dir(rep, saved, "kept", 1);
    mc_report_dir(rep, published, "played", 0);
    for (i = 0; i < rep->nitems; i++) free(rep->items[i]);
    free(rep->items);
    rep->items = NULL;
    rep->nitems = 0;
    free(saved);
    free(published);
}

char *sh_cache_storage_line(void)
{
    mc_report rep;
    char size[32], *line;
    memset(&rep, 0, sizeof rep);
    mc_report_run(&rep);
    mc_size_text(size, sizeof size, rep.total);
    line = sh_textf("Map content: %s for %u map%s.", size, (unsigned)rep.maps, rep.maps == 1 ? "" : "s");
    free(rep.files.v);
    return line;
}

int sh_cache_report(sh_cache_line_fn fn, void *context)
{
    mc_report rep;
    char size[32], *line;
    memset(&rep, 0, sizeof rep);
    rep.fn = fn;
    rep.ctx = context;
    mc_report_run(&rep);
    mc_size_text(size, sizeof size, rep.total);
    line = sh_textf("Map content: %s for %u map%s.", size, (unsigned)rep.maps, rep.maps == 1 ? "" : "s");
    if (line && fn) { fn(context, line, NULL); rep.lines++; }
    free(line);
    free(rep.files.v);
    return (int)rep.lines;
}

void sh_cache_copy_free(sh_cache_copy *c)
{
    if (!c) return;
    if (c->dir && mc_dir_exists(c->dir)) (void)mc_rmtree(c->dir);
    free(c->dir);
    free(c->modjson);
    free(c->name);
    memset(c, 0, sizeof *c);
}

int sh_cache_stage_copy(const char *chash, sh_cache_copy *out, char **why)
{
    mc_report rep;
    const sh_modmanifest_row *row;
    mc_unit *units = NULL;
    const char **manifests = NULL;
    size_t *lens = NULL, i, n;
    sh_mb_bundle built;
    sh_mb_resolver resolver;
    mc_src src;
    char *reason = NULL, *text = NULL;
    const char *title;
    int ok = 0;
    if (why) *why = NULL;
    if (out) memset(out, 0, sizeof *out);
    memset(&built, 0, sizeof built);
    if (!out || !mc_hex64(chash)) { if (why) *why = sh_text("This map's mod could not be found on this computer."); return 0; }
    memset(&rep, 0, sizeof rep);
    rep.want = chash;
    mc_report_run(&rep);
    free(rep.files.v);
    if (!rep.found.count) {
        if (why) *why = sh_text("No map on this computer uses this mod any more; press Refresh.");
        sh_modmanifest_list_free(&rep.found);
        return 0;
    }
    row = &rep.found.rows[0];
    title = sh_modmanifest_row_name(row)[0] ? row->name : "This mod";
    n = row->unit_count;
    units = n ? (mc_unit *)calloc(n, sizeof *units) : NULL;
    manifests = n ? (const char **)calloc(n, sizeof *manifests) : NULL;
    lens = n ? (size_t *)calloc(n, sizeof *lens) : NULL;
    if (!n || !units || !manifests || !lens) { reason = sh_textf("%s could not be copied: Snapmap+ ran out of memory.", title); goto done; }
    for (i = 0; i < n; i++) {
        mc_unit_load(&units[i], row->units[i]);
        if (units[i].state != 1) {
            reason = sh_textf("%s cannot be copied: some of its content is not on this computer. Play the map once online, "
                              "then try again.", title);
            goto done;
        }
        /* A unit whose binaries spell its namespace copies too; the copy's binaries keep spelling it, its build records
         * "spelled", and serving maps that namespace to the copy's salt (modserve.c). */
        manifests[i] = units[i].bytes;
        lens[i] = units[i].len;
        if (strcmp(row->roots[i], SH_UNIT_WHOLE)) out->objects++;
    }
    strcpy_s(out->salt, sizeof out->salt, row->salt);
    out->name = sh_modmanifest_row_name(row)[0] ? sh_textf("%s (my copy)", row->name) : sh_text("My copy of a mod");
    out->dir = mc_path("partial\\copy-%lu-%llu", GetCurrentThreadId(), (unsigned long long)GetTickCount64());
    if (!out->name || !out->dir) { reason = sh_textf("%s could not be copied: Snapmap+ ran out of memory.", title); goto done; }
    {
        char *partial = mc_path("partial");
        int made = partial && mc_mkdirs(partial);
        free(partial);
        if (!made) { reason = sh_textf("%s could not be copied: the content folder could not be written.", title); goto done; }
    }
    memset(&resolver, 0, sizeof resolver);
    if (g_resolver_set) resolver = g_resolver;
    src.units = units;
    src.n = n;
    /* Copies (link 0): the folder becomes the player's own, whose files they may edit; a hard link would edit the
     * cache's bytes. */
    if (!sh_unit_compose_ex(manifests, lens, n, mc_compose_source, &src, NULL, 0, 0, row->salt, out->name, &resolver,
                            out->dir, &built, &reason)) {
        char *said = sh_textf("%s could not be copied: %s.", title, reason ? reason : "its files did not check");
        free(reason);
        reason = said;
        goto done;
    }
    {
        char *mj = sh_textf("%s\\mod.json", out->dir);
        size_t len = 0;
        unsigned char *bytes = mj ? sh_mb_read_file(mj, (size_t)1 << 24, &len) : NULL;
        int had = 0;
        text = bytes ? sh_mb_author_bytes((const char *)bytes, len, &out->modjson_len, &had) : NULL;
        free(bytes);
        free(mj);
        if (!text) { reason = sh_textf("%s could not be copied: its mod.json could not be read back.", title); goto done; }
        out->modjson = text;
        text = NULL;
    }
    ok = 1;
done:
    sh_mb_free(&built);
    for (i = 0; units && i < n; i++) mc_unit_free(&units[i]);
    free(units);
    free((void *)manifests);
    free(lens);
    free(text);
    sh_modmanifest_list_free(&rep.found);
    if (!ok) {
        sh_cache_copy_free(out);
        if (why) *why = reason;
        else free(reason);
    } else free(reason);
    return ok;
}

/* The units this computer's published maps claim (modclaim.h modclaim_held), kept by every sweep: a claim's
 * units serve its self-repair after its map left this computer. Bound at install. */
static sh_cache_held_fn g_held;
void sh_cache_set_held(sh_cache_held_fn fn) { g_held = fn; }

int sh_cache_sweep_kept(int drop_played, sh_cache_sweep_stats *out)
{
    char (*held)[SH_CACHE_HASH_HEX + 1] = NULL;
    const char **list = NULL;
    size_t n = g_held ? g_held(&held) : 0, i;
    int r;
    if (n && (list = (const char **)calloc(n, sizeof *list)) != NULL)
        for (i = 0; i < n; i++) list[i] = held[i];
    r = sh_cache_sweep(drop_played, list, list ? n : 0, out);
    free((void *)list);
    free(held);
    return r;
}

/* The sweep at start and after a map is deleted or a mod uninstalled, off the calling thread. */
static DWORD WINAPI mc_sweep_thread(LPVOID unused)
{
    (void)unused;
    (void)sh_cache_sweep_kept(0, NULL);
    return 0;
}

void sh_cache_sweep_async(void)
{
    HANDLE h = CreateThread(NULL, 0, mc_sweep_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
/* Read a whole file (UTF-8 path) into memory; NULL when it cannot be read. */
static unsigned char *mc_read_all(const char *path, unsigned long long size, size_t *len)
{
    return sh_mb_read_file(path, (size_t)size + 1, len);
}

int sh_cache_publish_unit(const char *hash, const modnet_signer *signer, const modnet_io *io, sh_cache_upload_stats *stats,
                          char **why)
{
    mc_unit u;
    char (*missing)[MODNET_HASH_HEX + 1] = NULL;
    size_t nmissing = 0, i, f;
    int held = 0, code;
    modnet_result r;
    sh_cache_upload_stats st;
    char *dir = mc_path("partial");
    memset(&st, 0, sizeof st);
    if (why) *why = NULL;
    mc_unit_load(&u, hash);
    if (u.state == -1 || u.state == -2) { mc_unit_free(&u); free(dir); if (why) *why = sh_text("the unit's manifest is not here"); return MODNET_E_ARG; }
    code = modnet_begin_unit(hash, u.bytes, u.len, signer, &held, &missing, &nmissing, io, &r);
    st.requests += r.requests;
    if (code != MODNET_OK) goto done;
    if (held) { st.held = 1; goto done; }
    /* The blobs the store lacks: those larger than one request in parts, the rest in archives of at most one request,
     * each with unit.json. Every file is read from here (the cache, or a local mod's file of that SHA-256). */
    {
        sh_zip_writer w;
        char *zpath = NULL;
        unsigned long long in_zip = 0;
        int open = 0;
        memset(&w, 0, sizeof w);
        w.h = INVALID_HANDLE_VALUE;
        for (i = 0; i <= nmissing && code == MODNET_OK; i++) {
            const mc_file *file = NULL;
            char *src = NULL;
            int flush = i == nmissing;
            if (!flush) {
                for (f = 0; f < u.nfiles && !file; f++) if (!strcmp(u.files[f].sha, missing[i])) file = &u.files[f];
                if (!file || !(src = mc_source_of(file->sha, file->size))) {
                    code = MODNET_E_ARG;
                    if (why) *why = sh_textf("%s is not on this computer", file ? file->path : "a file of the unit");
                    free(src);
                    break;
                }
                if (file->size > MODNET_ONE_REQUEST_MAX - (1u << 20)) {
                    code = modnet_put_blob(file->sha, src, file->size, signer, io, &r);
                    st.requests += r.requests;
                    st.bytes_sent += r.sent;
                    st.parts += r.parts;
                    free(src);
                    continue;
                }
                flush = open && in_zip + file->size + 65536 > MODNET_ONE_REQUEST_MAX - (1u << 20);
            }
            if (flush && open) {
                unsigned long long size = w.off;
                int ok = sh_zip_writer_finish(&w), now = 0;
                if (w.h != INVALID_HANDLE_VALUE) { size = w.off; FlushFileBuffers(w.h); CloseHandle(w.h); w.h = INVALID_HANDLE_VALUE; }
                sh_zip_writer_free(&w);
                open = 0;
                if (!ok) { code = MODNET_E_ARG; if (why) *why = sh_text("the upload could not be written"); free(src); break; }
                code = modnet_put_unit_archive(hash, zpath, size, signer, &now, io, &r);
                st.requests += r.requests;
                st.bytes_sent += r.sent;
                st.archives++;
                mc_delete(zpath);
                free(zpath);
                zpath = NULL;
                in_zip = 0;
                if (code != MODNET_OK) { free(src); break; }
            }
            if (i == nmissing) { free(src); break; }
            if (!open) {
                wchar_t *wz;
                zpath = sh_textf("%s\\%s.upload-%lu.zip", dir ? dir : ".", hash, GetCurrentThreadId());
                wz = zpath && dir && mc_mkdirs(dir) ? sh_mb_wpath(zpath) : NULL;
                if (!wz || !sh_zip_writer_open(&w, wz) || !sh_zip_writer_add(&w, "unit.json", u.bytes, u.len)) {
                    free(wz);
                    code = MODNET_E_ARG;
                    if (why) *why = sh_text("the upload could not be written");
                    free(src);
                    break;
                }
                free(wz);
                open = 1;
            }
            {
                size_t len = 0;
                unsigned char *bytes = mc_read_all(src, file->size, &len);
                if (!bytes || len != file->size || !sh_zip_writer_add(&w, file->path, bytes, len)) {
                    code = MODNET_E_ARG;
                    if (why) *why = sh_textf("%s could not be read", file->path);
                }
                in_zip += file->size;
                free(bytes);
            }
            free(src);
        }
        if (open) { sh_zip_writer_free(&w); if (zpath) mc_delete(zpath); }
        free(zpath);
    }
    if (code == MODNET_OK) {
        code = modnet_complete_unit(hash, signer, io, &r);
        st.requests += r.requests;
        if (code == MODNET_OK) st.held = 1;
    }
done:
    if (code != MODNET_OK && why && !*why) *why = sh_text(r.message ? r.message : modnet_code_text(code));
    if (stats) *stats = st;
    free(missing);
    mc_unit_free(&u);
    free(dir);
    return code;
}
