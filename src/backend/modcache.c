/* modcache.c -- see modcache.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modcache.h"
#include "modmanifest.h"
#include "modunit.h"
#include "modbundle.h"
#include "modzip.h"
#include "map_carrier.h"
#include "mp_json.h"
#include "backend_log.h"
#include "../common/text.h"

/* ------------------------------------------------------------ state */

static SRWLOCK g_lock = SRWLOCK_INIT;     /* the rows, the notices, the local index, the settings */
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
    char *m = mc_mods_root(), *out = m ? sh_textf("%s\\" SH_CACHE_DIR, m) : NULL;
    free(m);
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

/* ------------------------------------------------------------ files */

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
    int ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
    free(a);
    free(b);
    return ok;
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

/* ------------------------------------------------------------ blobs and manifests */

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
    else if (ok) ok = mc_move(from, dest);
    if (ok && (w = sh_mb_wpath(dest)) != NULL) { SetFileAttributesW(w, FILE_ATTRIBUTE_READONLY); free(w); }
    free(dir);
    free(dest);
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
    if (strcmp(have, hash)) { free(bytes); return NULL; }   /* damaged: treated as absent (N17) */
    return (char *)bytes;
}

/* ------------------------------------------------------------ the local mods' files (D21, D51) */

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

/* Every built mod folder at the top of the mods root (never a dot folder or the retired maps\): its carried files,
 * by SHA-256. Called with the lock held exclusively. */
static void mc_local_build_locked(void)
{
    char *root = g_mods_root ? sh_text(g_mods_root) : (g_store_root && g_store_root() ? sh_text(g_store_root()) : NULL);
    char *pattern = root ? sh_textf("%s\\*", root) : NULL;
    wchar_t *wp = pattern ? sh_mb_wpath(pattern) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h = wp ? FindFirstFileW(wp, &fd) : INVALID_HANDLE_VALUE;
    size_t cap = 0;
    g_local_ready = 1;
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

/* D51: a local mod's file serves a unit only with the exact SHA-256 the unit names, checked again whenever its size
 * or write time changed since it was last checked (an author editing the folder, N18). Lock held exclusively. */
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
    {
        mc_local key;
        const mc_local *hit;
        strcpy_s(key.sha, sizeof key.sha, sha);
        hit = g_nlocal ? (const mc_local *)bsearch(&key, g_local, g_nlocal, sizeof *g_local, mc_local_cmp) : NULL;
        if (hit && hit->size == size && mc_local_verified((mc_local *)hit)) out = sh_text(hit->path);
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

/* ------------------------------------------------------------ manifests read */

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

/* ------------------------------------------------------------ rows */

typedef struct mc_absent { char *object, *mod; int cause; } mc_absent;
typedef struct mc_row {
    char chash[SH_CACHE_HASH_HEX + 1];
    char salt[SH_MODMANIFEST_SALT_CAP];
    char *name;
    size_t n;
    char (*units)[SH_CACHE_HASH_HEX + 1];
    char **roots;
    char **placed; size_t nplaced;     /* names the map places under mods/<salt>/ */
    mc_absent *absent; size_t nabsent; /* what its last composition stood in for */
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
    for (i = 0; i < r->nplaced; i++) free(r->placed[i]);
    free(r->placed);
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

void sh_cache_note_rows(const sh_modmanifest_list *rows, char *const *placed, size_t placed_count)
{
    size_t i, k;
    if (!rows) return;
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
    }
    ReleaseSRWLockExclusive(&g_lock);
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

/* ------------------------------------------------------------ notices (SPEC section 8) */

char *sh_cache_object_name(const char *root)
{
    const char *leaf = root ? strrchr(root, '/') : NULL;
    char *out;
    size_t i;
    int up = 1;
    leaf = leaf ? leaf + 1 : (root ? root : "");
    if (!strcmp(leaf, "*")) return sh_text("its objects");
    if (!(out = sh_text(leaf))) return NULL;
    for (i = 0; out[i]; i++) {
        if (out[i] == '_' || out[i] == '-' || out[i] == '.') { out[i] = ' '; up = 1; continue; }
        if (up && out[i] >= 'a' && out[i] <= 'z') out[i] = (char)(out[i] - 'a' + 'A');
        up = 0;
    }
    return out;
}

/* The cause order of SPEC section 8: the most actionable first. */
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
    default: return 7;
    }
}

char *sh_cache_notice_text(const sh_cache_absent *a, size_t n)
{
    char *subject = NULL, *out = NULL;
    size_t i, k, mods = 0, best = 0;
    const char *mod0 = NULL;
    int plural = n > 1;
    if (!a || !n) return NULL;
    for (i = 1; i < n; i++) if (mc_cause_rank(a[i].cause) < mc_cause_rank(a[best].cause)) best = i;
    for (i = 0; i < n; i++) {
        int seen = 0;
        for (k = 0; k < i && !seen; k++) seen = !strcmp(a[k].mod ? a[k].mod : "", a[i].mod ? a[i].mod : "");
        if (!seen) { mods++; if (!mod0) mod0 = a[i].mod; }
    }
    if (!plural) subject = sh_textf("the %s from %s", a[0].object, a[0].mod);
    else {
        char *list = NULL;
        size_t shown = n > 3 ? 3 : n;
        for (i = 0; i < shown; i++) {
            const char *sep = !i ? "" : (i + 1 == shown && n <= 3) ? " and " : ", ";
            (void)sh_text_set(&list, "%s%sthe %s", list ? list : "", sep, a[i].object);
        }
        if (n > 3) (void)sh_text_set(&list, "%s and %u more", list ? list : "", (unsigned)(n - 3));
        subject = mods > 1 ? sh_textf("%u objects from %u mods (%s)", (unsigned)n, (unsigned)mods, list ? list : "")
                           : sh_textf("%u objects from %s (%s)", (unsigned)n, mod0 ? mod0 : "a mod", list ? list : "");
        free(list);
    }
    if (!subject) return NULL;
    switch (a[best].cause) {
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
    default:
        out = sh_textf("This map is playing without %s.", subject);
        break;
    }
    free(subject);
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

/* ------------------------------------------------------------ composing */

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

typedef struct mc_extras { sh_unit_extra *v; char **paths; size_t n; } mc_extras;

static void mc_extra_add(mc_extras *e, const mc_unit *units, size_t nunits, char *path)
{
    size_t i;
    if (!path) return;
    for (i = 0; i < e->n; i++) if (!_stricmp(e->paths[i], path)) { free(path); return; }
    if (mc_carried(units, nunits, path)) { free(path); return; }
    {
        sh_unit_extra *gv = (sh_unit_extra *)realloc(e->v, (e->n + 1) * sizeof *gv);
        char **gp = gv ? (char **)realloc(e->paths, (e->n + 1) * sizeof *gp) : NULL;
        if (gv) e->v = gv;
        if (!gv || !gp) { free(path); return; }
        e->paths = gp;
        e->paths[e->n] = path;
        e->v[e->n].path = path;
        e->v[e->n].text = MC_ENTITY_STANDIN;
        e->v[e->n].len = sizeof MC_ENTITY_STANDIN - 1;
        e->n++;
    }
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
    (void)sh_text_set(&t, "%s]}", t ? t : "");
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

/* Compose row `chash`. causes[i] (or `cause` when NULL or 0) is the absence cause of unit i when it is not here. */
static int mc_compose(const char *chash, const int *causes, int cause, char **why, int *built)
{
    mc_row snapshot;
    mc_unit *units = NULL;
    mc_extras extras = { 0 };
    const char **manifests = NULL;
    size_t *lens = NULL, nm = 0, i, k;
    char *dest = NULL, *staging = NULL, *side = NULL, *old = NULL, *reason = NULL;
    sh_mb_bundle out;
    sh_mb_resolver resolver;
    int ok = 0, complete = 1, had_complete = 0, here;
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
        if (units[i].state == 1) { manifests[nm] = units[i].bytes; lens[nm] = units[i].len; nm++; continue; }
        complete = 0;
        units[i].cause = units[i].state == -2 ? SH_CACHE_ABSENT_NEWER : (causes && causes[i]) ? causes[i] : cause;
        if (!units[i].root) units[i].root = sh_text(snapshot.roots[i]);
    }
    here = sh_cache_composed(chash, &had_complete);
    if (here && had_complete) { ok = 1; complete = 1; goto record; }   /* the same units compose the same bytes (D22a) */
    if (here && !complete && mc_side_same(chash, units, snapshot.n)) { ok = 1; goto record; }   /* nothing new is here */

    /* Stand-ins (D60) for what is absent: the absent roots that are entities, and every name the map places under
     * the namespace that no present unit provides. */
    if (!complete) {
        for (i = 0; i < snapshot.n; i++) {
            if (units[i].state == 1 || !snapshot.roots[i]) continue;
            if (!strncmp(snapshot.roots[i], "entities/", 9)) mc_extra_add(&extras, units, snapshot.n, sh_textf("%s.decl", snapshot.roots[i]));
        }
        for (k = 0; k < snapshot.nplaced; k++) {
            const char *rest = snapshot.placed[k] + strlen("mods/") + strlen(snapshot.salt) + 1;
            char *lower = sh_textf("entities/%s.decl", rest), *c;
            for (c = lower; c && *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
            mc_extra_add(&extras, units, snapshot.n, lower);
        }
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
    src.units = units;
    src.n = snapshot.n;
    if (!sh_unit_compose_ex(manifests, lens, nm, mc_compose_source, &src, extras.v, extras.n, 1, snapshot.salt,
                            snapshot.name, &resolver, staging, &out, &reason)) {
        goto done;
    }
    sh_mb_free(&out);
    /* Swap the new composition in: the old one (stand-ins) moves aside first and goes after. */
    AcquireSRWLockExclusive(&g_disk);
    if (mc_dir_exists(dest)) {
        old = sh_textf("%s.old-%llu", dest, (unsigned long long)GetTickCount64());
        if (!old || !mc_move(dest, old)) {
            ReleaseSRWLockExclusive(&g_disk);
            mc_rmtree(staging);
            reason = sh_text("the earlier composition is in use; it is replaced at the next start");
            goto done;
        }
    }
    ok = mc_move(staging, dest) && mc_sidecar(side, &snapshot, units, complete);
    ReleaseSRWLockExclusive(&g_disk);
    if (old) mc_rmtree(old);
    if (!ok) { mc_rmtree(staging); reason = sh_text("the composition could not be put in place"); goto done; }
    mc_logf("MODS: content: %s composed from %zu unit(s)%s%s", snapshot.name, nm, complete ? "" : ", with stand-ins for ",
            complete ? "" : "the objects that are not here");
    if (built) *built = 1;

record:
    /* What the load's notice names (S2-S6): the objects of the units that are not here. */
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
                    r->absent[r->nabsent].object = sh_cache_object_name(snapshot.roots[i]);
                    r->absent[r->nabsent].mod = sh_text(snapshot.name ? snapshot.name : snapshot.salt);
                    r->absent[r->nabsent].cause = units[i].cause;
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
    for (i = 0; i < extras.n; i++) free(extras.paths[i]);
    free(extras.paths);
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

/* ------------------------------------------------------------ downloads */

/* A failure's absence cause. */
static int mc_net_cause(int code, const modnet_result *r)
{
    if (code == MODNET_E_BLOCKED) return SH_CACHE_ABSENT_REMOVED;
    if (code == MODNET_E_NOT_FOUND) return SH_CACHE_ABSENT_NOT_IN_STORE;
    if (code == MODNET_E_ARG && r && r->message && strstr(r->message, "(error 112)")) return SH_CACHE_ABSENT_DISK;
    return SH_CACHE_ABSENT_UNREACHABLE;
}

/* One blob into files\, resumed from partial\, checked; one retry after a bad download (N22). 0, or a cause. */
static int mc_fetch_blob(const char *sha, unsigned long long size, const modnet_io *io)
{
    char *dir = mc_path("partial"), *part = mc_path("partial\\%s.part", sha);
    int tries, cause = SH_CACHE_ABSENT_UNREACHABLE;
    if (!dir || !part || !mc_mkdirs(dir)) { free(dir); free(part); return SH_CACHE_ABSENT_DISK; }
    for (tries = 0; tries < 2; tries++) {
        unsigned long long have = 0;
        modnet_result r;
        int code;
        if (!mc_exists(part, &have) || have > size) have = 0;
        code = modnet_get_blob(sha, part, have, io, &r);
        if (code != MODNET_OK) { cause = mc_net_cause(code, &r); if (code == MODNET_E_CANCELLED) cause = -1; break; }
        if (sh_cache_put_blob(part, sha, size)) { cause = 0; break; }
        /* the bytes were not the blob's: discarded by put_blob; once more from the start */
    }
    free(dir);
    free(part);
    return cause;
}

/* The unit's files from its archive (more than half of its bytes missing): each entry checked into files\. */
static int mc_fetch_archive(const mc_unit *u, const modnet_io *io)
{
    char *dir = mc_path("partial"), *part = mc_path("partial\\%s.zip.part", u->hash), *cause_text = NULL;
    unsigned long long have = 0;
    modnet_result r;
    sh_zip_reader z;
    int code, cause = 0;
    size_t f;
    if (!dir || !part || !mc_mkdirs(dir)) { free(dir); free(part); return SH_CACHE_ABSENT_DISK; }
    if (!mc_exists(part, &have)) have = 0;
    code = modnet_get_unit_archive(u->hash, part, have, io, &r);
    if (code != MODNET_OK) { free(dir); free(part); return code == MODNET_E_CANCELLED ? -1 : mc_net_cause(code, &r); }
    memset(&z, 0, sizeof z);
    if (!sh_zip_reader_open(&z, part, &cause_text)) {
        mc_logf("MODS: content: a unit's archive did not read (%s); it is fetched file by file", cause_text ? cause_text : "?");
        free(cause_text);
        mc_delete(part);
        free(dir);
        free(part);
        return 1;   /* fall back to the blobs */
    }
    for (f = 0; f < u->nfiles && !cause; f++) {
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
        tmp = mc_path("partial\\%s.part", u->files[f].sha);
        if (!tmp || !mc_write(tmp, bytes, len)) cause = SH_CACHE_ABSENT_DISK;
        else (void)sh_cache_put_blob(tmp, u->files[f].sha, u->files[f].size);
        free(tmp);
        free(bytes);
    }
    sh_zip_reader_close(&z);
    mc_delete(part);
    free(dir);
    free(part);
    return cause;
}

/* The game's files a unit points to must be this game's bytes (N14). 1 when they are. */
static int mc_game_ok(const mc_unit *u)
{
    mpj *m = mpj_parse(u->bytes, u->len);
    const mpj *game = m ? mpj_get(m, "game") : NULL;
    int i, ok = 1;
    for (i = 0; ok && game && game->type == MPJ_ARR && i < game->n; i++) {
        const mpj *p = mpj_get(game->items[i], "path"), *s = mpj_get(game->items[i], "sha256"), *src = mpj_get(game->items[i], "source");
        unsigned long long size = 0;
        if (!p || p->type != MPJ_STR || !s || s->type != MPJ_STR) continue;
        if (src && mpj_str_is(src, "product")) continue;   /* the product's own derived files: checked where they serve */
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

int sh_cache_prepare(const char *chash, const modnet_io *io)
{
    mc_row snapshot;
    const char **want = NULL;
    int *states = NULL, *causes = NULL;
    size_t i, nwant = 0, f;
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
        /* The store's state of each absent unit (held, missing, removed); then each held one fetched. */
        int st = modnet_unit_status(want, nwant, states, io, &r);
        size_t w = 0;
        for (i = 0; i < snapshot.n; i++) {
            mc_unit u;
            if (w < nwant && want[w] == snapshot.units[i]) {
                int state = states[w++];
                if (st != MODNET_OK) { causes[i] = st == MODNET_E_CANCELLED ? SH_CACHE_ABSENT_UNREACHABLE : mc_net_cause(st, &r); continue; }
                if (state == MODNET_UNIT_REMOVED) { causes[i] = SH_CACHE_ABSENT_REMOVED; continue; }
                if (state == MODNET_UNIT_MISSING) {
                    /* Not in the store: what is here may still compose it (a local mod's files and the manifest). */
                    causes[i] = SH_CACHE_ABSENT_NOT_IN_STORE;
                    continue;
                }
                mc_unit_load(&u, snapshot.units[i]);
                if (u.state == -1) {
                    unsigned char *bytes = NULL;
                    size_t len = 0;
                    int g = modnet_get_unit(snapshot.units[i], &bytes, &len, io, &r);
                    if (g != MODNET_OK) { causes[i] = mc_net_cause(g, &r); mc_unit_free(&u); continue; }
                    if (!sh_cache_put_manifest(snapshot.units[i], bytes, len)) { causes[i] = SH_CACHE_ABSENT_DISK; free(bytes); mc_unit_free(&u); continue; }
                    free(bytes);
                    mc_unit_free(&u);
                    mc_unit_load(&u, snapshot.units[i]);
                }
                if (u.state == -2) { causes[i] = SH_CACHE_ABSENT_NEWER; mc_unit_free(&u); continue; }
                if (!mc_game_ok(&u)) { causes[i] = SH_CACHE_ABSENT_GAME; mc_unit_free(&u); continue; }
                if (u.state == 0) {
                    int c = 0;
                    if (u.bytes_missing * 2 > u.bytes_total) c = mc_fetch_archive(&u, io);
                    if (c < 0) { code = MODNET_E_CANCELLED; mc_unit_free(&u); goto done; }
                    if (c > 1) causes[i] = c;
                    for (f = 0; f < u.nfiles && !causes[i]; f++) {
                        char *src = mc_source_of(u.files[f].sha, u.files[f].size);
                        if (src) { free(src); continue; }
                        c = mc_fetch_blob(u.files[f].sha, u.files[f].size, io);
                        if (c < 0) { code = MODNET_E_CANCELLED; mc_unit_free(&u); goto done; }
                        if (c) causes[i] = c;
                    }
                }
                mc_unit_free(&u);
            }
        }
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

/* ------------------------------------------------------------ the load step */

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

/* ------------------------------------------------------------ kept maps and the sweep */

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

/* The pinned rows of one map text: units into `units`, composition hashes into `comps`. */
static size_t mc_map_refs(const char *path, mc_set *units, mc_set *comps, unsigned long long *bytes_out)
{
    size_t len = 0, i, k, n = 0;
    unsigned char *text = sh_mb_read_file(path, (size_t)1 << 31, &len);
    sh_modmanifest_list rows;
    if (!text) return 0;
    memset(&rows, 0, sizeof rows);
    if (sh_carrier_part((const char *)text, len, "mods", NULL, NULL) == 1) sh_modmanifest_read((const char *)text, len, &rows, NULL);
    for (i = 0; i < rows.count; i++) {
        if (!rows.rows[i].pinned) continue;
        n++;
        mc_set_add(comps, rows.rows[i].build);
        for (k = 0; k < rows.rows[i].unit_count; k++) mc_set_add(units, rows.rows[i].units[k]);
    }
    (void)bytes_out;
    sh_modmanifest_list_free(&rows);
    free(text);
    return n;
}

/* Every *.decl in `dir`. */
static size_t mc_scan_maps(const char *dir, mc_set *units, mc_set *comps)
{
    char *pattern = dir ? sh_textf("%s\\*.decl", dir) : NULL;
    wchar_t *wp = pattern ? sh_mb_wpath(pattern) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h = wp ? FindFirstFileW(wp, &fd) : INVALID_HANDLE_VALUE;
    size_t maps = 0;
    while (h != INVALID_HANDLE_VALUE) {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            char *leaf = sh_mb_utf8(fd.cFileName), *path = leaf ? sh_textf("%s\\%s", dir, leaf) : NULL;
            if (path && mc_map_refs(path, units, comps, NULL)) maps++;
            free(path);
            free(leaf);
        }
        if (!FindNextFileW(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
    }
    free(wp);
    free(pattern);
    return maps;
}

/* The files of `dir` matching `pattern` (leaf names, malloc'd array of malloc'd). */
static char **mc_list(const char *dir, const char *pattern, int dirs, size_t *count)
{
    char *pat = sh_textf("%s\\%s", dir, pattern), **out = NULL;
    wchar_t *wp = pat ? sh_mb_wpath(pat) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h = wp ? FindFirstFileW(wp, &fd) : INVALID_HANDLE_VALUE;
    size_t n = 0;
    while (h != INVALID_HANDLE_VALUE) {
        int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (fd.cFileName[0] != L'.' && isdir == dirs) {
            char **g = (char **)realloc(out, (n + 1) * sizeof *g);
            if (g) { out = g; if ((out[n] = sh_mb_utf8(fd.cFileName)) != NULL) n++; }
        }
        if (!FindNextFileW(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
    }
    free(wp);
    free(pat);
    *count = n;
    return out;
}

static void mc_list_free(char **v, size_t n) { size_t i; for (i = 0; i < n; i++) free(v[i]); free(v); }

int sh_cache_sweep(int drop_played, const char *const *held, size_t held_count, sh_cache_sweep_stats *out)
{
    mc_set units = { 0 }, comps = { 0 }, blobs = { 0 };
    char *saved, *published, *udir = mc_path("units"), *sdir = mc_path("served"), *fdir = mc_path("files");
    char **names;
    size_t n, i;
    sh_cache_sweep_stats st;
    sh_modmanifest_list cur;
    memset(&st, 0, sizeof st);
    AcquireSRWLockShared(&g_lock);
    saved = g_saved_dir ? sh_text(g_saved_dir) : mc_game_maps("saved-maps");
    published = g_published_dir ? sh_text(g_published_dir) : mc_game_maps("published-maps");
    ReleaseSRWLockShared(&g_lock);
    /* The references (5.17). */
    st.maps += mc_scan_maps(saved, &units, &comps);
    if (!drop_played) st.maps += mc_scan_maps(published, &units, &comps);
    memset(&cur, 0, sizeof cur);
    if (sh_modmanifest_current(&cur)) {
        for (i = 0; i < cur.count; i++) {
            size_t k;
            if (!cur.rows[i].pinned) continue;
            mc_set_add(&comps, cur.rows[i].build);
            for (k = 0; k < cur.rows[i].unit_count; k++) mc_set_add(&units, cur.rows[i].units[k]);
        }
    }
    sh_modmanifest_list_free(&cur);
    for (i = 0; held && i < held_count; i++) mc_set_add(&units, held[i]);

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
        if (!mc_set_has(&units, hash)) { if (mc_delete(path)) st.units_removed++; free(path); continue; }
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
        if (!stale) { memcpy(hash, names[i], 65); stale = !mc_hex64(hash) || !mc_set_has(&comps, hash); }
        if (path && stale && mc_rmtree(path)) {
            char *side = strlen(names[i]) == 64 ? sh_textf("%s.json", path) : NULL;
            if (side) mc_delete(side);
            free(side);
            st.served_removed++;
        }
        free(path);
    }
    mc_list_free(names, names ? n : 0);
    ReleaseSRWLockExclusive(&g_disk);
    mc_logf("MODS: content: the sweep kept %zu unit(s) of %zu map(s) and removed %zu unit(s), %zu file(s) (%llu bytes) "
            "and %zu composition(s)%s", st.units_kept, st.maps, st.units_removed, st.blobs_removed, st.bytes_removed,
            st.served_removed, drop_played ? "; content of maps only played was removed" : "");
    if (out) *out = st;
    free(units.v);
    free(comps.v);
    free(blobs.v);
    free(saved);
    free(published);
    free(udir);
    free(sdir);
    free(fdir);
    return 1;
}

/* ------------------------------------------------------------ the Mods tab */

static void mc_size_text(char *out, size_t cap, unsigned long long bytes)
{
    double v = (double)bytes;
    if (v >= 1024.0 * 1024.0 * 1024.0) _snprintf_s(out, cap, _TRUNCATE, "%.1f GB", v / (1024.0 * 1024.0 * 1024.0));
    else if (v >= 1024.0 * 1024.0) _snprintf_s(out, cap, _TRUNCATE, "%.1f MB", v / (1024.0 * 1024.0));
    else _snprintf_s(out, cap, _TRUNCATE, "%.0f KB", v / 1024.0);
}

/* Each kept map's rows: one line per row, and the totals. */
typedef struct mc_report { sh_cache_line_fn fn; void *ctx; mc_set files; unsigned long long total; size_t maps, lines; } mc_report;

static void mc_report_map(mc_report *rep, const char *path, const char *kind)
{
    size_t len = 0, i, k, f;
    unsigned char *text = sh_mb_read_file(path, (size_t)1 << 31, &len);
    sh_modmanifest_list rows;
    int counted = 0;
    if (!text) return;
    memset(&rows, 0, sizeof rows);
    if (sh_carrier_part((const char *)text, len, "mods", NULL, NULL) == 1) sh_modmanifest_read((const char *)text, len, &rows, NULL);
    for (i = 0; i < rows.count; i++) {
        const sh_modmanifest_row *row = &rows.rows[i];
        unsigned long long bytes = 0, shared = 0;
        size_t objects = 0;
        char size[32], share[32], *line;
        if (!row->pinned) continue;
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
        if (line && rep->fn) { rep->fn(rep->ctx, line); rep->lines++; }
        free(line);
    }
    sh_modmanifest_list_free(&rows);
    free(text);
}

static void mc_report_dir(mc_report *rep, const char *dir, const char *kind)
{
    char **names;
    size_t n, i;
    if (!dir) return;
    names = mc_list(dir, "*.decl", 0, &n);
    for (i = 0; names && i < n; i++) {
        char *path = sh_textf("%s\\%s", dir, names[i]);
        if (path) mc_report_map(rep, path, kind);
        free(path);
    }
    mc_list_free(names, names ? n : 0);
}

static void mc_report_run(mc_report *rep)
{
    char *saved, *published;
    AcquireSRWLockShared(&g_lock);
    saved = g_saved_dir ? sh_text(g_saved_dir) : mc_game_maps("saved-maps");
    published = g_published_dir ? sh_text(g_published_dir) : mc_game_maps("published-maps");
    ReleaseSRWLockShared(&g_lock);
    mc_report_dir(rep, saved, "kept");
    mc_report_dir(rep, published, "played");
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
    if (line && fn) { fn(context, line); rep.lines++; }
    free(line);
    free(rep.files.v);
    return (int)rep.lines;
}

/* The sweep at start, off the boot thread. */
static DWORD WINAPI mc_sweep_thread(LPVOID unused)
{
    (void)unused;
    (void)sh_cache_sweep(0, NULL, 0, NULL);
    return 0;
}

void sh_cache_sweep_async(void)
{
    HANDLE h = CreateThread(NULL, 0, mc_sweep_thread, NULL, 0, NULL);
    if (h) CloseHandle(h);
}
/* ------------------------------------------------------------ upload (5.3 steps 2-6) */

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