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

/* A remembered shape order gives the same partition as a full
 * sort: reused while it still sorts the nodes, replaced when a change breaks
 * it, ignored when it is not a permutation of these nodes. */
static void same_identities(const sh_package_link_classes *a, const sh_package_link_classes *b, size_t count)
{
    assert(a->count == b->count && a->node_count == count && b->node_count == count);
    for (size_t i = 0; i < count; i++) {
        for (size_t j = 0; j < count; j++) assert((a->nodes[i] == a->nodes[j]) == (b->nodes[i] == b->nodes[j]));
    }
}
static void ordered_passes(void)
{
    sh_package_link_edge edges[] = {{"model", 1}, {"material", 2}, {"model", 4}, {"material", 5},
        {"model", 7}, {"material", 8}};
    sh_package_link_node graph[9];
    sh_package_link_classes plain = {0}, ordered = {0};
    sh_package_link_order order = {0};
    char error[256];
    for (size_t i = 0; i < 3; i++) {
        graph[i * 3] = node("entityDef", "props/stone", 1, &edges[i * 2], 1);
        graph[i * 3 + 1] = node("model", "models/stone", 2, &edges[i * 2 + 1], 1);
        graph[i * 3 + 2] = node("material", "materials/stone", i == 1 ? 4 : 3, NULL, 0);
    }
    graph[8].content[0] = 5; /* materials 2, 5, 8 remembered in content order 3, 4, 5 */
    assert(sh_package_link_equivalence(graph, 9, &plain, error, sizeof(error)));
    assert(sh_package_link_equivalence_ordered(graph, 9, NULL, NULL, &order, &ordered, error, sizeof(error)));
    assert(order.count == 9 && order.items);
    same_identities(&plain, &ordered, 9);
    /* Reused unchanged. */
    size_t kept[9]; memcpy(kept, order.items, sizeof(kept));
    assert(sh_package_link_equivalence_ordered(graph, 9, NULL, NULL, &order, &ordered, error, sizeof(error)));
    same_identities(&plain, &ordered, 9);
    assert(!memcmp(kept, order.items, sizeof(kept)));
    /* A content change that breaks the remembered order: material 8 now equals
     * material 2, with material 5 between them in the old order. Sorted afresh,
     * so both pairs (materials 2/8 and their entities 0/6) share classes. */
    graph[8].content[0] = 3;
    assert(sh_package_link_equivalence(graph, 9, &plain, error, sizeof(error)));
    assert(sh_package_link_equivalence_ordered(graph, 9, NULL, NULL, &order, &ordered, error, sizeof(error)));
    same_identities(&plain, &ordered, 9);
    assert(plain.nodes[2] == plain.nodes[8] && plain.nodes[0] == plain.nodes[6] && plain.nodes[2] != plain.nodes[5]);
    /* Not a permutation of these nodes: ignored. */
    for (size_t i = 0; i < 9; i++) order.items[i] = 0;
    assert(sh_package_link_equivalence_ordered(graph, 9, NULL, NULL, &order, &ordered, error, sizeof(error)));
    same_identities(&plain, &ordered, 9);
    /* A hint for another node count is ignored and replaced. */
    order.count = 4;
    assert(sh_package_link_equivalence_ordered(graph, 9, NULL, NULL, &order, &ordered, error, sizeof(error)));
    same_identities(&plain, &ordered, 9);
    assert(order.count == 9);
    sh_package_link_order_free(&order);
    sh_package_link_classes_free(&plain); sh_package_link_classes_free(&ordered);
}
/* Reused orders over several passes, against an independent oracle: equal
 * strings at distinct addresses, several families and paths, edges in any
 * slot order (sorted only when not already sorted), and changes between
 * passes that keep, break or shuffle the remembered order. Every pass must
 * give exactly the classes an unordered partition gives. */
static void randomized_ordered(void)
{
    enum { N = 40, EDGES = 3 };
    static const char *const families[] = {"entitydef", "material", "model"};
    static const char *const paths[] = {"a/x", "a/y", "b/x", "b/xy", "c"};
    static const char *const slots[] = {"k:edit/k:model", "k:edit/k:target", "k:inherit", "k:edit/k:material"};
    char names[N][2][16];
    for (unsigned run = 0; run < 60; run++) {
        sh_package_link_edge edges[N][EDGES];
        sh_package_link_node graph[N];
        sh_package_link_order order = {0};
        char error[256];
        for (size_t i = 0; i < N; i++) {
            size_t count = next_random() % (EDGES + 1), first = next_random() % 4;
            snprintf(names[i][0], sizeof(names[i][0]), "%s", families[next_random() % 3]);
            snprintf(names[i][1], sizeof(names[i][1]), "%s", paths[next_random() % 5]);
            graph[i] = node(names[i][0], names[i][1], (unsigned char)(next_random() % 2), edges[i], count);
            for (size_t k = 0; k < count; k++)
                edges[i][k] = (sh_package_link_edge){slots[(first + k) % 4], next_random() % N};
        }
        for (int pass = 0; pass < 5; pass++) {
            sh_package_link_classes plain = {0}, ordered = {0};
            unsigned char equal[N][N], previous[N][N];
            int changed;
            if (pass) {
                /* Change contents, shuffle one node's slot order, retarget an edge. */
                size_t a = next_random() % N, b = next_random() % N, c = next_random() % N;
                graph[a].content[0] ^= (unsigned char)(pass & 1);
                if (graph[b].edge_count > 1) {
                    sh_package_link_edge swap = edges[b][0];
                    edges[b][0] = edges[b][graph[b].edge_count - 1]; edges[b][graph[b].edge_count - 1] = swap;
                }
                if (graph[c].edge_count) edges[c][0].target = next_random() % N;
            }
            for (size_t i = 0; i < N; i++) for (size_t j = 0; j < N; j++) {
                const sh_package_link_node *x = graph + i, *y = graph + j;
                int same = shape_equal(x, y);
                for (size_t k = 0; same && k < x->edge_count; k++) {
                    size_t m = 0;
                    while (m < y->edge_count && strcmp(x->edges[k].slot, y->edges[m].slot)) m++;
                    same = m < y->edge_count;
                }
                equal[i][j] = (unsigned char)same;
            }
            do {
                changed = 0; memcpy(previous, equal, sizeof(equal));
                for (size_t i = 0; i < N; i++) for (size_t j = 0; j < N; j++) if (previous[i][j]) {
                    for (size_t k = 0; k < graph[i].edge_count; k++) {
                        size_t m = 0;
                        while (strcmp(graph[i].edges[k].slot, graph[j].edges[m].slot)) m++;
                        if (!previous[graph[i].edges[k].target][graph[j].edges[m].target]) { equal[i][j] = 0; changed = 1; break; }
                    }
                }
            } while (changed);
            assert(sh_package_link_equivalence(graph, N, &plain, error, sizeof(error)));
            assert(sh_package_link_equivalence_ordered(graph, N, NULL, NULL, &order, &ordered, error, sizeof(error)));
            for (size_t i = 0; i < N; i++) for (size_t j = 0; j < N; j++) {
                assert((plain.nodes[i] == plain.nodes[j]) == equal[i][j]);
                assert((ordered.nodes[i] == ordered.nodes[j]) == equal[i][j]);
            }
            assert(plain.count == ordered.count && order.count == N);
            sh_package_link_classes_free(&plain); sh_package_link_classes_free(&ordered);
        }
        sh_package_link_order_free(&order);
    }
}
/* A remembered order in which a few shapes moved is repaired, not sorted
 * again: the first and last remembered items trade ends and a middle item
 * joins another tie. An unchanged pass keeps the order it was given, and an
 * order in which many shapes moved falls back to a full sort. Every pass gives
 * exactly the classes of an unordered partition. */
static void repaired_orders(void)
{
    enum { N = 96 };
    static const char *const families[] = {"entitydef", "material", "model"};
    static const char *const paths[] = {"a/x", "a/y", "b/z"};
    sh_package_link_edge edges[N];
    sh_package_link_node graph[N];
    sh_package_link_order order = {0};
    char error[256];
    for (size_t i = 0; i < N; i++) {
        /* Nine shapes of about ten nodes each; a third bind one target. */
        graph[i] = node(families[i % 3], paths[(i / 3) % 3], (unsigned char)(i % 2), NULL, 0);
        if (i % 3 == 0) {
            edges[i] = (sh_package_link_edge){"k:edit/k:model", (i * 7 + 1) % N};
            graph[i].edges = edges + i; graph[i].edge_count = 1;
        }
    }
    for (int pass = 0; pass < 6; pass++) {
        sh_package_link_classes plain = {0}, ordered = {0};
        size_t kept[N];
        unsigned char seen[N] = {0};
        if (pass == 1 || pass == 3) {
            size_t first = order.items[0], last = order.items[N - 1], middle = order.items[N / 2];
            size_t twin = (middle + 3) % N; /* the same edge shape as middle */
            graph[first].family = "zz-last"; graph[last].family = "aa-first";
            graph[middle].family = graph[twin].family; graph[middle].path = graph[twin].path;
            graph[middle].content[0] = graph[twin].content[0];
        }
        if (pass == 5) for (size_t i = 0; i < N; i += 2) graph[i].content[0] ^= 2; /* many moved */
        if (pass) memcpy(kept, order.items, sizeof(kept));
        assert(sh_package_link_equivalence(graph, N, &plain, error, sizeof(error)));
        assert(sh_package_link_equivalence_ordered(graph, N, NULL, NULL, &order, &ordered, error, sizeof(error)));
        same_identities(&plain, &ordered, N);
        assert(order.count == N);
        for (size_t i = 0; i < N; i++) { assert(order.items[i] < N && !seen[order.items[i]]); seen[order.items[i]] = 1; }
        if (pass == 2 || pass == 4) assert(!memcmp(kept, order.items, sizeof(kept)));
        if (pass == 1 || pass == 3) assert(memcmp(kept, order.items, sizeof(kept)));
        sh_package_link_classes_free(&plain); sh_package_link_classes_free(&ordered);
    }
    sh_package_link_order_free(&order);
}
/* Retained preparation against the unretained path over one evolving graph:
 * every call's status, error, verification calls, order, classes and
 * representatives agree, while unchanged nodes are reused, and only nodes whose
 * shape differs by exact comparison are prepared again. */
enum { RN = 600, RS = 6 };
static sh_package_link_node retained_graph[RN + 16];
static sh_package_link_edge retained_edges[RN + 16][6];
static char retained_paths[RN + 16][48];
static const char *retained_slots[] = {"a", "b", "c", "d", "e", "f", "g", "h"};
static size_t retained_calls[2], retained_side;
static int retained_verify(void *context, size_t left, size_t right, char *error, size_t capacity)
{
    (void)context; (void)left; (void)right; (void)error; (void)capacity;
    retained_calls[retained_side]++; return 1;
}
static void retained_node(size_t i, size_t count, unsigned char salt)
{
    size_t cls = i / RS, k, edges = cls % 5;
    snprintf(retained_paths[i], sizeof(retained_paths[i]), "decls/f%zu/r%04zu.decl", cls % 3, cls);
    retained_graph[i] = node(cls % 2 ? "model" : "entityDef", retained_paths[i], (unsigned char)(cls * 7 + salt), retained_edges[i], edges);
    for (k = 0; k < edges; k++) retained_edges[i][k] = (sh_package_link_edge){retained_slots[k],
        (((cls * 13 + k * 5 + 1) % (RN / RS)) * RS + i % RS) % count};
}
static void retained_step(sh_package_link_order *kept, sh_package_link_order *fresh, size_t count,
    int expect_ok, size_t expect_reused)
{
    sh_package_link_classes a = {0}, b = {0};
    char ea[256] = "", eb[256] = "";
    retained_calls[0] = retained_calls[1] = 0;
    retained_side = 0;
    int ra = sh_package_link_equivalence_ordered(retained_graph, count, retained_verify, NULL, kept, &a, ea, sizeof(ea));
    retained_side = 1;
    int rb = sh_package_link_equivalence_ordered(retained_graph, count, retained_verify, NULL, fresh, &b, eb, sizeof(eb));
    assert(ra == expect_ok && rb == expect_ok && !strcmp(ea, eb));
    assert(retained_calls[0] == retained_calls[1] && kept->count == fresh->count);
    assert(!kept->count || !memcmp(kept->items, fresh->items, kept->count * sizeof(*kept->items)));
    assert(!fresh->prepared && (ra ? kept->prepared != NULL : kept->prepared == NULL));
    if (ra) {
        assert(a.count == b.count && !memcmp(a.nodes, b.nodes, count * sizeof(*a.nodes)));
        assert(!memcmp(a.representatives, b.representatives, a.count * sizeof(*a.representatives)));
        assert(a.reused == expect_reused && b.reused == 0);
    }
    sh_package_link_classes_free(&a); sh_package_link_classes_free(&b);
}
static void retained_preparation(void)
{
    sh_package_link_order kept = {0}, fresh = {0};
    size_t i, count = RN, changed;
    char moved[48];
    fresh.unretained = 1;
    for (i = 0; i < count; i++) retained_node(i, count, 0);
    retained_step(&kept, &fresh, count, 1, 0);
    retained_step(&kept, &fresh, count, 1, count);           /* unchanged: all reused */
    for (i = 3, changed = 0; i < count; i += 97, changed++) retained_graph[i].content[1] ^= 1;
    retained_step(&kept, &fresh, count, 1, count - changed); /* edited content */
    for (i = 5, changed = 0; i < count; i += 89) if (retained_graph[i].edge_count) {
        retained_edges[i][0].target = (retained_edges[i][0].target + RS) % count; changed++;
    }
    retained_step(&kept, &fresh, count, 1, count - changed); /* changed bound edges */
    for (i = 7, changed = 0; i < count; i += 61) if (retained_graph[i].edge_count > 1) {
        sh_package_link_edge t = retained_edges[i][0]; retained_edges[i][0] = retained_edges[i][1]; retained_edges[i][1] = t; changed++;
    }
    retained_step(&kept, &fresh, count, 1, count - changed); /* reordered input bindings */
    retained_step(&kept, &fresh, count, 1, count);
    /* Same text at a new address is unchanged; text edited in place is not. */
    memcpy(moved, retained_paths[11], sizeof(moved)); retained_graph[11].path = moved;
    retained_step(&kept, &fresh, count, 1, count);
    retained_paths[13][0] = 'D';
    retained_step(&kept, &fresh, count, 1, count - 1);
    retained_graph[11].path = retained_paths[11];
    /* Appended, then removed nodes. */
    for (i = count; i < count + 9; i++) retained_node(i, count + 9, 1);
    retained_step(&kept, &fresh, count + 9, 1, count);
    for (i = 0, changed = 0; i < count - RS; i++) {
        int edited = 0;
        for (size_t k = 0; k < retained_graph[i].edge_count; k++) if (retained_edges[i][k].target >= count - RS) {
            retained_edges[i][k].target %= count - RS; edited = 1;
        }
        changed += (size_t)edited;
    }
    assert(changed);
    retained_step(&kept, &fresh, count - RS, 1, count - RS - changed);
    retained_step(&kept, &fresh, count - RS, 1, count - RS);
    /* A rejected graph discards the retained copy; the next call starts over. */
    retained_edges[17][1].slot = retained_edges[17][0].slot;
    assert(retained_graph[17].edge_count > 1);
    retained_step(&kept, &fresh, count - RS, 0, 0);
    retained_edges[17][1].slot = "z";
    retained_step(&kept, &fresh, count - RS, 1, 0);
    retained_edges[19][0].target = count;
    retained_step(&kept, &fresh, count - RS, 0, 0);
    retained_edges[19][0].target = 0;
    retained_step(&kept, &fresh, count - RS, 1, 0);
    retained_step(&kept, &fresh, count - RS, 1, count - RS);
    sh_package_link_order_free(&kept); sh_package_link_order_free(&fresh);
    assert(!kept.prepared && !kept.items);
}
/* Retained bytes stay bounded by the current preparation: zero-binding nodes
 * whose texts change every call, and a large graph shrinking to a few nodes,
 * rebuild instead of accumulating replaced texts and arrays. */
static char churn_text[RN][20480];
static void retained_bounds(void)
{
    {   /* Short unique paths: the interning table, not text bytes, dominates. */
        sh_package_link_order order = {0};
        sh_package_link_classes out = {0};
        char error[256];
        size_t i, round, peak = 0, rebuilt = 0, previous = 0;
        for (round = 0; round < 1200; round++) {
            for (i = 0; i < RN; i++) {
                snprintf(churn_text[i], 8, "%06zu", round * RN + i);
                retained_graph[i] = node("model", churn_text[i], 1, NULL, 0);
            }
            assert(sh_package_link_equivalence_ordered(retained_graph, RN, NULL, NULL, &order, &out, error, sizeof(error)));
            if (out.retained_held < previous) rebuilt++;
            previous = out.retained_held;
            if (out.retained_held > peak) peak = out.retained_held;
        }
        assert(rebuilt >= 2 && peak < ((size_t)32 << 20)); /* one call may double the table */
        sh_package_link_classes_free(&out); sh_package_link_order_free(&order);
    }
    sh_package_link_order order = {0};
    sh_package_link_classes out = {0};
    char error[256];
    size_t i, round, peak = 0, rebuilt = 0, previous = 0;
    for (round = 0; round < 300; round++) {
        for (i = 0; i < RN; i++) {
            memset(churn_text[i], 'a' + (int)(i % 26), 200);
            snprintf(churn_text[i] + 200, 40, "/%zu/%zu", round, i);
            retained_graph[i] = node("model", churn_text[i], 1, NULL, 0);
        }
        assert(sh_package_link_equivalence_ordered(retained_graph, RN, NULL, NULL, &order, &out, error, sizeof(error)));
        assert(out.reused == 0 && out.retained_held && out.retained_live);
        if (out.retained_held < previous) rebuilt++;
        previous = out.retained_held;
        if (out.retained_held > peak) peak = out.retained_held;
    }
    /* 300 rounds replace ~40 MB of texts; held stays within 2 x live + 8 MB plus one call. */
    assert(rebuilt && peak < ((size_t)16 << 20));
    for (i = 0; i < RN; i++) {
        memset(churn_text[i], 'b', 20000); churn_text[i][20000] = 0;
        snprintf(churn_text[i] + 19990, 10, "%zu", i);
        retained_graph[i] = node("model", churn_text[i], 1, NULL, 0);
    }
    for (round = 0; round < 8; round++)
        assert(sh_package_link_equivalence_ordered(retained_graph, RN, NULL, NULL, &order, &out, error, sizeof(error)));
    size_t large = out.retained_held;
    assert(sh_package_link_equivalence_ordered(retained_graph, 4, NULL, NULL, &order, &out, error, sizeof(error)));
    assert(sh_package_link_equivalence_ordered(retained_graph, 4, NULL, NULL, &order, &out, error, sizeof(error)));
    assert(out.reused == 0 && out.retained_held < large); /* shrunk: rebuilt */
    assert(sh_package_link_equivalence_ordered(retained_graph, 4, NULL, NULL, &order, &out, error, sizeof(error)));
    assert(out.reused == 4);
    sh_package_link_classes_free(&out); sh_package_link_order_free(&order);
}
/* Every allocation of a retained call refused in turn: the call either
 * succeeds exactly like the unretained path or fails and discards the
 * retained copy (including the final output allocation); the next call
 * recovers. */
static void retained_allocation_failures(void)
{
    size_t i, k, total, failed = 0, fallback = 0;
    for (i = 0; i < RN; i++) retained_node(i, RN, 0);
    sh_package_link_order fresh = {0};
    sh_package_link_classes want = {0};
    char error[256];
    fresh.unretained = 1;
    assert(sh_package_link_equivalence_ordered(retained_graph, RN, NULL, NULL, &fresh, &want, error, sizeof(error)));
    for (k = 1;; k++) {
        sh_package_link_order kept = {0};
        sh_package_link_classes got = {0};
        assert(sh_package_link_equivalence_ordered(retained_graph, RN, NULL, NULL, &kept, &got, error, sizeof(error)));
        retained_graph[3].content[2] ^= 1; /* one changed node: both paths of the call */
        sh_package_link_graph_test_refuse(k);
        int ok = sh_package_link_equivalence_ordered(retained_graph, RN, NULL, NULL, &kept, &got, error, sizeof(error));
        total = sh_package_link_graph_test_allocations();
        sh_package_link_graph_test_refuse(0);
        retained_graph[3].content[2] ^= 1;
        if (k > total) { assert(ok && kept.prepared); sh_package_link_classes_free(&got); sh_package_link_order_free(&kept); break; }
        if (!ok) { failed++; assert(!kept.prepared && !got.nodes && *error); }
        else if (!kept.prepared) fallback++;
        assert(sh_package_link_equivalence_ordered(retained_graph, RN, NULL, NULL, &kept, &got, error, sizeof(error)));
        assert(got.count == want.count);
        for (i = 0; i < RN; i++) for (size_t j = 0; j < RN; j++) assert((got.nodes[i] == got.nodes[j]) == (want.nodes[i] == want.nodes[j]));
        sh_package_link_classes_free(&got); sh_package_link_order_free(&kept);
    }
    assert(failed && fallback); /* output allocation failed the call; retaining fell back */
    sh_package_link_classes_free(&want); sh_package_link_order_free(&fresh);
}
int main(void)
{
    retained_preparation();
    retained_bounds();
    retained_allocation_failures();
    ordered_passes();
    randomized_ordered();
    repaired_orders();
    variants(); cycles(); edge_order_and_failures(); randomized(); deep_dependency_change(); original_cycles();
    positive_fingerprint_verification();
    puts("package_link_graph_test: PASS");
    return 0;
}
