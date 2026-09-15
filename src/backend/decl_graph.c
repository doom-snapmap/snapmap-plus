#include "decl_graph.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sh_decl_graph {
    sh_decl_node *syntax;
    const sh_decl_node *state;
    sh_decl_graph_record *records;
    size_t count, capacity;
    sh_decl_graph_text layers[32]; /* Native graph items store a uint32 mask. */
    size_t layer_count;
    char *error;
    size_t error_capacity;
};

static int dg_fail(sh_decl_graph *g, const char *where, const char *reason)
{
    if (g->error && g->error_capacity && !g->error[0])
        snprintf(g->error, g->error_capacity, "graph %s: %s", where, reason);
    return 0;
}

static int dg_text(sh_decl_graph *g, const sh_decl_node *node,
    sh_decl_graph_text *text, const char *where)
{
    if (!sh_decl_tree_literal(node, &text->text, &text->length))
        return dg_fail(g, where, "expected an unescaped literal");
    return 1;
}

static int dg_same(sh_decl_graph_text a, sh_decl_graph_text b)
{ return a.length == b.length && !memcmp(a.text, b.text, a.length); }

/* These are reader-owned envelope fields, not ordinary reflected state. */
static int dg_fields(sh_decl_graph *g, const sh_decl_node *node,
    const char *const *keys, size_t count, const char *where)
{
    const sh_decl_node *child, *previous;
    size_t previous_index = 0;
    int have_previous = 0;
    if (!node || !node->compound || node->reset)
        return dg_fail(g, where, "expected a graph envelope block");
    for (child = node->children; child; child = child->next) {
        size_t i;
        for (i = 0; i < count && strcmp(keys[i], child->key); i++) {}
        if (i == count) return dg_fail(g, where, "unsupported graph envelope field");
        for (previous = node->children; previous != child; previous = previous->next)
            if (!strcmp(previous->key, child->key))
                return dg_fail(g, where, "repeated singleton field");
        if (have_previous && i < previous_index)
            return dg_fail(g, where, "field order does not match the native graph reader");
        previous_index = i; have_previous = 1;
    }
    return 1;
}

static int dg_collection(sh_decl_graph *g, const sh_decl_node *node,
    const char *key, const char *where)
{
    const sh_decl_node *child;
    if (!node) return 1;
    if (!node->compound || node->reset) return dg_fail(g, where, "expected a graph record collection");
    for (child = node->children; child; child = child->next)
        if (key && strcmp(child->key, key)) return dg_fail(g, where, "unexpected graph record kind");
    return 1;
}

static int dg_layers(sh_decl_graph *g, const sh_decl_node *node, int define,
    uint32_t *mask, const char *where)
{
    const sh_decl_node *child;
    if (mask) *mask = 0;
    if (!dg_collection(g, node, "layer", where)) return 0;
    for (child = node ? node->children : NULL; child; child = child->next) {
        sh_decl_graph_text name;
        size_t i;
        if (!dg_text(g, child, &name, where)) return 0;
        for (i = 0; i < g->layer_count && !dg_same(g->layers[i], name); i++) {}
        if (define) {
            if (i != g->layer_count) return dg_fail(g, where, "duplicate layer name has ambiguous bit identity");
            if (g->layer_count == 32) return dg_fail(g, where, "native graph layer mask has only 32 bits");
            g->layers[g->layer_count++] = name;
        } else {
            if (i == g->layer_count) return dg_fail(g, where, "layer name does not resolve");
            if (mask) *mask |= UINT32_C(1) << i;
        }
    }
    return 1;
}

static int dg_record(sh_decl_graph *g, const sh_decl_node *node,
    sh_decl_graph_kind kind, size_t parent, size_t *index)
{
    static const char *const sub_fields[] = {"object", "layers", "nodes", "links"};
    static const char *const node_fields[] = {"object", "layers"};
    static const char *const link_fields[] = {"object", "startNode", "endNode"};
    static const char *const wrapper_fields[] = {"className", "object"};
    const sh_decl_node *wrapper, *state, *name, *member;
    sh_decl_graph_record record = {0};
    const char *where = kind == SH_DECL_GRAPH_SUBGRAPH ? "subGraph" :
        kind == SH_DECL_GRAPH_NODE ? "node" : "link";
    if (!dg_fields(g, node, kind == SH_DECL_GRAPH_SUBGRAPH ? sub_fields :
        kind == SH_DECL_GRAPH_NODE ? node_fields : link_fields,
        kind == SH_DECL_GRAPH_SUBGRAPH ? 4 : kind == SH_DECL_GRAPH_NODE ? 2 : 3, where)) return 0;
    wrapper = sh_decl_tree_member(node, "object");
    if (!dg_fields(g, wrapper, wrapper_fields, 2, where)) return 0;
    state = sh_decl_tree_member(wrapper, "object");
    if (!state || !state->compound || state->reset) return dg_fail(g, where, "missing object state");
    name = sh_decl_tree_member(state, "name");
    /* Only the graph name is interpreted here. A nested object may itself have
     * a custom reader, so preserving its repeated fields is essential. */
    for (member = name ? name->next : NULL; member; member = member->next)
        if (!strcmp(member->key, "name")) return dg_fail(g, where, "repeated object name");
    if (!dg_text(g, name, &record.name, where) ||
        !dg_text(g, sh_decl_tree_member(wrapper, "className"), &record.class_name, where) ||
        !record.class_name.length) return dg_fail(g, where, "missing object class");
    if (kind != SH_DECL_GRAPH_LINK && !dg_layers(g, sh_decl_tree_member(node, "layers"),
        0, &record.layers, where)) return 0;
    record.kind = kind; record.syntax = node; record.state = state; record.parent = parent;
    record.start = record.end = SIZE_MAX;
    if (g->count == g->capacity) {
        size_t capacity = g->capacity ? g->capacity * 2 : 32;
        sh_decl_graph_record *grown;
        if (capacity < g->capacity || capacity > SIZE_MAX / sizeof(*grown) ||
            !(grown = (sh_decl_graph_record *)realloc(g->records, capacity * sizeof(*grown))))
            return dg_fail(g, where, "record allocation failed");
        g->records = grown; g->capacity = capacity;
    }
    *index = g->count;
    g->records[g->count++] = record;
    return 1;
}

static size_t dg_find_node(const sh_decl_graph *g, sh_decl_graph_text name)
{
    size_t i;
    for (i = 0; i < g->count; i++)
        if (g->records[i].kind == SH_DECL_GRAPH_NODE && dg_same(g->records[i].name, name)) return i;
    return SIZE_MAX;
}

static int dg_read(sh_decl_graph *g)
{
    static const char *const root_fields[] = {"inherit", "edit"};
    static const char *const edit_fields[] = {"object", "layers", "layersConfigs", "subGraphs"};
    const sh_decl_node *edit, *configs, *subgraphs, *subgraph, *config, *parent;
    sh_decl_graph_text ignored;
    if (!dg_fields(g, g->syntax, root_fields, 2, "root")) return 0;
    parent = sh_decl_tree_member(g->syntax, "inherit");
    if (parent && !dg_text(g, parent, &ignored, "inherit")) return 0;
    edit = sh_decl_tree_member(g->syntax, "edit");
    if (!edit) return 1;
    if (!dg_fields(g, edit, edit_fields, 4, "edit")) return 0;
    g->state = sh_decl_tree_member(edit, "object");
    if (g->state && (!g->state->compound || g->state->reset))
        return dg_fail(g, "edit.object", "expected root object state");
    if (!dg_layers(g, sh_decl_tree_member(edit, "layers"), 1, NULL, "layers")) return 0;
    configs = sh_decl_tree_member(edit, "layersConfigs");
    if (!dg_collection(g, configs, NULL, "layersConfigs")) return 0;
    for (config = configs ? configs->children : NULL; config; config = config->next)
        if (!dg_layers(g, config, 0, NULL, "layersConfigs")) return 0;
    subgraphs = sh_decl_tree_member(edit, "subGraphs");
    if (!dg_collection(g, subgraphs, "subGraph", "subGraphs")) return 0;
    /* Resolve endpoints only after all subgraphs and nodes have been collected,
     * just as the native reader does. This retains forward and cross-graph links. */
    for (subgraph = subgraphs ? subgraphs->children : NULL; subgraph; subgraph = subgraph->next) {
        const sh_decl_node *nodes, *node;
        size_t parent_index, unused;
        if (!dg_record(g, subgraph, SH_DECL_GRAPH_SUBGRAPH, SIZE_MAX, &parent_index)) return 0;
        nodes = sh_decl_tree_member(subgraph, "nodes");
        if (!dg_collection(g, nodes, "node", "nodes")) return 0;
        for (node = nodes ? nodes->children : NULL; node; node = node->next)
            if (!dg_record(g, node, SH_DECL_GRAPH_NODE, parent_index, &unused)) return 0;
    }
    {
        size_t i, objects = g->count;
        for (i = 0; i < objects; i++) if (g->records[i].kind == SH_DECL_GRAPH_SUBGRAPH) {
            const sh_decl_node *links = sh_decl_tree_member(g->records[i].syntax, "links"), *group;
            if (!dg_collection(g, links, NULL, "links")) return 0;
            for (group = links ? links->children : NULL; group; group = group->next) {
                const sh_decl_node *link;
                sh_decl_node key = {0};
                sh_decl_graph_text group_name;
                key.value = group->key;
                if (!dg_text(g, &key, &group_name, "link group") ||
                    !dg_collection(g, group, "link", "link group")) return 0;
                /* Empty groups contain no behavior and can remain unresolved. */
                if (group->children) {
                    size_t source = dg_find_node(g, group_name);
                    if (source == SIZE_MAX) return dg_fail(g, "link group", "source node does not resolve");
                    /* The group is looked up while its subgraph is read; unlike
                     * the final endpoint fixup it cannot see later subgraphs. */
                    if (g->records[source].parent > i)
                        return dg_fail(g, "link group", "source node belongs to a later subgraph");
                }
                for (link = group->children; link; link = link->next) {
                    sh_decl_graph_text start, end;
                    size_t index;
                    if (!dg_record(g, link, SH_DECL_GRAPH_LINK, i, &index) ||
                        !dg_text(g, sh_decl_tree_member(link, "startNode"), &start, "link.startNode") ||
                        !dg_text(g, sh_decl_tree_member(link, "endNode"), &end, "link.endNode")) return 0;
                    if (!dg_same(start, group_name)) return dg_fail(g, "link", "group and startNode disagree");
                    g->records[index].start = dg_find_node(g, start);
                    g->records[index].end = dg_find_node(g, end);
                    if (g->records[index].start == SIZE_MAX || g->records[index].end == SIZE_MAX)
                        return dg_fail(g, "link", "endpoint node does not resolve");
                }
            }
        }
    }
    return 1;
}

sh_decl_graph *sh_decl_graph_open(sh_decl_source source, char *error, size_t error_capacity)
{
    sh_decl_graph *g;
    if (error && error_capacity) error[0] = 0;
    g = (sh_decl_graph *)calloc(1, sizeof(*g));
    if (!g) {
        if (error && error_capacity) snprintf(error, error_capacity, "graph allocation failed");
        return NULL;
    }
    g->error = error; g->error_capacity = error_capacity;
    g->syntax = sh_decl_tree_parse_ordered(source, error, error_capacity);
    if (!g->syntax || !dg_read(g)) { sh_decl_graph_close(g); return NULL; }
    g->error = NULL; g->error_capacity = 0;
    return g;
}
const sh_decl_node *sh_decl_graph_syntax(const sh_decl_graph *g) { return g ? g->syntax : NULL; }
const sh_decl_node *sh_decl_graph_state(const sh_decl_graph *g) { return g ? g->state : NULL; }
size_t sh_decl_graph_count(const sh_decl_graph *g) { return g ? g->count : 0; }
const sh_decl_graph_record *sh_decl_graph_at(const sh_decl_graph *g, size_t index)
{ return g && index < g->count ? &g->records[index] : NULL; }
void sh_decl_graph_close(sh_decl_graph *g)
{ if (g) { sh_decl_tree_free(g->syntax); free(g->records); free(g); } }
