/* store_convert.h -- the store tool's one-time conversion to the one shape
 * (ONE-SHAPE rule 6): every mod folder and every saved map an earlier
 * release wrote is converted once, at install or update, and the game reads
 * only the result. Linked only into the store tool (src\store\build.ps1).
 *
 * Folders. Every folder under mods\ (outside mods\maps\) whose mod.json is an
 * earlier shape (a format-1 block, an author id, key, shadows, stock, scope
 * or another retired key, package.json), from beta.20 to beta.26:
 *   - its old key is its author id, else its block's key, else its name;
 *   - extra copies of one key: the player's own folders are kept and
 *     converted; folders the product made ("<key>@<hash16>", and those under
 *     downloads\ or imports\) go to the Recycle Bin when the player has one,
 *     else the one that held the key first is kept and converted. Of several
 *     kept folders of a key, the key's maps follow the one the record already
 *     gives the key, else the one named after the key, else the first by path
 *     (DECISIONS 15: never a "built" time);
 *   - a kept folder is salted into a work folder beside it ("<name>.staging-
 *     <pid>", sh_ms_salt_copy, reading a view of mod.json and assets\ only)
 *     from a fresh author part (the block, the retired author keys and a hud
 *     "schema" cut out, every other byte kept), its mods/<old key>/ paths and
 *     texts rewritten to mods/<salt>/, its strings keys named "<salt>_<key>"
 *     with their #str_ references, its sound banks named "<salt>_<name>" with
 *     their bank id, every other file beside mod.json and assets\ copied
 *     unchanged (DECISIONS 16), then built by the real builder under the
 *     current rules (replacements kept: content the product received). A
 *     build that fails, or a salting the folder refuses (then the folder is
 *     copied as it is with a fresh salt), is written as a failed build with
 *     the builder's plain cause (DECISIONS 14): the game shows the cause and
 *     the author fixes the folder. The work folder is swapped in under the
 *     folder's own name; the folder as it was goes to the Recycle Bin. Only a
 *     folder whose descriptor does not read is left as it was, with its cause.
 * The record's section "converted" keeps what was done: {"keys": {"<old
 * key>": "<salt>"}, "builds": {"<old build hash>": "<salt>"}, "folders":
 * {"<folder under mods\>": "<salt>"}, "pending": {"<folder>": "<salt> <old
 * key> <old build or ->"}, "refused": {"<folder>": "<tree>"}, "maps":
 * {"<file>": "<sha256>"}} (maps left as they are). It is written after every
 * folder and every map. A folder's row is "pending" from just before its swap
 * until just after it, so a run stopped at any point resumes and loses
 * nothing: the next run puts back a folder set aside ("<name>.old-<pid>"),
 * removes work folders, and commits a pending row whose folder carries its
 * salt. Every run looks at every folder and map again (DECISIONS 21).
 *
 * Retirement. mods\.builds\ goes to the Recycle Bin, and the record's retired
 * sections ("cache", "aliases", "snapshots", "renamed", "downloads") are
 * removed after their old build hashes are recorded.
 *
 * Maps. Every saved map of My Maps (store_saves.h), the data folder's
 * rawmap.json and every prefab under prefabs\: a text in an earlier shape
 * (map_old.h; packages a map carried are converted first, modlegacy.h) has
 * its rows and placed names rewritten from old keys to salts at the root
 * folder's current build, its retired carriers turned into the member, and
 * is written back atomically with a copy of the file as it was beside it
 * ("<name>.before-snapmap-plus-1.0"; for a saved map, map.decl and
 * map.decl.verify, re-signed). A map naming a key no installed folder
 * provides, or whose signature cannot be matched, is left untouched with its
 * cause. A saved map is written as one step: the
 * new map.decl.verify and map.decl are written beside them, then put in
 * place map first; a run stopped in between leaves the new signature beside
 * it, which the next run puts in place. */
#ifndef SNAPMAP_PLUS_STORE_CONVERT_H
#define SNAPMAP_PLUS_STORE_CONVERT_H

#include <stddef.h>
#include <wchar.h>
#include "modstore_upgrade.h"

#define SH_CONV_BACKUP_SUFFIX ".before-snapmap-plus-1.0"
#define SH_CONV_RECORD        "converted"

typedef void (*sh_conv_line_fn)(void *context, int err, const char *line);

typedef struct sh_conv_opts {
    const char *data;           /* the data folder (UTF-8) */
    const char *mods;           /* <data>\mods */
    const wchar_t *saves;       /* savegame.user; NULL: no saved maps */
    int stock;                  /* the game's archives are registered: folders can be built */
    sh_ms_discard_fn discard;   /* the Recycle Bin */
    void *discard_context;
    sh_conv_line_fn line;       /* one plain line per action */
    void *line_context;
} sh_conv_opts;

typedef struct sh_conv_check {
    unsigned leftovers;         /* work folders and record rows an interrupted run left */
    unsigned folders;           /* folders in an earlier shape to convert */
    unsigned duplicates;        /* second builds the product made, to remove */
    unsigned folders_left;      /* left as they were before and unchanged since */
    int builds_dir;             /* mods\.builds\ */
    unsigned sections;          /* retired record sections */
    char section_names[96];
    unsigned maps;              /* maps in an earlier shape to convert */
    unsigned maps_left;         /* left as they were before and unchanged since */
    char first_map[260];
} sh_conv_check;

/* What the conversion would do (read only). `list` emits one line per thing
 * left as it was. Returns the kinds of work found. */
int sh_conv_scan(const sh_conv_opts *o, sh_conv_check *out, int list);

typedef struct sh_conv_stats {
    unsigned converted;         /* folders now in the one shape */
    unsigned failed;            /* of them, those whose build failed (the cause is in their block) */
    unsigned refused, removed, waiting;
    unsigned maps_converted, maps_refused;
    unsigned retired;
} sh_conv_stats;

/* The three steps, in this order. Each is idempotent: a second run finds
 * nothing and changes nothing. Each returns 1 when it did something, 0 when
 * there was nothing to do, -1 when the store cannot be read. */
int sh_conv_folders(const sh_conv_opts *o, sh_conv_stats *st);
int sh_conv_maps(const sh_conv_opts *o, sh_conv_stats *st);
int sh_conv_retire(const sh_conv_opts *o, sh_conv_stats *st);

/* One map text in the current shape. 1 converted (*out malloc'd), 0 already
 * current, -1 left as it is with the cause. Exposed for tests. */
int sh_conv_map_text(const sh_conv_opts *o, const char *text, size_t n, char **out, size_t *out_len, char *cause, size_t cap);

#endif /* SNAPMAP_PLUS_STORE_CONVERT_H */
