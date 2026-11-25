/* modscene_place.h -- a mod's scene node plays its scene at the player who activated it and puts the
 * player back when the scene ends; the DoAction, USED, OnActionRequest and sync-start detours do it,
 * and a mod's scenes block can change the anchor, the spawns and the return per node. */
#ifndef SNAPMAP_PLUS_MODSCENE_PLACE_H
#define SNAPMAP_PLUS_MODSCENE_PLACE_H

#include <stddef.h>
#include <string.h>

#define SH_MODSCENE_PLACE_ROW_PREFIX "mods/"
#define SH_MODSCENE_PLACE_ROW_SUFFIX "/actions/scene_play"

/* 1 when an entityDef name is a mod's scene_play Activate row
 * (mods/<salt>/actions/scene_play), else 0. */
static __inline int sh_modscene_place_is_scene_row(const char *name)
{
    size_t n, p = sizeof SH_MODSCENE_PLACE_ROW_PREFIX - 1, s = sizeof SH_MODSCENE_PLACE_ROW_SUFFIX - 1;
    if (!name) return 0;
    n = strlen(name);
    if (n <= p + s) return 0;
    return !strncmp(name, SH_MODSCENE_PLACE_ROW_PREFIX, p) && !strcmp(name + n - s, SH_MODSCENE_PLACE_ROW_SUFFIX);
}

enum {
    SH_MODSCENE_PLACE_PASS = 0,   /* call the engine unchanged */
    SH_MODSCENE_PLACE_PLACE = 1,  /* move the targets to the player, then call the engine */
    SH_MODSCENE_PLACE_DROP = 2    /* do not call the engine: no player to play the scene */
};

/* What the DoAction detour does with one action. in_play is
 * sh_modserve_engine_in_play() (1 during SnapMap Play). */
static __inline int sh_modscene_place_decide(int in_play, int is_scene_row, int activator_is_player)
{
    if (in_play != 1 || !is_scene_row) return SH_MODSCENE_PLACE_PASS;
    return activator_is_player ? SH_MODSCENE_PLACE_PLACE : SH_MODSCENE_PLACE_DROP;
}

/* The row-major axis of a yaw-only rotation (degrees), as idAngles(0, yaw,
 * 0).ToMat3(): forward (cos, sin, 0), left (-sin, cos, 0), up (0, 0, 1). */
void sh_modscene_place_yaw_axis(float yaw_degrees, float axis[9]);

/* 1 when an origin and view angles are finite and inside the world range the
 * engine accepts (|coordinate| < 1e6, |angle| < 1e5). */
int sh_modscene_place_pose_ok(const float origin[3], const float angles[3]);

/* The remembered player pose for one moved node. */
typedef struct sh_modscene_place_record {
    const void *node;         /* the moved interactable */
    char *player;             /* the player's entity name, whole (malloc'd), found again at return */
    float origin[3];          /* the player's feet origin at Activate */
    float angles[3];          /* pitch, yaw, roll at Activate */
} sh_modscene_place_record;

/* One row per moved node, as many as are moved: the table grows and nothing
 * is dropped. */
typedef struct sh_modscene_place_table {
    sh_modscene_place_record *rows;   /* malloc'd, `capacity` of them; rows[0..count) in use */
    size_t count, capacity;
} sh_modscene_place_table;

void sh_modscene_place_table_clear(sh_modscene_place_table *t);

/* Remember a pose for node, replacing that node's existing pose. Returns 1 when stored, 0 for a bad
 * argument or when memory runs out (the table is then unchanged). */
int sh_modscene_place_remember(sh_modscene_place_table *t, const void *node, const char *player,
                               const float origin[3], const float angles[3]);

/* Take and remove node's pose. Returns 1 and fills out when found; out->player is then the caller's
 * to free. */
int sh_modscene_place_take(sh_modscene_place_table *t, const void *node, sh_modscene_place_record *out);

#ifndef SH_MODSCENE_PLACE_PURE
#include "signatures.h"

/* Install the detours from the resolved signatures. The empty-requester guard installs on its own;
 * DoAction and USED install together or not at all. Returns 1 when installed, 0 when skipped (logged). */
int sh_modscene_place_install(const sig_result *results, size_t n, const unsigned char *module_base);

/* Play is starting: forget the previous Play's nodes and re-arm the once-per-Play logs. */
void sh_modscene_place_play_begin(void);
#endif

#endif
