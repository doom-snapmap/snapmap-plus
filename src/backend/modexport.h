/* modexport.h -- an export that carries its mod content: one ZIP file, a map's or a prefab's, with the same layout
 * for both (SPEC-distribution 5.16).
 *
 *   <name>.json          the map text, or the prefab file (its "snapmapPlus" block, prefab_block.h), whose rows name
 *                        units (pinned rows: "objects" and "whole")
 *   units/<hash>.json    each unit those rows name: its canonical manifest, whose SHA-256 is <hash> (modunit.h)
 *   files/<sha256>       each file those manifests carry, once, its SHA-256 its name
 *
 * Export: every local row is pinned to the units of the objects it places (modpin.h; a map-level row to its whole
 * build), exactly as a publish pins it; a row no folder here serves refuses the export with the mod's name. The text
 * is written with the pinned rows, then each unit's manifest (from the content cache, where the pin wrote it) and
 * each file (from the cache, else a local mod's file with that SHA-256, D21). A unit or a file this computer lacks
 * (a downloaded map whose content never fully came) refuses the export: the export is complete offline. No store is
 * called and nothing of the game's own files is carried (pointers name them).
 *
 * Import: the units and files go into the content cache, each checked against its name (a damaged export changes
 * nothing it has not checked); the map is then one this computer keeps, and its load composes its rows as a
 * downloaded map's are (5.7, 5.8: content the export does not carry stands in). D35: nothing is installed, and a
 * prefab carries units, not mods. An export of an earlier shape (whole mod folders under mods/, any
 * bundle.canonical.json, an export.json index, a map or a prefab in an earlier shape) is refused with
 * MODEXPORT_EARLIER: converting earlier shapes is the installer's work.
 *
 * Entries are stored (method 0); the reader also accepts deflate (8). The writer uses ZIP64 records where a value
 * needs them (modzip.h). File work only, no network. Every path is UTF-8, opened through the wide Windows calls.
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
/* Every refusal below is a sentence in *cause (malloc'd, any length; the
 * old one freed; the caller frees it), NULL cause for none. */
/* The refusal of an export in an earlier shape (ONE-SHAPE rule 6). */
#define MODEXPORT_EARLIER "This export was saved by an earlier Snapmap+; run the Snapmap+ installer to update it."

typedef struct modexport_stats {
    unsigned bundles;              /* the rows (mods) the map or prefab names */
    unsigned bundles_installed;    /* export: units carried; import: units newly put in the content cache */
    unsigned bundles_present;      /* import: units the cache held already */
    unsigned bundles_absent;       /* import: rows whose content is not all here now (their objects stand in) */
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

/* Check a map's export without installing anything: a readable ZIP, not
 * in an earlier shape, holding one top-level .json that is a map. */
int   modexport_probe(const char *zip_path, char **cause);

/* Import a map's export: its units and files into the content cache, then the map text (malloc'd,
 * NUL-terminated), or NULL with the cause. */
char *modexport_read(const char *zip_path, size_t *out_len, modexport_stats *st, char **cause);

/* ---------------------------------------------- prefab exports ------ */
/* A prefab's export is written for every prefab, with or without mods, in
 * the layout above: <name>.json is the prefab file, whose block's "mods" rows
 * name the units the ZIP carries. An export that has been extracted (a folder
 * holding <name>.json, units/ and files/) imports as the ZIP does: the
 * importer is given the .json, and its folder is the export.
 *
 * The importers tell the kinds apart by the carried .json: an
 * idSnapEntityPrefab, or an idSnapMap with a block, is a prefab; an idSnapMap
 * (or an object without ~type) without a block is a map. Each refuses the
 * other's export. */

/* The prefab text to carry, given its rows as the export pinned them (`moved` is 1 when any row changed).
 * malloc'd, *len set; NULL refuses the export. */
typedef char *(*modexport_prefab_fn)(void *ctx, const sh_modmanifest_list *rows, int moved, size_t *len);

/* Write a prefab's export, `rows` pinned as a map's are. `name` (UTF-8) names the entry <name>.json. text_for may
 * be NULL (the prefab text is carried as given). zip_path is UTF-8. */
int modexport_write_prefab(const char *name, const char *prefab, size_t len, const sh_modmanifest_list *rows,
                           modexport_prefab_fn text_for, void *ctx, const char *zip_path, modexport_stats *st,
                           char **cause);

/* The rows a prefab text names (its block's), for the import's count of content it lacks. Returns the count, -1 to
 * refuse, -2 to refuse a prefab in an earlier shape (MODEXPORT_EARLIER). */
typedef int (*modexport_rows_fn)(void *ctx, const char *prefab, size_t len, sh_modmanifest_list *rows);

/* Import a prefab's export: path (UTF-8) is the ZIP, or a .json whose folder is an extracted export. Its units and
 * files go into the content cache, then the prefab text (malloc'd) and its name (the .json's file name without the
 * extension, UTF-8). 1 on success, else 0 with the cause. st->bundles_absent counts rows whose content is not all
 * here now. */
int modexport_read_prefab(const char *path, modexport_rows_fn rows_of, void *ctx, char **prefab, size_t *prefab_len,
                          char **name, modexport_stats *st, char **cause);

/* Kept for the callers that register it; an import builds nothing now. */
void modexport_set_stock(sh_mb_exists_fn stock, void *ctx);

/* ---------------------------------------------- rawmap file hooks ---- */
/* The rawmap layer (rawmap.c) calls these for a path ending in .zip (UTF-8,
 * as every rawmap path is). Each logs one MODS: line with byte counts and the
 * time taken. */

/* The writer: returns the ZIP's byte count, 0 on failure. */
unsigned long long modexport_write_text(const char *data, size_t len, const char *path);
/* The same writer for a caller that reports the outcome itself: *bundles
 * (optional) receives the mods the map's rows name, and on failure `cause`
 * says why (a mod the map names is not installed, the folder is not
 * writable). */
unsigned long long modexport_write_text_report(const char *data, size_t len, const char *path,
                                               unsigned *bundles, char **cause);
/* The reader for a caller that tells the player: the map text as modexport_read returns it, in a NUL-terminated
 * process-heap buffer (HeapFree), or NULL with the importer's words in *cause. */
char *modexport_read_text_report(const char *path, size_t *out_len, char **cause);
/* The source check: 1 when the export can be loaded, else 0 with a reason
 * (*out_msg, malloc'd: "ok" or the reason). */
int   modexport_validate_source(const char *path, char **out_msg);

#endif /* SNAPMAP_PLUS_MODEXPORT_H */
