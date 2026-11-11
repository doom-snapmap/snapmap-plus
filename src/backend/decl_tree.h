/* decl_tree.h -- the structural decl model behind derived edits
 * (DESIGN-additive 2.4): a parser from the decl grammar to a tree, the
 * structural diff of an author's edited copy of a game file against the
 * game's own text, and the apply of several mods' diffs onto the game's text.
 * Pure C: no engine, filesystem or index access.
 *
 * Grammar (the one ms_compose has served since phase 2, extended to keep
 * every token): a text is one block; a block holds statements
 *
 *   stmt := key '=' value [';']            a field (`num = 3;`, `color = ( 1, 1, 1, 1 );`)
 *         | key value [';']                the md6Def spelling (no '=')
 *         | key ['='] '{' stmt* '}' [';']  a block (`edit = {`, `editorVars {`)
 *         | key name '{' stmt* '}' [';']   a named block (`anim "clip" {`, `event "ae_sound" {`)
 *         | key                            a bare member before '}' (`{ "MD6_WEIGHTGROUP_ALL" }`)
 *   key, name, value := word | "string"     value also ( group )
 *
 * Comments are dropped; every other token, every '=' and every ';' is kept,
 * so a generated text lexes to the token sequence the engine would read from
 * the same tree. Keys are matched without case; values and names are compared
 * as written (keyed names without case, as the engine looks graphs up).
 *
 * Kinds of block member, decided from both texts of a diff:
 *   field        a key that appears at most once on each side and has no
 *                name: set (path, value) or removed (path); blocks on both
 *                sides are compared member by member.
 *   ordered list a block of `num = N;` and item[0] .. item[n-1], dense and
 *                in order: entries are the items, matched by their text (a
 *                conductor aiTypeList item is its aiType and entityDef pair);
 *                written back renumbered with num kept at N plus the items
 *                added. Sparse item[i] blocks (damageGroups) are fields.
 *   named entry  a block with a name (`anim "<clip>"`), unique by name on
 *                each side: added, removed, or compared member by member;
 *                two mods adding one name merge their rows.
 *   keyed entry  repeated blocks each carrying a name field (a `name`
 *                member, or one under `object` or `object/object`: the
 *                subGraphs and nodes of aifsmmanager states), unique by
 *                name on each side: added, removed or replaced whole.
 *   bag entry    any other repeated statement (`alias { }`, `event "x" { }`
 *                rows, `layer = "x";`), and the members a block holds when
 *                the game's block is empty: matched by their whole text,
 *                added or removed.
 *
 * Edits JSON (the product block's "edits", one element per edited file):
 *   {"file": "<request>", "stock": "<64-bit FNV-1a of the game text diffed against, hex>",
 *    "counts": {"set": n, "remove": n, "list": n, "keyed": n, "bag": n, "drop": n},
 *    "ops": [ op, ... ]}
 *   op := {"op": "set",    "path": [seg...], "entry": "<statement>"}
 *       | {"op": "remove", "path": [seg...]}
 *       | {"op": "list",   "path": [seg...], "entry": "<item value>"}
 *       | {"op": "unlist", "path": [seg...], "entry": "<item value>"}
 *       | {"op": "keyed",  "path": [seg...], "word": "<key>", "name": "<name>", "entry": "<statement>"}
 *       | {"op": "unkey",  "path": [seg...], "word": "<key>", "name": "<name>"}
 *       | {"op": "bag",    "path": [seg...], "entry": "<statement>"}
 *       | {"op": "unbag",  "path": [seg...], "entry": "<statement>"}
 * A seg is a member's key (`edit`, `item[4]`) or a named block's head
 * (`anim "clip"`). A set's path ends at the field it sets; every other path
 * is the container's.
 */
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
    int list;                          /* a dense ordered list (num + item[0..n-1]) */
    long num_delta;                    /* a list's num minus its item count */
    int src;                           /* -1 parsed from the base; else the apply source that added it */
} sh_dt_node;

typedef struct sh_dt sh_dt;            /* a tree and the arena its nodes live in */

/* Parse a decl text. NULL with a cause ("line N: ...") when it does not read
 * in the grammar. */
sh_dt *sh_dt_parse(const char *text, size_t len, char *cause, size_t cap);
void   sh_dt_free(sh_dt *t);
sh_dt_node *sh_dt_root(sh_dt *t);
/* The text of a tree (malloc'd, NUL-terminated): tabs per depth, the base
 * text's line ending, every kept token in order. */
char  *sh_dt_text(const sh_dt *t, size_t *len);
/* Semantic equality: fields and entries in any order, list items in order. */
int    sh_dt_equal(const sh_dt *a, const sh_dt *b);
/* 1 when both texts parse and are semantically equal. */
int    sh_dt_texts_equal(const char *a, size_t alen, const char *b, size_t blen);
/* The unique block member at `path` ('/' between keys, named heads as
 * `anim "clip"`) below `n`, or NULL. */
sh_dt_node *sh_dt_at(sh_dt_node *n, const char *path);
/* The member of `n` with key word `key` (any case), when exactly one. */
sh_dt_node *sh_dt_child(sh_dt_node *n, const char *key);
/* Replace a scalar's value token (copied into the tree's arena). 1 on success. */
int    sh_dt_set_value(sh_dt *t, sh_dt_node *n, const char *val, size_t len);

/* Per-kind operation counts of one file's edits. */
typedef struct sh_dt_counts { int set, remove, list, keyed, bag, drop; } sh_dt_counts;
int sh_dt_counts_total(const sh_dt_counts *c);

/* The edits element of one edited copy against the game's text: a JSON
 * object (malloc'd) as above, its counts in *counts. NULL with a cause when
 * either text does not parse ("the game's text: ..." or "line N: ..."). A
 * cause quotes at most 60 characters of the decl text around the line it
 * names, so SH_DT_CAUSE_CAP bytes always hold it whole. */
#define SH_DT_CAUSE_CAP 512
char *sh_dt_diff(const char *request, const char *stock, size_t stock_len, const char *copy, size_t copy_len,
                 sh_dt_counts *counts, char *cause, size_t cap);

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

/* The game's text `stock` of `request` with every source's operations for
 * that file applied in row order: identical operations merge, additions
 * union, a slot set to different values is the choice's (slot
 * "<type>/<decl name>/<path>", key) else the first source's. NULL when the
 * stock text does not parse, or no source has operations for the file. */
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
