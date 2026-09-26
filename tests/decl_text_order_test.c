/* Dependency ordering: the adjacency-list implementation must reproduce the
 * earlier matrix implementation exactly (kept below as the reference) on
 * random reference graphs, and scale to a large library. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "decl_text.h"

/* ---- reference: the previous implementation, verbatim ---- */
static int ascii_lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static int ascii_equal_span(const unsigned char *text, size_t length,
                            const char *name)
{
    size_t i;
    if (!text || !name || strlen(name) != length) return 0;
    for (i = 0; i < length; i++)
        if (ascii_lower(text[i]) != ascii_lower((unsigned char)name[i])) return 0;
    return 1;
}

static int decl_name_compare(const sh_decl_reference_item *items,
                             size_t left, size_t right)
{
    const char *a = items[left].name ? items[left].name : "";
    const char *b = items[right].name ? items[right].name : "";
    size_t i = 0;

    while (a[i] && b[i]) {
        int ca = ascii_lower((unsigned char)a[i]);
        int cb = ascii_lower((unsigned char)b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
        i++;
    }
    if (a[i] || b[i]) return a[i] ? 1 : -1;
    if (strcmp(a, b) != 0) return strcmp(a, b) < 0 ? -1 : 1;
    return left < right ? -1 : (left > right ? 1 : 0);
}

static int resource_name_equal(const unsigned char *text, size_t length, const char *name)
{
    if (!text || !name || strlen(name) != length) return 0;
    for (size_t i = 0; i < length; i++) {
        int a = text[i] == '\\' ? '/' : ascii_lower(text[i]);
        int b = name[i] == '\\' ? '/' : ascii_lower((unsigned char)name[i]);
        if (a != b) return 0;
    }
    return 1;
}

static int is_decl_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

static int is_decl_key_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

/* The engine's inheritance field is an assignment, so identify it from the
 * quote's immediate lexical context instead of treating every quoted string
 * as an inheritance edge. */
static int quote_is_inherit_key(const unsigned char *text, size_t quote_start)
{
    size_t i = quote_start;
    size_t key_end;

    while (i > 0 && is_decl_space(text[i - 1])) i--;
    if (i == 0 || text[i - 1] != '=') return 0;
    i--;
    while (i > 0 && is_decl_space(text[i - 1])) i--;
    key_end = i;
    while (i > 0 && is_decl_key_char(text[i - 1])) i--;
    return ascii_equal_span(text + i, key_end - i, "inherit");
}


static void add_reference(const sh_decl_reference_item *items, size_t count,
                                 size_t dependent, const unsigned char *text,
                                 size_t length, unsigned char *edges,
                                 unsigned char *inherit_edges,
                                 size_t *indegree, size_t *edge_count,
                                 int is_inherit, const char *required_type)
{
    size_t i;
    size_t match = count;
    size_t matches = 0;
    for (i = 0; i < count; i++) {
        if (required_type && (!items[i].type ||
            !ascii_equal_span((const unsigned char *)items[i].type,
                               strlen(items[i].type), required_type))) continue;
        if (!resource_name_equal(text, length, items[i].name)) continue;
        match = i;
        matches++;
        if (matches > 1) return;
    }
    if (matches == 1) {
        if (is_inherit) inherit_edges[match * count + dependent] = 1;
        if (match != dependent && !edges[match * count + dependent]) {
            edges[match * count + dependent] = 1;
            indegree[dependent]++;
            (*edge_count)++;
        }
    }
}

static void scan_references(const sh_decl_reference_item *items, size_t count,
                            size_t dependent, unsigned char *edges,
                            unsigned char *inherit_edges, size_t *indegree,
                            size_t *edge_count)
{
    const unsigned char *text = items[dependent].text;
    size_t length = items[dependent].text_length;
    size_t i = 0;
    int line_comment = 0;
    int block_comment = 0;

    while (text && i < length) {
        unsigned char c = text[i];
        unsigned char next = i + 1 < length ? text[i + 1] : 0;
        if (line_comment) {
            if (c == '\n') line_comment = 0;
            i++;
            continue;
        }
        if (block_comment) {
            if (c == '*' && next == '/') {
                block_comment = 0;
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (c == '/' && next == '/') {
            line_comment = 1;
            i += 2;
            continue;
        }
        if (c == '/' && next == '*') {
            block_comment = 1;
            i += 2;
            continue;
        }
        if (c == '"') {
            size_t quote_start = i;
            size_t start = ++i;
            while (i < length && text[i] != '"') i++;
            if (i < length)
                add_reference(items, count, dependent, text + start,
                                     i - start, edges, inherit_edges, indegree,
                                     edge_count, quote_is_inherit_key(text, quote_start), NULL);
            if (i < length) i++;
            continue;
        }
        /* Table lookups in native material expressions are bare identifiers,
         * not quoted paths. Match the complete token and only the table type:
         * an unrelated decl with the same name cannot make this ambiguous.
         * Array fields in other decl types are not material expressions. */
        if (is_decl_key_char(c) && items[dependent].type &&
            ascii_equal_span((const unsigned char *)items[dependent].type,
                              strlen(items[dependent].type), "material")) {
            size_t start = i;
            size_t end;
            while (i < length && is_decl_key_char(text[i])) i++;
            end = i;
            while (i < length && is_decl_space(text[i])) i++;
            if (i < length && text[i] == '[')
                add_reference(items, count, dependent, text + start, end - start,
                              edges, inherit_edges, indegree, edge_count, 0, "table");
            continue;
        }
        i++;
    }
}

typedef struct decl_scc_context {
    const unsigned char *edges;
    size_t count;
    size_t next_index;
    size_t *indices;
    size_t *lowlink;
    unsigned char *on_stack;
    size_t *stack;
    size_t stack_count;
    size_t *component;
    size_t component_count;
} decl_scc_context;

static void decl_scc_visit(decl_scc_context *context, size_t vertex)
{
    size_t adjacent;
    size_t root;

    context->indices[vertex] = context->next_index;
    context->lowlink[vertex] = context->next_index;
    context->next_index++;
    context->stack[context->stack_count++] = vertex;
    context->on_stack[vertex] = 1;

    for (adjacent = 0; adjacent < context->count; adjacent++) {
        if (!context->edges[vertex * context->count + adjacent]) continue;
        if (context->indices[adjacent] == SIZE_MAX) {
            decl_scc_visit(context, adjacent);
            if (context->lowlink[adjacent] < context->lowlink[vertex])
                context->lowlink[vertex] = context->lowlink[adjacent];
        } else if (context->on_stack[adjacent] &&
                   context->indices[adjacent] < context->lowlink[vertex]) {
            context->lowlink[vertex] = context->indices[adjacent];
        }
    }

    if (context->lowlink[vertex] != context->indices[vertex]) return;
    do {
        root = context->stack[--context->stack_count];
        context->on_stack[root] = 0;
        context->component[root] = context->component_count;
    } while (root != vertex);
    context->component_count++;
}

static size_t component_first_member(const sh_decl_reference_item *items,
                                     const size_t *component, size_t count,
                                     size_t component_id)
{
    size_t i;
    size_t first = count;
    for (i = 0; i < count; i++) {
        if (component[i] != component_id) continue;
        if (first == count || decl_name_compare(items, i, first) < 0) first = i;
    }
    return first;
}

static int component_before(const sh_decl_reference_item *items,
                            const size_t *component, size_t count,
                            size_t left, size_t right)
{
    size_t left_first;
    size_t right_first;
    if (right == count) return 1;
    left_first = component_first_member(items, component, count, left);
    right_first = component_first_member(items, component, count, right);
    return decl_name_compare(items, left_first, right_first) < 0;
}

static int order_component_members(const sh_decl_reference_item *items,
                                   size_t count, size_t component_id,
                                   const size_t *component,
                                   const unsigned char *inherit_edges,
                                   size_t *member_indegree,
                                   unsigned char *member_emitted,
                                   sh_decl_reference_item *ordered,
                                   size_t *ordered_count)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (component[i] != component_id) continue;
        member_indegree[i] = 0;
        member_emitted[i] = 0;
    }
    for (i = 0; i < count; i++) {
        size_t dependent;
        if (component[i] != component_id) continue;
        for (dependent = 0; dependent < count; dependent++) {
            if (component[dependent] == component_id &&
                inherit_edges[i * count + dependent]) {
                member_indegree[dependent]++;
            }
        }
    }

    for (;;) {
        size_t selected = count;
        size_t dependent;
        for (i = 0; i < count; i++) {
            int parent_i = 0, parent_s = 0;
            if (component[i] != component_id || member_emitted[i] ||
                member_indegree[i] != 0) continue;
            /* Members another member of the component inherits come first. */
            for (dependent = 0; dependent < count; dependent++) {
                if (component[dependent] != component_id) continue;
                if (inherit_edges[i * count + dependent]) parent_i = 1;
                if (selected != count && inherit_edges[selected * count + dependent]) parent_s = 1;
            }
            if (selected == count || parent_i > parent_s ||
                (parent_i == parent_s && decl_name_compare(items, i, selected) < 0))
                selected = i;
        }
        if (selected == count) {
            /* A cycle in explicit inheritance is unsafe: there is no valid
             * parent-before-child order, so refuse the whole snapshot. */
            for (i = 0; i < count; i++)
                if (component[i] == component_id && !member_emitted[i]) return 0;
            return 1;
        }
        member_emitted[selected] = 1;
        ordered[(*ordered_count)++] = items[selected];
        for (dependent = 0; dependent < count; dependent++) {
            if (component[dependent] == component_id &&
                inherit_edges[selected * count + dependent] &&
                member_indegree[dependent] > 0) {
                member_indegree[dependent]--;
            }
        }
    }
}

static int old_order_by_references(sh_decl_reference_item *items, size_t count,
                                     size_t *out_edge_count,
                                     size_t *out_cycle_count)
{
    unsigned char *edges = NULL;
    unsigned char *inherit_edges = NULL;
    unsigned char *component_edges = NULL;
    unsigned char *component_emitted = NULL;
    unsigned char *member_emitted = NULL;
    size_t *indegree = NULL;
    size_t *component_indegree = NULL;
    size_t *component = NULL;
    size_t *indices = NULL;
    size_t *lowlink = NULL;
    size_t *stack = NULL;
    size_t *member_indegree = NULL;
    sh_decl_reference_item *ordered = NULL;
    size_t edge_count = 0;
    size_t ordered_count = 0;
    size_t component_count = 0;
    size_t cycle_count = 0;
    size_t i;
    int ok = 0;

    if (out_edge_count) *out_edge_count = 0;
    if (out_cycle_count) *out_cycle_count = 0;
    if (!items || count == 0) return count == 0;
    if (count > SIZE_MAX / count) return 0;

    edges = (unsigned char *)calloc(count * count, sizeof(edges[0]));
    inherit_edges = (unsigned char *)calloc(count * count, sizeof(inherit_edges[0]));
    component_edges = (unsigned char *)calloc(count * count, sizeof(component_edges[0]));
    component_emitted = (unsigned char *)calloc(count, sizeof(component_emitted[0]));
    member_emitted = (unsigned char *)calloc(count, sizeof(member_emitted[0]));
    indegree = (size_t *)calloc(count, sizeof(indegree[0]));
    component_indegree = (size_t *)calloc(count, sizeof(component_indegree[0]));
    component = (size_t *)malloc(count * sizeof(component[0]));
    indices = (size_t *)malloc(count * sizeof(indices[0]));
    lowlink = (size_t *)malloc(count * sizeof(lowlink[0]));
    stack = (size_t *)malloc(count * sizeof(stack[0]));
    member_indegree = (size_t *)calloc(count, sizeof(member_indegree[0]));
    ordered = (sh_decl_reference_item *)malloc(count * sizeof(ordered[0]));
    if (!edges || !inherit_edges || !component_edges || !component_emitted ||
        !member_emitted || !indegree || !component_indegree || !component ||
        !indices || !lowlink || !stack || !member_indegree || !ordered) goto done;

    for (i = 0; i < count; i++) {
        component[i] = SIZE_MAX;
        indices[i] = SIZE_MAX;
        scan_references(items, count, i, edges, inherit_edges, indegree,
                        &edge_count);
    }

    {
        decl_scc_context context;
        memset(&context, 0, sizeof(context));
        context.edges = edges;
        context.count = count;
        context.indices = indices;
        context.lowlink = lowlink;
        context.on_stack = member_emitted;
        context.stack = stack;
        context.component = component;
        for (i = 0; i < count; i++)
            if (indices[i] == SIZE_MAX) decl_scc_visit(&context, i);
        component_count = context.component_count;
    }

    for (i = 0; i < count; i++) {
        size_t dependent;
        size_t source_component = component[i];
        for (dependent = 0; dependent < count; dependent++) {
            size_t dependent_component = component[dependent];
            if (!edges[i * count + dependent]) continue;
            if (source_component == dependent_component) continue;
            if (!component_edges[source_component * count + dependent_component]) {
                component_edges[source_component * count + dependent_component] = 1;
                component_indegree[dependent_component]++;
            }
        }
    }
    for (i = 0; i < component_count; i++) {
        size_t member_count = 0;
        size_t member;
        for (member = 0; member < count; member++)
            if (component[member] == i) member_count++;
        if (member_count > 1) cycle_count += member_count;
        for (member = 0; member < count; member++) {
            if (component[member] == i && inherit_edges[member * count + member])
                cycle_count += member_count == 1 ? 1 : 0;
        }
    }
    while (ordered_count < count) {
        size_t selected_component = component_count;
        size_t component_id;
        for (component_id = 0; component_id < component_count; component_id++) {
            if (component_emitted[component_id] || component_indegree[component_id] != 0)
                continue;
            if (selected_component == component_count ||
                component_before(items, component, count, component_id,
                                 selected_component)) {
                selected_component = component_id;
            }
        }
        if (selected_component == component_count) goto done;
        if (!order_component_members(items, count, selected_component, component,
                                     inherit_edges, member_indegree, member_emitted,
                                     ordered, &ordered_count)) goto done;
        component_emitted[selected_component] = 1;
        for (component_id = 0; component_id < component_count; component_id++) {
            if (component_edges[selected_component * count + component_id] &&
                component_indegree[component_id] > 0) {
                component_indegree[component_id]--;
            }
        }
    }

    memcpy(items, ordered, count * sizeof(items[0]));
    if (out_edge_count) *out_edge_count = edge_count;
    if (out_cycle_count) *out_cycle_count = cycle_count;
    ok = 1;

done:
    free(ordered);
    free(member_indegree);
    free(stack);
    free(lowlink);
    free(indices);
    free(component);
    free(component_indegree);
    free(indegree);
    free(member_emitted);
    free(component_emitted);
    free(component_edges);
    free(inherit_edges);
    free(edges);
    return ok;
}

/* ---- random graphs ---- */
static unsigned long long seed = 88172645463325252ULL;
static unsigned rnd(unsigned n) { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return (unsigned)(seed % n); }

static const char *const names[] = {
    "a/base", "a/child", "a/grand", "A/Base", "b\\side", "b/side", "c/loop", "c/loop2",
    "d/table", "tab1", "tab2", "e/solo", "f/x", "f/y", "g/deep/name", "h", "i/j", "k/l"
};
static const char *const types[] = { "material", "table", "entitydef", "fx", NULL };

static void append(char *out, size_t *used, size_t capacity, const char *text)
{
    size_t n = strlen(text);
    if (*used + n + 1 >= capacity) return;
    memcpy(out + *used, text, n); *used += n; out[*used] = 0;
}

static void random_body(char *out, size_t capacity, size_t count, char name_pool[][32], const char **type_pool)
{
    size_t used = 0; char piece[128];
    out[0] = 0;
    append(out, &used, capacity, "{ ");
    for (unsigned k = rnd(6); k; k--) {
        const char *target = name_pool[rnd((unsigned)count)];
        switch (rnd(8)) {
        case 0: snprintf(piece, sizeof(piece), "inherit = \"%s\"; ", target); break;
        case 1: snprintf(piece, sizeof(piece), "inherit=\"%s\" ", target); break;
        case 2: snprintf(piece, sizeof(piece), "// \"%s\"\n", target); break;
        case 3: snprintf(piece, sizeof(piece), "/* \"%s\" */ ", target); break;
        case 4: snprintf(piece, sizeof(piece), "%s[ 0 ] ", rnd(2) ? "tab1" : "tab2"); break;
        case 5: snprintf(piece, sizeof(piece), "value = \"%s\"; ", names[rnd(sizeof(names) / sizeof(*names))]); break;
        default: snprintf(piece, sizeof(piece), "ref = \"%s\"; ", target); break;
        }
        append(out, &used, capacity, piece);
    }
    (void)type_pool;
    append(out, &used, capacity, "}");
}

static int same(const sh_decl_reference_item *a, const sh_decl_reference_item *b, size_t count)
{
    for (size_t i = 0; i < count; i++) if (a[i].value != b[i].value) return 0;
    return 1;
}

int main(void)
{
    static char bodies[64][512];
    static char pool[64][32];
    sh_decl_reference_item left[64], right[64];
    unsigned cases = 0, refused = 0, cycles = 0;
    for (unsigned round = 0; round < 20000; round++) {
        size_t count = 1 + rnd(40), old_edges = 0, new_edges = 0, old_cycles = 0, new_cycles = 0;
        const char *type_of[64];
        for (size_t i = 0; i < count; i++) {
            snprintf(pool[i], sizeof(pool[i]), "%s", names[rnd(sizeof(names) / sizeof(*names))]);
            type_of[i] = types[rnd(5)];
        }
        for (size_t i = 0; i < count; i++) {
            random_body(bodies[i], sizeof(bodies[i]), count, pool, type_of);
            left[i].name = rnd(40) ? pool[i] : NULL;
            left[i].type = type_of[i];
            left[i].text = (const unsigned char *)bodies[i];
            left[i].text_length = rnd(30) ? strlen(bodies[i]) : 0;
            left[i].value = (void *)(uintptr_t)(i + 1);
            right[i] = left[i];
        }
        int a = old_order_by_references(left, count, &old_edges, &old_cycles);
        int b = sh_decl_text_order_by_references(right, count, &new_edges, &new_cycles);
        if (a != b || (a && (!same(left, right, count) || old_edges != new_edges || old_cycles != new_cycles))) {
            printf("mismatch in round %u (count %zu): old %d/%zu/%zu new %d/%zu/%zu\n",
                round, count, a, old_edges, old_cycles, b, new_edges, new_cycles);
            return 1;
        }
        cases++; refused += !a; cycles += a && old_cycles;
    }
    assert(refused && cycles && refused < cases);

    /* A large chain with a cycle and shared bases orders quickly and exactly. */
    size_t big = 30000;
    sh_decl_reference_item *items = calloc(big, sizeof(*items));
    char (*text)[96] = calloc(big, sizeof(*text)), (*name)[24] = calloc(big, sizeof(*name));
    assert(items && text && name);
    for (size_t i = 0; i < big; i++) {
        snprintf(name[i], sizeof(name[i]), "n/%zu", i);
        if (i == 0) snprintf(text[i], sizeof(text[i]), "{ ref = \"n/%zu\"; }", big - 1);
        else snprintf(text[i], sizeof(text[i]), "{ inherit = \"n/%zu\"; ref = \"n/%zu\"; }", i / 2, i - 1);
        items[i].name = name[i]; items[i].type = "entitydef";
        items[i].text = (const unsigned char *)text[i]; items[i].text_length = strlen(text[i]);
        items[i].value = (void *)(uintptr_t)(i + 1);
    }
    size_t edges = 0, cyclic = 0; clock_t started = clock();
    assert(sh_decl_text_order_by_references(items, big, &edges, &cyclic));
    double seconds = (double)(clock() - started) / CLOCKS_PER_SEC;
    assert(cyclic == big && edges >= big);
    /* One component: every parent precedes its children. */
    size_t *position = calloc(big, sizeof(*position)); assert(position);
    for (size_t i = 0; i < big; i++) position[(size_t)(uintptr_t)items[i].value - 1] = i;
    for (size_t i = 1; i < big; i++) assert(position[i / 2] < position[i]);
    free(position);
    free(items); free(text); free(name);
    printf("dependency order: %u random graphs match the reference (%u refused, %u cyclic); %zu items in %.2f s\n",
        cases, refused, cycles, big, seconds);
    return 0;
}
