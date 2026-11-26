/* prefab_mods.h -- a prefab's details and the mods it needs. A prefab is one file: its description, tags, a
 * module prefab's summary and the mods it needs (rows found as a map's save finds them, modmanifest.h) live in
 * the product's top-level "snapmapPlus" block after the engine's own members (prefab_block.h), which every
 * reader strips before the engine sees the text. Load and Place resolve the rows in the open map's context
 * and fetch nothing: a local row's folder is activated, a pinned row's units join the map, absent content is
 * refused. Export writes one ZIP (modexport.h), import reads it; paths are UTF-8, only console arguments are ANSI. */
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

/* Where a prefab's details came from (META_READ's answer): its block, none, or a <name>.meta.json sidecar
 * beside a file with no block (not read, no sidecar is ever written). */
#define SH_PREFAB_META_NONE    0
#define SH_PREFAB_META_BLOCK   1
#define SH_PREFAB_META_EARLIER 2

/* A prefab in the sidecar form (or a block whose rows name "id" and "bundle"): the rows functions return this. */
#define SH_PREFAB_ROWS_EARLIER (-2)
#define SH_PREFAB_EARLIER_TEXT "This prefab was saved by an earlier Snapmap+; run the Snapmap+ installer to update it."

/* The rows a prefab text needs (its block ignored): the map manifest's save-time closure over its placed
 * names, sorted by salt. Returns the row count, -1 when the text does not read or a bundle is invalid. */
int   sh_prefab_mods_closure(const char *text, size_t len, sh_modmanifest_list *out);

/* The rows of a view's or a block's "mods" array (duplicates by salt and build merged): the count (0 for no
 * body or "mods"), -1 for a non-object body or non-array "mods", SH_PREFAB_ROWS_EARLIER when a row names "id"
 * or "bundle"; other unreadable rows are skipped. */
int   sh_prefab_mods_view_rows(const char *view, size_t len, sh_modmanifest_list *out);

/* A view with "mods" set to `rows` (removed when there are none), other members kept; a NULL or empty body
 * starts from {"desc":"","tags":[]}. malloc'd; NULL when the body is not a JSON object. */
char *sh_prefab_mods_view_set(const char *view, size_t len, const sh_modmanifest_list *rows, size_t *out_len);

/* 1 when a row names `name` among the names it provides (case-insensitive). */
int   sh_prefab_mods_row_provides(const sh_modmanifest_list *rows, const char *name);

/* A row's name, the salt when it has no name (the refusal's label). */
const char *sh_prefab_mods_label(const sh_modmanifest_row *row);

/* The caller owns the JSON containing count, missing and mods. Each row has id (display name), salt, build,
 * size, installed (all required content available), state (served, missing or incomplete), and an optional
 * refusal cause for local rows. */
char *sh_prefab_mods_status_json(const sh_modmanifest_list *rows, size_t *missing);

/* The block for a view: "description" and "tags" from its "desc" and "tags", "mods" from `rows` (NULL keeps the
 * view's), every other member under "summary". A NULL or unreadable view composes an empty block. malloc'd. */
char *sh_prefab_mods_block_compose(const char *view, size_t len, const sh_modmanifest_list *rows, size_t *out_len);

/* The view of a block (its object text). malloc'd; NULL when the block is not
 * a JSON object. */
char *sh_prefab_mods_block_view(const char *block, size_t len, size_t *out_len);

/* A prefab's details as a view: its block when the text has one (tail: the text is the end of a larger file),
 * else NULL; *source receives an SH_PREFAB_META_* value (EARLIER when a sidecar exists beside the prefab, may be NULL). */
char *sh_prefab_mods_view_of(const char *text, size_t len, int tail, const char *path, int *source,
                             size_t *out_len);

/* The rows a prefab names: its block's. Returns the count, -1 unreadable, SH_PREFAB_ROWS_EARLIER for the
 * sidecar form. */
int   sh_prefab_mods_rows_of(const char *text, size_t len, const char *path, sh_modmanifest_list *out);

/* The prefab text with the block composed from `view` and `rows` (NULL keeps
 * the view's). malloc'd; NULL when the text is not a JSON object. */
char *sh_prefab_mods_embed(const char *text, size_t len, const char *view, size_t view_len,
                           const sh_modmanifest_list *rows, size_t *out_len);

/* A copy of the prefab text without its block. malloc'd. */
char *sh_prefab_mods_strip_copy(const char *text, size_t len, size_t *out_len);

/* Read a whole file (at most SH_PREFAB_MODS_FILE_MAX). malloc'd,
 * NUL-terminated; NULL when it cannot be read. */
char *sh_prefab_mods_read_file(const char *path_utf8, size_t *len);

/* Write a file through a temporary beside it. 1 written. */
int   sh_prefab_mods_write_file(const char *path_utf8, const char *data, size_t len);

/* Create: `text` is the prefab the engine just serialized to `path`; the closure over it goes into a fresh
 * block written into the file. *view_out (optional, malloc'd) receives the view. Returns the row count, -1
 * when nothing could be recorded (the file is left as written). */
int   sh_prefab_mods_record(const char *text, size_t len, const char *path, char **view_out);

/* A module prefab's save: the cut file `prefab` with a block composed from its
 * summary view and the closure over it. malloc'd (NULL when the cut is not an
 * object); *view_out (optional) the view written, *count the row count. */
char *sh_prefab_mods_module_file(const char *prefab, size_t len, const char *summary, size_t *out_len, char **view_out,
                                 int *count);

/* The Prefabs tab's description and tag edit: `body` is a view ("" clears both), only "desc" and "tags" are
 * taken; the block at `path` is rewritten in place (written fresh when none). A sidecar-form prefab (
 * no block) is not written. 1 written, -1 failed. */
int   sh_prefab_mods_meta_write(const char *body, size_t len, const char *path);

/* Bring a prefab's mods into the open map before it is placed (main thread). `text` is the prefab file, for a
 * module prefab's module check. Returns an SH_PREFAB_MODS_* answer; msg receives what the Prefabs tab shows
 * for NEEDS_MODULE, REFUSED and HELD; *seq the placement to poll when HELD. */
int   sh_prefab_mods_resolve_main(const char *text, size_t len, const sh_modmanifest_list *rows, const char *name,
                                  int *seq, char **msg);

/* A held placement: 2 preparing local files, 1 ready to join the open map, 0 refused or dropped. Any thread. */
int   sh_prefab_mods_poll(int seq);

/* Export the prefab at prefab_path (a sidecar beside the prefab refuses it) as `name` to zip_utf8. Rows come from
 * its block, else from the closure; a prefab with no block or with rows moved to the current builds is
 * carried with a block written for them (the file on disk is unchanged). 1 written, -1 refused; msg says why. */
int   sh_prefab_mods_export(const char *prefab_path, const char *name, const char *zip_utf8,
                            char **msg);

/* Import a prefab's export (a .zip, or a .json inside an extracted export folder) into prefabs_dir: add the
 * prefab under the .json's file name and verify its carried content in the cache. A failed import removes the new prefab. The same bytes are left alone, other bytes get the next free "<name> (N)". 1
 * added, 0 already there, -1 refused (nothing stays); name_out receives the library name. */
int   sh_prefab_mods_import(const char *src_utf8, const char *prefabs_dir, char **name_out, char **msg);

/* 1 when `name` can be a prefab file name: the one rule the Prefabs tab
 * uses too (prefab_block.h, sh_prefab_block_valid_name). */
int   sh_prefab_mods_valid_name(const char *name);

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
    /* An imported prefab's pinned rows (units all in the content cache) join the open map: composed, added to the
     * manifest, declared; a salt the map uses at another version is left to it when it carries every object the
     * prefab places, else *cause (malloc'd, a clause after "was not placed: "). 1 used, -1 refused. Main thread. */
    int  (*use_pinned)(const sh_modmanifest_list *pinned, char **cause);
    int  (*editor_context)(void);   /* editor map identity, zero during a transition */
} sh_prefab_mods_ops;
void  sh_prefab_mods_bind(const sh_prefab_mods_ops *ops);

/* Where the console finds prefabs (tests; NULL restores %LOCALAPPDATA%\snapmap-plus\prefabs\). */
void  sh_prefab_mods_set_root(const char *dir);
int   sh_prefab_mods_kept(sh_modmanifest_list *rows);

/* `sh_prefab`: argv[0] is the verb (NULL or "help" prints the verbs):
 *   mods "<name>"                 the prefab's mods and whether their content is available in
 *                                 the open map's context (it changes nothing)
 *   export "<name>" "<path.zip>"  the prefab with its mods, as one ZIP
 *   import "<path>"               a .zip, or a .json in an extracted export folder:
 *                                 cache the included mod content and add the prefab to the library
 * A name may be "<folder>/<name>". Game thread. */
void  sh_prefab_mods_command(int argc, const char *const *argv);

#define SH_PREFAB_CMD_NAME "sh_prefab"
#define SH_PREFAB_CMD_HELP "Prefabs and their mods: 'sh_prefab mods \"<name>\"' lists the mods a prefab needs and whether " \
    "their content is ready. 'sh_prefab export \"<name>\" \"<path.zip>\"' writes the prefab and its required mod " \
    "content to one ZIP. 'sh_prefab import \"<path.zip or path.json>\"' adds the prefab to the library and caches " \
    "the included content from the ZIP or extracted export folder."

struct sh_module_prefab_call;
/* The details and mods operations of the module_prefab slot (snapmap_plus_iface.h): MODS_RECORD, MODS_STATUS,
 * MODS_RESOLVE, MODS_POLL, META_WRITE, META_READ, EXPORT and IMPORT. Returns 0 for any other op. */
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
