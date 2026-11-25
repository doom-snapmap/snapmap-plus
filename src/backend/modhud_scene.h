/* modhud_scene.h -- the HUD follows a scene's own hide and show animation events: a mod's sync scene can carry
 * ae_hideHudForContextPrompt and ae_showHudForContextPrompt, which SnapMap's co-op HUD ignores. The backend detours
 * the base event dispatcher both pass through and, during SnapMap Play only, turns them into g_showHud 0 and 1 before
 * the engine's own dispatch; a hide the scene never paired is undone at the next Play entry.
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

/* Play is starting: show the HUD again if a scene hid it and never showed it, and reread the weapon rules. */
void sh_modhud_scene_play_begin(void);

/* A mod scene node finished its scene (On Scene Finished): show the HUD again if a scene hid it and its show event
 * never fired (e.g. its frame is not played). Returns 1 when it showed the HUD. */
int sh_modhud_scene_restore(void);

#endif
