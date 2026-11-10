/* modtypeinfo.h -- the editor's entity type-info cache (idSnapTypeInfoCache)
 * and the rows installed mods register after the game built it.
 *
 * The game builds the cache once per process, from every snapEditorEntityDef
 * decl registered at that moment: one default property tree per entityDef,
 * keyed by the entityDef's name (the pooled string pointer at resource+8).
 * Every placed entity's property tree starts as a copy of its type's cached
 * tree. A type the cache lacks gets a tree built from the instance alone
 * (the game logs "idSnapTypeInfoCache::Get parent tree not in cache <name>"
 * once), so the editor reads its model scale as zero -- "modelMatrix invert
 * failed on model <model>" every frame the entity is drawn on its own -- and
 * shows it as hidden. Rows a mod registers after the build are such types.
 *
 * sh_typeinfo_cache_sync compares the cache with the keys it must hold and,
 * when one is missing, rebuilds it the way the game does: every cached tree
 * destroyed, the count zeroed, the game's own builder run. Pure: the engine
 * is reached through the two callbacks, so a test drives it with a buffer.
 * sh_modtypeinfo_sync binds it to the live palette and the resolved engine. */
#ifndef BACKEND_MODTYPEINFO_H
#define BACKEND_MODTYPEINFO_H

#include <stddef.h>
#include <stdint.h>

/* idSnapTypeInfoCache on build 13954591, both renderers: int count at +0,
 * MAX_ENT_TYPES keys of 8 bytes at +8, then one 0x48-byte tree per entry at
 * +0x2ee8 (the keys are sorted by value; the trees follow the key order). The
 * builder is fatal when the registered row count reaches 0x5dc. */
#define SH_TYPEINFO_COUNT_OFF   0x0
#define SH_TYPEINFO_KEYS_OFF    0x8
#define SH_TYPEINFO_TREES_OFF   0x2ee8
#define SH_TYPEINFO_TREE_SIZE   0x48
#define SH_TYPEINFO_MAX_TYPES   0x5db   /* MAX_ENT_TYPES: the most entries the cache holds */
#define SH_TYPEINFO_ROW_LIMIT   0x5dc   /* the builder stops the game at this many rows */
#define SH_TYPEINFO_CACHE_BYTES (SH_TYPEINFO_TREES_OFF + SH_TYPEINFO_MAX_TYPES * SH_TYPEINFO_TREE_SIZE)

typedef struct sh_typeinfo_ops {
    void (*destroy_tree)(void *tree);   /* the engine's tree destructor, on one cached tree */
    void (*build)(void);                /* the engine's builder (begin, Build, end), on an empty cache */
} sh_typeinfo_ops;

typedef struct sh_typeinfo_report {
    int entries;        /* cache entries before */
    int wanted;         /* keys checked */
    int missing;        /* of them, absent before */
    int entries_after;  /* cache entries after a rebuild (else entries) */
    int missing_after;  /* keys still absent after a rebuild (else missing) */
} sh_typeinfo_report;

#define SH_TYPEINFO_COMPLETE  0   /* every key present (or none wanted); nothing done */
#define SH_TYPEINFO_REBUILT   1   /* a key was missing; the cache was rebuilt */
#define SH_TYPEINFO_EMPTY     2   /* the game has not built it yet; its build will see the rows */
#define SH_TYPEINFO_BAD      -1   /* the count is not a cache count; nothing touched */
#define SH_TYPEINFO_LIMIT    -2   /* a key is missing but the row count would stop the game */

/* Compare the cache's keys with `keys` and rebuild on a miss. `rows` is the
 * number of snapEditorEntityDef decls registered (the builder's input), read
 * by the caller. Fills `out` when given. */
int sh_typeinfo_cache_sync(void *cache, const void *const *keys, int nkeys, int rows,
                           const sh_typeinfo_ops *ops, sh_typeinfo_report *out);

#ifndef SH_MODTYPEINFO_PURE
struct sig_result;
/* Resolve the engine's cache, builder, tree destructor and row count. Returns
 * 1 when every part resolved; else logs once and the sync is unavailable. */
int sh_modtypeinfo_install(const struct sig_result *results, size_t count, const uint8_t *module_base);
/* After a palette build, on the main thread: the keys of every palette row a
 * mod registered are checked against the cache and it is rebuilt on a miss.
 * Logs one "MODS: typeinfo cache: ..." line when something was found or done.
 * Returns the sync result, or SH_TYPEINFO_BAD when unavailable. */
int sh_modtypeinfo_sync(void *palette, const char *phase);
#endif

#endif /* BACKEND_MODTYPEINFO_H */
