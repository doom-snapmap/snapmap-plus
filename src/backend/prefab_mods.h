/* prefab_mods.h -- a prefab's details and the mods it needs.
 *
 * A prefab is one file. Its description, tags, a module prefab's summary and
 * the mods it needs live in the file itself, in the product's top-level
 * "snapmapPlus" block after the engine's own members (prefab_block.h):
 *
 *   "snapmapPlus":{"format":1,"description":"","tags":[],
 *                  "mods":[{"mod":"<salt>@<build hash>","name","size","names"}...],
 *                  "summary":{...}}            (module prefabs only)
 *
 * The mods are found exactly as a map's save finds them (modmanifest.h): the
 * placed names of the prefab text (every "inherit", "moduleName" and string
 * inside an "edit" object) go through the serving layer's save-time closure,
 * which names every admitted build that provides one, with the builds its
 * needs reach. Each build becomes one row in the map manifest's form
 * (modmanifest.h), its name from the build. Create writes the block
 * once the engine has serialized the prefab, a module prefab's save writes it
 * with the file, and the Prefabs tab's description and tag edits rewrite it in
 * place. The engine never re-saves a prefab file, so the block persists; every
 * reader strips it before the engine or the module merge sees the text.
 *
 * A prefab with a <name>.meta.json sidecar and no block, or a block whose
 * rows name "id" and "bundle", was saved by an earlier version (ONE-SHAPE
 * rule 6). Nothing of it is read: its details show nothing, its placement and
 * export are refused with SH_PREFAB_EARLIER_TEXT, and a description edit
 * writes nothing. No sidecar is ever written.
 *
 * The page and the slot see the details as one flat object, the "view":
 * {"desc","tags",...summary members...,"mods"}. A block reads as a view and a
 * view composes into a block.
 *
 * Load and Place resolve the rows before the engine sees the prefab, in the
 * open map's own context. Nothing is fetched or installed (D35: a prefab
 * carries units, not mods):
 *   a local row         (a prefab made on this computer: its mod's folder)
 *                       served by the folder at the root: the mods are
 *                       activated for the open map, and its placed objects
 *                       carry a mod into the map as placed objects do (the
 *                       save writes the row, a publish pins those objects'
 *                       units); only a mod the prefab uses as a whole (its
 *                       map-level row, or a row naming nothing placed) that
 *                       the open map does not list is attached to it (as the
 *                       Mods tab's Use in this map), with one console line
 *                       when that fails;
 *   a pinned row        (an imported prefab's units, all in the content cache)
 *                       joins the open map as a pinned row of those units,
 *                       composed and declared (ops.use_pinned; SPEC 5.16
 *                       step 3). A mod the map already uses at another
 *                       version is left to the installed version when it
 *                       carries every object the prefab places (D23), else
 *                       the placement is refused;
 *   content not here    the placement is refused with one sentence naming
 *                       the mods, printed and toasted, asking for the
 *                       prefab's .zip export.
 * A module prefab whose module no row provides and the palette lacks (DLC the
 * player does not own) is refused first, with the module guard's message
 * (module_prefab_map.h): nothing can install it.
 *
 * Export always writes one ZIP (modexport.h): <name>.json, the prefab file
 * with its block, whose rows the export pins to units, and every unit and file
 * those rows name. Import takes that ZIP, or a .json inside an extracted
 * export folder, adds the prefab to the library, then puts its units and files
 * into the content cache (modexport_read_prefab); when that fails the prefab is
 * taken out again, so a failed import leaves nothing. Importing an export whose
 * prefab and content are already here writes nothing.
 *
 * Paths and prefab names are UTF-8 end to end, opened with the wide file
 * calls; only the console's arguments are ANSI, converted on entry.
 */
#ifndef SH_PREFAB_MODS_H
#define SH_PREFAB_MODS_H

#include <stddef.h>
#include "modmanifest.h"
#include "prefab_block.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SH_PREFAB_MODS_KEY      "mods"
#define SH_PREFAB_MODS_FILE_MAX (16u * 1024u * 1024u)

/* MODS_RESOLVE's answers (snapmap_plus_iface.h). */
#define SH_PREFAB_MODS_NO_ANSWER     0
#define SH_PREFAB_MODS_PLACE         1
#define SH_PREFAB_MODS_HELD          2
#define SH_PREFAB_MODS_NEEDS_MODULE (-6)
#define SH_PREFAB_MODS_REFUSED      (-7)

/* Where a prefab's details came from (META_READ's answer): its block, none,
 * or an earlier version's sidecar beside a file with no block (not read). */
#define SH_PREFAB_META_NONE    0
#define SH_PREFAB_META_BLOCK   1
#define SH_PREFAB_META_EARLIER 2

/* A prefab saved by an earlier version: the rows functions return this. */
#define SH_PREFAB_ROWS_EARLIER (-2)
#define SH_PREFAB_EARLIER_TEXT "This prefab was saved by an earlier version of Snapmap+ and cannot be used. " \
    "Ask its author to export it again."

/* ------------------------------------------------------------ rows */

/* The rows a prefab text needs (its block ignored): the map manifest's
 * save-time closure over its placed names, sorted by salt. Returns the row
 * count, -1 when the text does not read or the serving layer reported an
 * invalid bundle. */
int   sh_prefab_mods_closure(const char *text, size_t len, sh_modmanifest_list *out);

/* The rows of a view's or a block's "mods" array (duplicates by salt and
 * build merged). Returns the count (0 for no body or no "mods"), -1 when the
 * body is not a JSON object or "mods" is not an array, SH_PREFAB_ROWS_EARLIER
 * when a row names "id" or "bundle"; other rows that do not read are skipped. */
int   sh_prefab_mods_view_rows(const char *view, size_t len, sh_modmanifest_list *out);

/* A view with "mods" set to `rows` (removed when there are none), every other
 * member kept as it was. A NULL or empty body starts from {"desc":"","tags":[]}.
 * malloc'd, NUL-terminated; NULL when the body is not a JSON object. */
char *sh_prefab_mods_view_set(const char *view, size_t len, const sh_modmanifest_list *rows, size_t *out_len);

/* 1 when a row names `name` among the names it provides (case-insensitive). */
int   sh_prefab_mods_row_provides(const sh_modmanifest_list *rows, const char *name);

/* A row's name, the salt when it has no name (the refusal's label). */
const char *sh_prefab_mods_label(const sh_modmanifest_row *row);

/* {"count":N,"missing":M,"mods":[{"id","salt","build","size",
 * "installed","state","cause"}...]}, "id" the row's name (else its salt),
 * with each row's state under the asset rule
 * (row_state): "served", "missing" or "incomplete", and the refusal sentence
 * for the last two; a build a ZIP's import holds counts as served (its
 * placement installs it at the root). malloc'd. */
char *sh_prefab_mods_status_json(const sh_modmanifest_list *rows, size_t *missing);

/* ------------------------------------------------------------ the block */

/* The block for a view: "description" and "tags" from the view's "desc" and
 * "tags", "mods" from `rows` (NULL keeps the view's "mods"), and every other
 * member of the view under "summary" (left out when there is none). A NULL,
 * empty or unreadable view composes an empty block. malloc'd. */
char *sh_prefab_mods_block_compose(const char *view, size_t len, const sh_modmanifest_list *rows, size_t *out_len);

/* The view of a block (its object text). malloc'd; NULL when the block is not
 * a JSON object. */
char *sh_prefab_mods_block_view(const char *block, size_t len, size_t *out_len);

/* A prefab's details as a view: its block when the text has one (tail: the
 * text is the end of a larger file), else NULL, with *source
 * SH_PREFAB_META_EARLIER when a sidecar exists at meta_path (may be NULL).
 * *source receives an SH_PREFAB_META_* value. malloc'd. */
char *sh_prefab_mods_view_of(const char *text, size_t len, int tail, const char *meta_path, int *source,
                             size_t *out_len);

/* The rows a prefab names: its block's. Returns the count, -1 unreadable,
 * SH_PREFAB_ROWS_EARLIER for a prefab an earlier version saved. */
int   sh_prefab_mods_rows_of(const char *text, size_t len, const char *meta_path, sh_modmanifest_list *out);

/* The prefab text with the block composed from `view` and `rows` (NULL keeps
 * the view's). malloc'd; NULL when the text is not a JSON object. */
char *sh_prefab_mods_embed(const char *text, size_t len, const char *view, size_t view_len,
                           const sh_modmanifest_list *rows, size_t *out_len);

/* A copy of the prefab text without its block. malloc'd. */
char *sh_prefab_mods_strip_copy(const char *text, size_t len, size_t *out_len);

/* ------------------------------------------------------------ files */

/* Read a whole file (at most SH_PREFAB_MODS_FILE_MAX). malloc'd,
 * NUL-terminated; NULL when it cannot be read. */
char *sh_prefab_mods_read_file(const char *path_utf8, size_t *len);

/* Write a file through a temporary beside it. 1 written. */
int   sh_prefab_mods_write_file(const char *path_utf8, const char *data, size_t len);

/* Create: `text` is the prefab the engine just serialized to `path` (the host
 * has removed its comments, sh_prefab_block_strip_comments; any left are
 * removed here). The closure over it goes into a fresh block, written into
 * the file with the plain JSON. *view_out (optional, malloc'd) receives the
 * view written. Returns the row count, -1 when nothing could be recorded
 * (the file is left as the host wrote it). */
int   sh_prefab_mods_record(const char *text, size_t len, const char *path, char **view_out);

/* A module prefab's save: the cut file `prefab` with a block composed from its
 * summary view and the closure over it. malloc'd (NULL when the cut is not an
 * object); *view_out (optional) the view written, *count the row count. */
char *sh_prefab_mods_module_file(const char *prefab, size_t len, const char *summary, size_t *out_len, char **view_out,
                                 int *count);

/* The Prefabs tab's description and tag edit: `body` is a view ("" clears
 * both); only its "desc" and "tags" are taken. The block of the prefab at
 * `path` is rewritten in place (written fresh when it has none). A prefab an
 * earlier version saved (a sidecar at meta_path and no block) is not
 * written. 1 written, -1 failed. */
int   sh_prefab_mods_meta_write(const char *body, size_t len, const char *path, const char *meta_path);

/* ------------------------------------------------------------ resolve */

/* Bring a prefab's mods into the open map before it is placed (main thread;
 * see the top of this file). `rows` are the prefab's; `text` is the prefab
 * file (for a module prefab's module check; its block is ignored). Returns an
 * SH_PREFAB_MODS_* answer; msg receives what the Prefabs tab shows for
 * NEEDS_MODULE, REFUSED and HELD; *seq the placement to poll when HELD. */
int   sh_prefab_mods_resolve_main(const char *text, size_t len, const sh_modmanifest_list *rows, const char *name,
                                  int *seq, char **msg);

/* A held placement: 2 waiting, 1 ready (its mods are installed and in the
 * open map: place it now), 0 refused or dropped. Any thread. */
int   sh_prefab_mods_poll(int seq);

/* ------------------------------------------------------------ export, import */

/* Export the prefab at prefab_path (an earlier version's sidecar at
 * meta_path refuses it) as `name` (UTF-8) to zip_path (UTF-8). Rows come from
 * its block, else from the closure over the prefab. The ZIP carries the file's
 * bytes as they are on disk; a prefab with no block, or whose rows had to be
 * found or moved to the current builds, is carried with its block written for
 * the rows the ZIP carries (the file on disk is not changed). 1 written, -1
 * refused; msg says which and why. */
int   sh_prefab_mods_export(const char *prefab_path, const char *meta_path, const char *name, const char *zip_utf8,
                            char **msg);

/* Import a prefab's export (a .zip, or a .json inside an extracted export
 * folder; UTF-8) into prefabs_dir (UTF-8, with or without a trailing
 * separator): add the prefab under the .json's file name, then install the
 * carried mod folders nothing serves (modexport_read_prefab); a failed
 * install takes the prefab out again. A prefab of that name with the same
 * bytes is left alone; one with other bytes gets the next free "<name> (N)".
 * 1 added, 0 already there (nothing written), -1 refused (nothing of the
 * import stays). name_out receives the name it has in the library. */
int   sh_prefab_mods_import(const char *src_utf8, const char *prefabs_dir, char **name_out, char **msg);

/* 1 when `name` can be a prefab file name: the one rule the Prefabs tab
 * uses too (prefab_block.h, sh_prefab_block_valid_name). */
int   sh_prefab_mods_valid_name(const char *name);

/* ------------------------------------------------------------ engine */

/* What the steps above ask of the game. The product binds its defaults on
 * first use (none in a SH_PREFAB_MODS_PURE build); tests bind fakes. */
typedef struct sh_prefab_mods_ops {
    /* The asset rule (modserve.h sh_modserve_row_served_for, subject
     * "prefab"): SH_MODPUB_ROW_*, the refusal sentence in `cause`. */
    int  (*row_state)(const sh_modmanifest_row *row, char **cause);   /* *cause malloc'd (cause may be NULL) */
    int  (*listed)(const sh_modmanifest_row *row);                 /* 1 the open map lists the build serving it */
    int  (*attach)(const sh_modmanifest_row *row, char **note);   /* 1 attached, 0 already, -1 failed (*note malloc'd) */
    int  (*activate)(const sh_modmanifest_list *rows);             /* register the rows' decls for the open map */
    int  (*module_known)(const char *module_name);                 /* the module guard: 1, 0, -1 */
    void (*print)(const char *line);                               /* one console line */
    void (*toast)(const char *text);                               /* one editor toast */
    int  (*refresh)(void);                                         /* after an import installed bundles */
    int  (*call_main)(int (*fn)(void *), void *ctx);               /* run fn on the game thread; 1 ran */
    /* An imported prefab's pinned rows (their units all in the content cache) join the open map: composed, added to
     * the current manifest with their names, declared. A row of a salt the map already uses at another version is
     * left to the installed version when it carries every object the prefab places (D23), else refuses with *cause
     * (malloc'd, a clause after "was not placed: "). 1 used, -1 refused. Main thread. */
    int  (*use_pinned)(const sh_modmanifest_list *pinned, char **cause);
} sh_prefab_mods_ops;
void  sh_prefab_mods_bind(const sh_prefab_mods_ops *ops);

/* Where the console finds prefabs (tests; NULL restores
 * %LOCALAPPDATA%\snapmap-plus\prefabs\). */
void  sh_prefab_mods_set_root(const char *dir);

/* `sh_prefab`: argv[0] is the verb (NULL or "help" prints the verbs):
 *   mods "<name>"                 the prefab's mods and whether each is installed, in
 *                                 the open map's context (it changes nothing)

 *   export "<name>" "<path.zip>"  the prefab with its mods, as one ZIP
 *   import "<path>"               a .zip, or a .json in an extracted export folder:
 *                                 install the mods, add the prefab to the library
 * A name may be "<folder>/<name>". Game thread. */
void  sh_prefab_mods_command(int argc, const char *const *argv);

#define SH_PREFAB_CMD_NAME "sh_prefab"
#define SH_PREFAB_CMD_HELP "Prefabs and their mods: 'sh_prefab mods \"<name>\"' lists the mods a prefab needs and whether " \
    "each is installed; 'sh_prefab export \"<name>\" \"<path.zip>\"' writes the prefab with its mods to one ZIP; " \
    "'sh_prefab import \"<path.zip or path.json>\"' installs the mods and adds the prefab to the library (a .json " \
    "imports the extracted export folder it is in)."

struct sh_module_prefab_call;
/* The details and mods operations of the module_prefab slot
 * (snapmap_plus_iface.h): MODS_RECORD, MODS_STATUS, MODS_RESOLVE, MODS_POLL,
 * META_WRITE, META_READ, EXPORT and IMPORT. Returns 0 for any other op. */
int   sh_prefab_mods_slot(struct sh_module_prefab_call *call);

#ifndef SH_PREFAB_MODS_PURE
#include "commands.h"
/* The console handler. */
void  h_sh_prefab(idCmdArgs *args);
#endif

#ifdef SH_PREFAB_MODS_TESTING
void  sh_prefab_mods_test_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
