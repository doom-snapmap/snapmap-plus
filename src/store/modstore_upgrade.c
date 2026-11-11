/* modstore_upgrade.c -- see modstore_upgrade.h. The store tool's half of the
 * mod store: the Contract 2 layout, the second step of its upgrade and the
 * retired recycle bin. Never linked into the game DLL. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modstore_internal.h"
#include "modstore_upgrade.h"
#include "modbundle_old.h"
#include "config_json.h"
#include "mp_json.h"
#include "backend_log.h"
#include "store_text.h"

/* This tool's cause fields are fixed records: a sentence goes in whole or
 * is said in the log (store_text.h). */
#undef ms_cause
static int ms_cause(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    char *text;
    va_start(ap, fmt);
    text = sh_vtextf(fmt, ap);
    va_end(ap);
    st_put(out, cap, text ? text : "out of memory");
    free(text);
    return 0;
}

/* An earlier store's folder path under mods\ and its label: the store this
 * tool upgrades admitted at most 1,023 bytes and SH_MB_ID_MAX + 150 (its
 * MSU_NAME_MAX and MSU_KEY_MAX). A cause field holds a sentence whole or
 * says it is in the log (store_text.h). */
#define MSU_NAME_MAX  1023
#define MSU_KEY_MAX   (SH_MB_ID_MAX + 1 + 128 + 1 + 20)
#define MSU_CAUSE_CAP 1024

/* ---------------------------------------------------- the retired bin --- */

/* How the store upgrade and the bin sweep remove what they retire: through
 * the caller's discard (the store tool's Recycle Bin) when one is set, else
 * outright. */
static sh_ms_discard_fn g_ms_discard;
static void *g_ms_discard_ctx;
static const char *g_ms_discard_verb = "deleted";

void sh_ms_set_discard(sh_ms_discard_fn fn, void *context, const char *verb)
{
    g_ms_discard = fn;
    g_ms_discard_ctx = context;
    g_ms_discard_verb = fn && verb && verb[0] ? verb : "deleted";
}

/* Retire `path`, which must lie strictly inside `within`. Without a discard a
 * folder is removed recursively and a link is removed as a link, never
 * followed. 1 when it is gone (or was never there). */
static int ms_discard(const wchar_t *path, const wchar_t *within)
{
    DWORD a;
    if (!path || !within || !ms_inside(path, within)) return 0;
    a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD err = GetLastError();
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
    }
    if (g_ms_discard) return g_ms_discard(g_ms_discard_ctx, path) != 0;
    if ((a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT)) return ms_rmtree(path, within);
    if (a & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryW(path) != 0;
    return ms_delete_file(path);
}

int sh_ms_retire_sibling(const wchar_t *leaf)
{
    wchar_t *root = ms_wroot(), *slash, *path = NULL;
    int ok = 0;
    if (!root || !leaf || !leaf[0] || wcschr(leaf, L'\\') || wcschr(leaf, L'/')) { free(root); return 0; }
    if (!g_ms_discard) { free(root); return -1; }
    slash = wcsrchr(root, L'\\');
    if (slash && slash > root) {
        *slash = 0;
        path = ms_wjoin(root, leaf);
        ok = path ? ms_discard(path, root) : 0;
    }
    free(path);
    free(root);
    return ok;
}

const char *sh_ms_discard_verb(void) { return g_ms_discard_verb; }

int sh_ms_bin_sweep(sh_ms_sweep_line_fn line, void *context)
{
    wchar_t *root = ms_wroot(), *bin = NULL, *pattern = NULL, *slash;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    char root8[MAX_PATH * 4], text[MAX_PATH * 4 + 160], *record = NULL;
    int deleted = 0, failed = 0, had_record = 0;
    if (!root) return -1;
    modstore_lock();
    /* removed-mods\ is the store root's sibling: <data>\removed-mods. */
    slash = wcsrchr(root, L'\\');
    if (slash && slash > root) {
        size_t n = (size_t)(slash - root);
        bin = (wchar_t *)malloc((n + 16) * sizeof(wchar_t));
        if (bin) { memcpy(bin, root, n * sizeof(wchar_t)); wcscpy_s(bin + n, 16, L"\\removed-mods"); }
    }
    if (bin && ms_is_dir(bin) && (pattern = ms_wjoin(bin, L"*")) != NULL &&
        (h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0)) != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *child;
            char leaf[MSU_NAME_MAX * 4 + 1];
            int ok;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            child = ms_wjoin(bin, fd.cFileName);
            if (!child) { failed++; continue; }
            if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, leaf, (int)sizeof leaf, NULL, NULL)) strcpy_s(leaf, sizeof leaf, "?");
            /* A link is removed as a link; a folder's tree never follows one. */
            ok = ms_discard(child, bin);
            free(child);
            if (ok) {
                deleted++;
                st_putf(text, sizeof text, "MODS: removed-mods: %s %s", leaf, g_ms_discard_verb);
            } else {
                failed++;
                st_putf(text, sizeof text, "MODS: removed-mods: %s could not be removed (%s); it stays "
                            "for the next run", leaf, ms_why(GetLastError(), leaf, 0, 0));
            }
            backend_log(text);
            if (line) line(context, text);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(pattern);
    if (bin && ms_is_dir(bin) && !failed) (void)RemoveDirectoryW(bin);
    /* The record's "removed" section goes with the bin. */
    if (st_path(root8, sizeof root8, sh_ms_root()) && sh_mb_record_get(root8, "removed", &record) == 1 && record) {
        had_record = 1;
        (void)sh_mb_record_put(root8, "removed", NULL);
    }
    free(record);
    if (deleted || failed || had_record) {
        if (failed)
            st_putf(text, sizeof text, "MODS: removed-mods: %d folder(s) %s, %d could not be removed; "
                        "the bin is retired", deleted, g_ms_discard_verb, failed);
        else st_putf(text, sizeof text, "MODS: removed-mods: %d folder(s) %s; the bin is retired", deleted,
                         g_ms_discard_verb);
        backend_log(text);
        if (line) line(context, text);
    }
    free(bin);
    free(root);
    modstore_unlock();
    return failed ? -1 : deleted;
}

static DWORD ms_link_count(const wchar_t *path)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (GetFileInformationByHandle(h, &info)) n = info.nNumberOfLinks;
    CloseHandle(h);
    return n;
}

/* ---------------------------------------------------------- upgrade ---- */
/* The retired layout (Contract 2): <id>@<version>+<build>\ folders with
 * bundle.json, bundle.canonical.json and .state.json, their assets\ hard-linked
 * to blobs\<sha256> where the volume allowed it, legacy\<id>@<digest16>\ for
 * delivered packages, and claims.json, publish-links.json,
 * pending-uploads.json and .migrated.json beside them. */

typedef struct ms_up_item {
    wchar_t *dir;               /* where it is now */
    char from[MSU_NAME_MAX + 16];   /* its name before the upgrade ("legacy\..." for a delivered one) */
    char name[MSU_NAME_MAX + 1];    /* its name after */
    int legacy;                 /* under legacy\ */
    int product;                /* named <id>@<version>+<build> by the product: renamed */
    int dropped;                /* a delivered copy an installed mod already serves */
    size_t kept;                /* when dropped: the item that serves it */
    sh_mb_bundle b;
    char version[160];          /* the old folder name's version label */
    char content[SH_MB_HASH_HEX + 1];
    unsigned long long build;   /* bundle.json's build counter */
} ms_up_item;

typedef struct ms_up_ctx {
    wchar_t *root, *blobs;
    char rootu[MAX_PATH * 4];
    ms_up_item *items;
    size_t count, cap;
    sh_ms_upgrade_stats *st;
} ms_up_ctx;

/* A folder of the retired layout that may be a package: not the layout's
 * own blobs\ and legacy\ (legacy\ is read on its own, as delivered
 * packages). The game reads both as ordinary folder names now. */
static int ms_up_candidate(const WIN32_FIND_DATAW *fd)
{
    return ms_candidate(fd) && _wcsicmp(fd->cFileName, L"blobs") && _wcsicmp(fd->cFileName, L"legacy");
}

/* Something of the retired layout is present. */
static int ms_up_needed(const wchar_t *root)
{
    static const wchar_t *const files[] = { L"blobs", L"legacy", L"claims.json", L"publish-links.json",
                                            L"pending-uploads.json", L".migrated.json", NULL };
    WIN32_FIND_DATAW fd;
    wchar_t *pattern;
    HANDLE h;
    int i, found = 0;
    for (i = 0; files[i] && !found; i++) found = ms_has_file(root, files[i]);
    if (found) return 1;
    pattern = ms_wjoin(root, L"*");
    h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        wchar_t *child;
        if (!ms_up_candidate(&fd)) continue;
        child = ms_wjoin(root, fd.cFileName);
        found = child && (ms_has_file(child, L"bundle.json") || ms_has_file(child, L".state.json") ||
                          ms_has_file(child, L"bundle.canonical.json"));
        free(child);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

#ifdef SH_MODSTORE_TESTING
int sh_ms_test_fail_unlink_copy;
#endif

/* Give the hard-linked file `p` (a carried file `f` inside `dir`) its own
 * bytes without ever being without them: the original is renamed aside, copied
 * back to its name, the copy is verified against the bundle, and only then is
 * the renamed original retired. On any failure the original is put back. A
 * renamed original left by an interrupted run is put back first
 * (ms_up_unlink_recover). */
#define MS_UP_ASIDE L".snapmap-plus-unlink"
static int ms_up_unlink(const wchar_t *p, const wchar_t *dir, const sh_mb_file *f, char *cause, size_t cap)
{
    size_t n = wcslen(p) + 32;
    wchar_t *aside = (wchar_t *)malloc(sizeof(wchar_t) * n);
    char got[SH_MB_HASH_HEX + 1];
    unsigned long long size = 0;
    DWORD err = 0;
    int copied;
    if (!aside) return ms_cause(cause, cap, "out of memory");
    _snwprintf_s(aside, n, _TRUNCATE, L"%s%s", p, MS_UP_ASIDE);
    if (!MoveFileExW(p, aside, 0)) {
        err = GetLastError();
        free(aside);
        return ms_cause(cause, cap, "%s is a hard link that could not be set aside for copying (%s); it is unchanged",
                        f->path, ms_why(err, f->path, 0, 0));
    }
    copied = CopyFileW(aside, p, TRUE);
#ifdef SH_MODSTORE_TESTING
    if (copied && sh_ms_test_fail_unlink_copy > 0) { sh_ms_test_fail_unlink_copy--; copied = 0; SetLastError(ERROR_DISK_FULL); }
#endif
    if (!copied) err = GetLastError();
    else {
        SetFileAttributesW(p, FILE_ATTRIBUTE_NORMAL);
        if (!ms_hash_wfile(p, &size, got) || size != f->size || strcmp(got, f->blob) != 0) { copied = 0; err = ERROR_CRC; }
    }
    if (copied && !ms_discard(aside, dir)) { copied = 0; err = GetLastError(); }
    if (!copied) {
        /* The copy goes (a duplicate of the original, or partial) and the original returns. */
        ms_delete_file(p);
        if (!MoveFileExW(aside, p, 0))
            ms_cause(cause, cap, "%s is a hard link that could not be replaced by a copy (%s); the original is kept "
                     "beside it as %s.snapmap-plus-unlink", f->path, ms_why(err, f->path, 0, 0), f->path);
        else ms_cause(cause, cap, "%s is a hard link that could not be replaced by a copy (%s); the original was put "
                      "back", f->path, ms_why(err, f->path, 0, 0));
        free(aside);
        return 0;
    }
    free(aside);
    return 1;
}

/* An interrupted ms_up_unlink: the renamed original is the file. */
static void ms_up_unlink_recover(const wchar_t *p)
{
    size_t n = wcslen(p) + 32;
    wchar_t *aside = (wchar_t *)malloc(sizeof(wchar_t) * n);
    if (!aside) return;
    _snwprintf_s(aside, n, _TRUNCATE, L"%s%s", p, MS_UP_ASIDE);
    if (ms_exists(aside)) {
        if (ms_exists(p)) ms_delete_file(p);   /* the run's own copy, complete or partial */
        if (MoveFileExW(aside, p, 0)) ms_log("MODS: store: a file set aside by an interrupted run was put back");
    }
    free(aside);
}

/* Put every carried file of `b` in place as a real file: restored from
 * blobs\ when missing or of the wrong size, copied over a hard link. */
static int ms_up_files(ms_up_ctx *c, const wchar_t *dir, const sh_mb_bundle *b, unsigned *unlinked, unsigned *restored,
                       char *cause, size_t cap)
{
    size_t i;
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        wchar_t *p, *blob;
        unsigned long long size = 0;
        int ok = 1;
        if (f->stock) continue;
        p = ms_wjoin_utf8(dir, f->path);
        if (!p) return ms_cause(cause, cap, "out of memory");
        ms_up_unlink_recover(p);
        if (!ms_file_size(p, &size) || size != f->size) {
            unsigned long long bsize = 0;
            blob = c->blobs ? ms_wjoin_utf8(c->blobs, f->blob) : NULL;
            ok = blob && ms_file_size(blob, &bsize) && bsize == f->size && sh_ms_copy_file(blob, p);
            free(blob);
            if (!ok) { free(p); return ms_cause(cause, cap, "%s is missing and blobs\\ has no copy of it", f->path); }
            (*restored)++;
        } else if (ms_link_count(p) > 1) {
            /* Editing a linked file would edit every copy: the folder gets its own. */
            ok = ms_up_unlink(p, dir, f, cause, cap);
            if (!ok) { free(p); return 0; }
            (*unlinked)++;
        } else {
            DWORD a = GetFileAttributesW(p);
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) SetFileAttributesW(p, a & ~FILE_ATTRIBUTE_READONLY);
        }
        free(p);
    }
    return 1;
}

/* Write a file whole through a temporary beside it. */
static int ms_up_write(const wchar_t *path, const char *text, size_t n)
{
    size_t pl = wcslen(path) + 16;
    wchar_t *tmp = (wchar_t *)malloc(sizeof(wchar_t) * pl);
    HANDLE h;
    DWORD done = 0;
    int ok = 0;
    if (!tmp) return 0;
    _snwprintf_s(tmp, pl, _TRUNCATE, L"%s.tmp-up", path);
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        ok = WriteFile(h, text, (DWORD)n, &done, NULL) && done == n && FlushFileBuffers(h);
        CloseHandle(h);
        ok = ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok) DeleteFileW(tmp);
    }
    free(tmp);
    return ok;
}

static char *ms_up_strdup_body(const mpj *v)
{
    return v && v->type == MPJ_STR ? _strdup(v->text) : NULL;
}

/* The fields the upgrade needs from a Contract 2 bundle.json (a canonical
 * manifest of format 1: id, version, name, files with blob and size, stock
 * pointers): b->id the old id, b->hash its bundle hash, b->files their path,
 * request, blob, size and stock flag. 1 when it reads. */
static int ms_up_read_c2(const char *text, size_t len, sh_mb_bundle *b, char *version, size_t vcap, char *cause,
                         size_t cap)
{
    mpj *j = mpj_parse(text, len);
    const mpj *v, *files;
    int i, ok = 0;
    memset(b, 0, sizeof *b);
    if (!j || j->type != MPJ_OBJ) { ms_cause(cause, cap, "it is not a JSON object"); goto out; }
    v = mpj_get(j, "id");
    if (!v || v->type != MPJ_STR || !sh_mb_id_normalize(v->text, strlen(v->text), b->id)) {
        ms_cause(cause, cap, "it names no id");
        goto out;
    }
    v = mpj_get(j, "version");
    st_cpy(version, vcap, v && v->type == MPJ_STR ? v->text : "0.0.0");
    b->name = ms_up_strdup_body(mpj_get(j, "name"));
    v = mpj_get(j, "bundle");
    if (v && v->type == MPJ_STR && ms_is_hash(v->text)) strcpy_s(b->hash, sizeof b->hash, v->text);
    else {
        /* Earlier canonical manifests omit the hash: it is the canonical text's. */
        sh_mb_sha256_hex((const unsigned char *)text, len, b->hash);
    }
    files = mpj_get(j, "files");
    if (files && files->type == MPJ_ARR && files->n > 0) {
        b->files = (sh_mb_file *)calloc((size_t)files->n, sizeof *b->files);
        if (!b->files) { ms_cause(cause, cap, "out of memory"); goto out; }
        for (i = 0; i < files->n; i++) {
            const mpj *f = files->items[i], *p = mpj_get(f, "path"), *bl = mpj_get(f, "blob"), *st = mpj_get(f, "stock");
            sh_mb_file *o = &b->files[b->file_count];
            size_t k;
            if (!p || p->type != MPJ_STR || _strnicmp(p->text, "assets/", 7)) continue;
            o->path = _strdup(p->text);
            o->request = _strdup(p->text + 7);
            if (!o->path || !o->request) { ms_cause(cause, cap, "out of memory"); goto out; }
            for (k = 0; o->request[k]; k++) if (o->request[k] >= 'A' && o->request[k] <= 'Z') o->request[k] = (char)(o->request[k] + 32);
            if (st && st->type == MPJ_OBJ) {
                const mpj *sh = mpj_get(st, "sha256");
                o->stock = 1;
                if (sh && sh->type == MPJ_STR) st_cpy(o->blob, sizeof o->blob, sh->text);
            } else {
                v = mpj_get(f, "size");
                if (bl && bl->type == MPJ_STR) st_cpy(o->blob, sizeof o->blob, bl->text);
                o->size = v && mpj_is_num(v) ? (unsigned long long)mpj_int(v) : 0;
                b->total_size += o->size;
            }
            b->file_count++;
        }
    }
    ok = 1;
out:
    mpj_free(j);
    return ok;
}

/* The mod.json a Contract 2 folder becomes: its author part (id, name,
 * version, author, homepage, requirements and the preserved keys of its
 * manifest's "extra") and a format-1 product block naming its key and build,
 * the shape the store tool's conversion to the one shape reads next
 * (store_convert.h). */
static char *ms_up_c2_modjson(const char *text, size_t len, const sh_mb_bundle *b, size_t *out_len)
{
    mpj *j = mpj_parse(text, len), *o = mpj_new(MPJ_OBJ), *blk = mpj_new(MPJ_OBJ);
    const mpj *v;
    char *out = NULL;
    static const char *const author[] = { "id", "name", "version", "author", "homepage", "requirements", NULL };
    int i;
    if (!j || !o || !blk) goto out;
    for (i = 0; author[i]; i++)
        if ((v = mpj_get(j, author[i])) != NULL) mpj_set(o, author[i], mpj_clone(v));
    v = mpj_get(j, "extra");
    if (v && v->type == MPJ_OBJ) {
        for (i = 0; i < v->n; i++)
            if (!mpj_get(o, v->keys[i])) mpj_set(o, v->keys[i], mpj_clone(v->items[i]));
    }
    mpj_set(blk, "format", mpj_new_int(1));
    mpj_set(blk, "key", mpj_new_str(b->id));
    mpj_set(blk, "bundle", mpj_new_str(b->hash));
    if (b->built) mpj_set(blk, "built", mpj_new_int(b->built));
    {
        mpj *val = mpj_new(MPJ_BOOL);
        if (val) val->b = b->validated;
        mpj_set(blk, "validated", val);
    }
    if (!b->validated && sh_mb_state_cause(b)[0]) {
        const char *sc = sh_mb_state_cause(b);
        size_t w = 0, k, ecap = strlen(sc) * 2 + 1;
        char *esc = (char *)malloc(ecap);
        for (k = 0; esc && sc[k] && w + 3 < ecap; k++) {
            char c = sc[k];
            if (c == '"' || c == '\\') esc[w++] = '\\';
            esc[w++] = (unsigned char)c < 0x20 ? ' ' : c;
        }
        if (esc) {
            esc[w] = 0;
            mpj_set(blk, "cause", mpj_new_str(esc));
            free(esc);
        }
    }
    mpj_set(o, SH_MB_BLOCK_KEY, blk);
    blk = NULL;
    out = mpj_dump(o, out_len);
out:
    mpj_free(j);
    mpj_free(o);
    mpj_free(blk);
    return out;
}

/* A folder this upgrade wrote before (an interrupted pass): its format-1
 * mod.json read back for the steps after. 1 when it reads. */
static int ms_up_read_old(const char *dir, sh_mb_bundle *b, char *version, size_t vcap)
{
    sh_mb_old old;
    size_t i;
    int ok = 0;
    memset(b, 0, sizeof *b);
    if (!sh_mb_old_load(dir, &old) || old.block_format != 1 || !old.key[0]) { sh_mb_old_free(&old); return 0; }
    strcpy_s(b->id, sizeof b->id, old.key);
    if (!version) {
        /* the caller wants the files only */
    } else if (old.version && old.version[0] == '"') {
        size_t n = strlen(old.version);
        st_fmt(version, vcap, "%.*s", (int)(n > 2 ? n - 2 : 0), old.version + 1);
    } else strcpy_s(version, vcap, "0.0.0");
    strcpy_s(b->hash, sizeof b->hash, old.bundle);
    b->files = old.file_count ? (sh_mb_file *)calloc(old.file_count, sizeof *b->files) : NULL;
    for (i = 0; b->files && i < old.file_count; i++) {
        sh_mb_file *o = &b->files[b->file_count];
        if (!old.files[i].path || _strnicmp(old.files[i].path, "assets/", 7)) continue;
        o->path = _strdup(old.files[i].path);
        o->request = _strdup(old.files[i].path + 7);
        strcpy_s(o->blob, sizeof o->blob, old.files[i].sha256);
        o->size = old.files[i].size;
        o->stock = old.files[i].pointer;
        b->file_count++;
    }
    ok = b->hash[0] != 0;
    sh_mb_old_free(&old);
    return ok;
}

/* Convert one folder of the retired layout in place: mod.json written in the
 * format-1 shape the conversion to the one shape reads (store_convert.h) and
 * read back with the bundle's hash, then the old files removed. A folder
 * already converted is read back. Returns 1 converted or read, 0 not a built
 * folder, -1 left as it was (logged). */
static int ms_up_convert(ms_up_ctx *c, ms_up_item *it)
{
    wchar_t *bj = ms_wjoin(it->dir, L"bundle.json"), *mj = ms_wjoin(it->dir, L"mod.json");
    char *dir = sh_mb_utf8(it->dir), *text = NULL, *modjson = NULL, cause[MSU_CAUSE_CAP];
    size_t len = 0, mlen = 0;
    unsigned unlinked = 0, restored = 0;
    int r = -1;
    sh_mb_bundle chk;
    memset(&chk, 0, sizeof chk);
    cause[0] = 0;
    if (!bj || !dir || !mj) goto out;
    if (!ms_exists(bj)) {
        /* Converted already (an interrupted pass: the leftovers go now), or
         * not a built folder. */
        r = ms_up_read_old(dir, &it->b, it->version, sizeof it->version) ? 1 : 0;
        if (!r) { sh_mb_free(&it->b); memset(&it->b, 0, sizeof it->b); }
        else {
            wchar_t *p = ms_wjoin(it->dir, L"bundle.canonical.json");
            if (p && ms_exists(p)) ms_discard(p, it->dir);
            free(p);
            p = ms_wjoin(it->dir, L".state.json");
            if (p && ms_exists(p)) ms_discard(p, it->dir);
            free(p);
        }
        goto out;
    }
    text = ms_read_text(bj, (size_t)-1, &len);
    if (text) {
        mpj *j = mpj_parse(text, len);
        const mpj *v = j ? mpj_get(j, "build") : NULL;
        it->build = v && mpj_is_num(v) ? (unsigned long long)mpj_int(v) : 0;
        mpj_free(j);
    }
    if (!text || !ms_up_read_c2(text, len, &it->b, it->version, sizeof it->version, cause, sizeof cause)) {
        char why[MSU_CAUSE_CAP];
        strcpy_s(why, sizeof why, cause[0] ? cause : "it cannot be read");
        st_putf(cause, sizeof cause, "bundle.json does not read: %s", why);
        goto out;
    }
    it->b.validated = 1;
    {
        /* .state.json carries the verification result and the install time. */
        wchar_t *sp = ms_wjoin(it->dir, L".state.json");
        size_t sl = 0;
        char *s = ms_read_text(sp, 65536, &sl), *sc = NULL;
        int validated = 1;
        long long at = 0;
        if (s && sh_mb_state_parse(s, sl, &validated, &at, &sc)) {
            it->b.validated = validated;
            it->b.built = at;
            if (!validated) (void)sh_mb_set_state_cause(&it->b, sc ? sc : "");
        }
        free(sc);
        free(s);
        free(sp);
    }
    if (!ms_up_files(c, it->dir, &it->b, &unlinked, &restored, cause, sizeof cause)) goto out;
    modjson = ms_up_c2_modjson(text, len, &it->b, &mlen);
    if (!modjson || !ms_up_write(mj, modjson, mlen)) {
        ms_cause(cause, sizeof cause, "mod.json could not be written (%s)", ms_why(GetLastError(), "mod.json", 1, 0));
        goto out;
    }
    if (!ms_up_read_old(dir, &chk, NULL, 0) || strcmp(chk.hash, it->b.hash) != 0) {
        st_putf(cause, sizeof cause, "the written mod.json does not read back as bundle %.16s", it->b.hash);
        goto out;
    }
    {
        static const wchar_t *const gone[] = { L"bundle.json", L"bundle.canonical.json", L".state.json", NULL };
        int i;
        for (i = 0; gone[i]; i++) {
            wchar_t *p = ms_wjoin(it->dir, gone[i]);
            if (p && ms_exists(p)) ms_discard(p, it->dir);
            free(p);
        }
    }
    c->st->unlinked += unlinked;
    c->st->restored += restored;
    {
        char extra[160] = "";
        if (unlinked || restored)
            st_fmt(extra, sizeof extra, "; %u hard link(s) replaced by copies, %u file(s) restored from blobs\\",
                        unlinked, restored);
        ms_log("MODS: store: %s: mod.json written for %s@%s+%llu (bundle %.16s, %zu file(s)); bundle.json, "
               "bundle.canonical.json and .state.json removed%s",
               it->from, it->b.id, it->version, it->build, it->b.hash, it->b.file_count, extra);
    }
    r = 1;
out:
    if (r < 0) {
        c->st->failed++;
        ms_log("MODS: store: %s: left as it was: %s", it->from, cause[0] ? cause : "it cannot be read");
    }
    sh_mb_free(&chk);
    free(text);
    free(modjson);
    free(dir);
    free(bj);
    free(mj);
    return r;
}

static ms_up_item *ms_up_add(ms_up_ctx *c)
{
    if (c->count == c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 32;
        ms_up_item *grown = (ms_up_item *)realloc(c->items, sizeof *grown * cap);
        if (!grown) return NULL;
        c->items = grown;
        c->cap = cap;
    }
    memset(&c->items[c->count], 0, sizeof c->items[0]);
    return &c->items[c->count++];
}

static void ms_up_scan(ms_up_ctx *c, const wchar_t *dir, int legacy)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = ms_wjoin(dir, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        ms_up_item *it;
        char *leaf;
        int r;
        if (!ms_up_candidate(&fd)) continue;
        it = ms_up_add(c);
        if (!it) break;
        it->dir = ms_wjoin(dir, fd.cFileName);
        it->legacy = legacy;
        leaf = sh_mb_utf8(fd.cFileName);
        if (leaf) {
            st_fmt(it->from, sizeof it->from, "%s%s", legacy ? "legacy\\" : "", leaf);
            st_cpy(it->name, sizeof it->name, leaf);
            free(leaf);
        }
        r = it->dir ? ms_up_convert(c, it) : -1;
        if (r != 1) {
            /* Not a built folder, or left as it was: not renamed, not counted. */
            sh_mb_free(&it->b);
            free(it->dir);
            c->count--;
            continue;
        }
        {
            char key[MSU_KEY_MAX + 1];
            st_fmt(key, sizeof key, "%s@%s+%llu", it->b.id, it->version, it->build);
            it->product = legacy || !_stricmp(key, it->name);
        }
        sh_mb_content_key(&it->b, it->content);
        if (!legacy) c->st->folders++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* A name for an upgraded product folder: "<id>" when it is the only folder
 * of its id, else "<id>@<version>", then with "+<build>", then with the
 * hash; never one in use or taken by an earlier item. */
static void ms_up_pick(ms_up_ctx *c, size_t k)
{
    ms_up_item *it = &c->items[k];
    size_t i, same_id = 0;
    int round;
    for (i = 0; i < c->count; i++) if (!c->items[i].dropped && !strcmp(c->items[i].b.id, it->b.id)) same_id++;
    for (round = same_id > 1 ? 1 : 0; round < 5; round++) {
        char leaf[MSU_NAME_MAX + 1];
        wchar_t *p;
        int taken = 0;
        if (round == 0) st_cpy(leaf, sizeof leaf, it->b.id);
        else if (round == 1) st_fmt(leaf, sizeof leaf, "%s@%s", it->b.id, it->version);
        else if (round == 2) st_fmt(leaf, sizeof leaf, "%s@%s+%llu", it->b.id, it->version, it->build);
        else st_fmt(leaf, sizeof leaf, "%s@%s-%.*s", it->b.id, it->version, round == 3 ? 8 : 16, it->b.hash);
        if (!it->legacy && !_stricmp(leaf, it->name)) return;   /* already there */
        /* Folders are renamed one at a time, so the disk says what is taken. */
        p = ms_wjoin_utf8(c->root, leaf);
        taken = !p || ms_exists(p);
        free(p);
        if (!taken) { st_cpy(it->name, sizeof it->name, leaf); return; }
    }
}

/* Replace every quoted occurrence of `from` with `to` (record folder names). */
static char *ms_up_rename_text(char *text, const char *from, const char *to)
{
    char qf[MSU_NAME_MAX + 24], qt[MSU_NAME_MAX + 24];
    size_t lf, lt;
    char *hit;
    st_fmt(qf, sizeof qf, "\"%s\"", from);
    st_fmt(qt, sizeof qt, "\"%s\"", to);
    lf = strlen(qf);
    lt = strlen(qt);
    while (text && (hit = strstr(text, qf)) != NULL) {
        size_t at = (size_t)(hit - text), n = strlen(text);
        char *next = (char *)malloc(n - lf + lt + 1);
        if (!next) break;
        memcpy(next, text, at);
        memcpy(next + at, qt, lt);
        memcpy(next + at + lt, hit + lf, n - at - lf + 1);
        free(text);
        text = next;
    }
    return text;
}

/* Fold one retired product file into the record and delete it. `member`
 * names the object inside it that becomes the section (NULL: the whole). */
static int ms_up_fold(ms_up_ctx *c, const wchar_t *leaf, const char *member, const char *section, int renames)
{
    wchar_t *p = ms_wjoin(c->root, leaf);
    size_t len = 0, i;
    char *text = p ? ms_read_text(p, 64u << 20, &len) : NULL, *value = NULL;
    sh_json_object o;
    int ok = 0;
    memset(&o, 0, sizeof o);
    if (!text) { free(p); return 0; }
    if (sh_json_parse_object(text, len, 32, &o)) {
        const char *v = member ? sh_json_object_get(&o, member) : NULL;
        value = member ? (v && v[0] == '{' ? _strdup(v) : NULL) : _strdup(text);
    }
    if (value && renames) {
        for (i = 0; i < c->count; i++) {
            char from[MSU_NAME_MAX + 24];
            const ms_up_item *it = &c->items[i];
            const char *to = it->name;
            size_t k;
            if (it->dropped) to = c->items[it->kept].name;
            strcpy_s(from, sizeof from, it->from);
            for (k = 0; from[k]; k++) if (from[k] == '\\') from[k] = '/';
            if (strcmp(from, to)) value = ms_up_rename_text(value, from, to);
        }
    }
    if (value && sh_mb_record_put(c->rootu, section, value)) {
        ok = 1;
        ms_discard(p, c->root);
        c->st->records++;
        ms_log("MODS: store: %ls folded into snapmap-plus.json (%s) and removed", leaf, section);
    } else {
        ms_log("MODS: store: %ls could not be folded into snapmap-plus.json; it is left in place", leaf);
    }
    sh_json_object_free(&o);
    free(value);
    free(text);
    free(p);
    return ok;
}

static unsigned long long ms_tree_bytes(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = ms_wjoin(dir, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    unsigned long long n = 0;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) n += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* ------------------------------------------------- the second step ---- */
/* The folders of the store as the earlier reader sees them (modbundle_old.h):
 * the second step works on format-1 folders, which the game refuses. */

typedef struct ms_old_entry {
    char *folder;               /* absolute UTF-8 */
    char  name[MSU_NAME_MAX + 1];   /* under mods\ */
    sh_mb_old o;
    int   loaded;               /* its descriptor read */
    int   delivered;            /* a package a map delivered: origin "delivery", or a "legacy" record */
} ms_old_entry;

typedef struct ms_old_list { ms_old_entry *v; size_t n, cap; } ms_old_list;

static int ms_old_walk(void *ctx, const char *folder, const char *name, const char *place)
{
    ms_old_list *l = (ms_old_list *)ctx;
    ms_old_entry *e;
    if (place && place[0]) return 0;   /* a map's place holds received builds only */
    if (l->n == l->cap) {
        size_t cap = l->cap ? l->cap * 2 : 16;
        ms_old_entry *grown = (ms_old_entry *)realloc(l->v, cap * sizeof *grown);
        if (!grown) return 1;
        l->v = grown;
        l->cap = cap;
    }
    e = &l->v[l->n++];
    memset(e, 0, sizeof *e);
    e->folder = _strdup(folder);
    st_cpy(e->name, sizeof e->name, name);
    e->loaded = e->folder && sh_mb_old_load(e->folder, &e->o) && e->o.key[0];
    e->delivered = e->loaded && (!strcmp(e->o.origin, "delivery") || e->o.block_legacy != NULL);
    return 0;
}

static void ms_old_read(ms_old_list *l)
{
    memset(l, 0, sizeof *l);
    sh_ms_walk(ms_old_walk, l);
}

static void ms_old_free(ms_old_list *l)
{
    size_t i;
    for (i = 0; i < l->n; i++) { sh_mb_old_free(&l->v[i].o); free(l->v[i].folder); }
    free(l->v);
    memset(l, 0, sizeof *l);
}

/* Whether every carried path of `part` (at least one) is one of `big`'s. */
static int ms_old_covers(const sh_mb_old *big, const sh_mb_old *part)
{
    size_t i, k, n = 0;
    for (i = 0; i < part->file_count; i++) {
        int hit = 0;
        if (part->files[i].pointer || !part->files[i].path) continue;
        n++;
        for (k = 0; k < big->file_count && !hit; k++)
            hit = big->files[k].path && !_stricmp(big->files[k].path, part->files[i].path);
        if (!hit) return 0;
    }
    return n > 0;
}

static size_t ms_old_carried(const sh_mb_old *o)
{
    size_t i, n = 0;
    for (i = 0; i < o->file_count; i++) if (!o->files[i].pointer) n++;
    return n;
}

/* The installed mod a delivered package `d` is a copy of, or NULL: a folder
 * that was not itself delivered, declaring d's key (rule 1: one whose paths
 * cover d's first, then a validated one), else one under another key
 * providing every path d provides (rule 2, the one with the fewest files). */
static const ms_old_entry *ms_old_counterpart(const ms_old_list *l, const ms_old_entry *d, int *rule)
{
    const ms_old_entry *cover = NULL, *usable = NULL, *any = NULL, *other = NULL;
    size_t i;
    if (rule) *rule = 0;
    for (i = 0; i < l->n; i++) {
        const ms_old_entry *e = &l->v[i];
        if (!e->loaded || e->delivered || e == d) continue;
        if (!strcmp(e->o.key, d->o.key)) {
            if (!cover && ms_old_covers(&e->o, &d->o)) cover = e;
            if (!usable && e->o.validated) usable = e;
            if (!any) any = e;
        } else if (ms_old_covers(&e->o, &d->o) && (!other || ms_old_carried(&e->o) < ms_old_carried(&other->o))) {
            other = e;
        }
    }
    if (cover || usable || any) {
        if (rule) *rule = 1;
        return cover ? cover : usable ? usable : any;
    }
    if (other && rule) *rule = 2;
    return other;
}

/* A folder name the second step changed: `from` now answers as `to`. */
typedef struct ms_pair {
    char from[MSU_NAME_MAX + 1];
    char to[MSU_NAME_MAX + 1];
} ms_pair;

/* The 64-hex tree a delivered package's legacy record keys it by
 * ("delivery.<tree>"), or 0. */
static int ms_delivery_tree(const sh_mb_old *o, char tree[SH_MB_HASH_HEX + 1])
{
    int pass, ok = 0;
    for (pass = 0; pass < 2 && !ok; pass++) {
        const char *text = pass ? o->retired : o->block_legacy;
        mpj *x = text ? mpj_parse(text, strlen(text)) : NULL;
        const mpj *lg = !x ? NULL : pass ? mpj_get(x, "legacy") : x, *k = lg ? mpj_get(lg, "key") : NULL;
        ok = k && k->type == MPJ_STR && k->text && !strncmp(k->text, "delivery.", 9) && ms_is_hash(k->text + 9);
        if (ok) memcpy(tree, k->text + 9, SH_MB_HASH_HEX + 1);
        mpj_free(x);
    }
    return ok;
}

static const char *ms_pair_apply(const ms_pair *p, size_t n, const char *name)
{
    size_t i;
    for (i = 0; i < n; i++) if (!strcmp(p[i].from, name)) name = p[i].to;
    return name;
}

static int ms_outcome_done(const mpj *row)
{
    const mpj *o = mpj_get(row, "outcome");
    return mpj_str_is(o, "built") || mpj_str_is(o, "converted") || mpj_str_is(o, "reused");
}

/* The migration section after the drops and renames; collapsed once complete. */
static void ms_tidy_migration(const char *rootu, const ms_pair *p, size_t np, mpj *trees_add, sh_ms_upgrade_stats *st)
{
    char *text = NULL, *out;
    mpj *m, *next;
    size_t len = 0;
    int i, k, complete, dropped = 0;
    if (sh_mb_record_get(rootu, "migration", &text) != 1 || !text) { free(text); return; }
    m = mpj_parse(text, strlen(text));
    free(text);
    if (!m || m->type != MPJ_OBJ || !(next = mpj_new(MPJ_OBJ))) { mpj_free(m); return; }
    {
        const mpj *c = mpj_get(m, "complete");
        complete = c && c->type == MPJ_BOOL && c->b;
    }
    for (i = 0; i < m->n; i++) {
        const char *key = m->keys[i];
        const mpj *v = m->items[i];
        mpj *copy = NULL;
        if (!strcmp(key, "trees") && v->type == MPJ_OBJ) {
            copy = mpj_new(MPJ_OBJ);
            for (k = 0; copy && k < v->n; k++) {
                const mpj *to = v->items[k];
                const char *name = to->type == MPJ_STR && to->text ? ms_pair_apply(p, np, to->text) : NULL;
                if (!trees_add || !mpj_get(trees_add, v->keys[k]))
                    mpj_set(copy, v->keys[k], name ? mpj_new_str(name) : mpj_clone(to));
            }
            for (k = 0; copy && trees_add && k < trees_add->n; k++) {
                const mpj *to = trees_add->items[k];
                mpj_set(copy, trees_add->keys[k], to->type == MPJ_STR && to->text ? mpj_new_str(ms_pair_apply(p, np, to->text))
                                                                                  : mpj_clone(to));
            }
        } else if (!strcmp(key, "folders") && v->type == MPJ_ARR) {
            copy = mpj_new(MPJ_ARR);
            for (k = 0; copy && k < v->n; k++) {
                mpj *row;
                const mpj *store;
                if (complete && ms_outcome_done(v->items[k])) { dropped++; continue; }
                row = mpj_clone(v->items[k]);
                store = row ? mpj_get(row, "store") : NULL;
                if (store && store->type == MPJ_STR && store->text && strcmp(ms_pair_apply(p, np, store->text), store->text))
                    mpj_set(row, "store", mpj_new_str(ms_pair_apply(p, np, store->text)));
                if (row) mpj_push(copy, row);
            }
        } else if (!complete || !strcmp(key, "complete") || !strcmp(key, "migratedAt") || !strcmp(key, "source")) {
            copy = mpj_clone(v);
        } else {
            dropped++;   /* a counter of the finished pass */
        }
        if (copy) mpj_set(next, key, copy);
    }
    if (complete && !mpj_get(next, "folders")) mpj_set(next, "folders", mpj_new(MPJ_ARR));
    out = mpj_dump(next, &len);
    if (out && sh_mb_record_put(rootu, "migration", out)) st->migration_collapsed = complete && dropped > 0;
    free(out);
    mpj_free(next);
    mpj_free(m);
}

/* Rename one product folder; 1 when it moved. */
static int ms_tidy_move(const char *folder, const char *rootu, const char *to)
{
    char path[MAX_PATH * 4];
    wchar_t *from = sh_mb_wpath(folder), *dst;
    int ok;
    st_fmt(path, sizeof path, "%s\\%s", rootu, to);
    dst = sh_mb_wpath(path);
    ok = from && dst && !ms_exists(dst) && MoveFileExW(from, dst, 0);
    free(from);
    free(dst);
    return ok;
}

static int ms_tidy(sh_ms_upgrade_stats *st, const char *rootu)
{
    char *text = NULL;
    wchar_t *wroot;
    ms_old_list l;
    ms_pair *pairs = NULL;
    size_t np = 0, i;
    mpj *trees_add;
    ULONGLONG t0 = GetTickCount64();
    int done = sh_mb_record_get(rootu, "layout", &text) == 1 && text && atoi(text) >= 2;
    free(text);
    if (done) return 0;
    modstore_lock();
    st->tidied = 1;
    trees_add = mpj_new(MPJ_OBJ);
    wroot = ms_wroot();
    if (!wroot) { modstore_unlock(); mpj_free(trees_add); return -1; }
    ms_old_read(&l);
    pairs = (ms_pair *)calloc(l.n * 2 + 1, sizeof *pairs);
    if (!pairs) { ms_old_free(&l); free(wroot); modstore_unlock(); mpj_free(trees_add); return -1; }

    /* 1. A package a map delivered that copies an installed mod goes. */
    for (i = 0; i < l.n; i++) {
        ms_old_entry *d = &l.v[i];
        const ms_old_entry *m;
        char tree[SH_MB_HASH_HEX + 1];
        int rule = 0;
        if (!d->delivered) continue;
        m = ms_old_counterpart(&l, d, &rule);
        if (!m) continue;
        {
            wchar_t *w = sh_mb_wpath(d->folder);
            int gone = w && ms_discard(w, wroot);
            free(w);
            if (!gone) {
                st->tidy_failed++;
                ms_log("MODS: store: %s: a copy of %s that a map delivered, but it could not be removed; it stays", d->name,
                       m->name);
                continue;
            }
        }
        strcpy_s(pairs[np].from, sizeof pairs[np].from, d->name);
        strcpy_s(pairs[np].to, sizeof pairs[np].to, m->name);
        np++;
        if (trees_add && ms_delivery_tree(&d->o, tree)) mpj_set(trees_add, tree, mpj_new_str(m->name));
        d->loaded = 0;   /* gone */
        if (rule == 1) {
            st->same_id++;
            ms_log("MODS: store: %s: dropped, delivered by a map under the id of the installed %s", d->name, m->name);
        } else {
            st->covered++;
            ms_log("MODS: store: %s: dropped, delivered by a map as %s; the installed %s (%s) provides all %zu of its "
                   "path(s)", d->name, d->o.key, m->name, m->o.key, ms_old_carried(&d->o));
        }
    }

    /* 2. Names: a product folder alone with its id is "<id>"; a generated
     * version never names a folder. */
    for (i = 0; i < l.n; i++) {
        const ms_old_entry *e = &l.v[i];
        char to[MSU_NAME_MAX + 1], prefix[80];
        size_t k, same = 0, pl;
        int generated;
        if (!e->loaded || !e->o.origin[0] || !_stricmp(e->name, e->o.key)) continue;
        if (strchr(e->name, '\\')) continue;   /* a folder the player nested stays where it is */
        for (k = 0; k < l.n; k++) if (l.v[k].loaded && !strcmp(l.v[k].o.key, e->o.key)) same++;
        st_fmt(prefix, sizeof prefix, "%s@", e->o.key);
        pl = strlen(prefix);
        generated = strstr(e->name, "@legacy-") != NULL;
        if (same == 1 && !_strnicmp(e->name, prefix, pl) && ms_tidy_move(e->folder, rootu, e->o.key))
            strcpy_s(to, sizeof to, e->o.key);
        else if (generated) {
            st_fmt(to, sizeof to, "%s-%.8s", e->o.key, e->o.bundle);
            if (!ms_tidy_move(e->folder, rootu, to)) {
                st_fmt(to, sizeof to, "%s-%.16s", e->o.key, e->o.bundle);
                if (!ms_tidy_move(e->folder, rootu, to)) {
                    st->tidy_failed++;
                    ms_log("MODS: store: %s could not be renamed (%s); it keeps its name", e->name, ms_why(GetLastError(), e->name, 1, 0));
                    continue;
                }
            }
        } else continue;
        st->tidy_renamed++;
        ms_log("MODS: store: %s -> %s", e->name, to);
        strcpy_s(pairs[np].from, sizeof pairs[np].from, e->name);
        strcpy_s(pairs[np].to, sizeof pairs[np].to, to);
        np++;
    }
    ms_old_free(&l);

    /* 3. The migration record. */
    ms_tidy_migration(rootu, pairs, np, trees_add, st);
    if (!st->tidy_failed) sh_mb_record_put(rootu, "layout", "2");
    st->tidy_ms = GetTickCount64() - t0;
    ms_log("MODS: store: tidied in %llu ms: %u package(s) a map delivered dropped as copies of an installed mod (%u by "
           "id, %u by paths), %u folder(s) renamed, the migration record %s%s",
           st->tidy_ms, st->same_id + st->covered, st->same_id, st->covered, st->tidy_renamed,
           st->migration_collapsed ? "keeps only the folders not yet done" : "unchanged",
           st->tidy_failed ? "; some folders were left as they were, so this runs again at the next start" : "");
    modstore_unlock();
    free(pairs);
    free(wroot);
    mpj_free(trees_add);
    return 1;
}

int sh_ms_upgrade(sh_ms_upgrade_stats *stats)
{
    sh_ms_upgrade_stats local;
    ms_up_ctx c;
    wchar_t *legacy;
    ULONGLONG t0 = GetTickCount64();
    size_t i, k;
    if (!stats) stats = &local;
    memset(stats, 0, sizeof *stats);
    memset(&c, 0, sizeof c);
    c.st = stats;
    c.root = ms_wroot();
    if (!c.root || !st_path(c.rootu, sizeof c.rootu, sh_ms_root())) { free(c.root); return -1; }
    if (!ms_exists(c.root)) { free(c.root); return 0; }
    if (!ms_up_needed(c.root)) {
        int tidied = ms_tidy(stats, c.rootu);
        free(c.root);
        return tidied;
    }
    modstore_lock();
    stats->ran = 1;
    c.blobs = ms_wjoin(c.root, L"blobs");
    if (c.blobs && !ms_is_dir(c.blobs)) { free(c.blobs); c.blobs = NULL; }
    ms_log("MODS: store: upgrading %s to the mod.json layout (Contract 5)", c.rootu);

    /* 1. Every built folder, then every delivered package, converted in place. */
    ms_up_scan(&c, c.root, 0);
    legacy = ms_wjoin(c.root, L"legacy");
    if (legacy && ms_is_dir(legacy)) ms_up_scan(&c, legacy, 1);

    /* 2. A delivered copy that an installed mod already serves is dropped: the
     * same bundle hash, or the same content. */
    for (i = 0; i < c.count; i++) {
        ms_up_item *it = &c.items[i];
        if (!it->legacy) continue;
        for (k = 0; k < c.count && !it->dropped; k++) {
            const ms_up_item *o = &c.items[k];
            if (k == i || o->dropped || (o->legacy && k > i)) continue;
            if (!strcmp(o->b.hash, it->b.hash) || (!strcmp(o->b.id, it->b.id) && !strcmp(o->content, it->content))) {
                it->dropped = 1;
                it->kept = k;
                ms_log("MODS: store: %s: dropped, %s serves the same content (bundle %.16s)", it->from, o->from, it->b.hash);
                if (!ms_discard(it->dir, c.root)) {
                    /* Served by the kept one; the next run drops it again. */
                    ms_log("MODS: store: %s could not be removed (%s); it stays for the next run", it->from,
                           ms_why(GetLastError(), it->from, 1, 0));
                    stats->failed++;
                }
                stats->dropped++;
            }
        }
    }
    /* 3. Names: product folders become <id> (or <id>@<version>); delivered
     * packages move out of legacy\ the same way. */
    for (i = 0; i < c.count; i++) {
        ms_up_item *it = &c.items[i];
        wchar_t *to;
        char before[MSU_NAME_MAX + 16];
        if (it->dropped || !it->product) continue;
        strcpy_s(before, sizeof before, it->name);
        ms_up_pick(&c, i);
        if (!it->legacy && !_stricmp(before, it->name)) continue;
        to = ms_wjoin_utf8(c.root, it->name);
        if (to && MoveFileExW(it->dir, to, 0)) {
            free(it->dir);
            it->dir = to;
            if (it->legacy) stats->delivered++;
            else stats->renamed++;
            ms_log("MODS: store: %s -> %s", it->from, it->name);
        } else {
            DWORD err = GetLastError();
            free(to);
            strcpy_s(it->name, sizeof it->name, before);
            ms_log("MODS: store: %s could not be renamed to its new folder (%s); it keeps its name", it->from, ms_why(err, it->from, 1, 0));
            if (it->legacy) stats->failed++;
        }
    }
    if (legacy && ms_is_dir(legacy) && RemoveDirectoryW(legacy)) ms_log("MODS: store: legacy\\ removed (empty)");

    /* 4. blobs\ goes when every folder holds its own files. */
    if (c.blobs) {
        if (!stats->failed) {
            stats->blob_bytes = ms_tree_bytes(c.blobs);
            stats->blobs_removed = ms_discard(c.blobs, c.root);
            ms_log("MODS: store: blobs\\ %s (%llu MiB)", stats->blobs_removed ? "removed" : "could not be removed completely",
                   stats->blob_bytes >> 20);
        } else {
            ms_log("MODS: store: blobs\\ kept: %u folder(s) of the retired layout still need it", stats->failed);
        }
    }

    /* 5. The product's files become sections of the one record. */
    if (ms_has_file(c.root, L"claims.json")) ms_up_fold(&c, L"claims.json", "claims", "claims", 0);
    if (ms_has_file(c.root, L"publish-links.json")) ms_up_fold(&c, L"publish-links.json", "links", "links", 0);
    if (ms_has_file(c.root, L"pending-uploads.json")) ms_up_fold(&c, L"pending-uploads.json", "pending", "pending", 0);
    if (ms_has_file(c.root, L".migrated.json")) ms_up_fold(&c, L".migrated.json", NULL, "migration", 1);
    stats->ms = GetTickCount64() - t0;
    ms_log("MODS: store: upgraded to the mod.json layout in %llu ms: %u folder(s) converted (%u renamed), %u delivered "
           "package(s) moved out of legacy\\, %u dropped as copies of an installed mod, %u hard link(s) replaced by copies, "
           "%u file(s) restored from blobs\\, blobs\\ %s, %u record file(s) folded into snapmap-plus.json, %u folder(s) "
           "left as they were",
           stats->ms, stats->folders, stats->renamed, stats->delivered, stats->dropped, stats->unlinked, stats->restored,
           stats->blobs_removed ? "removed" : (c.blobs ? "kept" : "absent"), stats->records, stats->failed);
    ms_tidy(stats, c.rootu);
    modstore_unlock();
    for (i = 0; i < c.count; i++) { sh_mb_free(&c.items[i].b); free(c.items[i].dir); }
    free(c.items);
    free(legacy);
    free(c.blobs);
    free(c.root);
    return 1;
}

/* ------------------------------------------------------ the dry run ---- */

static unsigned ms_count_children(const wchar_t *dir, int folders_only, unsigned long long *bytes)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = ms_wjoin(dir, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    unsigned n = 0;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (folders_only && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        n++;
        if (bytes && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            *bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

/* What the second step would change: drops, renames, a migration record to collapse. */
static void ms_check_tidy(const char *rootu, sh_ms_upgrade_check *out)
{
    char *text = NULL;
    ms_old_list l;
    size_t i, k;
    unsigned char *dropped;
    if (sh_mb_record_get(rootu, "layout", &text) == 1 && text && atoi(text) >= 2) { free(text); return; }
    free(text);
    text = NULL;
    ms_old_read(&l);
    dropped = (unsigned char *)calloc(l.n + 1, 1);
    for (i = 0; dropped && i < l.n; i++) {
        int rule = 0;
        if (l.v[i].delivered && ms_old_counterpart(&l, &l.v[i], &rule)) {
            dropped[i] = 1;
            out->tidy_drops++;
        }
    }
    for (i = 0; dropped && i < l.n; i++) {
        const ms_old_entry *e = &l.v[i];
        char prefix[80];
        size_t same = 0;
        if (dropped[i] || !e->loaded || !e->o.origin[0] || !_stricmp(e->name, e->o.key)) continue;
        for (k = 0; k < l.n; k++) if (!dropped[k] && l.v[k].loaded && !strcmp(l.v[k].o.key, e->o.key)) same++;
        st_fmt(prefix, sizeof prefix, "%s@", e->o.key);
        if (strchr(e->name, '\\')) continue;
        if ((same == 1 && !_strnicmp(e->name, prefix, strlen(prefix))) || strstr(e->name, "@legacy-")) out->tidy_renames++;
    }
    free(dropped);
    ms_old_free(&l);
    if (sh_mb_record_get(rootu, "migration", &text) == 1 && text) {
        mpj *m = mpj_parse(text, strlen(text));
        const mpj *c = m ? mpj_get(m, "complete") : NULL, *rows = m ? mpj_get(m, "folders") : NULL;
        int n;
        if (c && c->type == MPJ_BOOL && c->b) {
            for (n = 0; n < m->n; n++) {
                const char *key = m->keys[n];
                if (strcmp(key, "complete") && strcmp(key, "migratedAt") && strcmp(key, "source") && strcmp(key, "folders") &&
                    strcmp(key, "trees")) out->tidy_migration = 1;
            }
            for (n = 0; rows && rows->type == MPJ_ARR && n < rows->n; n++)
                if (ms_outcome_done(rows->items[n])) out->tidy_migration = 1;
        }
        mpj_free(m);
    }
    free(text);
}

int sh_ms_upgrade_scan(sh_ms_upgrade_check *out)
{
    static const struct { const wchar_t *w; const char *u; } records[] = {
        { L"claims.json", "claims.json" }, { L"publish-links.json", "publish-links.json" },
        { L"pending-uploads.json", "pending-uploads.json" }, { L".migrated.json", ".migrated.json" }, { NULL, NULL } };
    wchar_t *root, *p, *pattern, *slash;
    char rootu[MAX_PATH * 4], *section = NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int i;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    root = ms_wroot();
    if (!root || !st_path(rootu, sizeof rootu, sh_ms_root())) { free(root); return -1; }
    /* removed-mods\ is the store root's sibling. */
    slash = wcsrchr(root, L'\\');
    if (slash && slash > root) {
        size_t n = (size_t)(slash - root);
        wchar_t *bin = (wchar_t *)malloc((n + 16) * sizeof(wchar_t));
        if (bin) {
            memcpy(bin, root, n * sizeof(wchar_t));
            wcscpy_s(bin + n, 16, L"\\removed-mods");
            if (ms_is_dir(bin)) { out->bin_present = 1; out->bin_entries = ms_count_children(bin, 0, NULL); }
            free(bin);
        }
    }
    if (!ms_is_dir(root)) { free(root); return out->bin_present; }
    if (sh_mb_record_get(rootu, "removed", &section) == 1 && section) out->bin_section = 1;
    free(section);
    p = ms_wjoin(root, L"blobs");
    if (p && ms_is_dir(p)) { out->blobs_present = 1; out->blob_files = ms_count_children(p, 0, &out->blob_bytes); }
    free(p);
    p = ms_wjoin(root, L"legacy");
    if (p && ms_is_dir(p)) { out->legacy_present = 1; out->legacy_folders = ms_count_children(p, 1, NULL); }
    free(p);
    for (i = 0; records[i].w; i++) {
        if (!ms_has_file(root, records[i].w)) continue;
        out->record_files++;
        if (out->record_names[0]) strncat_s(out->record_names, sizeof out->record_names, ", ", _TRUNCATE);
        strncat_s(out->record_names, sizeof out->record_names, records[i].u, _TRUNCATE);
    }
    pattern = ms_wjoin(root, L"*");
    h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *child;
            if (!ms_up_candidate(&fd)) continue;
            child = ms_wjoin(root, fd.cFileName);
            if (child && (ms_has_file(child, L"bundle.json") || ms_has_file(child, L".state.json") ||
                          ms_has_file(child, L"bundle.canonical.json"))) out->layout_folders++;
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    ms_check_tidy(rootu, out);
    free(root);
    return (out->layout_folders > 0) + out->blobs_present + out->legacy_present + (out->record_files > 0) +
           (out->tidy_drops + out->tidy_renames > 0) + out->tidy_migration + out->bin_present + out->bin_section;
}
