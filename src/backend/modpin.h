/* Pins a map's local rows to units when the map leaves this machine (a publish, an export); a row already pinned and
 * the product's own entry are left as they are. No blob is copied and nothing here talks to the store; it is pure of
 * the engine and runs off the main thread (the publish and export workers). */
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
/* Seed the units computed afterwards with the requests players asked of them that they lacked (modunit.h
 * sh_unit_env.asked). Copied; forgets the units remembered so far. */
void sh_modpin_set_asked(const sh_unit_asked *asked, size_t n);
/* The default game reader, for tools that pin as the product does. */
int  sh_modpin_game_installed(void *context, const char *request, char sha256[SH_MB_HASH_HEX + 1], char *source,
                              size_t cap);

#define SH_MODPIN_PINNED  1   /* the row names units */
#define SH_MODPIN_WHOLE   2   /* an object's unit did not prove, so the row names the whole build */
#define SH_MODPIN_ABSENT  3   /* no folder on this computer serves the row: it stays local */

typedef struct sh_modpin_row_result {
    char salt[16];
    int state;                /* SH_MODPIN_* */
    size_t units;             /* units the row names */
    unsigned long long bytes; /* their carried bytes, each distinct file once */
    char *why;                /* SH_MODPIN_WHOLE / ABSENT: the reason, for the log (malloc'd) */
} sh_modpin_row_result;

typedef struct sh_modpin_result {
    sh_modpin_row_result *rows;   /* one per local row met, in row order */
    size_t count;
} sh_modpin_result;
void sh_modpin_result_free(sh_modpin_result *r);

/* Pin every local row of `rows` in place (see above). `map_json` is the map text the rows belong to (for the module
 * records). `content_root` is where the manifests go (the profile's cache/content folder; NULL: the cache's own,
 * modcache.h sh_cache_root). Returns the rows pinned (whole included), -1 when memory ran out or a manifest could not be
 * written (nothing is half-pinned: the rows are as they were). `out` (optional) says what happened per row. */
int  sh_modpin_rows(struct sh_modmanifest_list *rows, const char *map_json, size_t len, const char *content_root,
                    sh_modpin_result *out);

/* Whether the installed version of a pinned row's mod would change what the row serves: 1 when one of its objects'
 * units (or its whole-mod unit, for a map-level row) computed from the installed build differs from the pinned one,
 * counting an object the installed build lacks; 0 when all are the same; -1 when no built folder of the salt is here or
 * the row is not pinned. Units are remembered by build and root. Any thread. */
int  sh_modpin_row_differs(const struct sh_modmanifest_row *row);

/* One row (the same rules). Returns SH_MODPIN_* or -1. */
int  sh_modpin_row(struct sh_modmanifest_row *row, const char *map_json, size_t len, const char *content_root,
                   sh_modpin_row_result *out);

int sh_modpin_publish_row(struct sh_modmanifest_row *row, sh_modpin_row_result *out);
void sh_modpin_record_modules(struct sh_modmanifest_row *row, const char *map_json, size_t len);

#endif
