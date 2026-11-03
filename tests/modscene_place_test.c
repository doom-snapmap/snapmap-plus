/* modscene_place_test.c -- scene-node placement decisions, the record table and
 * the three signature entries. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../src/backend/modscene_place.h"
#include "../src/backend/signatures.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int close_to(float a, float b) { return fabsf(a - b) < 1e-5f; }

static const sig_entry *sig(const char *name)
{
    size_t i;
    for (i = 0; BACKEND_ENGINE_SIGNATURES[i].name; i++)
        if (!strcmp(BACKEND_ENGINE_SIGNATURES[i].name, name)) return &BACKEND_ENGINE_SIGNATURES[i];
    return NULL;
}

static void check_sig(const char *name, unsigned vk, unsigned gl, const char *prologue)
{
    const sig_entry *s = sig(name);
    CHECK(s != NULL);
    if (!s) return;
    CHECK(s->rva_vk == vk);
    CHECK(s->rva_gl == gl);
    CHECK(!strncmp(s->pattern, prologue, strlen(prologue)));
}

int main(void)
{
    float axis[9], o[3] = { -548.0f, -2544.0f, 0.25f }, a[3] = { 10.0f, 90.0f, 0.0f };
    sh_modscene_place_table t;
    sh_modscene_place_record r;
    int i, dummy[20];

    /* Only a mod's scene_play row is a scene row. */
    CHECK(sh_modscene_place_is_scene_row("mods/campaign-interactables-2/actions/scene_play"));
    CHECK(sh_modscene_place_is_scene_row("mods/x/actions/scene_play"));
    CHECK(!sh_modscene_place_is_scene_row("snapmaps/action/door/open"));
    CHECK(!sh_modscene_place_is_scene_row("mods/campaign-interactables/actions/crucible_activate"));
    CHECK(!sh_modscene_place_is_scene_row("snapmaps/actions/scene_play"));
    CHECK(!sh_modscene_place_is_scene_row("mods/actions/scene_play"));
    CHECK(!sh_modscene_place_is_scene_row("mods//actions/scene_play_x"));
    CHECK(!sh_modscene_place_is_scene_row("mods/x/actions/scene_play/extra"));
    CHECK(!sh_modscene_place_is_scene_row(""));
    CHECK(!sh_modscene_place_is_scene_row(NULL));

    /* Outside Play and for other rows the engine runs unchanged; a scene row
     * in Play places for a player and is dropped for anyone else. */
    CHECK(sh_modscene_place_decide(1, 1, 1) == SH_MODSCENE_PLACE_PLACE);
    CHECK(sh_modscene_place_decide(1, 1, 0) == SH_MODSCENE_PLACE_DROP);
    CHECK(sh_modscene_place_decide(1, 0, 1) == SH_MODSCENE_PLACE_PASS);
    CHECK(sh_modscene_place_decide(1, 0, 0) == SH_MODSCENE_PLACE_PASS);
    CHECK(sh_modscene_place_decide(0, 1, 1) == SH_MODSCENE_PLACE_PASS);
    CHECK(sh_modscene_place_decide(0, 1, 0) == SH_MODSCENE_PLACE_PASS);
    CHECK(sh_modscene_place_decide(-1, 1, 0) == SH_MODSCENE_PLACE_PASS);

    /* Yaw-only axis, rows forward / left / up. */
    sh_modscene_place_yaw_axis(0.0f, axis);
    CHECK(close_to(axis[0], 1) && close_to(axis[1], 0) && close_to(axis[2], 0));
    CHECK(close_to(axis[3], 0) && close_to(axis[4], 1) && close_to(axis[5], 0));
    CHECK(close_to(axis[6], 0) && close_to(axis[7], 0) && close_to(axis[8], 1));
    sh_modscene_place_yaw_axis(90.0f, axis);
    CHECK(close_to(axis[0], 0) && close_to(axis[1], 1) && close_to(axis[2], 0));
    CHECK(close_to(axis[3], -1) && close_to(axis[4], 0) && close_to(axis[5], 0));
    sh_modscene_place_yaw_axis(-135.0f, axis);
    CHECK(close_to(axis[0], -0.70710678f) && close_to(axis[1], -0.70710678f));
    CHECK(close_to(axis[3], 0.70710678f) && close_to(axis[4], -0.70710678f));
    CHECK(close_to(axis[8], 1));

    /* Pose validation. */
    CHECK(sh_modscene_place_pose_ok(o, a));
    {
        volatile float zero = 0.0f;
        float bad[3] = { 0.0f, 0.0f, 0.0f }, nan_v = zero / zero;
        bad[1] = nan_v;
        CHECK(!sh_modscene_place_pose_ok(bad, a));
        CHECK(!sh_modscene_place_pose_ok(o, bad));
        bad[1] = 2.0e6f;
        CHECK(!sh_modscene_place_pose_ok(bad, a));
        CHECK(!sh_modscene_place_pose_ok(NULL, a));
    }

    /* The record table: keyed by node, replaced per node, oldest evicted. */
    sh_modscene_place_table_clear(&t);
    CHECK(!sh_modscene_place_take(&t, &dummy[0], &r));
    CHECK(!sh_modscene_place_remember(&t, NULL, "player1", o, a));
    CHECK(!sh_modscene_place_remember(&t, &dummy[0], "", o, a));
    CHECK(!sh_modscene_place_remember(&t, &dummy[0],
          "a-player-name-that-is-much-longer-than-the-sixty-three-bytes-the-row-keeps", o, a));
    CHECK(sh_modscene_place_remember(&t, &dummy[0], "player1", o, a));
    a[1] = 45.0f;
    CHECK(sh_modscene_place_remember(&t, &dummy[0], "player2", o, a));
    CHECK(sh_modscene_place_take(&t, &dummy[0], &r));
    CHECK(!strcmp(r.player, "player2") && close_to(r.angles[1], 45.0f) && close_to(r.origin[0], -548.0f));
    CHECK(!sh_modscene_place_take(&t, &dummy[0], &r));
    for (i = 0; i < SH_MODSCENE_PLACE_RECORDS + 1; i++)
        CHECK(sh_modscene_place_remember(&t, &dummy[i], "player1", o, a));
    CHECK(!sh_modscene_place_take(&t, &dummy[0], &r));   /* the oldest went */
    CHECK(sh_modscene_place_take(&t, &dummy[1], &r));
    CHECK(sh_modscene_place_take(&t, &dummy[SH_MODSCENE_PLACE_RECORDS], &r));
    sh_modscene_place_table_clear(&t);
    CHECK(!sh_modscene_place_take(&t, &dummy[2], &r));

    /* The three detour targets name both renderers and start with the
     * prologues whose stolen windows the backend replays. */
    check_sig("SceneInteractionDoAction", 0x9DCCC0u, 0x9DBFB0u,
              "4C 8B DC 55 57 41 56 48 83 EC 60 8B 81 F8 03 00 00");
    check_sig("InteractableUsedOutput", 0x91EA30u, 0x91DB40u,
              "48 89 5C 24 08 57 48 83 EC 20 48 8D 99 00 52 00 00");
    check_sig("InteractionOnActionRequest", 0x6BEE80u, 0x6BE150u,
              "48 89 6C 24 18 56 41 56 41 57 48 83 EC 30");
    CHECK(sig_db_count() <= SIG_RESULTS_MAX);

    printf(fails ? "modscene_place_test: %d failure(s)\n" : "modscene_place_test: ok\n", fails);
    return fails ? 1 : 0;
}
