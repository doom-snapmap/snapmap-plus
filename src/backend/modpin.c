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

/* The bytes the units carry, each file of the build once. */
static unsigned long long mpn_bytes(const sh_mb_bundle *b, const unsigned char *const *keeps, size_t n)
{
    unsigned long long total = 0;
    size_t i, k;
    for (i = 0; i < b->file_count; i++) {
        int kept = 0;
        if (b->files[i].stock || b->files[i].absent) continue;
        for (k = 0; k < n && !kept; k++) kept = keeps[k] && keeps[k][i];
        if (kept) total += b->files[i].size;
    }
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

int sh_modpin_row(sh_modmanifest_row *row, const char *map_json, size_t len, const char *content_root,
                  sh_modpin_row_result *out)
{
    sh_mb_bundle b;
    sh_unit_env env;
    sh_unit *units = NULL;
    const char **roots = NULL, **hashes = NULL;
    const unsigned char **keeps = NULL;
    char *folder = NULL, *why = NULL, *fail = NULL, *display = NULL;
    size_t n = 0, cap, i;
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
        mpn_logf("MODS: pin: %s; its row stays local", reason ? reason : "a mod");
        mpn_result(out, row, SH_MODPIN_ABSENT, 0, 0, reason);
        free(reason);
        sh_mb_free(&b);
        free(folder);
        return SH_MODPIN_ABSENT;
    }
    memset(&env, 0, sizeof env);
    env.game = g_game;
    env.game_context = g_game_ctx;
    cap = row->name_count + 1;
    units = (sh_unit *)calloc(cap, sizeof *units);
    roots = (const char **)calloc(cap, sizeof *roots);
    hashes = (const char **)calloc(cap, sizeof *hashes);
    keeps = (const unsigned char **)calloc(cap, sizeof *keeps);
    if (!units || !roots || !hashes || !keeps) goto done;
    /* Each placed object's unit; a map-level row adds the whole build. */
    for (i = 0; i < row->name_count && !whole_only; i++) {
        size_t k;
        int dup = 0;
        if (!row->names[i] || !row->names[i][0]) continue;
        if (!sh_unit_of(&b, folder, row->names[i], &env, &units[n], &why)) {
            fail = sh_textf("%s: %s", row->names[i], why ? why : "the unit did not prove");
            free(why);
            why = NULL;
            whole_only = 1;
            break;
        }
        for (k = 0; k < n && !dup; k++) dup = !strcmp(units[k].hash, units[n].hash);
        if (dup) { sh_unit_free(&units[n]); memset(&units[n], 0, sizeof units[n]); continue; }
        n++;
    }
    if (whole_only) {
        for (i = 0; i < n; i++) sh_unit_free(&units[i]);
        memset(units, 0, cap * sizeof *units);
        n = 0;
    }
    if (whole_only || row->attached || !n) {
        if (!sh_unit_of(&b, folder, SH_UNIT_WHOLE, &env, &units[n], &why)) {
            /* The whole build closes by definition (3.7): a failure here is the folder changing under the pin. */
            mpn_logf("MODS: pin: %s could not be pinned (%s); its row stays local", b.name ? b.name : row->salt,
                     why ? why : "?");
            mpn_result(out, row, SH_MODPIN_ABSENT, 0, 0, why);
            state = SH_MODPIN_ABSENT;
            goto done;
        }
        n++;
    }
    for (i = 0; i < n; i++) {
        if (!mpn_store(&units[i], content_root)) goto done;
        roots[i] = units[i].root;
        hashes[i] = units[i].hash;
        keeps[i] = units[i].keep;
    }
    display = sh_text(sh_modmanifest_row_name(row)[0] ? sh_modmanifest_row_name(row) : (b.name ? b.name : ""));
    if (!sh_modmanifest_row_pin(row, roots, hashes, n)) goto done;
    if (display && display[0]) (void)sh_modmanifest_row_set_name(row, display);
    row->size = mpn_bytes(&b, keeps, n);
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
    free((void *)keeps);
    free(fail);
    free(display);
    free(folder);
    free(why);
    sh_mb_free(&b);
    return state;
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
            /* Nothing half-pinned: every row back as it was. */
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
