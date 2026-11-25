/* modexport.c -- see modexport.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>

#include "modexport.h"
#include "modbundle.h"
#include "modmanifest.h"
#include "modcache.h"
#include "modpin.h"
#include "mp_json.h"
#include "prefab_block.h"
#include "backend_log.h"
#include "plain_reason.h"
#include "modzip.h"
#include "map_carrier.h"
#include "modui.h"
#include "../common/text.h"

/* Set *cause (malloc'd; any previous one is freed) to the sentence; returns 0. */
static int me_cause(char **cause, const char *fmt, ...)
{
    va_list ap;
    char *text;
    if (cause) {
        va_start(ap, fmt);
        text = sh_vtextf(fmt, ap);
        va_end(ap);
        if (text) { free(*cause); *cause = text; }
    }
    return 0;
}

static void me_log(const char *fmt, ...)
{
    va_list ap;
    char *line;
    va_start(ap, fmt);
    line = sh_vtextf(fmt, ap);
    va_end(ap);
    backend_log(line ? line : "MODS: export: (out of memory)");
    free(line);
}

static double me_now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

int modexport_is_zip_path(const char *path)
{
    size_t n = path ? strlen(path) : 0;
    return n > 4 && _stricmp(path + n - 4, ".zip") == 0;
}

/* The entry name of a map's export: the ZIP's base name with .json ("my map.zip" -> "my map.json"), malloc'd, or NULL. */
static char *me_map_entry(const char *zip_path)
{
    const char *base = zip_path, *p;
    size_t n;
    char *out;
    for (p = zip_path; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
    n = strlen(base);
    if (n > 4 && !_stricmp(base + n - 4, ".zip")) n -= 4;
    if (!n) return sh_text(MODEXPORT_MAP_FALLBACK);
    out = sh_textf("%.*s.json", (int)n, base);
    if (out && !sh_zip_name_ok(out, strlen(out))) { free(out); out = sh_text(MODEXPORT_MAP_FALLBACK); }
    return out;
}

static int me_hex64(const char *s, size_t n)
{
    size_t i;
    if (n != SH_MB_HASH_HEX) return 0;
    for (i = 0; i < n; i++) if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

typedef struct me_payload {
    const char *entry;                 /* "<name>.json" */
    const char *text;
    size_t len;
    const sh_modmanifest_list *rows;   /* prefab: the rows; map: NULL (read from its text) */
    modexport_prefab_fn text_for;      /* prefab: the text to carry for the pinned rows */
    void *ctx;
} me_payload;

/* The names already written (sorted lookups are not worth it: a map names few units, a unit few hundred files). */
typedef struct me_seen { char **v; size_t n, cap; } me_seen;
static int me_seen_add(me_seen *s, const char *name)
{
    size_t i;
    for (i = 0; i < s->n; i++) if (!strcmp(s->v[i], name)) return 0;
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 64;
        char **g = (char **)realloc(s->v, cap * sizeof *g);
        if (!g) return -1;
        s->v = g;
        s->cap = cap;
    }
    if (!(s->v[s->n] = sh_text(name))) return -1;
    s->n++;
    return 1;
}
static void me_seen_free(me_seen *s)
{
    size_t i;
    for (i = 0; i < s->n; i++) free(s->v[i]);
    free(s->v);
    memset(s, 0, sizeof *s);
}

static const char *me_row_label(const sh_modmanifest_row *row)
{
    const char *n = sh_modmanifest_row_name(row);
    return n && n[0] ? n : "a mod";
}

/* One unit: units/<hash>.json, then every file it carries as files/<sha256>, each once per export. The bytes come
 * from the content cache, else from a local mod's file with that SHA-256. */
static int me_add_unit(sh_zip_writer *w, const char *what, const sh_modmanifest_row *row, const char *hash,
                       me_seen *seen, modexport_stats *st, char **cause)
{
    char *manifest, *name;
    size_t mlen = 0;
    mpj *m;
    const mpj *files;
    int k, r;
    if ((r = me_seen_add(seen, hash)) <= 0) return r == 0 ? 1 : me_cause(cause, "out of memory");
    if (!(manifest = sh_cache_manifest(hash, &mlen)))
        return me_cause(cause, "the content of %s that this %s uses is not on this computer. %s, or install the "
                        "mod, then export again", me_row_label(row), what,
                        strcmp(what, "map") ? "Import the prefab's .zip export again" : "Play the map once online");
    name = sh_textf("units/%s.json", hash);
    if (!name) { free(manifest); return me_cause(cause, "out of memory"); }
    sh_zip_writer_add(w, name, manifest, mlen);
    free(name);
    st->bundles_installed++;   /* units carried */
    m = mpj_parse(manifest, mlen);
    free(manifest);
    files = m ? mpj_get(m, "files") : NULL;
    for (k = 0; files && files->type == MPJ_ARR && k < files->n && !w->failed; k++) {
        const mpj *sha = mpj_get(files->items[k], "sha256"), *size = mpj_get(files->items[k], "size");
        char *src, actual[SH_MB_HASH_HEX + 1];
        unsigned char *bytes;
        size_t blen = 0;
        if (!sha || sha->type != MPJ_STR || !size || !mpj_is_num(size)) continue;
        if ((r = me_seen_add(seen, sha->text)) <= 0) { if (r < 0) { mpj_free(m); return me_cause(cause, "out of memory"); } continue; }
        src = sh_cache_file_source(sha->text, (unsigned long long)mpj_int(size));
        bytes = src ? sh_mb_read_file(src, (size_t)-1, &blen) : NULL;
        free(src);
        if (bytes) {
            sh_mb_sha256_hex(bytes, blen, actual);
            if ((unsigned long long)blen != (unsigned long long)mpj_int(size) || strcmp(actual, sha->text)) { free(bytes); bytes = NULL; }
        }
        if (!bytes) {
            mpj_free(m);
            return me_cause(cause, "some files of %s that this %s uses are not on this computer. %s, or install the "
                            "mod, then export again", me_row_label(row), what,
                            strcmp(what, "map") ? "Import the prefab's .zip export again" : "Play the map once online");
        }
        name = sh_textf("files/%s", sha->text);
        if (!name) { free(bytes); mpj_free(m); return me_cause(cause, "out of memory"); }
        sh_zip_writer_add(w, name, bytes, blen);
        free(name);
        free(bytes);
        st->files++;
    }
    mpj_free(m);
    if (w->failed) return me_cause(cause, "%s", w->cause);
    return 1;
}

static int me_write(const me_payload *pl, const char *zip_path, modexport_stats *stp, char **cause)
{
    const int prefab = pl->rows != NULL;
    const char *what = prefab ? "prefab" : "map";
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    sh_modmanifest_list rows;
    sh_modpin_result pr;
    sh_zip_writer w;
    me_seen seen;
    wchar_t *wfinal = NULL, *wtmp = NULL;
    char *tmp = NULL, *text = NULL;
    size_t i, k, len = 0;
    unsigned rejected = 0;
    int ok = 0, heap = 0;
    double t0 = me_now_ms();

    memset(st, 0, sizeof *st);
    memset(&rows, 0, sizeof rows);
    memset(&pr, 0, sizeof pr);
    memset(&w, 0, sizeof w);
    memset(&seen, 0, sizeof seen);
    w.h = INVALID_HANDLE_VALUE;
    if (!pl->text || !pl->len || !zip_path || !zip_path[0])
        return me_cause(cause, prefab ? "no prefab text or no destination" : "no map text or no destination");
    if (prefab) {
        for (i = 0; i < pl->rows->count; i++)
            if (!sh_modmanifest_list_add(&rows, &pl->rows->rows[i])) { sh_modmanifest_list_free(&rows); return me_cause(cause, "out of memory"); }
    } else {
        if (sh_map_old_shape(pl->text, pl->len) != SH_MAP_OLD_NONE) return me_cause(cause, "%s", MODEXPORT_EARLIER);
        sh_modmanifest_read(pl->text, pl->len, &rows, &rejected);
    }
    st->bundles = (unsigned)rows.count;

    /* The rows leave as units: every local row is pinned from its folder (modpin.h); a row no folder here serves
     * cannot be carried. */
    if (rows.count && sh_modpin_rows(&rows, pl->text, pl->len, NULL, &pr) < 0) {
        me_cause(cause, "the %s's mod content could not be prepared. Try again", what);
        goto done;
    }
    for (i = 0; i < pr.count; i++)
        if (pr.rows[i].state == SH_MODPIN_ABSENT) {
            for (k = 0; k < rows.count && strcmp(rows.rows[k].salt, pr.rows[i].salt); k++) {}
            me_cause(cause, "this %s uses %s, which is not on this computer. Install it in the Mods tab, or remove its "
                     "objects from the %s", what, k < rows.count ? me_row_label(&rows.rows[k]) : "a mod", what);
            goto done;
        }
    for (i = 0; i < rows.count; i++)
        if (!rows.rows[i].pinned) {
            me_cause(cause, "this %s uses %s, whose content could not be prepared. Press Refresh in the Mods tab, "
                     "then export again", what, me_row_label(&rows.rows[i]));
            goto done;
        }

    /* The text the ZIP carries names those units. */
    if (prefab) {
        text = pl->text_for ? pl->text_for(pl->ctx, &rows, 1, &len) : NULL;
        if (pl->text_for && (!text || !len)) { me_cause(cause, "the prefab's export could not be written. Export it again"); goto done; }
        if (!text) { text = sh_textn(pl->text, pl->len); len = pl->len; }
    } else if (rows.count) {
        char err[160] = "";
        size_t n = 0;
        char *written = sh_modmanifest_write(pl->text, pl->len, &rows, &n, err, sizeof err);
        if (!written) { me_cause(cause, "the map's list of mods could not be written (%s). Save the map again", err[0] ? err : "out of memory"); goto done; }
        text = written;
        len = n;
        heap = 1;
    } else { text = sh_textn(pl->text, pl->len); len = pl->len; }
    if (!text) { me_cause(cause, "out of memory"); goto done; }
    st->map_bytes = len;

    tmp = sh_textf("%s.smp-%lu.tmp", zip_path, GetCurrentProcessId());
    wfinal = sh_mb_wpath(zip_path);
    wtmp = tmp ? sh_mb_wpath(tmp) : NULL;
    if (!wfinal || !wtmp) { me_cause(cause, "the destination path is not usable"); goto done; }
    if (!sh_zip_writer_open(&w, wtmp)) { me_cause(cause, "cannot create %s: %s", zip_path, sh_reason_win32(GetLastError(), zip_path, 0)); goto done; }
    sh_zip_writer_add(&w, pl->entry, text, len);
    for (i = 0; i < rows.count && !w.failed; i++)
        for (k = 0; k < rows.rows[i].unit_count; k++)
            if (!me_add_unit(&w, what, &rows.rows[i], rows.rows[i].units[k], &seen, st, cause)) goto done;
    sh_zip_writer_finish(&w);
    if (w.failed) { me_cause(cause, "%s", w.cause); goto done; }
    if (!FlushFileBuffers(w.h)) { me_cause(cause, "writing the ZIP failed: %s", sh_reason_win32(GetLastError(), zip_path, 0)); goto done; }
    CloseHandle(w.h);
    w.h = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(wtmp, wfinal, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        me_cause(cause, "cannot replace %s: %s", zip_path, sh_reason_win32(GetLastError(), zip_path, 0));
        goto done;
    }
    st->entries = (unsigned)w.n;
    st->zip_bytes = w.off;
    ok = 1;
done:
    sh_zip_writer_free(&w);
    if (!ok && wtmp) DeleteFileW(wtmp);
    if (heap) HeapFree(GetProcessHeap(), 0, text);
    else free(text);
    free(tmp);
    free(wfinal);
    free(wtmp);
    me_seen_free(&seen);
    sh_modpin_result_free(&pr);
    sh_modmanifest_list_free(&rows);
    st->ms = me_now_ms() - t0;
    return ok;
}

int modexport_write(const char *map_json, size_t len, const char *zip_path, modexport_stats *stp, char **cause)
{
    me_payload pl;
    char *entry;
    int ok;
    memset(&pl, 0, sizeof pl);
    entry = me_map_entry(zip_path ? zip_path : "");
    if (!entry) return me_cause(cause, "out of memory");
    pl.entry = entry;
    pl.text = map_json;
    pl.len = len;
    ok = me_write(&pl, zip_path, stp, cause);
    free(entry);
    return ok;
}

int modexport_write_prefab(const char *name, const char *prefab, size_t len, const sh_modmanifest_list *rows,
                           modexport_prefab_fn text_for, void *ctx, const char *zip_path, modexport_stats *stp,
                           char **cause)
{
    me_payload pl;
    sh_modmanifest_list none;
    char *entry;
    size_t n;
    int ok;
    memset(&pl, 0, sizeof pl);
    memset(&none, 0, sizeof none);
    if (stp) memset(stp, 0, sizeof *stp);
    if (!name || !name[0]) return me_cause(cause, "the prefab has no name");
    n = strlen(name);
    if (n + sizeof ".json" - 1 > SH_ZIP_NAME_MAX)
        return me_cause(cause, "the prefab's name is %zu bytes, and a ZIP entry's name holds at most 65,535 bytes "
                        "(the ZIP format's limit); give it a shorter name", n + sizeof ".json" - 1);
    if (strchr(name, '/') || strchr(name, '\\') || !sh_zip_name_ok(name, n))
        return me_cause(cause, "the prefab's name cannot name a file in the ZIP");
    if (!(entry = sh_textf("%s.json", name))) return me_cause(cause, "out of memory");
    pl.entry = entry;
    pl.text = prefab;
    pl.len = len;
    pl.rows = rows ? rows : &none;
    pl.text_for = text_for;
    pl.ctx = ctx;
    ok = me_write(&pl, zip_path, stp, cause);
    free(entry);
    return ok;
}

enum { ME_KIND_MAP = 1, ME_KIND_PREFAB = 2 };

void modexport_set_stock(sh_mb_exists_fn stock, void *ctx) { (void)stock; (void)ctx; }

/* The entry is a top-level .json (no folder). */
static int me_top_json(const char *name)
{
    size_t n = strlen(name);
    return n > 5 && !strchr(name, '/') && !_stricmp(name + n - 5, ".json");
}

typedef struct me_walk_dir { wchar_t *abs; char *rel; } me_walk_dir;

/* Add every file under abs (the folder rel names) as an entry, at any depth (a heap stack, no recursion). Links and
 * junctions are not followed; a name no ZIP entry can hold is left out, as a ZIP's would be. */
static int me_walk(sh_zip_reader *r, const wchar_t *abs, const char *rel, char **cause)
{
    me_walk_dir *stack = NULL;
    size_t n = 0, cap = 0;
    int ok = 1;
    me_walk_dir top;
    top.abs = sh_wtextf(L"%s", abs);
    top.rel = sh_text(rel);
    if (!top.abs || !top.rel) { free(top.abs); free(top.rel); return me_cause(cause, "out of memory"); }
    stack = (me_walk_dir *)malloc(sizeof *stack);
    if (!stack) { free(top.abs); free(top.rel); return me_cause(cause, "out of memory"); }
    stack[n++] = top;
    cap = 1;
    while (ok && n) {
        me_walk_dir d = stack[--n];
        wchar_t *pattern = sh_wtextf(L"%s\\*", d.abs);
        WIN32_FIND_DATAW fd;
        HANDLE h = pattern ? FindFirstFileW(pattern, &fd) : INVALID_HANDLE_VALUE;
        free(pattern);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                char *leaf, *name;
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                if (!(leaf = sh_mb_utf8(fd.cFileName))) continue;
                name = sh_textf("%s/%s", d.rel, leaf);
                free(leaf);
                if (!name) { ok = me_cause(cause, "out of memory"); break; }
                if (!sh_zip_name_ok(name, strlen(name))) { free(name); continue; }
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    me_walk_dir child;
                    child.abs = sh_wtextf(L"%s\\%s", d.abs, fd.cFileName);
                    child.rel = name;
                    if (n == cap) {
                        size_t next = cap * 2;
                        me_walk_dir *grown = (me_walk_dir *)realloc(stack, next * sizeof *grown);
                        if (grown) { stack = grown; cap = next; }
                    }
                    if (!child.abs || n == cap) { free(child.abs); free(name); ok = me_cause(cause, "out of memory"); break; }
                    stack[n++] = child;
                } else {
                    int added = sh_zip_reader_add(r, name, ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow);
                    free(name);
                    if (!added) { ok = me_cause(cause, "out of memory while listing the export folder"); break; }
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        free(d.abs);
        free(d.rel);
    }
    while (n) { --n; free(stack[n].abs); free(stack[n].rel); }
    free(stack);
    return ok;
}

/* An export of an earlier shape (whole mod folders mods/<folder>/..., a bundle.canonical.json, or an export.json
 * index), refused with MODEXPORT_EARLIER. */
static int me_retired(const sh_zip_reader *r)
{
    static const char LEAF[] = "bundle.canonical.json";
    const size_t ll = sizeof LEAF - 1;
    size_t i;
    long idx;
    for (i = 0; i < r->n; i++) {
        const char *name = r->e[i].name;
        size_t n = strlen(name);
        if (!_strnicmp(name, "mods/", 5)) return 1;
        if (n >= ll && !_stricmp(name + n - ll, LEAF) && (n == ll || name[n - ll - 1] == '/')) return 1;
    }
    idx = sh_zip_reader_find(r, MODEXPORT_INDEX);
    if (idx >= 0) {
        char *ignored = NULL, type[64];
        size_t len = 0;
        unsigned char *text = sh_zip_reader_read(r, idx, (size_t)-1, &len, &ignored);
        int t = text ? sh_prefab_block_top_type((const char *)text, len, type, sizeof type) : -1;
        free(ignored);
        free(text);
        if (t != 1) return 1;
    }
    return 0;
}

/* What a carried .json is: a prefab (an idSnapEntityPrefab, or an idSnapMap with a block), a map (an idSnapMap or an
 * object without ~type, without a block), 0 not a JSON object, -1 an object of another type. */
static int me_classify(const char *text, size_t len)
{
    sh_prefab_block_span span;
    char type[64];
    int t = sh_prefab_block_top_type(text, len, type, sizeof type);
    if (t < 0) return 0;
    if (t == 1 && strcmp(type, "idSnapEntityPrefab") && strcmp(type, "idSnapMap")) return -1;
    if (t == 1 && !strcmp(type, "idSnapEntityPrefab")) return ME_KIND_PREFAB;
    return sh_prefab_block_find(text, len, &span) == 1 ? ME_KIND_PREFAB : ME_KIND_MAP;
}

static const char *me_kind_word(int kind) { return kind == ME_KIND_PREFAB ? "prefab" : "map"; }

/* Open an export for `want` and read its .json: a ZIP, or (a prefab only) the folder of a picked .json, whose units/
 * and files/ are walked into entries. The .json is checked before anything goes into the cache. */
static int me_open(sh_zip_reader *zr, const char *path, int want, char **text, size_t *len, long *idx, char **cause)
{
    size_t n = path ? strlen(path) : 0, i;
    int kind, dir = 0;
    memset(zr, 0, sizeof *zr);
    *text = NULL;
    *len = 0;
    *idx = -1;
    if (want == ME_KIND_PREFAB && n > 5 && !_stricmp(path + n - 5, ".json")) {
        wchar_t *w = sh_mb_wpath(path), *slash;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        char *leaf = NULL;
        int added;
        if (!w || !GetFileAttributesExW(w, GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            free(w);
            return me_cause(cause, "cannot open the prefab file");
        }
        slash = wcsrchr(w, L'\\');
        if (!slash) slash = wcsrchr(w, L'/');
        if (!slash || !(leaf = sh_mb_utf8(slash + 1)) || !sh_zip_name_ok(leaf, strlen(leaf))) {
            free(leaf);
            free(w);
            return me_cause(cause, "the prefab file's path is not usable");
        }
        *slash = 0;
        zr->dir = w;
        zr->h = NULL;
        added = sh_zip_reader_add(zr, leaf, ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow);
        free(leaf);
        if (!added) { sh_zip_reader_close(zr); return me_cause(cause, "out of memory"); }
        *idx = 0;
        dir = 1;
    } else {
        if (!sh_zip_reader_open(zr, path, cause)) { sh_zip_reader_close(zr); return 0; }
        if (me_retired(zr)) { sh_zip_reader_close(zr); return me_cause(cause, "%s", MODEXPORT_EARLIER); }
        for (i = 0; i < zr->n; i++) {
            if (!me_top_json(zr->e[i].name)) continue;
            if (*idx >= 0) { sh_zip_reader_close(zr); return me_cause(cause, "the export holds more than one %s", me_kind_word(want)); }
            *idx = (long)i;
        }
        if (*idx < 0) { sh_zip_reader_close(zr); return me_cause(cause, "the export holds no %s", me_kind_word(want)); }
    }
    *text = (char *)sh_zip_reader_read(zr, *idx, (size_t)-1, len, cause);
    if (!*text) { sh_zip_reader_close(zr); return 0; }
    kind = me_classify(*text, *len);
    if (kind != want) {
        const char *file = zr->e[*idx].name;
        if (kind == 0)
            me_cause(cause, want == ME_KIND_PREFAB ? "the export's prefab is not prefab JSON" : "the export's map is not rawmap JSON");
        else if (want == ME_KIND_MAP)
            me_cause(cause, kind == ME_KIND_PREFAB ? "this is a prefab's export, not a map's" : "the export's map is not rawmap JSON");
        else if (kind < 0) me_cause(cause, "%s is not a prefab", file);
        else if (dir) me_cause(cause, "%s is a map, not a prefab", file);
        else me_cause(cause, "this is a map's export, not a prefab's");
        free(*text);
        *text = NULL;
        sh_zip_reader_close(zr);
        return 0;
    }
    if (want == ME_KIND_MAP && sh_map_old_shape(*text, *len) != SH_MAP_OLD_NONE) {
        free(*text);
        *text = NULL;
        sh_zip_reader_close(zr);
        return me_cause(cause, "%s", MODEXPORT_EARLIER);
    }
    if (dir) {
        /* the extracted export's units/ and files/ (and mods/, to refuse an earlier shape) */
        static const char *const parts[] = { "units", "files", "mods" };
        int p, walked = 1;
        for (p = 0; p < 3 && walked; p++) {
            wchar_t *sub = sh_wtextf(L"%s\\%hs", zr->dir, parts[p]);
            walked = sub ? me_walk(zr, sub, parts[p], cause) : me_cause(cause, "out of memory");
            free(sub);
        }
        if (!walked || !sh_zip_reader_sort(zr, cause)) { free(*text); *text = NULL; sh_zip_reader_close(zr); return 0; }
        for (i = 0; i < zr->n; i++) if (me_top_json(zr->e[i].name)) { *idx = (long)i; break; }
        if (me_retired(zr)) { free(*text); *text = NULL; sh_zip_reader_close(zr); return me_cause(cause, "%s", MODEXPORT_EARLIER); }
    }
    return 1;
}

/* The export's units and files into the content cache (modcache.h), each checked against its SHA-256 name; anything
 * else is a damaged export, refused before the rest is read. A unit already cached is not written again. */
static int me_take(sh_zip_reader *zr, int kind, modexport_stats *st, char **cause)
{
    size_t i;
    /* The player turned off "Play maps whose mods include scripts": an export whose units carry scripts brings
     * nothing in, and the sentence says what to turn on and where (modui.h). */
    if (!sh_modui_scripted_maps_allowed())
        for (i = 0; i < zr->n; i++) {
            size_t blen = 0;
            unsigned char *bytes;
            mpj *m;
            const mpj *files;
            int k, scripts = 0;
            if (_strnicmp(zr->e[i].name, "units/", 6)) continue;
            if (!(bytes = sh_zip_reader_read(zr, (long)i, (size_t)-1, &blen, cause))) return 0;
            m = mpj_parse((const char *)bytes, blen);
            free(bytes);
            files = m ? mpj_get(m, "files") : NULL;
            for (k = 0; files && files->type == MPJ_ARR && k < files->n && !scripts; k++) {
                const mpj *path = mpj_get(files->items[k], "path");
                scripts = path && path->type == MPJ_STR && !_strnicmp(path->text, "scripts/", 8);
            }
            mpj_free(m);
            if (scripts) {
                me_log("MODS: import: %s carries scripts and the player turned scripts off", zr->e[i].name);
                return me_cause(cause, "%s", kind == ME_KIND_PREFAB ? SH_MODUI_SCRIPTS_OFF_PREFAB : SH_MODUI_SCRIPTS_OFF);
            }
        }
    for (i = 0; i < zr->n; i++) {
        const char *name = zr->e[i].name;
        size_t n = strlen(name), blen = 0;
        unsigned char *bytes;
        char actual[SH_MB_HASH_HEX + 1];
        int unit = !_strnicmp(name, "units/", 6), file = !_strnicmp(name, "files/", 6);
        if (!unit && !file) continue;
        if ((unit && !(n == 6 + SH_MB_HASH_HEX + 5 && me_hex64(name + 6, SH_MB_HASH_HEX) && !_stricmp(name + n - 5, ".json"))) ||
            (file && !me_hex64(name + 6, n - 6))) {
            return me_cause(cause, "the export is damaged: one of its mod files is not what it says; export it again");
        }
        if (unit) {
            char hash[SH_MB_HASH_HEX + 1];
            memcpy(hash, name + 6, SH_MB_HASH_HEX);
            hash[SH_MB_HASH_HEX] = 0;
            if (sh_cache_unit_state(hash, NULL, NULL, NULL) != -1) { st->bundles_present++; continue; }
        } else {
            char *have = sh_cache_blob_path(name + 6);
            if (have) { free(have); continue; }
        }
        bytes = sh_zip_reader_read(zr, (long)i, (size_t)-1, &blen, cause);
        if (!bytes) return 0;
        sh_mb_sha256_hex(bytes, blen, actual);
        if (unit ? _strnicmp(actual, name + 6, SH_MB_HASH_HEX) : _stricmp(actual, name + 6)) {
            free(bytes);
            return me_cause(cause, "the export is damaged: one of its mod files is not what it says; export it again");
        }
        if (unit) {
            char hash[SH_MB_HASH_HEX + 1];
            memcpy(hash, name + 6, SH_MB_HASH_HEX);
            hash[SH_MB_HASH_HEX] = 0;
            if (!sh_cache_put_manifest(hash, bytes, blen)) { free(bytes); return me_cause(cause, "the map's mod content could not be saved on this computer"); }
            st->bundles_installed++;
        } else {
            if (!sh_cache_import_blob(name + 6, bytes, blen)) {
                free(bytes);
                return me_cause(cause, "the map's mod content could not be saved on this computer");
            }
            st->files++;
        }
        free(bytes);
    }
    return 1;
}

/* The rows whose content is not all here (an export that does not carry it): the map or prefab stands their objects in. */
static void me_count_absent(const sh_modmanifest_list *rows, modexport_stats *st)
{
    size_t i, k;
    for (i = 0; i < rows->count; i++) {
        int whole = rows->rows[i].pinned;
        for (k = 0; whole && k < rows->rows[i].unit_count; k++)
            whole = sh_cache_unit_state(rows->rows[i].units[k], NULL, NULL, NULL) == 1;
        if (!whole) st->bundles_absent++;
    }
}

int modexport_probe(const char *zip_path, char **cause)
{
    sh_zip_reader zr;
    char *text = NULL;
    size_t len = 0;
    long idx;
    if (!me_open(&zr, zip_path, ME_KIND_MAP, &text, &len, &idx, cause)) return 0;
    free(text);
    sh_zip_reader_close(&zr);
    return 1;
}

char *modexport_read(const char *zip_path, size_t *out_len, modexport_stats *stp, char **cause)
{
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    sh_zip_reader zr;
    sh_modmanifest_list rows;
    char *text = NULL;
    size_t len = 0;
    unsigned rejected = 0;
    long idx;
    double t0 = me_now_ms();
    if (out_len) *out_len = 0;
    memset(st, 0, sizeof *st);
    memset(&rows, 0, sizeof rows);
    if (!me_open(&zr, zip_path, ME_KIND_MAP, &text, &len, &idx, cause)) { st->ms = me_now_ms() - t0; return NULL; }
    /* The map's units go into the content cache and its load composes them as a downloaded map's; nothing is installed. */
    if (!me_take(&zr, ME_KIND_MAP, st, cause)) { free(text); text = NULL; len = 0; goto done; }
    sh_modmanifest_read(text, len, &rows, &rejected);
    st->bundles = (unsigned)rows.count;
    me_count_absent(&rows, st);
    st->map_bytes = len;
done:
    sh_modmanifest_list_free(&rows);
    sh_zip_reader_close(&zr);
    st->ms = me_now_ms() - t0;
    if (out_len) *out_len = len;
    return text;
}

int modexport_read_prefab(const char *path, modexport_rows_fn rows_of, void *ctx, char **prefab, size_t *prefab_len,
                          char **name, modexport_stats *stp, char **cause)
{
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    sh_zip_reader zr;
    sh_modmanifest_list rows;
    char *text = NULL;
    size_t len = 0;
    long idx = -1;
    int ok = 0;
    double t0 = me_now_ms();
    memset(&rows, 0, sizeof rows);
    if (prefab) *prefab = NULL;
    if (prefab_len) *prefab_len = 0;
    if (name) *name = NULL;
    memset(st, 0, sizeof *st);
    if (!prefab || !path || !path[0]) return me_cause(cause, "nothing to read");
    if (!me_open(&zr, path, ME_KIND_PREFAB, &text, &len, &idx, cause)) { st->ms = me_now_ms() - t0; return 0; }
    if (name && !(*name = sh_textn(zr.e[idx].name, strlen(zr.e[idx].name) - 5))) { me_cause(cause, "out of memory"); goto done; }
    if (rows_of) {
        int n = rows_of(ctx, text, len, &rows);
        if (n == -2) { me_cause(cause, "%s", MODEXPORT_EARLIER); goto done; }
        if (n < 0) { me_cause(cause, "the prefab's mods cannot be read"); goto done; }
    }
    /* A prefab carries units, not installs: its units go into the content cache and placing it composes them. */
    if (!me_take(&zr, ME_KIND_PREFAB, st, cause)) goto done;
    st->bundles = (unsigned)rows.count;
    me_count_absent(&rows, st);
    st->map_bytes = len;
    ok = 1;
done:
    sh_zip_reader_close(&zr);
    sh_modmanifest_list_free(&rows);
    if (ok) {
        *prefab = text;
        if (prefab_len) *prefab_len = len;
    } else {
        free(text);
        if (name) { free(*name); *name = NULL; }
    }
    st->ms = me_now_ms() - t0;
    return ok;
}

unsigned long long modexport_write_text(const char *data, size_t len, const char *path)
{
    return modexport_write_text_report(data, len, path, NULL, NULL);
}

unsigned long long modexport_write_text_report(const char *data, size_t len, const char *path,
                                               unsigned *bundles, char **cause_out)
{
    modexport_stats st;
    char *cause = NULL;
    if (bundles) *bundles = 0;
    if (!modexport_write(data, len, path, &st, &cause)) {
        me_log("MODS: export %s: refused: %s", path, cause ? cause : "out of memory");
        if (bundles) *bundles = st.bundles;
        if (cause_out) { free(*cause_out); *cause_out = cause; }
        else free(cause);
        return 0;
    }
    free(cause);
    if (bundles) *bundles = st.bundles;
    me_log("MODS: export %s: map %llu byte(s), %u row(s), %u unit(s) and %u file(s) carried, %u entr%s, %llu byte(s) "
           "in %.0f ms", path, st.map_bytes, st.bundles, st.bundles_installed, st.files, st.entries,
           st.entries == 1 ? "y" : "ies", st.zip_bytes, st.ms);
    return st.zip_bytes;
}

char *modexport_read_text_report(const char *path, size_t *out_len, char **cause_out)
{
    modexport_stats st;
    char *cause = NULL;
    char *text, *heap;
    size_t len = 0;
    if (out_len) *out_len = 0;
    text = modexport_read(path, &len, &st, &cause);
    if (!text) {
        me_log("MODS: import %s: refused: %s", path, cause ? cause : "out of memory");
        if (cause_out) { free(*cause_out); *cause_out = cause; }
        else free(cause);
        return NULL;
    }
    free(cause);
    me_log("MODS: import %s: %u row(s); %u unit(s) and %u file(s) into the content cache, %u unit(s) already there; "
           "map %llu byte(s) in %.0f ms%s", path, st.bundles, st.bundles_installed, st.files, st.bundles_present,
           st.map_bytes, st.ms,
           st.bundles_absent ? "; the export does not carry all of the map's content: its objects stand in" : "");
    if (!len || len > (size_t)INT_MAX) { free(text); return NULL; }
    heap = (char *)HeapAlloc(GetProcessHeap(), 0, len + 1);
    if (heap) { memcpy(heap, text, len); heap[len] = 0; if (out_len) *out_len = len; }
    free(text);
    return heap;
}

int modexport_validate_source(const char *path, char **out_msg)
{
    char *cause = NULL;
    int ok = modexport_probe(path, &cause);
    if (out_msg) {
        free(*out_msg);
        *out_msg = ok ? sh_text("ok") : cause ? cause : sh_text("the export cannot be read");
        if (ok || !cause) free(cause);
    } else free(cause);
    return ok;
}
