/* decl_text.c -- see decl_text.h. Pure C; no engine or filesystem dependency. */
#include "decl_text.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

/* Dependency ordering works on adjacency lists: a hashed name index resolves
 * each quoted reference, and every scan below is linear in items and edges.
 * The order equals the definition above: repeatedly emit the ready component
 * whose first member sorts lowest, members parent-first then by name. */
typedef struct decl_pair {
    size_t from, to;
} decl_pair;

typedef struct decl_order_work {
    const sh_decl_reference_item *items;
    size_t count;
    size_t *bucket_head;
    size_t *bucket_next;
    size_t bucket_mask;
    decl_pair *edges;
    size_t edge_count, edge_capacity;
    decl_pair *inherits;
    size_t inherit_count, inherit_capacity;
    int failed;
} decl_order_work;

static uint64_t decl_name_hash(const unsigned char *text, size_t length)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    size_t i;
    for (i = 0; i < length; i++) {
        int c = text[i] == '\\' ? '/' : ascii_lower(text[i]);
        hash ^= (unsigned char)c;
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int decl_pair_push(decl_pair **pairs, size_t *count, size_t *capacity,
                          size_t from, size_t to)
{
    if (*count == *capacity) {
        size_t next = *capacity ? *capacity * 2 : 256;
        decl_pair *grown = next > *capacity && next <= SIZE_MAX / sizeof(*grown) ?
            (decl_pair *)realloc(*pairs, next * sizeof(*grown)) : NULL;
        if (!grown) return 0;
        *pairs = grown;
        *capacity = next;
    }
    (*pairs)[*count].from = from;
    (*pairs)[*count].to = to;
    (*count)++;
    return 1;
}

static int decl_pair_order(const void *left, const void *right)
{
    const decl_pair *a = (const decl_pair *)left, *b = (const decl_pair *)right;
    if (a->from != b->from) return a->from < b->from ? -1 : 1;
    return a->to < b->to ? -1 : (a->to > b->to ? 1 : 0);
}

static size_t decl_pair_unique(decl_pair *pairs, size_t count)
{
    size_t kept = 0, i;
    if (count > 1) qsort(pairs, count, sizeof(*pairs), decl_pair_order);
    for (i = 0; i < count; i++)
        if (!kept || pairs[kept - 1].from != pairs[i].from || pairs[kept - 1].to != pairs[i].to)
            pairs[kept++] = pairs[i];
    return kept;
}

/* One quoted or typed reference: a single matching item gives an edge from
 * that item to the dependent; an ambiguous name gives none. */
static void add_reference(decl_order_work *w, size_t dependent,
                          const unsigned char *text, size_t length,
                          int is_inherit, const char *required_type)
{
    size_t match = w->count, matches = 0, i;
    if (w->failed) return;
    for (i = w->bucket_head[decl_name_hash(text, length) & w->bucket_mask];
         i != SIZE_MAX; i = w->bucket_next[i]) {
        const sh_decl_reference_item *item = w->items + i;
        if (required_type && (!item->type ||
            !ascii_equal_span((const unsigned char *)item->type,
                              strlen(item->type), required_type))) continue;
        if (!resource_name_equal(text, length, item->name)) continue;
        match = i;
        if (++matches > 1) return;
    }
    if (matches != 1) return;
    if (is_inherit && !decl_pair_push(&w->inherits, &w->inherit_count,
                                      &w->inherit_capacity, match, dependent))
        w->failed = 1;
    if (match != dependent && !decl_pair_push(&w->edges, &w->edge_count,
                                              &w->edge_capacity, match, dependent))
        w->failed = 1;
}

static void scan_references(decl_order_work *w, size_t dependent)
{
    const unsigned char *text = w->items[dependent].text;
    size_t length = w->items[dependent].text_length;
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
                add_reference(w, dependent, text + start, i - start,
                              quote_is_inherit_key(text, quote_start), NULL);
            if (i < length) i++;
            continue;
        }
        /* Table lookups in native material expressions are bare identifiers,
         * not quoted paths. Match the complete token and only the table type:
         * an unrelated decl with the same name cannot make this ambiguous.
         * Array fields in other decl types are not material expressions. */
        if (is_decl_key_char(c) && w->items[dependent].type &&
            ascii_equal_span((const unsigned char *)w->items[dependent].type,
                              strlen(w->items[dependent].type), "material")) {
            size_t start = i;
            size_t end;
            while (i < length && is_decl_key_char(text[i])) i++;
            end = i;
            while (i < length && is_decl_space(text[i])) i++;
            if (i < length && text[i] == '[')
                add_reference(w, dependent, text + start, end - start, 0, "table");
            continue;
        }
        i++;
    }
}

/* Binary min-heap of item or component indices ordered by a first item. */
typedef struct decl_heap {
    size_t *slots;
    size_t count;
    const sh_decl_reference_item *items;
    const size_t *key; /* NULL: the entry is its own item */
} decl_heap;

static int decl_heap_less(const decl_heap *heap, size_t left, size_t right)
{
    size_t a = heap->key ? heap->key[left] : left;
    size_t b = heap->key ? heap->key[right] : right;
    return decl_name_compare(heap->items, a, b) < 0;
}

static void decl_heap_push(decl_heap *heap, size_t value)
{
    size_t at = heap->count++;
    heap->slots[at] = value;
    while (at && decl_heap_less(heap, heap->slots[at], heap->slots[(at - 1) / 2])) {
        size_t parent = (at - 1) / 2, swap = heap->slots[parent];
        heap->slots[parent] = heap->slots[at];
        heap->slots[at] = swap;
        at = parent;
    }
}

static size_t decl_heap_pop(decl_heap *heap)
{
    size_t top = heap->slots[0], at = 0;
    heap->slots[0] = heap->slots[--heap->count];
    for (;;) {
        size_t left = at * 2 + 1, right = left + 1, least = at, swap;
        if (left < heap->count && decl_heap_less(heap, heap->slots[left], heap->slots[least])) least = left;
        if (right < heap->count && decl_heap_less(heap, heap->slots[right], heap->slots[least])) least = right;
        if (least == at) break;
        swap = heap->slots[least];
        heap->slots[least] = heap->slots[at];
        heap->slots[at] = swap;
        at = least;
    }
    return top;
}

/* Compressed adjacency: offsets[from]..offsets[from + 1] index targets. */
static int decl_adjacency(const decl_pair *pairs, size_t pair_count, size_t vertices,
                          size_t **offsets, size_t **targets)
{
    size_t i;
    *offsets = (size_t *)calloc(vertices + 1, sizeof(**offsets));
    *targets = (size_t *)malloc((pair_count ? pair_count : 1) * sizeof(**targets));
    if (!*offsets || !*targets) return 0;
    for (i = 0; i < pair_count; i++) (*offsets)[pairs[i].from + 1]++;
    for (i = 0; i < vertices; i++) (*offsets)[i + 1] += (*offsets)[i];
    for (i = 0; i < pair_count; i++) (*targets)[i] = pairs[i].to; /* pairs are sorted by from */
    return 1;
}

int sh_decl_text_order_by_references(sh_decl_reference_item *items, size_t count,
                                     size_t *out_edge_count,
                                     size_t *out_cycle_count)
{
    decl_order_work w;
    size_t *edge_offsets = NULL, *edge_targets = NULL;
    size_t *inherit_offsets = NULL, *inherit_targets = NULL;
    size_t *indices = NULL, *lowlink = NULL, *stack = NULL, *frames = NULL, *positions = NULL;
    size_t *component = NULL, *first = NULL, *member_offsets = NULL, *members = NULL, *cursor = NULL;
    size_t *component_offsets = NULL, *component_targets = NULL, *component_indegree = NULL;
    size_t *member_indegree = NULL, *heap_slots = NULL, *member_slots = NULL;
    unsigned char *on_stack = NULL;
    decl_pair *component_pairs = NULL;
    sh_decl_reference_item *ordered = NULL;
    size_t component_count = 0, cycle_count = 0, ordered_count = 0, next_index = 0;
    size_t component_pair_count = 0, buckets = 1, i;
    int ok = 0;

    if (out_edge_count) *out_edge_count = 0;
    if (out_cycle_count) *out_cycle_count = 0;
    if (!items || count == 0) return count == 0;
    memset(&w, 0, sizeof(w));
    w.items = items;
    w.count = count;
    if (count >= SIZE_MAX / 4) return 0;
    while (buckets < count * 2) buckets <<= 1;
    w.bucket_mask = buckets - 1;
    w.bucket_head = (size_t *)malloc(buckets * sizeof(*w.bucket_head));
    w.bucket_next = (size_t *)malloc(count * sizeof(*w.bucket_next));
    indices = (size_t *)malloc(count * sizeof(*indices));
    lowlink = (size_t *)malloc(count * sizeof(*lowlink));
    stack = (size_t *)malloc(count * sizeof(*stack));
    frames = (size_t *)malloc(count * sizeof(*frames));
    positions = (size_t *)malloc(count * sizeof(*positions));
    component = (size_t *)malloc(count * sizeof(*component));
    on_stack = (unsigned char *)calloc(count, 1);
    ordered = (sh_decl_reference_item *)malloc(count * sizeof(*ordered));
    if (!w.bucket_head || !w.bucket_next || !indices || !lowlink || !stack || !frames ||
        !positions || !component || !on_stack || !ordered) goto done;
    for (i = 0; i < buckets; i++) w.bucket_head[i] = SIZE_MAX;
    for (i = count; i-- > 0;) {
        size_t slot;
        w.bucket_next[i] = SIZE_MAX;
        if (!items[i].name) continue;
        slot = (size_t)decl_name_hash((const unsigned char *)items[i].name,
                                      strlen(items[i].name)) & w.bucket_mask;
        w.bucket_next[i] = w.bucket_head[slot];
        w.bucket_head[slot] = i;
    }
    for (i = 0; i < count; i++) scan_references(&w, i);
    if (w.failed) goto done;
    w.edge_count = decl_pair_unique(w.edges, w.edge_count);
    w.inherit_count = decl_pair_unique(w.inherits, w.inherit_count);
    if (!decl_adjacency(w.edges, w.edge_count, count, &edge_offsets, &edge_targets) ||
        !decl_adjacency(w.inherits, w.inherit_count, count, &inherit_offsets, &inherit_targets))
        goto done;

    /* Tarjan's strongly connected components, iteratively. */
    {
        size_t stack_count = 0, root;
        for (i = 0; i < count; i++) { indices[i] = SIZE_MAX; component[i] = SIZE_MAX; on_stack[i] = 0; }
        next_index = 0;
        for (root = 0; root < count; root++) {
            size_t depth = 0;
            if (indices[root] != SIZE_MAX) continue;
            indices[root] = lowlink[root] = next_index++;
            stack[stack_count++] = root; on_stack[root] = 1;
            frames[depth] = root; positions[depth] = edge_offsets[root]; depth++;
            while (depth) {
                size_t v = frames[depth - 1];
                if (positions[depth - 1] < edge_offsets[v + 1]) {
                    size_t u = edge_targets[positions[depth - 1]++];
                    if (indices[u] == SIZE_MAX) {
                        indices[u] = lowlink[u] = next_index++;
                        stack[stack_count++] = u; on_stack[u] = 1;
                        frames[depth] = u; positions[depth] = edge_offsets[u]; depth++;
                    } else if (on_stack[u] && indices[u] < lowlink[v]) {
                        lowlink[v] = indices[u];
                    }
                    continue;
                }
                if (lowlink[v] == indices[v]) {
                    size_t member;
                    do {
                        member = stack[--stack_count];
                        on_stack[member] = 0;
                        component[member] = component_count;
                    } while (member != v);
                    component_count++;
                }
                depth--;
                if (depth && lowlink[v] < lowlink[frames[depth - 1]])
                    lowlink[frames[depth - 1]] = lowlink[v];
            }
        }
    }

    /* Members and first (lowest-named) member of every component. */
    first = (size_t *)malloc(component_count * sizeof(*first));
    member_offsets = (size_t *)calloc(component_count + 1, sizeof(*member_offsets));
    members = (size_t *)malloc(count * sizeof(*members));
    cursor = (size_t *)malloc(component_count * sizeof(*cursor));
    member_indegree = (size_t *)calloc(count, sizeof(*member_indegree));
    member_slots = (size_t *)malloc(count * sizeof(*member_slots));
    heap_slots = (size_t *)malloc(component_count * sizeof(*heap_slots));
    component_indegree = (size_t *)calloc(component_count, sizeof(*component_indegree));
    component_pairs = (decl_pair *)malloc((w.edge_count ? w.edge_count : 1) * sizeof(*component_pairs));
    if (!first || !member_offsets || !members || !cursor || !member_indegree || !member_slots ||
        !heap_slots || !component_indegree || !component_pairs) goto done;
    for (i = 0; i < component_count; i++) first[i] = SIZE_MAX;
    for (i = 0; i < count; i++) {
        size_t c = component[i];
        member_offsets[c + 1]++;
        if (first[c] == SIZE_MAX || decl_name_compare(items, i, first[c]) < 0) first[c] = i;
    }
    for (i = 0; i < component_count; i++) {
        member_offsets[i + 1] += member_offsets[i];
        cursor[i] = member_offsets[i];
    }
    for (i = 0; i < count; i++) members[cursor[component[i]]++] = i;
    for (i = 0; i < component_count; i++) {
        size_t member_count = member_offsets[i + 1] - member_offsets[i];
        if (member_count > 1) cycle_count += member_count;
    }
    /* A self-inherit never reaches indegree zero and refuses below. */
    for (i = 0; i < w.inherit_count; i++)
        if (component[w.inherits[i].from] == component[w.inherits[i].to]) member_indegree[w.inherits[i].to]++;

    /* Condensed edges and their indegrees. */
    for (i = 0; i < w.edge_count; i++) {
        size_t from = component[w.edges[i].from], to = component[w.edges[i].to];
        if (from == to) continue;
        component_pairs[component_pair_count].from = from;
        component_pairs[component_pair_count].to = to;
        component_pair_count++;
    }
    component_pair_count = decl_pair_unique(component_pairs, component_pair_count);
    if (!decl_adjacency(component_pairs, component_pair_count, component_count,
                        &component_offsets, &component_targets)) goto done;
    for (i = 0; i < component_pair_count; i++) component_indegree[component_pairs[i].to]++;

    {
        decl_heap ready = {heap_slots, 0, items, first};
        for (i = 0; i < component_count; i++)
            if (!component_indegree[i]) decl_heap_push(&ready, i);
        while (ready.count) {
            size_t selected = decl_heap_pop(&ready), emitted = 0, k;
            decl_heap order = {member_slots, 0, items, NULL};
            for (k = member_offsets[selected]; k < member_offsets[selected + 1]; k++)
                if (!member_indegree[members[k]]) decl_heap_push(&order, members[k]);
            while (order.count) {
                size_t member = decl_heap_pop(&order);
                ordered[ordered_count++] = items[member];
                emitted++;
                for (k = inherit_offsets[member]; k < inherit_offsets[member + 1]; k++) {
                    size_t dependent = inherit_targets[k];
                    if (component[dependent] != selected || !member_indegree[dependent]) continue;
                    if (!--member_indegree[dependent]) decl_heap_push(&order, dependent);
                }
            }
            /* A cycle in explicit inheritance is unsafe: there is no valid
             * parent-before-child order, so refuse the whole snapshot. */
            if (emitted != member_offsets[selected + 1] - member_offsets[selected]) goto done;
            for (k = component_offsets[selected]; k < component_offsets[selected + 1]; k++)
                if (!--component_indegree[component_targets[k]]) decl_heap_push(&ready, component_targets[k]);
        }
    }
    if (ordered_count != count) goto done;

    memcpy(items, ordered, count * sizeof(items[0]));
    if (out_edge_count) *out_edge_count = w.edge_count;
    if (out_cycle_count) *out_cycle_count = cycle_count;
    ok = 1;

done:
    free(w.bucket_head); free(w.bucket_next); free(w.edges); free(w.inherits);
    free(edge_offsets); free(edge_targets); free(inherit_offsets); free(inherit_targets);
    free(indices); free(lowlink); free(stack); free(frames); free(positions); free(on_stack);
    free(component); free(first); free(member_offsets); free(members); free(cursor);
    free(component_offsets); free(component_targets); free(component_indegree);
    free(component_pairs); free(member_indegree); free(heap_slots); free(member_slots);
    free(ordered);
    return ok;
}

/* The effect Parse on both renderers resets its items and ends in the shared
 * reflected reader, which ignores its braced section's end-of-file result.
 * Stock fists_1st and a coop candle effect omit their final brace. */
int sh_decl_text_open_body_family(const char *type)
{
    return type && !strcmp(type, "fx");
}

int sh_decl_text_well_formed(const unsigned char *text, size_t length)
{
    size_t i;
    long depth = 0;
    int seen_brace = 0;
    int in_quote = 0;
    int line_comment = 0;
    int block_comment = 0;

    if (!text || length == 0) return 0;
    for (i = 0; i < length; i++) {
        unsigned char c = text[i];
        unsigned char next = i + 1 < length ? text[i + 1] : 0;

        if (c == 0) return 0;
        if (line_comment) {
            if (c == '\n') line_comment = 0;
            continue;
        }
        if (block_comment) {
            if (c == '*' && next == '/') {
                block_comment = 0;
                i++;
            }
            continue;
        }
        if (in_quote) {
            if (c == '\n' || c == '\r') return 0;
            if (c == '"') in_quote = 0;
            continue;
        }
        if (c == '/' && next == '/') {
            line_comment = 1;
            i++;
        } else if (c == '/' && next == '*') {
            block_comment = 1;
            i++;
        } else if (c == '"') {
            in_quote = 1;
        } else if (c == '{') {
            depth++;
            seen_brace = 1;
        } else if (c == '}') {
            if (depth == 0) return 0;
            depth--;
        }
    }
    return seen_brace && depth == 0 && !in_quote && !block_comment;
}

enum sedef_lex_token_kind {
    SEDEF_LEX_END = 0,
    SEDEF_LEX_IDENTIFIER,
    SEDEF_LEX_DOT,
    SEDEF_LEX_EQUALS,
    SEDEF_LEX_OPEN_BRACE,
    SEDEF_LEX_CLOSE_BRACE,
    SEDEF_LEX_OTHER
};

typedef struct sedef_lex_token {
    int kind;
    size_t start;
    size_t length;
} sedef_lex_token;

/* Return one lexical token and advance *offset. Comments are whitespace;
 * quoted strings are opaque tokens so decoy key text inside them cannot be
 * mistaken for a declaration assignment. The caller has already required
 * sh_decl_text_well_formed(), so the bounded skips below cannot run past a
 * malformed unterminated construct. */
static int sedef_next_lex_token(const unsigned char *text, size_t length,
                                size_t *offset, sedef_lex_token *token)
{
    size_t i;

    if (!text || !offset || !token) return 0;
    i = *offset;
    for (;;) {
        unsigned char c;
        unsigned char next;
        if (i >= length) {
            token->kind = SEDEF_LEX_END;
            token->start = i;
            token->length = 0;
            *offset = i;
            return 1;
        }
        c = text[i];
        next = i + 1 < length ? text[i + 1] : 0;
        if (is_decl_space(c)) {
            i++;
            continue;
        }
        if (c == '/' && next == '/') {
            i += 2;
            while (i < length && text[i] != '\n') i++;
            continue;
        }
        if (c == '/' && next == '*') {
            i += 2;
            while (i + 1 < length && !(text[i] == '*' && text[i + 1] == '/')) i++;
            if (i + 1 < length) i += 2;
            continue;
        }
        if (c == '"') {
            size_t start = i++;
            while (i < length) {
                c = text[i++];
                if (c == '"') break;
            }
            token->kind = SEDEF_LEX_OTHER;
            token->start = start;
            token->length = i - start;
            *offset = i;
            return 1;
        }
        token->start = i;
        token->length = 1;
        if (is_decl_key_char(c)) {
            i++;
            while (i < length && is_decl_key_char(text[i])) i++;
            token->kind = SEDEF_LEX_IDENTIFIER;
            token->length = i - token->start;
        } else if (c == '.') {
            token->kind = SEDEF_LEX_DOT;
            i++;
        } else if (c == '=') {
            token->kind = SEDEF_LEX_EQUALS;
            i++;
        } else if (c == '{') {
            token->kind = SEDEF_LEX_OPEN_BRACE;
            i++;
        } else if (c == '}') {
            token->kind = SEDEF_LEX_CLOSE_BRACE;
            i++;
        } else {
            token->kind = SEDEF_LEX_OTHER;
            i++;
        }
        *offset = i;
        return 1;
    }
}

static int sedef_lex_token_is(const unsigned char *text,
                              const sedef_lex_token *token,
                              const char *name)
{
    return token && token->kind == SEDEF_LEX_IDENTIFIER &&
           ascii_equal_span(text + token->start, token->length, name);
}

int sh_decl_text_sedef_has_materializable_source(const unsigned char *text,
                                                 size_t length)
{
    size_t offset = 0;
    int brace_depth = 0;
    int edit_block_depth = 0;
    sedef_lex_token token;

    if (!sh_decl_text_well_formed(text, length)) return 0;
    while (sedef_next_lex_token(text, length, &offset, &token) &&
           token.kind != SEDEF_LEX_END) {
        if (token.kind == SEDEF_LEX_OPEN_BRACE) {
            brace_depth++;
            continue;
        }
        if (token.kind == SEDEF_LEX_CLOSE_BRACE) {
            if (brace_depth == edit_block_depth) edit_block_depth = 0;
            if (brace_depth > 0) brace_depth--;
            continue;
        }
        if (token.kind != SEDEF_LEX_IDENTIFIER) continue;

        if (brace_depth == 1 && sedef_lex_token_is(text, &token, "inherit")) {
            size_t probe = offset;
            sedef_lex_token next;
            if (sedef_next_lex_token(text, length, &probe, &next) &&
                next.kind == SEDEF_LEX_EQUALS)
                return 1;
        }

        if (brace_depth == 1 && sedef_lex_token_is(text, &token, "edit")) {
            size_t probe = offset;
            sedef_lex_token next;
            if (!sedef_next_lex_token(text, length, &probe, &next)) continue;
            if (next.kind == SEDEF_LEX_DOT) {
                if (!sedef_next_lex_token(text, length, &probe, &next) ||
                    !sedef_lex_token_is(text, &next, "entityDef") ||
                    !sedef_next_lex_token(text, length, &probe, &next))
                    continue;
                if (next.kind == SEDEF_LEX_EQUALS) return 1;
            } else if (next.kind == SEDEF_LEX_EQUALS) {
                if (sedef_next_lex_token(text, length, &probe, &next) &&
                    next.kind == SEDEF_LEX_OPEN_BRACE)
                    edit_block_depth = brace_depth + 1;
            }
            continue;
        }

        if (edit_block_depth > 0 && brace_depth == edit_block_depth &&
            sedef_lex_token_is(text, &token, "entityDef")) {
            size_t probe = offset;
            sedef_lex_token next;
            if (sedef_next_lex_token(text, length, &probe, &next) &&
                next.kind == SEDEF_LEX_EQUALS)
                return 1;
        }
    }
    return 0;
}

static int sedef_lex_string_view(const unsigned char *text,
                                 const sedef_lex_token *token,
                                 const unsigned char **name,
                                 size_t *name_length)
{
    if (!text || !token || !name || !name_length || token->kind != SEDEF_LEX_OTHER ||
        token->length < 2 || text[token->start] != '"' ||
        text[token->start + token->length - 1] != '"')
        return 0;
    *name = text + token->start + 1;
    *name_length = token->length - 2;
    return *name_length != 0;
}

static int sedef_probe_assignment_string(const unsigned char *text,
                                         size_t length, size_t offset,
                                         const unsigned char **name,
                                         size_t *name_length)
{
    sedef_lex_token token;

    if (!sedef_next_lex_token(text, length, &offset, &token) ||
        token.kind != SEDEF_LEX_EQUALS ||
        !sedef_next_lex_token(text, length, &offset, &token))
        return 0;
    return sedef_lex_string_view(text, &token, name, name_length);
}

/* `item[0] = "name"` is the only quoted value admitted from the
 * buildGameRefEntityDefs list. Skip the bounded index punctuation, but stop
 * at a brace so a malformed field cannot borrow a string from a sibling. */
static int sedef_probe_item_string(const unsigned char *text, size_t length,
                                   size_t offset,
                                   const unsigned char **name,
                                   size_t *name_length)
{
    sedef_lex_token token;
    int saw_equals = 0;

    while (sedef_next_lex_token(text, length, &offset, &token) &&
           token.kind != SEDEF_LEX_END) {
        if (token.kind == SEDEF_LEX_OPEN_BRACE ||
            token.kind == SEDEF_LEX_CLOSE_BRACE)
            return 0;
        if (token.kind == SEDEF_LEX_EQUALS) {
            saw_equals = 1;
            continue;
        }
        if (!saw_equals) continue;
        return sedef_lex_string_view(text, &token, name, name_length);
    }
    return 0;
}

static int sedef_emit_dependency(sh_decl_text_dependency_fn callback,
                                 const char *type,
                                 const unsigned char *name,
                                 size_t name_length,
                                 void *context)
{
    if (!callback || !type || !name || name_length == 0) return 0;
    return callback(type, name, name_length, context) ? 1 : 0;
}

int sh_decl_text_collect_sedef_dependencies(const unsigned char *text,
                                            size_t length,
                                            sh_decl_text_dependency_fn callback,
                                            void *context)
{
    size_t offset = 0;
    int brace_depth = 0;
    int edit_block_depth = 0;
    int build_block_depth = 0;
    sedef_lex_token token;

    if (!callback || !sh_decl_text_well_formed(text, length)) return 0;
    while (sedef_next_lex_token(text, length, &offset, &token) &&
           token.kind != SEDEF_LEX_END) {
        if (token.kind == SEDEF_LEX_OPEN_BRACE) {
            brace_depth++;
            continue;
        }
        if (token.kind == SEDEF_LEX_CLOSE_BRACE) {
            if (brace_depth == build_block_depth) build_block_depth = 0;
            if (brace_depth == edit_block_depth) edit_block_depth = 0;
            if (brace_depth > 0) brace_depth--;
            continue;
        }
        if (token.kind != SEDEF_LEX_IDENTIFIER) continue;

        if (brace_depth == 1 && sedef_lex_token_is(text, &token, "inherit")) {
            const unsigned char *name;
            size_t name_length;
            if (sedef_probe_assignment_string(text, length, offset,
                                               &name, &name_length) &&
                !sedef_emit_dependency(callback, "snapEditorEntityDef", name,
                                        name_length, context))
                return 0;
        }

        if (brace_depth == 1 && sedef_lex_token_is(text, &token, "edit")) {
            size_t probe = offset;
            sedef_lex_token next;
            if (sedef_next_lex_token(text, length, &probe, &next) &&
                next.kind == SEDEF_LEX_DOT) {
            if (sedef_next_lex_token(text, length, &probe, &next) &&
                    sedef_lex_token_is(text, &next, "entityDef")) {
                    const unsigned char *name;
                    size_t name_length;
                    if (!sedef_probe_assignment_string(text, length, probe,
                                                       &name, &name_length) ||
                        !sedef_emit_dependency(callback, "entityDef", name,
                                                name_length, context))
                        return 0;
                } else if (sedef_lex_token_is(text, &next,
                                              "buildGameRefEntityDefs")) {
                    if (sedef_next_lex_token(text, length, &probe, &next) &&
                        next.kind == SEDEF_LEX_EQUALS &&
                        sedef_next_lex_token(text, length, &probe, &next) &&
                        next.kind == SEDEF_LEX_OPEN_BRACE)
                        build_block_depth = brace_depth + 1;
                }
            } else if (next.kind == SEDEF_LEX_EQUALS &&
                       sedef_next_lex_token(text, length, &probe, &next) &&
                       next.kind == SEDEF_LEX_OPEN_BRACE) {
                edit_block_depth = brace_depth + 1;
            }
        }

        if (edit_block_depth != 0 && brace_depth == edit_block_depth) {
            if (sedef_lex_token_is(text, &token, "entityDef")) {
                const unsigned char *name;
                size_t name_length;
                if (sedef_probe_assignment_string(text, length, offset,
                                                   &name, &name_length) &&
                    !sedef_emit_dependency(callback, "entityDef", name,
                                            name_length, context))
                    return 0;
            } else if (sedef_lex_token_is(text, &token,
                                          "buildGameRefEntityDefs")) {
                size_t probe = offset;
                sedef_lex_token next;
                if (sedef_next_lex_token(text, length, &probe, &next) &&
                    next.kind == SEDEF_LEX_EQUALS &&
                    sedef_next_lex_token(text, length, &probe, &next) &&
                    next.kind == SEDEF_LEX_OPEN_BRACE)
                    build_block_depth = brace_depth + 1;
            }
        }

        if (build_block_depth != 0 && brace_depth == build_block_depth &&
            sedef_lex_token_is(text, &token, "item")) {
            const unsigned char *name;
            size_t name_length;
            if (sedef_probe_item_string(text, length, offset,
                                        &name, &name_length) &&
                !sedef_emit_dependency(callback, "entityDef", name,
                                        name_length, context))
                return 0;
        }
    }
    return 1;
}

int sh_decl_text_collect_entitydef_dependencies(const unsigned char *text,
                                                size_t length,
                                                sh_decl_text_dependency_fn callback,
                                                void *context)
{
    size_t offset = 0;
    int brace_depth = 0;
    sedef_lex_token token;

    if (!callback || !sh_decl_text_well_formed(text, length)) return 0;
    while (sedef_next_lex_token(text, length, &offset, &token) &&
           token.kind != SEDEF_LEX_END) {
        if (token.kind == SEDEF_LEX_OPEN_BRACE) {
            brace_depth++;
            continue;
        }
        if (token.kind == SEDEF_LEX_CLOSE_BRACE) {
            if (brace_depth > 0) brace_depth--;
            continue;
        }
        if (brace_depth == 1 && token.kind == SEDEF_LEX_IDENTIFIER &&
            sedef_lex_token_is(text, &token, "inherit")) {
            const unsigned char *name;
            size_t name_length;
            if (sedef_probe_assignment_string(text, length, offset,
                                               &name, &name_length) &&
                !sedef_emit_dependency(callback, "entityDef", name,
                                        name_length, context))
                return 0;
        }
    }
    return 1;
}
