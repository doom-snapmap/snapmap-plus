/* modmigrate.h -- the one-time move of the retired overrides\ tree into the
 * mods\ store.
 *
 * Releases up to v0.2.1-beta.19 kept a player's packages under
 * %LOCALAPPDATA%\snapmap-plus\overrides\: authored packs (a folder with
 * package.json or mod.json, era B or era C layout) and, from beta.16, the
 * packages a map delivered (overrides\map-<16 hex>\<package>). The store
 * tool (snapmap-plus-store migrate, which the installer runs) does this once,
 * after the store upgrade, with the game's archives as the stock bridge's
 * install. Linked only into the store tool:
 *
 *   - every direct child folder with a descriptor is built by the normal
 *     builder into mods\<folder>\ (the overrides folder's own name; with the
 *     tree's first 8 hex when another mod holds that name) as mod.json and
 *     assets\ (modstore.c). The author block is the package's descriptor: its
 *     era B or C layout is migrated to assets/ (modlegacy.c), its requirements,
 *     strings and hud are carried, every other key (composes, an era B schema)
 *     is kept; package.json is not carried as a file;
 *   - every map-<16 hex>\<package> folder is built as an ordinary mod folder
 *     mods\<id>\, listed in the record's "trees" under the beta.16-19 tree
 *     fingerprint that is also the "smpkg.delivery.<tree>" key a map carries
 *     for it, so the conversion of a map carrying the same delivery reuses it;
 *     a delivery an installed mod already serves is not stored twice;
 *   - a folder without a descriptor whose one package is a direct child
 *     folder has that child migrated under the child's id (beta.19 read such
 *     folders as groups); with several, it is skipped, and a ZIP is listed as
 *     skipped with the reason; two folders with one id are both built (in
 *     their own folders) and the duplicate is reported;
 *   - the retired loose-file layer is not this pass's: every other file
 *     directly in overrides\ and every folder with no descriptor and no
 *     package folder one level down (overrides\<engine path>, as SnapHak and
 *     the first releases served it); the loose step below moves it;
 *   - a folder over 512 MiB or 8192 files is skipped with its size;
 *   - a file byte-identical to installed game content (any archive, the
 *     .patch store included, or a loose install file) becomes a stock
 *     pointer: the store never receives it, and each folder's line and the
 *     summary say how many files became pointers and how many bytes were not
 *     stored;
 *   - nothing under overrides\ is created or changed; an empty backups
 *     folder (beta.15) is removed and a non-empty one is left alone. The
 *     store tool retires overrides\ itself (sh_modmigrate_retire) once every
 *     entry of it is in mods\.
 *
 * The outcome per folder goes to the "migration" section of
 * mods\snapmap-plus.json (modbundle.h) and to one "MODS:
 * migration:" log line per folder plus a summary line. The record is written
 * with "complete": false while a pass runs, and an incomplete record makes the
 * next pass run again (every step is idempotent).
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
    SH_MODMIGRATE_PENDING        /* not done yet in this pass */
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

/* The data root (default %LOCALAPPDATA%\snapmap-plus, UTF-8 like the legacy
 * converter's); NULL restores the default. Tests and measurements point it,
 * the legacy converter (sh_modlegacy_set_data_root) and the store
 * (sh_ms_set_root, "<root>\mods") at one scratch folder. */
void sh_modmigrate_set_data_root(const char *root_utf8);

/* The whole pass on the calling thread. `again` runs it even when the record
 * says it is complete. Returns 1 when a pass ran, 0 when there was nothing to
 * do, -1 when the pass failed. */
int sh_modmigrate_run(int again, sh_modmigrate_stats *stats);

/* Entries of overrides\ that a pass would still pick up: not in the record,
 * listed as pending, or skipped or refused and changed (the entry or its
 * descriptor written) since the record was written. Built, converted and
 * reused entries never count. Returns the count (0 when there is no
 * overrides\ folder) and sets *entries to the entries looked at, or -1 when
 * the data folder cannot be resolved. File reads only. */
int sh_modmigrate_pending(unsigned *entries);

/* The loose step. The retired loose-file layer of overrides\ and the strings
 * document of earlier releases (<data>\strings\strids.json, a flat object of
 * "id": "text" pairs that strids.c no longer reads) become one mod,
 * mods\my-overrides\: the loose files under assets\ at their paths below
 * overrides\, converted as a package is (files a mod cannot carry left out;
 * copies of installed content become stock pointers), and the document's keys
 * as the author part's strings.en, with the name "My overrides" (the salting
 * prefixes the keys with the salt). When
 * mods\my-overrides\ already exists, only the files and keys it lacks are
 * added: one "kept my-overrides' <path or key>" line for each it has with
 * other bytes or text, nothing for an identical one; its tree changes, so the
 * next start or rescan builds it again. A document that is not such an object
 * is left with one line. The "loose" section of mods\snapmap-plus.json
 * records the step; an entry or document added or changed after it is due
 * again. Takes the store lock. Returns 1 when it ran, 0 when there was nothing
 * to do, -1 when it could not finish (a later run tries again). */
typedef struct sh_modmigrate_loose_stats {
    int created;                 /* this step made mods\my-overrides\ */
    unsigned entries;            /* loose entries of overrides\ moved */
    unsigned files_added, files_kept, files_same, files_failed, left_out, pointers;
    unsigned strings_added, strings_kept, strings_same;
    int strings_bad;             /* the document is not a strings document; left */
} sh_modmigrate_loose_stats;
int sh_modmigrate_loose_run(sh_modmigrate_loose_stats *stats);

/* What the loose step and the retirement would do (file reads only). Returns
 * the kinds of work found, -1 when the data folder cannot be resolved. */
typedef struct sh_modmigrate_loose_check {
    unsigned loose_entries;      /* loose entries of overrides\ */
    unsigned loose;              /* loose entries of overrides\ added or changed since the loose step */
    int strings_due;             /* the document is there and not yet moved */
    unsigned strings;            /* its keys */
    int strings_bad;             /* it is not a strings document: `why` */
    char why[200];
    unsigned left;               /* entries of overrides\ not in mods\ (skipped, refused or not yet moved) */
    char first_left[260];
    int overrides_done;          /* overrides\ exists and every entry of it is in mods\ */
    int strings_done;            /* strings\ exists and holds no document left to move */
} sh_modmigrate_loose_check;
int sh_modmigrate_loose_scan(sh_modmigrate_loose_check *out);

/* Retire overrides\ when every entry of it is in mods\, and strings\ when its
 * document was moved (or it has none), through the store's discard
 * (sh_ms_retire_sibling: the store tool's Recycle Bin; without a discard both
 * stay). One line each, and one naming the first entry that keeps overrides\.
 * Returns the folders retired, -1 when the data folder cannot be resolved. */
int sh_modmigrate_retire(void);

#endif /* SNAPMAP_PLUS_MODMIGRATE_H */
