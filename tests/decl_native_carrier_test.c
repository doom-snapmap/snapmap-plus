#include "../src/backend/decl_native_carrier.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)

/* Accepted spelling: the JSON text SnapMap loads back exactly. */
static void accepts(const char *value, const char *type, const char *json)
{
    char *out = NULL; sh_decl_carrier_refusal refusal;
    int result = sh_decl_carrier_scalar(value, type, &out, &refusal);
    if (result != 1 || !out || strcmp(out, json))
        fprintf(stderr, "accept %s (%s): got %d '%s' (%s)\n", value, type ? type : "-", result,
            out ? out : "", refusal.reason ? refusal.reason : "");
    CHECK(result == 1 && out && !strcmp(out, json));
    free(out);
}
static void refuses(const char *value, const char *type, sh_decl_carrier_limit limit)
{
    char *out = NULL; sh_decl_carrier_refusal refusal;
    int result = sh_decl_carrier_scalar(value, type, &out, &refusal);
    if (result != 0 || refusal.limit != limit)
        fprintf(stderr, "refuse %s (%s): got %d limit %d\n", value, type ? type : "-", result, (int)refusal.limit);
    CHECK(result == 0 && !out && refusal.limit == limit && refusal.reason);
    free(out);
}

static void scalars(void)
{
    /* Identity spellings load back byte-identical, whatever the field type. */
    accepts("5", NULL, "5");
    accepts("-12", "float", "-12");
    accepts("0", "int", "0");
    accepts("2147483646", NULL, "2147483646");
    accepts("-2147483647", NULL, "-2147483647");
    accepts("true", NULL, "true");
    accepts("false", "bool", "false");
    accepts("NULL", NULL, "null");
    accepts("\"ai/demon/imp\"", NULL, "\"ai/demon/imp\"");
    accepts("\"\"", NULL, "\"\"");
    /* JsonCpp asInt logs and truncates at these bounds and beyond. */
    refuses("2147483647", NULL, SH_DECL_CARRIER_ENGINE);
    refuses("-2147483648", NULL, SH_DECL_CARRIER_ENGINE);
    refuses("99999999999999999999999", NULL, SH_DECL_CARRIER_ENGINE);
    /* The lexer reads a leading zero as octal; the carrier writer as decimal. */
    refuses("010", NULL, SH_DECL_CARRIER_GAP);
    refuses("-0", NULL, SH_DECL_CARRIER_GAP);
    /* The carrier escapes these bytes; declaration parsing keeps them raw. */
    refuses("\"don't\"", NULL, SH_DECL_CARRIER_ENGINE);
    refuses("\"why?\"", NULL, SH_DECL_CARRIER_ENGINE);
    refuses("\"a\\b\"", NULL, SH_DECL_CARRIER_ENGINE);
    refuses("\"tab\there\"", NULL, SH_DECL_CARRIER_ENGINE);
    refuses("", NULL, SH_DECL_CARRIER_ENGINE);
    /* Outside the writer's grammar: equivalence would need its reader. */
    refuses("\"a\" \"b\"", NULL, SH_DECL_CARRIER_GAP);
    refuses("SPAWN_AI_TYPE_IMP", NULL, SH_DECL_CARRIER_GAP);
    refuses("( 1, 1, 1 )", NULL, SH_DECL_CARRIER_GAP);
    refuses("1.2.3", "float", SH_DECL_CARRIER_GAP);
    refuses("-", NULL, SH_DECL_CARRIER_GAP);
    /* Decimal spellings are rewritten through "%1.10f": exact float fields only. */
    accepts("0.35", "float", "0.349999994");
    accepts("1.5", "float", "1.5");
    accepts("5.0", "float", "5.0");
    accepts(".5", "float", "0.5");
    accepts("123456.789", "float", "123456.7890625");
    accepts("0.0000000001", "float", "0.0000000001");
    /* Native float tokens may use a lowercase exponent. Preserve the float
     * value through the carrier's decimal writer, including integral results. */
    accepts("1e5", "float", "100000.0");
    accepts("-1e5", "float", "-100000.0");
    accepts("1.25e-1", "float", "0.125");
    accepts(".5e+2", "float", "50.0");
    accepts("0.5e1", "float", "5.0");
    accepts("1.e1", "float", "10.0");
    accepts("1e-10", "float", "0.0000000001");
    accepts("0.0e0", "float", "0.0");
    /* ReadNumber includes an empty exponent; native atof rewinds it and
     * converts the already-read mantissa on both renderers. */
    accepts("1e", "float", "1.0");
    accepts("1e+", "float", "1.0");
    accepts("1e-", "float", "1.0");
    accepts("-1.25e+", "float", "-1.25");
    accepts("0.0e", "float", "0.0");
    /* These do not form the same native float token, or need a different
     * field reader. Never turn a partial token into a different valid value. */
    const char *invalid[] = {"1E5", "1e+-", "1e1.5", "1e2e3",
        "1ee2", "1e 2", "1.2.3e1", "e1", ".e1", "+1e2", "0e5", "00.5", "01.5e1"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        refuses(invalid[i], "float", SH_DECL_CARRIER_GAP);
    refuses("1e5", NULL, SH_DECL_CARRIER_GAP);
    refuses("1e5", "int", SH_DECL_CARRIER_GAP);
    refuses("1e5", "double", SH_DECL_CARRIER_GAP);
    refuses("1e+", "int", SH_DECL_CARRIER_GAP);
    refuses("1e", NULL, SH_DECL_CARRIER_GAP);
    refuses("1e-11", "float", SH_DECL_CARRIER_ENGINE);
    refuses("1e-39", "float", SH_DECL_CARRIER_ENGINE);
    refuses("-0.0e0", "float", SH_DECL_CARRIER_ENGINE);
    refuses("1e100", "float", SH_DECL_CARRIER_GAP);
    refuses("1.6777216e7", "float", SH_DECL_CARRIER_GAP);
    /* FS_WriteFloatString flushes |v| < FLT_MIN, zero included, to +0: the
     * sign of -0.0 does not survive loading. */
    refuses("-0.0", "float", SH_DECL_CARRIER_ENGINE);
    refuses("0.35", "int", SH_DECL_CARRIER_GAP);
    refuses("0.35", NULL, SH_DECL_CARRIER_GAP);
    refuses("0.35", "double", SH_DECL_CARRIER_GAP);
    refuses("0.0000001234567", "float", SH_DECL_CARRIER_ENGINE);
    refuses("0.000000000000000000000000000000000000001", "float", SH_DECL_CARRIER_ENGINE);
    refuses("16777216.5", "float", SH_DECL_CARRIER_GAP);
}

/* Leaf types by member name for the state fixtures. */
static void fixture_type(void *context, const sh_decl_node *leaf, sh_decl_carrier_leaf *info)
{
    (void)context;
    if (!strcmp(leaf->key, "speed") || !strcmp(leaf->key, "x") || !strcmp(leaf->key, "y")) info->type = "float";
    if (!strcmp(leaf->key, "health")) info->type = "int";
}
static sh_decl_node *parse(const char *text)
{
    char error[256];
    sh_decl_node *tree = sh_decl_tree_parse((sh_decl_source){text, strlen(text)}, error, sizeof(error));
    if (!tree) fprintf(stderr, "parse: %s\n", error);
    assert(tree);
    return tree;
}
static void state(void)
{
    sh_decl_carrier_refusal refusal; size_t length = 0;
    sh_decl_node *tree = parse("{ edit = { health = 50; speed = 0.35; model = \"md6def/imp\";"
        " spawnPosition = { x = 1.25; y = -3; } list = { num = 2; item[0] = 7; item[1] = 8; } } }");
    char *json = sh_decl_carrier_state(tree, fixture_type, NULL, &length, &refusal);
    const char *expected = "{\"edit\":{\"health\":50,\"speed\":0.349999994,\"model\":\"md6def/imp\","
        "\"spawnPosition\":{\"x\":1.25,\"y\":-3},\"list\":{\"num\":2,\"item[0]\":7,\"item[1]\":8}}}";
    if (!json || strcmp(json, expected)) fprintf(stderr, "state: %s\n", json ? json : refusal.reason);
    CHECK(json && !strcmp(json, expected) && length == strlen(expected));
    free(json); sh_decl_tree_free(tree);

    /* Exponents survive the JSON -> loaded declaration -> JSON round trip. */
    tree = parse("{ edit = { spawnPosition = { x = 1e5; } } }");
    json = sh_decl_carrier_state(tree, fixture_type, NULL, &length, &refusal);
    CHECK(json && !strcmp(json, "{\"edit\":{\"spawnPosition\":{\"x\":100000.0}}}"));
    if (json) {
        char error[256]; size_t again_length = 0;
        sh_decl_node *loaded = sh_decl_carrier_tree(json, length, NULL, NULL, error, sizeof(error));
        char *again = loaded ? sh_decl_carrier_state(loaded, fixture_type, NULL, &again_length, &refusal) : NULL;
        CHECK(again && !strcmp(again, "{\"edit\":{\"spawnPosition\":{\"x\":100000}}}"));
        free(again); sh_decl_tree_free(loaded);
    }
    free(json); sh_decl_tree_free(tree);
    /* The refusal names the member and whether it is a native limit. */
    tree = parse("{ edit = { spawnPosition = { x = 1E5; } } }");
    json = sh_decl_carrier_state(tree, fixture_type, NULL, &length, &refusal);
    CHECK(!json && refusal.limit == SH_DECL_CARRIER_GAP && !strcmp(refusal.path, "edit.spawnPosition.x"));
    free(json); sh_decl_tree_free(tree);
    tree = parse("{ edit = { title = \"it's\"; } }");
    json = sh_decl_carrier_state(tree, fixture_type, NULL, &length, &refusal);
    CHECK(!json && refusal.limit == SH_DECL_CARRIER_ENGINE && !strcmp(refusal.path, "edit.title"));
    free(json); sh_decl_tree_free(tree);
    /* No carrier spelling for a reset block; a bare block is rewritten with '='. */
    tree = parse("{ edit = { renderModelInfo = ! { model = \"x\"; } } }");
    json = sh_decl_carrier_state(tree, fixture_type, NULL, &length, &refusal);
    CHECK(!json && refusal.limit == SH_DECL_CARRIER_ENGINE && !strcmp(refusal.path, "edit.renderModelInfo"));
    free(json); sh_decl_tree_free(tree);
    tree = parse("{ edit { health = 5; } }");
    json = sh_decl_carrier_state(tree, fixture_type, NULL, &length, &refusal);
    CHECK(!json && refusal.limit == SH_DECL_CARRIER_GAP && !strcmp(refusal.path, "edit"));
    free(json); sh_decl_tree_free(tree);
    /* Empty object round trips. */
    tree = parse("{ edit = { } }");
    json = sh_decl_carrier_state(tree, fixture_type, NULL, &length, &refusal);
    CHECK(json && !strcmp(json, "{\"edit\":{}}"));
    free(json); sh_decl_tree_free(tree);
}

/* The loaded declaration text SnapMap writes from JSON state, leaf by leaf,
 * with each leaf's original JSON span for verbatim re-emission. */
typedef struct loaded_leaf { char key[32], json[64]; } loaded_leaf;
typedef struct loaded_leaves { loaded_leaf rows[16]; size_t count; } loaded_leaves;
static int record_leaf(void *context, const sh_decl_node *leaf, const char *json, size_t length)
{
    loaded_leaves *leaves = context;
    if (leaves->count == 16 || length >= sizeof(leaves->rows[0].json)) return 0;
    snprintf(leaves->rows[leaves->count].key, sizeof(leaves->rows[0].key), "%s", leaf->key);
    memcpy(leaves->rows[leaves->count].json, json, length); leaves->rows[leaves->count++].json[length] = 0;
    return 1;
}
static void loaded(void)
{
    const char *json = "{\"edit\":{\"spawnPosition\":{\"x\":-1397.2294921875,\"y\":3840},"
        "\"title\":\"it's\",\"n\":5,\"big\":4294967296,\"on\":true,\"none\":null,\"f\":0.35}}";
    loaded_leaves leaves = {0}; char error[256];
    sh_decl_node *tree = sh_decl_carrier_tree(json, strlen(json), record_leaf, &leaves, error, sizeof(error));
    CHECK(tree != NULL);
    if (!tree) { fprintf(stderr, "loaded: %s\n", error); return; }
    size_t length = 0; char *text = sh_decl_tree_write(tree, &length);
    /* The loaded string keeps the carrier's escape (declaration parsing does not
     * decode it); a big integer is truncated to 32 bits by asInt. */
    const char *expected[] = {"x = -1397.2294921875;", "y = 3840;", "title = \"it\\'s\";", "n = 5;",
        "big = 0;", "on = true;", "none = NULL;", "f = 0.349999994;"};
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        if (!text || !strstr(text, expected[i])) fprintf(stderr, "loaded text lacks %s\n%s\n", expected[i], text ? text : "");
        CHECK(text && strstr(text, expected[i]));
    }
    CHECK(leaves.count == 8 && !strcmp(leaves.rows[0].key, "x") && !strcmp(leaves.rows[0].json, "-1397.2294921875") &&
        !strcmp(leaves.rows[2].json, "\"it's\""));
    free(text); sh_decl_tree_free(tree);
    /* The native state reader rejects arrays. */
    const char *array = "{\"edit\":{\"list\":[1,2]}}";
    tree = sh_decl_carrier_tree(array, strlen(array), NULL, NULL, error, sizeof(error));
    CHECK(!tree && strstr(error, "array"));
}

int main(void)
{
    scalars();
    state();
    loaded();
    if (failures) { fprintf(stderr, "%d carrier check(s) failed\n", failures); return 1; }
    puts("native carrier checks passed");
    return 0;
}
