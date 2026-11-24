/* modtypeinfo.h -- default trees for the entity types installed mods register after the game built its
 * idSnapTypeInfoCache (one tree per entityDef, capacity MAX_ENT_TYPES 1,499). Get and its lookup sibling are
 * detoured: a name the game's cache holds is served by the game, any other from a product table built on first
 * use, so the editor does not read a zero model scale ("modelMatrix invert failed") for a mod's entity.
 * The pure part (table, cache search, view) is tested with buffers; the rest binds it to the engine. */
#ifndef BACKEND_MODTYPEINFO_H
#define BACKEND_MODTYPEINFO_H

#include <stddef.h>
#include <stdint.h>

/* idSnapTypeInfoCache on build 13954591, both renderers: int count at +0,
 * MAX_ENT_TYPES keys of 8 bytes at +8 sorted by value, then one 0x48-byte
 * tree per entry at +0x2ee8 in key order. */
#define SH_TYPEINFO_COUNT_OFF   0x0
#define SH_TYPEINFO_KEYS_OFF    0x8
#define SH_TYPEINFO_TREES_OFF   0x2ee8
#define SH_TYPEINFO_TREE_SIZE   0x48
#define SH_TYPEINFO_MAX_TYPES   0x5db   /* MAX_ENT_TYPES: the most entries the cache holds */
#define SH_TYPEINFO_ROW_LIMIT   0x5dc   /* the game's builder stops the game at this many rows */
#define SH_TYPEINFO_CACHE_BYTES (SH_TYPEINFO_TREES_OFF + SH_TYPEINFO_MAX_TYPES * SH_TYPEINFO_TREE_SIZE)
/* A one-entry cache: what Get reads for a hit at index 0. */
#define SH_TYPEINFO_VIEW_BYTES  (SH_TYPEINFO_TREES_OFF + SH_TYPEINFO_TREE_SIZE)

/* The game cache's own search (lower bound by key value): the entry index,
 * or -1 when the key is absent or the count is not a cache count. */
int sh_typeinfo_cache_index(const void *cache, const void *key);

/* Fill `view` (SH_TYPEINFO_VIEW_BYTES) as a cache holding only `key` with a
 * copy of `tree` (SH_TYPEINFO_TREE_SIZE bytes) at index 0. */
void sh_typeinfo_view_fill(void *view, const void *key, const void *tree);

/* The product's table: entries sorted by key value, one tree each. */
typedef struct sh_typeinfo_entry {
    const void *key;    /* the pooled entityDef name (a C string the game interned) */
    void *tree;         /* the product-built default tree, SH_TYPEINFO_TREE_SIZE bytes */
} sh_typeinfo_entry;

typedef struct sh_typeinfo_table {
    sh_typeinfo_entry *entries;
    int count, capacity;
} sh_typeinfo_table;

void *sh_typeinfo_table_find(const sh_typeinfo_table *t, const void *key);
/* Insert keeping key order. 0 when the key is present already or memory is out. */
int sh_typeinfo_table_insert(sh_typeinfo_table *t, const void *key, void *tree);
/* Remove one key; returns its tree (the caller destroys it) or NULL. */
void *sh_typeinfo_table_remove(sh_typeinfo_table *t, const void *key);
/* Forget every entry whose key names `name` (case-insensitive, the engine's
 * decl-name rule). The trees are never destroyed: the lookup handed the
 * engine the tree itself, and the engine may still hold it. Returns the count. */
int sh_typeinfo_table_forget_name(sh_typeinfo_table *t, const char *name);
void sh_typeinfo_table_free(sh_typeinfo_table *t, void (*destroy)(void *context, void *tree), void *context);

/* 1 when the boot self-check can run: the game's cache has entries and the game object the product builds a
 * type through exists; otherwise it is asked again. Pure. */
int sh_typeinfo_check_due(int cache_count, int max_types, int game_ready);

#ifndef SH_MODTYPEINFO_PURE
struct sig_result;
/* Resolve the engine and install the two detours. Returns 1 when they are live; else logs one plain line and
 * the editor keeps its own behavior. */
int sh_modtypeinfo_install(const struct sig_result *results, size_t count, const uint8_t *module_base);
/* After a palette build, on the main thread: once per boot, checks the product's builder against the game's cache
 * on stock types (the detours serve only after it passes) and logs how many mod rows resolve through the table. */
void sh_modtypeinfo_after_palette(void *palette);
/* A decl's served bytes changed at a refresh: forget its tree. */
void sh_modtypeinfo_decl_changed(const char *type, const char *name);
#endif

#endif /* BACKEND_MODTYPEINFO_H */
