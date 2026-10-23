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
    const char *event_json;
    size_t event_json_length;
    char site_event[128], site_signature[64];
    size_t site_index, site_count, event_sites, ordinary_sites;
    int no_signature;
    char value_sites[4096];
    size_t text_values, json_values, old_values;
    int refuse_value, refuse_old_value;
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
    else if (!strcmp(type.name, "TestScript")) shape->kind = SH_DECL_VALUE_SCRIPT;
    else if (!strcmp(type.name, "TestScriptRestrict")) shape->kind = SH_DECL_VALUE_OBJECT;
    else if (!strcmp(type.name, "gameTeam_t")) { shape->kind = SH_DECL_VALUE_IGNORE; shape->plain = 1; }
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
    } else if (!strcmp(type.name, "TestFixedStrings")) {
        shape->kind = SH_DECL_VALUE_COLLECTION;
        shape->element.name = "idStr"; shape->element.ops = "";
        shape->count = 3; shape->count_known = 1;
    } else if (!strcmp(type.name, "TestStringArrays")) {
        shape->kind = SH_DECL_VALUE_COLLECTION;
        shape->element.name = "TestFixedStrings"; shape->element.ops = "";
        shape->item_key = "item"; shape->count_key = "num";
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
        else if (!strcmp(key, "strings")) out->name = "TestFixedStrings";
        else if (!strcmp(key, "stringArrays")) out->name = "TestStringArrays";
        else if (!strcmp(key, "eventCall")) out->name = "TestEventCall";
        else if (!strcmp(key, "script")) out->name = "TestScript";
        else if (!strcmp(key, "custom")) out->name = "UnsupportedTemplate";
        else if (!strcmp(key, "unknownProperty")) return -1;
        else return 0;
    } else if (!strcmp(owner.name, "TestModelInfo")) {
        if (!strcmp(key, "model")) { out->name = "idRenderModel"; out->ops = "*"; }
        else if (!strcmp(key, "editorModel")) out->name = "idStr";
        else return 0;
    } else if (!strcmp(owner.name, "TestScriptRestrict")) {
        if (!strcmp(key, "target")) { out->name = "idDeclEntityDef"; out->ops = "*"; }
        else if (!strcmp(key, "limitTurningOnly")) out->name = "int";
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

/* The one script class the fixture reflects, compatible with TestScript. */
static int dynamic_type(void *context, sh_decl_value_type base, const char *name, size_t length,
    sh_decl_value_type *selected)
{
    (void)context;
    *selected = (sh_decl_value_type){0};
    if (strcmp(base.name, "TestScript") || length != 19 || memcmp(name, "restrictPlayerInput", 19)) return 0;
    selected->name = "TestScriptRestrict"; selected->ops = "";
    return 1;
}

static int inspect_json(const char *json, fixture *f, sh_decl_dependency_result *result)
{
    sh_decl_dependency_schema schema = {f, describe, field, reference, gap, dynamic_type};
    sh_decl_value_type type = {"TestEntity", ""};
    return sh_decl_json_state_dependencies(json, strlen(json), type, &schema, result);
}

/* event_value: every value argument with its call's literal eventDef, index
 * and reader key, and each call once (index SIZE_MAX, key NULL), in both the
 * declaration and JSON walkers; decl arguments stay references. */
static int event_value(void *context, const char *path, const char *event, size_t event_length, size_t index,
    const char *key, const sh_decl_reference_site *site)
{
    fixture *f = (fixture *)context;
    size_t used = strlen(f->references);
    char at[24];
    const char *value = "";
    size_t value_length = 0;
    int written;
    (void)path;
    if (!key && site) { f->event_json = site->json; f->event_json_length = site->json_length; }
    if (index == SIZE_MAX) snprintf(at, sizeof(at), "call"); else snprintf(at, sizeof(at), "%zu", index);
    if (site && site->node) { value = site->node->compound ? "(compound)" : site->node->value; value_length = strlen(value); }
    else if (site && site->json) { value = site->json; value_length = site->json_length; }
    written = snprintf(f->references + used, sizeof(f->references) - used, "E|%.*s|%s|%s|%.*s\n",
        event ? (int)event_length : 1, event ? event : "-", at, key ? key : "-", (int)value_length, value);
    assert(written > 0 && (size_t)written < sizeof(f->references) - used);
    return 1;
}
static int event_reference_site(void *context, const char *path, sh_decl_value_type type, const char *name, size_t length,
    const sh_decl_reference_site *site)
{
    fixture *f = context;
    (void)path; (void)type; (void)name; (void)length;
    assert(site);
    if (!site->event_name) {
        assert(!site->event_name_length && !site->event_argument_count && !site->event_argument_signature);
        f->ordinary_sites++; return 1;
    }
    assert(site->event_name_length < sizeof(f->site_event));
    memcpy(f->site_event, site->event_name, site->event_name_length); f->site_event[site->event_name_length] = 0;
    f->site_index = site->event_argument_index; f->site_count = site->event_argument_count;
    f->no_signature = !site->event_argument_signature;
    if (site->event_argument_signature) {
        assert(strlen(site->event_argument_signature) < sizeof(f->site_signature));
        strcpy(f->site_signature, site->event_argument_signature);
    }
    f->event_sites++; return 1;
}
static void event_reference_checks(void)
{
    static const struct { const char *text, *json, *signature; size_t count, index; } cases[] = {
        {"num = 6; item[0] = { decl = { sound = \"x\"; } } item[1] = { entity = \"y\"; } "
         "item[2] = { vec3 = { x = 0; } } item[3] = { int = 1; } item[4] = { bool = false; } "
         "item[5] = { gameTeam_t = TEAM_UNASSIGNED; }",
         "\"num\":6,\"item[0]\":{\"decl\":{\"sound\":\"x\"}},\"item[1]\":{\"entity\":\"y\"},"
         "\"item[2]\":{\"vec3\":{\"x\":0}},\"item[3]\":{\"int\":1},\"item[4]\":{\"bool\":false},"
         "\"item[5]\":{\"gameTeam_t\":\"TEAM_UNASSIGNED\"}", "devibi", 6},
        {"num = 1; item[0] = { decl = { sound = \"x\"; } }",
         "\"num\":1,\"item[0]\":{\"decl\":{\"sound\":\"x\"}}", "d", 1},
        {"num = 6; item[0] = { decl = { sound = \"x\"; } } item[5] = { int = 1; }",
         "\"num\":6,\"item[0]\":{\"decl\":{\"sound\":\"x\"}},\"item[5]\":{\"int\":1}", "d????i", 6},
        {"num = 2; item[0] = { decl = { sound = \"x\"; } } item[5] = { int = 1; }",
         "\"num\":2,\"item[0]\":{\"decl\":{\"sound\":\"x\"}},\"item[5]\":{\"int\":1}", "d????i", 2},
        {"num = 1; item[5] = { decl = { sound = \"x\"; } }",
         "\"num\":1,\"item[5]\":{\"decl\":{\"sound\":\"x\"}}", "?????d", 1, 5},
        {"num = 3; item[0] = { decl = { sound = \"x\"; } } item[1] = NULL; item[2] = { NULL = NULL; }",
         "\"num\":3,\"item[0]\":{\"decl\":{\"sound\":\"x\"}},\"item[1]\":null,\"item[2]\":{\"NULL\":null}", "d00", 3},
        {"num = 2; item[0] = { decl = { sound = \"x\"; } } item[1] = { unknownReader = 1; }",
         "\"num\":2,\"item[0]\":{\"decl\":{\"sound\":\"x\"}},\"item[1]\":{\"unknownReader\":1}", NULL, 2},
        {"num = 2; item[0] = { decl = { sound = \"x\"; } } item[1] = { int = 1; bool = false; }",
         "\"num\":2,\"item[0]\":{\"decl\":{\"sound\":\"x\"}},\"item[1]\":{\"int\":1,\"bool\":false}", NULL, 2},
    };
    fixture f = {0};
    sh_decl_dependency_schema schema = {&f, describe, field, reference, gap, NULL, event_reference_site};
    sh_decl_value_type entity = {"TestEntity", ""};
    sh_decl_dependency_result result;
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) for (int json = 0; json <= 1; json++) {
        char input[2048];
        memset(&f, 0, sizeof(f));
        if (json) {
            snprintf(input, sizeof(input), "{\"eventCall\":{\"args\":{%s},\"\\neventHandle_t eventDef\":\"dropEntity\"}}", cases[i].json);
            assert(sh_decl_json_state_dependencies(input, strlen(input), entity, &schema, &result));
        } else {
            snprintf(input, sizeof(input), "{ edit = { eventCall = { args = { %s } eventDef = \"dropEntity\"; } } }", cases[i].text);
            sh_decl_node *root = sh_decl_tree_parse_ordered((sh_decl_source){input, strlen(input)}, NULL, 0);
            assert(root && sh_decl_entity_dependencies(root, entity, &schema, &result)); sh_decl_tree_free(root);
        }
        assert(f.event_sites == 1 && !f.ordinary_sites && !strcmp(f.site_event, "dropEntity") &&
            f.site_index == cases[i].index && f.site_count == cases[i].count && f.no_signature == !cases[i].signature &&
            (!cases[i].signature || !strcmp(f.site_signature, cases[i].signature)) && !result.gaps);
    }
    memset(&f, 0, sizeof(f));
    const char *json = "{\"inventoryItemDecl\":\"ordinary\"}";
    assert(sh_decl_json_state_dependencies(json, strlen(json), entity, &schema, &result) && f.ordinary_sites == 1 && !f.event_sites);
    /* Annotated native keys keep their own reader context; no dotted-path
     * reconstruction is needed by a reference consumer. */
    memset(&f, 0, sizeof(f));
    json = "{\"eventCall\":{\"eventDef\":\"dropEntity\",\"args\":{\"\\nnum\":1,"
        "\"\\nidEventArg item[0]\":{\"decl\":{\"sound\":\"x\"}}}}}";
    assert(sh_decl_json_state_dependencies(json, strlen(json), entity, &schema, &result) && f.event_sites == 1 &&
        !strcmp(f.site_signature, "d") && !f.site_index && f.site_count == 1);
    /* args.num counts records in source order. It is not a bound on index,
     * and records beyond that count are not native argument reads. */
    static const struct { const char *text, *json; size_t index; } malformed[] = {
        {"num = 1; item[5] = { decl = { sound = \"first\"; } } item[0] = { decl = { sound = \"unread\"; } }",
         "\"num\":1,\"item[5]\":{\"decl\":{\"sound\":\"first\"}},\"item[0]\":{\"decl\":{\"sound\":\"unread\"}}", 5},
        {"num = 2; item[0] = { decl = { sound = \"first\"; } } item[0] = { decl = { sound = \"unread\"; } }",
         "\"num\":2,\"item[0]\":{\"decl\":{\"sound\":\"first\"}},\"\\nidEventArg item[0]\":{\"decl\":{\"sound\":\"unread\"}}"},
        {"num = 2; item[0] = { decl = { sound = \"first\"; } } item[8] = { decl = { sound = \"unread\"; } }",
         "\"num\":2,\"item[0]\":{\"decl\":{\"sound\":\"first\"}},\"item[8]\":{\"decl\":{\"sound\":\"unread\"}}"},
        {"num = 2; item[0] = { decl = { sound = \"first\"; } } strange = { decl = { sound = \"unread\"; } }",
         "\"num\":2,\"item[0]\":{\"decl\":{\"sound\":\"first\"}},\"strange\":{\"decl\":{\"sound\":\"unread\"}}"},
    };
    for (size_t i = 0; i < sizeof(malformed) / sizeof(*malformed); i++) for (int is_json = 0; is_json <= 1; is_json++) {
        char input[1024]; memset(&f, 0, sizeof(f));
        if (is_json) {
            snprintf(input, sizeof(input), "{\"eventCall\":{\"eventDef\":\"dropEntity\",\"args\":{%s}}}", malformed[i].json);
            (void)sh_decl_json_state_dependencies(input, strlen(input), entity, &schema, &result);
        } else {
            snprintf(input, sizeof(input), "{ edit = { eventCall = { eventDef = \"dropEntity\"; args = { %s } } } }", malformed[i].text);
            sh_decl_node *root = sh_decl_tree_parse_ordered((sh_decl_source){input, strlen(input)}, NULL, 0);
            assert(root); (void)sh_decl_entity_dependencies(root, entity, &schema, &result); sh_decl_tree_free(root);
        }
        assert(result.gaps && result.references == 1 && f.event_sites == 1 && f.site_index == malformed[i].index &&
            f.no_signature && strstr(f.references, "|first\n") && !strstr(f.references, "unread"));
    }
    size_t count;
    /* Exact duplicate JSON keys are rejected by the JSON validator before
     * the typed walk; the annotated duplicate above reaches its index check. */
    memset(&f, 0, sizeof(f));
    json = "{\"eventCall\":{\"eventDef\":\"dropEntity\",\"args\":{\"num\":2,\"item[0]\":null,\"item[0]\":null}}}";
    assert(!sh_decl_json_state_dependencies(json, strlen(json), entity, &schema, &result) && result.aborted && !result.references);
    assert(sh_decl_event_argument_count("-2147483648", 11, &count) && count == 0);
    assert(sh_decl_event_argument_count(" 2 ", 3, &count) && count == 2);
    assert(!sh_decl_event_argument_count("+2", 2, &count));
    assert(!sh_decl_event_argument_count("2147483648", 10, &count));
    assert(!sh_decl_event_argument_count("1.5", 3, &count));
    memset(&f, 0, sizeof(f));
    json = "{\"eventCall\":{\"eventDef\":\"dropEntity\",\"args\":{\"num\":-1}}}";
    assert(sh_decl_json_state_dependencies(json, strlen(json), entity, &schema, &result) && !result.gaps && !result.references);
}
static void event_value_checks(void)
{
    sh_decl_dependency_schema schema = {0};
    sh_decl_dependency_result result;
    sh_decl_value_type entity = {"TestEntity", ""};
    fixture f = {0};
    schema.context = &f; schema.describe = describe; schema.field = field; schema.reference = reference;
    schema.gap = gap; schema.event_value = event_value;
    const char *text = "{ edit = { eventCall = { eventDef = \"findEntity\"; args = { num = 3; "
        "item[0] = { string = \"inlinestock/base_3\"; } item[1] = { entity = \"player1\"; } "
        "item[2] = { decl = { sound = \"play_noise_hat\"; } } } } } }";
    sh_decl_node *root = sh_decl_tree_parse((sh_decl_source){text, strlen(text)}, NULL, 0);
    assert(root && sh_decl_entity_dependencies(root, entity, &schema, &result) && !result.gaps);
    sh_decl_tree_free(root);
    assert(strstr(f.references, "E|findEntity|call|-|"));
    assert(strstr(f.references, "E|findEntity|0|string|\"inlinestock/base_3\""));
    assert(strstr(f.references, "E|findEntity|1|entity|\"player1\""));
    assert(!strstr(f.references, "|2|decl|") &&
        strstr(f.references, "edit.eventCall.args.item[2].decl|idSoundShader|play_noise_hat\n"));
    memset(&f, 0, sizeof(f));
    text = "{ edit = { eventCall = { eventDef = \"getName\"; } } }";
    root = sh_decl_tree_parse((sh_decl_source){text, strlen(text)}, NULL, 0);
    assert(root && sh_decl_entity_dependencies(root, entity, &schema, &result));
    sh_decl_tree_free(root);
    assert(!strcmp(f.references, "E|getName|call|-|\"getName\"\n"));
    memset(&f, 0, sizeof(f));
    text = "{ edit = { eventCall = { eventDef = \"setName\"; args = { num = 1; item[0] = { string = { a = 1; } } } } } }";
    root = sh_decl_tree_parse((sh_decl_source){text, strlen(text)}, NULL, 0);
    assert(root);
    (void)sh_decl_entity_dependencies(root, entity, &schema, &result);
    sh_decl_tree_free(root);
    assert(strstr(f.references, "E|setName|0|string|(compound)"));
    memset(&f, 0, sizeof(f));
    const char *json = "{\"eventCall\":{\"\\neventHandle_t eventDef\":\"setName\",\"args\":{\"num\":2,"
        "\"item[0]\":{\"string\":\"INLINESTOCK/BASE_3\"},\"item[1]\":{\"decl\":{\"sound\":\"play_noise_hat\"}}}}}";
    assert(sh_decl_json_state_dependencies(json, strlen(json), entity, &schema, &result) && !result.gaps);
    assert(strstr(f.references, "E|setName|call|-|"));
    assert(strstr(f.references, "E|setName|0|string|\"INLINESTOCK/BASE_3\""));
    assert(!strstr(f.references, "|1|decl|") &&
        strstr(f.references, "edit.eventCall.args.item[1].decl|idSoundShader|play_noise_hat\n"));
    memset(&f, 0, sizeof(f));
    json = "{\"eventCall\":{\"eventDef\":\"getName\"}}";
    assert(sh_decl_json_state_dependencies(json, strlen(json), entity, &schema, &result));
    assert(!strcmp(f.references, "E|getName|call|-|\"getName\"\n"));
    assert(f.event_json == strstr(json, "\"getName\"") && f.event_json_length == 9);
    /* Preserve the selected source token, including its JSON escaping; the
     * callback receives the last native eventDef member, not a decoded copy. */
    memset(&f, 0, sizeof(f));
    json = "{\"eventCall\":{\"eventDef\":\"setName\",\"\\neventHandle_t eventDef\":  \"get\\u004eame\"  }}";
    assert(sh_decl_json_state_dependencies(json, strlen(json), entity, &schema, &result));
    assert(!strcmp(f.references, "E|getName|call|-|\"get\\u004eame\"\n"));
    assert(f.event_json == strstr(json, "\"get\\u004eame\"") && f.event_json_length == 14);
    memset(&f, 0, sizeof(f));
    json = "{\"eventCall\":{\"eventDef\":null}}";
    assert(sh_decl_json_state_dependencies(json, strlen(json), entity, &schema, &result));
    assert(!strcmp(f.references, "E|-|call|-|null\n") && f.event_json == strstr(json, "null") && f.event_json_length == 4);
    memset(&f, 0, sizeof(f));
    json = "{\"eventCall\":{}}";
    assert(sh_decl_json_state_dependencies(json, strlen(json), entity, &schema, &result));
    assert(!strcmp(f.references, "E|-|call|-|\n") && !f.event_json && !f.event_json_length);
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
    assert(result.gaps == 6 && result.references == 3 && !result.aborted);
    assert(!strcmp(f.references, "edit.eventCall.args.item[0].md6Anim|idMD6Anim|models/anim.md6\n"
        "edit.eventCall.args.item[5].decl|idSoundShader|proven\n"
        "edit.eventCall.args.item[7].decl|idSoundShader|outside\n"));
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

/* M102: an empty quoted literal of a type whose reader is a declaration lookup
 * (empty_lookup) is reported as the lookup of _emptyName at the literal's own
 * site, in a map's JSON state as in declaration text. JSON null, the native
 * "NULL" spelling, an unquoted NULL, a non-declaration pointer and a plain
 * string keep naming nothing. */
typedef struct empty_fixture { fixture base; char sites[1024]; const char *json; } empty_fixture;
static int empty_lookup(void *context, sh_decl_value_type type)
{ (void)context; return !strcmp(type.name, "idDeclInventory") || !strcmp(type.name, "idDeclEntityDef"); }
static int empty_site(void *context, const char *path, sh_decl_value_type type,
    const char *name, size_t length, const sh_decl_reference_site *site)
{
    empty_fixture *f = context; size_t used = strlen(f->sites);
    (void)type;
    int written = site->json ? snprintf(f->sites + used, sizeof(f->sites) - used, "%s|%.*s|%zu+%zu|%.*s\n", path,
        (int)length, name, (size_t)(site->json - f->json), site->json_length, (int)site->json_length, site->json) :
        snprintf(f->sites + used, sizeof(f->sites) - used, "%s|%.*s|node %s\n", path, (int)length, name,
            site->node && site->node->value ? site->node->value : "(none)");
    assert(written > 0 && (size_t)written < sizeof(f->sites) - used);
    return 1;
}
static void empty_checks(void)
{
    empty_fixture f;
    sh_decl_dependency_result result;
    sh_decl_dependency_schema schema = {&f, describe, field, reference, gap, NULL, empty_site};
    const char *json = "{\"inventoryItemDecl\":\"\",\"name\":\"\",\"renderModelInfo\":{\"model\":\"\"},"
        "\"startingInventory\":{\"num\":3,\"item[0]\":{\"inventoryDecl\":null},\"item[1]\":{\"inventoryDecl\":\"NULL\"},"
        "\"item[2]\":{\"inventoryDecl\":\"\"}},\"links\":[\"\",\"kept\"]}";
    schema.empty_lookup = empty_lookup;
    memset(&f, 0, sizeof(f)); f.json = json;
    assert(sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"TestEntity", ""}, &schema, &result));
    if (strcmp(f.base.references, "edit.inventoryItemDecl|idDeclInventory|_emptyName\n"
            "edit.startingInventory.item[2].inventoryDecl|idDeclInventory|_emptyName\n"
            "edit.links.[0]|idDeclEntityDef|_emptyName\nedit.links.[1]|idDeclEntityDef|kept\n"))
        fprintf(stderr, "json empty references:\n%s", f.base.references);
    assert(!result.gaps && result.references == 4);
    assert(!strcmp(f.base.references, "edit.inventoryItemDecl|idDeclInventory|_emptyName\n"
        "edit.startingInventory.item[2].inventoryDecl|idDeclInventory|_emptyName\n"
        "edit.links.[0]|idDeclEntityDef|_emptyName\nedit.links.[1]|idDeclEntityDef|kept\n"));
    /* Each site is the quoted "" itself, which a binder rewrites in place. */
    {
        size_t first = (size_t)(strstr(json, "\"inventoryItemDecl\":\"\"") - json) + 20;
        size_t item = (size_t)(strstr(json, "\"item[2]\":{\"inventoryDecl\":\"\"") - json) + 27;
        size_t link = (size_t)(strstr(json, "\"links\":[\"\"") - json) + 9;
        char expected[512];
        snprintf(expected, sizeof(expected), "edit.inventoryItemDecl|_emptyName|%zu+2|\"\"\n"
            "edit.startingInventory.item[2].inventoryDecl|_emptyName|%zu+2|\"\"\n"
            "edit.links.[0]|_emptyName|%zu+2|\"\"\n", first, item, link);
        if (strncmp(f.sites, expected, strlen(expected))) fprintf(stderr, "json empty sites:\n%s\nexpected:\n%s", f.sites, expected);
        assert(!strncmp(f.sites, expected, strlen(expected)));
    }
    /* Without the callback the same state names nothing for "" (other schema users). */
    memset(&f, 0, sizeof(f)); f.json = json; schema.empty_lookup = NULL;
    assert(sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"TestEntity", ""}, &schema, &result));
    assert(!result.gaps && result.references == 1 && !strcmp(f.base.references, "edit.links.[1]|idDeclEntityDef|kept\n"));
    /* Declaration text: "" is the lookup, NULL stores NULL, "NULL" is a name. */
    {
        const char *text = "{ edit = { inventoryItemDecl = \"\"; name = \"\"; renderModelInfo = { model = \"\"; }"
            " startingInventory = { num = 2; item[0] = { inventoryDecl = NULL; } item[1] = { inventoryDecl = \"NULL\"; } } } }";
        sh_decl_node *root = sh_decl_tree_parse((sh_decl_source){text, strlen(text)}, NULL, 0);
        memset(&f, 0, sizeof(f)); schema.empty_lookup = empty_lookup;
        assert(root && sh_decl_entity_dependencies(root, (sh_decl_value_type){"TestEntity", ""}, &schema, &result));
        assert(!result.gaps && result.references == 2);
        assert(!strcmp(f.base.references, "edit.inventoryItemDecl|idDeclInventory|_emptyName\n"
            "edit.startingInventory.item[1].inventoryDecl|idDeclInventory|NULL\n"));
        assert(strstr(f.sites, "edit.inventoryItemDecl|_emptyName|node \"\"\n"));
        sh_decl_tree_free(root);
    }
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
    /* A script object's class selector types its members wherever the engine
     * placed it: a saved map writes {"limitTurningOnly":false,"object":
     * "restrictPlayerInput"} (alphabetical), an authored state may lead with it. */
    memset(&f, 0, sizeof(f));
    assert(inspect_json("{\"script\":{\"limitTurningOnly\":false,\"object\":\"restrictPlayerInput\","
        "\"target\":\"entity/after\"}}", &f, &result));
    assert(result.references == 1 && !result.gaps && !result.aborted);
    assert(strstr(f.references, "edit.script.target|idDeclEntityDef|entity/after\n"));
    memset(&f, 0, sizeof(f));
    assert(inspect_json("{\"script\":{\"object\":\"restrictPlayerInput\",\"target\":\"entity/first\"}}", &f, &result));
    assert(result.references == 1 && !result.gaps && strstr(f.references, "entity/first"));
    memset(&f, 0, sizeof(f));
    assert(inspect_json("{\"script\":{\"~type\":\"idSSObject\",\"object\":\"NULL\"}}", &f, &result));
    assert(!result.references && !result.gaps && !result.aborted);
    assert(inspect_json("{\"script\":\"restrictPlayerInput\"}", &f, &result) && !result.gaps);
    memset(&f, 0, sizeof(f));
    assert(!inspect_json("{\"script\":{\"limitTurningOnly\":true}}", &f, &result));
    assert(!result.references && result.gaps == 1 && !result.aborted);
    memset(&f, 0, sizeof(f));
    assert(!inspect_json("{\"script\":{\"limitTurningOnly\":true,\"object\":\"unknownClass\"}}", &f, &result));
    assert(!result.references && result.gaps == 1 && !result.aborted);
    /* The engine writes an absent file resource as the number 0; an unquoted
     * scalar names nothing and is neither a reference nor a gap. */
    memset(&f, 0, sizeof(f));
    assert(inspect_json("{\"renderModelInfo\":{\"model\":0},\"inventoryItemDecl\":0,"
        "\"links\":[\"entity/kept\",0,-1],\"startingInventory\":[{\"inventoryDecl\":false}]}", &f, &result));
    assert(result.references == 1 && !result.gaps && !result.aborted && strstr(f.references, "entity/kept"));
    memset(&f, 0, sizeof(f));
    assert(!inspect_json("{\"renderModelInfo\":{\"model\":{\"name\":\"object\"}}}", &f, &result));
    assert(!result.references && result.gaps == 1 && !result.aborted);
    memset(&f, 0, sizeof(f)); f.refuse = 1;
    assert(!inspect_json(json, &f, &result) && result.aborted);
}

static int primitive_old_value(void *context, const char *path, sh_decl_value_type type, const sh_decl_node *node)
{
    fixture *f = context;
    (void)path; (void)type; assert(node);
    f->old_values++;
    return !f->refuse_old_value;
}
static int primitive_value_site(void *context, const char *path, sh_decl_value_type type,
    const sh_decl_reference_site *site)
{
    fixture *f = context;
    size_t used = strlen(f->value_sites);
    int n;
    assert(type.name && site && site->owner_type && site->member);
    if (site->node) {
        assert(!site->json && !site->json_length);
        f->text_values++;
    } else {
        assert(site->json && site->json_length);
        f->json_values++;
    }
    n = snprintf(f->value_sites + used, sizeof(f->value_sites) - used, "%s|%s|%s\n",
        path, site->owner_type, site->member);
    assert(n > 0 && (size_t)n < sizeof(f->value_sites) - used);
    return !f->refuse_value;
}
static void primitive_site_checks(void)
{
    fixture f = {0};
    sh_decl_dependency_schema schema = {&f, describe, field, reference, gap};
    sh_decl_dependency_result result;
    assert(!schema.value_site && !schema.value && !schema.event_value);
    schema.value = primitive_old_value;
    schema.value_site = primitive_value_site;
    const char *text = "{ name = \"n\"; renderModelInfo = { editorModel = \"m\"; } "
        "strings = { strings[0] = \"a\"; strings[1] = \"b\"; } "
        "stringArrays = { num = 1; item[0] = { item[0] = \"c\"; item[1] = \"d\"; } } }";
    sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){text, strlen(text)}, NULL, 0);
    assert(tree && sh_decl_state_dependencies(tree, (sh_decl_value_type){"TestEntity", ""}, &schema, &result));
    assert(f.text_values == 6 && f.old_values == 6 && !f.json_values);
    assert(strstr(f.value_sites, "edit.name|TestEntity|name\n"));
    assert(strstr(f.value_sites, "edit.renderModelInfo.editorModel|TestModelInfo|editorModel\n"));
    assert(strstr(f.value_sites, "edit.strings.strings[1]|TestEntity|strings\n"));
    assert(strstr(f.value_sites, "edit.stringArrays.item[0].item[1]|TestEntity|stringArrays\n"));
    memset(&f, 0, sizeof(f)); f.refuse_old_value = 1;
    assert(!sh_decl_state_dependencies(tree, (sh_decl_value_type){"TestEntity", ""}, &schema, &result));
    assert(result.aborted && f.old_values == 1 && !f.text_values);
    memset(&f, 0, sizeof(f)); f.refuse_value = 1;
    assert(!sh_decl_state_dependencies(tree, (sh_decl_value_type){"TestEntity", ""}, &schema, &result));
    assert(result.aborted && f.old_values == 1 && f.text_values == 1);
    sh_decl_tree_free(tree);
    {
        const char *json = "{\"name\":\"n\",\"renderModelInfo\":{\"editorModel\":\"m\"},"
            "\"strings\":{\"strings[0]\":\"a\",\"strings[1]\":\"b\"},"
            "\"stringArrays\":[[\"c\",\"d\"]]}";
        memset(&f, 0, sizeof(f));
        assert(sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"TestEntity", ""}, &schema, &result));
        assert(f.json_values == 6 && !f.old_values && !f.text_values);
        assert(strstr(f.value_sites, "edit.name|TestEntity|name\n"));
        assert(strstr(f.value_sites, "edit.renderModelInfo.editorModel|TestModelInfo|editorModel\n"));
        assert(strstr(f.value_sites, "edit.strings.strings[1]|TestEntity|strings\n"));
        assert(strstr(f.value_sites, "edit.stringArrays.[0].[1]|TestEntity|stringArrays\n"));
        memset(&f, 0, sizeof(f)); f.refuse_value = 1;
        assert(!sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"TestEntity", ""}, &schema, &result));
        assert(result.aborted && f.json_values == 1 && !f.old_values);
    }
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
    empty_checks();
    event_checks();
    event_value_checks();
    event_reference_checks();
    primitive_site_checks();
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
        /* Only the positional inherit is a header; the later one is an inert
         * state statement, while each later edit updates the inherited edit. */
        assert(!result.gaps && result.references == 3 && !result.aborted);
        assert(!strcmp(f.references, "inherit|idDeclEntityDef|base/first\n"
            "edit.inventoryItemDecl|idDeclInventory|first\n"
            "edit.inventoryItemDecl|idDeclInventory|second\n"));
        sh_decl_tree_free(state);
    }
    puts("decl_dependencies_test: PASS");
    return 0;
}
