/* modexport.h -- an export that carries its mod content: one ZIP file, a map's or a prefab's, with the same layout
 * for both.
 *
 *   <name>.json          the map text, or the prefab file (its "snapmapPlus" block, prefab_block.h), whose rows name
 *                        units (pinned rows: "objects" and "whole")
 *   units/<hash>.json    each unit those rows name: its canonical manifest, whose SHA-256 is <hash> (modunit.h)
 *   files/<sha256>       each file those manifests carry, once, its SHA-256 its name
 *
 * Export pins every local row to the units of the objects it places (modpin.h); a missing unit or file refuses it,
 * so it is complete offline. Import puts each unit and file into the content cache, checked against its name, and
 * installs nothing; an export of an earlier shape is refused (MODEXPORT_EARLIER). Entries are stored (method 0, the
 * reader also takes deflate, 8); ZIP64 where needed (modzip.h). File work only; paths are UTF-8, opened wide.
 */
#ifndef SNAPMAP_PLUS_MODEXPORT_H
#define SNAPMAP_PLUS_MODEXPORT_H

#include <stddef.h>

#include "modmanifest.h"
#include "modbundle.h"

/* A map's entry is named after the ZIP ("my map.zip" carries "my map.json"); this when the name cannot. */
#define MODEXPORT_MAP_FALLBACK "map.json"
/* An export.json without ~type is an index of an earlier shape. */
#define MODEXPORT_INDEX        "export.json"
/* Every refusal below is a sentence in *cause (malloc'd; any previous one is freed; the caller frees it); NULL cause for none. */
/* The refusal of an export in an earlier shape. */
#define MODEXPORT_EARLIER "This export was saved by an earlier Snapmap+; run the Snapmap+ installer to update it."

typedef struct modexport_stats {
    unsigned bundles;              /* the rows (mods) the map or prefab names */
    unsigned bundles_installed;    /* export: units carried; import: units newly put in the content cache */
    unsigned bundles_present;      /* import: units the cache held already */
    unsigned bundles_absent;       /* import: rows whose content is not all here (their objects stand in) */
    unsigned entries;              /* ZIP entries written */
    unsigned files;                /* export: files carried; import: files newly put in the cache */
    unsigned long long map_bytes;
    unsigned long long zip_bytes;
    double ms;
} modexport_stats;

/* 1 when the path ends in ".zip" (any case). */
int   modexport_is_zip_path(const char *path);

/* Write the export of `map_json` to zip_path (replacing it atomically), its rows pinned (above). Returns 1 on
 * success, else 0 with the cause. */
int   modexport_write(const char *map_json, size_t len, const char *zip_path, modexport_stats *st,
                      char **cause);

/* Check a map's export without installing anything: a readable ZIP of the current shape holding one top-level .json
 * that is a map. */
int   modexport_probe(const char *zip_path, char **cause);

/* Import a map's export: its units and files into the content cache, then the map text (malloc'd,
 * NUL-terminated), or NULL with the cause. */
char *modexport_read(const char *zip_path, size_t *out_len, modexport_stats *st, char **cause);

/* A prefab's export (every prefab, with or without mods) uses the layout above; an extracted export folder imports
 * as the ZIP does, given its .json. The carried .json tells the kinds apart: an idSnapEntityPrefab, or an idSnapMap
 * with a block, is a prefab; an idSnapMap or untyped object without a block is a map. Each refuses the other's. */

/* The prefab text to carry, given its rows as the export pinned them (`moved` is 1 when any row changed).
 * malloc'd, *len set; NULL refuses the export. */
typedef char *(*modexport_prefab_fn)(void *ctx, const sh_modmanifest_list *rows, int moved, size_t *len);

/* Write a prefab's export, `rows` pinned as a map's are. `name` (UTF-8) names the entry <name>.json. text_for may
 * be NULL (the prefab text is carried as given). zip_path is UTF-8. */
int modexport_write_prefab(const char *name, const char *prefab, size_t len, const sh_modmanifest_list *rows,
                           modexport_prefab_fn text_for, void *ctx, const char *zip_path, modexport_stats *st,
                           char **cause);

/* The rows a prefab text names (its block's), for the import's count of content it lacks. Returns the count, -1 to
 * refuse, -2 to refuse an earlier shape (MODEXPORT_EARLIER). */
typedef int (*modexport_rows_fn)(void *ctx, const char *prefab, size_t len, sh_modmanifest_list *rows);

/* Import a prefab's export (a ZIP, or a .json in an extracted folder): units and files go into the content cache;
 * returns the prefab text (malloc'd) and its name (the .json's name without extension). 1 on success, else 0 with the
 * cause. */
int modexport_read_prefab(const char *path, modexport_rows_fn rows_of, void *ctx, char **prefab, size_t *prefab_len,
                          char **name, modexport_stats *st, char **cause);

/* No effect: an import builds nothing; kept for callers that register it. */
void modexport_set_stock(sh_mb_exists_fn stock, void *ctx);

/* The rawmap layer (rawmap.c) calls these for a UTF-8 path ending in .zip; each logs one MODS: line. */

/* The writer: returns the ZIP's byte count, 0 on failure. */
unsigned long long modexport_write_text(const char *data, size_t len, const char *path);
/* The same writer for a caller that reports the outcome itself: *bundles (optional) receives the mods the map's
 * rows name; on failure `cause` says why. */
unsigned long long modexport_write_text_report(const char *data, size_t len, const char *path,
                                               unsigned *bundles, char **cause);
/* The reader for a caller that tells the player: the map text as modexport_read returns it, in a NUL-terminated
 * process-heap buffer (HeapFree), or NULL with the importer's words in *cause. */
char *modexport_read_text_report(const char *path, size_t *out_len, char **cause);
/* The source check: 1 when the export can be loaded, else 0 with the reason in *out_msg (malloc'd; "ok" on success). */
int   modexport_validate_source(const char *path, char **out_msg);

#endif /* SNAPMAP_PLUS_MODEXPORT_H */
