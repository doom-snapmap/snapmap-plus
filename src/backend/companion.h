/* companion.h -- owner-bound companion entities a mod declares for its tiles.
 *
 * A mod's descriptor may carry, in mod.json's author block (the manifest keeps
 * it under "extra"):
 *
 *   "companions": {
 *     "mods/<id>/<owner entityDef>": [
 *       { "entityDef": "<entityDef to spawn>",
 *         "origin": [x, y, z],                     owner-local, default 0 0 0
 *         "axis":   [[r0], [r1], [r2]],            owner-local idMat3 rows, default identity
 *         "activate": true,                        add to the owner's targets, default true
 *         "bind": true }                           bind to the owner, default true
 *     ]
 *   }
 *
 * Every entity the engine spawns from the owner entityDef (a placed SnapMap
 * tile's play copy inherits it) gets its own new companions, spawned right
 * after the owner's spawn chain, at world origin = ownerOrigin + origin * ownerAxis
 * and world axis = axis * ownerAxis. A bound companion moves with its owner and
 * is removed by the owner's destructor. An activated companion is appended to
 * the owner's targets in list order, so the owner's native target activation
 * (Use with activateTargetsOnUse, a relay-style ActivateTargets) reaches each
 * companion's own Activate in that order, after the owner's earlier targets.
 *
 * Owners must be the declaring bundle's own names (mods/<id>/..., with '-' and
 * '_' equivalent in the id); a block naming another namespace is refused whole.
 * The first bundle to declare an owner keeps it; a later one is refused for
 * that owner and logged.
 *
 * This file is pure: parsing, validation, lookup and the transform. The engine
 * side is companion_engine.c. */
#ifndef SH_COMPANION_H
#define SH_COMPANION_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_COMPANION_NAME_CAP      192   /* including the terminator */
#define SH_COMPANION_MAX_PER_OWNER 16
#define SH_COMPANION_MAX_OWNERS    256   /* across all bundles */
#define SH_COMPANION_ORIGIN_MAX    65536.0
#define SH_COMPANION_AXIS_TOL      1e-3  /* row length and orthogonality tolerance */

typedef struct sh_companion_entry {
    char  entitydef[SH_COMPANION_NAME_CAP];
    float origin[3];
    float axis[9];      /* rows */
    int   activate;
    int   bind;
} sh_companion_entry;

typedef struct sh_companion_owner {
    char owner[SH_COMPANION_NAME_CAP];
    char bundle[SH_COMPANION_NAME_CAP];
    int  count;
    sh_companion_entry entries[SH_COMPANION_MAX_PER_OWNER];
} sh_companion_owner;

typedef struct sh_companion_table {
    sh_companion_owner *owners;
    int count, cap;
    int refused;        /* bundles or owners refused while building */
} sh_companion_table;

typedef void (*sh_companion_log_fn)(void *context, const char *line);

/* Parse one bundle's extra object (the manifest's "extra" JSON text) and add
 * its "companions" owners. Returns the number of owners added, 0 when the
 * bundle declares none, or -1 when its block was refused (nothing of it is
 * added); `log` (may be NULL) receives one line per refusal. */
int sh_companion_add_bundle(sh_companion_table *t, const char *bundle_id, const char *extra_json,
                            size_t length, sh_companion_log_fn log, void *context);

/* The owner named `name` (ASCII case-insensitive, either separator), or NULL. */
const sh_companion_owner *sh_companion_find(const sh_companion_table *t, const char *name);

/* 1 when `name` could name an owner: it starts with "mods/" (any case). The
 * engine side uses it to stop an inherit walk at the first stock name. */
int sh_companion_owner_namespace(const char *name);

/* World transform of `e` for an owner at origin/axis (idMat3 rows). */
void sh_companion_compose(const float owner_origin[3], const float owner_axis[9],
                          const sh_companion_entry *e, float out_origin[3], float out_axis[9]);

/* The spawn pose the engine reads in the companion's own spawn chain: world
 * origin, then world axis rows, the twelve floats idEntity keeps from
 * spawnPosition (+0x3A8) through spawnOrientation (+0x3B4). The engine side
 * writes them between create and finish, as idTarget_Spawn does, so the
 * class's Spawn places the companion itself (companion_engine.h). */
#define SH_COMPANION_SPAWN_POSE_FLOATS 12
void sh_companion_spawn_pose(const float owner_origin[3], const float owner_axis[9],
                             const sh_companion_entry *e, float out[SH_COMPANION_SPAWN_POSE_FLOATS]);

void sh_companion_table_free(sh_companion_table *t);

/* Arming the CallSpawn detour after boot (companion_engine.h,
 * sh_companions_ensure). The engine side fills what it knows and asks; a
 * BUILD answer means "rebuild the table from the current index, set `owners`,
 * and ask again". Fields: resolved (boot resolution succeeded), on_main (1 the
 * caller is DOOM's main thread, 0 another thread, -1 unknown), installed (the
 * detour is live), index_changed (the serving index was published again since
 * the table was built), owners (owners in the table, -1 not rebuilt yet),
 * prologue_ok (CallSpawn's first 14 bytes are the expected ones). Pure. */
#define SH_COMPANION_ARM_UNCHANGED   0   /* nothing to do: no new index since the last table */
#define SH_COMPANION_ARM_LIVE        1   /* already armed; the live detour follows the index itself */
#define SH_COMPANION_ARM_COMMIT      2   /* commit the detour now */
#define SH_COMPANION_ARM_BUILD       3   /* rebuild the table, then ask again */
#define SH_COMPANION_ARM_UNRESOLVED (-1) /* boot resolution did not run or failed */
#define SH_COMPANION_ARM_OFF_MAIN   (-2) /* never armed from another thread */
#define SH_COMPANION_ARM_NO_OWNER   (-3) /* the rebuilt table has no owner: the spawn chain stays untouched */
#define SH_COMPANION_ARM_FOREIGN    (-4) /* CallSpawn's prologue changed (another detour): refused */
typedef struct sh_companion_arm {
    int resolved, on_main, installed, index_changed, owners, prologue_ok;
} sh_companion_arm;
int sh_companion_arm_decide(const sh_companion_arm *a);
const char *sh_companion_arm_name(int decision);

#ifdef __cplusplus
}
#endif
#endif
