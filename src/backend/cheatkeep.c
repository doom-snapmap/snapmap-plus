/* Player cheats kept over the player's own spawn: see cheatkeep.h. */
#include "cheatkeep.h"

#include <string.h>

void sh_cheat_keep_note(sh_cheat_keep *k, const void *player, int32_t spawns, int which, uint8_t bit, int on)
{
    if (!k || !player || which < 0 || which > 1) return;
    if (k->player != player) memset(k, 0, sizeof *k);
    k->player = player;
    k->spawns = spawns;
    if (on) k->want[which] |= bit;
    else k->want[which] &= (uint8_t)~bit;
}

int sh_cheat_keep_active(const sh_cheat_keep *k)
{
    return k && k->player && (k->want[0] || k->want[1]);
}

void sh_cheat_keep_end(sh_cheat_keep *k)
{
    if (k) memset(k, 0, sizeof *k);
}

int sh_cheat_keep_poll(sh_cheat_keep *k, int in_play, sh_cheat_keep_spawns_fn spawns_of)
{
    if (!sh_cheat_keep_active(k) || !spawns_of) return 0;
    if (in_play != 1) {                                 /* the Play is over, or cannot be told: read nothing */
        sh_cheat_keep_end(k);
        return 0;
    }
    return spawns_of(k->player) != k->spawns;
}

uint8_t sh_cheat_keep_frame(sh_cheat_keep *k, const void *player, int32_t spawns, uint8_t *flags, uint8_t *noclip)
{
    uint8_t set = 0;
    if (!sh_cheat_keep_active(k)) return 0;
    if (player != k->player || spawns < k->spawns) {   /* another player: the next Play asks again */
        memset(k, 0, sizeof *k);
        return 0;
    }
    if (spawns == k->spawns) return 0;                  /* no spawn since: the engine's own writes stand */
    k->spawns = spawns;
    if (flags && k->want[0]) { set |= (uint8_t)(k->want[0] & ~*flags); *flags |= k->want[0]; }
    if (noclip && k->want[1]) { set |= (uint8_t)(k->want[1] & ~*noclip); *noclip |= k->want[1]; }
    return set;
}
