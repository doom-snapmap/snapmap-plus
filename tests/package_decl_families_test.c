/* Reflected non-entity declarations through the complete package compiler. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_compiler.h"
#include "decl_graph_compose.h"
#include "decl_md6_compose.h"
#include "package_fixture.h"

static const char *active_path, *original;
static int metadata_missing, graph_adapter, graph_calls;
static int baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    if (!original || strcmp(path, active_path)) return 0;
    *body = (unsigned char *)_strdup(original); *length = strlen(original); return *body ? 1 : -1;
}
static int derives(void *context, const char *a, const char *b)
{ (void)context; return !strcmp(a, b); }
static int root_type(void *context, const char *type, sh_decl_value_type *out)
{
    (void)context; *out = (sh_decl_value_type){0};
    if (metadata_missing) return -1;
    if (!strcmp(type, "material") || !strcmp(type, "md6def")) return 0;
    *out = (sh_decl_value_type){!strcmp(type, "aifsmmanager") ? "CustomReader" : "ReflectedDecl", ""}; return 1;
}
static int describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    (void)context;
    if (!strcmp(type.ops, "[3]")) {
        shape->kind = SH_DECL_VALUE_COLLECTION; shape->count = 3; shape->count_known = 1;
        shape->element = (sh_decl_value_type){"Entry", ""};
    } else if (!strcmp(type.name, "Entries")) {
        shape->kind = SH_DECL_VALUE_COLLECTION; shape->item_key = "item"; shape->count_key = "num";
        shape->element = (sh_decl_value_type){"Entry", ""};
    } else if (!strcmp(type.name, "ReflectedDecl") || !strcmp(type.name, "Entry")) {
        shape->kind = SH_DECL_VALUE_OBJECT;
    } else if (!strcmp(type.name, "int") || !strcmp(type.name, "idStr")) shape->kind = SH_DECL_VALUE_IGNORE;
    return 1;
}
static int field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *out)
{
    (void)context; (void)type;
    if (!strcmp(key, "channels")) *out = (sh_decl_value_type){"Entry", "[3]"};
    else if (!strcmp(key, "stages") || !strcmp(key, "renderModelInfoList")) *out = (sh_decl_value_type){"Entries", ""};
    else if (!strcmp(key, "tuning")) *out = (sh_decl_value_type){"Entry", ""};
    else if (!strcmp(key, "custom")) *out = (sh_decl_value_type){"CustomReader", ""};
    else if (!strcmp(key, "renderModelMaterial")) *out = (sh_decl_value_type){"idStr", ""};
    else if (!strcmp(key, "health") || !strcmp(key, "cooldown") || !strcmp(key, "x") || !strcmp(key, "y"))
        *out = (sh_decl_value_type){"int", ""};
    else return -1;
    return 1;
}
static int dynamic(void *context, sh_decl_value_type base, const char *name, size_t length, sh_decl_value_type *out)
{
    (void)context; (void)base; (void)name; (void)length; (void)out;
    return 0; /* The integration fixture edits only the graph root object. */
}
static int custom(void *context, const char *type, sh_decl_source base, const sh_decl_source *sources,
    size_t count, char **body, size_t *length, char *error, size_t capacity, sh_decl_conflict *conflict,
    const sh_package_source_view *source_view)
{
    sh_decl_dependency_schema types = {NULL, describe, field, NULL, NULL, dynamic};
    (void)context; (void)source_view;
    if (!strcmp(type, "md6def")) {
        *body = sh_decl_md6_compose(base, sources, count, length, error, capacity, conflict);
        return *body ? 1 : -1;
    }
    if (!graph_adapter || strcmp(type, "aifsmmanager")) return 0;
    graph_calls++;
    *body = sh_decl_graph_compose(base, sources, count, &types, (sh_decl_value_type){"CustomReader", ""},
        length, error, capacity, conflict);
    return *body ? 1 : -1;
}
static sh_package_compilation *compile(const char *a, const char *b, const char *product,
    sh_package_sources **sources, char *error)
{
    char path[1024];
    sh_decl_dependency_schema types = {NULL, describe, field, NULL, NULL};
    sh_package_builtin builtin = {active_path, (const unsigned char *)product, product ? strlen(product) : 0};
    sh_package_compile_environment environment = {baseline, NULL, product ? &builtin : NULL,
        product ? 1 : 0, &types, NULL, derives, root_type};
    environment.compose_custom = custom;
    snprintf(path, sizeof(path), "overrides/a/assets/%s", active_path); create(path, a);
    snprintf(path, sizeof(path), "overrides/b/assets/%s", active_path); create(path, b);
    *sources = sh_package_sources_scan(root, error, 2048); CHECK(*sources);
    return *sources ? sh_package_compile_with(*sources, &environment, error, 2048) : NULL;
}
static void begin(const char *path, const char *base)
{
    char temporary[MAX_PATH];
    active_path = path; original = base; metadata_missing = 0; graph_adapter = graph_calls = 0;
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "pdr", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    create("overrides/a/package.json", "{\"id\":\"a\",\"name\":\"A\"}");
    create("overrides/b/package.json", "{\"id\":\"b\",\"name\":\"B\"}");
}
static void end(sh_package_compilation *compiled, sh_package_sources *sources)
{
    size_t i;
    for (i = 0; sources && i < sources->file_count; i++)
        if (!sources->files[i].directory) CHECK(sh_package_source_verify(&sources->files[i]));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources); cleanup();
}
int main(void)
{
    const char *path = "generated/decls/aicomponent_cyberdemon/fixture.decl";
    char error[2048];
    sh_package_sources *sources;
    sh_package_compilation *compiled;
    const sh_compiled_resource *resource;
    begin(path, "{ edit = { channels = { channels[0] = { x = 1; } channels[2] = { x = 2; } } tuning = { health = 100; cooldown = 1; } } }");
    compiled = compile(
        "{ inherit = \"parent\"; edit = { channels = { channels[0] = { x = 5; } channels[2] = { x = 2; } } tuning = { health = 200; cooldown = 1; } } }",
        "{ inherit = \"parent\"; edit = { channels = { channels[0] = { x = 1; } channels[2] = { x = 6; } } tuning = { health = 100; cooldown = 2; } } }",
        "{ edit = { channels = { channels[0] = { x = 1; y = 9; } channels[2] = { x = 2; } } tuning = { health = 100; cooldown = 1; } } }", &sources, error);
    if (!compiled) fprintf(stderr, "%s\n", error);
    CHECK(compiled);
    if (compiled) {
        resource = sh_package_compilation_find(compiled, path); CHECK(resource);
        CHECK(resource && resource->gameplay_owners.bits == 3 && resource->source_count == 2 && resource->composed);
        CHECK(resource && strstr((char *)resource->body, "health = 200;") && strstr((char *)resource->body, "cooldown = 2;"));
        CHECK(resource && strstr((char *)resource->body, "x = 5;") && strstr((char *)resource->body, "x = 6;") && strstr((char *)resource->body, "y = 9;"));
        CHECK(resource && strstr((char *)resource->body, "inherit") < strstr((char *)resource->body, "edit"));
        CHECK(resource && !strstr((char *)resource->body, "num ="));
    }
    end(compiled, sources);

    begin(path, "{}");
    compiled = compile("{ edit = { channels = { channels[0] = { x = 1; } } } }",
        "{ edit = { channels = { channels[3] = { x = 2; } } } }", NULL, &sources, error);
    CHECK(!compiled && strstr(error, "fixed native array") && strstr(error, "out-of-range")); end(compiled, sources);

    begin(path, "{}");
    compiled = compile("{ edit = { stages = { item[0] = { x = 1; } } } }",
        "{ edit = { stages = { item[1] = { y = 2; } } } }", NULL, &sources, error);
    CHECK(!compiled && strstr(error, "edit.stages") && strstr(error, "composition adapter")); end(compiled, sources);

    begin(path, "{ edit = { custom = { x = 0; y = 0; } } }");
    compiled = compile("{ edit = { custom = { x = 1; y = 0; } } }",
        "{ edit = { custom = { x = 0; y = 2; } } }", NULL, &sources, error);
    CHECK(!compiled && strstr(error, "edit.custom") && strstr(error, "native reader")); end(compiled, sources);

    /* A generic declaration parser can dispatch the entire edit block through
     * a custom registered reader, as the native graph declaration families do. */
    begin("generated/decls/aifsmmanager/fixture.decl", "{ edit = { x = 0; y = 0; } }");
    compiled = compile("{ edit = { x = 1; y = 0; } }", "{ edit = { x = 0; y = 2; } }", NULL, &sources, error);
    CHECK(!compiled && strstr(error, "edit:") && strstr(error, "native reader")); end(compiled, sources);

    begin("generated/decls/aifsmmanager/fixture.decl", "{ edit = { object = { x = 0; y = 0; health = 100; } } }");
    graph_adapter = 1;
    compiled = compile("{ edit = { object = { x = 1; y = 0; health = 100; } } }",
        "{ edit = { object = { x = 0; y = 2; health = 100; } } }",
        "{ edit = { object = { x = 0; y = 0; health = 200; } } }", &sources, error);
    if (!compiled) fprintf(stderr, "%s\n", error);
    CHECK(compiled && graph_calls == 1);
    if (compiled) {
        resource = sh_package_compilation_find(compiled, active_path);
        CHECK(resource && resource->composed && resource->source_count == 2 && resource->owners.bits == 3);
        CHECK(resource && strstr((char *)resource->body, "x = 1;") && strstr((char *)resource->body, "y = 2;") &&
            strstr((char *)resource->body, "health = 200;") && !strstr((char *)resource->body, "@graph/"));
    }
    end(compiled, sources);
    begin("generated/decls/aifsmmanager/fixture.decl", "{ edit = { object = { x = 0; } } }");
    graph_adapter = 1;
    compiled = compile("{ edit = { object = { x = 1; } } }", "{ edit = { object = { x = 2; } } }", NULL, &sources, error);
    CHECK(!compiled && graph_calls == 1 && strstr(error, "edit.object.x") && strstr(error, "[a; b]"));
    end(compiled, sources);
    begin("generated/decls/aifsmmanager/fixture.decl", "{}"); graph_adapter = 1;
    compiled = compile("{ custom native grammar }", "{ custom native grammar }", NULL, &sources, error);
    CHECK(compiled && !graph_calls); end(compiled, sources);

    begin(path, "{ edit = { tuning = { health = 100; } } }");
    compiled = compile("{ edit = { tuning = { health = 200; } } }",
        "{ edit = { tuning = { health = 300; } } }", NULL, &sources, error);
    CHECK(!compiled && strstr(error, "tuning.health") && strstr(error, "[a; b]")); end(compiled, sources);

    path = "generated/decls/snappropertyinspector_blockingvolumerendermodel/fixture.decl";
    begin(path, "{ edit = { renderModelInfoList = { num = 1; item[0] = { renderModelMaterial = \"base\"; } } } }");
    compiled = compile("{ edit = { renderModelInfoList = { num = 2; item[0] = { renderModelMaterial = \"base\"; } item[1] = { renderModelMaterial = \"added-a\"; } } } }",
        "{ edit = { renderModelInfoList = { num = 2; item[0] = { renderModelMaterial = \"base\"; } item[1] = { renderModelMaterial = \"added-b\"; } } } }", NULL, &sources, error);
    if (!compiled) fprintf(stderr, "%s\n", error);
    CHECK(compiled);
    if (compiled) {
        resource = sh_package_compilation_find(compiled, path);
        CHECK(resource && resource->owners.bits == 3 && !sh_package_owners_count(&resource->gameplay_owners));
        CHECK(resource && strstr((char *)resource->body, "num = 3;") && strstr((char *)resource->body, "added-a") && strstr((char *)resource->body, "added-b"));
    }
    end(compiled, sources);

    path = "generated/decls/material/fixture.decl";
    begin(path, "{}");
    compiled = compile("{ stageprogram one }", "{ stageprogram one }", NULL, &sources, error);
    CHECK(compiled);
    if (compiled) {
        resource = sh_package_compilation_find(compiled, path);
        CHECK(resource && !strcmp((char *)resource->body, "{ stageprogram one }") && resource->source_count == 2);
    }
    end(compiled, sources);
    begin(path, "{}");
    compiled = compile("{ stageprogram one }", "{ stageprogram two }", NULL, &sources, error);
    CHECK(!compiled && strstr(error, "native declaration family") && strstr(error, "[a; b]")); end(compiled, sources);

    path = "generated/decls/md6def/fixture.decl";
    begin(path, "{ init { mesh \"model\" } jointGroups {} events {} aliases {} props {} }");
    compiled = compile(
        "{ init { mesh \"model\" } jointGroups { damageGroup \"head\" { head } } events {} aliases {} props {} }",
        "{ init { mesh \"model\" } jointGroups {} events {} aliases { alias { name \"attack\" anim \"attack.md6anim\" } } props {} }",
        "{ init { mesh \"model\" } jointGroups {} events { anim \"attack.md6anim\" { event \"ae_attack\" { frame 2 } } } aliases {} props {} }", &sources, error);
    if (!compiled) fprintf(stderr, "%s\n", error);
    CHECK(compiled);
    if (compiled) {
        resource = sh_package_compilation_find(compiled, path);
        CHECK(resource && resource->composed && resource->source_count == 2 && resource->gameplay_owners.bits == 3);
        CHECK(resource && strstr((char *)resource->body, "damageGroup \"head\"") &&
            strstr((char *)resource->body, "name \"attack\"") && strstr((char *)resource->body, "event \"ae_attack\""));
    }
    end(compiled, sources);
    /* Two packages adding different joints to one group both keep their entry. */
    begin(path, "{ init { mesh \"model\" } jointGroups { damageGroup \"head\" { head } } events {} aliases {} props {} }");
    compiled = compile(
        "{ init { mesh \"model\" } jointGroups { damageGroup \"head\" { head neck } } events {} aliases {} props {} }",
        "{ init { mesh \"model\" } jointGroups { damageGroup \"head\" { head spine } } events {} aliases {} props {} }",
        NULL, &sources, error);
    if (!compiled) fprintf(stderr, "%s\n", error);
    CHECK(compiled);
    if (compiled) {
        resource = sh_package_compilation_find(compiled, path);
        CHECK(resource && resource->composed && resource->gameplay_owners.bits == 3);
        CHECK(resource && strstr((char *)resource->body, "neck") && strstr((char *)resource->body, "spine"));
    }
    end(compiled, sources);
    /* Two different payloads for one joint of one group still report the exact
     * overlapping owners. */
    begin(path, "{ init { mesh \"model\" } jointGroups { hitTestGroup \"a\" { head {\nradius 1\n} } } events {} aliases {} props {} }");
    compiled = compile(
        "{ init { mesh \"model\" } jointGroups { hitTestGroup \"a\" { head {\nradius 2\n} } } events {} aliases {} props {} }",
        "{ init { mesh \"model\" } jointGroups { hitTestGroup \"a\" { head {\nradius 3\n} } } events {} aliases {} props {} }",
        NULL, &sources, error);
    CHECK(!compiled && strstr(error, "same field") && strstr(error, "[a; b]")); end(compiled, sources);
    /* Init contributions use the same package and built-in composition pass. */
    begin(path, "{ init { mesh \"model\" offset ( 0 0 0 ) calcRefBoundsFromJoints 0 } jointGroups {} events {} aliases {} props {} }");
    compiled = compile(
        "{ init { mesh \"model\" offset ( 1 2 3 ) calcRefBoundsFromJoints 0 } jointGroups {} events {} aliases {} props {} }",
        "{ init { mesh \"model\" offset ( 0 0 0 ) calcRefBoundsFromJoints 1 } jointGroups {} events {} aliases {} props {} }",
        "{ init { mesh \"model\" offset ( 0 0 0 ) calcRefBoundsFromJoints 0 } jointGroups {} events {} aliases { alias { name \"idle\" anim \"idle.md6anim\" } } props {} }",
        &sources, error);
    CHECK(compiled);
    if (compiled) {
        resource = sh_package_compilation_find(compiled, path);
        CHECK(resource && resource->composed && resource->source_count == 2 && resource->gameplay_owners.bits == 3);
        CHECK(resource && strstr((char *)resource->body, "offset ( 1 2 3 )") &&
            strstr((char *)resource->body, "calcRefBoundsFromJoints 1") && strstr((char *)resource->body, "name \"idle\""));
    }
    end(compiled, sources);
    begin(path, "{ init { mesh \"model\" } jointGroups {} events {} aliases { alias { name \"idle\" anim \"idle.md6anim\" } } props {} }");
    compiled = compile(
        "{ init { mesh \"model\" } jointGroups {} events {} aliases { alias { name \"idle\" anim \"idle.md6anim\" anim \"look.md6anim\" } } props {} }",
        "{ init { mesh \"model\" } jointGroups {} events {} aliases { alias { name \"idle\" anim \"idle.md6anim\" anim \"wait.md6anim\" } } props {} }",
        "{ init { mesh \"model\" } jointGroups {} events {} aliases { alias { name \"idle\" flags { forceLoad } anim \"idle.md6anim\" } } props {} }",
        &sources, error);
    CHECK(compiled);
    if (compiled) {
        resource = sh_package_compilation_find(compiled, path);
        CHECK(resource && resource->composed && resource->source_count == 2 && resource->gameplay_owners.bits == 3);
        CHECK(resource && strstr((char *)resource->body, "anim \"look.md6anim\"") &&
            strstr((char *)resource->body, "anim \"wait.md6anim\"") && strstr((char *)resource->body, "forceLoad"));
    }
    end(compiled, sources);
    /* Exact custom bytes stay intact when two authored bundles duplicate them. */
    begin(path, "{}");
    compiled = compile("{ untouched custom dialect }", "{ untouched custom dialect }", NULL, &sources, error);
    CHECK(compiled);
    if (compiled) {
        resource = sh_package_compilation_find(compiled, path);
        CHECK(resource && !resource->composed && resource->source_count == 2 && resource->gameplay_owners.bits == 3 &&
            !strcmp((char *)resource->body, "{ untouched custom dialect }"));
    }
    end(compiled, sources);

    begin("generated/decls/snapeditorentitydef/fixture.decl", "{}"); metadata_missing = 1;
    compiled = compile("{ edit = { tuning = { x = 1; } } }", "{ edit = { tuning = { y = 2; } } }", NULL, &sources, error);
    CHECK(!compiled && strstr(error, "metadata is unavailable")); end(compiled, sources);
    if (failures) return 1;
    puts("package_decl_families_test: PASS"); return 0;
}
