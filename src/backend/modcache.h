/* Verify, compose and retain the map content shared by this profile. */
#ifndef SNAPMAP_PLUS_MODCACHE_H
#define SNAPMAP_PLUS_MODCACHE_H

#include <stddef.h>
#include "modnet.h"
#include "modbundle.h"

struct sh_modmanifest_row;
struct sh_modmanifest_list;

#define SH_CACHE_HASH_HEX   64

/* Why an object of a map is absent, the most actionable cause first, and a disk that filled. */
enum {
    SH_CACHE_ABSENT_OFFLINE = 1,     /* S2: not on this computer, and the store was not asked (offline, the editor) */
    SH_CACHE_ABSENT_UNREACHABLE,     /* S4: the store could not be reached */
    SH_CACHE_ABSENT_NEWER,           /* S6: a unit format, or a product file, this Snapmap+ does not have */
    SH_CACHE_ABSENT_GAME,            /* S4b: a DOOM file of another game revision */
    SH_CACHE_ABSENT_NOT_IN_STORE,    /* S5: never uploaded, or still queued */
    SH_CACHE_ABSENT_REMOVED,         /* S3: moderation removed it */
    SH_CACHE_ABSENT_DISK,            /* N15: not enough free disk space */
    SH_CACHE_ABSENT_LOCAL,           /* D6a: a local row whose mod folder is not on this computer (manager Q8) */
    SH_CACHE_ABSENT_BROKEN,          /* D6a: a local row whose mod folder is here but cannot be used (its build failed) */
    SH_CACHE_ABSENT_LACKED,          /* D6a: a name a local row lists that its mod folder, here, no longer has */
    SH_CACHE_ABSENT_REFUSED          /* a required decl failed to parse */
};

/* Read the pinned rows still used by library prefabs. The importer and collector agree on a complete library view. */
typedef int (*sh_cache_library_fn)(struct sh_modmanifest_list *rows);
void sh_cache_set_library(sh_cache_library_fn fn);
void sh_cache_library_import_begin(void);
void sh_cache_library_import_end(void);

/* The cache's folder: <data>\cache\content, resolved from the selected mods root. malloc'd. */
char *sh_cache_root(void);
/* Tests and tools resolve the cache and local mods from this root. NULL restores the product's root. */
void  sh_cache_set_mods_root(const char *mods_root);

/* Bound at install (dllmain.c): the mods root (modstore.h sh_ms_root), the builder's resolver a composition builds
 * with (the store's own), and the index rescan run after a load composed something. */
void  sh_cache_set_store_root(const char *(*fn)(void));
void  sh_cache_set_resolver(const sh_mb_resolver *resolver);
void  sh_cache_set_rescan(int (*rescan)(void));
/* Scope the published metadata read on this thread; its launch worker prepares a new composition. */
void sh_cache_published_probe(int on);

/* The map manifest's content step (modmanifest.h sh_modmanifest_set_content): notes the rows, composes every
 * pinned row whose composition is not on disk (or stood in for units that are here now), and rescans when it
 * made one. */
void  sh_cache_on_load(const struct sh_modmanifest_list *rows, char *const *placed, size_t count);
/* 1 while a composition stood in for objects whose notice has not been taken. */
int   sh_cache_notice_pending(void);
/* Forget the pending notice (a gate took the load: its own fetch decides what is absent). */
void  sh_cache_notice_drop(void);

/* The composition hash of `units` (unit hashes, any order, duplicates ignored) under `salt`. 1 on success. */
int   sh_cache_composition_hash(const char *salt, const char *const *units, size_t count, char out[SH_CACHE_HASH_HEX + 1]);

/* The blob's path when the cache holds it (malloc'd), else NULL. */
char *sh_cache_blob_path(const char *sha);
/* Move the file at `from` into files\ when its SHA-256 and size are `sha` and `size` (deleted otherwise). 1 held. */
int   sh_cache_put_blob(const char *from, const char *sha, unsigned long long size);
/* Verify and keep a file read from a map or prefab export. */
int   sh_cache_import_blob(const char *sha, const void *bytes, size_t len, int *damaged);
/* Where a file with this SHA-256 and size is on this computer: its blob, else a local mod's file with those bytes (D21);
 * malloc'd, NULL when neither. An export reads a unit's files from it (SPEC 5.16). */
char *sh_cache_file_source(const char *sha, unsigned long long size);
/* Write a manifest whose SHA-256 is `hash`. 1 written or already there; 0 when the bytes are not the unit's. */
int   sh_cache_put_manifest(const char *hash, const void *bytes, size_t len);
/* A cheap presence check before scheduling a verified read. */
int   sh_cache_manifest_exists(const char *hash);
/* The manifest's bytes (malloc'd, checked against the hash), or NULL. */
char *sh_cache_manifest(const char *hash, size_t *len);

/* A local mod's file with this SHA-256 and size (its path, malloc'd), or NULL: how an installed mod holding the
 * exact unit serves it. The default looks through every built mod folder of the mods root; tests may bind their
 * own. */
typedef char *(*sh_cache_local_fn)(void *context, const char *sha, unsigned long long size);
void  sh_cache_set_local(sh_cache_local_fn fn, void *context);
/* Forget the local mods' files (after a mods scan: the next question reads them again). */
void  sh_cache_local_changed(void);

/* A unit's state: 1 present, 0 its manifest is here but `missing` files (bytes in *missing_bytes) are not, -1 no
 * manifest, -2 a unit format this product does not know. */
int   sh_cache_unit_state(const char *hash, size_t *missing, unsigned long long *missing_bytes,
                          unsigned long long *bytes);

/* The rows of a map being loaded: each pinned row is remembered by its composition hash with its units and the
 * names the map places under its namespace (`placed`, the map's placed names), so a later step (the gate's fetch,
 * the editor's stand-ins) composes it. Any thread. */
void  sh_cache_note_rows(const struct sh_modmanifest_list *rows, char *const *placed, size_t placed_count);
/* 1 when `chash` is a pinned row noted from a map. */
int   sh_cache_row_known(const char *chash);
/* 1 when a unit's pointer from `source` (its "game" entry) is checked against this install's bytes before the unit
 * is used; 0 for the product's own container, whose files this product derives again where they serve. */
int   sh_cache_pointer_checked(const char *source);
/* A unit file's transfer that failed in the network (a reset connection, a timeout, the store busy: 429, 5xx)
 * is tried again, resumed from what arrived, after a pause that doubles: the milliseconds to wait before try
 * ATTEMPT + 1 (ATTEMPT counts from 0), or -1 when CODE is not such a failure or the tries are spent. */
int   sh_cache_net_retry_ms(int code, int attempt);
/* 1 when the composition `chash` is on disk and complete (every unit present when it was made). */
int   sh_cache_composed(const char *chash, int *complete);
/* 1 when the composition `chash` is on disk, with *mask the causes its absent units recorded (bit 1 << cause,
 * SH_CACHE_ABSENT_*; bit 0 when one stood in with no cause known; 0 when it is complete). 0 when it is not on disk. */
int   sh_cache_absent_causes(const char *chash, unsigned *mask);

/* Compose the noted row `chash` from what is here, with stand-ins for what is not (no download). `cause` is the
 * absence cause given to objects whose units are not here (SH_CACHE_ABSENT_OFFLINE for the editor's open). 1 when a
 * composition is on disk; 0 with *why (malloc'd). Main thread or worker. */
int   sh_cache_compose_now(const char *chash, int cause, char **why);

/* Build and index the noted rows from a carried prefab off the game thread. Only complete units can be placed. */
int   sh_cache_prepare_prefab(const struct sh_modmanifest_list *rows, char **why);

/* The gate's fetch (a worker thread): for the noted row `chash`, ask the store about the units that are not here,
 * download the ones it holds (each blob checked, a broken-off file resumed), then compose (with stand-ins for what
 * stayed absent). Returns 0 when a composition is on disk (the map loads), else a MODNET_E_ code. */
int   sh_cache_prepare(const char *chash, const modnet_io *io);

/* The notice of the objects the last compositions stood in for, once (malloc'd), or NULL: the first fitting
 * line naming every absent object. */
char *sh_cache_take_notice(void);

/* The absence cause a failed download of a unit's file gives (SH_CACHE_ABSENT_*): a 410 (MODNET_E_RETIRED) is a
 * newer Snapmap+, a 451 the removal, a 404 the absence at the store, anything else the store not reached. */
int sh_cache_net_cause(int modnet_code);

/* An absent object, for the sentence. */
typedef struct sh_cache_absent { const char *object, *mod; int cause; } sh_cache_absent;
/* The one sentence for `n` absent objects (malloc'd): the most actionable cause's naming all of them, "the
 * Tyrant" or "3 objects from 2 mods (the Tyrant, the Marauder and the Hell Guard)". Never over 250 bytes: names
 * too long for that give way to the count ("5 objects from 2 mods"), the cause's tail kept. */
char *sh_cache_notice_text(const sh_cache_absent *absent, size_t n);
/* The objects a sentence names (malloc'd): "the Tyrant from Eternal Demons", or "3 objects from 2 mods (the Tyrant, the
 * Marauder and the Hell Guard)", at most three names then "and N more". */
char *sh_cache_objects_subject(const sh_cache_absent *absent, size_t n);
/* An object's name for a sentence from its root (entities/ai/demon/tyrant -> "Tyrant"), malloc'd. A whole-mod row's root
 * ("*") gives "": the sentence names the mod itself ("without Eternal Demons"). */
char *sh_cache_object_name(const char *root);

/* Stand-ins, malloc'd decl texts. The entityDef: snapmaps/unknown with `cls`, the class the map saved for the
 * entity (bare snapmaps/unknown for idEntity or no class). The palette row at
 * decls/snapeditorentitydef/snapmapplus_standin/<path>.decl: entityDef `def_source`, not placeable from the
 * Toybox, movable and deletable, drawn as labelled bounds, inheriting the stock row DOOM gives `cls` with the
 * same reference slot counts (standin_rows.h), or that class's row with the counts written over it. */
char *sh_cache_standin_def_text(const char *cls);
/* Module stand-ins. The record a pinned row carries for a placed mod module, from its snapModule decl text:
 * {"name": `name`, "bounds": [x0,y0,z0,x1,y1,z1], "portals": [{"bounds": [...6], "dir": "PORTAL_DIR_..."},
 * ...]} (malloc'd; NULL when the text is not a module). Whoever pins a row (a publish, an export) records it; a
 * save keeps it. */
char *sh_cache_module_record(const char *name, const char *decl, size_t len);
/* The pinning step's half: every module of the row's namespace the map text places (instances[].moduleName
 * under maps/modules/mods/<salt>/), each once, read by `read` (its decl text, malloc'd, or NULL) and recorded
 * into row->modules. Returns the modules recorded, -1 on bad arguments or memory. */
typedef char *(*sh_cache_read_fn)(void *context, const char *request, size_t *len);
int   sh_cache_row_record_modules(struct sh_modmanifest_row *row, const char *map_json, size_t len, sh_cache_read_fn read,
                                  void *context);
/* The stand-in module decl for one record: the product's grid room of the record's size (*room, malloc'd, its
 * moduleName) with the record's bounds and portals written over it (malloc'd; NULL when the record does not read). */
char *sh_cache_module_standin_text(const char *record_json, size_t len, char **room);
/* 1 while the module `logical` ("mods/<salt>/rooms/hall") stands in, with the grid room size in *size: the last
 * composition of a row recording it had no present unit carrying it. The Grid Room route then builds it under
 * its own name (grid_room_native.h sh_grid_native_set_standins), so the map's instance, doors and entities load
 * whole. Any thread. */
struct sh_grid_size;
int   sh_cache_module_standin(const char *logical, struct sh_grid_size *size);
char *sh_cache_standin_row_text(const char *def_source, const char *cls, int entrefs, int varrefs);

typedef struct sh_cache_sweep_stats {
    size_t maps, units_kept, units_removed, blobs_removed, served_removed;
    size_t partials_removed;     /* partial downloads of content no kept map names, untouched for an hour */
    size_t unreadable;           /* kept maps or map folders that did not read: the sweep then removed nothing */
    unsigned long long bytes_kept, bytes_removed;
} sh_cache_sweep_stats;

/* The game's folders of map texts: saved-maps (the cloud copies of maps saved online) and published-maps (NULL:
 * the game's own, under %LOCALAPPDATA%\id Software\DOOM\base\doomsnapmaps). Tests set their own. */
void  sh_cache_set_map_folders(const char *saved, const char *published);
/* References: every unit a kept map names (local saves, cloud copies, published-map texts unless
 * `drop_played`), the open map's rows, library prefabs, and `held` (uploads in progress). Removes manifests no reference names,
 * then blobs no remaining manifest names, then compositions no kept map or library prefab uses, then partial downloads no
 * reference names that nothing wrote for an hour. A kept map whose text does not read, or a folder that cannot
 * be listed, may use anything, so the sweep then removes nothing. Never touches a local mod's files, never runs
 * on a timer. 1 done. */
int   sh_cache_sweep(int drop_played, const char *const *held, size_t held_count, sh_cache_sweep_stats *out);

/* The sweep at start (dllmain.c), on a thread of its own. */
void  sh_cache_sweep_async(void);

/* Map content on disk: one call per kept map with what its rows hold, then the storage line ("Map content: 1.3
 * GB for 14 maps."). Lines are plain text, never a hash, salt or path. `row` is the line's row, its composition
 * hash, which Add to my mods takes (sh_cache_stage_copy); NULL for the storage line. Returns the lines. */
typedef void (*sh_cache_line_fn)(void *context, const char *line, const char *row);
int   sh_cache_report(sh_cache_line_fn fn, void *context);

/* Add to my mods from a row of a map on this computer: the pinned row whose composition hash is `row`, found in
 * the kept and played maps' texts, made a mod folder from its units' files and their mod shares merged, named
 * "<the row's name> (my copy)", at a staging folder of the content cache (copies, never links). The caller
 * salts it into the store (modstore.h sh_ms_add_copy) and frees the stage (sh_cache_copy_free). A unit whose
 * binaries spell its namespace copies as any other, serving mapping it to the copy's salt. 1 staged; 0 with the
 * player's sentence in *why (malloc'd); any thread. */
typedef struct sh_cache_copy {
    char *dir;                  /* the staged mod folder (UTF-8, absolute) */
    char *modjson;              /* its author text: the name and the merged mod shares, no builder block */
    size_t modjson_len;
    char salt[SH_MB_ID_MAX + 1];  /* the namespace its files are under */
    char *name;                 /* "<the row's name> (my copy)" */
    size_t objects;             /* the placed objects it carries (the row's units but the whole-mod one) */
} sh_cache_copy;
int   sh_cache_stage_copy(const char *row, sh_cache_copy *out, char **why);
void  sh_cache_copy_free(sh_cache_copy *copy);
/* The storage line alone (malloc'd). */
char *sh_cache_storage_line(void);

/* The units this computer's published maps claim (modclaim.h modclaim_held: the count, *out malloc'd), which
 * the sweep at start keeps as `held`. NULL: none. */
typedef size_t (*sh_cache_held_fn)(char (**out)[SH_CACHE_HASH_HEX + 1]);
void  sh_cache_set_held(sh_cache_held_fn fn);
/* sh_cache_sweep with what this computer's published maps claim kept (sh_cache_set_held). */
int   sh_cache_sweep_kept(int drop_played, sh_cache_sweep_stats *out);

typedef struct sh_cache_upload_stats {
    int held;                    /* the store holds the unit now */
    unsigned requests, archives, parts;
    unsigned long long bytes_sent;
} sh_cache_upload_stats;

/* Send the unit `hash` the store lacks (its manifest here, its files in the cache or a local mod): begin, then
 * the missing files in archives of at most one request each (a larger file in parts), then complete. Units are
 * verified by hash. MODNET_OK when the store holds it; else a MODNET_E_ code with *why. */
int   sh_cache_publish_unit(const char *hash, const modnet_signer *signer, const modnet_io *io, sh_cache_upload_stats *stats,
                            char **why);

#endif /* SNAPMAP_PLUS_MODCACHE_H */
