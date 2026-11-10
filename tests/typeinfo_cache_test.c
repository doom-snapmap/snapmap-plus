/* typeinfo_cache_test.c -- sh_typeinfo_cache_sync against a buffer shaped
 * like idSnapTypeInfoCache: nothing to do while the game has not built it or
 * every mod row's key is cached; on a miss every cached tree is destroyed in
 * order, the count is zero when the builder runs, and the result is reported;
 * a row count at the game's limit refuses; a count that is not a cache count
 * touches nothing. Built with /DSH_MODTYPEINFO_PURE: no engine binding. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/modtypeinfo.h"

static int g_failed;
#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

static unsigned char *g_cache;
static const void *g_destroyed[8];
static int g_ndestroyed, g_builds, g_count_at_build;
static const void *g_build_keys[8];
static int g_build_nkeys;

static void fake_destroy(void *tree) { if (g_ndestroyed < 8) g_destroyed[g_ndestroyed] = tree; g_ndestroyed++; }

/* The engine's builder as seen from outside: it fills an empty cache with the
 * keys of every registered row. */
static void fake_build(void)
{
    const void **keys = (const void **)(g_cache + SH_TYPEINFO_KEYS_OFF);
    int i;
    g_builds++;
    g_count_at_build = *(int *)(g_cache + SH_TYPEINFO_COUNT_OFF);
    for (i = 0; i < g_build_nkeys; i++) keys[i] = g_build_keys[i];
    *(int *)(g_cache + SH_TYPEINFO_COUNT_OFF) = g_build_nkeys;
}

static void fill(const void *const *keys, int n)
{
    const void **k = (const void **)(g_cache + SH_TYPEINFO_KEYS_OFF);
    int i;
    memset(g_cache, 0, SH_TYPEINFO_CACHE_BYTES);
    for (i = 0; i < n; i++) k[i] = keys[i];
    *(int *)(g_cache + SH_TYPEINFO_COUNT_OFF) = n;
    g_ndestroyed = g_builds = g_count_at_build = 0;
}

int main(void)
{
    static const char a[] = "a", b[] = "b", c[] = "c", m[] = "mods/x/one", m2[] = "mods/x/two";
    const void *stock[3] = { a, b, c };
    const void *want[2] = { m, m2 };
    const void *want_stock[2] = { b, a };
    sh_typeinfo_ops ops = { fake_destroy, fake_build };
    sh_typeinfo_report rep;
    int r;

    g_cache = (unsigned char *)malloc(SH_TYPEINFO_CACHE_BYTES);
    CHECK(g_cache != NULL);
    if (!g_cache) return 1;

    /* Not built yet: the game's own build will read the rows. */
    fill(stock, 0);
    r = sh_typeinfo_cache_sync(g_cache, want, 2, 10, &ops, &rep);
    CHECK(r == SH_TYPEINFO_EMPTY);
    CHECK(g_ndestroyed == 0 && g_builds == 0);
    CHECK(rep.entries == 0 && rep.wanted == 2);

    /* Every key cached (order free): nothing to do. */
    fill(stock, 3);
    r = sh_typeinfo_cache_sync(g_cache, want_stock, 2, 10, &ops, &rep);
    CHECK(r == SH_TYPEINFO_COMPLETE);
    CHECK(g_ndestroyed == 0 && g_builds == 0);
    CHECK(rep.entries == 3 && rep.wanted == 2 && rep.missing == 0);

    /* No keys wanted: complete. */
    fill(stock, 3);
    r = sh_typeinfo_cache_sync(g_cache, NULL, 0, 10, &ops, &rep);
    CHECK(r == SH_TYPEINFO_COMPLETE);
    CHECK(g_ndestroyed == 0 && g_builds == 0 && rep.wanted == 0);

    /* One mod row missing: every cached tree destroyed in order, the count
     * zero when the builder runs, the rebuilt cache reported. */
    fill(stock, 3);
    g_build_keys[0] = a; g_build_keys[1] = b; g_build_keys[2] = c; g_build_keys[3] = m; g_build_keys[4] = m2;
    g_build_nkeys = 5;
    r = sh_typeinfo_cache_sync(g_cache, want, 2, 1495, &ops, &rep);
    CHECK(r == SH_TYPEINFO_REBUILT);
    CHECK(g_ndestroyed == 3);
    CHECK(g_destroyed[0] == g_cache + SH_TYPEINFO_TREES_OFF);
    CHECK(g_destroyed[1] == g_cache + SH_TYPEINFO_TREES_OFF + SH_TYPEINFO_TREE_SIZE);
    CHECK(g_destroyed[2] == g_cache + SH_TYPEINFO_TREES_OFF + 2 * SH_TYPEINFO_TREE_SIZE);
    CHECK(g_builds == 1 && g_count_at_build == 0);
    CHECK(rep.entries == 3 && rep.missing == 2 && rep.entries_after == 5 && rep.missing_after == 0);
    CHECK(*(int *)(g_cache + SH_TYPEINFO_COUNT_OFF) == 5);

    /* A builder that still lacks one key reports it. */
    fill(stock, 3);
    g_build_nkeys = 4;   /* a, b, c, m */
    r = sh_typeinfo_cache_sync(g_cache, want, 2, 10, &ops, &rep);
    CHECK(r == SH_TYPEINFO_REBUILT);
    CHECK(rep.entries_after == 4 && rep.missing_after == 1);

    /* At the game's limit the rebuild would stop the game: refused, untouched. */
    fill(stock, 3);
    r = sh_typeinfo_cache_sync(g_cache, want, 2, SH_TYPEINFO_ROW_LIMIT, &ops, &rep);
    CHECK(r == SH_TYPEINFO_LIMIT);
    CHECK(g_ndestroyed == 0 && g_builds == 0);
    CHECK(rep.missing == 2 && *(int *)(g_cache + SH_TYPEINFO_COUNT_OFF) == 3);
    fill(stock, 3);
    r = sh_typeinfo_cache_sync(g_cache, want, 2, SH_TYPEINFO_ROW_LIMIT - 1, &ops, &rep);
    CHECK(r == SH_TYPEINFO_REBUILT);

    /* Not a cache count: nothing touched. */
    fill(stock, 3);
    *(int *)(g_cache + SH_TYPEINFO_COUNT_OFF) = SH_TYPEINFO_MAX_TYPES + 1;
    r = sh_typeinfo_cache_sync(g_cache, want, 2, 10, &ops, &rep);
    CHECK(r == SH_TYPEINFO_BAD);
    CHECK(g_ndestroyed == 0 && g_builds == 0);
    *(int *)(g_cache + SH_TYPEINFO_COUNT_OFF) = -1;
    r = sh_typeinfo_cache_sync(g_cache, want, 2, 10, &ops, &rep);
    CHECK(r == SH_TYPEINFO_BAD);

    /* No cache or no engine: refused. */
    CHECK(sh_typeinfo_cache_sync(NULL, want, 2, 10, &ops, &rep) == SH_TYPEINFO_BAD);
    CHECK(sh_typeinfo_cache_sync(g_cache, want, 2, 10, NULL, &rep) == SH_TYPEINFO_BAD);

    free(g_cache);
    if (g_failed) { fprintf(stderr, "typeinfo_cache_test: %d failure(s)\n", g_failed); return 1; }
    printf("typeinfo_cache_test: ok\n");
    return 0;
}
