#include "decl_native_schema.h"
#include "resource_types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct dn_owned { struct dn_owned *next; } dn_owned;
typedef struct dn_field {
    struct dn_field *next;
    char *name;
    sh_decl_value_type type;
    int32_t size;
} dn_field;
typedef struct dn_type {
    char *name, *super;
    uintptr_t address, object_reader, pointer_reader;
    sh_decl_value_type element;
    dn_field *fields;
    int loaded;
} dn_type;
typedef struct dn_index { dn_type **slots; size_t count, capacity; } dn_index;

struct sh_decl_native_schema {
    sh_decl_native_source source;
    sh_decl_dependency_observer observer;
    sh_decl_dependency_schema schema;
    dn_owned *owned;
    dn_index classes, enums;
    uintptr_t references[3], string_reader, atomic_string_reader, list_reader, static_list_reader, float_wrapper_reader;
    uintptr_t polymorphic_reader;
};

static void *dn_allocate(sh_decl_native_schema *s, size_t size)
{
    dn_owned *memory;
    if (size > SIZE_MAX - sizeof(*memory)) return NULL;
    memory = (dn_owned *)calloc(1, sizeof(*memory) + size);
    if (!memory) return NULL;
    memory->next = s->owned; s->owned = memory;
    return memory + 1;
}

static int dn_read(sh_decl_native_schema *s, uintptr_t address, size_t offset, void *out, size_t size)
{
    if (!address || offset > UINTPTR_MAX - address || size > UINTPTR_MAX - address - offset) return 0;
    return s->source.read(s->source.context, address + offset, out, size);
}

static int dn_pointer(sh_decl_native_schema *s, uintptr_t address, size_t offset, uintptr_t *out)
{ return dn_read(s, address, offset, out, sizeof(*out)); }

static char *dn_copy(sh_decl_native_schema *s, const char *text, size_t size)
{
    char *copy;
    if (size == SIZE_MAX || !(copy = (char *)dn_allocate(s, size + 1))) return NULL;
    memcpy(copy, text, size); return copy;
}

static char *dn_string(sh_decl_native_schema *s, uintptr_t address)
{
    char *text = NULL, *result = NULL;
    size_t used = 0, capacity = 0;
    if (!address) return dn_copy(s, "", 0);
    for (;;) {
        char bytes[64];
        size_t count = sizeof(bytes), i;
        if (!dn_read(s, address, used, bytes, count)) {
            count = 1;
            if (!dn_read(s, address, used, bytes, count)) break;
        }
        for (i = 0; i < count && bytes[i]; i++) {}
        if (i >= SIZE_MAX - used) break;
        if (used + i + 1 > capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            char *grown;
            if (next < capacity) break;
            if (next < used + i + 1) next = used + i + 1;
            grown = (char *)realloc(text, next);
            if (!grown) break;
            text = grown; capacity = next;
        }
        memcpy(text + used, bytes, i); used += i;
        if (i < count) { result = dn_copy(s, text, used); break; }
    }
    free(text); return result;
}

static char *dn_text(sh_decl_native_schema *s, uintptr_t address, size_t offset)
{
    uintptr_t pointer;
    return dn_pointer(s, address, offset, &pointer) ? dn_string(s, pointer) : NULL;
}

static size_t dn_hash(const char *name)
{
    uint64_t hash = UINT64_C(14695981039346656037);
    while (*name) { hash ^= (unsigned char)*name++; hash *= UINT64_C(1099511628211); }
    return (size_t)hash;
}

static dn_type *dn_find(const dn_index *index, const char *name)
{
    size_t slot;
    if (!name || !index->capacity) return NULL;
    slot = dn_hash(name) & (index->capacity - 1);
    while (index->slots[slot]) {
        if (!strcmp(index->slots[slot]->name, name)) return index->slots[slot];
        slot = (slot + 1) & (index->capacity - 1);
    }
    return NULL;
}

static int dn_insert(dn_index *index, dn_type *type)
{
    size_t slot;
    if (dn_find(index, type->name)) return 0;
    if (index->count >= index->capacity / 2) {
        size_t capacity = index->capacity ? index->capacity * 2 : 16, i;
        dn_type **slots;
        if (capacity < index->capacity || capacity > SIZE_MAX / sizeof(*slots)) return 0;
        slots = (dn_type **)calloc(capacity, sizeof(*slots));
        if (!slots) return 0;
        for (i = 0; i < index->capacity; i++) if (index->slots[i]) {
            slot = dn_hash(index->slots[i]->name) & (capacity - 1);
            while (slots[slot]) slot = (slot + 1) & (capacity - 1);
            slots[slot] = index->slots[i];
        }
        free(index->slots); index->slots = slots; index->capacity = capacity;
    }
    slot = dn_hash(type->name) & (index->capacity - 1);
    while (index->slots[slot]) slot = (slot + 1) & (index->capacity - 1);
    index->slots[slot] = type; index->count++; return 1;
}

/* Callback-table counts include the terminating metadata record in both
 * supported images. Counts bound the native registry, not package payloads. */
static int dn_registry(sh_decl_native_schema *s, uintptr_t container, int classes)
{
    size_t i, stride = classes ? 0x38 : 0x18, table_offset = classes ? 0x88 : 0x58;
    uintptr_t records, readers, pointer_readers = 0;
    int32_t count, pointer_count = 0;
    dn_index *index = classes ? &s->classes : &s->enums;
    if (!dn_pointer(s, container, classes ? 0x20 : 0x10, &records) ||
        !dn_pointer(s, s->source.reflection, table_offset, &readers) ||
        !dn_read(s, s->source.reflection, table_offset + 8, &count, sizeof(count)) || count < 0) return 0;
    if (classes && (!dn_pointer(s, s->source.reflection, 0xa0, &pointer_readers) ||
        !dn_read(s, s->source.reflection, 0xa8, &pointer_count, sizeof(pointer_count)) || pointer_count != count)) return 0;
    if (!count) return !classes;
    if (!records || !readers || (classes && !pointer_readers)) return 0;
    for (i = 0; i < (size_t)count; i++) {
        dn_type *type;
        char *name;
        uintptr_t address;
        if (i > (UINTPTR_MAX - records) / stride) return 0;
        address = records + i * stride;
        name = dn_text(s, address, 0);
        if (!name) return 0;
        if (!*name) return !classes || index->count != 0;
        type = (dn_type *)dn_allocate(s, sizeof(*type));
        if (!type) return 0;
        type->name = name; type->address = address;
        if (!dn_pointer(s, readers, i * 16 + 8, &type->object_reader) ||
            (classes && !dn_pointer(s, pointer_readers, i * 16 + 8, &type->pointer_reader)) ||
            !dn_insert(index, type)) return 0;
    }
    return 0;
}

static int dn_load(sh_decl_native_schema *s, dn_type *type)
{
    uintptr_t fields, element;
    size_t i;
    dn_field **tail = &type->fields;
    if (type->loaded) return type->loaded > 0;
    type->loaded = -1;
    if (!type->super) type->super = dn_text(s, type->address, 8);
    if (!type->super || !dn_pointer(s, type->address, 0x20, &fields) ||
        !dn_pointer(s, type->address, 0x18, &element)) return 0;
    if (element) {
        type->element.name = dn_text(s, element, 0);
        type->element.ops = dn_text(s, element, 8);
        if (!type->element.name || !type->element.ops) return 0;
    }
    for (i = 0; fields; i++) {
        uintptr_t address;
        dn_field *field;
        char *name;
        if (i > (UINTPTR_MAX - fields) / 0x48) return 0;
        address = fields + i * 0x48;
        name = dn_text(s, address, 0x10);
        if (!name) return 0;
        if (!*name) break;
        field = (dn_field *)dn_allocate(s, sizeof(*field));
        if (!field) return 0;
        field->name = name;
        field->type.name = dn_text(s, address, 0);
        field->type.ops = dn_text(s, address, 8);
        if (!field->type.name || !field->type.ops ||
            !dn_read(s, address, 0x1c, &field->size, sizeof(field->size))) return 0;
        *tail = field; tail = &field->next;
    }
    type->loaded = 1; return 1;
}

/* Every reflected instantiation must agree on the same registered reader;
 * names select a known native family, never an arbitrary template's shape. */
static uintptr_t dn_family(const dn_index *index, const char *prefix)
{
    size_t i, length = strlen(prefix);
    uintptr_t reader = 0;
    for (i = 0; i < index->capacity; i++) {
        const dn_type *type = index->slots[i];
        if (!type || strncmp(type->name, prefix, length)) continue;
        if (!type->object_reader || (reader && reader != type->object_reader)) return 0;
        reader = type->object_reader;
    }
    return reader;
}

static int dn_primitive(const char *name)
{
    static const char *const names[] = {"bool", "char", "unsigned char", "short", "unsigned short",
        "int", "unsigned int", "long", "unsigned long", "long long", "unsigned long long",
        "float", "double", "idStrId", "idVec4", "idVecX"};
    size_t i;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) if (!strcmp(name, names[i])) return 1;
    return 0;
}

static int dn_array(sh_decl_native_schema *s, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    const char *ops = type.ops ? type.ops : "", *cursor = strchr(ops, '['), *suffix = cursor;
    size_t count = 1;
    if (!cursor) return 0;
    while (*cursor) {
        size_t dimension = 0;
        if (*cursor++ != '[' || *cursor < '0' || *cursor > '9') return -1;
        while (*cursor >= '0' && *cursor <= '9') {
            unsigned digit = (unsigned)(*cursor++ - '0');
            if (dimension > (SIZE_MAX - digit) / 10) return -1;
            dimension = dimension * 10 + digit;
        }
        if (*cursor++ != ']' || (dimension && count > SIZE_MAX / dimension)) return -1;
        count *= dimension;
    }
    shape->element.name = type.name;
    shape->element.ops = dn_copy(s, ops, (size_t)(suffix - ops));
    if (!shape->element.ops) return -1;
    shape->kind = SH_DECL_VALUE_COLLECTION; shape->count = count; shape->count_known = 1;
    return 1;
}

static int dn_describe(void *context, sh_decl_value_type value, sh_decl_value_shape *shape)
{
    sh_decl_native_schema *s = (sh_decl_native_schema *)context;
    dn_type *type = dn_find(&s->classes, value.name);
    const char *ops = value.ops ? value.ops : "";
    int array = dn_array(s, value, shape);
    size_t i;
    if (array) return array > 0;
    if (!strcmp(ops, "*") && type) {
        if (!type->pointer_reader) shape->kind = SH_DECL_VALUE_IGNORE;
        else for (i = 0; i < 3; i++) if (s->references[i] && type->pointer_reader == s->references[i]) {
            shape->kind = SH_DECL_VALUE_REFERENCE; break;
        }
    } else if (*ops) return 0;
    else if (type) {
        if ((s->string_reader && type->object_reader == s->string_reader) ||
            (s->atomic_string_reader && type->object_reader == s->atomic_string_reader) || dn_primitive(value.name))
            shape->kind = SH_DECL_VALUE_IGNORE;
        else if (s->float_wrapper_reader && type->object_reader == s->float_wrapper_reader) {
            /* This registered reader consumes either a float or { value = float; }.
             * Verify the reflected element too; other templates remain unknown. */
            if (!dn_load(s, type) || !type->element.name || strcmp(type->element.name, "float") ||
                !type->element.ops || *type->element.ops) return 0;
            shape->kind = SH_DECL_VALUE_IGNORE;
        }
        else if (s->polymorphic_reader && type->object_reader == s->polymorphic_reader) {
            if (!dn_load(s, type) || !type->element.name || !*type->element.name ||
                !type->element.ops || *type->element.ops || !dn_find(&s->classes, type->element.name)) return 0;
            shape->kind = SH_DECL_VALUE_POLYMORPHIC; shape->element = type->element;
        }
        else if (type->object_reader && (type->object_reader == s->list_reader || type->object_reader == s->static_list_reader)) {
            if (!dn_load(s, type) || !type->element.name || !*type->element.name) return 0;
            shape->kind = SH_DECL_VALUE_COLLECTION; shape->element = type->element;
            shape->item_key = "item"; shape->count_key = "num";
        } else if (!type->object_reader) shape->kind = SH_DECL_VALUE_OBJECT;
    } else {
        type = dn_find(&s->enums, value.name);
        if (type && !type->object_reader) shape->kind = SH_DECL_VALUE_IGNORE;
    }
    return 1;
}

static int dn_field_type(void *context, sh_decl_value_type value, const char *key, sh_decl_value_type *out)
{
    sh_decl_native_schema *s = (sh_decl_native_schema *)context;
    const char *name = value.name;
    size_t depth;
    for (depth = 0; name && *name && depth < s->classes.count; depth++) {
        dn_type *type = dn_find(&s->classes, name);
        dn_field *field;
        if (!type || !dn_load(s, type)) return -1;
        for (field = type->fields; field; field = field->next) if (!strcmp(field->name, key)) {
            if (field->size < 0) {
                const char *bit_type = field->type.name;
                return !strcmp(bit_type, "bool") || !strcmp(bit_type, "char") || !strcmp(bit_type, "unsigned char") ||
                    !strcmp(bit_type, "int") || !strcmp(bit_type, "unsigned int") ? 0 : -1;
            }
            *out = field->type; return 1;
        }
        name = type->super;
    }
    return name && *name ? -1 : 0;
}

static int dn_reference(void *context, const char *path, sh_decl_value_type type, const char *name, size_t length)
{
    sh_decl_native_schema *s = (sh_decl_native_schema *)context;
    return s->observer.reference ? s->observer.reference(s->observer.context, path, type, name, length) : 0;
}
static void dn_gap(void *context, const char *path, sh_decl_value_type type, const char *reason)
{
    sh_decl_native_schema *s = (sh_decl_native_schema *)context;
    if (s->observer.gap) s->observer.gap(s->observer.context, path, type, reason);
}

static int dn_dynamic_type(void *context, sh_decl_value_type base, const char *name,
    size_t length, sh_decl_value_type *selected)
{
    sh_decl_native_schema *s = context;
    char *terminated;
    dn_type *type;
    if (!selected) return 0;
    *selected = (sh_decl_value_type){0};
    if (!s || !base.name || (base.ops && *base.ops) || !name || !length || length == SIZE_MAX ||
        memchr(name, 0, length) || !(terminated = malloc(length + 1))) return 0;
    memcpy(terminated, name, length); terminated[length] = 0;
    type = dn_find(&s->classes, terminated); free(terminated);
    if (!type || sh_decl_native_schema_class_derives(s, type->name, base.name) != 1) return 0;
    *selected = (sh_decl_value_type){type->name, ""}; return 1;
}

sh_decl_native_schema *sh_decl_native_schema_open(sh_decl_native_source source,
    sh_decl_dependency_observer observer, char *error, size_t error_capacity)
{
    sh_decl_native_schema *s = NULL;
    const char *const resources[] = {"idDeclEntityDef", "idImage", "idRenderModel"};
    uintptr_t container;
    dn_type *type;
    size_t i;
    if (error && error_capacity) error[0] = 0;
    if (sizeof(uintptr_t) != 8 || !source.read || !source.reflection) goto failed;
    s = (sh_decl_native_schema *)calloc(1, sizeof(*s));
    if (!s) goto failed;
    s->source = source; s->observer = observer;
    if (!dn_pointer(s, source.reflection, 0, &container) || !container ||
        !dn_registry(s, container, 1) || !dn_registry(s, container, 0)) goto failed;
    for (i = 0; i < 3; i++) {
        type = dn_find(&s->classes, resources[i]);
        if (type) s->references[i] = type->pointer_reader;
    }
    type = dn_find(&s->classes, "idStr"); if (type) s->string_reader = type->object_reader;
    /* The atomic-string reader only interns text. Later consumers can assign
     * resource meaning to that text, but deserialization itself has no edge. */
    type = dn_find(&s->classes, "idAtomicString"); if (type) s->atomic_string_reader = type->object_reader;
    type = dn_find(&s->classes, "idTypesafeNumber < float , DegreesUnique_t >");
    if (type) s->float_wrapper_reader = type->object_reader;
    s->list_reader = dn_family(&s->classes, "idList < ");
    s->static_list_reader = dn_family(&s->classes, "idStaticList < ");
    s->polymorphic_reader = dn_family(&s->classes, "idTypeInfoObjectPtr < ");
    s->schema.context = s; s->schema.describe = dn_describe; s->schema.field = dn_field_type;
    s->schema.reference = dn_reference; s->schema.gap = dn_gap;
    s->schema.dynamic_type = dn_dynamic_type;
    return s;
failed:
    if (error && error_capacity) snprintf(error, error_capacity, "native reflection metadata is unavailable or inconsistent");
    sh_decl_native_schema_close(s); return NULL;
}

const sh_decl_dependency_schema *sh_decl_native_schema_view(sh_decl_native_schema *s)
{ return s ? &s->schema : NULL; }

int sh_decl_native_schema_class_derives(sh_decl_native_schema *s,
    const char *class_name, const char *base_name)
{
    dn_type *type, *base;
    size_t depth;
    int derives = 0;
    if (!s || !class_name || !*class_name || !base_name || !*base_name ||
        !(type = dn_find(&s->classes, class_name)) || !(base = dn_find(&s->classes, base_name))) return -1;
    /* Validate the complete ancestry even after a match; a malformed cycle or
     * missing superclass must not certify a usable root type. */
    for (depth = 0; depth < s->classes.count; depth++) {
        if (type == base) derives = 1;
        if (!type->super) type->super = dn_text(s, type->address, 8);
        if (!type->super) return -1;
        if (!*type->super) return derives;
        type = dn_find(&s->classes, type->super);
        if (!type) return -1;
    }
    return -1;
}

int sh_decl_native_schema_decl_type(sh_decl_native_schema *s,
    const char *declaration_type, sh_decl_value_type *state_type)
{
    size_t i;
    int derives;
    dn_type *type;
    if (!state_type) return -1;
    *state_type = (sh_decl_value_type){0};
    if (!s || !declaration_type || !*declaration_type) return -1;
    for (i = 0; i < sizeof(SH_RESOURCE_TYPES) / sizeof(SH_RESOURCE_TYPES[0]); i++) {
        const sh_resource_type_binding *binding = &SH_RESOURCE_TYPES[i];
        if (strcmp(binding->type, declaration_type)) continue;
        type = dn_find(&s->classes, binding->class_name);
        if (!type) return -1;
        derives = sh_decl_native_schema_class_derives(s, type->name, "idDeclTypeInfo");
        if (derives != 1) return derives;
        *state_type = (sh_decl_value_type){type->name, ""}; return 1;
    }
    return 0;
}

int sh_decl_native_schema_graph_type(sh_decl_native_schema *s, sh_decl_value_type value)
{
    dn_type *type, *base;
    int derives;
    if (!s || !value.name || !*value.name) return -1;
    if (value.ops && *value.ops) return 0;
    type = dn_find(&s->classes, value.name);
    base = dn_find(&s->classes, "idDeclTypeInfoGraph");
    if (!type || !base) return -1;
    derives = sh_decl_native_schema_class_derives(s, type->name, base->name);
    if (derives != 1) return derives;
    return base->object_reader && type->object_reader == base->object_reader ? 1 : -1;
}

sh_decl_reference_reader sh_decl_native_schema_reader(sh_decl_native_schema *s,
    sh_decl_value_type value)
{
    dn_type *type;
    size_t i;
    if (!s || !value.name || !value.ops || strcmp(value.ops, "*")) return SH_DECL_READER_UNKNOWN;
    type = dn_find(&s->classes, value.name);
    if (type && type->pointer_reader) for (i = 0; i < 3; i++)
        if (s->references[i] == type->pointer_reader) return (sh_decl_reference_reader)(i + 1);
    return SH_DECL_READER_UNKNOWN;
}

void sh_decl_native_schema_close(sh_decl_native_schema *s)
{
    if (s) {
        while (s->owned) { dn_owned *next = s->owned->next; free(s->owned); s->owned = next; }
        free(s->classes.slots); free(s->enums.slots); free(s);
    }
}
static int dn_relative(uintptr_t next, int32_t displacement, uintptr_t *out)
{
    if (displacement < 0) {
        uintptr_t distance = (uintptr_t)(-(int64_t)displacement);
        if (distance > next) return 0;
        *out = next - distance;
    } else {
        if ((uintptr_t)displacement > UINTPTR_MAX - next) return 0;
        *out = next + (uintptr_t)displacement;
    }
    return 1;
}

/* Both supported accessors use a guarded singleton constructor. Decode their
 * data references; never call them from the compiler/bootstrap thread. Offsets
 * describe the verified accessor instruction layout, not executable RVAs. */
int sh_decl_native_source_bind(sh_decl_native_source *source,
    uintptr_t accessor, uintptr_t type_container)
{
    unsigned char code[0x81];
    static const unsigned char tail[] = {0x48, 0x83, 0xc4, 0x30, 0x5b, 0xc3};
    int32_t first, second;
    uintptr_t manager, same, reflection, observed;
    if (!source) return -1;
    source->reflection = 0;
    if (!source->read || !accessor || !type_container || accessor > UINTPTR_MAX - sizeof(code) ||
        !source->read(source->context, accessor, code, sizeof(code)) ||
        memcmp(code + 0x47, "\x48\x8d\x1d", 3) ||
        memcmp(code + 0x74, "\x48\x8d\x05", 3) ||
        memcmp(code + 0x7b, tail, sizeof(tail))) return -1;
    memcpy(&first, code + 0x4a, sizeof(first));
    memcpy(&second, code + 0x77, sizeof(second));
    if (!dn_relative(accessor + 0x4e, first, &manager) ||
        !dn_relative(accessor + 0x7b, second, &same) ||
        !manager || manager != same || manager > UINTPTR_MAX - 0x80) return -1;
    reflection = manager + 0x80;
    if (!source->read(source->context, reflection, &observed, sizeof(observed))) return -1;
    if (!observed) return 0;
    if (observed != type_container) return -1;
    source->reflection = reflection; return 1;
}

int sh_decl_native_source_ready(sh_decl_native_source *source,
    uintptr_t accessor, uintptr_t type_container, uintptr_t game_system_export)
{
    unsigned char code[8], list[16];
    uintptr_t system, items;
    int32_t displacement, count, capacity;
    if (!source) return -1;
    source->reflection = 0;
    if (!source->read || !accessor || !type_container || !game_system_export || game_system_export > UINTPTR_MAX - sizeof(code) ||
        !source->read(source->context, game_system_export, code, sizeof(code)) ||
        memcmp(code, "\x48\x8d\x05", 3) || code[7] != 0xc3) return -1;
    memcpy(&displacement, code + 3, sizeof(displacement));
    if (!dn_relative(game_system_export + 7, displacement, &system) ||
        !system || system > UINTPTR_MAX - 24 ||
        !source->read(source->context, system + 8, list, sizeof(list))) return -1;
    memcpy(&items, list, 8); memcpy(&count, list + 8, 4); memcpy(&capacity, list + 12, 4);
    if (count < 0 || capacity < count || (count && !items)) return -1;
    if (!count) return 0;
    return sh_decl_native_source_bind(source, accessor, type_container);
}
