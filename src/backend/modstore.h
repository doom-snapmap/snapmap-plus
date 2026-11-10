/* modstore.h -- the local mod store, %LOCALAPPDATA%\snapmap-plus\mods\.
 *
 *   <folder>\mod.json            the author block and the product block (modbundle.h)
 *   <folder>\assets\...          the mod's files, real files only (no links, no blob store)
 *   snapmap-plus.json            the product record (modbundle.h): claims, links,
 *                                the migration record, the build cache, aliases,
 *                                the last good build snapshots
 *   .builds\<key>@<hash16>\      a folder's last good build (below): never a mod folder
 *
 * A folder is a mod when it holds mod.json (or, before its first build,
 * package.json). Folder names are the author's for a folder dropped in by
 * hand and, for one the product creates (an install, an import, a download),
 * rule 1 of DESIGN-additive 4 (sh_ms_build_folder_name): "<key>" for the
 * first build of a key, "<key>@<hash16>" for every later one, beside it.
 * Identity is the bundle hash: two folders declaring one key are two builds,
 * both kept and both listed, and a map serves the one it pins. Stock pointers (modbundle.h) are never
 * stored: they are resolved against the installed game at install and on
 * verification, and a pointer the game lacks (or holds with other bytes)
 * fails the bundle with that first cause.
 *
 * Rebuild rule (sh_ms_sync): a folder is built again when its tree hash
 * (sh_mb_tree_hash: the author block and the size and time of every file
 * under assets\) is not the key of the record's cache entry for its bundle.
 * Building needs the game's resource reader, so it runs on a rescan (the Mods
 * tab, sh_mods rescan, install, build), never at boot; boot reads the product
 * blocks and checks sizes only. The game runs no store migration at boot: the
 * upgrade of the retired layout and the bin sweep below are run by the store
 * tool (src/store, snapmap-plus-store.exe), which the installer runs.
 *
 * Installing a folder from elsewhere builds it in memory (the source folder
 * is not written) and installs by hash: a usable folder that already holds
 * the bundle is the install (nothing is copied); otherwise its mod.json
 * author text and its carried files are copied into a new folder by rule 1,
 * beside any other build of its key, never over one, the copy is verified
 * and recorded. Nothing is refused for "same version, other files". Verification (rehash every file) runs at install
 * and on request, never at boot.
 *
 * File work only, on the calling thread; no engine calls. Every writer of the
 * store serialises on modstore_lock: the install, verification, sync and
 * upgrade below take it themselves, and longer steps (one migration folder, a
 * download install, `sh_mods install` and `sh_mods migrate`, an export) hold
 * it around their whole step. A second writer waits for the first, it never
 * fails because of it; the wait is one step long.
 */
#ifndef SNAPMAP_PLUS_MODSTORE_H
#define SNAPMAP_PLUS_MODSTORE_H

#include <stddef.h>
#include "modbundle.h"

/* The store lock (recursive). modsync_store_lock and modsync_store_unlock
 * (modsync.h) are the same lock under their older names. A thread that finds
 * it held logs one "MODS: store: waiting" line and waits. */
void modstore_lock(void);
void modstore_unlock(void);
/* How many lock calls had to wait so far (tests, diagnostics). */
unsigned modstore_lock_waits(void);
/* Any thread: 1 while some thread holds the lock (a store write may be in
 * progress), else 0. *generation (when non-NULL) counts the outermost holds
 * released so far, so a change tells that a hold ended since the last look. */
int      modstore_lock_busy(unsigned *generation);

/* id@version+build; version may hold up to 4 UTF-8 bytes per code point. */
#define SH_MS_KEY_MAX (SH_MB_ID_MAX + 1 + 4 * SH_MB_VERSION_MAX + 1 + 20)
/* A folder name under mods\. */
#define SH_MS_NAME_MAX 1023   /* a nested folder's path under mods\ */
#define SH_MS_NEST_MAX 8      /* organizing folders are entered this many levels below mods\ */

/* The store root. NULL or "" restores the default (%LOCALAPPDATA%\snapmap-plus\mods).
 * Tests point it at a temporary folder. Returns 1 when the root is set. */
int  sh_ms_set_root(const char *root_utf8);
/* The current root as UTF-8, or 0 when %LOCALAPPDATA% cannot be resolved. */
int  sh_ms_root(char *out, size_t cap);

typedef struct sh_ms_entry {
    char *folder;               /* absolute UTF-8 folder of the mod */
    char  name[SH_MS_NAME_MAX + 1];   /* the folder's path under mods\ ("x", or "group\x" when nested) */
    char  key[SH_MS_KEY_MAX + 1];     /* id@version+build from mod.json, or the folder name when unbuilt */
    int   delivered;            /* a package a map delivered (its product block's legacy object) */
    int   loaded;               /* mod.json read and its hash matched */
    int   sizes_ok;             /* every carried file present with its recorded size */
    int   validated;            /* the product block's "validated" */
    long long installed_at;     /* the product block's "built" */
    char  cause[SH_MB_CAUSE_MAX];   /* why the entry is unusable, or the block's cause */
    /* A loaded entry whose latest rebuild in this process was refused: its
     * cause. The bundle above, the last good build, keeps serving
     * (DESIGN-additive 2.8); "" when the latest build succeeded or none ran. */
    char  build_failed[SH_MB_CAUSE_MAX];
    /* The snapshot of the bundle above (sh_ms_snapshot_of), absolute UTF-8,
     * when one is complete; else NULL. While build_failed is set, the folder's
     * own files may hold other bytes: the last good bytes are read from here
     * (bundle.json and assets\ at each file's path), and sizes_ok was checked
     * against it. */
    char *snapshot;
    sh_mb_bundle bundle;
} sh_ms_entry;

/* One alias of the record: a bundle hash a map may pin that answers as `to`. */
typedef struct sh_ms_alias {
    char from[SH_MB_HASH_HEX + 1];
    char to[SH_MB_HASH_HEX + 1];
} sh_ms_alias;

typedef struct sh_ms_list {
    sh_ms_entry *items;
    size_t count, cap;
    sh_ms_alias *aliases;       /* the record's aliases when the list was read */
    size_t alias_count;
} sh_ms_list;

/* An entry's bundle may serve names: loaded, sizes match, and validated. */
int  sh_ms_entry_usable(const sh_ms_entry *entry);

/* A loaded entry that is a package a map delivered (sh_mb_is_delivered). */
int  sh_ms_entry_delivered(const sh_ms_entry *entry);

/* The installed mod a delivered package `d` is a copy of, or NULL: a loaded
 * folder that was not itself delivered, declaring d's id (rule 1, whatever
 * the content: one whose paths cover d's first, then a usable one), else
 * one under another id providing every path d provides (rule 2, the one with
 * the fewest files). *rule receives 1 or 2. */
const sh_ms_entry *sh_ms_delivered_counterpart(const sh_ms_list *list, const sh_mb_bundle *d, int *rule);

/* Whether `spec` names the entry: its folder name, id@version+build,
 * id@version (every build of it) or id (every folder declaring it),
 * case-insensitively. */
int  sh_ms_entry_matches(const sh_ms_entry *entry, const char *spec);

/* List every mod folder under the root, and the record's aliases. Folders
 * with a descriptor but no built product block are listed unloaded with the
 * cause. Returns 1 when the root could be read (an absent root is an empty
 * list). */
int  sh_ms_enumerate(sh_ms_list *out);
void sh_ms_list_free(sh_ms_list *list);

/* Whether `folder` holds mod.json or package.json (a mod folder). */
int  sh_ms_folder_is_mod(const char *folder);

/* Every mod folder under `dir`, at any depth, the way the store walks its own
 * root: a folder with a descriptor is a mod and is not entered, one without is
 * entered to SH_MS_NEST_MAX levels; hidden and staging folders are skipped.
 * `dir` itself is not listed. Absolute UTF-8 paths in *out, freed with
 * sh_ms_find_free. Returns 1 when `dir` could be read. */
int  sh_ms_find_mods(const char *dir, char ***out, size_t *count);
void sh_ms_find_free(char **folders, size_t count);

/* Aliases (the record's "aliases" section, hash -> hash) keep a map that pins
 * an earlier bundle hash loading: a delivered copy or a download whose content
 * an installed mod already serves, and every earlier build of a folder that
 * was built again (a rebuild on rescan, sh_mods build, an install replacing
 * the folder of the same id and version). The section is flat: every target
 * is a bundle hash, never another alias, so one hop always resolves.
 *
 * sh_ms_resolve finds the entry serving `hash`: an entry whose bundle hash is
 * `hash` (a usable one first), else the entry the alias of `hash` names.
 * Every lookup of a map's pinned hash goes through it. NULL when neither. */
const sh_ms_entry *sh_ms_resolve(const sh_ms_list *list, const char *hash);

/* Record that `old_hash` now answers as `new_hash`: aliases naming old_hash
 * are re-pointed at new_hash, old_hash -> new_hash is added, and an alias
 * from new_hash (a build returning to earlier content) is dropped. Returns
 * the number of aliases now naming new_hash, 0 when nothing was recorded. */
int  sh_ms_alias_note(const char *old_hash, const char *new_hash);

/* After a folder of `id` serving `hash` was removed: when no folder of the
 * store serves `hash` and none declares `id`, the aliases naming `hash` are
 * dropped (they could resolve nothing). Otherwise they stay. Returns the
 * number dropped. */
int  sh_ms_alias_forget(const char *id, const char *hash);

/* Drop every alias whose target hash no folder of the store builds to any
 * more (a folder deleted by hand, or replaced by one with other bytes).
 * Returns the number dropped. sh_ms_sync runs it after each pass. */
int  sh_ms_alias_prune(void);

/* A folder of the store was built and its mod.json written: the build cache
 * notes it (sh_ms_cache_note) and, when the product block named another
 * bundle before (b->prev_hash), that hash becomes an alias of the new one.
 * Returns 1 when the cache note was written. */
int  sh_ms_note_build(const char *folder, const sh_mb_bundle *b);

typedef struct sh_ms_install_stats {
    unsigned copied;            /* carried files copied into the store */
    int in_place;               /* the source folder was inside the store */
    int replaced;               /* the named target (as_name), or a damaged copy of this bundle, was replaced */
    int present;                /* a usable folder already holds this bundle: nothing was copied */
    unsigned stock_files;       /* stock pointers: nothing stored for them */
    unsigned long long stock_bytes;   /* their installed size */
} sh_ms_install_stats;

/* Build `folder`: in place when it is a folder of the store (its mod.json
 * product block is written), else by hash: when a usable folder already
 * holds the built bundle, that folder is the result (stats->present, nothing
 * copied); else into a new store folder named `as_name` when given (a folder
 * of that name with the same id and version is replaced: a migration's
 * target), else by sh_ms_build_folder_name, beside every other build of the
 * key; verify the result and record it in the build cache. `resolver` may be NULL; the store's installed bundles are
 * consulted in addition to it. On success name_out holds the folder name.
 * bundle_out (optional) receives the built bundle; release it with
 * sh_mb_free. Returns 1 on success, else 0 with the first cause. */
int  sh_ms_install_from_folder(const char *folder, const sh_mb_resolver *resolver,
                               char *name_out, size_t name_cap, sh_ms_install_stats *stats,
                               sh_mb_bundle *bundle_out, char *cause, size_t cause_cap);
int  sh_ms_install_as(const char *folder, const char *as_name, const sh_mb_resolver *resolver,
                      char *name_out, size_t name_cap, sh_ms_install_stats *stats,
                      sh_mb_bundle *bundle_out, char *cause, size_t cause_cap);
/* The same, recording `origin` (SH_MB_ORIGIN_*, SH_MS_ORIGIN_IMPORTED; NULL:
 * "install") in the new folder's product block. */
#define SH_MS_ORIGIN_IMPORTED "imported"   /* carried by a rawmap or prefab ZIP */
int  sh_ms_install_with(const char *folder, const char *as_name, const char *origin, const sh_mb_resolver *resolver,
                        char *name_out, size_t name_cap, sh_ms_install_stats *stats,
                        sh_mb_bundle *bundle_out, char *cause, size_t cause_cap);

/* Rule 1 (DESIGN-additive 4): the folder name under `mods_root` (UTF-8) for
 * the build of `key` at `hash`. With no folder of the key installed
 * (`key_installed` 0) the key itself when that name is free; else, and for
 * every later build, "<key>@<first 16 hex of hash>", then "<key>@<hash16>-2"
 * and so on. Returns 1 with a free name, 2 with the name of a folder that
 * already holds a build at `hash` (placed again when it is damaged), 0 when
 * no name is free. */
int  sh_ms_build_folder_name(const char *mods_root, const char *key, const char *hash, int key_installed,
                             char *out, size_t cap);

typedef struct sh_ms_verify_stats {
    unsigned files, bytes_mb, entries;
    unsigned pointers;          /* of files, stock pointers resolved against the installed game */
} sh_ms_verify_stats;

/* Rehash every file of the entries `spec` names (sh_ms_entry_matches)
 * against their product blocks and record the result there ("validated",
 * "cause"). Returns 1 when every matching entry passes. */
int  sh_ms_verify(const char *spec, sh_ms_verify_stats *stats, char *cause, size_t cause_cap);

typedef struct sh_ms_sync_stats {
    unsigned folders;           /* folders looked at */
    unsigned rebuilt;           /* built again: new, or their tree hash changed */
    unsigned failed;            /* refused by the builder (the cause stays on the entry) */
    unsigned long long ms;
} sh_ms_sync_stats;

/* The rebuild rule over every folder of the store, then the cache section of
 * the record rewritten to the current folders. Logs one "MODS: store:" line
 * per rebuilt or refused folder. Returns 1 when the store could be read. */
int  sh_ms_sync(const sh_mb_resolver *resolver, sh_ms_sync_stats *stats);

/* The folders sh_ms_sync would build now (no product block, or a tree its
 * cache does not hold as built), from the tree hashes alone (file sizes and
 * times: no file is read). -1 when the store cannot be read. */
int  sh_ms_sync_pending(void);

/* The boot's rebuild rule (DESIGN-additive 2.8): a store whose folders are
 * not built yet (a first boot after a conversion or an install by hand) is
 * built as a rescan builds it, so a fresh folder serves on its first boot.
 * With folders pending, sh_ms_sync runs and then `rescan` (the serving index;
 * its listeners pass the new table to the game). background 1 does the check
 * and the build on a thread of its own (a first build of the 15 converted
 * folders takes about 10 s) and returns 1 when it started; background 0 runs
 * them here and returns the folders that were pending (0: nothing built, no
 * rescan). -1 when the store cannot be read or the thread cannot start. The
 * resolver is copied. */
typedef int (*sh_ms_rescan_fn)(void);
int  sh_ms_boot_sync(const sh_mb_resolver *resolver, sh_ms_rescan_fn rescan, int background);

/* The work folders an earlier session left when it ended mid-install, mid-
 * download or mid-rename ("<name>.staging-<pid>", "<name>.old-<pid>" beside
 * their target, and the snapshots' staging under .builds\), whose process is
 * no longer running. A ".old-" folder whose target is missing is the folder a
 * crash took away between the two renames: it is put back. Every other one is
 * removed. One "MODS: store:" line per action. Run at boot before the serving
 * index is read. Returns the folders put back, or -1 when the store cannot be
 * read. */
int  sh_ms_recover_work(void);

/* The startup install is done with the store: the background boot build,
 * which waits for this (up to two minutes), may start. The install takes the
 * store itself (the publish launch reads the upload queue and the claims), so
 * the build starts after it and never holds it up; while the build runs it
 * hands the store to any operation that waits for it, between two folders.
 * Any thread; idempotent. sh_ms_boot_released: 1 once called. */
void sh_ms_boot_release(void);
int  sh_ms_boot_released(void);
#ifdef SH_MODSTORE_TESTING
void sh_ms_test_boot_rearm(void);                            /* the next boot build waits for a release again */
extern void (*sh_ms_test_between_folders)(unsigned built);   /* the boot build, between two folders */
#endif

/* The first cause of the last refused build of `folder` in this process, or
 * "" (sh_ms_sync and installs record it; enumeration reports it, with the
 * snapshot that serves meanwhile in sh_ms_entry.snapshot). */
void sh_ms_build_cause(const char *folder, char *out, size_t cap);

/* 1 when `folder` (absolute UTF-8) has a mod.json that is not a JSON object,
 * with the parse error and the next step in `out`; 0 otherwise (no mod.json,
 * or one that parses). sh_ms_sync records it as the folder's refused build,
 * so its last good build keeps serving until the file is fixed. */
int sh_ms_descriptor_unparsable(const char *folder, char *out, size_t cap);

/* Record a refused build of `folder` (absolute) that a caller ran itself
 * (`sh_mods build`): the same per-folder state sh_ms_sync records. The
 * folder's mod.json is not touched, so a previous build keeps serving. A
 * later successful build (sh_ms_note_build) clears it. */
void sh_ms_note_build_failure(const char *folder, const char *cause);

/* New mod (DESIGN-additive 2.2, 4): a folder under the store root named by a
 * key generated from `name` (sh_mb_key_generate), holding a descriptor (name,
 * version "0.1.0", empty description and author, an empty "player" intent)
 * whose product block records the key and origin "by-hand", and an empty
 * assets\mods\<key>\. The folder is built at the next rescan. Returns 1 with
 * the key and the absolute folder; 0 with the cause. */
int  sh_ms_new_mod(const char *name, char key_out[SH_MB_ID_MAX + 1], char *folder_out, size_t folder_cap,
                   char *cause, size_t cause_cap);

/* A hand-made folder's first build (DESIGN-additive 2.2): when `folder` is a
 * mod folder of the store whose descriptor has neither an author "id" nor a
 * product-block "key", it is renamed to a key generated from its name
 * (sh_mb_key_for_folder), the author's references to mods/<folder>/ are
 * rewritten to mods/<key>/ (decl text, mod.json, the folder segments of the
 * paths under assets/, in the written and the cooked '_' spelling), and its
 * product block records the key with origin "by-hand", as New mod writes it.
 * Organizing folders above it are left alone. Takes the store lock. Returns
 * 1 with the new folder (absolute, plain) in `out`; 0 when there is nothing
 * to do (an id, a key, not a store folder); -1 with the cause, the folder
 * untouched (a file that cannot be read, a binary file naming the folder,
 * two files landing on one path, a rename Windows refuses), which is also
 * recorded as the folder's build cause. Every build of a store folder in
 * place runs it first (rescan, install, sh_mods build and rebuild, the Mods
 * tab's Rebuild). An install from elsewhere of such a folder copies it in
 * under a generated key the same way; the source is only read. */
int  sh_ms_first_key(const char *folder, char *out, size_t cap, char *cause, size_t cause_cap);

/* What a first build renamed `folder` (absolute) from: 1 with the old folder
 * name and the count of references rewritten, from the record's "renamed"
 * section; 0 for any other folder. */
int  sh_ms_renamed_from(const char *folder, char *from, size_t cap, unsigned *references);

/* Record `folder` (absolute) as built from its current tree: cache[tree] =
 * bundle hash. Returns 1 on success. */
int  sh_ms_cache_note(const char *folder, const char *bundle_hash);

/* Last good build (DESIGN-additive 2.8). Every successful build of a store
 * folder (sh_ms_note_build, an install, a sync that finds a current folder
 * without one) keeps a snapshot of its carried files, never its stock
 * pointers, each copy checked against the build:
 *
 *   mods\.builds\<key>@<first 16 hex of the bundle hash>\bundle.json
 *       the folder's mod.json as that build wrote it (author and product
 *       block; sh_mb_load_text gives the same bundle hash)
 *   mods\.builds\<key>@<hash16>\assets\...
 *       every carried file at its "path" from the product block
 *
 * The record's "snapshots" section maps each folder's name under mods\ to
 * its one snapshot name. The next successful build replaces it, removing
 * the previous one unless another folder's row names it; uninstall
 * (sh_ms_delete_mod_folder, sh_ms_remove_folder) removes it; a sync removes
 * rows of folders that are gone and snapshots no row names. ".builds" starts
 * with '.', so no scan of the store enters it.
 *
 * sh_ms_snapshot_of: 1 with the absolute folder of `folder`'s complete
 * snapshot (it holds bundle.json), else 0. sh_ms_entry.snapshot is the same
 * path when the snapshot is of the entry's own bundle. */
#define SH_MS_BUILDS_DIR ".builds"
int  sh_ms_snapshot_of(const char *folder, char *out, size_t cap);

typedef struct sh_ms_upgrade_stats {
    int ran;                    /* the retired layout was found */
    unsigned folders;           /* built folders converted to mod.json */
    unsigned renamed;           /* of them, moved to <id> or <id>@<version> */
    unsigned delivered;         /* legacy\ folders moved into mods\ */
    unsigned dropped;           /* legacy\ folders dropped: an installed mod serves the same content */
    unsigned failed;            /* folders left as they were (logged) */
    unsigned unlinked;          /* hard-linked files replaced by real copies */
    unsigned restored;          /* files restored from blobs\ */
    int blobs_removed;          /* blobs\ was deleted */
    unsigned long long blob_bytes;
    unsigned records;           /* product files folded into snapmap-plus.json */
    unsigned long long ms;
    /* The second step (record "layout" 2): */
    int tidied;                 /* it ran */
    unsigned same_id;           /* delivered packages dropped: an installed mod has their id */
    unsigned covered;           /* delivered packages dropped: an installed mod under another id provides every path */
    unsigned tidy_renamed;      /* folders renamed to <id> (or away from a generated version) */
    unsigned tidy_failed;       /* folders that could not be removed or renamed */
    int migration_collapsed;    /* the completed migration record lost its finished folder rows */
    unsigned long long tidy_ms;
} sh_ms_upgrade_stats;

/* Convert the retired layout (<id>@<version>+<build>\ with bundle.json,
 * bundle.canonical.json and .state.json, hard links into blobs\, legacy\,
 * claims.json, publish-links.json, pending-uploads.json, .migrated.json) to
 * Contract 5, logging one "MODS: store:" line per folder and a summary.
 * Idempotent and safe to interrupt: each folder's mod.json is written (and
 * read back with the same bundle hash) before the old files go, and a record
 * file is folded before it is retired. A hard-linked file is renamed aside,
 * copied back to its name and verified against the bundle before the renamed
 * original is retired; on any failure the original is put back (and an
 * original an interrupted run left aside is put back at the next run).
 * Everything retired goes through sh_ms_set_discard's function when one is
 * set.
 *
 * Then, once per store (until the record's "layout" section is 2), the
 * second step: every package a map delivered that copies an installed mod
 * (sh_ms_delivered_counterpart) is removed and its bundle hash becomes an
 * alias of that mod's; a product folder left alone with its id under
 * "<id>@<version>" is renamed "<id>", and no folder keeps a generated version
 * in its name; a completed migration record keeps only "complete",
 * "migratedAt", "source", "trees" (the folder each converted tree became,
 * which resolves maps published before Contract 5; folder names follow the
 * drops and renames) and the rows of folders not yet done. One "MODS: store:"
 * line per dropped or renamed folder and a summary.
 *
 * Returns 1 when either step ran, 0 when there was nothing to do, -1 when the
 * store could not be read. */
int  sh_ms_upgrade(sh_ms_upgrade_stats *stats);

/* What sh_ms_upgrade and sh_ms_bin_sweep would do (sh_ms_upgrade_scan, read only). */
typedef struct sh_ms_upgrade_check {
    unsigned layout_folders;    /* folders with bundle.json, bundle.canonical.json or .state.json */
    int blobs_present;          /* blobs\ */
    unsigned blob_files;
    unsigned long long blob_bytes;
    int legacy_present;         /* legacy\ */
    unsigned legacy_folders;
    unsigned record_files;      /* claims.json, publish-links.json, pending-uploads.json, .migrated.json */
    char record_names[96];      /* their names, ", " between */
    unsigned tidy_drops;        /* the second step: delivered copies of an installed mod */
    unsigned tidy_renames;      /* the second step: product folders to rename */
    int tidy_migration;         /* the second step: a completed migration record to collapse */
    int bin_present;            /* removed-mods\ beside mods\ */
    unsigned bin_entries;
    int bin_section;            /* the record's "removed" section */
} sh_ms_upgrade_check;

/* Fill *out. Returns the number of kinds of work found (0: nothing to do),
 * -1 when the store folder cannot be resolved. */
int  sh_ms_upgrade_scan(sh_ms_upgrade_check *out);

/* How the upgrade and the bin sweep retire files and folders. NULL (the
 * default) deletes outright, links removed and never followed. `fn` receives
 * an absolute long-form path that exists and returns 1 when it is gone; `verb`
 * ("moved to the Recycle Bin") replaces "deleted" in the sweep's lines. Not
 * thread-safe: set it before the first call. */
typedef int (*sh_ms_discard_fn)(void *context, const wchar_t *path);
void sh_ms_set_discard(sh_ms_discard_fn fn, void *context, const char *verb);

#ifdef SH_MODSTORE_TESTING
/* > 0: that many hard-link copies of the upgrade fail after the copy (tests). */
extern int sh_ms_test_fail_unlink_copy;
#endif

enum { SH_MS_MISS = 0, SH_MS_HIT = 1, SH_MS_DEDUPE = 2, SH_MS_CONFLICT = 3 };

typedef struct sh_ms_hit {
    int    status;
    size_t entry, file;         /* the first match */
    size_t providers;           /* distinct usable bundles providing the name */
    size_t other_entry;         /* the second provider when providers > 1 */
} sh_ms_hit;

/* Find who provides logical (type, name) across usable installed bundles:
 * provides.type and provides.name compared case-insensitively, or a file
 * whose request path is one the engine would ask for that name. The first
 * match wins. Two bundles providing it are a CONFLICT for decl names (and for
 * raw files with different bytes) and a DEDUPE for raw files with equal
 * hashes. Returns the status. */
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

/* Remove a folder under the store root, under the store lock: to the Recycle
 * Bin (SHFileOperationW, FOF_ALLOWUNDO, no UI, reparse points moved as
 * themselves), else deleted outright with links removed and never followed.
 * One "MODS: store:" line says which. Refuses anything outside the root and
 * the root itself. SH_MS_RECYCLED, SH_MS_DELETED, or 0. */
int  sh_ms_remove_folder(const char *folder);

/* Uninstall's removal: `folder` (absolute) must be a direct child of the
 * store root that exists; it goes as sh_ms_remove_folder's does.
 * SH_MS_RECYCLED; SH_MS_DELETED with why the bin could not take it in
 * `cause`; else 0 with the first cause. The record's aliases are the caller's
 * (sh_ms_alias_forget) and the build cache follows at the next sync. */
int  sh_ms_delete_mod_folder(const char *folder, char *cause, size_t cap);

#if defined(SH_MODSTORE_TESTING) || defined(SH_MODUI_TESTING)
/* Test seam: nonzero makes every removal take the outright-delete fallback,
 * as when the shell refuses. */
void sh_ms_recycle_refuse_for_test(int refuse);
#endif

/* The retired recycle bin. Retires every entry of removed-mods\ (the store
 * root's sibling) through the discard (sh_ms_set_discard), then removes the
 * empty folder and the record's "removed" section, under the store lock. One
 * "MODS: removed-mods: <name> deleted" line per entry ("moved to the Recycle
 * Bin" with the store tool's discard) and, when anything was there, the
 * summary "MODS: removed-mods: N folder(s) deleted; the bin is retired", each
 * to the backend log and to `line` when given. Idempotent: a later run finds
 * nothing. The store tool runs it. Returns the entries removed, or -1 when one
 * could not be removed (it is tried again at the next run). */
typedef void (*sh_ms_sweep_line_fn)(void *context, const char *line);
int  sh_ms_bin_sweep(sh_ms_sweep_line_fn line, void *context);

#endif /* SNAPMAP_PLUS_MODSTORE_H */
