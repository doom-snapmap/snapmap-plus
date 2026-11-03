/* modhud_scene.h -- the HUD follows a scene's own hide and show animation events.
 *
 * A campaign scene played in SnapMap (a mod's sync animation on the player's
 * third-person body) can carry the game's own HUD events in its md6Def rows:
 * ae_hideHudForContextPrompt on the frame the scene starts and
 * ae_showHudForContextPrompt on the frame it releases the player. In the
 * engine the hide event reaches an empty stub and the show event restores the
 * campaign HUD screen's saved state; SnapMap's co-op HUD honours neither
 * (R306). The backend detours the base event dispatcher both events pass
 * through and, during SnapMap Play only, turns them into g_showHud 0 and 1,
 * then runs the engine's own dispatch. A hide the scene never paired (the
 * sync cut short) is undone at the next Play entry.
 *
 * Logs:
 *   scene-hud installed: HUD follows a scene's hide and show events during Play
 *   scene-hud SKIPPED -- scenes cannot hide the HUD on this build
 *   scene-hud: hidden by a scene / shown again (first transition each way per Play)
 */
#ifndef SNAPMAP_PLUS_MODHUD_SCENE_H
#define SNAPMAP_PLUS_MODHUD_SCENE_H

#include <stddef.h>
#include "signatures.h"

#define SH_MODHUD_SCENE_EV_HIDE 0x123   /* ae_hideHudForContextPrompt, idEventDef+0x34 */
#define SH_MODHUD_SCENE_EV_SHOW 0x124   /* ae_showHudForContextPrompt */

/* What g_showHud should become for an event number: 0 hide, 1 show, -1 leave
 * it (another event, or the game is not in SnapMap Play). */
static __inline int sh_modhud_scene_decide(int event_number, int in_play)
{
    if (in_play != 1) return -1;
    if (event_number == SH_MODHUD_SCENE_EV_HIDE) return 0;
    if (event_number == SH_MODHUD_SCENE_EV_SHOW) return 1;
    return -1;
}

/* Install the dispatcher detour from the resolved signature database. Returns
 * 1 when installed, 0 when skipped (logged). */
int sh_modhud_scene_install(const sig_result *results, size_t n);

/* Play is starting: show the HUD again if a scene hid it and never showed it. */
void sh_modhud_scene_play_begin(void);

/* A mod scene node finished its scene (On Scene Finished): show the HUD again
 * if a scene hid it and its show event never fired (a show row on a frame the
 * sync no longer plays, R320). Returns 1 when it showed the HUD. */
int sh_modhud_scene_restore(void);

/* Counters for diagnostics: events seen (hide, show) and whether the HUD is hidden now. */
void sh_modhud_scene_stats(unsigned *hides, unsigned *shows, int *hidden);

#endif
