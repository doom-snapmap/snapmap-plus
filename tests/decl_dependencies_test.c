#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/decl_dependencies.h"

typedef struct fixture {
    char references[4096];
    size_t gaps;
    int refuse;
} fixture;

static int describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    (void)context;
    if (!strcmp(type.name, "idDeclEntityDef") || !strcmp(type.name, "idDeclInventory") ||
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
        else if (!strcmp(key, "arrays")) out->name = "TestArrayList";
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

static void json_checks(void)
{
    fixture f = {0};
    sh_decl_dependency_result result;
    const char *json = "{\"inventoryItemDecl\":\"\\u0077eapon\\/test\","
        "\"name\":\"weapon/not-a-reference\",\"~type\":\"TestEntity\","
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
    assert(result.gaps == 2 && result.references == 0);

    memset(&f, 0, sizeof(f));
    assert(inspect("{ edit = { arrays = { num = 1; item[0] = { item[2] = \"nested/ref\"; } } } }", &f, &result));
    assert(result.references == 1 && strstr(f.references, "edit.arrays.item[0].item[2]"));

    memset(&f, 0, sizeof(f));
    assert(inspect("{ editorVars { custom = { model = \"local/only\"; } } edit = {} }", &f, &result));
    assert(result.references == 0 && !result.gaps);
    assert(inspect("{ edit = { renderModelInfo = { model = \"\"; } } }", &f, &result));
    assert(result.references == 0 && !result.gaps);

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
    {
        const char *ordered = "{ inventoryItemDecl = \"first\"; inventoryItemDecl = \"second\"; }";
        sh_decl_node *state = sh_decl_tree_parse_ordered((sh_decl_source){ordered, strlen(ordered)}, NULL, 0);
        sh_decl_dependency_schema schema = {&f, describe, field, reference, gap};
        memset(&f, 0, sizeof(f));
        assert(state);
        assert(!sh_decl_state_dependencies(state, (sh_decl_value_type){"TestEntity", ""}, &schema, &result));
        assert(result.gaps == 1 && result.references == 2 && !result.aborted);
        assert(strstr(f.references, "first") && strstr(f.references, "second"));
        sh_decl_tree_free(state);
    }
    puts("decl_dependencies_test: PASS");
    return 0;
}
