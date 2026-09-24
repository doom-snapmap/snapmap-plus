#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/package_link_graph.h"

static sh_package_link_node node(const char *family, const char *path, unsigned char content,
    const sh_package_link_edge *edges, size_t count)
{
    sh_package_link_node out = {0};
    out.family = family; out.path = path; out.length = 16;
    out.content[0] = content; out.edges = edges; out.edge_count = count;
    return out;
}

static void variants(void)
{
    sh_package_link_edge edges[] = {{"model", 1}, {"material", 2}, {"model", 4}, {"material", 5},
        {"model", 7}, {"material", 8}};
    sh_package_link_node graph[9];
    sh_package_link_classes classes = {0};
    char error[256];
    size_t i;
    for (i = 0; i < 3; i++) {
        graph[i * 3] = node("entityDef", "props/stone", 1, &edges[i * 2], 1);
        graph[i * 3 + 1] = node("model", "models/stone", 2, &edges[i * 2 + 1], 1);
        graph[i * 3 + 2] = node("material", "materials/stone", i == 1 ? 4 : 3, NULL, 0);
    }
    assert(sh_package_link_equivalence(graph, 9, &classes, error, sizeof(error)));
    assert(classes.count == 6);
    for (i = 0; i < 3; i++) {
        assert(classes.nodes[i] == classes.nodes[6 + i]);
        assert(classes.nodes[i] != classes.nodes[3 + i]);
    }
    /* A different presentation binding also creates a distinct exposed asset. */
    graph[6].semantics[0] = 1;
    assert(sh_package_link_equivalence(graph, 9, &classes, error, sizeof(error)));
    assert(classes.count == 7 && classes.nodes[0] != classes.nodes[6]);
    assert(classes.nodes[1] == classes.nodes[7]);
    sh_package_link_classes_free(&classes);
}

static void cycles(void)
{
    sh_package_link_edge edges[] = {{"next", 1}, {"next", 0}, {"next", 3}, {"next", 2},
        {"next", 5}, {"next", 4}};
    sh_package_link_node graph[6];
    sh_package_link_classes classes = {0};
    char error[256];
    size_t i;
    for (i = 0; i < 6; i++) graph[i] = node("behavior", i % 2 ? "states/run" : "states/wait", 1, &edges[i], 1);
    assert(sh_package_link_equivalence(graph, 6, &classes, error, sizeof(error)) && classes.count == 2);
    graph[5].content[0] = 2;
    assert(sh_package_link_equivalence(graph, 6, &classes, error, sizeof(error)) && classes.count == 4);
    assert(classes.nodes[0] == classes.nodes[2] && classes.nodes[1] == classes.nodes[3]);
    assert(classes.nodes[0] != classes.nodes[4] && classes.nodes[1] != classes.nodes[5]);
    /* A self-referential identical binding does not recurse indefinitely. */
    graph[1].path = graph[0].path; edges[0].target = 0; edges[1].target = 1;
    assert(sh_package_link_equivalence(graph, 2, &classes, error, sizeof(error)) && classes.count == 1);
    sh_package_link_classes_free(&classes);
}

static void edge_order_and_failures(void)
{
    sh_package_link_edge a[] = {{"primary", 2}, {"secondary", 3}};
    sh_package_link_edge b[] = {{"secondary", 3}, {"primary", 2}};
    sh_package_link_node graph[] = {
        node("weapon", "tools/test", 1, a, 2), node("weapon", "tools/test", 1, b, 2),
        node("effect", "effects/primary", 2, NULL, 0), node("effect", "effects/secondary", 3, NULL, 0)
    };
    sh_package_link_classes classes = {0};
    char error[256];
    assert(sh_package_link_equivalence(graph, 4, &classes, error, sizeof(error)));
    assert(classes.nodes[0] == classes.nodes[1] && classes.count == 3);
    assert(!strcmp(b[0].slot, "secondary") && b[0].target == 3);
    b[0].target = 2; b[1].target = 3;
    assert(sh_package_link_equivalence(graph, 4, &classes, error, sizeof(error)) && classes.count == 4);
    b[0].slot = "primary";
    assert(!sh_package_link_equivalence(graph, 4, &classes, error, sizeof(error)) && strstr(error, "slot"));
    assert(!classes.nodes && !classes.representatives && !classes.count);
    b[0].slot = "secondary"; b[0].target = 4;
    assert(!sh_package_link_equivalence(graph, 4, &classes, error, sizeof(error)) && strstr(error, "binding"));
    b[0].target = 2; graph[0].family = NULL;
    assert(!sh_package_link_equivalence(graph, 4, &classes, error, sizeof(error)) && strstr(error, "identities"));
    assert(sh_package_link_equivalence(NULL, 0, &classes, error, sizeof(error)) && !classes.count);
    sh_package_link_classes_free(&classes);
}

/* An intentionally slow pair-relation oracle checks the optimized partition
 * independently, including cyclic graphs and mixed incoming dependencies. */
static unsigned random_state = 1729;
static unsigned next_random(void) { random_state = random_state * 1664525u + 1013904223u; return random_state; }

static int shape_equal(const sh_package_link_node *a, const sh_package_link_node *b)
{
    return !strcmp(a->family, b->family) && !strcmp(a->path, b->path) && a->length == b->length &&
        !memcmp(a->content, b->content, 32) && !memcmp(a->semantics, b->semantics, 32) && a->edge_count == b->edge_count;
}

static void randomized(void)
{
    enum { N = 24 };
    unsigned run;
    for (run = 0; run < 160; run++) {
        sh_package_link_edge edges[N][2];
        sh_package_link_node graph[N];
        sh_package_link_classes classes = {0};
        unsigned char equal[N][N], previous[N][N];
        char error[256];
        size_t i, j, k;
        int changed;
        for (i = 0; i < N / 2; i++) {
            size_t count = next_random() % 3;
            edges[i][0] = (sh_package_link_edge){"a", next_random() % (N / 2)};
            edges[i][1] = (sh_package_link_edge){"b", next_random() % (N / 2)};
            graph[i] = node("graph", next_random() % 2 ? "states/a" : "states/b", (unsigned char)(next_random() % 2), edges[i], count);
            graph[i + N / 2] = graph[i]; graph[i + N / 2].edges = edges[i + N / 2];
            for (j = 0; j < 2; j++) {
                edges[i + N / 2][j] = edges[i][j]; edges[i + N / 2][j].target += N / 2;
            }
        }
        graph[next_random() % N].content[0] += (unsigned char)(run % 3);
        for (i = 0; i < N; i++) for (j = 0; j < N; j++) equal[i][j] = (unsigned char)shape_equal(&graph[i], &graph[j]);
        do {
            changed = 0; memcpy(previous, equal, sizeof(equal));
            for (i = 0; i < N; i++) for (j = 0; j < N; j++) if (previous[i][j]) {
                for (k = 0; k < graph[i].edge_count; k++)
                    if (!previous[edges[i][k].target][edges[j][k].target]) { equal[i][j] = 0; changed = 1; break; }
            }
        } while (changed);
        assert(sh_package_link_equivalence(graph, N, &classes, error, sizeof(error)));
        for (i = 0; i < N; i++) for (j = 0; j < N; j++)
            assert((classes.nodes[i] == classes.nodes[j]) == equal[i][j]);
        unsigned char local[N], expected[N], original[N];
        for (i = 0; i < N; i++) local[i] = expected[i] = next_random() % 5 != 0;
        do {
            changed = 0;
            for (i = 0; i < N; i++) if (expected[i]) for (j = 0; j < graph[i].edge_count; j++)
                if (!expected[graph[i].edges[j].target]) { expected[i] = 0; changed = 1; break; }
        } while (changed);
        assert(sh_package_link_originals(graph, N, local, original, error, sizeof(error)));
        assert(!memcmp(expected, original, N));
        sh_package_link_classes_free(&classes);
    }
}

static void deep_dependency_change(void)
{
    enum { DEPTH = 10000, N = DEPTH * 2 };
    sh_package_link_node *graph = calloc(N, sizeof(*graph));
    sh_package_link_edge *edges = calloc(N, sizeof(*edges));
    char (*paths)[32] = calloc(DEPTH, sizeof(*paths));
    sh_package_link_classes classes = {0};
    char error[256];
    size_t i;
    assert(graph && edges && paths);
    for (i = 0; i < DEPTH; i++) {
        snprintf(paths[i], sizeof(paths[i]), "state/%zu", i);
        edges[i] = (sh_package_link_edge){"next", i + 1};
        edges[i + DEPTH] = (sh_package_link_edge){"next", i + DEPTH + 1};
        graph[i] = node("graph", paths[i], 1, &edges[i], i + 1 < DEPTH);
        graph[i + DEPTH] = node("graph", paths[i], 1, &edges[i + DEPTH], i + 1 < DEPTH);
    }
    graph[N - 1].content[0] = 2;
    assert(sh_package_link_equivalence(graph, N, &classes, error, sizeof(error)));
    assert(classes.count == N && classes.examined < N * 3);
    unsigned char *originals = malloc(N); assert(originals);
    memset(originals, 1, N); originals[N - 1] = 0;
    assert(sh_package_link_originals(graph, N, originals, originals, error, sizeof(error)));
    for (i = 0; i < N; i++) assert(originals[i] == (i < DEPTH));
    free(originals);
    sh_package_link_classes_free(&classes); free(paths); free(edges); free(graph);
}

static void original_cycles(void)
{
    sh_package_link_edge edges[] = {{"next", 1}, {"next", 0}, {"child", 1}};
    sh_package_link_node graph[] = {
        node("graph", "a", 1, edges, 1), node("graph", "b", 1, edges + 1, 1),
        node("graph", "parent", 1, edges + 2, 1), node("graph", "independent", 1, NULL, 0)
    };
    unsigned char local[] = {1, 1, 1, 1}, out[4];
    char error[256];
    assert(sh_package_link_originals(graph, 4, local, out, error, sizeof(error)));
    assert(!memcmp(local, out, 4));
    local[1] = 0;
    assert(sh_package_link_originals(graph, 4, local, out, error, sizeof(error)));
    assert(!out[0] && !out[1] && !out[2] && out[3] && local[0]);
    edges[0].target = 4;
    assert(!sh_package_link_originals(graph, 4, local, out, error, sizeof(error)) && *error);
    assert(!out[0] && !out[1] && !out[2] && !out[3]);
    assert(sh_package_link_originals(NULL, 0, NULL, NULL, error, sizeof(error)));
}

typedef struct exact_inputs {
    const char *bytes[3];
    size_t calls;
    int unavailable;
} exact_inputs;

static int exact_equal(void *context, size_t left, size_t right, char *error, size_t capacity)
{
    exact_inputs *inputs = context;
    inputs->calls++;
    if (inputs->unavailable) {
        snprintf(error, capacity, "retained source is unavailable"); return 0;
    }
    return !strcmp(inputs->bytes[left], inputs->bytes[right]);
}

static void positive_fingerprint_verification(void)
{
    sh_package_link_node graph[3] = {
        node("image", "shared", 1, NULL, 0), node("image", "shared", 1, NULL, 0),
        node("image", "shared", 2, NULL, 0)
    };
    sh_package_link_classes out = {0};
    exact_inputs inputs = {{"first", "first", "other"}, 0, 0};
    char error[256];
    for (size_t i = 0; i < 3; i++) graph[i].length = 5;
    assert(sh_package_link_equivalence_verified(graph, 3, exact_equal, &inputs, &out, error, sizeof(error)));
    assert(inputs.calls == 1 && out.count == 2 && out.nodes[0] == out.nodes[1]);
    /* An injected digest match is only a candidate. Same length and identical
     * graph metadata cannot make unequal payload bytes safe to collapse. */
    inputs.bytes[1] = "other";
    assert(!sh_package_link_equivalence_verified(graph, 3, exact_equal, &inputs, &out, error, sizeof(error)));
    assert(strstr(error, "exact verification") && !out.count && !out.nodes);
    inputs.bytes[0] = "other"; inputs.bytes[1] = "first";
    assert(!sh_package_link_equivalence_verified(graph, 3, exact_equal, &inputs, &out, error, sizeof(error)));
    inputs.bytes[0] = "first"; inputs.unavailable = 1;
    assert(!sh_package_link_equivalence_verified(graph, 3, exact_equal, &inputs, &out, error, sizeof(error)));
    assert(strstr(error, "unavailable") && !out.count);
    sh_package_link_classes_free(&out);
}

int main(void)
{
    variants(); cycles(); edge_order_and_failures(); randomized(); deep_dependency_change(); original_cycles();
    positive_fingerprint_verification();
    puts("package_link_graph_test: PASS");
    return 0;
}
