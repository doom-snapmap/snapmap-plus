/* modtypeinfo.c -- see modtypeinfo.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modtypeinfo.h"

static int ti_has(const unsigned char *cache, int n, const void *key)
{
    int i;
    const void *const *keys = (const void *const *)(cache + SH_TYPEINFO_KEYS_OFF);
    for (i = 0; i < n; i++) if (keys[i] == key) return 1;
    return 0;
}

static int ti_missing(const unsigned char *cache, int n, const void *const *keys, int nkeys)
{
    int i, missing = 0;
    for (i = 0; i < nkeys; i++) if (keys[i] && !ti_has(cache, n, keys[i])) missing++;
    return missing;
}

int sh_typeinfo_cache_sync(void *cache, const void *const *keys, int nkeys, int rows,
                           const sh_typeinfo_ops *ops, sh_typeinfo_report *out)
{
    unsigned char *c = (unsigned char *)cache;
    sh_typeinfo_report r;
    int i, n;
    memset(&r, 0, sizeof r);
    if (out) *out = r;
    if (!c || !ops || !ops->destroy_tree || !ops->build) return SH_TYPEINFO_BAD;
    n = *(const int *)(c + SH_TYPEINFO_COUNT_OFF);
    r.entries = r.entries_after = n;
    r.wanted = nkeys > 0 ? nkeys : 0;
    if (out) *out = r;
    if (n < 0 || n > SH_TYPEINFO_MAX_TYPES) return SH_TYPEINFO_BAD;
    if (n == 0) return SH_TYPEINFO_EMPTY;
    r.missing = r.missing_after = ti_missing(c, n, keys, nkeys);
    if (out) *out = r;
    if (!r.missing) return SH_TYPEINFO_COMPLETE;
    if (rows >= SH_TYPEINFO_ROW_LIMIT) return SH_TYPEINFO_LIMIT;
    /* The game's own order: the cached trees go, the count is zero, Build
     * reads every registered row. The trees hold no pointers into each other
     * and no live entity keeps a pointer to one (every reader copies). */
    for (i = 0; i < n; i++) ops->destroy_tree(c + SH_TYPEINFO_TREES_OFF + (size_t)i * SH_TYPEINFO_TREE_SIZE);
    *(int *)(c + SH_TYPEINFO_COUNT_OFF) = 0;
    ops->build();
    n = *(const int *)(c + SH_TYPEINFO_COUNT_OFF);
    r.entries_after = n;
    r.missing_after = (n > 0 && n <= SH_TYPEINFO_MAX_TYPES) ? ti_missing(c, n, keys, nkeys) : r.missing;
    if (out) *out = r;
    return SH_TYPEINFO_REBUILT;
}

#ifndef SH_MODTYPEINFO_PURE
#include "signatures.h"
#include "engine_globals.h"
#include "backend_log.h"
#include "modserve.h"

/* The entity palette (editor+0x20660) as modserve_engine.c and timeline_tile.c
 * read it: entries at +0x10 of 0x6f0 bytes, count at +0x18; each entry starts
 * with its sedef, whose name is at +8 and whose entityDef resource at +0x1c8;
 * the cache key is that resource's name at +8. */
#define PAL_ENTRIES_OFF     0x10
#define PAL_COUNT_OFF       0x18
#define PAL_STRIDE          0x6f0
#define PAL_MAX             100000
#define SEDEF_NAME_OFF      0x08
#define SEDEF_ENTITYDEF_OFF 0x1c8
#define RESOURCE_NAME_OFF   0x08
#define TI_KEYS_MAX         SH_TYPEINFO_MAX_TYPES

typedef void (*ti_destroy_fn)(void *);
typedef void (*ti_build_fn)(void *);

static unsigned char *g_cache;
static const int *g_rows;
static ti_destroy_fn g_destroy;
static ti_build_fn g_build;
static int g_available;

static int ti_rd_ptr(const void *at, void **out)
{
    __try { *out = *(void *const *)at; return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out = NULL; return 0; }
}

static void ti_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static void ti_destroy_tree(void *tree) { g_destroy(tree); }
static void ti_build(void) { g_build(NULL); }   /* the wrapper ignores its argument */

int sh_modtypeinfo_install(const struct sig_result *results, size_t count, const uint8_t *module_base)
{
    g_cache = (unsigned char *)glb_resolve(module_base, "snap_typeinfo_cache", NULL);
    g_rows = (const int *)glb_resolve(module_base, "snap_editor_row_count", NULL);
    g_build = (ti_build_fn)sig_addr_by_name(results, count, "SnapTypeInfoCacheBuild");
    g_destroy = (ti_destroy_fn)sig_addr_by_name(results, count, "GridTreeDestroy");
    g_available = g_cache && g_rows && g_build && g_destroy;
    if (!g_available)
        ti_log("MODS: typeinfo cache sync unavailable (%s unresolved); a mod's rows placed before a restart may "
               "log 'modelMatrix invert failed' while hovered",
               !g_cache ? "the cache" : !g_rows ? "the row count" : !g_build ? "SnapTypeInfoCacheBuild" : "GridTreeDestroy");
    return g_available;
}

int sh_modtypeinfo_sync(void *palette, const char *phase)
{
    const void **keys;
    void *entries = NULL;
    int count = 0, i, nkeys = 0, rows = 0, result;
    sh_typeinfo_ops ops;
    sh_typeinfo_report rep;
    if (!g_available || !palette) return SH_TYPEINFO_BAD;
    if (!ti_rd_ptr((unsigned char *)palette + PAL_ENTRIES_OFF, &entries)) return SH_TYPEINFO_BAD;
    __try { count = *(const int *)((unsigned char *)palette + PAL_COUNT_OFF); rows = *g_rows; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return SH_TYPEINFO_BAD; }
    if (!entries || count <= 0 || count > PAL_MAX) return SH_TYPEINFO_BAD;
    keys = (const void **)calloc(TI_KEYS_MAX, sizeof *keys);
    if (!keys) return SH_TYPEINFO_BAD;
    for (i = 0; i < count && nkeys < TI_KEYS_MAX; ++i) {
        void *sedef = NULL, *name = NULL, *resource = NULL, *key = NULL;
        const unsigned char *entry = (const unsigned char *)entries + (size_t)i * PAL_STRIDE;
        int is_mod = 0;
        if (!ti_rd_ptr(entry, &sedef) || !sedef || !ti_rd_ptr((unsigned char *)sedef + SEDEF_NAME_OFF, &name) || !name)
            continue;
        __try { is_mod = sh_modserve_is_mod_sedef((const char *)name); }
        __except (EXCEPTION_EXECUTE_HANDLER) { is_mod = 0; }
        if (!is_mod) continue;
        if (!ti_rd_ptr((unsigned char *)sedef + SEDEF_ENTITYDEF_OFF, &resource) || !resource) continue;
        if (!ti_rd_ptr((unsigned char *)resource + RESOURCE_NAME_OFF, &key) || !key) continue;
        keys[nkeys++] = key;
    }
    ops.destroy_tree = ti_destroy_tree;
    ops.build = ti_build;
    result = sh_typeinfo_cache_sync(g_cache, keys, nkeys, rows, &ops, &rep);
    free(keys);
    switch (result) {
    case SH_TYPEINFO_REBUILT:
        ti_log("MODS: typeinfo cache: %d of %d mod rows were not in the game's cache of %d entity types at the %s; "
               "rebuilt from %d rows: %d types, %d still missing", rep.missing, rep.wanted, rep.entries, phase,
               rows, rep.entries_after, rep.missing_after);
        break;
    case SH_TYPEINFO_LIMIT:
        ti_log("MODS: typeinfo cache: %d of %d mod rows are not in the game's cache, but the editor holds at most %d "
               "entity types and %d are registered; uninstall a mod with editor rows and restart", rep.missing,
               rep.wanted, SH_TYPEINFO_ROW_LIMIT - 1, rows);
        break;
    case SH_TYPEINFO_BAD:
        ti_log("MODS: typeinfo cache: not a cache count (%d) at the %s; left alone", rep.entries, phase);
        break;
    default:
        break;   /* complete, or not built yet: the game's build reads the rows */
    }
    return result;
}
#endif
