/* modstore_upgrade.h -- the store tool's half of the mod store: the retired
 * layouts of earlier releases and the retired recycle bin. Linked only into
 * the store tool (src\store\build.ps1, snapmap-plus-store.exe), never into
 * the game DLL: the game reads only the one shape (modstore.h) and refuses
 * every other with its reason; the installer's store tool converts what
 * exists once, at update (ONE-SHAPE rule 6).
 *
 * The earlier shapes it reads (modbundle_old.h for mod.json of format 1):
 *   - Contract 2: <id>@<version>+<build>\ folders with bundle.json,
 *     bundle.canonical.json and .state.json, their assets\ hard-linked to
 *     blobs\<sha256>, legacy\<id>@<digest16>\ for delivered packages, and
 *     claims.json, publish-links.json, pending-uploads.json and
 *     .migrated.json beside them;
 *   - the second step of that upgrade (record "layout" below 2): format-1
 *     folders a map delivered that copy an installed mod, product folders
 *     named "<id>@<version>" or "@legacy-", the completed migration record;
 *   - removed-mods\ beside mods\.
 */
#ifndef SNAPMAP_PLUS_MODSTORE_UPGRADE_H
#define SNAPMAP_PLUS_MODSTORE_UPGRADE_H

#include <stddef.h>
#include <wchar.h>

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

/* Convert the Contract 2 layout in place, then (once per store, until the
 * record's "layout" section is 2) the second step. One "MODS: store:" line
 * per folder and a summary. Idempotent and safe to interrupt. A folder whose
 * bundle.json this release cannot read is left as it was, logged, and keeps
 * blobs\ in place. Everything retired goes through sh_ms_set_discard's
 * function when one is set. Returns 1 when either step ran, 0 when there was
 * nothing to do, -1 when the store could not be read. */
int  sh_ms_upgrade(sh_ms_upgrade_stats *stats);

/* What sh_ms_upgrade and sh_ms_bin_sweep would do (read only). */
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

/* Retire `leaf`, a folder beside the store root in the data folder (the
 * retired overrides\ and strings\ of earlier releases), through the discard
 * set with sh_ms_set_discard. Without one it is left where it is. 1 when it
 * is gone (or was never there), 0 when it could not be retired, -1 when no
 * discard is set. */
int  sh_ms_retire_sibling(const wchar_t *leaf);
/* The discard's verb ("moved to the Recycle Bin"; "deleted" without one). */
const char *sh_ms_discard_verb(void);

#ifdef SH_MODSTORE_TESTING
/* > 0: that many hard-link copies of the upgrade fail after the copy (tests). */
extern int sh_ms_test_fail_unlink_copy;
#endif

/* The retired recycle bin. Retires every entry of removed-mods\ (the store
 * root's sibling) through the discard, then removes the empty folder and the
 * record's "removed" section, under the store lock. One "MODS: removed-mods:
 * <name> deleted" line per entry ("moved to the Recycle Bin" with the store
 * tool's discard) and, when anything was there, the summary "MODS:
 * removed-mods: N folder(s) deleted; the bin is retired", each to the backend
 * log and to `line` when given. Idempotent. Returns the entries removed, or -1
 * when one could not be removed (it is tried again at the next run). */
typedef void (*sh_ms_sweep_line_fn)(void *context, const char *line);
int  sh_ms_bin_sweep(sh_ms_sweep_line_fn line, void *context);

#endif /* SNAPMAP_PLUS_MODSTORE_UPGRADE_H */
