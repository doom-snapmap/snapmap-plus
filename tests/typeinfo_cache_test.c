/* typeinfo_cache_test.c -- the pure part of modtypeinfo.c: the game cache's
 * search on a buffer shaped like idSnapTypeInfoCache, the product's table
 * (insert keeps key order, lookup, remove by key and by name, free) and the
 * one-entry view Get runs on. Built with /DSH_MODTYPEINFO_PURE: no engine
 * binding. */
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

static const void *g_destroyed[8];
static int g_ndestroyed;
static void note_destroy(void *context, void *tree)
{
    (void)context;
    if (g_ndestroyed < 8) g_destroyed[g_ndestroyed] = tree;
    g_ndestroyed++;
}

int main(void)
{
    /* Keys are pointers the game interned; sort order is pointer order. */
    static char pool[8][32] = { "zeta", "alpha", "mods/x/one", "MODS/X/ONE", "beta", "mods/x/two", "gamma", "delta" };
    const void *k0 = pool[0], *k1 = pool[1], *k2 = pool[2], *k3 = pool[3], *k4 = pool[4], *k5 = pool[5];
    unsigned char *cache = (unsigned char *)calloc(1, SH_TYPEINFO_CACHE_BYTES);
    unsigned char *view = (unsigned char *)malloc(SH_TYPEINFO_VIEW_BYTES);
    unsigned char trees[6][SH_TYPEINFO_TREE_SIZE];
    sh_typeinfo_table t;
    const void **keys;
    int i;

    CHECK(cache && view);
    if (!cache || !view) return 1;
    for (i = 0; i < 6; i++) memset(trees[i], 0x10 + i, SH_TYPEINFO_TREE_SIZE);

    /* The game's cache: keys sorted by value. Absent, present, out of range. */
    keys = (const void **)(cache + SH_TYPEINFO_KEYS_OFF);
    keys[0] = pool[1]; keys[1] = pool[4]; keys[2] = pool[7];   /* ascending addresses */
    *(int *)(cache + SH_TYPEINFO_COUNT_OFF) = 3;
    CHECK(sh_typeinfo_cache_index(cache, pool[1]) == 0);
    CHECK(sh_typeinfo_cache_index(cache, pool[4]) == 1);
    CHECK(sh_typeinfo_cache_index(cache, pool[7]) == 2);
    CHECK(sh_typeinfo_cache_index(cache, pool[0]) == -1);
    CHECK(sh_typeinfo_cache_index(cache, pool[2]) == -1);
    CHECK(sh_typeinfo_cache_index(cache, NULL) == -1);
    CHECK(sh_typeinfo_cache_index(NULL, pool[1]) == -1);
    *(int *)(cache + SH_TYPEINFO_COUNT_OFF) = 0;
    CHECK(sh_typeinfo_cache_index(cache, pool[1]) == -1);
    *(int *)(cache + SH_TYPEINFO_COUNT_OFF) = SH_TYPEINFO_MAX_TYPES + 1;
    CHECK(sh_typeinfo_cache_index(cache, pool[1]) == -1);
    *(int *)(cache + SH_TYPEINFO_COUNT_OFF) = 1;
    CHECK(sh_typeinfo_cache_index(cache, pool[1]) == 0 && sh_typeinfo_cache_index(cache, pool[4]) == -1);

    /* The view: a one-entry cache the game's search hits at index 0. */
    memset(view, 0xee, SH_TYPEINFO_VIEW_BYTES);
    sh_typeinfo_view_fill(view, k2, trees[2]);
    CHECK(*(int *)(view + SH_TYPEINFO_COUNT_OFF) == 1);
    CHECK(sh_typeinfo_cache_index(view, k2) == 0);
    CHECK(sh_typeinfo_cache_index(view, k1) == -1);
    CHECK(!memcmp(view + SH_TYPEINFO_TREES_OFF, trees[2], SH_TYPEINFO_TREE_SIZE));

    /* The table: empty, insert out of order, kept in key order, no duplicates. */
    memset(&t, 0, sizeof t);
    CHECK(sh_typeinfo_table_find(&t, k2) == NULL);
    CHECK(sh_typeinfo_table_remove(&t, k2) == NULL);
    CHECK(sh_typeinfo_table_insert(&t, k5, trees[5]) == 1);
    CHECK(sh_typeinfo_table_insert(&t, k2, trees[2]) == 1);
    CHECK(sh_typeinfo_table_insert(&t, k3, trees[3]) == 1);
    CHECK(sh_typeinfo_table_insert(&t, k0, trees[0]) == 1);
    CHECK(sh_typeinfo_table_insert(&t, k2, trees[1]) == 0);   /* present already */
    CHECK(sh_typeinfo_table_insert(&t, NULL, trees[1]) == 0);
    CHECK(sh_typeinfo_table_insert(&t, k1, NULL) == 0);
    CHECK(t.count == 4);
    for (i = 1; i < t.count; i++) CHECK((const char *)t.entries[i - 1].key < (const char *)t.entries[i].key);
    CHECK(sh_typeinfo_table_find(&t, k0) == trees[0]);
    CHECK(sh_typeinfo_table_find(&t, k2) == trees[2]);
    CHECK(sh_typeinfo_table_find(&t, k3) == trees[3]);
    CHECK(sh_typeinfo_table_find(&t, k5) == trees[5]);
    CHECK(sh_typeinfo_table_find(&t, k1) == NULL);
    CHECK(sh_typeinfo_table_find(&t, k4) == NULL);

    /* Growth past the first allocation keeps order and every entry. */
    {
        static char many[200][8];
        int n;
        for (n = 0; n < 200; n++) {
            sprintf(many[199 - n], "k%d", n);
            CHECK(sh_typeinfo_table_insert(&t, many[199 - n], trees[n % 6]) == 1);
        }
        CHECK(t.count == 204);
        for (i = 1; i < t.count; i++) CHECK((const char *)t.entries[i - 1].key < (const char *)t.entries[i].key);
        for (n = 0; n < 200; n++) CHECK(sh_typeinfo_table_find(&t, many[n]) == trees[(199 - n) % 6]);
        for (n = 0; n < 200; n++) CHECK(sh_typeinfo_table_remove(&t, many[n]) == trees[(199 - n) % 6]);
        CHECK(t.count == 4);
    }

    /* Remove by key hands the tree back and keeps the rest in order. */
    CHECK(sh_typeinfo_table_remove(&t, k3) == trees[3]);
    CHECK(sh_typeinfo_table_remove(&t, k3) == NULL);
    CHECK(t.count == 3 && sh_typeinfo_table_find(&t, k3) == NULL);
    CHECK(sh_typeinfo_table_find(&t, k0) == trees[0] && sh_typeinfo_table_find(&t, k5) == trees[5]);

    /* Remove by name: the decl-name rule ignores case; every match goes. */
    CHECK(sh_typeinfo_table_insert(&t, k3, trees[3]) == 1);
    g_ndestroyed = 0;
    CHECK(sh_typeinfo_table_remove_name(&t, "Mods/X/One", note_destroy, NULL) == 2);
    CHECK(g_ndestroyed == 2);
    CHECK((g_destroyed[0] == trees[2] && g_destroyed[1] == trees[3]) || (g_destroyed[0] == trees[3] && g_destroyed[1] == trees[2]));
    CHECK(t.count == 2 && sh_typeinfo_table_find(&t, k2) == NULL && sh_typeinfo_table_find(&t, k3) == NULL);
    CHECK(sh_typeinfo_table_remove_name(&t, "nobody", note_destroy, NULL) == 0);
    CHECK(sh_typeinfo_table_remove_name(&t, "", note_destroy, NULL) == 0);
    CHECK(sh_typeinfo_table_remove_name(&t, NULL, note_destroy, NULL) == 0);
    CHECK(g_ndestroyed == 2);

    /* Free destroys what is left. */
    g_ndestroyed = 0;
    sh_typeinfo_table_free(&t, note_destroy, NULL);
    CHECK(g_ndestroyed == 2 && t.count == 0 && t.entries == NULL);
    sh_typeinfo_table_free(&t, note_destroy, NULL);
    CHECK(g_ndestroyed == 2);

    free(cache);
    free(view);
    if (g_failed) { fprintf(stderr, "typeinfo_cache_test: %d failure(s)\n", g_failed); return 1; }
    printf("typeinfo_cache_test: ok\n");
    return 0;
}
