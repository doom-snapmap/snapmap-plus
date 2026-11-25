/* Native Toybox placement can have an inherit before it has resolved decl text. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/backend/editor_placed_names.h"
#include "../src/backend/editor_undo.h"

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
    static uint8_t map[LM_INSTANCES_CNT_OFF + 8], stock[sizeof map], editor[0x204c8 + 8], history[0x20];
    void *snapshots[] = { stock, map };
    uint8_t ent[ENT_DEFSUB_OFF + 8] = {0}, def[DEFSUB_INHERIT_OFF + 8] = {0};
    static char long_decl[18000];
    uint8_t record[MOD_STRIDE] = {0}, module[MOD_NAME_OFF + 8] = {0};
    void *entities_by_id[] = { NULL, ent };
    *(void **)(editor + 0x204c8) = map;
    *(void **)(editor + SH_EDITOR_UNDO_LIST_OFF) = history;
    *(void ***)history = snapshots;
    *(int *)(history + SH_EDITOR_UNDO_COUNT_OFF) = 2;
    *(int *)(history + SH_EDITOR_UNDO_CAP_OFF) = 2;
    *(void ***)(history + SH_EDITOR_UNDO_CURSOR_OFF) = &snapshots[0];
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
    /* The live map contains the picker entity and room. Cursor changes and cancel leave the stock snapshot selected. */
    assert(sh_editor_placed_names(editor, collect, NULL) == 0 && entities == 0 && rooms == 0);
    *(const char **)(def + DEFSUB_INHERIT_OFF) = fx;
    assert(sh_editor_placed_names(editor, collect, NULL) == 0 && effects == 0);
    *(void **)(editor + 0x204c8) = stock;
    assert(sh_editor_placed_names(editor, collect, NULL) == 0 && entities == 0 && rooms == 0);
    /* Space commits this map, then the picker can immediately create and retype another preview. */
    *(const char **)(def + DEFSUB_INHERIT_OFF) = entity;
    *(void ***)(history + SH_EDITOR_UNDO_CURSOR_OFF) = &snapshots[1];
    assert(sh_editor_placed_names(editor, collect, NULL) == 1 && entities == 1 && rooms == 1 && !effects);
    *(void ***)(history + SH_EDITOR_UNDO_CURSOR_OFF) = &snapshots[0];  /* Undo */
    assert(sh_editor_placed_names(editor, collect, NULL) == 0 && entities == 1 && rooms == 1);
    *(void ***)(history + SH_EDITOR_UNDO_CURSOR_OFF) = &snapshots[1];  /* Redo */
    *(const char **)(def + DECL_BLOB_B_OFF) = "effect = \"mods/testfx/glow\";";
    assert(sh_editor_placed_names(editor, collect, NULL) == 1 && entities == 2 && rooms == 2 && effects == 1);
    memset(long_decl, ' ', sizeof long_decl - 50);
    strcpy_s(long_decl + sizeof long_decl - 50, 50, "effect = \"mods/testfx/glow\";");
    *(const char **)(def + DECL_BLOB_B_OFF) = long_decl;
    assert(sh_editor_placed_names(editor, collect, NULL) == 1 && entities == 3 && rooms == 3 && effects == 2);
    record[0x30] = 0;
    *(void **)(ent + ENT_VALID_OFF) = NULL;
    assert(sh_editor_placed_names(editor, collect, NULL) == 0 && entities == 3 && rooms == 3 && effects == 2);
    *(void **)(ent + ENT_VALID_OFF) = ent;
    *(const char **)(def + DEFSUB_INHERIT_OFF) = (const char *)1;
    assert(sh_editor_placed_names(editor, collect, NULL) == -1);
    *(void ***)(history + SH_EDITOR_UNDO_CURSOR_OFF) = &snapshots[2];
    assert(sh_editor_placed_names(editor, collect, NULL) == -1);
    *(int *)(history + SH_EDITOR_UNDO_COUNT_OFF) = 0;
    assert(sh_editor_placed_names(editor, collect, NULL) == -1);
    puts("editor_placed_names_test: native previews, commits, undo, redo, long decls and failed reads passed");
    return 0;
}
