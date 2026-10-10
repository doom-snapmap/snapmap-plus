#include "decl_dependencies.h"
#include "decl_polymorphic.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct dd_pending {
    const sh_decl_node *node;
    sh_decl_value_type type;
    char *path;
} dd_pending;

typedef struct dd_walk {
    const sh_decl_dependency_schema *schema;
    sh_decl_dependency_result *result;
    dd_pending *pending;
    size_t count, capacity;
} dd_walk;

static int dd_gap(dd_walk *walk, const char *path, sh_decl_value_type type, const char *reason)
{
    walk->result->gaps++;
    if (walk->schema->gap) walk->schema->gap(walk->schema->context, path, type, reason);
    return 1;
}

static char *dd_path(const char *parent, const char *key)
{
    size_t a = parent ? strlen(parent) : 0, b = strlen(key);
    char *path;
    if (a > SIZE_MAX - b || a + b > SIZE_MAX - 2) return NULL;
    path = (char *)malloc(a + b + 2);
    if (!path) return NULL;
    if (a) { memcpy(path, parent, a); path[a++] = '.'; }
    memcpy(path + a, key, b + 1);
    return path;
}

static int dd_push(dd_walk *walk, const sh_decl_node *node, sh_decl_value_type type,
                    const char *parent, const char *key)
{
    dd_pending *pending;
    char *path;
    if (walk->count == walk->capacity) {
        size_t capacity = walk->capacity ? walk->capacity * 2 : 16;
        if (capacity < walk->capacity || capacity > SIZE_MAX / sizeof(*pending)) return 0;
        pending = (dd_pending *)realloc(walk->pending, capacity * sizeof(*pending));
        if (!pending) return 0;
        walk->pending = pending; walk->capacity = capacity;
    }
    path = dd_path(parent, key);
    if (!path) return 0;
    pending = &walk->pending[walk->count++];
    pending->node = node; pending->type = type; pending->path = path;
    return 1;
}

int sh_decl_reference_literal(const sh_decl_node *node, const char **name, size_t *length)
{
    if (!sh_decl_tree_literal(node, name, length)) return 0;
    if (node->value && !strcmp(node->value, "NULL")) { *name = ""; *length = 0; }
    return 1;
}

static int dd_reference(dd_walk *walk, const sh_decl_node *node,
                         const char *path, sh_decl_value_type type)
{
    const char *name;
    size_t length;
    if (!sh_decl_reference_literal(node, &name, &length))
        return dd_gap(walk, path, type, "resource value requires a native syntax adapter");
    if (!length) return 1;
    if (!walk->schema->reference(walk->schema->context, path, type, name, length)) return 0;
    if (walk->schema->reference_site) {
        sh_decl_reference_site site = {node, NULL, 0};
        if (!walk->schema->reference_site(walk->schema->context, path, type, name, length, &site)) return 0;
    }
    walk->result->references++;
    return 1;
}

static int dd_unsigned(const char *text, size_t length, size_t *value)
{
    size_t i, number = 0;
    if (!length) return 0;
    for (i = 0; i < length; i++) {
        unsigned digit = (unsigned char)text[i] - (unsigned)'0';
        if (digit > 9 || number > (SIZE_MAX - digit) / 10) return 0;
        number = number * 10 + digit;
    }
    *value = number;
    return 1;
}

static int dd_index(const char *key, const char *prefix, size_t *index)
{
    size_t a = strcspn(prefix, "["), b = strlen(key);
    return a < b && b - a > 2 && !memcmp(key, prefix, a) && key[a] == '[' && key[b - 1] == ']' &&
        dd_unsigned(key + a + 1, b - a - 2, index);
}

/* Collection sizing/indexing needs unique controls until its custom sequential
 * semantics are established. Reflected object assignments are retained and
 * visited in source order, including repeated scalar/object fields. Binding
 * rewrites each typed site without composing or removing an assignment. */
static int dd_unique_fields(dd_walk *walk, const dd_pending *item)
{
    const sh_decl_node *child;
    const char **keys;
    size_t count = 0, capacity = 16;
    for (child = item->node->children; child; child = child->next) count++;
    if (count < 2) return 1;
    while (count >= capacity / 2) {
        if (capacity > SIZE_MAX / 2) return 0;
        capacity *= 2;
    }
    if (capacity > SIZE_MAX / sizeof(*keys) || !(keys = calloc(capacity, sizeof(*keys)))) return 0;
    for (child = item->node->children; child; child = child->next) {
        const unsigned char *text = (const unsigned char *)child->key;
        uint64_t hash = UINT64_C(14695981039346656037);
        size_t slot;
        while (*text) { hash ^= *text++; hash *= UINT64_C(1099511628211); }
        slot = (size_t)hash & (capacity - 1);
        while (keys[slot] && strcmp(keys[slot], child->key)) slot = (slot + 1) & (capacity - 1);
        if (keys[slot]) {
            char *path = dd_path(item->path, child->key);
            if (!path) { free(keys); return 0; }
            dd_gap(walk, path, item->type, "repeated assignment requires a native syntax adapter");
            free(path);
        } else keys[slot] = child->key;
    }
    free(keys); return 1;
}

static int dd_children(dd_walk *walk, const dd_pending *item, const sh_decl_value_shape *shape)
{
    const sh_decl_node *child;
    size_t first = walk->count, limit = shape->count;
    int limited = shape->count_known;
    const char *prefix = shape->item_key ? shape->item_key : item->node->key;
    if (!item->node->compound)
        return dd_gap(walk, item->path, item->type, "expected a state block");
    if (shape->kind == SH_DECL_VALUE_COLLECTION && !dd_unique_fields(walk, item)) return 0;
    if (shape->kind == SH_DECL_VALUE_COLLECTION && !prefix)
        return dd_gap(walk, item->path, item->type, "collection has no verified item spelling");
    if (shape->kind == SH_DECL_VALUE_COLLECTION && shape->count_key) {
        child = sh_decl_tree_member(item->node, shape->count_key);
        if (child) {
            size_t declared;
            if (child->compound || !child->value ||
                !dd_unsigned(child->value, strlen(child->value), &declared))
                dd_gap(walk, item->path, item->type, "unsupported collection count");
            else if (limited && declared > limit)
                dd_gap(walk, item->path, item->type, "collection count exceeds native capacity");
            else { limit = declared; limited = 1; }
        }
    }
    for (child = item->node->children; child; child = child->next) {
        sh_decl_value_type type = {0};
        int found;
        const char *reason = NULL;
        if (shape->kind == SH_DECL_VALUE_OBJECT) {
            found = walk->schema->field(walk->schema->context, item->type, child->key, &type);
            if (!found) continue;
            if (found < 0 || !type.name) reason = "field semantics are unavailable";
        } else {
            size_t index;
            if (shape->count_key && !strcmp(child->key, shape->count_key)) continue;
            type = shape->element;
            if (!dd_index(child->key, prefix, &index)) reason = "unsupported collection member";
            else if (limited && index >= limit) reason = "collection index exceeds native count";
            else if (!type.name) reason = "collection element type is unavailable";
        }
        if (reason) {
            char *path = dd_path(item->path, child->key);
            if (!path) return 0;
            dd_gap(walk, path, item->type, reason);
            free(path);
        } else if (!dd_push(walk, child, type, item->path, child->key)) return 0;
    }
    /* LIFO storage, authored visitation order. */
    if (walk->count > first) {
        size_t a = first, b = walk->count - 1;
        while (a < b) {
            dd_pending temporary = walk->pending[a];
            walk->pending[a++] = walk->pending[b]; walk->pending[b--] = temporary;
        }
    }
    return 1;
}

const char *sh_decl_event_member(const char *key)
{
    const char *space;
    if (!key) return "";
    if (*key == '\n') key++;
    else if (key[0] == '\\' && key[1] == 'n') key += 2;
    else return key;
    space = strrchr(key, ' ');
    return space ? space + 1 : key;
}

/* idEventCallInfo's argument reader (Vulkan 0x1409caa70, OpenGL 0x1409ca230)
 * selects its reader from the item's key. Only decl and md6Anim resolve a
 * resource; every other key reads numbers, strings, a map entity's name,
 * joint or alias names, or an enum constant of the event's signature. The
 * comparison here ignores case so a differently spelled resource key can
 * only add an edge or a gap, never hide one. */
int sh_decl_event_argument_kind(const char *key)
{
    key = sh_decl_event_member(key);
    return !_stricmp(key, "decl") ? 1 : !_stricmp(key, "md6Anim") ? 2 : 0;
}

/* decl = { <declaration type> = "<name>"; } or decl = NULL. The schema proves
 * which manager the type names; an unproven type stays a gap. */
static int dd_event_decl(dd_walk *walk, const sh_decl_node *node, const char *path, sh_decl_value_type type)
{
    const sh_decl_node *member = node->compound ? node->children : NULL;
    sh_decl_value_shape shape = {0};
    const char *name;
    size_t length;
    if (!node->compound) {
        if (sh_decl_reference_literal(node, &name, &length) && !length) return 1;
        return dd_gap(walk, path, type, "event decl argument is not a native declaration block");
    }
    if (!member || member->next || member->compound)
        return dd_gap(walk, path, type, "event decl argument must name exactly one declaration type");
    if (!sh_decl_reference_literal(member, &name, &length))
        return dd_gap(walk, path, type, "event decl argument name requires a native syntax adapter");
    if (!length) return 1;
    if (!walk->schema->describe(walk->schema->context,
            (sh_decl_value_type){member->key, SH_DECL_EVENT_DECL_OPS}, &shape) ||
        (shape.kind != SH_DECL_VALUE_IGNORE && (shape.kind != SH_DECL_VALUE_REFERENCE || !shape.element.name)))
        return dd_gap(walk, path, type, "event decl argument type has no verified declaration manager");
    if (shape.kind == SH_DECL_VALUE_IGNORE) return 1;
    if (!walk->schema->reference(walk->schema->context, path, shape.element, name, length)) return 0;
    if (walk->schema->reference_site) {
        sh_decl_reference_site site = {member, NULL, 0};
        if (!walk->schema->reference_site(walk->schema->context, path, shape.element, name, length, &site)) return 0;
    }
    walk->result->references++;
    return 1;
}

/* md6Anim = "<name>": the native reader reads a string, not a pointer, so the
 * name is kept exactly (NULL is not special); only an empty name reads none. */
static int dd_event_anim(dd_walk *walk, const sh_decl_node *node, const char *path, sh_decl_value_type type)
{
    sh_decl_value_shape shape = {0};
    const char *name;
    size_t length;
    if (node->compound || !sh_decl_tree_literal(node, &name, &length))
        return dd_gap(walk, path, type, "md6Anim event argument is not a literal animation name");
    if (!length) return 1;
    if (!walk->schema->describe(walk->schema->context,
            (sh_decl_value_type){SH_DECL_EVENT_ANIM_TYPE, SH_DECL_EVENT_ANIM_OPS}, &shape) ||
        shape.kind != SH_DECL_VALUE_REFERENCE || !shape.element.name)
        return dd_gap(walk, path, type, "md6Anim event argument has no verified animation resource reader");
    if (!walk->schema->reference(walk->schema->context, path, shape.element, name, length)) return 0;
    if (walk->schema->reference_site) {
        sh_decl_reference_site site = {node, NULL, 0};
        if (!walk->schema->reference_site(walk->schema->context, path, shape.element, name, length, &site)) return 0;
    }
    walk->result->references++;
    return 1;
}

static int dd_event_call(dd_walk *walk, const dd_pending *item)
{
    const sh_decl_node *child, *args = NULL, *argument;
    size_t extent = 0; /* an unstated count reads no arguments */
    if (!item->node->compound) return dd_gap(walk, item->path, item->type, "expected a state block");
    for (child = item->node->children; child; child = child->next) {
        const char *name = sh_decl_event_member(child->key);
        if (!strcmp(name, "eventDef")) continue; /* an event name, never a resource */
        if (strcmp(name, "args") || args) {
            char *path = dd_path(item->path, child->key);
            if (!path) return 0;
            dd_gap(walk, path, item->type, strcmp(name, "args") ?
                "event call member is not read by the native reader" : "repeated event arguments");
            free(path); continue;
        }
        args = child;
    }
    if (!args) return 1;
    if (!args->compound) {
        const char *name; size_t length;
        if (sh_decl_reference_literal(args, &name, &length) && !length) return 1;
        return dd_gap(walk, item->path, item->type, "event arguments are not a native argument block");
    }
    {
        dd_pending block = {args, item->type, item->path};
        if (!dd_unique_fields(walk, &block)) return 0;
    }
    for (argument = args->children; argument; argument = argument->next)
        if (!strcmp(sh_decl_event_member(argument->key), "num")) {
            if (argument->compound || !argument->value ||
                !dd_unsigned(argument->value, strlen(argument->value), &extent))
                return dd_gap(walk, item->path, item->type, "unsupported event argument count");
        }
    for (argument = args->children; argument; argument = argument->next) {
        const char *key = sh_decl_event_member(argument->key), *reason = NULL;
        const sh_decl_node *reader = NULL;
        size_t index = 0, readers = 0;
        int ok = 1, kind = 0;
        char *path;
        if (!strcmp(key, "num")) continue;
        if (!dd_index(key, "item", &index)) reason = "unsupported event argument member";
        else if (index >= extent) reason = "event argument index exceeds its stated count";
        else if (argument->compound) for (child = argument->children; child; child = child->next) {
            int selected = sh_decl_event_argument_kind(child->key);
            readers++;
            if (selected) { kind = selected; reader = child; }
        }
        if (!reason && argument->compound && kind && readers > 1) reason = "event argument has more than one reader key";
        if (!reason && !argument->compound) {
            const char *name; size_t length;
            if (!sh_decl_reference_literal(argument, &name, &length) || length)
                reason = "event argument is not a native argument block";
        }
        path = dd_path(item->path, "args");
        if (path) { char *at = dd_path(path, argument->key); free(path); path = at; }
        if (path && reader && !reason) { char *at = dd_path(path, reader->key); free(path); path = at; }
        if (!path) return 0;
        if (reason) dd_gap(walk, path, item->type, reason);
        else if (reader) ok = kind == 2 ? dd_event_anim(walk, reader, path, item->type) :
            dd_event_decl(walk, reader, path, item->type);
        free(path);
        if (!ok) return 0;
    }
    return 1;
}

static int dd_run(dd_walk *walk)
{
    while (walk->count) {
        dd_pending item = walk->pending[--walk->count];
        sh_decl_value_shape shape = {0};
        int ok = 1;
        walk->result->visited++;
        if (!item.type.name || !walk->schema->describe(walk->schema->context, item.type, &shape))
            shape.kind = SH_DECL_VALUE_UNKNOWN;
        switch (shape.kind) {
        case SH_DECL_VALUE_IGNORE:
            if (walk->schema->value && !walk->schema->value(walk->schema->context, item.path, item.type, item.node)) ok = 0;
            break;
        case SH_DECL_VALUE_REFERENCE:
            ok = dd_reference(walk, item.node, item.path, item.type); break;
        case SH_DECL_VALUE_OBJECT:
        case SH_DECL_VALUE_COLLECTION:
            ok = dd_children(walk, &item, &shape); break;
        case SH_DECL_VALUE_POLYMORPHIC: {
            sh_decl_polymorphic_state state;
            const char *reason;
            /* Skipped by the native reader: whatever the destination held came
             * from earlier text or a parent, whose own walk reports it. */
            if (shape.null_keeps && sh_decl_polymorphic_unchanged(item.node)) break;
            if (sh_decl_polymorphic_read(item.node, shape.element, walk->schema, &state, &reason) != 1)
                dd_gap(walk, item.path, item.type, reason);
            else if (state.object) ok = dd_push(walk, state.object, state.type, item.path, "object");
            break;
        }
        case SH_DECL_VALUE_SCRIPT: {
            sh_decl_value_type selected;
            sh_decl_node fields;
            const char *reason;
            if (!sh_decl_script_read(item.node, item.type, walk->schema, &selected, &fields, &reason))
                dd_gap(walk, item.path, item.type, reason);
            else if (selected.name) {
                dd_pending object = {0};
                sh_decl_value_shape object_shape = {0};
                object.node = &fields; object.type = selected; object.path = item.path;
                object_shape.kind = SH_DECL_VALUE_OBJECT;
                ok = dd_children(walk, &object, &object_shape);
            }
            break;
        }
        case SH_DECL_VALUE_EVENT_CALL:
            ok = dd_event_call(walk, &item); break;
        default:
            dd_gap(walk, item.path, item.type, "type requires a verified native reader adapter"); break;
        }
        free(item.path);
        if (!ok) return 0;
    }
    return 1;
}

static int dd_finish(dd_walk *walk, int ok)
{
    while (walk->count) free(walk->pending[--walk->count].path);
    free(walk->pending);
    if (!ok) walk->result->aborted = 1;
    return ok && !walk->result->gaps;
}

static int dd_valid(const sh_decl_dependency_schema *schema, sh_decl_dependency_result *result)
{
    if (result) memset(result, 0, sizeof(*result));
    if (schema && result && schema->describe && schema->field && schema->reference) return 1;
    if (result) result->aborted = 1;
    return 0;
}

int sh_decl_state_dependencies(const sh_decl_node *state, sh_decl_value_type type,
    const sh_decl_dependency_schema *schema, sh_decl_dependency_result *result)
{
    dd_walk walk = {schema, result, NULL, 0, 0};
    if (!dd_valid(schema, result)) return 0;
    return dd_finish(&walk, state && dd_push(&walk, state, type, NULL, "edit") && dd_run(&walk));
}

int sh_decl_envelope_dependencies(const sh_decl_node *definition, sh_decl_value_type state_type,
    sh_decl_value_type parent_type, const sh_decl_dependency_schema *schema,
    sh_decl_dependency_result *result)
{
    dd_walk walk = {schema, result, NULL, 0, 0};
    const sh_decl_node *node;
    int ok = 0;
    if (!dd_valid(schema, result)) return 0;
    if (!definition || !definition->compound) return dd_finish(&walk, 0);
    for (node = definition->children; node; node = node->next) {
        if (!strcmp(node->key, "inherit")) {
            if (!dd_reference(&walk, node, "inherit", parent_type)) goto done;
        } else if (!strcmp(node->key, "edit")) {
            if (!dd_push(&walk, node, state_type, NULL, "edit") || !dd_run(&walk)) goto done;
        }
    }
    ok = 1;
done:
    return dd_finish(&walk, ok);
}

int sh_decl_entity_dependencies(const sh_decl_node *definition, sh_decl_value_type entity_type,
    const sh_decl_dependency_schema *schema, sh_decl_dependency_result *result)
{
    return sh_decl_envelope_dependencies(definition, entity_type,
        (sh_decl_value_type){"idDeclEntityDef", "*"}, schema, result);
}
