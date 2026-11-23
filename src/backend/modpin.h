/* modpin.h -- pinning a map's local rows to units (SPEC-distribution 5.3 step 1, 5.16).
 *
 * A local row (modmanifest.h: "local": true, D6a) follows the local folder of its namespace and names no unit. When
 * a map leaves this machine (a publish, an export) every local row is pinned: for each object the row places (its
 * `names`, the placed names under its namespace), the unit of that object is computed from the folder's current
 * build (modunit.h sh_unit_of, proved there), its manifest is written into the content cache (units\<hash>.json),
 * and the row becomes a pinned row naming those units (sh_modmanifest_row_pin), with a module record for each placed
 * mod module (modcache.h sh_cache_row_record_modules, D60a). A map-level row ("attached", "Use in this map") pins the
 * whole build as its `whole` unit, with every placed object's unit beside it. A row already pinned is left as it is
 * (a downloaded map keeps its units, I2), and so is the product's own entry, which is not a row.
 *
 * A unit whose walk or proof fails is a builder defect, not an author's error (3.7): that row is pinned to the
 * whole build instead, with one log line naming the object, and the map carries more but works. A row whose folder
 * is not on this computer (or does not read) is left local and reported, so the publish says S8 for its objects.
 *
 * No blob is copied: the folder's files serve the composition and the upload by their SHA-256 (D21, D51). Nothing
 * here talks to the store. Pure of the engine: the folder, the game's files and the module texts come through the
 * bound functions below. Runs off the main thread (the publish and export workers).
 */
#ifndef SNAPMAP_PLUS_MODPIN_H
#define SNAPMAP_PLUS_MODPIN_H

#include <stddef.h>
#include "modunit.h"

struct sh_modmanifest_row;
struct sh_modmanifest_list;

/* The built folder serving a local row on this machine (absolute path, malloc'd), or NULL when none does. The default
 * asks the serving index (modserve.h sh_modserve_row_build) and the mods root (modstore.h sh_ms_root). */
typedef char *(*sh_modpin_folder_fn)(void *context, const struct sh_modmanifest_row *row);
/* A placed module's decl text (malloc'd, its length in *len), or NULL: the default reads what the serving index
 * serves for the request. */
typedef char *(*sh_modpin_read_fn)(void *context, const char *request, size_t *len);

void sh_modpin_set_folder(sh_modpin_folder_fn fn, void *context);
void sh_modpin_set_read(sh_modpin_read_fn fn, void *context);
/* The game's files for the units' pointers (modunit.h sh_unit_game_fn). The default reads the installed game
 * (modstock.h) and the product's own container (modprog.h, source "snapmapplus"). */
void sh_modpin_set_game(sh_unit_game_fn fn, void *context);
/* The requests players asked a unit for that it lacked (3.8: the store's asked list for this computer's claims),
 * seeds of the units computed from now on (modunit.h sh_unit_env.asked). Copied; forgets the units remembered so far. */
void sh_modpin_set_asked(const sh_unit_asked *asked, size_t n);
/* The default game reader, for tools that pin as the product does. */
int  sh_modpin_game_installed(void *context, const char *request, char sha256[SH_MB_HASH_HEX + 1], char *source,
                              size_t cap);

#define SH_MODPIN_PINNED  1   /* the row names units now */
#define SH_MODPIN_WHOLE   2   /* an object's unit did not prove, so the row names the whole build */
#define SH_MODPIN_ABSENT  3   /* no folder on this computer serves the row: it stays local (S8 at a publish) */

typedef struct sh_modpin_row_result {
    char salt[16];
    int state;                /* SH_MODPIN_* */
    size_t units;             /* units the row names now */
    unsigned long long bytes; /* their carried bytes, each distinct file once */
    char *why;                /* SH_MODPIN_WHOLE / ABSENT: the reason, for the log (malloc'd) */
} sh_modpin_row_result;

typedef struct sh_modpin_result {
    sh_modpin_row_result *rows;   /* one per local row met, in row order */
    size_t count;
} sh_modpin_result;
void sh_modpin_result_free(sh_modpin_result *r);

/* Pin every local row of `rows` in place (see above). `map_json` is the map text the rows belong to (for the module
 * records). `content_root` is where the manifests go (the mods root's .content; NULL: the cache's own,
 * modcache.h sh_cache_root). Returns the rows pinned (whole included), -1 when memory ran out or a manifest could not be
 * written (nothing is half-pinned: the rows are as they were). `out` (optional) says what happened per row. */
int  sh_modpin_rows(struct sh_modmanifest_list *rows, const char *map_json, size_t len, const char *content_root,
                    sh_modpin_result *out);

/* One row (the same rules). Returns SH_MODPIN_* or -1. */
int  sh_modpin_row(struct sh_modmanifest_row *row, const char *map_json, size_t len, const char *content_root,
                   sh_modpin_row_result *out);

#endif
