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
        free(node->key); free(node->value); free(node->annotation); free(node);
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

int sh_decl_tree_equal(const sh_decl_node *a, const sh_decl_node *b, const char *unordered)
{
    typedef struct frame { const sh_decl_node *a, *b; int top, any_order; } frame;
    frame *stack = malloc(16 * sizeof(*stack));
    size_t count = 0, capacity = 16;
    int equal = 1;
    if (!stack) return -1;
    stack[count++] = (frame){a, b, 1, 0};
    while (count && equal) {
        frame current = stack[--count];
        const sh_decl_node *left, *right;
        if (current.a->compound != current.b->compound || current.a->reset != current.b->reset ||
            (!current.top && current.a->assignment != current.b->assignment) ||
            strcmp(current.a->annotation ? current.a->annotation : "", current.b->annotation ? current.b->annotation : "")) {
            equal = 0; break;
        }
        if (!current.a->compound) {
            equal = !strcmp(current.a->value ? current.a->value : "", current.b->value ? current.b->value : "");
            continue;
        }
        size_t lefts = 0, rights = 0;
        for (left = current.a->children; left; left = left->next) lefts++;
        for (right = current.b->children; right; right = right->next) rights++;
        if (lefts != rights) { equal = 0; break; }
        for (left = current.a->children, right = current.b->children; left && equal; left = left->next) {
            /* Keys are unique within a parsed block, so a by-key match is exact. */
            const sh_decl_node *other = current.any_order ? sh_decl_tree_member(current.b, left->key) : right;
            if (!current.any_order) right = right->next;
            if (!other || strcmp(left->key, other->key)) { equal = 0; break; }
            if (count == capacity) {
                frame *grown = capacity <= SIZE_MAX / 2 / sizeof(*stack) ? realloc(stack, capacity * 2 * sizeof(*stack)) : NULL;
                if (!grown) { free(stack); return -1; }
                stack = grown; capacity *= 2;
            }
            stack[count++] = (frame){left, other, 0, current.top && unordered && !strcmp(left->key, unordered)};
        }
    }
    free(stack); return equal;
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

/* Native saved declarations can prefix a key with type and ops tokens.
 * Keep those tokens as source spelling; they never become declaration paths.
 * A quoted field name is also accepted. The field schema decides its meaning. */
static char *dt_field(dt_parser *p, char **annotation)
{
    size_t begin, starts[3], ends[3], token_begin[3], count = 0;
    int indexed[3] = {0};
    dt_space(p); begin = p->offset;
    while (!p->failed && p->offset < p->length) {
        unsigned char c = (unsigned char)p->text[p->offset];
        if (c == '=' || c == '{' || c == '!' || c == '}' || c == ';') break;
        if (count == 3) return NULL;
        token_begin[count] = p->offset;
        if (c == '"') {
            starts[count] = ++p->offset;
            while (p->offset < p->length && p->text[p->offset] != '"') {
                c = (unsigned char)p->text[p->offset++];
                if (c < 32 || c == '\\') return NULL;
            }
            if (p->offset == p->length) return NULL;
            ends[count++] = p->offset++;
        } else {
            starts[count] = p->offset;
            if (c == '*') p->offset++;
            else {
                char *token = dt_key(p);
                if (!token) return NULL;
                size_t n = strlen(token);
                int open = n && token[n - 1] == '[';
                free(token);
                if (open) {
                    indexed[count] = 1;
                    /* The native lexer reads item[ 0 ] as separate tokens: an
                     * array index may be surrounded by blanks. */
                    size_t digits;
                    while (p->offset < p->length && (p->text[p->offset] == ' ' || p->text[p->offset] == '\t')) p->offset++;
                    digits = p->offset;
                    while (p->offset < p->length && isdigit((unsigned char)p->text[p->offset])) p->offset++;
                    if (digits == p->offset) return NULL;
                    while (p->offset < p->length && (p->text[p->offset] == ' ' || p->text[p->offset] == '\t')) p->offset++;
                    if (p->offset >= p->length || p->text[p->offset] != ']') return NULL;
                    p->offset++;
                }
            }
            ends[count++] = p->offset;
        }
        dt_space(p);
    }
    if (!count || p->failed) return NULL;
    count--;
    size_t key_begin = token_begin[count];
    char *key = dt_copy(p->text + starts[count], ends[count] - starts[count]);
    if (!key || !*key) { free(key); return NULL; }
    if (indexed[count]) {
        size_t kept = 0;
        for (size_t i = 0; key[i]; i++) if (key[i] != ' ' && key[i] != '\t') key[kept++] = key[i];
        key[kept] = 0;
    }
    for (size_t i = 0; key[i]; i++) {
        unsigned char c = (unsigned char)key[i];
        if (!(isalnum(c) || c == '_' || c == '.' || c == '[' || c == ']' || c == '-')) {
            free(key); return NULL;
        }
    }
    if (count && !(*annotation = dt_copy(p->text + begin, key_begin - begin))) {
        free(key); return NULL;
    }
    return key;
}

static char *dt_value(dt_parser *p)
{
    size_t start, end, parentheses = 0;
    int quoted = 0;
    dt_space(p); start = p->offset;
    while (p->offset < p->length) {
        unsigned char c = (unsigned char)p->text[p->offset++];
        if (quoted) {
            /* Declaration lexer flags disable escapes. A backslash before
             * the closing quote is still a path byte, not an escape. The
             * quoted reader keeps control bytes; a newline is its error. */
            if (c == '"') quoted = 0;
            else if (c == '\n') return NULL;
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

static sh_decl_node *dt_parse(sh_decl_source source, int unique_fields, int open_body,
    char *error, size_t error_capacity)
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
        if (open_body && p.depth == 1 && !p.failed && p.offset == p.length) {
            free(p.stack[--p.depth].keys); break;
        }
        node = (sh_decl_node *)calloc(1, sizeof(*node));
        if (!node) { dt_fail(&p, "allocation failed"); break; }
        node->key = dt_field(&p, &node->annotation);
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
{ return dt_parse(source, 1, 0, error, error_capacity); }

sh_decl_node *sh_decl_tree_parse_ordered(sh_decl_source source, char *error, size_t error_capacity)
{ return dt_parse(source, 0, 0, error, error_capacity); }

sh_decl_node *sh_decl_tree_parse_reflected_body(sh_decl_source source, int ordered,
    char *error, size_t error_capacity)
{ return dt_parse(source, !ordered, 1, error, error_capacity); }

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
        if (!node->key || (node->annotation && !dt_append(&out, node->annotation)) ||
            !dt_append(&out, node->key) ||
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
        for (i = 0; i < size; i++) if (value[i] == '"') return 0;
    } else {
        for (i = 0; i < size; i++) {
            unsigned char c = (unsigned char)value[i];
            if (!(isalnum(c) || strchr("_/\\.:$-", c))) return 0;
        }
    }
    *text = value; *length = size;
    return 1;
}

int sh_decl_tree_list_count(const sh_decl_node *node, size_t *count)
{
    const char *value; size_t length, at = 0, number = 0;
    if (!count || !sh_decl_tree_literal(node, &value, &length) || !length) return 0;
    int negative = value[0] == '-';
    if (negative) at++;
    if (at == length) return 0;
    size_t limit = negative ? 2147483648u : 2147483647u;
    for (; at < length; at++) {
        unsigned digit = (unsigned char)value[at] - '0';
        if (digit > 9 || number > (limit - digit) / 10u) return 0;
        number = number * 10u + digit;
    }
    *count = negative ? 0 : number; return 1;
}
