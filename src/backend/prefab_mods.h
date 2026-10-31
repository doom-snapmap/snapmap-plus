/* prefab_mods.h -- the mods a prefab needs.
 *
 * A prefab is saved with the mods its entities and modules depend on, found
 * exactly as a map's save finds them (modmanifest.h): the placed names of the
 * prefab text (every "inherit", "moduleName" and string inside an "edit"
 * object) go through the serving layer's save-time closure, which names every
 * admitted bundle that provides one, with the bundles its needs reach. Each
 * bundle becomes one row in the map manifest's form ({"id","version",
 * "bundle","size","names"}; aliases resolve through the store as for a map),
 * kept in the prefab's sidecar, <name>.meta.json, under the key "mods". The
 * prefab file itself stays the engine's bytes. A sidecar without "mods" (every
 * prefab saved before this) names no mods and loads as before.
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
 * Export writes the prefab, its sidecar and every bundle its rows name into
 * one ZIP in the rawmap export's layout (modexport.h); import installs the
 * bundles the store lacks and adds the prefab to the library. Importing an
 * export whose mods and prefab are already there writes nothing.
 *
 * Paths: prefab and sidecar paths are ANSI, as the Prefabs tab resolves them;
 * ZIP paths are UTF-8 (the file dialog's), except on the console, whose
 * arguments are ANSI.
 */
#ifndef SH_PREFAB_MODS_H
#define SH_PREFAB_MODS_H

#include <stddef.h>
#include "modmanifest.h"

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

/* ------------------------------------------------------------ rows */

/* The rows a prefab text needs: the map manifest's save-time closure over its
 * placed names, sorted by id. Returns the row count, -1 when the text does not
 * read or the serving layer reported an invalid bundle. */
int   sh_prefab_mods_closure(const char *text, size_t len, sh_modmanifest_list *out);

/* The rows of a sidecar body's "mods" array (duplicates by hash merged).
 * Returns the count (0 for no body or no "mods"), -1 when the body is not a
 * JSON object or "mods" is not an array; rows that do not read are skipped. */
int   sh_prefab_mods_sidecar_rows(const char *side, size_t len, sh_modmanifest_list *out);

/* The sidecar body with "mods" set to `rows` (removed when there are none),
 * every other member kept as it was. A NULL or empty body starts from
 * {"desc":"","tags":[]}. malloc'd, NUL-terminated; NULL when the body is not
 * a JSON object or out of memory. */
char *sh_prefab_mods_sidecar_set(const char *side, size_t len, const sh_modmanifest_list *rows, size_t *out_len);

/* 1 when a row names `name` among the names it provides (case-insensitive). */
int   sh_prefab_mods_row_provides(const sh_modmanifest_list *rows, const char *name);

/* "id version" for a row (the refusal's label). */
void  sh_prefab_mods_label(const sh_modmanifest_row *row, char *out, size_t cap);

/* {"count":N,"missing":M,"mods":[{"id","version","bundle","size","installed"}...]}
 * with each row's state from the bound bundle_state. malloc'd. */
char *sh_prefab_mods_status_json(const sh_modmanifest_list *rows, size_t *missing);

/* ------------------------------------------------------------ files */

/* Read a whole file (at most SH_PREFAB_MODS_FILE_MAX). malloc'd,
 * NUL-terminated; NULL when it cannot be read. */
char *sh_prefab_mods_read_file(const char *path_acp, size_t *len);

/* Write a file through a temporary beside it. 1 written. */
int   sh_prefab_mods_write_file(const char *path_acp, const char *data, size_t len);

/* The saved prefab's rows into its sidecar: the closure over `text`, merged
 * into the sidecar at meta_path (created when there is none and the prefab
 * needs mods; the "mods" member dropped when it needs none). *side_out
 * (optional, malloc'd) receives the sidecar body as written or found. Returns
 * the row count, -1 when nothing could be recorded. */
int   sh_prefab_mods_record(const char *text, size_t len, const char *meta_path, char **side_out);

/* The module prefab save's sidecar `side` with the closure over the cut file
 * `prefab` added. malloc'd, or NULL (the caller writes `side` unchanged).
 * *count receives the row count. */
char *sh_prefab_mods_with_closure(const char *side, const char *prefab, size_t len, int *count);

/* Write a sidecar body the Prefabs tab sent (description and tags): a body
 * without "mods" keeps the rows the sidecar on disk records, and an empty
 * body deletes the sidecar unless it records rows. 1 written, -1 failed. */
int   sh_prefab_mods_meta_write(const char *body, size_t len, const char *meta_path);

/* ------------------------------------------------------------ resolve */

/* Bring a prefab's mods into the open map before it is placed (main thread;
 * see the top of this file). `rows` are the sidecar's; `text` is the prefab
 * file (for a module prefab's module check). Returns an SH_PREFAB_MODS_*
 * answer; msg receives what the Prefabs tab shows for NEEDS_MODULE, REFUSED
 * and HELD; *seq the placement to poll when HELD. */
int   sh_prefab_mods_resolve_main(const char *text, size_t len, const sh_modmanifest_list *rows, const char *name,
                                  int *seq, char *msg, size_t cap);

/* A held placement: 2 waiting, 1 ready (its mods are installed and in the
 * open map: place it now), 0 refused or dropped. Any thread. */
int   sh_prefab_mods_poll(int seq);

/* ------------------------------------------------------------ export, import */

/* Export the prefab at prefab_path (sidecar at meta_path, may be absent) to
 * zip_path (UTF-8). Rows come from the sidecar, else from the closure over the
 * prefab (a prefab saved before rows were recorded). 1 written, -1 refused;
 * msg says which and why. */
int   sh_prefab_mods_export(const char *prefab_path, const char *meta_path, const char *name, const char *zip_utf8,
                            char *msg, size_t cap);

/* Import a prefab's export into prefabs_dir (ANSI, with or without a trailing
 * separator): install the bundles the store lacks, then add the prefab under
 * the name the export gives (else the ZIP's file name). A prefab of that name
 * with the same bytes is left alone; one with other bytes gets the next free
 * "<name> (N)". 1 added, 0 already there (nothing written), -1 refused.
 * name_out receives the name it has in the library. */
int   sh_prefab_mods_import(const char *zip_utf8, const char *prefabs_dir, char *name_out, size_t name_cap,
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
 *   import "<path.zip>"           install the mods, add the prefab to the library
 * A name may be "<folder>/<name>". Game thread. */
void  sh_prefab_mods_command(int argc, const char *const *argv);

#define SH_PREFAB_CMD_NAME "sh_prefab"
#define SH_PREFAB_CMD_HELP "Prefabs and their mods: 'sh_prefab mods \"<name>\"' lists the mods a prefab needs and whether " \
    "each is installed; 'sh_prefab export \"<name>\" \"<path.zip>\"' writes the prefab with its mods to one ZIP; " \
    "'sh_prefab import \"<path.zip>\"' installs the mods and adds the prefab to the library."

struct sh_module_prefab_call;
/* The mods operations of the module_prefab slot (snapmap_plus_iface.h):
 * MODS_RECORD, MODS_STATUS, MODS_RESOLVE, MODS_POLL, META_WRITE, EXPORT and
 * IMPORT. Returns 0 for any other op. */
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
