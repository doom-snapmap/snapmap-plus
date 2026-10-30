/* modsync.c -- see modsync.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "modsync.h"
#include "modstore.h"
#include "modserve.h"
#include "mp_json.h"
#include "backend_log.h"

#define MSY_KEY_MAX (SH_MB_ID_MAX + 1 + 4 * SH_MB_VERSION_MAX + 1 + 20 + 8)
#define MSY_DOC_MAX (4u * 1024u * 1024u)

/* ------------------------------------------------------------ lock ------ */

/* The store lock lives in modstore.c (modstore_lock); these are its older
 * names, kept for the callers that already use them. */
void modsync_store_lock(void)
{
    modstore_lock();
}

void modsync_store_unlock(void)
{
    modstore_unlock();
}

/* ------------------------------------------------------------ helpers --- */

static void msy_log(const char *fmt, ...)
{
    char line[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static int msy_cause(char *cause, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (cause && cap) {
        va_start(ap, fmt);
        _vsnprintf_s(cause, cap, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    return 0;
}

static double msy_now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static int msy_cancelled(const modnet_io *io)
{
    return io && io->cancel && InterlockedCompareExchange(io->cancel, 0, 0) != 0;
}

/* UTF-8 dir + '\' + UTF-8 relative path ('/' turned into '\'). */
static int msy_join(char *out, size_t cap, const char *dir, const char *rel)
{
    size_t i, n;
    if (_snprintf_s(out, cap, _TRUNCATE, "%s\\%s", dir, rel) < 0) return 0;
    n = strlen(out);
    for (i = strlen(dir); i < n; i++) if (out[i] == '/') out[i] = '\\';
    return 1;
}

static int msy_exists_w(const wchar_t *w, unsigned long long *size, int *is_dir)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &d)) return 0;
    if (size) *size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    if (is_dir) *is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return 1;
}

static int msy_exists(const char *path, unsigned long long *size, int *is_dir)
{
    wchar_t *w = sh_mb_wpath(path);
    int ok = w && msy_exists_w(w, size, is_dir);
    free(w);
    return ok;
}

static int msy_mkdirs_w(const wchar_t *path)
{
    size_t n = wcslen(path), i, start = 0;
    wchar_t *copy = (wchar_t *)malloc(sizeof(wchar_t) * (n + 1));
    int ok = 1;
    if (!copy) return 0;
    memcpy(copy, path, sizeof(wchar_t) * (n + 1));
    if (wcsncmp(copy, L"\\\\?\\UNC\\", 8) == 0) {
        int seps = 0;
        for (start = 8; start < n && seps < 2; start++) if (copy[start] == L'\\') seps++;
    } else if (wcsncmp(copy, L"\\\\?\\", 4) == 0) {
        start = 7;
    }
    for (i = start; i <= n && ok; i++) {
        if (copy[i] == L'\\' || copy[i] == 0) {
            wchar_t keep = copy[i];
            copy[i] = 0;
            if (i > start && !CreateDirectoryW(copy, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ok = 0;
            copy[i] = keep;
        }
    }
    free(copy);
    return ok;
}

static int msy_mkdirs(const char *path)
{
    wchar_t *w = sh_mb_wpath(path);
    int ok = w && msy_mkdirs_w(w);
    free(w);
    return ok;
}

static int msy_mkdirs_parent_w(const wchar_t *file)
{
    const wchar_t *slash = wcsrchr(file, L'\\');
    wchar_t *dir;
    int ok;
    if (!slash) return 0;
    dir = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(slash - file + 1));
    if (!dir) return 0;
    memcpy(dir, file, sizeof(wchar_t) * (size_t)(slash - file));
    dir[slash - file] = 0;
    ok = msy_mkdirs_w(dir);
    free(dir);
    return ok;
}

/* Write a file through a temporary in the same folder, replacing atomically. */
static int msy_write_file(const char *path, const void *data, size_t len, int read_only)
{
    wchar_t *w = sh_mb_wpath(path), *tmp;
    HANDLE h;
    size_t n, off = 0;
    int ok = 1;
    if (!w) return 0;
    n = wcslen(w);
    tmp = (wchar_t *)malloc(sizeof(wchar_t) * (n + 32));
    if (!tmp) { free(w); return 0; }
    _snwprintf_s(tmp, n + 32, _TRUNCATE, L"%s.tmp-%lu", w, GetCurrentProcessId());
    SetFileAttributesW(tmp, FILE_ATTRIBUTE_NORMAL);
    DeleteFileW(tmp);
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { free(tmp); free(w); return 0; }
    while (ok && off < len) {
        DWORD chunk = (DWORD)((len - off) > (1u << 24) ? (1u << 24) : (len - off)), put = 0;
        ok = WriteFile(h, (const unsigned char *)data + off, chunk, &put, NULL) && put == chunk;
        off += put;
    }
    ok = ok && FlushFileBuffers(h);
    CloseHandle(h);
    if (ok && read_only) SetFileAttributesW(tmp, FILE_ATTRIBUTE_READONLY);
    ok = ok && MoveFileExW(tmp, w, read_only ? 0 : (MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH));
    if (!ok) { SetFileAttributesW(tmp, FILE_ATTRIBUTE_NORMAL); DeleteFileW(tmp); }
    free(tmp);
    free(w);
    return ok;
}

#ifndef FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE
#define FILE_DISPOSITION_FLAG_DELETE                    0x00000001
#define FILE_DISPOSITION_FLAG_POSIX_SEMANTICS           0x00000002
#define FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE 0x00000010
#endif
typedef struct msy_disposition_ex { DWORD Flags; } msy_disposition_ex;

/* Delete one file or link without clearing a read-only attribute it shares
 * with a blob when the system allows that. */
static int msy_delete_file_w(const wchar_t *path)
{
    HANDLE h = CreateFileW(path, DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        msy_disposition_ex d;
        BOOL done;
        d.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                  FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
        done = SetFileInformationByHandle(h, (FILE_INFO_BY_HANDLE_CLASS)21, &d, sizeof d);
        CloseHandle(h);
        if (done) return 1;
    }
    SetFileAttributesW(path, FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

/* Remove a folder this module created: its leaf must carry ".staging-" or
 * ".old-" and it must lie inside the store root. Reparse points are removed,
 * never followed. */
static int msy_rmtree_w(const wchar_t *path, const wchar_t *root, int top)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern;
    HANDLE h;
    size_t rn = wcslen(root), pn = wcslen(path);
    int ok = 1;
    if (pn <= rn + 1 || _wcsnicmp(path, root, rn) != 0 || path[rn] != L'\\') return 0;
    if (top) {
        const wchar_t *leaf = wcsrchr(path, L'\\');
        if (!leaf || (!wcsstr(leaf, L".staging-") && !wcsstr(leaf, L".old-"))) return 0;
    }
    pattern = (wchar_t *)malloc(sizeof(wchar_t) * (pn + 3));
    if (!pattern) return 0;
    _snwprintf_s(pattern, pn + 3, _TRUNCATE, L"%s\\*", path);
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            size_t cn;
            wchar_t *child;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            cn = pn + wcslen(fd.cFileName) + 2;
            child = (wchar_t *)malloc(sizeof(wchar_t) * cn);
            if (!child) { ok = 0; continue; }
            _snwprintf_s(child, cn, _TRUNCATE, L"%s\\%s", path, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                if (!msy_rmtree_w(child, root, 0)) ok = 0;
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!RemoveDirectoryW(child)) ok = 0;
            } else if (!msy_delete_file_w(child)) {
                ok = 0;
            }
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (!RemoveDirectoryW(path) && GetLastError() != ERROR_FILE_NOT_FOUND) ok = 0;
    return ok;
}

/* JSON string body (no quotes) with '"', '\\' and controls escaped. */
static char *msy_escape(const char *s)
{
    size_t n = 0, i;
    char *out, *w;
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        n += (c == '"' || c == '\\') ? 2 : (c < 0x20 ? 6 : 1);
    }
    out = (char *)malloc(n + 1);
    if (!out) return NULL;
    for (w = out, i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { *w++ = '\\'; *w++ = (char)c; }
        else if (c < 0x20) { _snprintf_s(w, 7, _TRUNCATE, "\\u%04x", c); w += 6; }
        else *w++ = (char)c;
    }
    *w = 0;
    return out;
}

/* Printable ASCII without quote or backslash (stored error text). */
static void msy_plain(const char *in, char *out, size_t cap)
{
    size_t w = 0;
    for (; in && *in && w + 1 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') out[w++] = (char)c;
    }
    out[w] = 0;
}

static void msy_obj_remove(mpj *obj, const char *key)
{
    int i;
    if (!obj || obj->type != MPJ_OBJ) return;
    for (i = 0; i < obj->n; i++) {
        if (strcmp(obj->keys[i], key) != 0) continue;
        mpj_free(obj->items[i]);
        free(obj->keys[i]);
        memmove(&obj->items[i], &obj->items[i + 1], sizeof(mpj *) * (size_t)(obj->n - i - 1));
        memmove(&obj->keys[i], &obj->keys[i + 1], sizeof(char *) * (size_t)(obj->n - i - 1));
        obj->n--;
        return;
    }
}

static int msy_root(char *out, size_t cap)
{
    return sh_ms_root(out, cap);
}

/* ------------------------------------------------------------ docs ------ */

/* A section of the product record (mods\snapmap-plus.json) as the document
 * {"format":1,"<member>":{...}} the code below edits. A missing section is an
 * empty one; an unreadable record is moved aside by the record layer when
 * the section is written next (never silently lost). */
static mpj *msy_doc_load(const char *section, const char *member)
{
    char root[MAX_PATH * 4], *text = NULL;
    mpj *doc, *m = NULL;
    if (!msy_root(root, sizeof root)) return NULL;
    if (sh_mb_record_get(root, section, &text) == 1 && text) m = mpj_parse(text, strlen(text));
    if (text) { SecureZeroMemory(text, strlen(text)); free(text); }
    if (m && m->type != MPJ_OBJ) { mpj_free(m); m = NULL; }
    doc = mpj_new(MPJ_OBJ);
    if (!doc || !mpj_set(doc, "format", mpj_new_int(1)) || !mpj_set(doc, member, m ? m : mpj_new(MPJ_OBJ))) {
        mpj_free(doc);
        return NULL;
    }
    return doc;
}

static int msy_doc_save(const char *section, const mpj *doc)
{
    char root[MAX_PATH * 4];
    const mpj *m = doc ? mpj_get(doc, section) : NULL;
    size_t len = 0;
    char *text;
    int ok;
    if (!m || !msy_root(root, sizeof root)) return 0;
    text = mpj_dump(m, &len);
    if (!text) return 0;
    ok = sh_mb_record_put(root, section, text);
    SecureZeroMemory(text, len);
    free(text);
    return ok;
}

static mpj *msy_hash_array(const char *const *hashes, int n)
{
    mpj *arr = mpj_new(MPJ_ARR);
    int i;
    if (!arr) return NULL;
    for (i = 0; i < n; i++)
        if (!mpj_push(arr, mpj_new_str(hashes[i]))) { mpj_free(arr); return NULL; }
    return arr;
}

int modsync_claim_lookup(const char *ref, char claim_out[MODNET_CLAIM_HEX + 1], char secret_out[MODNET_HASH_HEX + 1])
{
    mpj *doc, *claims, *e;
    const mpj *c, *s;
    char *key;
    int found = 0;
    if (claim_out) claim_out[0] = 0;
    if (secret_out) secret_out[0] = 0;
    if (!ref || !(key = msy_escape(ref))) return 0;
    modsync_store_lock();
    doc = msy_doc_load(MODSYNC_CLAIMS_SECTION, "claims");
    modsync_store_unlock();
    claims = doc ? mpj_get(doc, "claims") : NULL;
    e = claims ? mpj_get(claims, key) : NULL;
    c = e ? mpj_get(e, "claim") : NULL;
    s = e ? mpj_get(e, "secret") : NULL;
    if (c && c->type == MPJ_STR && strlen(c->text) == MODNET_CLAIM_HEX && s && s->type == MPJ_STR && modnet_valid_hash(s->text)) {
        if (claim_out) strcpy_s(claim_out, MODNET_CLAIM_HEX + 1, c->text);
        if (secret_out) strcpy_s(secret_out, MODNET_HASH_HEX + 1, s->text);
        found = 1;
    }
    if (doc) {
        /* The secret is in this tree's text; clear it before release. */
        if (s && s->type == MPJ_STR && s->text) SecureZeroMemory(s->text, strlen(s->text));
        mpj_free(doc);
    }
    free(key);
    return found;
}

static int msy_claim_store(const char *ref, const char *claim, const char *secret, const char *const *hashes, int n)
{
    mpj *doc, *claims, *e;
    char *key = msy_escape(ref);
    int ok = 0;
    if (!key) return 0;
    modsync_store_lock();
    doc = msy_doc_load(MODSYNC_CLAIMS_SECTION, "claims");
    claims = doc ? mpj_get(doc, "claims") : NULL;
    e = claims ? mpj_new(MPJ_OBJ) : NULL;
    if (e && mpj_set(e, "bundles", msy_hash_array(hashes, n)) && mpj_set(e, "claim", mpj_new_str(claim)) &&
        mpj_set(e, "secret", mpj_new_str(secret)) && mpj_set(e, "updated", mpj_new_int((long long)time(NULL)))) {
        if (mpj_set(claims, key, e)) { e = NULL; ok = msy_doc_save(MODSYNC_CLAIMS_SECTION, doc); }
    }
    mpj_free(e);
    mpj_free(doc);
    modsync_store_unlock();
    free(key);
    return ok;
}

static int msy_claim_forget(const char *ref)
{
    mpj *doc, *claims;
    char *key = msy_escape(ref);
    int ok = 0;
    if (!key) return 0;
    modsync_store_lock();
    doc = msy_doc_load(MODSYNC_CLAIMS_SECTION, "claims");
    claims = doc ? mpj_get(doc, "claims") : NULL;
    if (claims) { msy_obj_remove(claims, key); ok = msy_doc_save(MODSYNC_CLAIMS_SECTION, doc); }
    mpj_free(doc);
    modsync_store_unlock();
    free(key);
    return ok;
}

static int msy_pending_put(const char *ref, const char *const *hashes, int n, const char *error)
{
    mpj *doc, *pend, *e, *old;
    const mpj *att;
    char *key = msy_escape(ref), plain[200];
    long long attempts = 1;
    int ok = 0;
    if (!key) return 0;
    msy_plain(error, plain, sizeof plain);
    modsync_store_lock();
    doc = msy_doc_load(MODSYNC_PENDING_SECTION, "pending");
    pend = doc ? mpj_get(doc, "pending") : NULL;
    old = pend ? mpj_get(pend, key) : NULL;
    att = old ? mpj_get(old, "attempts") : NULL;
    if (att && mpj_is_num(att)) attempts = mpj_int(att) + 1;
    e = pend ? mpj_new(MPJ_OBJ) : NULL;
    if (e && mpj_set(e, "attempts", mpj_new_int(attempts)) && mpj_set(e, "bundles", msy_hash_array(hashes, n)) &&
        mpj_set(e, "lastError", mpj_new_str(plain)) && mpj_set(e, "queuedAt", mpj_new_int((long long)time(NULL)))) {
        if (mpj_set(pend, key, e)) { e = NULL; ok = msy_doc_save(MODSYNC_PENDING_SECTION, doc); }
    }
    mpj_free(e);
    mpj_free(doc);
    modsync_store_unlock();
    free(key);
    return ok;
}

static void msy_pending_drop(const char *ref)
{
    mpj *doc, *pend;
    char *key = msy_escape(ref);
    if (!key) return;
    modsync_store_lock();
    doc = msy_doc_load(MODSYNC_PENDING_SECTION, "pending");
    pend = doc ? mpj_get(doc, "pending") : NULL;
    if (pend && mpj_get(pend, key)) { msy_obj_remove(pend, key); msy_doc_save(MODSYNC_PENDING_SECTION, doc); }
    mpj_free(doc);
    modsync_store_unlock();
    free(key);
}

int modsync_pending_count(void)
{
    mpj *doc, *pend;
    int n;
    modsync_store_lock();
    doc = msy_doc_load(MODSYNC_PENDING_SECTION, "pending");
    modsync_store_unlock();
    pend = doc ? mpj_get(doc, "pending") : NULL;
    n = pend ? pend->n : -1;
    mpj_free(doc);
    return n;
}

/* ------------------------------------------------------------ install --- */

int modsync_find_installed(const char *hash, char *folder, size_t cap)
{
    sh_ms_list l;
    size_t i;
    int found = 0;
    if (folder && cap) folder[0] = 0;
    if (!modnet_valid_hash(hash)) return 0;
    modsync_store_lock();
    if (sh_ms_enumerate(&l)) {
        for (i = 0; i < l.count && !found; i++) {
            const sh_ms_entry *e = &l.items[i];
            if (sh_ms_entry_usable(e) && strcmp(e->bundle.hash, hash) == 0) {
                found = 1;
                if (folder && cap) strncpy_s(folder, cap, e->folder, _TRUNCATE);
            }
        }
        sh_ms_list_free(&l);
    }
    modsync_store_unlock();
    return found;
}

/* The bundle.json form of a canonical manifest: the same members plus
 * "bundle", which sh_mb_load_manifest_text needs and checks against the text. */
static char *msy_with_bundle(const char *canonical, size_t len, const char *hash)
{
    size_t need = len + 96;
    char *out;
    if (!len || canonical[0] != '{') return NULL;
    out = (char *)malloc(need);
    if (!out) return NULL;
    _snprintf_s(out, need, _TRUNCATE, "{\"bundle\":\"%s\"%s%.*s", hash, (len > 1 && canonical[1] != '}') ? "," : "",
                (int)(len - 1), canonical + 1);
    return out;
}

/* A carried file of an installed mod with this hash: its path (UTF-8), so a
 * download copies it instead of fetching it again. */
typedef struct msy_local { char sha[SH_MB_HASH_HEX + 1]; char *path; } msy_local;

static void msy_locals_free(msy_local *l, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) free(l[i].path);
    free(l);
}

static msy_local *msy_locals(const sh_ms_list *l, const sh_mb_bundle *b, size_t *count)
{
    msy_local *out = (msy_local *)calloc(b->file_count ? b->file_count : 1, sizeof *out);
    size_t i, k, f, n = 0;
    *count = 0;
    if (!out) return NULL;
    for (f = 0; f < b->file_count; f++) {
        int found = 0;
        if (b->files[f].stock) continue;
        for (k = 0; k < n && !found; k++) found = !strcmp(out[k].sha, b->files[f].blob);
        for (i = 0; i < l->count && !found; i++) {
            const sh_ms_entry *e = &l->items[i];
            if (!sh_ms_entry_usable(e)) continue;
            for (k = 0; k < e->bundle.file_count && !found; k++) {
                const sh_mb_file *g = &e->bundle.files[k];
                char path[MAX_PATH * 4];
                if (g->stock || strcmp(g->blob, b->files[f].blob)) continue;
                msy_join(path, sizeof path, e->folder, g->path);
                strcpy_s(out[n].sha, sizeof out[n].sha, g->blob);
                out[n].path = _strdup(path);
                if (out[n].path) { n++; found = 1; }
            }
        }
    }
    *count = n;
    return out;
}

/* Put one carried file at `dst`: a local copy of the same bytes, else the
 * source; the bytes are checked against the manifest either way. */
static int msy_place_file(const char *dst, const sh_mb_file *f, const msy_local *locals, size_t nlocals,
                          modsync_blob_source_fn source, void *ctx, modsync_install_info *info, char *cause, size_t cap)
{
    char actual[SH_MB_HASH_HEX + 1], why[256];
    unsigned char *bytes = NULL;
    size_t len = 0, i;
    int ok, fetched = 0;
    for (i = 0; i < nlocals && !bytes; i++) {
        if (strcmp(locals[i].sha, f->blob)) continue;
        bytes = sh_mb_read_file(locals[i].path, (size_t)f->size + 1, &len);
        if (bytes) {
            sh_mb_sha256_hex(bytes, len, actual);
            if (len != f->size || strcmp(actual, f->blob)) { free(bytes); bytes = NULL; }
        }
    }
    if (!bytes) {
        why[0] = 0;
        if (!source || !source(ctx, f->blob, f->size, &bytes, &len, why, sizeof why))
            return msy_cause(cause, cap, "%s (%.16s): %s", f->path, f->blob, why[0] ? why : "not available");
        fetched = 1;
        sh_mb_sha256_hex(bytes, len, actual);
        if (len != f->size || strcmp(actual, f->blob) != 0) {
            free(bytes);
            return msy_cause(cause, cap, "%s (%.16s) does not match the manifest; refused", f->path, f->blob);
        }
    }
    {
        char parent[MAX_PATH * 4], *slash;
        strcpy_s(parent, sizeof parent, dst);
        slash = strrchr(parent, '\\');
        if (slash) { *slash = 0; msy_mkdirs(parent); }
    }
    ok = msy_write_file(dst, bytes, len, 0);
    free(bytes);
    if (!ok) return msy_cause(cause, cap, "cannot write %s (error %lu)", f->path, GetLastError());
    if (fetched) { info->blobs_fetched++; info->bytes_fetched += len; }
    else info->blobs_present++;
    return 1;
}

int modsync_install_manifest(const char *canonical, size_t len, const char *hash,
                             modsync_blob_source_fn source, void *ctx,
                             modsync_install_info *info, char *cause, size_t cap)
{
    modsync_install_info local;
    sh_mb_bundle b;
    sh_ms_list list;
    msy_local *locals = NULL;
    size_t nlocals = 0, i;
    char actual[SH_MB_HASH_HEX + 1], root[MAX_PATH * 4], final[MAX_PATH * 4], staging[MAX_PATH * 4],
         old[MAX_PATH * 4], name[SH_MS_NAME_MAX + 1], one[SH_MB_CAUSE_MAX], content[SH_MB_HASH_HEX + 1];
    char *with = NULL, *text = NULL;
    size_t text_len = 0;
    wchar_t *wroot = NULL, *wfinal = NULL, *wstaging = NULL, *wold = NULL;
    int ok = 0, replace = 0, named, listed = 0;
    if (!info) info = &local;
    memset(info, 0, sizeof *info);
    memset(&b, 0, sizeof b);
    memset(&list, 0, sizeof list);
    if (cause && cap) cause[0] = 0;
    if (!canonical || !len || !modnet_valid_hash(hash)) return msy_cause(cause, cap, "no manifest");
    sh_mb_sha256_hex(canonical, len, actual);
    if (strcmp(actual, hash) != 0) return msy_cause(cause, cap, "the manifest does not hash to %.16s", hash);
    with = msy_with_bundle(canonical, len, hash);
    if (!with) return msy_cause(cause, cap, "the manifest is not a JSON object");
    if (!sh_mb_load_manifest_text(with, strlen(with), &b)) {
        msy_cause(cause, cap, "the manifest is not a bundle this release accepts: %s", b.first_cause);
        free(with);
        sh_mb_free(&b);
        return 0;
    }
    free(with);
    if (strcmp(b.hash, hash) != 0 || b.canonical_len != len || memcmp(b.canonical, canonical, len) != 0) {
        sh_mb_free(&b);
        return msy_cause(cause, cap, "the manifest is not in canonical form");
    }
    /* Stock pointers name this player's installed game: a game that lacks an
     * entry (or holds other bytes, a different revision) refuses the bundle
     * before anything is fetched. */
    for (i = 0; i < b.file_count; i++) {
        if (!b.files[i].stock) continue;
        if (sh_mb_stock_verify(b.files[i].stock_source, b.files[i].blob, NULL, b.files[i].path, one, sizeof one) !=
            SH_MB_STOCK_OK) {
            msy_cause(cause, cap, "refused: %s", one);
            sh_mb_free(&b);
            return 0;
        }
        info->stock_files++;
    }
    sh_mb_content_key(&b, content);

    modsync_store_lock();
    if (!msy_root(root, sizeof root)) { msy_cause(cause, cap, "the store folder cannot be resolved"); goto done; }
    /* Installed already, in whatever folder (a hand-built mod is built in place). */
    if (modsync_find_installed(hash, final, sizeof final)) {
        strncpy_s(info->rel, sizeof info->rel, strrchr(final, '\\') ? strrchr(final, '\\') + 1 : final, _TRUNCATE);
        info->already_installed = 1;
        ok = 1;
        goto done;
    }
    listed = sh_ms_enumerate(&list);
    /* A copy of what an installed mod of the same id already serves is not
     * stored twice: the hash answers as the installed bundle's (an alias in
     * the record, which the serving index reads). */
    for (i = 0; listed && i < list.count; i++) {
        const sh_ms_entry *e = &list.items[i];
        char key[SH_MB_HASH_HEX + 1];
        if (!sh_ms_entry_usable(e) || strcmp(e->bundle.id, b.id)) continue;
        sh_mb_content_key(&e->bundle, key);
        if (strcmp(key, content)) continue;
        {
            char *text_aliases = NULL, *out;
            size_t n;
            mpj *aliases = NULL;
            sh_mb_record_lock();
            if (sh_mb_record_get(root, "aliases", &text_aliases) == 1 && text_aliases)
                aliases = mpj_parse(text_aliases, strlen(text_aliases));
            free(text_aliases);
            if (!aliases || aliases->type != MPJ_OBJ) { mpj_free(aliases); aliases = mpj_new(MPJ_OBJ); }
            if (aliases && mpj_set(aliases, hash, mpj_new_str(e->bundle.hash)) && (out = mpj_dump(aliases, &n)) != NULL) {
                sh_mb_record_put(root, "aliases", out);
                free(out);
            }
            mpj_free(aliases);
            sh_mb_record_unlock();
        }
        msy_log("MODS: pull %.16s: %s serves the same content; recorded as an alias of %.16s, nothing stored", hash,
                e->name, e->bundle.hash);
        strncpy_s(info->rel, sizeof info->rel, e->name, _TRUNCATE);
        info->already_installed = 1;
        ok = 1;
        goto done;
    }
    named = sh_mb_folder_name(root, b.id, b.version, hash, name, sizeof name);
    if (!named) { msy_cause(cause, cap, "no free folder for %s@%s in the store", b.id, b.version); goto done; }
    replace = named == 2;   /* the same bundle, damaged or unbuilt: placed again */
    strncpy_s(info->rel, sizeof info->rel, name, _TRUNCATE);
    _snprintf_s(final, sizeof final, _TRUNCATE, "%s\\%s", root, name);
    _snprintf_s(staging, sizeof staging, _TRUNCATE, "%s.staging-%lu", final, GetCurrentProcessId());
    _snprintf_s(old, sizeof old, _TRUNCATE, "%s.old-%lu", final, GetCurrentProcessId());
    wroot = sh_mb_wpath(root);
    wfinal = sh_mb_wpath(final);
    wstaging = sh_mb_wpath(staging);
    wold = sh_mb_wpath(old);
    if (!wroot || !wfinal || !wstaging || !wold) { msy_cause(cause, cap, "out of memory"); goto done; }
    if (!msy_mkdirs(root)) { msy_cause(cause, cap, "cannot create the store folder (error %lu)", GetLastError()); goto done; }
    if (msy_exists_w(wstaging, NULL, NULL)) msy_rmtree_w(wstaging, wroot, 1);
    if (!msy_mkdirs_w(wstaging)) { msy_cause(cause, cap, "cannot create %s (error %lu)", name, GetLastError()); goto done; }
    locals = listed ? msy_locals(&list, &b, &nlocals) : NULL;
    sh_ms_list_free(&list);
    listed = 0;
    /* Files are fetched without the store lock: a long download never holds
     * up a save or an export on another thread. The staging folder is this
     * process's own. */
    modsync_store_unlock();
    for (i = 0; i < b.file_count; i++) {
        char dst[MAX_PATH * 4];
        if (b.files[i].stock) { b.files[i].absent = 1; continue; }   /* never fetched: the installed game holds it */
        msy_join(dst, sizeof dst, staging, b.files[i].path);
        if (!msy_place_file(dst, &b.files[i], locals, nlocals, source, ctx, info, one, sizeof one)) {
            msy_cause(cause, cap, "%s", one);
            modsync_store_lock();
            msy_rmtree_w(wstaging, wroot, 1);
            goto done;
        }
    }
    modsync_store_lock();
    if (modsync_find_installed(hash, NULL, 0)) {   /* another thread finished it meanwhile */
        msy_rmtree_w(wstaging, wroot, 1);
        info->already_installed = 1;
        ok = 1;
        goto done;
    }
    /* mod.json: an author block from the manifest (the download has no other)
     * and the product block. */
    strcpy_s(b.origin, sizeof b.origin, SH_MB_ORIGIN_DOWNLOAD);
    text = sh_mb_modjson_text(&b, NULL, 0, &text_len);
    {
        char mj[MAX_PATH * 4];
        msy_join(mj, sizeof mj, staging, "mod.json");
        if (!text || !msy_write_file(mj, text, text_len, 0)) {
            msy_rmtree_w(wstaging, wroot, 1);
            msy_cause(cause, cap, "cannot write mod.json for %s", name);
            goto done;
        }
    }
    if (replace || msy_exists_w(wfinal, NULL, NULL)) {
        if (msy_exists_w(wold, NULL, NULL)) msy_rmtree_w(wold, wroot, 1);
        if (!MoveFileExW(wfinal, wold, 0)) {
            DWORD e = GetLastError();
            msy_rmtree_w(wstaging, wroot, 1);
            msy_cause(cause, cap, "cannot replace the damaged %s; is a file in it open? (error %lu)", name, e);
            goto done;
        }
        replace = 1;
    }
    if (!MoveFileExW(wstaging, wfinal, 0)) {
        DWORD e = GetLastError();
        if (replace) MoveFileExW(wold, wfinal, 0);
        msy_rmtree_w(wstaging, wroot, 1);
        msy_cause(cause, cap, "cannot place %s (error %lu)", name, e);
        goto done;
    }
    if (replace) msy_rmtree_w(wold, wroot, 1);
    {
        sh_ms_verify_stats vs;
        if (!sh_ms_verify(name, &vs, one, sizeof one)) {
            msy_cause(cause, cap, "installed, but verification failed: %s", one);
            goto done;
        }
    }
    sh_ms_cache_note(final, hash);
    ok = 1;
done:
    modsync_store_unlock();
    if (listed) sh_ms_list_free(&list);
    msy_locals_free(locals, nlocals);
    free(text);
    free(wroot); free(wfinal); free(wstaging); free(wold);
    sh_mb_free(&b);
    return ok;
}

/* ------------------------------------------------------------ progress -- */

typedef struct msy_prog {
    const modnet_io *outer;
    unsigned long long base, total;
} msy_prog;

static void msy_prog_cb(void *ctx, unsigned long long done, unsigned long long total)
{
    msy_prog *p = (msy_prog *)ctx;
    (void)total;
    if (p->outer && p->outer->progress) p->outer->progress(p->outer->context, p->base + done, p->total);
}

static modnet_io msy_sub_io(const modnet_io *outer, msy_prog *p)
{
    modnet_io io;
    io.cancel = outer ? outer->cancel : NULL;
    io.progress = msy_prog_cb;
    io.context = p;
    return io;
}

static void msy_absorb(modsync_stats *st, const modnet_result *r)
{
    st->bytes_up += r->sent;
    st->bytes_down += r->received;
    st->requests += r->requests;
}

static int msy_stats_fail(modsync_stats *st, int code, const char *fmt, ...)
{
    va_list ap;
    st->code = code;
    va_start(ap, fmt);
    _vsnprintf_s(st->cause, sizeof st->cause, _TRUNCATE, fmt, ap);
    va_end(ap);
    return 0;
}

/* ------------------------------------------------------------ upload ---- */

typedef struct msy_up_bundle {
    char hash[MODNET_HASH_HEX + 1];
    char *canonical;
    size_t len;
    char label[MSY_KEY_MAX];
    char (*blobs)[MODNET_HASH_HEX + 1];
    size_t nblobs;
} msy_up_bundle;

typedef struct msy_up_blob {
    char sha[MODNET_HASH_HEX + 1];
    unsigned long long size;
    char *fallback;            /* the mod folder's file holding these bytes */
} msy_up_blob;

typedef struct msy_up {
    msy_up_bundle *bundles;
    int nb;
    msy_up_blob *blobs;
    size_t nblob, capblob;
    unsigned stock_files;      /* stock pointers named: never uploaded */
} msy_up;

static void msy_up_free(msy_up *u)
{
    int i;
    size_t k;
    for (i = 0; i < u->nb; i++) { free(u->bundles[i].canonical); free(u->bundles[i].blobs); }
    free(u->bundles);
    for (k = 0; k < u->nblob; k++) free(u->blobs[k].fallback);
    free(u->blobs);
    memset(u, 0, sizeof *u);
}

static msy_up_blob *msy_up_find_blob(msy_up *u, const char *sha)
{
    size_t k;
    for (k = 0; k < u->nblob; k++) if (strcmp(u->blobs[k].sha, sha) == 0) return &u->blobs[k];
    return NULL;
}

/* Read what the upload needs from the store, under the store lock. */
static int msy_up_collect(msy_up *u, const char *const *hashes, int n, char *cause, size_t cap)
{
    sh_ms_list l;
    int i, ok = 1;
    memset(u, 0, sizeof *u);
    u->bundles = (msy_up_bundle *)calloc((size_t)n, sizeof *u->bundles);
    if (!u->bundles) return msy_cause(cause, cap, "out of memory");
    modsync_store_lock();
    if (!sh_ms_enumerate(&l)) { modsync_store_unlock(); return msy_cause(cause, cap, "the store cannot be listed"); }
    for (i = 0; i < n && ok; i++) {
        const sh_ms_entry *e = NULL;
        msy_up_bundle *ub = &u->bundles[u->nb];
        char path[MAX_PATH * 4], actual[SH_MB_HASH_HEX + 1];
        unsigned char *text;
        size_t len = 0, k;
        int dup = 0, j;
        for (j = 0; j < u->nb && !dup; j++) dup = strcmp(u->bundles[j].hash, hashes[i]) == 0;
        if (dup) continue;
        for (k = 0; k < l.count && !e; k++)
            if (sh_ms_entry_usable(&l.items[k]) && strcmp(l.items[k].bundle.hash, hashes[i]) == 0) e = &l.items[k];
        if (!e) { ok = msy_cause(cause, cap, "bundle %.16s is not installed (or failed verification)", hashes[i]); break; }
        strcpy_s(ub->hash, sizeof ub->hash, hashes[i]);
        strncpy_s(ub->label, sizeof ub->label, e->key, _TRUNCATE);
        /* The canonical manifest, derived from mod.json in memory: the exact
         * bytes whose SHA-256 is the bundle hash, the only text the store accepts. */
        text = (unsigned char *)malloc(e->bundle.canonical_len + 1);
        if (!text) { ok = msy_cause(cause, cap, "out of memory"); break; }
        memcpy(text, e->bundle.canonical, e->bundle.canonical_len);
        len = e->bundle.canonical_len;
        text[len] = 0;
        sh_mb_sha256_hex(text, len, actual);
        if (strcmp(actual, hashes[i]) != 0) { free(text); ok = msy_cause(cause, cap, "%s: the canonical manifest does not hash to the bundle", e->key); break; }
        ub->canonical = (char *)text;
        ub->len = len;
        ub->blobs = (char (*)[MODNET_HASH_HEX + 1])calloc(e->bundle.file_count ? e->bundle.file_count : 1, sizeof *ub->blobs);
        if (!ub->blobs) { ok = msy_cause(cause, cap, "out of memory"); break; }
        for (k = 0; k < e->bundle.file_count && ok; k++) {
            const sh_mb_file *f = &e->bundle.files[k];
            msy_up_blob *ob;
            if (f->stock) { u->stock_files++; continue; }   /* the store never receives installed content */
            ob = msy_up_find_blob(u, f->blob);
            strcpy_s(ub->blobs[ub->nblobs++], MODNET_HASH_HEX + 1, f->blob);
            if (ob) continue;
            if (u->nblob == u->capblob) {
                size_t c2 = u->capblob ? u->capblob * 2 : 64;
                msy_up_blob *g = (msy_up_blob *)realloc(u->blobs, sizeof *g * c2);
                if (!g) { ok = msy_cause(cause, cap, "out of memory"); break; }
                u->blobs = g;
                u->capblob = c2;
            }
            ob = &u->blobs[u->nblob++];
            memset(ob, 0, sizeof *ob);
            strcpy_s(ob->sha, sizeof ob->sha, f->blob);
            ob->size = f->size;
            msy_join(path, sizeof path, e->folder, f->path);
            ob->fallback = _strdup(path);
        }
        u->nb++;
    }
    sh_ms_list_free(&l);
    modsync_store_unlock();
    return ok;
}

static int msy_read_blob(const msy_up_blob *ob, unsigned char **bytes, size_t *len)
{
    char actual[SH_MB_HASH_HEX + 1];
    unsigned char *t = NULL;
    *bytes = NULL;
    *len = 0;
    if (ob->fallback) t = sh_mb_read_file(ob->fallback, (size_t)MODNET_MAX_BLOB, len);
    if (t) {
        sh_mb_sha256_hex(t, *len, actual);
        if (*len != ob->size || strcmp(actual, ob->sha) != 0) { free(t); t = NULL; *len = 0; }
    }
    *bytes = t;
    return t != NULL;
}

/* Upload the blobs in `want` the store reported missing. */
static int msy_send_blobs(msy_up *u, char (*want)[MODNET_HASH_HEX + 1], int nwant, const modnet_io *io,
                          msy_prog *prog, modsync_stats *st)
{
    int i;
    for (i = 0; i < nwant; i++) {
        msy_up_blob *ob = msy_up_find_blob(u, want[i]);
        unsigned char *bytes = NULL;
        size_t len = 0;
        modnet_result r;
        modnet_io sub;
        int code;
        if (msy_cancelled(io)) return msy_stats_fail(st, MODNET_E_CANCELLED, "cancelled");
        if (!ob) return msy_stats_fail(st, MODNET_E_PROTOCOL, "the store asked for blob %.16s, which no bundle names", want[i]);
        if (!msy_read_blob(ob, &bytes, &len))
            return msy_stats_fail(st, MODNET_E_ARG, "file %.16s is missing or damaged in the local store (run sh_mods doctor verify)", ob->sha);
        sub = msy_sub_io(io, prog);
        code = modnet_put_blob(ob->sha, bytes, len, &sub, &r);
        free(bytes);
        msy_absorb(st, &r);
        if (code != MODNET_OK) return msy_stats_fail(st, code, "%s", r.message);
        prog->base += len;
        st->blobs_transferred++;
    }
    return 1;
}

static int msy_upload(const char *const *hashes, int n, const char *ref, const modnet_io *io, modsync_stats *st)
{
    msy_up u;
    char (*missing)[MODNET_HASH_HEX + 1] = NULL;
    int nmissing = 0, i, code;
    const char **all = NULL;
    msy_prog prog;
    modnet_result r;
    char claim[MODNET_CLAIM_HEX + 1], secret[MODNET_HASH_HEX + 1], stored_claim[MODNET_CLAIM_HEX + 1],
         stored_secret[MODNET_HASH_HEX + 1], cause[256];
    int ok = 0, have_secret;
    size_t k;

    cause[0] = 0;
    if (!msy_up_collect(&u, hashes, n, cause, sizeof cause)) {
        msy_up_free(&u);
        return msy_stats_fail(st, MODNET_E_ARG, "%s", cause);
    }
    st->bundles = (unsigned)u.nb;
    st->blobs_total = (unsigned)u.nblob;
    st->stock_files = u.stock_files;
    memset(&prog, 0, sizeof prog);
    prog.outer = io;

    all = (const char **)malloc(sizeof(char *) * (u.nblob ? u.nblob : 1));
    if (!all) { msy_stats_fail(st, MODNET_E_NOMEM, "out of memory"); goto done; }
    for (k = 0; k < u.nblob; k++) all[k] = u.blobs[k].sha;
    code = modnet_missing(all, (int)u.nblob, &missing, &nmissing, io, &r);
    msy_absorb(st, &r);
    if (code != MODNET_OK) { msy_stats_fail(st, code, "%s", r.message); goto done; }
    st->blobs_missing = (unsigned)nmissing;
    for (i = 0; i < nmissing; i++) { msy_up_blob *ob = msy_up_find_blob(&u, missing[i]); if (ob) prog.total += ob->size; }
    for (i = 0; i < u.nb; i++) prog.total += u.bundles[i].len;
    if (!msy_send_blobs(&u, missing, nmissing, io, &prog, st)) goto done;

    for (i = 0; i < u.nb; i++) {
        msy_up_bundle *ub = &u.bundles[i];
        modnet_io sub = msy_sub_io(io, &prog);
        int attempt;
        for (attempt = 0; attempt < 2; attempt++) {
            if (msy_cancelled(io)) { msy_stats_fail(st, MODNET_E_CANCELLED, "cancelled"); goto done; }
            code = modnet_put_bundle(ub->hash, ub->canonical, ub->len, &sub, &r);
            msy_absorb(st, &r);
            if (code != MODNET_E_CONFLICT || attempt) break;
            /* The store lost a blob between negotiation and the manifest (a
             * purge, or a concurrent upload): negotiate this bundle again. */
            free(missing);
            missing = NULL;
            {
                const char **mine = (const char **)malloc(sizeof(char *) * (ub->nblobs ? ub->nblobs : 1));
                size_t j;
                if (!mine) { msy_stats_fail(st, MODNET_E_NOMEM, "out of memory"); goto done; }
                for (j = 0; j < ub->nblobs; j++) mine[j] = ub->blobs[j];
                code = modnet_missing(mine, (int)ub->nblobs, &missing, &nmissing, io, &r);
                free(mine);
            }
            msy_absorb(st, &r);
            if (code != MODNET_OK) break;
            msy_log("MODS: push %s: the store lacked %d blob(s) at the manifest; sending them again", ub->label, nmissing);
            if (!msy_send_blobs(&u, missing, nmissing, io, &prog, st)) goto done;
        }
        if (code != MODNET_OK) { msy_stats_fail(st, code, "%s: %s", ub->label, r.message); goto done; }
        if (r.created) st->bundles_created++;
        prog.base += ub->len;
    }

    /* One claim for the whole set: re-claim with the stored secret when the
     * ref has one, which replaces the set the store keeps for it. */
    {
        const char **bh = (const char **)malloc(sizeof(char *) * (size_t)u.nb);
        if (!bh) { msy_stats_fail(st, MODNET_E_NOMEM, "out of memory"); goto done; }
        for (i = 0; i < u.nb; i++) bh[i] = u.bundles[i].hash;
        have_secret = modsync_claim_lookup(ref, stored_claim, stored_secret);
        code = modnet_claim(ref, bh, u.nb, have_secret ? stored_secret : NULL, claim, secret, io, &r);
        msy_absorb(st, &r);
        if (code == MODNET_E_FORBIDDEN && have_secret) {
            msy_log("MODS: push: the stored claim for this ref is not known to the store; making a new claim");
            code = modnet_claim(ref, bh, u.nb, NULL, claim, secret, io, &r);
            msy_absorb(st, &r);
            have_secret = 0;
        }
        if (code == MODNET_OK) {
            st->reclaimed = have_secret && strcmp(claim, stored_claim) == 0;
            strcpy_s(st->claim, sizeof st->claim, claim);
            if (!msy_claim_store(ref, claim, secret, bh, u.nb)) {
                free(bh);
                SecureZeroMemory(secret, sizeof secret);
                msy_stats_fail(st, MODNET_E_ARG, "claimed as %s, but the claims section of mods\\snapmap-plus.json could not "
                               "be written; the claim cannot be released later", claim);
                goto done;
            }
        }
        free(bh);
        SecureZeroMemory(secret, sizeof secret);
        SecureZeroMemory(stored_secret, sizeof stored_secret);
        if (code != MODNET_OK) { msy_stats_fail(st, code, "%s", r.message); goto done; }
    }
    ok = 1;
done:
    free(all);
    free(missing);
    msy_up_free(&u);
    return ok;
}

int modsync_upload_bundles(const char *const *bundle_hashes, int n, const char *ref,
                           const modnet_io *io, modsync_stats *stats)
{
    modsync_stats local;
    modsync_stats *st = stats ? stats : &local;
    double t0 = msy_now_ms();
    int i, ok;
    memset(st, 0, sizeof *st);
    if (!modnet_valid_ref(ref)) return msy_stats_fail(st, MODNET_E_ARG, "the ref must be 1-64 characters of text");
    if (n <= 0 || n > MODSYNC_MAX_BUNDLES || !bundle_hashes)
        return msy_stats_fail(st, MODNET_E_ARG, "name 1 to %d bundles", MODSYNC_MAX_BUNDLES);
    for (i = 0; i < n; i++)
        if (!modnet_valid_hash(bundle_hashes[i])) return msy_stats_fail(st, MODNET_E_ARG, "bundle %d is not a 64-hex bundle hash", i + 1);
    ok = msy_upload(bundle_hashes, n, ref, io, st);
    st->ms = msy_now_ms() - t0;
    if (ok) {
        st->code = MODNET_OK;
        msy_pending_drop(ref);
        return 1;
    }
    if (modnet_transient(st->code)) st->queued = msy_pending_put(ref, bundle_hashes, n, st->cause);
    return 0;
}

/* ------------------------------------------------------------ download -- */

typedef struct msy_net_src {
    const modnet_io *io;
    msy_prog *prog;
    modsync_stats *st;
    int code;
} msy_net_src;

static int msy_net_source(void *ctx, const char *sha, unsigned long long size, unsigned char **bytes, size_t *len,
                          char *cause, size_t cap)
{
    msy_net_src *s = (msy_net_src *)ctx;
    modnet_result r;
    modnet_io sub = msy_sub_io(s->io, s->prog);
    int code = modnet_get_blob(sha, size, bytes, len, &sub, &r);
    msy_absorb(s->st, &r);
    if (code != MODNET_OK) {
        s->code = code;
        strncpy_s(cause, cap, r.message, _TRUNCATE);
        return 0;
    }
    s->prog->base += *len;
    s->st->blobs_transferred++;
    return 1;
}

int modsync_fetch_bundle(const char *hash, const modnet_io *io, modsync_stats *stats)
{
    modsync_stats local;
    modsync_stats *st = stats ? stats : &local;
    modsync_install_info info;
    modnet_result r;
    msy_prog prog;
    msy_net_src src;
    char *text = NULL, cause[300], folder[MAX_PATH * 4];
    size_t len = 0;
    double t0 = msy_now_ms();
    int code, ok;
    memset(st, 0, sizeof *st);
    if (!modnet_valid_hash(hash)) return msy_stats_fail(st, MODNET_E_ARG, "not a 64-hex bundle hash");
    st->bundles = 1;
    if (modsync_find_installed(hash, folder, sizeof folder)) {
        st->already_installed = 1;
        st->ms = msy_now_ms() - t0;
        return 1;
    }
    code = modnet_get_bundle(hash, &text, &len, io, &r);
    msy_absorb(st, &r);
    if (code != MODNET_OK) { st->ms = msy_now_ms() - t0; return msy_stats_fail(st, code, "%s", r.message); }
    {
        /* The download total: the manifest's file sizes (an upper bound; blobs
         * already in the local store are skipped). */
        sh_mb_bundle b;
        char *with = msy_with_bundle(text, len, hash);
        memset(&b, 0, sizeof b);
        memset(&prog, 0, sizeof prog);
        prog.outer = io;
        if (with && sh_mb_load_manifest_text(with, strlen(with), &b)) {
            size_t i;
            st->blobs_total = (unsigned)(b.file_count - b.stock_count);
            for (i = 0; i < b.file_count; i++) prog.total += b.files[i].size;   /* a pointer's size is 0 */
        }
        sh_mb_free(&b);
        free(with);
    }
    src.io = io;
    src.prog = &prog;
    src.st = st;
    src.code = MODNET_OK;
    ok = modsync_install_manifest(text, len, hash, msy_net_source, &src, &info, cause, sizeof cause);
    free(text);
    strncpy_s(st->key, sizeof st->key, info.rel, _TRUNCATE);
    st->blobs_missing = info.blobs_fetched;
    if (!ok) {
        st->ms = msy_now_ms() - t0;
        return msy_stats_fail(st, src.code != MODNET_OK ? src.code : MODNET_E_REFUSED, "%s", cause);
    }
    st->already_installed = info.already_installed;
    if (!info.already_installed) st->bundles_created = 1;
    if (sh_modserve_index_rescan() < 0) msy_log("MODS: pull %.16s: installed, but the serving index could not be rebuilt", hash);
    st->ms = msy_now_ms() - t0;
    st->code = MODNET_OK;
    return 1;
}

/* ------------------------------------------------------------ release --- */

int modsync_release(const char *ref, const modnet_io *io, modsync_stats *stats)
{
    modsync_stats local;
    modsync_stats *st = stats ? stats : &local;
    char claim[MODNET_CLAIM_HEX + 1], secret[MODNET_HASH_HEX + 1];
    modnet_result r;
    double t0 = msy_now_ms();
    int code;
    memset(st, 0, sizeof *st);
    if (!modnet_valid_ref(ref)) return msy_stats_fail(st, MODNET_E_ARG, "the ref must be 1-64 characters of text");
    if (!modsync_claim_lookup(ref, claim, secret)) {
        msy_pending_drop(ref);
        return msy_stats_fail(st, MODNET_E_NOT_FOUND, "no claim is stored for this ref");
    }
    strcpy_s(st->claim, sizeof st->claim, claim);
    code = modnet_release(claim, secret, io, &r);
    SecureZeroMemory(secret, sizeof secret);
    msy_absorb(st, &r);
    st->ms = msy_now_ms() - t0;
    if (code == MODNET_OK || code == MODNET_E_NOT_FOUND) {
        msy_claim_forget(ref);
        msy_pending_drop(ref);
        st->code = MODNET_OK;
        if (code == MODNET_E_NOT_FOUND) strcpy_s(st->cause, sizeof st->cause, "the store no longer knew the claim; forgotten locally");
        return 1;
    }
    return msy_stats_fail(st, code, "%s", r.message);
}

/* ------------------------------------------------------------ retry ----- */

int modsync_retry_pending(const modnet_io *io, unsigned *done, unsigned *dropped, modsync_stats *last)
{
    mpj *doc, *pend;
    int i, left;
    unsigned ok_n = 0, drop_n = 0;
    if (done) *done = 0;
    if (dropped) *dropped = 0;
    if (last) memset(last, 0, sizeof *last);
    modsync_store_lock();
    doc = msy_doc_load(MODSYNC_PENDING_SECTION, "pending");
    modsync_store_unlock();
    pend = doc ? mpj_get(doc, "pending") : NULL;
    if (!pend) { mpj_free(doc); return -1; }
    for (i = 0; i < pend->n; i++) {
        const mpj *e = pend->items[i], *arr = mpj_get(e, "bundles");
        const char *hashes[MODSYNC_MAX_BUNDLES];
        char ref[4 * MODNET_REF_MAX + 1];
        modsync_stats st;
        int n = 0, k, valid = 1;
        size_t rl = strlen(pend->keys[i]);
        if (msy_cancelled(io)) break;
        /* Keys are written escaped; a ref that needed escapes is skipped
         * rather than decoded (refs are plain text in practice). */
        if (rl >= sizeof ref || strchr(pend->keys[i], '\\')) valid = 0;
        else strcpy_s(ref, sizeof ref, pend->keys[i]);
        if (!arr || arr->type != MPJ_ARR || arr->n < 1 || arr->n > MODSYNC_MAX_BUNDLES) valid = 0;
        for (k = 0; valid && k < arr->n; k++) {
            if (arr->items[k]->type != MPJ_STR || !modnet_valid_hash(arr->items[k]->text)) valid = 0;
            else hashes[n++] = arr->items[k]->text;
        }
        if (!valid) {
            msy_log("MODS: retry: a queued upload is malformed; dropped");
            if (rl < sizeof ref && !strchr(pend->keys[i], '\\')) msy_pending_drop(pend->keys[i]);
            drop_n++;
            continue;
        }
        if (modsync_upload_bundles(hashes, n, ref, io, &st)) {
            ok_n++;
            msy_log("MODS: retry %s: uploaded %u bundle(s), %u blob(s), %llu byte(s) in %.0f ms", ref, st.bundles,
                    st.blobs_transferred, st.bytes_up, st.ms);
        } else if (!modnet_transient(st.code)) {
            msy_pending_drop(ref);
            drop_n++;
            msy_log("MODS: retry %s: dropped, the store refused it: %s", ref, st.cause);
        } else {
            msy_log("MODS: retry %s: still failing: %s", ref, st.cause);
        }
        if (last) *last = st;
    }
    mpj_free(doc);
    if (done) *done = ok_n;
    if (dropped) *dropped = drop_n;
    left = modsync_pending_count();
    return left;
}
