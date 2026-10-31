/* prefab_mods.h -- a prefab's details and the mods it needs.
 *
 * A prefab is one file. Its description, tags, a module prefab's summary and
 * the mods it needs live in the file itself, in the product's top-level
 * "snapmapPlus" block after the engine's own members (prefab_block.h):
 *
 *   "snapmapPlus":{"format":1,"description":"","tags":[],
 *                  "mods":[{"id","version","bundle","size","names"}...],
 *                  "summary":{...}}            (module prefabs only)
 *
 * The mods are found exactly as a map's save finds them (modmanifest.h): the
 * placed names of the prefab text (every "inherit", "moduleName" and string
 * inside an "edit" object) go through the serving layer's save-time closure,
 * which names every admitted bundle that provides one, with the bundles its
 * needs reach. Each bundle becomes one row in the map manifest's form
 * (aliases resolve through the store as for a map). Create writes the block
 * once the engine has serialized the prefab, a module prefab's save writes it
 * with the file, and the Prefabs tab's description and tag edits rewrite it in
 * place. The engine never re-saves a prefab file, so the block persists; every
 * reader strips it before the engine or the module merge sees the text.
 *
 * A prefab saved before this has no block. When a <name>.meta.json sidecar
 * sits beside it, its description, tags, summary and "mods" rows are read
 * instead, read-only: the first description or tag edit writes the block into
 * the file and leaves the sidecar on disk as it was. No sidecar is written.
 *
 * The page and the slot see the details as one flat object, the "view": the
 * sidecar's shape, {"desc","tags",...summary members...,"mods"}. A block reads
 * as a view and a view composes into a block.
 *
 * Load and Place resolve the rows before the engine sees the prefab:
 *   every row installed   the mods are activated for the open map, a mod the
 *                         open map does not list is attached to it (a
 *                         map-level row, as the Mods tab's Use in this map)
 *                         with one console line, and the placement goes on;
 *   some row missing      the placement is held (modpublished.h,
 *                         sh_modpub_prefab_hold): the mod store is asked;
 *                         when it has them all the player is asked Yes/No, Yes
 *                         downloads and installs them, the ready step above
 *                         runs and the Prefabs tab places the prefab; when it
 *                         lacks any or cannot be reached, the placement is
 *                         refused with the native notice and one console line
 *                         naming each mod and asking for the prefab's ZIP;
 *   no gates bound        refused the same way, without asking the store.
 * A module prefab whose module no row provides and the palette lacks (DLC the
 * player does not own) is refused first, with the module guard's message
 * (module_prefab_map.h): nothing can install it.
 *
 * Export always writes one ZIP (modexport.h): <name>.json, the prefab file
 * with its block, and every bundle its rows name. Import takes that ZIP, or a
 * .json inside an extracted export folder, installs the bundles the store
 * lacks and adds the prefab to the library. Importing an export whose mods
 * and prefab are already there writes nothing.
 *
 * Paths: prefab and sidecar paths are ANSI, as the Prefabs tab resolves them;
 * ZIP paths and prefab names given to export are UTF-8 (the file dialog's),
 * except on the console, whose arguments are ANSI.
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

/* Where a prefab's details came from (META_READ's answer). */
#define SH_PREFAB_META_NONE    0
#define SH_PREFAB_META_BLOCK   1
#define SH_PREFAB_META_SIDECAR 2

/* ------------------------------------------------------------ rows */

/* The rows a prefab text needs (its block ignored): the map manifest's
 * save-time closure over its placed names, sorted by id. Returns the row
 * count, -1 when the text does not read or the serving layer reported an
 * invalid bundle. */
int   sh_prefab_mods_closure(const char *text, size_t len, sh_modmanifest_list *out);

/* The rows of a view's or a block's "mods" array (duplicates by hash merged).
 * Returns the count (0 for no body or no "mods"), -1 when the body is not a
 * JSON object or "mods" is not an array; rows that do not read are skipped. */
int   sh_prefab_mods_sidecar_rows(const char *side, size_t len, sh_modmanifest_list *out);

/* A view with "mods" set to `rows` (removed when there are none), every other
 * member kept as it was. A NULL or empty body starts from {"desc":"","tags":[]}.
 * malloc'd, NUL-terminated; NULL when the body is not a JSON object. */
char *sh_prefab_mods_sidecar_set(const char *side, size_t len, const sh_modmanifest_list *rows, size_t *out_len);

/* 1 when a row names `name` among the names it provides (case-insensitive). */
int   sh_prefab_mods_row_provides(const sh_modmanifest_list *rows, const char *name);

/* "id version" for a row (the refusal's label). */
void  sh_prefab_mods_label(const sh_modmanifest_row *row, char *out, size_t cap);

/* {"count":N,"missing":M,"mods":[{"id","version","bundle","size","installed"}...]}
 * with each row's state from the bound bundle_state. malloc'd. */
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
 * text is the end of a larger file), else the sidecar at meta_path (may be
 * NULL), else NULL. *source receives an SH_PREFAB_META_* value. malloc'd. */
char *sh_prefab_mods_view_of(const char *text, size_t len, int tail, const char *meta_path, int *source,
                             size_t *out_len);

/* The rows a prefab names: its block's, else its sidecar's. Returns the count,
 * -1 unreadable. */
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
char *sh_prefab_mods_read_file(const char *path_acp, size_t *len);

/* Write a file through a temporary beside it. 1 written. */
int   sh_prefab_mods_write_file(const char *path_acp, const char *data, size_t len);

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
 * `path` is rewritten in place (from its sidecar at meta_path when it has no
 * block yet; the sidecar is left as it is). 1 written, -1 failed. */
int   sh_prefab_mods_meta_write(const char *body, size_t len, const char *path, const char *meta_path);

/* ------------------------------------------------------------ resolve */

/* Bring a prefab's mods into the open map before it is placed (main thread;
 * see the top of this file). `rows` are the prefab's; `text` is the prefab
 * file (for a module prefab's module check; its block is ignored). Returns an
 * SH_PREFAB_MODS_* answer; msg receives what the Prefabs tab shows for
 * NEEDS_MODULE, REFUSED and HELD; *seq the placement to poll when HELD. */
int   sh_prefab_mods_resolve_main(const char *text, size_t len, const sh_modmanifest_list *rows, const char *name,
                                  int *seq, char *msg, size_t cap);

/* A held placement: 2 waiting, 1 ready (its mods are installed and in the
 * open map: place it now), 0 refused or dropped. Any thread. */
int   sh_prefab_mods_poll(int seq);

/* ------------------------------------------------------------ export, import */

/* Export the prefab at prefab_path (sidecar at meta_path, may be absent) as
 * `name` (UTF-8) to zip_path (UTF-8). Rows come from its block (else the
 * sidecar), else from the closure over the prefab. The ZIP carries the file's
 * bytes as they are on disk; a prefab with no block, or whose rows had to be
 * found or moved to the current builds, is carried with its block written for
 * the rows the ZIP carries (the file on disk is not changed). 1 written, -1
 * refused; msg says which and why. */
int   sh_prefab_mods_export(const char *prefab_path, const char *meta_path, const char *name, const char *zip_utf8,
                            char *msg, size_t cap);

/* Import a prefab's export (a .zip, or a .json inside an extracted export
 * folder; UTF-8) into prefabs_dir (ANSI, with or without a trailing
 * separator): install the bundles the store lacks, then add the prefab under
 * the .json's file name. A prefab of that name with the same bytes is left
 * alone; one with other bytes gets the next free "<name> (N)". 1 added, 0
 * already there (nothing written), -1 refused. name_out receives the name it
 * has in the library. */
int   sh_prefab_mods_import(const char *src_utf8, const char *prefabs_dir, char *name_out, size_t name_cap,
                            char *msg, size_t cap);

/* 1 when `name` can be a prefab file name: 1-120 characters, none of
 * \ / : * ? " < > | or a control character, no leading or trailing space or
 * dot, not a device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9, with or without
 * an extension) and not ending in ".meta". */
int   sh_prefab_mods_valid_name(const char *name);

/* ------------------------------------------------------------ engine */

/* What the steps above ask of the game. The product binds its defaults on
 * first use (none in a SH_PREFAB_MODS_PURE build); tests bind fakes. */
typedef struct sh_prefab_mods_ops {
    int  (*bundle_state)(const char *hash);                        /* 1 installed and admitted */
    int  (*restore)(const char *const *hashes, int n);             /* the recycle bin; how many came back */
    int  (*listed)(const char *hash);                              /* 1 the open map lists the bundle */
    int  (*attach)(const sh_modmanifest_row *row, char *note, size_t cap);   /* 1 attached, 0 already, -1 failed */
    int  (*activate)(const char *const *hashes, int n);            /* register the bundles' decls for the open map */
    int  (*module_known)(const char *module_name);                 /* the module guard: 1, 0, -1 */
    void (*print)(const char *line);                               /* one console line */
    void (*toast)(const char *text);                               /* one editor toast */
    int  (*refresh)(void);                                         /* after an import installed bundles */
    int  (*call_main)(int (*fn)(void *), void *ctx);               /* run fn on the game thread; 1 ran */
} sh_prefab_mods_ops;
void  sh_prefab_mods_bind(const sh_prefab_mods_ops *ops);

/* Where the console finds prefabs (tests; NULL restores
 * %LOCALAPPDATA%\snapmap-plus\prefabs\). */
void  sh_prefab_mods_set_root(const char *dir);

/* `sh_prefab`: argv[0] is the verb (NULL or "help" prints the verbs):
 *   mods "<name>"                 the prefab's rows and whether each is installed
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
