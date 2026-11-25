/* The map manifest: one row per mod build a map needs, carried in the "mods" part of the map's "snapmap-plus"
 * member (map_carrier.h). A load reads the rows and activates the builds that serve them; a save writes one row per build. */
#ifndef SNAPMAP_PLUS_MODMANIFEST_H
#define SNAPMAP_PLUS_MODMANIFEST_H

#include <stddef.h>
#include "map_carrier.h"
/* Build hash text: 64 lowercase hex characters. */
#define SH_MOD_HASH_CHARS 64

#define SH_MODMANIFEST_SALT_LEN   8
#define SH_MODMANIFEST_SALT_CAP   (SH_MODMANIFEST_SALT_LEN + 1)
#define SH_MODMANIFEST_HASH_CAP   (SH_MOD_HASH_CHARS + 1)

/* A row is compact JSON: pinned {"mod":"<salt>","name","size","objects":{"<root>":"<unit>"},"whole"?,"modules"?,"choices"?}
 * or local {"mod":"<salt>","name","size","local":true,"names":[...],"attached"?,"choices"?}. A row with an "id" or "bundle"
 * member, or a "<salt>@<build>" mod, is an earlier shape and is refused. All its text is heap strings of their own length. */
typedef struct sh_modmanifest_row {
    char salt[SH_MODMANIFEST_SALT_CAP];          /* the mod */
    char build[SH_MODMANIFEST_HASH_CAP];         /* its canonical manifest's SHA-256 */
    char *name;            /* display only: malloc'd, NULL for none (sh_modmanifest_row_name) */
    unsigned long long size;
    char **names;          /* malloc'd strings; owned by the row */
    size_t name_count;
    int attached;          /* a map-level row ("attached": true) */
    char *choices;         /* its "choices": "<slot>\t<salt>\n" per entry, in text order; malloc'd, NULL for none */
    /* "labels": "<placed name>\t<label>\n" per entry, each placed object's name as its Toybox row shows it; malloc'd,
     * NULL for none. Display only: a notice names an object by it. */
    char *labels;
    /* Format 2: a pinned row names the units of the objects it places ("objects", and "whole" for a map-level row) and
     * its `build` is their composition hash; a local row ("local": true) follows its salt's local folder and names no
     * unit until a publish or an export pins it. */
    int pinned;
    size_t unit_count;
    char (*units)[SH_MOD_HASH_CHARS + 1];   /* malloc'd: the objects' units in root order, then "whole" */
    char **roots;          /* malloc'd, parallel to units: the author root name, "*" for whole */
    /* A pinned row's "modules": a JSON array with one record per placed mod module, {"name", "bounds":
     * [x0,y0,z0,x1,y1,z1], "portals": [{"bounds": [...6], "dir": "PORTAL_DIR_..."}]}, for a stand-in. Malloc'd, NULL for none. */
    char *modules;
    /* The content step only: 1 when a folder of the salt is installed but unusable (else it is not here), or
     * SH_MODMANIFEST_LOCAL_LACKS when a folder that is here lacks names, which stand in beside it. */
    int local_broken;
} sh_modmanifest_row;

#define SH_MODMANIFEST_LOCAL_LACKS 2
#define SH_MODMANIFEST_LOCAL_REFUSED 3

/* The name the author sees for the object the map places as `placed` (the row's "labels"; malloc'd), or NULL. */
char *sh_modmanifest_row_label(const sh_modmanifest_row *row, const char *placed);

/* Where a save finds the labels (modui.c, from the installed mods' Toybox rows): for each of `placed[0..n)` the name
 * its Toybox row shows, written to labels[k] (malloc'd) or left NULL when none is known. */
typedef void (*sh_modmanifest_labeler_fn)(const char *const *placed, size_t n, char **labels);
void sh_modmanifest_set_labeler(sh_modmanifest_labeler_fn fn);

typedef struct sh_modmanifest_list {
    sh_modmanifest_row *rows;
    size_t count;
    size_t capacity;
    /* What the map uses of Snapmap+ itself (SH_MODMANIFEST_USES_*), kept as the product's own entry in the "mods" part:
     * {"mod":"snapmap-plus","name":"Snapmap+","uses":[<words>]}. It is not a row, but a map that has one counts as modded. */
    unsigned uses;
    /* Rows of an earlier shape ("id" or "bundle" members): refused and also counted in `rejected`, and they name no unit. */
    unsigned earlier;
} sh_modmanifest_list;

#define SH_MODMANIFEST_PRODUCT              "snapmap-plus"
#define SH_MODMANIFEST_USES_MODULE_SIZES    1u   /* "module-sizes": a module resized past its stock size (grid_room.h) */
#define SH_MODMANIFEST_USES_NAVIGATION      2u   /* "navigation": a Blocking Box marked for AI navigation (nav_regions.h) */

/* The product features a map text uses (bound at install to map_uses.h sh_map_uses): `names` are its placed names
 * (sh_modmanifest_placed_names). Returns SH_MODMANIFEST_USES_* bits. */
typedef unsigned (*sh_modmanifest_uses_fn)(const char *json, size_t length, const char *const *names, size_t count);
void sh_modmanifest_set_uses(sh_modmanifest_uses_fn fn);

void sh_modmanifest_row_free(sh_modmanifest_row *row);
/* The row's display name, never NULL ("" for none). */
const char *sh_modmanifest_row_name(const sh_modmanifest_row *row);
/* Replace the row's display name with a copy of `name` (NULL or "": none). Returns 0 when memory runs out, leaving it
 * unchanged. */
int  sh_modmanifest_row_set_name(sh_modmanifest_row *row, const char *name);
void sh_modmanifest_list_free(sh_modmanifest_list *list);
/* Append a copy of `row` (its strings copied). Returns 0 on allocation failure. */
int  sh_modmanifest_list_add(sh_modmanifest_list *list, const sh_modmanifest_row *row);
/* Add `name` to the row's list unless an equal name (case-insensitive) is there. */
int  sh_modmanifest_row_add_name(sh_modmanifest_row *row, const char *name);
/* Make `row` a pinned row of `n` units (roots[i] -> units[i]; root "*" is the whole), and its build their
 * composition hash. 1 on success. */
int  sh_modmanifest_row_pin(sh_modmanifest_row *row, const char *const *roots, const char *const *units, size_t n);
/* The composition hash of `units` under `salt` (modcache.h): SHA-256 of "snapmap-plus/composition/1\n1\n", the salt
 * and "\n", then each distinct unit hash in byte order followed by "\n". 1 on success. */
int  sh_modmanifest_composition_hash(const char *salt, const char *const *units, size_t count,
                                     char out[SH_MODMANIFEST_HASH_CAP]);
/* The content cache's step of every load (modcache.h, bound at install): the rows just read and the names the map
 * places. It remembers the pinned rows and composes those whose composition is not on disk from what is here. */
typedef void (*sh_modmanifest_content_fn)(const struct sh_modmanifest_list *rows, char *const *placed, size_t count);
void sh_modmanifest_set_content(sh_modmanifest_content_fn fn);
/* Inside the content step, the map text it runs for (its length in *length), so the step can read what the map
 * saved of each placed object (its class and reference slots, for a stand-in); NULL outside the step. */
const char *sh_modmanifest_content_text(size_t *length);
/* Run the content step for a map text's pinned rows (`rows` read from `json`), with the names the map places, as
 * a load does before its rows resolve. A rawmap's mod check calls it before its hold, which fetches a pinned row's
 * units only when the content cache knows the row. */
void sh_modmanifest_content_for(const char *json, size_t length, const struct sh_modmanifest_list *rows);
/* The placeholder build of a local row (64 zeros): it never names a build, so the resolver takes the local folder. */
#define SH_MODMANIFEST_LOCAL_BUILD "0000000000000000000000000000000000000000000000000000000000000000"

/* 1 when `text` is exactly 64 lowercase hex characters. */
int sh_modmanifest_valid_hash(const char *text, size_t length);

/* Parse a row value. 1 on success; 0 with the reason in err (an earlier-shape row is refused). */
int sh_modmanifest_parse_value(const char *json, size_t length, sh_modmanifest_row *out,
                               char *err, size_t err_cap);
/* The compact row value, NUL-terminated, malloc'd (free). Every name is kept. */
char *sh_modmanifest_format_value(const sh_modmanifest_row *row, size_t *out_length);

/* Read every well-formed row of the member's "mods" part in `json` into `out`
 * (appended, duplicates by salt and build merged). Returns the number of rows
 * read; rows that do not parse are counted in *rejected. */
size_t sh_modmanifest_read(const char *json, size_t length, sh_modmanifest_list *out,
                           unsigned *rejected);

/* Put one row per entry of `rows` as the member's "mods" part (map_carrier.h). Returns an owned process-heap buffer
 * (HeapFree), or NULL with the reason in err; with count 0 it only removes the part (NULL when there was none). */
char *sh_modmanifest_write(const char *json, size_t length, const sh_modmanifest_list *rows,
                           size_t *out_length, char *err, size_t err_cap);

/* The placed logical names: string values keyed "inherit" or "moduleName" and string values inside an object keyed
 * "edit", decoded, deduplicated case-insensitively, empties skipped. Free with sh_modmanifest_names_free. Returns the
 * count, or 0 on a structural failure (*failed set). */
size_t sh_modmanifest_placed_names(const char *json, size_t length, char ***out_names,
                                   int *failed);
void sh_modmanifest_names_free(char **names, size_t count);

/* Read a built mod's mod.json: the "snapmap-plus" block's salt and build, the author name, the total size and every
 * "files" and "stock" provides.name (as `names`). 1 on success; 0 with the reason in err (a block without a salt or
 * build is not built). */
int sh_modmanifest_read_mod_json(const char *text, size_t length, sh_modmanifest_row *out,
                                 char *err, size_t err_cap);

/* Activate the rows' mods, each through the build that serves it (sh_modserve_row_build), else the one the editor
 * substitutes, else its own hash. Returns modserve_activate_bundles' result. Main thread. */
int sh_modmanifest_activate(const sh_modmanifest_list *rows);

/* Read, retain and activate the map's rows. The parse detour removes the complete carrier. */
void sh_modmanifest_prepare_load(const char *json, size_t length);
/* The load path already read and checked these rows. Takes their ownership; NULL clears the active scope. */
void sh_modmanifest_load_rows(const char *json, size_t length, sh_modmanifest_list *rows, unsigned rejected);
size_t sh_modmanifest_read_member(const char *json, const sh_carrier_span *mods, sh_modmanifest_list *out,
                                  unsigned *rejected);

/* The SerializeToJson step: compute the placed names, ask modserve_bundles_for_names for their bundles, add the
 * current manifest's map-level rows, and return the text with one row per bundle. NULL when no change is needed or on
 * failure (logged); the engine's bytes are then saved unchanged. */
char *sh_modmanifest_on_save(const char *json, size_t length, size_t *out_length);

/* The map scope's hook (modui.c): called at every load with the rows just read and, when `armed` answers above 0,
 * the placed names (`when` SH_MODSERVE_AT_LOAD), before activation; at every save with the rows written (names NULL,
 * SH_MODSERVE_AT_SAVE). NULL unbinds. */
typedef void (*sh_modmanifest_scope_fn)(const sh_modmanifest_list *rows, const char *const *names, size_t name_count,
                                        int when);
void sh_modmanifest_set_scope(sh_modmanifest_scope_fn scope, int (*armed)(void));

/* The builds the map scope lists after a load's hook (modui.c: sh_modserve_scope_hashes): up to `cap` copied into
 * `out` (NULL to count), returning the count. A load activates its rows and these builds in one pass, so a map that
 * places a mod without a row still registers it. NULL unbinds: rows alone. */
typedef int (*sh_modmanifest_listed_fn)(char (*out)[SH_MODMANIFEST_HASH_CAP], int cap);
void sh_modmanifest_set_listed(sh_modmanifest_listed_fn listed);

/* The build that serves a row at save (modui.c: sh_modserve_row_build), for `build` of `salt`: fills salt, build,
 * name and size and returns 1, or 0 when nothing installed answers (the row is written as it is). */
typedef int (*sh_modmanifest_resolve_fn)(const char *build, const char *salt, sh_modmanifest_row *out);
void sh_modmanifest_set_resolver(sh_modmanifest_resolve_fn resolve);

/* The editor's substitute for a row whose pinned build nothing serves: another build of the same salt serves for
 * this editor session (the row keeps its pin until a save re-pins it). Answers per row: 1 with the build in
 * hash_out, 0 none (*why: the gate's sentence, malloc'd, or NULL). `placed` says the map's placed entities name
 * the row's namespace, in which case the engine's validator refuses the map. Bound by modpublished.c; only a parse
 * made for the editor (sh_modmanifest_set_editor_load 1) substitutes here. */
typedef int (*sh_modmanifest_substitute_fn)(const sh_modmanifest_row *row, char *hash_out, size_t hash_cap,
                                            int placed, char **why);
void sh_modmanifest_set_substitute(sh_modmanifest_substitute_fn substitute);
void sh_modmanifest_set_editor_load(int editor);
/* The cause sentences found for rows nothing serves ("MODS: map: <salt>: <sentence>") are held until the gate has
 * decided. Say 0 drops them (a gate that took the parse says its own cause), 1 logs them (an editor parse); a new load
 * replaces them. Returns the number logged or dropped. */
int  sh_modmanifest_load_notes_flush(int say);
int  sh_modmanifest_load_notes_pending(void);
/* The first held note whose row the map's placed entities name (the editor refuses such a map): its sentence in *out
 * (malloc'd, without the "MODS: map:" prefix) when out is not NULL, returning 1; 0 when none. Cleared with the notes. */
int  sh_modmanifest_load_refusal(char **out);
/* Refuse the map being parsed before anything reads it (an earlier-shape map; the open map's manifest stays):
 * `sentence` replaces the held notes as the load's one note and its refusal. */
void sh_modmanifest_load_refuse(const char *sentence);
/* The build serving in place of a declared one in the open map: 1 with it in
 * out, 0 when `declared` is served as itself. */
int  sh_modmanifest_substituted(const char *declared, char *out, size_t cap);

/* A copy of the manifest of the map last parsed (rows from its text). Free
 * with sh_modmanifest_list_free. Returns the row count. */
size_t sh_modmanifest_current(sh_modmanifest_list *out);
/* Drop the current manifest's rows naming `build` (exact hash), as the Mods tab does when it removes that mod from
 * the open map. Returns the number of rows dropped. */
size_t sh_modmanifest_current_remove(const char *build);
/* Attach a mod to the open map: mark the current row of `row->salt` at `row->build` map-level, or add `row` as a
 * map-level row (names dropped); the next save writes it. Returns 1 attached now, 0 already attached, -1 invalid or
 * out of memory. */
int    sh_modmanifest_current_attach(const sh_modmanifest_row *row);
/* Add `row` (its names kept) to the current manifest, or merge its names into the row of the same salt and build.
 * 1 added or merged, -1 invalid or out of memory. The next save writes it (a placed prefab's pinned row). */
int    sh_modmanifest_current_add_row(const sh_modmanifest_row *row);
/* A copy of the current manifest's map-level rows. Returns the row count. */
size_t sh_modmanifest_current_attached(sh_modmanifest_list *out);
/* The open map's pinned row of `salt` becomes a local row of the installed build `build` naming `names` (the Mods tab's
 * Update). 1 changed, 0 no pinned row of that salt, -1 bad arguments or no memory. */
int sh_modmanifest_current_make_local(const char *salt, const char *build, const char *const *names, size_t n);
/* The version rule on the open map: with `names` the names it places, every mod placed both from its pinned units and
 * from the installed version moves to the installed version. The number of mods moved, -1 on failure. Main thread. */
int sh_modmanifest_version_rule(const char *const *names, size_t count);
/* Forget the current manifest (tests). */
void sh_modmanifest_reset(void);
/* The open map closed: its rows, substitutions and registry choices go, so nothing reads them as the next map's. */
void sh_modmanifest_current_close(void);
/* Put back the state the last load replaced (rows, registry choices, substitutions), once, after a refused launch.
 * Returns the rows restored, -1 when nothing is kept. */
int    sh_modmanifest_current_restore_previous(void);

/* The open map's registry choices (modregistry.h): read from the rows at every load, written at every save on the
 * row of the salt each names (a choice naming a salt with no row is dropped, logged), and given to the serving index
 * (sh_modserve_scope_set_choices) before the listing. */
/* Set (salt) or clear (salt NULL or "") the choice for `slot` and give the choices to the serving index. Returns the
 * builds whose serving changed (>= 0), -1 for an invalid slot or salt. */
int    sh_modmanifest_current_choose(const char *slot, const char *salt);
/* 1 with the salt chosen for `slot` in `salt`, 0 when there is none. */
int    sh_modmanifest_current_choice(const char *slot, char *salt, size_t cap);
/* The choices as "slot\tsalt\n" lines in *out (malloc'd). Returns their count. */
size_t sh_modmanifest_current_choices(char **out);
/* The choices a list of rows carries, merged (the first row naming a slot wins), as "slot\tsalt\n" lines in *out
 * (malloc'd, NULL for none). Returns their count. */
size_t sh_modmanifest_rows_choices(const sh_modmanifest_list *rows, char **out);

/* The rows a save of a map placing `names` would write (the placed names' closure, the map-level rows, the pinned
 * rows kept), without a map text. Returns the count, -1 on failure; free *out. */
int sh_modmanifest_rows_for(const char *const *names, size_t count, sh_modmanifest_list *out);

/* The publish's pin step (modpin.h), bound while a publish is armed: the save calls it with a copy of the rows it
 * writes and the saved text, to pin that copy for the published copy alone. Returns 1 when it pinned them. */
typedef int (*sh_modmanifest_pin_fn)(sh_modmanifest_list *rows, const char *json, size_t length);
void sh_modmanifest_set_pin(sh_modmanifest_pin_fn pin);

/* Where the version rule's one-line notices go (modpublished.h sh_modpublished_notice). NULL: the log only. */
void sh_modmanifest_set_notice(void (*fn)(const char *text));
/* Whether the installed version of `salt` carries the placed name `name` (modserve.h sh_modserve_root_carries): 1 yes.
 * NULL (unbound): the version rule never moves a mod. */
void sh_modmanifest_set_carries(int (*fn)(const char *salt, const char *name));
/* Whether a pinned row's composition `build` is here whole (every unit present): 1 yes. A save keeps the recorded
 * spawn-type pins of a row whose content is not here, since the listing cannot say its demons' types. NULL: whole. */
void sh_modmanifest_set_whole(int (*fn)(const char *build));

/* The builds a map declares to the map scope, in order: per row, the build that serves it (the editor's substitute, else
 * the resolver's, else its composition), then, given `names`, each build their closure reaches whose salt no row declares.
 * Returns the count and the hashes in *out (malloc'd), or -1 when memory ran out. */
int sh_modmanifest_listing(const sh_modmanifest_list *rows, const char *const *names, size_t name_count,
                           char (**out)[SH_MODMANIFEST_HASH_CAP]);

#endif /* SNAPMAP_PLUS_MODMANIFEST_H */
