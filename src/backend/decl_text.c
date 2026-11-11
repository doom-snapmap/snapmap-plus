/* decl_text.c -- see decl_text.h. Pure C; no engine or filesystem dependency. */
#include "decl_text.h"

#include <stdlib.h>
#include <string.h>

static int is_decl_space(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
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

/* ---- own state of a placed entity: its prepared text less what its inherit says ----
 *
 * The engine prepares a placed entity's text as its inherit chain merged root
 * first, then its own members. Reading a saved entity drops every own leaf
 * whose key and value equal its inherit's prepared text, so the entity's own
 * state is that prepared text with those leaves removed, and with the blocks
 * left empty by it. This applies the same rule to show and save an entity's
 * own decl text. */

typedef struct strip_node {
    const unsigned char *key;
    size_t key_length;
    const unsigned char *value;   /* trimmed leaf value; NULL for a block */
    size_t value_length;
    int first_child;              /* -1 when none */
    int next;                     /* next sibling, -1 when last */
    int keep;
} strip_node;

typedef struct strip_tree {
    strip_node *nodes;
    int count;
    int capacity;
} strip_tree;

static size_t strip_skip_space(const unsigned char *t, size_t n, size_t i)
{
    for (;;) {
        while (i < n && is_decl_space(t[i])) i++;
        if (i + 1 < n && t[i] == '/' && t[i + 1] == '/') {
            while (i < n && t[i] != '\n') i++;
            continue;
        }
        if (i + 1 < n && t[i] == '/' && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(t[i] == '*' && t[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            continue;
        }
        return i;
    }
}

/* Parse statements until the closing brace (depth > 0) or the end (depth 0).
 * The list's first node index (-1 when empty) goes to *first. Returns 0 on a
 * malformed or truncated text. */
static int strip_parse_list(strip_tree *tree, const unsigned char *t, size_t n,
                            size_t *pos, int depth, int *first)
{
    int prev = -1;
    size_t i = *pos;
    *first = -1;
    if (depth > 64) return 0;
    for (;;) {
        i = strip_skip_space(t, n, i);
        if (i >= n) {
            if (depth > 0) return 0;              /* an unclosed block: truncated text */
            break;
        }
        if (t[i] == '}') {
            if (depth == 0) return 0;
            i++;
            break;
        }
        if (t[i] == ';') { i++; continue; }
        size_t key_start = i;
        while (i < n && !is_decl_space(t[i]) && t[i] != '=' && t[i] != '{' &&
               t[i] != '}' && t[i] != ';' && t[i] != '"') i++;
        if (i == key_start) return 0;
        if (tree->count >= tree->capacity) return 0;
        int index = tree->count++;
        strip_node *node = &tree->nodes[index];
        node->key = t + key_start;
        node->key_length = i - key_start;
        node->value = NULL;
        node->value_length = 0;
        node->first_child = -1;
        node->next = -1;
        node->keep = 0;
        i = strip_skip_space(t, n, i);
        if (i < n && t[i] == '=') i = strip_skip_space(t, n, i + 1);
        if (i < n && t[i] == '{') {
            int child;
            i++;
            if (!strip_parse_list(tree, t, n, &i, depth + 1, &child)) return 0;
            tree->nodes[index].first_child = child;
        } else {
            size_t value_start = i;
            int quote = 0, paren = 0;
            while (i < n) {
                unsigned char c = t[i];
                if (quote) {
                    if (c == '\\' && i + 1 < n) { i += 2; continue; }
                    if (c == '"') quote = 0;
                } else if (c == '"') quote = 1;
                else if (c == '(') paren++;
                else if (c == ')') { if (paren > 0) paren--; }
                else if (paren == 0 && (c == ';' || c == '}' || c == '{')) break;
                i++;
            }
            if (quote || paren) return 0;
            if (i < n && t[i] == '{') return 0;
            size_t value_end = i;
            while (value_end > value_start && is_decl_space(t[value_end - 1])) value_end--;
            if (value_end == value_start) return 0;
            tree->nodes[index].value = t + value_start;
            tree->nodes[index].value_length = value_end - value_start;
            if (i < n && t[i] == ';') i++;
        }
        if (prev < 0) *first = index; else tree->nodes[prev].next = index;
        prev = index;
    }
    *pos = i;
    return 1;
}

static int strip_parse(strip_tree *tree, const unsigned char *t, size_t n, int *first)
{
    size_t pos = 0;
    tree->capacity = (int)(n / 2 + 2);
    tree->count = 0;
    tree->nodes = (strip_node *)malloc(sizeof(strip_node) * (size_t)tree->capacity);
    if (!tree->nodes) return 0;
    return strip_parse_list(tree, t, n, &pos, 0, first);
}

static int strip_same(const unsigned char *a, size_t al, const unsigned char *b, size_t bl)
{
    return al == bl && (al == 0 || memcmp(a, b, al) == 0);
}

/* Mark the own nodes that differ from the parent list, comparing each with the
 * first parent member of the same key. Returns 1 when any node of this list is
 * kept; a block is kept only through a kept member. */
static int strip_mark(strip_tree *own, int list, const strip_tree *parent, int parent_list)
{
    int any = 0;
    for (int i = list; i >= 0; i = own->nodes[i].next) {
        strip_node *node = &own->nodes[i];
        int match = -1;
        for (int p = parent_list; p >= 0; p = parent->nodes[p].next)
            if (strip_same(parent->nodes[p].key, parent->nodes[p].key_length,
                           node->key, node->key_length)) { match = p; break; }
        if (node->value) {
            node->keep = !(match >= 0 && parent->nodes[match].value &&
                           strip_same(parent->nodes[match].value, parent->nodes[match].value_length,
                                      node->value, node->value_length));
        } else {
            int sub = (match >= 0 && !parent->nodes[match].value) ? parent->nodes[match].first_child : -1;
            node->keep = strip_mark(own, node->first_child, parent, sub);
        }
        any |= node->keep;
    }
    return any;
}

typedef struct strip_out { char *buf; size_t cap, len; int overflow; } strip_out;

static void strip_put(strip_out *o, const void *s, size_t n)
{
    if (o->overflow) return;
    if (o->len + n + 1 > o->cap) { o->overflow = 1; return; }
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';
}

static void strip_emit(strip_out *o, const strip_tree *tree, int list, int depth)
{
    for (int i = list; i >= 0; i = tree->nodes[i].next) {
        const strip_node *node = &tree->nodes[i];
        if (!node->keep) continue;
        if (o->len) strip_put(o, "\n", 1);
        for (int d = 0; d < depth; d++) strip_put(o, "\t", 1);
        strip_put(o, node->key, node->key_length);
        if (node->value) {
            strip_put(o, " = ", 3);
            strip_put(o, node->value, node->value_length);
            strip_put(o, ";", 1);
        } else {
            strip_put(o, " = {", 4);
            strip_emit(o, tree, node->first_child, depth + 1);
            strip_put(o, "\n", 1);
            for (int d = 0; d < depth; d++) strip_put(o, "\t", 1);
            strip_put(o, "}", 1);
        }
    }
}

int sh_decl_text_own_state(const unsigned char *prepared, size_t prepared_length,
                           const unsigned char *inherited, size_t inherited_length,
                           char *out, size_t capacity, size_t *out_length)
{
    strip_tree own = {0}, parent = {0};
    int own_first = -1, parent_first = -1, ok = 0;
    if (out_length) *out_length = 0;
    if (out && capacity) out[0] = '\0';
    if (!prepared || !prepared_length || !out || capacity < 2) return 0;
    if (!strip_parse(&own, prepared, prepared_length, &own_first) || own_first < 0) goto done;
    if (inherited && inherited_length &&
        !strip_parse(&parent, inherited, inherited_length, &parent_first)) goto done;
    strip_mark(&own, own_first, &parent, parent_first);
    own.nodes[own_first].keep = 1;                  /* the first statement is always written */
    {
        strip_out o = { out, capacity, 0, 0 };
        out[0] = '\0';
        strip_emit(&o, &own, own_first, 0);
        if (o.overflow) { out[0] = '\0'; goto done; }
        if (out_length) *out_length = o.len;
        ok = 1;
    }
done:
    free(own.nodes);
    free(parent.nodes);
    return ok;
}
