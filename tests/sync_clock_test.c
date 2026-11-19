/* sync_clock_test.c -- the pure half of the sync clock (src/backend/sync_clock_rule.c): which animators of a sync
 * run on the player's clock, and the precision of one re-anchor. */
#include <stdio.h>
#include <string.h>
#include "sync_clock.h"
#include "clip_events.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static int a_player, a_sync, a_demon, a_helper;

static void names(void)
{
    CHECK(sh_ce_decl_under_mods("mods/wuahwfly/ai/demon/tyrant"));
    CHECK(sh_ce_decl_under_mods("MODS\\x\\y"));
    CHECK(!sh_ce_decl_under_mods("ai/demon/baron"));
    CHECK(!sh_ce_decl_under_mods("mod/x"));
    CHECK(!sh_ce_decl_under_mods("mods"));
    CHECK(!sh_ce_decl_under_mods(NULL));
}

/* A ported kill: the player (instigator), the sync entity, the mod's demon (target) and a helper body: all four on
 * the player's clock. */
static void mod_kill(void)
{
    const sh_sc_participant p[] = {
        { &a_player, 0, "player/sp/player" },
        { &a_sync, 0, "mods/wuahwfly/syncmelee/tyrant" },
        { &a_demon, 1, "mods/wuahwfly/ai/demon/tyrant" },
        { &a_helper, 0, "mods/wuahwfly/syncdeath/playervsai/tyrant/front_upper_tyrant" },
    };
    const void *out[SH_SC_PARTICIPANTS_MAX];
    CHECK(sh_sc_sync_of_mod(p, 4));
    CHECK(sh_sc_player_clock(p, 4, out, SH_SC_PARTICIPANTS_MAX) == 4);
    CHECK(out[0] == &a_player && out[1] == &a_sync && out[2] == &a_demon && out[3] == &a_helper);
    CHECK(sh_sc_player_clock(p, 4, out, 2) == 2);   /* the cap holds */
    CHECK(sh_sc_player_clock(p, 4, NULL, 4) == 0);
}

/* A stock kill (a stock target) is never on the player's clock, even when another participant is a mod's (a mod
 * sync entity on a stock demon): the kill is the target's. */
static void stock_kill(void)
{
    const sh_sc_participant p[] = {
        { &a_player, 0, "player/sp/player" },
        { &a_sync, 0, "mods/x/syncmelee/baron" },
        { &a_demon, 1, "ai/demon/baron" },
    };
    const void *out[SH_SC_PARTICIPANTS_MAX];
    CHECK(!sh_sc_sync_of_mod(p, 3));
    CHECK(sh_sc_player_clock(p, 3, out, SH_SC_PARTICIPANTS_MAX) == 0);
    CHECK(!sh_sc_sync_of_mod(NULL, 0));
}

/* A participant without an animator, or whose name could not be read, is left out; one animator twice is kept once;
 * a mod's entity that is not the target does not make the kill a mod demon's. */
static void odd_rows(void)
{
    const sh_sc_participant p[] = {
        { NULL, 0, "player/sp/player" },
        { &a_demon, 1, "mods/x/ai/demon/tyrant" },
        { &a_demon, 0, NULL },
    };
    const sh_sc_participant q[] = {
        { &a_helper, 0, "mods/x/helper" },
        { &a_demon, 1, NULL },
    };
    const void *out[SH_SC_PARTICIPANTS_MAX];
    CHECK(sh_sc_player_clock(p, 3, out, SH_SC_PARTICIPANTS_MAX) == 1 && out[0] == &a_demon);
    CHECK(!sh_sc_sync_of_mod(q, 2));
}

/* The precision: the loop's own off the player's clock; on it the player's, which is precise only in mode 2. */
static void precision(void)
{
    CHECK(sh_sc_precision(1, 0, 1) == 1);
    CHECK(sh_sc_precision(0, 0, 1) == 0);
    CHECK(sh_sc_precision(1, 1, 1) == 0);   /* the mod's demon under the default mode: as the player */
    CHECK(sh_sc_precision(1, 1, 0) == 0);
    CHECK(sh_sc_precision(1, 1, 2) == 1);   /* mode 2: the player is precise too */
    CHECK(sh_sc_precision(0, 1, 2) == 1);
}

int main(void)
{
    names();
    mod_kill();
    stock_kill();
    odd_rows();
    precision();
    printf("sync_clock_test: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
