/* modstore.h -- the local mod store, %LOCALAPPDATA%\snapmap-plus\mods\.
 *
 *   <folder>\mod.json            the author's part and the builder block (modbundle.h)
 *   <folder>\assets\...          the mod's files, real files only (no links, no blob store)
 *   maps\<map title>\<mod name>\ a mod a map brought (a download, or an import of a
 *                                rawmap or prefab ZIP): a "place" (below)
 *   snapmap-plus.json            the product record (modbundle.h): claims, links,
 *                                the places, which folder holds each salt
 *
 * Identity is the salt (modsalt.h, ONE-SHAPE rule 1): eight characters in the
 * builder block, given once. A folder name is the author's and never carries
 * identity, so any folder may be renamed at any time and the next scan reads
 * the same salt. A folder is a mod when it holds mod.json (package.json is an
 * earlier shape and is refused with its reason).
 *
 * Areas (rule 4). The root is everything under mods\ except mods\maps\:
 * the player's own mods, with organizing folders to SH_MS_NEST_MAX levels.
 * One salt has one folder at the root. A place, mods\maps\<segment>\, holds
 * the builds one map pins, one folder per salt, named after its mod; a place
 * folder is a received build and is never built again here. "maps" is a
 * reserved name at the top of mods\.
 *
 * The scan (sh_ms_sync: boot, Refresh, and after an install, a download or
 * New mod; no folder watcher, rule 3). Under the store lock:
 *   1. a root folder with no builder block is a new mod: it gets its salt
 *      (sh_ms_assign_salt) and keeps its folder name;
 *   2. two root folders carrying one salt (a copied folder): the folder the
 *      record's "salts" section names (the one that held it at the last
 *      scan) keeps it, else the one created first; every other one gets a
 *      new salt with its own references rewritten. The scan then records
 *      each salt's folder;
 *   3. a root folder whose tree hash (sh_mb_tree_hash) or builder rules
 *      differ from its block's, or whose last build failed, is built again;
 *      a failed build records its cause in the block and the folder serves
 *      nothing until fixed (rule 2: no snapshots, no last good build).
 * Before a root folder is built (1 and 3), what the author wrote under its
 * folder name moves under the salt (DECISIONS 20): files under
 * assets\...\mods\<folder name>\, references to mods/<folder name>/ in
 * mod.json and every text under assets\ (scripts among them), strings keys
 * without the "<salt>_" prefix that are not string edits with their #str_
 * references, and a sound bank "<folder name>_x.bnk" (renamed
 * "<salt>_x.bnk", its id following the name). A folder name equal to a salt
 * of the store names that mod and is never moved. Renaming a folder changes
 * nothing already moved. Files beside mod.json and assets\ are left alone.

 * A folder in an earlier shape is listed with its plain reason and nothing in
 * it is written. Enumeration only reads.
 *
 * File work only, on the calling thread; no engine calls. Every writer of the
 * store serialises on modstore_lock: the install, verification and sync take
 * it themselves, and longer steps (a download install, `sh_mods install`, an
 * export) hold it around their whole step. A second writer waits for the
 * first, it never fails because of it.
 */
#ifndef SNAPMAP_PLUS_MODSTORE_H
#define SNAPMAP_PLUS_MODSTORE_H

#include <stddef.h>
#include "modbundle.h"
#include "modsalt.h"

/* The store lock (recursive per thread): the mods folder's named lock
 * (store_lock.h; the record lock is the same mutex), shared with every other
 * process on the folder (a second game, the store tool, which holds it for
 * its whole run). modsync_store_lock and modsync_store_unlock (modsync.h)

 * take the same lock. A thread that finds it held logs one "MODS: store:
 * waiting" line and waits. The counts below are this process's. */

void modstore_lock(void);
void modstore_unlock(void);
/* How many lock calls had to wait so far (tests, diagnostics). */
unsigned modstore_lock_waits(void);
/* Any thread: 1 while some thread holds the lock (a store write may be in
 * progress), else 0. *generation (when non-NULL) counts the outermost holds
 * released so far, so a change tells that a hold ended since the last look. */
int      modstore_lock_busy(unsigned *generation);

/* A label buffer of an entry (sh_ms_entry.key). */
#define SH_MS_KEY_MAX (SH_MB_ID_MAX + 1 + 128 + 1 + 20)
/* A folder name under mods\. */
#define SH_MS_NAME_MAX 1023   /* a nested folder's path under mods\ */
#define SH_MS_NEST_MAX 8      /* organizing folders are entered this many levels below mods\ */
/* The top folder of the places: mods\maps\<segment>\<mod>. */
#define SH_MS_PLACES "maps"

/* The store root. NULL or "" restores the default (%LOCALAPPDATA%\snapmap-plus\mods).
 * Tests point it at a temporary folder. Returns 1 when the root is set. */
int  sh_ms_set_root(const char *root_utf8);
/* The current root as UTF-8, or 0 when %LOCALAPPDATA% cannot be resolved. */
int  sh_ms_root(char *out, size_t cap);

typedef struct sh_ms_entry {
    char *folder;               /* absolute UTF-8 folder of the mod */
    char  name[SH_MS_NAME_MAX + 1];   /* the folder's path under mods\ ("x", "group\x", "maps\<map>\x") */
    char  key[SH_MS_KEY_MAX + 1];     /* a label: the salt, or the folder name while it has none */
    char  salt[SH_SALT_LEN + 1];      /* the block's salt; "" when it has none */
    char  place[SH_MS_NAME_MAX + 1];  /* "" at the root; "maps\<segment>" for a folder in a map's place */
    int   state;                /* sh_mb_descriptor_state: SH_MB_DESC_* */
    int   refused;              /* listed with its cause and serves nothing: an earlier shape, files under
                                   another salt, a copy waiting for its new salt, a second folder of one
                                   salt in a place, a new folder in a place */
    int   loaded;               /* mod.json read and its hash matched */
    int   sizes_ok;             /* every carried file present with its recorded size */
    int   validated;            /* the block records no "cause" */
    long long installed_at;     /* the block's "built" */
    char  cause[SH_MB_CAUSE_MAX];   /* why the entry is unusable, or the block's cause */
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

/* List every mod folder of the store, root and places, sorted by name. A
 * folder not built yet, or refused, is listed unloaded with its cause; a copy
 * of a root salt (before the scan gives it its own) and a second folder of one
 * salt in a place are listed refused. Returns 1 when the root could be read
 * (an absent root is an empty list). */
int  sh_ms_enumerate(sh_ms_list *out);
void sh_ms_list_free(sh_ms_list *list);

/* The walk every reader of the store shares (enumeration, the scan, the
 * serving index), so they agree on what is a mod: below the root, a folder
 * holding mod.json or package.json is a mod and is not entered; one without is
 * an organizing folder and is entered to SH_MS_NEST_MAX levels; dot folders,
 * reparse points and "<name>.staging-<pid>" / "<name>.old-<pid>" work folders
 * are skipped; mods\maps\ is entered as the places. `fn` receives the absolute
 * folder (UTF-8, long-path form), its name under mods\ and its place ("" at
 * the root, else "maps\<segment>"); a nonzero return stops the walk. Returns
 * 1 when the root could be read (an absent root walks nothing). */
typedef int (*sh_ms_walk_fn)(void *context, const char *folder, const char *name, const char *place);
int  sh_ms_walk(sh_ms_walk_fn fn, void *context);

/* Whether `folder` holds mod.json or package.json (a mod folder). */
int  sh_ms_folder_is_mod(const char *folder);

/* Every mod folder under `dir`, at any depth, as the walk above finds them
 * (the places rule applies when `dir` is the store root). `dir` itself is not
 * listed. Absolute UTF-8 paths in *out, freed with sh_ms_find_free. Returns 1
 * when `dir` could be read. */
int  sh_ms_find_mods(const char *dir, char ***out, size_t *count);
void sh_ms_find_free(char **folders, size_t count);

/* The entry whose build hash is `hash`, a usable one first; NULL when none. */
const sh_ms_entry *sh_ms_resolve(const sh_ms_list *list, const char *hash);

/* The entry of `salt` in an area: `place` NULL for the root, "*" for any
 * place, else that place ("maps\<segment>"); `hash` NULL for any build, else
 * that build only. A usable entry first; refused entries never. NULL when none. */
const sh_ms_entry *sh_ms_find_salt(const sh_ms_list *list, const char *salt, const char *place, const char *hash);

/* A folder of the store was built and its mod.json written: no earlier
 * failure of it stands. Returns 1. */
int  sh_ms_note_build(const char *folder, const sh_mb_bundle *b);

/* Identity (rule 1). A folder whose mod.json has the author's part only (no
 * builder block) is a new mod: it gets a new salt (sh_salt_generate, never one
 * a folder of the store holds), every reference to mods/<from>/ in its own
 * files becomes mods/<salt>/ (decl text and any other text, the folder
 * segments of paths under assets/, both spellings, sh_mb_rekey_text and
 * sh_mb_rekey_path), and its block becomes {"format": 2, "salt": <salt>}
 * (sh_mb_salt_modjson). Its strings keys gain the salt's prefix, "<key>"
 * becoming "<salt>_<key>", and every "#str_<key>" reference in its own text
 * files (mod.json among them) follows (sh_mb_salt_strings,
 * sh_mb_restr_text; DECISIONS 6), so an author writes plain keys in a new
 * folder. `from` is normally the folder's own name; the store
 * tool passes a converted package's id. The folder keeps its name: the result
 * is staged beside it ("<name>.staging-<pid>", unchanged files hard-linked)
 * and swapped in with two renames, the second undone on failure; the old
 * folder goes to the Recycle Bin. A file that cannot be read, a binary file
 * naming mods/<from>/ or one of its strings, files under another mod's mods/<x>/, two files landing
 * on one path, or a refused rename leaves the folder exactly as it was.
 * Works on any folder (the store tool salts a staged folder outside the
 * store); a new folder in a map's place is refused (a place holds received
 * builds only). Takes the store lock.
 *
 * Returns 1 with the new salt in salt_out; 0 when the folder already has its
 * salt (salt_out receives it); -1 with the plain reason in `cause`, the
 * folder untouched (also an earlier shape, or a mod.json that is not a JSON
 * object). A refusal of a store folder is also recorded as its build cause. */
int  sh_ms_assign_salt(const char *folder, const char *from, char salt_out[SH_SALT_LEN + 1], char *cause, size_t cause_cap);

/* sh_ms_assign_salt with `salt` instead of a new one: the store tool's
 * overrides migration, moving a package again into the folder an earlier
 * pass made of it, keeps that folder's identity. */
int  sh_ms_assign_salt_as(const char *folder, const char *from, const char *salt, char *cause, size_t cause_cap);

/* The same salting written to a new folder `dest` (it must not exist; files
 * the salting does not rewrite are hard-linked), with `modjson` (a fresh
 * author text: no block, no retired key) in place of the folder's own
 * descriptor; `folder` is never touched. The store tool converts a folder
 * of an earlier shape through it: salted and built in `dest`, swapped in
 * only when the build succeeds. Returns 1 with the salt, -1 with the cause. */
int  sh_ms_salt_copy(const char *folder, const char *modjson, size_t modjson_len, const char *from, const char *dest,
                     char salt_out[SH_SALT_LEN + 1], char *cause, size_t cause_cap);

/* A renamed sound bank's stem, old and new. */
typedef struct sh_ms_bank_rename { char from[SH_MB_BANK_STEM_MAX + 1], to[SH_MB_BANK_STEM_MAX + 1]; } sh_ms_bank_rename;

/* `text` with every bank stem `names[k].from` that stands as a whole word
 * (letters, digits and '_' around it do not) replaced by its `to`, ignoring
 * case. Returns the count with the result in *out (malloc'd) when it is not
 * 0, or (size_t)-1 out of memory. A renamed bank takes every text of its mod
 * that names it along: decls, scripts and mod.json. */
size_t sh_ms_rebank_text(const char *text, size_t len, const sh_ms_bank_rename *names, size_t count, char **out,
                         size_t *out_len);

typedef struct sh_ms_install_stats {
    unsigned copied;            /* files copied into the store (the game files the folder holds too) */
    int in_place;               /* the source folder was inside the store */
    int replaced;               /* the named target (as_name), or the place's folder of the salt, was replaced */
    int present;                /* a usable folder already holds this build: nothing was copied */
    int resalted;               /* a copy of a root mod: it was given a new salt */
    unsigned stock_files;       /* stock pointers (borrows); a copy the folder holds is kept */
    unsigned long long stock_bytes;   /* their installed size */
} sh_ms_install_stats;

/* Install `folder` at the root. A folder of the store (at the root) is built
 * in place. From elsewhere (the source is only read):
 *   - a new mod (no builder block) is copied whole to mods\<its name> (" (2)"
 *     when taken), salted with its references to mods/<its name>/ rewritten,
 *     and built; a refused build leaves nothing installed;
 *   - a built mod is built in memory; when the root folder of its salt holds
 *     this very build, that folder is the install (stats->present); else
 *     every file it holds (the game files it carries too: nothing is dropped)
 *     and its mod.json are copied to mods\<its name> (" (2)" ...),
 *     verified, and, when the root already holds the salt, the arriving copy
 *     gets a new salt (stats->resalted) and is built under it.
 * `as_name` (sh_ms_install_as: the store tool's target) names the folder under
 * mods\ instead; a folder of that name is replaced. `resolver` may be NULL;
 * the store's installed bundles are consulted in addition to it. On success
 * name_out holds the folder name under mods\. bundle_out (optional) receives
 * the bundle as installed; release it with sh_mb_free. Returns 1, else 0 with
 * the first cause. */
int  sh_ms_install_from_folder(const char *folder, const sh_mb_resolver *resolver,
                               char *name_out, size_t name_cap, sh_ms_install_stats *stats,
                               sh_mb_bundle *bundle_out, char *cause, size_t cause_cap);
int  sh_ms_install_as(const char *folder, const char *as_name, const sh_mb_resolver *resolver,
                      char *name_out, size_t name_cap, sh_ms_install_stats *stats,
                      sh_mb_bundle *bundle_out, char *cause, size_t cause_cap);

/* ------------------------------------------------------------ places ---- */
/* A place is mods\maps\<segment>\: the segment sh_ms_title_segment of the
 * map's title (a rawmap's file name, a prefab ZIP's name), followed by
 * " (<map id>)" only when the record already gives that segment to another map.
 * The record's "places" section maps each map to its place:
 *   {"<published item id> | zip:<file name> | title:<segment>":
 *    {"title": "<the map's title>", "folder": "maps\\<segment>"}} */
typedef struct sh_ms_place {
    const char *title;     /* the map's title, a rawmap's file name or a prefab ZIP's name */
    const char *map_id;    /* the published item id, or "zip:<file name>"; NULL or "": "title:<segment>" */
} sh_ms_place;

/* `title` made a folder name (UTF-8): the characters Windows refuses in a
 * name and control characters dropped, runs of spaces folded, leading dots
 * and spaces and trailing dots and spaces cut, a work-folder suffix
 * (".staging-", ".old-") spelled with "_", a device name (con, prn, aux, nul,
 * com1 to com9, lpt1 to lpt9) given a trailing "_", at most 60 bytes cut at a
 * character boundary. 1 with the segment, 0 when nothing is left. */
int  sh_ms_title_segment(const char *title, char *out, size_t cap);

/* The place of `place` under mods\ ("maps\<segment>"): the record's when the
 * map is noted, else a free one. 1 with it, 0 when none can be made. */
int  sh_ms_place_dir(const sh_ms_place *place, char *out, size_t cap);

/* The folder under mods\ for the build of `salt` (mod display name `name`) in
 * `place`: 2 with the place's folder that already holds the salt (one build
 * per salt per place: it is replaced), 1 with a free name
 * "maps\<segment>\<sh_ms_title_segment(name), else mod>" (" (2)", " (3)" ...
 * when another mod of the place has the name), 0 when none is free. */
int  sh_ms_place_folder(const sh_ms_place *place, const char *name, const char *salt, char *out, size_t cap);

/* Record that `place`'s map is served from `folder` (under mods\: the place
 * "maps\<segment>" or a mod's folder in it, either slash; NULL: its
 * sh_ms_place_dir). Returns 1 when written. */
int  sh_ms_place_note(const sh_ms_place *place, const char *folder);

/* The place the record gives the map `key` (an item id, "zip:<name>" or
 * "title:<segment>"): 1 with "maps\<segment>" in `dir` and its title (either
 * may be NULL), 0 when none. */
int  sh_ms_place_of(const char *key, char *dir, size_t dir_cap, char *title, size_t title_cap);

/* The title of the map whose place holds `folder` (its name under mods\,
 * either slash, or absolute): 1 with it, else 0 (a root folder, or a place no
 * map of the record names). */
int  sh_ms_place_title(const char *folder, char *title, size_t cap);

/* A map whose place is `dir` ("maps\<segment>") was loaded now: every entry
 * of the record of the mods folder `root` (the folder the serving index
 * reads; NULL: the store root) naming that place notes the time ("played").
 * Returns 1 when written, 0 when no entry names it or the record cannot be
 * written. */
int  sh_ms_place_played(const char *root, const char *dir);

/* When the map of place `dir` was last played (seconds since 1970): the
 * latest "played" of the entries naming it, and never earlier than the place
 * folder's creation, so a place that arrived and was never played counts
 * from its arrival. 0 when neither is known. */
long long sh_ms_place_played_at(const char *dir);

/* sh_ms_install_from_folder into `place`: the source must be built (a
 * received build); the place's folder of its salt is replaced (the old one to
 * the Recycle Bin), else a free name is taken (sh_ms_place_folder), and the
 * map is noted (sh_ms_place_note). A place folder is never built again by the
 * scan. */
int  sh_ms_install_placed(const char *folder, const sh_ms_place *place,
                          const sh_mb_resolver *resolver, char *name_out, size_t name_cap, sh_ms_install_stats *stats,
                          sh_mb_bundle *bundle_out, char *cause, size_t cause_cap);

/* Install a built folder at the root of mods\ as one of the player's own
 * mods, keeping its salt: a folder of a map's place (Add to my mods, a
 * placed prefab whose mod a map brought) or a received build unpacked
 * outside the store (a download for a map without a place). Every file it
 * holds is copied to mods\<its name> (" (2)" when taken) and verified; the
 * source is only read. A root folder of the salt at this very build is the
 * install (stats->present); at another build the install is refused, since
 * the player's own folder of a mod stays theirs. Returns 1 with the folder
 * name under mods\, else 0 with the plain cause. Any thread; takes the store
 * lock. */
int  sh_ms_install_to_root(const char *folder, const sh_mb_resolver *resolver, char *name_out, size_t name_cap,
                           sh_ms_install_stats *stats, sh_mb_bundle *bundle_out, char *cause, size_t cause_cap);

typedef struct sh_ms_verify_stats {
    unsigned files, bytes_mb, entries;
    unsigned pointers;          /* of files, stock pointers resolved against the installed game */
} sh_ms_verify_stats;

/* Rehash every file of the entries `spec` names (sh_ms_entry_matches)
 * against their builder blocks and record the result there ("cause").
 * Returns 1 when every matching entry passes. */
int  sh_ms_verify(const char *spec, sh_ms_verify_stats *stats, char *cause, size_t cause_cap);

typedef struct sh_ms_sync_stats {
    unsigned folders;           /* folders looked at */
    unsigned salted;            /* new mods given their salt */
    unsigned resalted;          /* copies given a new salt */
    unsigned rebuilt;           /* built again: new, or their tree or the builder rules changed */
    unsigned failed;            /* refused (the cause stays on the entry) */
    unsigned long long ms;
} sh_ms_sync_stats;

/* The scan (steps 1 to 3 above) over the root. Logs one "MODS: store:" line
 * per salted, re-salted, built or refused folder. Returns 1 when the store
 * could be read. */
int  sh_ms_sync(const sh_mb_resolver *resolver, sh_ms_sync_stats *stats);

/* The root folders sh_ms_sync would change now: new mods, copies, and built
 * folders whose block's tree or rules differ (file sizes and times: no file
 * is read) or whose last build failed. -1 when the store cannot be read. */
int  sh_ms_sync_pending(void);

/* The boot scan: with root folders pending (a fresh folder dropped in while
 * the game was closed, an edit), sh_ms_sync runs and then `rescan` (the
 * serving index; its listeners pass the new table to the game). background 1
 * does the check and the scan on a thread of its own and returns 1 when it
 * started; background 0 runs them here and returns the folders that were
 * pending (0: nothing built, no rescan). -1 when the store cannot be read or
 * the thread cannot start. The resolver is copied. */
typedef int (*sh_ms_rescan_fn)(void);
int  sh_ms_boot_sync(const sh_mb_resolver *resolver, sh_ms_rescan_fn rescan, int background);

/* The work folders an earlier session left when it ended mid-install, mid-
 * download or mid-salting ("<name>.staging-<pid>", "<name>.old-<pid>" beside
 * their target), whose process is no longer running. A ".old-" folder whose
 * target is missing is the folder a crash took away between the two renames:
 * it is put back. Every other one is removed. One "MODS: store:" line per
 * action. Run at boot before the serving index is read. Returns the folders
 * put back, or -1 when the store cannot be read. */
int  sh_ms_recover_work(void);

/* The startup install is done with the store: the background boot scan,
 * which waits for this (up to two minutes), may start. While it runs it hands
 * the store to any operation that waits for it, between two folders. Any
 * thread; idempotent. sh_ms_boot_released: 1 once called. */
void sh_ms_boot_release(void);
int  sh_ms_boot_released(void);
#ifdef SH_MODSTORE_TESTING
void sh_ms_test_boot_rearm(void);                            /* the next boot build waits for a release again */
extern void (*sh_ms_test_between_folders)(unsigned built);   /* the boot build, between two folders */
#endif

/* The first cause of the last refused build or salting of `folder` in this
 * process, or "" (sh_ms_sync and installs record it; enumeration reports it). */
void sh_ms_build_cause(const char *folder, char *out, size_t cap);

/* 1 when `folder` (absolute UTF-8) has a mod.json that is not a JSON object,
 * with the parse error and the next step in `out`; 0 otherwise (no mod.json,
 * or one that parses). */
int sh_ms_descriptor_unparsable(const char *folder, char *out, size_t cap);

/* Record a refused build of `folder` (absolute) that a caller ran itself
 * (`sh_mods build`): the same per-folder state sh_ms_sync records. A later
 * successful build (sh_ms_note_build) clears it. */
void sh_ms_note_build_failure(const char *folder, const char *cause);

/* New mod: a folder mods\<sh_ms_title_segment(name), else "mod"> (" (2)" ...
 * when taken) holding a mod.json of name, empty description
 * and author, and the block {"format": 2, "salt": <a new salt>}, and an
 * empty assets\. The caller's scan builds it. Returns 1 with the salt and the
 * absolute folder; 0 with the cause. */
int  sh_ms_new_mod(const char *name, char salt_out[SH_MB_ID_MAX + 1], char *folder_out, size_t folder_cap,
                   char *cause, size_t cause_cap);
/* New mod from the Mods tab: as sh_ms_new_mod, plus an example that places
 * and plays untouched: an entityDef at mods/<salt>/example_crate (the game's
 * SnapMap crate01 model), its Toybox row (snapEditorEntityDef) and the row's
 * strings in mod.json's author part (<salt>_category named after the mod,
 * <salt>_example_crate_name and _desc). Nothing binary.

 * `example_out` receives the row's path below the folder, with '/'. */
int  sh_ms_new_mod_example(const char *name, char salt_out[SH_MB_ID_MAX + 1], char *folder_out, size_t folder_cap,
                           char *example_out, size_t example_cap, char *cause, size_t cause_cap);

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
 * the root itself. An emptied place folder goes with its record entry.
 * SH_MS_RECYCLED, SH_MS_DELETED, or 0. */
int  sh_ms_remove_folder(const char *folder);

/* Uninstall's removal: `folder` (absolute) must be a mod folder of the store
 * that exists; it goes as sh_ms_remove_folder's does. SH_MS_RECYCLED;
 * SH_MS_DELETED with why the bin could not take it in `cause`; else 0 with
 * the first cause. */
int  sh_ms_delete_mod_folder(const char *folder, char *cause, size_t cap);

#if defined(SH_MODSTORE_TESTING) || defined(SH_MODUI_TESTING)
/* Test seam: nonzero makes every removal take the outright-delete fallback,
 * as when the shell refuses. */
void sh_ms_recycle_refuse_for_test(int refuse);
#endif

#endif /* SNAPMAP_PLUS_MODSTORE_H */
