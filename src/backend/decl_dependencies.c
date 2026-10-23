#include "decl_dependencies.h"
#include "decl_polymorphic.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct dd_pending {
    const sh_decl_node *node;
    sh_decl_value_type type;
    char *path;
    const char *owner, *member; /* sh_decl_reference_site */
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
    pending->owner = pending->member = NULL;
    return 1;
}

int sh_decl_reference_literal(const sh_decl_node *node, const char **name, size_t *length)
{
    if (!sh_decl_tree_literal(node, name, length)) return 0;
    if (node->value && !strcmp(node->value, "NULL")) { *name = ""; *length = 0; }
    return 1;
}

static int dd_reference(dd_walk *walk, const sh_decl_node *node,
                         const char *path, sh_decl_value_type type, const dd_pending *item)
{
    const char *name;
    size_t length;
    if (!sh_decl_reference_literal(node, &name, &length))
        return dd_gap(walk, path, type, "resource value requires a native syntax adapter");
    if (!length) {
        if (!walk->schema->empty_lookup || (node->value && !strcmp(node->value, "NULL")) ||
            !walk->schema->empty_lookup(walk->schema->context, type)) return 1;
        name = "_emptyName"; length = 10;
    }
    if (!walk->schema->reference(walk->schema->context, path, type, name, length)) return 0;
    if (walk->schema->reference_site) {
        sh_decl_reference_site site = {node, NULL, 0, item ? item->owner : NULL, item ? item->member : NULL};
        if (!walk->schema->reference_site(walk->schema->context, path, type, name, length, &site)) return 0;
    }
    walk->result->references++;
    return 1;
}

/* A header's own parent lookup passes makeDefault 0: entityDef Parse and its
 * merge (lookup(entityDef list, name, 0), signatures.c) and the generic typeinfo
 * Parse (VK 0x17bc4d0). That lookup returns NULL for an empty name before it
 * would rename it to _emptyName, so an empty inherit names no parent. Only a
 * declaration-pointer reader's lookup defaults it (empty_lookup). */
static int dd_parent(dd_walk *walk, const sh_decl_node *node, sh_decl_value_type type)
{
    const char *name;
    size_t length;
    if (sh_decl_reference_literal(node, &name, &length) && !length) return 1;
    return dd_reference(walk, node, "inherit", type, NULL);
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
        else if (shape->kind == SH_DECL_VALUE_OBJECT) {
            walk->pending[walk->count - 1].owner = item->type.name;
            walk->pending[walk->count - 1].member = child->key;
        } else {
            walk->pending[walk->count - 1].owner = item->owner;
            walk->pending[walk->count - 1].member = item->member;
        }
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
int sh_decl_event_argument_count(const char *text, size_t length, size_t *count)
{
    size_t value = 0, limit = INT32_MAX, at = 0;
    int negative = 0;
    if (!text || !count) return 0;
    while (length && strchr(" \t\r\n", *text)) { text++; length--; }
    while (length && strchr(" \t\r\n", text[length - 1])) length--;
    if (length && *text == '-') { negative = 1; at++; }
    if (negative) limit++;
    if (at == length) return 0;
    for (; at < length; at++) {
        unsigned digit = (unsigned char)text[at] - (unsigned)'0';
        if (digit > 9 || value > (limit - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    *count = negative ? 0 : value; return 1;
}
char sh_decl_event_argument_format(const sh_decl_dependency_schema *schema, const char *key)
{
    static const struct { const char *key; char format; } readers[] = {
        {"int", 'i'}, {"float", 'f'}, {"bool", 'b'}, {"vec3", 'v'}, {"angles", 'a'}, {"string", 's'},
        {"entity", 'e'}, {"decl", 'd'}, {"animWebPath", '1'}, {"jointName", '2'}, {"animAlias", '3'},
        {"md6Anim", '4'}, {"color", 'c'}, {"NULL", '0'}
    };
    sh_decl_value_shape shape = {0};
    key = sh_decl_event_member(key);
    if (!key || !*key) return 0;
    for (size_t i = 0; i < sizeof(readers) / sizeof(*readers); i++) if (!_stricmp(key, readers[i].key)) return readers[i].format;
    /* ReadEventArg's remaining type-name branch is the registered enum reader,
     * whose native event format is 'i'. Unknown types supply no signature. */
    return schema && schema->describe && schema->describe(schema->context, (sh_decl_value_type){key, ""}, &shape) &&
        shape.kind == SH_DECL_VALUE_IGNORE && shape.plain ? 'i' : 0;
}
static char *dd_event_signature(const sh_decl_dependency_schema *schema, const sh_decl_node *args, size_t extent)
{
    size_t last = 0, supplied = 0, counts = 0;
    unsigned seen = 0;
    char *signature;
    for (const sh_decl_node *arg = args->children; arg; arg = arg->next) {
        const char *key = sh_decl_event_member(arg->key); size_t index = 0;
        if (!strcmp(key, "num")) { counts++; continue; }
        if (supplied++ >= extent || !dd_index(key, "item", &index) || index >= SH_DECL_EVENT_MAX_ARGUMENTS ||
            (seen & (1u << index))) return NULL;
        seen |= 1u << index;
        if (index + 1 > last) last = index + 1;
    }
    if (counts != 1 || !(signature = malloc(last + 1))) return NULL;
    memset(signature, '?', last); signature[last] = 0;
    for (const sh_decl_node *arg = args->children; arg; arg = arg->next) {
        const char *key = sh_decl_event_member(arg->key), *literal = NULL; size_t index = 0, length = 0;
        char format = 0;
        if (!strcmp(key, "num")) continue;
        if (!dd_index(key, "item", &index) || signature[index] != '?') { free(signature); return NULL; }
        if (!arg->compound && sh_decl_reference_literal(arg, &literal, &length) && !length) format = '0';
        else if (arg->compound && arg->children && !arg->children->next)
            format = sh_decl_event_argument_format(schema, arg->children->key);
        if (!format) { free(signature); return NULL; }
        signature[index] = format;
    }
    return signature;
}
static int dd_event_decl(dd_walk *walk, const sh_decl_node *node, const char *path, sh_decl_value_type type,
    const char *event, size_t event_length, size_t index, size_t count, const char *signature)
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
        site.event_name = event; site.event_name_length = event_length; site.event_argument_index = index;
        site.event_argument_count = count; site.event_argument_signature = signature;
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
    const sh_decl_node *child, *args = NULL, *argument, *event = NULL;
    const char *event_name = NULL;
    char *signature = NULL;
    size_t extent = 0, event_length = 0, supplied = 0, counts = 0; /* an unstated count reads no arguments */
    unsigned seen = 0;
    if (!item->node->compound) return dd_gap(walk, item->path, item->type, "expected a state block");
    for (child = item->node->children; child; child = child->next) {
        const char *name = sh_decl_event_member(child->key);
        if (!strcmp(name, "eventDef")) { event = child; continue; } /* an event name, never a resource */
        if (strcmp(name, "args") || args) {
            char *path = dd_path(item->path, child->key);
            if (!path) return 0;
            dd_gap(walk, path, item->type, strcmp(name, "args") ?
                "event call member is not read by the native reader" : "repeated event arguments");
            free(path); continue;
        }
        args = child;
    }
    if (event && (event->compound || !sh_decl_tree_literal(event, &event_name, &event_length))) {
        event_name = NULL; event_length = 0;
    }
    if (walk->schema->event_value) {
        sh_decl_reference_site site = {event ? event : item->node};
        if (!walk->schema->event_value(walk->schema->context, item->path, event_name, event_length, SIZE_MAX, NULL,
                &site)) return 0;
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
            if (counts++) { dd_gap(walk, item->path, item->type, "repeated event argument count"); continue; }
            if (argument->compound || !argument->value ||
                !sh_decl_event_argument_count(argument->value, strlen(argument->value), &extent))
                return dd_gap(walk, item->path, item->type, "unsupported event argument count");
        }
    signature = dd_event_signature(walk->schema, args, extent);
    for (argument = args->children; argument; argument = argument->next) {
        const char *key = sh_decl_event_member(argument->key), *reason = NULL;
        const sh_decl_node *reader = NULL, *value = NULL;
        size_t index = 0, readers = 0;
        int ok = 1, kind = 0;
        char *path;
        if (!strcmp(key, "num")) continue;
        if (supplied++ >= extent) reason = "event argument record exceeds its stated count";
        else if (!dd_index(key, "item", &index)) reason = "unsupported event argument member";
        else if (index >= SH_DECL_EVENT_MAX_ARGUMENTS) reason = "event argument index exceeds native argument storage";
        else if (seen & (1u << index)) reason = "repeated event argument index";
        else seen |= 1u << index;
        if (!reason && argument->compound) for (child = argument->children; child; child = child->next) {
            int selected = sh_decl_event_argument_kind(child->key);
            readers++;
            if (selected) { kind = selected; reader = child; }
            else value = child;
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
        if (!path) { free(signature); return 0; }
        if (reason) dd_gap(walk, path, item->type, reason);
        else if (reader) ok = kind == 2 ? dd_event_anim(walk, reader, path, item->type) :
            dd_event_decl(walk, reader, path, item->type, event_name, event_length, index, extent, signature);
        else if (value && readers == 1 && value->key && walk->schema->event_value) {
            char *at = dd_path(path, value->key);
            sh_decl_reference_site site = {value};
            ok = at && walk->schema->event_value(walk->schema->context, at, event_name, event_length, index,
                sh_decl_event_member(value->key), &site);
            free(at);
        }
        free(path);
        if (!ok) { free(signature); return 0; }
    }
    free(signature);
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
            if (ok && walk->schema->value_site) {
                sh_decl_reference_site site = {item.node, NULL, 0, item.owner, item.member};
                ok = walk->schema->value_site(walk->schema->context, item.path, item.type, &site);
            }
            break;
        case SH_DECL_VALUE_REFERENCE:
            /* A by-name reference names a declaration of its element's class. */
            ok = dd_reference(walk, item.node, item.path, shape.element.ops &&
                !strcmp(shape.element.ops, SH_DECL_BY_NAME_DECL_OPS) ? shape.element : item.type, &item); break;
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
            if (!dd_parent(&walk, node, parent_type)) goto done;
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
    return sh_decl_entity_state_dependencies(definition, entity_type, NULL, schema, result);
}

int sh_decl_entity_every_state_dependencies(const sh_decl_node *definition, sh_decl_value_type entity_type,
    const sh_decl_dependency_schema *schema, sh_decl_dependency_result *result)
{
    dd_walk walk = {schema, result, NULL, 0, 0};
    sh_decl_entity_headers headers;
    int ok = 0;
    if (!dd_valid(schema, result)) return 0;
    if (!definition || !definition->compound) return dd_finish(&walk, 0);
    sh_decl_entity_split(definition, &headers);
    if (headers.inherit && !dd_parent(&walk, headers.inherit,
            (sh_decl_value_type){"idDeclEntityDef", "*"})) goto done;
    for (const sh_decl_node *node = headers.state; node; node = node->next)
        if (node->compound && (!dd_push(&walk, node, entity_type, NULL, "edit") || !dd_run(&walk))) goto done;
    ok = 1;
done:
    return dd_finish(&walk, ok);
}
int sh_decl_entity_header_dependencies(const sh_decl_node *definition,
    const sh_decl_dependency_schema *schema, sh_decl_dependency_result *result)
{
    dd_walk walk = {schema, result, NULL, 0, 0};
    sh_decl_entity_headers headers;
    if (!dd_valid(schema, result)) return 0;
    if (!definition || !definition->compound) return dd_finish(&walk, 0);
    sh_decl_entity_split(definition, &headers);
    return dd_finish(&walk, !headers.inherit || dd_parent(&walk, headers.inherit,
        (sh_decl_value_type){"idDeclEntityDef", "*"}));
}
int sh_decl_entity_state_dependencies(const sh_decl_node *definition, sh_decl_value_type entity_type,
    const char *state, const sh_decl_dependency_schema *schema, sh_decl_dependency_result *result)
{
    dd_walk walk = {schema, result, NULL, 0, 0};
    sh_decl_entity_headers headers;
    int update = 0, ok = 0;
    if (!dd_valid(schema, result)) return 0;
    if (!definition || !definition->compound) return dd_finish(&walk, 0);
    sh_decl_entity_split(definition, &headers);
    if (headers.inherit && !dd_parent(&walk, headers.inherit,
            (sh_decl_value_type){"idDeclEntityDef", "*"})) goto done;
    for (const sh_decl_node *node = sh_decl_entity_state_statement(definition, state, &update); node;
         node = update ? sh_decl_entity_next_state(node) : NULL)
        if (!dd_push(&walk, node, entity_type, NULL, "edit") || !dd_run(&walk)) goto done;
    ok = 1;
done:
    return dd_finish(&walk, ok);
}
