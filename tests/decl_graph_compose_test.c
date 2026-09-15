#include "decl_graph_compose.h"
#include "decl_graph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); failures++; } } while (0)
#define OBJECT(c, n, s) "object = { className = \"" c "\"; object = { name = \"" n "\"; " s " } } "
#define NODE(n, s) "node = { " OBJECT("Node", n, s) "} "
#define LINK(n, a, b, s) "link = { " OBJECT("Link", n, s) "startNode = \"" a "\"; endNode = \"" b "\"; } "
#define SUB(n, nodes, links) "subGraph = { " OBJECT("Sub", n, "") "nodes = { " nodes "} links = { " links "} } "
#define GRAPH(subs) "{ edit = { subGraphs = { " subs "} } }"
#define A NODE("a", "x = 1; y = 2;")
#define AX NODE("a", "x = 3; y = 2;")
#define AY NODE("a", "x = 1; y = 4;")
#define B NODE("b", "")
static sh_decl_source view(const char *text) { return (sh_decl_source){text, strlen(text)}; }
static int describe(void *context, sh_decl_value_type type, sh_decl_value_shape *out)
{
    (void)context;
    if (!strcmp(type.name, "Root")) return 1; /* Its native reader is custom. */
    if (!strcmp(type.name, "Sub") || !strcmp(type.name, "Node") || !strcmp(type.name, "Link")) out->kind = SH_DECL_VALUE_OBJECT;
    else if (!strcmp(type.name, "number") || !strcmp(type.name, "string")) out->kind = SH_DECL_VALUE_IGNORE;
    return 1;
}
static int field(void *context, sh_decl_value_type owner, const char *key, sh_decl_value_type *out)
{
    (void)context; (void)owner;
    out->ops = "";
    if (!strcmp(key, "name")) out->name = "string";
    else if (!strcmp(key, "x") || !strcmp(key, "y")) out->name = "number";
    else if (!strcmp(key, "custom")) out->name = "Opaque";
    else return 0;
    return 1;
}
static int dynamic(void *context, sh_decl_value_type base, const char *name, size_t length, sh_decl_value_type *out)
{
    const char *expected = !strcmp(base.name, "idTypeInfoSubGraph") ? "Sub" :
        !strcmp(base.name, "idTypeInfoGraphNode") ? "Node" : !strcmp(base.name, "idTypeInfoGraphLink") ? "Link" : NULL;
    (void)context;
    if (!expected || strlen(expected) != length || memcmp(expected, name, length)) return 0;
    *out = (sh_decl_value_type){expected, ""}; return 1;
}
static char *compose(const char *base, const char *a, const char *b, char *error)
{
    sh_decl_dependency_schema types = {NULL, describe, field, NULL, NULL, dynamic};
    sh_decl_source sources[] = {view(a), view(b)};
    size_t length = 999;
    char *out = sh_decl_graph_compose(view(base), sources, 2, &types, (sh_decl_value_type){"Root", ""},
        &length, error, 512, NULL);
    CHECK(out ? strlen(out) == length : length == 0);
    return out;
}
static void pass(const char *base, const char *a, const char *b, size_t count, const char *x, const char *y)
{
    char error[512], *out = compose(base, a, b, error), *reverse;
    sh_decl_graph *graph;
    CHECK(out != NULL);
    if (!out) { fprintf(stderr, "unexpected refusal: %s\n", error); return; }
    CHECK(!strstr(out, "startTarget") && !strstr(out, "endTarget") && !strstr(out, "nodeBindings") &&
        !strstr(out, "@graph/") && !strstr(out, "id ="));
    if (x) CHECK(strstr(out, x));
    if (y) CHECK(strstr(out, y));
    graph = sh_decl_graph_open(view(out), error, sizeof(error));
    CHECK(graph && sh_decl_graph_count(graph) == count);
    sh_decl_graph_close(graph);
    reverse = compose(base, b, a, error);
    CHECK(reverse && !strcmp(out, reverse));
    free(out); free(reverse);
}
static void refuse(const char *base, const char *a, const char *b, const char *reason)
{
    char error[512], *out = compose(base, a, b, error);
    CHECK(!out && strstr(error, reason));
    if (out || !strstr(error, reason)) fprintf(stderr, "expected %s; got %s\n", reason, out ? out : error);
    free(out);
}
static void state_and_additions(void)
{
    pass(GRAPH(SUB("g", A, "")), GRAPH(SUB("g", AX, "")), GRAPH(SUB("g", AY, "")), 2, "x = 3;", "y = 4;");
    pass(GRAPH(SUB("g", A, "")), GRAPH(SUB("g", A B, "")), GRAPH(SUB("g", A NODE("c", ""), "")), 4, "\"b\"", "\"c\"");
    /* Same newly introduced graph records still compose against empty state. */
    pass("{}", GRAPH(SUB("g", NODE("a", "x = 3;"), "")), GRAPH(SUB("g", NODE("a", "y = 4;"), "")), 2, "x = 3;", "y = 4;");
    refuse(GRAPH(SUB("g", A, "")), GRAPH(SUB("g", AX, "")), GRAPH(SUB("g", NODE("a", "x = 8; y = 2;"), "")), "same field");
    /* Native root object bypasses only its own custom reader. */
    pass("{ edit = { object = { x = 1; y = 2; } } }", "{ edit = { object = { x = 3; y = 2; } } }",
        "{ edit = { object = { x = 1; y = 4; } } }", 0, "x = 3;", "y = 4;");
    refuse(GRAPH(SUB("g", NODE("a", "custom = { x = 1; y = 2; }"), "")),
        GRAPH(SUB("g", NODE("a", "custom = { x = 3; y = 2; }"), "")),
        GRAPH(SUB("g", NODE("a", "custom = { x = 1; y = 4; }"), "")), "verified composition adapter");
}
static void occurrences(void)
{
    /* Identical transitions retain multiplicity; edits to different slots sum. */
#define L LINK("same", "a", "a", "x = 1; y = 2;")
#define LX LINK("same", "a", "a", "x = 3; y = 2;")
#define LY LINK("same", "a", "a", "x = 1; y = 4;")
    pass(GRAPH(SUB("g", A, "a = { " L L "}")), GRAPH(SUB("g", A, "a = { " LX L "}")),
        GRAPH(SUB("g", A, "a = { " L LY "}")), 4, "x = 3;", "y = 4;");
    pass(GRAPH(SUB("g", A, "a = { " L "}")), GRAPH(SUB("g", A, "a = { " L L "}")),
        GRAPH(SUB("g", A, "a = { " L L L "}")), 5, NULL, NULL);
    refuse(GRAPH(SUB("g", A, "a = { " L L "}")), GRAPH(SUB("g", A, "a = { " LX "}")),
        GRAPH(SUB("g", AX, "a = { " L L "}")), "multiplicity");
    /* Node names are scoped by subgraph for alignment, globally first for lookup. */
    pass(GRAPH(SUB("first", A, "") SUB("second", A, "")),
        GRAPH(SUB("first", AX, "") SUB("second", A, "")),
        GRAPH(SUB("first", A, "") SUB("second", AY, "")), 4, "x = 3;", "y = 4;");
    /* A removed unique node must conflict with a surviving reference to it. */
    refuse(GRAPH(SUB("g", A B, "a = { " LINK("t", "a", "b", "") "}")),
        GRAPH(SUB("g", A, "")), GRAPH(SUB("g", A B, "a = { " LINK("t", "a", "b", "x = 3;") "}")), "same field");
}
static void ordering_and_references(void)
{
    const char *base = GRAPH(SUB("first", A, "a = { " LINK("t", "a", "a", "") "}") SUB("second", B, ""));
    const char *a = GRAPH(SUB("first", A, "a = { " LINK("t", "a", "a", "x = 3;") "}") SUB("second", B, ""));
    const char *b = GRAPH(SUB("first", A, "a = { " LINK("t", "a", "b", "") "}") SUB("second", B, ""));
    pass(base, a, b, 5, "x = 3;", "endNode = \"b\";");
    /* Independent additions introduce the same globally looked-up node name.
     * Each author's link originally targets its own subgraph. Output cannot
     * honor both, even though textual endpoints still resolve successfully. */
    refuse("{}", GRAPH(SUB("first", A, "a = { " LINK("t", "a", "a", "") "}")),
        GRAPH(SUB("second", A, "a = { " LINK("t", "a", "a", "") "}")), "nodeBindings");
    /* The first-name lookup contract also applies without an explicit link. */
    refuse("{}", GRAPH(SUB("first", A, "")), GRAPH(SUB("second", A, "")), "nodeBindings");
    pass(GRAPH(SUB("g", A B NODE("c", ""), "")), GRAPH(SUB("g", B A NODE("c", ""), "")),
        GRAPH(SUB("g", A B NODE("c", "x = 3;"), "")), 4, "x = 3;", NULL);
    /* Removing an unused first duplicate must not accidentally transfer the
     * second author's newly added reference to a different surviving node. */
    refuse(GRAPH(SUB("first", A, "") SUB("second", A, "")), GRAPH(SUB("second", A, "")),
        GRAPH(SUB("first", A, "") SUB("second", A, "a = { " LINK("t", "a", "a", "") "}")), "endpoint occurrence");
}
static void layers(void)
{
    pass("{ edit = { layers = { layer = a; } } }",
        "{ edit = { layers = { layer = a; layer = b; } layersConfigs = { enabled = { layer = b; } } } }",
        "{ edit = { layers = { layer = \"a\"; layer = c; } layersConfigs = { enabled = { layer = c; } } } }",
        0, "layer = \"b\";", "layer = \"c\";");
    char base[2048] = "{ edit = { layers = { ", a[2048], b[2048], entry[64];
    for (int i = 0; i < 31; i++) { snprintf(entry, sizeof(entry), "layer = l%d; ", i); strcat_s(base, sizeof(base), entry); }
    strcpy_s(a, sizeof(a), base); strcpy_s(b, sizeof(b), base); strcat_s(base, sizeof(base), "} } }");
    strcat_s(a, sizeof(a), "layer = a; } } }"); strcat_s(b, sizeof(b), "layer = b; } } }");
    refuse(base, a, b, "32 bits");
}
int main(void)
{
    state_and_additions(); occurrences(); ordering_and_references(); layers();
    printf("graph composition: %d failure(s)\n", failures); return failures != 0;
}
