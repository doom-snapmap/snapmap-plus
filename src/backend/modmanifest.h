/* The map manifest: one "smpmod1.<index>.<count>" string variable per bundle a
 * map needs (Contract 3).
 *
 * A row is an ordinary snapVarString_t in variables.string[], appended last so
 * existing variable bindings keep their (type, index). Its initialValue is the
 * compact JSON {"id","version","bundle","size","attached"?,"choices"?,"names":[...]}
 * where "bundle" is the bundle hash and "names" the placed logical names the
 * bundle provides (informational). "choices": {"<slot>": "<key>", ...} records
 * the map's registry choices (modregistry.h: which declared mod serves a
 * player field or weapon slot several of them set, "player.hands",
 * "weapons/<stock weapon decl>"); a save writes each choice on the row of the
 * key it names, a load reads them from every row (the first row naming a
 * slot wins). Names are dropped before choices when a row would exceed its
 * budget. "attached": true marks a map-level row: the mod
 * was attached to the map (the Mods tab's Use in this map, `sh_mods map add`)
 * without placing anything, so the map keeps the row whatever it places. A
 * reader that does not know the key treats the row as a placed mod's; an older
 * product drops the row at its next save. Rows never carry asset bytes.
 *
 * Load: every parse the DeserializeFromJson detour sees reads the rows, keeps
 * them as the current map's manifest, strips them (allocCount[4] rewritten)
 * and asks the serving layer to activate the named bundles. Save: the
 * SerializeToJson detour computes the placed names, asks the serving layer
 * which bundles provide them, adds the current manifest's map-level rows, and
 * appends one row per bundle.
 *
 * Product version (DESIGN-additive 2.7): one more string variable,
 * "smpprod1.<version>" (its initialValue the same version), appended last the
 * same way, says the least Snapmap+ version the map needs. A save writes it
 * when the map uses a registry choice, a declared mod with registry intent
 * (player fields, weapons, extends, conductor rows) or surfaced content, or a
 * product-only feature (a Grid Room variant module); the version is the
 * running product's (SH_PRODUCT_VERSION, stamped at build time). A load reads
 * and strips it like the rows (sh_modmanifest_product); a product that
 * predates it keeps it as an inert string variable. It is never a row, so a
 * map that needs the product but no mod is never counted as missing a mod.
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
/* Registry choices a row carries ("slot\tkey\n" lines) and per map. */
#define SH_MODMANIFEST_CHOICES_CAP 768
#define SH_MODMANIFEST_SLOT_MAX    160
#define SH_MODMANIFEST_CHOICES_MAX 64

typedef struct sh_modmanifest_row {
    char id[SH_MODMANIFEST_ID_CAP];
    char version[SH_MODMANIFEST_VERSION_CAP];
    char bundle[SH_MODMANIFEST_HASH_CAP];
    unsigned long long size;
    char **names;          /* malloc'd strings; owned by the row */
    size_t name_count;
    int attached;          /* a map-level row ("attached": true) */
    char choices[SH_MODMANIFEST_CHOICES_CAP];   /* its "choices": "<slot>\t<key>\n" per entry, in text order */
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

/* ------------------------------------------------------------ product ---- */

#define SH_MODMANIFEST_PRODUCT_MAGIC     "smpprod1."
#define SH_MODMANIFEST_PRODUCT_MAGIC_LEN 9

/* 1 when `v` is a product version a map may record: 1-32 printable ASCII
 * bytes without quotes, backslashes or spaces ("v0.2.1-beta.24", "0.3.0"). */
int  sh_modmanifest_version_valid(const char *v);
/* Compare two product versions: [v]MAJOR.MINOR.PATCH[-pre.release]; a
 * release is newer than its prereleases, prerelease parts compare as numbers
 * when both are digits, else as text. -1 a older than b, 0 equal, 1 newer,
 * -2 either does not read. */
int  sh_modmanifest_version_compare(const char *a, const char *b);
/* The "smpprod1.<version>" variable of `json`: 1 with the version in out
 * (the first that reads), 0 none. */
int  sh_modmanifest_product_read(const char *json, size_t length, char *out, size_t cap);
/* `json` with every "smpprod1." variable removed and, when `version` is a
 * valid one, one appended. Process-heap buffer (HeapFree), or NULL when
 * nothing changed or the text does not read. */
char *sh_modmanifest_product_put(const char *json, size_t length, const char *version, size_t *out_length);
/* The running product's version, as a save writes it: SH_PRODUCT_VERSION
 * (stamped by src/backend/build.ps1), else "" (nothing is written). */
int  sh_modmanifest_product_version(char *out, size_t cap);
/* Tests and tools: the version the save writes (NULL returns to the rule above). */
void sh_modmanifest_set_product_version(const char *version);
/* The open map's product version (read at its load, written at its save):
 * 1 with it in out, 0 when the map records none. */
int  sh_modmanifest_product(char *out, size_t cap);
/* 1 when a declared mod of these rows needs the product (modui binds it:
 * registry intent or surfaced content). NULL unbinds. */
typedef int (*sh_modmanifest_product_need_fn)(const sh_modmanifest_list *rows);
void sh_modmanifest_set_product_need(sh_modmanifest_product_need_fn need);

/* ------------------------------------------------------------ hooks ------ */

/* Activate the rows' mods: each row through the build that serves it now
 * (sh_modserve_bundle_serving: its hash or an alias of it, else the installed
 * build of its id), so a map plays with the installed copy whatever hash it
 * pins. Returns modserve_activate_bundles' result. Main thread. */
int sh_modmanifest_activate(const sh_modmanifest_list *rows);

/* The DeserializeFromJson step: read and keep the rows as the current
 * manifest, strip them, then activate them with the builds the map scope
 * lists beyond them (sh_modmanifest_set_listed; count 0 for a map without
 * rows or placed mods). Returns the stripped buffer (HeapFree), or
 * NULL to parse `json` unchanged. Never throws for malformed input. */
char *sh_modmanifest_prepare_load(const char *json, size_t length, size_t *out_length);

/* The SerializeToJson step: compute the placed names, ask
 * modserve_bundles_for_names for the bundles that provide them, add every
 * map-level row of the current manifest (renamed to the build that serves
 * when the bundle resolver knows one; a row of a mod that is not installed is
 * kept as it is), and return the text with one row per bundle appended (stale
 * rows removed). NULL when the text needs no change (no bundle and no stale
 * row) or on failure, which is logged; the engine's bytes are then saved
 * unchanged. */
char *sh_modmanifest_on_save(const char *json, size_t length, size_t *out_length);

/* The map scope's hook (modui.c, modserve.h map scope): at every load, with
 * the rows just read and, when `armed` answers above 0, the map's placed
 * names (`when` SH_MODSERVE_AT_LOAD), before the bundles are activated;
 * at every save with the rows written (names NULL, `when` SH_MODSERVE_AT_SAVE). NULL
 * unbinds. */
typedef void (*sh_modmanifest_scope_fn)(const sh_modmanifest_list *rows, const char *const *names, size_t name_count,
                                        int when);
void sh_modmanifest_set_scope(sh_modmanifest_scope_fn scope, int (*armed)(void));

/* The builds the map scope lists after a load's hook (modui.c binds
 * sh_modserve_scope_hashes): up to `cap` copied into `out` (NULL to count),
 * returning the count. A load activates its rows and every listed build
 * beyond them, the placed names' closure included, in one pass, so a map
 * that places a mod without a row (a rawmap opened as a new map) registers
 * it as a saved map's load would. NULL unbinds: rows alone. */
typedef int (*sh_modmanifest_listed_fn)(char (*out)[SH_MODMANIFEST_HASH_CAP], int cap);
void sh_modmanifest_set_listed(sh_modmanifest_listed_fn listed);

/* The build that serves a map-level row at save (modui.c binds the serving
 * index): `bundle_hash` (aliases resolve), else the installed build of `id`.
 * Fills id, version, bundle and size and returns 1, or 0 when nothing
 * installed answers (the row is written as it is). */
typedef int (*sh_modmanifest_resolve_fn)(const char *bundle_hash, const char *id, sh_modmanifest_row *out);
void sh_modmanifest_set_resolver(sh_modmanifest_resolve_fn resolve);

/* An editor open of the author's own map whose row pins a build that is no
 * longer installed (DESIGN-additive 2.5, 2.8): the installed build of the
 * same key serves in its place for this editor session, reported "declared
 * at build X, installed build Y"; the row keeps X until the author upgrades
 * the map (sh_mods map upgrade, the Mods tab) or a save re-pins it. The
 * substitute op answers per row: 1 with the build to serve (hash_out), 0 none
 * (why: the gate's sentence, may be empty). Bound by modpublished.c, which
 * asks its denial gate (sh_modpub_gate_decide, an editor open). Play, the hub,
 * rawmap loads and prefab placements keep their gate's denial; only a parse
 * the engine makes for the editor (sh_modmanifest_set_editor_load 1, set by
 * the parse detour for a read that is not a published map's) substitutes. */
typedef int (*sh_modmanifest_substitute_fn)(const sh_modmanifest_row *row, char *hash_out, size_t hash_cap,
                                            char *why, size_t why_cap);
void sh_modmanifest_set_substitute(sh_modmanifest_substitute_fn substitute);
void sh_modmanifest_set_editor_load(int editor);
/* The build serving in place of a declared one in the open map: 1 with it in
 * out, 0 when `declared` is served as itself. */
int  sh_modmanifest_substituted(const char *declared, char *out, size_t cap);

/* A copy of the manifest of the map last parsed (rows from its text). Free
 * with sh_modmanifest_list_free. Returns the row count. */
size_t sh_modmanifest_current(sh_modmanifest_list *out);
/* Drop the current manifest's rows naming `bundle` (exact hash): the Mods
 * tab removed that mod from the open map, so the map no longer carries it
 * (the next save rebuilds the rows from the placed entities). Returns the
 * number of rows dropped. */
size_t sh_modmanifest_current_remove(const char *bundle);
/* Attach a mod to the open map: mark the current manifest's row of
 * `row->bundle` map-level, or add `row` as a map-level row (names dropped).
 * The next save writes it (sh_modmanifest_on_save) and every later load keeps
 * it. Returns 1 attached now, 0 already attached, -1 invalid or out of memory. */
int    sh_modmanifest_current_attach(const sh_modmanifest_row *row);
/* Point the current manifest's row of `from` (exact hash) at the build `to`
 * names (id, version, bundle, size): the Mods tab's "upgrade this map to the
 * installed build". The next listing and save follow it. 1 renamed, 0 no such
 * row or `to` is already a row. */
int    sh_modmanifest_current_rename(const char *from, const sh_modmanifest_row *to);
/* A copy of the current manifest's map-level rows. Returns the row count. */
size_t sh_modmanifest_current_attached(sh_modmanifest_list *out);
/* Forget the current manifest (tests). */
void sh_modmanifest_reset(void);
/* The open map closed (the editor left it, or a hub launch's Play is over):
 * its rows, substitutions and registry choices go, so nothing reads them as
 * the next map's. */
void sh_modmanifest_current_close(void);
/* Put back the state the last load replaced (its rows, registry choices,
 * substitutions and product version), once: a launch the gate refused ran
 * nothing, so the map open before it is the open map again. Returns the
 * rows restored, -1 when nothing is kept (no load since the last restore). */
int    sh_modmanifest_current_restore_previous(void);

/* The open map's registry choices (modregistry.h): read from the rows at
 * every load, written at every save on the row of the key each names (a
 * choice naming a key the save writes no row for is dropped, logged), and
 * given to the serving index as its choice input
 * (sh_modserve_scope_set_choices) at the load, before the listing. */
/* Set (key) or clear (key NULL or "") the choice for `slot`, and give the
 * choices to the serving index, which recomputes the registries. Returns the
 * bundles whose serving changed (>= 0), -1 for an invalid slot or key. */
int    sh_modmanifest_current_choose(const char *slot, const char *key);
/* 1 with the key chosen for `slot` in `key`, 0 when there is none. */
int    sh_modmanifest_current_choice(const char *slot, char *key, size_t cap);
/* The choices as "slot\tkey\n" lines in `out`. Returns their count. */
size_t sh_modmanifest_current_choices(char *out, size_t cap);
/* The choices a list of rows carries, merged (the first row naming a slot
 * wins), as "slot\tkey\n" lines. Returns their count. */
size_t sh_modmanifest_rows_choices(const sh_modmanifest_list *rows, char *out, size_t cap);

#endif /* SNAPMAP_PLUS_MODMANIFEST_H */
