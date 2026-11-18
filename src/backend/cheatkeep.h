/* Player cheats kept over the player's own spawn.
 *
 * The five player-cheat commands (noClip, infiniteHealth, noPlayerDeath, noPlayerKill, noTarget) toggle bits of
 * the local idPlayer. A SnapMap Play reaches gameplay frames with the player already in the world, then the game
 * mode places it at its spawn point a second or more later (VK FUN_140857EC0 -> FUN_140DE2290, "SPAWN: ..."), and
 * that spawn rewrites the cheat byte to 0 and increments the player's spawn count (+0x27D70). A cheat asked for in
 * between was silently undone: a test that turned noTarget on as soon as Play began was fought by every demon.
 *
 * The rule: a cheat the user turned on stays on for that player entity until it is turned off. When the same
 * player's spawn count changes (a spawn reset its cheats) the bits asked for are set again. Nothing else is
 * touched: the engine's own writes between spawns (the environment suit's fake death and revive set and clear
 * noTarget) are left alone, and a different player entity (the next Play) starts with nothing asked for.
 * Pure: no engine access, so the rule is tested on its own (tests/cheatkeep_test.c). */
#ifndef SH_CHEATKEEP_H
#define SH_CHEATKEEP_H

#include <stdint.h>

typedef struct sh_cheat_keep {
    const void *player;   /* the entity the asked bits belong to */
    int32_t spawns;       /* its spawn count when the bits were last set */
    uint8_t want[2];      /* bits asked for: [0] the cheat-flags byte, [1] the noclip byte */
} sh_cheat_keep;

/* After a toggle of `bit` in byte `which` (0 flags, 1 noclip) of `player`, now `on`: remember it. */
void sh_cheat_keep_note(sh_cheat_keep *k, const void *player, int32_t spawns, int which, uint8_t bit, int on);

/* Each frame with the local player's entity, spawn count and its two bytes: sets the asked bits again when the
 * same player has spawned since they were set (nonzero when a bit had to be set again). A different
 * player, or a spawn count that went down, forgets what was asked. */
uint8_t sh_cheat_keep_frame(sh_cheat_keep *k, const void *player, int32_t spawns, uint8_t *flags, uint8_t *noclip);

/* Whether anything is asked for (so the frame can skip finding the player). */
int sh_cheat_keep_active(const sh_cheat_keep *k);

#endif
