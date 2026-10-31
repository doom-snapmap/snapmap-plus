/* module_prefab_map.h -- cut Blueprint modules out of a saved map and merge them
 * into another. Pure functions over idSnapMap JSON (the engine's SerializeToJson
 * text); no engine dependency, unit-tested offline.
 *
 * A module prefab file is itself an idSnapMap holding only the chosen modules:
 *   instances        the chosen instances in selection order, origins relative
 *                    to the first one (the anchor);
 *   instanceEntities their groups (CSR of uniqueIds), then an empty global group;
 *   entities         the entities of those groups, uniqueIds unchanged;
 *   doorsAndCaps     the portal slots of those instances, in instance order;
 *   targets          the wires with both ends inside the set;
 *   references       each entity's entityEntRefs / entityVarRefs rows, slot
 *                    counts unchanged: an entity reference that leaves the set
 *                    becomes -1, a variable reference is remapped to the
 *                    variables carried in the file;
 *   variables        only the variables those entities reference (plus the
 *                    fixed persistent integers), every other top-level member
 *                    copied from the source map.
 * Entity positions are module-local, so moving an instance moves its entities.
 *
 * Variable references are (type << 24) | index, index 0xFFFFFF meaning unbound.
 * Types: 0 integer, 1 number, 2 boolean, 3 color, 4 string, 5 playerResource,
 * 6 teamResource, 7 cachedEntity, 8 customEvent, 9 persistentInteger (fixed,
 * never carried or remapped). */
#ifndef SH_MODULE_PREFAB_MAP_H
#define SH_MODULE_PREFAB_MAP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_MP_MAX_ENVS      16
#define SH_MP_NAME_CAP      260
#define SH_MP_MAX_MODULES   1024

typedef struct sh_mp_module {
    char  name[SH_MP_NAME_CAP];   /* moduleName (decl path) */
    float origin[3];              /* relative to the anchor */
    int   rotation;               /* orientation, 0..7 */
} sh_mp_module;

typedef struct sh_mp_summary {
    int module_count;
    int entity_count;
    int door_count;               /* portal slots (doorways, capped or open) */
    int variable_count;           /* carried variables, persistent integers excluded */
    int dropped_wires;            /* wires with one end outside the set */
    int dropped_refs;             /* entity references set to -1 at the boundary */
    int has_nav_boxes;            /* a Blocking Box ticked for AI Navigation (flags.noFlood) */
    int env_count;
    char envs[SH_MP_MAX_ENVS][SH_MP_NAME_CAP];
    sh_mp_module *modules;        /* module_count entries, malloc-owned */
} sh_mp_summary;

void sh_mp_summary_free(sh_mp_summary *s);

/* Cut the instances sel[0..nsel) (selection order; sel[0] is the anchor) out of
 * map. On success returns 1 with *out (malloc-owned, NUL-terminated) and *sum;
 * returns 0 with a reason in err. */
int sh_mp_cut(const char *map, size_t len, const int *sel, int nsel,
              char **out, size_t *out_len, sh_mp_summary *sum, char *err, int errcap);

/* Merge the prefab frag into map with the anchor at `at` (world). New
 * entities get fresh uniqueIds above every id the map uses (and above any
 * stale reference rows); variables are appended and remapped. *first_instance
 * is the index of the first added instance and *added the number added.
 * Returns 1 with *out, or 0 with a reason in err. */
int sh_mp_merge(const char *map, size_t len, const char *frag, size_t flen, const float at[3],
                char **out, size_t *out_len, int *first_instance, int *added, char *err, int errcap);

/* Re-derive a summary from a prefab file. 1 ok. Dropped counts are 0 (they are
 * only known at cut time). */
int sh_mp_inspect(const char *frag, size_t flen, sh_mp_summary *sum, char *err, int errcap);

/* 1 when text's top-level "~type" is "idSnapMap" (a module prefab), 0 when it
 * is anything else, including "idSnapEntityPrefab". Scans only the top level. */
int sh_mp_is_module_file(const char *text, size_t len);

/* The module guard. A prefab is placed only when every module it names is one
 * the receiving editor can build: a record in its module palette (stock rooms
 * and owned DLC), a Grid Room size the grid-room adapter makes, or a module an
 * installed mod serves. The engine has no safe answer for any other name: the
 * map validator drops the instance and keeps its entities, and the module
 * record builder raises "Couldn't find module in modulePalette". */

/* known(ctx, name): 1 the receiver can build the module, 0 it cannot, -1 it
 * cannot tell (no editor palette yet). */
typedef int (*sh_mp_module_known_fn)(void *ctx, const char *name);

#define SH_MP_CHECK_NAMES 8
typedef struct sh_mp_module_check {
    int checked;            /* distinct names the callback answered (0 or 1) */
    int unknown;            /* distinct names it could not tell (-1) */
    int missing_count;      /* distinct names it cannot build, all of them */
    int listed;             /* names kept in missing[], at most SH_MP_CHECK_NAMES */
    char missing[SH_MP_CHECK_NAMES][SH_MP_NAME_CAP];
} sh_mp_module_check;

/* Ask known() once for each distinct moduleName in s (ASCII case-insensitive).
 * An empty name is missing. Returns missing_count, or -1 when no name could be
 * answered (nothing is known about the palette: the caller must not refuse). */
int sh_mp_check_modules(const sh_mp_summary *s, sh_mp_module_known_fn known, void *ctx,
                        sh_mp_module_check *out);

/* "maps/modules/ind_dlc/ind_room.decl" -> "ind_dlc/ind_room": the name a toast shows. */
void sh_mp_module_short_name(const char *name, char *out, size_t cap);

/* The refusal texts for a check with missing_count > 0. Each returns the length
 * written (truncated to cap). The console line names every listed module in full:
 *   needs module <name>; this room is not in your palette (DLC or a mod you do not have)
 * The toast names them shortly and says nothing was placed. The status is the
 * Prefabs tab line shown before Load. */
int sh_mp_missing_console_line(const sh_mp_module_check *c, char *out, size_t cap);
int sh_mp_missing_toast(const sh_mp_module_check *c, char *out, size_t cap);
int sh_mp_missing_status(const sh_mp_module_check *c, char *out, size_t cap);

/* The Prefabs tab's check result: {"checked":true,"missing":["<name>",...],"more":N},
 * or {"checked":false} when check is NULL or nothing could be answered. Returns
 * malloc-owned text or NULL. */
char *sh_mp_check_json(const sh_mp_module_check *c);

/* The summary view the save composes into the prefab's snapmapPlus block
 * (prefab_mods.h): {"desc":"","tags":[],"kind":"modules",...}; every member
 * but desc and tags becomes the block's "summary". bounds, when
 * not NULL, holds one world-axis box per module (min xyz, max xyz) relative to
 * the anchor; labels (may be NULL) the display names. desc/tags are written
 * empty. Returns malloc-owned text or NULL. */
char *sh_mp_sidecar_json(const sh_mp_summary *s, const float (*bounds)[6], const char *const *labels);

#ifdef __cplusplus
}
#endif

#endif
