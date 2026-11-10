/* modtypeinfo.h -- the editor's entity type-info cache (idSnapTypeInfoCache)
 * and the default trees of the rows installed mods register after the game
 * built it.
 *
 * The game builds the cache once per process, from every snapEditorEntityDef
 * decl registered at that moment: one default property tree per entityDef,
 * keyed by the entityDef's name (the pooled string pointer at resource+8),
 * capacity MAX_ENT_TYPES (1,499; its builder stops the game at 1,500 rows).
 * Every placed entity's property tree starts as a copy of its type's cached
 * tree (idSnapTypeInfoCache::Get). A type the cache lacks gets a tree built
 * from the instance alone, so the editor reads its model scale as zero
 * ("modelMatrix invert failed on model <model>" every frame the entity is
 * drawn on its own) and shows it as hidden. Rows a mod registers after the
 * build are such types, and the game's cache cannot be rebuilt once the
 * registered rows exceed its capacity.
 *
 * The product keeps its own table of default trees for those types. Get and
 * its lookup-only sibling are detoured: a name the game's cache holds is
 * served by the game untouched; a name it lacks is served from the product's
 * table, where the tree is built on first use exactly as the game's builder
 * builds each cache entry (create the type's default entity through the
 * game, write its state to text, destroy it, parse the text into a tree),
 * inside process heap 0 for the life of the process. Get runs on a one-entry
 * view of the table so its own copy-and-apply path produces the result; the
 * lookup returns the tree itself. An entityDef whose served bytes change at a
 * refresh loses its entry and is rebuilt at its next use.
 *
 * The pure part (the table, the engine cache's search and the view) has no
 * engine dependency and is tested with buffers; the rest binds it to the
 * resolved engine. */
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
/* Remove every entry whose key names `name` (case-insensitive, the engine's
 * decl-name rule), handing each tree to `destroy`. Returns the count. */
int sh_typeinfo_table_remove_name(sh_typeinfo_table *t, const char *name,
                                  void (*destroy)(void *context, void *tree), void *context);
void sh_typeinfo_table_free(sh_typeinfo_table *t, void (*destroy)(void *context, void *tree), void *context);

#ifndef SH_MODTYPEINFO_PURE
struct sig_result;
/* Resolve the engine and install the two detours. Returns 1 when they are
 * live; else logs one plain line and the editor keeps its own behavior. */
int sh_modtypeinfo_install(const struct sig_result *results, size_t count, const uint8_t *module_base);
/* After a palette build, on the main thread: once per boot, check the
 * product's builder against the game's cache on stock types (the detours
 * serve only after that check passes) and log one line with how many mod
 * rows resolve through the product's table. */
void sh_modtypeinfo_after_palette(void *palette);
/* A decl's served bytes changed at a refresh: forget its tree. */
void sh_modtypeinfo_decl_changed(const char *type, const char *name);
#endif

#endif /* BACKEND_MODTYPEINFO_H */
