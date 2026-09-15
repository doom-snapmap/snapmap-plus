#include "decl_compose.h"
#include "decl_polymorphic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); failures++; } } while (0)
typedef struct fixture { size_t references, gaps; char last_path[256]; } fixture;
static sh_decl_source view(const char *text) { return (sh_decl_source){text, strlen(text)}; }
static int describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    (void)context;
    if (!strcmp(type.name, "Root") || !strncmp(type.name, "State", 5)) shape->kind = SH_DECL_VALUE_OBJECT;
    else if (!strcmp(type.name, "Variant")) {
        shape->kind = SH_DECL_VALUE_POLYMORPHIC; shape->element = (sh_decl_value_type){"StateBase", ""};
    } else if (!strcmp(type.name, "int")) shape->kind = SH_DECL_VALUE_IGNORE;
    else if (!strcmp(type.name, "Resource")) shape->kind = SH_DECL_VALUE_REFERENCE;
    else if (!strcmp(type.name, "FixedVariants")) {
        shape->kind = SH_DECL_VALUE_COLLECTION; shape->element = (sh_decl_value_type){"Variant", ""};
        shape->count_known = 1; shape->count = 3; shape->item_key = "item";
    }
    return 1;
}
static int field(void *context, sh_decl_value_type owner, const char *key, sh_decl_value_type *type)
{
    (void)context;
    *type = (sh_decl_value_type){"int", ""};
    if (!strcmp(key, "variant") || !strcmp(key, "nested")) type->name = "Variant";
    else if (!strcmp(key, "slots")) type->name = "FixedVariants";
    else if (!strcmp(key, "target")) type->name = "Resource";
    else if (!strcmp(key, "custom")) type->name = "Unsupported";
    else if (!strcmp(owner.name, "StateB") && !strcmp(key, "y")) type->name = "Resource";
    else if (strcmp(key, "x") && strcmp(key, "y") && strcmp(key, "other")) return -1;
    return 1;
}
static int dynamic_type(void *context, sh_decl_value_type base, const char *name, size_t length, sh_decl_value_type *type)
{
    static const char *const names[] = {"StateA", "StateB", "StateC"};
    size_t i;
    (void)context;
    memset(type, 0, sizeof(*type));
    if (strcmp(base.name, "StateBase")) return 0;
    for (i = 0; i < 3; i++) if (strlen(names[i]) == length && !memcmp(names[i], name, length)) {
        *type = (sh_decl_value_type){names[i], ""}; return 1;
    }
    return 0;
}
static int reference(void *context, const char *path, sh_decl_value_type type, const char *name, size_t length)
{
    fixture *f = context;
    (void)type; (void)name; (void)length;
    f->references++; snprintf(f->last_path, sizeof(f->last_path), "%s", path); return 1;
}
static void gap(void *context, const char *path, sh_decl_value_type type, const char *reason)
{ fixture *f = context; (void)path; (void)type; (void)reason; f->gaps++; }
static sh_decl_dependency_schema schema(fixture *f)
{ return (sh_decl_dependency_schema){f, describe, field, reference, gap, dynamic_type}; }

static char *compose(const char *original, const char *a, const char *b, char *error)
{
    fixture f = {0};
    sh_decl_dependency_schema types = schema(&f);
    sh_decl_composition_schema input = {0};
    sh_decl_source sources[] = {view(a), view(b)};
    char *text;
    size_t length;
    input.types = &types; input.state_type = (sh_decl_value_type){"Root", ""};
    text = sh_decl_compose_typed(view(original), sources, 2, NULL, 0, &input, &length, error, 512, NULL);
    CHECK(text ? strlen(text) == length : !length);
    return text;
}

#define WRAP(c, state) "{ className = \"" c "\"; object = { " state " } }"
#define DEF(value) "{ edit = { variant = " value "; } }"
static void composition(void)
{
    const char *base = DEF(WRAP("StateA", "x = 1; y = 2;"));
    const char *a = DEF(WRAP("StateA", "x = 3; y = 2;"));
    const char *b = DEF(WRAP("StateA", "x = 1; y = 4;"));
    char error[512];
    char *ab = compose(base, a, b, error), *ba = compose(base, b, a, error);
    CHECK(ab && ba && !strcmp(ab, ba));
    if (ab) CHECK(strstr(ab, "x = 3;") && strstr(ab, "y = 4;") && strstr(ab, "className") < strstr(ab, "object"));
    else fprintf(stderr, "%s\n", error);
    free(ab); free(ba);
    ab = compose(DEF("\"StateA\""), DEF(WRAP("StateA", "x = 3;")), DEF("StateA"), error);
    CHECK(ab && strstr(ab, "x = 3;")); free(ab);
    ab = compose(DEF("{ className = \"StateA\"; }"),
        DEF(WRAP("StateA", "x = 3;")), DEF(WRAP("StateA", "y = 4;")), error);
    CHECK(ab && strstr(ab, "x = 3;") && strstr(ab, "y = 4;")); free(ab);
    ab = compose("{ edit = {} }", DEF(WRAP("StateA", "x = 3;")), DEF(WRAP("StateA", "y = 4;")), error);
    CHECK(ab && strstr(ab, "x = 3;") && strstr(ab, "y = 4;")); free(ab);
    ab = compose(base, a, DEF(WRAP("StateC", "x = 1; y = 2;")), error);
    CHECK(ab && strstr(ab, "StateC") && strstr(ab, "x = 3;")); free(ab);
    ab = compose(base, DEF(WRAP("StateB", "x = 1; y = 2;")), DEF(WRAP("StateC", "x = 1; y = 2;")), error);
    CHECK(!ab && strstr(error, "className")); free(ab);
    ab = compose(base, b, DEF(WRAP("StateB", "x = 1; y = 2;")), error);
    CHECK(!ab && strstr(error, "reader type")); free(ab);
    ab = compose(base, a, DEF(WRAP("StateA", "x = 5; y = 2;")), error);
    CHECK(!ab && strstr(error, "object.x")); free(ab);
    ab = compose(base, a, "{ edit = {} }", error);
    CHECK(!ab && strstr(error, "deletion")); free(ab);
    ab = compose(base, a, DEF(WRAP("Unknown", "x = 1; y = 2;")), error);
    CHECK(!ab && strstr(error, "selected object class")); free(ab);
    ab = compose(DEF("NULL"), DEF("NULL"), "{ edit = { variant = NULL; other = 1; } }", error);
    CHECK(ab && strstr(ab, "variant = NULL;")); free(ab);
    ab = compose(DEF("NULL"), a, b, error);
    CHECK(!ab && strstr(error, "destination state")); free(ab);
    ab = compose(DEF(WRAP("StateA", "custom = { x = 1; y = 2; }")),
        DEF(WRAP("StateA", "custom = { x = 3; y = 2; }")),
        DEF(WRAP("StateA", "custom = { x = 1; y = 4; }")), error);
    CHECK(!ab && strstr(error, "native reader")); free(ab);
    ab = compose("{ edit = { slots = { item[0] = " WRAP("StateA", "x = 1; y = 2;") "; } } }",
        "{ edit = { slots = { item[0] = " WRAP("StateA", "x = 3; y = 2;") "; } } }",
        "{ edit = { slots = { item[0] = " WRAP("StateA", "x = 1; y = 4;") "; } } }", error);
    CHECK(ab && strstr(ab, "x = 3;") && strstr(ab, "y = 4;")); free(ab);
}

static void reader_forms(void)
{
    static const struct { const char *text; int status; } cases[] = {
        {"{ variant = StateA; }", 1},
        {"{ variant = { className = \"StateA\"; } }", 1},
        {"{ variant = { className = \"StateA\"; object = {} } }", 1},
        {"{ variant = NULL; }", 0},
        {"{ variant = \"NULL\"; }", 0},
        {"{ variant = \"\"; }", 0},
        {"{ variant = {} }", 0},
        {"{ variant = { className = NULL; } }", 0},
        {"{ variant = { object = {} className = \"StateA\"; } }", 0},
        {"{ variant = { className = \"StateA\"; other = {} } }", 0},
        {"{ variant = { className = \"StateA\"; object = 2; } }", 0},
        {"{ variant = { className = \"StateA\"; object = {} extra = 1; } }", 0},
        {"{ variant = { className = \"StateA\"; className = \"StateB\"; } }", 0},
        {"{ variant = Missing; }", -1}
    };
    fixture f = {0};
    sh_decl_dependency_schema types = schema(&f);
    sh_decl_value_type base = {"StateBase", ""};
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sh_decl_node *tree = sh_decl_tree_parse_ordered(view(cases[i].text), NULL, 0);
        sh_decl_polymorphic_state state;
        const char *reason = NULL;
        int status = sh_decl_polymorphic_read(tree ? tree->children : NULL, base, &types, &state, &reason);
        CHECK(tree && status == cases[i].status);
        CHECK(status == 1 ? state.type.name && !strcmp(state.type.name, "StateA") : !state.type.name && reason);
        sh_decl_tree_free(tree);
    }
    {
        sh_decl_node *tree = sh_decl_tree_parse(view(cases[0].text), NULL, 0);
        sh_decl_polymorphic_state state;
        CHECK(tree);
        if (tree) {
            tree->children->reset = 1;
            CHECK(!sh_decl_polymorphic_read(tree->children, base, &types, &state, NULL));
            tree->children->reset = 0; types.dynamic_type = NULL;
            CHECK(!sh_decl_polymorphic_read(tree->children, base, &types, &state, NULL));
        }
        sh_decl_tree_free(tree);
    }
}

static void three_contributors(void)
{
    const char *base = DEF(WRAP("StateA", "x = 1; y = 2; target = \"old\";"));
    const char *authored[] = {
        DEF(WRAP("StateA", "x = 3; y = 2; target = \"old\";")),
        DEF(WRAP("StateA", "x = 1; y = 4; target = \"old\";")),
        DEF(WRAP("StateC", "x = 1; y = 2; target = \"new\";"))
    };
    static const unsigned orders[][3] = {{0,1,2},{0,2,1},{1,0,2},{1,2,0},{2,0,1},{2,1,0}};
    fixture f = {0};
    sh_decl_dependency_schema types = schema(&f);
    sh_decl_composition_schema input = {0};
    char *first = NULL, error[512];
    size_t i, j, length;
    input.types = &types; input.state_type = (sh_decl_value_type){"Root", ""};
    for (i = 0; i < sizeof(orders) / sizeof(orders[0]); i++) {
        sh_decl_source sources[3];
        char *output;
        for (j = 0; j < 3; j++) sources[j] = view(authored[orders[i][j]]);
        output = sh_decl_compose_typed(view(base), sources, 3, NULL, 0, &input, &length, error, sizeof(error), NULL);
        CHECK(output && strstr(output, "StateC") && strstr(output, "x = 3;") &&
            strstr(output, "y = 4;") && strstr(output, "target = \"new\";"));
        if (!i) first = output;
        else { CHECK(first && output && !strcmp(first, output)); free(output); }
        for (j = 0; j < 3; j++) CHECK(!strcmp(sources[j].text, authored[orders[i][j]]));
    }
    free(first);
    {
        char *output = compose(DEF(WRAP("StateA", "x = 1; y = 2;")),
            DEF("{ className = \"StateA\"; }"), DEF(WRAP("StateA", "x = 3; y = 2;")), error);
        CHECK(!output && strstr(error, "object")); free(output);
        output = compose(DEF(WRAP("StateA", "nested = " WRAP("StateA", "x = 1; y = 2;") ";")),
            DEF(WRAP("StateA", "nested = " WRAP("StateC", "x = 1; y = 2;") ";")),
            DEF(WRAP("StateA", "nested = " WRAP("StateA", "x = 3; y = 2;") ";")), error);
        CHECK(output && strstr(output, "StateC") && strstr(output, "x = 3;")); free(output);
    }
}

static void dependencies(void)
{
    fixture f = {0};
    sh_decl_dependency_schema types = schema(&f);
    sh_decl_dependency_result result;
    const char *text = "{ variant = " WRAP("StateA", "target = \"asset/a\"; nested = "
        WRAP("StateA", "target = \"asset/b\";") ";") "; }";
    sh_decl_node *root = sh_decl_tree_parse(view(text), NULL, 0);
    CHECK(root && sh_decl_state_dependencies(root, (sh_decl_value_type){"Root", ""}, &types, &result));
    CHECK(result.references == 2 && !result.gaps && !strcmp(f.last_path, "edit.variant.object.nested.object.target"));
    sh_decl_tree_free(root);
    text = "{ variant = { className = \"Unknown\"; } target = \"known/asset\"; }";
    root = sh_decl_tree_parse(view(text), NULL, 0);
    CHECK(root && !sh_decl_state_dependencies(root, (sh_decl_value_type){"Root", ""}, &types, &result));
    CHECK(result.references == 1 && result.gaps == 1 && !result.aborted); sh_decl_tree_free(root);
    text = "{ variant = { className = \"StateA\"; } }";
    root = sh_decl_tree_parse(view(text), NULL, 0);
    CHECK(root && sh_decl_state_dependencies(root, (sh_decl_value_type){"Root", ""}, &types, &result));
    CHECK(!result.references && !result.gaps); sh_decl_tree_free(root);
    text = "{ variant = {} target = \"known/asset\"; }";
    root = sh_decl_tree_parse(view(text), NULL, 0);
    CHECK(root && !sh_decl_state_dependencies(root, (sh_decl_value_type){"Root", ""}, &types, &result));
    CHECK(result.references == 1 && result.gaps == 1 && !result.aborted); sh_decl_tree_free(root);
}
int main(void)
{
    composition(); reader_forms(); three_contributors(); dependencies();
    if (failures) { fprintf(stderr, "%d polymorphic failures\n", failures); return 1; }
    puts("decl_polymorphic_test: PASS"); return 0;
}
