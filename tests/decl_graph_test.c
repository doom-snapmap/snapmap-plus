#include "decl_graph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); failures++; } } while (0)
#define OBJECT(c, n, fields) "object = { className = \"" c "\"; object = { name = \"" n "\"; " fields " } } "
#define NODE(n) "node = { " OBJECT("Node", n, "") "} "
#define LINK(n, a, b, state) "link = { " OBJECT("Link", n, state) "startNode = \"" a "\"; endNode = \"" b "\"; } "

static sh_decl_graph *parse(const char *text, char *error)
{ return sh_decl_graph_open((sh_decl_source){text, strlen(text)}, error, 256); }
static int same(sh_decl_graph_text value, const char *text)
{ return value.length == strlen(text) && !memcmp(value.text, text, value.length); }

static void topology(void)
{
    const char *source = "{ edit = { object = { enabled = true; } "
        "layers = { layer = \"primary\"; layer = \"secondary\"; } "
        "layersConfigs = { config = { layer = \"secondary\"; } } subGraphs = { "
        "subGraph = { " OBJECT("SubGraph", "first", "")
        "layers = { layer = \"primary\"; } nodes = { "
        "node = { " OBJECT("Node", "duplicate", "custom = { record = 1; record = 2; }")
        "layers = { layer = \"secondary\"; layer = \"primary\"; layer = \"primary\"; } } "
        "} links = { duplicate = { " LINK("transition", "duplicate", "later", "condition = 1;")
        LINK("transition", "duplicate", "later", "condition = 2;")
        LINK("transition", "duplicate", "later", "condition = 2;") "} } } "
        "subGraph = { " OBJECT("SubGraph", "second", "") "nodes = { " NODE("duplicate") NODE("later")
        "} links = { later = { " LINK("return", "later", "duplicate", "") "} } } } } }";
    char error[256];
    sh_decl_graph *g = parse(source, error), *roundtrip;
    const sh_decl_graph_record *record;
    char *written;
    size_t length;
    CHECK(g != NULL);
    if (!g) { fprintf(stderr, "%s\n", error); return; }
    CHECK(sh_decl_graph_count(g) == 9);
    CHECK(sh_decl_graph_state(g) && sh_decl_tree_member(sh_decl_graph_state(g), "enabled"));
    record = sh_decl_graph_at(g, 0);
    CHECK(record && record->kind == SH_DECL_GRAPH_SUBGRAPH && record->parent == SIZE_MAX && record->layers == 1);
    record = sh_decl_graph_at(g, 1);
    CHECK(record && same(record->name, "duplicate") && same(record->class_name, "Node") && record->layers == 3);
    record = sh_decl_graph_at(g, 3);
    CHECK(record && same(record->name, "duplicate") && record->parent == 2);
    record = sh_decl_graph_at(g, 5);
    CHECK(record && record->kind == SH_DECL_GRAPH_LINK && record->parent == 0 && record->start == 1 && record->end == 4);
    CHECK(sh_decl_graph_at(g, 6)->start == 1 && sh_decl_graph_at(g, 7)->start == 1);
    CHECK(sh_decl_graph_at(g, 8)->start == 4 && sh_decl_graph_at(g, 8)->end == 1);
    CHECK(!sh_decl_graph_at(g, 9));
    written = sh_decl_tree_write(sh_decl_graph_syntax(g), &length);
    CHECK(written && length == strlen(written));
    roundtrip = written ? parse(written, error) : NULL;
    CHECK(roundtrip && sh_decl_graph_count(roundtrip) == 9);
    if (roundtrip) {
        const sh_decl_node *custom = sh_decl_tree_member(sh_decl_graph_at(roundtrip, 1)->state, "custom");
        CHECK(custom && custom->children && custom->children->next);
        CHECK(!strcmp(custom->children->value, "1") && !strcmp(custom->children->next->value, "2"));
        CHECK(sh_decl_graph_at(roundtrip, 8)->end == 1);
    }
    free(written); sh_decl_graph_close(roundtrip); sh_decl_graph_close(g);
}

static void reject(const char *source, const char *message)
{
    char error[256];
    sh_decl_graph *g = parse(source, error);
    CHECK(!g && strstr(error, message));
    if (g || !strstr(error, message)) fprintf(stderr, "expected %s; got %s\n", message, error);
    sh_decl_graph_close(g);
}

static void invalid_envelopes(void)
{
    reject("{ edit = {} edit = {} }", "repeated singleton");
    reject("{ edit = { unknown = 1; } }", "unsupported graph envelope");
    reject("{ edit = { subGraphs = {} layers = {} } }", "field order");
    reject("{ edit = { layers = 3; } }", "collection");
    reject("{ edit = { subGraphs = { node = {} } } }", "record kind");
    reject("{ edit = { subGraphs = { subGraph = { object = { className = \"SubGraph\"; object = {} } } } } }", "literal");
    reject("{ edit = { subGraphs = { subGraph = { " OBJECT("SubGraph", "a", "name = \"b\";") "} } } }", "repeated object name");
    reject("{ edit = { subGraphs = { subGraph = { " OBJECT("SubGraph", "a", "") "object = {} } } } }", "repeated singleton");
    reject("{ edit = { subGraphs = { subGraph = { object = { className = \"\"; object = { name = \"a\"; } } } } } }", "class");
    reject("{ edit = { subGraphs = { subGraph = { " OBJECT("SubGraph", "a", "") "nodes = { node = { "
        OBJECT("Node", "A", "") "layers = { layer = \"unknown\"; } } } } } } }", "does not resolve");
    reject("{ edit = { layers = { layer = \"a\"; layer = \"a\"; } } }", "duplicate layer");
    reject("{ edit = { layersConfigs = { config = { layer = \"a\"; } } } }", "does not resolve");
    reject("{ edit = { layers = { layer = \"a\\nb\"; } } }", "unescaped literal");
    reject("{ edit = { subGraphs = { subGraph = { " OBJECT("SubGraph", "a", "") "nodes = { " NODE("A")
        "} links = { A = { " LINK("t", "A", "a", "") "} } } } } }", "endpoint node does not resolve");
    reject("{ edit = { subGraphs = { subGraph = { " OBJECT("SubGraph", "a", "") "nodes = { " NODE("A") NODE("B")
        "} links = { A = { " LINK("t", "B", "A", "") "} } } } } }", "group and startNode disagree");
    reject("{ edit = { subGraphs = { subGraph = { " OBJECT("SubGraph", "a", "") "nodes = { " NODE("A")
        "} links = { missing = { " LINK("t", "missing", "A", "") "} } } } } }", "source node does not resolve");
    reject("{ edit = ! {} }", "envelope block");
    reject("{ edit = { subGraphs = { subGraph = { " OBJECT("SubGraph", "a", "")
        "links = { empty = { unknown = 3; } } } } } }", "record kind");
    reject("{ edit = { subGraphs = { subGraph = { " OBJECT("SubGraph", "a", "")
        "links = { later = { " LINK("ignored", "later", "later", "") "} } } subGraph = { "
        OBJECT("SubGraph", "b", "") "nodes = { " NODE("later") "} } } } }", "later subgraph");
}

static void extent(void)
{
    char source[8192], error[256];
    size_t i, used = 0;
    sh_decl_graph *g;
    used += (size_t)sprintf(source + used, "{ edit = { layers = { ");
    for (i = 0; i < 32; i++) used += (size_t)sprintf(source + used, "layer = \"layer%zu\"; ", i);
    sprintf(source + used, "} subGraphs = { subGraph = { " OBJECT("SubGraph", "a", "")
        "layers = { layer = \"layer31\"; } } } } }");
    g = parse(source, error);
    CHECK(g && sh_decl_graph_at(g, 0)->layers == UINT32_C(0x80000000));
    sh_decl_graph_close(g);
    sprintf(source + used, "layer = \"overflow\"; } } }");
    reject(source, "only 32 bits");
    /* Repeated empty graphs and empty unresolved groups must not disappear. */
    g = parse("{ edit = { subGraphs = { subGraph = { " OBJECT("SubGraph", "same", "")
        "links = { unknown = {} } } subGraph = { " OBJECT("SubGraph", "same", "") "} } } }", error);
    CHECK(g && sh_decl_graph_count(g) == 2);
    sh_decl_graph_close(g);
    g = parse("{ inherit = \"parent\"; edit = {} }", error);
    CHECK(g && !sh_decl_graph_count(g)); sh_decl_graph_close(g);
    g = parse("{}", error); CHECK(g && !sh_decl_graph_count(g)); sh_decl_graph_close(g);
    CHECK(!sh_decl_graph_at(NULL, 0) && !sh_decl_graph_count(NULL));
    CHECK(!sh_decl_graph_syntax(NULL) && !sh_decl_graph_state(NULL)); sh_decl_graph_close(NULL);
}

int main(void)
{
    topology(); invalid_envelopes(); extent();
    if (failures) { fprintf(stderr, "%d graph failures\n", failures); return 1; }
    puts("decl_graph_test: PASS"); return 0;
}
