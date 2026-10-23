#include "decl_rebind.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct dr_edit {
    sh_decl_node *node;
    size_t offset, length, replacement_length;
    char *replacement;
} dr_edit;

typedef struct dr_walk {
    const sh_decl_dependency_schema *schema;
    sh_decl_binding_resolver resolver;
    void *context;
    sh_decl_source source;
    dr_edit *edits;
    size_t count, capacity;
    int json;
    sh_decl_site_binding_resolver site_resolver;
} dr_walk;

static int dr_describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    dr_walk *walk = context;
    return walk->schema->describe(walk->schema->context, type, shape);
}

static int dr_field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *out)
{
    dr_walk *walk = context;
    return walk->schema->field(walk->schema->context, type, key, out);
}

static int dr_dynamic(void *context, sh_decl_value_type base, const char *name,
    size_t length, sh_decl_value_type *selected)
{
    dr_walk *walk = context;
    return walk->schema->dynamic_type &&
        walk->schema->dynamic_type(walk->schema->context, base, name, length, selected);
}

static void dr_gap(void *context, const char *path, sh_decl_value_type type, const char *reason)
{
    dr_walk *walk = context;
    if (walk->schema->gap) walk->schema->gap(walk->schema->context, path, type, reason);
}

static int dr_reference(void *context, const char *path, sh_decl_value_type type,
    const char *name, size_t length)
{
    (void)context; (void)path; (void)type; (void)name; (void)length;
    return 1;
}

/* Declaration names are emitted as native quoted literals; that lexer has
 * escapes disabled. JSON uses its own quoting while preserving native bytes. */
static char *dr_quote(const char *name, int json, size_t *length)
{
    static const char hex[] = "0123456789abcdef";
    size_t i, n = strlen(name), required = 2, used = 0;
    char *out;
    if (!n) return NULL;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        size_t extra = json ? (c < 32 ? 6 : (c == '"' || c == '\\' ? 2 : 1)) : 1;
        if (!json && (c < 32 || c == '"')) return NULL;
        if (required > SIZE_MAX - extra) return NULL;
        required += extra;
    }
    if (required == SIZE_MAX || !(out = malloc(required + 1))) return NULL;
    out[used++] = '"';
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 32) {
            memcpy(out + used, "\\u00", 4); used += 4;
            out[used++] = hex[c >> 4]; out[used++] = hex[c & 15];
        } else {
            if (json && (c == '"' || c == '\\')) out[used++] = '\\';
            out[used++] = (char)c;
        }
    }
    out[used++] = '"'; out[used] = 0; *length = used;
    return out;
}

static int dr_site(void *context, const char *path, sh_decl_value_type type,
    const char *name, size_t length, const sh_decl_reference_site *site)
{
    dr_walk *walk = context;
    const char *replacement = NULL;
    dr_edit edit = {0};
    int resolved = walk->site_resolver ? walk->site_resolver(walk->context, path, type, name, length, site, &replacement) :
        walk->resolver(walk->context, path, type, name, length, &replacement);
    if (resolved < 0) return 0;
    if (!resolved) return 1;
    if (!replacement) return 0;
    if (strlen(replacement) == length && !memcmp(name, replacement, length)) return 1;
    if (walk->json) {
        uintptr_t base = (uintptr_t)walk->source.text, start = (uintptr_t)site->json;
        if (site->node || !site->json || start < base || start - base > walk->source.length ||
            site->json_length > walk->source.length - (size_t)(start - base)) return 0;
        edit.offset = (size_t)(start - base); edit.length = site->json_length;
    } else {
        if (!site->node || site->json || site->node->compound) return 0;
        edit.node = (sh_decl_node *)site->node;
    }
    if (walk->count == walk->capacity) {
        size_t capacity = walk->capacity ? walk->capacity * 2 : 16;
        dr_edit *edits;
        if (capacity < walk->capacity || capacity > SIZE_MAX / sizeof(*edits) ||
            !(edits = realloc(walk->edits, capacity * sizeof(*edits)))) return 0;
        walk->edits = edits; walk->capacity = capacity;
    }
    edit.replacement = dr_quote(replacement, walk->json, &edit.replacement_length);
    if (!edit.replacement) return 0;
    for (size_t i = 0; i < walk->count; i++) {
        const dr_edit *old = walk->edits + i;
        if (walk->json ? old->offset != edit.offset || old->length != edit.length : old->node != edit.node) continue;
        int equal = old->replacement_length == edit.replacement_length &&
            !memcmp(old->replacement, edit.replacement, edit.replacement_length);
        free(edit.replacement); return equal;
    }
    walk->edits[walk->count++] = edit;
    return 1;
}

/* An empty literal's site is rewritten like any other: a declaration node,
 * or in JSON the quoted "" span the walk reports. */
static int dr_empty(void *context, sh_decl_value_type type)
{
    dr_walk *walk = context;
    return walk->schema->empty_lookup(walk->schema->context, type);
}

static int dr_order(const void *left, const void *right)
{
    const dr_edit *a = left, *b = right;
    return a->offset < b->offset ? -1 : a->offset > b->offset;
}

static char *dr_copy(dr_walk *walk, size_t *length)
{
    size_t i, total = walk->source.length, previous = 0, used = 0;
    char *out;
    if (walk->count > 1) qsort(walk->edits, walk->count, sizeof(*walk->edits), dr_order);
    for (i = 0; i < walk->count; i++) {
        const dr_edit *edit = &walk->edits[i];
        if (edit->offset < previous || edit->length > total) return NULL;
        previous = edit->offset + edit->length;
        total -= edit->length;
        if (edit->replacement_length > SIZE_MAX - total) return NULL;
        total += edit->replacement_length;
    }
    if (total == SIZE_MAX || !(out = malloc(total + 1))) return NULL;
    previous = 0;
    for (i = 0; i < walk->count; i++) {
        const dr_edit *edit = &walk->edits[i];
        size_t prefix = edit->offset - previous;
        memcpy(out + used, walk->source.text + previous, prefix); used += prefix;
        memcpy(out + used, edit->replacement, edit->replacement_length); used += edit->replacement_length;
        previous = edit->offset + edit->length;
    }
    memcpy(out + used, walk->source.text + previous, walk->source.length - previous);
    out[total] = 0; *length = total;
    return out;
}

/* Swap only after traversal has finished, then restore even when serialization
 * fails. Graph record names and other borrowed tree spans remain valid. */
static char *dr_write(dr_walk *walk, const sh_decl_node *tree, size_t *length)
{
    char *out;
    for (size_t i = 0; i < walk->count; i++) {
        char *old = walk->edits[i].node->value;
        walk->edits[i].node->value = walk->edits[i].replacement;
        walk->edits[i].replacement = old;
    }
    out = sh_decl_tree_write(tree, length);
    for (size_t i = walk->count; i; i--) {
        dr_edit *edit = &walk->edits[i - 1];
        char *replacement = edit->node->value;
        edit->node->value = edit->replacement; edit->replacement = replacement;
    }
    return out;
}

static void dr_free(dr_walk *walk)
{
    for (size_t i = 0; i < walk->count; i++) free(walk->edits[i].replacement);
    free(walk->edits);
}

static char *dr_run(sh_decl_source source, sh_decl_value_type type,
    const sh_decl_dependency_schema *schema, sh_decl_binding_resolver resolver,
    void *context, size_t *length, sh_decl_rebind_result *result, int mode)
{
    dr_walk walk = {schema, resolver, context, source, NULL, 0, 0, mode == 2};
    sh_decl_dependency_schema adapter = {&walk, dr_describe, dr_field, dr_reference, dr_gap, dr_dynamic, dr_site};
    if (schema && schema->empty_lookup) adapter.empty_lookup = dr_empty;
    sh_decl_rebind_result local = {0};
    sh_decl_node *tree = NULL;
    char *out = NULL;
    size_t output_length = 0;
    int ok = 0;
    if (length) *length = 0;
    if (!result) result = &local;
    memset(result, 0, sizeof(*result));
    if (!source.text || !schema || !schema->describe || !schema->field || !resolver) goto done;
    if (mode == 2)
        ok = sh_decl_json_state_dependencies(source.text, source.length, type, &adapter, &result->dependencies);
    else if ((tree = sh_decl_tree_parse(source, NULL, 0)) != NULL)
        ok = mode ? sh_decl_entity_dependencies(tree, type, &adapter, &result->dependencies) :
            sh_decl_state_dependencies(tree, type, &adapter, &result->dependencies);
    if (!ok) goto done;
    if (walk.json || !walk.count) out = dr_copy(&walk, &output_length);
    else out = dr_write(&walk, tree, &output_length);
    if (out) { result->changed = walk.count; if (length) *length = output_length; }
done:
    if (!out && !result->dependencies.gaps) result->dependencies.aborted = 1;
    dr_free(&walk); sh_decl_tree_free(tree);
    return out;
}

char *sh_decl_rebind_entity(sh_decl_source source, sh_decl_value_type type,
    const sh_decl_dependency_schema *schema, sh_decl_binding_resolver resolver,
    void *context, size_t *length, sh_decl_rebind_result *result)
{ return dr_run(source, type, schema, resolver, context, length, result, 1); }

char *sh_decl_rebind_state(sh_decl_source source, sh_decl_value_type type,
    const sh_decl_dependency_schema *schema, sh_decl_binding_resolver resolver,
    void *context, size_t *length, sh_decl_rebind_result *result)
{ return dr_run(source, type, schema, resolver, context, length, result, 0); }

char *sh_decl_rebind_json(sh_decl_source source, sh_decl_value_type type,
    const sh_decl_dependency_schema *schema, sh_decl_binding_resolver resolver,
    void *context, size_t *length, sh_decl_rebind_result *result)
{ return dr_run(source, type, schema, resolver, context, length, result, 2); }

static char *dr_custom(sh_decl_source source, const sh_decl_node *syntax,
    const sh_decl_dependency_schema *schema, sh_decl_binding_walk reader, void *reader_context,
    sh_decl_binding_resolver resolver, void *context, size_t *length, sh_decl_rebind_result *result, int json,
    sh_decl_site_binding_resolver site_resolver)
{
    dr_walk walk = {schema, resolver, context, source, NULL, 0, 0, json};
    walk.site_resolver = site_resolver;
    sh_decl_dependency_schema adapter = {&walk, dr_describe, dr_field, dr_reference, dr_gap, dr_dynamic, dr_site};
    if (schema && schema->empty_lookup) adapter.empty_lookup = dr_empty;
    sh_decl_rebind_result local = {0};
    size_t output_length = 0;
    char *out = NULL;
    if (length) *length = 0;
    if (!result) result = &local;
    memset(result, 0, sizeof(*result));
    if (source.text && (json || syntax) && schema && schema->describe && schema->field && reader && (resolver || site_resolver) &&
        reader(reader_context, &adapter, &result->dependencies) &&
        !result->dependencies.gaps && !result->dependencies.aborted)
        out = walk.count && !json ? dr_write(&walk, syntax, &output_length) : dr_copy(&walk, &output_length);
    if (out) { result->changed = walk.count; if (length) *length = output_length; }
    else if (!result->dependencies.gaps) result->dependencies.aborted = 1;
    dr_free(&walk); return out;
}

char *sh_decl_rebind_parsed(sh_decl_source source, const sh_decl_node *syntax,
    const sh_decl_dependency_schema *schema, sh_decl_binding_walk reader, void *reader_context,
    sh_decl_binding_resolver resolver, void *context, size_t *length, sh_decl_rebind_result *result)
{ return dr_custom(source, syntax, schema, reader, reader_context, resolver, context, length, result, 0, NULL); }

char *sh_decl_rebind_json_walk(sh_decl_source source,
    const sh_decl_dependency_schema *schema, sh_decl_binding_walk reader, void *reader_context,
    sh_decl_binding_resolver resolver, void *context, size_t *length, sh_decl_rebind_result *result)
{ return dr_custom(source, NULL, schema, reader, reader_context, resolver, context, length, result, 1, NULL); }

char *sh_decl_rebind_json_walk_sites(sh_decl_source source,
    const sh_decl_dependency_schema *schema, sh_decl_binding_walk reader, void *reader_context,
    sh_decl_site_binding_resolver resolver, void *context, size_t *length, sh_decl_rebind_result *result)
{ return dr_custom(source, NULL, schema, reader, reader_context, NULL, context, length, result, 1, resolver); }
