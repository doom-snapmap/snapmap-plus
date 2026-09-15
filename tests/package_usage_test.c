#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_usage.h"
#include "package_fixture.h"
#include "resource_graph.h"

static const char *original = "{ class = \"idTest\"; editorVars { description = \"Stock\"; } edit = { speed = 1; } }";
static int baseline(void *context, const char *path, unsigned char **out, size_t *length)
{
    (void)context; *out = NULL; *length = 0;
    if (!strcmp(path, "generated/image/stock_body.bimage")) {
        *out = (unsigned char *)_strdup("stock-image"); *length = strlen("stock-image");
        return *out ? 1 : -1;
    }
    if (!strstr(path, "/entitydef/stock/") && !strstr(path, "/entitydef/replaced.decl")) return 0;
    *length = strlen(original); *out = (unsigned char *)_strdup(original); return *out ? 1 : -1;
}
static uint64_t used(sh_package_compilation *compiled, const char *json)
{
    sh_package_owners owners = {0};
    CHECK(sh_package_map_owners(compiled, NULL, json, strlen(json), NULL, &owners, NULL));
    CHECK(sh_package_owners_within(&owners, 64));
    { uint64_t bits = owners.bits; sh_package_owners_free(&owners); return bits; }
}
static const char *lift = "{\"entityDef\":{\"inherit\":\"stock/lift\",\"targetType\":\"idDeclEntityDef\"}}";
static void entity(const char *name, const char *child_type, const char *child_name)
{
    sh_resource_graph_frame frame;
    sh_resource_graph_begin(&frame, "entitydef", name);
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin_state(&frame, "entitydef", name, 1);
    if (child_type) sh_resource_graph_reference(child_type, child_name);
    sh_resource_graph_end(&frame, 1);
}
static void dependency_checks(sh_package_compilation *compiled)
{
    sh_resource_graph_frame frame;
    sh_package_owners owners = {0};
    int complete = 1;
    /* A cold graph retains direct owners and reports missing coverage. */
    CHECK(sh_package_map_owners(compiled, NULL, lift, strlen(lift), NULL, &owners, &complete));
    CHECK(!sh_package_owners_count(&owners) && !complete);
    entity("stock/lift", "material", "stock/body");
    sh_resource_graph_begin(&frame, "material", "stock/body");
    sh_resource_graph_file("GENERATED\\IMAGE\\stock_body.bimage");
    sh_resource_graph_reference("material", "stock/cycle");
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin(&frame, "material", "stock/cycle");
    sh_resource_graph_reference("material", "stock/body");
    sh_resource_graph_end(&frame, 1);
    CHECK(sh_package_map_owners(compiled, NULL, lift, strlen(lift), NULL, &owners, &complete));
    CHECK(owners.bits == 16 && complete); /* Vanilla root, transitive image replacement. */
    {
        sh_package_policy policy = {0};
        char error[1024];
        CHECK(sh_package_map_policy(compiled, NULL, lift, strlen(lift), NULL, &policy, &owners,
                                     &complete, error, sizeof(error)));
        CHECK(owners.bits == 16 && complete);
        CHECK(sh_json_object_get(&policy.hud, "weapons") &&
              strstr(sh_json_object_get(&policy.hud, "weapons"), "weapon/test/surface"));
        CHECK(!policy.strings.count && !policy.requirements.count);
        sh_package_policy_free(&policy);
    }
    entity("stock/encounter", "entitydef", "demons/cyber");
    entity("demons/cyber", NULL, NULL);
    CHECK(used(compiled, "{\"inherit\":\"stock/encounter\",\"targetType\":\"idDeclEntityDef\"}") == 12);
    /* Opaque duplicate model ownership also preserves both authored packages. */
    entity("stock/model", "model", "stock/shared");
    sh_resource_graph_begin(&frame, "model", "stock/shared");
    sh_resource_graph_file("generated/models/shared.bmodel");
    sh_resource_graph_end(&frame, 1);
    CHECK(used(compiled, "{\"inherit\":\"stock/model\",\"targetType\":\"idDeclEntityDef\"}") == 12);
    /* Prune an editor branch before traversing its gameplay-shaped preview. */
    entity("stock/unknown", "snapeditorentitydef", "palette");
    sh_resource_graph_begin(&frame, "snapeditorentitydef", "palette");
    sh_resource_graph_file("generated/image/editor_preview.bimage");
    sh_resource_graph_reference("entitydef", "demons/cyber");
    sh_resource_graph_end(&frame, 1);
    {
        const char *json = "{\"inherit\":\"stock/unknown\",\"targetType\":\"idDeclEntityDef\",\"editorVars\":{\"inherit\":\"stock/lift\",\"targetType\":\"idDeclEntityDef\"}}";
        CHECK(sh_package_map_owners(compiled, NULL, json, strlen(json), NULL, &owners, &complete));
        CHECK(!sh_package_owners_count(&owners) && complete);
    }
    /* Missing phases retain known replacements without claiming completeness. */
    sh_resource_graph_begin(&frame, "material", "stock/body");
    sh_resource_graph_file("generated/image/stock_body.bimage");
    sh_resource_graph_reference("sound", "unobserved");
    sh_resource_graph_end(&frame, 0);
    CHECK(sh_package_map_owners(compiled, NULL, lift, strlen(lift), NULL, &owners, &complete));
    CHECK(owners.bits == 16 && !complete);
    CHECK(used(compiled, "{\"a\":{\"inherit\":\"stock/lift\",\"targetType\":\"idDeclEntityDef\"},\"b\":{\"value\":\"replaced\",\"targetType\":\"idDeclEntityDef\"}}") == 18);
    /* A resource reload removes obsolete dependencies from future saves. */
    sh_resource_graph_begin(&frame, "material", "stock/body");
    sh_resource_graph_file("generated/image/vanilla_body.bimage");
    sh_resource_graph_end(&frame, 1);
    CHECK(sh_package_map_owners(compiled, NULL, lift, strlen(lift), NULL, &owners, &complete));
    CHECK(!sh_package_owners_count(&owners) && complete);
    sh_package_owners_free(&owners);
    sh_resource_graph_test_reset();
}

static void policy_checks(sh_package_compilation *compiled)
{
    static const char cyber[] = "{\"value\":\"demons/cyber\",\"targetType\":\"idDeclEntityDef\"}";
    static const char vanilla[] = "{\"entities\":[{\"value\":\"stock/unknown\",\"targetType\":\"idDeclEntityDef\"}],"
        "\"editorVars\":{\"value\":\"demons/cyber\",\"targetType\":\"idDeclEntityDef\"}}";
    sh_package_policy policy = {0};
    char error[1024], *before, *after;
    size_t before_length = 0, after_length = 0;
    sh_package_owners owners = {0};
    int complete = 1;
    const char *raw;
    before = sh_json_serialize_object(&compiled->policy.strings, 0, &before_length);
    CHECK(before && strstr(before, "local_tool"));
    CHECK(sh_package_map_policy(compiled, NULL, cyber, sizeof(cyber) - 1, NULL, &policy, &owners,
                                 &complete, error, sizeof(error)));
    CHECK(owners.bits == 12 && !complete); /* Both intact duplicate delivery units. */
    raw = sh_json_object_get(&policy.requirements, "cvars");
    CHECK(raw && strstr(raw, "g_useResourceBlackList") && !strstr(raw, "g_useImageBlackList"));
    raw = sh_json_object_get(&policy.strings, "en");
    CHECK(raw && strstr(raw, "Cyberdemon") && strstr(raw, "boss_tool") && !strstr(raw, "local_tool"));
    raw = sh_json_object_get(&policy.hud, "weapons");
    CHECK(raw && strstr(raw, "weapon/test/boss") && !strstr(raw, "weapon/test/surface"));
    sh_package_policy_free(&policy);

    /* Switching to a stock map clears every gameplay policy. Its editor-only
     * changes and preview references do not select either demon package. */
    CHECK(sh_package_map_policy(compiled, NULL, vanilla, sizeof(vanilla) - 1, NULL, &policy, &owners,
                                 &complete, error, sizeof(error)));
    CHECK(!sh_package_owners_count(&owners) && !policy.requirements.count && !policy.strings.count && !policy.hud.count);
    sh_package_policy_free(&policy);
    owners.bits = 123; complete = 1;
    CHECK(!sh_package_map_policy(compiled, NULL, "{", 1, NULL, &policy, &owners,
                                  &complete, error, sizeof(error)));
    CHECK(!sh_package_owners_count(&owners) && !complete && !policy.requirements.count && !policy.strings.count && !policy.hud.count);
    CHECK(error[0]);
    sh_package_owners_add(&owners, 63);
    CHECK(!sh_package_compilation_policy(compiled, &owners, &policy, error, sizeof(error)));
    CHECK(!policy.requirements.count && !policy.strings.count && !policy.hud.count);

    /* Projection never mutates the installed editor strings or source data. */
    after = sh_json_serialize_object(&compiled->policy.strings, 0, &after_length);
    CHECK(before && after && before_length == after_length && !memcmp(before, after, before_length));
    free(before); free(after); sh_package_owners_free(&owners);
}

static int has_path(const sh_package_references *paths, const char *name)
{
    for (size_t i = 0; i < paths->count; i++)
        if (!paths->items[i].type[0] && !strcmp(paths->items[i].name, name)) return 1;
    return 0;
}
static void resource_path_checks(sh_package_compilation *compiled)
{
    const char *cyber = "{\"inherit\":\"demons/cyber\",\"targetType\":\"idDeclEntityDef\","
        "\"editorVars\":{\"inherit\":\"replaced\",\"targetType\":\"idDeclEntityDef\"}}";
    sh_package_references paths = {0}, instance = {0};
    sh_resource_graph_frame frame;
    sh_resource_graph_test_reset();
    CHECK(sh_package_map_resources(compiled, NULL, cyber, strlen(cyber), NULL, &paths));
    CHECK(paths.incomplete && paths.count == 1 && has_path(&paths, "generated/decls/entitydef/demons/cyber.decl"));
    entity("demons/cyber", "model", "demons/cyber");
    sh_resource_graph_begin(&frame, "model", "demons/cyber");
    sh_resource_graph_file("GENERATED\\MODELS\\shared.bmodel");
    sh_resource_graph_file("sound/not-delivered.wem");
    sh_resource_graph_reference("snapeditorentitydef", "palette");
    sh_resource_graph_end(&frame, 1);
    sh_resource_graph_begin(&frame, "snapeditorentitydef", "palette");
    sh_resource_graph_file("image/editor-preview.bimage"); sh_resource_graph_end(&frame, 1);
    CHECK(sh_package_map_resources(compiled, NULL, cyber, strlen(cyber), NULL, &paths));
    CHECK(!paths.incomplete && paths.count == 3);
    CHECK(has_path(&paths, "generated/models/shared.bmodel") && has_path(&paths, "sound/not-delivered.wem"));
    CHECK(!has_path(&paths, "image/editor-preview.bimage") && !has_path(&paths, "generated/decls/entitydef/replaced.decl"));
    CHECK(sh_package_references_add(&instance, "", "GENERATED/MODELS/shared.bmodel"));
    instance.incomplete = 1;
    CHECK(sh_package_map_resources(compiled, NULL, cyber, strlen(cyber), &instance, &paths));
    CHECK(paths.incomplete && paths.count == 3); /* Canonical duplicates collapse. */
    CHECK(!sh_package_map_resources(compiled, NULL, "{", 1, &instance, &paths));
    CHECK(!paths.count && !paths.items);
    CHECK(sh_package_references_add(&instance, "", "../invalid"));
    CHECK(!sh_package_map_resources(compiled, NULL, cyber, strlen(cyber), &instance, &paths));
    CHECK(!paths.count && !paths.items);
    sh_package_references_free(&instance); sh_package_references_free(&paths);
    sh_resource_graph_test_reset();
}

static void producer_policy_checks(sh_package_compilation *compiled)
{
    const char *output = "maps/smpnav/fixture/0/generated.baas_monster48";
    const char *inputs[] = {"generated/image/stock_body.bimage"};
    sh_package_references refs = {0};
    sh_package_owners owners = {0};
    sh_package_policy policy = {0};
    sh_resource_graph_frame frame;
    char error[1024];
    int complete = 0;
    sh_resource_graph_begin(&frame, "", output);
    sh_resource_graph_file("generated/models/shared.bmodel"); sh_resource_graph_end(&frame, 1);
    CHECK(sh_resource_graph_producer_inputs("navigation bake", output, inputs, 1));
    CHECK(sh_package_references_add(&refs, "", output));
    CHECK(sh_package_map_policy(compiled, NULL, "{}", 2, &refs, &policy, &owners, &complete, error, sizeof(error)));
    CHECK(complete && owners.bits == 28); /* Both intact model packages and the added input replacement. */
    CHECK(strstr(sh_json_object_get(&policy.strings, "en"), "Cyberdemon"));
    CHECK(strstr(sh_json_object_get(&policy.strings, "en"), "boss_tool"));
    sh_package_policy_free(&policy);
    inputs[0] = "vanilla-navigation-input";
    CHECK(sh_resource_graph_producer_inputs("navigation bake", output, inputs, 1));
    CHECK(sh_package_map_owners(compiled, NULL, "{}", 2, &refs, &owners, &complete));
    CHECK(complete && owners.bits == 12); /* Retire only the replaced stage's old package. */
    sh_resource_graph_begin(&frame, "", output); sh_resource_graph_end(&frame, 1);
    inputs[0] = "generated/decls/snapeditorentitydef/palette.decl";
    CHECK(sh_resource_graph_producer_inputs("navigation bake", output, inputs, 1));
    CHECK(sh_package_map_policy(compiled, NULL, "{}", 2, &refs, &policy, &owners, &complete, error, sizeof(error)));
    CHECK(complete && !sh_package_owners_count(&owners) && !policy.requirements.count && !policy.strings.count);
    sh_package_policy_free(&policy); sh_package_owners_free(&owners); sh_package_references_free(&refs);
    sh_resource_graph_test_reset();
}

static int inline_state(void *context, const char *class_name, const char *inherit,
    const char *edit, size_t length)
{
    size_t *count = context;
    CHECK(!strcmp(class_name, "Entity") && !strcmp(inherit, "stock/unknown"));
    CHECK(length == strlen(edit) && strstr(edit, "gameplay/child"));
    (*count)++; return 1;
}
static void inline_checks(sh_package_compilation *compiled)
{
    static const char json[] = "{\"entities\":[{\"entityDef\":{\"targetType\":\"idDeclEntityDef\","
        "\"className\":\"Entity\",\"inherit\":\"stock/unknown\",\"state\":{\"edit\":{\"target\":\"gameplay/child\"}}}}],"
        "\"editorVars\":{\"entityDef\":{\"targetType\":\"idDeclEntityDef\",\"className\":\"Preview\","
        "\"state\":{\"edit\":{\"target\":\"preview/child\"}}}},"
        "\"empty\":{\"~type\":\"idDeclEntityDef\",\"state\":{\"edit\":null}}}";
    sh_package_references references = {0};
    sh_package_policy policy = {0};
    char error[1024], name[] = "demons/cyber";
    size_t count = 0;
    sh_package_owners owners = {0};
    int complete;
    CHECK(sh_package_map_states(json, sizeof(json) - 1, inline_state, &count) && count == 1);
    CHECK(sh_package_references_add(&references, "entitydef", name));
    CHECK(sh_package_references_add(&references, "entitydef", name) && references.count == 1);
    name[0] = 'X'; /* Collected identities own their bytes. */
    references.incomplete = 1;
    CHECK(sh_package_map_owners(compiled, NULL, json, sizeof(json) - 1, &references, &owners, &complete));
    CHECK(owners.bits == 12 && !complete);
    CHECK(sh_package_map_policy(compiled, NULL, json, sizeof(json) - 1, &references,
        &policy, &owners, &complete, error, sizeof(error)) && owners.bits == 12);
    CHECK(strstr(sh_json_object_get(&policy.strings, "en"), "Cyberdemon"));
    sh_package_policy_free(&policy);
    CHECK(used(compiled, json) == 0); /* No canonical graph contamination. */
    CHECK(!sh_package_map_owners(compiled, NULL, "{", 1, &references, &owners, &complete) && !sh_package_owners_count(&owners) && !complete);
    sh_package_references_free(&references);
    CHECK(!references.count && !references.items && !references.incomplete);
    sh_package_owners_free(&owners);
}
int main(void)
{
    char temp[MAX_PATH], error[1024];
    sh_package_sources *sources = NULL;
    sh_package_compilation *compiled = NULL;
    sh_package_owners owners = {0};
    CHECK(GetTempPathA(sizeof(temp), temp)); CHECK(GetTempFileNameA(temp, "pu", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    create("overrides/a-editor/package.json", "{\"id\":\"editor\",\"name\":\"Editor\","
        "\"requirements\":{\"cvars\":{\"g_useImageBlackList\":0}},\"strings\":{\"en\":{\"local_tool\":\"Local tool\"}}}");
    create("overrides/a-editor/assets/generated/decls/snapeditorentitydef/palette.decl", "{ edit = { tool = 1; } }");
    create("overrides/a-editor/assets/generated/decls/entitydef/stock/unknown.decl",
        "{ class = \"idTest\"; editorVars { description = \"Exposed tool\"; } edit = { speed = 1; } }");
    create("overrides/b-replacement/package.json", "{\"id\":\"replacement\",\"name\":\"Replacement\"}");
    create("overrides/b-replacement/assets/generated/decls/entitydef/replaced.decl", "{ class = \"idTest\"; edit = { speed = 2; } }");
    create("overrides/c-cyber/package.json", "{\"id\":\"cyber\",\"name\":\"Cyberdemon\",\"strings\":{\"en\":{\"cyber\":\"Cyberdemon\"}}}");
    create("overrides/c-cyber/assets/generated/decls/entitydef/demons/cyber.decl", "{ edit = { encounter = \"cyber\"; } }");
    create("overrides/d-bosses/package.json", "{\"id\":\"bosses\",\"name\":\"Bosses\","
        "\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0}}}");
    create("overrides/d-bosses/editor/package.json", "{\"id\":\"tools\",\"name\":\"Boss tools\","
        "\"strings\":{\"en\":{\"boss_tool\":\"Boss tool\"}},"
        "\"hud\":{\"weapons\":{\"weapon/test/boss\":{\"ammo_display\":\"weapon\"}}}}");
    create("overrides/d-bosses/cyberdemon/package.json", "{\"id\":\"cyber\",\"name\":\"Cyberdemon\",\"strings\":{\"en\":{\"cyber\":\"Cyberdemon\"}}}");
    create("overrides/d-bosses/cyberdemon/assets/generated/decls/entitydef/demons/cyber.decl", "{ edit = { encounter = \"cyber\"; } }");
    create("overrides/c-cyber/assets/generated/models/shared.bmodel", "shared-model");
    create("overrides/d-bosses/cyberdemon/assets/generated/models/shared.bmodel", "shared-model");
    create("overrides/e-surface/package.json", "{\"id\":\"surface\",\"name\":\"Surface replacement\","
        "\"hud\":{\"weapons\":{\"weapon/test/surface\":{\"ammo_display\":\"weapon\"}}}}");
    create("overrides/e-surface/assets/generated/image/stock_body.bimage", "replacement-image");
    create("overrides/f-preview/package.json", "{\"id\":\"preview\",\"name\":\"Editor preview\"}");
    create("overrides/f-preview/assets/generated/image/editor_preview.bimage", "editor-preview");
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    if (!sources) goto done;
    compiled = sh_package_compile(sources, baseline, NULL, error, sizeof(error)); CHECK(compiled);
    if (!compiled) { fprintf(stderr, "%s\n", error); goto done; }
    CHECK(used(compiled, "{\"entities\":[{\"entityDef\":{\"inherit\":\"stock/unknown\",\"targetType\":\"idDeclEntityDef\"}},{\"entityDef\":{\"inherit\":\"stock/timeline\",\"targetType\":\"idDeclEntityDef\"}},{\"entityDef\":{\"inherit\":\"stock/lift\",\"targetType\":\"idDeclEntityDef\"}}]}") == 0);
    CHECK(used(compiled, "{\"entityDef\":{\"inherit\":\"replaced\",\"targetType\":\"idDeclEntityDef\"}}") == 2);
    CHECK(used(compiled, "{\"displayName\":\"demons/cyber\",\"description\":\"cyber\",\"inherit\":\"replaced\"}") == 0);
    CHECK(used(compiled, "{\"value\":\"palette\",\"targetType\":\"idDeclSnapEditorEntityDef\"}") == 0);
    CHECK(used(compiled, "{\"entityDef\":{\"inherit\":\"demons\\/cyber\",\"targetType\":\"idDeclEntityDef\"}}") == 12);
    CHECK(used(compiled, "{\"value\":\"cyber\",\"targetType\":\"idDeclEntityDef\"}") == 0);
    /* Editor previews may themselves reference gameplay resource types. Their
     * ancestor context must not turn vanilla authoring into a modded map. */
    CHECK(used(compiled, "{\"editorVars\":{\"preview\":{\"value\":\"demons/cyber\",\"targetType\":\"idDeclEntityDef\"}}}") == 0);
    CHECK(used(compiled, "{\"edit\":{\"editor\\u0056ars\":[{\"nested\":[{\"inherit\":\"replaced\",\"targetType\":\"idDeclEntityDef\"}]}]}}") == 0);
    CHECK(used(compiled, "{\"before\":{\"value\":\"demons/cyber\",\"targetType\":\"idDeclEntityDef\"},\"editorVars\":{\"value\":\"replaced\",\"targetType\":\"idDeclEntityDef\"}}") == 12);
    CHECK(used(compiled, "{\"editorVars\":{\"value\":\"demons/cyber\",\"targetType\":\"idDeclEntityDef\"},\"after\":{\"value\":\"replaced\",\"targetType\":\"idDeclEntityDef\"}}") == 2);
    owners.bits = 123;
    CHECK(!sh_package_map_owners(compiled, NULL, "{\"editorVars\":{\"invalid\":true,\"invalid\":false}}", strlen("{\"editorVars\":{\"invalid\":true,\"invalid\":false}}"), NULL, &owners, NULL)); CHECK(!sh_package_owners_count(&owners));
    owners.bits = 123;
    CHECK(!sh_package_map_owners(compiled, NULL, "{\"entityDef\":{\"inherit\":\"replaced\",\"targetType\":\"idDeclEntityDef\"},", strlen("{\"entityDef\":{\"inherit\":\"replaced\",\"targetType\":\"idDeclEntityDef\"},"), NULL, &owners, NULL)); CHECK(!sh_package_owners_count(&owners));
    policy_checks(compiled);
    inline_checks(compiled);
    dependency_checks(compiled);
    resource_path_checks(compiled);
    producer_policy_checks(compiled);
done:
    sh_package_owners_free(&owners);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources); cleanup();
    if (failures) return 1;
    puts("typed map root and dependency ownership checks passed"); return 0;
}
