#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/decl_rebind.h"

typedef struct fixture {
    const char *scope;
    unsigned calls;
    int fail_at, same, keep;
    char replacement[128];
} fixture;

static int describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    (void)context;
    if (!strcmp(type.name, "Entity") || !strcmp(type.name, "ModelInfo")) shape->kind = SH_DECL_VALUE_OBJECT;
    else if (!strcmp(type.name, "Text")) shape->kind = SH_DECL_VALUE_IGNORE;
    else if (!strcmp(type.name, "References")) {
        shape->kind = SH_DECL_VALUE_COLLECTION; shape->element = (sh_decl_value_type){"Resource", "*"};
        shape->item_key = "item"; shape->count_key = "num";
    } else if (!strcmp(type.name, "Fixed")) {
        shape->kind = SH_DECL_VALUE_COLLECTION; shape->element = (sh_decl_value_type){"Resource", "*"};
        shape->count = 3; shape->count_known = 1;
    } else if (!strcmp(type.name, "Resource") || !strcmp(type.name, "idDeclEntityDef"))
        shape->kind = SH_DECL_VALUE_REFERENCE;
    return 1;
}

static int field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *out)
{
    (void)context; (void)type;
    *out = (sh_decl_value_type){"Text", ""};
    if (!strcmp(key, "modelInfo")) out->name = "ModelInfo";
    else if (!strcmp(key, "resource")) *out = (sh_decl_value_type){"Resource", "*"};
    else if (!strcmp(key, "references")) out->name = "References";
    else if (!strcmp(key, "slots")) out->name = "Fixed";
    else if (!strcmp(key, "opaque")) out->name = "Unsupported";
    return 1;
}

static int resolve(void *context, const char *path, sh_decl_value_type type,
    const char *name, size_t length, const char **replacement)
{
    fixture *f = context;
    assert(path && *path && type.name && name && length);
    f->calls++;
    if (f->fail_at && f->calls == (unsigned)f->fail_at) return -1;
    if (f->keep) return 0;
    assert(length < 64);
    snprintf(f->replacement, sizeof(f->replacement), "%s%.*s", f->same ? "" : f->scope, (int)length, name);
    *replacement = f->replacement;
    return 1;
}

static const sh_decl_dependency_schema schema = {NULL, describe, field};
static const sh_decl_value_type entity = {"Entity", ""};

static void json_bindings(void)
{
    const char *json = "{ \"text\" : \"assets/rock\x97\",\n"
        "\"modelInfo\" : {\"resource\":\"assets\\/rock\",\"text\":\"assets/rock\"},"
        "\"references\": [\"material/red\", null, \"\", \"sound/wind\"],"
        "\"editorVars\":{\"resource\":\"assets/rock\"} }";
    const char *expected = "{ \"text\" : \"assets/rock\x97\",\n"
        "\"modelInfo\" : {\"resource\":\"scope/red/assets/rock\",\"text\":\"assets/rock\"},"
        "\"references\": [\"scope/red/material/red\", null, \"\", \"scope/red/sound/wind\"],"
        "\"editorVars\":{\"resource\":\"assets/rock\"} }";
    fixture f = {"scope/red/"};
    sh_decl_rebind_result result;
    size_t length;
    char *out = sh_decl_rebind_json((sh_decl_source){json, strlen(json)}, entity, &schema,
        resolve, &f, &length, &result);
    assert(out && !strcmp(out, expected) && length == strlen(expected));
    assert(result.changed == 3 && result.dependencies.references == 3 && f.calls == 3);
    free(out);

    /* Identical original resources acquire independent package bindings. */
    f = (fixture){"scope/green/"};
    out = sh_decl_rebind_json((sh_decl_source){json, strlen(json)}, entity, &schema, resolve, &f, &length, &result);
    assert(out && strstr(out, "scope/green/assets/rock") && !strstr(out, "scope/red/"));
    free(out);

    /* Retain original escaping and whitespace if the resolved name is equal. */
    f = (fixture){"unused/", 0, 0, 1};
    out = sh_decl_rebind_json((sh_decl_source){json, strlen(json)}, entity, &schema, resolve, &f, &length, &result);
    assert(out && !memcmp(out, json, length + 1) && length == strlen(json) && !result.changed);
    free(out);
    f = (fixture){"unused/", 0, 0, 0, 1};
    out = sh_decl_rebind_json((sh_decl_source){json, strlen(json)}, entity, &schema, resolve, &f, &length, &result);
    assert(out && !strcmp(out, json) && !result.changed);
    free(out);

    f = (fixture){"scope/red/", 0, 2};
    out = sh_decl_rebind_json((sh_decl_source){json, strlen(json)}, entity, &schema, resolve, &f, &length, &result);
    assert(!out && !length && !result.changed && result.dependencies.aborted && f.calls == 2);

    /* A fixed array keeps its native keyed members; each element rebinds in place. */
    const char *fixed = "{\"slots\":{\"slots[0]\":\"material/red\",\"slots[2]\":\"sound/wind\"}}";
    f = (fixture){"scope/red/"};
    out = sh_decl_rebind_json((sh_decl_source){fixed, strlen(fixed)}, entity, &schema, resolve, &f, &length, &result);
    assert(out && !strcmp(out, "{\"slots\":{\"slots[0]\":\"scope/red/material/red\",\"slots[2]\":\"scope/red/sound/wind\"}}"));
    assert(result.changed == 2 && f.calls == 2);
    free(out);
}

static void declaration_bindings(void)
{
    const char *text = "{ inherit = \"base/tool\"; editorVars { resource = \"local/preview\"; }"
        " edit = { text = \"assets/rock\"; modelInfo = { resource = assets/rock; }"
        " references = { num = 2; item[0] = \"material/red\"; item[1] = \"sound/wind\"; } } }";
    fixture f = {"scope/red/"};
    sh_decl_rebind_result result;
    size_t length;
    char *out = sh_decl_rebind_entity((sh_decl_source){text, strlen(text)}, entity, &schema,
        resolve, &f, &length, &result);
    sh_decl_node *tree;
    const sh_decl_node *edit, *info;
    assert(out && result.changed == 4 && result.dependencies.references == 4);
    tree = sh_decl_tree_parse((sh_decl_source){out, length}, NULL, 0);
    assert(tree);
    assert(!strcmp(sh_decl_tree_member(tree, "inherit")->value, "\"scope/red/base/tool\""));
    edit = sh_decl_tree_member(tree, "edit"); info = sh_decl_tree_member(edit, "modelInfo");
    assert(!strcmp(sh_decl_tree_member(info, "resource")->value, "\"scope/red/assets/rock\""));
    assert(!strcmp(sh_decl_tree_member(edit, "text")->value, "\"assets/rock\""));
    assert(strstr(out, "\"local/preview\"") && !strstr(out, "scope/red/local/preview"));
    sh_decl_tree_free(tree); free(out);

    f = (fixture){"unused/", 0, 0, 1};
    out = sh_decl_rebind_entity((sh_decl_source){text, strlen(text)}, entity, &schema, resolve, &f, &length, &result);
    assert(out && !strcmp(out, text) && !result.changed); free(out);

    /* A late gap prevents publishing even already resolved earlier edges. */
    text = "{ resource = \"first\"; opaque = { resource = \"hidden\"; } }";
    f = (fixture){"scope/red/"};
    out = sh_decl_rebind_state((sh_decl_source){text, strlen(text)}, entity, &schema, resolve, &f, &length, &result);
    assert(!out && !length && !result.changed && result.dependencies.gaps == 1 && f.calls == 1);

    text = "{ resource = \"first\"; }";
    f = (fixture){"unsafe\"name/"};
    out = sh_decl_rebind_state((sh_decl_source){text, strlen(text)}, entity, &schema, resolve, &f, &length, &result);
    assert(!out && !result.changed && result.dependencies.aborted);
    /* Source paths use literal backslashes, unlike JSON escaping. Rebinding
     * must not double them or modify matching ordinary strings. */
    text = "{ resource = \"folder\\name\"; text = \"folder\\name\"; }";
    f = (fixture){"scope\\red\\"};
    out = sh_decl_rebind_state((sh_decl_source){text, strlen(text)}, entity, &schema,
        resolve, &f, &length, &result);
    assert(out && result.changed == 1 && strstr(out, "\"scope\\red\\folder\\name\"") &&
        strstr(out, "text = \"folder\\name\";"));
    free(out);
    f = (fixture){"", 0, 0, 1};
    out = sh_decl_rebind_state((sh_decl_source){text, strlen(text)}, entity, &schema,
        resolve, &f, &length, &result);
    assert(out && !result.changed && length == strlen(text) && !memcmp(out, text, length));
    free(out);
}

static void json_failures(void)
{
    const char *cases[] = {
        "{\"resource\":\"first\",\"resource\":\"second\"}",
        "{\"resource\":\"first\",\"broken\": [}",
        "{\"resource\":\"first\",\"opaque\": {}}"
    };
    size_t i, length;
    sh_decl_rebind_result result;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fixture f = {"scope/red/"};
        char *out = sh_decl_rebind_json((sh_decl_source){cases[i], strlen(cases[i])}, entity, &schema,
            resolve, &f, &length, &result);
        assert(!out && !length && !result.changed);
        if (i < 2) assert(!f.calls && result.dependencies.aborted);
        else assert(f.calls == 1 && result.dependencies.gaps == 1);
    }
    {
        const char *json = "{\"resource\":\"\\u0072ock\",\"text\":\"\xe9\"}";
        fixture f = {"quote\"slash\\tab\t/"};
        char *out = sh_decl_rebind_json((sh_decl_source){json, strlen(json)}, entity, &schema,
            resolve, &f, &length, &result);
        assert(out && !strcmp(out, "{\"resource\":\"quote\\\"slash\\\\tab\\u0009/rock\",\"text\":\"\xe9\"}"));
        free(out);
    }
}

static int parsed_walk(void *context, const sh_decl_dependency_schema *view, sh_decl_dependency_result *result)
{ return sh_decl_state_dependencies(context, entity, view, result); }

static void parsed_bindings(void)
{
    const char *text = "{ resource = \"assets/rock\"; text = \"assets/rock\"; }";
    sh_decl_source source = {text, strlen(text)};
    sh_decl_node *tree = sh_decl_tree_parse(source, NULL, 0);
    const sh_decl_node *value = sh_decl_tree_member(tree, "resource");
    char *original_value = value->value, *out;
    fixture f = {"scope/blue/"};
    sh_decl_rebind_result result;
    size_t length;
    assert(tree);
    out = sh_decl_rebind_parsed(source, tree, &schema, parsed_walk, tree,
        resolve, &f, &length, &result);
    assert(out && result.changed == 1 && strstr(out, "scope/blue/assets/rock"));
    assert(value->value == original_value && !strcmp(value->value, "\"assets/rock\""));
    free(out);
    f = (fixture){"unsafe\"name/"};
    out = sh_decl_rebind_parsed(source, tree, &schema, parsed_walk, tree,
        resolve, &f, &length, &result);
    assert(!out && !length && !result.changed && value->value == original_value);
    sh_decl_tree_free(tree);
}

static int json_walk_twice(void *context, const sh_decl_dependency_schema *view, sh_decl_dependency_result *result)
{
    sh_decl_source *source = context;
    return sh_decl_json_state_dependencies(source->text, source->length, entity, view, result) &&
        sh_decl_json_state_dependencies(source->text, source->length, entity, view, result);
}
static int changing_resolve(void *context, const char *path, sh_decl_value_type type,
    const char *name, size_t length, const char **replacement)
{
    fixture *f = context;
    if (f->calls) f->scope = "different/";
    return resolve(context, path, type, name, length, replacement);
}
static void repeated_json_sites(void)
{
    sh_decl_source source = {"{\"resource\":\"rock\",\"text\":\"\x97\"}", 0}; source.length = strlen(source.text);
    fixture f = {"shared/"}; sh_decl_rebind_result result; size_t length;
    char *out = sh_decl_rebind_json_walk(source, &schema, json_walk_twice, &source, resolve, &f, &length, &result);
    assert(out && result.changed == 1 && !strcmp(out, "{\"resource\":\"shared/rock\",\"text\":\"\x97\"}")); free(out);
    f = (fixture){"shared/"};
    out = sh_decl_rebind_json_walk(source, &schema, json_walk_twice, &source, changing_resolve, &f, &length, &result);
    assert(!out && !length && !result.changed && result.dependencies.aborted);
}

int main(void)
{
    json_bindings(); declaration_bindings(); json_failures(); parsed_bindings(); repeated_json_sites();
    puts("decl_rebind_test: PASS");
    return 0;
}
