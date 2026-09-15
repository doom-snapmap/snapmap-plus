#include "decl_tree.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct dt_frame {
    sh_decl_node **tail, **keys;
    size_t count, capacity;
} dt_frame;

typedef struct dt_parser {
    const char *text;
    size_t length, offset;
    dt_frame *stack;
    size_t depth, capacity;
    char *error;
    size_t error_capacity;
    int failed;
} dt_parser;

static int dt_fail(dt_parser *p, const char *message)
{
    if (!p->failed && p->error && p->error_capacity)
        snprintf(p->error, p->error_capacity, "declaration byte %zu: %s", p->offset, message);
    p->failed = 1;
    return 0;
}

static char *dt_copy(const char *text, size_t length)
{
    char *copy;
    if (length == SIZE_MAX) return NULL;
    copy = (char *)malloc(length + 1);
    if (copy) { memcpy(copy, text, length); copy[length] = 0; }
    return copy;
}

void sh_decl_tree_free(sh_decl_node *node)
{
    while (node) {
        sh_decl_node *next = node->next;
        if (node->children) {
            sh_decl_node *last = node->children;
            while (last->next) last = last->next;
            last->next = next;
            next = node->children;
        }
        free(node->key); free(node->value); free(node);
        node = next;
    }
}

const sh_decl_node *sh_decl_tree_member(const sh_decl_node *parent, const char *key)
{
    const sh_decl_node *node;
    if (!key) return NULL;
    for (node = parent ? parent->children : NULL; node; node = node->next)
        if (!strcmp(node->key, key)) return node;
    return NULL;
}

static void dt_space(dt_parser *p)
{
    for (;;) {
        while (p->offset < p->length && isspace((unsigned char)p->text[p->offset])) p->offset++;
        if (p->length - p->offset < 2 || p->text[p->offset] != '/') return;
        if (p->text[p->offset + 1] == '/') {
            p->offset += 2;
            while (p->offset < p->length && p->text[p->offset] != '\n') p->offset++;
        } else if (p->text[p->offset + 1] == '*') {
            p->offset += 2;
            while (p->length - p->offset >= 2 &&
                   (p->text[p->offset] != '*' || p->text[p->offset + 1] != '/')) p->offset++;
            if (p->length - p->offset < 2) { dt_fail(p, "unterminated comment"); return; }
            p->offset += 2;
        } else return;
    }
}

static int dt_take(dt_parser *p, char expected)
{
    dt_space(p);
    if (p->failed || p->offset == p->length || p->text[p->offset] != expected) return 0;
    p->offset++;
    return 1;
}

static char *dt_key(dt_parser *p)
{
    size_t start;
    dt_space(p); start = p->offset;
    while (p->offset < p->length) {
        unsigned char c = (unsigned char)p->text[p->offset];
        if (!(isalnum(c) || c == '_' || c == '.' || c == '[' || c == ']' || c == '-')) break;
        p->offset++;
    }
    return start == p->offset ? NULL : dt_copy(p->text + start, p->offset - start);
}

static char *dt_value(dt_parser *p)
{
    size_t start, end, parentheses = 0;
    int quoted = 0, escaped = 0;
    dt_space(p); start = p->offset;
    while (p->offset < p->length) {
        unsigned char c = (unsigned char)p->text[p->offset++];
        if (quoted) {
            if (escaped) { escaped = 0; continue; }
            if (c == '\\') escaped = 1;
            else if (c == '"') quoted = 0;
            else if (c < 32) return NULL;
            continue;
        }
        if (c == '"') { quoted = 1; continue; }
        if (c == '(') parentheses++;
        else if (c == ')') { if (!parentheses) return NULL; parentheses--; }
        else if (c == '{' || c == '}' || c == '=' || (c < 32 && !isspace(c))) return NULL;
        else if (c == '/' && p->offset < p->length &&
                 (p->text[p->offset] == '/' || p->text[p->offset] == '*')) return NULL;
        else if (c == ';') {
            if (parentheses) return NULL;
            end = p->offset - 1;
            while (end > start && isspace((unsigned char)p->text[end - 1])) end--;
            return end == start ? NULL : dt_copy(p->text + start, end - start);
        }
    }
    return NULL;
}

static size_t dt_hash(const char *key)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    while (*key) { hash ^= (unsigned char)*key++; hash *= UINT64_C(1099511628211); }
    return (size_t)hash;
}

static int dt_insert(dt_parser *p, sh_decl_node *node, int unique_fields)
{
    dt_frame *f = &p->stack[p->depth - 1];
    size_t slot;
    if (!unique_fields) {
        *f->tail = node; f->tail = &node->next;
        return 1;
    }
    if (f->count >= f->capacity / 2) {
        size_t capacity = f->capacity ? f->capacity * 2 : 16, i;
        sh_decl_node **keys;
        if (capacity < f->capacity || capacity > SIZE_MAX / sizeof(*keys)) return dt_fail(p, "allocation overflow");
        keys = (sh_decl_node **)calloc(capacity, sizeof(*keys));
        if (!keys) return dt_fail(p, "allocation failed");
        for (i = 0; i < f->capacity; i++) if (f->keys[i]) {
            slot = dt_hash(f->keys[i]->key) & (capacity - 1);
            while (keys[slot]) slot = (slot + 1) & (capacity - 1);
            keys[slot] = f->keys[i];
        }
        free(f->keys); f->keys = keys; f->capacity = capacity;
    }
    slot = dt_hash(node->key) & (f->capacity - 1);
    while (f->keys[slot]) {
        if (!strcmp(f->keys[slot]->key, node->key)) return dt_fail(p, "duplicate field");
        slot = (slot + 1) & (f->capacity - 1);
    }
    f->keys[slot] = node; f->count++;
    *f->tail = node; f->tail = &node->next;
    return 1;
}

static int dt_push(dt_parser *p, sh_decl_node *node)
{
    if (p->depth == p->capacity) {
        size_t capacity = p->capacity ? p->capacity * 2 : 16;
        dt_frame *stack;
        if (capacity < p->capacity || capacity > SIZE_MAX / sizeof(*stack)) return dt_fail(p, "allocation overflow");
        stack = (dt_frame *)realloc(p->stack, capacity * sizeof(*stack));
        if (!stack) return dt_fail(p, "allocation failed");
        p->stack = stack; p->capacity = capacity;
    }
    memset(&p->stack[p->depth], 0, sizeof(*p->stack));
    p->stack[p->depth++].tail = &node->children;
    node->compound = 1;
    return 1;
}

static sh_decl_node *dt_parse(sh_decl_source source, int unique_fields, char *error, size_t error_capacity)
{
    dt_parser p = {0};
    sh_decl_node *root = NULL;
    if (error && error_capacity) error[0] = 0;
    p.text = source.text; p.length = source.length; p.error = error; p.error_capacity = error_capacity;
    if (!source.text || !source.length || memchr(source.text, 0, source.length)) {
        dt_fail(&p, "empty input or embedded NUL"); goto done;
    }
    if (!dt_take(&p, '{')) { dt_fail(&p, "expected opening brace"); goto done; }
    root = (sh_decl_node *)calloc(1, sizeof(*root));
    if (!root || !dt_push(&p, root)) { dt_fail(&p, "allocation failed"); goto done; }
    while (p.depth && !p.failed) {
        sh_decl_node *node;
        if (dt_take(&p, '}')) {
            free(p.stack[--p.depth].keys);
            if (p.depth) (void)dt_take(&p, ';');
            continue;
        }
        node = (sh_decl_node *)calloc(1, sizeof(*node));
        if (!node) { dt_fail(&p, "allocation failed"); break; }
        node->key = dt_key(&p);
        if (!node->key || !dt_insert(&p, node, unique_fields)) {
            sh_decl_tree_free(node); dt_fail(&p, "expected field or closing brace"); break;
        }
        node->assignment = dt_take(&p, '=');
        node->reset = dt_take(&p, '!');
        if (dt_take(&p, '{')) {
            if (!dt_push(&p, node)) break;
        } else if (!node->assignment || node->reset || !(node->value = dt_value(&p))) {
            dt_fail(&p, "unsupported or malformed scalar"); break;
        }
    }
    dt_space(&p);
    if (p.offset != p.length) dt_fail(&p, "unexpected trailing text");
done:
    while (p.depth) free(p.stack[--p.depth].keys);
    free(p.stack);
    if (p.failed) { sh_decl_tree_free(root); return NULL; }
    return root;
}

sh_decl_node *sh_decl_tree_parse(sh_decl_source source, char *error, size_t error_capacity)
{ return dt_parse(source, 1, error, error_capacity); }

sh_decl_node *sh_decl_tree_parse_ordered(sh_decl_source source, char *error, size_t error_capacity)
{ return dt_parse(source, 0, error, error_capacity); }

typedef struct dt_output { char *text; size_t length, capacity; } dt_output;
static int dt_append(dt_output *out, const char *text)
{
    size_t length = strlen(text), needed, capacity;
    char *grown;
    if (out->length == SIZE_MAX || length > SIZE_MAX - out->length - 1) return 0;
    needed = out->length + length + 1;
    if (needed > out->capacity) {
        capacity = out->capacity ? out->capacity : 256;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        grown = (char *)realloc(out->text, capacity);
        if (!grown) return 0;
        out->text = grown; out->capacity = capacity;
    }
    memcpy(out->text + out->length, text, length + 1); out->length += length;
    return 1;
}

char *sh_decl_tree_write(const sh_decl_node *root, size_t *length)
{
    const sh_decl_node **stack = NULL, *node;
    size_t depth = 0, capacity = 0;
    dt_output out = {0};
    int ok = 0;
    if (length) *length = 0;
    if (!root || root->key || !root->compound || root->next || !length || !dt_append(&out, "{ ")) goto done;
    node = root->children;
    for (;;) {
        if (!node) {
            if (!dt_append(&out, "} ")) goto done;
            if (!depth) break;
            node = stack[--depth]; continue;
        }
        if (!node->key || !dt_append(&out, node->key) ||
            !dt_append(&out, node->assignment ? " = " : " ") ||
            (node->reset && !dt_append(&out, "! "))) goto done;
        if (!node->compound) {
            if (!node->assignment || node->reset || !node->value ||
                !dt_append(&out, node->value) || !dt_append(&out, "; ")) goto done;
            node = node->next; continue;
        }
        if (!dt_append(&out, "{ ")) goto done;
        if (depth == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            const sh_decl_node **grown;
            if (next < capacity || next > SIZE_MAX / sizeof(*stack)) goto done;
            grown = (const sh_decl_node **)realloc(stack, next * sizeof(*stack));
            if (!grown) goto done;
            stack = grown; capacity = next;
        }
        stack[depth++] = node->next; node = node->children;
    }
    if (!dt_append(&out, "\n")) goto done;
    *length = out.length; ok = 1;
done:
    free(stack);
    if (!ok) { free(out.text); return NULL; }
    return out.text;
}

int sh_decl_tree_literal(const sh_decl_node *node, const char **text, size_t *length)
{
    const char *value;
    size_t size, i;
    if (text) *text = NULL;
    if (length) *length = 0;
    if (!node || node->compound || !node->value || !text || !length) return 0;
    value = node->value; size = strlen(value);
    if (!size) return 0;
    if (*value == '"') {
        if (size < 2 || value[size - 1] != '"') return 0;
        value++; size -= 2;
        for (i = 0; i < size; i++) if (value[i] == '"' || value[i] == '\\') return 0;
    } else {
        for (i = 0; i < size; i++) {
            unsigned char c = (unsigned char)value[i];
            if (!(isalnum(c) || strchr("_/.:$-", c))) return 0;
        }
    }
    *text = value; *length = size;
    return 1;
}
