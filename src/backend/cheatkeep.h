/* Player cheats kept over the player's own spawn. A SnapMap Play spawns the player after gameplay frames start
 * (VK FUN_140857EC0 -> FUN_140DE2290), zeroing the cheat byte and incrementing the spawn count (+0x27D70); the keep
 * sets a toggled cheat's bits again when the count changes, and a different player entity starts with nothing asked.
 * The keep ends with its Play: returning from Play (idSnapEditorLocal vtable +0x18, VK FUN_141189190, GL
 * FUN_1411882f0) runs "disconnect", which frees the player and then sets the editor's active flag (+9), and
 * CancelMapChange calls sh_cheat_keep_end at entry, so sh_cheat_keep_poll reads the flag first. Pure. */
#ifndef SH_CHEATKEEP_H
#define SH_CHEATKEEP_H

#include <stdint.h>

typedef struct sh_cheat_keep {
    const void *player;   /* the entity the asked bits belong to */
    int32_t spawns;       /* its spawn count at the last set */
    uint8_t want[2];      /* bits asked for: [0] the cheat-flags byte, [1] the noclip byte */
} sh_cheat_keep;

/* Records a toggle of `bit` in byte `which` (0 flags, 1 noclip) of `player`, to state `on`. */
void sh_cheat_keep_note(sh_cheat_keep *k, const void *player, int32_t spawns, int which, uint8_t bit, int on);

/* Each frame, with the local player's entity, spawn count and two bytes: sets the asked bits again when the same
 * player has respawned since the last set and returns the bits set (nonzero when one had to be). A different
 * player, or a lower spawn count, clears the request. */
uint8_t sh_cheat_keep_frame(sh_cheat_keep *k, const void *player, int32_t spawns, uint8_t *flags, uint8_t *noclip);

/* Returns whether anything is asked for, so the frame can skip finding the player. */
int sh_cheat_keep_active(const sh_cheat_keep *k);

/* Ends the keep: forgets the player and the bits asked for. */
void sh_cheat_keep_end(sh_cheat_keep *k);

/* Between-frames check, outside the game's frame; in_play is the editor's flag (1 Play on, 0 editor active, -1
 * unknown). It reads the held player's spawn count through spawns_of (may fault, returns -1) only during a Play,
 * else ends the keep; returns 1 when the count differs from the stored one, so the caller queues in-frame work. */
typedef int32_t (*sh_cheat_keep_spawns_fn)(const void *player);
int sh_cheat_keep_poll(sh_cheat_keep *k, int in_play, sh_cheat_keep_spawns_fn spawns_of);

/* idPlayer's cheat-flags byte and its noTarget bit. */
#define SH_PLAYER_CHEAT_FLAGS_OFF 0x45ea8u
#define SH_PLAYER_CHEAT_NOTARGET  0x20u

/* The engine's target queries (VK FUN_1407E7670 and FUN_1409CFBC0) refuse a player whose noTarget bit is set
 * (player+0x45EA8 & 0x20), while the AI's enemy field may still hold that player from the encounter's spawn; a
 * script reading the enemy field must treat that player as no enemy (ai:enemy, script_api_ai.c). */
static __inline int sh_player_flags_no_target(uint8_t flags) { return (flags & SH_PLAYER_CHEAT_NOTARGET) != 0; }

#endif
