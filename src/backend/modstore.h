/* Local mod folders hold their assets and mod.json. Snapmap+ writes build results in each
 * mod's .build directory and keeps folder identities and publishing records in snapmap-plus.json.
 * Store writes share a cross-process lock; boot, Refresh and install rescan changed mods. */
#ifndef SNAPMAP_PLUS_MODSTORE_H
#define SNAPMAP_PLUS_MODSTORE_H

#include <stddef.h>
#include "modbundle.h"
#include "modsalt.h"

/* The store lock (recursive per thread): the mods folder's named lock (store_lock.h), shared with every other
 * process on the folder. A thread that finds it held logs one "MODS: store: waiting" line and waits; the counts
 * below are this process's. */

void modstore_lock(void);
void modstore_unlock(void);
/* How many lock calls had to wait so far (tests, diagnostics). */
unsigned modstore_lock_waits(void);
/* Any thread: 1 while some thread holds the lock (a store write may be in
 * progress), else 0. *generation (when non-NULL) counts the outermost holds
 * released so far, so a change tells that a hold ended since the last look. */
int      modstore_lock_busy(unsigned *generation);

/* Paths under mods\ have no length or depth limit of the product's, only what Windows' long-path form (32,767
 * characters, wide_path.h) leaves room for. */
/* mods\maps\: a reserved name at the top of mods\, never read or written. */
#define SH_MS_RETIRED_PLACES "maps"

/* The store root. NULL or "" restores the default (%LOCALAPPDATA%\snapmap-plus\mods).
 * Tests point it elsewhere. Returns 1 when the root is set. */
int  sh_ms_set_root(const char *root_utf8);
/* The current root as UTF-8 (stable for the process), or NULL when
 * %LOCALAPPDATA% cannot be resolved. */
const char *sh_ms_root(void);

typedef struct sh_ms_entry {
    char *folder;               /* absolute UTF-8 folder of the mod */
    char *name;                 /* the folder's path under mods\ ("x", "group\x") */
    char *key;                  /* a label: the salt, or the folder name while it has none */
    char  salt[SH_SALT_LEN + 1];      /* the block's salt; "" when it has none */
    int   state;                /* sh_mb_descriptor_state: SH_MB_DESC_* */
    int   refused;              /* listed with its cause and serves nothing */
    int   loaded;               /* mod.json read and its hash matched */
    int   sizes_ok;             /* every carried file present with its recorded size */
    int   validated;            /* the block records no "cause" */
    long long installed_at;     /* the block's "built" */
    char *cause;                /* why the entry is unusable, or the block's cause; never NULL in a list */
    sh_mb_bundle bundle;
} sh_ms_entry;

typedef struct sh_ms_list {
    sh_ms_entry *items;
    size_t count, cap;
} sh_ms_list;

/* An entry's bundle may serve names: loaded, sizes match, validated, not refused. */
int  sh_ms_entry_usable(const sh_ms_entry *entry);

/* Whether `spec` names the entry: its folder name under mods\ (either slash),
 * a nested folder's own name, or its salt, case-insensitively. */
int  sh_ms_entry_matches(const sh_ms_entry *entry, const char *spec);

/* List every mod folder of the store, sorted by name. A folder not built yet, refused, or a copy of a root salt
 * is listed unloaded or refused with its cause. Returns 1 when the root could be read (an absent root is an
 * empty list). */
int  sh_ms_enumerate(sh_ms_list *out);
void sh_ms_list_free(sh_ms_list *list);

/* The walk every reader of the store shares (enumeration, the scan, the serving index), so they agree on what
 * is a mod: a folder holding mod.json or package.json is a mod and is not entered, any other is entered at any
 * depth; dot folders, reparse points, "<name>.staging-<pid>" / "<name>.old-<pid>" work folders and mods\maps\
 * are skipped. `fn` receives the absolute long-path UTF-8 folder and its name under mods\; a nonzero return
 * stops the walk. Returns 1 when the root could be read (an absent root walks nothing). */
typedef int (*sh_ms_walk_fn)(void *context, const char *folder, const char *name);
int  sh_ms_walk(sh_ms_walk_fn fn, void *context);

/* Whether `folder` holds mod.json or package.json (a mod folder). */
int  sh_ms_folder_is_mod(const char *folder);

/* Every mod folder under `dir` at any depth, as the walk above finds them (mods\maps\ skipped at the store
 * root; `dir` itself not listed). Absolute UTF-8 paths in *out, freed with sh_ms_find_free. Returns 1 when
 * `dir` could be read. */
int  sh_ms_find_mods(const char *dir, char ***out, size_t *count);
void sh_ms_find_free(char **folders, size_t count);

/* The entry whose build hash is `hash`, a usable one first; NULL when none. */
const sh_ms_entry *sh_ms_resolve(const sh_ms_list *list, const char *hash);

/* The entry of `salt`: `hash` NULL for any build, else that build only. A
 * usable entry first; refused entries never. NULL when none. */
const sh_ms_entry *sh_ms_find_salt(const sh_ms_list *list, const char *salt, const char *hash);

/* A folder of the store was built and its mod.json written: its recorded build failure is cleared. Returns 1. */
int  sh_ms_note_build(const char *folder, const sh_mb_bundle *b);

/* Gives a mod.json with no builder block a new salt (sh_salt_generate, never one the store holds): references to
 * mods/<from>/ in its text files and asset paths become mods/<salt>/ (sh_mb_rekey_text, sh_mb_rekey_path), its
 * strings keys gain "<salt>_" with their "#str_" references (sh_mb_salt_strings, sh_mb_restr_text), and its
 * block becomes {"format": 2, "salt": <salt>}. `from` is the folder's own name; the folder is staged as
 * "<name>.staging-<pid>" and swapped in by two renames, so any failure leaves it untouched. Returns 1 with the
 * salt in salt_out, 0 when it has one already (salt_out receives it), -1 with a plain malloc'd *cause (also a
 * refusal in mods\maps\); takes the store lock. */
int  sh_ms_assign_salt(const char *folder, const char *from, char salt_out[SH_SALT_LEN + 1], char **cause);

/* Add to my mods: the folder `staged` (outside the store, its files under `salt`'s namespace) becomes a new
 * mod at mods\<sh_ms_title_segment(name)> (" (2)" when taken) with `modjson` as its descriptor and a new salt:
 * its files move from mods\<salt>\ to the new salt and text references follow, as a copied folder's do
 * (ms_assign_to); a binary naming mods/<salt>/ is kept as it is, and serving maps it. `staged` is only read,
 * so the caller removes it after; the scan builds the new folder. Returns 1 with its name under mods\ in
 * *name_out (malloc'd), else 0 with the plain cause. Takes the store lock. */
int  sh_ms_add_copy(const char *staged, const char *modjson, size_t modjson_len, const char *salt, const char *name,
                    char **name_out, char **cause);

/* A renamed sound bank's stem, old and new. */
typedef struct sh_ms_bank_rename { char from[SH_MB_BANK_STEM_MAX + 1], to[SH_MB_BANK_STEM_MAX + 1]; } sh_ms_bank_rename;

/* `text` with every bank stem `names[k].from` that stands as a whole word (no letter, digit or '_' beside it)
 * replaced by its `to`, ignoring case. Returns the count with the result in *out (malloc'd) when it is not 0,
 * or (size_t)-1 out of memory. */
size_t sh_ms_rebank_text(const char *text, size_t len, const sh_ms_bank_rename *names, size_t count, char **out,
                         size_t *out_len);

typedef struct sh_ms_install_stats {
    unsigned copied;            /* files copied into the store (the game files the folder holds too) */
    int in_place;               /* the source folder was inside the store */
    int replaced;               /* a folder of the install's name was replaced */
    int present;                /* a usable folder already holds this build: nothing was copied */
    int resalted;               /* a copy of a root mod: it was given a new salt */
    unsigned stock_files;       /* stock pointers (borrows); a copy the folder holds is kept */
    unsigned long long stock_bytes;   /* their installed size */
} sh_ms_install_stats;

/* Install `folder` at the root: a folder already in the store is built in place; a new mod (no builder block)
 * from elsewhere is copied to mods\<name> (" (2)" when taken), salted and built; a built mod is verified and
 * copied there, taking a new salt when the root holds its salt (stats->resalted), or is the install already
 * when the root holds this very build (stats->present). The source is only read; `resolver` may be NULL.
 * *name_out (malloc'd) and bundle_out (release with sh_mb_free) are optional. Returns 1, else 0 with the
 * first cause. */
int  sh_ms_install_from_folder(const char *folder, const sh_mb_resolver *resolver,
                               char **name_out, sh_ms_install_stats *stats,
                               sh_mb_bundle *bundle_out, char **cause);

/* ------------------------------------------------------------ names ---- */

/* `title` made a folder name (UTF-8): characters Windows refuses and control characters dropped, spaces folded,
 * edge dots and spaces cut, a work-folder suffix (".staging-", ".old-") spelled with "_", a device name (con,
 * prn, aux, nul, com1 to com9, lpt1 to lpt9) given a trailing "_". Only a title over 240 characters (room for
 * the " (2)" a taken name gets) is cut, at a character boundary. Returns the segment (malloc'd), or NULL when
 * nothing is left. */
#define SH_MS_SEGMENT_CHARS 240
char *sh_ms_title_segment(const char *title);

/* Install a built folder (a received build unpacked outside the store) at the root of mods\ as one of the
 * player's own mods, keeping its salt: every file is copied to mods\<its name> (" (2)" when taken) and
 * verified, the source only read. A root folder of the salt at this very build is the install (stats->present);
 * at another build the install is refused, since the player's own folder stays theirs. Returns 1 with the
 * folder name under mods\, else 0 with the plain cause. Any thread; takes the store lock. */
int  sh_ms_install_to_root(const char *folder, const sh_mb_resolver *resolver, char **name_out,
                           sh_ms_install_stats *stats, sh_mb_bundle *bundle_out, char **cause);

typedef struct sh_ms_verify_stats {
    unsigned files, bytes_mb, entries;
    unsigned pointers;          /* of files, stock pointers resolved against the installed game */
} sh_ms_verify_stats;

/* Rehash every file of the entries `spec` names (sh_ms_entry_matches)
 * against their builder blocks and record the result there ("cause").
 * Returns 1 when every matching entry passes. */
int  sh_ms_verify(const char *spec, sh_ms_verify_stats *stats, char **cause);

typedef struct sh_ms_sync_stats {
    unsigned folders;           /* folders looked at */
    unsigned salted;            /* new mods given their salt */
    unsigned resalted;          /* copies given a new salt */
    unsigned rebuilt;           /* built again: new, or their tree or the builder rules changed */
    unsigned failed;            /* refused (the cause stays on the entry) */
    unsigned long long ms;
} sh_ms_sync_stats;

/* The scan over the root, under the store lock: salts a root folder with no builder block; gives a new salt to
 * all but one of the folders carrying one salt (the one the record's "salts" section names keeps it, else the
 * first created); builds a folder whose tree hash or builder rules differ from its block's, or whose last build
 * failed (a failed build records its cause and serves nothing). Before a build, what the author wrote under
 * the folder name (assets files, mods/<name>/ references, unprefixed strings keys, "<name>_x.bnk" banks) moves
 * under the salt. Logs one "MODS: store:" line per change. Returns 1 when the store could be read. */
int  sh_ms_sync(const sh_mb_resolver *resolver, sh_ms_sync_stats *stats);

/* The root folders sh_ms_sync would change: new mods, copies, and built folders whose block's tree or rules
 * differ (file sizes and times: no file is read) or whose last build failed. -1 when the store cannot be read. */
int  sh_ms_sync_pending(void);

/* The boot scan: when root folders are pending (a folder dropped in while the game was closed, an edit), runs
 * sh_ms_sync and then `rescan` (the serving index; its listeners pass the new table to the game). background 1
 * does this on a thread of its own and returns 1 when it started; background 0 runs it here and returns the
 * folders that were pending (0: nothing built, no rescan). -1 when the store cannot be read or the thread
 * cannot start. The resolver is copied. */
typedef int (*sh_ms_rescan_fn)(void);
int  sh_ms_boot_sync(const sh_mb_resolver *resolver, sh_ms_rescan_fn rescan, int background);

/* Removes the work folders ("<name>.staging-<pid>", "<name>.old-<pid>" beside their target) left by a process
 * that is no longer running; a ".old-" folder whose target is missing (a crash between the two renames) is put
 * back instead. One "MODS: store:" line per action; run at boot before the serving index is read. Returns the
 * folders put back, or -1 when the store cannot be read. */
int  sh_ms_recover_work(void);

/* The startup install is done with the store: the background boot scan (it waits up to two minutes for this)
 * may start, and yields the store to a waiting operation between two folders. Any thread; idempotent.
 * sh_ms_boot_released: 1 once called. */
void sh_ms_boot_release(void);
int  sh_ms_boot_released(void);
#ifdef SH_MODSTORE_TESTING
void sh_ms_test_boot_rearm(void);                            /* the next boot build waits for a release again */
extern void (*sh_ms_test_between_folders)(unsigned built);   /* the boot build, between two folders */
#endif

/* The first cause of the last refused build or salting of `folder` in this
 * process (malloc'd), or NULL (sh_ms_sync and installs record it;
 * enumeration reports it). */
char *sh_ms_build_cause(const char *folder);

/* 1 when `folder` (absolute UTF-8) has a mod.json that is not a JSON object,
 * with the parse error and the next step in *out (malloc'd); 0 otherwise (no
 * mod.json, or one that parses). */
int sh_ms_descriptor_unparsable(const char *folder, char **out);

/* Record a refused build of `folder` (absolute) that a caller ran itself
 * (`sh_mods build`): the same per-folder state sh_ms_sync records. A later
 * successful build (sh_ms_note_build) clears it. */
void sh_ms_note_build_failure(const char *folder, const char *cause);

/* New mod: mods\<sh_ms_title_segment(name), else "mod"> (" (2)" when taken) with a mod.json of name, empty
 * description and author, the block {"format": 2, "salt": <a new salt>} and an empty assets\; the caller's scan
 * builds it. Returns 1 with the salt and the absolute folder (malloc'd into *folder_out), else 0 with the cause. */
int  sh_ms_new_mod(const char *name, char salt_out[SH_MB_ID_MAX + 1], char **folder_out, char **cause);
/* New mod from the Mods tab: as sh_ms_new_mod, plus an example that places and plays untouched: an entityDef
 * at mods/<salt>/example_crate (the game's SnapMap crate01 model), its Toybox row (snapEditorEntityDef) and the
 * row's strings in mod.json's author part (<salt>_category named after the mod, <salt>_example_crate_name and
 * _desc). Nothing binary. *example_out (malloc'd) receives the row's path below the folder, with '/'. */
int  sh_ms_new_mod_example(const char *name, char salt_out[SH_MB_ID_MAX + 1], char **folder_out, char **example_out,
                           char **cause);

enum { SH_MS_MISS = 0, SH_MS_HIT = 1, SH_MS_DEDUPE = 2, SH_MS_CONFLICT = 3 };

typedef struct sh_ms_hit {
    int    status;
    size_t entry, file;         /* the first match */
    size_t providers;           /* distinct usable bundles providing the name */
    size_t other_entry;         /* the second provider when providers > 1 */
} sh_ms_hit;

/* Find who provides logical (type, name) across usable installed bundles: provides.type and provides.name
 * compared case-insensitively, or a file whose request path is one the engine would ask for that name; the
 * first match wins. Two providers are a CONFLICT for decl names (and for raw files with different bytes) and a
 * DEDUPE for raw files with equal hashes. Returns the status. */
int  sh_ms_resolve_need(const sh_ms_list *list, const char *type, const char *name, sh_ms_hit *hit);

/* sh_mb_exists_fn over a list: 1 when a usable bundle ships `request`. */
int  sh_ms_request_exists(void *list, const char *request);

/* Copy one file (wide long paths) through a temporary and a rename, never a
 * link; the copy is writable. Returns 1. */
int  sh_ms_copy_file(const wchar_t *from, const wchar_t *to);

/* How a folder left the store: moved to the Windows Recycle Bin, or deleted
 * outright because the bin could not take it (a network or removable drive,
 * a path the shell refuses, a shell error). */
#define SH_MS_RECYCLED 1
#define SH_MS_DELETED  2

/* Remove a folder under the store root, under the store lock: to the Recycle Bin (SHFileOperationW,
 * FOF_ALLOWUNDO, no UI, reparse points moved as themselves), else deleted outright with links removed and
 * never followed. One "MODS: store:" line says which; refuses anything outside the root and the root itself.
 * Returns SH_MS_RECYCLED, SH_MS_DELETED, or 0. */
int  sh_ms_remove_folder(const char *folder);

/* Uninstall's removal: `folder` (absolute) must be an existing mod folder of the store and goes as
 * sh_ms_remove_folder's does. Returns SH_MS_RECYCLED; SH_MS_DELETED with why the bin could not take it in
 * `cause`; else 0 with the first cause. */
int  sh_ms_delete_mod_folder(const char *folder, char **cause);

#ifdef SH_MODUI_TESTING
int sh_ms_test_enumerations(void);
#endif

#if defined(SH_MODSTORE_TESTING) || defined(SH_MODUI_TESTING)
/* Test seam: nonzero makes every removal take the outright-delete fallback,
 * as when the shell refuses. */
void sh_ms_recycle_refuse_for_test(int refuse);
#endif

#endif /* SNAPMAP_PLUS_MODSTORE_H */
