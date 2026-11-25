/* modauthor_test.c -- the mod folder an author writes and the engine names it stands for (src/backend/modauthor.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modauthor.h"

static int g_fail, g_checks;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const char *S = "abcdefgh";

static void req(const char *path, const char *want)
{
    char *r = NULL, *cause = NULL, *back;
    int ok = sh_ma_request(S, path, &r, &cause);
    g_checks++;
    if (!ok || !r || strcmp(r, want)) {
        g_fail++;
        printf("request %s: got %s (%s), want %s\n", path, r ? r : "-", cause ? cause : "", want);
    } else {
        back = sh_ma_path(S, r);
        g_checks++;
        if (!back || _stricmp(back, path)) { g_fail++; printf("path of %s: got %s, want %s\n", r, back ? back : "-", path); }
        free(back);
    }
    free(r);
    free(cause);
}

static void refused(const char *path, const char *needle)
{
    char *r = NULL, *cause = NULL;
    int ok = sh_ma_request(S, path, &r, &cause);
    g_checks++;
    if (ok || (needle && (!cause || !strstr(cause, needle)))) {
        g_fail++;
        printf("refused %s: ok=%d cause=%s (want '%s')\n", path, ok, cause ? cause : "-", needle ? needle : "");
    }
    free(r);
    free(cause);
}

static void name(const char *in, const char *want)
{
    char *e = sh_ma_engine_name(S, in, strlen(in));
    g_checks++;
    if (want ? (!e || strcmp(e, want)) : e != NULL) { g_fail++; printf("name %s: got %s, want %s\n", in, e ? e : "-", want ? want : "-"); }
    free(e);
}

static int is_game(const char *k, size_t n) { return n == 16 && !strncmp(k, "snapentity_crate", 16); }

static void text(const char *in, const char *want, sh_ma_keys *keys)
{
    size_t n = 0;
    char *t = sh_ma_text(S, in, strlen(in), sh_ma_keys_has, keys, &n);
    g_checks++;
    if (want ? (!t || strcmp(t, want) || n != strlen(want)) : t != NULL) {
        g_fail++;
        printf("text:\n  in:   %s\n  got:  %s\n  want: %s\n", in, t ? t : "(unchanged)", want ? want : "(unchanged)");
    }
    free(t);
}

int main(void)
{
    char **keys = NULL, *own_list[] = { "boss_name", "tyrant_desc" };
    size_t count = 0, n = 0;
    sh_ma_keys own = { own_list, 2 };
    char *j;
    const char *json =
        "{\"name\": \"Boss\", \"strings\": {\"en\": {\"boss_name\": \"Boss\", \"snapentity_crate\": \"Box\"}},"
        " \"animation_events\": {\"meshes/boss.md6\": {\"walk\": \"#str_boss_name\"}},"
        " \"snapmap-plus\": {\"format\": 2, \"salt\": \"abcdefgh\", \"files\": [{\"path\": \"materials/body.decl\"}]}}";

    /* the folders and the engine paths they stand for, both ways */
    req("materials/body.decl", "generated/decls/material/mods/abcdefgh/body.decl");
    req("materials/demon/skin.decl", "generated/decls/material/mods/abcdefgh/demon/skin.decl");
    req("effects/explosion.decl", "generated/decls/fx/mods/abcdefgh/explosion.decl");
    req("environments/hell_red.decl", "generated/decls/env/mods/abcdefgh/hell_red.decl");
    req("particles/sparks.decl", "generated/decls/particle/mods/abcdefgh/sparks.decl");
    req("entities/boss.decl", "generated/decls/entitydef/mods/abcdefgh/boss.decl");
    req("decls/sound/machines/titan_beam_2.decl", "generated/decls/sound/mods/abcdefgh/machines/titan_beam_2.decl");
    name("decls/sound/machines/titan_beam_2", "mods/abcdefgh/machines/titan_beam_2");
    req("sounds/play_roar.decl", "generated/decls/sound/mods/abcdefgh/abcdefgh_play_roar.decl");
    req("sounds/imp/play_roar.decl", "generated/decls/sound/mods/abcdefgh/imp/abcdefgh_play_roar.decl");
    req("sounds/boss.bnk", "sound/soundbanks/pc/abcdefgh_boss.bnk");
    req("images/rock.bimage", "generated/image/mods/abcdefgh/rock.bimage");
    req("atlases/decals/blood.bimage", "generated/decalatlas/mods/abcdefgh/blood.bimage");
    req("meshes/boss.md6.decl", "generated/decls/md6def/mods/abcdefgh/boss.md6.decl");
    req("meshes/boss/mesh.bmd6model", "generated/basemodel/md6/mods/abcdefgh/boss/mesh.bmd6model");
    req("meshes/boss/mesh.bmd6skl", "generated/skeleton/md6/mods/abcdefgh/boss/mesh.bmd6skl");
    req("meshes/rock.bmodel", "cooked/model/mods/abcdefgh/rock.bmodel");
    /* an md6 mesh's collision models at its mesh's path */
    req("collision/boss/collision/chest.bmodel", "generated/model/md6/mods/abcdefgh/boss/collision/chest.bmodel");
    req("collision/boss/collision/chest.bcm", "generated/cm/md6/mods/abcdefgh/boss/collision/chest.bcm");
    req("meshes/chunk.dmodel", "generated/discreteanimation/mods/abcdefgh/chunk.dmodel");
    req("meshes/fx/strip.pmodel", "generated/mods/abcdefgh/fx/strip.pmodel");
    req("animations/boss/walk.bmd6anim", "cooked/anim/md6/mods/abcdefgh/boss/walk.bmd6anim");
    req("movies/hud.bswf", "generated/swf/mods/abcdefgh/hud.bswf");
    req("scripts/boss.lua", "mods/abcdefgh/scripts/boss.lua");
    req("scripts/lib/brain.lua", "mods/abcdefgh/scripts/lib/brain.lua");
    req("virtual-textures/boss_sq1.mega2", "virtualtextures/mods/abcdefgh/boss_sq1.mega2");
    req("map-modules/room.decl", "maps/modules/mods/abcdefgh/room.decl");
    req("map-modules/room/_combo/world.bcm", "maps/modules/mods/abcdefgh/room/_combo/world.bcm");
    req("map-modules/room.baas_monster48", "generated/maps/modules/mods/abcdefgh/room.baas_monster48");
    req("map-modules/palettes/mega_blessed/room.bmodel", "maps/modules/palettes/mega_blessed/mods/abcdefgh/room.bmodel");
    req("movies/hud.bimage", "generated/swf/mods/abcdefgh/hud.bimage");
    req("decls/damage/stomp.decl", "generated/decls/damage/mods/abcdefgh/stomp.decl");
    req("decls/snapeditorentitydef/boss.decl", "generated/decls/snapeditorentitydef/mods/abcdefgh/boss.decl");
    req("programs/tint.decl", "generated/decls/renderprog/mods/abcdefgh/tint.decl");
    req("programs/tint_pc_vulkan.bin", "generated/renderprogs/mods/abcdefgh/tint_pc_vulkan.bin");
    req("programs/permutations/tint_0000000000000001.decl",
        "generated/renderprogs/permutations/mods/abcdefgh/tint_0000000000000001.decl");
    req("programs/spirv/tint.fspv", "generated/spirv/mods/abcdefgh/tint.fspv");
    req("programs/spirv/permutations/tint_0000000000000001.vspv",
        "generated/spirv/generated/renderprogs/permutations/mods/abcdefgh/tint_0000000000000001.vspv");
    req("native/generated/decls/entitydef/ai/conductor.decl", "generated/decls/entitydef/ai/conductor.decl");
    {
        char *r = NULL;
        CHECK(sh_ma_request(S, "Materials\\Body.DECL", &r, NULL) && r && !strcmp(r, "generated/decls/material/mods/abcdefgh/body.decl"));
        free(r);
        r = NULL;
        CHECK(!sh_ma_request(S, "readme.txt", &r, NULL) && !r);   /* beside mod.json: not the mod's */
    }
    refused("decls/material/body.decl", "goes in materials/");
    refused("decls/env/hell_red.decl", "goes in environments/");
    refused("material/body.decl", "is not one of a mod's folders");
    refused("assets/generated/decls/material/x.decl", "is not one of a mod's folders");
    refused("materials/body.txt", "holds .decl files");
    refused("decls/sound/abcdefgh_hit.decl", "own sound event belongs in sounds/");
    refused("sounds/music/boss.bnk", "goes in sounds/ itself");
    refused("native/generated/decls/material/mods/abcdefgh/x.decl", "native/ holds DOOM's own files the mod changes");
    /* a closure pointer to one of the product's own decls that names a mod (an SDK effect's emitter) is not a mod's */
    req("native/generated/decls/entitydef/snapmapplus/effect/mods/abcdefgh/blast.decl",
        "generated/decls/entitydef/snapmapplus/effect/mods/abcdefgh/blast.decl");
    refused("native/generated/decls/material/mods/abcdefgh/snapmapplus/x.decl", "native/ holds DOOM's own files the mod changes");
    refused("atlases/x.bimage", "decals/, lights/ and transparency/");

    /* the names a text writes */
    name("materials/body", "mods/abcdefgh/body");
    name("images/rock.tga", "mods/abcdefgh/rock.tga");
    name("meshes/boss.md6", "mods/abcdefgh/boss.md6");
    name("meshes/boss/mesh.md6mesh", "md6/mods/abcdefgh/boss/mesh.md6mesh");
    name("meshes/boss/mesh.md6skl", "md6/mods/abcdefgh/boss/mesh.md6skl");
    name("meshes/rock.lwo", "mods/abcdefgh/rock.lwo");
    name("animations/boss/walk.md6anim", "md6/mods/abcdefgh/boss/walk.md6anim");
    name("animations/boss/stagger/front_into", "md6/mods/abcdefgh/boss/stagger/front_into");
    name("movies/hud.swf", "swf/mods/abcdefgh/hud.swf");
    name("decls/damage/stomp", "mods/abcdefgh/stomp");
    name("programs/tint", "mods/abcdefgh/tint");
    name("environments/hell_red", "mods/abcdefgh/hell_red");
    name("native:effects/explosion", "effects/explosion");
    name("art/decals/x", NULL);
    name("models/monsters/imp/imp", NULL);
    name("materials/", NULL);
    name("decls/damage", NULL);

    /* a whole text */
    text("{\n\tdiffusemap\timages/rock.tga\n\tmodel \"meshes/rock.lwo\"\n}\n",
         "{\n\tdiffusemap\tmods/abcdefgh/rock.tga\n\tmodel \"mods/abcdefgh/rock.lwo\"\n}\n", &own);
    text("x \"native:effects/burn\" y art/decals/a.tga\n", "x \"effects/burn\" y art/decals/a.tga\n", &own);
    text("name = \"#str_boss_name\"; other = \"#str_snapentity_crate\";",
         "name = \"#str_abcdefgh_boss_name\"; other = \"#str_snapentity_crate\";", &own);
    text("textures/materials/x materials/x_y", "textures/materials/x mods/abcdefgh/x_y", &own);
    text("addnormals(images/a.tga, images/b.tga)", "addnormals(mods/abcdefgh/a.tga, mods/abcdefgh/b.tga)", &own);
    text("// see materials/body\n", "// see mods/abcdefgh/body\n", &own);
    /* native("...") is the Graphics SDK's other spelling of a DOOM name: left as written */
    text("sound native(\"effects/boom\") use effects/x", "sound native(\"effects/boom\") use mods/abcdefgh/x", &own);
    text("sound native( \"effects/boom\")", NULL, &own);
    text("nothing of the mod here: mods/abcdefgh/body stays", NULL, &own);

    /* mod.json's author part */
    CHECK(sh_ma_string_keys(json, strlen(json), is_game, &keys, &count) && count == 1 && !strcmp(keys[0], "boss_name"));
    sh_ma_keys_free(keys, count);
    j = sh_ma_json(S, json, strlen(json), is_game, &n);
    CHECK(j && strstr(j, "\"abcdefgh_boss_name\": \"Boss\"") && strstr(j, "\"snapentity_crate\": \"Box\""));
    CHECK(j && strstr(j, "\"mods/abcdefgh/boss.md6\"") && strstr(j, "\"#str_abcdefgh_boss_name\""));
    CHECK(j && strstr(j, "\"path\": \"materials/body.decl\""));   /* the builder's own part is never rewritten */
    CHECK(j && n == strlen(j));
    free(j);

    printf("modauthor_test\n%s -- %d checks, %d failed\n", g_fail ? "FAILED" : "PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
