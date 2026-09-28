#include "../src/backend/package_native_inline.h"

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
{ (void)context; return !strcmp(class_name, "idProp") && !strcmp(base_name, "idProp") ? 1 : 0; }
static int fixture_looked_up(void *context, size_t entity, const char *name)
{ (void)context; (void)entity; return !strcmp(name, "pkg/lookedup"); }
static const char *fixture_keep(void *context, size_t entity, const char *name, sh_decl_carrier_limit *limit)
{
    (void)context; (void)entity; *limit = SH_DECL_CARRIER_ENGINE;
    return !strcmp(name, "pkg/stock_changed") ? "its package also changes a stock definition globally" : NULL;
}
static const char *fixture_placement(void *context, const char *from, const char *base, sh_decl_carrier_limit *limit)
{
    (void)context; (void)from; *limit = SH_DECL_CARRIER_ENGINE;
    return strcmp(base, "snapmaps/props/base") ? "no SnapMap editor entry" : NULL;
}

/* Reflected schema: idProp { int health; float speed; idStr model; idList<int> list; } */
static int schema_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    (void)context;
    if (!strcmp(type.name, "idProp")) { shape->kind = SH_DECL_VALUE_OBJECT; return 1; }
    if (!strcmp(type.name, "idList<int>")) {
        shape->kind = SH_DECL_VALUE_COLLECTION; shape->element = (sh_decl_value_type){"int", ""};
        shape->item_key = "item"; shape->count_key = "num"; return 1;
    }
    if (!strcmp(type.name, "int") || !strcmp(type.name, "float") || !strcmp(type.name, "idStr")) {
        shape->kind = SH_DECL_VALUE_IGNORE; return 1;
    }
    return 0;
}
static int schema_field(void *context, sh_decl_value_type object, const char *key, sh_decl_value_type *field)
{
    (void)context;
    if (strcmp(object.name, "idProp")) return -1;
    if (!strcmp(key, "health")) { *field = (sh_decl_value_type){"int", ""}; return 1; }
    if (!strcmp(key, "speed")) { *field = (sh_decl_value_type){"float", ""}; return 1; }
    if (!strcmp(key, "model")) { *field = (sh_decl_value_type){"idStr", ""}; return 1; }
    if (!strcmp(key, "list")) { *field = (sh_decl_value_type){"idList<int>", ""}; return 1; }
    return -1;
}
static int schema_reference(void *context, const char *path, sh_decl_value_type type, const char *name, size_t length)
{ (void)context; (void)path; (void)type; (void)name; (void)length; return 1; }

#define ENTITY(inherit, state, version) \
    "{\"entityDef\":{\"className\":\"idProp\",\"inherit\":\"" inherit "\",\"name\":\"\",\"state\":" state \
    ",\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\",\"~version\":" version "},\"uniqueId\":1}"

static const sh_native_inline_site *site_for(const sh_native_inline_report *report, size_t entity)
{
    for (size_t i = 0; i < report->count; i++) if (report->sites[i].entity == entity) return report->sites + i;
    return NULL;
}

int main(void)
{
    sh_decl_dependency_schema schema = {NULL, schema_describe, schema_field, schema_reference, NULL, NULL, NULL, NULL};
    sh_native_inline_source source = {NULL, fixture_read, fixture_derives, &schema, fixture_looked_up, fixture_keep, 26,
        fixture_placement};
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
        ENTITY("pkg/hidden", "{\"edit\":{}}", "26")
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
    /* P shrinks the inherited list and the instance regrows it: one inline
     * layer would resurrect pruned items, so this use keeps its package. */
    site = site_for(&report, 2);
    CHECK(site && !site->converted && site->limit == SH_DECL_CARRIER_ENGINE);
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
    /* Native definitions are not sites; other state versions stay unconverted. */
    CHECK(!site_for(&report, 8));
    CHECK(strstr(out, ENTITY("snapmaps/props/base", "{\"edit\":{\"health\":3}}", "26")) != NULL);
    site = site_for(&report, 9); CHECK(site && !site->converted && strstr(site->reason, "version"));
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
    CHECK(report.count == 14);
    /* Bytes outside converted entity values are exact. */
    CHECK(strstr(out, "],\"version\":7,\"note\":\"ordinary \\\"string\\\" bytes stay exact\"}") != NULL);
    CHECK(!strncmp(out, "{\"entities\":[{\"entityDef\":", 26));
    if (failures)
        for (size_t i = 0; i < report.count; i++)
            fprintf(stderr, "site %zu %s: %s limit %d %s\n", report.sites[i].entity, report.sites[i].from,
                report.sites[i].converted ? "converted" : "kept", (int)report.sites[i].limit, report.sites[i].reason);
    free(out); sh_native_inline_report_free(&report);
    if (failures) { fprintf(stderr, "%d native inline check(s) failed\n", failures); return 1; }
    puts("native inline representation checks passed");
    return 0;
}
