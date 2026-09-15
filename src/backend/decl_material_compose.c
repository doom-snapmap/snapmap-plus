#include "decl_material_compose.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct mm_context {
    const sh_decl_material_composition_schema *schema;
    sh_decl_composition_role role;
    size_t index;
    char *error;
    size_t capacity;
    int failed;
    char **bindings;
    size_t binding_count, binding_capacity;
} mm_context;
typedef struct mm_input {
    sh_decl_material material;
    sh_decl_node *tree;
    char **names, **ids;
    char *text;
    size_t length;
} mm_input;
static int mm_fail(mm_context *c, const char *reason)
{
    if (!c->failed && c->error && c->capacity && !*c->error)
        snprintf(c->error, c->capacity, "material composition: %s", reason);
    c->failed = 1; return 0;
}
static char *mm_copy(mm_context *c, sh_decl_source text)
{
    char *out = text.text && text.length < SIZE_MAX ? malloc(text.length + 1) : NULL;
    if (!out) { mm_fail(c, "allocation or source failure"); return NULL; }
    memcpy(out, text.text, text.length); out[text.length] = 0; return out;
}
static char *mm_hex(mm_context *c, sh_decl_source text)
{
    static const char digits[] = "0123456789abcdef";
    char *out = text.length <= (SIZE_MAX - 3) / 2 ? malloc(text.length * 2 + 3) : NULL;
    if (!out || (!text.text && text.length)) { free(out); mm_fail(c, "encoding allocation failure"); return NULL; }
    out[0] = '"';
    for (size_t i = 0; i < text.length; i++) {
        out[i * 2 + 1] = digits[(unsigned char)text.text[i] >> 4];
        out[i * 2 + 2] = digits[(unsigned char)text.text[i] & 15];
    }
    out[text.length * 2 + 1] = '"'; out[text.length * 2 + 2] = 0; return out;
}
static int mm_digit(char ch)
{ return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; }
static char *mm_unhex(mm_context *c, const sh_decl_node *node)
{
    const char *s; size_t n; char *out;
    if (!sh_decl_tree_literal(node, &s, &n) || n % 2) { mm_fail(c, "invalid private payload"); return NULL; }
    out = malloc(n / 2 + 1);
    if (!out) { mm_fail(c, "decoding allocation failure"); return NULL; }
    for (size_t i = 0; i < n; i += 2) {
        int a = mm_digit(s[i]), b = mm_digit(s[i + 1]);
        if (a < 0 || b < 0 || !(a | b)) { free(out); mm_fail(c, "invalid private byte"); return NULL; }
        out[i / 2] = (char)((a << 4) | b);
    }
    out[n / 2] = 0; return out;
}
static sh_decl_node *mm_node(mm_context *c, const char *key, const char *value)
{
    sh_decl_node *node = calloc(1, sizeof(*node));
    if (!node) { mm_fail(c, "node allocation failure"); return NULL; }
    node->key = key ? mm_copy(c, (sh_decl_source){key, strlen(key)}) : NULL;
    node->value = value ? mm_copy(c, (sh_decl_source){value, strlen(value)}) : NULL;
    node->compound = !value; node->assignment = key != NULL;
    if (c->failed) { sh_decl_tree_free(node); return NULL; }
    return node;
}
static sh_decl_node *mm_add(sh_decl_node *parent, sh_decl_node *child)
{
    sh_decl_node **tail;
    if (!parent || !child) { sh_decl_tree_free(child); return NULL; }
    for (tail = &parent->children; *tail; tail = &(*tail)->next) {}
    *tail = child; return child;
}
static int mm_field(mm_context *c, sh_decl_node *parent, const char *key, sh_decl_source bytes)
{
    char *encoded = mm_hex(c, bytes);
    sh_decl_node *node = encoded ? mm_add(parent, mm_node(c, key, encoded)) : NULL;
    free(encoded); return node != NULL;
}
static char *mm_name(mm_context *c, sh_decl_source source)
{
    char *name = mm_copy(c, source);
    if (!name) return NULL;
    for (size_t i = 0; i < source.length; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch <= 32 || ch >= 127 || strchr("\"'{}()[]=;", ch)) {
            free(name); mm_fail(c, "unsupported destination/reference name"); return NULL;
        }
        if (ch >= 'A' && ch <= 'Z') name[i] += 'a' - 'A';
        if (ch == '\\') name[i] = '/';
    }
    return name;
}
static void mm_clear_bindings(mm_context *c)
{
    for (size_t i = 0; i < c->binding_count; i++) free(c->bindings[i]);
    free(c->bindings); c->bindings = NULL; c->binding_count = c->binding_capacity = 0;
}
static int mm_resolve(void *context, const char *family, sh_decl_source name, int *kind)
{
    mm_context *c = context;
    sh_decl_source binding = {0};
    int result = c->schema->resolve(c->schema->context, c->role, c->index, family, name, kind, &binding);
    if (result <= 0) return result;
    if (!strcmp(family, "renderparm") && (!binding.text || !binding.length)) {
        mm_fail(c, "renderparm has no verified schema binding"); return -1;
    }
    if (c->binding_count == c->binding_capacity) {
        size_t cap = c->binding_capacity ? c->binding_capacity * 2 : 16;
        char **next = cap > c->binding_capacity && cap <= SIZE_MAX / sizeof(*next) ?
            realloc(c->bindings, cap * sizeof(*next)) : NULL;
        if (!next) { mm_fail(c, "binding allocation failure"); return -1; }
        c->bindings = next; c->binding_capacity = cap;
    }
    c->bindings[c->binding_count] = mm_hex(c, binding);
    if (!c->bindings[c->binding_count]) return -1;
    c->binding_count++; return 1;
}
static int mm_equal(const sh_decl_node *a, const sh_decl_node *b)
{
    size_t na = 0, nb = 0;
    if (!a || !b) return a == b;
    if (a->compound != b->compound) return 0;
    if (!a->compound) return a->value && b->value && !strcmp(a->value, b->value);
    for (const sh_decl_node *n = a->children; n; n = n->next) {
        na++; if (!mm_equal(n, sh_decl_tree_member(b, n->key))) return 0;
    }
    for (const sh_decl_node *n = b->children; n; n = n->next) nb++;
    return na == nb;
}
static int mm_parse(mm_context *c, sh_decl_source source, mm_input *out)
{
    sh_decl_material_schema schema = {c, mm_resolve};
    sh_decl_node *edit, *list;
    char number[32];
    mm_clear_bindings(c);
    if (!sh_decl_material_read(source, &schema, &out->material, c->error, c->capacity)) return mm_fail(c, "source cannot be read");
    if (out->material.reference_count != c->binding_count) return mm_fail(c, "schema binding count differs from source references");
    if (out->material.count > SIZE_MAX / sizeof(*out->names)) return mm_fail(c, "name allocation overflow");
    out->names = calloc(out->material.count ? out->material.count : 1, sizeof(*out->names));
    out->ids = calloc(out->material.count ? out->material.count : 1, sizeof(*out->ids));
    if (!out->names || !out->ids) return mm_fail(c, "name allocation failure");
    for (size_t i = 0; i < out->material.count; i++) {
        size_t ordinal = 0, n;
        out->names[i] = mm_name(c, out->material.writes[i].name);
        if (!out->names[i]) return 0;
        for (size_t j = 0; j < i; j++) ordinal += !strcmp(out->names[i], out->names[j]);
        n = strlen(out->names[i]);
        if (n > SIZE_MAX - 32 || !(out->ids[i] = malloc(n + 32))) return mm_fail(c, "identity allocation failure");
        snprintf(out->ids[i], n + 32, "%s#%zu", out->names[i], ordinal);
    }
    out->tree = mm_node(c, NULL, NULL);
    edit = mm_add(out->tree, mm_node(c, "edit", NULL));
    list = mm_add(edit, mm_node(c, "writes", NULL));
    snprintf(number, sizeof(number), "%zu", out->material.count);
    mm_add(list, mm_node(c, "num", number));
    for (size_t i = 0; i < out->material.count && !c->failed; i++) {
        sh_decl_material_write *write = &out->material.writes[i];
        sh_decl_node *item, *value, *reads;
        snprintf(number, sizeof(number), "item[%zu]", i);
        item = mm_add(list, mm_node(c, number, NULL));
        mm_field(c, item, "id", (sh_decl_source){out->ids[i], strlen(out->ids[i])});
        mm_field(c, item, "name", (sh_decl_source){out->names[i], strlen(out->names[i])});
        snprintf(number, sizeof(number), "%d", write->kind);
        mm_add(item, mm_node(c, "kind", number));
        mm_add(item, mm_node(c, "schema", c->bindings[write->first_reference]));
        if (write->constant && write->mask == 15) {
            value = mm_add(item, mm_node(c, "value", NULL));
            for (size_t k = 0; k < 4; k++) {
                char key[2] = {"xyzw"[k], 0}; char *scalar = mm_copy(c, write->components[k]);
                if (scalar) mm_add(value, mm_node(c, key, scalar)); free(scalar);
            }
        } else {
            mm_field(c, item, "value", write->value);
            snprintf(number, sizeof(number), "%u", write->mask);
            mm_add(item, mm_node(c, "mask", number));
        }
        reads = mm_add(item, mm_node(c, "reads", NULL));
        for (size_t r = write->first_reference + 1; r < write->first_reference + write->reference_count && !c->failed; r++) {
            const sh_decl_material_reference *reference = &out->material.references[r];
            sh_decl_node *binding;
            char *reference_name, *key, *writers = NULL;
            size_t writer_length = 0;
            const sh_decl_node *previous;
            if (strcmp(reference->family, "renderparm")) continue;
            reference_name = mm_name(c, reference->name);
            if (!reference_name) break;
            key = mm_hex(c, (sh_decl_source){reference_name, strlen(reference_name)});
            if (!key) { free(reference_name); break; }
            /* Hex is a private field key; remove quotes and add a letter so
             * arbitrary engine names never acquire assignment-tree syntax. */
            key[0] = 'r'; key[strlen(key) - 1] = 0;
            previous = sh_decl_tree_member(reads, key);
            binding = mm_node(c, key, NULL);
            mm_add(binding, mm_node(c, "schema", c->bindings[r]));
            for (size_t j = 0; j < i; j++) if (!strcmp(out->names[j], reference_name)) {
                size_t n = strlen(out->ids[j]); char *next;
                if (writer_length > SIZE_MAX - n - 2 || !(next = realloc(writers, writer_length + n + 2))) {
                    mm_fail(c, "prior-write allocation failure"); break;
                }
                writers = next; memcpy(writers + writer_length, out->ids[j], n);
                writer_length += n; writers[writer_length++] = '\n'; writers[writer_length] = 0;
            }
            mm_field(c, binding, "writers", (sh_decl_source){writers ? writers : "", writer_length});
            if (previous) {
                if (!mm_equal(previous, binding)) mm_fail(c, "reference schema changed during one source read");
                sh_decl_tree_free(binding);
            } else mm_add(reads, binding);
            free(writers); free(key); free(reference_name);
        }
    }
    if (!c->failed) out->text = sh_decl_tree_write(out->tree, &out->length);
    return out->text ? 1 : mm_fail(c, "normalization allocation failure");
}
static int mm_append(mm_context *c, char **text, size_t *length, const char *part)
{
    size_t n; char *next;
    if (!part) return mm_fail(c, "missing output part");
    n = strlen(part);
    if (*length > SIZE_MAX - n - 1 || !(next = realloc(*text, *length + n + 1))) return mm_fail(c, "output allocation failure");
    memcpy(next + *length, part, n + 1); *text = next; *length += n; return 1;
}
static char *mm_emit(mm_context *c, const sh_decl_node *tree, size_t *length)
{
    const sh_decl_node *list = sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "writes");
    char *out = NULL;
    *length = 0; mm_append(c, &out, length, "{\n");
    for (const sh_decl_node *item = list ? list->children : NULL; item && !c->failed; item = item->next) {
        const sh_decl_node *value, *mask;
        char *name;
        if (!strcmp(item->key, "num")) continue;
        name = mm_unhex(c, sh_decl_tree_member(item, "name"));
        if (!name) break;
        mm_append(c, &out, length, name); free(name);
        mask = sh_decl_tree_member(item, "mask");
        if (mask && mask->value) {
            unsigned bits = (unsigned)strtoul(mask->value, NULL, 10);
            if (!bits || bits > 15) { mm_fail(c, "invalid result write mask"); break; }
            if (bits != 15) {
                char suffix[6] = {'.', 0}; size_t n = 1;
                for (size_t i = 0; i < 4; i++) if (bits & (1u << i)) suffix[n++] = "xyzw"[i];
                suffix[n] = 0; mm_append(c, &out, length, suffix);
            }
        }
        mm_append(c, &out, length, " ");
        value = sh_decl_tree_member(item, "value");
        if (value && value->compound) {
            mm_append(c, &out, length, "{ ");
            for (size_t i = 0; i < 4; i++) {
                char key[2] = {"xyzw"[i], 0}; const sh_decl_node *v = sh_decl_tree_member(value, key);
                if (!v || !v->value) { mm_fail(c, "missing result vector component"); break; }
                mm_append(c, &out, length, v->value);
                if (i < 3) mm_append(c, &out, length, ", ");
            }
            mm_append(c, &out, length, " }");
        } else {
            char *raw = mm_unhex(c, value);
            if (raw) mm_append(c, &out, length, raw); free(raw);
        }
        mm_append(c, &out, length, "\n");
    }
    if (!c->failed) mm_append(c, &out, length, "}\n");
    if (c->failed) { free(out); out = NULL; *length = 0; }
    return out;
}
static void mm_free(mm_input *input)
{
    for (size_t i = 0; i < input->material.count; i++) {
        if (input->names) free(input->names[i]);
        if (input->ids) free(input->ids[i]);
    }
    free(input->names); free(input->ids);
    sh_decl_material_free(&input->material); sh_decl_tree_free(input->tree); free(input->text);
}
static const sh_decl_node *mm_writes(const sh_decl_node *tree)
{ return sh_decl_tree_member(sh_decl_tree_member(tree, "edit"), "writes"); }
static const sh_decl_node *mm_write(const sh_decl_node *tree, const sh_decl_node *id)
{
    const sh_decl_node *list = mm_writes(tree);
    for (const sh_decl_node *item = list ? list->children : NULL; item; item = item->next)
        if (mm_equal(sh_decl_tree_member(item, "id"), id)) return item;
    return NULL;
}
static int mm_authored_bindings(mm_context *c, const mm_input *original, const mm_input *input, const mm_input *result)
{
    const sh_decl_node *list = mm_writes(input->tree);
    for (const sh_decl_node *item = list ? list->children : NULL; item; item = item->next) {
        const sh_decl_node *id, *before, *after;
        if (!strcmp(item->key, "num")) continue;
        id = sh_decl_tree_member(item, "id");
        before = mm_write(original->tree, id); after = mm_write(result->tree, id);
        if (before && mm_equal(sh_decl_tree_member(before, "value"), sh_decl_tree_member(item, "value")) &&
            mm_equal(sh_decl_tree_member(before, "mask"), sh_decl_tree_member(item, "mask")) &&
            mm_equal(sh_decl_tree_member(before, "kind"), sh_decl_tree_member(item, "kind"))) continue;
        if (!after || !mm_equal(sh_decl_tree_member(item, "schema"), sh_decl_tree_member(after, "schema")) ||
            !mm_equal(sh_decl_tree_member(item, "reads"), sh_decl_tree_member(after, "reads"))) {
            char *name = mm_unhex(c, id);
            if (name && c->error && c->capacity && !*c->error)
                snprintf(c->error, c->capacity, "material parameter %s: schema or prior-write binding changed underneath an authored value", name);
            free(name); return mm_fail(c, "authored parameter binding changed");
        }
    }
    return 1;
}
static int mm_occurrences(mm_context *c, const mm_input *original, const mm_input *input)
{
    for (size_t i = 0; i < input->material.count; i++) {
        size_t before_count = 0, after_count = 0, prior = 0;
        for (size_t j = 0; j < i; j++) prior += !strcmp(input->names[i], input->names[j]);
        if (prior) continue;
        for (size_t j = 0; j < original->material.count; j++) before_count += !strcmp(input->names[i], original->names[j]);
        for (size_t j = i; j < input->material.count; j++) after_count += !strcmp(input->names[i], input->names[j]);
        if (before_count == after_count) continue;
        /* Changing multiplicity while altering retained occurrences makes their
         * ordinal correspondence ambiguous. A stable prefix may append/truncate;
         * peer edits to that prefix can still merge in the common composer. */
        for (size_t j = i; j < input->material.count; j++) if (!strcmp(input->names[j], input->names[i])) {
            char *id = mm_hex(c, (sh_decl_source){input->ids[j], strlen(input->ids[j])});
            sh_decl_node id_node = {0}; const sh_decl_node *a, *b;
            if (!id) return 0;
            id_node.value = id;
            a = mm_write(original->tree, &id_node); b = mm_write(input->tree, &id_node);
            free(id);
            if (a && !mm_equal(a, b)) return mm_fail(c, "write multiplicity changed with an ambiguous retained occurrence");
        }
    }
    return 1;
}
/* Binding payloads are hex-encoded native names/IDs. Compare complete encoded
 * records directly: names cannot contain newline, so 0a separates writer IDs.
 * No decoding/allocation is needed during the pairwise topological walk. */
static int mm_reader_order(mm_context *c, const sh_decl_node *reader, const sh_decl_node *writer)
{
    const sh_decl_node *reads = sh_decl_tree_member(reader, "reads");
    const char *name, *id; size_t name_length, id_length;
    if (!sh_decl_tree_literal(sh_decl_tree_member(writer, "name"), &name, &name_length) ||
        !sh_decl_tree_literal(sh_decl_tree_member(writer, "id"), &id, &id_length)) {
        mm_fail(c, "missing private writer identity"); return 2;
    }
    for (const sh_decl_node *ref = reads ? reads->children : NULL; ref; ref = ref->next) {
        const char *prefix; size_t length, start = 0;
        if (!ref->key || ref->key[0] != 'r' || strlen(ref->key + 1) != name_length ||
            memcmp(ref->key + 1, name, name_length)) continue;
        if (!sh_decl_tree_literal(sh_decl_tree_member(ref, "writers"), &prefix, &length) || length % 2) {
            mm_fail(c, "invalid private prior-write binding"); return 2;
        }
        for (size_t end = 0; end <= length; end += 2) {
            if (end != length && !(prefix[end] == '0' && prefix[end + 1] == 'a')) continue;
            if (end - start == id_length && !memcmp(prefix + start, id, id_length)) return 1;
            start = end + 2;
        }
        /* Preserve the source's default/prefix read before this new writer. */
        return -1;
    }
    return 0;
}

static int mm_order(void *context, const char *path,
    const sh_decl_node *left, const sh_decl_node *right)
{
    mm_context *c = context; int forward, reverse;
    if (strcmp(path, "edit.writes")) return 0;
    forward = mm_reader_order(c, left, right);
    reverse = mm_reader_order(c, right, left);
    if (forward == 2 || reverse == 2 || (forward && reverse && forward != -reverse)) return 2;
    return forward ? forward : -reverse;
}

char *sh_decl_material_compose(sh_decl_source baseline, const sh_decl_source *sources,
    size_t count, const sh_decl_material_composition_schema *schema,
    size_t *length, char *error, size_t capacity, sh_decl_conflict *conflict)
{
    static const sh_decl_collection_rule rule = {"edit.writes", "id"};
    mm_context c = {0}; mm_input original = {0}, result = {0}, *inputs = NULL;
    sh_decl_collection_order order = {&c, mm_order};
    sh_decl_source *views = NULL;
    sh_decl_node *merged = NULL;
    char *text = NULL, *out = NULL; size_t n = 0;
    if (length) *length = 0;
    if (error && capacity) *error = 0;
    if (conflict) *conflict = (sh_decl_conflict){SIZE_MAX, SIZE_MAX};
    c.schema = schema; c.error = error; c.capacity = capacity;
    if (!schema || !schema->resolve || (!sources && count) || count > SIZE_MAX / sizeof(*inputs) || count > SIZE_MAX / sizeof(*views)) {
        mm_fail(&c, "invalid composition inputs"); goto done;
    }
    inputs = calloc(count ? count : 1, sizeof(*inputs)); views = calloc(count ? count : 1, sizeof(*views));
    if (!inputs || !views) { mm_fail(&c, "input allocation failure"); goto done; }
    c.role = SH_DECL_COMPOSITION_ORIGINAL;
    if (!mm_parse(&c, baseline, &original)) goto done;
    c.role = SH_DECL_COMPOSITION_CONTRIBUTION;
    for (size_t i = 0; i < count; i++) {
        c.index = i;
        if (!mm_parse(&c, sources[i], &inputs[i])) { if (conflict) conflict->first = i; goto done; }
        if (!mm_occurrences(&c, &original, &inputs[i])) { if (conflict) conflict->first = i; goto done; }
        views[i] = (sh_decl_source){inputs[i].text, inputs[i].length};
    }
    text = sh_decl_compose_ordered((sh_decl_source){original.text, original.length}, views, count,
        &rule, 1, &order, &n, error, capacity, conflict);
    if (!text) goto done;
    merged = sh_decl_tree_parse((sh_decl_source){text, n}, error, capacity);
    if (!merged) goto done;
    out = mm_emit(&c, merged, &n);
    if (!out) goto done;
    c.role = SH_DECL_COMPOSITION_RESULT; c.index = 0;
    if (!mm_parse(&c, (sh_decl_source){out, n}, &result)) goto done;
    if (!mm_equal(merged, result.tree)) {
        mm_fail(&c, "result changes a retained prior-write or schema binding"); goto done;
    }
    for (size_t i = 0; i < count; i++) if (!mm_authored_bindings(&c, &original, &inputs[i], &result)) {
        if (conflict) conflict->first = i;
        goto done;
    }
    if (length) *length = n;
done:
    if (c.failed) { free(out); out = NULL; }
    free(text); sh_decl_tree_free(merged); mm_free(&original); mm_free(&result);
    if (inputs) for (size_t i = 0; i < count; i++) mm_free(&inputs[i]);
    free(inputs); free(views); mm_clear_bindings(&c); return out;
}
