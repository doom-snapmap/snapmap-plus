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
/* A prefab's export has the same layout, with the prefab in place of the map:
 *
 *   prefab.json         the prefab file as the library holds it
 *   prefab.meta.json    its sidecar, whose "mods" rows name the builds carried
 *   export.json         {"bundles":[...],"format":1,"kind":"prefab","name":"<name>","prefab":"prefab.json"}
 *   mods/<id>@<version>+<build>/...   each bundle the rows name, as for a map
 *
 * A map's importer refuses a prefab's export and the other way round. */
#define MODEXPORT_PREFAB_ENTRY "prefab.json"
#define MODEXPORT_PREFAB_META  "prefab.meta.json"
#define MODEXPORT_PREFAB_MAX   (16u * 1024u * 1024u)

/* The sidecar to carry, written for the rows the export carries (a row that
 * pinned an earlier build names the current one). malloc'd, *len set; NULL
 * refuses the export. */
typedef char *(*modexport_meta_fn)(void *ctx, const sh_modmanifest_list *rows, size_t *len);

/* Write a prefab's export: every bundle `rows` names must be installed and
 * usable. `meta` may be NULL (no sidecar entry). zip_path is UTF-8. */
int modexport_write_prefab(const char *name, const char *prefab, size_t len, const sh_modmanifest_list *rows,
                           modexport_meta_fn meta, void *meta_ctx, const char *zip_path, modexport_stats *st,
                           char *cause, size_t cause_cap);

/* Import a prefab's export: verify it, install the bundles the store does not
 * hold (nothing already installed is written again), then return the prefab
 * text and its sidecar (malloc'd; *meta NULL when the export has none) and the
 * name export.json gives (may be empty). 1 on success, else 0 with the cause;
 * a damaged export installs nothing. */
int modexport_read_prefab(const char *zip_path, char **prefab, size_t *prefab_len, char **meta, size_t *meta_len,
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
