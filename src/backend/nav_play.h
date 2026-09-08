/* nav_play.h -- refresh the author's navigation marks when Play starts.
 *
 * WHY THIS EXISTS
 * ---------------
 * The author ticks "AI Navigation" on a Blocking Box and presses Play. Pressing
 * Play does NOT serialize the map -- SnapMapEditToSnapBuild (0x4F27B0) reaches
 * neither DeserializeFromJson nor SerializeToJson, verified against the binary --
 * so the map JSON the deserialize funnel handed the baker is the map as it was
 * LOADED, and the tick made this session exists only in the live editor. Without
 * something to read it there, the author has to save and reload before their mark
 * has any effect, which is the reported bad UX.
 *
 * WHY NOT JUST READ IT FROM THE BAKE
 * ----------------------------------
 * Because that is what shipped, and it is what issues #87 and #89 are. The bake
 * runs inside the engine's AAS loader, several sub-builds into
 * SnapMapEditToSnapBuild, and reading live entities from there called the
 * engine's EntityClone (0x5A6460) on entities whose defsub was still NULL. That
 * is an unconditional read of [NULL+0x40] -- thousands of access violations
 * inside the loader, one of which the fault shield escalated to idCommon::Error(6)
 * and killed the process with.
 *
 * So the read moves to the ONE point that is both on DOOM's main thread and
 * before any of the building starts: the entry of SnapMapEditToSnapBuild itself.
 * BuildAAS is called from inside it (three times, once per demon size class, at
 * +0x35C/+0x373/+0x38A), so entry strictly precedes every bake, and the editor
 * still owns its map because turning that map into the build map is precisely
 * what the function has not done yet.
 */
#ifndef SNAPMAP_PLUS_NAV_PLAY_H
#define SNAPMAP_PLUS_NAV_PLAY_H

/* Detour SnapMapEditToSnapBuild so the live marks are re-read before the build.
 * `status_ok` is false when the signature only resolved through the hook-tolerant
 * fallback, in which case the prologue is already somebody else's detour and we
 * refuse rather than stealing detour bytes. Returns 1 when installed. */
int sh_nav_play_install(void *snapbuild_fn, int status_ok);

#endif /* SNAPMAP_PLUS_NAV_PLAY_H */
