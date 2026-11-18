/* Player cheats kept over the player's own spawn (src/backend/cheatkeep.h). */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/backend/cheatkeep.h"

static int player_a, player_b;

/* The fault: noTarget turned on while Play starts; the game mode's spawn rewrites the byte to 0 and counts a spawn. */
static void test_a_spawn_does_not_undo_a_cheat(void)
{
    sh_cheat_keep k; uint8_t flags = 0x01, noclip = 0;
    memset(&k, 0, sizeof k);
    flags |= 0x20;                                     /* noTarget ON on spawn count 1 */
    sh_cheat_keep_note(&k, &player_a, 1, 0, 0x20, 1);
    assert(sh_cheat_keep_active(&k));
    assert(sh_cheat_keep_frame(&k, &player_a, 1, &flags, &noclip) == 0 && flags == 0x21);
    flags = 0;                                         /* the spawn: byte rewritten, count 2 */
    assert(sh_cheat_keep_frame(&k, &player_a, 2, &flags, &noclip) != 0);
    assert(flags == 0x20 && noclip == 0);
    assert(sh_cheat_keep_frame(&k, &player_a, 2, &flags, &noclip) == 0);
}

/* Between spawns the engine's own writes stand (the suit's revive clears noTarget). */
static void test_engine_writes_between_spawns_stand(void)
{
    sh_cheat_keep k; uint8_t flags = 0x20, noclip = 0;
    memset(&k, 0, sizeof k);
    sh_cheat_keep_note(&k, &player_a, 2, 0, 0x20, 1);
    flags &= (uint8_t)~0x20;
    assert(sh_cheat_keep_frame(&k, &player_a, 2, &flags, &noclip) == 0 && flags == 0);
}

/* Turned off: nothing is set again. Several bits and the noclip byte are kept apart. */
static void test_off_and_two_bytes(void)
{
    sh_cheat_keep k; uint8_t flags = 0, noclip = 0;
    memset(&k, 0, sizeof k);
    sh_cheat_keep_note(&k, &player_a, 1, 0, 0x20, 1);
    sh_cheat_keep_note(&k, &player_a, 1, 0, 0x04, 1);
    sh_cheat_keep_note(&k, &player_a, 1, 1, 0x04, 1);
    sh_cheat_keep_note(&k, &player_a, 1, 0, 0x20, 0);
    assert(sh_cheat_keep_frame(&k, &player_a, 2, &flags, &noclip) != 0);
    assert(flags == 0x04 && noclip == 0x04);
    sh_cheat_keep_note(&k, &player_a, 2, 0, 0x04, 0);
    sh_cheat_keep_note(&k, &player_a, 2, 1, 0x04, 0);
    assert(!sh_cheat_keep_active(&k));
    flags = noclip = 0;
    assert(sh_cheat_keep_frame(&k, &player_a, 3, &flags, &noclip) == 0 && flags == 0 && noclip == 0);
}

/* The next Play's player starts with nothing asked for, and so does a count that went down. */
static void test_another_player_forgets(void)
{
    sh_cheat_keep k; uint8_t flags = 0, noclip = 0;
    memset(&k, 0, sizeof k);
    sh_cheat_keep_note(&k, &player_a, 2, 0, 0x20, 1);
    assert(sh_cheat_keep_frame(&k, &player_b, 1, &flags, &noclip) == 0 && flags == 0);
    assert(!sh_cheat_keep_active(&k));
    sh_cheat_keep_note(&k, &player_a, 3, 0, 0x20, 1);
    assert(sh_cheat_keep_frame(&k, &player_a, 1, &flags, &noclip) == 0 && flags == 0);
    assert(!sh_cheat_keep_active(&k));
    sh_cheat_keep_note(&k, &player_a, 1, 0, 0x20, 1);
    sh_cheat_keep_note(&k, &player_b, 1, 0, 0x04, 1);   /* a note on another player starts over */
    assert(k.want[0] == 0x04 && k.player == &player_b);
}

/* DOOM's target queries skip a player whose noTarget bit is set; ai:enemy gives no enemy then. */
static void test_no_target_flag(void)
{
    assert(sh_player_flags_no_target(0x20) && sh_player_flags_no_target(0x25));
    assert(!sh_player_flags_no_target(0x00) && !sh_player_flags_no_target(0x1f) && !sh_player_flags_no_target(0xdf));
}

int main(void)
{
    test_no_target_flag();
    test_a_spawn_does_not_undo_a_cheat();
    test_engine_writes_between_spawns_stand();
    test_off_and_two_bytes();
    test_another_player_forgets();
    printf("cheatkeep_test: all passed\n");
    return 0;
}
