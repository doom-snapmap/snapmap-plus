/* decl_tree.h -- the structural decl model behind derived edits: parse a decl text to a tree, diff an
 * author's edited copy against the game's text, and apply several mods' diffs. Pure C: no engine,
 * filesystem or index access. */
#ifndef BACKEND_DECL_TREE_H
#define BACKEND_DECL_TREE_H

#include <stddef.h>

typedef struct sh_dt_node {
    const char *key; size_t key_n;     /* the key word, or a bare member's token */
    const char *name; size_t name_n;   /* a named block's name token, quotes included, or NULL */
    const char *val; size_t val_n;     /* a scalar's value token; NULL for a block; "" for a bare member */
    struct sh_dt_node **kids;
    int nkids, ckids;
    int block, eq, semi;               /* a block; written with '='; followed by ';' */
    int list;                          /* a dense ordered list */
    long num_delta;                    /* a list's num minus its item count */
    int src;                           /* -1 parsed from the base, else the adding source */
} sh_dt_node;

typedef struct sh_dt sh_dt;            /* a tree and the arena its nodes live in */

/* Parses a decl text: a block of statements `key = value;`, `key value;` (md6Def), `key { ... }`,
 * `key "name" { ... }` or a bare `key`, where a value is a word, "string" or ( group ). Comments are
 * dropped, every other token is kept, and keys match without case. NULL with a cause ("line N: ...") on a parse failure. */
sh_dt *sh_dt_parse(const char *text, size_t len, char *cause, size_t cap);
void   sh_dt_free(sh_dt *t);
sh_dt_node *sh_dt_root(sh_dt *t);
/* The text of a tree (malloc'd, NUL-terminated): tabs per depth, the base
 * text's line ending, every kept token in order. */
char  *sh_dt_text(const sh_dt *t, size_t *len);
/* The unique block member at `path` ('/' between keys, named heads as
 * `anim "clip"`) below `n`, or NULL. */
sh_dt_node *sh_dt_at(sh_dt_node *n, const char *path);
/* The member of `n` with key word `key` (any case), when exactly one. */
sh_dt_node *sh_dt_child(sh_dt_node *n, const char *key);
/* Read the bound entityDef's effective AI behavior from this mod's typed declarations. A NULL body
 * asks only whether the declaration exists; otherwise read supplies a parsed tree the caller owns. */
typedef int (*sh_dt_enemy_read)(void *context, const char *type, const char *name, sh_dt **body);
char *sh_dt_enemy_behavior(const char *salt, const char *entitydef, sh_dt_enemy_read read, void *context);
/* Read the placed entity and caption tag of a Toybox row. Each returned string is caller-owned. */
void sh_dt_tile_fields(const char *text, size_t len, char **entity, char **tag);
void sh_dt_remove_child(sh_dt_node *parent, int index);
/* Allocate a detached block or scalar, then attach it to its containing tree. */
sh_dt_node *sh_dt_member(sh_dt *t, const char *key, int block);
int sh_dt_append(sh_dt *t, sh_dt_node *parent, sh_dt_node *child);
sh_dt_node *sh_dt_ensure_block(sh_dt *t, sh_dt_node *parent, const char *key);
int sh_dt_assign_scalar(sh_dt *t, sh_dt_node *parent, const char *key, const char *token);
/* Deep-copy one member into another tree. */
int sh_dt_copy_child(sh_dt *t, sh_dt_node *parent, const sh_dt_node *child);
/* Replace a scalar's value token (copied into the tree's arena). 1 on success. */
int    sh_dt_set_value(sh_dt *t, sh_dt_node *n, const char *val, size_t len);

/* Per-kind operation counts of one file's edits. */
typedef struct sh_dt_counts { int set, remove, list, keyed, bag, drop; } sh_dt_counts;
int sh_dt_counts_total(const sh_dt_counts *c);

/* The diff classifies each block member as: field (set or removed; blocks recurse), ordered list (dense
 * `num = N;` plus item[0..n-1], matched by item text), named entry (unique `key "name"` block), keyed
 * entry (repeated blocks with a name field, replaced whole) or bag entry (any other repeated
 * statement, matched by whole text). */

/* Edits JSON, one element per edited file: {"file", "stock" (FNV-1a hex of the diffed game text),
 * "counts", "ops": [{"op": set|remove|list|unlist|keyed|unkey|bag|unbag, "path": [seg...], "entry"?,
 * "word"?, "name"?}]}. A seg is a member key (`item[4]`) or a named block's head (`anim "clip"`); a
 * set's path ends at its field, every other path is the container's. */

/* The edits element (malloc'd JSON, counts in *counts) of one edited copy against the game's text.
 * NULL with a cause ("the game's text: ..." or "line N: ...") when either text does not parse; a cause
 * quotes at most 60 decl characters, so SH_DT_CAUSE_CAP bytes always hold it. */
#define SH_DT_CAUSE_CAP 512
char *sh_dt_diff(const char *request, const char *stock, size_t stock_len, const char *copy, size_t copy_len,
                 sh_dt_counts *counts, char *cause, size_t cap);

/* The edits element `element` with only the operations whose entry `keep` keeps (operations without an entry are
 * kept), its counts recomputed (malloc'd; NULL when it does not read). *dropped: the operations left out. */
char *sh_dt_edits_keep(const char *element, size_t len, int (*keep)(void *context, const char *entry), void *context,
                       sh_dt_counts *counts, int *dropped);

/* One declared bundle's edits for the apply, in row order. */
typedef struct sh_dt_source {
    const char *key;                   /* its key (a choice names it) */
    const char *who;                   /* key@version (hash16), for lines */
    const char *edits;                 /* its product block's "edits" array (JSON), or NULL */
    size_t edits_len;
} sh_dt_source;

typedef struct sh_dt_choice { const char *slot, *key; } sh_dt_choice;

/* What an apply did. Callbacks may be NULL. */
typedef struct sh_dt_report {
    void (*line)(void *context, const char *text);
    /* A slot two or more sources set to different values: the sources (row
     * order), the one serving, and whether the map's choice picked it. */
    void (*clash)(void *context, const char *slot, const int *sources, int n, int serving, int by_choice);
    void *context;
    int applied, merged, skipped, clashes;   /* operations */
    int sources;                       /* sources with operations for the file */
    unsigned char *used;               /* optional, one per source: set when its operations changed the text */
} sh_dt_report;

/* The game's `stock` text of `request` with every source's operations applied in row order: identical
 * ones merge, additions union, a slot set differently is the choice's (slot "<type>/<decl name>/<path>",
 * key) else the first source's. NULL when stock does not parse or no source has operations. */
sh_dt *sh_dt_apply(const char *request, const char *stock, size_t stock_len, const sh_dt_source *src, int n,
                   const sh_dt_choice *choices, int nchoices, sh_dt_report *report);

/* Each file an edits array names: fn(file, counts) per element. Returns the
 * count, -1 when the JSON does not read. */
int sh_dt_edits_files(const char *edits, size_t len,
                      void (*fn)(void *context, const char *file, const sh_dt_counts *counts), void *context);
/* Each slot a source's operations for `request` claim (set, remove, keyed,
 * unkey), with a digest of the value it sets: fn(slot, digest). */
int sh_dt_edits_slots(const char *request, const char *edits, size_t len,
                      void (*fn)(void *context, const char *slot, unsigned long long digest), void *context);
/* "<type>/<decl name>" of a generated/decls/<type>/<name>.decl request, else
 * the request itself (malloc'd), or NULL. */
char *sh_dt_game_name(const char *request);

#endif /* BACKEND_DECL_TREE_H */
