/* Chooses which map object a navigation snapshot describes: during a conversion the build map (the
 * idSnapMapEdit the SnapBuild wrapper converts the idSnapMap at editor+0x23C98 through), else the editor's edit
 * map at +0x204C8. The edit map is a fallback only when it is the same object or places exactly the same
 * modules in the same order; engine reads are callbacks, so the rule is pure. */
#ifndef SNAPMAP_PLUS_NAV_SNAPSHOT_H
#define SNAPMAP_PLUS_NAV_SNAPSHOT_H

#include <stddef.h>
#include "nav_regions.h"

typedef enum sh_nav_snapshot_source {
    SH_NAV_SNAPSHOT_NONE = 0,       /* nothing readable, or the fallback did not match */
    SH_NAV_SNAPSHOT_EDITOR,         /* no conversion running: the editor's edit map */
    SH_NAV_SNAPSHOT_CONVERTED,      /* the map being converted */
    SH_NAV_SNAPSHOT_EDITOR_SAME,    /* the edit map, the same object as the converted one */
    SH_NAV_SNAPSHOT_EDITOR_FALLBACK /* the edit map, used because the converted map is unreadable */
} sh_nav_snapshot_source;

/* Read one idSnapMapEdit into `out`. `converting` makes an entity the engine cannot measure take its inherited
 * size from the entityDef (nav_entitydef). Returns 1 for a usable map; `out` is overwritten either way and its
 * old tables are not freed (sh_nav_snapshot_take frees them between reads). */
typedef int (*sh_nav_snapshot_read_fn)(const void *edit_map, int converting,
                                       sh_nav_map *out, void *ctx);

/* Write the module declaration name of each placed instance of `edit_map`,
 * in record order, into names[i] (each `name_cap` bytes). Returns the number
 * of instances, or -1 when the records cannot be read or exceed `cap`. */
typedef int (*sh_nav_snapshot_modules_fn)(const void *edit_map, char *names, size_t name_cap,
                                          int cap, void *ctx);

typedef struct sh_nav_snapshot_sources {
    const void *build;              /* the map being converted; NULL outside a conversion */
    const void *edit;               /* editor+0x204C8; NULL when no editor map is open */
    sh_nav_snapshot_read_fn read;
    sh_nav_snapshot_modules_fn modules;
    void *ctx;
} sh_nav_snapshot_sources;

typedef struct sh_nav_snapshot_result {
    sh_nav_snapshot_source source;
    /* Why the converted map was not used, or why nothing was; empty when it was used or no conversion ran. */
    char why[160];
} sh_nav_snapshot_result;

/* Take the snapshot `src` calls for. Returns 1 when `out` holds a usable map. */
int sh_nav_snapshot_take(const sh_nav_snapshot_sources *src, sh_nav_map *out,
                         sh_nav_snapshot_result *result);

/* Reduce a module declaration name ("maps/modules/<category>/<module>", with
 * or without ".decl") to the "<category>/<module>" form sh_nav_map uses.
 * Returns 0 for a name outside maps/modules/. */
int sh_nav_snapshot_module_key(const char *decl, char *out, size_t cap);

/* Do the map's instances name exactly these modules, in this order? An
 * instance the reader could not name never matches. On a mismatch `why`
 * (optional) says where. */
int sh_nav_snapshot_modules_match(const sh_nav_map *map, const char *names, size_t name_cap,
                                  int count, char *why, size_t why_cap);

const char *sh_nav_snapshot_source_name(sh_nav_snapshot_source source);

/* Converts every floor and wall of `m` from world to module-local space using the owning instance's origin and
 * orientation; boxes of an unusable or unknown instance are untouched. */
void sh_nav_snapshot_map_to_local(sh_nav_map *m);

#endif /* SNAPMAP_PLUS_NAV_SNAPSHOT_H */
