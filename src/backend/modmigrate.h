/* modmigrate.h -- the one-time move of the retired overrides\ tree into the
 * mods\ store.
 *
 * Releases up to v0.2.1-beta.19 kept a player's packages under
 * %LOCALAPPDATA%\snapmap-plus\overrides\: authored packs (a folder with
 * package.json or mod.json, era B or era C layout) and, from beta.16, the
 * packages a map delivered (overrides\map-<16 hex>\<package>). The first
 * start of a build with this module, after the store is ready and before the
 * serving index is built, does this once:
 *
 *   - every direct child folder with a descriptor is built by the normal
 *     builder into mods\<id>@<version>+<build> with blobs (modstore.c). The
 *     descriptor is the authoring descriptor: its era B or C layout is
 *     migrated to assets/ (modlegacy.c), its requirements, strings and hud are
 *     carried, every other key (composes, an era B schema) is preserved under
 *     "extra", and a "legacy" block grandfathers its non-namespaced names;
 *   - every map-<16 hex>\<package> folder is built as a legacy bundle under
 *     mods\legacy\<id>@<tree16>, keyed by the beta.16-19 tree fingerprint that
 *     is also the "smpkg.delivery.<tree>" key a map carries for it, so a later
 *     load of a map carrying the same delivery reuses it instead of converting
 *     again;
 *   - a folder without a descriptor whose one package is a direct child
 *     folder has that child migrated under the child's id (beta.19 read such
 *     folders as groups); with none or several, it is skipped, and a ZIP and
 *     any other file are listed as skipped with the reason; two folders with one id are both built (the
 *     second under a distinct version) and the duplicate is reported;
 *   - a folder over 512 MiB or 8192 files is skipped with its size;
 *   - nothing under overrides\ is created, changed or deleted; an empty
 *     backups folder (beta.15) is removed and a non-empty one is left alone.
 *
 * The outcome per folder goes to mods\.migrated.json and to one "MODS:
 * migration:" log line per folder plus a summary line. Folders up to
 * SH_MODMIGRATE_BOOT_BYTES and SH_MODMIGRATE_BOOT_FILES are done at boot; larger ones are done on a worker
 * thread that rebuilds the serving index when it finishes. The record is
 * written with "complete": false until the worker is done, and an incomplete
 * record makes the next start run the pass again (every step is idempotent).
 * `sh_mods migrate [--again]` runs the same pass on demand on the worker.
 *
 * File work only; no engine calls. Stock names are not checked during the
 * migration (the game's resource reader is not up at boot): a legacy bundle's
 * unresolved needs only warn, as they do for a converted map carrier.
 */
#ifndef SNAPMAP_PLUS_MODMIGRATE_H
#define SNAPMAP_PLUS_MODMIGRATE_H

#include <stddef.h>

#define SH_MODMIGRATE_MAX_BYTES   (512ull << 20)   /* a bigger folder is skipped */
#define SH_MODMIGRATE_MAX_FILES   8192u            /* a folder with more files is skipped */
/* During boot only small folders are migrated (the cost is per file: about
 * 3-7 ms each for the walk, staging, blob copy, link and verify); the rest
 * goes to the worker. */
#define SH_MODMIGRATE_BOOT_BYTES  (16ull << 20)    /* a folder up to this many bytes ... */
#define SH_MODMIGRATE_BOOT_FILES  32u              /* ... and this many files is migrated during boot */
#define SH_MODMIGRATE_BOOT_TOTAL  (64ull << 20)    /* at most this many bytes ... */
#define SH_MODMIGRATE_BOOT_TOTAL_FILES 256u        /* ... and files are migrated during boot */
#define SH_MODMIGRATE_BOOT_MS     1000u            /* and no folder starts after this much boot time */
#define SH_MODMIGRATE_RECORD      ".migrated.json"

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
    unsigned long long ms;       /* wall time of the pass so far */
    unsigned long long boot_ms;  /* the part done before the serving index (boot only) */
    int ran;                     /* 1 when a pass ran; 0 when it had nothing to do */
    int backups_removed;         /* an empty beta.15 backups folder was removed */
} sh_modmigrate_stats;

/* The data root (default %LOCALAPPDATA%\snapmap-plus, ANSI like the legacy
 * converter's); NULL restores the default. Tests and measurements point it,
 * the legacy converter (sh_modlegacy_set_data_root) and the store
 * (sh_ms_set_root, "<root>\mods") at one scratch folder. */
void sh_modmigrate_set_data_root(const char *root_acp);

/* Backend start, before sh_modserve_index_boot: runs the pass when overrides\
 * exists and the record does not (or says incomplete). Small folders are done
 * here; a worker is started for the rest. Returns 1 when a pass ran or
 * started, 0 when there was nothing to do, -1 on a failure (logged). */
int sh_modmigrate_boot(void);

/* The whole pass on the calling thread (tests, measurements). `again` runs it
 * even when the record says it is complete. Returns 1 when a pass ran, 0 when
 * there was nothing to do, -1 when the worker is busy or the pass failed. */
int sh_modmigrate_run(int again, sh_modmigrate_stats *stats);

/* The whole pass on the worker thread (`sh_mods migrate [--again]`). Returns
 * 1 when started, 0 when there is nothing to do, -1 when busy. */
int sh_modmigrate_start(int again);

/* 1 while the worker runs. */
int sh_modmigrate_busy(void);

/* Wait up to `ms` for the worker; 1 when it is idle. */
int sh_modmigrate_wait(unsigned ms);

/* The statistics of the last pass (or the running one). */
void sh_modmigrate_last(sh_modmigrate_stats *out);

#endif /* SNAPMAP_PLUS_MODMIGRATE_H */
