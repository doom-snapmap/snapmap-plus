#include "decl_graph_compose.h"
#include "decl_graph.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* These types exist only in the compiler's normalized graph view. They cannot
 * be selected by authored className fields, whose resolver is the native one. */
#define GC_TYPE(s) "@graph/" s
typedef struct gc_context {
    const sh_decl_dependency_schema *types;
    sh_decl_value_type root_type;
    char *error;
    size_t error_capacity;
    int failed;
} gc_context;
typedef struct gc_input {
    sh_decl_graph *graph;
    char **ids;
    char *text;
    size_t length;
} gc_input;

static int gc_fail(gc_context *c, const char *reason)
{
    if (!c->failed && c->error && c->error_capacity)
        snprintf(c->error, c->error_capacity, "graph composition: %s", reason);
    c->failed = 1; return 0;
}
static char *gc_copy(gc_context *c, const char *text)
{
    size_t n = strlen(text);
    char *out = malloc(n + 1);
    if (!out) gc_fail(c, "allocation failed");
    else memcpy(out, text, n + 1);
    return out;
}
static sh_decl_node *gc_node(gc_context *c, const char *key, const char *value)
{
    sh_decl_node *out = calloc(1, sizeof(*out));
    if (!out) { gc_fail(c, "allocation failed"); return NULL; }
    out->key = key ? gc_copy(c, key) : NULL;
    out->value = value ? gc_copy(c, value) : NULL;
    out->compound = value == NULL; out->assignment = key != NULL;
    if (c->failed) { sh_decl_tree_free(out); return NULL; }
    return out;
}
static sh_decl_node *gc_add(sh_decl_node *parent, sh_decl_node *child)
{
    sh_decl_node **tail;
    if (!parent) { sh_decl_tree_free(child); return NULL; }
    tail = &parent->children;
    while (*tail) tail = &(*tail)->next;
    *tail = child; return child;
}
static sh_decl_node *gc_clone(gc_context *c, const sh_decl_node *source, const char *key)
{
    sh_decl_node view, *out;
    char *text;
    size_t length;
    if (!source) return NULL;
    if (!source->compound) return gc_node(c, key, source->value);
    view = *source; view.key = NULL; view.next = NULL;
    text = sh_decl_tree_write(&view, &length);
    out = text ? sh_decl_tree_parse_ordered((sh_decl_source){text, length}, NULL, 0) : NULL;
    free(text);
    if (!out) { gc_fail(c, "state copy failed"); return NULL; }
    out->key = key ? gc_copy(c, key) : NULL;
    out->assignment = source->assignment; out->reset = source->reset;
    if (c->failed) { sh_decl_tree_free(out); return NULL; }
    return out;
}
static const sh_decl_node *gc_member(const sh_decl_node *parent, const char *key)
{ return sh_decl_tree_member(parent, key); }
static int gc_same(sh_decl_graph_text a, sh_decl_graph_text b)
{ return a.length == b.length && !memcmp(a.text, b.text, a.length); }
static int gc_key_equal(const sh_decl_graph_record *a, const sh_decl_graph_record *b)
{ return a->kind == b->kind && a->parent == b->parent && gc_same(a->name, b->name); }

/* Hex names avoid native quoting/escape ambiguities. Ordinals preserve repeated
 * authored occurrences, including several byte-identical transitions. */
static char *gc_id(gc_context *c, sh_decl_graph_text name, size_t ordinal)
{
    static const char digits[] = "0123456789abcdef";
    char *out;
    size_t i, n;
    if (name.length > (SIZE_MAX - 32) / 2 || !(out = malloc(name.length * 2 + 32))) {
        gc_fail(c, "identity allocation failed"); return NULL;
    }
    out[0] = '"';
    for (i = 0; i < name.length; i++) {
        unsigned byte = (unsigned char)name.text[i];
        out[1 + 2 * i] = digits[byte >> 4]; out[2 + 2 * i] = digits[byte & 15];
    }
    n = 1 + name.length * 2;
    snprintf(out + n, 31, "_%zu\"", ordinal); return out;
}
static char *gc_target(gc_context *c, const gc_input *input, size_t index)
{
    const sh_decl_graph_record *record = sh_decl_graph_at(input->graph, index);
    const char *parent, *id;
    size_t a, b;
    char *out;
    if (!record || record->kind != SH_DECL_GRAPH_NODE) { gc_fail(c, "invalid endpoint occurrence"); return NULL; }
    parent = input->ids[record->parent]; id = input->ids[index];
    if (!parent || !id) return NULL;
    a = strlen(parent); b = strlen(id);
    if (b > SIZE_MAX - a || !(out = malloc(a + b))) { gc_fail(c, "endpoint allocation failed"); return NULL; }
    memcpy(out, parent, a - 1); out[a - 1] = '/';
    memcpy(out + a, id + 1, b); return out;
}
static char *gc_binding_key(gc_context *c, sh_decl_graph_text name)
{
    char *key = gc_id(c, name, 0);
    if (key) { key[0] = 'n'; key[strlen(key) - 1] = 0; }
    return key;
}
static int gc_ids(gc_context *c, gc_input *input)
{
    size_t i, n = sh_decl_graph_count(input->graph);
    if (n > SIZE_MAX / sizeof(*input->ids) || !(input->ids = calloc(n ? n : 1, sizeof(*input->ids))))
        return gc_fail(c, "identity allocation failed");
    for (i = 0; i < n; i++) {
        const sh_decl_graph_record *record = sh_decl_graph_at(input->graph, i);
        size_t j, ordinal = 0;
        for (j = 0; j < i; j++) if (gc_key_equal(record, sh_decl_graph_at(input->graph, j))) ordinal++;
        input->ids[i] = gc_id(c, record->name, ordinal);
        if (!input->ids[i]) return 0;
    }
    return 1;
}
static sh_decl_node *gc_list(gc_context *c, const char *key)
{
    sh_decl_node *list = gc_node(c, key, NULL);
    gc_add(list, gc_node(c, "num", "0")); return list;
}
static void gc_item(gc_context *c, sh_decl_node *list, sh_decl_node *item, size_t *count)
{
    char number[32], key[40];
    if (!list || !item || c->failed) { sh_decl_tree_free(item); return; }
    snprintf(key, sizeof(key), "item[%zu]", *count);
    free(item->key); item->key = gc_copy(c, key); item->assignment = 1;
    gc_add(list, item); (*count)++;
    snprintf(number, sizeof(number), "%zu", *count);
    free(list->children->value); list->children->value = gc_copy(c, number);
}
static sh_decl_node *gc_layers(gc_context *c, const sh_decl_node *source, const char *key)
{
    sh_decl_node *list = gc_list(c, key);
    const sh_decl_node *layer;
    size_t count = 0;
    for (layer = source ? source->children : NULL; layer; layer = layer->next) {
        const sh_decl_node *prior;
        const char *name; size_t length;
        if (!sh_decl_tree_literal(layer, &name, &length)) { gc_fail(c, "invalid layer literal"); break; }
        /* Membership is a native bitset; duplicate membership spells the same
         * bit. Top-level duplicate definitions were already rejected. */
        for (prior = source->children; prior != layer; prior = prior->next) {
            const char *other; size_t n;
            if (sh_decl_tree_literal(prior, &other, &n) && n == length && !memcmp(name, other, n)) break;
        }
        if (prior != layer) continue;
        /* Normalize bare and quoted spellings to one exact layer identity. */
        char *value = malloc(length + 3);
        if (!value) { gc_fail(c, "layer allocation failed"); break; }
        value[0] = '"'; memcpy(value + 1, name, length); value[length + 1] = '"'; value[length + 2] = 0;
        gc_item(c, list, gc_node(c, NULL, value), &count); free(value);
    }
    return list;
}
static sh_decl_node *gc_record(gc_context *c, const gc_input *input, size_t index)
{
    const sh_decl_graph_record *r = sh_decl_graph_at(input->graph, index);
    sh_decl_node *out = gc_node(c, NULL, NULL);
    gc_add(out, gc_node(c, "id", input->ids[index]));
    gc_add(out, gc_clone(c, gc_member(r->syntax, "object"), "object"));
    if (r->kind != SH_DECL_GRAPH_LINK)
        gc_add(out, gc_layers(c, gc_member(r->syntax, "layers"), "layers"));
    else {
        char *start = gc_target(c, input, r->start), *end = gc_target(c, input, r->end);
        gc_add(out, gc_clone(c, gc_member(r->syntax, "startNode"), "startNode"));
        gc_add(out, gc_clone(c, gc_member(r->syntax, "endNode"), "endNode"));
        if (start) gc_add(out, gc_node(c, "startTarget", start));
        if (end) gc_add(out, gc_node(c, "endTarget", end));
        free(start); free(end);
    }
    return out;
}
static sh_decl_node *gc_normalize(gc_context *c, const gc_input *input)
{
    const sh_decl_node *syntax = sh_decl_graph_syntax(input->graph), *native_edit = gc_member(syntax, "edit");
    const sh_decl_node *config, *configs = gc_member(native_edit, "layersConfigs");
    sh_decl_node *out = gc_node(c, NULL, NULL), *edit, *list, *graphs, *bindings;
    size_t i, config_count = 0, graph_count = 0;
    gc_add(out, gc_clone(c, gc_member(syntax, "inherit"), "inherit"));
    edit = gc_add(out, gc_node(c, "edit", NULL));
    gc_add(edit, gc_clone(c, sh_decl_graph_state(input->graph), "object"));
    gc_add(edit, gc_layers(c, gc_member(native_edit, "layers"), "layers"));
    list = gc_add(edit, gc_list(c, "layersConfigs"));
    for (config = configs ? configs->children : NULL; config; config = config->next) {
        const sh_decl_node *prior;
        size_t ordinal = 0;
        char *id;
        sh_decl_node *item = gc_node(c, NULL, NULL);
        for (prior = configs->children; prior != config; prior = prior->next)
            if (!strcmp(prior->key, config->key)) ordinal++;
        id = gc_id(c, (sh_decl_graph_text){config->key, strlen(config->key)}, ordinal);
        if (id) gc_add(item, gc_node(c, "id", id));
        free(id); gc_add(item, gc_layers(c, config, "layers")); gc_item(c, list, item, &config_count);
    }
    graphs = gc_add(edit, gc_list(c, "subGraphs"));
    for (i = 0; i < sh_decl_graph_count(input->graph); i++) {
        const sh_decl_graph_record *r = sh_decl_graph_at(input->graph, i);
        sh_decl_node *graph, *nodes, *links;
        size_t j, node_count = 0, link_count = 0;
        if (r->kind != SH_DECL_GRAPH_SUBGRAPH) continue;
        graph = gc_record(c, input, i);
        nodes = gc_add(graph, gc_list(c, "nodes")); links = gc_add(graph, gc_list(c, "links"));
        for (j = 0; j < sh_decl_graph_count(input->graph); j++) {
            const sh_decl_graph_record *child = sh_decl_graph_at(input->graph, j);
            if (child->parent != i) continue;
            if (child->kind == SH_DECL_GRAPH_NODE) gc_item(c, nodes, gc_record(c, input, j), &node_count);
            else if (child->kind == SH_DECL_GRAPH_LINK) gc_item(c, links, gc_record(c, input, j), &link_count);
        }
        gc_item(c, graphs, graph, &graph_count);
    }
    /* First-name lookup is native observable state even when a declaration
     * contains no links. Compose its authored targets too, so two independent
     * additions cannot silently select different nodes for the same name. */
    bindings = gc_add(edit, gc_node(c, "nodeBindings", NULL));
    for (i = 0; i < sh_decl_graph_count(input->graph); i++) {
        const sh_decl_graph_record *r = sh_decl_graph_at(input->graph, i);
        char *key, *target;
        if (r->kind != SH_DECL_GRAPH_NODE) continue;
        key = gc_binding_key(c, r->name);
        if (!key) break;
        if (!gc_member(bindings, key)) {
            target = gc_target(c, input, i);
            if (target) gc_add(bindings, gc_node(c, key, target));
            free(target);
        }
        free(key);
    }
    if (c->failed) { sh_decl_tree_free(out); return NULL; }
    return out;
}

static int gc_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    gc_context *c = context;
    const char *name = type.name;
    if (!strncmp(name, GC_TYPE(""), sizeof(GC_TYPE("")) - 1)) {
        name += sizeof(GC_TYPE("")) - 1;
        if (!strcmp(name, "scalar")) shape->kind = SH_DECL_VALUE_IGNORE;
        else if (!strcmp(name, "subObject") || !strcmp(name, "nodeObject") || !strcmp(name, "linkObject")) {
            shape->kind = SH_DECL_VALUE_POLYMORPHIC;
            shape->element = (sh_decl_value_type){!strcmp(name, "subObject") ? "idTypeInfoSubGraph" :
                !strcmp(name, "nodeObject") ? "idTypeInfoGraphNode" : "idTypeInfoGraphLink", ""};
        } else if (!strncmp(name, "list/", 5)) {
            shape->kind = SH_DECL_VALUE_COLLECTION; shape->item_key = "item"; shape->count_key = "num";
            shape->element = (sh_decl_value_type){!strcmp(name + 5, "layers") ? GC_TYPE("scalar") :
                !strcmp(name + 5, "configs") ? GC_TYPE("config") : !strcmp(name + 5, "subs") ? GC_TYPE("sub") :
                !strcmp(name + 5, "nodes") ? GC_TYPE("node") : GC_TYPE("link"), ""};
        } else shape->kind = SH_DECL_VALUE_OBJECT;
        return 1;
    }
    return c->types->describe(c->types->context, type, shape);
}
static int gc_field(void *context, sh_decl_value_type owner, const char *key, sh_decl_value_type *out)
{
    gc_context *c = context;
    const char *name = owner.name;
    if (!strcmp(name, GC_TYPE("rootState")))
        return c->types->field(c->types->context, c->root_type, key, out);
    if (strncmp(name, GC_TYPE(""), sizeof(GC_TYPE("")) - 1))
        return c->types->field(c->types->context, owner, key, out);
    out->ops = ""; out->name = GC_TYPE("scalar");
    if (!strcmp(key, "object")) out->name = !strcmp(name, GC_TYPE("envelope")) ? GC_TYPE("rootState") :
        !strcmp(name, GC_TYPE("sub")) ? GC_TYPE("subObject") :
        !strcmp(name, GC_TYPE("node")) ? GC_TYPE("nodeObject") : GC_TYPE("linkObject");
    else if (!strcmp(key, "layers")) out->name = GC_TYPE("list/layers");
    else if (!strcmp(key, "layersConfigs")) out->name = GC_TYPE("list/configs");
    else if (!strcmp(key, "subGraphs")) out->name = GC_TYPE("list/subs");
    else if (!strcmp(key, "nodes")) out->name = GC_TYPE("list/nodes");
    else if (!strcmp(key, "links")) out->name = GC_TYPE("list/links");
    else if (!strcmp(key, "nodeBindings")) out->name = GC_TYPE("bindings");
    return 1;
}
static int gc_dynamic(void *context, sh_decl_value_type base, const char *name, size_t length, sh_decl_value_type *out)
{
    gc_context *c = context;
    return c->types->dynamic_type ? c->types->dynamic_type(c->types->context, base, name, length, out) : 0;
}

static const sh_decl_node *gc_first(const sh_decl_node *list)
{ return list && list->children ? list->children->next : NULL; }
static const sh_decl_node *gc_find(const sh_decl_node *list, const char *id)
{
    const sh_decl_node *item;
    for (item = gc_first(list); item; item = item->next) {
        const sh_decl_node *key = gc_member(item, "id");
        if (key && !strcmp(key->value, id)) return item;
    }
    return NULL;
}
static int gc_id_group(const char *a, const char *b)
{
    size_t n = strcspn(a, "_");
    return n == strcspn(b, "_") && !memcmp(a, b, n);
}
static int gc_state_equal(const sh_decl_node *a, const sh_decl_node *b)
{
    sh_decl_node av = *a, bv = *b;
    char *at, *bt; size_t an, bn;
    int same;
    av.key = bv.key = NULL; av.next = bv.next = NULL;
    at = sh_decl_tree_write(&av, &an); bt = sh_decl_tree_write(&bv, &bn);
    same = at && bt && an == bn && !memcmp(at, bt, an);
    free(at); free(bt); return same;
}
/* A duplicated name is positional only while its extent is unchanged. For a
 * changed extent, unchanged common slots establish a trailing append/remove.
 * Refuse a simultaneous replacement of those slots instead of guessing which
 * duplicate was inserted, removed or modified. */
static int gc_extent(gc_context *c, const sh_decl_node *base, const sh_decl_node *source)
{
    const sh_decl_node *item;
    for (item = gc_first(base); item; item = item->next) {
        const sh_decl_node *id = gc_member(item, "id"), *other, *same;
        size_t before = 0, after = 0;
        for (other = gc_first(base); other; other = other->next)
            before += gc_id_group(id->value, gc_member(other, "id")->value);
        for (other = gc_first(source); other; other = other->next)
            after += gc_id_group(id->value, gc_member(other, "id")->value);
        if (before == after || !after) continue;
        same = gc_find(source, id->value);
        if (same && !gc_state_equal(item, same))
            return gc_fail(c, "duplicate multiplicity changed together with retained record state");
    }
    return 1;
}
static int gc_extents(gc_context *c, const sh_decl_node *baseline, const sh_decl_node *source)
{
    const sh_decl_node *b = gc_member(baseline, "edit"), *s = gc_member(source, "edit"), *graph;
    if (!gc_extent(c, gc_member(b, "layersConfigs"), gc_member(s, "layersConfigs")) ||
        !gc_extent(c, gc_member(b, "subGraphs"), gc_member(s, "subGraphs"))) return 0;
    for (graph = gc_first(gc_member(b, "subGraphs")); graph; graph = graph->next) {
        const sh_decl_node *other = gc_find(gc_member(s, "subGraphs"), gc_member(graph, "id")->value);
        if (!other) continue;
        if (!gc_extent(c, gc_member(graph, "nodes"), gc_member(other, "nodes")) ||
            !gc_extent(c, gc_member(graph, "links"), gc_member(other, "links"))) return 0;
    }
    return 1;
}
static sh_decl_node *gc_native_layers(gc_context *c, const sh_decl_node *list, const char *key)
{
    sh_decl_node *out = gc_node(c, key, NULL);
    const sh_decl_node *item;
    for (item = gc_first(list); item; item = item->next) gc_add(out, gc_clone(c, item, "layer"));
    return out;
}
static sh_decl_node *gc_native_record(gc_context *c, const sh_decl_node *record, const char *key, int layers)
{
    sh_decl_node *out = gc_node(c, key, NULL);
    gc_add(out, gc_clone(c, gc_member(record, "object"), "object"));
    if (layers) gc_add(out, gc_native_layers(c, gc_member(record, "layers"), "layers"));
    return out;
}
static int gc_digit(char ch)
{ return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; }
static char *gc_config_key(gc_context *c, const char *id)
{
    size_t i, n = strcspn(id + 1, "_");
    char *out;
    if ((n & 1) || !(out = malloc(n / 2 + 1))) { gc_fail(c, "invalid configuration identity"); return NULL; }
    for (i = 0; i < n; i += 2) {
        int a = gc_digit(id[i + 1]), b = gc_digit(id[i + 2]);
        if (a < 0 || b < 0) { free(out); gc_fail(c, "invalid configuration identity"); return NULL; }
        out[i / 2] = (char)((a << 4) | b);
    }
    out[n / 2] = 0; return out;
}
static sh_decl_node *gc_native(gc_context *c, const sh_decl_node *normalized)
{
    const sh_decl_node *state = gc_member(normalized, "edit"), *item, *sub;
    sh_decl_node *out = gc_node(c, NULL, NULL), *edit, *configs, *subgraphs;
    gc_add(out, gc_clone(c, gc_member(normalized, "inherit"), "inherit"));
    edit = gc_add(out, gc_node(c, "edit", NULL));
    gc_add(edit, gc_clone(c, gc_member(state, "object"), "object"));
    gc_add(edit, gc_native_layers(c, gc_member(state, "layers"), "layers"));
    configs = gc_add(edit, gc_node(c, "layersConfigs", NULL));
    for (item = gc_first(gc_member(state, "layersConfigs")); item; item = item->next) {
        char *key = gc_config_key(c, gc_member(item, "id")->value);
        if (key) gc_add(configs, gc_native_layers(c, gc_member(item, "layers"), key));
        free(key);
    }
    subgraphs = gc_add(edit, gc_node(c, "subGraphs", NULL));
    for (sub = gc_first(gc_member(state, "subGraphs")); sub; sub = sub->next) {
        sh_decl_node *graph = gc_add(subgraphs, gc_native_record(c, sub, "subGraph", 1));
        sh_decl_node *nodes = gc_add(graph, gc_node(c, "nodes", NULL)), *links;
        for (item = gc_first(gc_member(sub, "nodes")); item; item = item->next)
            gc_add(nodes, gc_native_record(c, item, "node", 1));
        links = gc_add(graph, gc_node(c, "links", NULL));
        for (item = gc_first(gc_member(sub, "links")); item; item = item->next) {
            const sh_decl_node *start = gc_member(item, "startNode");
            const char *name;
            size_t name_length;
            char *group_key;
            if (!sh_decl_tree_literal(start, &name, &name_length) ||
                !(group_key = malloc(name_length + 1))) { gc_fail(c, "link group allocation failed"); break; }
            memcpy(group_key, name, name_length); group_key[name_length] = 0;
            sh_decl_node *group = gc_add(links, gc_node(c, group_key, NULL));
            free(group_key);
            sh_decl_node *link = gc_add(group, gc_native_record(c, item, "link", 0));
            gc_add(link, gc_clone(c, start, "startNode"));
            gc_add(link, gc_clone(c, gc_member(item, "endNode"), "endNode"));
        }
    }
    if (c->failed) { sh_decl_tree_free(out); return NULL; }
    return out;
}

/* IDs deliberately do not get regenerated from output ordinals: removal or
 * reordering changes those ordinals. Pair normalized records with native order
 * and verify links using the identities retained by composition. */
static int gc_endpoints(gc_context *c, const sh_decl_node *normalized, const sh_decl_graph *native)
{
    size_t n = sh_decl_graph_count(native), index = 0, i;
    const sh_decl_node *sub, *item, *graphs = gc_member(gc_member(normalized, "edit"), "subGraphs");
    char **targets = NULL;
    int ok = 0;
    if (n > SIZE_MAX / sizeof(*targets) || !(targets = calloc(n ? n : 1, sizeof(*targets))))
        { gc_fail(c, "endpoint verification allocation failed"); goto done; }
    for (sub = gc_first(graphs); sub; sub = sub->next) {
        const char *sid = gc_member(sub, "id")->value;
        if (index >= n) goto mismatch;
        index++;
        for (item = gc_first(gc_member(sub, "nodes")); item; item = item->next) {
            const char *id = gc_member(item, "id")->value;
            size_t a = strlen(sid), b = strlen(id);
            if (index >= n || b > SIZE_MAX - a) goto mismatch;
            targets[index] = malloc(a + b);
            if (!targets[index]) { gc_fail(c, "endpoint verification allocation failed"); goto done; }
            memcpy(targets[index], sid, a - 1); targets[index][a - 1] = '/';
            memcpy(targets[index] + a, id + 1, b);
            index++;
        }
    }
    for (sub = gc_first(graphs); sub; sub = sub->next)
        for (item = gc_first(gc_member(sub, "links")); item; item = item->next) {
            const sh_decl_graph_record *r = sh_decl_graph_at(native, index);
            const sh_decl_node *start = gc_member(item, "startTarget"), *end = gc_member(item, "endTarget");
            if (!r || r->kind != SH_DECL_GRAPH_LINK || r->start >= n || r->end >= n ||
                !targets[r->start] || !targets[r->end] || !start || !end ||
                strcmp(start->value, targets[r->start]) || strcmp(end->value, targets[r->end]))
                goto mismatch;
            index++;
        }
    if (index != n) goto mismatch;
    {
        const sh_decl_node *bindings = gc_member(gc_member(normalized, "edit"), "nodeBindings"), *binding;
        size_t unique = 0, expected = 0;
        for (i = 0; i < n; i++) {
            const sh_decl_graph_record *r = sh_decl_graph_at(native, i);
            char *key;
            size_t prior;
            if (r->kind != SH_DECL_GRAPH_NODE) continue;
            for (prior = 0; prior < i; prior++) {
                const sh_decl_graph_record *p = sh_decl_graph_at(native, prior);
                if (p->kind == SH_DECL_GRAPH_NODE && gc_same(p->name, r->name)) break;
            }
            if (prior != i) continue;
            unique++; key = gc_binding_key(c, r->name);
            if (!key) goto done;
            binding = gc_member(bindings, key); free(key);
            if (!binding || !targets[i] || strcmp(binding->value, targets[i])) {
                gc_fail(c, "composed ordering changes an authored node lookup"); goto done;
            }
        }
        for (binding = bindings ? bindings->children : NULL; binding; binding = binding->next) expected++;
        if (expected != unique) { gc_fail(c, "composed node lookup set differs from authored targets"); goto done; }
    }
    ok = 1; goto done;
mismatch:
    gc_fail(c, "composed ordering changes an authored endpoint occurrence");
done:
    if (targets) for (i = 0; i < n; i++) free(targets[i]);
    free(targets); return ok;
}

char *sh_decl_graph_compose(sh_decl_source baseline, const sh_decl_source *sources, size_t count,
    const sh_decl_dependency_schema *types, sh_decl_value_type root_type,
    size_t *length, char *error, size_t error_capacity, sh_decl_conflict *conflict)
{
    static const sh_decl_collection_rule rules[] = {
        {"edit.layers", NULL}, {"edit.layersConfigs", "id"}, {"edit.layersConfigs.item[*].layers", NULL},
        {"edit.subGraphs", "id"}, {"edit.subGraphs.item[*].layers", NULL},
        {"edit.subGraphs.item[*].nodes", "id"}, {"edit.subGraphs.item[*].nodes.item[*].layers", NULL},
        {"edit.subGraphs.item[*].links", "id"}
    };
    gc_context c = {types, root_type, error, error_capacity, 0};
    gc_input *inputs = NULL;
    sh_decl_source *views = NULL;
    sh_decl_node **trees = NULL, *merged = NULL, *native_tree = NULL;
    sh_decl_graph *native = NULL;
    sh_decl_dependency_schema adapter = {&c, gc_describe, gc_field, NULL, NULL, gc_dynamic};
    sh_decl_composition_schema schema = {0};
    char *combined = NULL, *out = NULL;
    size_t i, j, combined_length, out_length = 0;
    if (length) *length = 0;
    if (error && error_capacity) error[0] = 0;
    if (conflict) conflict->first = conflict->second = SIZE_MAX;
    if (!types || !types->describe || !types->field || !types->dynamic_type || !root_type.name ||
        !*root_type.name || !count || !sources || count == SIZE_MAX ||
        count + 1 > SIZE_MAX / sizeof(*inputs) || count + 1 > SIZE_MAX / sizeof(*views)) {
        gc_fail(&c, "invalid native graph schema or inputs"); return NULL;
    }
    inputs = calloc(count + 1, sizeof(*inputs)); views = calloc(count + 1, sizeof(*views));
    trees = calloc(count + 1, sizeof(*trees));
    if (!inputs || !views || !trees) { gc_fail(&c, "allocation failed"); goto done; }
    for (i = 0; i <= count; i++) {
        inputs[i].graph = sh_decl_graph_open(i ? sources[i - 1] : baseline, error, error_capacity);
        if (!inputs[i].graph || !gc_ids(&c, &inputs[i]) || !(trees[i] = gc_normalize(&c, &inputs[i]))) goto input_failed;
        if (i && !gc_extents(&c, trees[0], trees[i])) goto input_failed;
        inputs[i].text = sh_decl_tree_write(trees[i], &inputs[i].length);
        if (!inputs[i].text) { gc_fail(&c, "normalized state allocation failed"); goto input_failed; }
        views[i] = (sh_decl_source){inputs[i].text, inputs[i].length};
        continue;
input_failed:
        if (conflict && i) conflict->first = i - 1;
        goto done;
    }
    schema.types = &adapter; schema.state_type = (sh_decl_value_type){GC_TYPE("envelope"), ""};
    combined = sh_decl_compose_typed(views[0], views + 1, count, rules, sizeof(rules) / sizeof(rules[0]),
        &schema, &combined_length, error, error_capacity, conflict);
    if (!combined) goto done;
    merged = sh_decl_tree_parse((sh_decl_source){combined, combined_length}, error, error_capacity);
    if (!merged || !(native_tree = gc_native(&c, merged))) goto done;
    out = sh_decl_tree_write(native_tree, &out_length);
    if (!out) { gc_fail(&c, "output allocation failed"); goto done; }
    native = sh_decl_graph_open((sh_decl_source){out, out_length}, error, error_capacity);
    if (!native || !gc_endpoints(&c, merged, native)) { free(out); out = NULL; }
done:
    if (inputs) for (i = 0; i <= count; i++) {
        if (inputs[i].ids) for (j = 0; j < sh_decl_graph_count(inputs[i].graph); j++) free(inputs[i].ids[j]);
        free(inputs[i].ids); free(inputs[i].text); sh_decl_graph_close(inputs[i].graph);
    }
    if (trees) for (i = 0; i <= count; i++) sh_decl_tree_free(trees[i]);
    free(inputs); free(views); free(trees); free(combined);
    sh_decl_tree_free(merged); sh_decl_tree_free(native_tree); sh_decl_graph_close(native);
    if (out && length) *length = out_length;
    return out;
}
