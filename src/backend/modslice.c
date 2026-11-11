/* modslice.c -- see modslice.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>

#include "modslice.h"
#include "../common/text.h"

/* ------------------------------------------------------------ names ---- */

static int sl_lower(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c; }

/* A character a name in decl text or in a binary is spelled with. */
static int sl_name_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           c == '/' || c == '\\' || c == '$' || c == '-' || c == '+' || c == '@' || c == '~' || c == '(' || c == ')';
}

/* "mods/<salt>" at s (n bytes left), any case. */
static int sl_salt_at(const char *s, size_t n, const char *salt, size_t sl)
{
    size_t i;
    if (n < 5 + sl) return 0;
    if (sl_lower((unsigned char)s[0]) != 'm' || sl_lower((unsigned char)s[1]) != 'o' ||
        sl_lower((unsigned char)s[2]) != 'd' || sl_lower((unsigned char)s[3]) != 's' || (s[4] != '/' && s[4] != '\\'))
        return 0;
    for (i = 0; i < sl; i++) if (sl_lower((unsigned char)s[5 + i]) != sl_lower((unsigned char)salt[i])) return 0;
    return 1;
}

/* The key of the run at s (it starts with "mods/<salt>"): lower case, '\'
 * as '/', cut at '$', every character but [a-z0-9/._] as '_', the last
 * segment's extensions cut. NULL when nothing follows "mods/<salt>/". */
static char *sl_key_run(const char *s, size_t n, size_t sl)
{
    char *k;
    size_t i, len = 0, last = 0;
    for (i = 0; i < n && sl_name_char((unsigned char)s[i]) && s[i] != '$'; i++) {}
    n = i;
    if (n <= 6 + sl) return NULL;
    if (!(k = (char *)malloc(n + 1))) return NULL;
    for (i = 0; i < n; i++) {
        int c = sl_lower((unsigned char)s[i]);
        if (c == '\\') c = '/';
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_')) c = '_';
        k[len++] = (char)c;
        if (c == '/') last = len;
    }
    k[len] = 0;
    while (len > last && k[len - 1] == '/') k[--len] = 0;
    {
        char *dot = strchr(k + last, '.');
        if (dot) *dot = 0;
    }
    len = strlen(k);
    if (len <= 6 + sl || k[len - 1] == '/') { free(k); return NULL; }
    return k;
}

char *sh_slice_key(const char *s, size_t n, const char *salt)
{
    size_t i, sl = salt ? strlen(salt) : 0;
    if (!s || !sl) return NULL;
    for (i = 0; i + 5 + sl <= n; i++)
        if (sl_salt_at(s + i, n - i, salt, sl)) return sl_key_run(s + i, n - i, sl);
    return NULL;
}

int sh_slice_enumerated_type(const char *type)
{
    /* The conductor numbers the AI types: the encounters and scores of a
     * mod's demons are found by that number, never by a name. */
    static const char *const types[] = { "snapAIScore", "snapAIEncounter", "snapAIEncounterTable", NULL };
    int i;
    for (i = 0; type && types[i]; i++) if (!_stricmp(type, types[i])) return 1;
    return 0;
}

/* ------------------------------------------------------------ the walk ---- */

typedef struct sl_pair { const char *key; size_t file; } sl_pair;

typedef struct sl_walk {
    const sh_mb_bundle *b;
    const char *folder;
    size_t sl;
    sl_pair *keys;          /* sorted by key */
    size_t nkeys;
    sl_pair *reqs;          /* sorted by request */
    size_t nreqs;
    char **owned;           /* the keys, freed at the end */
    unsigned char *keep;
    size_t *queue, qhead, qtail;
    char *whole;            /* the reason the whole build ships, or NULL */
    int oom;
} sl_walk;

static int sl_pair_cmp(const void *a, const void *b)
{
    const sl_pair *x = (const sl_pair *)a, *y = (const sl_pair *)b;
    int c = strcmp(x->key, y->key);
    return c ? c : (x->file < y->file ? -1 : x->file > y->file);
}

static void sl_whole(sl_walk *w, const char *fmt, const char *arg)
{
    if (w->whole) return;
    w->whole = sh_textf(fmt, arg);
    if (!w->whole) w->oom = 1;
}

static void sl_reach(sl_walk *w, size_t i)
{
    if (w->keep[i]) return;
    w->keep[i] = 1;
    w->queue[w->qtail++] = i;
}

/* The first pair at or after `key` in a sorted array. */
static size_t sl_lower_bound(const sl_pair *p, size_t n, const char *key)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(p[mid].key, key) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static void sl_reach_key(sl_walk *w, const char *key)
{
    size_t i = sl_lower_bound(w->keys, w->nkeys, key);
    for (; i < w->nkeys && !strcmp(w->keys[i].key, key); i++) sl_reach(w, w->keys[i].file);
}

/* A spelled name reaches the file of that name and the file of each of its
 * leading folders below mods/<salt>/: a path inside a decl
 * ("mods/<salt>/web/base/idle", a node of the animWeb mods/<salt>/web) names
 * that decl. */
static void sl_reach_spelled(sl_walk *w, char *key)
{
    size_t floor = 5 + w->sl + 1, n = strlen(key);
    sl_reach_key(w, key);
    while (n > floor) {
        while (n > floor && key[n - 1] != '/') n--;
        if (n <= floor) break;
        key[--n] = 0;
        sl_reach_key(w, key);
    }
}

/* Every file whose key lies below `key` (a module's own files). */
static void sl_reach_below(sl_walk *w, const char *key)
{
    size_t kl = strlen(key), i = sl_lower_bound(w->keys, w->nkeys, key);
    for (; i < w->nkeys && !strncmp(w->keys[i].key, key, kl); i++)
        if (w->keys[i].key[kl] == 0 || w->keys[i].key[kl] == '/') sl_reach(w, w->keys[i].file);
}

static void sl_reach_request(sl_walk *w, const char *request)
{
    size_t i = sl_lower_bound(w->reqs, w->nreqs, request);
    for (; i < w->nreqs && !strcmp(w->reqs[i].key, request); i++) sl_reach(w, w->reqs[i].file);
}

static void sl_reach_name(sl_walk *w, const char *name, size_t n)
{
    char *k = sh_slice_key(name, n, w->b->id);
    if (k) { sl_reach_spelled(w, k); free(k); }
}

/* Every name the bytes spell under mods/<salt>/. */
static void sl_scan(sl_walk *w, const unsigned char *t, size_t n)
{
    size_t i;
    for (i = 0; i + 5 + w->sl <= n; i++) {
        char *k;
        if (sl_lower(t[i]) != 'm' || !sl_salt_at((const char *)t + i, n - i, w->b->id, w->sl)) continue;
        k = sl_key_run((const char *)t + i, n - i, w->sl);
        if (k) { sl_reach_spelled(w, k); free(k); }
        i += 4 + w->sl;
    }
}

/* The file's bytes from the folder (malloc'd), or NULL. */
static unsigned char *sl_read(const sl_walk *w, const sh_mb_file *f, size_t *len)
{
    char *path = sh_textf("%s\\%s", w->folder, f->path);
    unsigned char *bytes;
    char *p;
    if (!path) return NULL;
    for (p = path; *p; p++) if (*p == '/') *p = '\\';
    bytes = sh_mb_read_file(path, (size_t)-1, len);
    free(path);
    return bytes;
}

static int sl_is(const sh_mb_file *f, const char *type) { return f->ptype && !_stricmp(f->ptype, type); }

static void sl_reach_all(sl_walk *w, const char *type)
{
    size_t i;
    for (i = 0; i < w->b->file_count; i++)
        if (!w->b->files[i].absent && sl_is(&w->b->files[i], type)) sl_reach(w, i);
}

static void sl_visit(sl_walk *w, size_t i)
{
    const sh_mb_file *f = &w->b->files[i];
    size_t k, len = 0;
    unsigned char *bytes;
    if (sl_is(f, "file") && !f->stock && !sh_mb_vt_member(f->request, NULL, NULL)) {
        sl_whole(w, "%s is a kind of file the cut does not read", f->path);
        return;
    }
    if (f->is_module || sl_is(f, "snapModuleInfo")) {
        char *own = sh_slice_key(f->request, strlen(f->request), w->b->id);
        if (own) { sl_reach_below(w, own); free(own); }
    }
    if (sl_is(f, "sound")) sl_reach_all(w, "soundBank");
    if (sl_is(f, "renderProg")) sl_reach_all(w, "renderProg");
    for (k = 0; k < f->need_count; k++) {
        char *req[4];
        int n = sh_mb_need_requests(f->needs[k].type, f->needs[k].name, req, 4), r;
        for (r = 0; r < n; r++) sl_reach_request(w, req[r]);
        sh_mb_requests_free(req, n);
        sl_reach_name(w, f->needs[k].name, strlen(f->needs[k].name));
    }
    if (f->stock) return;   /* the game's own bytes: they spell nothing of the mod */
    bytes = sl_read(w, f, &len);
    if (!bytes) {
        sl_whole(w, "%s could not be read", f->path);
        return;
    }
    sl_scan(w, bytes, len);
    free(bytes);
}

/* Index the files: by key (a virtual-texture member under its family's) and
 * by request. */
static int sl_index(sl_walk *w)
{
    const sh_mb_bundle *b = w->b;
    size_t i;
    w->keys = (sl_pair *)calloc(b->file_count + 1, sizeof *w->keys);
    w->reqs = (sl_pair *)calloc(b->file_count + 1, sizeof *w->reqs);
    w->owned = (char **)calloc(b->file_count + 1, sizeof *w->owned);
    if (!w->keys || !w->reqs || !w->owned) return 0;
    for (i = 0; i < b->file_count; i++) {
        const sh_mb_file *f = &b->files[i];
        char *family = NULL, *k;
        if (f->absent) continue;
        w->reqs[w->nreqs].key = f->request;
        w->reqs[w->nreqs++].file = i;
        if (sh_mb_vt_member(f->request, &family, NULL) && family) {
            k = sh_slice_key(family, strlen(family), b->id);
            free(family);
        } else {
            free(family);
            k = sh_slice_key(f->request, strlen(f->request), b->id);
        }
        if (!k) continue;
        w->owned[i] = k;
        w->keys[w->nkeys].key = k;
        w->keys[w->nkeys++].file = i;
    }
    qsort(w->keys, w->nkeys, sizeof *w->keys, sl_pair_cmp);
    qsort(w->reqs, w->nreqs, sizeof *w->reqs, sl_pair_cmp);
    return 1;
}

/* The Toybox rows of the mod whose bytes name a placed name. */
static void sl_rows_of(sl_walk *w, char **seeds, size_t nseeds)
{
    size_t i, s;
    for (i = 0; i < w->b->file_count && nseeds; i++) {
        const sh_mb_file *f = &w->b->files[i];
        unsigned char *bytes;
        size_t len = 0, p;
        int hit = 0;
        if (f->absent || f->stock || !w->owned[i] || !sl_is(f, "snapEditorEntityDef") || w->keep[i]) continue;
        if (!(bytes = sl_read(w, f, &len))) { sl_whole(w, "%s could not be read", f->path); return; }
        for (p = 0; !hit && p + 5 + w->sl <= len; p++) {
            char *k;
            if (!sl_salt_at((const char *)bytes + p, len - p, w->b->id, w->sl)) continue;
            k = sl_key_run((const char *)bytes + p, len - p, w->sl);
            for (s = 0; k && s < nseeds && !hit; s++) hit = !strcmp(k, seeds[s]);
            free(k);
        }
        free(bytes);
        if (hit) sl_reach(w, i);
    }
}

void sh_slice_plan_free(sh_slice_plan *plan)
{
    if (!plan) return;
    free(plan->why);
    free(plan->keep);
    memset(plan, 0, sizeof *plan);
}

static int sl_plan_whole(sh_slice_plan *plan, const char *why)
{
    free(plan->keep);
    plan->keep = NULL;
    plan->whole = 1;
    free(plan->why);
    plan->why = sh_text(why);
    plan->kept_files = plan->files;
    plan->kept_bytes = plan->bytes;
    return plan->why != NULL;
}

int sh_slice_needed(const sh_mb_bundle *src, const char *folder, const char *const *names, size_t name_count,
                    int attached, const char *const *asked, size_t asked_count, sh_slice_plan *plan)
{
    sl_walk w;
    char **seeds = NULL;
    size_t i, nseeds = 0, editor_files = 0;
    int ok = 0;
    if (!plan) return 0;
    memset(plan, 0, sizeof *plan);
    if (!src || !folder) return 0;
    for (i = 0; i < src->file_count; i++)
        if (!src->files[i].stock) { plan->files++; plan->bytes += src->files[i].size; }
    if (attached) return sl_plan_whole(plan, "the map uses the mod as a whole");
    if (!sh_mb_sliceable(src)) return sl_plan_whole(plan, "the mod carries scripts");
    if (!name_count) return sl_plan_whole(plan, "the map places nothing of it");

    memset(&w, 0, sizeof w);
    w.b = src;
    w.folder = folder;
    w.sl = strlen(src->id);
    w.keep = (unsigned char *)calloc(src->file_count + 1, 1);
    w.queue = (size_t *)calloc(src->file_count + 1, sizeof *w.queue);
    seeds = (char **)calloc(name_count + 1, sizeof *seeds);
    if (!w.keep || !w.queue || !seeds || !sl_index(&w)) goto done;

    /* What always travels, and what never does. */
    for (i = 0; i < src->file_count; i++) {
        const sh_mb_file *f = &src->files[i];
        if (f->absent) continue;
        if (f->cls == SH_MB_CLASS_EDITOR) continue;
        if (f->cls == SH_MB_CLASS_REPLACE || (f->is_decl && sh_slice_enumerated_type(f->ptype))) sl_reach(&w, i);
    }
    /* The placed names, the Toybox rows naming them, and what players asked for. */
    for (i = 0; i < name_count; i++) {
        size_t k;
        if (!names[i] || !names[i][0]) continue;
        seeds[nseeds] = sh_slice_key(names[i], strlen(names[i]), src->id);
        if (seeds[nseeds]) sl_reach_key(&w, seeds[nseeds++]);
        for (k = 0; k < src->file_count; k++)
            if (!src->files[k].absent && src->files[k].pname && !_stricmp(src->files[k].pname, names[i])) sl_reach(&w, k);
    }
    sl_rows_of(&w, seeds, nseeds);
    for (i = 0; i < asked_count; i++) {
        char *lower;
        if (!asked[i] || !asked[i][0] || !(lower = sh_text(asked[i]))) continue;
        for (char *c = lower; *c; c++) *c = (char)sl_lower((unsigned char)*c);
        sl_reach_request(&w, lower);
        sl_reach_name(&w, lower, strlen(lower));
        free(lower);
    }
    while (!w.whole && !w.oom && w.qhead < w.qtail) sl_visit(&w, w.queue[w.qhead++]);
    if (w.oom) goto done;
    if (w.whole) { ok = sl_plan_whole(plan, w.whole); goto done; }

    for (i = 0; i < src->file_count; i++) {
        const sh_mb_file *f = &src->files[i];
        if (f->stock) continue;
        if (f->cls == SH_MB_CLASS_EDITOR) editor_files++;
        if (!w.keep[i]) continue;
        plan->kept_files++;
        plan->kept_bytes += f->size;
    }
    if (plan->kept_files + editor_files == plan->files) {
        ok = sl_plan_whole(plan, "the map uses every file of it");
        goto done;
    }
    /* What a player downloads of a cut is its files and the source's list of
     * files (source.json): when that is more than three quarters of the
     * whole build's files, a second archive of nearly the same files is not
     * worth it, nor the cut's build at the publish save. */
    if ((plan->kept_bytes + src->canonical_len) * 4 > plan->bytes * 3) {
        ok = sl_plan_whole(plan, "the map uses more than three quarters of it");
        goto done;
    }
    plan->keep = w.keep;
    w.keep = NULL;
    ok = 1;
done:
    for (i = 0; i < nseeds; i++) free(seeds[i]);
    free(seeds);
    if (w.owned) for (i = 0; i < src->file_count; i++) free(w.owned[i]);
    free(w.owned);
    free(w.keys);
    free(w.reqs);
    free(w.keep);
    free(w.queue);
    free(w.whole);
    if (!ok) sh_slice_plan_free(plan);
    return ok;
}

/* ------------------------------------------------------------ the cut ---- */

static int sl_mkdirs(const wchar_t *path)
{
    wchar_t *p, *copy;
    if (CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS) return 1;
    if (!(copy = _wcsdup(path))) return 0;
    p = wcsrchr(copy, L'\\');
    if (p && p > copy) {
        *p = 0;
        if (!sl_mkdirs(copy)) { free(copy); return 0; }
    }
    free(copy);
    return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

/* Remove the folder `top` and everything below it (it was made here). */
static void sl_rmtree(const wchar_t *top)
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
                    sl_rmtree(child);
                else {
                    SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                    DeleteFileW(child);
                }
                free(child);
            }
        }
        if (!FindNextFileW(h, &fd)) { FindClose(h); h = INVALID_HANDLE_VALUE; }
    }
    RemoveDirectoryW(top);
}

static wchar_t *sl_wjoin(const char *dir, const char *rel)
{
    char *u = sh_textf("%s\\%s", dir, rel), *p;
    wchar_t *w;
    if (!u) return NULL;
    for (p = u; *p; p++) if (*p == '/') *p = '\\';
    w = sh_mb_wpath(u);
    free(u);
    return w;
}

static int sl_write(const char *dir, const char *rel, const void *data, size_t len)
{
    wchar_t *w = sl_wjoin(dir, rel);
    HANDLE h;
    DWORD put = 0;
    int ok;
    if (!w) return 0;
    h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return 0;
    ok = len <= 0xFFFFFFFFu && WriteFile(h, data, (DWORD)len, &put, NULL) && put == (DWORD)len;
    CloseHandle(h);
    return ok;
}

/* The source's author part with "slice-of" added as its last member. */
static char *sl_author(const char *folder, const char *of, size_t *out_len)
{
    char *mj = sh_textf("%s\\mod.json", folder), *author, *out = NULL;
    unsigned char *text;
    size_t len = 0, alen = 0, close, open, k;
    if (!mj) return NULL;
    text = sh_mb_read_file(mj, (size_t)-1, &len);
    free(mj);
    if (!text) return NULL;
    author = sh_mb_author_bytes((const char *)text, len, &alen, NULL);
    free(text);
    if (!author) return NULL;
    for (open = 0; open < alen && author[open] != '{'; open++) {}
    for (close = alen; close > open && author[close - 1] != '}'; close--) {}
    if (close > open + 1) {
        /* The text up to the last member's end, then the new member. */
        k = close - 1;
        while (k > open + 1 && strchr(" \t\r\n", author[k - 1])) k--;
        out = sh_textf("%.*s%s\n  \"" SH_MB_SLICE_KEY "\": \"%s\"\n}\n", (int)k, author, k == open + 1 ? "" : ",", of);
    }
    free(author);
    if (out && out_len) *out_len = strlen(out);
    return out;
}

/* 1 when one of the need's requests is a file of `b`. */
static int sl_need_local(const sh_mb_bundle *b, const sh_mb_need *need)
{
    char *req[4];
    int n = sh_mb_need_requests(need->type, need->name, req, 4), r, hit = 0;
    size_t i;
    for (r = 0; r < n && !hit; r++)
        for (i = 0; i < b->file_count && !hit; i++) hit = !strcmp(b->files[i].request, req[r]);
    sh_mb_requests_free(req, n);
    return hit;
}

static const sh_mb_file *sl_file_at(const sh_mb_bundle *b, const char *request)
{
    size_t i;
    for (i = 0; i < b->file_count; i++) if (!strcmp(b->files[i].request, request)) return &b->files[i];
    return NULL;
}

/* The proof: what the cut built is the source's own bytes, and every need a
 * kept file resolved inside the source resolves inside the cut. */
static int sl_prove(const sh_mb_bundle *src, const sh_mb_bundle *cut, char **why)
{
    size_t i, k;
    if (_stricmp(cut->slice_of, src->hash) != 0) return sh_text_set(why, "it does not name the build it came from"), 0;
    if (strcmp(cut->id, src->id) != 0) return sh_text_set(why, "it is another mod"), 0;
    for (i = 0; i < cut->file_count; i++) {
        const sh_mb_file *f = &cut->files[i], *s;
        if (f->stock) continue;
        s = sl_file_at(src, f->request);
        if (!s || s->stock || s->size != f->size || strcmp(s->blob, f->blob) != 0)
            return sh_text_set(why, "%s is not the source build's file", f->path), 0;
    }
    for (i = 0; i < cut->file_count; i++) {
        const sh_mb_file *f = &cut->files[i];
        for (k = 0; k < f->need_count; k++)
            if (!sl_need_local(cut, &f->needs[k]) && sl_need_local(src, &f->needs[k]))
                return sh_text_set(why, "%s needs %s %s, which the cut leaves out", f->path, f->needs[k].type,
                                   f->needs[k].name), 0;
    }
    return 1;
}

int sh_slice_cut(const sh_mb_bundle *src, const char *folder, const sh_slice_plan *plan, const char *dest,
                 const sh_mb_resolver *stock, sh_mb_bundle *out, char **why)
{
    sh_mb_resolver r;
    wchar_t *wdest = NULL;
    char *author = NULL, *one = NULL;
    size_t i, alen = 0;
    char *reason = NULL;
    int ok = 0, made = 0;
    if (why) *why = NULL;
    if (out) memset(out, 0, sizeof *out);
    if (!src || !folder || !plan || plan->whole || !plan->keep || !dest || !out) {
        if (why) *why = sh_text("nothing to cut");
        return 0;
    }
    if (!(wdest = sh_mb_wpath(dest))) { if (why) *why = sh_text("out of memory"); return 0; }
    if (GetFileAttributesW(wdest) != INVALID_FILE_ATTRIBUTES || !sl_mkdirs(wdest)) {
        if (why) *why = sh_textf("its folder %s could not be made", dest);
        free(wdest);
        return 0;
    }
    made = 1;
    for (i = 0; i < src->file_count; i++) {
        const sh_mb_file *f = &src->files[i];
        wchar_t *from, *to, *slash;
        int copied;
        if (!plan->keep[i] || f->absent) continue;
        from = sl_wjoin(folder, f->path);
        to = sl_wjoin(dest, f->path);
        slash = to ? wcsrchr(to, L'\\') : NULL;
        if (slash) { *slash = 0; sl_mkdirs(to); *slash = L'\\'; }
        copied = from && to && CopyFileW(from, to, TRUE);
        if (copied) SetFileAttributesW(to, FILE_ATTRIBUTE_NORMAL);
        free(from);
        free(to);
        /* A pointer's file the folder no longer holds: the game supplies it. */
        if (!copied && !f->stock) { sh_text_set(&reason, "%s could not be copied", f->path); goto done; }
    }
    if (!(author = sl_author(folder, src->hash, &alen)) || !sl_write(dest, "mod.json", author, alen) ||
        !src->canonical || !sl_write(dest, SH_SLICE_SOURCE_FILE, src->canonical, src->canonical_len)) {
        sh_text_set(&reason, "its mod.json could not be written");
        goto done;
    }
    memset(&r, 0, sizeof r);
    if (stock) { r.stock = stock->stock; r.stock_context = stock->stock_context; }
    if (!sh_mb_build(dest, src->id, &r, out)) {
        sh_text_set(&reason, "it does not build on its own: %s", sh_mb_first_cause(out));
        goto done;
    }
    if (!sl_prove(src, out, &reason)) goto done;
    if (!sh_mb_write(out, dest, &one)) {
        sh_text_set(&reason, "its mod.json could not be written: %s", one ? one : "");
        goto done;
    }
    ok = 1;
done:
    if (!ok) {
        if (!reason) reason = sh_text("out of memory");
        sh_mb_free(out);
        memset(out, 0, sizeof *out);
        if (made) sl_rmtree(wdest);
    }
    if (why) *why = reason;
    else free(reason);
    free(one);
    free(author);
    free(wdest);
    return ok;
}
