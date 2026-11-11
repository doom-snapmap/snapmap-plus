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
#include "mp_json.h"
#include "backend_log.h"
#include "plain_reason.h"

#define MSY_PLACES "maps"   /* mods\maps\<map title>\<mod name>\: the places (ONE-SHAPE rule 4) */

/* The places of modstore.h (lane D of the one-shape plan): the folder under
 * mods\ that `place` gives the mod `salt` (1 a free name for `name`, 2 the
 * place's folder that holds the salt already, 0 none), and the record's note
 * that the folder serves the place's map. Declared here until modstore.h
 * carries them. */
int sh_ms_place_folder(const sh_ms_place *place, const char *name, const char *salt, char *out, size_t cap);
int sh_ms_place_note(const sh_ms_place *place, const char *folder);

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
 * empty one. A record that cannot be read loads as empty too, and the record
 * layer then refuses the section's save until it reads again (modbundle.h),
 * so the claims it holds are never replaced with fewer. */
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

/* Record a build whose fetch found only editor-time content: section
 * "editorOnly", keyed by build hash. 1 written. */
static int msy_editor_only_record(const char *hash, const char *id, const char *name)
{
    mpj *doc = msy_doc_load("editorOnly", "editorOnly"), *section, *entry;
    int ok = 0;
    if (!doc) return 0;
    section = mpj_get(doc, "editorOnly");
    entry = mpj_new(MPJ_OBJ);
    if (section && entry && mpj_set(entry, "id", mpj_new_str(id ? id : "")) &&
        mpj_set(entry, "name", mpj_new_str(name ? name : "")) &&
        mpj_set(entry, "at", mpj_new_int((long long)_time64(NULL))) && mpj_set(section, hash, entry))
        ok = msy_doc_save("editorOnly", doc);
    else mpj_free(entry);
    mpj_free(doc);
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

/* The folder `full` (absolute) under the store root `root`, as its name under mods\. */
static void msy_rel_of(const char *root, const char *full, char *out, size_t cap)
{
    size_t rl;
    if (!strncmp(root, "\\\\?\\", 4)) root += 4;   /* either may carry the long-path prefix */
    if (!strncmp(full, "\\\\?\\", 4)) full += 4;
    rl = strlen(root);
    while (rl && (root[rl - 1] == '\\' || root[rl - 1] == '/')) rl--;
    if (!_strnicmp(full, root, rl) && (full[rl] == '\\' || full[rl] == '/')) strncpy_s(out, cap, full + rl + 1, _TRUNCATE);
    else strncpy_s(out, cap, strrchr(full, '\\') ? strrchr(full, '\\') + 1 : full, _TRUNCATE);
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

static int msy_entry_is(const sh_ms_entry *e, const char *salt, const char *hash)
{
    return sh_ms_entry_usable(e) && !strcmp(e->bundle.id, salt) && (!hash || !strcmp(e->bundle.hash, hash));
}

/* The place's folder of the salt (its name under mods\), when it has one. */
static int msy_place_has(const sh_ms_place *place, const char *salt, char *rel, size_t cap)
{
    return place && sh_ms_place_folder(place, "mod", salt, rel, cap) == 2;
}

/* The entry serving salt at hash: a root folder, else the place's folder of
 * the salt (place NULL: any place). Under the store lock. */
static const sh_ms_entry *msy_locate(const sh_ms_list *l, const char *salt, const char *hash, const sh_ms_place *place)
{
    char rel[MODSYNC_FOLDER_MAX];
    int have_rel = 0;
    size_t i;
    for (i = 0; i < l->count; i++)
        if (!msy_in_places(&l->items[i]) && msy_entry_is(&l->items[i], salt, hash)) return &l->items[i];
    if (place) have_rel = msy_place_has(place, salt, rel, sizeof rel);
    if (place && !have_rel) return NULL;
    for (i = 0; i < l->count; i++) {
        const sh_ms_entry *e = &l->items[i];
        if (!msy_in_places(e) || !msy_entry_is(e, salt, hash)) continue;
        if (!have_rel || !_stricmp(e->name, rel)) return e;
    }
    return NULL;
}

int modsync_find_installed(const char *salt, const char *hash, const sh_ms_place *place, char *folder, size_t cap)
{
    sh_ms_list l;
    int found = 0;
    if (folder && cap) folder[0] = 0;
    if (!salt || !sh_salt_valid(salt, strlen(salt)) || !modnet_valid_hash(hash)) return 0;
    modsync_store_lock();
    if (sh_ms_enumerate(&l)) {
        const sh_ms_entry *e = msy_locate(&l, salt, hash, place);
        if (e) {
            found = 1;
            if (folder && cap) strncpy_s(folder, cap, e->folder, _TRUNCATE);
        }
        sh_ms_list_free(&l);
    }
    modsync_store_unlock();
    return found;
}

/* A temporary file for one archive: %TEMP%\snapmap-plus-<hash16>-<pid>-<n>.zip. */
static int msy_temp_archive(const char *hash, char *out, size_t cap)
{
    static volatile long counter;
    wchar_t dir[MAX_PATH + 1];
    char *udir;
    DWORD n = GetTempPathW(MAX_PATH + 1, dir);
    if (!n || n > MAX_PATH) return 0;
    udir = sh_mb_utf8(dir);
    if (!udir) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%ssnapmap-plus-%.16s-%lu-%ld.zip", udir, hash, GetCurrentProcessId(),
                InterlockedIncrement(&counter));
    free(udir);
    return 1;
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

/* ------------------------------------------------------------ archive --- */

int modsync_write_archive(const char *folder, const char *hash, const char *out_path, unsigned long long *size,
                          unsigned *pointers, int *code, char *cause, size_t cap)
{
    char mj[MAX_PATH * 4], actual[SH_MB_HASH_HEX + 1];
    unsigned char *text = NULL;
    size_t text_len = 0, i;
    unsigned long long carried = 0;
    sh_mb_bundle b;
    sh_zip_writer w;
    wchar_t *wout = NULL;
    const char *label;
    int ok = 0, opened = 0;
    if (size) *size = 0;
    if (pointers) *pointers = 0;
    if (code) *code = MODNET_E_ARG;
    if (cause && cap) cause[0] = 0;
    memset(&b, 0, sizeof b);
    memset(&w, 0, sizeof w);
    w.h = INVALID_HANDLE_VALUE;
    if (!folder || !modnet_valid_hash(hash) || !out_path) return msy_cause(cause, cap, "bad argument");
    msy_join(mj, sizeof mj, folder, "mod.json");
    text = sh_mb_read_file(mj, SH_MB_DESCRIPTOR_MAX + 1, &text_len);
    if (!text) return msy_cause(cause, cap, "%s cannot be read: %s", mj, sh_reason_win32(GetLastError(), mj, 0));
    if (text_len > SH_MB_DESCRIPTOR_MAX || !sh_mb_load_text((const char *)text, text_len, &b)) {
        msy_cause(cause, cap, "%s is not a built mod: %s", mj, b.first_cause[0] ? b.first_cause : "over 1 MiB");
        goto done;
    }
    label = b.name && b.name[0] ? b.name : b.id;
    if (strcmp(b.hash, hash) != 0) {
        msy_cause(cause, cap, "%s was built again since the map was saved; save the map again, then publish it", label);
        goto done;
    }
    if (!b.validated) {
        msy_cause(cause, cap, "%s does not build: %s", label, b.state_cause[0] ? b.state_cause : "see the Mods tab");
        goto done;
    }
    for (i = 0; i < b.file_count; i++) {
        if (b.files[i].stock) { if (pointers) (*pointers)++; continue; }
        carried += b.files[i].size;
    }
    if (carried + text_len > MODNET_MAX_ARCHIVE) {
        if (code) *code = MODNET_E_TOO_LARGE;
        msy_cause(cause, cap, "%s: this mod is too large to publish (%u MB; the limit is %u MB)", label,
                  msy_mb(carried + text_len), msy_mb(MODNET_MAX_ARCHIVE));
        goto done;
    }
    wout = sh_mb_wpath(out_path);
    if (!wout || !sh_zip_writer_open(&w, wout)) {
        msy_cause(cause, cap, "cannot write the archive %s: %s", out_path, sh_reason_win32(GetLastError(), out_path, 0));
        goto done;
    }
    opened = 1;
    if (!sh_zip_writer_add(&w, "mod.json", text, text_len)) { msy_cause(cause, cap, "%s", w.cause); goto done; }
    for (i = 0; i < b.file_count; i++) {
        const sh_mb_file *f = &b.files[i];
        char path[MAX_PATH * 4], entry[SH_MB_REQUEST_MAX + 16];
        unsigned char *bytes;
        size_t len = 0;
        if (f->stock) continue;   /* the player's own game holds it */
        msy_join(path, sizeof path, folder, f->path);
        bytes = sh_mb_read_file(path, (size_t)SH_MB_MAX_FILE + 1, &len);
        if (!bytes) {
            msy_cause(cause, cap, "%s: %s cannot be read; open the Mods tab and Refresh", label, f->path);
            goto done;
        }
        sh_mb_sha256_hex(bytes, len, actual);
        if (len != f->size || strcmp(actual, f->blob) != 0) {
            free(bytes);
            msy_cause(cause, cap, "%s: %s changed since the mod was built; open the Mods tab and Refresh", label, f->path);
            goto done;
        }
        _snprintf_s(entry, sizeof entry, _TRUNCATE, "assets/%s", f->request);
        ok = sh_zip_writer_add(&w, entry, bytes, len);
        free(bytes);
        if (!ok) { msy_cause(cause, cap, "%s", w.cause); goto done; }
        ok = 0;
    }
    if (!sh_zip_writer_finish(&w) || !FlushFileBuffers(w.h)) {
        msy_cause(cause, cap, "cannot write the archive %s: %s", out_path, w.cause[0] ? w.cause : "flush failed");
        goto done;
    }
    if (w.off > MODNET_MAX_ARCHIVE) {
        if (code) *code = MODNET_E_TOO_LARGE;
        msy_cause(cause, cap, "%s: this mod is too large to publish (%u MB; the limit is %u MB)", label, msy_mb(w.off),
                  msy_mb(MODNET_MAX_ARCHIVE));
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
    sh_mb_free(&b);
    return ok;
}

/* ------------------------------------------------------------ install --- */

#define MSY_MISMATCH "the mod store sent a build that does not match the map"

/* The archive's mod.json, read and loaded. 1 with b filled (release with
 * sh_mb_free) and *text (malloc'd) the file; 0 with the cause. */
static int msy_archive_modjson(const sh_zip_reader *zr, sh_mb_bundle *b, unsigned char **text, size_t *len,
                               char *cause, size_t cap)
{
    char why[256];
    long idx = sh_zip_reader_find(zr, "mod.json");
    *text = NULL;
    *len = 0;
    memset(b, 0, sizeof *b);
    if (idx < 0) return msy_cause(cause, cap, MSY_MISMATCH " (it has no mod.json)");
    why[0] = 0;
    *text = sh_zip_reader_read(zr, idx, SH_MB_DESCRIPTOR_MAX, len, why, sizeof why);
    if (!*text) return msy_cause(cause, cap, MSY_MISMATCH " (mod.json: %s)", why[0] ? why : "unreadable");
    if (!sh_mb_load_text((const char *)*text, *len, b)) {
        msy_cause(cause, cap, MSY_MISMATCH " (mod.json: %s)", b->first_cause);
        free(*text);
        *text = NULL;
        sh_mb_free(b);
        return 0;
    }
    return 1;
}

/* The archive is exactly the build: salt, hash, and one entry per carried
 * file besides mod.json. */
static int msy_archive_matches(const sh_zip_reader *zr, const sh_mb_bundle *b, const char *salt, const char *hash,
                               char *cause, size_t cap)
{
    size_t i, carried = 0;
    if (salt && salt[0] && strcmp(b->id, salt) != 0)
        return msy_cause(cause, cap, MSY_MISMATCH " (it is the mod %s, not %s)", b->id, salt);
    if (strcmp(b->hash, hash) != 0) return msy_cause(cause, cap, MSY_MISMATCH " (it is build %.16s)", b->hash);
    if (!b->validated) return msy_cause(cause, cap, MSY_MISMATCH " (its block records a cause)");
    for (i = 0; i < b->file_count; i++) {
        char entry[SH_MB_REQUEST_MAX + 16];
        if (b->files[i].stock) continue;
        carried++;
        _snprintf_s(entry, sizeof entry, _TRUNCATE, "assets/%s", b->files[i].request);
        if (sh_zip_reader_find(zr, entry) < 0) return msy_cause(cause, cap, MSY_MISMATCH " (%s is missing)", entry);
    }
    if (zr->n != carried + 1) return msy_cause(cause, cap, MSY_MISMATCH " (it holds files its mod.json does not list)");
    return 1;
}

/* The map a download serves, noted in the record. */
static void msy_note_place(const sh_ms_place *place, const char *rel)
{
    if (place && rel && rel[0]) (void)sh_ms_place_note(place, rel);
}

int modsync_install_archive(const char *archive, const char *salt, const char *hash, const sh_ms_place *place,
                            modsync_install_info *info, char *cause, size_t cap)
{
    modsync_install_info local;
    sh_zip_reader zr;
    sh_mb_bundle b;
    unsigned char *modjson = NULL;
    size_t modjson_len = 0, i;
    char root[MAX_PATH * 4], final[MAX_PATH * 4], staging[MAX_PATH * 4], old[MAX_PATH * 4], rel[MODSYNC_FOLDER_MAX],
         one[SH_MB_CAUSE_MAX], have[MAX_PATH * 4];
    wchar_t *wroot = NULL, *wfinal = NULL, *wstaging = NULL, *wold = NULL;
    int ok = 0, locked = 0, named, replace = 0;
    if (!info) info = &local;
    memset(info, 0, sizeof *info);
    memset(&zr, 0, sizeof zr);
    memset(&b, 0, sizeof b);
    if (cause && cap) cause[0] = 0;
    if (!archive || !modnet_valid_hash(hash) || (salt && salt[0] && !sh_salt_valid(salt, strlen(salt))))
        return msy_cause(cause, cap, "bad argument");
    if (!place || !place->title || !place->title[0]) return msy_cause(cause, cap, "a received build needs a map's place");
    if (!sh_zip_reader_open(&zr, archive, one, sizeof one)) {
        info->mismatch = 1;
        return msy_cause(cause, cap, MSY_MISMATCH " (%s)", one);
    }
    if (!msy_archive_modjson(&zr, &b, &modjson, &modjson_len, cause, cap) ||
        !msy_archive_matches(&zr, &b, salt, hash, cause, cap)) {
        info->mismatch = 1;
        goto done;
    }
    strncpy_s(info->name, sizeof info->name, b.name && b.name[0] ? b.name : b.id, _TRUNCATE);
    /* Stock pointers name this player's installed game: a game that lacks an
     * entry (or holds other bytes, a different revision) refuses the build
     * before anything is placed. */
    for (i = 0; i < b.file_count; i++) {
        if (!b.files[i].stock) continue;
        if (sh_mb_stock_verify(b.files[i].stock_source, b.files[i].blob, NULL, b.files[i].path, one, sizeof one) !=
            SH_MB_STOCK_OK) {
            /* DESIGN-additive 2.8, case 4: permanent for this machine. */
            info->game_lacks = 1;
            strncpy_s(info->game_source, sizeof info->game_source,
                      b.files[i].stock_source ? b.files[i].stock_source : b.files[i].path, _TRUNCATE);
            msy_cause(cause, cap, "refused: %s", one);
            goto done;
        }
        info->stock_files++;
    }

    modsync_store_lock();
    locked = 1;
    if (!msy_root(root, sizeof root)) { msy_cause(cause, cap, "the store folder cannot be resolved"); goto done; }
    /* The root at this hash serves it (rule 4), or the place has it already. */
    if (modsync_find_installed(b.id, hash, place, have, sizeof have)) {
        msy_rel_of(root, have, info->rel, sizeof info->rel);
        if (msy_place_has(place, b.id, rel, sizeof rel) && !_stricmp(rel, info->rel)) msy_note_place(place, info->rel);
        info->already_installed = 1;
        ok = 1;
        goto done;
    }
    /* One build per salt per place: the place's folder of the salt is
     * replaced, else a free folder named after the mod. */
    named = sh_ms_place_folder(place, info->name, b.id, rel, sizeof rel);
    if (!named) { msy_cause(cause, cap, "no free folder for %s in the map's place", info->name); goto done; }
    strncpy_s(info->rel, sizeof info->rel, rel, _TRUNCATE);
    _snprintf_s(final, sizeof final, _TRUNCATE, "%s\\%s", root, rel);
    _snprintf_s(staging, sizeof staging, _TRUNCATE, "%s.staging-%lu", final, GetCurrentProcessId());
    _snprintf_s(old, sizeof old, _TRUNCATE, "%s.old-%lu", final, GetCurrentProcessId());
    wroot = sh_mb_wpath(root);
    wfinal = sh_mb_wpath(final);
    wstaging = sh_mb_wpath(staging);
    wold = sh_mb_wpath(old);
    if (!wroot || !wfinal || !wstaging || !wold) { msy_cause(cause, cap, "out of memory"); goto done; }
    if (msy_exists_w(wstaging, NULL, NULL)) msy_rmtree_w(wstaging, wroot, 1);
    if (!msy_mkdirs_w(wstaging)) {
        msy_cause(cause, cap, "cannot create %s: %s", rel, sh_reason_win32(GetLastError(), staging, 0));
        goto done;
    }
    /* Files are unpacked without the store lock: a large build never holds
     * up a save or an export on another thread. The staging folder is this
     * process's own. */
    modsync_store_unlock();
    locked = 0;
    for (i = 0; i < b.file_count; i++) {
        const sh_mb_file *f = &b.files[i];
        char entry[SH_MB_REQUEST_MAX + 16], dst[MAX_PATH * 4], actual[SH_MB_HASH_HEX + 1], why[256];
        unsigned char *bytes;
        size_t len = 0;
        long idx;
        if (f->stock) continue;   /* never carried: the installed game holds it */
        _snprintf_s(entry, sizeof entry, _TRUNCATE, "assets/%s", f->request);
        idx = sh_zip_reader_find(&zr, entry);
        why[0] = 0;
        bytes = idx < 0 ? NULL : sh_zip_reader_read(&zr, idx, (size_t)f->size, &len, why, sizeof why);
        if (bytes) sh_mb_sha256_hex(bytes, len, actual);
        if (!bytes || len != f->size || strcmp(actual, f->blob) != 0) {
            free(bytes);
            info->mismatch = 1;
            msy_cause(cause, cap, MSY_MISMATCH " (%s %s)", f->path, why[0] ? why : "differs from its mod.json");
            goto unstage;
        }
        msy_join(dst, sizeof dst, staging, f->path);
        {
            char parent[MAX_PATH * 4], *slash;
            strcpy_s(parent, sizeof parent, dst);
            slash = strrchr(parent, '\\');
            if (slash) { *slash = 0; msy_mkdirs(parent); }
        }
        if (!msy_write_file(dst, bytes, len, 0)) {
            DWORD e = GetLastError();
            free(bytes);
            msy_cause(cause, cap, "cannot write %s: %s", f->path, sh_reason_win32(e, dst, len));
            goto unstage;
        }
        free(bytes);
        info->files++;
        info->bytes += len;
    }
    {
        char mj[MAX_PATH * 4];
        msy_join(mj, sizeof mj, staging, "mod.json");
        if (!msy_write_file(mj, modjson, modjson_len, 0)) {
            msy_cause(cause, cap, "cannot write mod.json for %s: %s", rel, sh_reason_win32(GetLastError(), mj, 0));
            goto unstage;
        }
    }
    modsync_store_lock();
    locked = 1;
    if (modsync_find_installed(b.id, hash, place, NULL, 0)) {   /* another thread finished it meanwhile */
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
            msy_cause(cause, cap, "cannot replace %s: %s", rel, sh_reason_win32(e, final, 0));
            goto done;
        }
        replace = 1;
    }
    if (!MoveFileExW(wstaging, wfinal, 0)) {
        DWORD e = GetLastError();
        if (replace) MoveFileExW(wold, wfinal, 0);
        msy_rmtree_w(wstaging, wroot, 1);
        msy_cause(cause, cap, "cannot place %s: %s", rel, sh_reason_win32(e, final, 0));
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

typedef struct msy_up_build {
    char salt[MODSYNC_SALT_HEX + 1];
    char hash[MODNET_HASH_HEX + 1];
    char *folder;              /* the folder that holds the salt at the hash */
    char label[128];           /* its display name */
    int present;               /* HEAD: the store holds it */
} msy_up_build;

static void msy_up_free(msy_up_build *u, int n)
{
    int i;
    for (i = 0; i < n; i++) free(u[i].folder);
    free(u);
}

/* Find every build's folder, under the store lock. */
static int msy_up_collect(const char *const *mods, int n, msy_up_build **out, int *count, char *cause, size_t cap)
{
    msy_up_build *u = (msy_up_build *)calloc((size_t)n, sizeof *u);
    sh_ms_list l;
    int i, nb = 0, ok = 1;
    *out = NULL;
    *count = 0;
    if (!u) return msy_cause(cause, cap, "out of memory");
    modsync_store_lock();
    if (!sh_ms_enumerate(&l)) { modsync_store_unlock(); free(u); return msy_cause(cause, cap, "the mods folder cannot be read"); }
    for (i = 0; i < n && ok; i++) {
        char salt[MODSYNC_SALT_HEX + 1], hash[MODNET_HASH_HEX + 1];
        const sh_ms_entry *e;
        int j, dup = 0;
        modsync_parse_mod(mods[i], salt, hash);
        for (j = 0; j < nb && !dup; j++) dup = !strcmp(u[j].hash, hash);
        if (dup) continue;
        e = msy_locate(&l, salt, hash, NULL);
        if (!e) {
            /* What the root holds of the salt names the next step. */
            const sh_ms_entry *other = NULL;
            size_t k;
            for (k = 0; k < l.count && !other; k++)
                if (!msy_in_places(&l.items[k]) && msy_entry_is(&l.items[k], salt, NULL)) other = &l.items[k];
            if (other)
                ok = msy_cause(cause, cap, "%s was built again since the map was saved; save the map again, then publish it",
                               other->bundle.name && other->bundle.name[0] ? other->bundle.name : other->name);
            else
                ok = msy_cause(cause, cap, "the map uses a mod this machine does not have (%s); add it to your mods folder "
                               "or remove it from the map", mods[i]);
            break;
        }
        strcpy_s(u[nb].salt, sizeof u[nb].salt, salt);
        strcpy_s(u[nb].hash, sizeof u[nb].hash, hash);
        strncpy_s(u[nb].label, sizeof u[nb].label, e->bundle.name && e->bundle.name[0] ? e->bundle.name : e->name, _TRUNCATE);
        u[nb].folder = _strdup(e->folder);
        if (!u[nb].folder) { ok = msy_cause(cause, cap, "out of memory"); break; }
        nb++;
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
         stored_secret[MODNET_HASH_HEX + 1], cause[512];
    const char **bh = NULL;

    cause[0] = 0;
    if (!msy_up_collect(mods, n, &u, &nb, cause, sizeof cause)) return msy_stats_fail(st, MODNET_E_ARG, "%s", cause);
    st->builds = (unsigned)nb;
    memset(&prog, 0, sizeof prog);
    prog.outer = io;

    /* The store keeps one archive per build: ask before writing one. */
    for (i = 0; i < nb; i++) {
        if (msy_cancelled(io)) { msy_stats_fail(st, MODNET_E_CANCELLED, "cancelled"); goto done; }
        code = modnet_head_build(u[i].hash, NULL, io, &r);
        msy_absorb(st, &r);
        if (code == MODNET_OK) { u[i].present = 1; st->builds_present++; continue; }
        if (code != MODNET_E_NOT_FOUND) { msy_stats_fail(st, code, "%s: %s", u[i].label, r.message); goto done; }
    }
    for (i = 0; i < nb; i++) {
        char path[MAX_PATH * 4];
        unsigned long long size = 0;
        unsigned pointers = 0;
        int wcode = MODNET_E_ARG;
        modnet_io sub;
        if (u[i].present) continue;
        if (msy_cancelled(io)) { msy_stats_fail(st, MODNET_E_CANCELLED, "cancelled"); goto done; }
        if (!msy_temp_archive(u[i].hash, path, sizeof path)) {
            msy_stats_fail(st, MODNET_E_ARG, "the temporary folder cannot be found");
            goto done;
        }
        if (!modsync_write_archive(u[i].folder, u[i].hash, path, &size, &pointers, &wcode, cause, sizeof cause)) {
            msy_delete_utf8(path);
            msy_stats_fail(st, wcode, "%s", cause);
            goto done;
        }
        st->stock_files += pointers;
        prog.total += size;
        sub = msy_sub_io(io, &prog);
        code = modnet_put_build(u[i].hash, path, size, &sub, &r);
        msy_delete_utf8(path);
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
            char why[MAX_PATH * 4 + 240];
            SecureZeroMemory(secret, sizeof secret);
            SecureZeroMemory(stored_secret, sizeof stored_secret);
            if (!sh_mb_record_why(why, sizeof why))
                strcpy_s(why, sizeof why, "mods\\snapmap-plus.json could not be written");
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
    if (!modnet_valid_ref(ref)) return msy_stats_fail(st, MODNET_E_ARG, "the ref must be 1-64 characters of text");
    if (n <= 0 || n > MODSYNC_MAX_BUILDS || !mods)
        return msy_stats_fail(st, MODNET_E_ARG, "name 1 to %d builds", MODSYNC_MAX_BUILDS);
    for (i = 0; i < n; i++)
        if (!modsync_parse_mod(mods[i], NULL, NULL))
            return msy_stats_fail(st, MODNET_E_ARG, "mod %d is not <salt>@<build hash>", i + 1);
    ok = msy_upload_build(mods, n, ref, io, st);
    st->ms = msy_now_ms() - t0;
    if (ok) {
        st->code = MODNET_OK;
        msy_pending_drop(ref);
        msy_retry_releases(io);
        return 1;
    }
    if (modnet_transient(st->code)) st->queued = msy_pending_put(ref, mods, n, st->cause);
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
    char path[MAX_PATH * 4], cause[600], folder[MAX_PATH * 4], root[MAX_PATH * 4];
    double t0 = msy_now_ms();
    int code, ok;
    memset(st, 0, sizeof *st);
    if (!row || !sh_salt_valid(row->salt, strlen(row->salt)) || !modnet_valid_hash(row->build))
        return msy_stats_fail(st, MODNET_E_ARG, "not a <salt>@<build hash> row");
    if (!place || !place->title || !place->title[0]) return msy_stats_fail(st, MODNET_E_ARG, "a download needs a map's place");
    st->builds = 1;
    if (modsync_find_installed(row->salt, row->build, place, folder, sizeof folder)) {
        /* The root at the row's hash, or this map's place, serves it. */
        if (msy_root(root, sizeof root)) {
            char rel[MODSYNC_FOLDER_MAX];
            msy_rel_of(root, folder, st->folder, sizeof st->folder);
            modsync_store_lock();
            if (msy_place_has(place, row->salt, rel, sizeof rel) && !_stricmp(rel, st->folder))
                msy_note_place(place, st->folder);
            modsync_store_unlock();
        }
        st->already_installed = 1;
        st->ms = msy_now_ms() - t0;
        return 1;
    }
    if (!msy_temp_archive(row->build, path, sizeof path))
        return msy_stats_fail(st, MODNET_E_ARG, "the temporary folder cannot be found");
    memset(&prog, 0, sizeof prog);
    prog.outer = io;
    prog.total = row->size;   /* the carried bytes the map records: near the archive's size */
    sub = msy_sub_io(io, &prog);
    code = modnet_get_build(row->build, path, &sub, &r);
    msy_absorb(st, &r);
    if (code != MODNET_OK) { st->ms = msy_now_ms() - t0; return msy_stats_fail(st, code, "%s", r.message); }
    {
        /* A build of editor-time content only (an author's editor overrides
         * a map named): nothing in it plays, so nothing is installed. The
         * record remembers it, and the row is served from now on. */
        sh_zip_reader zr;
        sh_mb_bundle b;
        unsigned char *text = NULL;
        size_t len = 0;
        char why[300];
        int editor = 0;
        memset(&zr, 0, sizeof zr);
        memset(&b, 0, sizeof b);
        if (sh_zip_reader_open(&zr, path, why, sizeof why) &&
            msy_archive_modjson(&zr, &b, &text, &len, why, sizeof why) &&
            msy_archive_matches(&zr, &b, row->salt, row->build, why, sizeof why) && sh_mb_editor_only(&b)) {
            int recorded = msy_editor_only_record(row->build, b.id, b.name);
            editor = 1;
            msy_log("MODS: pull %.16s: %s (%s) changes only the editor (%u file(s), no requirements); not installed, "
                    "the map plays without it%s", row->build, b.name && b.name[0] ? b.name : b.id, b.id,
                    (unsigned)b.file_count, recorded ? "" : " (the record could not be written; it will be asked again)");
        }
        sh_mb_free(&b);
        free(text);
        sh_zip_reader_close(&zr);
        if (editor) {
            msy_delete_utf8(path);
            st->editor_only = 1;
            if (sh_modserve_index_rescan() < 0)
                msy_log("MODS: pull %.16s: the serving index could not be rebuilt after the record was written", row->build);
            st->ms = msy_now_ms() - t0;
            st->code = MODNET_OK;
            return 1;
        }
    }
    ok = modsync_install_archive(path, row->salt, row->build, place, &info, cause, sizeof cause);
    msy_delete_utf8(path);
    strncpy_s(st->folder, sizeof st->folder, info.rel, _TRUNCATE);
    st->ms = msy_now_ms() - t0;
    if (!ok) {
        if (info.game_lacks) {
            st->game_lacks = 1;
            strncpy_s(st->game_source, sizeof st->game_source, info.game_source, _TRUNCATE);
            return msy_stats_fail(st, MODNET_E_GAME, "%s", cause);
        }
        msy_log("MODS: pull %.16s: %s", row->build, cause);
        return msy_stats_fail(st, info.mismatch ? MODNET_E_HASH : MODNET_E_REFUSED, "%s", cause);
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
    code = modnet_head_build(hash, &out->size, io, &r);
    out->code = code;
    if (code == MODNET_OK) { out->found = 1; return 1; }
    if (code == MODNET_E_NOT_FOUND || code == MODNET_E_BLOCKED) { out->found = 0; return 0; }
    out->found = -1;
    strncpy_s(out->cause, sizeof out->cause, r.message[0] ? r.message : modnet_code_text(code), _TRUNCATE);
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
        else if (!modnet_transient(st.code)) { msy_pending_drop(ref); msy_log("MODS: retry %s: release dropped: %s", ref, st.cause); }
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
    if (modnet_transient(st->code)) st->queued = msy_pending_put(ref, NULL, 0, st->cause);
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
        const char *mods[MODSYNC_MAX_BUILDS];
        char ref[4 * MODNET_REF_MAX + 1];
        modsync_stats st;
        int n = 0, k, valid = 1;
        size_t rl = strlen(pend->keys[i]);
        if (msy_cancelled(io)) break;
        if (mpj_str_is(mpj_get(e, "kind"), "release") && rl < sizeof ref && !strchr(pend->keys[i], '\\')) {
            strcpy_s(ref, sizeof ref, pend->keys[i]);
            t_msy_retrying = 1;
            valid = msy_release(ref, io, &st);
            t_msy_retrying = 0;
            if (valid) { ok_n++; msy_log("MODS: retry %s: released the queued claim", ref); }
            else if (!modnet_transient(st.code)) {
                msy_pending_drop(ref);
                drop_n++;
                msy_log("MODS: retry %s: release dropped: %s", ref, st.cause);
            } else msy_log("MODS: retry %s: the release is still failing: %s", ref, st.cause);
            if (last) *last = st;
            continue;
        }
        /* Keys are written escaped; a ref that needed escapes is skipped
         * rather than decoded (refs are plain text in practice). */
        if (rl >= sizeof ref || strchr(pend->keys[i], '\\')) valid = 0;
        else strcpy_s(ref, sizeof ref, pend->keys[i]);
        if (!arr || arr->type != MPJ_ARR || arr->n < 1 || arr->n > MODSYNC_MAX_BUILDS) valid = 0;
        for (k = 0; valid && k < arr->n; k++) {
            if (arr->items[k]->type != MPJ_STR || !modsync_parse_mod(arr->items[k]->text, NULL, NULL)) valid = 0;
            else mods[n++] = arr->items[k]->text;
        }
        if (!valid) {
            msy_log("MODS: retry: a queued upload is malformed; dropped");
            if (rl < sizeof ref && !strchr(pend->keys[i], '\\')) msy_pending_drop(pend->keys[i]);
            drop_n++;
            continue;
        }
        if (modsync_upload_builds(mods, n, ref, io, &st)) {
            ok_n++;
            msy_log("MODS: retry %s: claimed %u build(s), %u sent, %llu byte(s) in %.0f ms", ref, st.builds,
                    st.builds_sent, st.bytes_up, st.ms);
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
