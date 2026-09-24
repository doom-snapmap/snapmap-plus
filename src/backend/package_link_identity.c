#include "package_link_graph.h"
#include "package_sources.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct li_node {
    sh_package_link_node source;
    sh_package_link_edge *edges;
    size_t component, color;
} li_node;
typedef struct li_buffer { unsigned char *body; size_t length, capacity; int failed; } li_buffer;
typedef struct li_frame { size_t node, edge; } li_frame;
typedef struct li_key { size_t node, color; li_buffer key; } li_key;
typedef struct li_work {
    li_node *nodes;
    size_t count, component_count;
    size_t *offsets, *incoming, *stack, *order, *components;
    size_t *starts, *members;
    li_frame *frames;
    unsigned char *seen;
    unsigned char (*identities)[32];
} li_work;

static void *li_alloc(size_t count, size_t stride)
{ return count <= SIZE_MAX / stride ? calloc(count ? count : 1, stride) : NULL; }

static int li_fail(char *error, size_t capacity, const char *reason)
{ if (error && capacity) snprintf(error, capacity, "%s", reason); return 0; }

static void li_put(li_buffer *out, const void *bytes, size_t length)
{
    size_t needed, next;
    unsigned char *grown;
    if (out->failed) return;
    if (length > SIZE_MAX - out->length) { out->failed = 1; return; }
    needed = out->length + length;
    if (needed > out->capacity) {
        next = out->capacity ? out->capacity : 256;
        while (next < needed) { if (next > SIZE_MAX / 2) { next = needed; break; } next *= 2; }
        grown = realloc(out->body, next);
        if (!grown) { out->failed = 1; return; }
        out->body = grown; out->capacity = next;
    }
    if (length) memcpy(out->body + out->length, bytes, length);
    out->length += length;
}

static void li_number(li_buffer *out, uint64_t value)
{
    unsigned char bytes[8];
    for (size_t i = 0; i < 8; i++) bytes[7 - i] = (unsigned char)(value >> (i * 8));
    li_put(out, bytes, 8);
}

static void li_string(li_buffer *out, const char *value)
{ size_t length = strlen(value); li_number(out, length); li_put(out, value, length); }

static void li_shape(li_buffer *out, const sh_package_link_node *node)
{
    li_string(out, node->family); li_string(out, node->path); li_number(out, node->length);
    li_put(out, node->content, 32); li_put(out, node->semantics, 32); li_number(out, node->edge_count);
}

static int li_edge_order(const void *left, const void *right)
{ return strcmp(((const sh_package_link_edge *)left)->slot, ((const sh_package_link_edge *)right)->slot); }

static int li_key_order(const void *left, const void *right)
{
    const li_key *a = left, *b = right;
    size_t common = a->key.length < b->key.length ? a->key.length : b->key.length;
    int cmp = common ? memcmp(a->key.body, b->key.body, common) : 0;
    return cmp ? cmp : a->key.length < b->key.length ? -1 : a->key.length > b->key.length;
}

static int li_prepare(li_work *work, const sh_package_link_node *nodes, const sh_package_link_classes *classes)
{
    size_t count = classes->count, total = 0, *cursor = NULL;
    work->count = count;
    work->nodes = li_alloc(count, sizeof(*work->nodes));
    work->offsets = li_alloc(count + 1, sizeof(*work->offsets));
    work->stack = li_alloc(count, sizeof(*work->stack));
    work->order = li_alloc(count, sizeof(*work->order));
    work->components = li_alloc(count, sizeof(*work->components));
    work->frames = li_alloc(count, sizeof(*work->frames));
    work->seen = li_alloc(count, sizeof(*work->seen));
    work->starts = li_alloc(count + 1, sizeof(*work->starts));
    work->members = li_alloc(count, sizeof(*work->members));
    work->identities = li_alloc(count, sizeof(*work->identities));
    cursor = li_alloc(count, sizeof(*cursor));
    if (!work->nodes || !work->offsets || !work->stack || !work->order || !work->components ||
        !work->frames || !work->seen || !work->starts || !work->members || !work->identities || !cursor) goto failed;
    for (size_t i = 0; i < count; i++) {
        li_node *node = &work->nodes[i];
        node->source = nodes[classes->representatives[i]];
        if (node->source.edge_count > SIZE_MAX - total) goto failed;
        total += node->source.edge_count;
        node->edges = li_alloc(node->source.edge_count, sizeof(*node->edges));
        if (!node->edges) goto failed;
        for (size_t j = 0; j < node->source.edge_count; j++) {
            node->edges[j] = node->source.edges[j];
            node->edges[j].target = classes->nodes[node->edges[j].target];
            work->offsets[node->edges[j].target + 1]++;
        }
        if (node->source.edge_count > 1) qsort(node->edges, node->source.edge_count, sizeof(*node->edges), li_edge_order);
    }
    work->incoming = li_alloc(total, sizeof(*work->incoming));
    if (!work->incoming) goto failed;
    for (size_t i = 0; i < count; i++) { work->offsets[i + 1] += work->offsets[i]; cursor[i] = work->offsets[i]; }
    for (size_t i = 0; i < count; i++) for (size_t j = 0; j < work->nodes[i].source.edge_count; j++)
        work->incoming[cursor[work->nodes[i].edges[j].target]++] = i;
    free(cursor); return 1;
failed:
    free(cursor); return 0;
}

/* Iterative Kosaraju: native inheritance/behavior chains do not consume the C
 * stack. Component IDs are in source-before-dependency order. */
static void li_components(li_work *work)
{
    size_t used = 0;
    for (size_t i = 0; i < work->count; i++) if (!work->seen[i]) {
        size_t depth = 1;
        work->frames[0] = (li_frame){i, 0}; work->seen[i] = 1;
        while (depth) {
            li_frame *frame = &work->frames[depth - 1];
            li_node *node = &work->nodes[frame->node];
            if (frame->edge < node->source.edge_count) {
                size_t target = node->edges[frame->edge++].target;
                if (!work->seen[target]) { work->seen[target] = 1; work->frames[depth++] = (li_frame){target, 0}; }
            } else { work->order[used++] = frame->node; depth--; }
        }
    }
    memset(work->seen, 0, work->count);
    for (size_t i = used; i; i--) {
        size_t root = work->order[i - 1], depth = 0, component;
        if (work->seen[root]) continue;
        component = work->component_count++;
        work->stack[depth++] = root; work->seen[root] = 1;
        while (depth) {
            size_t node = work->stack[--depth];
            work->nodes[node].component = component; work->starts[component + 1]++;
            for (size_t j = work->offsets[node]; j < work->offsets[node + 1]; j++) {
                size_t from = work->incoming[j];
                if (!work->seen[from]) { work->seen[from] = 1; work->stack[depth++] = from; }
            }
        }
    }
    for (size_t i = 0; i < work->component_count; i++) {
        work->starts[i + 1] += work->starts[i]; work->components[i] = work->starts[i];
    }
    for (size_t i = 0; i < work->count; i++) work->members[work->components[work->nodes[i].component]++] = i;
}

static void li_signature(li_work *work, size_t index, li_buffer *out, int refinement)
{
    li_node *node = &work->nodes[index];
    if (refinement) li_number(out, node->color);
    li_shape(out, &node->source);
    for (size_t i = 0; i < node->source.edge_count; i++) {
        const sh_package_link_edge *edge = &node->edges[i];
        li_node *target = &work->nodes[edge->target];
        int internal = target->component == node->component;
        li_string(out, edge->slot); li_number(out, internal);
        if (internal) li_number(out, target->color);
        else li_put(out, work->identities[edge->target], 32);
    }
}

static int li_hash(const li_buffer *buffer, unsigned char out[32])
{
    sh_package_file_identity identity;
    if (buffer->failed || !sh_package_bytes_identity(buffer->body, buffer->length, &identity)) return 0;
    memcpy(out, identity.digest, 32); return 1;
}

static int li_component(li_work *work, size_t component)
{
    size_t start = work->starts[component], count = work->starts[component + 1] - start;
    li_key *keys = li_alloc(count, sizeof(*keys));
    li_buffer buffer = {0};
    unsigned char digest[32];
    int ok = 0;
    if (!keys) return 0;
    for (size_t i = 0; i < count; i++) keys[i].node = work->members[start + i];
    if (count > 1) {
        size_t previous = 0;
        for (;;) {
            size_t colors = 1;
            for (size_t i = 0; i < count; i++) {
                keys[i].key.length = 0;
                li_signature(work, keys[i].node, &keys[i].key, 1);
                if (keys[i].key.failed) goto done;
            }
            qsort(keys, count, sizeof(*keys), li_key_order);
            keys[0].color = 0;
            for (size_t i = 1; i < count; i++) {
                if (li_key_order(&keys[i - 1], &keys[i])) colors++;
                keys[i].color = colors - 1;
            }
            for (size_t i = 0; i < count; i++) work->nodes[keys[i].node].color = keys[i].color;
            if (colors == count) break;
            /* Equivalent nodes were already collapsed. A stable non-discrete
             * partition here indicates an inconsistent input/hash contract. */
            if (colors == previous) goto done;
            previous = colors;
        }
    }
    li_string(&buffer, "snapmap-plus/resource-bindings"); li_number(&buffer, count);
    for (size_t i = 0; i < count; i++) li_signature(work, keys[i].node, &buffer, 0);
    if (!li_hash(&buffer, digest)) goto done;
    for (size_t i = 0; i < count; i++) {
        buffer.length = 0;
        li_string(&buffer, "snapmap-plus/resource-member"); li_put(&buffer, digest, 32); li_number(&buffer, i);
        if (!li_hash(&buffer, work->identities[keys[i].node])) goto done;
    }
    ok = 1;
done:
    for (size_t i = 0; i < count; i++) free(keys[i].key.body);
    free(keys); free(buffer.body); return ok;
}

static void li_free(li_work *work)
{
    if (work->nodes) for (size_t i = 0; i < work->count; i++) free(work->nodes[i].edges);
    free(work->nodes); free(work->offsets); free(work->incoming); free(work->stack);
    free(work->order); free(work->components); free(work->frames); free(work->seen);
    free(work->starts); free(work->members); free(work->identities);
}

int sh_package_link_identify_verified(const sh_package_link_node *nodes, size_t count,
    sh_package_link_verify_equal verify, void *context,
    sh_package_link_classes *out, char *error, size_t capacity)
{
    li_work work = {0};
    int ok = 0;
    if (!sh_package_link_equivalence_verified(nodes, count, verify, context, out, error, capacity)) return 0;
    if (!li_prepare(&work, nodes, out)) goto done;
    li_components(&work);
    for (size_t i = work.component_count; i; i--) if (!li_component(&work, i - 1)) goto done;
    out->identities = work.identities; work.identities = NULL; ok = 1;
done:
    li_free(&work);
    if (!ok) { sh_package_link_classes_free(out); li_fail(error, capacity, "resource binding identity could not be constructed"); }
    return ok;
}

int sh_package_link_identify(const sh_package_link_node *nodes, size_t count,
    sh_package_link_classes *out, char *error, size_t capacity)
{
    return sh_package_link_identify_verified(nodes, count, NULL, NULL, out, error, capacity);
}
