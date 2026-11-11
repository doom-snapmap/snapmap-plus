/* modscene_decl.h -- a mod's "scenes" block: where a scene node's scene plays,
 * what it spawns, and whether its player is turned and returned.
 *
 * A mod's descriptor may carry, in mod.json's author block (the manifest keeps
 * it under "extra"):
 *
 *   "scenes": {
 *     "mods/<salt>/<scene node entityDef>": {
 *       "plays":  "at_player",            or "at_node"; default "at_player"
 *       "anchor": [x, y, z, yaw],         the scene-frame pose the node marks; default 0 0 0 0
 *       "player": [x, y, z, yaw],         the player's first pose in the scene frame
 *       "spawn":  [ { "entityDef": "<entityDef>", "at": [x, y, z, yaw] } ],
 *                                         spawned when the node fires; "at" defaults to the anchor
 *       "reaim":  true,                   turn the player toward the anchor; default true with "player"
 *       "return": false }                 put the player back after the scene
 *   }
 *
 * Poses are yaw-only (degrees about +z) in the scene's own frame, the frame of
 * the node's sync. The node's placed transform in the editor is the anchor:
 * the scene frame the node marks is node * inverse(anchor), and every "spawn"
 * entity goes there, at that frame * at, whatever "plays" says. So the author
 * places the node where the scene's anchor pose belongs (where a demon
 * arrives) and turns it the way that pose faces.
 *
 *   at_player  the scene's sync plays at the activating player. With "player"
 *              the frame is placed so the scene's first player pose lands on
 *              the player's feet, facing the player's view yaw; with "reaim" it
 *              faces so that, from that first pose, the anchor is where the
 *              node stands. Without "player" the frame is the player's feet
 *              and view yaw (R313).
 *   at_node    the sync plays at the frame the node marks; the player stays.
 *
 * "return" defaults to true for an at_player entry without "spawn" (R313: the
 * player is teleported back when the node raises On Scene Finished), and to
 * false otherwise. A node without an entry plays at the player and returns
 * them, as before. "reaim" needs "player" and at_player.
 *
 * Owners must be the declaring mod's own names (mods/<salt>/...); a block
 * naming another namespace is refused whole. Unknown keys are refused. The
 * table holds one build per salt (the one the open map serves,
 * modscene_place.c), so no owner is declared twice.
 *
 * This file is pure: parsing, validation, lookup and the frame math. */
#ifndef SH_MODSCENE_DECL_H
#define SH_MODSCENE_DECL_H
#include <stddef.h>
#include "signatures.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SH_SCENE_NAME_CAP    192   /* including the terminator */
#define SH_SCENE_MAX_SPAWNS  4
#define SH_SCENE_MAX_OWNERS  256   /* across all bundles */
#define SH_SCENE_COORD_MAX   65536.0
#define SH_SCENE_YAW_MAX     3600.0

/* A yaw-only pose: origin and degrees about +z. */
typedef struct sh_scene_pose {
    float x, y, z, yaw;
} sh_scene_pose;

typedef struct sh_scene_spawn {
    char entitydef[SH_SCENE_NAME_CAP];
    sh_scene_pose at;              /* in the scene frame */
} sh_scene_spawn;

typedef struct sh_scene_entry {
    char node[SH_SCENE_NAME_CAP];
    char bundle[SH_SCENE_NAME_CAP];   /* the declaring mod's salt */
    int at_node;                   /* 1: "plays": "at_node" */
    sh_scene_pose anchor;          /* scene-frame pose the node's placed transform marks */
    int has_player;
    sh_scene_pose player;          /* the player's first pose in the scene frame */
    int reaim;
    int return_player;
    int spawn_count;
    sh_scene_spawn spawns[SH_SCENE_MAX_SPAWNS];
} sh_scene_entry;

typedef struct sh_scene_table {
    sh_scene_entry *rows;
    int count, cap;
    int refused;                   /* bundles or owners refused while building */
} sh_scene_table;

typedef void (*sh_scene_log_fn)(void *context, const char *line);

/* Parse one mod's author keys (the serving index's "extra" JSON text) and add
 * its "scenes" owners. Returns the number added, 0 when the mod declares
 * none, or -1 when its block was refused (nothing of it is added); `log` (may
 * be NULL) receives one clause per refusal, beginning "its scenes entry ..."
 * (a caller names the mod before it). */
int sh_scene_add_bundle(sh_scene_table *t, const char *salt, const char *extra_json, size_t length,
                        sh_scene_log_fn log, void *context);


/* The table of the builds the open map serves: `each` visits every admitted
 * build with its salt, hash, place flag and author keys (the serving layer's
 * sh_modserve_each_bundle_extra, root folders first), `serves_now(hash)` says
 * whether the open map uses that build (NULL: every build). The first build
 * of a salt that serves now is read; a block that does not read logs one line
 * ("SCENES: <salt>: its scenes entry ...; its scene nodes play at the
 * player") and adds nothing. Returns the entries, -1 when the builds could not
 * be visited (the table is empty). */
typedef void (*sh_scene_build_fn)(void *context, const char *salt, const char *hash, int place, const char *extra);
typedef int (*sh_scene_each_fn)(sh_scene_build_fn fn, void *context);
int sh_scene_table_load(sh_scene_table *t, sh_scene_each_fn each, int (*serves_now)(const char *bundle_hash),
                        sh_scene_log_fn log, void *context);

/* The entry for `name` (ASCII case-insensitive, either separator), or NULL. */

const sh_scene_entry *sh_scene_find(const sh_scene_table *t, const char *name);

/* 1 when `name` could name an owner (it starts with "mods/", any case). */
int sh_scene_owner_namespace(const char *name);

void sh_scene_table_free(sh_scene_table *t);

/* Spawns queued by a node's Activate until its sync starts: one row per node
 * (a later Activate replaces it), the oldest evicted when full. `node` is
 * compared, never dereferenced; the spawn poses are world poses. */
#define SH_SCENE_PENDING_ROWS 16

typedef struct sh_scene_pending {
    const void *node;
    char node_name[SH_SCENE_NAME_CAP];
    int count;
    sh_scene_spawn spawns[SH_SCENE_MAX_SPAWNS];
    unsigned stamp;
} sh_scene_pending;

typedef struct sh_scene_pending_table {
    sh_scene_pending rows[SH_SCENE_PENDING_ROWS];
    int count;                     /* rows in use */
    unsigned next_stamp;
} sh_scene_pending_table;

/* Queue `count` (1 to SH_SCENE_MAX_SPAWNS) spawns for `node`. Returns 1, or 0
 * for a bad argument. */
int sh_scene_pending_set(sh_scene_pending_table *t, const void *node, const char *node_name,
                         const sh_scene_spawn *spawns, int count);
/* Take and remove `node`'s row into *out. Returns 1 when there was one. */
int sh_scene_pending_take(sh_scene_pending_table *t, const void *node, sh_scene_pending *out);
void sh_scene_pending_clear(sh_scene_pending_table *t);

/* Pose algebra: a * b (b expressed in a's frame), and the inverse. */
sh_scene_pose sh_scene_compose(sh_scene_pose a, sh_scene_pose b);
sh_scene_pose sh_scene_inverse(sh_scene_pose a);

/* The scene frame the node marks: node * inverse(anchor). Spawns go at
 * sh_scene_compose(sh_scene_node_frame(e, node), spawn.at). */
sh_scene_pose sh_scene_node_frame(const sh_scene_entry *e, sh_scene_pose node);

/* Where the node's sync plays. `node` is the node's placed pose, `player` the
 * activating player's feet origin with their view yaw. For at_node this is the
 * node frame; for at_player see the header comment. *reaimed is set to 1 when
 * the frame was turned toward the anchor. */
sh_scene_pose sh_scene_play_frame(const sh_scene_entry *e, sh_scene_pose node, sh_scene_pose player, int *reaimed);

/* The engine calls a spawn makes (modscene_sigs.c), for the two-image test:
 * SceneCreateEntity (idGameLocal vtable +0x448), SceneFinishSpawn (+0x458) and
 * SceneSyncStart (the interaction link's sync-start helper, detoured).
 * NULL-terminated, in that order. */
extern const sig_entry SH_MODSCENE_SPAWN_SIGNATURES[];

#ifdef __cplusplus
}
#endif
#endif
