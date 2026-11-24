/* companion.h -- owner-bound companion entities that a mod's "companions" block (in mod.json's "extra")
 * spawns with each owner entity. Pure: parsing, validation, lookup and the transform; the engine side is
 * companion_engine.c.
 *
 * Owners must be the declaring bundle's own mods/<id>/ names ('-' and '_' equivalent); a block naming
 * another namespace is refused whole, and the first bundle to declare an owner keeps it. */
#ifndef SH_COMPANION_H
#define SH_COMPANION_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_COMPANION_NAME_CAP      192   /* including the terminator */
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
    sh_companion_entry *entries;   /* count of them (malloc'd; any number, no count of the product's own) */
} sh_companion_owner;

typedef struct sh_companion_table {
    sh_companion_owner *owners;
    int count, cap;
    int refused;        /* bundles or owners refused while building */
} sh_companion_table;

typedef void (*sh_companion_log_fn)(void *context, const char *line);

/* Adds the "companions" owners of one bundle's "extra" JSON. Returns the owners added, 0 when none
 * are declared, or -1 when the block was refused whole; `log` (may be NULL) gets one line per refusal. */
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

/* The twelve floats idEntity keeps from spawnPosition (+0x3A8) through spawnOrientation (+0x3B4): world
 * origin, then world axis rows. The engine side writes them between create and finish, as
 * idTarget_Spawn does (companion_engine.h). */
#define SH_COMPANION_SPAWN_POSE_FLOATS 12
void sh_companion_spawn_pose(const float owner_origin[3], const float owner_axis[9],
                             const sh_companion_entry *e, float out[SH_COMPANION_SPAWN_POSE_FLOATS]);

void sh_companion_table_free(sh_companion_table *t);

/* Decides whether to arm the CallSpawn detour (sh_companions_ensure in companion_engine.h). BUILD means
 * rebuild the table from the current index, set `owners`, and ask again. on_main is 1 on DOOM's main
 * thread, 0 elsewhere, -1 unknown; owners is -1 until the table is rebuilt; prologue_ok means
 * CallSpawn's first 14 bytes are the expected ones. */
#define SH_COMPANION_ARM_UNCHANGED   0   /* nothing to do: no new index since the last table */
#define SH_COMPANION_ARM_LIVE        1   /* already armed; the live detour follows the index itself */
#define SH_COMPANION_ARM_COMMIT      2   /* commit the detour */
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
