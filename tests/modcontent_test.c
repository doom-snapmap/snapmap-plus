/* modcontent_test.c -- the build-time check of a mod's "scenes" and "hud"
 * blocks (modcontent.c): what the game's readers accept passes, and each
 * refusal is one sentence naming the block in mod.json. */
#include <stdio.h>
#include <string.h>
#include "modcontent.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static int check(const char *extra, char *cause, size_t cap)
{
    return sh_modcontent_check("alexrimp", extra, strlen(extra), cause, cap);
}

int main(void)
{
    char cause[512];
    printf("-- content blocks: scenes and hud read at build as the game reads them\n");
    CHECK(check("{}", cause, sizeof cause) && !cause[0]);
    CHECK(check("{\"strings\":{\"en\":{\"alexrimp_x\":\"X\"}}}", cause, sizeof cause));
    /* Scenes. */
    CHECK(check("{\"scenes\":{\"mods/alexrimp/node\":{\"plays\":\"at_node\",\"spawn\":[{\"entityDef\":\"mods/alexrimp/imp\"}]}}}",
                cause, sizeof cause));
    CHECK(!check("{\"scenes\":{\"mods/someoneelse/node\":{}}}", cause, sizeof cause) &&
          strstr(cause, "mod.json \"scenes\" is refused: ") && strstr(cause, "mods/someoneelse/node") &&
          strstr(cause, "press Refresh"));
    printf("   %s\n", cause);
    CHECK(!check("{\"scenes\":{\"mods/alexrimp/node\":{\"plays\":\"sideways\"}}}", cause, sizeof cause) &&
          strstr(cause, "\"scenes\""));
    printf("   %s\n", cause);
    CHECK(!check("{\"scenes\":[1]}", cause, sizeof cause) && strstr(cause, "\"scenes\""));
    /* Hud. */
    CHECK(check("{\"hud\":{\"weapons\":{\"weapon/player/pistol\":{\"ammo_display\":\"weapon\"}}}}", cause, sizeof cause));
    CHECK(check("{\"hud\":{}}", cause, sizeof cause));
    CHECK(!check("{\"hud\":{\"weapons\":{\"weapon/player/pistol\":{\"ammo_display\":\"numbers\"}}}}", cause, sizeof cause) &&
          strstr(cause, "mod.json \"hud\" gives the weapon weapon/player/pistol") && strstr(cause, "press Refresh"));
    printf("   %s\n", cause);
    CHECK(!check("{\"hud\":{\"weapons\":{\"Weapon/Pistol\":{\"ammo_display\":\"weapon\"}}}}", cause, sizeof cause) &&
          strstr(cause, "not a weapon decl name"));
    CHECK(!check("{\"hud\":{\"schema\":1,\"weapons\":{}}}", cause, sizeof cause) && strstr(cause, "only \"weapons\""));
    CHECK(!check("{\"hud\":[]}", cause, sizeof cause) && strstr(cause, "must be an object"));
    printf(g_fail ? "modcontent_test: FAILED\n" : "modcontent_test: scenes and hud checks pass\n");
    return g_fail;
}
