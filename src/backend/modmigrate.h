/* modmigrate.h -- the one-time move of the retired overrides\ tree into the
 * mods\ store.
 *
 * Releases up to v0.2.1-beta.19 kept a player's packages under
 * %LOCALAPPDATA%\snapmap-plus\overrides\: authored packs (a folder with
 * package.json or mod.json, era B or era C layout) and, from beta.16, the
 * packages a map delivered (overrides\map-<16 hex>\<package>). The store
 * tool (snapmap-plus-store migrate, which the installer runs) does this once,
 * after the store upgrade, with the game's archives as the stock bridge's
 * install; the game runs it only by hand (`sh_mods migrate [--again]`):
 *
 *   - every direct child folder with a descriptor is built by the normal
 *     builder into mods\<folder>\ (the overrides folder's own name; with the
 *     tree's first 8 hex when another mod holds that name) as mod.json and
 *     assets\ (modstore.c). The author block is the package's descriptor: its
 *     era B or C layout is migrated to assets/ (modlegacy.c), its requirements,
 *     strings and hud are carried, every other key (composes, an era B schema)
 *     is kept, and the product block's "legacy" record grandfathers its
 *     non-namespaced names; package.json is not carried as a file;
 *   - every map-<16 hex>\<package> folder is built as an ordinary mod folder
 *     mods\<id>\, its legacy record keyed by the beta.16-19 tree fingerprint
 *     that is also the "smpkg.delivery.<tree>" key a map carries for it, so a
 *     later load of a map carrying the same delivery reuses it instead of
 *     converting again; a delivery an installed mod already serves is not
 *     stored twice;
 *   - a folder without a descriptor whose one package is a direct child
 *     folder has that child migrated under the child's id (beta.19 read such
 *     folders as groups); with none or several, it is skipped, and a ZIP and
 *     any other file are listed as skipped with the reason; two folders with one id are both built (in
 *     their own folders) and the duplicate is reported;
 *   - a folder over 512 MiB or 8192 files is skipped with its size;
 *   - a file byte-identical to installed game content (any archive, the
 *     .patch store included, or a loose install file) becomes a stock
 *     pointer: the store never receives it, and each folder's line and the
 *     summary say how many files became pointers and how many bytes were not
 *     stored;
 *   - nothing under overrides\ is created, changed or deleted; an empty
 *     backups folder (beta.15) is removed and a non-empty one is left alone.
 *
 * The outcome per folder goes to the "migration" section of
 * mods\snapmap-plus.json (modbundle.h) and to one "MODS:
 * migration:" log line per folder plus a summary line. The record is written
 * with "complete": false while a pass runs, and an incomplete record makes the
 * next pass run again (every step is idempotent). The store tool runs the pass
 * on its own thread (sh_modmigrate_run); `sh_mods migrate [--again]` runs it on
 * the game's worker, which rebuilds the serving index when it finishes.
 *
 * File work only; no engine calls. Stock names are not checked during the
 * migration (it runs without the game's resource reader): a legacy bundle's
 * unresolved needs only warn, as they do for a converted map carrier.
 */
#ifndef SNAPMAP_PLUS_MODMIGRATE_H
#define SNAPMAP_PLUS_MODMIGRATE_H

#include <stddef.h>

#define SH_MODMIGRATE_MAX_BYTES   (512ull << 20)   /* a bigger folder is skipped */
#define SH_MODMIGRATE_MAX_FILES   8192u            /* a folder with more files is skipped */
#define SH_MODMIGRATE_RECORD      "migration"      /* the section of mods\snapmap-plus.json */

/* Per-folder outcomes in the record. */
enum {
    SH_MODMIGRATE_BUILT = 1,     /* built into the store (or found there unchanged) */
    SH_MODMIGRATE_CONVERTED,     /* a delivered package built as a legacy bundle */
    SH_MODMIGRATE_REUSED,        /* a delivered package already converted (from a map or earlier) */
    SH_MODMIGRATE_SKIPPED,       /* not a package, or over the cap: nothing built */
    SH_MODMIGRATE_REFUSED,       /* a package the builder refused: the first cause is recorded */
    SH_MODMIGRATE_PENDING        /* waiting for the worker */
};

typedef struct sh_modmigrate_stats {
    unsigned entries;            /* direct children of overrides\ looked at (and delivered packages) */
    unsigned built, converted, reused, skipped, refused, duplicates, pending;
    unsigned long long bytes;    /* bytes of the folders that were built or converted */
    unsigned pointers;           /* files that became stock pointers to installed content */
    unsigned long long pointer_bytes;   /* their bytes: never stored or uploaded */
    unsigned long long ms;       /* wall time of the pass so far */
    int ran;                     /* 1 when a pass ran; 0 when it had nothing to do */
    int backups_removed;         /* an empty beta.15 backups folder was removed */
} sh_modmigrate_stats;

/* The data root (default %LOCALAPPDATA%\snapmap-plus, ANSI like the legacy
 * converter's); NULL restores the default. Tests and measurements point it,
 * the legacy converter (sh_modlegacy_set_data_root) and the store
 * (sh_ms_set_root, "<root>\mods") at one scratch folder. */
void sh_modmigrate_set_data_root(const char *root_acp);

/* The whole pass on the calling thread (the store tool, tests). `again` runs
 * it even when the record says it is complete. Returns 1 when a pass ran, 0 when
 * there was nothing to do, -1 when the worker is busy or the pass failed. */
int sh_modmigrate_run(int again, sh_modmigrate_stats *stats);

/* The whole pass on the worker thread (`sh_mods migrate [--again]`). Returns
 * 1 when started, 0 when there is nothing to do, -1 when busy. */
int sh_modmigrate_start(int again);

/* 1 while the worker runs. */
int sh_modmigrate_busy(void);

/* Wait up to `ms` for the worker; 1 when it is idle. */
int sh_modmigrate_wait(unsigned ms);

/* Entries of overrides\ that a pass would still pick up: not in the record,
 * listed as pending, or skipped or refused and changed (the entry or its
 * descriptor written) since the record was written. Built, converted and
 * reused entries never count. Returns the count (0 when there is no
 * overrides\ folder) and sets *entries to the entries looked at, or -1 when
 * the data folder cannot be resolved. File reads only. */
int sh_modmigrate_pending(unsigned *entries);

/* The statistics of the last pass (or the running one). */
void sh_modmigrate_last(sh_modmigrate_stats *out);

#endif /* SNAPMAP_PLUS_MODMIGRATE_H */
