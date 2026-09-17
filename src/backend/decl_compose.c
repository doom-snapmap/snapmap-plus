#include "decl_compose.h"
#include "decl_polymorphic.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define DC_DEPTH_LIMIT 48u
#define DC_PATH_CAP 2048u

typedef sh_decl_node dc_node;
typedef struct dc_annotation { const dc_node *node; sh_decl_value_type type; } dc_annotation;
typedef struct dc_context {
    const sh_decl_collection_rule *rules;
    size_t rule_count;
    char *error;
    size_t error_capacity;
    int failed;
    sh_decl_conflict *conflict;
    const sh_decl_composition_schema *schema;
    const sh_decl_collection_order *order;
    sh_decl_value_type state_type;
    dc_annotation *annotations;
    size_t annotation_count, annotation_capacity;
    int record_types;
    int inherited_input, inherited_layout_changed;
    unsigned char *conflicting_sources;
} dc_context;
typedef struct dc_buffer { char *text; size_t length, capacity; } dc_buffer;

static char *dc_copy(const char *text, size_t length)
{
    char *result;
    if (length == SIZE_MAX) return NULL;
    result = (char *)malloc(length + 1u);
    if (result) { memcpy(result, text, length); result[length] = '\0'; }
    return result;
}

static void dc_free(dc_node *node) { sh_decl_tree_free(node); }
static dc_node *dc_member(const dc_node *parent, const char *key)
{ return (dc_node *)sh_decl_tree_member(parent, key); }

static int dc_fail(dc_context *context, const char *path, const char *reason)
{
    if (!context->failed && context->error && context->error_capacity)
        snprintf(context->error, context->error_capacity, "%s: %s",
                 path && path[0] ? path : "declaration", reason);
    context->failed = 1;
    return 0;
}

static int dc_fail_from(dc_context *context, const char *path, const char *reason,
                         size_t first, size_t second)
{
    if (!context->failed && context->conflict) {
        context->conflict->first = first; context->conflict->second = second;
    }
    return dc_fail(context, path, reason);
}

/* Composition still uses recursive structural comparison and emission. Keep
 * its existing structural depth contract while sharing the unbounded parser. */
static dc_node *dc_parse(sh_decl_source source)
{
    dc_node *root = sh_decl_tree_parse(source, NULL, 0);
    const dc_node *stack[DC_DEPTH_LIMIT + 1u];
    size_t depth = 0;
    if (!root) return NULL;
    stack[0] = root->children;
    for (;;) {
        const dc_node *node = stack[depth];
        if (!node) { if (!depth) break; depth--; continue; }
        stack[depth] = node->next;
        if (node->compound) {
            if (depth == DC_DEPTH_LIMIT) { dc_free(root); return NULL; }
            stack[++depth] = node->children;
        }
    }
    return root;
}

static int dc_equal(const dc_node *a, const dc_node *b)
{
    const dc_node *child;
    size_t ac = 0, bc = 0;
    if (!a || !b) return a == b;
    if (a->compound != b->compound || a->reset != b->reset || a->assignment != b->assignment) return 0;
    if (!a->compound) return !strcmp(a->value, b->value);
    for (child = a->children; child; child = child->next) {
        if (!dc_equal(child, dc_member(b, child->key))) return 0;
        ac++;
    }
    for (child = b->children; child; child = child->next) bc++;
    return ac == bc;
}

int sh_decl_entity_gameplay_equal(sh_decl_source baseline, sh_decl_source source)
{
    dc_node *a = dc_parse(baseline), *b = dc_parse(source);
    const dc_node *child;
    size_t ac = 0, bc = 0;
    int equal = 0;
    if (!a || !b) goto done;
    for (child = a->children; child; child = child->next) {
        if (!strcmp(child->key, "editorVars")) continue;
        if (!dc_equal(child, dc_member(b, child->key))) goto done;
        ac++;
    }
    for (child = b->children; child; child = child->next) if (strcmp(child->key, "editorVars")) bc++;
    equal = ac == bc;
done:
    dc_free(a); dc_free(b); return equal;
}

static dc_node *dc_clone(const dc_node *source, dc_context *context)
{
    dc_node *node, **tail;
    const dc_node *child;
    if (!source) return NULL;
    node = (dc_node *)calloc(1, sizeof(*node));
    if (!node) goto failed;
    node->compound = source->compound; node->reset = source->reset;
    node->assignment = source->assignment;
    if (source->key && !(node->key = dc_copy(source->key, strlen(source->key)))) goto failed;
    if (source->value && !(node->value = dc_copy(source->value, strlen(source->value)))) goto failed;
    tail = &node->children;
    for (child = source->children; child; child = child->next) {
        *tail = dc_clone(child, context);
        if (!*tail) goto failed;
        tail = &(*tail)->next;
    }
    return node;
failed:
    dc_free(node); dc_fail(context, NULL, "composition allocation failed"); return NULL;
}

/* A numeric index wildcard describes native nested collections, never an
 * author-selected merge strategy or an arbitrary path prefix. */
static int dc_rule_path_matches(const char *pattern, const char *path)
{
    while (*pattern && *path) {
        if (pattern[0] == '[' && pattern[1] == '*' && pattern[2] == ']') {
            if (*path++ != '[' || !isdigit((unsigned char)*path)) return 0;
            while (isdigit((unsigned char)*path)) path++;
            if (*path++ != ']') return 0;
            pattern += 3;
        } else if (*pattern++ != *path++) return 0;
    }
    return !*pattern && !*path;
}

static const sh_decl_collection_rule *dc_rule(dc_context *context, const char *path)
{
    size_t i;
    for (i = 0; i < context->rule_count; i++)
        if (dc_rule_path_matches(context->rules[i].path, path)) return &context->rules[i];
    return NULL;
}

static const char *dc_item_id(const dc_node *item, const sh_decl_collection_rule *rule)
{
    const dc_node *key;
    if (!item) return NULL;
    if (!rule->item_key) return item->compound ? NULL : item->value;
    if (!rule->item_key[0]) return item->key;
    key = dc_member(item, rule->item_key);
    return key && !key->compound ? key->value : NULL;
}

static const dc_node *dc_find_item(const dc_node *list, const char *id,
                                   const sh_decl_collection_rule *rule)
{
    const dc_node *item;
    for (item = list ? list->children : NULL; item; item = item->next) {
        const char *candidate;
        if (!strcmp(item->key, "num")) continue;
        candidate = dc_item_id(item, rule);
        if (candidate && !strcmp(candidate, id)) return item;
    }
    return NULL;
}

static int dc_list_valid(const dc_node *list, const sh_decl_collection_rule *rule)
{
    const dc_node *item, *other, *count;
    size_t found = 0;
    unsigned long long declared;
    char *end;
    if (!list) return 1;
    count = dc_member(list, "num");
    if (!list->compound || !count || count->compound || !count->value ||
        !isdigit((unsigned char)count->value[0])) return 0;
    errno = 0;
    declared = strtoull(count->value, &end, 10);
    if (*end || errno == ERANGE) return 0;
    for (item = list->children; item; item = item->next) {
        char expected[40];
        const char *id;
        if (!strcmp(item->key, "num")) continue;
        snprintf(expected, sizeof(expected), "item[%zu]", found++);
        if (strcmp(item->key, expected) || !(id = dc_item_id(item, rule))) return 0;
        for (other = list->children; other != item; other = other->next) {
            const char *previous;
            if (!strcmp(other->key, "num")) continue;
            previous = dc_item_id(other, rule);
            if (previous && !strcmp(previous, id)) return 0;
        }
    }
    return found == declared;
}

static int dc_join_path(char *out, size_t capacity, const char *path, const char *key)
{
    int count = snprintf(out, capacity, "%s%s%s", path, path[0] ? "." : "", key);
    return count >= 0 && (size_t)count < capacity;
}

static int dc_shape(dc_context *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    const sh_decl_dependency_schema *schema;
    memset(shape, 0, sizeof(*shape));
    if (!context->schema || !type.name) return 0;
    schema = context->schema->types;
    if (!*type.name || !schema->describe(schema->context, type, shape))
        memset(shape, 0, sizeof(*shape));
    return 1;
}

static int dc_record_type(dc_context *context, const dc_node *node, sh_decl_value_type type)
{
    if (!context->record_types || !node || !type.name) return 1;
    if (context->annotation_count == context->annotation_capacity) {
        size_t capacity = context->annotation_capacity ? context->annotation_capacity * 2 : 64;
        dc_annotation *grown;
        if (capacity < context->annotation_capacity || capacity > SIZE_MAX / sizeof(*grown) ||
            !(grown = (dc_annotation *)realloc(context->annotations, capacity * sizeof(*grown))))
            return dc_fail(context, NULL, "native type annotation allocation failed");
        context->annotations = grown; context->annotation_capacity = capacity;
    }
    context->annotations[context->annotation_count++] = (dc_annotation){node, type}; return 1;
}

static int dc_annotation_compare(const void *a, const void *b)
{
    uintptr_t left = (uintptr_t)((const dc_annotation *)a)->node, right = (uintptr_t)((const dc_annotation *)b)->node;
    return left < right ? -1 : left != right;
}

static sh_decl_value_type dc_recorded_type(const dc_context *context, const dc_node *node)
{
    size_t low = 0, high = context->annotation_count;
    uintptr_t address = (uintptr_t)node;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        uintptr_t candidate = (uintptr_t)context->annotations[middle].node;
        if (candidate < address) low = middle + 1;
        else if (candidate > address) high = middle;
        else return context->annotations[middle].type;
    }
    return (sh_decl_value_type){0};
}

static int dc_compatible_type(dc_context *context, sh_decl_value_type a, sh_decl_value_type b, int compound)
{
    sh_decl_value_shape left, right;
    if (!a.name || !b.name) return a.name == b.name;
    if (!strcmp(a.name, b.name) && !strcmp(a.ops ? a.ops : "", b.ops ? b.ops : "")) return 1;
    if (!compound || !dc_shape(context, a, &left) || !dc_shape(context, b, &right)) return 0;
    /* Different reflected classes can retain compatible inherited members. */
    if (left.kind == SH_DECL_VALUE_OBJECT && right.kind == SH_DECL_VALUE_OBJECT) return 1;
    /* Native fixed positions keep the same identity across an extent change.
     * Each input and the result are independently checked against their bounds;
     * removing a now-invalid position still conflicts with an edit to it. */
    if (left.kind == SH_DECL_VALUE_COLLECTION && right.kind == SH_DECL_VALUE_COLLECTION &&
        left.count_known && right.count_known && !left.count_key && !right.count_key &&
        !strcmp(left.item_key ? left.item_key : "", right.item_key ? right.item_key : "") &&
        left.element.name && right.element.name && !strcmp(left.element.name, right.element.name) &&
        !strcmp(left.element.ops ? left.element.ops : "", right.element.ops ? right.element.ops : "")) return 1;
    return 0;
}

static int dc_typed_equal(const dc_node *a, const dc_node *b, dc_context *context)
{
    const dc_node *child;
    size_t ac = 0, bc = 0;
    if (!context->annotation_count) return dc_equal(a, b);
    if (!a || !b) return a == b;
    if (a->compound != b->compound || a->reset != b->reset || a->assignment != b->assignment ||
        !dc_compatible_type(context, dc_recorded_type(context, a), dc_recorded_type(context, b), a->compound)) return 0;
    if (!a->compound) return !strcmp(a->value, b->value);
    for (child = a->children; child; child = child->next) {
        if (!dc_typed_equal(child, dc_member(b, child->key), context)) return 0;
        ac++;
    }
    for (child = b->children; child; child = child->next) bc++;
    return ac == bc;
}

/* A native fixed array is a sparse set of positions, not a growable idList.
 * Require one spelling per position so two textual keys cannot alias an index.
 * Work is bounded by authored entries rather than by the native extent. */
static int dc_native_index(const char *key, const char *prefix, size_t *out)
{
    size_t length, index = 0;
    const char *cursor;
    if (!key || !prefix) return 0;
    length = strcspn(prefix, "[");
    if (strncmp(key, prefix, length) || key[length] != '[') return 0;
    cursor = key + length + 1;
    if (!isdigit((unsigned char)*cursor) || (*cursor == '0' && cursor[1] != ']')) return 0;
    while (isdigit((unsigned char)*cursor)) {
        unsigned digit = (unsigned)(*cursor++ - '0');
        if (index > (SIZE_MAX - digit) / 10) return 0;
        index = index * 10 + digit;
    }
    if (*cursor != ']' || cursor[1]) return 0;
    *out = index; return 1;
}

typedef struct dc_indexed_item { dc_node *node; size_t index; } dc_indexed_item;
static int dc_index_compare(const void *a, const void *b)
{
    size_t x = ((const dc_indexed_item *)a)->index, y = ((const dc_indexed_item *)b)->index;
    return x < y ? -1 : x > y;
}

/* A native inherited list can override selected slots without restating their
 * identities or count. Those slots are addresses into the parent's collection,
 * not an incomplete full list that the compiler may compact. */
static int dc_sparse_collection(const dc_node *list, const sh_decl_collection_rule *rule,
    const dc_context *context)
{
    const dc_node *num, *item;
    size_t count = 0;
    char *end;
    unsigned long long extent;
    if (!context->inherited_input || !list || !list->compound || list->reset) return 0;
    num = dc_member(list, "num");
    if (!num) return 1;
    if (num->compound || !num->value || !isdigit((unsigned char)*num->value)) return 0;
    errno = 0; extent = strtoull(num->value, &end, 10);
    if (*end || errno == ERANGE) return 0;
    for (item = list->children; item; item = item->next) if (strcmp(item->key, "num")) {
        count++;
        if (!rule->group_items && !dc_item_id(item, rule)) return 1;
        if (rule->group_items) {
            sh_decl_collection_rule members = {0};
            const dc_node *items = dc_member(item, rule->group_items);
            members.item_key = rule->group_identity;
            if (!items || dc_sparse_collection(items, &members, context)) return 1;
        }
    }
    return extent > count;
}

static int dc_sparse_valid(const dc_node *list, dc_context *context, const char *path)
{
    const dc_node *item, *num = dc_member(list, "num");
    unsigned long long extent = num ? strtoull(num->value, NULL, 10) : ULLONG_MAX;
    for (item = list->children; item; item = item->next) {
        size_t index;
        if (!strcmp(item->key, "num")) continue;
        if (!dc_native_index(item->key, "item", &index) || index >= extent)
            return dc_fail(context, path, "inherited collection has an invalid slot index");
    }
    return 1;
}

/* Only an identity adapter establishes that these numeric slots are storage,
 * not references from other engine objects. Repair its count/gaps in a private
 * tree and coalesce exact semantic duplicates. Fixed arrays stay untouched. */
static int dc_normalize_list(dc_node *list, const sh_decl_collection_rule *rule,
    dc_context *context, const char *path)
{
    dc_node *node, *num, **tail;
    dc_indexed_item *items = NULL;
    size_t count = 0, kept = 0, i, j;
    char value[40];
    if (dc_sparse_collection(list, rule, context)) return dc_sparse_valid(list, context, path);
    if (!rule->group_items && rule->item_key && !*rule->item_key) return 1;
    if (!list->compound) return dc_fail(context, path, "semantic collection requires a state block");
    for (node = list->children; node; node = node->next) if (strcmp(node->key, "num")) count++;
    if (count > SIZE_MAX / sizeof(*items) || !(items = calloc(count ? count : 1, sizeof(*items))))
        return dc_fail(context, path, "collection normalization allocation failed");
    for (node = list->children, i = 0; node; node = node->next) {
        if (!strcmp(node->key, "num")) continue;
        if (!dc_native_index(node->key, "item", &items[i].index) ||
            (!rule->group_items && !dc_item_id(node, rule))) {
            free(items); return dc_fail(context, path, "collection entry has no valid index or semantic identity");
        }
        items[i++].node = node;
    }
    qsort(items, count, sizeof(*items), dc_index_compare);
    for (i = 0; i < count; i++) {
        if (i && items[i].index == items[i-1].index) {
            free(items); return dc_fail(context, path, "duplicate collection index");
        }
        if (!rule->group_items) for (j = 0; j < i; j++) {
            if (!strcmp(dc_item_id(items[i].node, rule), dc_item_id(items[j].node, rule)) &&
                !dc_equal(items[i].node, items[j].node)) {
                free(items); return dc_fail(context, path, "different entries use the same collection identity");
            }
        }
    }
    num = dc_member(list, "num");
    if (num && num->compound) {
        free(items); return dc_fail(context, path, "collection count must be a scalar");
    }
    if (!num) {
        num = calloc(1, sizeof(*num));
        if (!num || !(num->key = dc_copy("num", 3))) {
            dc_free(num); free(items); return dc_fail(context, path, "collection count allocation failed");
        }
        num->next = list->children; list->children = num;
    }
    /* Validation above completes before any nodes are detached. */
    list->children = num; num->next = NULL; tail = &num->next;
    for (i = 0; i < count; i++) {
        node = items[i].node; node->next = NULL;
        if (!rule->group_items) {
            for (j = 0; j < kept; j++)
                if (!strcmp(dc_item_id(items[j].node, rule), dc_item_id(node, rule))) break;
            if (j < kept) { dc_free(node); continue; }
        }
        snprintf(value, sizeof(value), "item[%zu]", kept);
        char *key = dc_copy(value, strlen(value));
        if (!key) {
            *tail = node; tail = &node->next;
            for (j = i + 1; j < count; j++) { *tail = items[j].node; tail = &items[j].node->next; }
            *tail = NULL; free(items); return dc_fail(context, path, "collection index allocation failed");
        }
        free(node->key); node->key = key;
        *tail = node; tail = &node->next; items[kept++].node = node;
    }
    snprintf(value, sizeof(value), "%zu", kept);
    char *count_text = dc_copy(value, strlen(value));
    if (!count_text) { free(items); return dc_fail(context, path, "collection count allocation failed"); }
    dc_free(num->children); num->children = NULL;
    free(num->value); num->value = count_text; num->compound = num->reset = 0; num->assignment = 1;
    free(items); return 1;
}

static int dc_normalize(dc_node *node, dc_context *context, const char *path)
{
    const sh_decl_collection_rule *rule = dc_rule(context, path);
    dc_node *child;
    if (rule && !dc_normalize_list(node, rule, context, path)) return 0;
    if (rule && rule->group_items && !dc_sparse_collection(node, rule, context)) {
        sh_decl_collection_rule members = {0};
        dc_node *group, *other, *item;
        members.item_key = rule->group_identity;
        if (!members.item_key || !*members.item_key)
            return dc_fail(context, path, "grouped collection requires a member identity");
        for (group = node->children; group; group = group->next) {
            dc_node *items;
            if (!strcmp(group->key, "num")) continue;
            items = dc_member(group, rule->group_items);
            if (!items) return dc_fail(context, path, "group has no member collection");
            if (!dc_normalize_list(items, &members, context, path)) return 0;
            for (item = items->children; item; item = item->next) {
                if (!strcmp(item->key, "num")) continue;
                for (other = node->children; other != group; other = other->next) {
                    if (!strcmp(other->key, "num")) continue;
                    if (dc_find_item(dc_member(other, rule->group_items), dc_item_id(item, &members), &members))
                        return dc_fail(context, path, "member identity appears in multiple groups");
                }
            }
        }
    }
    for (child = node->children; child; child = child->next) {
        char nested[DC_PATH_CAP];
        if (!dc_join_path(nested, sizeof(nested), path, child->key))
            return dc_fail(context, path, "field path exceeds limit");
        if (!dc_normalize(child, context, nested)) return 0;
    }
    return 1;
}

static sh_decl_value_type dc_child_type(dc_context *context, sh_decl_value_type type,
    const dc_node *parent, const char *key)
{
    sh_decl_value_type child = {NULL, NULL};
    sh_decl_value_shape shape;
    const sh_decl_dependency_schema *schema;
    size_t index;
    if (!context->schema) return child;
    if (parent && !parent->key && !strcmp(key, "edit")) return context->state_type;
    if (!dc_shape(context, type, &shape)) return child;
    schema = context->schema->types;
    if (shape.kind == SH_DECL_VALUE_OBJECT) {
        if (schema->field(schema->context, type, key, &child) > 0 && child.name) return child;
        /* An unresolved field must not become an untyped recursive merge. */
        return (sh_decl_value_type){"", ""};
    }
    if (shape.kind == SH_DECL_VALUE_POLYMORPHIC) {
        sh_decl_polymorphic_state state;
        if (!strcmp(key, "className")) return child;
        if (!strcmp(key, "object") && sh_decl_polymorphic_read(parent, shape.element, schema, &state, NULL) == 1)
            return state.type;
        return (sh_decl_value_type){"", ""};
    }
    if (shape.kind == SH_DECL_VALUE_COLLECTION) {
        if (shape.count_key && !strcmp(key, shape.count_key)) return child;
        if (dc_native_index(key, shape.item_key ? shape.item_key : parent->key, &index))
            return shape.element.name ? shape.element : (sh_decl_value_type){"", ""};
        return (sh_decl_value_type){"", ""};
    }
    /* A custom reader owns its full block. Do not interpret its children. */
    return child;
}

static int dc_validate(const dc_node *node, dc_context *context, const char *path,
    sh_decl_value_type type)
{
    const dc_node *child;
    const sh_decl_collection_rule *rule = dc_rule(context, path);
    sh_decl_value_shape shape;
    int typed = dc_shape(context, type, &shape);
    if (!dc_record_type(context, node, type)) return 0;
    if (typed && shape.kind == SH_DECL_VALUE_POLYMORPHIC) {
        sh_decl_polymorphic_state state;
        char nested[DC_PATH_CAP];
        const char *reason;
        int status = sh_decl_polymorphic_read(node, shape.element, context->schema->types, &state, &reason);
        /* Unsupported forms remain whole values. Their children must not
         * acquire accidental untyped merge semantics. */
        if (status < 0) return dc_fail(context, path, reason);
        if (!status) return 1;
        if (!state.object) return 1;
        if (!dc_join_path(nested, sizeof(nested), path, "object")) return dc_fail(context, path, "field path exceeds limit");
        return dc_validate(state.object, context, nested, state.type);
    }
    if (rule && !dc_sparse_collection(node, rule, context) && !dc_list_valid(node, rule))
        return dc_fail(context, path, "invalid indexed collection or duplicate entry identity");
    if (rule && typed && (shape.kind != SH_DECL_VALUE_COLLECTION ||
        !shape.item_key || strcmp(shape.item_key, "item") ||
        !shape.count_key || strcmp(shape.count_key, "num")))
        return dc_fail(context, path, "collection adapter does not match the native reader");
    if (typed && shape.kind == SH_DECL_VALUE_COLLECTION &&
        shape.count_known && !shape.count_key) {
        if (!node || !node->compound) return dc_fail(context, path, "fixed native array requires a state block");
        for (child = node->children; child; child = child->next) {
            size_t index;
            if (!dc_native_index(child->key, shape.item_key ? shape.item_key : node->key, &index) ||
                index >= shape.count)
                return dc_fail(context, path, "fixed native array has an invalid member or out-of-range index");
        }
    }
    for (child = node ? node->children : NULL; child; child = child->next) {
        char nested[DC_PATH_CAP];
        if (!dc_join_path(nested, sizeof(nested), path, child->key)) return dc_fail(context, path, "field path exceeds limit");
        if (!dc_validate(child, context, nested, dc_child_type(context, type, node, child->key))) return 0;
    }
    return 1;
}

/* Whole-subtree shortcuts must preserve reader semantics too. The effective
 * parent can change outside this resource's contributions, so comparing only
 * the input trees does not establish compatibility with the result class. */
static int dc_clone_compatible(const dc_node *node, dc_context *context,
    const char *path, sh_decl_value_type type, size_t source)
{
    const dc_node *child;
    sh_decl_value_shape shape;
    if (!node || !context->annotation_count) return 1;
    if (type.name && !dc_compatible_type(context, dc_recorded_type(context, node), type, node->compound))
        return dc_fail_from(context, path, source == SIZE_MAX ?
            "native reader type changed without a replacement value" :
            "contribution uses a different native reader type", source, SIZE_MAX);
    if (dc_shape(context, type, &shape)) {
        if (shape.kind == SH_DECL_VALUE_POLYMORPHIC) {
            sh_decl_polymorphic_state state;
            char nested[DC_PATH_CAP];
            const char *reason;
            int status = sh_decl_polymorphic_read(node, shape.element, context->schema->types, &state, &reason);
            if (status < 0) return dc_fail(context, path, reason);
            if (!status || !state.object) return 1;
            if (!dc_join_path(nested, sizeof(nested), path, "object")) return dc_fail(context, path, "field path exceeds limit");
            return dc_clone_compatible(state.object, context, nested, state.type, source);
        }
        if (shape.kind != SH_DECL_VALUE_OBJECT && shape.kind != SH_DECL_VALUE_COLLECTION) return 1;
    }
    for (child = node->children; child; child = child->next) {
        char nested[DC_PATH_CAP];
        if (!dc_join_path(nested, sizeof(nested), path, child->key))
            return dc_fail(context, path, "field path exceeds limit");
        if (!dc_clone_compatible(child, context, nested,
            dc_child_type(context, type, node, child->key), source)) return 0;
    }
    return 1;
}

static dc_node *dc_clone_result(const dc_node *node, dc_context *context,
    const char *path, sh_decl_value_type type, size_t source)
{
    return dc_clone_compatible(node, context, path, type, source) ? dc_clone(node, context) : NULL;
}

static dc_node *dc_merge(const dc_node *base, const dc_node *const *sources, size_t source_count,
                          dc_context *context, const char *path, sh_decl_value_type type);

static dc_node *dc_header(const dc_node *source, dc_context *context)
{
    dc_node *result = (dc_node *)calloc(1, sizeof(*result));
    if (!result) goto failed;
    result->compound = source->compound; result->assignment = source->assignment;
    result->reset = source->reset;
    if (source->key && !(result->key = dc_copy(source->key, strlen(source->key)))) goto failed;
    return result;
failed:
    dc_free(result); dc_fail(context, NULL, "composition allocation failed"); return NULL;
}

static int dc_append_item(dc_node *list, dc_node *item, size_t *count, dc_context *context)
{
    dc_node **tail = &list->children;
    char key[40];
    if (!item) return !context->failed;
    snprintf(key, sizeof(key), "item[%zu]", (*count)++);
    free(item->key); item->key = dc_copy(key, strlen(key));
    if (!item->key) { dc_free(item); return dc_fail(context, NULL, "composition allocation failed"); }
    while (*tail) tail = &(*tail)->next;
    *tail = item;
    return 1;
}

static dc_node *dc_class_name(sh_decl_value_type type, dc_context *context)
{
    dc_node *node = (dc_node *)calloc(1, sizeof(*node));
    size_t length = strlen(type.name);
    if (!node || length > SIZE_MAX - 3) goto failed;
    node->key = dc_copy("className", 9); node->value = (char *)malloc(length + 3);
    node->assignment = 1;
    if (!node->key || !node->value) goto failed;
    node->value[0] = '"'; memcpy(node->value + 1, type.name, length);
    node->value[length + 1] = '"'; node->value[length + 2] = 0;
    return node;
failed:
    dc_free(node); dc_fail(context, NULL, "polymorphic class allocation failed"); return NULL;
}

static dc_node *dc_merge_polymorphic(const dc_node *base, const dc_node *const *sources,
    size_t count, dc_context *context, const char *path, sh_decl_value_type type,
    const sh_decl_value_shape *shape, int distinct, size_t first_change, size_t second_change)
{
    sh_decl_polymorphic_state original = {0}, state;
    sh_decl_value_type selected = {0};
    dc_node *base_class = NULL, *class_name = NULL, *object = NULL, *result = NULL;
    const dc_node **classes = NULL, **objects = NULL, *example = base;
    const char *reason = NULL, *name;
    size_t i, length;
    char nested[DC_PATH_CAP];
    if (!distinct) return dc_clone_result(sources[first_change], context, path, type, first_change);
    if (count > SIZE_MAX / sizeof(*classes) || !(classes = calloc(count, sizeof(*classes))) ||
        !(objects = calloc(count, sizeof(*objects)))) goto failed;
    if (base) {
        if (sh_decl_polymorphic_read(base, shape->element, context->schema->types, &original, &reason) != 1) goto unsupported;
        if (!(base_class = dc_class_name(original.type, context))) goto failed;
    }
    for (i = 0; i < count; i++) {
        if (!sources[i]) {
            if (base) {
                dc_fail_from(context, path, "polymorphic object deletion conflicts with another edit", first_change, second_change);
                goto failed;
            }
            continue;
        }
        if (!example) example = sources[i];
        if (sh_decl_polymorphic_read(sources[i], shape->element, context->schema->types, &state, &reason) != 1) goto unsupported;
        if (!(classes[i] = dc_class_name(state.type, context))) goto failed;
        objects[i] = state.object;
    }
    if (!dc_join_path(nested, sizeof(nested), path, "className")) goto failed;
    class_name = dc_merge(base_class, classes, count, context, nested, (sh_decl_value_type){0});
    if (!class_name || context->failed) goto failed;
    if (!sh_decl_tree_literal(class_name, &name, &length) ||
        context->schema->types->dynamic_type(context->schema->types->context, shape->element, name, length, &selected) != 1 ||
        !selected.name) {
        dc_fail(context, nested, "composed object class is unavailable or incompatible"); goto failed;
    }
    if (!dc_join_path(nested, sizeof(nested), path, "object")) goto failed;
    object = dc_merge(original.object, objects, count, context, nested, selected);
    if (context->failed || !(result = dc_header(example, context))) goto failed;
    result->compound = 1; result->reset = 0;
    result->children = class_name; class_name->next = object;
    class_name = object = NULL;
    goto done;
unsupported:
    dc_fail_from(context, path, reason ? reason : "polymorphic reader requires a verified composition adapter",
        first_change, second_change);
failed:
    if (!context->failed) dc_fail(context, path, "polymorphic composition allocation or field path limit exceeded");
    dc_free(result); result = NULL;
done:
    dc_free(base_class); dc_free(class_name); dc_free(object);
    if (classes) for (i = 0; i < count; i++) dc_free((dc_node *)classes[i]);
    free(classes); free(objects); return result;
}

typedef struct dc_entry {
    const char *id;
    dc_node *merged;
    size_t *positions; /* baseline, then each authored contribution; SIZE_MAX absent */
    size_t incoming;
    int emitted;
} dc_entry;

static int dc_entry_compare(const void *a, const void *b)
{ return strcmp(((const dc_entry *)a)->id, ((const dc_entry *)b)->id); }

static size_t dc_position(const dc_node *list, const char *id, const sh_decl_collection_rule *rule)
{
    const dc_node *item;
    size_t position = 0;
    for (item = list ? list->children : NULL; item; item = item->next) {
        const char *candidate;
        if (!strcmp(item->key, "num")) continue;
        candidate = dc_item_id(item, rule);
        if (candidate && !strcmp(candidate, id)) return position;
        position++;
    }
    return SIZE_MAX;
}

/* Baseline order is a default, not an extra author's vote. A reversal is an
 * authored edit. Relations involving additions come only from sources that
 * contain both entries. Keeping all source positions avoids inventing edges
 * between independent additions after an intermediate pairwise merge. */
static int dc_relation(const dc_entry *a, const dc_entry *b, size_t source_count,
                         size_t *first, size_t *second)
{
    size_t i, order_source = SIZE_MAX;
    int baseline = 0, order = 0;
    if (a->positions[0] != SIZE_MAX && b->positions[0] != SIZE_MAX)
        baseline = a->positions[0] < b->positions[0] ? -1 : 1;
    for (i = 1; i <= source_count; i++) {
        int next;
        if (a->positions[i] == SIZE_MAX || b->positions[i] == SIZE_MAX) continue;
        next = a->positions[i] < b->positions[i] ? -1 : 1;
        if (baseline) { if (next != baseline) return next; }
        else if (order && order != next) {
            if (first) *first = order_source;
            if (second) *second = i - 1;
            return 2;
        } else if (!order) { order = next; order_source = i - 1; }
    }
    return baseline ? baseline : order;
}

static int dc_entry_relation(dc_context *context, const char *path,
    const dc_entry *a, const dc_entry *b, size_t source_count,
    size_t *first, size_t *second)
{
    int authored = dc_relation(a, b, source_count, first, second), derived, reverse;
    if (authored == 2 || !context->order) return authored;
    derived = context->order->relation(context->order->context, path, a->merged, b->merged);
    if (derived < -1 || derived > 1) return 2;
    reverse = context->order->relation(context->order->context, path, b->merged, a->merged);
    if (reverse < -1 || reverse > 1 || derived != -reverse) return 2;
    if (derived && authored && derived != authored) return 2;
    return derived ? derived : authored;
}

static void dc_entry_error(dc_context *context, const char *id)
{
    size_t length;
    if (!context->error || !context->error_capacity || !id) return;
    length = strlen(context->error);
    if (length < context->error_capacity)
        snprintf(context->error + length, context->error_capacity - length, " (entry %s)", id);
}

/* Emit a semantic collection from already merged entries. The caller retains
 * positions and unconsumed nodes; emitted nodes transfer to the result. */
static dc_node *dc_emit_entries(const dc_node *first, dc_entry *entries, size_t entry_count,
    size_t source_count, dc_context *context, const char *path)
{
    dc_node *result = NULL, *num = NULL;
    const dc_node *item;
    size_t i, j, count = 0;
    /* Keep O(entries * sources) storage. Relations are recomputed during the
     * topological walk instead of allocating a quadratic adjacency matrix. */
    for (i = 0; i < entry_count; i++) if (entries[i].merged) {
        for (j = i + 1; j < entry_count; j++) if (entries[j].merged) {
            size_t first_source = SIZE_MAX, second_source = SIZE_MAX;
            int relation = dc_entry_relation(context, path, &entries[i], &entries[j], source_count, &first_source, &second_source);
            if (relation == 2) {
                dc_fail_from(context, path, "incompatible collection ordering between authored entries",
                    first_source, second_source);
                dc_entry_error(context, entries[i].id); dc_entry_error(context, entries[j].id); goto failed;
            }
            if (relation < 0) entries[j].incoming++;
            else if (relation > 0) entries[i].incoming++;
        }
    }
    result = dc_header(first, context); num = (dc_node *)calloc(1, sizeof(*num));
    if (!result || !num) goto allocation;
    result->children = num; num->key = dc_copy("num", 3); num->assignment = 1;
    if (!num->key) goto allocation;
    for (;;) {
        size_t next = SIZE_MAX;
        int remaining = 0;
        for (i = 0; i < entry_count; i++) if (entries[i].merged && !entries[i].emitted) {
            remaining = 1;
            if (!entries[i].incoming) { next = i; break; }
        }
        if (!remaining) break;
        if (next == SIZE_MAX) {
            if (context->conflict) context->conflict->all_sources = 1;
            dc_fail(context, path, "collection ordering constraints form a cycle"); goto failed;
        }
        entries[next].emitted = 1;
        for (j = 0; j < entry_count; j++) if (entries[j].merged && !entries[j].emitted &&
            dc_entry_relation(context, path, &entries[next], &entries[j], source_count, NULL, NULL) < 0) entries[j].incoming--;
        item = entries[next].merged; entries[next].merged = NULL;
        if (!dc_append_item(result, (dc_node *)item, &count, context)) goto failed;
    }
    {
        char number[40];
        snprintf(number, sizeof(number), "%zu", count);
        num->value = dc_copy(number, strlen(number));
        if (!num->value) goto allocation;
    }
    return result;
allocation:
    if (num && (!result || result->children != num)) dc_free(num);
    dc_fail(context, path, "composition allocation failed");
failed:
    dc_free(result); return NULL;
}

static int dc_deleted_order(dc_entry *entries, size_t entry_count, size_t source_count,
    dc_context *context, const char *path)
{
    size_t i, j, k;
    /* Removing an entry conflicts with moving that same baseline entry. New
     * neighbors alone do not count as a move of an existing entry. */
    for (i = 0; i < entry_count; i++) if (!entries[i].merged && entries[i].positions[0] != SIZE_MAX) {
        for (j = 0; j < entry_count; j++) if (i != j && entries[j].positions[0] != SIZE_MAX) {
            int original = entries[i].positions[0] < entries[j].positions[0];
            for (k = 1; k <= source_count; k++) if (entries[i].positions[k] != SIZE_MAX &&
                entries[j].positions[k] != SIZE_MAX &&
                original != (entries[i].positions[k] < entries[j].positions[k])) {
                size_t removed;
                for (removed = 1; removed <= source_count; removed++)
                    if (entries[i].positions[removed] == SIZE_MAX) break;
                dc_fail_from(context, path, "collection deletion conflicts with reordering",
                    removed <= source_count ? removed - 1 : SIZE_MAX, k - 1);
                dc_entry_error(context, entries[i].id); return 0;
            }
        }
    }
    return 1;
}

static dc_node *dc_merge_list(const dc_node *base, const dc_node *const *sources, size_t source_count,
    const sh_decl_collection_rule *rule, dc_context *context, const char *path, sh_decl_value_type type)
{
    const dc_node *first = base, *item, **members = NULL;
    dc_node *result = NULL;
    dc_entry *entries = NULL;
    size_t entry_count = 0, capacity = 0, i, j;
    for (i = 0; i < source_count; i++) if (!first && sources[i]) first = sources[i];
    if (!first) return NULL;
    if (source_count == SIZE_MAX || source_count > SIZE_MAX / sizeof(*members) ||
        source_count + 1 > SIZE_MAX / sizeof(size_t)) goto allocation;
    members = (const dc_node **)calloc(source_count, sizeof(*members));
    if (!members) goto allocation;
    for (i = 0; i <= source_count; i++) {
        const dc_node *list = i ? sources[i - 1] : base;
        if (!list) continue;
        if (!dc_list_valid(list, rule)) {
            dc_fail(context, path, "invalid indexed collection or duplicate entry identity"); goto failed;
        }
        if (rule->item_key && !rule->item_key[0] &&
            !dc_equal(dc_member(first, "num"), dc_member(list, "num"))) {
            dc_fail_from(context, path, "fixed positional collection ordering requires an unchanged extent",
                i ? i - 1 : SIZE_MAX, SIZE_MAX); goto failed;
        }
        for (item = list->children; item; item = item->next) {
            const char *id;
            if (!strcmp(item->key, "num")) continue;
            id = dc_item_id(item, rule);
            for (j = 0; j < entry_count; j++) if (!strcmp(entries[j].id, id)) break;
            if (j < entry_count) continue;
            if (entry_count == capacity) {
                size_t next = capacity ? capacity * 2 : 16;
                dc_entry *grown;
                if (next < capacity || next > SIZE_MAX / sizeof(*entries)) goto allocation;
                grown = (dc_entry *)realloc(entries, next * sizeof(*entries));
                if (!grown) goto allocation;
                entries = grown; capacity = next;
            }
            memset(&entries[entry_count], 0, sizeof(*entries));
            entries[entry_count++].id = id;
        }
    }
    if (entry_count > 1) qsort(entries, entry_count, sizeof(*entries), dc_entry_compare);
    for (i = 0; i < entry_count; i++) {
        const dc_node *original = dc_find_item(base, entries[i].id, rule), *example = original;
        char nested[DC_PATH_CAP];
        entries[i].positions = (size_t *)malloc((source_count + 1) * sizeof(size_t));
        if (!entries[i].positions) goto allocation;
        entries[i].positions[0] = dc_position(base, entries[i].id, rule);
        for (j = 0; j < source_count; j++) {
            members[j] = dc_find_item(sources[j], entries[i].id, rule);
            if (!example && members[j]) example = members[j];
            entries[i].positions[j + 1] = dc_position(sources[j], entries[i].id, rule);
        }
        /* Rules follow native indexed paths even when identity is a string.
         * Diagnostics additionally name the semantic entry being merged. */
        if (!example || !dc_join_path(nested, sizeof(nested), path, example->key)) {
            dc_fail(context, path, "field path exceeds limit"); goto failed;
        }
        entries[i].merged = dc_merge(original, members, source_count, context, nested,
            dc_child_type(context, type, first, example->key));
        if (context->failed) { dc_entry_error(context, entries[i].id); goto failed; }
    }
    if (!dc_deleted_order(entries, entry_count, source_count, context, path)) goto failed;
    result = dc_emit_entries(first, entries, entry_count, source_count, context, path);
    goto done;
allocation:
    dc_fail(context, path, "composition allocation failed");
failed:
    dc_free(result); result = NULL;
done:
    for (i = 0; i < entry_count; i++) { dc_free(entries[i].merged); free(entries[i].positions); }
    free(entries); free(members); return result;
}

static int dc_indexed(const dc_node *node)
{
    const dc_node *child;
    if (!node || !node->compound || !dc_member(node, "num")) return 0;
    if (!node->children->next) return 1;
    for (child = node->children; child; child = child->next)
        if (!strncmp(child->key, "item[", 5)) return 1;
    return 0;
}

/* Presentation groups partition one semantic collection. A member's value,
 * placement and order are independent edits. Numeric group slots describe the
 * layout, not the identity of the member being edited. */
static const dc_node *dc_group_member(const dc_node *list, const char *id,
    const sh_decl_collection_rule *rule, size_t *group, size_t *position)
{
    const dc_node *page;
    sh_decl_collection_rule members = {0};
    size_t index = 0;
    members.item_key = rule->group_identity;
    *group = *position = SIZE_MAX;
    for (page = list ? list->children : NULL; page; page = page->next) {
        const dc_node *items, *found;
        if (!strcmp(page->key, "num")) continue;
        items = dc_member(page, rule->group_items);
        found = dc_find_item(items, id, &members);
        if (found) {
            *group = index; *position = dc_position(items, id, &members); return found;
        }
        index++;
    }
    return NULL;
}

/* Temporarily omit the independently merged member list from private parsed
 * trees. Keeping the original nodes preserves native reader annotations. */
static dc_node *dc_group_metadata(const dc_node *base, const dc_node *const *sources,
    size_t count, const char *key, dc_context *context, const char *path, sh_decl_value_type type)
{
    dc_node ***links = NULL, **saved = NULL, *result = NULL;
    size_t i;
    if (count == SIZE_MAX || count + 1 > SIZE_MAX / sizeof(*links)) goto allocation;
    links = calloc(count + 1, sizeof(*links)); saved = calloc(count + 1, sizeof(*saved));
    if (!links || !saved) goto allocation;
    for (i = 0; i <= count; i++) {
        dc_node *node = (dc_node *)(i ? sources[i - 1] : base), **link;
        if (!node) continue;
        for (link = &node->children; *link; link = &(*link)->next) if (!strcmp((*link)->key, key)) {
            links[i] = link; saved[i] = *link; *link = (*link)->next; break;
        }
    }
    result = dc_merge(base, sources, count, context, path, type);
    for (i = 0; i <= count; i++) if (links[i]) *links[i] = saved[i];
    free(links); free(saved); return result;
allocation:
    free(links); free(saved); dc_fail(context, path, "group metadata allocation failed"); return NULL;
}

static dc_node *dc_merge_groups(const dc_node *base, const dc_node *const *sources, size_t source_count,
    const sh_decl_collection_rule *rule, dc_context *context, const char *path, sh_decl_value_type type)
{
    sh_decl_collection_rule member_rule = {0};
    const dc_node *first = base, **members = NULL;
    dc_entry *entries = NULL;
    dc_node **pages = NULL, *result = NULL;
    size_t *destinations = NULL, *groups = NULL;
    size_t entry_count = 0, page_count = 0, i, j, k, output_count = 0;
    member_rule.item_key = rule->group_identity;
    if (!rule->group_identity || !*rule->group_identity) {
        dc_fail(context, path, "grouped collection requires a member identity"); return NULL;
    }
    if (source_count == SIZE_MAX || source_count + 1 > SIZE_MAX / sizeof(*members)) goto allocation;
    members = calloc(source_count + 1, sizeof(*members));
    groups = malloc((source_count + 1) * sizeof(*groups));
    if (!members || !groups) goto allocation;
    for (i = 0; i <= source_count; i++) {
        const dc_node *list = i ? sources[i - 1] : base, *page;
        size_t n = 0;
        if (!first) first = list;
        for (page = list ? list->children : NULL; page; page = page->next) {
            const dc_node *items, *item;
            if (!strcmp(page->key, "num")) continue;
            n++; items = dc_member(page, rule->group_items);
            for (item = items ? items->children : NULL; item; item = item->next) {
                const char *id;
                dc_entry *grown;
                if (!strcmp(item->key, "num")) continue;
                id = dc_item_id(item, &member_rule);
                if (!id) {
                    dc_fail_from(context, path, "group member has no identity", i ? i - 1 : SIZE_MAX, SIZE_MAX); goto done;
                }
                for (j = 0; j < entry_count; j++) if (!strcmp(entries[j].id, id)) break;
                if (j < entry_count) continue;
                if (entry_count == SIZE_MAX / sizeof(*entries)) goto allocation;
                grown = realloc(entries, (entry_count + 1) * sizeof(*entries));
                if (!grown) goto allocation;
                entries = grown; memset(&entries[entry_count], 0, sizeof(*entries));
                entries[entry_count++].id = id;
            }
        }
        if (n > page_count) page_count = n;
    }
    if (page_count > SIZE_MAX / sizeof(*pages) || entry_count > SIZE_MAX / sizeof(*destinations)) goto allocation;
    pages = calloc(page_count ? page_count : 1, sizeof(*pages));
    destinations = malloc((entry_count ? entry_count : 1) * sizeof(*destinations));
    if (!pages || !destinations) goto allocation;
    if (entry_count > 1) qsort(entries, entry_count, sizeof(*entries), dc_entry_compare);
    for (i = 0; i < entry_count; i++) {
        const dc_node *original, *example, *example_page, *example_items;
        sh_decl_value_type page_type, list_type;
        size_t destination, mover = SIZE_MAX, removed = SIZE_MAX;
        char key[40], nested[DC_PATH_CAP], item_path[DC_PATH_CAP];
        entries[i].positions = malloc((source_count + 1) * sizeof(size_t));
        if (!entries[i].positions) goto allocation;
        original = dc_group_member(base, entries[i].id, rule, &groups[0], &entries[i].positions[0]);
        example = original;
        destination = groups[0];
        for (j = 0; j < source_count; j++) {
            members[j] = dc_group_member(sources[j], entries[i].id, rule, &groups[j+1], &entries[i].positions[j+1]);
            if (!example) example = members[j];
            if (!members[j]) { if (original) removed = j; continue; }
            if (groups[j+1] == groups[0]) continue;
            if (mover != SIZE_MAX && destination != groups[j+1]) {
                dc_fail_from(context, path, "member moved to different groups", mover, j);
                dc_entry_error(context, entries[i].id); goto done;
            }
            destination = groups[j+1]; mover = j;
        }
        if (removed != SIZE_MAX && mover != SIZE_MAX) {
            dc_fail_from(context, path, "member deletion conflicts with group placement", removed, mover);
            dc_entry_error(context, entries[i].id); goto done;
        }
        destinations[i] = destination;
        /* Only entries that coexisted in the destination can constrain its
         * ordering. Their old page cannot invent relations on a new page. */
        for (j = 0; j <= source_count; j++)
            if (groups[j] != destination) entries[i].positions[j] = SIZE_MAX;
        k = original ? 0 : 1;
        while (k <= source_count && groups[k] == SIZE_MAX) k++;
        if (k > source_count) goto allocation;
        snprintf(key, sizeof(key), "item[%zu]", groups[k]);
        example_page = dc_member(k ? sources[k-1] : base, key);
        example_items = dc_member(example_page, rule->group_items);
        page_type = dc_child_type(context, type, first, key);
        list_type = dc_child_type(context, page_type, example_page, rule->group_items);
        if (!dc_join_path(nested, sizeof(nested), path, key) ||
            !dc_join_path(item_path, sizeof(item_path), nested, rule->group_items) ||
            !dc_join_path(nested, sizeof(nested), item_path, example->key)) goto allocation;
        entries[i].merged = dc_merge(original, members, source_count, context, nested,
            dc_child_type(context, list_type, example_items, example->key));
        if (context->failed) { dc_entry_error(context, entries[i].id); goto done; }
    }
    /* Deleted members still participate in move/delete validation. Check
     * each partition separately so positions on different pages never compare. */
    for (i = 0; i < page_count; i++) {
        dc_entry *selected = calloc(entry_count ? entry_count : 1, sizeof(*selected));
        size_t n = 0;
        if (!selected) goto allocation;
        for (j = 0; j < entry_count; j++) if (destinations[j] == i) selected[n++] = entries[j];
        int valid = dc_deleted_order(selected, n, source_count, context, path);
        free(selected);
        if (!valid) goto done;
    }
    for (i = 0; i < page_count; i++) {
        const dc_node *original, *example;
        dc_node *items;
        dc_entry *selected;
        size_t selected_count = 0;
        char key[40], nested[DC_PATH_CAP], list_path[DC_PATH_CAP];
        snprintf(key, sizeof(key), "item[%zu]", i);
        original = dc_member(base, key); example = original;
        for (j = 0; j < source_count; j++) {
            members[j] = dc_member(sources[j], key);
            if (!example) example = members[j];
        }
        if (!dc_join_path(nested, sizeof(nested), path, key) ||
            !dc_join_path(list_path, sizeof(list_path), nested, rule->group_items)) goto allocation;
        pages[i] = dc_group_metadata(original, members, source_count, rule->group_items, context, nested,
            dc_child_type(context, type, first, key));
        if (context->failed) goto done;
        for (j = 0; j < entry_count; j++) if (entries[j].merged && destinations[j] == i) selected_count++;
        if (!pages[i]) {
            if (selected_count) {
                if (context->conflict) context->conflict->all_sources = 1;
                dc_fail(context, nested, "removed group still contains authored members"); goto done;
            }
            continue;
        }
        selected = calloc(selected_count ? selected_count : 1, sizeof(*selected));
        if (!selected) goto allocation;
        for (j = 0, k = 0; j < entry_count; j++) if (entries[j].merged && destinations[j] == i) {
            selected[k++] = entries[j]; entries[j].merged = NULL;
        }
        /* Even an empty authored group retains its native list header. */
        example = dc_member(example, rule->group_items);
        if (!example) {
            dc_fail(context, list_path, "group has no member collection"); items = NULL;
        } else items = dc_emit_entries(example, selected, selected_count, source_count, context, list_path);
        for (j = 0; j < selected_count; j++) dc_free(selected[j].merged);
        free(selected);
        if (!items) goto done;
        items->next = pages[i]->children; pages[i]->children = items;
    }
    result = dc_emit_entries(first, NULL, 0, source_count, context, path);
    if (!result) goto done;
    for (i = 0; i < page_count; i++) if (pages[i]) {
        dc_node *page = pages[i]; pages[i] = NULL;
        if (!dc_append_item(result, page, &output_count, context)) goto done;
    }
    if (!dc_normalize_list(result, rule, context, path)) goto done;
    goto done;
allocation:
    dc_fail(context, path, "grouped collection allocation or field path limit exceeded");
done:
    for (i = 0; i < entry_count; i++) { dc_free(entries[i].merged); free(entries[i].positions); }
    if (pages) for (i = 0; i < page_count; i++) dc_free(pages[i]);
    free(entries); free(pages); free(members); free(groups); free(destinations);
    if (context->failed) { dc_free(result); result = NULL; }
    return result;
}

static dc_node *dc_merge(const dc_node *base, const dc_node *const *sources, size_t source_count,
                          dc_context *context, const char *path, sh_decl_value_type type)
{
    const dc_node *change = NULL, *child, *example = base, **members = NULL;
    const sh_decl_collection_rule *rule;
    sh_decl_value_shape shape;
    dc_node *result = NULL, **tail;
    size_t i, first_change = SIZE_MAX, second_change = SIZE_MAX;
    int changed = 0, distinct = 0, all_compound = 1, indexed = dc_indexed(base), sparse = 0;
    if (context->annotation_count && type.name) {
        int replacing = base && !dc_compatible_type(context, dc_recorded_type(context, base), type, base->compound);
        const dc_node **converted = NULL;
        size_t conversion_changes = 0;
        if (replacing) {
            if (source_count > SIZE_MAX / sizeof(*converted) ||
                !(converted = (const dc_node **)calloc(source_count, sizeof(*converted)))) {
                dc_fail(context, path, "native type conversion allocation failed"); return NULL;
            }
        }
        for (i = 0; i < source_count; i++) {
            const dc_node *source = sources[i];
            if (dc_typed_equal(base, source, context)) continue;
            conversion_changes++;
            if (source && !dc_compatible_type(context, dc_recorded_type(context, source), type, source->compound)) {
                free(converted);
                dc_fail_from(context, path, "contribution uses a different native reader type", i, SIZE_MAX); return NULL;
            }
            if (converted) converted[i] = source;
        }
        if (converted) {
            /* Old values are not defaults for a different native reader. Only
             * contributions that actually replace their semantics participate. */
            result = dc_merge(NULL, converted, source_count, context, path, type);
            free(converted);
            if (!conversion_changes && !context->failed)
                dc_fail(context, path, "native reader type changed without a replacement value");
            return result;
        }
    }
    for (i = 0; i < source_count; i++) if (!dc_typed_equal(base, sources[i], context)) {
        if (!changed) { change = sources[i]; changed = 1; first_change = i; }
        else if (!dc_typed_equal(change, sources[i], context)) {
            if (!distinct) second_change = i;
            distinct = 1;
        }
    }
    if (!changed) {
        /* A format ordering contract also applies to unchanged collections.
         * Traverse their containers instead of bypassing it with a root clone. */
        if (!context->order || !base || !base->compound)
            return dc_clone_result(base, context, path, type, SIZE_MAX);
        change = base;
    }
    if (dc_shape(context, type, &shape) && shape.kind == SH_DECL_VALUE_POLYMORPHIC)
        return dc_merge_polymorphic(base, sources, source_count, context, path, type, &shape,
            distinct, first_change, second_change);
    if (!example) example = change;
    if (!example || !example->compound) all_compound = 0;
    for (i = 0; i < source_count; i++) {
        const dc_node *source = sources[i];
        if (dc_indexed(source)) indexed = 1;
        if (!source && !base) continue; /* no contribution to a new object */
        if (!source || !example || !source->compound || source->reset != example->reset ||
            source->assignment != example->assignment) all_compound = 0;
    }
    if (!all_compound) {
        if (!distinct) return dc_clone_result(change, context, path, type, first_change);
        if (context->conflicting_sources && context->conflict) {
            context->conflict->precise_sources = 1;
            for (i = 0; i < source_count; i++)
                context->conflicting_sources[i] = !dc_typed_equal(base, sources[i], context);
        }
        dc_fail_from(context, path, "incompatible edits to the same field", first_change, second_change); return NULL;
    }
    rule = dc_rule(context, path);
    if (rule) {
        int complete = base && !dc_sparse_collection(base, rule, context);
        sparse = dc_sparse_collection(base, rule, context);
        for (i = 0; i < source_count; i++) if (sources[i]) {
            if (dc_sparse_collection(sources[i], rule, context)) sparse = 1;
            else complete = 1;
        }
        if (sparse && (complete || context->inherited_layout_changed)) {
            if (context->conflict) context->conflict->all_sources = 1;
            dc_fail(context, path, "inherited collection composition requires a consistent parent layout"); return NULL;
        }
        if (sparse) { rule = NULL; indexed = 0; }
    }
    if (rule && rule->group_items) return dc_merge_groups(base, sources, source_count, rule, context, path, type);
    if (rule) return dc_merge_list(base, sources, source_count, rule, context, path, type);
    if (!sparse && dc_shape(context, type, &shape)) {
        if (shape.kind == SH_DECL_VALUE_COLLECTION) {
            /* Only native fixed positions compose without an identity adapter.
             * A dynamic collection remains a collection even without num. */
            indexed = !(shape.count_known && !shape.count_key);
        } else if (shape.kind != SH_DECL_VALUE_OBJECT) {
            if (!distinct) return dc_clone_result(change, context, path, type, first_change);
            dc_fail_from(context, path, "native reader requires a verified composition adapter",
                first_change, second_change); return NULL;
        }
    }
    /* Unsupported collection semantics can pass one whole replacement, but
     * must never be inferred from coincidentally matching numeric indices. */
    if (indexed) {
        if (!distinct) return dc_clone_result(change, context, path, type, first_change);
        dc_fail_from(context, path, "indexed collection requires a supported composition adapter",
            first_change, second_change); return NULL;
    }
    result = dc_header(example, context);
    if (!result || source_count > SIZE_MAX / sizeof(*members)) goto failed;
    members = (const dc_node **)calloc(source_count, sizeof(*members));
    if (!members) goto failed;
    tail = &result->children;
    for (child = base ? base->children : NULL; child; child = child->next) {
        char nested[DC_PATH_CAP];
        if (!dc_join_path(nested, sizeof(nested), path, child->key)) goto failed;
        for (i = 0; i < source_count; i++) members[i] = dc_member(sources[i], child->key);
        *tail = dc_merge(child, members, source_count, context, nested,
            dc_child_type(context, type, example, child->key));
        if (context->failed) goto failed;
        if (*tail) tail = &(*tail)->next;
    }
    {
        const char *previous = NULL;
        for (;;) {
            const char *next = NULL;
            for (i = 0; i < source_count; i++) {
                for (child = sources[i] ? sources[i]->children : NULL; child; child = child->next) {
                    if (dc_member(base, child->key) || (previous && strcmp(child->key, previous) <= 0)) continue;
                    if (!next || strcmp(child->key, next) < 0) next = child->key;
                }
            }
            if (!next) break;
            {
                char nested[DC_PATH_CAP];
                if (!dc_join_path(nested, sizeof(nested), path, next)) goto failed;
                for (i = 0; i < source_count; i++) members[i] = dc_member(sources[i], next);
                *tail = dc_merge(NULL, members, source_count, context, nested,
                    dc_child_type(context, type, example, next));
                if (!*tail || context->failed) goto failed;
                tail = &(*tail)->next;
            }
            previous = next;
        }
    }
    free(members); return result;
failed:
    if (!context->failed) dc_fail(context, path, "composition allocation or field path limit exceeded");
    free(members); dc_free(result); return NULL;
}

static int dc_append(dc_buffer *buffer, const char *text, size_t length)
{
    size_t capacity;
    char *grown;
    if (length >= SIZE_MAX - buffer->length) return 0;
    if (buffer->length + length + 1u > buffer->capacity) {
        capacity = buffer->capacity ? buffer->capacity : 256u;
        while (capacity < buffer->length + length + 1u) {
            if (capacity > SIZE_MAX / 2u) { capacity = buffer->length + length + 1u; break; }
            capacity *= 2u;
        }
        grown = (char *)realloc(buffer->text, capacity);
        if (!grown) return 0;
        buffer->text = grown; buffer->capacity = capacity;
    }
    memcpy(buffer->text + buffer->length, text, length);
    buffer->length += length; buffer->text[buffer->length] = '\0';
    return 1;
}

static int dc_emit(dc_buffer *buffer, const dc_node *node)
{
    const dc_node *child;
    if (!dc_append(buffer, "{\n", 2)) return 0;
    for (child = node->children; child; child = child->next) {
        if (!dc_append(buffer, child->key, strlen(child->key)) ||
            !dc_append(buffer, child->assignment ? " = " : " ", child->assignment ? 3u : 1u)) return 0;
        if (child->reset && !dc_append(buffer, "! ", 2)) return 0;
        if (child->compound) {
            if (!dc_emit(buffer, child) || !dc_append(buffer, "\n", 1)) return 0;
        } else if (!dc_append(buffer, child->value, strlen(child->value)) || !dc_append(buffer, ";\n", 2)) return 0;
    }
    return dc_append(buffer, "}", 1);
}

/* The entity parser consumes this preamble sequentially, before its state
 * reader starts. Original ordering cannot anchor a header absent from the
 * baseline, and an empty baseline must not alphabetize inherit after edit. */
static void dc_entity_headers(dc_node *root)
{
    static const char *const keys[] = {
        "inherit", "class", "expandInheritance", "poolCount", "poolGranularity", "editorVars"
    };
    dc_node *ordered = NULL, **tail = &ordered;
    size_t i;
    for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        dc_node **link = &root->children;
        while (*link && strcmp((*link)->key, keys[i])) link = &(*link)->next;
        if (*link) {
            dc_node *node = *link;
            *link = node->next; node->next = NULL;
            *tail = node; tail = &node->next;
        }
    }
    *tail = root->children; root->children = ordered;
}

/* idDeclTypeInfo reads inherit before handing edit to the reflected reader.
 * A newly added parent cannot be left after edit by alphabetical key order. */
static void dc_reflected_headers(dc_node *root)
{
    dc_node **link = &root->children;
    while (*link && strcmp((*link)->key, "inherit")) link = &(*link)->next;
    if (*link) {
        dc_node *node = *link;
        *link = node->next; node->next = root->children; root->children = node;
    }
}

static int dc_resolve_state(dc_context *context, const dc_node *definition,
    sh_decl_composition_role role, size_t index)
{
    const sh_decl_composition_schema *schema = context->schema;
    int result;
    context->state_type = (sh_decl_value_type){0};
    if (!schema) return 1;
    if (!schema->resolve) { context->state_type = schema->state_type; return 1; }
    if (role == SH_DECL_COMPOSITION_ORIGINAL && !definition->children) return 1;
    result = schema->resolve(schema->context, definition, role, index, &context->state_type,
        context->error, context->error_capacity);
    if (result == 1 && context->state_type.name && *context->state_type.name) return 1;
    if (!result && role != SH_DECL_COMPOSITION_RESULT && !dc_member(definition, "edit")) {
        context->state_type = (sh_decl_value_type){0}; return 1;
    }
    if (!context->error || !context->error_capacity || !context->error[0])
        dc_fail(context, "edit", "native state class could not be resolved");
    context->failed = 1;
    if (context->conflict && role == SH_DECL_COMPOSITION_CONTRIBUTION) {
        context->conflict->first = index; context->conflict->second = SIZE_MAX;
    }
    return 0;
}

static dc_node *dc_scalar_metadata(const dc_node *definition, dc_context *context)
{
    const dc_node *child;
    dc_node *root = dc_header(definition, context), **tail;
    if (!root) return NULL;
    tail = &root->children;
    for (child = definition->children; child; child = child->next) if (!child->compound) {
        *tail = dc_clone(child, context);
        if (!*tail) { dc_free(root); return NULL; }
        tail = &(*tail)->next;
    }
    return root;
}

static dc_node *dc_result_metadata(const dc_node *base, const dc_node *const *inputs,
    size_t count, dc_context *context)
{
    dc_context untyped = {0};
    dc_node *original = NULL, *result = NULL;
    const dc_node **sources = NULL;
    size_t i;
    untyped.error = context->error; untyped.error_capacity = context->error_capacity;
    untyped.conflict = context->conflict;
    if (count > SIZE_MAX / sizeof(*sources) ||
        !(sources = (const dc_node **)calloc(count, sizeof(*sources)))) goto done;
    original = dc_scalar_metadata(base, &untyped);
    if (!original) goto done;
    for (i = 0; i < count; i++) {
        sources[i] = dc_scalar_metadata(inputs[i], &untyped);
        if (!sources[i]) goto done;
    }
    result = dc_merge(original, sources, count, &untyped, "", (sh_decl_value_type){0});
done:
    dc_free(original);
    if (sources) for (i = 0; i < count; i++) dc_free((dc_node *)sources[i]);
    free(sources);
    if (!result) {
        if (untyped.failed) context->failed = 1;
        else dc_fail(context, NULL, "native class metadata composition failed");
    }
    return result;
}

char *sh_decl_compose_root_metadata(sh_decl_source baseline,
    const sh_decl_source *sources, size_t count, size_t *out_length,
    char *error, size_t error_capacity, sh_decl_conflict *conflict)
{
    dc_context context = {0};
    dc_node *base = NULL, *result = NULL;
    const dc_node **inputs = NULL;
    dc_buffer buffer = {0};
    size_t i;
    if (out_length) *out_length = 0;
    if (error && error_capacity) error[0] = 0;
    if (conflict) { conflict->first = conflict->second = SIZE_MAX; conflict->all_sources = conflict->precise_sources = 0; }
    context.error = error; context.error_capacity = error_capacity; context.conflict = conflict;
    if (!out_length || !sources || !count || count > SIZE_MAX / sizeof(*inputs)) {
        dc_fail(&context, NULL, "invalid metadata composition inputs"); return NULL;
    }
    base = sh_decl_tree_parse(baseline, NULL, 0);
    if (!base) { dc_fail(&context, NULL, "baseline has unsupported or malformed syntax"); goto done; }
    inputs = (const dc_node **)calloc(count, sizeof(*inputs));
    if (!inputs) { dc_fail(&context, NULL, "metadata composition allocation failed"); goto done; }
    for (i = 0; i < count; i++) {
        inputs[i] = sh_decl_tree_parse(sources[i], NULL, 0);
        if (!inputs[i]) {
            dc_fail_from(&context, NULL, "source has unsupported or malformed syntax", i, SIZE_MAX); goto done;
        }
    }
    result = dc_result_metadata(base, inputs, count, &context);
    if (!result) goto done;
    dc_entity_headers(result);
    if (!dc_emit(&buffer, result) || !dc_append(&buffer, "\n", 1)) {
        dc_fail(&context, NULL, "metadata output allocation failed"); goto done;
    }
    *out_length = buffer.length;
done:
    if (inputs) for (i = 0; i < count; i++) dc_free((dc_node *)inputs[i]);
    free(inputs); dc_free(base); dc_free(result);
    if (context.failed || !*out_length) { free(buffer.text); return NULL; }
    return buffer.text;
}

static int dc_same_order(const dc_node *a, const dc_node *b)
{
    const dc_node *left = a->children, *right = b->children;
    for (; left && right; left = left->next, right = right->next)
        if (strcmp(left->key, right->key) || !dc_same_order(left, right)) return 0;
    return !left && !right;
}

int sh_decl_normalize_collections(sh_decl_source source,
    const sh_decl_collection_rule *rules, size_t rule_count, char **body,
    size_t *length, char *error, size_t error_capacity)
{
    dc_context context = {0};
    dc_node *original = NULL, *normalized = NULL;
    dc_buffer buffer = {0};
    int ok = 0;
    context.rules = rules; context.rule_count = rule_count;
    context.error = error; context.error_capacity = error_capacity;
    *body = NULL; *length = 0;
    if (error && error_capacity) *error = 0;
    original = dc_parse(source); normalized = dc_parse(source);
    if (!original || !normalized) {
        dc_fail(&context, NULL, "source has unsupported or malformed syntax"); goto done;
    }
    context.inherited_input = dc_member(original, "inherit") != NULL;
    if (!dc_normalize(normalized, &context, "") ||
        !dc_validate(normalized, &context, "", (sh_decl_value_type){0})) goto done;
    if (dc_equal(original, normalized) && dc_same_order(original, normalized)) { ok = 1; goto done; }
    if (!dc_emit(&buffer, normalized) || !dc_append(&buffer, "\n", 1)) {
        dc_fail(&context, NULL, "normalized collection output allocation failed"); goto done;
    }
    *body = buffer.text; *length = buffer.length; buffer.text = NULL; ok = 1;
done:
    dc_free(original); dc_free(normalized); free(buffer.text); return ok;
}

static char *dc_compose_resource(const char *declaration_type,
    sh_decl_source baseline, const sh_decl_source *sources,
    size_t count, const sh_decl_collection_rule *rules, size_t rule_count,
    const sh_decl_composition_schema *schema, const sh_decl_collection_order *order, size_t *out_length,
    char *error, size_t error_capacity, sh_decl_conflict *conflict, unsigned char *conflicting_sources)
{
    dc_context context = {0};
    dc_node *base = NULL, *combined = NULL, *metadata = NULL;
    const dc_node **inputs = NULL;
    dc_buffer buffer = {0};
    size_t i;
    context.rules = rules; context.rule_count = rule_count;
    context.error = error; context.error_capacity = error_capacity;
    context.conflict = conflict; context.schema = schema; context.order = order;
    context.conflicting_sources = conflicting_sources;
    if (conflicting_sources) memset(conflicting_sources, 0, count);
    if (out_length) *out_length = 0;
    if (conflict) { conflict->first = conflict->second = SIZE_MAX; conflict->all_sources = conflict->precise_sources = 0; }
    if (error && error_capacity) error[0] = '\0';
    if (!out_length || !sources || !count || (rule_count && !rules) || (order && !order->relation)) {
        dc_fail(&context, NULL, "invalid composition inputs"); return NULL;
    }
    if (schema && (!schema->types || !schema->types->describe || !schema->types->field ||
        (!schema->resolve && (!schema->state_type.name || !*schema->state_type.name)))) {
        dc_fail(&context, NULL, "native composition schema or state type is unavailable"); return NULL;
    }
    for (i = 0; i < rule_count; i++) if (!rules[i].path) {
        dc_fail(&context, NULL, "invalid collection adapter"); return NULL;
    }
    base = dc_parse(baseline);
    if (!base) { dc_fail(&context, NULL, "baseline has unsupported or malformed syntax"); goto done; }
    context.inherited_input = dc_member(base, "inherit") != NULL;
    if (!dc_normalize(base, &context, "")) goto done;
    context.record_types = schema != NULL;
    if (!dc_resolve_state(&context, base, SH_DECL_COMPOSITION_ORIGINAL, 0)) goto done;
    if (!dc_validate(base, &context, "", (sh_decl_value_type){0})) goto done;
    if (count > SIZE_MAX / sizeof(*inputs) ||
        !(inputs = (const dc_node **)calloc(count, sizeof(*inputs)))) {
        dc_fail(&context, NULL, "composition allocation failed"); goto done;
    }
    for (i = 0; i < count; i++) {
        inputs[i] = dc_parse(sources[i]);
        if (!inputs[i]) {
            dc_fail_from(&context, NULL, "source has unsupported or malformed syntax", i, SIZE_MAX); goto done;
        }
        context.inherited_input = dc_member(inputs[i], "inherit") != NULL;
        if (!dc_normalize((dc_node *)inputs[i], &context, "") ||
            !dc_resolve_state(&context, inputs[i], SH_DECL_COMPOSITION_CONTRIBUTION, i) ||
            !dc_validate(inputs[i], &context, "", (sh_decl_value_type){0})) {
            if (conflict) conflict->first = i;
            goto done;
        }
    }
    context.record_types = 0;
    {
        const dc_node *parent = dc_member(base, "inherit");
        context.inherited_input = parent != NULL;
        for (i = 0; i < count; i++) {
            const dc_node *next = dc_member(inputs[i], "inherit");
            if (!parent) parent = next;
            if (next) context.inherited_input = 1;
            if (parent && !dc_equal(parent, next)) context.inherited_layout_changed = 1;
        }
    }
    if (context.annotation_count > 1)
        qsort(context.annotations, context.annotation_count, sizeof(*context.annotations), dc_annotation_compare);
    if (schema && schema->resolve) {
        metadata = dc_result_metadata(base, inputs, count, &context);
        if (!metadata || !dc_resolve_state(&context, metadata, SH_DECL_COMPOSITION_RESULT, 0)) goto done;
    } else if (schema) context.state_type = schema->state_type;
    combined = dc_merge(base, inputs, count, &context, "", (sh_decl_value_type){0});
    if (!combined || context.failed) goto done;
    if (declaration_type && !_stricmp(declaration_type, "entitydef")) dc_entity_headers(combined);
    else if (declaration_type && schema) dc_reflected_headers(combined);
    context.inherited_input = dc_member(combined, "inherit") != NULL;
    if (!dc_validate(combined, &context, "", (sh_decl_value_type){0}) || !dc_emit(&buffer, combined) ||
        !dc_append(&buffer, "\n", 1)) {
        dc_fail(&context, NULL, "compiled declaration exceeds output limits"); goto done;
    }
    *out_length = buffer.length;
done:
    if (inputs) for (i = 0; i < count; i++) dc_free((dc_node *)inputs[i]);
    free(inputs);
    dc_free(base); dc_free(combined); dc_free(metadata); free(context.annotations);
    if (context.failed || !*out_length) { free(buffer.text); return NULL; }
    return buffer.text;
}

char *sh_decl_compose_resource(const char *declaration_type,
    sh_decl_source baseline, const sh_decl_source *sources,
    size_t count, const sh_decl_collection_rule *rules, size_t rule_count,
    const sh_decl_composition_schema *schema, size_t *out_length,
    char *error, size_t error_capacity, sh_decl_conflict *conflict)
{
    return dc_compose_resource(declaration_type, baseline, sources, count, rules,
        rule_count, schema, NULL, out_length, error, error_capacity, conflict, NULL);
}

char *sh_decl_compose_resource_report(const char *declaration_type,
    sh_decl_source baseline, const sh_decl_source *sources,
    size_t count, const sh_decl_collection_rule *rules, size_t rule_count,
    const sh_decl_composition_schema *schema, size_t *out_length,
    char *error, size_t error_capacity, sh_decl_conflict *conflict,
    unsigned char *conflicting_sources)
{
    return dc_compose_resource(declaration_type, baseline, sources, count, rules,
        rule_count, schema, NULL, out_length, error, error_capacity, conflict, conflicting_sources);
}

char *sh_decl_compose_ordered(sh_decl_source baseline, const sh_decl_source *sources,
    size_t count, const sh_decl_collection_rule *rules, size_t rule_count,
    const sh_decl_collection_order *order, size_t *out_length,
    char *error, size_t error_capacity, sh_decl_conflict *conflict)
{
    return dc_compose_resource(NULL, baseline, sources, count, rules, rule_count,
        NULL, order, out_length, error, error_capacity, conflict, NULL);
}

char *sh_decl_compose_typed(sh_decl_source baseline, const sh_decl_source *sources,
    size_t count, const sh_decl_collection_rule *rules, size_t rule_count,
    const sh_decl_composition_schema *schema, size_t *out_length,
    char *error, size_t error_capacity, sh_decl_conflict *conflict)
{
    return sh_decl_compose_resource(NULL, baseline, sources, count, rules, rule_count,
        schema, out_length, error, error_capacity, conflict);
}

char *sh_decl_compose(sh_decl_source baseline, const sh_decl_source *sources,
    size_t count, const sh_decl_collection_rule *rules, size_t rule_count, size_t *out_length,
    char *error, size_t error_capacity, sh_decl_conflict *conflict)
{
    return sh_decl_compose_typed(baseline, sources, count, rules, rule_count, NULL,
        out_length, error, error_capacity, conflict);
}
