/* modhud_scene_test.c -- the scene HUD decision and its signature entry. */
#include <stdio.h>
#include <string.h>
#include "../src/backend/modhud_scene.h"
#include "../src/backend/signatures.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(void)
{
    size_t i, n = 0;
    const sig_entry *dispatch = NULL;

    /* Only the two HUD events act, and only during SnapMap Play. */
    CHECK(sh_modhud_scene_decide(SH_MODHUD_SCENE_EV_HIDE, 1) == 0);
    CHECK(sh_modhud_scene_decide(SH_MODHUD_SCENE_EV_SHOW, 1) == 1);
    CHECK(sh_modhud_scene_decide(SH_MODHUD_SCENE_EV_HIDE, 0) == -1);
    CHECK(sh_modhud_scene_decide(SH_MODHUD_SCENE_EV_HIDE, -1) == -1);
    CHECK(sh_modhud_scene_decide(SH_MODHUD_SCENE_EV_SHOW, 0) == -1);
    CHECK(sh_modhud_scene_decide(0x122, 1) == -1);
    CHECK(sh_modhud_scene_decide(0x125, 1) == -1);
    CHECK(sh_modhud_scene_decide(-1, 1) == -1);

    /* The dispatcher signature names both renderers and starts with the shared prologue. */
    for (i = 0; BACKEND_ENGINE_SIGNATURES[i].name; i++) {
        n++;
        if (!strcmp(BACKEND_ENGINE_SIGNATURES[i].name, "EventDispatchBase")) dispatch = &BACKEND_ENGINE_SIGNATURES[i];
    }
    CHECK(n > 0);
    CHECK(dispatch != NULL);
    if (dispatch) {
        CHECK(dispatch->rva_vk == 0x14A5950u);
        CHECK(dispatch->rva_gl == 0x14A5AF0u);
        CHECK(!strncmp(dispatch->pattern, "40 55 53 56 57 41 56 41 57 48 8D 6C 24 D1 48 81", 47));
    }
    printf(fails ? "modhud_scene_test: %d failure(s)\n" : "modhud_scene_test: ok\n", fails);
    return fails ? 1 : 0;
}
