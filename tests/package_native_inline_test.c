#include "../src/backend/package_native_inline.h"
#include "../src/backend/config_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

typedef struct fixture_decl { const char *name, *text; int served; } fixture_decl;
static const fixture_decl decls[] = {
    {"snapmaps/props/base", "{ class = \"idProp\"; edit = { health = 10; speed = 1; model = \"md6/base\";"
        " list = { num = 3; item[0] = 1; item[1] = 2; item[2] = 3; } } }", 0},
    {"pkg/prop", "{ inherit = \"snapmaps/props/base\"; class = \"idProp\"; editorVars { placeable = true; }"
        " edit = { health = 50; speed = 0.35; model = \"md6/custom\"; } }", 1},
    {"pkg/prop2", "{ inherit = \"pkg/prop\"; edit = { speed = 2.5; } }", 1},
    {"pkg/shrink", "{ inherit = \"snapmaps/props/base\"; edit = { list = { num = 1; } } }", 1},
    {"pkg/reset", "{ inherit = \"snapmaps/props/base\"; edit = { list = ! { num = 1; item[0] = 9; } } }", 1},
    {"pkg/apostrophe", "{ inherit = \"snapmaps/props/base\"; edit = { model = \"it's\"; } }", 1},
    {"pkg/lookedup", "{ inherit = \"snapmaps/props/base\"; edit = { health = 7; } }", 1},
    {"pkg/intdecimal", "{ inherit = \"snapmaps/props/base\"; edit = { health = 2.5; } }", 1},
    {"pkg/unknown", "{ inherit = \"snapmaps/props/base\"; edit = { mystery = 3; } }", 1},
    {"pkg/stock_changed", "{ inherit = \"snapmaps/props/base\"; edit = { health = 11; } }", 1},
    /* Package replacements of the stock base under private names. */
    {"smp/replaced", "{ class = \"idProp\"; edit = { health = 12; speed = 1; model = \"md6/base\";"
        " list = { num = 3; item[0] = 1; item[1] = 2; item[2] = 3; } } }", 1},
    {"smp/dropped", "{ class = \"idProp\"; edit = { health = 12; } }", 1},
    /* A native base with no SnapMap editor entry. */
    {"snapmaps/props/hidden", "{ class = \"idProp\"; edit = { health = 10; } }", 0},
    {"pkg/hidden", "{ inherit = \"snapmaps/props/hidden\"; edit = { health = 5; } }", 1},
    /* Package chains with no native ancestor: a served root and its child. */
    {"pkg/root", "{ class = \"idProp\"; edit = { health = 4; model = \"md6/root\"; } }", 1},
    {"pkg/rootchild", "{ inherit = \"pkg/root\"; edit = { speed = 3; } }", 1},
    {"pkg/rootkept", "{ class = \"idProp\"; edit = { health = 6; } }", 1},
    /* A served child selecting another reflected class than its stock base. */
    {"pkg/variant", "{ inherit = \"snapmaps/props/base\"; class = \"idPropVariant\"; edit = { health = 7; } }", 1},
    /* Nested reflected values can fold into the entity carrier. Declaration
     * pointers within them remain names, never substitute resource bodies. */
    {"pkg/nested", "{ inherit = \"snapmaps/props/base\"; edit = { nested = { damage = \"damage/package\"; gain = 2.5; }"
        " nestedList = { num = 2; item[0] = { damage = \"damage/other\"; gain = 0.5; }"
        " item[1] = { damage = NULL; gain = 1.5; } } } }", 1},
    {"pkg/nested_unknown", "{ inherit = \"snapmaps/props/base\"; edit = { nested = { unknown = 1; } } }", 1},
    {"pkg/exponent", "{ inherit = \"snapmaps/props/base\"; edit = { speed = 1e5; } }", 1},
    {"pkg/exponent_loss", "{ inherit = \"snapmaps/props/base\"; edit = { speed = 1e-11; } }", 1},
    {"pkg/exponent_empty", "{ inherit = \"snapmaps/props/base\"; edit = { speed = 1e+; } }", 1},
    {"snapmaps/volume/blocking", "{ inherit = \"snapmaps/props/base\"; edit = { health = 10; } }", 1},
    {"pkg/blocking", "{ inherit = \"snapmaps/volume/blocking\"; edit = { health = 5; } }", 1},
    {"snapmaps/event/survive", "{ inherit = \"snapmaps/props/base\"; edit = { health = 6; } }", 1},
    {"snapmaps/event/survive_deprecated", "{ inherit = \"snapmaps/props/base\"; edit = { health = 5; } }", 1},
    {"snapmaps/damageable_props/default", "{ inherit = \"snapmaps/props/base\"; edit = { health = 6; } }", 1},
};

static int fixture_read(void *context, const char *name, sh_decl_source *text, int *served, char **replaces)
{
    (void)context;
    for (size_t i = 0; i < sizeof(decls) / sizeof(decls[0]); i++) if (!strcmp(decls[i].name, name)) {
        char *copy = _strdup(decls[i].text);
        if (!copy) return -1;
        *text = (sh_decl_source){copy, strlen(copy)}; *served = decls[i].served;
        if (replaces && !strncmp(name, "smp/", 4) && !(*replaces = _strdup("snapmaps/props/base"))) { free(copy); return -1; }
        return 1;
    }
    return 0;
}
static int fixture_derives(void *context, const char *class_name, const char *base_name)
{
    (void)context;
    if (!strcmp(base_name, "idProp")) return !strcmp(class_name, "idProp") || !strcmp(class_name, "idPropVariant");
    return !strcmp(class_name, "idPropVariant") && !strcmp(base_name, "idPropVariant");
}
static int fixture_looked_up(void *context, size_t entity, const char *name)
{ (void)context; (void)entity; return !strcmp(name, "pkg/lookedup"); }
static const char *fixture_keep(void *context, size_t entity, const char *name, sh_decl_carrier_limit *limit)
{
    (void)context; (void)entity; *limit = SH_DECL_CARRIER_ENGINE;
    return !strcmp(name, "pkg/stock_changed") ? "its package also changes a stock definition globally" : NULL;
}
static const char *fixture_placement(void *context, const char *from, const char *base, sh_decl_carrier_limit *limit)
{
    (void)context; *limit = SH_DECL_CARRIER_ENGINE;
    /* A placeable package root: the empty base (a class-only definition) of
     * pkg/rootchild is admitted, pkg/rootkept's is not. */
    if (!*base && !strcmp(from, "pkg/rootchild")) return NULL;
    return strcmp(base, "snapmaps/props/base") ? "no SnapMap editor entry" : NULL;
}

/* Reflected schema includes nested objects and a collection containing native
 * declaration pointers. These links carry names, not declaration bodies (R136). */
static int schema_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    (void)context;
    if (!strcmp(type.name, "idProp") || !strcmp(type.name, "idPropVariant") || !strcmp(type.name, "idNested")) {
        shape->kind = SH_DECL_VALUE_OBJECT; return 1;
    }
    if (!strcmp(type.name, "idDeclDamage") && !strcmp(type.ops, "*")) {
        shape->kind = SH_DECL_VALUE_REFERENCE; return 1;
    }
    if (!strcmp(type.name, "idList<idNested>")) {
        shape->kind = SH_DECL_VALUE_COLLECTION; shape->element = (sh_decl_value_type){"idNested", ""};
        shape->item_key = "item"; shape->count_key = "num"; return 1;
    }
    if (!strcmp(type.name, "idList<int>")) {
        shape->kind = SH_DECL_VALUE_COLLECTION; shape->element = (sh_decl_value_type){"int", ""};
        shape->item_key = "item"; shape->count_key = "num"; return 1;
    }
    if (!strcmp(type.name, "int") || !strcmp(type.name, "float") || !strcmp(type.name, "idStr") || !strcmp(type.name, "bool")) {
        shape->kind = SH_DECL_VALUE_IGNORE; return 1;
    }
    return 0;
}
static int schema_field(void *context, sh_decl_value_type object, const char *key, sh_decl_value_type *field)
{
    (void)context;
    if (!strcmp(object.name, "idNested")) {
        if (!strcmp(key, "damage")) { *field = (sh_decl_value_type){"idDeclDamage", "*"}; return 1; }
        if (!strcmp(key, "gain")) { *field = (sh_decl_value_type){"float", ""}; return 1; }
        return -1;
    }
    if (strcmp(object.name, "idProp") && strcmp(object.name, "idPropVariant")) return -1;
    if (!strcmp(key, "isVisible") || !strcmp(key, "isNetworkStatic")) { *field = (sh_decl_value_type){"bool", ""}; return 1; }
    if (!strcmp(key, "health")) { *field = (sh_decl_value_type){"int", ""}; return 1; }
    if (!strcmp(key, "speed")) { *field = (sh_decl_value_type){"float", ""}; return 1; }
    if (!strcmp(key, "model")) { *field = (sh_decl_value_type){"idStr", ""}; return 1; }
    if (!strcmp(key, "list")) { *field = (sh_decl_value_type){"idList<int>", ""}; return 1; }
    if (!strcmp(key, "nested")) { *field = (sh_decl_value_type){"idNested", ""}; return 1; }
    if (!strcmp(key, "nestedList")) { *field = (sh_decl_value_type){"idList<idNested>", ""}; return 1; }
    return -1;
}
static int schema_reference(void *context, const char *path, sh_decl_value_type type, const char *name, size_t length)
{ (void)context; (void)path; (void)type; (void)name; (void)length; return 1; }

#define ENTITY_BARE(inherit, state) \
    "{\"entityDef\":{\"inherit\":\"" inherit "\",\"name\":\"\",\"state\":" state \
    ",\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\"},\"uniqueId\":1}"
#define ENTITY_NO_CLASS(inherit, state, version) \
    "{\"entityDef\":{\"inherit\":\"" inherit "\",\"name\":\"\",\"state\":" state \
    ",\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\",\"~version\":" version "},\"uniqueId\":1}"
#define ENTITY(inherit, state, version) \
    "{\"entityDef\":{\"className\":\"idProp\",\"inherit\":\"" inherit "\",\"name\":\"\",\"state\":" state \
    ",\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\",\"~version\":" version "},\"uniqueId\":1}"

static const sh_native_inline_site *site_for(const sh_native_inline_report *report, size_t entity)
{
    for (size_t i = 0; i < report->count; i++) if (report->sites[i].entity == entity) return report->sites + i;
    return NULL;
}

#ifdef SH_NATIVE_INLINE_TESTING
static char *version_path(const char *json, const char *path)
{
    char *value = _strdup(json), *keys = _strdup(path), *next = NULL;
    if (!value || !keys) { free(value); free(keys); return NULL; }
    for (char *key = strtok_s(keys, ".", &next); key; key = strtok_s(NULL, ".", &next)) {
        sh_json_object object = {0};
        if (!sh_native_json_parse_object(value, strlen(value), 64, &object)) { free(value); value = NULL; break; }
        const char *member = sh_json_object_get(&object, key);
        char *copy = member ? _strdup(member) : NULL;
        sh_json_object_free(&object); free(value); value = copy;
        if (!value) break;
    }
    free(keys); return value;
}
static void version_case(const char *inherit, int version, const char *edit,
    const char *path, const char *expected, int expected_result)
{
    char json[4096], version_json[32];
    snprintf(json, sizeof(json), "{\"inherit\":\"%s\",\"className\":\"old\",\"state\":{\"edit\":%s},"
        "\"~version\":%d,\"ordinary\":{\"bytes\":[\"x\",  1.0]}}", inherit, edit, version);
    snprintf(version_json, sizeof(version_json), "%d", version);
    char original[4096]; strcpy(original, json);
    char *copy = NULL;
    int result = sh_native_inline_test_version_copy(json, 26, &copy);
    if (result != expected_result) fprintf(stderr, "version %d %s: result %d expected %d\n", version, inherit, result, expected_result);
    CHECK(result == expected_result); CHECK(!strcmp(json, original));
    if (result == 1 && copy) {
        char *value = version_path(copy, path);
        if ((expected && (!value || strcmp(value, expected))) || (!expected && value))
            fprintf(stderr, "version %d %s %s: %s expected %s\n", version, inherit, path, value ? value : "<absent>", expected ? expected : "<absent>");
        CHECK(expected ? value && !strcmp(value, expected) : !value); free(value);
        value = version_path(copy, "ordinary"); CHECK(value && !strcmp(value, "{\"bytes\":[\"x\",  1.0]}")); free(value);
        value = version_path(copy, "~version"); CHECK(value && !strcmp(value, version_json)); free(value);
    } else CHECK(copy == NULL);
    free(copy);
}
static void version_policy_checks(void)
{
    /* Both sides of every actual native threshold; unrelated values stay raw. */
    version_case("prop/moveable/bucket", 14, "{}", "inherit", "\"snapmaps/moveable_props/bucket\"", 1);
    version_case("prop/moveable/bucket", 15, "{}", "inherit", "\"prop/moveable/bucket\"", 1);
    version_case("snapmaps/moveable_props/battery", 14, "{}", "className", "\"idProp\"", 1);
    version_case("snapmaps/moveable_props/battery", 15, "{}", "className", "\"old\"", 1);
    version_case("snapmaps/event/survive", 15, "{\"eventDurationMS\":125}", "state.edit.eventDuration", "{\"milliseconds\":125}", 1);
    version_case("snapmaps/event/survive", 16, "{\"eventDurationMS\":125}", "state.edit.eventDurationMS", "125", 1);
    /* These delimiters had whitespace before ';' in retained source. An
     * extraction that merges neighboring calls misses this distinct selector. */
    version_case("snapmaps/capture_point/demon_signal", 15, "{\"animationTimeMS\":4}", "state.edit.animationTime", "{\"milliseconds\":4}", 1);
    version_case("snapmaps/damageable_props/default", 15, "{\"fxFadeOutMS\":5,\"autoRespawnTimeMS\":6}", "state.edit.autoRespawnTime", "{\"milliseconds\":6}", 1);
    version_case("snapmaps/logic/repeater", 15, "{\"delayBetweenRepeats\":9}", "state.edit.delayBetweenRepeats", "{\"milliseconds\":9}", 1);
    version_case("snapmaps/action/player/give_droppable", 16, "{}", "state.edit.droppableDecl", "\"power_core/power_core_normal\"", 1);
    version_case("snapmaps/action/player/take_droppable", 16, "{\"droppableDecl\":\"flag/flag_blue\"}", "state.edit.droppableDecl", NULL, 1);
    version_case("snapmaps/action/player/take_droppable", 17, "{\"droppableDecl\":\"flag/flag_blue\"}", "state.edit.droppableDecl", "\"flag/flag_blue\"", 1);
    version_case("snapmaps/settings/hud_settings", 17, "{\"topLeft\":{\"type\":\"SNAP_HUD_TOP_LEADERBOARD\",\"other\":1}}",
        "state.edit.topLeft.type", "\"SNAP_HUD_GAME_PAR_TIME\"", 1);
    version_case("snapmaps/settings/hud_settings", 18, "{\"bottom\":{\"type\":\"SNAP_HUD_TOP_LEADERBOARD\"}}",
        "state.edit.bottom.type", "\"SNAP_HUD_TOP_LEADERBOARD\"", 1);
    version_case("snapmaps/event/survive", 18, "{}", "inherit", "\"snapmaps/event/survive_deprecated\"", 1);
    version_case("snapmaps/event/survive", 19, "{}", "inherit", "\"snapmaps/event/survive\"", 1);
    version_case("snapmaps/spawners/spawn_weapon", 19, "{\"spawnOnStart\":true}", "state.edit.spawnOnStart", NULL, 1);
    version_case("snapmaps/spawners/spawn_weapon", 20, "{\"spawnOnStart\":true}", "state.edit.spawnOnStart", "true", 1);
    version_case("snapmaps/visblockers/industrial/cap_01", 20, "{}", "className", "\"idSnapMapCapEntity\"", 1);
    version_case("snapmaps/visblockers/industrial/cap_01", 21, "{}", "className", "\"old\"", 1);
    version_case("snapmaps/spawners/spawn_backpack", 21, "{\"colorParam\":{\"constantValue\":{\"r\":1,\"g\":2,\"b\":3,\"a\":0,\"other\": 4.0}}}",
        "state.edit.colorParam.constantValue", "{\"r\":0,\"g\":0,\"b\":0,\"a\":0,\"other\":4.0}", 1);
    version_case("snapmaps/spawners/spawn_backpack", 22, "{\"colorParam\":{\"constantValue\":{\"a\":0,\"r\":1}}}", "state.edit.colorParam.constantValue.r", "1", 1);
    version_case("snapmaps/spawners/spawn_backpack", 21, "{\"colorParam\":{\"constantValue\":{\"a\":-1}}}", "state.edit.colorParam.constantValue.b", "0", 1);
    version_case("snapmaps/spawners/spawn_backpack", 21, "{\"colorParam\":{\"constantValue\":{\"a\":0.5,\"r\":7}}}", "state.edit.colorParam.constantValue.r", "7", 1);
    version_case("snapmaps/spawners/spawn_backpack", 21, "{\"colorParam\":{\"constantValue\":{\"r\":7}}}", "state.edit.colorParam.constantValue.a", NULL, 1);
    version_case("snapmaps/message/codex_message", 23, "{}", "state.edit.image", "\"textures/guis/codex_images/uac_personnel/vega\"", 1);
    version_case("snapmaps/message/codex_message", 24, "{}", "state.edit.image", NULL, 1);
    version_case("snapmaps/message/codex_message", 23, "{\"image\":\"textures/guis/snapmaps/codex_images/vega\"}", "state.edit.image", NULL, 1);
    version_case("snapmaps/volume/blocking", 24, "{\"isVisible\":false,\"isNetworkStatic\":null}", "state.edit.isVisible", NULL, 1);
    version_case("snapmaps/volume/blocking", 24, "{}", "state.edit.isVisible", "false", 1);
    version_case("snapmaps/volume/blocking", 25, "{\"isVisible\":true,\"isNetworkStatic\":false}", "state.edit.isVisible", "true", 1);
    version_case("snapmaps/volume/blocking", 25, "{\"isVisible\":true,\"isNetworkStatic\":false}", "state.edit.isNetworkStatic", NULL, 1);
    version_case("snapmaps/volume/blocking", 26, "{\"isNetworkStatic\":false}", "state.edit.isNetworkStatic", "false", 1);
    version_case("Snapmaps/volume/blocking", 0, "{\"isVisible\":true}", "state.edit.isVisible", "true", 1);
    version_case("snapmaps\\\\volume\\\\blocking", 0, "{\"isVisible\":true}", "state.edit.isVisible", "true", 1);
    version_case("pkg/blocking", 0, "{\"isVisible\":true}", "state.edit.isVisible", "true", 1);
    /* Native integer and real checks differ. Conversion cannot approximate
     * error-domain integers or coerce strings/containers into milliseconds. */
    const char *numbers[] = {"-2.75", "true", "null", "1e3", "2147483647.0", "-2147483648.0"};
    const char *wrapped[] = {"{\"milliseconds\":-2}", "{\"milliseconds\":1}", "{\"milliseconds\":0}",
        "{\"milliseconds\":1000}", "{\"milliseconds\":2147483647}", "{\"milliseconds\":-2147483648}"};
    for (size_t i = 0; i < sizeof(numbers) / sizeof(*numbers); i++) {
        char edit[128]; snprintf(edit, sizeof(edit), "{\"eventDurationMS\":%s,\"eventDuration\":{\"milliseconds\":9}}", numbers[i]);
        version_case("snapmaps/event/survive", 15, edit, "state.edit.eventDuration", wrapped[i], 1);
    }
    const char *unsupported[] = {"2147483647", "-2147483648", "\"100\"", "[]", "{}", "1e400"};
    for (size_t i = 0; i < sizeof(unsupported) / sizeof(*unsupported); i++) {
        char edit[128]; snprintf(edit, sizeof(edit), "{\"eventDurationMS\":%s}", unsupported[i]);
        version_case("snapmaps/event/survive", 15, edit, "state.edit", NULL, 0);
    }
    version_case("snapmaps/damageable_props/default", 15, "{\"fxFadeOutMS\":5,\"autoRespawnTimeMS\":\"bad\"}", "state.edit", NULL, 0);
    version_case("pkg/prop", 27, "{}", "state.edit", NULL, 0);
    /* Failure after an earlier mutation still returns no candidate. Retrying
     * from the exact original starts clean and cannot strand partial output. */
    const char *allocation = "{\"inherit\":\"snapmaps/damageable_props/default\",\"state\":{\"edit\":{\"fxFadeOutMS\":5,\"autoRespawnTimeMS\":6}},\"~version\":15}";
    long allocation_failures = 0; int allocation_success = 0;
    for (long fail = 0; fail < 512; fail++) {
        char *copy = NULL;
        sh_native_inline_test_version_alloc_fail_after(fail);
        int result = sh_native_inline_test_version_copy(allocation, 26, &copy);
        CHECK(result == -1 || result == 1);
        if (result == -1) { CHECK(copy == NULL); allocation_failures++; }
        else {
            char *value = version_path(copy, "state.edit.autoRespawnTime");
            CHECK(value && !strcmp(value, "{\"milliseconds\":6}")); free(value); allocation_success = 1;
        }
        free(copy);
        if (allocation_success) break;
    }
    CHECK(allocation_failures > 10 && allocation_success);
    sh_native_inline_test_version_alloc_fail_after(-1);
    char *copy = NULL; CHECK(sh_native_inline_test_version_copy(allocation, 26, &copy) == 1); free(copy);
    const char *native_bytes = "{\"inherit\":\"pkg/prop\",\"state\":{\"edit\":{}},\"~version\":0,"
        "\"ordinary\\u0000tail\":\"\xff\",\"array\":[ 1.0,  2 ]}";
    copy = NULL; CHECK(sh_native_inline_test_version_copy(native_bytes, 26, &copy) == 1);
    CHECK(copy && strstr(copy, "\"ordinary\\u0000tail\":\"\xff\"") && strstr(copy, "[ 1.0,  2 ]")); free(copy);
    copy = NULL;
    CHECK(sh_native_inline_test_version_copy("{\"inherit\":\"snapmaps/volume/blocking\\u0000tail\",\"state\":{\"edit\":{}},\"~version\":24}", 26, &copy) == 0);
    CHECK(copy == NULL); free(copy);
}
static void version_integration_checks(const sh_native_inline_source *source)
{
    const char *json = "{\"entities\":["
        ENTITY("snapmaps/event/survive", "{\"edit\":{\"health\":99}}", "18") ","
        ENTITY("snapmaps/volume/blocking", "{\"edit\":{\"isVisible\":true,\"isNetworkStatic\":null}}", "24") ","
        ENTITY("pkg/blocking", "{\"edit\":{\"isVisible\":true,\"isNetworkStatic\":true}}", "0") ","
        ENTITY("snapmaps/damageable_props/default", "{\"edit\":{\"fxFadeOutMS\":5,\"autoRespawnTimeMS\":\"bad\"}}", "15") ","
        ENTITY("pkg/prop", "{\"edit\":{}}", "30") "],\"outside\": [  1.0 ]}";
    sh_native_inline_report report = {0}; size_t length = 0; char error[512];
    char *out = sh_native_inline_document(json, strlen(json), source, &length, &report, error, sizeof(error));
    CHECK(out != NULL);
    if (!out) return;
    const sh_native_inline_site *site = site_for(&report, 0);
    CHECK(site && site->converted && !strcmp(site->from, "snapmaps/event/survive_deprecated"));
    site = site_for(&report, 1); CHECK(site && site->converted);
    site = site_for(&report, 2); CHECK(site && site->converted);
    /* Original private inherit does not select its ancestor's old-version
     * boolean removal. Neither field may be upgraded a second time later. */
    CHECK(strstr(out, "\"isVisible\":true") != NULL && strstr(out, "\"isNetworkStatic\":true") != NULL);
    site = site_for(&report, 3); CHECK(site && !site->converted && !strcmp(site->from, "snapmaps/damageable_props/default"));
    CHECK(strstr(out, ENTITY("snapmaps/damageable_props/default", "{\"edit\":{\"fxFadeOutMS\":5,\"autoRespawnTimeMS\":\"bad\"}}", "15")) != NULL);
    site = site_for(&report, 4); CHECK(site && !site->converted);
    CHECK(strstr(out, ENTITY("pkg/prop", "{\"edit\":{}}", "30")) != NULL);
    CHECK(strstr(out, "],\"outside\": [  1.0 ]}") != NULL);
    char *again = NULL; sh_native_inline_report second = {0}; size_t again_length = 0;
    again = sh_native_inline_document(out, length, source, &again_length, &second, error, sizeof(error));
    CHECK(again && again_length == length && !memcmp(again, out, length));
    free(again); sh_native_inline_report_free(&second); free(out); sh_native_inline_report_free(&report);
    sh_native_inline_test_version_alloc_fail_after(0);
    out = sh_native_inline_document(json, strlen(json), source, &length, &report, error, sizeof(error));
    CHECK(out && length == strlen(json) && !memcmp(out, json, length) && !error[0]);
    for (size_t i = 0; i < report.count; i++) CHECK(!report.sites[i].converted);
    free(out); sh_native_inline_report_free(&report);
    sh_native_inline_test_version_alloc_fail_after(-1);
}
#endif

/* Plumbing double for the native evaluator: it returns a configured status
 * per entity and records which sites asked. It proves classification, fallback
 * and acceptance authority only, never native equivalence. */
static sh_native_eval_status fake_by_entity[64];
static unsigned long long fake_asked;
static char fake_from[64], fake_base[96];
static sh_native_eval_status fake_evaluate(void *context, size_t entity, const char *from, const char *base,
    const sh_decl_node *target, const sh_decl_node *candidate, char *detail, size_t capacity)
{
    (void)context;
    if (!target || !sh_decl_tree_member(target, "inherit")) return SH_NATIVE_EVAL_FAILED;
    if (entity >= 64) return SH_NATIVE_EVAL_FAILED;
    fake_asked |= 1ull << entity;
    if (entity == 2) {
        snprintf(fake_from, sizeof(fake_from), "%s", from ? from : "");
        snprintf(fake_base, sizeof(fake_base), "%s", base ? base : "");
    }
    snprintf(detail, capacity, "%s", candidate ? "candidate supplied" : "no candidate");
    return fake_by_entity[entity];
}
static void fake_reset(sh_native_eval_status all)
{
    for (size_t i = 0; i < 64; i++) fake_by_entity[i] = all;
    fake_asked = 0; fake_from[0] = fake_base[0] = 0;
}

int main(void)
{
    sh_decl_dependency_schema schema = {NULL, schema_describe, schema_field, schema_reference, NULL, NULL, NULL, NULL};
    sh_native_inline_source source = {NULL, fixture_read, fixture_derives, &schema, fixture_looked_up, fixture_keep, 26,
        fixture_placement, fake_evaluate};
    /* Plumbing verdicts: EQUAL unless a case below says otherwise. */
    fake_reset(SH_NATIVE_EVAL_EQUAL);
#ifdef SH_NATIVE_INLINE_TESTING
    version_policy_checks();
    version_integration_checks(&source);
    /* The shrink/regrow site's native evaluation is unavailable in the main run. */
    fake_by_entity[2] = SH_NATIVE_EVAL_UNAVAILABLE;
#endif
    const char *document = "{\"entities\":["
        ENTITY("pkg/prop", "{\"edit\":{\"health\":99,\"spawnPosition\":{\"x\":1.5}}}", "26") ","
        ENTITY("pkg/prop2", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/shrink", "{\"edit\":{\"list\":{\"num\":3}}}", "26") ","
        ENTITY("pkg/reset", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/apostrophe", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/lookedup", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/intdecimal", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/unknown", "{\"edit\":{}}", "26") ","
        ENTITY("snapmaps/props/base", "{\"edit\":{\"health\":3}}", "26") ","
        ENTITY("pkg/prop", "{\"edit\":{}}", "25") ","
        ENTITY("pkg/prop", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/stock_changed", "{\"edit\":{}}", "26") ","
        ENTITY("smp/replaced", "{\"edit\":{}}", "26") ","
        ENTITY("smp/dropped", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/hidden", "{\"edit\":{}}", "26") ","
        ENTITY_NO_CLASS("pkg/prop", "{\"edit\":{\"health\":42}}", "26") ","
        ENTITY_BARE("pkg/prop", "{\"edit\":{}}") ","
        ENTITY("pkg/rootchild", "{\"edit\":{\"health\":8}}", "26") ","
        ENTITY("pkg/rootkept", "{\"edit\":{}}", "26") ","
        ENTITY_NO_CLASS("pkg/rootchild", "{\"edit\":{\"health\":8}}", "26") ","
        ENTITY_NO_CLASS("pkg/variant", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/variant", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/nested", "{\"edit\":{\"nested\":{\"gain\":4}}}", "26") ","
        ENTITY("pkg/nested_unknown", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/exponent", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/exponent_loss", "{\"edit\":{}}", "26") ","
        ENTITY("pkg/exponent_empty", "{\"edit\":{}}", "26")
        "],\"version\":7,\"note\":\"ordinary \\\"string\\\" bytes stay exact\"}";
    sh_native_inline_report report = {0};
    size_t length = 0; char error[512];
    char *out = sh_native_inline_document(document, strlen(document), &source, &length, &report, error, sizeof(error));
    if (!out) fprintf(stderr, "document: %s\n", error);
    CHECK(out != NULL);
    if (!out) return 1;

    /* The first use converts: inherit the native ancestor, carry P's edits and
     * keep the instance's own values (health 99, spawnPosition) verbatim. */
    const char *first = "{\"entityDef\":{\"className\":\"idProp\",\"inherit\":\"snapmaps/props/base\",\"name\":\"\","
        "\"state\":{\"edit\":{\"health\":99,\"speed\":0.349999994,\"model\":\"md6/custom\",\"spawnPosition\":{\"x\":1.5}}},"
        "\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\",\"~version\":26},\"uniqueId\":1}";
    if (!strstr(out, first)) fprintf(stderr, "converted document:\n%s\n", out);
    CHECK(strstr(out, first) != NULL);
    const sh_native_inline_site *site = site_for(&report, 0);
    CHECK(site && site->converted && !strcmp(site->from, "pkg/prop") && !strcmp(site->to, "snapmaps/props/base"));
    /* A two-level chain folds both layers; an equal second use converts too. */
    CHECK(strstr(out, "\"state\":{\"edit\":{\"health\":50,\"speed\":2.5,\"model\":\"md6/custom\"}}") != NULL);
    site = site_for(&report, 1); CHECK(site && site->converted);
    site = site_for(&report, 10); CHECK(site && site->converted);
    /* P shrinks the inherited list and the instance regrows it. The source model
     * cannot decide the regrown slots and no native evaluator is bound, so this
     * use keeps its package as a product gap, not a native limit. */
    site = site_for(&report, 2);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_GAP && strstr(site->reason, "is unavailable"));
    CHECK(strstr(out, ENTITY("pkg/shrink", "{\"edit\":{\"list\":{\"num\":3}}}", "26")) != NULL);
    /* No carrier spelling for a reset block or an apostrophe: native limits. */
    site = site_for(&report, 3); CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE);
    site = site_for(&report, 4); CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE);
    /* A site whose definition other map logic looks up by name keeps it. */
    site = site_for(&report, 5); CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE);
    /* A decimal for an int field and an unverified field are product gaps. */
    site = site_for(&report, 6); CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_GAP);
    site = site_for(&report, 7); CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_GAP &&
        strstr(site->reason, "edit.mystery"));
    /* Native definitions are not sites. Older private identities normalize
     * without selecting their stock ancestor's native upgrade operations. */
    CHECK(!site_for(&report, 8));
    CHECK(strstr(out, ENTITY("snapmaps/props/base", "{\"edit\":{\"health\":3}}", "26")) != NULL);
    site = site_for(&report, 9); CHECK(site && site->converted);
    /* A caller-kept global requirement stays on the package route. */
    site = site_for(&report, 11);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE && strstr(site->reason, "globally"));
    /* A package replacement of a stock identity becomes that stock identity
     * plus one inline layer when exact; one that omits stock values keeps its
     * package, a product gap rather than a native limit. */
    site = site_for(&report, 12);
    CHECK(site && site->converted && !strcmp(site->from, "smp/replaced") && !strcmp(site->to, "snapmaps/props/base"));
    site = site_for(&report, 13);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_GAP && strstr(site->reason, "omitted stock values"));
    /* SnapMap's validator admits only palette definitions: a base without an
     * editor entry would make the converted map unloadable. */
    site = site_for(&report, 14);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE && strstr(site->reason, "editor entry"));
    /* D7: the native value reader needs only state. Without className the class
     * inherits along the chain, so the use converts and keeps no className;
     * without ~version the native policy starts at 0, selecting the original
     * private identity. An accepted conversion then explicitly emits 26. */
    site = site_for(&report, 15);
    CHECK(site && site->converted && !strcmp(site->to, "snapmaps/props/base"));
    CHECK(strstr(out, "{\"entityDef\":{\"inherit\":\"snapmaps/props/base\",\"name\":\"\",\"state\":{\"edit\":"
        "{\"health\":42,\"speed\":0.349999994,\"model\":\"md6/custom\"}}") != NULL);
    site = site_for(&report, 16);
    CHECK(site && site->converted);
    CHECK(strstr(out, ENTITY_NO_CLASS("snapmaps/props/base", "{\"edit\":{\"health\":50,\"speed\":0.349999994,\"model\":\"md6/custom\"}}", "26")) != NULL);
    /* D7: a chain with no native ancestor folds over an empty inherit, which
     * the native reader accepts without a lookup; SnapMap placement then
     * judges that base like any other (admitted here for pkg/rootchild, not
     * for pkg/rootkept). */
    site = site_for(&report, 17);
    CHECK(site && site->converted && !strcmp(site->to, ""));
    CHECK(strstr(out, "\"inherit\":\"\",\"name\":\"\",\"state\":{\"edit\":{\"health\":8,\"model\":\"md6/root\",\"speed\":3}}")
        != NULL);
    site = site_for(&report, 18);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE && strstr(site->reason, "editor entry"));
    /* D7 class identity: without className, the converted value states the
     * class its original chain gave whenever the new base would give another:
     * the empty base gives none (a package root's class), and a served child
     * selecting another reflected class than its stock base keeps it. A value
     * stating className keeps its own. */
    site = site_for(&report, 19);
    CHECK(site && site->converted && !strcmp(site->to, ""));
    CHECK(strstr(out, "{\"entityDef\":{\"className\":\"idProp\",\"inherit\":\"\",\"name\":\"\",\"state\":{\"edit\":"
        "{\"health\":8,\"model\":\"md6/root\",\"speed\":3}}") != NULL);
    site = site_for(&report, 20);
    CHECK(site && site->converted && !strcmp(site->to, "snapmaps/props/base"));
    CHECK(strstr(out, "{\"entityDef\":{\"className\":\"idPropVariant\",\"inherit\":\"snapmaps/props/base\"") != NULL);
    site = site_for(&report, 21);
    CHECK(site && site->converted && !strcmp(site->to, "snapmaps/props/base"));
    /* The value without className whose base gives the same class (site 15)
     * states none. */
    CHECK(strstr(out, "{\"entityDef\":{\"inherit\":\"snapmaps/props/base\",\"name\":\"\",\"state\":{\"edit\":"
        "{\"health\":42,") != NULL);
    /* The instance's nested override wins, the sibling reference survives,
     * and references in nested rows remain names or NULL. A referenced
     * declaration is deliberately unavailable through fixture_read: conversion
     * must not invent an inline resource body for the native pointer carrier. */
    site = site_for(&report, 22);
    CHECK(site && site->converted && !strcmp(site->to, "snapmaps/props/base"));
    CHECK(strstr(out, "\"nested\":{\"damage\":\"damage/package\",\"gain\":4}") != NULL);
    CHECK(strstr(out, "\"nestedList\":{\"num\":2,\"item[0]\":{\"damage\":\"damage/other\",\"gain\":0.5},"
        "\"item[1]\":{\"damage\":null,\"gain\":1.5}}") != NULL);
    /* Unknown nested fields stay on the original package route; recognizing
     * the enclosing object does not establish its children's native readers. */
    site = site_for(&report, 23);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_GAP && strstr(site->reason, "edit.nested.unknown"));
    CHECK(strstr(out, ENTITY("pkg/nested_unknown", "{\"edit\":{}}", "26")) != NULL);
    /* Safe exponent spelling uses native state; a value the carrier would
     * round away keeps the original package rather than changing behavior. */
    site = site_for(&report, 24);
    CHECK(site && site->converted && !strcmp(site->to, "snapmaps/props/base"));
    CHECK(strstr(out, "\"state\":{\"edit\":{\"speed\":100000.0}}") != NULL);
    site = site_for(&report, 25);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE && strstr(site->reason, "edit.speed"));
    CHECK(strstr(out, ENTITY("pkg/exponent_loss", "{\"edit\":{}}", "26")) != NULL);
    site = site_for(&report, 26);
    CHECK(site && site->converted && !strcmp(site->to, "snapmaps/props/base"));
    CHECK(strstr(out, "\"state\":{\"edit\":{\"speed\":1.0}}") != NULL);
    CHECK(report.count == 26);
    /* Bytes outside converted entity values are exact. */
    CHECK(strstr(out, "],\"version\":7,\"note\":\"ordinary \\\"string\\\" bytes stay exact\"}") != NULL);
    CHECK(!strncmp(out, "{\"entities\":[{\"entityDef\":", 26));
    if (failures)
        for (size_t i = 0; i < report.count; i++)
            fprintf(stderr, "site %zu %s: %s limit %d %s\n", report.sites[i].entity, report.sites[i].from,
                report.sites[i].converted ? "converted" : "kept", (int)report.sites[i].limit, report.sites[i].reason);
    free(out); sh_native_inline_report_free(&report);

    /* Each non-EQUAL verdict keeps site 2's package as a GAP with its own
     * reason; EQUAL lets the native verdict accept a candidate the source
     * model doubts. No verdict is ever an ENGINE limit. */
    {
        static const struct { sh_native_eval_status status; const char *reason; } cases[] = {
            {SH_NATIVE_EVAL_UNEQUAL, "found a different state"}, {SH_NATIVE_EVAL_UNSUPPORTED, "does not cover a value"},
            {SH_NATIVE_EVAL_FAILED, "evaluation failed"}, {SH_NATIVE_EVAL_UNAVAILABLE, "is unavailable"},
            {SH_NATIVE_EVAL_EQUAL, NULL}};
        for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
            sh_native_inline_report evaluated = {0};
            fake_reset(SH_NATIVE_EVAL_EQUAL); fake_by_entity[2] = cases[c].status;
            char *again = sh_native_inline_document(document, strlen(document), &source, &length, &evaluated, error, sizeof(error));
            CHECK(again != NULL);
            const sh_native_inline_site *shrink = site_for(&evaluated, 2);
            CHECK((fake_asked & (1ull << 2)) && !strcmp(fake_from, "pkg/shrink") && fake_base[0]);
            CHECK(shrink && shrink->limit != SH_DECL_CARRIER_ENGINE);
            if (cases[c].reason)
                CHECK(shrink && !shrink->converted && shrink->limit == SH_DECL_CARRIER_GAP &&
                    strstr(shrink->reason, "differs") && strstr(shrink->reason, cases[c].reason) &&
                    strstr(shrink->reason, "candidate supplied"));
            else
                CHECK(shrink && !strstr(shrink->reason, "native evaluation"));
            site = site_for(&evaluated, 0); CHECK(site && site->converted);
            free(again); sh_native_inline_report_free(&evaluated);
        }
    }
    /* Without a native evaluator nothing converts, even where the source model
     * agrees: every site keeps its complete package and the bytes are exact. */
    {
        sh_native_inline_report kept = {0};
        source.evaluate = NULL;
        char *unchanged = sh_native_inline_document(document, strlen(document), &source, &length, &kept, error, sizeof(error));
        CHECK(unchanged != NULL && !strcmp(unchanged, document));
        size_t agreed = 0;
        for (size_t i = 0; i < kept.count; i++) {
            CHECK(!kept.sites[i].converted && kept.sites[i].limit != SH_DECL_CARRIER_EXACT);
            if (strstr(kept.sites[i].reason, "the source model agrees (native evaluation is unavailable)")) agreed++;
        }
        CHECK(agreed >= 2);
        free(unchanged); sh_native_inline_report_free(&kept);
        source.evaluate = fake_evaluate;
    }
    if (failures) { fprintf(stderr, "%d native inline check(s) failed\n", failures); return 1; }
    puts("native inline representation checks passed");
    return 0;
}
