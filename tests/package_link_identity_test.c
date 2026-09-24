#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/package_link_graph.h"

static sh_package_link_node node(unsigned content, const sh_package_link_edge *edges, size_t count)
{
    sh_package_link_node out = {0};
    out.family = "behavior"; out.path = "states/test"; out.length = 12;
    out.content[0] = (unsigned char)content; out.edges = edges; out.edge_count = count;
    return out;
}

static const unsigned char *identity(const sh_package_link_classes *out, size_t node_index)
{ return out->identities[out->nodes[node_index]]; }

static void stable_contexts(void)
{
    enum { N = 11 };
    sh_package_link_edge edges[N][2] = {{{"next", 1}}, {{"next", 2}}, {{"next", 0}, {"leaf", 3}},
        {{0}}, {{"next", 5}}, {{"next", 6}}, {{"leaf", 7}, {"next", 4}}, {{0}},
        {{"next", 8}}, {{"next", 3}}, {{"next", 0}}};
    sh_package_link_node graph[N], shuffled[N];
    sh_package_link_edge shuffled_edges[N][2];
    sh_package_link_classes original = {0}, reordered = {0}, smaller = {0};
    size_t permutation[N] = {8, 7, 4, 2, 10, 0, 1, 6, 3, 9, 5}, inverse[N];
    char error[256];
    for (size_t i = 0; i < N; i++) graph[i] = node(1, edges[i], 1);
    graph[2].edge_count = graph[6].edge_count = 2;
    graph[3] = graph[7] = node(2, NULL, 0);
    graph[9].content[0] = 3; graph[10].content[0] = 4;
    assert(sh_package_link_identify(graph, N, &original, error, sizeof(error)));
    for (size_t i = 0; i < 4; i++) assert(!memcmp(identity(&original, i), identity(&original, i + 4), 32));
    assert(memcmp(identity(&original, 0), identity(&original, 1), 32));
    assert(memcmp(identity(&original, 0), identity(&original, 8), 32));
    for (size_t i = 0; i < N; i++) inverse[permutation[i]] = i;
    for (size_t i = 0; i < N; i++) {
        shuffled[i] = graph[permutation[i]]; shuffled[i].edges = shuffled_edges[i];
        for (size_t j = 0; j < shuffled[i].edge_count; j++) {
            shuffled_edges[i][j] = graph[permutation[i]].edges[j];
            shuffled_edges[i][j].target = inverse[shuffled_edges[i][j].target];
        }
    }
    assert(sh_package_link_identify(shuffled, N, &reordered, error, sizeof(error)));
    for (size_t i = 0; i < N; i++) assert(!memcmp(identity(&original, permutation[i]), identity(&reordered, i), 32));
    /* Neither duplicate bundles nor unrelated inbound references rename assets. */
    assert(sh_package_link_identify(graph, 4, &smaller, error, sizeof(error)));
    for (size_t i = 0; i < 4; i++) assert(!memcmp(identity(&original, i), identity(&smaller, i), 32));
    graph[3].semantics[31] = 7;
    assert(sh_package_link_identify(graph, 4, &smaller, error, sizeof(error)));
    for (size_t i = 0; i < 4; i++) assert(memcmp(identity(&original, i), identity(&smaller, i), 32));
    sh_package_link_classes_free(&original); sh_package_link_classes_free(&reordered); sh_package_link_classes_free(&smaller);
}

static void long_chain(void)
{
    const size_t count = 12000;
    sh_package_link_node *graph = calloc(count, sizeof(*graph));
    sh_package_link_edge *edges = calloc(count, sizeof(*edges));
    sh_package_link_classes out = {0};
    unsigned char leaf[32], parent[32];
    char error[256];
    assert(graph && edges);
    for (size_t i = 0; i < count; i++) { edges[i] = (sh_package_link_edge){"next", i + 1}; graph[i] = node(1, &edges[i], 1); }
    graph[count - 1] = node(2, NULL, 0);
    assert(sh_package_link_identify(graph, count, &out, error, sizeof(error)) && out.count == count);
    memcpy(leaf, identity(&out, count - 1), 32); memcpy(parent, identity(&out, count - 2), 32);
    assert(sh_package_link_identify(&graph[count - 1], 1, &out, error, sizeof(error)));
    assert(!memcmp(leaf, identity(&out, 0), 32));
    edges[count - 2].target = 1;
    assert(sh_package_link_identify(&graph[count - 2], 2, &out, error, sizeof(error)));
    assert(!memcmp(parent, identity(&out, 0), 32));
    assert(sh_package_link_identify(NULL, 0, &out, error, sizeof(error)) && out.count == 0);
    graph[0].family = NULL;
    assert(!sh_package_link_identify(graph, 1, &out, error, sizeof(error)) && !out.identities && !out.count);
    sh_package_link_classes_free(&out); free(graph); free(edges);
}

static int same_bytes(void *context, size_t left, size_t right, char *error, size_t capacity)
{
    const char *const *bytes = context;
    (void)error; (void)capacity;
    return !strcmp(bytes[left], bytes[right]);
}

static void verified_identity(void)
{
    sh_package_link_node graph[2] = {node(1, NULL, 0), node(1, NULL, 0)};
    const char *bytes[2] = {"same", "same"};
    sh_package_link_classes out = {0};
    char error[256];
    assert(sh_package_link_identify_verified(graph, 2, same_bytes, bytes, &out, error, sizeof(error)));
    assert(out.identities && out.count == 1);
    bytes[1] = "fake";
    assert(!sh_package_link_identify_verified(graph, 2, same_bytes, bytes, &out, error, sizeof(error)));
    assert(strstr(error, "exact verification") && !out.identities && !out.nodes && !out.count);
}

int main(void)
{
    stable_contexts(); long_chain(); verified_identity();
    puts("package link identity tests passed"); return 0;
}
