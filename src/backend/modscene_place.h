/* modscene_place.h -- a mod's scene node plays its scene at the player who
 * activated it, and puts that player back when the scene ends.
 *
 * A mod's campaign scene node is a SnapMap logic hexagon over an invisible
 * interactable. Its Activate input is an idSnapMapAction_Interaction whose palette
 * row is mods/<id>/actions/scene_play (IA_ACTIVATE); a placed copy has its own
 * entityDef that inherits that row, so both names are checked. The engine plays
 * the node's sync wherever the node entity stands: the interaction's
 * sync-start helper copies the owner's physics origin and axis into the sync
 * when the link runs, and the sync pulls the player there (R312). During
 * SnapMap Play the backend detours three engine functions:
 *
 *   Interaction DoAction   when the acting row is a mod's scene_play and the
 *                          activator is a player, every target node is moved
 *                          to the player's feet with the player's view yaw
 *                          (pitch and roll 0) before the deferred
 *                          interaction-action event reaches it, and the
 *                          player's origin and view angles are remembered
 *                          for that node. A scene_play row whose activator is
 *                          not a player (a demon in a trigger) or is missing
 *                          is dropped.
 *   Interactable USED      when a remembered node raises
 *                          SNAP_EVENT_INTERACTABLE_USED (On Scene Finished),
 *                          the player is teleported back to the remembered
 *                          origin and view angles before the output fires.
 *   OnActionRequest        an interaction request with no requester is
 *                          refused instead of faulting on the requester's
 *                          name (the R312 level-6 fault, raised by the
 *                          interactable's sync-end reset with an empty user
 *                          handle). This guard applies in every state: the
 *                          engine dereferences that requester unconditionally.
 *
 * Every other interaction action, every stock row and the editor are left to
 * the engine. Records are cleared at each Play entry.
 *
 * Logs (backend log and console; the runtime lines once per Play each):
 *   scene-place installed: mod scene nodes play at the activating player
 *   scene-place SKIPPED -- <reason>
 *   scene-place: interaction requests without a requester are refused
 *   scene-place: a scene node plays at <player> (<node> at x y z, yaw)
 *   scene-place: <player> returned after the scene (x y z, yaw)
 *   scene-place: dropped an Activate from <who>: only a player can play a scene node
 *   scene-place: refused interaction action <n> for <owner>: it has no requester
 *   scene-place: <a reason the scene plays at the node or the player stays>
 */
#ifndef SNAPMAP_PLUS_MODSCENE_PLACE_H
#define SNAPMAP_PLUS_MODSCENE_PLACE_H

#include <stddef.h>
#include <string.h>

#define SH_MODSCENE_PLACE_ROW_PREFIX "mods/"
#define SH_MODSCENE_PLACE_ROW_SUFFIX "/actions/scene_play"
#define SH_MODSCENE_PLACE_RECORDS    16

/* 1 when an entityDef name is a mod's scene_play Activate row
 * (mods/<id>/actions/scene_play), else 0. */
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
    const void *node;         /* the interactable that was moved; NULL = free */
    char player[64];          /* the player's entity name, found again at return */
    float origin[3];          /* the player's feet origin at Activate */
    float angles[3];          /* pitch, yaw, roll at Activate */
    unsigned stamp;           /* insertion order, for replacing the oldest */
} sh_modscene_place_record;

typedef struct sh_modscene_place_table {
    sh_modscene_place_record rows[SH_MODSCENE_PLACE_RECORDS];
    unsigned next_stamp;
} sh_modscene_place_table;

void sh_modscene_place_table_clear(sh_modscene_place_table *t);

/* Remember a pose for node, replacing that node's earlier pose, else using a
 * free row, else the oldest row. Returns 1 when stored. */
int sh_modscene_place_remember(sh_modscene_place_table *t, const void *node, const char *player,
                               const float origin[3], const float angles[3]);

/* Take and remove node's pose. Returns 1 and fills out when it was there. */
int sh_modscene_place_take(sh_modscene_place_table *t, const void *node, sh_modscene_place_record *out);

#ifndef SH_MODSCENE_PLACE_PURE
#include "signatures.h"

/* Install the detours from the resolved signature database. The empty-
 * requester guard installs on its own; the DoAction and USED detours install
 * together or not at all. Returns 1 when scenes play at the player, 0 when
 * that half is skipped (logged). */
int sh_modscene_place_install(const sig_result *results, size_t n, const unsigned char *module_base);

/* Play is starting: forget the previous Play's nodes and re-arm the once-per-Play logs. */
void sh_modscene_place_play_begin(void);
#endif

#endif
