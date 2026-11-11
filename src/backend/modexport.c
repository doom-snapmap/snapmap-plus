/* modexport.c -- see modexport.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <limits.h>

#include "modexport.h"
#include "modstore.h"
#include "modbundle.h"
#include "modmanifest.h"
#include "modserve.h"
#include "mp_json.h"
#include "prefab_block.h"
#include "backend_log.h"
#include "plain_reason.h"
#include "modzip.h"
#include "map_carrier.h"
#include "modsync.h"

#define ME_INDEX_MAX       (1024u * 1024u)

static int me_cause(char *cause, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (cause && cap) {
        va_start(ap, fmt);
        _vsnprintf_s(cause, cap, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    return 0;
}

static void me_log(const char *fmt, ...)
{
    char line[MODEXPORT_CAUSE_MAX + MAX_PATH * 4];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
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

/* ------------------------------------------------------------ helpers --- */

#define ME_MODJSON_MAX   (16u * 1024u * 1024u)
#define ME_MAX_MODS      256u

static void me_join_native(char *out, size_t cap, const char *dir, const char *rel)
{
    size_t i, n;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\%s", dir, rel);
    n = strlen(out);
    for (i = strlen(dir); i < n; i++) if (out[i] == '/') out[i] = '\\';
}

/* The entry a map's export names its map: the ZIP's own base name with .json
 * ("my map.zip" carries "my map.json"). */
static void me_map_entry(const char *zip_path, char *out, size_t cap)
{
    const char *base = zip_path, *p;
    size_t n;
    for (p = zip_path; *p; p++) if (*p == '\\' || *p == '/') base = p + 1;
    n = strlen(base);
    if (n > 4 && !_stricmp(base + n - 4, ".zip")) n -= 4;
    if (!n || n > 200) { strcpy_s(out, cap, MODEXPORT_MAP_FALLBACK); return; }
    _snprintf_s(out, cap, _TRUNCATE, "%.*s.json", (int)n, base);
    if (!sh_zip_name_ok(out, strlen(out))) strcpy_s(out, cap, MODEXPORT_MAP_FALLBACK);
}

/* ------------------------------------------------------------ write ----- */

static int mm_list_has(const sh_modmanifest_list *list, const sh_modmanifest_row *row)
{
    size_t i;
    for (i = 0; i < list->count; i++)
        if (!strcmp(list->rows[i].salt, row->salt) && !strcmp(list->rows[i].build, row->build)) return 1;
    return 0;
}

/* What one export carries besides its mods: a map (whose rows are read from
 * its text and rewritten when a row pins an earlier build) or a prefab (whose
 * rows are given, and whose text is asked for once the carried rows are
 * known). */
typedef struct me_payload {
    const char *entry;                 /* "<name>.json" */
    const char *text;
    size_t len;
    const char *place;                 /* where its rows resolve (modserve.h): "" the root */
    const sh_modmanifest_list *rows;   /* prefab: the rows; map: NULL (read from text) */
    modexport_prefab_fn text_for;      /* prefab: the text to carry for the carried rows */
    void *ctx;
} me_payload;

static int me_write(const me_payload *pl, const char *zip_path, modexport_stats *stp, char *cause, size_t cap);

int modexport_write(const char *map_json, size_t len, const char *zip_path, modexport_stats *stp,
                    char *cause, size_t cap)
{
    me_payload pl;
    char entry[SH_ZIP_NAME_MAX];
    char segment[SH_CARRIER_PLACE_CAP], place[SH_MODSERVE_PLACE_CAP] = "";
    memset(&pl, 0, sizeof pl);
    me_map_entry(zip_path ? zip_path : "", entry, sizeof entry);
    pl.entry = entry;
    pl.text = map_json;
    pl.len = len;
    /* The map's rows resolve in the place it records (none: the root). */
    if (map_json && sh_carrier_place(map_json, len, segment, sizeof segment) == 1 &&
        !sh_modserve_place_norm(segment, place, sizeof place))
        place[0] = 0;
    pl.place = place;
    return me_write(&pl, zip_path, stp, cause, cap);
}

int modexport_write_prefab(const char *name, const char *prefab, size_t len, const sh_modmanifest_list *rows,
                           modexport_prefab_fn text_for, void *ctx, const char *zip_path, modexport_stats *stp,
                           char *cause, size_t cap)
{
    me_payload pl;
    sh_modmanifest_list none;
    char entry[SH_ZIP_NAME_MAX];
    size_t n;
    memset(&pl, 0, sizeof pl);
    memset(&none, 0, sizeof none);
    if (stp) memset(stp, 0, sizeof *stp);
    if (!name || !name[0]) return me_cause(cause, cap, "the prefab has no name");
    n = strlen(name);
    if (n > 200 || strchr(name, '/') || strchr(name, '\\') || !sh_zip_name_ok(name, n))
        return me_cause(cause, cap, "the prefab's name cannot name a file in the ZIP");
    if (len > MODEXPORT_PREFAB_MAX) return me_cause(cause, cap, "the prefab is larger than %u bytes", MODEXPORT_PREFAB_MAX);
    _snprintf_s(entry, sizeof entry, _TRUNCATE, "%s.json", name);
    pl.entry = entry;
    pl.text = prefab;
    pl.len = len;
    pl.rows = rows ? rows : &none;
    pl.text_for = text_for;
    pl.ctx = ctx;
    return me_write(&pl, zip_path, stp, cause, cap);
}

/* One build as the folder it is installed as: mods/<folder>/mod.json as it is
 * on disk (its build hash checked), then every file its block carries under
 * assets/, each checked against the hash mod.json records. `folder` is the
 * name the ZIP carries it under (the store folder's own name, its last
 * segment). A stock pointer is an entry of mod.json and has no file here:
 * installed game content never leaves the machine. */
static int me_add_build(sh_zip_writer *w, const char *folder, const sh_ms_entry *e, modexport_stats *st, char *cause,
                        size_t cap)
{
    const sh_mb_bundle *b = &e->bundle;
    const char *label = b->name && b->name[0] ? b->name : e->name;
    char name[SH_ZIP_NAME_MAX + 1], path[MAX_PATH * 4], actual[SH_MB_HASH_HEX + 1];
    unsigned char *bytes;
    size_t blen = 0, k;
    sh_mb_bundle check;
    int same;
    /* Read without the store lock: every file is checked against the build
     * the listing named, so a folder changed meanwhile refuses the export. */
    me_join_native(path, sizeof path, e->folder, "mod.json");
    bytes = sh_mb_read_file(path, ME_MODJSON_MAX, &blen);
    if (!bytes) return me_cause(cause, cap, "%s's mod.json cannot be read. Press Refresh in the Mods tab to see why", label);
    memset(&check, 0, sizeof check);
    same = sh_mb_load_text((const char *)bytes, blen, &check) && !strcmp(check.hash, b->hash);
    sh_mb_free(&check);
    if (!same) {
        free(bytes);
        return me_cause(cause, cap, "%s changed since Snapmap+ last read it. Press Refresh in the Mods tab, then save again",
                        label);
    }
    _snprintf_s(name, sizeof name, _TRUNCATE, "mods/%s/mod.json", folder);
    sh_zip_writer_add(w, name, bytes, blen);
    free(bytes);
    for (k = 0; k < b->file_count && !w->failed; k++) {
        const sh_mb_file *f = &b->files[k];
        if (f->stock) continue;
        me_join_native(path, sizeof path, e->folder, f->path);
        bytes = sh_mb_read_file(path, (size_t)SH_MB_MAX_FILE, &blen);
        if (bytes) {
            sh_mb_sha256_hex(bytes, blen, actual);
            if (blen != f->size || strcmp(actual, f->blob) != 0) { free(bytes); bytes = NULL; }
        }
        if (!bytes)
            return me_cause(cause, cap, "%s's file %s is missing or changed. Press Refresh in the Mods tab, then save again",
                            label, f->path);
        _snprintf_s(name, sizeof name, _TRUNCATE, "mods/%s/%s", folder, f->path);
        sh_zip_writer_add(w, name, bytes, blen);
        free(bytes);
        st->files++;
    }
    if (w->failed) return me_cause(cause, cap, "%s", w->cause);
    return 1;
}

/* The folder that carries a row, as the one place resolver (modserve.h)
 * names it for a map whose place is `place`: the root folder of its salt at
 * the row's build; then, with a place, that place's folder at the row's
 * build, and without one (the player's own map or prefab) the root folder of
 * its salt at whatever build it holds, to which the row moves. 1 exact, 2
 * moved, 0 none. */
static int me_pick_row(const sh_ms_list *l, const sh_modmanifest_row *row, const char *place, const sh_ms_entry **out)
{
    const sh_ms_entry *e;
    *out = NULL;
    e = sh_ms_find_salt(l, row->salt, NULL, row->build);
    if (e && sh_ms_entry_usable(e)) { *out = e; return 1; }
    if (place && place[0]) {
        e = sh_ms_find_salt(l, row->salt, place, row->build);
        if (e && sh_ms_entry_usable(e)) { *out = e; return 1; }
        return 0;
    }
    e = sh_ms_find_salt(l, row->salt, NULL, NULL);
    if (e && sh_ms_entry_usable(e)) { *out = e; return 2; }
    return 0;
}

/* The ZIP's folder name for a carried mod: the last segment of its folder
 * below mods\ (a mod in an organizing folder, or in a map's place, is
 * carried as mods/<its own folder name>/), " (2)", " (3)" ... when another
 * carried mod has the name. */
static void me_carry_name(const sh_ms_entry *e, char (*taken)[SH_MS_NAME_MAX + 1], size_t ntaken, char *out, size_t cap)
{
    const char *leaf = e->name, *c;
    size_t i;
    int n;
    for (c = e->name; *c; c++) if (*c == '\\' || *c == '/') leaf = c + 1;
    for (n = 1; n < 1000; n++) {
        int clash = 0;
        if (n == 1) _snprintf_s(out, cap, _TRUNCATE, "%s", leaf);
        else _snprintf_s(out, cap, _TRUNCATE, "%s (%d)", leaf, n);
        for (i = 0; i < ntaken && !clash; i++) clash = !_stricmp(taken[i], out);
        if (!clash) return;
    }
}

static int me_write(const me_payload *pl, const char *zip_path, modexport_stats *stp, char *cause, size_t cap)
{
    const char *map_json = pl->text;
    size_t len = pl->len;
    const int prefab = pl->rows != NULL;
    char *prefab_text = NULL;
    size_t moved = 0;
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    sh_modmanifest_list rows;
    sh_ms_list l;
    const sh_ms_entry **picked = NULL;
    char (*names)[SH_MS_NAME_MAX + 1] = NULL;
    size_t nnames = 0;
    sh_zip_writer w;
    wchar_t *wfinal = NULL, *wtmp = NULL;
    char tmp[MAX_PATH * 4];
    unsigned rejected = 0;
    size_t i, k;
    double t0 = me_now_ms();
    int ok = 0, listed = 0, locked = 0;
    char *moved_map = NULL;

    memset(st, 0, sizeof *st);
    memset(&rows, 0, sizeof rows);
    memset(&w, 0, sizeof w);
    w.h = INVALID_HANDLE_VALUE;
    if (cause && cap) cause[0] = 0;
    if (!map_json || !len || len > MODEXPORT_MAP_MAX || !zip_path || !zip_path[0])
        return me_cause(cause, cap, prefab ? "no prefab text or no destination" : "no map text or no destination");
    if (prefab) {
        for (i = 0; i < pl->rows->count; i++)
            if (!mm_list_has(&rows, &pl->rows->rows[i]) && !sh_modmanifest_list_add(&rows, &pl->rows->rows[i])) {
                sh_modmanifest_list_free(&rows);
                return me_cause(cause, cap, "out of memory");
            }
    } else sh_modmanifest_read(map_json, len, &rows, &rejected);
    st->map_bytes = len;

    modstore_lock();
    locked = 1;
    listed = sh_ms_enumerate(&l);
    if (!listed) { me_cause(cause, cap, "the mods folder cannot be read. Check that it exists, then press Refresh in the Mods tab"); goto done; }
    picked = (const sh_ms_entry **)calloc(rows.count ? rows.count : 1, sizeof *picked);
    if (!picked) { me_cause(cause, cap, "out of memory"); goto done; }
    /* Each row at its build, else the root folder of its salt. */
    for (i = 0; i < rows.count; i++) {
        int how = me_pick_row(&l, &rows.rows[i], pl->place, &picked[i]);
        if (!how) {
            me_cause(cause, cap, "this %s uses %s, which is not installed or does not build. Press Refresh in the Mods "
                     "tab, or remove it from the %s", prefab ? "prefab" : "map",
                     rows.rows[i].name[0] ? rows.rows[i].name : "a mod", prefab ? "prefab" : "map");
            goto done;
        }
        if (how == 2) moved++;
    }
    if (moved) {
        /* The rows name the builds the export carries, so the importer gets
         * exactly what the ZIP holds. Rows of one salt merge. */
        sh_modmanifest_list fixed;
        size_t map_len = 0;
        char err[160] = "";
        memset(&fixed, 0, sizeof fixed);
        for (i = 0; i < rows.count; i++) {
            const sh_mb_bundle *b = &picked[i]->bundle;
            sh_modmanifest_row *r = NULL;
            for (k = 0; k < fixed.count && !r; k++) if (!strcmp(fixed.rows[k].build, b->hash)) r = &fixed.rows[k];
            if (r) {
                for (k = 0; k < rows.rows[i].name_count; k++)
                    if (!sh_modmanifest_row_add_name(r, rows.rows[i].names[k])) { r = NULL; break; }
                if (!r) { sh_modmanifest_list_free(&fixed); me_cause(cause, cap, "out of memory"); goto done; }
                continue;
            }
            if (!sh_modmanifest_list_add(&fixed, &rows.rows[i])) {
                sh_modmanifest_list_free(&fixed);
                me_cause(cause, cap, "out of memory");
                goto done;
            }
            r = &fixed.rows[fixed.count - 1];
            strcpy_s(r->build, sizeof r->build, b->hash);
            if (b->name) strncpy_s(r->name, sizeof r->name, b->name, _TRUNCATE);
            r->size = b->total_size;
        }
        me_log("MODS: export: %zu row(s) named another build; the export carries and names the root folder's build",
               moved);
        if (!prefab) {
            moved_map = sh_modmanifest_write(map_json, len, &fixed, &map_len, err, sizeof err);
            if (!moved_map) {
                sh_modmanifest_list_free(&fixed);
                me_cause(cause, cap, "the map's list of mods could not be updated (%s). Save the map again", err[0] ? err : "out of memory");
                goto done;
            }
            map_json = moved_map;
            len = map_len;
            st->map_bytes = len;
        }
        sh_modmanifest_list_free(&rows);
        rows = fixed;
        free((void *)picked);
        picked = (const sh_ms_entry **)calloc(rows.count ? rows.count : 1, sizeof *picked);
        if (!picked) { me_cause(cause, cap, "out of memory"); goto done; }
        for (i = 0; i < rows.count; i++)
            if (!me_pick_row(&l, &rows.rows[i], pl->place, &picked[i])) {
                me_cause(cause, cap, "this %s uses %s, which is not installed or does not build. Press Refresh in the "
                         "Mods tab, or remove it from the %s", prefab ? "prefab" : "map",
                         rows.rows[i].name[0] ? rows.rows[i].name : "a mod", prefab ? "prefab" : "map");
                goto done;
            }
    }
    st->bundles = (unsigned)rows.count;
    /* The builds are chosen: the files are read without the store lock. */
    modstore_unlock();
    locked = 0;
    if (prefab && pl->text_for) {
        prefab_text = pl->text_for(pl->ctx, &rows, moved > 0, &len);
        if (!prefab_text || !len || len > MODEXPORT_PREFAB_MAX) { me_cause(cause, cap, "the prefab's export could not be written. Export it again"); goto done; }
        map_json = prefab_text;
        st->map_bytes = len;
    }

    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s.smp-%lu.tmp", zip_path, GetCurrentProcessId());
    wfinal = sh_mb_wpath(zip_path);
    wtmp = sh_mb_wpath(tmp);
    names = calloc(rows.count ? rows.count : 1, sizeof *names);
    if (!wfinal || !wtmp || !names) { me_cause(cause, cap, "the destination path is not usable"); goto done; }
    if (!sh_zip_writer_open(&w, wtmp)) { me_cause(cause, cap, "cannot create %s: %s", zip_path, sh_reason_win32(GetLastError(), zip_path, 0)); goto done; }

    sh_zip_writer_add(&w, pl->entry, map_json, len);
    for (i = 0; i < rows.count && !w.failed; i++) {
        int dup = 0;
        for (k = 0; k < i && !dup; k++) dup = picked[k] == picked[i];
        if (dup) continue;
        me_carry_name(picked[i], names, nnames, names[nnames], sizeof names[nnames]);
        if (!me_add_build(&w, names[nnames], picked[i], st, cause, cap)) goto done;
        nnames++;
    }
    sh_zip_writer_finish(&w);
    if (w.failed) { me_cause(cause, cap, "%s", w.cause); goto done; }
    if (!FlushFileBuffers(w.h)) { me_cause(cause, cap, "writing the ZIP failed: %s", sh_reason_win32(GetLastError(), zip_path, 0)); goto done; }
    CloseHandle(w.h);
    w.h = INVALID_HANDLE_VALUE;
    if (!MoveFileExW(wtmp, wfinal, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        me_cause(cause, cap, "cannot replace %s: %s", zip_path, sh_reason_win32(GetLastError(), zip_path, 0));
        goto done;
    }
    st->entries = (unsigned)w.n;
    st->zip_bytes = w.off;
    ok = 1;
done:
    sh_zip_writer_free(&w);
    if (!ok && wtmp) DeleteFileW(wtmp);
    free((void *)picked);
    free(names);
    free(moved_map);
    free(prefab_text);
    free(wfinal);
    free(wtmp);
    if (listed) sh_ms_list_free(&l);
    if (locked) modstore_unlock();
    sh_modmanifest_list_free(&rows);
    st->ms = me_now_ms() - t0;
    return ok;
}

/* ------------------------------------------------------------ read ------ */

enum { ME_KIND_MAP = 1, ME_KIND_PREFAB = 2 };

static sh_mb_exists_fn g_stock_fn;
static void *g_stock_ctx;

void modexport_set_stock(sh_mb_exists_fn stock, void *ctx)
{
    g_stock_ctx = ctx;
    g_stock_fn = stock;
}

/* One mod folder an export carries that a row names. */
typedef struct me_carry {
    char name[SH_MS_NAME_MAX + 1];      /* the folder's name under mods/ */
    sh_mb_bundle b;                     /* its mod.json, loaded: the salt (b.id) and build hash it records */
    int present;                        /* something already serves its row: not installed */
    int installed;                      /* this import installed it, as `stored` */
    char stored[SH_MS_NAME_MAX + 1];    /* where: its folder below mods\ */
    char stage[MAX_PATH * 4];           /* its copy outside the store, which the install reads */
    char why[SH_MB_CAUSE_MAX];          /* the last install refusal */
} me_carry;

typedef struct me_import {
    sh_zip_reader zr;
    me_carry *c;
    size_t nc, cap;
    int kind;                           /* what the export holds */
    /* A map's place, mods\maps\<title>\ (modstore.h): named after its ZIP
     * and keyed "zip:<the ZIP's path>", so two ZIPs with one name get two
     * places. A prefab has none: its mods install at the root (DECISIONS
     * 12). */
    char place_title[MAX_PATH];
    char place_key[MAX_PATH * 4 + 48];
} me_import;

static void me_import_free(me_import *im)
{
    size_t i;
    for (i = 0; i < im->nc; i++) sh_mb_free(&im->c[i].b);
    free(im->c);
    sh_zip_reader_close(&im->zr);
    memset(im, 0, sizeof *im);
}

/* The entry is a top-level .json (no folder). */
static int me_top_json(const char *name)
{
    size_t n = strlen(name);
    return n > 5 && !strchr(name, '/') && !_stricmp(name + n - 5, ".json");
}

static int me_w_to_utf8(const wchar_t *w, char *out, size_t cap)
{
    return WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL) > 0;
}

/* Add every file under abs (the folder rel names) as an entry. Links and
 * junctions are not followed. */
static int me_walk(sh_zip_reader *r, const wchar_t *abs, const char *rel, int depth, char *cause, size_t cap)
{
    wchar_t pattern[MAX_PATH * 2];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (depth > 32) return me_cause(cause, cap, "%s is nested too deeply", rel);
    _snwprintf_s(pattern, MAX_PATH * 2, _TRUNCATE, L"%s\\*", abs);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 1;
    do {
        wchar_t child[MAX_PATH * 2];
        char leaf[MAX_PATH * 3], name[SH_ZIP_NAME_MAX + 1];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (!me_w_to_utf8(fd.cFileName, leaf, sizeof leaf)) continue;
        _snprintf_s(name, sizeof name, _TRUNCATE, "%s/%s", rel, leaf);
        if (!sh_zip_name_ok(name, strlen(name))) continue;
        _snwprintf_s(child, MAX_PATH * 2, _TRUNCATE, L"%s\\%s", abs, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!me_walk(r, child, name, depth + 1, cause, cap)) { FindClose(h); return 0; }
        } else if (!sh_zip_reader_add(r, name, ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow)) {
            FindClose(h);
            return me_cause(cause, cap, "the export folder holds too many files");
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
}

/* The bundle form of earlier releases: a bundle.canonical.json anywhere, or
 * an export.json that is an index rather than a prefab or map named
 * "export". Refused with MODEXPORT_EARLIER. */
static int me_retired(const sh_zip_reader *r)
{
    static const char LEAF[] = "bundle.canonical.json";
    const size_t ll = sizeof LEAF - 1;
    size_t i;
    long idx;
    for (i = 0; i < r->n; i++) {
        const char *name = r->e[i].name;
        size_t n = strlen(name);
        if (n >= ll && !_stricmp(name + n - ll, LEAF) && (n == ll || name[n - ll - 1] == '/')) return 1;
    }
    idx = sh_zip_reader_find(r, MODEXPORT_INDEX);
    if (idx >= 0) {
        char ignored[64], type[64];
        size_t len = 0;
        unsigned char *text = sh_zip_reader_read(r, idx, ME_INDEX_MAX, &len, ignored, sizeof ignored);
        int t = text ? sh_prefab_block_top_type((const char *)text, len, type, sizeof type) : -1;
        free(text);
        if (t != 1) return 1;
    }
    return 0;
}

/* What a carried .json is: a prefab (an idSnapEntityPrefab, or an idSnapMap
 * with a block), a map (an idSnapMap or an object without ~type, without a
 * block), 0 not a JSON object, -1 an object of another type. */
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

static void me_open_fail(me_import *im, char **text)
{
    free(*text);
    *text = NULL;
    me_import_free(im);
}

/* Open an export for `want` and read its .json: a ZIP, or (a prefab only)
 * the folder of a picked .json, whose mods/ folder is walked into entries as
 * the ZIP's would be. *text (malloc'd) and *idx are the carried .json. The
 * .json is read and checked before anything is installed: a damaged or
 * mistaken export installs nothing. */
static int me_open(me_import *im, const char *path, int want, char **text, size_t *len, long *idx, char *cause, size_t cap)
{
    size_t n = path ? strlen(path) : 0, i;
    int kind, dir = 0;
    memset(im, 0, sizeof *im);
    *text = NULL;
    *len = 0;
    *idx = -1;
    if (path && want == ME_KIND_MAP) {
        /* A map's mods go to the ZIP's place: mods\maps\<its name>\. */
        const char *leaf = path, *c;
        size_t ln;
        for (c = path; *c; c++) if (*c == '\\' || *c == '/') leaf = c + 1;
        ln = strlen(leaf);
        if (ln > 4 && !_stricmp(leaf + ln - 4, ".zip")) ln -= 4;
        if (ln > 5 && !_stricmp(leaf + ln - 5, ".json")) ln -= 5;
        if (ln >= sizeof im->place_title) ln = sizeof im->place_title - 1;
        memcpy(im->place_title, leaf, ln);
        im->place_title[ln] = 0;
        _snprintf_s(im->place_key, sizeof im->place_key, _TRUNCATE, "zip:%s", path);
    }
    if (want == ME_KIND_PREFAB && n > 5 && !_stricmp(path + n - 5, ".json")) {
        wchar_t *w = sh_mb_wpath(path), *slash;
        WIN32_FILE_ATTRIBUTE_DATA fa;
        char leaf[MAX_PATH * 3];
        if (!w || !GetFileAttributesExW(w, GetFileExInfoStandard, &fa) || (fa.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            free(w);
            return me_cause(cause, cap, "cannot open the prefab file");
        }
        slash = wcsrchr(w, L'\\');
        if (!slash) slash = wcsrchr(w, L'/');
        if (!slash || !me_w_to_utf8(slash + 1, leaf, sizeof leaf) || !sh_zip_name_ok(leaf, strlen(leaf))) {
            free(w);
            return me_cause(cause, cap, "the prefab file's path is not usable");
        }
        *slash = 0;
        im->zr.dir = w;
        im->zr.h = NULL;
        if (!sh_zip_reader_add(&im->zr, leaf, ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow)) {
            me_import_free(im);
            return me_cause(cause, cap, "out of memory");
        }
        *idx = 0;
        dir = 1;
    } else {
        if (!sh_zip_reader_open(&im->zr, path, cause, cap)) return 0;
        if (me_retired(&im->zr)) {
            me_import_free(im);
            return me_cause(cause, cap, "%s", MODEXPORT_EARLIER);
        }
        for (i = 0; i < im->zr.n; i++) {
            if (!me_top_json(im->zr.e[i].name)) continue;
            if (*idx >= 0) {
                me_import_free(im);
                return me_cause(cause, cap, "the export holds more than one %s", me_kind_word(want));
            }
            *idx = (long)i;
        }
        if (*idx < 0) { me_import_free(im); return me_cause(cause, cap, "the export holds no %s", me_kind_word(want)); }
    }
    *text = (char *)sh_zip_reader_read(&im->zr, *idx, MODEXPORT_MAP_MAX, len, cause, cap);
    if (!*text) { me_import_free(im); return 0; }
    kind = me_classify(*text, *len);
    if (kind != want) {
        const char *file = im->zr.e[*idx].name;
        if (kind == 0)
            me_cause(cause, cap, want == ME_KIND_PREFAB ? "the export's prefab is not prefab JSON" : "the export's map is not rawmap JSON");
        else if (want == ME_KIND_MAP)
            me_cause(cause, cap, kind == ME_KIND_PREFAB ? "this is a prefab's export, not a map's" : "the export's map is not rawmap JSON");
        else if (kind < 0) me_cause(cause, cap, "%s is not a prefab", file);
        else if (dir) me_cause(cause, cap, "%s is a map, not a prefab", file);
        else me_cause(cause, cap, "this is a map's export, not a prefab's");
        me_open_fail(im, text);
        return 0;
    }
    if (want == ME_KIND_PREFAB && *len > MODEXPORT_PREFAB_MAX) {
        me_open_fail(im, text);
        return me_cause(cause, cap, "the prefab is larger than %u bytes", MODEXPORT_PREFAB_MAX);
    }
    /* A map in an earlier shape (its variables or rows): nothing is read. */
    if (want == ME_KIND_MAP && sh_map_old_shape(*text, *len) != SH_MAP_OLD_NONE) {
        me_open_fail(im, text);
        return me_cause(cause, cap, "%s", MODEXPORT_EARLIER);
    }
    im->kind = want;
    if (dir) {
        /* The extracted export's mods/ folder. Sorting moves the entries, so
         * the picked .json (the only top-level one) is found again. */
        wchar_t mods[MAX_PATH * 2];
        _snwprintf_s(mods, MAX_PATH * 2, _TRUNCATE, L"%s\\mods", im->zr.dir);
        if (!me_walk(&im->zr, mods, "mods", 0, cause, cap) || !sh_zip_reader_sort(&im->zr, cause, cap)) {
            me_open_fail(im, text);
            return 0;
        }
        for (i = 0; i < im->zr.n; i++) if (me_top_json(im->zr.e[i].name)) { *idx = (long)i; break; }
        if (me_retired(&im->zr)) {
            me_open_fail(im, text);
            return me_cause(cause, cap, "%s", MODEXPORT_EARLIER);
        }
    }
    return 1;
}

static int me_rows_name(const sh_modmanifest_list *rows, const char *salt, const char *hash)
{
    size_t i;
    for (i = 0; rows && i < rows->count; i++)
        if (!_stricmp(rows->rows[i].salt, salt) && !_stricmp(rows->rows[i].build, hash)) return 1;
    return 0;
}

static int me_carried(const me_import *im, const char *salt, const char *hash)
{
    size_t i;
    for (i = 0; i < im->nc; i++) if (!_stricmp(im->c[i].b.id, salt) && !_stricmp(im->c[i].b.hash, hash)) return 1;
    return 0;
}

/* Whether a folder's path below mods/ lies inside another folder the export
 * carries (an ancestor with its own mod.json), or deeper than the store
 * enters organizing folders. */
static int me_inside_carried(const sh_zip_reader *r, const char *seg, size_t sl)
{
    char probe[SH_ZIP_NAME_MAX + 1];
    size_t i, depth = 0;
    for (i = 0; i < sl; i++) {
        if (seg[i] != '/') continue;
        if (++depth >= SH_MS_NEST_MAX) return 1;
        _snprintf_s(probe, sizeof probe, _TRUNCATE, "mods/%.*s/mod.json", (int)i, seg);
        if (sh_zip_reader_find(r, probe) >= 0) return 1;
    }
    return 0;
}

/* The carried folders, each mods/<name>/mod.json, whose build a row names
 * (every one when `all`). A folder no row names is left out. A carried
 * mod.json in an earlier shape refuses the whole export (MODEXPORT_EARLIER):
 * nothing of it is read. */
static int me_collect(me_import *im, const sh_modmanifest_list *rows, int all, char *cause, size_t cap)
{
    size_t i;
    for (i = 0; i < im->zr.n; i++) {
        const char *name = im->zr.e[i].name, *seg, *slash;
        size_t sl, len = 0;
        char *text;
        me_carry *c;
        sh_mb_bundle b;
        if (strncmp(name, "mods/", 5)) continue;
        seg = name + 5;
        /* mods/<folder>/mod.json, where <folder> may sit in organizing
         * folders (mods/<group>/<folder>/mod.json, the store's nesting rule):
         * a mod.json inside a carried folder is that folder's content. */
        slash = strrchr(seg, '/');
        if (!slash || strcmp(slash, "/mod.json")) continue;
        sl = (size_t)(slash - seg);
        if (!sl || sl > SH_MS_NAME_MAX || me_inside_carried(&im->zr, seg, sl)) continue;
        text = (char *)sh_zip_reader_read(&im->zr, (long)i, ME_MODJSON_MAX, &len, cause, cap);
        if (!text) return 0;
        memset(&b, 0, sizeof b);
        if (!sh_mb_load_text(text, len, &b)) {
            free(text);
            if (!strncmp(b.first_cause, SH_MB_SHAPE, sizeof SH_MB_SHAPE - 1)) {
                me_log("MODS: import: %s: %s", name, b.first_cause);
                sh_mb_free(&b);
                return me_cause(cause, cap, "%s", MODEXPORT_EARLIER);
            }
            me_log("MODS: import: %s is not a built mod (%s); left out", name, b.first_cause);
            sh_mb_free(&b);
            continue;
        }
        free(text);
        if ((!all && !me_rows_name(rows, b.id, b.hash)) || me_carried(im, b.id, b.hash)) {
            me_log("MODS: import: mods/%.*s (%s@%.16s) is carried but the %s does not name it; left out",
                   (int)sl, seg, b.id, b.hash, me_kind_word(im->kind));
            sh_mb_free(&b);
            continue;
        }
        if (im->nc >= ME_MAX_MODS) { sh_mb_free(&b); return me_cause(cause, cap, "the export carries too many mods"); }
        if (im->nc == im->cap) {
            size_t c2 = im->cap ? im->cap * 2 : 8;
            me_carry *g = (me_carry *)realloc(im->c, sizeof *g * c2);
            if (!g) { sh_mb_free(&b); return me_cause(cause, cap, "out of memory"); }
            im->c = g;
            im->cap = c2;
        }
        c = &im->c[im->nc++];
        memset(c, 0, sizeof *c);
        memcpy(c->name, seg, sl);
        c->name[sl] = 0;
        c->b = b;

    }
    return 1;
}

/* ------------------------------------------------------------ install --- */

static wchar_t *me_wjoin(const char *dir, const char *rel)
{
    char path[MAX_PATH * 4];
    me_join_native(path, sizeof path, dir, rel);
    return sh_mb_wpath(path);
}

static void me_wmkdirs_for(wchar_t *file)
{
    wchar_t *p = file;
    if (!wcsncmp(p, L"\\\\?\\", 4)) p += 4;
    if (p[0] && p[1] == L':') p += 3;
    for (; *p; p++)
        if (*p == L'\\') { *p = 0; CreateDirectoryW(file, NULL); *p = L'\\'; }
}

static int me_put_file(const char *dir, const char *rel, const void *data, size_t n)
{
    wchar_t *w = me_wjoin(dir, rel);
    HANDLE h;
    size_t off = 0;
    int ok = 1;
    if (!w) return 0;
    me_wmkdirs_for(w);
    h = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return 0;
    while (ok && off < n) {
        DWORD chunk = (DWORD)((n - off) > (1u << 24) ? (1u << 24) : (n - off)), put = 0;
        ok = WriteFile(h, (const unsigned char *)data + off, chunk, &put, NULL) && put == chunk;
        off += put;
    }
    CloseHandle(h);
    return ok;
}

/* Remove a folder this import made under the temporary directory. */
static void me_rmtree(const wchar_t *dir)
{
    size_t dl = wcslen(dir), cap = dl + 4;
    wchar_t *pattern = (wchar_t *)malloc(sizeof(wchar_t) * cap);
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!pattern) return;
    _snwprintf_s(pattern, cap, _TRUNCATE, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            size_t ccap = dl + wcslen(fd.cFileName) + 2;
            wchar_t *child;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            child = (wchar_t *)malloc(sizeof(wchar_t) * ccap);
            if (!child) continue;
            _snwprintf_s(child, ccap, _TRUNCATE, L"%s\\%s", dir, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                me_rmtree(child);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(child);
            else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child);
            }
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* A fresh folder under the temporary directory for this import's copies. */
static int me_stage_root(char *out, size_t cap)
{
    static volatile LONG seq;
    wchar_t tmp[MAX_PATH + 1];
    char base[MAX_PATH * 4];
    DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    wchar_t *w;
    int made;
    size_t bl;
    if (!n || n > MAX_PATH || !me_w_to_utf8(tmp, base, sizeof base)) return 0;
    bl = strlen(base);
    if (bl && (base[bl - 1] == '\\' || base[bl - 1] == '/')) base[bl - 1] = 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus-import-%lu-%ld", base, GetCurrentProcessId(),
                InterlockedIncrement(&seq));
    w = sh_mb_wpath(out);
    if (!w) return 0;
    me_rmtree(w);
    made = CreateDirectoryW(w, NULL) != 0;
    free(w);
    return made;
}

/* Copy one carried folder out of the export (mod.json and assets/), then
 * put back, from this player's game, the bytes of each stock pointer whose
 * file the folder held (the export never carries game content), so the
 * install builds the same pointers. */
static int me_stage(me_import *im, me_carry *c, const char *stage, size_t index, modexport_stats *st, char *cause,
                    size_t cap)
{
    char prefix[SH_MS_NAME_MAX + 8];
    size_t pl, i;
    /* Named as the export carries it: a root install takes the folder's name. */
    _snprintf_s(c->stage, sizeof c->stage, _TRUNCATE, "%s\\%zu\\%s", stage, index, c->name);
    _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "mods/%s/", c->name);
    pl = strlen(prefix);
    for (i = 0; i < im->zr.n; i++) {
        const char *name = im->zr.e[i].name, *rel;
        unsigned char *bytes;
        size_t len = 0;
        int put;
        if (strncmp(name, prefix, pl)) continue;
        rel = name + pl;
        if (strcmp(rel, "mod.json") && strncmp(rel, "assets/", 7)) {
            me_log("MODS: import: %s is not part of a mod folder; left out", name);
            continue;
        }
        bytes = sh_zip_reader_read(&im->zr, (long)i, (size_t)SH_MB_MAX_FILE, &len, cause, cap);
        if (!bytes) return 0;
        put = me_put_file(c->stage, rel, bytes, len);
        free(bytes);
        if (!put) return me_cause(cause, cap, "cannot copy %s out of the export: %s", name, sh_reason_win32(GetLastError(), name, len));
        st->files++;
    }
    for (i = 0; i < c->b.file_count; i++) {
        const sh_mb_file *f = &c->b.files[i];
        unsigned char *bytes = NULL;
        size_t len = 0;
        wchar_t *w;
        int have, put;
        if (!f->stock || f->absent) continue;
        w = me_wjoin(c->stage, f->path);
        have = w && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
        free(w);
        if (have) continue;
        if (cause && cap) cause[0] = 0;
        if (!f->stock_source || sh_mb_stock_read(f->stock_source, f->blob, &bytes, &len, f->path, cause, cap) != SH_MB_STOCK_OK) {
            if (!cause || !cap || !cause[0]) me_cause(cause, cap, "%s names game content this game does not have", f->path);
            return 0;
        }
        put = me_put_file(c->stage, f->path, bytes, len);
        HeapFree(GetProcessHeap(), 0, bytes);
        if (!put) return me_cause(cause, cap, "cannot copy %s out of the game: %s", f->path, sh_reason_win32(GetLastError(), f->path, len));
    }
    return 1;
}

/* Remove what this import installed, and only that. */
static void me_rollback(me_import *im, const char *root)
{
    size_t i;
    for (i = 0; i < im->nc; i++) {
        char path[MAX_PATH * 4];
        if (!im->c[i].installed) continue;
        me_join_native(path, sizeof path, root, im->c[i].stored);
        if (sh_ms_remove_folder(path)) me_log("MODS: import: mods\\%s removed again", im->c[i].stored);
        else me_log("MODS: import: mods\\%s could not be removed again; remove it by hand", im->c[i].stored);
        im->c[i].installed = 0;
    }
}

static int me_rows_name_one(const sh_modmanifest_row *row, const me_carry *c)
{
    return !_stricmp(row->salt, c->b.id) && !_stricmp(row->build, c->b.hash);
}

/* A carried folder's row: its salt and build, with every name the rows that
 * name that build list (`names` receives the borrowed array; free it). */
static void me_carry_row(const me_carry *c, const sh_modmanifest_list *rows, sh_modmanifest_row *row, char ***names)
{
    size_t i, k, n = 0, total = 0;
    memset(row, 0, sizeof *row);
    *names = NULL;
    strncpy_s(row->salt, sizeof row->salt, c->b.id, _TRUNCATE);
    strcpy_s(row->build, sizeof row->build, c->b.hash);
    if (c->b.name) strncpy_s(row->name, sizeof row->name, c->b.name, _TRUNCATE);
    for (i = 0; rows && i < rows->count; i++)
        if (me_rows_name_one(&rows->rows[i], c)) total += rows->rows[i].name_count;
    if (total && (*names = (char **)calloc(total, sizeof **names)) != NULL)
        for (i = 0; rows && i < rows->count; i++)
            if (me_rows_name_one(&rows->rows[i], c))
                for (k = 0; k < rows->rows[i].name_count; k++) (*names)[n++] = rows->rows[i].names[k];
    row->names = *names;
    row->name_count = n;
}

/* Whether something on this machine already serves a carried folder's row
 * for a map or prefab whose place is `place` ("" the root): the build the one
 * place resolver names for it provides every name the rows list (modserve.h,
 * the asset rule), or the record knows it changes only the editor. Such a
 * folder is not installed. */
static int me_carry_served(const me_carry *c, const sh_modmanifest_list *rows, const char *place)
{
    sh_modmanifest_row row;
    char **names;
    int st;
    me_carry_row(c, rows, &row, &names);
    st = sh_modserve_row_served_in(&row, place, NULL, NULL, 0);
    free(names);
    return st == SH_MODSERVE_ROW_SERVED;
}

/* Install each carried folder whose row nothing on this machine serves
 * (me_carry_served) into the import's place, mods\maps\<title>\<mod name>\
 * (sh_ms_install_placed: the place's folder of the salt is replaced, the
 * place noted in the record). A place holds received builds as they are:
 * replacements are kept, and the folder must build to exactly the hash its
 * mod.json records. A refusal removes what the import installed; nothing that
 * was installed before is written. */
static int me_install(me_import *im, const sh_modmanifest_list *rows, const char *dir, modexport_stats *st,
                      char *cause, size_t cap)
{
    sh_mb_resolver r;
    sh_ms_place place;
    char root[MAX_PATH * 4], stage[MAX_PATH * 4] = "";
    size_t i, left = 0;
    int ok = 0, progress, staged = 0, locked = 0;
    const char *what = me_kind_word(im->kind);

    st->bundles = (unsigned)im->nc;
    st->entries = (unsigned)im->zr.n;
    st->zip_bytes = im->zr.size;
    if (!im->nc) return 1;
    if (!sh_ms_root(root, sizeof root)) return me_cause(cause, cap, "the mods folder cannot be resolved");
    for (i = 0; i < im->nc; i++) {
        me_carry *c = &im->c[i];
        if (me_carry_served(c, rows, dir)) {
            c->present = 1;
            st->bundles_present++;
            me_log("MODS: import: mods/%s skipped: %s@%.16s is served on this computer", c->name, c->b.id, c->b.hash);
            continue;
        }
        left++;
    }
    if (!left) { ok = 1; goto done; }

    /* The files are copied out of the export without the store lock: a large
     * import never holds up a save, a download or a build. */
    if (!me_stage_root(stage, sizeof stage)) { me_cause(cause, cap, "no temporary folder could be made for the export's mods"); goto done; }
    staged = 1;
    for (i = 0; i < im->nc; i++)
        if (!im->c[i].present && !me_stage(im, &im->c[i], stage, i, st, cause, cap)) goto done;
    modstore_lock();
    locked = 1;

    place.title = im->place_title;
    place.map_id = im->place_key;
    /* A mod that needs another carried one builds once that one is installed. */
    r.installed = NULL;
    r.installed_context = NULL;
    r.stock = g_stock_fn;
    r.stock_context = g_stock_ctx;
    do {
        progress = 0;
        for (i = 0; i < im->nc && left; i++) {
            me_carry *c = &im->c[i];
            sh_ms_install_stats is;
            sh_mb_bundle built;
            if (c->present || c->installed) continue;
            memset(&is, 0, sizeof is);
            memset(&built, 0, sizeof built);
            /* An exported folder is its author's build as it was: its
             * replacements are kept, never refused (a received build). */
            sh_mb_keep_replacements_here(1);
            if (!(dir[0] ? sh_ms_install_placed(c->stage, &place, &r, c->stored, sizeof c->stored, &is, &built, c->why,
                                                sizeof c->why)
                         : sh_ms_install_from_folder(c->stage, &r, c->stored, sizeof c->stored, &is, &built, c->why,
                                                     sizeof c->why))) {
                sh_mb_keep_replacements_here(0);
                sh_mb_free(&built);
                continue;
            }
            sh_mb_keep_replacements_here(0);
            left--;
            progress = 1;
            if (strcmp(built.hash, c->b.hash) != 0 || !built.validated) {
                me_log("MODS: import: mods/%s built to %.16s, not %.16s as its mod.json records", c->name, built.hash,
                       c->b.hash);
                me_cause(cause, cap, "%s in the export does not build to the build the %s names",
                         c->b.name && c->b.name[0] ? c->b.name : c->b.id, what);
                if (!is.present) c->installed = 1;
                sh_mb_free(&built);
                goto rollback;
            }
            if (is.present) {
                c->present = 1;
                st->bundles_present++;
                me_log("MODS: import: mods/%s is already mods\\%s", c->name, c->stored);
            } else {
                c->installed = 1;
                st->bundles_installed++;
                me_log("MODS: import: mods/%s installed as mods\\%s (%s@%.16s)", c->name, c->stored, built.id, built.hash);
            }
            sh_mb_free(&built);
        }
    } while (progress && left);
    if (left) {
        for (i = 0; i < im->nc; i++) {
            const me_carry *c = &im->c[i];
            if (c->present || c->installed) continue;
            me_cause(cause, cap, "%s, a mod this %s carries, does not install on this computer (%s); nothing was "
                     "installed", c->b.name && c->b.name[0] ? c->b.name : c->b.id, what, c->why);
            break;
        }
        goto rollback;
    }
    ok = 1;
    if (st->bundles_installed && sh_modserve_index_rescan() < 0)
        me_log("MODS: import: mods installed, but the serving index could not be rebuilt");
    goto done;
rollback:
    me_rollback(im, root);
    st->bundles_installed = 0;
done:
    if (locked) modstore_unlock();
    if (staged) {
        wchar_t *w = sh_mb_wpath(stage);
        if (w) { me_rmtree(w); free(w); }
    }
    return ok;
}

/* Rows that name a build the export does not carry and nothing on this
 * machine serves in `place`. */
static void me_count_absent(const me_import *im, const sh_modmanifest_list *rows, const char *place, modexport_stats *st)
{
    size_t i;
    for (i = 0; i < rows->count; i++) {
        const sh_modmanifest_row *row = &rows->rows[i];
        if (me_carried(im, row->salt, row->build)) continue;
        if (sh_modserve_row_served_in(row, place, NULL, NULL, 0) == SH_MODSERVE_ROW_SERVED) continue;
        st->bundles_absent++;
        me_log("MODS: import: the %s names %s, which the export does not carry", me_kind_word(im->kind),
               row->name[0] ? row->name : row->salt);
    }
}

/* The map import's place ("maps\<segment>"), keyed by its ZIP's path: the
 * record's, else a free one named after the ZIP. A carried folder that would
 * replace the place's folder of its salt (an earlier import of that ZIP, at
 * another build) never does: the import takes a fresh place beside it, so a
 * map saved from the earlier import keeps its mods, and a failed import
 * leaves every installed folder as it was. 1 with `out`, else 0. */
static int me_place_dir(me_import *im, const sh_modmanifest_list *rows, char *out, size_t cap)
{
    sh_ms_place place;
    size_t i;
    char folder[SH_MS_NAME_MAX + 1];
    out[0] = 0;
    place.title = im->place_title[0] ? im->place_title : "map";
    place.map_id = im->place_key;
    if (!sh_ms_place_dir(&place, out, cap)) return 0;
    for (i = 0; i < im->nc; i++) {
        const me_carry *c = &im->c[i];
        if (me_carry_served(c, rows, out) ||
            sh_ms_place_folder(&place, c->b.name, c->b.id, folder, sizeof folder) != 2)
            continue;
        _snprintf_s(im->place_key + strlen(im->place_key), sizeof im->place_key - strlen(im->place_key), _TRUNCATE,
                    "#%llu", (unsigned long long)GetTickCount64());
        me_log("MODS: import: mods\\%s holds another build of %s; this import gets a place of its own", out,
               c->b.name && c->b.name[0] ? c->b.name : c->b.id);
        return sh_ms_place_dir(&place, out, cap);
    }
    return 1;
}

int modexport_probe(const char *zip_path, char *cause, size_t cap)
{
    me_import im;
    char *text = NULL;
    size_t len = 0;
    long idx;
    if (cause && cap) cause[0] = 0;
    if (!me_open(&im, zip_path, ME_KIND_MAP, &text, &len, &idx, cause, cap)) return 0;
    free(text);
    me_import_free(&im);
    return 1;
}

/* The map text with `place` recorded in its member when a row resolves there
 * (map_carrier.h), else with no place: the map then finds its mods at the
 * root, as the player's own maps do. malloc'd; NULL when it cannot be
 * written. */
static char *me_stamp_place(const char *text, size_t len, const sh_modmanifest_list *rows, const char *place,
                            size_t *out_len, char *cause, size_t cap)
{
    size_t i, n = 0;
    int used = 0;
    char err[96] = "", *heap, *out;
    for (i = 0; i < rows->count && !used; i++) {
        sh_modserve_bundle_ref ref;
        used = sh_modserve_row_build_in(&rows->rows[i], place, &ref) && ref.place[0];
    }
    heap = sh_carrier_put_place(text, len, used ? place + sizeof SH_MODSERVE_PLACES : NULL, &n, err, sizeof err);
    if (!heap && err[0]) { me_cause(cause, cap, "the map could not record where its mods are: %s", err); return NULL; }
    if (!heap) n = len;
    out = (char *)malloc(n + 1);
    if (!out) { if (heap) HeapFree(GetProcessHeap(), 0, heap); me_cause(cause, cap, "out of memory"); return NULL; }
    memcpy(out, heap ? heap : text, n);
    out[n] = 0;
    if (heap) HeapFree(GetProcessHeap(), 0, heap);
    *out_len = n;
    return out;
}

char *modexport_read(const char *zip_path, size_t *out_len, modexport_stats *stp, char *cause, size_t cap)
{
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    me_import im;
    sh_modmanifest_list rows;
    char *text = NULL, *stamped = NULL, dir[SH_MODSERVE_PLACE_CAP];
    size_t len = 0;
    unsigned rejected = 0;
    long idx;
    double t0 = me_now_ms();
    if (out_len) *out_len = 0;
    memset(st, 0, sizeof *st);
    memset(&rows, 0, sizeof rows);
    if (cause && cap) cause[0] = 0;
    if (!me_open(&im, zip_path, ME_KIND_MAP, &text, &len, &idx, cause, cap)) { st->ms = me_now_ms() - t0; return NULL; }
    sh_modmanifest_read(text, len, &rows, &rejected);
    /* The map this ZIP carries arrives with its mods: its place is the
     * ZIP's, and a carried folder installs only when nothing serves its row
     * there (the root at the row's build first). Nothing here changes what
     * the loaded map serves: the place is recorded in the text, and the
     * load of that text sets it. */
    if (!me_collect(&im, &rows, 0, cause, cap)) goto done;
    if (!me_place_dir(&im, &rows, dir, sizeof dir)) { me_cause(cause, cap, "no folder could be named for the map's mods"); goto done; }
    if (!me_install(&im, &rows, dir, st, cause, cap)) goto done;
    me_count_absent(&im, &rows, dir, st);
    stamped = me_stamp_place(text, len, &rows, dir, &len, cause, cap);
    if (stamped) st->map_bytes = len;
done:
    free(text);
    if (!stamped) len = 0;
    sh_modmanifest_list_free(&rows);
    me_import_free(&im);
    st->ms = me_now_ms() - t0;
    if (out_len) *out_len = len;
    return stamped;
}

int modexport_read_prefab(const char *path, modexport_rows_fn rows_of, void *ctx, char **prefab, size_t *prefab_len,
                          char *name, size_t name_cap, modexport_stats *stp, char *cause, size_t cap)
{
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    me_import im;
    sh_modmanifest_list rows;
    char *text = NULL;
    size_t len = 0;
    long idx = -1;
    int ok = 0;
    double t0 = me_now_ms();
    memset(&rows, 0, sizeof rows);
    if (prefab) *prefab = NULL;
    if (prefab_len) *prefab_len = 0;
    if (name && name_cap) name[0] = 0;
    memset(st, 0, sizeof *st);
    if (cause && cap) cause[0] = 0;
    if (!prefab || !path || !path[0]) return me_cause(cause, cap, "nothing to read");
    if (!me_open(&im, path, ME_KIND_PREFAB, &text, &len, &idx, cause, cap)) { st->ms = me_now_ms() - t0; return 0; }
    if (name && name_cap) {
        size_t nl = strlen(im.zr.e[idx].name) - 5;
        if (nl >= name_cap) nl = name_cap - 1;
        memcpy(name, im.zr.e[idx].name, nl);
        name[nl] = 0;
    }
    if (rows_of) {
        int n = rows_of(ctx, text, len, &rows);
        if (n == -2) { me_cause(cause, cap, "%s", MODEXPORT_EARLIER); goto done; }
        if (n < 0) { me_cause(cause, cap, "the prefab's mods cannot be read"); goto done; }
    }
    /* A placed prefab's mods are the player's own: a carried folder installs
     * at the root when no root folder holds its salt (DECISIONS 12). */
    if (!me_collect(&im, &rows, rows_of == NULL, cause, cap) || !me_install(&im, &rows, "", st, cause, cap)) goto done;
    me_count_absent(&im, &rows, "", st);
    st->map_bytes = len;
    ok = 1;
done:
    me_import_free(&im);
    sh_modmanifest_list_free(&rows);
    if (ok) {
        *prefab = text;
        if (prefab_len) *prefab_len = len;
    } else {
        free(text);
        if (name && name_cap) name[0] = 0;
    }
    st->ms = me_now_ms() - t0;
    return ok;
}

/* ------------------------------------------------------------ rawmap ---- */

unsigned long long modexport_write_text(const char *data, size_t len, const char *path)
{
    char cause[MODEXPORT_CAUSE_MAX];
    return modexport_write_text_report(data, len, path, NULL, cause, sizeof cause);
}

unsigned long long modexport_write_text_report(const char *data, size_t len, const char *path,
                                               unsigned *bundles, char *cause_out, size_t cause_cap)
{
    modexport_stats st;
    char cause[MODEXPORT_CAUSE_MAX];
    if (bundles) *bundles = 0;
    if (cause_out && cause_cap) cause_out[0] = 0;
    if (!modexport_write(data, len, path, &st, cause, sizeof cause)) {
        me_log("MODS: export %s: refused: %s", path, cause);
        if (bundles) *bundles = st.bundles;
        if (cause_out && cause_cap) strncpy_s(cause_out, cause_cap, cause, _TRUNCATE);
        return 0;
    }
    if (bundles) *bundles = st.bundles;
    me_log("MODS: export %s: map %llu byte(s), %u mod folder(s), %u file(s) carried, %u entr%s, %llu byte(s) in %.0f ms",
           path, st.map_bytes, st.bundles, st.files, st.entries, st.entries == 1 ? "y" : "ies", st.zip_bytes, st.ms);
    return st.zip_bytes;
}

char *modexport_read_text_report(const char *path, size_t *out_len, char *cause_out, size_t cause_cap)
{
    modexport_stats st;
    char cause[MODEXPORT_CAUSE_MAX];
    char *text, *heap;
    size_t len = 0;
    if (out_len) *out_len = 0;
    if (cause_out && cause_cap) cause_out[0] = 0;
    text = modexport_read(path, &len, &st, cause, sizeof cause);
    if (!text) {
        me_log("MODS: import %s: refused: %s", path, cause);
        if (cause_out && cause_cap) strncpy_s(cause_out, cause_cap, cause, _TRUNCATE);
        return NULL;
    }
    me_log("MODS: import %s: %u mod folder(s) (%u installed, %u already installed), %u file(s) copied out, map %llu byte(s), "
           "%llu byte(s) read in %.0f ms%s", path, st.bundles, st.bundles_installed, st.bundles_present, st.files,
           st.map_bytes, st.zip_bytes, st.ms,
           st.bundles_absent ? "; the map also names mods this computer does not have and the export does not install" : "");
    if (!len || len > (size_t)INT_MAX) { free(text); return NULL; }
    heap = (char *)HeapAlloc(GetProcessHeap(), 0, len + 1);
    if (heap) { memcpy(heap, text, len); heap[len] = 0; if (out_len) *out_len = len; }
    free(text);
    return heap;
}

int modexport_validate_source(const char *path, char *out_msg, int msg_capacity)
{
    char cause[MODEXPORT_CAUSE_MAX];
    int ok;
    if (out_msg && msg_capacity > 0) out_msg[0] = 0;
    ok = modexport_probe(path, cause, sizeof cause);
    if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity, ok ? "ok" : cause, _TRUNCATE);
    return ok;
}

/* ------------------------------------------------------------ received builds */

int modexport_fetch_build(const sh_modmanifest_row *row, const char *place, const modnet_io *io, modsync_stats *st,
                          char *where, size_t where_cap)
{
    sh_ms_place at;
    char key[SH_MODSERVE_PLACE_CAP + 16], title[SH_MODSERVE_PLACE_CAP];
    const char *segment = place && place[0] ? strchr(place, '\\') : NULL;
    if (where && where_cap) where[0] = 0;
    if (segment) {
        /* The map's place, under a record key that names exactly that folder. */
        strncpy_s(title, sizeof title, segment + 1, _TRUNCATE);
        _snprintf_s(key, sizeof key, _TRUNCATE, "place:%s", title);
        at.title = title;
        at.map_id = key;
        (void)sh_ms_place_note(&at, place);
    }
    /* Without a place the build installs at the root, as the player's own mod. */
    if (!modsync_fetch_build_for(row, segment ? &at : NULL, io, st)) return 0;
    if (where && where_cap) strncpy_s(where, where_cap, st->folder, _TRUNCATE);
    return 1;
}
