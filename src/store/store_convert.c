/* store_convert.c -- see store_convert.h. Store tool only. */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "store_convert.h"
#include "store_deflate.h"
#include "store_saves.h"
#include "modstore.h"
#include "modbundle.h"
#include "modbundle_old.h"
#include "modlegacy.h"
#include "modmanifest.h"
#include "modstock.h"
#include "map_old.h"
#include "prefab_block.h"
#include "mp_json.h"

#define CV_PATH      (MAX_PATH * 4)
#define CV_NEST_MAX  8
#define CV_MAP_MAX   ((size_t)512 << 20)
#define CV_DESC_MAX  ((size_t)16 << 20)

/* ------------------------------------------------------------ output ---- */

static void cv_say(const sh_conv_opts *o, int err, const char *fmt, ...)
{
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (o && o->line) o->line(o->line_context, err, line);
}

/* ------------------------------------------------------------- files ---- */

static char *cv_join(const char *a, const char *b)
{
    size_t n = strlen(a) + strlen(b) + 2;
    char *p = (char *)malloc(n);
    if (p) _snprintf_s(p, n, _TRUNCATE, "%s\\%s", a, b);
    return p;
}

static int cv_attr(const char *utf8, DWORD *attr)
{
    wchar_t *w = sh_mb_wpath(utf8);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    if (attr) *attr = a;
    return a != INVALID_FILE_ATTRIBUTES;
}

static int cv_is_dir(const char *utf8)
{
    DWORD a;
    return cv_attr(utf8, &a) && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static int cv_is_file(const char *dir, const char *leaf)
{
    char *p = cv_join(dir, leaf);
    DWORD a;
    int r = p && cv_attr(p, &a) && !(a & FILE_ATTRIBUTE_DIRECTORY);
    free(p);
    return r;
}

/* Remove a tree this tool made (a link is removed as a link). */
static void cv_rmtree_w(const wchar_t *dir)
{
    size_t n = wcslen(dir) + MAX_PATH + 4;
    wchar_t *pattern = (wchar_t *)malloc(sizeof(wchar_t) * n), *child = (wchar_t *)malloc(sizeof(wchar_t) * n);
    WIN32_FIND_DATAW fd;
    HANDLE h = INVALID_HANDLE_VALUE;
    if (pattern && child) {
        _snwprintf_s(pattern, n, _TRUNCATE, L"%s\\*", dir);
        h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    }
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            _snwprintf_s(child, n, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                cv_rmtree_w(child);
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryW(child);
            } else {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child);
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
    free(pattern);
    free(child);
}

static void cv_rmtree(const char *utf8)
{
    wchar_t *w = sh_mb_wpath(utf8);
    if (w) cv_rmtree_w(w);
    free(w);
}

static unsigned char *cv_read(const char *path, size_t max, size_t *len)
{
    return sh_mb_read_file(path, max, len);
}

/* Write `path` whole through a temporary beside it and a replacing rename. */
static int cv_write(const char *path, const void *data, size_t n)
{
    char *tmp = (char *)malloc(strlen(path) + 32);
    wchar_t *wt, *wp;
    HANDLE h;
    DWORD done = 0;
    int ok = 0;
    if (!tmp) return 0;
    _snprintf_s(tmp, strlen(path) + 32, _TRUNCATE, "%s.snapmap-plus-tmp", path);
    wt = sh_mb_wpath(tmp);
    wp = sh_mb_wpath(path);
    if (wt && wp) {
        h = CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            ok = (n == 0 || (WriteFile(h, data, (DWORD)n, &done, NULL) && done == n)) && FlushFileBuffers(h);
            CloseHandle(h);
            if (ok) ok = MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
            if (!ok) DeleteFileW(wt);
        }
    }
    free(wt);
    free(wp);
    free(tmp);
    return ok;
}

/* A copy of `path` as it was, beside it; the first copy is kept. */
static int cv_backup(const char *path)
{
    char *bak = (char *)malloc(strlen(path) + sizeof SH_CONV_BACKUP_SUFFIX);
    wchar_t *wb, *wp;
    int ok = 0;
    if (!bak) return 0;
    _snprintf_s(bak, strlen(path) + sizeof SH_CONV_BACKUP_SUFFIX, _TRUNCATE, "%s%s", path, SH_CONV_BACKUP_SUFFIX);
    wb = sh_mb_wpath(bak);
    wp = sh_mb_wpath(path);
    if (wb && wp) ok = CopyFileW(wp, wb, TRUE) || GetLastError() == ERROR_FILE_EXISTS;
    free(wb);
    free(wp);
    free(bak);
    return ok;
}

static void cv_hex_sha(const unsigned char *bytes, size_t n, char out[65])
{
    unsigned char d[32];
    int i;
    out[0] = 0;
    if (!sh_store_sha256(bytes, n, d)) return;
    for (i = 0; i < 32; i++) _snprintf_s(out + i * 2, 3, _TRUNCATE, "%02x", d[i]);
}

/* ------------------------------------------------------------ record ---- */

/* A JSON string body for `s` (", \ and control characters escaped). */
static char *cv_esc(const char *s)
{
    size_t n = strlen(s), i, w = 0;
    char *o = (char *)malloc(n * 6 + 1);
    if (!o) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') { o[w++] = '\\'; o[w++] = (char)c; }
        else if (c < 0x20) w += (size_t)_snprintf_s(o + w, 7, _TRUNCATE, "\\u%04x", c);
        else o[w++] = (char)c;
    }
    o[w] = 0;
    return o;
}

typedef struct cv_rec { mpj *root; } cv_rec;

static mpj *cv_rec_obj(cv_rec *r, const char *name)
{
    mpj *v = r->root ? mpj_get(r->root, name) : NULL;
    if (v && v->type == MPJ_OBJ) return v;
    if (!r->root) return NULL;
    if (!mpj_set(r->root, name, mpj_new(MPJ_OBJ))) return NULL;
    return mpj_get(r->root, name);
}

static void cv_rec_load(const sh_conv_opts *o, cv_rec *r)
{
    char *text = NULL;
    r->root = NULL;
    if (sh_mb_record_get(o->mods, SH_CONV_RECORD, &text) == 1 && text) r->root = mpj_parse(text, strlen(text));
    free(text);
    if (!r->root || r->root->type != MPJ_OBJ) { mpj_free(r->root); r->root = mpj_new(MPJ_OBJ); }
}

static int cv_rec_save(const sh_conv_opts *o, cv_rec *r)
{
    char *text = r->root ? mpj_dump(r->root, NULL) : NULL;
    int ok = text && sh_mb_record_put(o->mods, SH_CONV_RECORD, text);
    free(text);
    return ok;
}

static void cv_rec_free(cv_rec *r) { mpj_free(r->root); r->root = NULL; }

/* Set name[key] = value (both plain text). */
static void cv_rec_set(cv_rec *r, const char *name, const char *key, const char *value)
{
    mpj *obj = cv_rec_obj(r, name);
    char *k = cv_esc(key), *v = value ? cv_esc(value) : NULL;
    if (obj && k && (!value || v)) {
        if (value) mpj_set(obj, k, mpj_new_str(v));
        else {
            int i;
            for (i = 0; i < obj->n; i++)
                if (!strcmp(obj->keys[i], k)) {
                    int j;
                    mpj_free(obj->items[i]);
                    free(obj->keys[i]);
                    for (j = i + 1; j < obj->n; j++) { obj->items[j - 1] = obj->items[j]; obj->keys[j - 1] = obj->keys[j]; }
                    obj->n--;
                    break;
                }
        }
    }
    free(k);
    free(v);
}

/* name[key] as plain text into out (salts, hashes: never escaped). 1 when present. */
static int cv_rec_get(cv_rec *r, const char *name, const char *key, char *out, size_t cap)
{
    mpj *obj = r->root ? mpj_get(r->root, name) : NULL, *v;
    char *k = cv_esc(key);
    int ok = 0;
    if (out && cap) out[0] = 0;
    if (obj && obj->type == MPJ_OBJ && k && (v = mpj_get(obj, k)) != NULL && v->type == MPJ_STR) {
        strncpy_s(out, cap, v->text, _TRUNCATE);
        ok = 1;
    }
    free(k);
    return ok;
}

/* A key lookup without case (old keys are lower-cased, a map's may not be). */
static int cv_rec_get_key(cv_rec *r, const char *key, char *out, size_t cap)
{
    mpj *obj = r->root ? mpj_get(r->root, "keys") : NULL;
    int i;
    if (out && cap) out[0] = 0;
    if (!obj || obj->type != MPJ_OBJ) return 0;
    for (i = 0; i < obj->n; i++)
        if (!_stricmp(obj->keys[i], key) && obj->items[i]->type == MPJ_STR) {
            strncpy_s(out, cap, obj->items[i]->text, _TRUNCATE);
            return 1;
        }
    return 0;
}

/* ----------------------------------------------- the fresh author part ---- */
/* The descriptor's top-level members by byte spans, so cutting the retired
 * ones keeps every other byte the author wrote. */

static const char *cv_ws(const char *p, const char *e)
{
    while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static const char *cv_str(const char *p, const char *e)
{
    if (p >= e || *p != '"') return NULL;
    for (p++; p < e; p++) {
        if (*p == '\\') { p++; continue; }
        if (*p == '"') return p + 1;
    }
    return NULL;
}

static const char *cv_value(const char *p, const char *e, int depth)
{
    p = cv_ws(p, e);
    if (p >= e || depth > 96) return NULL;
    if (*p == '"') return cv_str(p, e);
    if (*p == '{' || *p == '[') {
        char close = *p == '{' ? '}' : ']';
        int obj = *p == '{';
        p = cv_ws(p + 1, e);
        if (p < e && *p == close) return p + 1;
        for (;;) {
            if (obj) {
                p = cv_str(cv_ws(p, e), e);
                p = p ? cv_ws(p, e) : NULL;
                if (!p || p >= e || *p != ':') return NULL;
                p++;
            }
            p = cv_value(p, e, depth + 1);
            p = p ? cv_ws(p, e) : NULL;
            if (!p || p >= e) return NULL;
            if (*p == close) return p + 1;
            if (*p != ',') return NULL;
            p++;
        }
    }
    while (p < e && !strchr(",}] \t\r\n", *p)) p++;
    return p;
}

typedef struct cv_member { size_t key_at, key_end, value_at, value_end; } cv_member;

/* The members of the object at text[at]; malloc'd array, count in *n; NULL
 * when it does not read (an empty object gives a 1-element dummy with n 0). */
static cv_member *cv_members(const char *text, size_t len, size_t at, size_t *n)
{
    const char *e = text + len, *p = cv_ws(text + at, e);
    cv_member *m = NULL;
    size_t cap = 0;
    *n = 0;
    if (p >= e || *p != '{') return NULL;
    p = cv_ws(p + 1, e);
    m = (cv_member *)malloc(sizeof *m * (cap = 16));
    if (!m) return NULL;
    if (p < e && *p == '}') return m;
    for (;;) {
        const char *k = cv_ws(p, e), *ke = cv_str(k, e), *v, *ve;
        if (!ke) break;
        p = cv_ws(ke, e);
        if (p >= e || *p != ':') break;
        v = cv_ws(p + 1, e);
        ve = cv_value(v, e, 1);
        if (!ve) break;
        if (*n == cap) {
            cv_member *g = (cv_member *)realloc(m, sizeof *m * (cap *= 2));
            if (!g) break;
            m = g;
        }
        m[*n].key_at = (size_t)(k - text);
        m[*n].key_end = (size_t)(ke - text);
        m[*n].value_at = (size_t)(v - text);
        m[*n].value_end = (size_t)(ve - text);
        (*n)++;
        p = cv_ws(ve, e);
        if (p < e && *p == '}') return m;
        if (p >= e || *p != ',') break;
        p++;
    }
    free(m);
    *n = 0;
    return NULL;
}

static int cv_key_is(const char *text, const cv_member *m, const char *name)
{
    size_t n = strlen(name);
    return m->key_end - m->key_at == n + 2 && !memcmp(text + m->key_at + 1, name, n);
}

/* The text with the members of the object at `at` that `cut` names removed
 * (the separator before or after each with it). malloc'd. */
static char *cv_cut(const char *text, size_t len, size_t at, const char *const *cut, size_t *out_len, unsigned *count)
{
    size_t n = 0, i, w = 0, from = 0;
    cv_member *m = cv_members(text, len, at, &n);
    char *out;
    unsigned char *drop;
    if (count) *count = 0;
    if (!m) return NULL;
    drop = (unsigned char *)calloc(n + 1, 1);
    out = (char *)malloc(len + 1);
    if (!drop || !out) { free(m); free(drop); free(out); return NULL; }
    for (i = 0; i < n; i++) {
        int k;
        for (k = 0; cut[k]; k++) if (cv_key_is(text, &m[i], cut[k])) { drop[i] = 1; if (count) (*count)++; break; }
    }
    for (i = 0; i < n; i++) {
        size_t a, b;
        long j;
        if (!drop[i]) continue;
        for (j = (long)i - 1; j >= 0 && drop[j]; j--) {}
        if (j >= 0) {
            /* After a kept member: its comma goes with this one. */
            a = m[j].value_end;
            b = m[i].value_end;
        } else {
            /* Before any kept member: up to the next member. */
            a = m[i].key_at;
            b = i + 1 < n ? m[i + 1].key_at : m[i].value_end;
        }
        if (a < from) a = from;
        memcpy(out + w, text + from, a - from);
        w += a - from;
        if (b > from) from = b;
    }
    memcpy(out + w, text + from, len - from);
    w += len - from;
    out[w] = 0;
    free(m);
    free(drop);
    if (out_len) *out_len = w;
    return out;
}

static const char *const CV_RETIRED[] = { "id", "key", "shadows", "stock", "scope", "player", "extends", "replaces", "legacy",
                                          "composes", "schema", "format", SH_MB_BLOCK_KEY, NULL };

/* A fresh author part from an earlier descriptor: the block and the retired
 * author keys cut, a hud's "schema" cut, a "name" given when it has none. */
static char *cv_fresh(const char *text, size_t len, const char *key, size_t *out_len, unsigned *retired)
{
    static const char *const schema[] = { "schema", NULL };
    size_t skip = len >= 3 && !memcmp(text, "\xef\xbb\xbf", 3) ? 3 : 0, n = 0, i, l1 = 0;
    char *t1 = cv_cut(text, len, skip, CV_RETIRED, &l1, retired), *t2 = NULL;
    cv_member *m;
    if (!t1) return NULL;
    m = cv_members(t1, l1, skip, &n);
    for (i = 0; m && i < n; i++) {
        if (cv_key_is(t1, &m[i], "hud") && t1[m[i].value_at] == '{') {
            size_t l2 = 0;
            unsigned c = 0;
            t2 = cv_cut(t1, l1, m[i].value_at, schema, &l2, &c);
            if (t2 && c) { free(t1); t1 = t2; l1 = l2; t2 = NULL; if (retired) (*retired)++; }
            free(t2);
            t2 = NULL;
            break;
        }
    }
    {
        int named = 0;
        for (i = 0; m && i < n; i++) if (cv_key_is(t1, &m[i], "name")) named = 1;
        free(m);
        m = cv_members(t1, l1, skip, &n);
        if (!named && m) {
            /* "name" first: the builder needs one; the old key reads as it. */
            const char *open = strchr(t1 + skip, '{');
            char *k = cv_esc(key);
            size_t cap = l1 + strlen(k ? k : "") + 32, at = (size_t)(open - t1) + 1;
            t2 = (char *)malloc(cap);
            if (t2 && k && open) {
                int w = _snprintf_s(t2, cap, _TRUNCATE, "%.*s\n  \"name\": \"%s\"%s%s", (int)at, t1, k, n ? "," : "\n", t1 + at);
                free(t1);
                t1 = t2;
                l1 = (size_t)w;
                t2 = NULL;
            }
            free(t2);
            free(k);
        }
        free(m);
    }
    if (out_len) *out_len = l1;
    return t1;
}

/* --------------------------------------------------- the folder list ---- */

typedef struct cv_folder {
    char *path;                 /* absolute UTF-8 */
    char *rel;                  /* under mods\ */
    const char *leaf;
    char key[SH_MB_ID_MAX + 1];
    char bundle[65];
    char tree[65];
    char descriptor[16];
    long long built;
    int product;                /* a second build the product made */
    int keep;                   /* 1 convert, 0 remove */
    int left;                   /* refused before and unchanged since */
    int done;                   /* converted in this run */
    char *why;                  /* the last cause while it waits for another folder */
    char salt[SH_SALT_LEN + 1];
} cv_folder;

typedef struct cv_list { cv_folder *f; size_t n, cap; } cv_list;

static void cv_list_free(cv_list *l)
{
    size_t i;
    for (i = 0; i < l->n; i++) { free(l->f[i].path); free(l->f[i].rel); free(l->f[i].why); }
    free(l->f);
    memset(l, 0, sizeof *l);
}

static int cv_hex16_at(const char *s)
{
    int i;
    for (i = 0; i < 16; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return s[16] == 0 || s[16] == '-';
}

static int cv_product_made(const char *rel, const char *leaf, const char *origin)
{
    const char *at = strrchr(leaf, '@');
    if (at && cv_hex16_at(at + 1)) return 1;
    if (!_strnicmp(rel, "downloads\\", 10) || !_strnicmp(rel, "imports\\", 8)) return 1;
    return origin && (!strcmp(origin, "download") || !strcmp(origin, "delivery") || !strcmp(origin, "imported") ||
                      !strcmp(origin, "migration-delivered"));
}

static void cv_walk(cv_list *l, const char *mods, const char *dir, const char *rel, int depth)
{
    WIN32_FIND_DATAW fd;
    char *pat = cv_join(dir, "*");
    wchar_t *w = pat ? sh_mb_wpath(pat) : NULL;
    HANDLE h = w ? FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pat);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char *name, *path, crel[CV_PATH];
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (fd.cFileName[0] == L'.') continue;
        if (wcsstr(fd.cFileName, L".staging-") || wcsstr(fd.cFileName, L".old-")) continue;
        name = sh_mb_utf8(fd.cFileName);
        if (!name) continue;
        if (depth == 0 && !_stricmp(name, "maps")) { free(name); continue; }
        _snprintf_s(crel, sizeof crel, _TRUNCATE, "%s%s%s", rel, rel[0] ? "\\" : "", name);
        path = cv_join(dir, name);
        free(name);
        if (!path) continue;
        if (cv_is_file(path, "mod.json") || cv_is_file(path, "package.json")) {
            if (sh_mb_old_is_old(path) || (!cv_is_file(path, "mod.json") && cv_is_file(path, "package.json"))) {
                sh_mb_old old;
                cv_folder *f;
                if (l->n == l->cap) {
                    size_t cap = l->cap ? l->cap * 2 : 32;
                    cv_folder *g = (cv_folder *)realloc(l->f, cap * sizeof *g);
                    if (!g) { free(path); continue; }
                    l->f = g;
                    l->cap = cap;
                }
                f = &l->f[l->n];
                memset(f, 0, sizeof *f);
                f->path = path;
                f->rel = _strdup(crel);
                if (!f->rel) { free(path); continue; }
                f->leaf = strrchr(f->rel, '\\') ? strrchr(f->rel, '\\') + 1 : f->rel;
                sh_mb_old_load(path, &old);
                strncpy_s(f->key, sizeof f->key, old.key[0] ? old.key : f->leaf, _TRUNCATE);
                if (!old.key[0]) {
                    size_t i;
                    for (i = 0; f->key[i]; i++) if (f->key[i] >= 'A' && f->key[i] <= 'Z') f->key[i] = (char)(f->key[i] + 32);
                }
                strncpy_s(f->bundle, sizeof f->bundle, old.bundle, _TRUNCATE);
                strncpy_s(f->descriptor, sizeof f->descriptor, old.descriptor[0] ? old.descriptor : "mod.json", _TRUNCATE);
                f->built = old.built;
                f->product = cv_product_made(f->rel, f->leaf, old.origin);
                sh_mb_old_free(&old);
                if (!sh_mb_tree_hash(path, f->tree)) f->tree[0] = 0;
                l->n++;
                (void)mods;
                continue;
            }
            free(path);
            continue;
        }
        if (depth < CV_NEST_MAX) cv_walk(l, mods, path, crel, depth + 1);
        free(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Which folders are kept: every folder of the player's own, and of a key
 * the product alone made, its newest; the other product-made ones go. */
static void cv_decide(cv_list *l, cv_rec *rec)
{
    size_t i, j;
    for (i = 0; i < l->n; i++) {
        char tree[80];
        l->f[i].keep = -1;
        if (cv_rec_get(rec, "refused", l->f[i].rel, tree, sizeof tree) && l->f[i].tree[0] && !strcmp(tree, l->f[i].tree))
            l->f[i].left = 1;
    }
    for (i = 0; i < l->n; i++) {
        int owner = 0;
        size_t newest = (size_t)-1;
        if (l->f[i].keep != -1) continue;
        for (j = i; j < l->n; j++) {
            if (_stricmp(l->f[j].key, l->f[i].key)) continue;
            if (!l->f[j].product) owner = 1;
            else if (newest == (size_t)-1 || l->f[j].built > l->f[newest].built) newest = j;
        }
        for (j = i; j < l->n; j++) {
            if (_stricmp(l->f[j].key, l->f[i].key)) continue;
            l->f[j].keep = !l->f[j].product || (!owner && j == newest);
        }
    }
}

/* The kept folder of `f`'s key, when it is not `f` itself. */
static const cv_folder *cv_keeper(const cv_list *l, const cv_folder *f)
{
    size_t i;
    const cv_folder *best = NULL;
    for (i = 0; i < l->n; i++) {
        const cv_folder *g = &l->f[i];
        if (g == f || !g->keep || _stricmp(g->key, f->key)) continue;
        if (!best || (best->product && !g->product) || (g->product == best->product && g->built < best->built)) best = g;
    }
    return best;
}

/* -------------------------------------------------------------- scan ---- */

static const char *const CV_SECTIONS[] = { "cache", "aliases", "snapshots", "renamed", "downloads", NULL };

static int cv_map_old(const char *text, size_t n)
{
    sh_map_old old;
    char err[160];
    int r = 0;
    if (sh_map_old_read(text, n, &old, err, sizeof err)) {
        r = old.families != 0;
        sh_map_old_free(&old);
    }
    return r;
}

/* A prefab whose block names a mod by "id" (an earlier row shape). */
static int cv_prefab_old(const char *text, size_t n)
{
    sh_prefab_block_span sp;
    const char *v, *e;
    if (sh_prefab_block_find(text, n, &sp) != 1) return 0;
    v = text + sp.value_at;
    e = v + sp.value_len;
    for (; v + 5 < e; v++) if (!memcmp(v, "\"id\":", 5) || !memcmp(v, "\"bundle\":", 9)) return 1;
    return 0;
}

typedef void (*cv_map_fn)(const sh_conv_opts *o, void *ctx, const char *path, int kind, const char *account);
enum { CV_MAP_SAVED = 1, CV_MAP_RAW = 2, CV_MAP_PREFAB = 3 };

static void cv_prefabs_walk(const sh_conv_opts *o, const char *dir, int depth, cv_map_fn fn, void *ctx)
{
    WIN32_FIND_DATAW fd;
    char *pat = cv_join(dir, "*");
    wchar_t *w = pat ? sh_mb_wpath(pat) : NULL;
    HANDLE h = w ? FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pat);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char *name, *path;
        size_t nl;
        if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        name = sh_mb_utf8(fd.cFileName);
        path = name ? cv_join(dir, name) : NULL;
        nl = name ? strlen(name) : 0;
        if (path && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            if (depth < CV_NEST_MAX) cv_prefabs_walk(o, path, depth + 1, fn, ctx);
        } else if (path && nl > 5 && !_stricmp(name + nl - 5, ".json") && !(nl > 10 && !_stricmp(name + nl - 10, ".meta.json"))) {
            fn(o, ctx, path, CV_MAP_PREFAB, NULL);
        }
        free(name);
        free(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Every map the conversion reads: saved maps, rawmap.json, prefabs. */
static void cv_maps_each(const sh_conv_opts *o, cv_map_fn fn, void *ctx)
{
    char *p;
    if (o->saves) {
        char *root = sh_mb_utf8(o->saves);
        WIN32_FIND_DATAW fa;
        char *pat = root ? cv_join(root, "*") : NULL;
        wchar_t *w = pat ? sh_mb_wpath(pat) : NULL;
        HANDLE ha = w ? FindFirstFileExW(w, FindExInfoBasic, &fa, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
        free(pat);
        free(w);
        if (ha != INVALID_HANDLE_VALUE) {
            do {
                char *acct, *adir;
                WIN32_FIND_DATAW fm;
                HANDLE hm;
                if (!(fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fa.cFileName[0] == L'.') continue;
                acct = sh_mb_utf8(fa.cFileName);
                adir = acct ? cv_join(root, acct) : NULL;
                pat = adir ? cv_join(adir, "SNAPMAPS*") : NULL;
                w = pat ? sh_mb_wpath(pat) : NULL;
                hm = w ? FindFirstFileExW(w, FindExInfoBasic, &fm, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
                free(pat);
                free(w);
                if (hm != INVALID_HANDLE_VALUE) {
                    do {
                        char *slot, *sdir, *decl;
                        if (!(fm.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                        slot = sh_mb_utf8(fm.cFileName);
                        sdir = slot ? cv_join(adir, slot) : NULL;
                        decl = sdir ? cv_join(sdir, "map.decl") : NULL;
                        if (decl && cv_is_file(sdir, "map.decl")) fn(o, ctx, decl, CV_MAP_SAVED, acct);
                        free(slot);
                        free(sdir);
                        free(decl);
                    } while (FindNextFileW(hm, &fm));
                    FindClose(hm);
                }
                free(acct);
                free(adir);
            } while (FindNextFileW(ha, &fa));
            FindClose(ha);
        }
        free(root);
    }
    if (o->data && cv_is_file(o->data, "rawmap.json") && (p = cv_join(o->data, "rawmap.json")) != NULL) {
        fn(o, ctx, p, CV_MAP_RAW, NULL);
        free(p);
    }
    if (o->data && (p = cv_join(o->data, "prefabs")) != NULL) {
        if (cv_is_dir(p)) cv_prefabs_walk(o, p, 0, fn, ctx);
        free(p);
    }
}

/* A saved map's text: the 4-byte header and a zlib stream. */
static char *cv_saved_text(const unsigned char *raw, size_t n, size_t *len)
{
    return n > 10 ? (char *)sh_store_unzlib(raw + 4, n - 4, CV_MAP_MAX, len) : NULL;
}

typedef struct cv_scan_ctx { sh_conv_check *c; cv_rec *rec; int list; } cv_scan_ctx;

static void cv_scan_map(const sh_conv_opts *o, void *vctx, const char *path, int kind, const char *account)
{
    cv_scan_ctx *x = (cv_scan_ctx *)vctx;
    size_t n = 0, tn = 0;
    unsigned char *raw = cv_read(path, CV_MAP_MAX, &n);
    char *text = NULL, sha[65], was[80];
    int old;
    (void)account;
    if (!raw) return;
    text = kind == CV_MAP_SAVED ? cv_saved_text(raw, n, &tn) : (char *)raw;
    if (kind != CV_MAP_SAVED) tn = n;
    old = text && (kind == CV_MAP_PREFAB ? cv_prefab_old(text, tn) : cv_map_old(text, tn));
    if (old) {
        cv_hex_sha(raw, n, sha);
        if (cv_rec_get(x->rec, "maps", path, was, sizeof was) && !strcmp(was, sha)) {
            x->c->maps_left++;
            if (x->list) cv_say(o, 0, "  %s: left as it is (see the earlier run's line for why)", path);
        } else {
            if (!x->c->maps) strncpy_s(x->c->first_map, sizeof x->c->first_map, path, _TRUNCATE);
            x->c->maps++;
        }
    }
    if (kind == CV_MAP_SAVED) free(text);
    free(raw);
}

int sh_conv_scan(const sh_conv_opts *o, sh_conv_check *out, int list)
{
    cv_list l;
    cv_rec rec;
    size_t i;
    int kinds;
    char *builds;
    memset(out, 0, sizeof *out);
    memset(&l, 0, sizeof l);
    if (!o || !o->mods) return -1;
    cv_rec_load(o, &rec);
    if (cv_is_dir(o->mods)) cv_walk(&l, o->mods, o->mods, "", 0);
    cv_decide(&l, &rec);
    for (i = 0; i < l.n; i++) {
        const cv_folder *f = &l.f[i];
        if (f->keep) {
            if (f->left) {
                out->folders_left++;
                if (list) cv_say(o, 0, "  mods\\%s: left as it was (it could not be converted; see the earlier run's line for why)",
                                 f->rel);
            } else out->folders++;
        } else {
            const cv_folder *k = cv_keeper(&l, f);
            if (!k || !k->left) out->duplicates++;
        }
    }
    builds = cv_join(o->mods, ".builds");
    out->builds_dir = builds && cv_is_dir(builds);
    free(builds);
    for (i = 0; CV_SECTIONS[i]; i++) {
        char *t = NULL;
        if (sh_mb_record_get(o->mods, CV_SECTIONS[i], &t) == 1) {
            size_t w = strlen(out->section_names);
            _snprintf_s(out->section_names + w, sizeof out->section_names - w, _TRUNCATE, "%s%s", w ? ", " : "", CV_SECTIONS[i]);
            out->sections++;
        }
        free(t);
    }
    {
        cv_scan_ctx x;
        x.c = out;
        x.rec = &rec;
        x.list = list;
        cv_maps_each(o, cv_scan_map, &x);
    }
    cv_list_free(&l);
    cv_rec_free(&rec);
    kinds = (out->folders > 0) + (out->duplicates > 0) + out->builds_dir + (out->sections > 0) + (out->maps > 0);
    return kinds;
}

/* ----------------------------------------------------- folder steps ---- */

static int cv_stock(void *context, const char *request)
{
    (void)context;
    return sh_modstock_exists(request);
}

/* Build `dest` with the real builder and write its mod.json. */
static int cv_build(const char *dest, sh_mb_bundle *b, char *cause, size_t cap)
{
    sh_ms_list list;
    sh_mb_resolver r;
    int ok = 0;
    memset(&list, 0, sizeof list);
    if (!sh_ms_enumerate(&list)) memset(&list, 0, sizeof list);
    r.installed = sh_ms_request_exists;
    r.installed_context = &list;
    r.stock = cv_stock;
    r.stock_context = NULL;
    sh_mb_keep_replacements_here(1);
    if (!sh_mb_build(dest, NULL, &r, b))
        _snprintf_s(cause, cap, _TRUNCATE, "it does not build under this version's rules: %s", b->first_cause);
    else if (!sh_mb_write(b, dest, cause, cap)) {}
    else ok = 1;
    sh_mb_keep_replacements_here(0);
    sh_ms_list_free(&list);
    return ok;
}

/* A folder rename, tried again for a while when another program (an indexer
 * or a virus scanner looking at the files just written) briefly holds it. */
static BOOL cv_move(const wchar_t *from, const wchar_t *to)
{
    int i;
    for (i = 0; i < 40; i++) {
        DWORD e;
        if (MoveFileExW(from, to, 0)) return TRUE;
        e = GetLastError();
        if (e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) return FALSE;
        Sleep(i < 10 ? 50 : 250);
    }
    return FALSE;
}

/* Put `dest` in place of `path`; the folder as it was goes to the Recycle Bin. */
static int cv_swap(const sh_conv_opts *o, const char *path, const char *dest, char *cause, size_t cap)
{
    char old[CV_PATH];
    wchar_t *wp, *wd, *wo;
    int ok = 0;
    _snprintf_s(old, sizeof old, _TRUNCATE, "%s.old-%lu", path, GetCurrentProcessId());
    wp = sh_mb_wpath(path);
    wd = sh_mb_wpath(dest);
    wo = sh_mb_wpath(old);
    if (!wp || !wd || !wo) _snprintf_s(cause, cap, _TRUNCATE, "out of memory");
    else {
        if (GetFileAttributesW(wo) != INVALID_FILE_ATTRIBUTES) cv_rmtree_w(wo);
        if (!cv_move(wp, wo))
            _snprintf_s(cause, cap, _TRUNCATE, "the folder could not be set aside (error %lu; a program may hold a file in it)",
                        GetLastError());
        else if (!cv_move(wd, wp)) {
            DWORD e = GetLastError();
            cv_move(wo, wp);
            _snprintf_s(cause, cap, _TRUNCATE, "the converted folder could not be put in place (error %lu)", e);
        } else {
            ok = 1;
            if (!o->discard || !o->discard(o->discard_context, wo))
                cv_say(o, 0, "  %s.old-%lu: the folder as it was could not go to the Recycle Bin; remove it by hand (Snapmap+ "
                       "does not read it)", path, GetCurrentProcessId());
        }
    }
    free(wp);
    free(wd);
    free(wo);
    return ok;
}

static int cv_convert_one(const sh_conv_opts *o, cv_folder *f, sh_mb_bundle *b, char *cause, size_t cap)
{
    char *dpath = cv_join(f->path, f->descriptor), dest[CV_PATH], *fresh = NULL;
    unsigned char *text = NULL;
    size_t n = 0, fl = 0;
    unsigned retired = 0;
    int r, ok = 0;
    cause[0] = 0;
    text = dpath ? cv_read(dpath, CV_DESC_MAX, &n) : NULL;
    free(dpath);
    if (!text) { _snprintf_s(cause, cap, _TRUNCATE, "its %s cannot be read", f->descriptor); return 0; }
    fresh = cv_fresh((const char *)text, n, f->key, &fl, &retired);
    free(text);
    if (!fresh) { _snprintf_s(cause, cap, _TRUNCATE, "its %s is not a JSON object", f->descriptor); return 0; }
    _snprintf_s(dest, sizeof dest, _TRUNCATE, "%s.staging-%lu", f->path, GetCurrentProcessId());
    if (cv_is_dir(dest)) cv_rmtree(dest);
    r = sh_ms_salt_copy(f->path, fresh, fl, f->key, dest, f->salt, cause, cap);
    if (r < 0 && strstr(cause, " file(s) are under mods/") && _stricmp(f->leaf, f->key)) {
        /* Its files are under its folder's name rather than its key. */
        if (cv_is_dir(dest)) cv_rmtree(dest);
        r = sh_ms_salt_copy(f->path, fresh, fl, f->leaf, dest, f->salt, cause, cap);
    }
    free(fresh);
    if (r < 0) {
        if (cv_is_dir(dest)) cv_rmtree(dest);
        return 0;
    }
    if (!cv_build(dest, b, cause, cap)) {
        cv_rmtree(dest);
        return 0;
    }
    if (!cv_swap(o, f->path, dest, cause, cap)) {
        cv_rmtree(dest);
        return 0;
    }
    ok = 1;
    (void)retired;
    return ok;
}

/* Old build hashes the retired record sections name: an alias whose target
 * is one of the converted builds is that salt's too. */
static void cv_alias_builds(const sh_conv_opts *o, cv_rec *rec)
{
    char *text = NULL;
    mpj *a;
    int i, pass;
    if (sh_mb_record_get(o->mods, "aliases", &text) != 1 || !text) { free(text); return; }
    a = mpj_parse(text, strlen(text));
    free(text);
    if (!a || a->type != MPJ_OBJ) { mpj_free(a); return; }
    for (pass = 0; pass < 4; pass++)
        for (i = 0; i < a->n; i++) {
            char salt[16];
            if (a->items[i]->type != MPJ_STR || cv_rec_get(rec, "builds", a->keys[i], salt, sizeof salt)) continue;
            if (cv_rec_get(rec, "builds", a->items[i]->text, salt, sizeof salt)) cv_rec_set(rec, "builds", a->keys[i], salt);
        }
    mpj_free(a);
}

/* The bundle hashes of mods\.builds\ snapshots, under their key's salt. */
static void cv_snapshot_builds(const sh_conv_opts *o, cv_rec *rec)
{
    char *dir = cv_join(o->mods, ".builds"), *pat = dir ? cv_join(dir, "*") : NULL;
    wchar_t *w = pat ? sh_mb_wpath(pat) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h = w ? FindFirstFileExW(w, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pat);
    free(w);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char *name, *path, salt[16];
            sh_mb_old old;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
            name = sh_mb_utf8(fd.cFileName);
            path = name ? cv_join(dir, name) : NULL;
            if (path && sh_mb_old_load(path, &old)) {
                if (old.bundle[0] && old.key[0] && cv_rec_get_key(rec, old.key, salt, sizeof salt))
                    cv_rec_set(rec, "builds", old.bundle, salt);
            }
            sh_mb_old_free(&old);
            free(name);
            free(path);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(dir);
}

int sh_conv_folders(const sh_conv_opts *o, sh_conv_stats *st)
{
    cv_list l;
    cv_rec rec;
    size_t i;
    int pass, progress, did = 0;
    memset(&l, 0, sizeof l);
    if (!o || !o->mods || !cv_is_dir(o->mods)) return 0;
    cv_rec_load(o, &rec);
    cv_walk(&l, o->mods, o->mods, "", 0);
    if (!l.n) { cv_rec_free(&rec); return 0; }
    cv_decide(&l, &rec);
    if (!o->stock) {
        for (i = 0; i < l.n; i++) if (l.f[i].keep && !l.f[i].left) st->waiting++;
        if (st->waiting)
            cv_say(o, 0, "  mods\\: %u folder(s) of an earlier release are not converted yet: building them needs the DOOM "
                   "folder, and DOOM was not found through Steam", st->waiting);
        cv_list_free(&l);
        cv_rec_free(&rec);
        return 0;
    }
    /* Folders that need another one's names build after it: passes until nothing more builds. */
    for (pass = 0, progress = 1; progress && pass < 4; pass++) {
        progress = 0;
        size_t order;
        for (order = 0; order < 2 * l.n; order++) {
            cv_folder *f = &l.f[order % l.n];
            sh_mb_bundle b;
            char cause[SH_MB_CAUSE_MAX + 128];
            i = order % l.n;
            if ((order < l.n) == (f->product != 0)) continue;   /* the player's own folders first */
            if (!f->keep || f->left || f->done) continue;
            memset(&b, 0, sizeof b);
            if (cv_convert_one(o, f, &b, cause, sizeof cause)) {
                char have[16];
                f->done = 1;
                progress = did = 1;
                st->converted++;
                cv_say(o, 0, "  mods\\%s: converted: old key %s, salt %s, build %.16s, %zu file(s)", f->rel, f->key, f->salt,
                       b.hash, b.file_count);
                if (!cv_rec_get_key(&rec, f->key, have, sizeof have)) cv_rec_set(&rec, "keys", f->key, f->salt);
                if (f->bundle[0]) cv_rec_set(&rec, "builds", f->bundle, f->salt);
                cv_rec_set(&rec, "folders", f->rel, f->salt);
                cv_rec_set(&rec, "refused", f->rel, NULL);
            } else if (pass == 3 || !strstr(cause, "resolves to nothing")) {
                free(f->why);
                f->why = NULL;
                /* The last pass, or a cause another folder cannot settle. */
                f->left = 1;
                st->refused++;
                did = 1;
                cv_say(o, 0, "  mods\\%s: left as it was: %s", f->rel, cause);
                cv_rec_set(&rec, "refused", f->rel, f->tree[0] ? f->tree : "-");
            } else {
                free(f->why);
                f->why = _strdup(cause);   /* tried again after the others */
            }
            sh_mb_free(&b);
        }
    }
    for (i = 0; i < l.n; i++) {
        cv_folder *f = &l.f[i];
        if (f->keep && !f->left && !f->done) {
            f->left = 1;
            st->refused++;
            cv_say(o, 0, "  mods\\%s: left as it was: %s", f->rel, f->why ? f->why : "it needs a mod that did not build");
            cv_rec_set(&rec, "refused", f->rel, f->tree[0] ? f->tree : "-");
        }
    }
    /* Second builds the product made: gone once the kept folder is converted. */
    for (i = 0; i < l.n; i++) {
        cv_folder *f = &l.f[i];
        const cv_folder *k;
        wchar_t *w;
        if (f->keep) continue;
        k = cv_keeper(&l, f);
        if (!k || !k->done) {
            cv_say(o, 0, "  mods\\%s: kept for now: a second build of %s, whose own folder was not converted", f->rel, f->key);
            continue;
        }
        w = sh_mb_wpath(f->path);
        if (w && o->discard && o->discard(o->discard_context, w)) {
            st->removed++;
            did = 1;
            if (f->bundle[0]) cv_rec_set(&rec, "builds", f->bundle, k->salt);
            cv_say(o, 0, "  mods\\%s: a second build of %s that Snapmap+ made; %s (mods\\%s is that mod, salt %s)", f->rel,
                   f->key, sh_ms_discard_verb(), k->rel, k->salt);
        } else
            cv_say(o, 0, "  mods\\%s: a second build of %s could not go to the Recycle Bin (error %lu)", f->rel, f->key,
                   GetLastError());
        free(w);
    }
    cv_alias_builds(o, &rec);
    cv_snapshot_builds(o, &rec);
    if (did && !cv_rec_save(o, &rec)) cv_say(o, 1, "  mods\\snapmap-plus.json: the conversion record could not be written");
    cv_list_free(&l);
    cv_rec_free(&rec);
    return did;
}

int sh_conv_retire(const sh_conv_opts *o, sh_conv_stats *st)
{
    char *builds = o && o->mods ? cv_join(o->mods, ".builds") : NULL;
    int did = 0, i;
    if (!builds) return -1;
    if (cv_is_dir(builds)) {
        wchar_t *w = sh_mb_wpath(builds);
        cv_rec rec;
        cv_rec_load(o, &rec);
        cv_snapshot_builds(o, &rec);
        cv_rec_save(o, &rec);
        cv_rec_free(&rec);
        if (w && o->discard && o->discard(o->discard_context, w)) {
            st->retired++;
            did = 1;
            cv_say(o, 0, "  mods\\.builds\\: the retired last good builds; %s", sh_ms_discard_verb());
        } else cv_say(o, 0, "  mods\\.builds\\ could not go to the Recycle Bin (error %lu); it is tried again at the next run",
                      GetLastError());
        free(w);
    }
    free(builds);
    for (i = 0; CV_SECTIONS[i]; i++) {
        char *t = NULL;
        if (sh_mb_record_get(o->mods, CV_SECTIONS[i], &t) == 1) {
            if (!strcmp(CV_SECTIONS[i], "aliases")) {
                cv_rec rec;
                cv_rec_load(o, &rec);
                cv_alias_builds(o, &rec);
                cv_rec_save(o, &rec);
                cv_rec_free(&rec);
            }
            if (sh_mb_record_put(o->mods, CV_SECTIONS[i], NULL)) {
                st->retired++;
                did = 1;
                cv_say(o, 0, "  mods\\snapmap-plus.json: the retired section \"%s\" removed", CV_SECTIONS[i]);
            }
        }
        free(t);
    }
    return did;
}

/* -------------------------------------------------------------- maps ---- */

typedef struct cv_ids {
    cv_rec rec;
    sh_ms_list list;
    cv_list old;                /* folders still in an earlier shape */
} cv_ids;

static const sh_ms_entry *cv_root_salt(const sh_ms_list *l, const char *salt)
{
    size_t i;
    for (i = 0; i < l->count; i++) {
        const sh_ms_entry *e = &l->items[i];
        if (!e->place[0] && !strcmp(e->salt, salt) && sh_ms_entry_usable(e)) return e;
    }
    return NULL;
}

static int cv_resolve(cv_ids *ids, const char *key, const char *bundle, sh_map_old_salt *out, char *cause, size_t cap)
{
    char salt[16] = "";
    const sh_ms_entry *e;
    memset(out, 0, sizeof *out);
    strncpy_s(out->key, sizeof out->key, key, _TRUNCATE);
    if (!cv_rec_get_key(&ids->rec, key, salt, sizeof salt) && !(bundle && bundle[0] &&
                                                                 cv_rec_get(&ids->rec, "builds", bundle, salt, sizeof salt))) {
        size_t i;
        for (i = 0; i < ids->old.n; i++)
            if (!_stricmp(ids->old.f[i].key, key)) {
                _snprintf_s(cause, cap, _TRUNCATE, "it uses %s, whose folder mods\\%s could not be converted", key,
                            ids->old.f[i].rel);
                return 0;
            }
        _snprintf_s(cause, cap, _TRUNCATE, "it uses %s, which no installed mod folder provides", key);
        return 0;
    }
    e = cv_root_salt(&ids->list, salt);
    if (!e) {
        _snprintf_s(cause, cap, _TRUNCATE, "it uses %s (now salt %s), whose folder is not installed or does not build", key, salt);
        return 0;
    }
    strncpy_s(out->salt, sizeof out->salt, salt, _TRUNCATE);
    strncpy_s(out->build, sizeof out->build, e->bundle.hash, _TRUNCATE);
    if (e->bundle.name) strncpy_s(out->name, sizeof out->name, e->bundle.name, _TRUNCATE);
    strncpy_s(out->version, sizeof out->version, e->bundle.version, _TRUNCATE);
    out->size = e->bundle.total_size;
    return 1;
}


static int cv_ids_open(const sh_conv_opts *o, cv_ids *ids)
{
    memset(ids, 0, sizeof *ids);
    cv_rec_load(o, &ids->rec);
    if (!sh_ms_enumerate(&ids->list)) memset(&ids->list, 0, sizeof ids->list);
    if (cv_is_dir(o->mods)) cv_walk(&ids->old, o->mods, o->mods, "", 0);
    return 1;
}

static void cv_ids_close(cv_ids *ids)
{
    cv_rec_free(&ids->rec);
    sh_ms_list_free(&ids->list);
    cv_list_free(&ids->old);
}

static int cv_has(const char *t, size_t n, const char *s)
{
    size_t k = strlen(s), i;
    for (i = 0; i + k <= n; i++) if (t[i] == s[0] && !memcmp(t + i, s, k)) return 1;
    return 0;
}

static int cv_map_text_ids(const sh_conv_opts *o, cv_ids *ids, const char *text, size_t n, char **out, size_t *out_len,
                           char *cause, size_t cap)
{
    const char *t = text;
    size_t tn = n, i, k;
    char *legacy = NULL, err[256] = "";
    sh_map_old old;
    sh_map_old_salt *map = NULL;
    size_t nmap = 0;
    int r = -1;
    *out = NULL;
    if (out_len) *out_len = 0;
    if (cv_has(text, n, "\"smpkg.")) {
        /* Packages a map carried first (beta.6 to beta.19): each becomes a salted and built mod folder. */
        size_t ln = 0;
        sh_modlegacy_set_data_root(o->data);
        sh_modlegacy_set_salter(sh_ms_assign_salt);
        legacy = sh_modlegacy_process(text, n, &ln);
        if (legacy) { t = legacy; tn = ln; }
        sh_ms_list_free(&ids->list);
        if (!sh_ms_enumerate(&ids->list)) memset(&ids->list, 0, sizeof ids->list);
    }
    if (!sh_map_old_read(t, tn, &old, err, sizeof err)) {
        _snprintf_s(cause, cap, _TRUNCATE, "it does not read as a map: %s", err);
        goto done;
    }
    if (!old.families) {
        sh_map_old_free(&old);
        if (legacy) {
            *out = (char *)malloc(tn + 1);
            if (*out) { memcpy(*out, t, tn); (*out)[tn] = 0; if (out_len) *out_len = tn; r = 1; }
        } else r = 0;
        goto done;
    }
    if (old.families & SH_MAP_OLD_F_PACKAGE) {
        _snprintf_s(cause, cap, _TRUNCATE, "it carries packages of beta.6 to beta.19 that could not be converted");
        sh_map_old_free(&old);
        goto done;
    }
    map = (sh_map_old_salt *)calloc(old.row_count + 1, sizeof *map);
    if (!map) { _snprintf_s(cause, cap, _TRUNCATE, "out of memory"); sh_map_old_free(&old); goto done; }
    for (i = 0; i < old.row_count; i++) {
        for (k = 0; k < nmap && _stricmp(map[k].key, old.rows[i].key); k++) {}
        if (k < nmap) continue;
        if (!cv_resolve(ids, old.rows[i].key, old.rows[i].bundle, &map[nmap], cause, cap)) { sh_map_old_free(&old); goto done; }
        if (!map[nmap].name[0]) strncpy_s(map[nmap].name, sizeof map[nmap].name, old.rows[i].key, _TRUNCATE);
        nmap++;
    }
    {
        size_t wl = 0;
        char *w = sh_map_old_write(t, tn, &old, map, nmap, &wl, err, sizeof err);
        if (!w) _snprintf_s(cause, cap, _TRUNCATE, "it could not be written in this version's shape: %s", err);
        else {
            *out = (char *)malloc(wl + 1);
            if (*out) { memcpy(*out, w, wl); (*out)[wl] = 0; if (out_len) *out_len = wl; r = 1; }
            HeapFree(GetProcessHeap(), 0, w);
        }
    }
    sh_map_old_free(&old);
done:
    free(map);
    if (legacy) HeapFree(GetProcessHeap(), 0, legacy);
    return r;
}

int sh_conv_map_text(const sh_conv_opts *o, const char *text, size_t n, char **out, size_t *out_len, char *cause, size_t cap)
{
    cv_ids ids;
    int r;
    cv_ids_open(o, &ids);
    r = cv_map_text_ids(o, &ids, text, n, out, out_len, cause, cap);
    cv_ids_close(&ids);
    return r;
}

/* A prefab's block rows from the "id" shape to "<salt>@<build>", and every
 * mods/<old key>/ name in the file to mods/<salt>/. */
static int cv_prefab_text(cv_ids *ids, const char *text, size_t n, char **out, size_t *out_len, char *cause, size_t cap)
{
    sh_prefab_block_span sp;
    mpj *block = NULL, *mods;
    char *t = NULL, *bt = NULL, *put = NULL;
    size_t tn = n, i, pl = 0;
    int r = -1, changed = 0;
    *out = NULL;
    if (!cv_prefab_old(text, n)) return 0;
    t = (char *)malloc(n + 1);
    if (!t) { _snprintf_s(cause, cap, _TRUNCATE, "out of memory"); return -1; }
    memcpy(t, text, n);
    t[n] = 0;
    if (sh_prefab_block_find(t, tn, &sp) != 1 || !(block = mpj_parse(t + sp.value_at, sp.value_len)) ||
        block->type != MPJ_OBJ) {
        _snprintf_s(cause, cap, _TRUNCATE, "its block does not read");
        goto done;
    }
    mods = mpj_get(block, "mods");
    if (mods && mods->type == MPJ_ARR) {
        for (i = 0; i < (size_t)mods->n; i++) {
            mpj *row = mods->items[i], *id, *v, *nm, *fresh;
            sh_map_old_salt s;
            sh_modmanifest_row mr;
            char *rowtext, *moved = NULL;
            size_t rl = 0, ml = 0, c;
            if (row->type != MPJ_OBJ || !(id = mpj_get(row, "id")) || id->type != MPJ_STR) continue;
            v = mpj_get(row, "bundle");
            if (!cv_resolve(ids, id->text, v && v->type == MPJ_STR ? v->text : NULL, &s, cause, cap)) goto done;
            /* Every name the file places under mods/<old key>/ follows the salt. */
            c = sh_mb_rekey_text(t, tn, id->text, s.salt, &moved, &ml);
            if (c && c != (size_t)-1 && moved) { free(t); t = moved; tn = ml; }
            memset(&mr, 0, sizeof mr);
            strcpy_s(mr.salt, sizeof mr.salt, s.salt);
            strcpy_s(mr.build, sizeof mr.build, s.build);
            strcpy_s(mr.name, sizeof mr.name, s.name[0] ? s.name : id->text);
            strcpy_s(mr.version, sizeof mr.version, s.version);
            mr.size = s.size;
            v = mpj_get(row, "attached");
            mr.attached = v && v->type == MPJ_BOOL && v->b;
            nm = mpj_get(row, "names");
            if (nm && nm->type == MPJ_ARR) {
                int q;
                for (q = 0; q < nm->n; q++) {
                    char nb[512], *mv = NULL;
                    size_t mvl = 0;
                    if (nm->items[q]->type != MPJ_STR) continue;
                    if (sh_mb_rekey_text(nm->items[q]->text, strlen(nm->items[q]->text), id->text, s.salt, &mv, &mvl) &&
                        mv) {
                        strncpy_s(nb, sizeof nb, mv, _TRUNCATE);
                        free(mv);
                    } else strncpy_s(nb, sizeof nb, nm->items[q]->text, _TRUNCATE);
                    sh_modmanifest_row_add_name(&mr, nb);
                }
            }
            rowtext = sh_modmanifest_format_value(&mr, &rl);
            sh_modmanifest_row_free(&mr);
            fresh = rowtext ? mpj_parse(rowtext, rl) : NULL;
            free(rowtext);
            if (!fresh) { _snprintf_s(cause, cap, _TRUNCATE, "a row could not be written"); goto done; }
            mpj_free(mods->items[i]);
            mods->items[i] = fresh;
            changed = 1;
        }
    }
    if (!changed) { r = 0; goto done; }
    bt = mpj_dump(block, &pl);
    put = bt ? sh_prefab_block_put(t, tn, bt, pl, out_len) : NULL;
    if (!put) { _snprintf_s(cause, cap, _TRUNCATE, "the block could not be written"); goto done; }
    *out = put;
    r = 1;
done:
    mpj_free(block);
    free(bt);
    free(t);
    return r;
}

typedef struct cv_map_ctx { cv_ids *ids; sh_conv_stats *st; int did; } cv_map_ctx;

static void cv_map_left(const sh_conv_opts *o, cv_map_ctx *x, const char *path, const unsigned char *raw, size_t n,
                        const char *cause)
{
    char sha[65];
    cv_hex_sha(raw, n, sha);
    cv_rec_set(&x->ids->rec, "maps", path, sha);
    x->st->maps_refused++;
    x->did = 1;
    cv_say(o, 0, "  %s: left as it was: %s", path, cause);
}

static void cv_do_map(const sh_conv_opts *o, void *vctx, const char *path, int kind, const char *account)
{
    cv_map_ctx *x = (cv_map_ctx *)vctx;
    size_t n = 0, tn = 0, on = 0;
    unsigned char *raw = cv_read(path, CV_MAP_MAX, &n);
    char *text = NULL, *out = NULL, cause[600] = "";
    int r;
    if (!raw) return;
    if (kind == CV_MAP_SAVED) {
        text = cv_saved_text(raw, n, &tn);
        if (!text) { free(raw); return; }
    } else {
        text = (char *)raw;
        tn = n;
    }
    r = kind == CV_MAP_PREFAB ? cv_prefab_text(x->ids, text, tn, &out, &on, cause, sizeof cause)
                              : cv_map_text_ids(o, x->ids, text, tn, &out, &on, cause, sizeof cause);
    if (r < 0) cv_map_left(o, x, path, raw, n, cause);
    else if (r > 0 && kind != CV_MAP_SAVED) {
        if (!cv_backup(path)) cv_map_left(o, x, path, raw, n, "a copy of it as it was could not be written beside it");
        else if (!cv_write(path, out, on)) cv_map_left(o, x, path, raw, n, "it could not be written (a program may hold it)");
        else {
            x->st->maps_converted++;
            x->did = 1;
            cv_rec_set(&x->ids->rec, "maps", path, NULL);
            cv_say(o, 0, "  %s: converted (%s as it was: %s%s)", path, kind == CV_MAP_PREFAB ? "the prefab" : "the rawmap",
                   strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path, SH_CONV_BACKUP_SUFFIX);
        }
    } else if (r > 0) {
        /* A saved map: re-framed, re-signed with the account its sidecar verifies with. */
        char *vpath = (char *)malloc(strlen(path) + 8), user[32];
        unsigned char *side = NULL, nside[SH_STORE_SIDECAR_LEN], *z = NULL, *nraw = NULL;
        size_t sn = 0, zn = 0;
        if (vpath) _snprintf_s(vpath, strlen(path) + 8, _TRUNCATE, "%s.verify", path);
        side = vpath ? cv_read(vpath, 64, &sn) : NULL;
        if (!side || sn != SH_STORE_SIDECAR_LEN) cv_map_left(o, x, path, raw, n, "its map.decl.verify is missing or not 40 bytes");
        else if (!sh_store_sidecar_user(account, raw, n, side, "map.decl", user, sizeof user))
            cv_map_left(o, x, path, raw, n, "its map.decl.verify does not match this Steam account, so it cannot be signed again");
        else if (!(z = sh_store_zlib((const unsigned char *)out, on, &zn)) || !(nraw = (unsigned char *)malloc(zn + 4)))
            cv_map_left(o, x, path, raw, n, "out of memory");
        else {
            size_t back = 0;
            char *check;
            memcpy(nraw, raw, 4);
            memcpy(nraw + 4, z, zn);
            check = cv_saved_text(nraw, zn + 4, &back);
            if (!check || back != on || memcmp(check, out, on))
                cv_map_left(o, x, path, raw, n, "the rewritten map did not read back the same");
            else if (!sh_store_sidecar(nraw, zn + 4, side, user, "map.decl", nside))
                cv_map_left(o, x, path, raw, n, "it could not be signed");
            else if (!cv_backup(path) || !cv_backup(vpath))
                cv_map_left(o, x, path, raw, n, "a copy of it as it was could not be written beside it");
            else if (!cv_write(path, nraw, zn + 4))
                cv_map_left(o, x, path, raw, n, "it could not be written (a program may hold it)");
            else if (!cv_write(vpath, nside, sizeof nside)) {
                cv_write(path, raw, n);   /* the map as it was, with its own sidecar */
                cv_map_left(o, x, path, raw, n, "its map.decl.verify could not be written; the map is as it was");
            } else {
                x->st->maps_converted++;
                x->did = 1;
                cv_rec_set(&x->ids->rec, "maps", path, NULL);
                cv_say(o, 0, "  %s: converted (map.decl and map.decl.verify as they were: *%s)", path, SH_CONV_BACKUP_SUFFIX);
            }
            free(check);
        }
        free(vpath);
        free(side);
        free(z);
        free(nraw);
    }
    free(out);
    if (kind == CV_MAP_SAVED) free(text);
    free(raw);
}

int sh_conv_maps(const sh_conv_opts *o, sh_conv_stats *st)
{
    cv_ids ids;
    cv_map_ctx x;
    if (!o) return -1;
    cv_ids_open(o, &ids);
    x.ids = &ids;
    x.st = st;
    x.did = 0;
    cv_maps_each(o, cv_do_map, &x);
    if (x.did && !cv_rec_save(o, &ids.rec)) cv_say(o, 1, "  mods\\snapmap-plus.json: the conversion record could not be written");
    cv_ids_close(&ids);
    return x.did;
}
