/* The map manifest: one row per mod build a map needs, carried in the "mods"
 * part of the map's "snapmap-plus" member (map_carrier.h, which describes the
 * member's grammar once).
 *
 * A row is the compact JSON
 * {"mod":"<salt>@<build hash>","of"?,"name","size","names":[...],"attached"?,"choices"?}
 * where "mod" names the mod by its salt (the eight characters in its
 * mod.json's "snapmap-plus" block) and the build by the SHA-256 of its
 * canonical manifest, which itself records the salt, so a build verified
 * against the hash is proved to be that salt. "name" is the author's, for
 * display only; a mod has no version (a build is named by its hash).
 * "names" lists the placed logical names the build provides (informational; every name is kept, there is no row
 * budget). "choices": {"<slot>": "<salt>", ...} records the map's choices
 * between its mods' edits (modregistry.h: which declared mod's value serves a
 * field or keyed entry several of them edit, slot "<game file>/<path>",
 * "entitydef/player/coop/edit/handsModelMPUseDoomHands"); a save writes each
 * choice on the row of the salt it names, a load reads them from every row
 * (the first row naming a slot wins). "attached": true marks a map-level row:
 * the mod was attached to the map (the Mods tab's Use in this map, `sh_mods
 * map add`) without placing anything, so the map keeps the row whatever it
 * places. "of": a published map names, for a mod it uses only part of, the
 * build of the files it needs (modslice.h), cut from the build "of" names: the
 * root folder of the salt at that build serves the row as well as the cut
 * does. Only the publish save writes it (sh_modmanifest_set_slicer). Rows
 * never carry asset bytes, and merge by salt and build.
 *
 * A row object with an "id" or "bundle" member is an earlier shape and does
 * not parse; the parse detour refuses a map carrying one before any of this
 * runs (sh_map_old_shape, map_carrier.h).
 *
 * Load: every parse the DeserializeFromJson detour sees reads the rows from
 * the member, keeps them as the current map's manifest and asks the serving
 * layer to activate the builds that serve them (sh_modserve_row_build); the
 * member itself is stripped last by the parse detour (rawmap.c). Save: the
 * SerializeToJson detour computes the placed names, asks the serving layer
 * which builds provide them, adds the current manifest's map-level rows, and
 * puts one row per build into the member.
 */
#ifndef SNAPMAP_PLUS_MODMANIFEST_H
#define SNAPMAP_PLUS_MODMANIFEST_H

#include <stddef.h>
/* Build hash text: 64 lowercase hex characters. */
#define SH_MOD_HASH_CHARS 64

#define SH_MODMANIFEST_SALT_LEN   8
#define SH_MODMANIFEST_SALT_CAP   (SH_MODMANIFEST_SALT_LEN + 1)
#define SH_MODMANIFEST_HASH_CAP   (SH_MOD_HASH_CHARS + 1)

/* A row's text is carried whole: the author's name, every placed name and
 * every choice slot are heap strings of their own length, and the row
 * format (JSON) has no field width. */
typedef struct sh_modmanifest_row {
    char salt[SH_MODMANIFEST_SALT_CAP];          /* the mod */
    char build[SH_MODMANIFEST_HASH_CAP];         /* its canonical manifest's SHA-256 */
    char of[SH_MODMANIFEST_HASH_CAP];            /* the build this one was cut from ("of"), "" for a whole build */
    char *name;            /* display only: malloc'd, NULL for none (sh_modmanifest_row_name) */
    unsigned long long size;
    char **names;          /* malloc'd strings; owned by the row */
    size_t name_count;
    int attached;          /* a map-level row ("attached": true) */
    char *choices;         /* its "choices": "<slot>\t<salt>\n" per entry, in text order; malloc'd, NULL for none */
} sh_modmanifest_row;

typedef struct sh_modmanifest_list {
    sh_modmanifest_row *rows;
    size_t count;
    size_t capacity;
} sh_modmanifest_list;

void sh_modmanifest_row_free(sh_modmanifest_row *row);
/* The row's display name, never NULL ("" for none). */
const char *sh_modmanifest_row_name(const sh_modmanifest_row *row);
/* Replace the row's display name with a copy of `name` (NULL or "": none).
 * Returns 0 when memory runs out, leaving it as it was. */
int  sh_modmanifest_row_set_name(sh_modmanifest_row *row, const char *name);
void sh_modmanifest_list_free(sh_modmanifest_list *list);
/* Append a copy of `row` (its strings copied). Returns 0 on allocation failure. */
int  sh_modmanifest_list_add(sh_modmanifest_list *list, const sh_modmanifest_row *row);
/* Add `name` to the row's list unless an equal name (case-insensitive) is there. */
int  sh_modmanifest_row_add_name(sh_modmanifest_row *row, const char *name);

/* ------------------------------------------------------------ grammar ---- */

/* 1 when `text` is exactly 64 lowercase hex characters. */
int sh_modmanifest_valid_hash(const char *text, size_t length);

/* Parse a row value. 1 on success; 0 with the reason in err (a row with an
 * "id" or "bundle" member is an earlier shape and is refused). */
int sh_modmanifest_parse_value(const char *json, size_t length, sh_modmanifest_row *out,
                               char *err, size_t err_cap);
/* The compact row value, NUL-terminated, malloc'd (free). Every name is kept. */
char *sh_modmanifest_format_value(const sh_modmanifest_row *row, size_t *out_length);

/* ------------------------------------------------------------ map text --- */

/* Read every well-formed row of the member's "mods" part in `json` into `out`
 * (appended, duplicates by salt and build merged). Returns the number of rows
 * read; rows that do not parse are counted in *rejected. */
size_t sh_modmanifest_read(const char *json, size_t length, sh_modmanifest_list *out,
                           unsigned *rejected);

/* Put one row per entry of `rows` as the member's "mods" part
 * (map_carrier.h). Returns an owned process-heap buffer (HeapFree), or NULL
 * with the reason in err. With count 0 this only removes the part (NULL when
 * there was nothing to remove). */
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

/* What the manifest needs from a built mod's mod.json: its salt and build
 * hash (the "snapmap-plus" block's "salt" and "build"), its name (author
 * block, display only), total carried size, and every file's
 * provides.name (block "files" and "stock", stored as `names`). 1 on
 * success; 0 with the reason in err (a block without a salt or a build, as
 * while its build fails, is not built). */
int sh_modmanifest_read_mod_json(const char *text, size_t length, sh_modmanifest_row *out,
                                 char *err, size_t err_cap);

/* ------------------------------------------------------------ hooks ------ */

/* Activate the rows' mods: each row through the build that serves it now
 * (sh_modserve_row_build: the root when it has the row's build or the map
 * is the player's own, else the map's own place), else the build the editor
 * substitutes, else its own hash. Returns

 * modserve_activate_bundles' result. Main thread. */
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
 * kept as it is), and return the text with one row per bundle in the member
 * (stale rows removed). NULL when the text needs no change (no bundle and no stale
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

/* The build that serves a row at save (modui.c binds the serving index,
 * sh_modserve_row_build): for `build` of `salt`. Fills salt, build, name
 * and size and returns 1, or 0 when nothing installed answers (the
 * row is written as it is). A map-level row is renamed to that build; a row
 * the placed names gave takes its name from it. */
typedef int (*sh_modmanifest_resolve_fn)(const char *build, const char *salt, sh_modmanifest_row *out);
void sh_modmanifest_set_resolver(sh_modmanifest_resolve_fn resolve);

/* The publish save's cut (modpublish.c binds it): called for every row a
 * save writes, after the map scope has seen the rows. 1 when it replaced the
 * row's build, size and "of" with a cut of the build that the map needs; 0
 * leaves the row naming the whole build (every save but a publish's, and a
 * publish's row that ships whole). The open map's own record keeps the whole
 * builds either way. NULL unbinds. */
typedef int (*sh_modmanifest_slice_fn)(sh_modmanifest_row *row);
void sh_modmanifest_set_slicer(sh_modmanifest_slice_fn slicer);

/* An editor open of the author's own map whose row pins a build that no
 * place serves: another build of the same salt
 * serves in its place for this editor session, with the hashes on
 * the backend log; the row keeps X until a save re-pins it. The
 * substitute op answers per row: 1 with the build to serve (hash_out), 0 none
 * (*why: the gate's sentence, malloc'd, or NULL). `placed` says the map's placed
 * entities name the row's namespace (`mods/<salt>/`): with nothing to serve
 * them, the engine's validator refuses the map, so the editor cannot open it. Bound by modpublished.c, which
 * asks its denial gate (sh_modpub_gate_decide, an editor open). Only a parse
 * the engine makes for the editor (sh_modmanifest_set_editor_load 1, set by
 * the parse detour for a read that is not a published map's, which includes
 * a My Maps play's and the hold's own resume) substitutes here. The holds
 * decide on their own: a My Maps play, a rawmap load or a prefab placement
 * whose declared build the store cannot supply substitutes too (the gate's
 * SUBSTITUTE for the player's own content), and the launched map's listing
 * declares the build this table names; a community map's Play denies. */
typedef int (*sh_modmanifest_substitute_fn)(const sh_modmanifest_row *row, char *hash_out, size_t hash_cap,
                                            int placed, char **why);
void sh_modmanifest_set_substitute(sh_modmanifest_substitute_fn substitute);
void sh_modmanifest_set_editor_load(int editor);
/* The cause sentences such a load found for rows nothing serves ("MODS: map:
 * <salt>: <sentence>") are held, not logged, until the gate has decided: a
 * gate that takes the parse (a published or launched map the parse detour
 * did not recognise as one) says its own cause, so it drops them (say 0); a
 * parse no gate took was the editor's, and its notes are logged once (say 1).
 * A new load replaces them. Returns the number logged or dropped. */
int  sh_modmanifest_load_notes_flush(int say);
int  sh_modmanifest_load_notes_pending(void);
/* Of those notes, the first whose row the map's placed entities name (the
 * editor refuses such a map): its sentence in *out (malloc'd; without the
 * "MODS: map:" prefix) when out is not NULL, returning 1; 0 when there is
 * none. Cleared with the notes. */
int  sh_modmanifest_load_refusal(char **out);
/* Refuse the map being parsed before anything reads it (rawmap.c: a map in an
 * earlier shape, sh_map_old_shape; the engine does not parse it, so the open
 * map's manifest stays): `sentence` replaces the held notes as the load's one
 * note and its refusal, said as the other refusals are. */
void sh_modmanifest_load_refuse(const char *sentence);
/* The build serving in place of a declared one in the open map: 1 with it in
 * out, 0 when `declared` is served as itself. */
int  sh_modmanifest_substituted(const char *declared, char *out, size_t cap);

/* A copy of the manifest of the map last parsed (rows from its text). Free
 * with sh_modmanifest_list_free. Returns the row count. */
size_t sh_modmanifest_current(sh_modmanifest_list *out);
/* Drop the current manifest's rows naming `build` (exact hash): the Mods
 * tab removed that mod from the open map, so the map no longer carries it
 * (the next save rebuilds the rows from the placed entities). Returns the
 * number of rows dropped. */
size_t sh_modmanifest_current_remove(const char *build);
/* Attach a mod to the open map: mark the current manifest's row of
 * `row->salt` at `row->build` map-level, or add `row` as a map-level row (names dropped).
 * The next save writes it (sh_modmanifest_on_save) and every later load keeps
 * it. Returns 1 attached now, 0 already attached, -1 invalid or out of memory. */
int    sh_modmanifest_current_attach(const sh_modmanifest_row *row);
/* A copy of the current manifest's map-level rows. Returns the row count. */
size_t sh_modmanifest_current_attached(sh_modmanifest_list *out);
/* Forget the current manifest (tests). */
void sh_modmanifest_reset(void);
/* The open map closed (the editor left it, or a hub launch's Play is over):
 * its rows, substitutions and registry choices go, so nothing reads them as
 * the next map's. */
void sh_modmanifest_current_close(void);
/* Put back the state the last load replaced (its rows, registry choices and
 * substitutions), once: a launch the gate refused ran nothing, so the map
 * open before it is the open map again. Returns the
 * rows restored, -1 when nothing is kept (no load since the last restore). */
int    sh_modmanifest_current_restore_previous(void);

/* The open map's registry choices (modregistry.h): read from the rows at
 * every load, written at every save on the row of the salt each names (a
 * choice naming a salt the save writes no row for is dropped, logged), and
 * given to the serving index as its choice input
 * (sh_modserve_scope_set_choices) at the load, before the listing. */
/* Set (salt) or clear (salt NULL or "") the choice for `slot`, and give the
 * choices to the serving index, which recomputes the registries. Returns the
 * builds whose serving changed (>= 0), -1 for an invalid slot or salt. */
int    sh_modmanifest_current_choose(const char *slot, const char *salt);
/* 1 with the salt chosen for `slot` in `salt`, 0 when there is none. */
int    sh_modmanifest_current_choice(const char *slot, char *salt, size_t cap);
/* The choices as "slot\tsalt\n" lines in *out (malloc'd). Returns their count. */
size_t sh_modmanifest_current_choices(char **out);
/* The choices a list of rows carries, merged (the first row naming a slot
 * wins), as "slot\tsalt\n" lines in *out (malloc'd, NULL for none).
 * Returns their count. */
size_t sh_modmanifest_rows_choices(const sh_modmanifest_list *rows, char **out);

#endif /* SNAPMAP_PLUS_MODMANIFEST_H */
