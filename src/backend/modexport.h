/* modexport.h -- a rawmap export that carries its mods: one ZIP file.
 *
 *   map.rawmap.json                         the map text, manifest rows included
 *   export.json                             {"format":1,"map":"map.rawmap.json",
 *                                            "bundles":[{"bundle","id","version","build","folder"}]}
 *   mods/<id>@<version>+<build>/bundle.json            each bundle the rows name, in the
 *   mods/<id>@<version>+<build>/bundle.canonical.json  Contract 1 bundle.json form (derived
 *   mods/<id>@<version>+<build>/assets/...             from mod.json; the ZIP format is unchanged)
 *   mods/legacy/<id>@<version>+<build>/...  a converted first-generation bundle
 *
 * Each distinct blob is stored once in the ZIP: a file whose bytes an
 * earlier entry already holds (in the same bundle or another) is left out,
 * and the importer takes it from that entry by the hash bundle.json names.
 * Entries are stored (method 0); the reader also accepts deflate (8), so a
 * file repacked by another tool still imports. No ZIP64: an export is
 * limited to 65535 entries and 4 GiB.
 *
 * Import verifies every manifest against its hash and every blob against
 * the manifest, installs the bundles as mod folders exactly as a download
 * does (modsync_install_manifest: mods\<id>\ with mod.json and assets\),
 * rebuilds the serving index, and returns the map text.
 *
 * File work only, no network. Paths are UTF-8 unless marked ACP.
 */
#ifndef SNAPMAP_PLUS_MODEXPORT_H
#define SNAPMAP_PLUS_MODEXPORT_H

#include <stddef.h>

#include "modmanifest.h"

#define MODEXPORT_FORMAT    1
#define MODEXPORT_MAP_ENTRY "map.rawmap.json"
#define MODEXPORT_INDEX     "export.json"
#define MODEXPORT_MAP_MAX   (64u * 1024u * 1024u)

typedef struct modexport_stats {
    unsigned bundles;              /* bundles the map's rows name */
    unsigned bundles_installed;    /* import: newly installed */
    unsigned bundles_present;      /* import: already installed */
    unsigned bundles_absent;       /* import: rows naming a bundle the export does not carry and
                                      this machine does not have (the map cannot load as it is) */
    unsigned entries;              /* ZIP entries written or read */
    unsigned blobs_written;        /* distinct blobs stored in the ZIP / installed from it */
    unsigned blobs_deduped;        /* files left out because an earlier entry holds their bytes */
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

/* Check an export's structure without installing anything: readable ZIP,
 * export.json of this format, a map entry that is a JSON object. */
int   modexport_probe(const char *zip_path, char *cause, size_t cause_cap);

/* Import: install every bundle the export carries (verifying hashes), then
 * return the map text (malloc'd, NUL-terminated), or NULL with the cause. */
char *modexport_read(const char *zip_path, size_t *out_len, modexport_stats *st, char *cause, size_t cause_cap);

/* ---------------------------------------------- prefab exports ------ */
/* A prefab's export is one ZIP, written for every prefab, with or without mods:
 *
 *   <name>.json                       the prefab file: the engine's JSON with its
 *                                     "snapmapPlus" block (prefab_block.h), whose
 *                                     "mods" rows name the builds the ZIP carries
 *   mods/<id>@<version>+<build>/...   each bundle those rows name, as for a map
 *   mods/legacy/<id>@<version>+<build>/...
 *
 * There is no export.json: the prefab's own block is the index. The importer
 * takes the one top-level .json (an idSnapEntityPrefab, or an idSnapMap that
 * carries a block) and installs the carried bundles its rows name, each found
 * by its bundle.canonical.json, whose SHA-256 is the bundle hash. An export
 * that has been extracted (a folder holding <name>.json and mods/) imports as
 * the ZIP does: the importer is given the .json, and its folder is the export.
 *
 * A map's importer refuses a prefab's export and the other way round; an
 * export.json index without "map" (an earlier prefab layout) is refused. */
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
 * is an extracted export. Verify it, install the bundles rows_of names that
 * the export carries and the store does not hold (every carried bundle when
 * rows_of is NULL; nothing already installed is written again), then return
 * the prefab text (malloc'd) and its name (the .json's file name without the
 * extension, UTF-8). 1 on success, else 0 with the cause; a damaged export
 * installs nothing. st->bundles_absent counts rows naming a bundle the export
 * does not carry and the store lacks. */
int modexport_read_prefab(const char *path, modexport_rows_fn rows_of, void *ctx, char **prefab, size_t *prefab_len,
                          char *name, size_t name_cap, modexport_stats *st, char *cause, size_t cause_cap);

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
