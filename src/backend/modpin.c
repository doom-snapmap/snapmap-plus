/* modpin.c -- pinning a map's local rows to units (modpin.h). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modpin.h"
#include "modmanifest.h"
#include "modunit.h"
#include "modbundle.h"
#include "modcache.h"
#include "mp_json.h"
#include "modprog.h"
#include "modstock.h"
#include "backend_log.h"
#include "../common/text.h"

#ifndef SH_MODPIN_PURE
#include "modserve.h"
#include "modstore.h"
#endif

static sh_modpin_folder_fn g_folder;
static void *g_folder_ctx;
static sh_modpin_read_fn g_read;
static void *g_read_ctx;
static sh_unit_game_fn g_game = sh_modpin_game_installed;
static void *g_game_ctx;

/* The units already computed, by build and placed name: a publish computes them before its save (modpublish.c) and
 * the save pins from here at once. Process-wide, under its lock; an entry stays valid while its build does (a build is
 * named by its hash). The asked requests change a unit, so a new set of them empties it. */
typedef struct mpn_memo { char build[SH_MB_HASH_HEX + 1]; char *name; char unit[SH_MB_HASH_HEX + 1]; char *root; } mpn_memo;
static mpn_memo *g_memo;
static size_t g_nmemo, g_cmemo;
static SRWLOCK g_memo_lock = SRWLOCK_INIT;
static sh_unit_asked *g_asked;
static size_t g_nasked;

static int mpn_memo_find(const char *build, const char *name, char unit[SH_MB_HASH_HEX + 1], char **root)
{
    size_t i;
    int hit = 0;
    AcquireSRWLockShared(&g_memo_lock);
    for (i = 0; i < g_nmemo && !hit; i++)
        if (!strcmp(g_memo[i].build, build) && !_stricmp(g_memo[i].name, name)) {
            strcpy_s(unit, SH_MB_HASH_HEX + 1, g_memo[i].unit);
            *root = sh_text(g_memo[i].root);
            hit = *root != NULL;
        }
    ReleaseSRWLockShared(&g_memo_lock);
    return hit;
}

static void mpn_memo_put(const char *build, const char *name, const char *unit, const char *root)
{
    AcquireSRWLockExclusive(&g_memo_lock);
    if (g_nmemo == g_cmemo) {
        size_t c = g_cmemo ? g_cmemo * 2 : 64;
        mpn_memo *g = (mpn_memo *)realloc(g_memo, c * sizeof *g);
        if (g) { g_memo = g; g_cmemo = c; }
    }
    if (g_nmemo < g_cmemo && (g_memo[g_nmemo].name = sh_text(name)) != NULL) {
        if ((g_memo[g_nmemo].root = sh_text(root)) != NULL) {
            strcpy_s(g_memo[g_nmemo].build, sizeof g_memo[g_nmemo].build, build);
            strcpy_s(g_memo[g_nmemo].unit, sizeof g_memo[g_nmemo].unit, unit);
            g_nmemo++;
        } else free(g_memo[g_nmemo].name);
    }
    ReleaseSRWLockExclusive(&g_memo_lock);
}

void sh_modpin_set_asked(const sh_unit_asked *asked, size_t n)
{
    size_t i;
    AcquireSRWLockExclusive(&g_memo_lock);
    for (i = 0; i < g_nasked; i++) { free((void *)g_asked[i].request); free((void *)g_asked[i].by); }
    free(g_asked);
    g_asked = NULL;
    g_nasked = 0;
    if (n && (g_asked = (sh_unit_asked *)calloc(n, sizeof *g_asked)) != NULL) {
        for (i = 0; i < n; i++) {
            g_asked[i].request = asked[i].request ? sh_text(asked[i].request) : NULL;
            g_asked[i].by = asked[i].by ? sh_text(asked[i].by) : NULL;
        }
        g_nasked = n;
    }
    for (i = 0; i < g_nmemo; i++) { free(g_memo[i].name); free(g_memo[i].root); }
    g_nmemo = 0;
    ReleaseSRWLockExclusive(&g_memo_lock);
}

/* The unit of `name`: from the memo, else walked (sh_unit_of) and remembered. A hit carries the manifest from the
 * content cache and no `keep`. */
static int mpn_unit_of(const sh_mb_bundle *b, const char *folder, const char *name, const sh_unit_env *env, sh_unit *u,
                       const char *content_root, char **why)
{
    char unit[SH_MB_HASH_HEX + 1], *root = NULL;
    memset(u, 0, sizeof *u);
    if (mpn_memo_find(b->hash, name, unit, &root)) {
        size_t len = 0;
        char *manifest = NULL;
        if (content_root) {
            char *path = sh_textf("%s\\units\\%s.json", content_root, unit);
            manifest = path ? (char *)sh_mb_read_file(path, 1u << 30, &len) : NULL;
            free(path);
        } else manifest = sh_cache_manifest(unit, &len);
        if (manifest) {
            strcpy_s(u->hash, sizeof u->hash, unit);
            u->root = root;
            u->manifest = manifest;
            u->manifest_len = len;
            return 1;
        }
        free(root);
    }
    if (!sh_unit_of(b, folder, name, env, u, why)) return 0;
    mpn_memo_put(b->hash, name, u->hash, u->root);
    return 1;
}

void sh_modpin_set_folder(sh_modpin_folder_fn fn, void *context) { g_folder = fn; g_folder_ctx = context; }
void sh_modpin_set_read(sh_modpin_read_fn fn, void *context) { g_read = fn; g_read_ctx = context; }
void sh_modpin_set_game(sh_unit_game_fn fn, void *context)
{
    g_game = fn ? fn : sh_modpin_game_installed;
    g_game_ctx = fn ? context : NULL;
}

static void mpn_logf(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

int sh_modpin_game_installed(void *context, const char *request, char sha256[SH_MB_HASH_HEX + 1], char *source,
                             size_t cap)
{
    unsigned long long size = 0;
    int got;
    (void)context;
    if (sh_modprog_owns(request)) {
        unsigned char *bytes = NULL;
        size_t len = 0;
        if (sh_modprog_read(request, &bytes, &len) != SH_MB_STOCK_OK || !bytes) return -1;
        sh_mb_sha256_hex(bytes, len, sha256);
        HeapFree(GetProcessHeap(), 0, bytes);
        strcpy_s(source, cap, SH_MODPROG_CONTAINER);
        return 1;
    }
    got = sh_modstock_exists(request);
    if (got != 1) return got;
    if (sh_modstock_identify(request, sha256, &size) != SH_MB_STOCK_OK) return -1;
    if (sh_modstock_container(request, sha256, source, cap) != SH_MB_STOCK_OK) strcpy_s(source, cap, "loose");
    return 1;
}

#ifndef SH_MODPIN_PURE
/* The default folder: the build the serving index names for the row, below the mods root. */
static char *mpn_folder_served(void *context, const sh_modmanifest_row *row)
{
    sh_modserve_bundle_ref ref;
    const char *root = sh_ms_root();
    char *out = NULL;
    (void)context;
    memset(&ref, 0, sizeof ref);
    if (root && sh_modserve_row_build(row, &ref) && ref.folder && ref.folder[0] && !(ref.place && ref.place[0]))
        out = sh_textf("%s\\%s", root, ref.folder);
    sh_modserve_bundle_ref_free(&ref);
    return out;
}

static char *mpn_read_served(void *context, const char *request, size_t *len)
{
    unsigned char *bytes = NULL;
    size_t n = 0;
    char *out;
    (void)context;
    if (sh_modserve_open(request, &bytes, &n) != 1 || !bytes) return NULL;
    out = (char *)malloc(n + 1);
    if (out) { memcpy(out, bytes, n); out[n] = 0; if (len) *len = n; }
    HeapFree(GetProcessHeap(), 0, bytes);
    return out;
}
#endif

static char *mpn_folder(const sh_modmanifest_row *row)
{
    if (g_folder) return g_folder(g_folder_ctx, row);
#ifndef SH_MODPIN_PURE
    return mpn_folder_served(NULL, row);
#else
    return NULL;
#endif
}

static char *mpn_read(void *context, const char *request, size_t *len)
{
    (void)context;
    if (g_read) return g_read(g_read_ctx, request, len);
#ifndef SH_MODPIN_PURE
    return mpn_read_served(NULL, request, len);
#else
    return NULL;
#endif
}

void sh_modpin_result_free(sh_modpin_result *r)
{
    size_t i;
    if (!r) return;
    for (i = 0; i < r->count; i++) free(r->rows[i].why);
    free(r->rows);
    memset(r, 0, sizeof *r);
}

/* Write a unit's manifest where the content goes. */
static int mpn_store(const sh_unit *u, const char *content_root)
{
    char *why = NULL;
    int ok;
    if (!content_root) return sh_cache_put_manifest(u->hash, u->manifest, u->manifest_len);
    ok = sh_unit_store(u, content_root, &why);
    if (!ok) mpn_logf("MODS: pin: the manifest of %s could not be written: %s", u->root, why ? why : "?");
    free(why);
    return ok;
}

/* The bytes the units carry, each file once (by its SHA-256): what the row costs a player. */
static unsigned long long mpn_bytes(const sh_unit *units, size_t n)
{
    unsigned long long total = 0;
    char (*seen)[SH_MB_HASH_HEX + 1] = NULL;
    size_t nseen = 0, cseen = 0, i;
    int k;
    for (i = 0; i < n; i++) {
        mpj *m = mpj_parse(units[i].manifest, units[i].manifest_len);
        const mpj *files = m ? mpj_get(m, "files") : NULL;
        for (k = 0; files && files->type == MPJ_ARR && k < files->n; k++) {
            const mpj *sha = mpj_get(files->items[k], "sha256"), *size = mpj_get(files->items[k], "size");
            size_t j;
            int dup = 0;
            if (!sha || sha->type != MPJ_STR || strlen(sha->text) != SH_MB_HASH_HEX) continue;
            for (j = 0; j < nseen && !dup; j++) dup = !strcmp(seen[j], sha->text);
            if (dup) continue;
            if (nseen == cseen) {
                size_t c = cseen ? cseen * 2 : 256;
                char (*g)[SH_MB_HASH_HEX + 1] = realloc(seen, c * sizeof *g);
                if (!g) break;
                seen = g;
                cseen = c;
            }
            strcpy_s(seen[nseen++], SH_MB_HASH_HEX + 1, sha->text);
            total += (unsigned long long)mpj_int(size);
        }
        mpj_free(m);
    }
    free(seen);
    return total;
}

static void mpn_result(sh_modpin_row_result *out, const sh_modmanifest_row *row, int state, size_t units,
                       unsigned long long bytes, const char *why)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    strcpy_s(out->salt, sizeof out->salt, row->salt);
    out->state = state;
    out->units = units;
    out->bytes = bytes;
    out->why = why ? sh_text(why) : NULL;
}

static int mpn_pin_row(sh_modmanifest_row *row, const char *map_json, size_t len, const char *content_root,
                       sh_modpin_row_result *out, int publish)
{
    sh_mb_bundle b;
    sh_unit_env env;
    sh_unit *units = NULL;
    const char **roots = NULL, **hashes = NULL;
    char *folder = NULL, *why = NULL, *fail = NULL, *display = NULL;
    size_t n = 0, cap, i;
    sh_modmanifest_row unproved = {0};
    int state = -1, whole_only = 0;
    if (!row) return -1;
    if (row->pinned || !_stricmp(row->salt, SH_MODMANIFEST_PRODUCT)) {
        mpn_result(out, row, SH_MODPIN_PINNED, row->unit_count, row->size, NULL);
        return SH_MODPIN_PINNED;
    }
    memset(&b, 0, sizeof b);
    folder = mpn_folder(row);
    if (!folder || !sh_mb_load_folder(folder, &b) || !b.validated || _stricmp(b.id, row->salt)) {
        char *reason = sh_textf("%s is not on this computer as a built mod", sh_modmanifest_row_name(row)[0] ?
                                sh_modmanifest_row_name(row) : row->salt);
        mpn_logf("MODS: pin: %s", reason ? reason : "a mod is unavailable");
        mpn_result(out, row, SH_MODPIN_ABSENT, 0, 0, reason);
        free(reason);
        sh_mb_free(&b);
        free(folder);
        if (publish && !sh_modmanifest_freeze_local(row)) return -1;
        return SH_MODPIN_ABSENT;
    }
    memset(&env, 0, sizeof env);
    env.game = g_game;
    env.game_context = g_game_ctx;
    env.asked = g_asked;
    env.asked_count = g_nasked;
    cap = row->name_count + 1;
    units = (sh_unit *)calloc(cap, sizeof *units);
    roots = (const char **)calloc(cap, sizeof *roots);
    hashes = (const char **)calloc(cap, sizeof *hashes);
    if (!units || !roots || !hashes) goto done;
    /* Each placed object's unit; a map-level row adds the whole build. */
    for (i = 0; i < row->name_count; i++) {
        size_t k;
        int dup = 0;
        if (!row->names[i] || !row->names[i][0]) continue;
        if (!mpn_unit_of(&b, folder, row->names[i], &env, &units[n], content_root, &why)) {
            if (publish && units[n].absent_root) {
                if (!sh_modmanifest_row_add_missing(row, row->names[i])) goto done;
                free(why);
                why = NULL;
                continue;
            }
            if (!fail) fail = sh_textf("%s: %s", row->names[i], why ? why : "the unit did not prove");
            free(why);
            why = NULL;
            whole_only = 1;
            if (!publish) break;
            if (!sh_modmanifest_row_add_name(&unproved, row->names[i])) goto done;
            continue;
        }
        for (k = 0; k < n && !dup; k++) dup = !strcmp(units[k].hash, units[n].hash);
        if (dup) { sh_unit_free(&units[n]); memset(&units[n], 0, sizeof units[n]); continue; }
        n++;
    }
    if (whole_only || row->attached || (!n && !row->unavailable)) {
        if (!mpn_unit_of(&b, folder, SH_UNIT_WHOLE, &env, &units[n], content_root, &why)) {
            mpn_logf("MODS: pin: %s could not be pinned as a whole (%s)", b.name ? b.name : row->salt, why ? why : "?");
            if (!publish || !n) {
                if (publish && !sh_modmanifest_freeze_local(row)) goto done;
                mpn_result(out, row, SH_MODPIN_ABSENT, 0, 0, why);
                state = SH_MODPIN_ABSENT;
                goto done;
            }
            for (i = 0; i < unproved.name_count; i++)
                if (!sh_modmanifest_row_add_missing(row, unproved.names[i])) goto done;
            if (row->attached && !sh_modmanifest_row_add_missing(row, "*")) goto done;
            whole_only = 0;
        } else if (whole_only && n) {
            for (i = 0; i < n; i++) sh_unit_free(&units[i]);
            units[0] = units[n];
            memset(&units[n], 0, sizeof units[n]);
            n = 1;
        } else n++;
    }
    for (i = 0; i < n; i++) {
        if (!mpn_store(&units[i], content_root)) goto done;
        roots[i] = units[i].root;
        hashes[i] = units[i].hash;
    }
    display = sh_text(sh_modmanifest_row_name(row)[0] ? sh_modmanifest_row_name(row) : (b.name ? b.name : ""));
    if (!sh_modmanifest_row_pin(row, roots, hashes, n)) goto done;
    if (display && display[0]) (void)sh_modmanifest_row_set_name(row, display);
    row->size = mpn_bytes(units, n);
    if (map_json && len) (void)sh_cache_row_record_modules(row, map_json, len, mpn_read, NULL);
    state = whole_only ? SH_MODPIN_WHOLE : SH_MODPIN_PINNED;
    if (whole_only) mpn_logf("MODS: pin: %s's object %s did not make a unit of its own; the row carries the whole mod",
                             b.name ? b.name : row->salt, fail ? fail : "?");
    mpn_result(out, row, state, n, row->size, whole_only ? fail : NULL);
done:
    if (state < 0 && out) mpn_result(out, row, SH_MODPIN_ABSENT, 0, 0, "the content cache could not be written");
    for (i = 0; units && i < cap; i++) sh_unit_free(&units[i]);
    free(units);
    free((void *)roots);
    free((void *)hashes);
    sh_modmanifest_row_free(&unproved);
    free(fail);
    free(display);
    free(folder);
    free(why);
    sh_mb_free(&b);
    return state;
}

int sh_modpin_row(sh_modmanifest_row *row, const char *map_json, size_t len, const char *content_root,
                  sh_modpin_row_result *out)
{
    return mpn_pin_row(row, map_json, len, content_root, out, 0);
}

int sh_modpin_publish_row(sh_modmanifest_row *row, sh_modpin_row_result *out)
{
    return mpn_pin_row(row, NULL, 0, NULL, out, 1);
}

void sh_modpin_record_modules(sh_modmanifest_row *row, const char *map_json, size_t len)
{
    if (map_json && len && !row->modules)
        (void)sh_cache_row_record_modules(row, map_json, len, mpn_read, NULL);
}

int sh_modpin_row_differs(const sh_modmanifest_row *row)
{
    sh_modmanifest_row local;
    sh_mb_bundle b;
    sh_unit_env env;
    char *folder;
    size_t k;
    int differs = 0;
    if (!row || !row->pinned || (!row->unit_count && !row->unavailable)) return -1;
    /* The installed version: the row as a local row of its salt resolves to the root folder. */
    memset(&local, 0, sizeof local);
    strcpy_s(local.salt, sizeof local.salt, row->salt);
    memset(local.build, '0', SH_MOD_HASH_CHARS);
    local.build[SH_MOD_HASH_CHARS] = 0;
    memset(&b, 0, sizeof b);
    folder = mpn_folder(&local);
    if (!folder || !sh_mb_load_folder(folder, &b) || !b.validated || _stricmp(b.id, row->salt)) {
        sh_mb_free(&b);
        free(folder);
        return -1;
    }
    memset(&env, 0, sizeof env);
    env.game = g_game;
    env.game_context = g_game_ctx;
    env.asked = g_asked;
    env.asked_count = g_nasked;
    if (row->unavailable && !row->missing_count) differs = 1;
    for (k = 0; k < row->missing_count && !differs; k++) {
        sh_unit u = {0};
        char *why = NULL;
        differs = !strcmp(row->missing[k], SH_UNIT_WHOLE) ||
                  mpn_unit_of(&b, folder, row->missing[k], &env, &u, NULL, &why);
        sh_unit_free(&u);
        free(why);
    }
    for (k = 0; k < row->unit_count && !differs; k++) {
        char unit[SH_MB_HASH_HEX + 1], *root = NULL, *why = NULL;
        const char *name = row->roots && row->roots[k] ? row->roots[k] : SH_UNIT_WHOLE;
        if (mpn_memo_find(b.hash, name, unit, &root)) {
            free(root);
            differs = strcmp(unit, row->units[k]) != 0;
            continue;
        }
        {
            sh_unit u;
            memset(&u, 0, sizeof u);
            if (!sh_unit_of(&b, folder, name, &env, &u, &why)) differs = 1;   /* the installed build lacks it */
            else {
                mpn_memo_put(b.hash, name, u.hash, u.root);
                differs = strcmp(u.hash, row->units[k]) != 0;
            }
            sh_unit_free(&u);
            free(why);
        }
    }
    sh_mb_free(&b);
    free(folder);
    return differs;
}

int sh_modpin_rows(sh_modmanifest_list *rows, const char *map_json, size_t len, const char *content_root,
                   sh_modpin_result *out)
{
    sh_modmanifest_list before;
    size_t i;
    int pinned = 0;
    if (out) memset(out, 0, sizeof *out);
    if (!rows) return -1;
    memset(&before, 0, sizeof before);
    before.uses = rows->uses;
    before.earlier = rows->earlier;
    for (i = 0; i < rows->count; i++)
        if (!sh_modmanifest_list_add(&before, &rows->rows[i])) { sh_modmanifest_list_free(&before); return -1; }
    if (out && rows->count && !(out->rows = (sh_modpin_row_result *)calloc(rows->count, sizeof *out->rows))) {
        sh_modmanifest_list_free(&before);
        return -1;
    }
    for (i = 0; i < rows->count; i++) {
        sh_modpin_row_result r;
        int s;
        if (rows->rows[i].pinned || !_stricmp(rows->rows[i].salt, SH_MODMANIFEST_PRODUCT)) continue;
        memset(&r, 0, sizeof r);
        s = sh_modpin_row(&rows->rows[i], map_json, len, content_root, &r);
        if (s < 0) {
            /* Nothing half-pinned: every row is left unchanged. */
            size_t k;
            free(r.why);
            for (k = 0; k < rows->count; k++) sh_modmanifest_row_free(&rows->rows[k]);
            free(rows->rows);
            *rows = before;
            if (out) sh_modpin_result_free(out);
            return -1;
        }
        if (s == SH_MODPIN_PINNED || s == SH_MODPIN_WHOLE) pinned++;
        if (out) out->rows[out->count++] = r;
        else free(r.why);
    }
    sh_modmanifest_list_free(&before);
    return pinned;
}
