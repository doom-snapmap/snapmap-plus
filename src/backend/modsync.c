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
#include "modmanifest.h"
#include "modsalt.h"
#include "modzip.h"
#include "modslice.h"
#include "mp_json.h"
#include "backend_log.h"
#include "plain_reason.h"
#include "text.h"

#define MSY_PLACES "maps"   /* mods\maps\<map title>\<mod name>\: the places (ONE-SHAPE rule 4) */

/* ------------------------------------------------------------ lock ------ */

/* The store lock (modstore_lock, the mods folder's named lock) under the
 * names the mod commands and publishing call. */
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
    char *line;
    va_list ap;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    backend_log(line ? line : "MODS: a line could not be written (out of memory)");
    free(line);
}

static int msy_cause(char **cause, const char *fmt, ...)
{
    va_list ap;
    char *text;
    if (!cause) return 0;
    va_start(ap, fmt);
    text = sh_vtextf(fmt, ap);
    va_end(ap);
    free(*cause);
    *cause = text ? text : _strdup("out of memory");
    return 0;
}

void modsync_stats_free(modsync_stats *st)
{
    if (!st) return;
    free(st->folder);
    free(st->cause);
    free(st->game_source);
    st->folder = st->cause = st->game_source = NULL;
}

const char *modsync_cause(const modsync_stats *st)
{
    return st && st->cause ? st->cause : "";
}

void modsync_install_info_free(modsync_install_info *info)
{
    if (!info) return;
    free(info->rel);
    free(info->name);
    free(info->game_source);
    info->rel = info->name = info->game_source = NULL;
}

void modsync_probe_free(modsync_probe *probe)
{
    if (!probe) return;
    free(probe->cause);
    probe->cause = NULL;
}

/* Replace *dst with a copy of src (NULL clears it). */
static void msy_set(char **dst, const char *src)
{
    char *copy = src ? _strdup(src) : NULL;
    if (src && !copy) return;
    free(*dst);
    *dst = copy;
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

/* UTF-8 dir + '\' + UTF-8 relative path ('/' turned into '\'), malloc'd. */
static char *msy_join(const char *dir, const char *rel)
{
    size_t i, n;
    char *out = sh_textf("%s\\%s", dir, rel);
    if (!out) return NULL;
    n = strlen(out);
    for (i = strlen(dir); i < n; i++) if (out[i] == '/') out[i] = '\\';
    return out;
}

static int msy_exists_w(const wchar_t *w, unsigned long long *size, int *is_dir)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(w, GetFileExInfoStandard, &d)) return 0;
    if (size) *size = ((unsigned long long)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    if (is_dir) *is_dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return 1;
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
    /* Any depth: folders wait on the heap, each removed after what it holds
     * (the list is emptied from its end). */
    wchar_t **dirs = NULL;
    size_t rn = wcslen(root), pn = wcslen(path), n = 0, cap = 0, next = 0;
    int ok = 1;
    if (pn <= rn + 1 || _wcsnicmp(path, root, rn) != 0 || path[rn] != L'\\') return 0;
    if (top) {
        const wchar_t *leaf = wcsrchr(path, L'\\');
        if (!leaf || (!wcsstr(leaf, L".staging-") && !wcsstr(leaf, L".old-"))) return 0;
    }
    if (!(dirs = (wchar_t **)malloc(sizeof *dirs * 16)) || !(dirs[0] = _wcsdup(path))) { free(dirs); return 0; }
    n = 1;
    cap = 16;
    while (next < n) {
        WIN32_FIND_DATAW fd;
        const wchar_t *dir = dirs[next++];
        wchar_t *pattern = sh_wtextf(L"%s\\*", dir);
        HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0)
                           : INVALID_HANDLE_VALUE;
        free(pattern);
        while (h != INVALID_HANDLE_VALUE) {
            if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) {
                wchar_t *child = sh_wtextf(L"%s\\%s", dir, fd.cFileName);
                if (!child) ok = 0;
                else if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                    if (n == cap) {
                        wchar_t **g = (wchar_t **)realloc(dirs, sizeof *dirs * cap * 2);
                        if (g) { dirs = g; cap *= 2; }
                    }
                    if (n < cap) { dirs[n++] = child; child = NULL; }
                    else ok = 0;
                } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    if (!RemoveDirectoryW(child)) ok = 0;
                } else if (!msy_delete_file_w(child)) {
                    ok = 0;
                }
                free(child);
            }
            if (!FindNextFileW(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
        }
    }
    while (n) {
        n--;
        if (!RemoveDirectoryW(dirs[n]) && GetLastError() != ERROR_FILE_NOT_FOUND) ok = 0;
        free(dirs[n]);
    }
    free(dirs);
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

/* Printable ASCII without quote or backslash (stored error text), whole (malloc'd). */
static char *msy_plain(const char *in)
{
    size_t w = 0;
    char *out = (char *)malloc((in ? strlen(in) : 0) + 1);
    if (!out) return NULL;
    for (; in && *in; in++) {
        unsigned char c = (unsigned char)*in;
        if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\') out[w++] = (char)c;
    }
    out[w] = 0;
    return out;
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

static const char *msy_root(void)
{
    return sh_ms_root();
}

/* ------------------------------------------------------------ docs ------ */

/* A section of the product record (mods\snapmap-plus.json) as the document
 * {"format":1,"<member>":{...}} the code below edits. A missing section is an
 * empty one. A record that cannot be read loads as empty too, and the record
 * layer then refuses the section's save until it reads again (modbundle.h),
 * so the claims it holds are never replaced with fewer. */
static mpj *msy_doc_load(const char *section, const char *member)
{
    const char *root = msy_root();
    char *text = NULL;
    mpj *doc, *m = NULL;
    if (!root) return NULL;
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
    const char *root = msy_root();
    const mpj *m = doc ? mpj_get(doc, section) : NULL;
    size_t len = 0;
    char *text;
    int ok;
    if (!m || !root) return 0;
    text = mpj_dump(m, &len);
    if (!text) return 0;
    ok = sh_mb_record_put(root, section, text);
    SecureZeroMemory(text, len);
    free(text);
    return ok;
}

static mpj *msy_hash_array(const char *const *items, int n)
{
    mpj *arr = mpj_new(MPJ_ARR);
    int i;
    if (!arr) return NULL;
    for (i = 0; i < n; i++)
        if (!mpj_push(arr, mpj_new_str(items[i]))) { mpj_free(arr); return NULL; }
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
    if (e && mpj_set(e, "builds", msy_hash_array(hashes, n)) && mpj_set(e, "claim", mpj_new_str(claim)) &&
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

/* Queue ref in the pending section: an upload of builds ("<salt>@<hash>"),
 * or (n == 0) the release of its claim. A release replaces a queued upload of the same ref
 * (the claim is no longer wanted); the provisional ref an item's upload
 * waits on is kept across attempts. */
static void msy_retry_releases(const modnet_io *io);

static int msy_pending_put(const char *ref, const char *const *mods, int n, const char *error)
{
    mpj *doc, *pend, *e, *old;
    const mpj *att, *prov;
    char *key = msy_escape(ref), *plain = msy_plain(error);
    long long attempts = 1;
    int ok = 0;
    if (!key || !plain) { free(key); free(plain); return 0; }
    modsync_store_lock();
    doc = msy_doc_load(MODSYNC_PENDING_SECTION, "pending");
    pend = doc ? mpj_get(doc, "pending") : NULL;
    old = pend ? mpj_get(pend, key) : NULL;
    att = old ? mpj_get(old, "attempts") : NULL;
    if (att && mpj_is_num(att)) attempts = mpj_int(att) + 1;
    prov = old && n > 0 ? mpj_get(old, "provisional") : NULL;
    e = pend ? mpj_new(MPJ_OBJ) : NULL;
    if (e && mpj_set(e, "attempts", mpj_new_int(attempts)) &&
        (n > 0 ? mpj_set(e, "builds", msy_hash_array(mods, n)) : mpj_set(e, "kind", mpj_new_str("release"))) &&
        mpj_set(e, "lastError", mpj_new_str(plain)) && mpj_set(e, "queuedAt", mpj_new_int((long long)time(NULL))) &&
        (!prov || prov->type != MPJ_STR || mpj_set(e, "provisional", mpj_clone(prov)))) {
        if (mpj_set(pend, key, e)) { e = NULL; ok = msy_doc_save(MODSYNC_PENDING_SECTION, doc); }
    }
    mpj_free(e);
    mpj_free(doc);
    modsync_store_unlock();
    free(key);
    free(plain);
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

void modsync_pending_drop(const char *ref)
{
    if (ref && ref[0]) msy_pending_drop(ref);
}

int modsync_pending_link(const char *ref, const char *provisional)
{
    mpj *doc, *pend, *e;
    char *key = ref ? msy_escape(ref) : NULL;
    int ok = 0;
    if (!key || !provisional || !modnet_valid_ref(provisional)) { free(key); return 0; }
    modsync_store_lock();
    doc = msy_doc_load(MODSYNC_PENDING_SECTION, "pending");
    pend = doc ? mpj_get(doc, "pending") : NULL;
    e = pend ? mpj_get(pend, key) : NULL;
    if (e && e->type == MPJ_OBJ && mpj_get(e, "builds") && mpj_set(e, "provisional", mpj_new_str(provisional)))
        ok = msy_doc_save(MODSYNC_PENDING_SECTION, doc);
    mpj_free(doc);
    modsync_store_unlock();
    free(key);
    return ok;
}

int modsync_pending_waits_on(const char *provisional)
{
    mpj *doc, *pend;
    int i, waits = 0;
    if (!provisional || !provisional[0]) return 0;
    modsync_store_lock();
    doc = msy_doc_load(MODSYNC_PENDING_SECTION, "pending");
    modsync_store_unlock();
    pend = doc ? mpj_get(doc, "pending") : NULL;
    for (i = 0; pend && i < pend->n && !waits; i++) {
        const mpj *p = mpj_get(pend->items[i], "provisional");
        waits = p && p->type == MPJ_STR && !strcmp(p->text, provisional);
    }
    mpj_free(doc);
    return waits;
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

/* The folder `full` (absolute) under the store root `root`, as its name under mods\ (malloc'd). */
static char *msy_rel_of(const char *root, const char *full)
{
    size_t rl;
    if (!strncmp(root, "\\\\?\\", 4)) root += 4;   /* either may carry the long-path prefix */
    if (!strncmp(full, "\\\\?\\", 4)) full += 4;
    rl = strlen(root);
    while (rl && (root[rl - 1] == '\\' || root[rl - 1] == '/')) rl--;
    if (!_strnicmp(full, root, rl) && (full[rl] == '\\' || full[rl] == '/')) return _strdup(full + rl + 1);
    return _strdup(strrchr(full, '\\') ? strrchr(full, '\\') + 1 : full);
}

/* ------------------------------------------------------------ builds ---- */

int modsync_parse_mod(const char *mod, char salt_out[MODSYNC_SALT_HEX + 1], char hash_out[MODNET_HASH_HEX + 1])
{
    if (salt_out) salt_out[0] = 0;
    if (hash_out) hash_out[0] = 0;
    if (!mod || strlen(mod) != MODSYNC_MOD_MAX || mod[SH_SALT_LEN] != '@') return 0;
    if (!sh_salt_valid(mod, SH_SALT_LEN) || !modnet_valid_hash(mod + SH_SALT_LEN + 1)) return 0;
    if (salt_out) { memcpy(salt_out, mod, SH_SALT_LEN); salt_out[SH_SALT_LEN] = 0; }
    if (hash_out) strcpy_s(hash_out, MODNET_HASH_HEX + 1, mod + SH_SALT_LEN + 1);
    return 1;
}

/* A folder in a map's place: its name under mods\ starts with "maps\". */
static int msy_in_places(const sh_ms_entry *e)
{
    size_t n = strlen(MSY_PLACES);
    return !_strnicmp(e->name, MSY_PLACES, n) && (e->name[n] == '\\' || e->name[n] == '/');
}

/* The same for an absolute folder under the store root. */
static int msy_in_places_rel(const char *root, const char *full)
{
    char *rel = msy_rel_of(root, full);
    size_t n = strlen(MSY_PLACES);
    int in = rel && !_strnicmp(rel, MSY_PLACES, n) && (rel[n] == '\\' || rel[n] == '/');
    free(rel);
    return in;
}

static int msy_entry_is(const sh_ms_entry *e, const char *salt, const char *hash)
{
    return sh_ms_entry_usable(e) && !strcmp(e->bundle.id, salt) && (!hash || !strcmp(e->bundle.hash, hash));
}

/* The place's folder of the salt (its name under mods\, malloc'd into
 * *rel), when it has one. */
static int msy_place_has(const sh_ms_place *place, const char *salt, char **rel)
{
    *rel = NULL;
    if (place && sh_ms_place_folder(place, "mod", salt, rel) == 2) return 1;
    free(*rel);
    *rel = NULL;
    return 0;
}

/* The entry serving salt at hash: a root folder, else the place's folder of
 * the salt (place NULL: any place). Under the store lock. */
static const sh_ms_entry *msy_locate(const sh_ms_list *l, const char *salt, const char *hash, const sh_ms_place *place)
{
    char *rel = NULL;
    int have_rel = 0;
    size_t i;
    const sh_ms_entry *hit = NULL;
    for (i = 0; i < l->count; i++)
        if (!msy_in_places(&l->items[i]) && msy_entry_is(&l->items[i], salt, hash)) return &l->items[i];
    if (place) have_rel = msy_place_has(place, salt, &rel);
    if (place && !have_rel) return NULL;
    for (i = 0; i < l->count && !hit; i++) {
        const sh_ms_entry *e = &l->items[i];
        if (!msy_in_places(e) || !msy_entry_is(e, salt, hash)) continue;
        if (!have_rel || !_stricmp(e->name, rel)) hit = e;
    }
    free(rel);
    return hit;
}

int modsync_find_installed(const char *salt, const char *hash, const sh_ms_place *place, char **folder)
{
    sh_ms_list l;
    int found = 0;
    if (folder) *folder = NULL;
    if (!salt || !sh_salt_valid(salt, strlen(salt)) || !modnet_valid_hash(hash)) return 0;
    modsync_store_lock();
    if (sh_ms_enumerate(&l)) {
        const sh_ms_entry *e = msy_locate(&l, salt, hash, place);
        if (e) {
            found = 1;
            if (folder && !(*folder = _strdup(e->folder))) found = 0;
        }
        sh_ms_list_free(&l);
    }
    modsync_store_unlock();
    return found;
}

/* The temporary folder, UTF-8 with its trailing separator (malloc'd), at
 * whatever length Windows reports it. */
static char *msy_temp_dir(void)
{
    DWORD need = GetTempPathW(0, NULL), n;
    wchar_t *dir;
    char *u;
    if (!need || !(dir = (wchar_t *)malloc(sizeof(wchar_t) * need))) return NULL;
    n = GetTempPathW(need, dir);
    u = n && n < need ? sh_mb_utf8(dir) : NULL;
    free(dir);
    return u;
}

/* A temporary file for one archive: %TEMP%\snapmap-plus-<hash16>-<pid>-<n>.zip (malloc'd). */
static char *msy_temp_archive(const char *hash)
{
    static volatile long counter;
    char *udir = msy_temp_dir(), *out;
    if (!udir) return NULL;
    out = sh_textf("%ssnapmap-plus-%.16s-%lu-%ld.zip", udir, hash, GetCurrentProcessId(), InterlockedIncrement(&counter));
    free(udir);
    return out;
}

static void msy_delete_utf8(const char *path)
{
    wchar_t *w = sh_mb_wpath(path);
    if (w) { SetFileAttributesW(w, FILE_ATTRIBUTE_NORMAL); DeleteFileW(w); free(w); }
}

static unsigned msy_mb(unsigned long long bytes)
{
    return (unsigned)((bytes + 1024u * 1024u - 1) / (1024u * 1024u));
}

/* ------------------------------------------------------------ part of a mod */

static sh_mb_exists_fn volatile g_cut_stock;
static void *volatile g_cut_stock_ctx;

void modsync_set_stock(sh_mb_exists_fn stock, void *ctx)
{
    g_cut_stock_ctx = ctx;
    g_cut_stock = stock;
}

/* <root>\.slices\<name> (malloc'd), or NULL. */
static char *msy_slice_dir(const char *name)
{
    const char *root = msy_root();
    return root ? sh_textf("%s\\" MODSYNC_SLICES "\\%s", root, name) : NULL;
}

/* The cut at `hash` staged for an upload: 1 with its folder (malloc'd) and
 * its name for lines, when its mod.json names that build of `salt`. */
static int msy_slice_find(const char *salt, const char *hash, char **folder, char **label)
{
    sh_mb_bundle b;
    char *dir = msy_slice_dir(hash);
    int ok;
    *folder = *label = NULL;
    if (!dir) return 0;
    ok = sh_mb_load_folder(dir, &b) && !strcmp(b.id, salt) && !strcmp(b.hash, hash) && b.slice_of[0];
    if (ok) {
        *label = _strdup(b.name && b.name[0] ? b.name : b.id);
        *folder = dir;
        dir = NULL;
        if (!*label) { free(*folder); *folder = NULL; ok = 0; }
    }
    sh_mb_free(&b);
    free(dir);
    return ok;
}

/* Remove the folder `dir` under .slices\, when it is there. */
static void msy_stage_drop(const char *dir)
{
    wchar_t *w = dir ? sh_mb_wpath(dir) : NULL, *top = NULL;
    if (w && (top = _wcsdup(w)) != NULL) {
        wchar_t *slash = wcsrchr(top, L'\\');
        if (slash) *slash = 0;   /* .slices: what the removal stays inside */
        if (msy_exists_w(w, NULL, NULL)) msy_rmtree_w(w, top, 0);
    }
    free(top);
    free(w);
}

/* Remove the staged cut at `hash`, when there is one. */
static void msy_slice_drop(const char *hash)
{
    char *dir = msy_slice_dir(hash);
    msy_stage_drop(dir);
    free(dir);
}

/* Byte counts as the publish lines print them. */
static void msy_size_text(char *out, size_t cap, unsigned long long bytes)
{
    if (bytes < 1024ull * 100ull) _snprintf_s(out, cap, _TRUNCATE, "%llu byte%s", bytes, bytes == 1 ? "" : "s");
    else _snprintf_s(out, cap, _TRUNCATE, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
}

/* The cuts this process staged and has not uploaded yet: a publish of this
 * session may still send them, so the tidy keeps them. */
static SRWLOCK g_staged_lock = SRWLOCK_INIT;
static char (*g_staged)[MODNET_HASH_HEX + 1];
static size_t g_nstaged;

static void msy_staged(const char *hash, int add)
{
    size_t i;
    AcquireSRWLockExclusive(&g_staged_lock);
    for (i = 0; i < g_nstaged && strcmp(g_staged[i], hash); i++) {}
    if (add && i == g_nstaged) {
        char (*grown)[MODNET_HASH_HEX + 1] = realloc(g_staged, (g_nstaged + 1) * sizeof *g_staged);
        if (grown) { g_staged = grown; strcpy_s(g_staged[g_nstaged++], MODNET_HASH_HEX + 1, hash); }
    } else if (!add && i < g_nstaged) {
        memmove(&g_staged[i], &g_staged[i + 1], (g_nstaged - i - 1) * sizeof *g_staged);
        g_nstaged--;
    }
    ReleaseSRWLockExclusive(&g_staged_lock);
}

static int msy_staged_here(const char *hash)
{
    size_t i;
    int hit = 0;
    AcquireSRWLockShared(&g_staged_lock);
    for (i = 0; i < g_nstaged && !hit; i++) hit = !strcmp(g_staged[i], hash);
    ReleaseSRWLockShared(&g_staged_lock);
    return hit;
}

static int msy_cut_stock(void *ctx, const char *request)
{
    sh_mb_exists_fn fn = g_cut_stock;
    (void)ctx;
    return fn ? fn(g_cut_stock_ctx, request) : -1;
}

int modsync_cut_row(sh_modmanifest_row *row, const char *const *asked, size_t asked_count, char **why)
{
    sh_ms_list l;
    const sh_ms_entry *e;
    sh_mb_bundle src, cut;
    sh_slice_plan plan;
    sh_mb_resolver stock;
    char *folder = NULL, *stage = NULL, *final = NULL, *reason = NULL, size_cut[32], size_all[32];
    const char *label;
    static volatile long counter;
    int ok = 0, loaded = 0;
    if (why) *why = NULL;
    memset(&plan, 0, sizeof plan);
    memset(&src, 0, sizeof src);
    memset(&cut, 0, sizeof cut);
    if (!row || !sh_salt_valid(row->salt, strlen(row->salt)) || !modnet_valid_hash(row->build)) return 0;
    if (!g_cut_stock) { reason = sh_text("the game's files cannot be read"); goto out; }
    modsync_store_lock();
    memset(&l, 0, sizeof l);
    if (sh_ms_enumerate(&l) && (e = msy_locate(&l, row->salt, row->build, NULL)) != NULL && !msy_in_places(e))
        folder = _strdup(e->folder);
    sh_ms_list_free(&l);
    if (!folder) { reason = sh_text("no folder at the top of the mods folder holds that build"); goto unlock; }
    loaded = sh_mb_load_folder(folder, &src) && !strcmp(src.hash, row->build);
    if (!loaded) { reason = sh_text("its folder was built again since the save began"); goto unlock; }
    if (!sh_slice_needed(&src, folder, (const char *const *)row->names, row->name_count, row->attached, asked,
                         asked_count, &plan)) {
        reason = sh_text("out of memory");
        goto unlock;
    }
    if (plan.whole) { reason = sh_text(plan.why ? plan.why : ""); goto unlock; }
    {
        char name[64];
        _snprintf_s(name, sizeof name, _TRUNCATE, "%lu-%ld.staging", GetCurrentProcessId(), InterlockedIncrement(&counter));
        stage = msy_slice_dir(name);
    }
    memset(&stock, 0, sizeof stock);
    stock.stock = msy_cut_stock;
    if (!stage || !sh_slice_cut(&src, folder, &plan, stage, &stock, &cut, &reason)) {
        if (!reason) reason = sh_text("out of memory");
        goto unlock;
    }
    {
        wchar_t *ws = sh_mb_wpath(stage), *wf = (final = msy_slice_dir(cut.hash)) ? sh_mb_wpath(final) : NULL;
        /* Another map's publish of this session staged these very files: one folder serves both. */
        int same = wf && msy_exists_w(wf, NULL, NULL);
        int kept = same || (ws && wf && MoveFileExW(ws, wf, 0));
        free(ws);
        free(wf);
        if (!kept || same) msy_stage_drop(stage);
        if (!kept) { reason = sh_text("its files could not be kept in mods\\" MODSYNC_SLICES); goto unlock; }
    }
    msy_staged(cut.hash, 1);
    strcpy_s(row->of, sizeof row->of, row->build);
    strcpy_s(row->build, sizeof row->build, cut.hash);
    row->size = cut.total_size;
    ok = 1;
unlock:
    modsync_store_unlock();
out:
    label = row->name && row->name[0] ? row->name : loaded && src.name ? src.name : row->salt;
    if (ok) {
        msy_size_text(size_cut, sizeof size_cut, plan.kept_bytes);
        msy_size_text(size_all, sizeof size_all, plan.bytes);
        msy_log("MODS: publish: %s: the map uses %zu of its %zu files (%s of %s); those go with the map (%.16s)", label,
                plan.kept_files, plan.files, size_cut, size_all, row->build);
    } else {
        msy_log("MODS: publish: %s goes with the map whole: %s", label, reason ? reason : "");
    }
    if (why && !ok) { *why = reason; reason = NULL; }
    free(reason);
    free(final);
    free(stage);
    free(folder);
    sh_slice_plan_free(&plan);
    sh_mb_free(&cut);
    sh_mb_free(&src);
    return ok;
}

/* The hashes every queued upload names, as "<salt>@<hash>" texts. */
static int msy_pending_names(const char *hash)
{
    mpj *doc, *pend;
    int i, k, named = 0;
    doc = msy_doc_load(MODSYNC_PENDING_SECTION, "pending");
    pend = doc ? mpj_get(doc, "pending") : NULL;
    for (i = 0; pend && i < pend->n && !named; i++) {
        const mpj *arr = mpj_get(pend->items[i], "builds");
        for (k = 0; arr && arr->type == MPJ_ARR && k < arr->n && !named; k++)
            named = arr->items[k]->type == MPJ_STR && strlen(arr->items[k]->text) > MODSYNC_SALT_HEX + 1 &&
                    !strcmp(arr->items[k]->text + MODSYNC_SALT_HEX + 1, hash);
    }
    mpj_free(doc);
    return named;
}

int modsync_slices_tidy(void)
{
    char *dir = msy_slice_dir("*");
    wchar_t *pattern = dir ? sh_mb_wpath(dir) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int removed = 0;
    free(dir);
    if (!pattern) return 0;
    modsync_store_lock();
    h = FindFirstFileW(pattern, &fd);
    while (h != INVALID_HANDLE_VALUE) {
        char *name = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && wcscmp(fd.cFileName, L".") &&
                     wcscmp(fd.cFileName, L"..") ? sh_mb_utf8(fd.cFileName) : NULL;
        if (name && (!modnet_valid_hash(name) || (!msy_pending_names(name) && !msy_staged_here(name)))) {
            msy_slice_drop(name);
            removed++;
        }
        free(name);
        if (!FindNextFileW(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
    }
    modsync_store_unlock();
    free(pattern);
    if (removed) msy_log("MODS: %d folder(s) of mods\\" MODSYNC_SLICES " no upload waits on were removed", removed);
    return removed;
}

/* ------------------------------------------------------------ asked ----- */

int modsync_asked_refresh(const modnet_io *io)
{
    mpj *claims_doc, *claims, *asked, *doc = NULL;
    int i, entries = 0, reached = 1;
    modsync_store_lock();
    claims_doc = msy_doc_load(MODSYNC_CLAIMS_SECTION, "claims");
    modsync_store_unlock();
    claims = claims_doc ? mpj_get(claims_doc, "claims") : NULL;
    asked = mpj_new(MPJ_OBJ);
    if (!claims || !asked) { mpj_free(claims_doc); mpj_free(asked); return -1; }
    for (i = 0; i < claims->n && reached; i++) {
        const mpj *c = mpj_get(claims->items[i], "claim"), *sec = mpj_get(claims->items[i], "secret");
        modnet_result r;
        char *json = NULL;
        mpj *answer, *list;
        int code;
        if (!c || c->type != MPJ_STR || !sec || sec->type != MPJ_STR) continue;
        code = modnet_list_asked(c->text, sec->text, &json, io, &r);
        if (code != MODNET_OK) {
            if (modnet_unreachable(code) || code == MODNET_E_CANCELLED) reached = 0;
            continue;   /* a claim the store no longer knows asks nothing */
        }
        answer = json ? mpj_parse(json, strlen(json)) : NULL;
        free(json);
        list = answer && answer->type == MPJ_OBJ ? mpj_get(answer, "asked") : NULL;
        if (list && list->type == MPJ_ARR && list->n) {
            mpj *kept = mpj_new(MPJ_ARR);
            int k;
            for (k = 0; kept && k < list->n; k++) {
                const mpj *e = list->items[k], *salt = mpj_get(e, "salt"), *build = mpj_get(e, "build"),
                          *req = mpj_get(e, "request"), *by = mpj_get(e, "by");
                mpj *one;
                if (!salt || salt->type != MPJ_STR || !sh_salt_valid(salt->text, strlen(salt->text)) || !build ||
                    build->type != MPJ_STR || !modnet_valid_hash(build->text) || !req || req->type != MPJ_STR ||
                    !req->text[0])
                    continue;
                one = mpj_new(MPJ_OBJ);
                if (!one || !mpj_set(one, "salt", mpj_new_str(salt->text)) || !mpj_set(one, "build", mpj_new_str(build->text)) ||
                    !mpj_set(one, "request", mpj_new_str(req->text)) ||
                    !mpj_set(one, "by", mpj_new_str(by && by->type == MPJ_STR ? by->text : ""))) {
                    mpj_free(one);
                    continue;
                }
                if (mpj_push(kept, one)) entries++;   /* the array owns it either way */
            }
            if (kept && kept->n) (void)mpj_set(asked, claims->keys[i], kept);
            else mpj_free(kept);
        }
        mpj_free(answer);
    }
    if (claims_doc) {
        for (i = 0; i < claims->n; i++) {
            const mpj *sec = mpj_get(claims->items[i], "secret");
            if (sec && sec->type == MPJ_STR && sec->text) SecureZeroMemory(sec->text, strlen(sec->text));
        }
        mpj_free(claims_doc);
    }
    if (!reached) { mpj_free(asked); return -1; }
    doc = mpj_new(MPJ_OBJ);
    if (!doc || !mpj_set(doc, MODSYNC_ASKED_SECTION, asked)) { mpj_free(doc); return -1; }   /* asked is freed */
    modsync_store_lock();
    if (!msy_doc_save(MODSYNC_ASKED_SECTION, doc)) entries = -1;
    modsync_store_unlock();
    mpj_free(doc);
    if (entries > 0) msy_log("MODS: players asked for %d file(s) your published maps did not carry", entries);
    return entries;
}

void modsync_asked_free(char **list, size_t n)
{
    size_t i;
    for (i = 0; list && i < n; i++) free(list[i]);
    free(list);
}

/* Every entry of the section passed to `fn`, with the ref it is under. */
typedef int (*msy_asked_fn)(void *ctx, const char *ref, const mpj *entry);
static void msy_asked_each(msy_asked_fn fn, void *ctx)
{
    mpj *doc;
    const mpj *all;
    int i, k;
    modsync_store_lock();
    doc = msy_doc_load(MODSYNC_ASKED_SECTION, "asked");
    modsync_store_unlock();
    all = doc ? mpj_get(doc, "asked") : NULL;
    for (i = 0; all && i < all->n; i++)
        for (k = 0; all->items[i]->type == MPJ_ARR && k < all->items[i]->n; k++)
            if (!fn(ctx, all->keys[i], all->items[i]->items[k])) { mpj_free(doc); return; }
    mpj_free(doc);
}

typedef struct msy_asked_list { const char *match; char **v; size_t n; int oom; int by_ref; } msy_asked_list;

static int msy_asked_take(void *ctx, const char *ref, const mpj *e)
{
    msy_asked_list *l = (msy_asked_list *)ctx;
    const mpj *salt = mpj_get(e, "salt"), *req = mpj_get(e, "request"), *by = mpj_get(e, "by");
    char *line, **grown;
    size_t i;
    if (!salt || salt->type != MPJ_STR || !req || req->type != MPJ_STR) return 1;
    if (l->by_ref ? strcmp(ref, l->match) != 0 : strcmp(salt->text, l->match) != 0) return 1;
    line = l->by_ref ? sh_textf("%s\t%s\t%s", salt->text, req->text, by && by->type == MPJ_STR ? by->text : "")
                     : sh_text(req->text);
    if (!line) { l->oom = 1; return 0; }
    for (i = 0; i < l->n; i++) if (!strcmp(l->v[i], line)) { free(line); return 1; }
    grown = (char **)realloc(l->v, (l->n + 1) * sizeof *grown);
    if (!grown) { free(line); l->oom = 1; return 0; }
    l->v = grown;
    l->v[l->n++] = line;
    return 1;
}

static size_t msy_asked_collect(const char *match, int by_ref, char ***out)
{
    msy_asked_list l;
    char *key = by_ref ? msy_escape(match) : NULL;
    memset(&l, 0, sizeof l);
    *out = NULL;
    if (!match || (by_ref && !key)) return 0;
    l.match = by_ref ? key : match;
    l.by_ref = by_ref;
    msy_asked_each(msy_asked_take, &l);
    free(key);
    *out = l.v;
    return l.n;
}

size_t modsync_asked_for(const char *salt, char ***out) { return msy_asked_collect(salt, 0, out); }
size_t modsync_asked_of_ref(const char *ref, char ***out) { return msy_asked_collect(ref, 1, out); }

/* ------------------------------------------------------------ archive --- */

int modsync_write_archive(const char *folder, const char *hash, const char *out_path, unsigned long long *size,
                          unsigned *pointers, int *code, char **cause)
{
    char *mj = NULL, actual[SH_MB_HASH_HEX + 1];
    unsigned char *text = NULL, *source = NULL;
    size_t text_len = 0, source_len = 0, i;
    sh_mb_bundle b;
    sh_zip_writer w;
    wchar_t *wout = NULL;
    const char *label;
    int ok = 0, opened = 0;
    if (size) *size = 0;
    if (pointers) *pointers = 0;
    if (code) *code = MODNET_E_ARG;
    memset(&b, 0, sizeof b);
    memset(&w, 0, sizeof w);
    w.h = INVALID_HANDLE_VALUE;
    if (!folder || !modnet_valid_hash(hash) || !out_path) return msy_cause(cause, "bad argument");
    if (!(mj = msy_join(folder, "mod.json"))) return msy_cause(cause, "out of memory");
    text = sh_mb_read_file(mj, (size_t)-1, &text_len);
    if (!text) { msy_cause(cause, "%s cannot be read: %s", mj, sh_reason_win32(GetLastError(), mj, 0)); goto done; }
    if (!sh_mb_load_text((const char *)text, text_len, &b)) {
        msy_cause(cause, "%s is not a built mod: %s", mj, sh_mb_first_cause(&b));
        goto done;
    }
    label = b.name && b.name[0] ? b.name : b.id;
    if (strcmp(b.hash, hash) != 0) {
        msy_cause(cause, "%s was built again since the map was saved; save the map again, then publish it", label);
        goto done;
    }
    if (!b.validated) {
        msy_cause(cause, "%s does not build: %s", label, sh_mb_state_cause(&b)[0] ? sh_mb_state_cause(&b) : "see the Mods tab");
        goto done;
    }
    for (i = 0; i < b.file_count; i++) if (b.files[i].stock && pointers) (*pointers)++;
    if (b.slice_of[0]) {
        /* Part of a build: the source build's canonical manifest goes with it,
         * so the store can check that every file is the source's own. */
        char *sp = msy_join(folder, SH_MB_SLICE_SOURCE);
        source = sp ? sh_mb_read_file(sp, (size_t)-1, &source_len) : NULL;
        free(sp);
        if (source) sh_mb_sha256_hex(source, source_len, actual);
        if (!source || strcmp(actual, b.slice_of) != 0) {
            msy_cause(cause, "%s: the files this map uses of it were not kept whole; save the map again, then publish it",
                      label);
            goto done;
        }
    }
    wout = sh_mb_wpath(out_path);
    if (!wout || !sh_zip_writer_open(&w, wout)) {
        msy_cause(cause, "cannot write the archive %s: %s", out_path, sh_reason_win32(GetLastError(), out_path, 0));
        goto done;
    }
    opened = 1;
    if (!sh_zip_writer_add(&w, "mod.json", text, text_len)) { msy_cause(cause, "%s", w.cause); goto done; }
    if (source && !sh_zip_writer_add(&w, SH_MB_SLICE_SOURCE, source, source_len)) { msy_cause(cause, "%s", w.cause); goto done; }
    for (i = 0; i < b.file_count; i++) {
        const sh_mb_file *f = &b.files[i];
        char *path, *entry;
        unsigned char *bytes;
        size_t len = 0;
        if (f->stock) continue;   /* the player's own game holds it */
        path = msy_join(folder, f->path);
        bytes = path ? sh_mb_read_file(path, (size_t)-1, &len) : NULL;
        free(path);
        if (!bytes) {
            msy_cause(cause, "%s: %s cannot be read; open the Mods tab and Refresh", label, f->path);
            goto done;
        }
        sh_mb_sha256_hex(bytes, len, actual);
        if (len != f->size || strcmp(actual, f->blob) != 0) {
            free(bytes);
            msy_cause(cause, "%s: %s changed since the mod was built; open the Mods tab and Refresh", label, f->path);
            goto done;
        }
        entry = sh_textf("assets/%s", f->request);
        ok = entry && sh_zip_writer_add(&w, entry, bytes, len);
        free(bytes);
        free(entry);
        if (!ok) { msy_cause(cause, "%s", w.cause ? w.cause : "out of memory"); goto done; }
        ok = 0;
    }
    if (!sh_zip_writer_finish(&w) || !FlushFileBuffers(w.h)) {
        msy_cause(cause, "cannot write the archive %s: %s", out_path, w.cause ? w.cause : "flush failed");
        goto done;
    }
    if (size) *size = w.off;
    if (code) *code = MODNET_OK;
    ok = 1;
done:
    sh_zip_writer_free(&w);
    if (!ok && opened && wout) DeleteFileW(wout);
    free(wout);
    free(text);
    free(source);
    free(mj);
    sh_mb_free(&b);
    return ok;
}

/* ------------------------------------------------------------ install --- */

#define MSY_MISMATCH "The downloaded mod was damaged on the way; try again, and if it happens again ask the map's " \
                     "author for the map's .zip"

/* The archive's mod.json, read and loaded. 1 with b filled (release with
 * sh_mb_free) and *text (malloc'd) the file; 0 with the cause. */
static int msy_archive_modjson(const sh_zip_reader *zr, sh_mb_bundle *b, unsigned char **text, size_t *len,
                               char **cause)
{
    char *why = NULL;
    long idx = sh_zip_reader_find(zr, "mod.json");
    *text = NULL;
    *len = 0;
    memset(b, 0, sizeof *b);
    if (idx < 0) return msy_cause(cause, MSY_MISMATCH " (it has no mod.json)");
    *text = sh_zip_reader_read(zr, idx, (size_t)-1, len, &why);
    if (!*text) {
        msy_cause(cause, MSY_MISMATCH " (mod.json: %s)", why ? why : "unreadable");
        free(why);
        return 0;
    }
    if (!sh_mb_load_text((const char *)*text, *len, b)) {
        msy_cause(cause, MSY_MISMATCH " (mod.json: %s)", sh_mb_first_cause(b));
        free(*text);
        *text = NULL;
        sh_mb_free(b);
        return 0;
    }
    return 1;
}

/* The archive is exactly the build: salt, hash, and one entry per carried
 * file besides mod.json; part of a build (`of`, else any) also carries the
 * source build's canonical manifest, hashing to the build it names. */
static int msy_archive_matches(const sh_zip_reader *zr, const sh_mb_bundle *b, const char *salt, const char *hash,
                               const char *of, char **cause)
{
    size_t i, carried = 0;
    if (salt && salt[0] && strcmp(b->id, salt) != 0)
        return msy_cause(cause, MSY_MISMATCH " (it is the mod %s, not %s)", b->id, salt);
    if (strcmp(b->hash, hash) != 0) return msy_cause(cause, MSY_MISMATCH " (it is build %.16s)", b->hash);
    if (!b->validated) return msy_cause(cause, MSY_MISMATCH " (its block records a cause)");
    if (of && of[0] && strcmp(b->slice_of, of) != 0) return msy_cause(cause, MSY_MISMATCH " (it is not part of %.16s)", of);
    if (b->slice_of[0]) {
        char actual[SH_MB_HASH_HEX + 1], *why = NULL;
        size_t len = 0;
        long idx = sh_zip_reader_find(zr, SH_MB_SLICE_SOURCE);
        unsigned char *text = idx < 0 ? NULL : sh_zip_reader_read(zr, idx, (size_t)-1, &len, &why);
        free(why);
        if (text) sh_mb_sha256_hex(text, len, actual);
        free(text);
        if (!text || strcmp(actual, b->slice_of) != 0)
            return msy_cause(cause, MSY_MISMATCH " (" SH_MB_SLICE_SOURCE " is not the build it names)");
        carried++;
    }
    for (i = 0; i < b->file_count; i++) {
        char *entry;
        int missing;
        if (b->files[i].stock) continue;
        carried++;
        if (!(entry = sh_textf("assets/%s", b->files[i].request))) return msy_cause(cause, "out of memory");
        missing = sh_zip_reader_find(zr, entry) < 0;
        if (missing) msy_cause(cause, MSY_MISMATCH " (%s is missing)", entry);
        free(entry);
        if (missing) return 0;
    }
    if (zr->n != carried + 1) return msy_cause(cause, MSY_MISMATCH " (it holds files its mod.json does not list)");
    return 1;
}

/* The map a download serves, noted in the record. */
static void msy_note_place(const sh_ms_place *place, const char *rel)
{
    if (place && rel && rel[0]) (void)sh_ms_place_note(place, rel);
}

/* Unpack the verified build `b` of `zr` into `dir` (created already): every
 * carried file checked against its hash, then mod.json. 1, else 0 with the
 * cause (info->mismatch for damaged bytes). */
static int msy_unpack(sh_zip_reader *zr, const sh_mb_bundle *bp, const char *dir, const unsigned char *modjson,
                      size_t modjson_len, modsync_install_info *info, char **cause)
{
    size_t i;
    for (i = 0; i < bp->file_count; i++) {
        const sh_mb_file *f = &bp->files[i];
        char *entry, *dst, *slash, actual[SH_MB_HASH_HEX + 1], *why = NULL;
        unsigned char *bytes;
        size_t len = 0;
        long idx;
        if (f->stock) continue;   /* never carried: the installed game holds it */
        if (!(entry = sh_textf("assets/%s", f->request))) return msy_cause(cause, "out of memory");
        idx = sh_zip_reader_find(zr, entry);
        free(entry);
        bytes = idx < 0 ? NULL : sh_zip_reader_read(zr, idx, (size_t)f->size, &len, &why);
        if (bytes) sh_mb_sha256_hex(bytes, len, actual);
        if (!bytes || len != f->size || strcmp(actual, f->blob) != 0) {
            free(bytes);
            info->mismatch = 1;
            msy_cause(cause, MSY_MISMATCH " (%s %s)", f->path, why ? why : "differs from its mod.json");
            free(why);
            return 0;
        }
        if (!(dst = msy_join(dir, f->path))) { free(bytes); return msy_cause(cause, "out of memory"); }
        slash = strrchr(dst, '\\');
        if (slash) { *slash = 0; msy_mkdirs(dst); *slash = '\\'; }
        if (!msy_write_file(dst, bytes, len, 0)) {
            DWORD e = GetLastError();
            free(bytes);
            msy_cause(cause, "cannot write %s: %s", f->path, sh_reason_win32(e, dst, len));
            free(dst);
            return 0;
        }
        free(dst);
        free(bytes);
        info->files++;
        info->bytes += len;
    }
    if (bp->slice_of[0]) {
        /* Kept beside mod.json: what the build it was cut from holds. */
        char *why = NULL, *dst = msy_join(dir, SH_MB_SLICE_SOURCE);
        size_t len = 0;
        long idx = sh_zip_reader_find(zr, SH_MB_SLICE_SOURCE);
        unsigned char *text = idx < 0 ? NULL : sh_zip_reader_read(zr, idx, (size_t)-1, &len, &why);
        int wrote = text && dst && msy_write_file(dst, text, len, 0);
        free(why);
        free(text);
        free(dst);
        if (!wrote) return msy_cause(cause, "cannot write " SH_MB_SLICE_SOURCE " for %s", info->name);
    }
    {
        char *mj = msy_join(dir, "mod.json");
        int wrote = mj && msy_write_file(mj, modjson, modjson_len, 0);
        if (!wrote) msy_cause(cause, "cannot write mod.json for %s: %s", info->name, sh_reason_win32(GetLastError(), mj ? mj : "mod.json", 0));
        free(mj);
        if (!wrote) return 0;
    }
    return 1;
}

/* A received build for a map without a place: unpacked into a folder of this
 * process's own outside the store, then installed at the root as the
 * player's own mod (sh_ms_install_to_root), and the unpacked copy removed. */
static int msy_install_root(sh_zip_reader *zr, const sh_mb_bundle *bp, const unsigned char *modjson, size_t modjson_len,
                            modsync_install_info *info, char **cause)
{
    static volatile long counter;
    wchar_t *wtmp = NULL, *wwork = NULL;
    char *utmp, *work = NULL, *dir = NULL, *seg, *name = NULL;
    sh_ms_install_stats st;
    int ok = 0;
    if (!(utmp = msy_temp_dir())) return msy_cause(cause, "the temporary folder cannot be found");
    work = sh_textf("%ssnapmap-plus-fetch.staging-%lu-%ld", utmp, GetCurrentProcessId(), InterlockedIncrement(&counter));
    free(utmp);
    seg = sh_ms_title_segment(info->name ? info->name : "");
    dir = work ? sh_textf("%s\\%s", work, seg ? seg : "mod") : NULL;
    free(seg);
    if (!work || !dir) { free(work); free(dir); return msy_cause(cause, "out of memory"); }
    wtmp = sh_mb_wpath(work);
    if (wtmp) {
        wchar_t *slash = wcsrchr(wtmp, L'\\');
        wwork = _wcsdup(wtmp);
        if (slash && wwork) wwork[slash - wtmp] = 0;   /* the temporary folder: what the removal stays inside */
    }
    if (!wtmp || !wwork) msy_cause(cause, "out of memory");
    else if (!msy_mkdirs(dir)) msy_cause(cause, "cannot create %s: %s", dir, sh_reason_win32(GetLastError(), dir, 0));
    else if (msy_unpack(zr, bp, dir, modjson, modjson_len, info, cause)) {
        memset(&st, 0, sizeof st);
        if (sh_ms_install_to_root(dir, NULL, &name, &st, NULL, cause)) {
            free(info->rel);
            info->rel = name;
            info->already_installed = st.present;
            ok = 1;
        }
    }
    if (wtmp && wwork) msy_rmtree_w(wtmp, wwork, 1);
    free(wtmp);
    free(wwork);
    free(work);
    free(dir);
    return ok;
}

int modsync_install_archive(const char *archive, const char *salt, const char *hash, const sh_ms_place *place,
                            modsync_install_info *info, char **cause)
{
    return modsync_install_archive_of(archive, salt, hash, NULL, place, info, cause);
}

int modsync_install_archive_of(const char *archive, const char *salt, const char *hash, const char *of,
                               const sh_ms_place *place, modsync_install_info *info, char **cause)
{
    modsync_install_info local;
    sh_zip_reader zr;
    sh_mb_bundle b;
    unsigned char *modjson = NULL;
    size_t modjson_len = 0, i;
    const char *root;
    char *final = NULL, *staging = NULL, *old = NULL, *rel = NULL, *one = NULL, *have = NULL;
    wchar_t *wroot = NULL, *wfinal = NULL, *wstaging = NULL, *wold = NULL;
    int ok = 0, locked = 0, named, replace = 0;
    if (!info) info = &local;
    memset(info, 0, sizeof *info);
    memset(&zr, 0, sizeof zr);
    memset(&b, 0, sizeof b);
    if (!archive || !modnet_valid_hash(hash) || (salt && salt[0] && !sh_salt_valid(salt, strlen(salt))))
        return msy_cause(cause, "bad argument");
    if (place && (!place->title || !place->title[0])) return msy_cause(cause, "a received build needs a map's place");
    if (!sh_zip_reader_open(&zr, archive, &one)) {
        info->mismatch = 1;
        msy_cause(cause, MSY_MISMATCH " (%s)", one ? one : "out of memory");
        free(one);
        return 0;
    }
    if (!msy_archive_modjson(&zr, &b, &modjson, &modjson_len, cause) ||
        !msy_archive_matches(&zr, &b, salt, hash, of, cause)) {
        info->mismatch = 1;
        goto done;
    }
    msy_set(&info->name, b.name && b.name[0] ? b.name : b.id);
    if (!info->name) { msy_cause(cause, "out of memory"); goto done; }
    /* Stock pointers name this player's installed game: a game that lacks an
     * entry (or holds other bytes, a different revision) refuses the build
     * before anything is placed. */
    for (i = 0; i < b.file_count; i++) {
        if (!b.files[i].stock) continue;
        if (sh_mb_stock_verify(b.files[i].stock_source, b.files[i].blob, NULL, b.files[i].path, &one) !=
            SH_MB_STOCK_OK) {
            /* The game lacks the bytes: permanent for this machine. */
            const char *source = b.files[i].stock_source ? b.files[i].stock_source : b.files[i].path;
            info->game_lacks = 1;
            msy_set(&info->game_source, source);
            msy_cause(cause, "%s needs a game file your copy of DOOM does not have (%s); verify the game's files in "
                      "Steam, then try again", info->name, source);
            msy_log("MODS: download: %s: %s", info->name, one ? one : "");
            goto done;
        }
        info->stock_files++;
    }
    if (!place) {
        ok = msy_install_root(&zr, &b, modjson, modjson_len, info, cause);
        goto done;
    }

    modsync_store_lock();
    locked = 1;
    if (!(root = msy_root())) { msy_cause(cause, "the store folder cannot be resolved"); goto done; }
    /* The root at this hash serves it (rule 4), or the place has it already. */
    if (modsync_find_installed(b.id, hash, place, &have)) {
        free(info->rel);
        if (!(info->rel = msy_rel_of(root, have))) { msy_cause(cause, "out of memory"); goto done; }
        if (msy_place_has(place, b.id, &rel) && !_stricmp(rel, info->rel)) msy_note_place(place, info->rel);
        free(rel);
        rel = NULL;
        info->already_installed = 1;
        ok = 1;
        goto done;
    }
    /* One build per salt per place: the place's folder of the salt is
     * replaced, else a free folder named after the mod. */
    named = sh_ms_place_folder(place, info->name, b.id, &rel);
    if (!named) { msy_cause(cause, "out of memory"); goto done; }
    msy_set(&info->rel, rel);
    final = sh_textf("%s\\%s", root, rel);
    staging = final ? sh_textf("%s.staging-%lu", final, GetCurrentProcessId()) : NULL;
    old = final ? sh_textf("%s.old-%lu", final, GetCurrentProcessId()) : NULL;
    if (!info->rel || !final || !staging || !old) { msy_cause(cause, "out of memory"); goto done; }
    wroot = sh_mb_wpath(root);
    wfinal = sh_mb_wpath(final);
    wstaging = sh_mb_wpath(staging);
    wold = sh_mb_wpath(old);
    if (!wroot || !wfinal || !wstaging || !wold) { msy_cause(cause, "out of memory"); goto done; }
    if (msy_exists_w(wstaging, NULL, NULL)) msy_rmtree_w(wstaging, wroot, 1);
    if (!msy_mkdirs_w(wstaging)) {
        msy_cause(cause, "cannot create %s: %s", rel, sh_reason_win32(GetLastError(), staging, 0));
        goto done;
    }
    /* Files are unpacked without the store lock: a large build never holds
     * up a save or an export on another thread. The staging folder is this
     * process's own. */
    modsync_store_unlock();
    locked = 0;
    if (!msy_unpack(&zr, &b, staging, modjson, modjson_len, info, cause)) goto unstage;
    modsync_store_lock();
    locked = 1;
    if (modsync_find_installed(b.id, hash, place, NULL)) {   /* another thread finished it meanwhile */
        msy_rmtree_w(wstaging, wroot, 1);
        info->already_installed = 1;
        msy_note_place(place, info->rel);
        ok = 1;
        goto done;
    }
    if (msy_exists_w(wfinal, NULL, NULL)) {
        if (msy_exists_w(wold, NULL, NULL)) msy_rmtree_w(wold, wroot, 1);
        if (!MoveFileExW(wfinal, wold, 0)) {
            DWORD e = GetLastError();
            msy_rmtree_w(wstaging, wroot, 1);
            msy_cause(cause, "cannot replace %s: %s", rel, sh_reason_win32(e, final, 0));
            goto done;
        }
        replace = 1;
    }
    if (!MoveFileExW(wstaging, wfinal, 0)) {
        DWORD e = GetLastError();
        if (replace) MoveFileExW(wold, wfinal, 0);
        msy_rmtree_w(wstaging, wroot, 1);
        msy_cause(cause, "cannot place %s: %s", rel, sh_reason_win32(e, final, 0));
        goto done;
    }
    /* The place's earlier build was received, never the player's work. */
    if (replace) msy_rmtree_w(wold, wroot, 1);
    info->replaced = replace;
    msy_note_place(place, rel);
    ok = 1;
    goto done;
unstage:
    if (!locked) { modsync_store_lock(); locked = 1; }
    msy_rmtree_w(wstaging, wroot, 1);
done:
    if (locked) modsync_store_unlock();
    sh_zip_reader_close(&zr);
    free(modjson);
    free(wroot); free(wfinal); free(wstaging); free(wold);
    free(final); free(staging); free(old); free(rel); free(one); free(have);
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
    free(st->cause);
    st->cause = sh_vtextf(fmt, ap);
    va_end(ap);
    return 0;
}

/* ------------------------------------------------------------ upload ---- */

typedef struct msy_up_build {
    char salt[MODSYNC_SALT_HEX + 1];
    char hash[MODNET_HASH_HEX + 1];
    char *folder;              /* the folder that holds the salt at the hash */
    char *label;               /* its display name */
    int present;               /* HEAD: the store holds it */
} msy_up_build;

static void msy_up_free(msy_up_build *u, int n)
{
    int i;
    for (i = 0; i < n; i++) { free(u[i].folder); free(u[i].label); }
    free(u);
}

/* Find every build's folder, under the store lock. */
static int msy_up_collect(const char *const *mods, int n, msy_up_build **out, int *count, char **cause)
{
    msy_up_build *u = (msy_up_build *)calloc((size_t)n, sizeof *u);
    sh_ms_list l;
    int i, nb = 0, ok = 1;
    *out = NULL;
    *count = 0;
    if (!u) return msy_cause(cause, "out of memory");
    modsync_store_lock();
    if (!sh_ms_enumerate(&l)) { modsync_store_unlock(); free(u); return msy_cause(cause, "the mods folder cannot be read"); }
    for (i = 0; i < n && ok; i++) {
        char salt[MODSYNC_SALT_HEX + 1], hash[MODNET_HASH_HEX + 1];
        const sh_ms_entry *e;
        int j, dup = 0;
        modsync_parse_mod(mods[i], salt, hash);
        for (j = 0; j < nb && !dup; j++) dup = !strcmp(u[j].hash, hash);
        if (dup) continue;
        e = msy_locate(&l, salt, hash, NULL);
        if (!e && msy_slice_find(salt, hash, &u[nb].folder, &u[nb].label)) {
            strcpy_s(u[nb].salt, sizeof u[nb].salt, salt);
            strcpy_s(u[nb].hash, sizeof u[nb].hash, hash);
            nb++;
            continue;
        }
        if (!e) {
            /* What the root holds of the salt names the next step. */
            const sh_ms_entry *other = NULL;
            size_t k;
            for (k = 0; k < l.count && !other; k++)
                if (!msy_in_places(&l.items[k]) && msy_entry_is(&l.items[k], salt, NULL)) other = &l.items[k];
            if (other)
                ok = msy_cause(cause, "%s was built again since the map was saved; save the map again, then publish it",
                               other->bundle.name && other->bundle.name[0] ? other->bundle.name : other->name);
            else
                ok = msy_cause(cause, "The map uses a mod that is not on this computer; remove it from the map in the "
                               "Mods tab (This map), or install the mod again");
            msy_log("MODS: publish: the map names %s, which no folder of this computer holds", mods[i]);
            break;
        }
        strcpy_s(u[nb].salt, sizeof u[nb].salt, salt);
        strcpy_s(u[nb].hash, sizeof u[nb].hash, hash);
        u[nb].label = _strdup(e->bundle.name && e->bundle.name[0] ? e->bundle.name : e->name);
        u[nb].folder = _strdup(e->folder);
        nb++;
        if (!u[nb - 1].folder || !u[nb - 1].label) { ok = msy_cause(cause, "out of memory"); break; }
    }
    sh_ms_list_free(&l);
    modsync_store_unlock();
    if (!ok) { msy_up_free(u, nb); return 0; }
    *out = u;
    *count = nb;
    return 1;
}

static int msy_upload_build(const char *const *mods, int n, const char *ref, const modnet_io *io, modsync_stats *st)
{
    msy_up_build *u = NULL;
    int nb = 0, i, code, ok = 0, have_secret;
    msy_prog prog;
    modnet_result r;
    char claim[MODNET_CLAIM_HEX + 1], secret[MODNET_HASH_HEX + 1], stored_claim[MODNET_CLAIM_HEX + 1],
         stored_secret[MODNET_HASH_HEX + 1], *cause = NULL;
    const char **bh = NULL;

    if (!msy_up_collect(mods, n, &u, &nb, &cause)) {
        msy_stats_fail(st, MODNET_E_ARG, "%s", cause ? cause : "out of memory");
        free(cause);
        return 0;
    }
    st->builds = (unsigned)nb;
    memset(&prog, 0, sizeof prog);
    prog.outer = io;

    /* The store keeps one archive per build: ask before writing one. */
    for (i = 0; i < nb; i++) {
        if (msy_cancelled(io)) { msy_stats_fail(st, MODNET_E_CANCELLED, "cancelled"); goto done; }
        code = modnet_head_build(u[i].hash, NULL, NULL, io, &r);
        msy_absorb(st, &r);
        if (code == MODNET_OK) { u[i].present = 1; st->builds_present++; continue; }
        if (code != MODNET_E_NOT_FOUND) { msy_stats_fail(st, code, "%s: %s", u[i].label, r.message); goto done; }
    }
    for (i = 0; i < nb; i++) {
        char *path;
        unsigned long long size = 0;
        unsigned pointers = 0;
        int wcode = MODNET_E_ARG;
        modnet_io sub;
        if (u[i].present) continue;
        if (msy_cancelled(io)) { msy_stats_fail(st, MODNET_E_CANCELLED, "cancelled"); goto done; }
        if (!(path = msy_temp_archive(u[i].hash))) {
            msy_stats_fail(st, MODNET_E_ARG, "the temporary folder cannot be found");
            goto done;
        }
        if (!modsync_write_archive(u[i].folder, u[i].hash, path, &size, &pointers, &wcode, &cause)) {
            msy_delete_utf8(path);
            free(path);
            msy_stats_fail(st, wcode, "%s", cause ? cause : "out of memory");
            goto done;
        }
        st->stock_files += pointers;
        prog.total += size;
        sub = msy_sub_io(io, &prog);
        code = modnet_put_build(u[i].hash, path, size, &sub, &r);
        msy_delete_utf8(path);
        free(path);
        msy_absorb(st, &r);
        if (code != MODNET_OK) { msy_stats_fail(st, code, "%s: %s", u[i].label, r.message); goto done; }
        prog.base += size;
        st->builds_sent++;
        if (r.created) st->builds_created++;
        msy_log("MODS: push %s (%.16s): %u MB sent, %u pointer(s) left to the player's game", u[i].label, u[i].hash,
                msy_mb(size), pointers);
    }

    /* One claim for the whole set: re-claim with the stored secret when the
     * ref has one, which replaces the set the store keeps for it. */
    bh = (const char **)malloc(sizeof(char *) * (size_t)nb);
    if (!bh) { msy_stats_fail(st, MODNET_E_NOMEM, "out of memory"); goto done; }
    for (i = 0; i < nb; i++) bh[i] = u[i].hash;
    have_secret = modsync_claim_lookup(ref, stored_claim, stored_secret);
    code = modnet_claim(ref, bh, nb, have_secret ? stored_secret : NULL, claim, secret, io, &r);
    msy_absorb(st, &r);
    if (code == MODNET_E_FORBIDDEN && have_secret) {
        msy_log("MODS: push: the stored claim for this ref is not known to the store; making a new claim");
        code = modnet_claim(ref, bh, nb, NULL, claim, secret, io, &r);
        msy_absorb(st, &r);
        have_secret = 0;
    }
    if (code == MODNET_OK) {
        st->reclaimed = have_secret && strcmp(claim, stored_claim) == 0;
        strcpy_s(st->claim, sizeof st->claim, claim);
        sh_mb_record_why_clear();
        if (!msy_claim_store(ref, claim, secret, bh, nb)) {
            const char *why = sh_mb_record_why();
            SecureZeroMemory(secret, sizeof secret);
            SecureZeroMemory(stored_secret, sizeof stored_secret);
            if (!why) why = "mods\\snapmap-plus.json could not be written";
            msy_log("MODS: push: claimed as %s, but the claim was not saved, so it cannot be released later: %s", claim,
                    why);
            /* The publish dialog shows this cause: the file and the next step first. */
            msy_stats_fail(st, MODNET_E_ARG, "%s", why);
            goto done;
        }
    }
    SecureZeroMemory(secret, sizeof secret);
    SecureZeroMemory(stored_secret, sizeof stored_secret);
    if (code != MODNET_OK) { msy_stats_fail(st, code, "%s", r.message); goto done; }
    ok = 1;
done:
    free((void *)bh);
    free(cause);
    msy_up_free(u, nb);
    return ok;
}

int modsync_upload_builds(const char *const *mods, int n, const char *ref, const modnet_io *io, modsync_stats *stats)
{
    modsync_stats local;
    modsync_stats *st = stats ? stats : &local;
    double t0 = msy_now_ms();
    int i, ok;
    memset(st, 0, sizeof *st);
    if (!modnet_valid_ref(ref)) return msy_stats_fail(st, MODNET_E_ARG, "%s", MODNET_REF_RULE);
    if (n <= 0 || !mods) return msy_stats_fail(st, MODNET_E_ARG, "name at least one build");
    for (i = 0; i < n; i++)
        if (!modsync_parse_mod(mods[i], NULL, NULL))
            return msy_stats_fail(st, MODNET_E_ARG, "mod %d is not <salt>@<build hash>", i + 1);
    ok = msy_upload_build(mods, n, ref, io, st);
    st->ms = msy_now_ms() - t0;
    if (ok) {
        st->code = MODNET_OK;
        msy_pending_drop(ref);
        modsync_store_lock();
        for (i = 0; i < n; i++) {
            msy_slice_drop(mods[i] + MODSYNC_SALT_HEX + 1);
            msy_staged(mods[i] + MODSYNC_SALT_HEX + 1, 0);
        }
        modsync_store_unlock();
        msy_retry_releases(io);
        return 1;
    }
    if (modnet_transient(st->code)) st->queued = msy_pending_put(ref, mods, n, modsync_cause(st));
    return 0;
}

/* ------------------------------------------------------------ download -- */

int modsync_fetch_build_for(const sh_modmanifest_row *row, const sh_ms_place *place, const modnet_io *io,
                            modsync_stats *stats)
{
    modsync_stats local;
    modsync_stats *st = stats ? stats : &local;
    modsync_install_info info;
    modnet_result r;
    msy_prog prog;
    modnet_io sub;
    char *path = NULL, *cause = NULL, *folder = NULL;
    const char *root = msy_root();
    double t0 = msy_now_ms();
    int code, ok;
    memset(st, 0, sizeof *st);
    memset(&info, 0, sizeof info);
    if (!row || !sh_salt_valid(row->salt, strlen(row->salt)) || !modnet_valid_hash(row->build))
        return msy_stats_fail(st, MODNET_E_ARG, "not a <salt>@<build hash> row");
    if (place && (!place->title || !place->title[0])) return msy_stats_fail(st, MODNET_E_ARG, "a download needs a map's place");
    st->builds = 1;
    if (row->of[0] && modsync_find_installed(row->salt, row->of, NULL, &folder) && root &&
        !msy_in_places_rel(root, folder)) {
        /* The root's whole build the files were cut from serves the row. */
        st->folder = msy_rel_of(root, folder);
        free(folder);
        st->already_installed = 1;
        st->ms = msy_now_ms() - t0;
        return 1;
    }
    free(folder);
    folder = NULL;
    if (!place && row->of[0]) {
        const char *title = row->name && row->name[0] ? row->name : "A mod";
        return msy_stats_fail(st, MODNET_E_REFUSED, MODSYNC_PART_ONLY, title, row->name && row->name[0] ? row->name : "it");
    }
    if (!place) {
        /* The root: a root folder at the row's hash serves it; a map's place
         * holding this very build is installed at the root from there. */
        sh_ms_list l;
        const sh_ms_entry *e = NULL;
        int served = 0;
        memset(&l, 0, sizeof l);
        modsync_store_lock();
        if (sh_ms_enumerate(&l) && (e = msy_locate(&l, row->salt, row->build, NULL)) != NULL) {
            served = !msy_in_places(e);
            folder = _strdup(e->folder);
        }
        sh_ms_list_free(&l);
        modsync_store_unlock();
        if (e && !folder) return msy_stats_fail(st, MODNET_E_NOMEM, "out of memory");
        if (e && served) {
            if (root) st->folder = msy_rel_of(root, folder);
            free(folder);
            st->already_installed = 1;
            st->ms = msy_now_ms() - t0;
            return 1;
        }
        if (e) {
            sh_ms_install_stats is;
            char *name = NULL;
            memset(&is, 0, sizeof is);
            ok = sh_ms_install_to_root(folder, NULL, &name, &is, NULL, &cause);
            free(folder);
            if (!ok) {
                msy_stats_fail(st, MODNET_E_REFUSED, "%s", cause ? cause : "out of memory");
                free(cause);
                return 0;
            }
            st->folder = name;
            st->builds_created = 1;
            if (sh_modserve_index_rescan() < 0)
                msy_log("MODS: %.16s: installed in mods\\%s, but the serving index could not be rebuilt", row->build, name);
            st->ms = msy_now_ms() - t0;
            st->code = MODNET_OK;
            return 1;
        }
    } else if (modsync_find_installed(row->salt, row->build, place, &folder)) {
        /* The root at the row's hash, or this map's place, serves it. */
        if (root) {
            char *rel = NULL;
            st->folder = msy_rel_of(root, folder);
            modsync_store_lock();
            if (st->folder && msy_place_has(place, row->salt, &rel) && !_stricmp(rel, st->folder))
                msy_note_place(place, st->folder);
            free(rel);
            modsync_store_unlock();
        }
        free(folder);
        st->already_installed = 1;
        st->ms = msy_now_ms() - t0;
        return 1;
    }
    if (!(path = msy_temp_archive(row->build)))
        return msy_stats_fail(st, MODNET_E_ARG, "the temporary folder cannot be found");
    memset(&prog, 0, sizeof prog);
    prog.outer = io;
    prog.total = row->size;   /* the carried bytes the map records: near the archive's size */
    sub = msy_sub_io(io, &prog);
    code = modnet_get_build(row->build, path, &sub, &r);
    msy_absorb(st, &r);
    if (code != MODNET_OK) {
        msy_delete_utf8(path);
        free(path);
        st->ms = msy_now_ms() - t0;
        return msy_stats_fail(st, code, "%s", r.message);
    }
    ok = modsync_install_archive_of(path, row->salt, row->build, row->of, place, &info, &cause);
    msy_delete_utf8(path);
    free(path);
    msy_set(&st->folder, info.rel);
    st->ms = msy_now_ms() - t0;
    if (!ok) {
        int fail = info.game_lacks ? MODNET_E_GAME : info.mismatch ? MODNET_E_HASH : MODNET_E_REFUSED;
        if (info.game_lacks) {
            st->game_lacks = 1;
            msy_set(&st->game_source, info.game_source);
        } else {
            msy_log("MODS: pull %.16s: %s", row->build, cause ? cause : "out of memory");
        }
        msy_stats_fail(st, fail, "%s", cause ? cause : "out of memory");
        free(cause);
        modsync_install_info_free(&info);
        return 0;
    }
    st->already_installed = info.already_installed;
    if (!info.already_installed) {
        st->builds_created = 1;
        msy_log("MODS: pull %s (%.16s): placed in mods\\%s, %u file(s), %u pointer(s)%s", info.name, row->build, info.rel,
                info.files, info.stock_files, info.replaced ? ", replacing the place's earlier build" : "");
    }
    if (sh_modserve_index_rescan() < 0)
        msy_log("MODS: pull %.16s: installed, but the serving index could not be rebuilt", row->build);
    st->code = MODNET_OK;
    modsync_install_info_free(&info);
    msy_retry_releases(io);
    return 1;
}

/* ------------------------------------------------------------ probe ----- */

int modsync_probe_build(const char *hash, const modnet_io *io, modsync_probe *out)
{
    modsync_probe local;
    modnet_result r;
    int code;
    if (!out) out = &local;
    memset(out, 0, sizeof *out);
    if (!modnet_valid_hash(hash)) { out->found = -1; out->code = MODNET_E_ARG; return -1; }
    code = modnet_head_build(hash, &out->size, &out->scripts, io, &r);
    out->code = code;
    if (code == MODNET_OK) { out->found = 1; return 1; }
    if (code == MODNET_E_NOT_FOUND || code == MODNET_E_BLOCKED) { out->found = 0; return 0; }
    out->found = -1;
    out->cause = _strdup(r.message && r.message[0] ? r.message : modnet_code_text(code));
    return -1;
}

int modsync_claim_names(const char *hash)
{
    mpj *doc, *claims;
    int i, k, found = 0;
    if (!modnet_valid_hash(hash)) return 0;
    modsync_store_lock();
    doc = msy_doc_load(MODSYNC_CLAIMS_SECTION, "claims");
    modsync_store_unlock();
    claims = doc ? mpj_get(doc, "claims") : NULL;
    for (i = 0; claims && claims->type == MPJ_OBJ && i < claims->n; i++) {
        const mpj *list = claims->items[i] ? mpj_get(claims->items[i], "builds") : NULL;
        for (k = 0; list && list->type == MPJ_ARR && k < list->n && !found; k++)
            found = mpj_str_is(list->items[k], hash);
        /* Clear the secret this tree holds before release. */
        {
            const mpj *sec = claims->items[i] ? mpj_get(claims->items[i], "secret") : NULL;
            if (sec && sec->type == MPJ_STR && sec->text) SecureZeroMemory(sec->text, strlen(sec->text));
        }
    }
    mpj_free(doc);
    return found;
}

/* ------------------------------------------------------------ release --- */

static __declspec(thread) int t_msy_retrying;   /* inside msy_retry_releases: no nested retry */

/* The release itself: no queueing, no retry of others. */
static int msy_release(const char *ref, const modnet_io *io, modsync_stats *st)
{
    char claim[MODNET_CLAIM_HEX + 1], secret[MODNET_HASH_HEX + 1];
    modnet_result r;
    double t0 = msy_now_ms();
    int code;
    memset(st, 0, sizeof *st);
    if (!modnet_valid_ref(ref)) return msy_stats_fail(st, MODNET_E_ARG, "%s", MODNET_REF_RULE);
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
        if (code == MODNET_E_NOT_FOUND) msy_set(&st->cause, "the store no longer knew the claim; forgotten locally");
        return 1;
    }
    return msy_stats_fail(st, code, "%s", r.message);
}

/* Retry the queued releases only (each one request). Run after a store call
 * succeeded, when the store is known to answer. */
static void msy_retry_releases(const modnet_io *io)
{
    mpj *doc, *pend;
    int i;
    if (t_msy_retrying) return;
    modsync_store_lock();
    doc = msy_doc_load(MODSYNC_PENDING_SECTION, "pending");
    modsync_store_unlock();
    pend = doc ? mpj_get(doc, "pending") : NULL;
    t_msy_retrying = 1;
    for (i = 0; pend && i < pend->n; i++) {
        const char *ref = pend->keys[i];
        modsync_stats st;
        if (msy_cancelled(io)) break;
        if (!mpj_str_is(mpj_get(pend->items[i], "kind"), "release") || strchr(ref, '\\')) continue;
        if (msy_release(ref, io, &st)) msy_log("MODS: retry %s: released the queued claim", ref);
        else if (!modnet_transient(st.code)) { msy_pending_drop(ref); msy_log("MODS: retry %s: release dropped: %s", ref, modsync_cause(&st)); }
        modsync_stats_free(&st);
    }
    t_msy_retrying = 0;
    mpj_free(doc);
}

int modsync_release(const char *ref, const modnet_io *io, modsync_stats *stats)
{
    modsync_stats local;
    modsync_stats *st = stats ? stats : &local;
    if (msy_release(ref, io, st)) {
        msy_retry_releases(io);
        return 1;
    }
    /* A release the store did not take is not forgotten: a leaked claim
     * keeps its builds on the store. Queued, retried at the next launch
     * and after the next store call that succeeds. */
    if (modnet_transient(st->code)) st->queued = msy_pending_put(ref, NULL, 0, modsync_cause(st));
    return 0;
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
        const mpj *e = pend->items[i], *arr = mpj_get(e, "builds");
        const char **mods;
        const char *ref = pend->keys[i];
        modsync_stats st;
        int n = 0, k, valid = 1;
        memset(&st, 0, sizeof st);
        if (msy_cancelled(io)) break;
        if (mpj_str_is(mpj_get(e, "kind"), "release") && !strchr(ref, '\\')) {
            t_msy_retrying = 1;
            valid = msy_release(ref, io, &st);
            t_msy_retrying = 0;
            if (valid) { ok_n++; msy_log("MODS: retry %s: released the queued claim", ref); }
            else if (!modnet_transient(st.code)) {
                msy_pending_drop(ref);
                drop_n++;
                msy_log("MODS: retry %s: release dropped: %s", ref, modsync_cause(&st));
            } else msy_log("MODS: retry %s: the release is still failing: %s", ref, modsync_cause(&st));
            if (last) { modsync_stats_free(last); *last = st; }
            else modsync_stats_free(&st);
            continue;
        }
        /* Keys are written escaped; a ref that needed escapes is skipped
         * rather than decoded (refs are plain text in practice). */
        if (strchr(ref, '\\')) valid = 0;
        if (!arr || arr->type != MPJ_ARR || arr->n < 1) valid = 0;
        mods = valid ? (const char **)calloc((size_t)arr->n, sizeof *mods) : NULL;
        if (valid && !mods) break;   /* out of memory: the queue stays for the next try */
        for (k = 0; valid && k < arr->n; k++) {
            if (arr->items[k]->type != MPJ_STR || !modsync_parse_mod(arr->items[k]->text, NULL, NULL)) valid = 0;
            else mods[n++] = arr->items[k]->text;
        }
        if (!valid) {
            msy_log("MODS: retry: a queued upload is malformed; dropped");
            if (!strchr(ref, '\\')) msy_pending_drop(ref);
            drop_n++;
            free((void *)mods);
            continue;
        }
        if (modsync_upload_builds(mods, n, ref, io, &st)) {
            ok_n++;
            msy_log("MODS: retry %s: claimed %u build(s), %u sent, %llu byte(s) in %.0f ms", ref, st.builds,
                    st.builds_sent, st.bytes_up, st.ms);
        } else if (!modnet_transient(st.code)) {
            msy_pending_drop(ref);
            drop_n++;
            msy_log("MODS: retry %s: dropped, the store refused it: %s", ref, modsync_cause(&st));
        } else {
            msy_log("MODS: retry %s: still failing: %s", ref, modsync_cause(&st));
        }
        free((void *)mods);
        if (last) { modsync_stats_free(last); *last = st; }
        else modsync_stats_free(&st);
    }
    mpj_free(doc);
    if (done) *done = ok_n;
    if (dropped) *dropped = drop_n;
    left = modsync_pending_count();
    return left;
}
