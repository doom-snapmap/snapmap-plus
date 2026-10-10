#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/decl_dependencies.h"

typedef struct fixture {
    char references[4096];
    size_t gaps;
    int refuse;
    int no_anim;
} fixture;

static int describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    if (type.ops && !strcmp(type.ops, SH_DECL_EVENT_DECL_OPS)) {
        /* Event decl arguments name a manager; only proven ones bind. */
        if (!strcmp(type.name, "sound")) shape->element.name = "idSoundShader";
        else if (!strcmp(type.name, "table")) shape->element.name = "idDeclTable";
        else if (!strcmp(type.name, "excluded")) { shape->kind = SH_DECL_VALUE_IGNORE; return 1; }
        else return 0;
        shape->kind = SH_DECL_VALUE_REFERENCE; shape->element.ops = "*";
        return 1;
    }
    if (type.ops && !strcmp(type.ops, SH_DECL_EVENT_ANIM_OPS)) {
        /* A schema that proves the animation list binds idMD6Anim. */
        if (!((fixture *)context)->no_anim && !strcmp(type.name, SH_DECL_EVENT_ANIM_TYPE)) {
            shape->kind = SH_DECL_VALUE_REFERENCE; shape->element.name = type.name; shape->element.ops = "*";
            return 1;
        }
        return 0;
    }
    if (!strcmp(type.name, "TestEventCall")) shape->kind = SH_DECL_VALUE_EVENT_CALL;
    else if (!strcmp(type.name, "idDeclEntityDef") || !strcmp(type.name, "idDeclInventory") ||
        !strcmp(type.name, "idRenderModel")) {
        if (type.ops && !strcmp(type.ops, "*")) shape->kind = SH_DECL_VALUE_REFERENCE;
    } else if (!strcmp(type.name, "TestEntity") || !strcmp(type.name, "TestModelInfo") ||
               !strcmp(type.name, "TestSlot")) shape->kind = SH_DECL_VALUE_OBJECT;
    else if (!strcmp(type.name, "idStr") || !strcmp(type.name, "int")) shape->kind = SH_DECL_VALUE_IGNORE;
    else if (!strcmp(type.name, "TestSlotList")) {
        shape->kind = SH_DECL_VALUE_COLLECTION;
        shape->element.name = "TestSlot"; shape->element.ops = "";
        shape->item_key = "item"; shape->count_key = "num";
    } else if (!strcmp(type.name, "TestFixedRefs")) {
        shape->kind = SH_DECL_VALUE_COLLECTION;
        shape->element.name = "idDeclEntityDef"; shape->element.ops = "*";
        shape->count = 3; shape->count_known = 1;
    } else if (!strcmp(type.name, "TestSlotFixed")) {
        shape->kind = SH_DECL_VALUE_COLLECTION;
        shape->element.name = "TestSlot"; shape->element.ops = "";
        shape->count = 2; shape->count_known = 1;
    } else if (!strcmp(type.name, "TestArrayList")) {
        shape->kind = SH_DECL_VALUE_COLLECTION;
        shape->element.name = "TestFixedRefs"; shape->element.ops = "";
        shape->item_key = "item"; shape->count_key = "num";
    }
    return 1;
}

static int field(void *context, sh_decl_value_type owner, const char *key, sh_decl_value_type *out)
{
    (void)context;
    out->ops = "";
    if (!strcmp(owner.name, "TestEntity")) {
        if (!strcmp(key, "inventoryItemDecl")) { out->name = "idDeclInventory"; out->ops = "*"; }
        else if (!strcmp(key, "renderModelInfo")) out->name = "TestModelInfo";
        else if (!strcmp(key, "startingInventory")) out->name = "TestSlotList";
        else if (!strcmp(key, "name")) out->name = "idStr";
        else if (!strcmp(key, "links")) out->name = "TestFixedRefs";
        else if (!strcmp(key, "slots")) out->name = "TestSlotFixed";
        else if (!strcmp(key, "arrays")) out->name = "TestArrayList";
        else if (!strcmp(key, "eventCall")) out->name = "TestEventCall";
        else if (!strcmp(key, "custom")) out->name = "UnsupportedTemplate";
        else if (!strcmp(key, "unknownProperty")) return -1;
        else return 0;
    } else if (!strcmp(owner.name, "TestModelInfo")) {
        if (!strcmp(key, "model")) { out->name = "idRenderModel"; out->ops = "*"; }
        else if (!strcmp(key, "editorModel")) out->name = "idStr";
        else return 0;
    } else if (!strcmp(owner.name, "TestSlot")) {
        if (!strcmp(key, "inventoryDecl")) { out->name = "idDeclInventory"; out->ops = "*"; }
        else if (!strcmp(key, "count")) out->name = "int";
        else return 0;
    } else return -1;
    return 1;
}

static int reference(void *context, const char *path, sh_decl_value_type type, const char *name, size_t length)
{
    fixture *f = (fixture *)context;
    size_t used = strlen(f->references);
    int written;
    if (f->refuse) return 0;
    written = snprintf(f->references + used, sizeof(f->references) - used, "%s|%s|%.*s\n",
                       path, type.name, (int)length, name);
    assert(written > 0 && (size_t)written < sizeof(f->references) - used);
    return 1;
}

static void gap(void *context, const char *path, sh_decl_value_type type, const char *reason)
{
    fixture *f = (fixture *)context;
    assert(path && *path && reason && *reason);
    (void)type;
    f->gaps++;
}

static int inspect(const char *text, fixture *f, sh_decl_dependency_result *result)
{
    sh_decl_dependency_schema schema = {f, describe, field, reference, gap};
    sh_decl_value_type entity = {"TestEntity", ""};
    sh_decl_source source = {text, strlen(text)};
    sh_decl_node *root = sh_decl_tree_parse(source, NULL, 0);
    int ok;
    assert(root);
    ok = sh_decl_entity_dependencies(root, entity, &schema, result);
    sh_decl_tree_free(root);
    return ok;
}

static int inspect_json(const char *json, fixture *f, sh_decl_dependency_result *result)
{
    sh_decl_dependency_schema schema = {f, describe, field, reference, gap};
    sh_decl_value_type type = {"TestEntity", ""};
    return sh_decl_json_state_dependencies(json, strlen(json), type, &schema, result);
}

/* idEventCallInfo: the event name, then counted typed arguments. Only decl
 * arguments carry a declaration; every other reader key is a plain value. */
static void event_checks(void)
{
    fixture f = {0};
    sh_decl_dependency_result result;
    /* The authored timeline call that M16 refused: no arguments at all. */
    assert(inspect_json("{\"eventCall\":{\"\\neventHandle_t eventDef\":\"disable\","
        "\"args\":{\"\\nnum\":0}}}", &f, &result));
    assert(!result.gaps && !result.references && !result.aborted);
    assert(inspect_json("{\"eventCall\":{\"eventDef\":\"enable\"}}", &f, &result) && !result.gaps);
    assert(inspect_json("{\"eventCall\":{\"eventDef\":\"enable\",\"args\":null}}", &f, &result) && !result.gaps);
    /* Typed arguments keep their order; the decl binds, the enum does not. */
    memset(&f, 0, sizeof(f));
    assert(inspect_json("{\"eventCall\":{\"\\neventHandle_t eventDef\":\"startSoundShader\","
        "\"args\":{\"\\nnum\":2,\"item[0]\":{\"decl\":{\"sound\":\"play_noise_hat\"}},"
        "\"item[1]\":{\"soundChannel_t\":\"SND_CHANNEL_ANY\"}}}}", &f, &result));
    assert(result.references == 1 && !result.gaps && !result.aborted);
    assert(!strcmp(f.references, "edit.eventCall.args.item[0].decl|idSoundShader|play_noise_hat\n"));
    /* Strings, entity names and other plain values are never references. */
    memset(&f, 0, sizeof(f));
    assert(inspect_json("{\"eventCall\":{\"eventDef\":\"setKey\",\"args\":{\"num\":7,"
        "\"item[0]\":{\"string\":\"models/not/a/reference\"},\"item[1]\":{\"entity\":\"sound/name\"},"
        "\"item[2]\":{\"int\":3},\"item[3]\":{\"vec3\":{\"x\":1,\"y\":2,\"z\":3}},"
        "\"item[4]\":{\"jointName\":\"head\"},\"item[5]\":{\"animWebPath\":\"web/path\"},"
        "\"item[6]\":{\"color\":{\"r\":1}}}}}", &f, &result));
    assert(!result.references && !result.gaps && !f.references[0]);
    /* Null and empty forms, and a type a projection excludes, add nothing. */
    memset(&f, 0, sizeof(f));
    assert(inspect_json("{\"eventCall\":{\"eventDef\":\"a\",\"args\":{\"num\":7,"
        "\"item[0]\":{\"decl\":null},\"item[1]\":{\"decl\":{\"sound\":\"\"}},\"item[2]\":{\"decl\":\"NULL\"},"
        "\"item[3]\":{},\"item[4]\":null,\"item[5]\":{\"decl\":{\"sound\":\"NULL\"}},"
        "\"item[6]\":{\"decl\":{\"excluded\":\"local/only\"}}}}}", &f, &result));
    assert(!result.references && !result.gaps && !f.references[0]);
    /* Each use is its own edge, even when two arguments name one decl. */
    memset(&f, 0, sizeof(f));
    assert(inspect_json("{\"eventCall\":{\"eventDef\":\"a\",\"args\":{\"num\":2,"
        "\"item[0]\":{\"decl\":{\"sound\":\"same\"}},\"item[1]\":{\"DECL\":{\"sound\":\"same\"}}}}}", &f, &result));
    assert(result.references == 2 && !result.gaps);
    assert(strstr(f.references, "edit.eventCall.args.item[0].decl|idSoundShader|same\n") &&
        strstr(f.references, "edit.eventCall.args.item[1].DECL|idSoundShader|same\n"));
    /* Unproven forms stay scoped gaps; proven siblings still bind. */
    memset(&f, 0, sizeof(f));
    assert(!inspect_json("{\"eventCall\":{\"eventDef\":\"a\",\"extra\":1,\"args\":{\"num\":7,"
        "\"item[0]\":{\"md6Anim\":\"models/anim.md6\"},\"item[1]\":{\"decl\":{\"sound\":\"a\",\"table\":\"b\"}},"
        "\"item[2]\":{\"decl\":{\"mystery\":\"unknown/manager\"}},\"item[3]\":{\"decl\":{\"sound\":\"kept\"},\"int\":1},"
        "\"item[4]\":{\"decl\":\"sound/bare\"},\"item[5]\":{\"decl\":{\"sound\":\"proven\"}},"
        "\"item[7]\":{\"decl\":{\"sound\":\"outside\"}},\"other\":1}}}", &f, &result));
    assert(result.gaps == 7 && result.references == 2 && !result.aborted);
    assert(!strcmp(f.references, "edit.eventCall.args.item[0].md6Anim|idMD6Anim|models/anim.md6\n"
        "edit.eventCall.args.item[5].decl|idSoundShader|proven\n"));
    memset(&f, 0, sizeof(f));
    assert(!inspect_json("{\"eventCall\":{\"eventDef\":\"a\",\"args\":{\"item[0]\":{\"decl\":{\"sound\":\"uncounted\"}}}}}", &f, &result));
    assert(result.gaps == 1 && !result.references);
    memset(&f, 0, sizeof(f));
    assert(!inspect_json("{\"eventCall\":\"disable\"}", &f, &result) && result.gaps == 1);

    /* Declaration text spells the same call; the reader is shared. */
    memset(&f, 0, sizeof(f));
    assert(inspect("{ edit = { eventCall = { eventDef = \"fadeWithTable\"; args = { num = 2;"
        " item[0] = { decl = { table = \"lights/flicker_fade_on\"; } } item[1] = { float = 6; } } } } }", &f, &result));
    assert(result.references == 1 && !result.gaps);
    assert(!strcmp(f.references, "edit.eventCall.args.item[0].decl|idDeclTable|lights/flicker_fade_on\n"));
    memset(&f, 0, sizeof(f));
    assert(inspect("{ edit = { eventCall = { eventDef = \"disable\"; args = { num = 0; } } } }", &f, &result));
    assert(!result.gaps && !result.references);
    assert(inspect("{ edit = { eventCall = { eventDef = \"a\"; args = { num = 4; item[0] = { decl = NULL; }"
        " item[1] = NULL; item[2] = { decl = { sound = \"\"; } } item[3] = { string = \"sound/text\"; } } } } }", &f, &result));
    assert(!result.gaps && !result.references && !f.references[0]);
    assert(inspect("{ edit = { eventCall = { eventDef = \"a\"; args = { num = 2;"
        " item[1] = { decl = { sound = \"same\"; } } item[0] = { decl = { sound = \"same\"; } } } } } }", &f, &result));
    assert(result.references == 2 && !result.gaps);
    memset(&f, 0, sizeof(f));
    assert(!inspect("{ edit = { eventCall = { eventDef = \"a\"; extra = 1; args = { num = 2;"
        " item[0] = { md6Anim = \"models/anim.md6\"; } item[1] = { decl = \"sound/bare\"; }"
        " item[2] = { decl = { sound = \"outside\"; } } } } } }", &f, &result));
    assert(result.gaps == 3 && result.references == 1);
    assert(!strcmp(f.references, "edit.eventCall.args.item[0].md6Anim|idMD6Anim|models/anim.md6\n"));

    /* md6Anim reads a string through the animation list: the exact name binds
     * in both spellings, NULL is an ordinary name, empty reads nothing, and an
     * entity or string argument beside it never becomes a dependency. */
    memset(&f, 0, sizeof(f));
    assert(inspect("{ edit = { eventCall = { eventDef = \"playAnim\"; args = { num = 4;"
        " item[0] = { entity = \"0_room/snapmaps/func/snap_lift_63\"; } item[1] = { md6Anim = \"models/a b.md6\"; }"
        " item[2] = { string = \"models/not_anim.md6\"; } item[3] = { md6Anim = \"\"; } } } } }", &f, &result));
    assert(result.references == 1 && !result.gaps);
    assert(!strcmp(f.references, "edit.eventCall.args.item[1].md6Anim|idMD6Anim|models/a b.md6\n"));
    memset(&f, 0, sizeof(f));
    assert(inspect_json("{\"eventCall\":{\"eventDef\":\"playAnim\",\"args\":{\"\\nnum\":3,"
        "\"item[0]\":{\"md6Anim\":\"NULL\"},\"item[1]\":{\"md6Anim\":\"\"},"
        "\"item[2]\":{\"MD6ANIM\":\"models/q\\\"uote.md6\"}}}}", &f, &result));
    assert(result.references == 2 && !result.gaps);
    assert(!strcmp(f.references, "edit.eventCall.args.item[0].md6Anim|idMD6Anim|NULL\n"
        "edit.eventCall.args.item[2].MD6ANIM|idMD6Anim|models/q\"uote.md6\n"));
    /* Without a proven animation reader, or with a non-string value, the
     * argument is a scoped gap rather than an exclusion. */
    memset(&f, 0, sizeof(f)); f.no_anim = 1;
    assert(!inspect("{ edit = { eventCall = { eventDef = \"a\"; args = { num = 1;"
        " item[0] = { md6Anim = \"models/anim.md6\"; } } } } }", &f, &result));
    assert(result.gaps == 1 && !result.references);
    memset(&f, 0, sizeof(f)); f.no_anim = 1;
    assert(!inspect_json("{\"eventCall\":{\"eventDef\":\"a\",\"args\":{\"num\":1,"
        "\"item[0]\":{\"md6Anim\":\"models/anim.md6\"}}}}", &f, &result));
    assert(result.gaps == 1 && !result.references);
    memset(&f, 0, sizeof(f));
    assert(!inspect_json("{\"eventCall\":{\"eventDef\":\"a\",\"args\":{\"num\":1,"
        "\"item[0]\":{\"md6Anim\":{\"name\":\"x\"}}}}}", &f, &result));
    assert(result.gaps == 1 && !result.references);
}

static void json_checks(void)
{
    fixture f = {0};
    sh_decl_dependency_result result;
    const char *json = "{\"inventoryItemDecl\":\"\\u0077eapon\\/test\","
        "\"name\":\"weapon/not-a-reference \x97 \xc3\xa9\",\"~type\":\"TestEntity\","
        "\"renderModelInfo\":{\"model\":\"model/test\"},"
        "\"startingInventory\":[{\"inventoryDecl\":\"ammo/with,comma[0]\"},{\"inventoryDecl\":null}],"
        "\"links\":[\"entity/test\",\"\",null],"
        "\"editorVars\":{\"inventoryItemDecl\":\"editor/preview\"}}";
    assert(inspect_json(json, &f, &result));
    assert(result.references == 4 && !result.gaps && !result.aborted);
    assert(strstr(f.references, "edit.inventoryItemDecl|idDeclInventory|weapon/test\n"));
    assert(strstr(f.references, "model/test") && strstr(f.references, "ammo/with,comma[0]"));
    assert(!strstr(f.references, "not-a-reference") && !strstr(f.references, "editor/preview"));
    memset(&f, 0, sizeof(f));
    assert(inspect_json("{\"arrays\":[[\"nested/reference\"]],\"startingInventory\":[]}", &f, &result));
    assert(result.references == 1 && !result.gaps && strstr(f.references, "nested/reference"));
    memset(&f, 0, sizeof(f));
    assert(inspect_json("{\"inventoryItemDecl\":\"legacy/\x97\",\"name\":\"\xe9\"}", &f, &result));
    assert(result.references == 1 && !result.gaps && strstr(f.references, "legacy/\x97"));
    memset(&f, 0, sizeof(f));
    assert(!inspect_json("{\"custom\":{\"name\":\"unknown/ref\"},\"inventoryItemDecl\":\"known/ref\"}", &f, &result));
    assert(result.references == 1 && result.gaps == 1 && !result.aborted);
    memset(&f, 0, sizeof(f));
    assert(!inspect_json("{\"inventoryItemDecl\":\"first\",\"inventoryItemDecl\":\"second\"}", &f, &result));
    assert(result.aborted && !result.references && !f.references[0]);
    memset(&f, 0, sizeof(f));
    assert(!inspect_json("{\"inventoryItemDecl\":\"first\",\"bad\":[}", &f, &result));
    assert(result.aborted && !result.references && !f.references[0]);
    memset(&f, 0, sizeof(f));
    assert(!inspect_json("{\"links\":[\"a\",\"b\",\"c\",\"outside\"]}", &f, &result));
    assert(result.references == 3 && result.gaps == 1 && !result.aborted && !strstr(f.references, "outside"));
    /* Native fixed arrays serialize as objects of their non-default elements. */
    memset(&f, 0, sizeof(f));
    assert(inspect_json("{\"links\":{\"links[0]\":\"entity/first\",\"links[2]\":\"entity/third\"},"
        "\"slots\":{\"slots[1]\":{\"inventoryDecl\":\"ammo/second\",\"count\":2}}}", &f, &result));
    assert(result.references == 3 && !result.gaps && !result.aborted);
    assert(strstr(f.references, "edit.links.links[2]|idDeclEntityDef|entity/third\n"));
    assert(strstr(f.references, "edit.slots.slots[1].inventoryDecl|idDeclInventory|ammo/second\n"));
    memset(&f, 0, sizeof(f));
    assert(!inspect_json("{\"links\":{\"links[3]\":\"entity/outside\",\"item[0]\":\"entity/misnamed\","
        "\"links[x]\":\"entity/letter\",\"links[]\":\"entity/empty\",\"links[1]\":\"entity/kept\"}}", &f, &result));
    assert(result.references == 1 && result.gaps == 4 && !result.aborted && strstr(f.references, "entity/kept"));
    /* Native entity state keys counted lists the same way (a saved player
     * inventory writes {"item[0]":...,"item[1]":"NULL"}). */
    memset(&f, 0, sizeof(f));
    assert(inspect_json("{\"startingInventory\":{\"item[0]\":{\"inventoryDecl\":\"ammo/listed\"}}}", &f, &result));
    assert(result.references == 1 && !result.gaps && !result.aborted && strstr(f.references, "ammo/listed"));
    memset(&f, 0, sizeof(f)); f.refuse = 1;
    assert(!inspect_json(json, &f, &result) && result.aborted);
}

int main(void)
{
    fixture f = {0};
    sh_decl_dependency_result result;
    const char *text = "{ inherit = \"base/test\"; editorVars {"
        " name = \"local/editor\"; renderModelInfo = { model = \"local/preview\"; } }"
        " edit = { name = \"game/model\"; renderModelInfo = {"
        " model = \"game/model\"; editorModel = \"local/preview\"; }"
        " startingInventory = { num = 2; item[0] = { inventoryDecl = \"weapon/test\"; count = 1; }"
        " item[1] = { inventoryDecl = ammo/test; } } links = { links[2] = \"entity/test\"; } } }";
    assert(inspect(text, &f, &result));
    assert(result.references == 5 && !result.gaps && !result.aborted);
    assert(!strcmp(f.references,
        "inherit|idDeclEntityDef|base/test\n"
        "edit.renderModelInfo.model|idRenderModel|game/model\n"
        "edit.startingInventory.item[0].inventoryDecl|idDeclInventory|weapon/test\n"
        "edit.startingInventory.item[1].inventoryDecl|idDeclInventory|ammo/test\n"
        "edit.links.links[2]|idDeclEntityDef|entity/test\n"));

    memset(&f, 0, sizeof(f));
    assert(!inspect("{ edit = { custom = { value = \"not-a-proven-reference\"; }"
                    " renderModelInfo = { model = \"known/model\"; }"
                    " unknownProperty = \"maybe/asset\"; } }", &f, &result));
    assert(result.gaps == 2 && f.gaps == 2 && result.references == 1 && !result.aborted);
    assert(strstr(f.references, "known/model") && !strstr(f.references, "maybe/asset"));

    memset(&f, 0, sizeof(f));
    assert(!inspect("{ edit = { links = { links[3] = \"out/of/range\";"
                    " links[1] = \"valid/ref\"; item[0] = \"wrong/spelling\"; } } }", &f, &result));
    assert(result.gaps == 2 && result.references == 1);

    memset(&f, 0, sizeof(f));
    assert(!inspect("{ edit = { startingInventory = { num = 1;"
                    " item[1] = { inventoryDecl = \"out/of/count\"; }"
                    " item[0] = { inventoryDecl = \"inside/count\"; } } } }", &f, &result));
    assert(result.gaps == 1 && result.references == 1 && strstr(f.references, "inside/count"));

    memset(&f, 0, sizeof(f));
    assert(!inspect("{ edit = { renderModelInfo = { model = \"escaped\\\\name\"; }"
                    " links = { links[184467440737095516160] = \"overflow\"; } } }", &f, &result));
    assert(result.gaps == 1 && result.references == 1 && strstr(f.references, "escaped\\\\name"));

    memset(&f, 0, sizeof(f));
    assert(inspect("{ edit = { arrays = { num = 1; item[0] = { item[2] = \"nested/ref\"; } } } }", &f, &result));
    assert(result.references == 1 && strstr(f.references, "edit.arrays.item[0].item[2]"));

    memset(&f, 0, sizeof(f));
    assert(inspect("{ editorVars { custom = { model = \"local/only\"; } } edit = {} }", &f, &result));
    assert(result.references == 0 && !result.gaps);
    assert(inspect("{ edit = { renderModelInfo = { model = \"\"; } } }", &f, &result));
    assert(result.references == 0 && !result.gaps);
    assert(inspect("{ inherit = NULL; edit = { inventoryItemDecl = NULL; renderModelInfo = { model = NULL; } } }", &f, &result));
    assert(result.references == 0 && !result.gaps);
    assert(inspect("{ edit = { inventoryItemDecl = \"NULL\"; } }", &f, &result));
    assert(result.references == 1 && strstr(f.references, "|NULL\n"));

    memset(&f, 0, sizeof(f)); f.refuse = 1;
    assert(!inspect(text, &f, &result) && result.aborted);
    {
        sh_decl_dependency_schema missing = {0};
        sh_decl_value_type entity = {"TestEntity", ""};
        assert(!sh_decl_state_dependencies(NULL, entity, &missing, &result) && result.aborted);
        assert(!result.visited && !result.references && !result.gaps);
        assert(!sh_decl_entity_dependencies(NULL, entity, NULL, &result) && result.aborted);
    }
    json_checks();
    event_checks();
    {
        const char *ordered = "{ inventoryItemDecl = \"first\"; inventoryItemDecl = \"second\"; }";
        sh_decl_node *state = sh_decl_tree_parse_ordered((sh_decl_source){ordered, strlen(ordered)}, NULL, 0);
        sh_decl_dependency_schema schema = {&f, describe, field, reference, gap};
        memset(&f, 0, sizeof(f));
        assert(state);
        assert(sh_decl_state_dependencies(state, (sh_decl_value_type){"TestEntity", ""}, &schema, &result));
        assert(!result.gaps && result.references == 2 && !result.aborted);
        assert(!strcmp(f.references, "edit.inventoryItemDecl|idDeclInventory|first\n"
            "edit.inventoryItemDecl|idDeclInventory|second\n"));
        sh_decl_tree_free(state);
    }
    {
        const char *ordered = "{ inherit = \"base/first\"; edit = { inventoryItemDecl = \"first\"; }"
            "inherit = \"base/second\"; edit = { inventoryItemDecl = \"second\"; } }";
        sh_decl_node *state = sh_decl_tree_parse_ordered((sh_decl_source){ordered, strlen(ordered)}, NULL, 0);
        sh_decl_dependency_schema schema = {&f, describe, field, reference, gap};
        memset(&f, 0, sizeof(f));
        assert(state && sh_decl_entity_dependencies(state, (sh_decl_value_type){"TestEntity", ""}, &schema, &result));
        assert(!result.gaps && result.references == 4 && !result.aborted);
        assert(!strcmp(f.references, "inherit|idDeclEntityDef|base/first\n"
            "edit.inventoryItemDecl|idDeclInventory|first\n"
            "inherit|idDeclEntityDef|base/second\n"
            "edit.inventoryItemDecl|idDeclInventory|second\n"));
        sh_decl_tree_free(state);
    }
    puts("decl_dependencies_test: PASS");
    return 0;
}
