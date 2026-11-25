/* Native Toybox placement can have an inherit before it has resolved decl text. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/backend/editor_placed_names.h"

static const char *entity = "mods/s4msvbiz/interact_chaingun_pickup", *room = "mods/testroom/module", *fx = "mods/testfx/glow";
static int entities, rooms, effects;

static void collect(void *ctx, const char *s)
{
    (void)ctx;
    entities += !strcmp(s, entity);
    rooms += !strcmp(s, room);
    effects += !strcmp(s, fx);
}

int main(void)
{
    static uint8_t map[LM_INSTANCES_CNT_OFF + 8];
    uint8_t ent[ENT_DEFSUB_OFF + 8] = {0}, def[DEFSUB_INHERIT_OFF + 8] = {0};
    static char long_decl[18000];
    uint8_t record[MOD_STRIDE] = {0}, module[MOD_NAME_OFF + 8] = {0};
    void *entities_by_id[] = { NULL, ent };
    *(void **)(map + ARR_ENT_ARRAY_OFF) = entities_by_id;
    *(uint32_t *)(map + ARR_ENT_COUNT_OFF) = 2;
    *(void **)(map + LM_MODTABLE_OFF) = record;
    *(int *)(map + LM_INSTANCES_CNT_OFF) = 1;
    *(void **)record = module;
    *(const char **)(module + MOD_NAME_OFF) = room;
    record[0x30] = 1;
    *(void **)(ent + ENT_VALID_OFF) = ent;
    *(void **)(ent + ENT_DEFSUB_OFF) = def;
    *(const char **)(def + DEFSUB_INHERIT_OFF) = entity;
    assert(sh_editor_placed_names(map, collect, NULL) == 1 && entities == 1 && rooms == 1 && !effects);
    *(const char **)(def + DECL_BLOB_B_OFF) = "effect = \"mods/testfx/glow\";";
    assert(sh_editor_placed_names(map, collect, NULL) == 1 && entities == 2 && rooms == 2 && effects == 1);
    memset(long_decl, ' ', sizeof long_decl - 50);
    strcpy_s(long_decl + sizeof long_decl - 50, 50, "effect = \"mods/testfx/glow\";");
    *(const char **)(def + DECL_BLOB_B_OFF) = long_decl;
    assert(sh_editor_placed_names(map, collect, NULL) == 1 && entities == 3 && rooms == 3 && effects == 2);
    record[0x30] = 0;
    *(void **)(ent + ENT_VALID_OFF) = NULL;
    assert(sh_editor_placed_names(map, collect, NULL) == 0 && entities == 3 && rooms == 3 && effects == 2);
    *(void **)(ent + ENT_VALID_OFF) = ent;
    *(const char **)(def + DEFSUB_INHERIT_OFF) = (const char *)1;
    assert(sh_editor_placed_names(map, collect, NULL) == -1);
    puts("editor_placed_names_test: inherited objects, nested edits, modules, deletion and failed reads passed");
    return 0;
}
