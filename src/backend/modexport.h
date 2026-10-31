/* modexport.h -- an export that carries its mods: one ZIP file, a map's or a
 * prefab's, with the same layout for both.
 *
 *   <name>.json                the map text (manifest rows included) or the
 *                              prefab file (its "snapmapPlus" block, prefab_block.h)
 *   mods/<folder>/mod.json     each mod the rows name, exactly as it is installed:
 *   mods/<folder>/assets/...   the store folder's own name (<id>, or <id>@<version>
 *                              when the store holds two folders for one id) and
 *                              its mod.json and carried files byte for byte
 *
 * There is no index, no bundle.json and no canonical manifest: the .json's
 * rows name the bundles, and each carried mod.json records its own bundle
 * hash in its product block. A stock pointer is an entry of mod.json; the
 * export never carries installed game content, so a pointer whose bytes the
 * author's folder holds under assets/ is left out and the importer reads
 * those bytes from its own game. Entries are stored (method 0); the reader
 * also accepts deflate (8), so a file repacked by another tool still imports.
 * No ZIP64: an export is limited to 65535 entries and 4 GiB.
 *
 * Import is the ordinary install of each carried folder whose bundle a row
 * names: a folder the store already serves (the bundle hash, or an alias of
 * it) is skipped and counted as already installed; otherwise the folder is
 * copied out of the export to a temporary folder and installed from there
 * (sh_ms_install_as, building and verifying it as any folder from elsewhere),
 * under its own name when no folder of its id is installed and that name is
 * free, else beside the installed id under the store's own name
 * (<id>@<version>). An installed folder of the same id and version with other
 * content refuses the import, because nothing installed is ever rewritten.
 * The built bundle hash must equal the row's; on any refusal the folders this
 * import installed are removed again. The serving index is rebuilt after an
 * install. A ZIP in the retired bundle form of earlier releases (any
 * bundle.canonical.json, or an export.json index) is refused.
 *
 * File work only, no network. Paths are UTF-8 unless marked ACP.
 */
#ifndef SNAPMAP_PLUS_MODEXPORT_H
#define SNAPMAP_PLUS_MODEXPORT_H

#include <stddef.h>

#include "modmanifest.h"
#include "modbundle.h"

/* A map's entry is named after the ZIP ("my map.zip" carries "my map.json"),
 * this when the ZIP's name cannot name one. */
#define MODEXPORT_MAP_FALLBACK "map.json"
/* The retired index name: an export.json without ~type is that index. */
#define MODEXPORT_INDEX        "export.json"
#define MODEXPORT_MAP_MAX      (64u * 1024u * 1024u)
/* The refusals of the retired bundle form. */
#define MODEXPORT_RETIRED_PREFAB "this export was made by an earlier release; export it again"
#define MODEXPORT_RETIRED_MAP    "this export was made by an earlier release; save the rawmap again"

typedef struct modexport_stats {
    unsigned bundles;              /* export: mods the rows name; import: carried folders the rows name */
    unsigned bundles_installed;    /* import: newly installed */
    unsigned bundles_present;      /* import: already installed */
    unsigned bundles_absent;       /* import: rows naming a bundle the export does not carry and
                                      this machine does not have (the map cannot load as it is) */
    unsigned entries;              /* ZIP entries written or read */
    unsigned files;                /* export: carried files written; import: files copied out */
    unsigned long long map_bytes;
    unsigned long long zip_bytes;
    double ms;
} modexport_stats;

/* 1 when the path ends in ".zip" (any case). */
int   modexport_is_zip_path(const char *path);

/* Write the export of `map_json` to zip_path (replacing it atomically).
 * Every bundle the map's smpmod1 rows name must be installed and usable.
 * Returns 1 on success, else 0 with the cause. */
int   modexport_write(const char *map_json, size_t len, const char *zip_path, modexport_stats *st,
                      char *cause, size_t cause_cap);

/* Check a map's export without installing anything: a readable ZIP, not
 * in the retired form, holding one top-level .json that is a map. */
int   modexport_probe(const char *zip_path, char *cause, size_t cause_cap);

/* Import a map's export: install the carried folders the map's rows name, as
 * above, then return the map text (malloc'd, NUL-terminated), or NULL with
 * the cause (nothing stays installed). */
char *modexport_read(const char *zip_path, size_t *out_len, modexport_stats *st, char *cause, size_t cause_cap);

/* ---------------------------------------------- prefab exports ------ */
/* A prefab's export is written for every prefab, with or without mods, in
 * the layout above: <name>.json is the prefab file, whose block's "mods" rows
 * name the bundles the ZIP carries. An export that has been extracted (a
 * folder holding <name>.json and mods/) imports as the ZIP does: the importer
 * is given the .json, and its folder is the export.
 *
 * The importers tell the kinds apart by the carried .json: an
 * idSnapEntityPrefab, or an idSnapMap with a block, is a prefab; an idSnapMap
 * (or an object without ~type) without a block is a map. Each refuses the
 * other's export. */
#define MODEXPORT_PREFAB_MAX (16u * 1024u * 1024u)

/* The prefab text to carry, given the rows the export carries (`moved` is 1
 * when a row pinned an earlier build and now names the current one).
 * malloc'd, *len set; NULL refuses the export. */
typedef char *(*modexport_prefab_fn)(void *ctx, const sh_modmanifest_list *rows, int moved, size_t *len);

/* Write a prefab's export: every bundle `rows` names must be installed and
 * usable. `name` (UTF-8) names the entry <name>.json. text_for may be NULL
 * (the prefab text is carried as given). zip_path is UTF-8. */
int modexport_write_prefab(const char *name, const char *prefab, size_t len, const sh_modmanifest_list *rows,
                           modexport_prefab_fn text_for, void *ctx, const char *zip_path, modexport_stats *st,
                           char *cause, size_t cause_cap);

/* The rows a prefab text names (its block's); the importer installs the
 * carried bundles among them. Returns the count, or -1 to refuse. */
typedef int (*modexport_rows_fn)(void *ctx, const char *prefab, size_t len, sh_modmanifest_list *rows);

/* Import a prefab's export: path (UTF-8) is the ZIP, or a .json whose folder
 * is an extracted export. Install the carried folders whose bundle rows_of
 * names (every carried folder when rows_of is NULL), as above, then return
 * the prefab text (malloc'd) and its name (the .json's file name without the
 * extension, UTF-8). 1 on success, else 0 with the cause; a refused import
 * leaves nothing installed. st->bundles_absent counts rows naming a bundle the
 * export does not carry and the store lacks. */
int modexport_read_prefab(const char *path, modexport_rows_fn rows_of, void *ctx, char **prefab, size_t *prefab_len,
                          char *name, size_t name_cap, modexport_stats *st, char *cause, size_t cause_cap);

/* The stock existence check the import's builds use for needs (the game's
 * installed archives; thread-safe). NULL leaves a need on a stock name
 * uncheckable, as in the tests. The backend registers one at start. */
void modexport_set_stock(sh_mb_exists_fn stock, void *ctx);

/* ---------------------------------------------- rawmap file hooks ---- */
/* The rawmap layer (rawmap.c) calls these for a path ending in .zip. Paths
 * are in the ANSI code page, as the rawmap layer's own are. Each logs one
 * MODS: line with byte counts and the time taken. */

/* The writer: returns the ZIP's byte count, 0 on failure. */
unsigned long long modexport_write_text(const char *data, size_t len, const char *path_acp);
/* The same writer for a caller that reports the outcome itself: *bundles
 * (optional) receives the bundles the map's rows name, and on failure `cause`
 * says why (a mod the map names is not installed, the folder is not
 * writable). */
unsigned long long modexport_write_text_report(const char *data, size_t len, const char *path_acp,
                                               unsigned *bundles, char *cause, size_t cause_cap);
/* The reader: a NUL-terminated process-heap buffer (HeapFree), or NULL. */
char *modexport_read_text(const char *path_acp, size_t *out_len);
/* The source check: 1 when the export can be loaded, else 0 with a reason. */
int   modexport_validate_source(const char *path_acp, char *out_msg, int msg_capacity);

#endif /* SNAPMAP_PLUS_MODEXPORT_H */
