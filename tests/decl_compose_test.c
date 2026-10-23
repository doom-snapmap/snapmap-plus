#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "decl_compose.h"

static int failures;
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s\n", __LINE__, #condition); failures++; } } while (0)

static sh_decl_source view(const char *text)
{
    sh_decl_source result = {text, strlen(text)};
    return result;
}

static char *compose(const char *base, const char *a, const char *b,
                       const sh_decl_collection_rule *rules, size_t rule_count,
                       char *error, size_t capacity)
{
    sh_decl_source sources[2] = {view(a), view(b)};
    size_t length = 999;
    char *result = sh_decl_compose(view(base), sources, 2, rules, rule_count,
                                    &length, error, capacity, NULL);
    CHECK(result ? length == strlen(result) : length == 0);
    return result;
}

static void fields(void)
{
    {
        sh_decl_source inputs[] = {
            view("{ edit = { x = 0; y = 1; } }"), view("{ edit = { x = 2; y = 0; } }"),
            view("{ edit = { x = 3; y = 0; } }"), view("{ edit = { x = 2; y = 0; z = 7; } }")
        };
        unsigned char participants[4] = {9,9,9,9};
        sh_decl_conflict cause = {0};
        char reason[512]; size_t length;
        char *output = sh_decl_compose_resource_report("entitydef", view("{ edit = { x = 0; y = 0; } }"),
            inputs, 4, NULL, 0, NULL, &length, reason, sizeof(reason), &cause, participants);
        CHECK(!output && cause.precise_sources && !participants[0] && participants[1] && participants[2] && participants[3]);
        free(output);
    }
    const char *base = "{ class = \"idAI2\"; editorVars { color = ( 1, 1, 1, 1 ); } edit = { x = 1; y = 2; } }";
    const char *a = "{ class = \"idAI2\"; editorVars { color = ( 1, 1, 1, 1 ); } edit = { x = 3; y = 2; } }";
    const char *b = "{ class = \"idAI2\"; editorVars { color = ( 1, 1, 1, 1 ); } edit = { x = 1; y = 4; } }";
    char error[256];
    char *ab = compose(base, a, b, NULL, 0, error, sizeof(error));
    char *ba = compose(base, b, a, NULL, 0, error, sizeof(error));
    CHECK(ab && ba);
    if (ab && ba) {
        CHECK(!strcmp(ab, ba)); CHECK(strstr(ab, "x = 3;")); CHECK(strstr(ab, "y = 4;"));
        CHECK(strstr(ab, "editorVars {")); CHECK(strstr(ab, "color = ( 1, 1, 1, 1 );"));
    }
    free(ab); free(ba);
    ab = compose("{ edit = { x = 1; } }", "{ edit = { x = 2; } }", "{ edit = { x = 3; } }",
                 NULL, 0, error, sizeof(error));
    CHECK(!ab); CHECK(strstr(error, "edit.x")); free(ab);
    ab = compose("{ edit = { x = 1; y = 1; } }", "{ edit = { y = 1; } }", "{ edit = { x = 1; y = 2; } }",
                 NULL, 0, error, sizeof(error));
    CHECK(ab); if (ab) { CHECK(!strstr(ab, "x =")); CHECK(strstr(ab, "y = 2;")); } free(ab);
    ab = compose("{ edit = { x = 1; } }", "{ edit = {} }", "{ edit = { x = 2; } }",
                 NULL, 0, error, sizeof(error));
    CHECK(!ab); CHECK(strstr(error, "edit.x")); free(ab);
}

static void lists(void)
{
    sh_decl_collection_rule rule = {"edit.validEncounters", NULL};
    const char *base = "{ edit = { validEncounters = { num = 1; item[0] = \"stock\"; } } }";
    const char *a = "{ edit = { validEncounters = { num = 2; item[0] = \"stock\"; item[1] = \"cyberdemon\"; } } }";
    const char *b = "{ edit = { validEncounters = { num = 2; item[0] = \"stock\"; item[1] = \"hell_guard\"; } } }";
    char error[256];
    char *ab = compose(base, a, b, &rule, 1, error, sizeof(error));
    char *ba = compose(base, b, a, &rule, 1, error, sizeof(error));
    CHECK(ab && ba);
    if (ab && ba) {
        CHECK(!strcmp(ab, ba)); CHECK(strstr(ab, "num = 3;"));
        CHECK(strstr(ab, "item[1] = \"cyberdemon\";")); CHECK(strstr(ab, "item[2] = \"hell_guard\";"));
    }
    free(ab); free(ba);
    ab = compose(base, a, a, &rule, 1, error, sizeof(error));
    CHECK(ab); if (ab) CHECK(strstr(ab, "num = 2;")); free(ab);
    ab = compose(base, a, b, NULL, 0, error, sizeof(error));
    CHECK(!ab); CHECK(strstr(error, "adapter")); free(ab);
    ab = compose(base, a, "{ edit = { validEncounters = { num = 9; item[0] = \"stock\"; } } }",
                 &rule, 1, error, sizeof(error));
    CHECK(ab); if (ab) CHECK(strstr(ab, "num = 2;")); free(ab);
    ab = compose(base, a, "{ edit = { validEncounters = { num = 2; item[0] = \"stock\"; item[1] = \"stock\"; } } }",
                 &rule, 1, error, sizeof(error));
    CHECK(ab); if (ab) CHECK(strstr(ab, "num = 2;")); free(ab);
    ab = compose(base, a, "{ edit = { validEncounters = { num = 2; item[0] = \"new\"; item[1] = \"stock\"; } } }",
                 &rule, 1, error, sizeof(error));
    CHECK(ab); if (ab) CHECK(strstr(ab, "item[0] = \"new\";")); free(ab);
}

static void grouped_collections(void)
{
    const sh_decl_collection_rule rules[] = {
        {"groups", "", "members", "id"}, {"groups.item[*].members", "id"}
    };
    const char *base = "{ groups = { num = 1; item[0] = { title = 1; members = { num = 2; "
        "item[0] = { id = \"a\"; label = 0; } item[1] = { id = \"b\"; label = 0; } } } } }";
    const char *move = "{ groups = { num = 2; item[0] = { title = 1; members = { num = 10; "
        "item[3] = { id = \"a\"; label = 0; } } } item[1] = { members = { num = 1; "
        "item[0] = { id = \"b\"; label = 0; } } } } }";
    const char *edit = "{ groups = { num = 1; item[0] = { title = 2; members = { num = 3; "
        "item[0] = { id = \"a\"; label = 0; } item[1] = { id = \"b\"; label = 9; } "
        "item[2] = { id = \"c\"; label = 4; } } } } }";
    const char *duplicates = "{ groups = { num = 1; item[0] = { title = 1; members = { num = 3; "
        "item[0] = { id = \"a\"; label = 0; } item[1] = { id = \"b\"; label = 0; } "
        "item[7] = { id = \"b\"; label = 0; } } } } }";
    const char *ambiguous = "{ groups = { num = 1; item[0] = { title = 1; members = { num = 3; "
        "item[0] = { id = \"a\"; label = 0; } item[1] = { id = \"b\"; label = 0; } "
        "item[7] = { id = \"b\"; label = 3; } } } } }";
    const char *remove = "{ groups = { num = 1; item[0] = { title = 1; members = { num = 1; "
        "item[0] = { id = \"a\"; label = 0; } } } } }";
    const char *move_again = "{ groups = { num = 3; item[0] = { title = 1; members = { num = 1; "
        "item[0] = { id = \"a\"; label = 0; } } } item[1] = { members = { num = 0; } } "
        "item[2] = { members = { num = 1; item[0] = { id = \"b\"; label = 0; } } } } }";
    char error[512];
    char *ab = compose(base, move, edit, rules, 2, error, sizeof(error));
    char *ba = compose(base, edit, move, rules, 2, error, sizeof(error));
    CHECK(ab && ba);
    if (!ab) fprintf(stderr, "group merge: %s\n", error);
    if (ab && ba) {
        sh_decl_node *tree = sh_decl_tree_parse(view(ab), error, sizeof(error));
        const sh_decl_node *groups = sh_decl_tree_member(tree, "groups");
        const sh_decl_node *first = sh_decl_tree_member(groups, "item[0]");
        const sh_decl_node *second = sh_decl_tree_member(groups, "item[1]");
        const sh_decl_node *items = sh_decl_tree_member(second, "members");
        const sh_decl_node *item = sh_decl_tree_member(items, "item[0]");
        CHECK(!strcmp(ab, ba));
        CHECK(first && !strcmp(sh_decl_tree_member(first, "title")->value, "2"));
        CHECK(item && !strcmp(sh_decl_tree_member(item, "id")->value, "\"b\""));
        CHECK(item && !strcmp(sh_decl_tree_member(item, "label")->value, "9"));
        CHECK(!strcmp(sh_decl_tree_member(items, "num")->value, "1"));
        items = sh_decl_tree_member(first, "members");
        CHECK(!strcmp(sh_decl_tree_member(items, "num")->value, "2"));
        sh_decl_tree_free(tree);
    }
    free(ab); free(ba);
    ab = compose(base, duplicates, edit, rules, 2, error, sizeof(error)); CHECK(ab); free(ab);
    ab = compose(base, ambiguous, edit, rules, 2, error, sizeof(error));
    CHECK(!ab && strstr(error, "same collection identity")); free(ab);
    ab = compose(base, move, remove, rules, 2, error, sizeof(error));
    CHECK(!ab && strstr(error, "deletion conflicts")); free(ab);
    ab = compose(base, move, move_again, rules, 2, error, sizeof(error));
    CHECK(!ab && strstr(error, "different groups")); free(ab);
    ab = compose("{}", move, move, rules, 2, error, sizeof(error)); CHECK(ab); free(ab);
    ab = compose(base, move, move, rules, 2, error, sizeof(error)); CHECK(ab); free(ab);
    /* Removing an empty page preserves another package's member edits. */
    ab = compose(move_again, move, move_again, rules, 2, error, sizeof(error)); CHECK(ab); free(ab);
}

static void presentation_duplicates(void)
{
    static const char *const labels[] = {"label", "help", NULL};
    const sh_decl_collection_rule rules[] = {
        {"groups", "", "members", "id", labels},
        {"groups.item[*].members", "id", NULL, NULL, labels}
    };
    const sh_decl_collection_rule exact_rules[] = {
        {"groups", "", "members", "id"}, {"groups.item[*].members", "id"}
    };
    /* Physical textual order does not choose the winner: page/slot order does.
     * The later duplicate fills missing help but keeps the earlier label. */
    const char *source = "{ inherit = \"parent\"; groups = { num = 2; "
        "item[1] = { members = { num = 3; "
        "item[0] = { id = \"size\"; inspector = \"vector\"; label = \"stock\"; help = \"tip\"; } "
        "item[1] = { id = \"other\"; inspector = \"vector\"; label = \"custom\"; } "
        "item[2] = { id = \"other\"; inspector = \"vector\"; label = \"different label\"; } } } "
        "item[0] = { members = { num = 1; item[0] = { id = \"size\"; inspector = \"vector\"; label = \"custom\"; } } } } }";
    char error[512], *out = NULL, *again = NULL;
    size_t length, again_length;
    CHECK(sh_decl_normalize_collections(view(source), rules, 2, &out, &length, error, sizeof(error)));
    CHECK(out);
    if (out) {
        sh_decl_node *tree = sh_decl_tree_parse(view(out), error, sizeof(error));
        const sh_decl_node *groups = sh_decl_tree_member(tree, "groups");
        const sh_decl_node *first = sh_decl_tree_member(sh_decl_tree_member(groups, "item[0]"), "members");
        const sh_decl_node *second = sh_decl_tree_member(sh_decl_tree_member(groups, "item[1]"), "members");
        const sh_decl_node *item = sh_decl_tree_member(first, "item[0]");
        CHECK(!strcmp(sh_decl_tree_member(first, "num")->value, "1"));
        CHECK(!strcmp(sh_decl_tree_member(second, "num")->value, "1"));
        CHECK(!strcmp(sh_decl_tree_member(item, "label")->value, "\"custom\""));
        CHECK(!strcmp(sh_decl_tree_member(item, "help")->value, "\"tip\""));
        item = sh_decl_tree_member(second, "item[0]");
        CHECK(!strcmp(sh_decl_tree_member(item, "id")->value, "\"other\""));
        CHECK(!strcmp(sh_decl_tree_member(item, "label")->value, "\"custom\""));
        CHECK(!sh_decl_tree_member(second, "item[1]"));
        sh_decl_tree_free(tree);
        CHECK(sh_decl_normalize_collections(view(out), rules, 2, &again, &again_length, error, sizeof(error)));
        CHECK(!again && !again_length);
    }
    free(out); free(again); out = NULL;
    /* Any registered presentation partition can collapse exact repeats, even
     * without special display fields. Empty groups retain their authored slot. */
    const char *exact = "{ groups = { num = 2; item[0] = { members = { num = 1; "
        "item[0] = { id = \"a\"; behavior = 7; } } } item[1] = { members = { num = 1; "
        "item[0] = { id = \"a\"; behavior = 7; } } } } }";
    CHECK(sh_decl_normalize_collections(view(exact), exact_rules, 2, &out, &length, error, sizeof(error)));
    CHECK(out && strstr(out, "num = 0;")); free(out); out = NULL;
    const char *different = "{ groups = { num = 2; item[0] = { members = { num = 1; "
        "item[0] = { id = \"a\"; inspector = \"vector\"; label = 1; } } } item[1] = { members = { num = 1; "
        "item[0] = { id = \"a\"; inspector = \"scalar\"; label = 2; } } } } }";
    CHECK(!sh_decl_normalize_collections(view(different), rules, 2, &out, &length, error, sizeof(error)));
    CHECK(!out && strstr(error, "same collection identity across groups"));
    /* Ignoring display fields must not erase a difference in the native block
     * operation. Equality already distinguishes assignment from a bare block. */
    const char *different_operation = "{ groups = { num = 2; item[0] = { members = { num = 1; "
        "item[0] = { id = \"a\"; inspector = \"vector\"; label = 1; } } } item[1] = { members = { num = 1; "
        "item[0] { id = \"a\"; inspector = \"vector\"; label = 2; } } } } }";
    CHECK(!sh_decl_normalize_collections(view(different_operation), rules, 2, &out, &length, error, sizeof(error)));
    CHECK(!out && strstr(error, "same collection identity across groups"));
    /* Metadata precedence belongs to duplicate rows inside one input. Two
     * packages deliberately editing one label differently still conflict. */
    const char *base = "{ groups = { num = 1; item[0] = { members = { num = 1; item[0] = { id = \"a\"; label = 0; } } } } }";
    const char *a = "{ groups = { num = 1; item[0] = { members = { num = 1; item[0] = { id = \"a\"; label = 1; } } } } }";
    const char *b = "{ groups = { num = 1; item[0] = { members = { num = 1; item[0] = { id = \"a\"; label = 2; } } } } }";
    out = compose(base, a, b, rules, 2, error, sizeof(error)); CHECK(!out); free(out);
}

static void inherited_collection_patches(void)
{
    const sh_decl_collection_rule rules[] = {
        {"groups", "", "members", "id"}, {"groups.item[*].members", "id"}
    };
    const char *base = "{ inherit = \"parent\"; groups = { item[0] = { members = { item[4] = { label = 0; } } } } }";
    const char *a = "{ inherit = \"parent\"; groups = { item[0] = { members = { item[4] = { label = 9; } } } } }";
    const char *b = "{ inherit = \"parent\"; groups = { item[0] = { members = { num = 6; item[4] = { label = 0; caption = 7; } } } } }";
    const char *other = "{ inherit = \"different\"; groups = { item[0] = { members = { item[4] = { label = 0; } } } } }";
    char error[512], *normalized = NULL;
    size_t length = 99;
    CHECK(sh_decl_normalize_collections(view(a), rules, 2, &normalized, &length, error, sizeof(error)));
    CHECK(!normalized && !length); free(normalized);
    CHECK(sh_decl_normalize_collections(view(b), rules, 2, &normalized, &length, error, sizeof(error)));
    CHECK(!normalized && !length); free(normalized);
    char *ab = compose(base, a, b, rules, 2, error, sizeof(error));
    char *ba = compose(base, b, a, rules, 2, error, sizeof(error));
    CHECK(ab && ba);
    if (ab && ba) {
        CHECK(!strcmp(ab, ba)); CHECK(strstr(ab, "item[4]"));
        CHECK(strstr(ab, "label = 9;") && strstr(ab, "caption = 7;") && strstr(ab, "num = 6;"));
        CHECK(!strstr(ab, "id ="));
    }
    free(ab); free(ba);
    ab = compose(base, a, other, rules, 2, error, sizeof(error));
    CHECK(!ab && strstr(error, "consistent parent layout")); free(ab);
    CHECK(!sh_decl_normalize_collections(view("{ groups = { item[0] = { members = { item[4] = { label = 9; } } } } }"),
        rules, 2, &normalized, &length, error, sizeof(error))); free(normalized);
}

static char *blocking_list(size_t count, const char *addition)
{
    size_t length = 0, i, capacity = 65536;
    char *text = (char *)malloc(capacity);
    if (!text) return NULL;
    length += (size_t)snprintf(text + length, capacity - length,
                              "{ edit = { renderModelInfoList = { num = %zu;\n", count + (addition ? 1u : 0u));
    for (i = 0; i < count; i++)
        length += (size_t)snprintf(text + length, capacity - length,
            "item[%zu] = { renderModelMaterial = \"material/stock_%zu\"; categoryId = \"stock\"; }\n", i, i);
    if (addition)
        length += (size_t)snprintf(text + length, capacity - length,
            "item[%zu] = { renderModelMaterial = \"%s\"; categoryId = \"custom\"; }\n", count, addition);
    snprintf(text + length, capacity - length, "} } }\n");
    return text;
}

static void blocking(void)
{
    sh_decl_collection_rule rule = {"edit.renderModelInfoList", "renderModelMaterial"};
    char *base = blocking_list(216, NULL), *a = blocking_list(216, "material/alpha"),
         *b = blocking_list(216, "material/beta");
    char *saved_a, *saved_b, *result;
    char error[256];
    CHECK(base && a && b);
    if (!base || !a || !b) { free(base); free(a); free(b); return; }
    saved_a = (char *)malloc(strlen(a) + 1u); saved_b = (char *)malloc(strlen(b) + 1u);
    CHECK(saved_a && saved_b);
    if (!saved_a || !saved_b) { free(saved_a); free(saved_b); free(base); free(a); free(b); return; }
    memcpy(saved_a, a, strlen(a) + 1u); memcpy(saved_b, b, strlen(b) + 1u);
    result = compose(base, a, b, &rule, 1, error, sizeof(error));
    CHECK(result);
    if (result) {
        CHECK(strstr(result, "num = 218;")); CHECK(strstr(result, "item[217]"));
        CHECK(strstr(result, "material/alpha")); CHECK(strstr(result, "material/beta"));
        CHECK(strstr(result, "material/stock_215"));
    }
    CHECK(!strcmp(saved_a, a)); CHECK(!strcmp(saved_b, b));
    free(result); free(saved_a); free(saved_b); free(base); free(a); free(b);
    result = compose("{ edit = { renderModelInfoList = { num = 0; } } }",
        "{ edit = { renderModelInfoList = { num = 1; item[0] = { renderModelMaterial = \"same\"; x = 1; } } } }",
        "{ edit = { renderModelInfoList = { num = 1; item[0] = { renderModelMaterial = \"same\"; x = 2; } } } }",
        &rule, 1, error, sizeof(error));
    CHECK(!result); CHECK(strstr(error, "same")); free(result);
}

static void editor_properties(void)
{
    sh_decl_collection_rule rules[] = {
        {"edit.propertySheets", ""},
        {"edit.propertySheets.item[*].properties", "path"}
    };
    const char *base = "{ edit = { propertySheets = { num = 2; item[0] = { properties = { num = 0; } } "
        "item[1] = { properties = { num = 1; item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } } } } } }";
    const char *a = "{ edit = { propertySheets = { num = 2; item[0] = { properties = { num = 0; } } "
        "item[1] = { properties = { num = 2; item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } "
        "item[1] = { path = \"allowClimb\"; inspector = \"boolinspector\"; readOnly = false; } } } } } }";
    const char *b = "{ edit = { propertySheets = { num = 2; item[0] = { properties = { num = 0; } } "
        "item[1] = { properties = { num = 2; item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } "
        "item[1] = { path = \"receiveDecals\"; inspector = \"boolinspector\"; readOnly = true; } } } } } }";
    const char *both = "{ edit = { propertySheets = { num = 2; item[0] = { properties = { num = 0; } } "
        "item[1] = { properties = { num = 3; item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } "
        "item[1] = { path = \"allowClimb\"; inspector = \"boolinspector\"; readOnly = false; } "
        "item[2] = { path = \"receiveDecals\"; inspector = \"boolinspector\"; readOnly = true; } } } } } }";
    const char *conflict = "{ edit = { propertySheets = { num = 2; item[0] = { properties = { num = 0; } } "
        "item[1] = { properties = { num = 2; item[0] = { path = \"stock\"; inspector = \"boolinspector\"; } "
        "item[1] = { path = \"allowClimb\"; inspector = \"boolinspector\"; readOnly = true; } } } } } }";
    char error[256];
    char *ab = compose(base, a, b, rules, 2, error, sizeof(error));
    char *ba = compose(base, b, a, rules, 2, error, sizeof(error));
    CHECK(ab && ba);
    if (ab && ba) {
        CHECK(!strcmp(ab, ba)); CHECK(strstr(ab, "num = 3;"));
        CHECK(strstr(ab, "allowClimb")); CHECK(strstr(ab, "receiveDecals"));
        CHECK(strstr(ab, "readOnly = false;")); CHECK(strstr(ab, "readOnly = true;"));
    }
    free(ba);
    ba = compose(base, a, both, rules, 2, error, sizeof(error));
    CHECK(ab && ba && !strcmp(ab, ba)); free(ab); free(ba);
    ab = compose(base, a, conflict, rules, 2, error, sizeof(error));
    CHECK(!ab); CHECK(strstr(error, "allowClimb")); free(ab);
    /* No semantic adapter means the enclosing sheet list remains atomic. */
    ab = compose(base, a, b, rules + 1, 1, error, sizeof(error));
    CHECK(!ab); CHECK(strstr(error, "adapter")); free(ab);
    ab = compose(base, a, "{ edit = { propertySheets = { num = 0; } } }", rules, 2, error, sizeof(error));
    CHECK(!ab); CHECK(strstr(error, "ordering")); free(ab);
}

static char *sequence(const char *ids)
{
    size_t i, count = strlen(ids), used, capacity = 64 + count * 40;
    char *text = (char *)malloc(capacity);
    if (!text) return NULL;
    used = (size_t)snprintf(text, capacity, "{ items = { num = %zu; ", count);
    for (i = 0; i < count; i++)
        used += (size_t)snprintf(text + used, capacity - used, "item[%zu] = \"%c\"; ", i, ids[i]);
    snprintf(text + used, capacity - used, "} }");
    return text;
}

static char *order_merge_flags(const char *original, const char *const *edits, size_t count,
    unsigned flags, char *error)
{
    sh_decl_collection_rule rule = {"items", NULL};
    sh_decl_source *sources = (sh_decl_source *)calloc(count, sizeof(*sources));
    char *base = sequence(original), *result = NULL;
    size_t i, length;
    rule.flags = flags;
    CHECK(sources && base);
    if (!sources || !base) goto done;
    for (i = 0; i < count; i++) {
        char *text = sequence(edits[i]);
        CHECK(text);
        if (!text) goto done;
        sources[i] = view(text);
    }
    result = sh_decl_compose(view(base), sources, count, &rule, 1, &length, error, 512, NULL);
done:
    if (sources) for (i = 0; i < count; i++) free((char *)sources[i].text);
    free(sources); free(base); return result;
}

static char *order_merge(const char *original, const char *const *edits, size_t count, char *error)
{ return order_merge_flags(original, edits, count, 0, error); }

static void order_is(const char *text, const char *expected)
{
    sh_decl_node *tree;
    const sh_decl_node *list, *num;
    size_t i;
    char number[40];
    CHECK(text);
    if (!text) return;
    tree = sh_decl_tree_parse(view(text), NULL, 0);
    CHECK(tree);
    if (!tree) return;
    list = sh_decl_tree_member(tree, "items"); num = sh_decl_tree_member(list, "num");
    snprintf(number, sizeof(number), "%zu", strlen(expected));
    CHECK(num && !strcmp(num->value, number));
    for (i = 0; i < strlen(expected); i++) {
        const sh_decl_node *item;
        char key[40], value[4] = {'"', expected[i], '"', 0};
        snprintf(key, sizeof(key), "item[%zu]", i);
        item = sh_decl_tree_member(list, key);
        CHECK(item && !strcmp(item->value, value));
    }
    sh_decl_tree_free(tree);
}

static void ordering(void)
{
    char error[512], *result;
    const char *insertions[] = {"abc", "axc"};
    const char *authored_order[] = {"azbc", "ac"};
    const char *reversal[] = {"bac", "abc"};
    const char *cycle[] = {"bac", "acb"};
    const char *opposed[] = {"abx", "axb"};
    const char *deletion[] = {"ac", "abc"};
    const char *delete_move[] = {"ac", "acb"};
    const char *compatible[] = {"ax", "ay", "ayx"};
    const char *three_cycle[] = {"axy", "ayz", "azx"};
    size_t i, j, k;
    result = order_merge("ac", insertions, 2, error);
    order_is(result, "abxc"); free(result);
    result = order_merge("ac", authored_order, 2, error);
    order_is(result, "azbc"); free(result);
    result = order_merge("abc", reversal, 2, error);
    order_is(result, "bac"); free(result);
    result = order_merge("abc", cycle, 2, error);
    CHECK(!result && strstr(error, "cycle")); free(result);
    result = order_merge("a", opposed, 2, error);
    CHECK(!result && strstr(error, "ordering")); free(result);
    result = order_merge("abc", deletion, 2, error);
    order_is(result, "ac"); free(result);
    result = order_merge("abc", delete_move, 2, error);
    CHECK(!result && strstr(error, "deletion conflicts with reordering")); free(result);
    /* A two-source intermediate would invent x<y and wrongly reject y<x.
     * All six input permutations must instead retain the actual constraints. */
    for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) for (k = 0; k < 3; k++) {
        const char *edits[3];
        if (i == j || i == k || j == k) continue;
        edits[0] = compatible[i]; edits[1] = compatible[j]; edits[2] = compatible[k];
        result = order_merge("a", edits, 3, error);
        order_is(result, "ayx"); free(result);
        edits[0] = three_cycle[i]; edits[1] = three_cycle[j]; edits[2] = three_cycle[k];
        result = order_merge("a", edits, 3, error);
        CHECK(!result && strstr(error, "cycle")); free(result);
    }
    /* A new list has no original anchors; author ordering still survives. */
    result = order_merge("", compatible, 3, error);
    order_is(result, "ayx"); free(result);
}

static int derived_relation(void *context, const char *path,
    const sh_decl_node *left, const sh_decl_node *right)
{
    int mode = *(int *)context; char a, b;
    if (strcmp(path, "items")) return 0;
    CHECK(left && right && left->value && right->value);
    if (!left || !right || !left->value || !right->value) return 2;
    a = left->value[1]; b = right->value[1];
    if (mode == 3) return -7;
    if (mode == 4) return 1; /* Invalid asymmetric adapter. */
    if (a == 'b' && b == 'a') return -1;
    if (a == 'a' && b == 'b') return 1;
    if (mode == 2) {
        if ((a == 'c' && b == 'b') || (a == 'a' && b == 'c')) return -1;
        if ((a == 'b' && b == 'c') || (a == 'c' && b == 'a')) return 1;
    }
    return 0;
}
static void derived_ordering(void)
{
    sh_decl_collection_rule rule = {"items", NULL};
    int mode = 1; sh_decl_collection_order order = {&mode, derived_relation};
    char *base = sequence(""), *a = sequence("a"), *b = sequence("b"), *c = sequence("c");
    sh_decl_source sources[3] = {view(a), view(b), view(c)};
    char error[512], *result; size_t length;
    result = sh_decl_compose_ordered(view(base), sources, 2, &rule, 1, &order,
        &length, error, sizeof(error), NULL);
    order_is(result, "ba"); free(result);
    mode = 2;
    result = sh_decl_compose_ordered(view(base), sources, 3, &rule, 1, &order,
        &length, error, sizeof(error), NULL);
    CHECK(!result && !length && strstr(error, "cycle")); free(result);
    for (mode = 3; mode <= 4; mode++) {
        result = sh_decl_compose_ordered(view(base), sources, 2, &rule, 1, &order,
            &length, error, sizeof(error), NULL);
        CHECK(!result && !length && strstr(error, "ordering")); free(result);
    }
    free(base); base = sequence("ab"); sources[0] = sources[1] = view(base); mode = 1;
    /* Equal input fast paths must not skip a supplied ordering contract. */
    result = sh_decl_compose_ordered(view(base), sources, 2, &rule, 1, &order,
        &length, error, sizeof(error), NULL);
    CHECK(!result && !length && strstr(error, "ordering")); free(result);
    order.relation = NULL;
    result = sh_decl_compose_ordered(view(base), sources, 2, &rule, 1, &order,
        &length, error, sizeof(error), NULL);
    CHECK(!result && !length && strstr(error, "invalid")); free(result);
    free(base); free(a); free(b); free(c);
}

/* Letters of one half of the alphabet observe each other's order; letters of
 * different halves do not. A negative mode defers every pair to the rule. */
static int significant_relation(void *context, const char *path,
    const sh_decl_node *left, const sh_decl_node *right)
{
    int mode = *(int *)context;
    if (strcmp(path, "items")) return -1;
    CHECK(left && right && left->value && right->value);
    if (!left || !right || !left->value || !right->value) return 1;
    if (mode < 0) return -1;
    return (left->value[1] <= 'm') == (right->value[1] <= 'm');
}
static char *order_merge_contract(const char *original, const char *const *edits, size_t count,
    unsigned flags, const sh_decl_collection_order *order, char *error)
{
    sh_decl_collection_rule rule = {"items", NULL};
    sh_decl_source sources[4];
    char *base = sequence(original), *result = NULL;
    size_t i, length;
    rule.flags = flags;
    CHECK(base && count <= 4);
    for (i = 0; i < count; i++) sources[i] = view(sequence(edits[i]));
    result = sh_decl_compose_ordered(view(base), sources, count, &rule, 1, order, &length, error, 512, NULL);
    for (i = 0; i < count; i++) free((char *)sources[i].text);
    free(base); return result;
}
static void significant_ordering(void)
{
    int mode = 1;
    sh_decl_collection_order order = {&mode, NULL, significant_relation};
    const char *free_pair[] = {"ax", "xa"}, *bound_pair[] = {"ab", "ba"}, *cycle[] = {"ax", "xb", "ba"};
    const char *free_move[] = {"ab", "xab"}, *bound_move[] = {"ac", "bac"};
    char error[512], *result;
    for (size_t i = 0; i < 2; i++) {
        const char *edits[] = {free_pair[i], free_pair[1 - i]};
        /* An order nobody observes composes, deterministically, in a strict
         * list; one observed order stays mandatory even in a presentation
         * list. */
        result = order_merge_contract("", edits, 2, 0, &order, error);
        order_is(result, "ax"); free(result);
        edits[0] = bound_pair[i]; edits[1] = bound_pair[1 - i];
        result = order_merge_contract("", edits, 2, 0, &order, error);
        CHECK(!result && strstr(error, "ordering")); free(result);
        result = order_merge_contract("", edits, 2, SH_DECL_COLLECTION_PRESENTATION_ORDER, &order, error);
        CHECK(!result && strstr(error, "ordering")); free(result);
        edits[0] = free_pair[i]; edits[1] = free_pair[1 - i];
        result = order_merge_contract("", edits, 2, SH_DECL_COLLECTION_PRESENTATION_ORDER, &order, error);
        order_is(result, "ax"); free(result);
        /* Deferring to the strict rule restores the conflict. */
        mode = -1;
        result = order_merge_contract("", edits, 2, 0, &order, error);
        CHECK(!result && strstr(error, "ordering")); free(result);
        mode = 1;
    }
    /* A cycle through unobserved preferences drops them and keeps the
     * observed b-before-a. */
    result = order_merge_contract("", cycle, 3, 0, &order, error);
    order_is(result, "bax"); free(result);
    /* Deleting x does not conflict with moving it where nobody observes; b's
     * deletion still conflicts with moving it before a. */
    result = order_merge_contract("abx", free_move, 2, 0, &order, error);
    order_is(result, "ab"); free(result);
    result = order_merge_contract("abc", bound_move, 2, 0, &order, error);
    CHECK(!result && strstr(error, "deletion conflicts with reordering")); free(result);
    result = order_merge_contract("abc", bound_move, 2, SH_DECL_COLLECTION_PRESENTATION_ORDER, &order, error);
    CHECK(!result && strstr(error, "deletion conflicts with reordering")); free(result);
}

static void presentation_ordering(void)
{
    const char *opposed[] = {"abx", "axb"}, *cycle[] = {"bac", "acb"};
    const char *three_cycle[] = {"axy", "ayz", "azx"}, *compatible[] = {"ax", "ay", "ayx"};
    char error[512], *result;
    const unsigned flags = SH_DECL_COLLECTION_PRESENTATION_ORDER;
    for (size_t i = 0; i < 2; i++) {
        const char *edits[] = {opposed[i], opposed[1-i]};
        result = order_merge_flags("a", edits, 2, flags, error);
        order_is(result, "abx"); CHECK(!*error); free(result);
        edits[0] = cycle[i]; edits[1] = cycle[1-i];
        result = order_merge_flags("abc", edits, 2, flags, error);
        order_is(result, "abc"); CHECK(!*error); free(result);
    }
    for (size_t i = 0; i < 3; i++) for (size_t j = 0; j < 3; j++) for (size_t k = 0; k < 3; k++) {
        if (i == j || i == k || j == k) continue;
        const char *edits[] = {three_cycle[i], three_cycle[j], three_cycle[k]};
        result = order_merge_flags("a", edits, 3, flags, error);
        order_is(result, "axyz"); free(result);
        edits[0] = compatible[i]; edits[1] = compatible[j]; edits[2] = compatible[k];
        result = order_merge_flags("a", edits, 3, flags, error);
        order_is(result, "ayx"); free(result);
    }
    {
        sh_decl_collection_rule rule = {"items", NULL, NULL, NULL, NULL, flags};
        int mode = 1; sh_decl_collection_order order = {&mode, derived_relation};
        char *base = sequence("ab"), *a = sequence("a"), *b = sequence("b"), *c = sequence("c");
        sh_decl_source sources[] = {view(base), view(base), view(c)};
        size_t length;
        result = sh_decl_compose_ordered(view(base), sources, 2, &rule, 1, &order,
            &length, error, sizeof(error), NULL);
        order_is(result, "ba"); free(result); /* Native order beats layout. */
        {
            char *original = sequence("abc"), *left = sequence("bac"), *right = sequence("acb");
            sh_decl_source cycle_sources[] = {view(left), view(right)};
            result = sh_decl_compose_ordered(view(original), cycle_sources, 2, &rule, 1, &order,
                &length, error, sizeof(error), NULL);
            order_is(result, "bac"); free(result); /* Soft cycle drops no hard edge. */
            free(original); free(left); free(right);
        }
        free(base); base = sequence(""); sources[0] = view(a); sources[1] = view(b);
        mode = 2;
        result = sh_decl_compose_ordered(view(base), sources, 3, &rule, 1, &order,
            &length, error, sizeof(error), NULL);
        CHECK(!result && !length && strstr(error, "cycle")); free(result);
        for (mode = 3; mode <= 4; mode++) {
            result = sh_decl_compose_ordered(view(base), sources, 2, &rule, 1, &order,
                &length, error, sizeof(error), NULL);
            CHECK(!result && !length && strstr(error, "ordering")); free(result);
        }
        /* A consumer cannot opt a fixed positional array into layout rules. */
        rule.item_key = "";
        result = sh_decl_compose(view(base), sources, 2, &rule, 1, &length, error, sizeof(error), NULL);
        CHECK(!result && !length && strstr(error, "adapter")); free(result);
        CHECK(!sh_decl_normalize_collections(view(base), &rule, 1, &result, &length, error, sizeof(error)));
        CHECK(!result && !length); free(result);
        free(base); free(a); free(b); free(c);
    }
}

static char *control_source(size_t page, const char *inspector, const char *action,
    const char *type, const char *label)
{
    char *text = malloc(2048); size_t used;
    if (!text) return NULL;
    used = (size_t)snprintf(text, 2048, "{ groups = { num = %zu; ", page + 1);
    for (size_t i = 0; i < page; i++) used += (size_t)snprintf(text + used, 2048 - used,
        "item[%zu] = { members = { num = 0; } } ", i);
    snprintf(text + used, 2048 - used,
        "item[%zu] = { members = { num = 1; item[0] = { id = \"size\"; inspector = \"%s\"; "
        "action = \"%s\"; type = \"%s\"; label = \"%s\"; } } } } }", page, inspector, action, type, label);
    return text;
}

static size_t control_count(const char *text, size_t *last_page)
{
    sh_decl_node *tree = text ? sh_decl_tree_parse(view(text), NULL, 0) : NULL;
    const sh_decl_node *groups = sh_decl_tree_member(tree, "groups");
    size_t count = 0, page = 0;
    CHECK(tree && groups);
    for (const sh_decl_node *group = groups ? groups->children : NULL; group; group = group->next) {
        if (!strcmp(group->key, "num")) continue;
        const sh_decl_node *items = sh_decl_tree_member(group, "members");
        for (const sh_decl_node *item = items ? items->children : NULL; item; item = item->next)
            if (strcmp(item->key, "num")) { count++; if (last_page) *last_page = page; }
        page++;
    }
    sh_decl_tree_free(tree); return count;
}

static void control_behavior_identity(void)
{
    static const char *const labels[] = {"label", "help", NULL};
    const unsigned flags = SH_DECL_COLLECTION_PRESENTATION_ORDER | SH_DECL_COLLECTION_DISTINCT_BEHAVIOR;
    const sh_decl_collection_rule rules[] = {
        {"groups", "", "members", "id", labels, flags},
        {"groups.item[*].members", "id", NULL, NULL, labels, flags}
    };
    char error[512], *result = NULL, *again = NULL; size_t length, again_length, page = SIZE_MAX;
    char *base = control_source(0, "vector", "edit", "Vec3", "stock");
    char *a = control_source(1, "vector", "edit", "Vec3", "red");
    char *b = control_source(2, "vector", "edit", "Vec3", "green");
    CHECK(base && a && b);
    if (!base || !a || !b) goto done;
    result = compose(base, a, b, rules, 2, error, sizeof(error));
    again = compose(base, b, a, rules, 2, error, sizeof(error));
    CHECK(result && again && !strcmp(result, again));
    CHECK(control_count(result, &page) == 1 && page == 1);
    CHECK(result && strstr(result, "label = \"green\""));
    free(result); free(again); result = again = NULL;
    /* A removal cannot erase a peer's changed control or page placement. */
    result = compose(base, a, "{ groups = { num = 0; } }", rules, 2, error, sizeof(error));
    CHECK(control_count(result, &page) == 1); free(result); result = NULL;
    {
        char *different[] = {
            control_source(0, "scalar", "edit", "Vec3", "other"),
            control_source(0, "vector", "private/other_action", "Vec3", "other"),
            control_source(0, "vector", "edit", "Scalar", "other")
        };
        for (size_t i = 0; i < 3; i++) {
            CHECK(different[i]);
            if (!different[i]) continue;
            result = compose("{ groups = { num = 0; } }", base, different[i], rules, 2, error, sizeof(error));
            again = compose("{ groups = { num = 0; } }", different[i], base, rules, 2, error, sizeof(error));
            CHECK(result && again && !strcmp(result, again));
            CHECK(control_count(result, NULL) == 2);
            free(result); free(again); result = again = NULL; free(different[i]);
        }
    }
    {
        const char *duplicates = "{ groups = { num = 2; item[0] = { members = { num = 1; "
            "item[0] = { id = \"size\"; inspector = \"vector\"; label = \"first\"; } } } "
            "item[1] = { members = { num = 3; "
            "item[0] = { label = \"second\"; inspector = \"vector\"; id = \"size\"; } "
            "item[1] = { id = \"size\"; inspector = \"scalar\"; } "
            "item[2] { id = \"size\"; inspector = \"vector\"; } } } } }";
        CHECK(sh_decl_normalize_collections(view(duplicates), rules, 2, &result, &length, error, sizeof(error)));
        CHECK(result && control_count(result, NULL) == 3 && strstr(result, "label = \"first\""));
        CHECK(result && !strstr(result, "label = \"second\""));
        if (result) {
            CHECK(sh_decl_normalize_collections(view(result), rules, 2, &again, &again_length, error, sizeof(error)));
            CHECK(!again && !again_length);
        }
    }
done:
    free(base); free(a); free(b); free(result); free(again);
}

static void added_objects(void)
{
    char error[512], *result, *reverse;
    sh_decl_collection_rule rules[] = {
        {"items", "name"}, {"items.item[*].children", "path"}
    };
    const char *base = "{ items = { num = 0; } }";
    const char *a = "{ items = { num = 1; item[0] = { name = \"shared\"; "
        "left = { enabled = true; } children = { num = 1; item[0] = { path = \"one\"; } } } } }";
    const char *b = "{ items = { num = 1; item[0] = { name = \"shared\"; "
        "right = { health = 42; } children = { num = 1; item[0] = { path = \"two\"; } } } } }";
    result = compose(base, a, b, rules, 2, error, sizeof(error));
    reverse = compose(base, b, a, rules, 2, error, sizeof(error));
    CHECK(result && reverse && !strcmp(result, reverse));
    if (result) {
        CHECK(strstr(result, "enabled = true;") && strstr(result, "health = 42;"));
        CHECK(strstr(result, "num = 2;") && strstr(result, "path = \"one\";") &&
            strstr(result, "path = \"two\";"));
    }
    free(result); free(reverse);
    result = compose("{}", "{ new = { shared = { left = 1; } } }",
        "{ new = { shared = { right = 2; } } }", NULL, 0, error, sizeof(error));
    CHECK(result && strstr(result, "left = 1;") && strstr(result, "right = 2;"));
    free(result);
    result = compose("{}", "{ new = { shared = { x = 1; } } }",
        "{ new = { shared = { x = 2; } } }", NULL, 0, error, sizeof(error));
    CHECK(!result && strstr(error, "new.shared.x")); free(result);
    /* An initially empty object cannot make an unknown collection mergeable. */
    result = compose("{ items = {} }", "{ items = { num = 1; item[0] = \"a\"; } }",
        "{ items = { num = 1; item[0] = \"b\"; } }", NULL, 0, error, sizeof(error));
    CHECK(!result && strstr(error, "adapter")); free(result);
}

static void conflict_sources(void)
{
    const char *ids[] = {"a", "axy", "ayx", "a"};
    sh_decl_collection_rule rule = {"items", NULL};
    char *base = sequence("a"), *texts[4], error[512];
    size_t a, b, c, d, i, length;
    for (i = 0; i < 4; i++) texts[i] = sequence(ids[i]);
    CHECK(base && texts[0] && texts[1] && texts[2] && texts[3]);
    if (!base || !texts[0] || !texts[1] || !texts[2] || !texts[3]) goto done;
    for (a = 0; a < 4; a++) for (b = 0; b < 4; b++) for (c = 0; c < 4; c++) for (d = 0; d < 4; d++) {
        size_t order[] = {a, b, c, d};
        sh_decl_source sources[4];
        sh_decl_conflict conflict = {77, 88};
        char *result;
        if (a == b || a == c || a == d || b == c || b == d || c == d) continue;
        for (i = 0; i < 4; i++) sources[i] = view(texts[order[i]]);
        result = sh_decl_compose(view(base), sources, 4, &rule, 1, &length, error, sizeof(error), &conflict);
        CHECK(!result && strstr(error, "ordering"));
        CHECK(conflict.first < 4 && conflict.second < 4);
        if (conflict.first < 4 && conflict.second < 4) {
            size_t first = order[conflict.first], second = order[conflict.second];
            CHECK((first == 1 && second == 2) || (first == 2 && second == 1));
        }
        free(result);
    }
    {
        sh_decl_source sources[] = {
            view("{ edit = { health = 1; } }"), view("{ edit = { health = 2; } }"),
            view("{ edit = { health = 3; } }"), view("{ edit = { health = 1; speed = 4; } }")
        };
        sh_decl_conflict conflict = {77, 88};
        char *result = sh_decl_compose(sources[0], sources, 4, NULL, 0, &length, error, sizeof(error), &conflict);
        CHECK(!result && strstr(error, "edit.health"));
        CHECK(conflict.first == 1 && conflict.second == 2); free(result);
        sources[2] = sources[1];
        result = sh_decl_compose(sources[0], sources, 4, NULL, 0, &length, error, sizeof(error), &conflict);
        CHECK(result && conflict.first == SIZE_MAX && conflict.second == SIZE_MAX); free(result);
        sources[2] = view("{ broken");
        result = sh_decl_compose(sources[0], sources, 4, NULL, 0, &length, error, sizeof(error), &conflict);
        CHECK(!result && conflict.first == 2 && conflict.second == SIZE_MAX); free(result);
    }
done:
    free(base); for (i = 0; i < 4; i++) free(texts[i]);
}

static int typed_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    (void)context;
    if (type.ops && !strcmp(type.ops, "[4]")) {
        shape->kind = SH_DECL_VALUE_COLLECTION;
        shape->element = (sh_decl_value_type){type.name, ""};
        shape->count = 4; shape->count_known = 1;
    } else if (!strcmp(type.name, "Fixture") || !strcmp(type.name, "Element"))
        shape->kind = SH_DECL_VALUE_OBJECT;
    else if (!strcmp(type.name, "List")) {
        shape->kind = SH_DECL_VALUE_COLLECTION;
        shape->element = (sh_decl_value_type){"Element", ""};
        shape->item_key = "item"; shape->count_key = "num";
    } else if (!strcmp(type.name, "int")) shape->kind = SH_DECL_VALUE_IGNORE;
    return 1;
}

static int typed_field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *out)
{
    (void)context; (void)type;
    if (!strcmp(key, "slots")) *out = (sh_decl_value_type){"Element", "[4]"};
    else if (!strcmp(key, "list")) *out = (sh_decl_value_type){"List", ""};
    else if (!strcmp(key, "custom")) *out = (sh_decl_value_type){"Custom", ""};
    else if (!strcmp(key, "x") || !strcmp(key, "y") || !strcmp(key, "id"))
        *out = (sh_decl_value_type){"int", ""};
    else return -1;
    return 1;
}

static char *typed_compose(const char *base, const char *a, const char *b, char *error, sh_decl_conflict *conflict)
{
    sh_decl_dependency_schema types = {NULL, typed_describe, typed_field, NULL, NULL};
    sh_decl_composition_schema schema = {&types, {"Fixture", ""}};
    sh_decl_collection_rule rule = {"edit.list", "id"};
    sh_decl_source sources[] = {view(a), view(b)};
    size_t length;
    return sh_decl_compose_typed(view(base), sources, 2, &rule, 1, &schema,
        &length, error, 512, conflict);
}

static void typed_collections(void)
{
    const char *base = "{ edit = { slots = { slots[3] = { x = 0; y = 0; } } } }";
    const char *a = "{ edit = { slots = { slots[3] = { x = 1; y = 0; } } } }";
    const char *b = "{ edit = { slots = { slots[1] = { x = 4; } slots[3] = { x = 0; y = 2; } } } }";
    const char *bad[] = {
        "{ edit = { slots = { slots[4] = {}; } } }",
        "{ edit = { slots = { slots[01] = {}; } } }",
        "{ edit = { slots = { item[0] = {}; } } }",
        "{ edit = { slots = { num = 4; } } }",
        "{ edit = { slots = { slots[18446744073709551616] = {}; } } }",
        "{ edit = { slots = 3; } }"
    };
    size_t i;
    char error[512];
    sh_decl_conflict conflict;
    char *ab = typed_compose(base, a, b, error, &conflict);
    char *ba = typed_compose(base, b, a, error, &conflict);
    CHECK(ab && ba);
    if (ab && ba) {
        CHECK(!strcmp(ab, ba));
        CHECK(strstr(ab, "slots[3] = {\nx = 1;\ny = 2;"));
        CHECK(strstr(ab, "slots[1] = {\nx = 4;"));
        CHECK(!strstr(ab, "num =") && !strstr(ab, "slots[0]"));
    }
    free(ab); free(ba);
    ab = typed_compose("{}", "{ edit = { slots = { slots[3] = { x = 1; } } } }",
        "{ edit = { slots = { slots[3] = { y = 2; } } } }", error, &conflict);
    CHECK(ab && strstr(ab, "x = 1;") && strstr(ab, "y = 2;")); free(ab);
    ab = typed_compose(base, a, "{ edit = { slots = {} } }", error, &conflict);
    CHECK(!ab && strstr(error, "edit.slots.slots[3]"));
    CHECK(conflict.first == 0 && conflict.second == 1); free(ab);
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ab = typed_compose("{}", bad[i], bad[i], error, &conflict);
        CHECK(!ab && strstr(error, "fixed native array"));
        CHECK(conflict.first == 0); free(ab);
    }
    ab = typed_compose("{ edit = { list = { num = 0; } } }",
        "{ edit = { list = { num = 1; item[0] = { id = 7; slots = { slots[3] = { x = 1; } } } } } }",
        "{ edit = { list = { num = 1; item[0] = { id = 7; slots = { slots[3] = { y = 2; } } } } } }",
        error, &conflict);
    CHECK(ab && strstr(ab, "slots[3]") && strstr(ab, "x = 1;") && strstr(ab, "y = 2;")); free(ab);
    ab = typed_compose("{}", "{ edit = { custom = { x = 1; } } }",
        "{ edit = { custom = { y = 2; } } }", error, &conflict);
    CHECK(!ab && strstr(error, "edit.custom") && strstr(error, "native reader")); free(ab);
    ab = typed_compose("{}", "{ edit = { unresolved = { x = 1; } } }",
        "{ edit = { unresolved = { y = 2; } } }", error, &conflict);
    CHECK(!ab && strstr(error, "edit.unresolved")); free(ab);
    {
        sh_decl_dependency_schema types = {NULL, typed_describe, typed_field, NULL, NULL};
        sh_decl_composition_schema schema = {&types, {"Fixture", ""}};
        sh_decl_source sources[] = {view("{ edit = { list = { item[0] = { x = 1; } } } }"),
            view("{ edit = { list = { item[0] = { y = 2; } } } }")};
        size_t length;
        ab = sh_decl_compose_typed(view("{}"), sources, 2, NULL, 0, &schema,
            &length, error, sizeof(error), &conflict);
        CHECK(!ab && strstr(error, "edit.list") && strstr(error, "indexed collection")); free(ab);
        /* One opaque replacement is still a supported delivery operation. */
        sources[1] = sources[0];
        ab = sh_decl_compose_typed(view("{}"), sources, 2, NULL, 0, &schema,
            &length, error, sizeof(error), &conflict);
        CHECK(ab != NULL); free(ab);
    }
}

static void entity_header_order(void)
{
    const char *keys[] = {"inherit", "class", "expandInheritance", "poolCount", "poolGranularity", "editorVars", "edit"};
    sh_decl_source inputs[] = {
        view("{ inherit = \"base\"; class = \"Entity\"; expandInheritance = true; poolCount = 2; poolGranularity = 1; editorVars = { x = 1; } edit = { a = 1; } }"),
        view("{ inherit = \"base\"; class = \"Entity\"; expandInheritance = true; poolCount = 2; poolGranularity = 1; editorVars = { y = 2; } edit = { b = 2; } }")
    };
    char error[512], *body;
    size_t length, i;
    sh_decl_node *tree;
    const sh_decl_node *child;
    body = sh_decl_compose_resource("entitydef", view("{}"), inputs, 2, NULL, 0, NULL,
        &length, error, sizeof(error), NULL);
    CHECK(body != NULL);
    if (!body) return;
    tree = sh_decl_tree_parse(view(body), error, sizeof(error)); CHECK(tree != NULL);
    child = tree ? tree->children : NULL;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        CHECK(child && !strcmp(child->key, keys[i])); if (child) child = child->next;
    }
    CHECK(!child); sh_decl_tree_free(tree); free(body);
    /* A baseline anchors edit first; a newly added header must still move
     * before it. Nested class/inherit fields retain their own normal order. */
    inputs[0] = view("{ class = \"Entity\"; edit = { class = 0; inherit = 0; x = 1; } }");
    inputs[1] = view("{ inherit = \"base\"; class = \"Entity\"; edit = { class = 0; inherit = 0; y = 2; } }");
    body = sh_decl_compose_resource("ENTITYDEF", view("{ class = \"Entity\"; edit = { class = 0; inherit = 0; } }"),
        inputs, 2, NULL, 0, NULL, &length, error, sizeof(error), NULL);
    CHECK(body && strstr(body, "inherit = \"base\"") < strstr(body, "class = \"Entity\""));
    CHECK(body && strstr(body, "class = 0;") < strstr(body, "inherit = 0;"));
    free(body);
    body = sh_decl_compose_resource("anotherfamily", view("{}"), inputs, 2, NULL, 0, NULL,
        &length, error, sizeof(error), NULL);
    CHECK(body && strstr(body, "class = \"Entity\"") < strstr(body, "inherit = \"base\""));
    free(body);
}

static void typed_source_annotations(void)
{
    const char *base = "{ \"Fixture\" \"\" edit = { int x = 1; int y = 2; } }";
    const char *a = "{ \"Fixture\" \"\" edit = { int x = 3; int y = 2; } }";
    const char *b = "{ \"Fixture\" \"\" edit = { int x = 1; int y = 4; idDeclEntityDef * target = \"child\"; } }";
    char error[256], *body = compose(base, a, b, NULL, 0, error, sizeof(error));
    sh_decl_node *tree;
    const sh_decl_node *edit, *target;
    CHECK(body != NULL);
    if (!body) return;
    CHECK(strstr(body, "\"Fixture\" \"\" edit ="));
    CHECK(strstr(body, "int x = 3;") && strstr(body, "int y = 4;"));
    CHECK(strstr(body, "idDeclEntityDef * target = \"child\";"));
    tree = sh_decl_tree_parse(view(body), error, sizeof(error));
    CHECK(tree != NULL);
    edit = sh_decl_tree_member(tree, "edit");
    target = sh_decl_tree_member(edit, "target");
    CHECK(edit && edit->annotation && !strcmp(edit->annotation, "\"Fixture\" \"\" "));
    CHECK(target && target->annotation && !strcmp(target->annotation, "idDeclEntityDef * "));
    sh_decl_tree_free(tree); free(body);
}

static void malformed(void)
{
    const char *bad[] = {
        "", "{} trailing", "{ x = 1; x = 2; }", "{ x = \"unterminated; }",
        "{ x = ( 1, 2; }", "{ x = ; }", "{ edit = { }", "{ x = 1; } /*",
        "{ x = 1 /* ambiguous scalar comment */; }"
    };
    size_t i;
    char error[256];
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char *result = compose("{}", bad[i], "{}", NULL, 0, error, sizeof(error));
        CHECK(!result); CHECK(error[0]); free(result);
    }
    {
        char *result = compose("/*base*/ { x = \"a;{}\\\"; }", "{ x = \"a;{}\\\"; }",
                               "{ x = \"a;{}\\\"; }", NULL, 0, error, sizeof(error));
        CHECK(result); free(result);
    }
}

static void native_file(const char *path)
{
    FILE *file = NULL;
    long size;
    char *text, *result;
    char error[256];
    sh_decl_collection_rule rule = {"edit.renderModelInfoList", "renderModelMaterial"};
    if (fopen_s(&file, path, "rb") || !file) { CHECK(0); return; }
    CHECK(!fseek(file, 0, SEEK_END)); size = ftell(file);
    if (size <= 0 || size > 8 * 1024 * 1024) { fclose(file); CHECK(0); return; }
    rewind(file); text = (char *)malloc((size_t)size + 1u);
    if (!text) { fclose(file); CHECK(0); return; }
    CHECK(fread(text, 1, (size_t)size, file) == (size_t)size);
    fclose(file); text[size] = 0;
    result = compose(text, text, text, &rule, 1, error, sizeof(error));
    if (!result) fprintf(stderr, "native declaration: %s\n", error);
    CHECK(result != NULL);
    free(result); free(text);
}

static void large_text(void)
{
    size_t payload = 9u * 1024u * 1024u, length;
    char *text = (char *)malloc(payload + 32), *result;
    char error[256];
    sh_decl_source input;
    CHECK(text != NULL);
    if (!text) return;
    memcpy(text, "{ value = \"", 11); memset(text + 11, 'x', payload);
    memcpy(text + 11 + payload, "\"; }", 5);
    input = view(text);
    result = sh_decl_compose(input, &input, 1, NULL, 0, &length, error, sizeof(error), NULL);
    CHECK(result != NULL && length > payload);
    free(result); free(text);
}

/* A first-match list (the conductor's modifiers): a later row sharing an
 * earlier row's identity is never reached natively, so it is dropped rather
 * than refused, and the earlier row stays exactly as authored; identities that
 * differ are kept in order. Without the flag the same list is refused. */
static void first_match_rows(void)
{
    const sh_decl_collection_rule first[] = {{"mods", "aiType", NULL, NULL, NULL, SH_DECL_COLLECTION_FIRST_MATCH}};
    const sh_decl_collection_rule plain[] = {{"mods", "aiType"}};
    const char *text = "{ mods = { num = 3; item[0] = { aiType = \"T5\"; healthMod = \"a\"; } "
        "item[1] = { aiType = \"T6\"; healthMod = \"c\"; } item[2] = { aiType = \"T5\"; healthMod = \"b\"; } } }";
    char error[512], *out = NULL;
    size_t length = 0;
    CHECK(sh_decl_normalize_collections(view(text), first, 1, &out, &length, error, sizeof(error)));
    CHECK(out && strstr(out, "num = 2;") && strstr(out, "\"a\"") && strstr(out, "\"c\"") && !strstr(out, "\"b\""));
    CHECK(out && strstr(out, "\"a\"") < strstr(out, "\"c\""));
    free(out); out = NULL;
    CHECK(!sh_decl_normalize_collections(view(text), plain, 1, &out, &length, error, sizeof(error)));
    CHECK(!out && strstr(error, "same collection identity"));
}
/* entityDef repeats (M85): the merge mode decides. Update mode collapses each
 * layer by first match into the inherited first statement's name; a first
 * stateful layer keeps its first statement's repeats as an ordered sequence. */
static int merges_mode;
static const char *merges_name = "edit";
static int fixed_merges(void *context, const sh_decl_node *definition, sh_decl_composition_role role,
    size_t index, char **state_name, char *error, size_t capacity)
{
    (void)context; (void)definition; (void)role; (void)index; (void)error; (void)capacity;
    if (merges_mode == 1) *state_name = _strdup(merges_name);
    return merges_mode;
}

static char *entity_compose(const sh_decl_composition_schema *schema, const char *base, const char *a,
    const char *b, char *error, sh_decl_conflict *conflict)
{
    sh_decl_source sources[] = {view(a), view(b ? b : a)};
    size_t length = 0;
    return sh_decl_compose_resource("entitydef", view(base), sources, b ? 2 : 1, NULL, 0, schema, &length,
        error, 512, conflict);
}

static void entity_repeats(void)
{
    sh_decl_dependency_schema types = {NULL, typed_describe, typed_field, NULL, NULL};
    sh_decl_composition_schema schema = {&types, {"Fixture", ""}};
    const char *base = "{ inherit = \"p\"; editorVars { placeable = true; } edit = { x = 0; y = 0; } }";
    const char *a = "{ inherit = \"p\"; editorVars { placeable = true; Placeable = false; } edit = { x = 1; x = 2; y = 0; } }";
    const char *b = "{ inherit = \"p\"; editorVars { placeable = true; } edit = { x = 0; y = 5; } edit = { y = 6; } }";
    sh_decl_conflict conflict;
    char error[512] = "", *body;
    schema.merges = fixed_merges;
    merges_mode = 1;
    body = entity_compose(&schema, base, a, b, error, NULL);
    CHECK(body && strstr(body, "x = 2;") && strstr(body, "y = 6;") && !strstr(body, "x = 1;") && !strstr(body, "y = 5;"));
    /* editorVars names are case-insensitive across inputs: one field. */
    CHECK(body && strstr(body, "placeable = false;") && !strstr(body, "placeable = true;") && !strstr(body, "Placeable"));
    free(body);
    /* A first stateful layer: its second edit is never read, and its repeats
     * compose as an ordered sequence with another input's change. */
    merges_mode = 0;
    body = entity_compose(&schema, base, b, NULL, error, NULL);
    CHECK(body && strstr(body, "y = 5;") && !strstr(body, "y = 6;")); free(body);
    body = entity_compose(&schema, base, a, b, error, NULL);
    CHECK(body && strstr(body, "x = 1;\nx = 2;\ny = 5;")); free(body);
    /* Each input's added copies keep their places. */
    body = entity_compose(&schema, base, a,
        "{ inherit = \"p\"; editorVars { placeable = true; } edit = { x = 0; y = 0; y = 7; } }", error, &conflict);
    CHECK(body && strstr(body, "x = 1;\nx = 2;\ny = 0;\ny = 7;")); free(body);
    /* A copy one input adds would hide the other's edit: a real conflict
     * naming the hidden edit first. */
    body = entity_compose(&schema, base, "{ inherit = \"p\"; edit = { x = 0; x = 4; y = 0; } }",
        "{ inherit = \"p\"; edit = { x = 5; y = 0; } }", error, &conflict);
    CHECK(!body && strstr(error, "hide another contribution") && conflict.first == 1 && conflict.second == 0);
    /* Opposite placements of the same additions. */
    body = entity_compose(&schema, base, "{ inherit = \"p\"; edit = { x = 0; x = 0; y = 0; z = 1; w = 2; } }",
        "{ inherit = \"p\"; edit = { x = 0; x = 0; y = 0; w = 2; z = 1; } }", error, &conflict);
    CHECK(!body && strstr(error, "incompatible placements") && conflict.first == 0 && conflict.second == 1);
    /* Equal sequence edits from two inputs compose once. */
    body = entity_compose(&schema, base, a, a, error, NULL);
    CHECK(body && strstr(body, "x = 1;\nx = 2;\ny = 0;") && !strstr(strstr(body, "x = 2;"), "x = 2;\nx")); free(body);
    /* Update mode reaches only the inherited first statement's name. */
    merges_mode = 1; merges_name = "data";
    body = entity_compose(&schema, "{ inherit = \"p\"; data = { x = 0; } }",
        "{ inherit = \"p\"; data = { x = 3; } edit = { x = 9; } }", NULL, error, NULL);
    /* The inert edit statement stays an authored member after the state
     * (M86, dc_keep_inert); x = 9 never reaches data. */
    {
        const char *data = body ? strstr(body, "data = {") : NULL, *inert = data ? strstr(data, "edit = {") : NULL;
        const char *three = data ? strstr(data, "x = 3;") : NULL, *nine = body ? strstr(body, "x = 9;") : NULL;
        CHECK(data && three && (!inert || three < inert) && (!nine || (inert && nine > inert)));
    }
    free(body);
    merges_name = "edit";
    /* Without a merge mode such state stays coverage, naming its input. */
    schema.merges = NULL;
    body = entity_compose(&schema, base, a, b, error, &conflict);
    CHECK(!body && strstr(error, SH_DECL_COMPOSE_COVERAGE) && conflict.first == 0);
    /* Other grammars stay strict. */
    body = sh_decl_compose_resource("weapon", view("{ edit = { x = 0; } }"), (sh_decl_source[]){view(a)}, 1, NULL, 0,
        NULL, &(size_t){0}, error, sizeof(error), &conflict);
    CHECK(!body && strstr(error, "malformed") && conflict.first == 0);
    /* Positional headers: an out-of-order inherit is a scalar first statement. */
    merges_mode = 0; schema.merges = fixed_merges;
    body = entity_compose(&schema, base, "{ class = \"Fixture\"; inherit = \"p\"; edit = { x = 3; } }", NULL, error, &conflict);
    CHECK(!body && strstr(error, "not a block assignment"));
}

int main(int argc, char **argv)
{
    first_match_rows();
    grouped_collections();
    presentation_duplicates();
    presentation_ordering(); control_behavior_identity();
    inherited_collection_patches();
    typed_source_annotations();
    fields(); lists(); blocking(); editor_properties(); ordering(); derived_ordering(); significant_ordering(); added_objects(); conflict_sources(); typed_collections(); entity_repeats(); entity_header_order(); malformed(); large_text();
    if (argc == 2) native_file(argv[1]);
    if (failures) return 1;
    puts("declaration composition checks passed");
    return 0;
}
