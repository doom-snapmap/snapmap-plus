/* The map manifest: one "smpmod1.<index>.<count>" string variable per bundle a
 * map needs (Contract 3).
 *
 * A row is an ordinary snapVarString_t in variables.string[], appended last so
 * existing variable bindings keep their (type, index). Its initialValue is the
 * compact JSON {"id","version","bundle","size","names":[...]} where "bundle"
 * is the bundle hash and "names" the placed logical names the bundle provides
 * (informational). Rows never carry asset bytes.
 *
 * Load: every parse the DeserializeFromJson detour sees reads the rows, keeps
 * them as the current map's manifest, strips them (allocCount[4] rewritten)
 * and asks the serving layer to activate the named bundles. Save: the
 * SerializeToJson detour computes the placed names, asks the serving layer
 * which bundles provide them, and appends one row per bundle.
 */
#ifndef SNAPMAP_PLUS_MODMANIFEST_H
#define SNAPMAP_PLUS_MODMANIFEST_H

#include <stddef.h>
/* Bundle hash text: 64 lowercase hex characters (Contract 1). */
#define SH_MOD_HASH_CHARS 64

#define SH_MODMANIFEST_MAGIC      "smpmod1."
#define SH_MODMANIFEST_MAGIC_LEN  8
#define SH_MODMANIFEST_ID_CAP     65    /* Contract 1: at most 64 characters */
#define SH_MODMANIFEST_VERSION_CAP 33   /* at most 32 */
#define SH_MODMANIFEST_HASH_CAP   (SH_MOD_HASH_CHARS + 1)
/* A row stays far below the 4,096-byte playtest message that a client without
 * the product would replicate it into; names beyond this budget are dropped
 * from the (informational) list, never the row itself. */
#define SH_MODMANIFEST_ROW_MAX    1024
#define SH_MODMANIFEST_NAME_MAX   512
#define SH_MODMANIFEST_MAX_NAMES  256    /* per row, on read */
#define SH_MODMANIFEST_MAX_ROWS   4096

typedef struct sh_modmanifest_row {
    char id[SH_MODMANIFEST_ID_CAP];
    char version[SH_MODMANIFEST_VERSION_CAP];
    char bundle[SH_MODMANIFEST_HASH_CAP];
    unsigned long long size;
    char **names;          /* malloc'd strings; owned by the row */
    size_t name_count;
} sh_modmanifest_row;

typedef struct sh_modmanifest_list {
    sh_modmanifest_row *rows;
    size_t count;
    size_t capacity;
} sh_modmanifest_list;

void sh_modmanifest_row_free(sh_modmanifest_row *row);
void sh_modmanifest_list_free(sh_modmanifest_list *list);
/* Append a copy of `row` (names copied). Returns 0 on allocation failure. */
int  sh_modmanifest_list_add(sh_modmanifest_list *list, const sh_modmanifest_row *row);
/* Add `name` to the row's list unless an equal name (case-insensitive) is there. */
int  sh_modmanifest_row_add_name(sh_modmanifest_row *row, const char *name);

/* ------------------------------------------------------------ grammar ---- */

/* 1 when `id` is a bundle id: lowercase [a-z0-9][a-z0-9._-]*, no consecutive
 * dots, no trailing dot, at most 64 characters. */
int sh_modmanifest_valid_id(const char *id);
/* 1 when `text` is exactly 64 lowercase hex characters. */
int sh_modmanifest_valid_hash(const char *text, size_t length);

/* 1 when `header` (quotes excluded) is "smpmod1.<index>.<count>", 1-7 digits
 * each, count at least 1, index below count. */
int sh_modmanifest_header(const char *header, size_t length, unsigned *index, unsigned *count);

/* Parse a decoded row value. 1 on success; 0 with the reason in err. */
int sh_modmanifest_parse_value(const char *json, size_t length, sh_modmanifest_row *out,
                               char *err, size_t err_cap);
/* The compact row value, NUL-terminated, malloc'd (free). Names are dropped
 * from the end while the value would exceed SH_MODMANIFEST_ROW_MAX. */
char *sh_modmanifest_format_value(const sh_modmanifest_row *row, size_t *out_length);

/* ------------------------------------------------------------ map text --- */

/* Read every well-formed row in `json` into `out` (appended, duplicates by
 * bundle hash merged). Returns the number of rows read; rows that do not
 * parse are counted in *rejected. */
size_t sh_modmanifest_read(const char *json, size_t length, sh_modmanifest_list *out,
                           unsigned *rejected);

/* Remove every smpmod1 row and rewrite allocCount[4]. Returns an owned
 * NUL-terminated process-heap buffer (HeapFree), or NULL when there is none or
 * the structure cannot be read (*doc_failed set). */
char *sh_modmanifest_strip(const char *json, size_t length, size_t *out_length,
                           unsigned *removed, int *doc_failed);

/* Strip any existing rows, then append one row per entry of `rows`, last in
 * variables.string[], and set allocCount[4]. Returns an owned process-heap
 * buffer (HeapFree), or NULL with the reason in err. With count 0 this only
 * strips (NULL when there was nothing to strip). */
char *sh_modmanifest_write(const char *json, size_t length, const sh_modmanifest_list *rows,
                           size_t *out_length, char *err, size_t err_cap);

/* The placed logical names: every string value keyed "inherit" or
 * "moduleName" and every string value inside an object keyed "edit", decoded,
 * deduplicated case-insensitively, empty values skipped. Names are malloc'd;
 * free with sh_modmanifest_names_free. Returns the count, or 0 on a
 * structural failure (*failed set). */
size_t sh_modmanifest_placed_names(const char *json, size_t length, char ***out_names,
                                   int *failed);
void sh_modmanifest_names_free(char **names, size_t count);

/* ------------------------------------------------------------ mod.json --- */

/* What the manifest needs from an installed bundle's mod.json: its id and
 * version (author block), bundle hash, total carried size, and every file's
 * provides.name (product block "files" and "stock", stored as `names`). 1 on
 * success; 0 with the reason in err. */
int sh_modmanifest_read_mod_json(const char *text, size_t length, sh_modmanifest_row *out,
                                 char *err, size_t err_cap);

/* ------------------------------------------------------------ hooks ------ */

/* The DeserializeFromJson step: read and keep the rows as the current
 * manifest, strip them, then call modserve_activate_bundles with their hashes
 * (count 0 for a map without rows). Returns the stripped buffer (HeapFree), or
 * NULL to parse `json` unchanged. Never throws for malformed input. */
char *sh_modmanifest_prepare_load(const char *json, size_t length, size_t *out_length);

/* The SerializeToJson step: compute the placed names, ask
 * modserve_bundles_for_names for the bundles that provide them, and return the
 * text with one row per bundle appended (stale rows removed). NULL when the
 * text needs no change (no bundle and no stale row) or on failure, which is
 * logged; the engine's bytes are then saved unchanged. */
char *sh_modmanifest_on_save(const char *json, size_t length, size_t *out_length);

/* The save step's restore hook (modui.c's recycle bin, the modserve.h
 * sh_modserve_restore_fn contract with `names`): when `armed` says a recycled
 * mod could be needed, the placed names go to `restore` before the rows are
 * built, so a mod the map still places is reinstated and gets its row. */
void sh_modmanifest_set_restore(int (*restore)(const char *const *hashes, int hash_count, const char *const *names,
                                               size_t name_count, int when), int (*armed)(void));

/* A copy of the manifest of the map last parsed (rows from its text). Free
 * with sh_modmanifest_list_free. Returns the row count. */
size_t sh_modmanifest_current(sh_modmanifest_list *out);
/* Drop the current manifest's rows naming `bundle` (exact hash): the Mods
 * tab removed that mod from the open map, so the map no longer carries it
 * (the next save rebuilds the rows from the placed entities). Returns the
 * number of rows dropped. */
size_t sh_modmanifest_current_remove(const char *bundle);
/* Forget the current manifest (tests). */
void sh_modmanifest_reset(void);

#endif /* SNAPMAP_PLUS_MODMANIFEST_H */
