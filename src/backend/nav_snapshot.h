/* Choose which map object a navigation snapshot describes.
 *
 * Every SnapMap game is converted by the SnapBuild wrapper from the
 * idSnapMap embedded in the editor at +0x23C98. The wrapper builds an
 * idSnapMapEdit from it on its stack and hands that object to
 * SnapMapEditToSnapBuild, whose detour sees it as the build map. That object
 * is the same type as the editor's own edit map at +0x204C8, which only an
 * editor session fills and which nothing clears when the editor is left. A
 * hub, map browser, co-op or direct launch therefore converts a map the edit
 * map does not describe, and the edit map may still hold an earlier session's
 * map.
 *
 * The rule: during a conversion, read the build map. Fall back to the edit map
 * only when it is that same object, or when the build map cannot be read and
 * the edit map places exactly the modules the build places, in the same
 * order. Outside a conversion the edit map is the only source, as before.
 *
 * Pure: the engine reads are callbacks, so the rule is testable off the game.
 */
#ifndef SNAPMAP_PLUS_NAV_SNAPSHOT_H
#define SNAPMAP_PLUS_NAV_SNAPSHOT_H

#include <stddef.h>
#include "nav_regions.h"

typedef enum sh_nav_snapshot_source {
    SH_NAV_SNAPSHOT_NONE = 0,       /* nothing was readable, or the fallback did not match */
    SH_NAV_SNAPSHOT_EDITOR,         /* no conversion running: the editor's edit map */
    SH_NAV_SNAPSHOT_CONVERTED,      /* the map being converted */
    SH_NAV_SNAPSHOT_EDITOR_SAME,    /* the edit map, which is the object being converted */
    SH_NAV_SNAPSHOT_EDITOR_FALLBACK /* the edit map, after the converted map was unreadable */
} sh_nav_snapshot_source;

/* Read one idSnapMapEdit into `out`. `converting` says a conversion is under
 * way, so an entity the engine cannot measure takes its inherited size from
 * the entityDef instead of waiting for a later read. Returns 1 for a usable
 * map; `out` is overwritten either way. */
typedef int (*sh_nav_snapshot_read_fn)(const void *edit_map, int converting,
                                       sh_nav_map *out, void *ctx);

/* Write the module declaration name of each placed instance of `edit_map`,
 * in record order, into names[i] (each `name_cap` bytes). Returns the number
 * of instances, or -1 when the records cannot be read or exceed `cap`. */
typedef int (*sh_nav_snapshot_modules_fn)(const void *edit_map, char *names, size_t name_cap,
                                          int cap, void *ctx);

typedef struct sh_nav_snapshot_sources {
    const void *build;              /* the map being converted; NULL outside a conversion */
    const void *edit;               /* editor+0x204C8; NULL when no editor map was ever opened */
    sh_nav_snapshot_read_fn read;
    sh_nav_snapshot_modules_fn modules;
    void *ctx;
} sh_nav_snapshot_sources;

typedef struct sh_nav_snapshot_result {
    sh_nav_snapshot_source source;
    /* Why the converted map was not used, or why nothing was; empty when the
     * converted map was used or no conversion was running. */
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

/* Size an entity inherits from its entityDef when it spells no axis. Only
 * snapmaps/volume/blocking reaches the navigation reader; on build 13954591
 * its edit.clipModelInfo is CLIPMODEL_BOX with size 16 x 128 x 64. Returns 0
 * for any other entityDef. */
int sh_nav_snapshot_inherited_size(const char *entity_def, float size[3]);

const char *sh_nav_snapshot_source_name(sh_nav_snapshot_source source);

#endif /* SNAPMAP_PLUS_NAV_SNAPSHOT_H */
