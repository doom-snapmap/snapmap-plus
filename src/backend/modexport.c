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
#include "modsync.h"
#include "mp_json.h"
#include "prefab_block.h"
#include "backend_log.h"
#include "plain_reason.h"
#include "modzip.h"

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

static int mm_list_has(const sh_modmanifest_list *list, const char *bundle)
{
    size_t i;
    for (i = 0; i < list->count; i++) if (!strcmp(list->rows[i].bundle, bundle)) return 1;
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
    memset(&pl, 0, sizeof pl);
    me_map_entry(zip_path ? zip_path : "", entry, sizeof entry);
    pl.entry = entry;
    pl.text = map_json;
    pl.len = len;
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

/* One declared build as the folder it is installed as: mods/<name>/mod.json
 * as it is on disk (its bundle hash checked), then every file its bundle
 * carries under assets/, each checked against the hash mod.json records,
 * read from `files_dir` and, where that copy differs, from `alt_dir` (the
 * folder's last good build snapshot, DESIGN-additive 2.8, or the folder
 * itself). A stock pointer is an entry of mod.json and has no file here:
 * installed game content never leaves the machine. */
static int me_add_build(sh_zip_writer *w, const char *zip_name, const sh_mb_bundle *b, const char *modjson_path,
                        const char *files_dir, const char *alt_dir, modexport_stats *st, char *cause, size_t cap)
{
    char name[SH_ZIP_NAME_MAX + 1], path[MAX_PATH * 4], actual[SH_MB_HASH_HEX + 1], folder[SH_ZIP_NAME_MAX + 1], *c;
    unsigned char *bytes;
    size_t blen = 0, k;
    sh_mb_bundle check;
    int same;
    /* A ZIP entry name separates with '/' only: a mod in an organizing
     * folder ("campaign-interactables\campaign-interactables-1") is carried
     * as mods/campaign-interactables/campaign-interactables-1/. */
    strcpy_s(folder, sizeof folder, zip_name);
    for (c = folder; *c; c++) if (*c == '\\') *c = '/';
    bytes = sh_mb_read_file(modjson_path, ME_MODJSON_MAX, &blen);
    if (!bytes) return me_cause(cause, cap, "mods\\%s\\mod.json cannot be read", zip_name);
    memset(&check, 0, sizeof check);
    same = sh_mb_load_text((const char *)bytes, blen, &check) && !strcmp(check.hash, b->hash);
    sh_mb_free(&check);
    if (!same) {
        free(bytes);
        return me_cause(cause, cap, "mods\\%s\\mod.json changed since the store was read; rescan and export again", zip_name);
    }
    _snprintf_s(name, sizeof name, _TRUNCATE, "mods/%s/mod.json", folder);
    sh_zip_writer_add(w, name, bytes, blen);
    free(bytes);
    for (k = 0; k < b->file_count && !w->failed; k++) {
        const sh_mb_file *f = &b->files[k];
        int pass;
        if (f->stock) continue;
        bytes = NULL;
        for (pass = 0; pass < 2 && !bytes; pass++) {
            const char *dir = pass ? alt_dir : files_dir;
            if (!dir || !dir[0]) continue;
            me_join_native(path, sizeof path, dir, f->path);
            bytes = sh_mb_read_file(path, (size_t)SH_MB_MAX_FILE, &blen);
            if (bytes) {
                sh_mb_sha256_hex(bytes, blen, actual);
                if (blen != f->size || strcmp(actual, f->blob) != 0) { free(bytes); bytes = NULL; }
            }
        }
        if (!bytes) return me_cause(cause, cap, "%s %s is missing or damaged (run sh_mods doctor verify)", zip_name, f->path);
        _snprintf_s(name, sizeof name, _TRUNCATE, "mods/%s/%s", folder, f->path);
        sh_zip_writer_add(w, name, bytes, blen);
        free(bytes);
        st->files++;
    }
    if (w->failed) return me_cause(cause, cap, "%s", w->cause);
    return 1;
}

/* A declared build the export carries: an installed folder at exactly the
 * row's hash, else a last good build snapshot at that hash with no folder
 * (mods\.builds\<key>@<hash16>\, the folder moved on), else the folder an
 * alias of the hash names (the author rebuilt it in place; the rows are moved
 * to it, as that is the build this machine serves). */
typedef struct me_pick {
    const sh_ms_entry *e;               /* exact or alias */
    int snap;                           /* a snapshot only: b and dir */
    sh_mb_bundle b;
    char dir[MAX_PATH * 4];
} me_pick;

static void me_pick_free(me_pick *p) { if (p->snap) sh_mb_free(&p->b); memset(p, 0, sizeof *p); }

static const sh_mb_bundle *me_pick_bundle(const me_pick *p) { return p->snap ? &p->b : p->e ? &p->e->bundle : NULL; }

static int me_snapshot_at(const char *root, const char *hash, me_pick *out)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern;
    char pat[MAX_PATH * 4];
    HANDLE h;
    int found = 0;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "%s\\%s\\*@%.16s", root, SH_MS_BUILDS_DIR, hash);
    pattern = sh_mb_wpath(pat);
    if (!pattern) return 0;
    h = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char leaf[MAX_PATH * 4], mj[MAX_PATH * 4];
        unsigned char *text;
        size_t len = 0;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, leaf, (int)sizeof leaf, NULL, NULL)) continue;
        _snprintf_s(out->dir, sizeof out->dir, _TRUNCATE, "%s\\%s\\%s", root, SH_MS_BUILDS_DIR, leaf);
        me_join_native(mj, sizeof mj, out->dir, "bundle.json");
        text = sh_mb_read_file(mj, ME_MODJSON_MAX, &len);
        if (!text) continue;
        memset(&out->b, 0, sizeof out->b);
        if (sh_mb_load_text((const char *)text, len, &out->b) && !strcmp(out->b.hash, hash)) found = 1;
        else sh_mb_free(&out->b);
        free(text);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    if (found) out->snap = 1;
    else out->dir[0] = 0;
    return found;
}

/* 1 exact, 2 snapshot, 3 alias, 0 none. */
static int me_pick_row(const sh_ms_list *l, const char *root, const char *hash, me_pick *out)
{
    size_t i;
    const sh_ms_entry *e;
    memset(out, 0, sizeof *out);
    for (i = 0; i < l->count; i++)
        if (!strcmp(l->items[i].bundle.hash, hash) && sh_ms_entry_usable(&l->items[i])) { out->e = &l->items[i]; return 1; }
    if (root && me_snapshot_at(root, hash, out)) return 2;
    e = sh_ms_resolve(l, hash);
    if (e && sh_ms_entry_usable(e)) { out->e = e; return 3; }
    return 0;
}

static int me_add_pick(sh_zip_writer *w, const me_pick *p, modexport_stats *st, char *cause, size_t cap)
{
    char mj[MAX_PATH * 4];
    if (p->snap) {
        me_join_native(mj, sizeof mj, p->dir, "bundle.json");
        return me_add_build(w, p->b.id, &p->b, mj, p->dir, NULL, st, cause, cap);
    }
    me_join_native(mj, sizeof mj, p->e->folder, "mod.json");
    /* While the folder's latest build is failed, its files may hold other
     * bytes: the last good build's are read from its snapshot first. */
    if (p->e->build_failed[0] && p->e->snapshot)
        return me_add_build(w, p->e->name, &p->e->bundle, mj, p->e->snapshot, p->e->folder, st, cause, cap);
    return me_add_build(w, p->e->name, &p->e->bundle, mj, p->e->folder, p->e->snapshot, st, cause, cap);
}

/* The rows were moved to the builds that serve them: pick again. */
static int me_repick(me_pick **picked, size_t old_n, const sh_ms_list *l, const char *root,
                     const sh_modmanifest_list *rows, char *cause, size_t cap)
{
    size_t i;
    me_pick *p = (me_pick *)calloc(rows->count ? rows->count : 1, sizeof *p);
    if (!p) return me_cause(cause, cap, "out of memory");
    for (i = 0; i < rows->count; i++)
        if (!me_pick_row(l, root, rows->rows[i].bundle, &p[i])) {
            size_t k;
            for (k = 0; k < i; k++) me_pick_free(&p[k]);
            free(p);
            return me_cause(cause, cap, "%s (bundle %.16s) is not installed", rows->rows[i].id, rows->rows[i].bundle);
        }
    for (i = 0; i < old_n; i++) me_pick_free(&(*picked)[i]);
    free(*picked);
    *picked = p;
    return 1;
}

static int me_write(const me_payload *pl, const char *zip_path, modexport_stats *stp, char *cause, size_t cap)
{
    const char *map_json = pl->text;
    size_t len = pl->len;
    const int prefab = pl->rows != NULL;
    char *prefab_text = NULL;
    size_t moved_rows = 0;
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    sh_modmanifest_list rows;
    sh_ms_list l;
    me_pick *picked = NULL;
    char root[MAX_PATH * 4] = "";
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
            if (!mm_list_has(&rows, pl->rows->rows[i].bundle) && !sh_modmanifest_list_add(&rows, &pl->rows->rows[i])) {
                sh_modmanifest_list_free(&rows);
                return me_cause(cause, cap, "out of memory");
            }
    } else sh_modmanifest_read(map_json, len, &rows, &rejected);
    st->bundles = (unsigned)rows.count;
    st->map_bytes = len;

    modstore_lock();
    locked = 1;
    listed = sh_ms_enumerate(&l);
    if (!listed) { me_cause(cause, cap, "the mods store cannot be listed"); goto done; }
    if (!sh_ms_root(root, sizeof root)) root[0] = 0;
    picked = (me_pick *)calloc(rows.count ? rows.count : 1, sizeof *picked);
    if (!picked) { me_cause(cause, cap, "out of memory"); goto done; }
    /* Each declared bundle at its declared hash (DESIGN-additive 4). */
    for (i = 0; i < rows.count; i++) {
        int how = me_pick_row(&l, root, rows.rows[i].bundle, &picked[i]);
        if (!how) {
            me_cause(cause, cap, "the %s needs %s@%s (bundle %.16s), which is not installed or failed verification",
                     prefab ? "prefab" : "map", rows.rows[i].id, rows.rows[i].version, rows.rows[i].bundle);
            goto done;
        }
        if (how == 2)
            me_log("MODS: export: %s at bundle %.16s is carried from its last good build snapshot %s", rows.rows[i].id,
                   rows.rows[i].bundle, picked[i].dir);
    }
    {
        /* A row may pin an earlier build (an alias): the export carries the
         * folder serving it now, and the exported rows name that build, so the
         * importer needs no alias. */
        sh_modmanifest_list fixed;
        size_t moved = 0, map_len = 0, old_n = rows.count;
        char err[160] = "";
        for (i = 0; i < rows.count; i++) if (strcmp(me_pick_bundle(&picked[i])->hash, rows.rows[i].bundle)) moved++;
        if (moved) {
            memset(&fixed, 0, sizeof fixed);
            for (i = 0; i < rows.count; i++) {
                const sh_mb_bundle *b = me_pick_bundle(&picked[i]);
                sh_modmanifest_row *r = NULL;
                for (k = 0; k < fixed.count && !r; k++) if (!strcmp(fixed.rows[k].bundle, b->hash)) r = &fixed.rows[k];
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
                strcpy_s(r->bundle, sizeof r->bundle, b->hash);
                strncpy_s(r->id, sizeof r->id, b->id, _TRUNCATE);
                strncpy_s(r->version, sizeof r->version, b->version, _TRUNCATE);
                r->size = b->total_size;
            }
            if (prefab) {
                /* A prefab's rows live in its block, written below from the rows carried. */
                moved_rows = moved;
                me_log("MODS: export: %zu row(s) pinned an earlier build; the export carries and names the current one(s)", moved);
                sh_modmanifest_list_free(&rows);
                rows = fixed;
                if (!me_repick(&picked, old_n, &l, root, &rows, cause, cap)) goto done;
                st->bundles = (unsigned)rows.count;
                goto rows_done;
            }
            moved_map = sh_modmanifest_write(map_json, len, &fixed, &map_len, err, sizeof err);
            if (!moved_map) {
                sh_modmanifest_list_free(&fixed);
                me_cause(cause, cap, "the map's rows could not be moved to the current builds: %s", err[0] ? err : "unknown");
                goto done;
            }
            me_log("MODS: export: %zu row(s) pinned an earlier build; the export carries and names the current one(s)", moved);
            map_json = moved_map;
            len = map_len;
            sh_modmanifest_list_free(&rows);
            rows = fixed;
            if (!me_repick(&picked, old_n, &l, root, &rows, cause, cap)) goto done;
            st->bundles = (unsigned)rows.count;
            st->map_bytes = len;
        }
    }
rows_done:
    if (prefab && pl->text_for) {
        prefab_text = pl->text_for(pl->ctx, &rows, moved_rows > 0, &len);
        if (!prefab_text || !len || len > MODEXPORT_PREFAB_MAX) { me_cause(cause, cap, "the prefab's mods could not be written into it"); goto done; }
        map_json = prefab_text;
        st->map_bytes = len;
    }

    _snprintf_s(tmp, sizeof tmp, _TRUNCATE, "%s.smp-%lu.tmp", zip_path, GetCurrentProcessId());
    wfinal = sh_mb_wpath(zip_path);
    wtmp = sh_mb_wpath(tmp);
    if (!wfinal || !wtmp) { me_cause(cause, cap, "the destination path is not usable"); goto done; }
    if (!sh_zip_writer_open(&w, wtmp)) { me_cause(cause, cap, "cannot create %s: %s", zip_path, sh_reason_win32(GetLastError(), zip_path, 0)); goto done; }

    sh_zip_writer_add(&w, pl->entry, map_json, len);
    for (i = 0; i < rows.count && !w.failed; i++) {
        int dup = 0;
        for (k = 0; k < i && !dup; k++) dup = !strcmp(me_pick_bundle(&picked[k])->hash, me_pick_bundle(&picked[i])->hash);
        if (dup) continue;
        if (!me_add_pick(&w, &picked[i], st, cause, cap)) goto done;
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
    if (picked) for (i = 0; i < rows.count; i++) me_pick_free(&picked[i]);
    free(picked);
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
    sh_mb_bundle b;                     /* its mod.json, loaded: the bundle hash it records */
    int present;                        /* the store serves that bundle already */
    int installed;                      /* this import installed it, as `stored` */
    int earlier_rules;                  /* its block was written under an earlier builder's rules:
                                           b.hash is the hash it records; the install rebuilds it */
    int left_out;                       /* an earlier-rules carry today's rules cannot build: not
                                           installed, its row counts as a mod this computer lacks */
    char stored[SH_MS_KEY_MAX + 1];
    char stage[MAX_PATH * 4];           /* its copy outside the store, which the install builds */
    char why[SH_MB_CAUSE_MAX];          /* the last install refusal */
} me_carry;

typedef struct me_import {
    sh_zip_reader zr;
    me_carry *c;
    size_t nc, cap;
    int kind;                           /* what the export holds */
    int retired;                        /* a map's export in beta.20's bundle form (me_install_retired) */
    char zip_name[MAX_PATH];            /* the export's file name without its folder and extension: mods\imports\<it>\ */
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

#define ME_RETIRED_MAP "map.rawmap.json"

/* The retired bundle form of earlier releases: a bundle.canonical.json
 * anywhere, or an export.json that is an index rather than a prefab or map
 * named "export". */
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
    if (path) {
        /* The name its imported mods are grouped under: mods\imports\<name>\. */
        const char *leaf = path, *c;
        size_t ln;
        for (c = path; *c; c++) if (*c == '\\' || *c == '/') leaf = c + 1;
        ln = strlen(leaf);
        if (ln > 4 && (!_stricmp(leaf + ln - 4, ".zip") || !_stricmp(leaf + ln - 4, ".json"))) ln -= 4;
        if (ln > 5 && !_stricmp(leaf + ln - 5, ".json")) ln -= 5;
        if (ln >= sizeof im->zip_name) ln = sizeof im->zip_name - 1;
        memcpy(im->zip_name, leaf, ln);
        im->zip_name[ln] = 0;
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
            /* beta.20's map export: its map is always map.rawmap.json. */
            *idx = want == ME_KIND_MAP ? sh_zip_reader_find(&im->zr, ME_RETIRED_MAP) : -1;
            if (*idx < 0) {
                me_import_free(im);
                return me_cause(cause, cap, "%s", want == ME_KIND_PREFAB ? MODEXPORT_RETIRED_PREFAB : MODEXPORT_RETIRED_MAP);
            }
            im->retired = 1;
        }
        for (i = 0; i < im->zr.n && !im->retired; i++) {
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
            return me_cause(cause, cap, "%s", MODEXPORT_RETIRED_PREFAB);
        }
    }
    return 1;
}

static int me_rows_name(const sh_modmanifest_list *rows, const char *hash)
{
    size_t i;
    for (i = 0; rows && i < rows->count; i++) if (!_stricmp(rows->rows[i].bundle, hash)) return 1;
    return 0;
}

static int me_carried(const me_import *im, const char *hash)
{
    size_t i;
    for (i = 0; i < im->nc; i++) if (!im->c[i].left_out && !_stricmp(im->c[i].b.hash, hash)) return 1;
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

/* The carried folders, each mods/<name>/mod.json, whose bundle a row names
 * (every one when `all`). A folder no row names is left out. */
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
            /* A block an earlier builder's rules wrote is rebuilt under
             * today's at the install, as the store rebuilds its own folders
             * once after a rules change; it answers for the hash it records. */
            sh_mb_bundle older;
            if (sh_mb_load_text_earlier_rules(text, len, &older)) {
                sh_mb_free(&b);
                b = older;
                strcpy_s(b.hash, sizeof b.hash, b.prev_hash);
                b.prev_hash[0] = 0;
                b.rules = -1;   /* marks the carry below */
            } else {
                sh_mb_free(&older);
                me_log("MODS: import: %s is not a built mod (%s); left out", name, b.first_cause);
                sh_mb_free(&b);
                free(text);
                continue;
            }
        }
        free(text);
        if ((!all && !me_rows_name(rows, b.hash)) || me_carried(im, b.hash)) {
            me_log("MODS: import: mods/%.*s (%s@%s, bundle %.16s) is carried but the %s does not name it; left out",
                   (int)sl, seg, b.id, b.version, b.hash, me_kind_word(im->kind));
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
        c->earlier_rules = b.rules < 0;
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
    _snprintf_s(c->stage, sizeof c->stage, _TRUNCATE, "%s\\%zu", stage, index);
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

/* The asset rule for a carried folder (modserve.h sh_modserve_row_served),
 * pinned to its hash: the serving index has that bundle (or an alias of it,
 * or the record knows it changes only the editor) and it resolves every name
 * the rows that name its bundle list. Another build of the key never serves
 * it (DESIGN-additive 2.5). */
static int me_carry_served(const me_carry *c, const sh_modmanifest_list *rows)
{
    sh_modmanifest_row row;
    char **names = NULL;
    size_t i, k, n = 0, total = 0;
    int st;
    memset(&row, 0, sizeof row);
    if (strlen(c->b.id) >= sizeof row.id) return 0;
    strcpy_s(row.id, sizeof row.id, c->b.id);
    strcpy_s(row.bundle, sizeof row.bundle, c->b.hash);
    for (i = 0; rows && i < rows->count; i++)
        if (!_stricmp(rows->rows[i].bundle, c->b.hash)) total += rows->rows[i].name_count;
    if (total && !(names = (char **)calloc(total, sizeof *names))) return 0;
    for (i = 0; rows && i < rows->count; i++)
        if (!_stricmp(rows->rows[i].bundle, c->b.hash))
            for (k = 0; k < rows->rows[i].name_count; k++) names[n++] = rows->rows[i].names[k];
    row.names = names;
    row.name_count = n;
    st = sh_modserve_row_served(&row, NULL, 0);
    free(names);
    return st == SH_MODSERVE_ROW_SERVED;
}

/* The ordinary install by hash (DESIGN-additive 4) of each carried folder the
 * store does not already serve. A carried folder whose bundle the store holds
 * (by hash or alias) is skipped and counted as already installed. Every other
 * one is copied out of the export and installed as a folder from elsewhere is
 * (sh_ms_install_with, origin "imported": built, copied in, verified), under
 * its key when no folder of the key is installed and that name is free, else
 * beside the installed builds of its key as <key>@<hash16>
 * (sh_ms_build_folder_name). Another build of the key is never a refusal and
 * never written over. The built bundle must be the one its own mod.json names.
 * A refusal removes what the import installed; nothing that was installed
 * before is ever written. */
static int me_install(me_import *im, const sh_modmanifest_list *rows, modexport_stats *st, char *cause, size_t cap)
{
    sh_ms_list l;
    sh_mb_resolver r;
    char root[MAX_PATH * 4], stage[MAX_PATH * 4] = "";
    size_t i, k, left = 0;
    int ok = 0, listed = 0, progress, staged = 0;
    const char *what = me_kind_word(im->kind);

    st->bundles = (unsigned)im->nc;
    st->entries = (unsigned)im->zr.n;
    st->zip_bytes = im->zr.size;
    if (!im->nc) return 1;
    modstore_lock();
    if (!sh_ms_root(root, sizeof root)) { me_cause(cause, cap, "the mods folder cannot be resolved"); goto done; }
    listed = sh_ms_enumerate(&l);
    if (!listed) { me_cause(cause, cap, "the mods folder cannot be listed"); goto done; }
    for (i = 0; i < im->nc; i++) {
        me_carry *c = &im->c[i];
        const sh_ms_entry *e = sh_ms_resolve(&l, c->b.hash);
        if ((e && sh_ms_entry_usable(e)) || me_carry_served(c, rows)) {
            c->present = 1;
            st->bundles_present++;
            me_log("MODS: import: mods/%s skipped: %s at bundle %.16s is installed", c->name, c->b.id, c->b.hash);
            continue;
        }
        if (e) {
            me_cause(cause, cap, "the installed %s %s needs attention (%s); run sh_mods doctor verify", c->b.id, c->b.version,
                     e->cause[0] ? e->cause : "it does not verify");
            goto done;
        }
        left++;
    }
    sh_ms_list_free(&l);
    listed = 0;
    if (!left) { ok = 1; goto done; }

    if (!me_stage_root(stage, sizeof stage)) { me_cause(cause, cap, "no temporary folder could be made for the export's mods"); goto done; }
    staged = 1;
    for (i = 0; i < im->nc; i++)
        if (!im->c[i].present && !me_stage(im, &im->c[i], stage, i, st, cause, cap)) goto done;

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
            sh_ms_place place;
            int same_id = 0;
            if (c->present || c->installed) continue;
            if (sh_ms_enumerate(&l)) {
                for (k = 0; k < l.count && !same_id; k++) same_id = l.items[k].loaded && !strcmp(l.items[k].bundle.id, c->b.id);
                sh_ms_list_free(&l);
            }
            /* A build an export carried is the product's to place (DESIGN-
             * additive A.4, owner 2026-11-10): mods\imports\<export name>\<key>\,
             * never <key>@<hash16> beside the player's own folders, whatever
             * the export's folder was called. An imported folder is the
             * author's build as it was: its replacements are kept, never
             * refused (DESIGN-additive 2.8). One built under earlier rules
             * gets the hash today's rules give, so the store names it by that
             * hash (and finds it if installed). */
            place.group = SH_MS_GROUP_IMPORTS;
            place.title = im->zip_name[0] ? im->zip_name : NULL;
            place.map_id = NULL;
            sh_mb_keep_replacements_here(1);
            if (!sh_ms_install_placed(c->stage, SH_MS_ORIGIN_IMPORTED, &place, &r, c->stored, sizeof c->stored, &is, &built,
                                      c->why, sizeof c->why)) {
                sh_mb_keep_replacements_here(0);
                sh_mb_free(&built);
                continue;
            }
            sh_mb_keep_replacements_here(0);
            left--;
            progress = 1;
            if (c->earlier_rules && built.validated && is.present) {
                /* Today's rules build it to a bundle the store already holds. */
                c->present = 1;
                st->bundles_present++;
                me_log("MODS: import: mods/%s (built under earlier rules as %.16s) builds to bundle %.16s, which mods\\%s "
                       "already holds", c->name, c->b.hash, built.hash, c->stored);
                sh_mb_free(&built);
                continue;
            }
            c->installed = 1;
            if (c->earlier_rules && built.validated) {
                if (strcmp(built.hash, c->b.hash))
                    me_log("MODS: import: mods/%s was built under earlier rules as bundle %.16s; today's rules build it to "
                           "%.16s, installed as mods\\%s (the %s's row keeps naming %.16s)", c->name, c->b.hash, built.hash,
                           c->stored, what, c->b.hash);
            } else if (strcmp(built.hash, c->b.hash) != 0 || !built.validated) {
                me_log("MODS: import: mods/%s built to bundle %.16s, not %.16s as its mod.json records", c->name, built.hash,
                       c->b.hash);
                me_cause(cause, cap, "%s %s in the export does not build to the bundle the %s names", c->b.id, c->b.version, what);
                sh_mb_free(&built);
                goto rollback;
            }
            me_log("MODS: import: mods/%s installed as mods\\%s (%s@%s+%llu, bundle %.16s)%s", c->name, c->stored, built.id,
                   built.version, built.build, built.hash, same_id ? ", beside the installed build(s) of that key" : "");
            sh_mb_free(&built);
            st->bundles_installed++;
        }
    } while (progress && left);
    /* A folder built under earlier rules that today's rules refuse (a retired
     * intent key's partial, say) is that one mod's failure, not the export's:
     * it is left out, and the map's row for it goes the way of any build this
     * computer lacks (the hold asks the store, or serves the installed build
     * of its key, or says why not). */
    for (i = 0; i < im->nc && left; i++) {
        me_carry *c = &im->c[i];
        if (c->present || c->installed || !c->earlier_rules) continue;
        c->left_out = 1;
        left--;
        me_log("MODS: import: mods/%s (%s %s, bundle %.16s) was built by an earlier development build and today's "
               "rules do not build it (%s); left out, so the %s treats it as a mod this computer does not have",
               c->name, c->b.id, c->b.version, c->b.hash, c->why, what);
    }
    if (left) {
        for (i = 0; i < im->nc; i++) {
            const me_carry *c = &im->c[i];
            if (c->present || c->installed || c->left_out) continue;
            me_cause(cause, cap, "%s %s, a mod this %s carries, does not install on this computer (%s); nothing was "
                     "installed. Ask whoever shared it for a new export saved with this release", c->b.id, c->b.version,
                     what, c->why);
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
    if (listed) sh_ms_list_free(&l);
    modstore_unlock();
    if (staged) {
        wchar_t *w = sh_mb_wpath(stage);
        if (w) { me_rmtree(w); free(w); }
    }
    return ok;
}

/* Rows that name a bundle the export does not carry and the store does not
 * serve (by its hash or an alias of it). */
static void me_count_absent(const me_import *im, const sh_modmanifest_list *rows, modexport_stats *st)
{
    sh_ms_list l;
    size_t i;
    int listed;
    modstore_lock();
    listed = sh_ms_enumerate(&l);
    for (i = 0; i < rows->count; i++) {
        const sh_ms_entry *e;
        if (me_carried(im, rows->rows[i].bundle)) continue;
        e = listed ? sh_ms_resolve(&l, rows->rows[i].bundle) : NULL;
        if (e && sh_ms_entry_usable(e)) continue;
        if (sh_modserve_row_served(&rows->rows[i], NULL, 0) == SH_MODSERVE_ROW_SERVED) continue;
        st->bundles_absent++;
        me_log("MODS: import: the %s names %s@%s (bundle %.16s), which the export does not carry",
               me_kind_word(im->kind), rows->rows[i].id, rows->rows[i].version, rows->rows[i].bundle);
    }
    if (listed) sh_ms_list_free(&l);
    modstore_unlock();
}

/* beta.20's map export (the only release that wrote it): map.rawmap.json,
 * export.json, and per bundle mods/[legacy/]<id>@<version>+<build>/ with
 * bundle.json, bundle.canonical.json and assets/, each distinct file stored
 * once in the ZIP. Each canonical manifest a row names is installed as a
 * download is (modsync_install_manifest), its files taken from the ZIP by
 * hash, so the bundle keeps the hash the map pins. On a refusal the bundles
 * this import installed are removed again. */
static int me_retired_blob(void *ctx, const char *sha, unsigned long long size, unsigned char **bytes, size_t *len,
                           char *cause, size_t cap)
{
    const sh_zip_reader *r = (const sh_zip_reader *)ctx;
    char got[SH_MB_HASH_HEX + 1];
    size_t i;
    for (i = 0; i < r->n; i++) {
        unsigned char *b;
        size_t n = 0;
        if (r->e[i].usize != size || strncmp(r->e[i].name, "mods/", 5) || !strstr(r->e[i].name, "/assets/")) continue;
        b = sh_zip_reader_read(r, (long)i, (size_t)size + 1, &n, cause, cap);
        if (!b) continue;
        sh_mb_sha256_hex(b, n, got);
        if (!strcmp(got, sha)) { *bytes = b; *len = n; return 1; }
        free(b);
    }
    return me_cause(cause, cap, "the export does not carry it");
}

static int me_install_retired(me_import *im, const sh_modmanifest_list *rows, modexport_stats *st, char *cause, size_t cap)
{
    static const char LEAF[] = "/bundle.canonical.json";
    char root[MAX_PATH * 4], done[ME_MAX_MODS][sizeof ((modsync_install_info *)0)->rel], hash[SH_MB_HASH_HEX + 1];
    size_t i, nd = 0, ll = sizeof LEAF - 1;
    int ok = 1;
    if (!sh_ms_root(root, sizeof root)) return me_cause(cause, cap, "the mods folder cannot be resolved");
    st->entries = (unsigned)im->zr.n;
    st->zip_bytes = im->zr.size;
    for (i = 0; i < im->zr.n && ok; i++) {
        const char *name = im->zr.e[i].name;
        size_t n = strlen(name), len = 0;
        modsync_install_info info;
        char why[SH_MB_CAUSE_MAX] = "";
        unsigned char *text;
        if (n <= ll || strcmp(name + n - ll, LEAF) || strncmp(name, "mods/", 5)) continue;
        text = sh_zip_reader_read(&im->zr, (long)i, ME_INDEX_MAX, &len, cause, cap);
        if (!text) { ok = 0; break; }
        sh_mb_sha256_hex(text, len, hash);
        if (!me_rows_name(rows, hash)) { free(text); continue; }
        st->bundles++;
        {
            sh_ms_place place;
            place.group = SH_MS_GROUP_IMPORTS;
            place.title = im->zip_name[0] ? im->zip_name : NULL;
            place.map_id = NULL;
            ok = modsync_install_manifest_for((const char *)text, len, hash, &place, me_retired_blob, &im->zr, &info, why,
                                              sizeof why);
        }
        free(text);
        if (!ok) { me_cause(cause, cap, "%.*s in the export does not install: %s", (int)(n - ll - 5), name + 5, why); break; }
        if (info.already_installed) { st->bundles_present++; continue; }
        if (nd < ME_MAX_MODS) strcpy_s(done[nd++], sizeof done[0], info.rel);
        st->bundles_installed++;
        st->files += info.blobs_present + info.blobs_fetched;
        me_log("MODS: import: %.*s (beta.20 form, bundle %.16s) installed as mods\\%s", (int)(n - ll - 5), name + 5, hash, info.rel);
    }
    if (!ok)
        for (i = 0; i < nd; i++) {
            char path[MAX_PATH * 4];
            me_join_native(path, sizeof path, root, done[i]);
            if (!sh_ms_remove_folder(path)) me_log("MODS: import: mods\\%s could not be removed again; remove it by hand", done[i]);
        }
    if (st->bundles_installed && sh_modserve_index_rescan() < 0)
        me_log("MODS: import: mods installed, but the serving index could not be rebuilt");
    return ok;
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

char *modexport_read(const char *zip_path, size_t *out_len, modexport_stats *stp, char *cause, size_t cap)
{
    modexport_stats local;
    modexport_stats *st = stp ? stp : &local;
    me_import im;
    sh_modmanifest_list rows;
    char *text = NULL;
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
    if (im.retired ? !me_install_retired(&im, &rows, st, cause, cap)
                   : (!me_collect(&im, &rows, 0, cause, cap) || !me_install(&im, &rows, st, cause, cap))) {
        free(text);
        text = NULL;
        len = 0;
    } else {
        me_count_absent(&im, &rows, st);
        st->map_bytes = len;
    }
    sh_modmanifest_list_free(&rows);
    me_import_free(&im);
    st->ms = me_now_ms() - t0;
    if (out_len) *out_len = len;
    return text;
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
    if (rows_of && rows_of(ctx, text, len, &rows) < 0) {
        me_cause(cause, cap, "the prefab's mods cannot be read");
        goto done;
    }
    if (!me_collect(&im, &rows, rows_of == NULL, cause, cap) || !me_install(&im, &rows, st, cause, cap)) goto done;
    me_count_absent(&im, &rows, st);
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

static int me_acp_to_utf8(const char *acp, char *out, size_t cap)
{
    wchar_t wide[MAX_PATH * 2];
    if (!acp || !acp[0] || !MultiByteToWideChar(CP_ACP, 0, acp, -1, wide, MAX_PATH * 2)) return 0;
    return WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, (int)cap, NULL, NULL) > 0;
}

unsigned long long modexport_write_text(const char *data, size_t len, const char *path_acp)
{
    char cause[MODEXPORT_CAUSE_MAX];
    return modexport_write_text_report(data, len, path_acp, NULL, cause, sizeof cause);
}

unsigned long long modexport_write_text_report(const char *data, size_t len, const char *path_acp,
                                               unsigned *bundles, char *cause_out, size_t cause_cap)
{
    modexport_stats st;
    char path[MAX_PATH * 4], cause[MODEXPORT_CAUSE_MAX];
    if (bundles) *bundles = 0;
    if (cause_out && cause_cap) cause_out[0] = 0;
    if (!me_acp_to_utf8(path_acp, path, sizeof path)) {
        me_log("MODS: export: the destination path is not usable");
        if (cause_out && cause_cap) strncpy_s(cause_out, cause_cap, "the destination path is not usable", _TRUNCATE);
        return 0;
    }
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

char *modexport_read_text(const char *path_acp, size_t *out_len)
{
    return modexport_read_text_report(path_acp, out_len, NULL, 0);
}

char *modexport_read_text_report(const char *path_acp, size_t *out_len, char *cause_out, size_t cause_cap)
{
    modexport_stats st;
    char path[MAX_PATH * 4], cause[MODEXPORT_CAUSE_MAX];
    char *text, *heap;
    size_t len = 0;
    if (out_len) *out_len = 0;
    if (cause_out && cause_cap) cause_out[0] = 0;
    if (!me_acp_to_utf8(path_acp, path, sizeof path)) {
        me_log("MODS: import: the source path is not usable");
        if (cause_out && cause_cap)
            strncpy_s(cause_out, cause_cap, "its path cannot be used; move it to a folder whose name has only plain letters",
                      _TRUNCATE);
        return NULL;
    }
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

int modexport_validate_source(const char *path_acp, char *out_msg, int msg_capacity)
{
    char path[MAX_PATH * 4], cause[MODEXPORT_CAUSE_MAX];
    int ok;
    if (out_msg && msg_capacity > 0) out_msg[0] = 0;
    if (!me_acp_to_utf8(path_acp, path, sizeof path)) {
        if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity, "no path given", _TRUNCATE);
        return 0;
    }
    ok = modexport_probe(path, cause, sizeof cause);
    if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity, ok ? "ok" : cause, _TRUNCATE);
    return ok;
}
