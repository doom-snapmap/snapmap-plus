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

static void add_quoted_reference(const sh_decl_reference_item *items, size_t count,
                                 size_t dependent, const unsigned char *text,
                                 size_t length, unsigned char *edges,
                                 unsigned char *inherit_edges,
                                 size_t *indegree, size_t *edge_count,
                                 int is_inherit)
{
    size_t i;
    size_t match = count;
    size_t matches = 0;
    for (i = 0; i < count; i++) {
        if (!ascii_equal_span(text, length, items[i].name)) continue;
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
            int escaped = 0;
            while (i < length && text[i] != '"') {
                if (text[i] == '\\') {
                    escaped = 1;
                    if (i + 1 < length) i += 2;
                    else i++;
                } else {
                    i++;
                }
            }
            if (i < length && !escaped)
                add_quoted_reference(items, count, dependent, text + start,
                                     i - start, edges, inherit_edges, indegree,
                                     edge_count, quote_is_inherit_key(text, quote_start));
            if (i < length) i++;
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
            if (component[i] != component_id || member_emitted[i] ||
                member_indegree[i] != 0) continue;
            if (selected == count || decl_name_compare(items, i, selected) < 0)
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

int sh_decl_text_order_by_references(sh_decl_reference_item *items, size_t count,
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

int sh_decl_text_well_formed(const unsigned char *text, size_t length)
{
    size_t i;
    long depth = 0;
    int seen_brace = 0;
    int in_quote = 0;
    int escaped = 0;
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
            if (escaped) {
                escaped = 0;
            } else if (c == '\\') {
                escaped = 1;
            } else if (c == '"') {
                in_quote = 0;
            }
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
    return seen_brace && depth == 0 && !in_quote && !escaped && !block_comment;
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
            int escaped = 0;
            while (i < length) {
                c = text[i++];
                if (escaped) escaped = 0;
                else if (c == '\\') escaped = 1;
                else if (c == '"') break;
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
