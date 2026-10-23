#include "decl_native_schema.h"
#include "resource_types.h"
#include "decl_native_registry.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct dn_owned { struct dn_owned *next; } dn_owned;
typedef struct dn_field {
    struct dn_field *next;
    char *name;
    sh_decl_value_type type;
    int32_t offset, size;
    uintptr_t setter; /* +0x38, called for a bitfield (negative size) */
} dn_field;
typedef struct dn_type {
    char *name, *super;
    uintptr_t address, object_reader, pointer_reader;
    sh_decl_value_type element;
    dn_field *fields;
    int loaded;
    int32_t size; /* the record's +0x10, the bytes the member walker reports */
} dn_type;
typedef struct dn_index { dn_type **slots; size_t count, capacity; } dn_index;

struct sh_decl_native_schema {
    sh_decl_native_source source;
    sh_decl_dependency_observer observer;
    sh_decl_dependency_schema schema;
    /* The schema whose open read the registry: this one, unless it was opened
     * by sh_decl_native_schema_open_shared. Its records hold the registry and
     * every loaded type's texts and fields, and its index slots are the ones
     * every sharing schema reads; both stay until no schema reading them is
     * open (holders, counted on the origin). */
    struct sh_decl_native_schema *origin;
    size_t holders;
    dn_owned *records;
    dn_owned *owned; /* This schema's own answers, freed when it closes. */
    dn_index classes, enums;
    uintptr_t references[6], string_reader, atomic_string_reader, color_reader, list_reader, static_list_reader;
    uintptr_t float_wrapper_reader, time_wrapper_reader, time_container_reader, random_value_reader;
    uintptr_t anim_path_reader, atomic_anim_path_reader, type_info_reader, type_info_ptr_reader;
    uintptr_t alias_handle_reader, atomic_alias_handle_reader, managed_handle_reader;
    uintptr_t polymorphic_reader;
    uintptr_t script_reader;
    uintptr_t movie_file_reader;
    uintptr_t event_call_reader;
    uintptr_t string_id_reader;
    uintptr_t bounded_int_reader, bounded_float_reader, voice_track_reader, collision_model_reader;
    int enum_pointer_readers; /* the enum pointer-reader table (+0x70) was read */
    sh_decl_native_registry *registry;
    const dn_type *preview_model;
};

static void *dn_allocate(dn_owned **list, size_t size)
{
    dn_owned *memory;
    if (size > SIZE_MAX - sizeof(*memory)) return NULL;
    memory = (dn_owned *)calloc(1, sizeof(*memory) + size);
    if (!memory) return NULL;
    memory->next = *list; *list = memory;
    return memory + 1;
}
static void dn_free(dn_owned **list)
{
    while (*list) { dn_owned *next = (*list)->next; free(*list); *list = next; }
}

static int dn_read(sh_decl_native_schema *s, uintptr_t address, size_t offset, void *out, size_t size)
{
    if (!address || offset > UINTPTR_MAX - address || size > UINTPTR_MAX - address - offset) return 0;
    return s->source.read(s->source.context, address + offset, out, size);
}

static int dn_pointer(sh_decl_native_schema *s, uintptr_t address, size_t offset, uintptr_t *out)
{ return dn_read(s, address, offset, out, sizeof(*out)); }

static char *dn_copy(dn_owned **list, const char *text, size_t size)
{
    char *copy;
    if (size == SIZE_MAX || !(copy = (char *)dn_allocate(list, size + 1))) return NULL;
    memcpy(copy, text, size); return copy;
}

/* A native string's storage ends at its terminator, and the source copies
 * whatever is requested from mapped memory, so a string is read one byte at a
 * time up to and including its terminator, never past it. A byte that cannot
 * be read before the terminator fails the whole string. */
static char *dn_string(sh_decl_native_schema *s, dn_owned **list, uintptr_t address)
{
    char *text = NULL, *result = NULL;
    size_t used = 0, capacity = 0;
    if (!address) return dn_copy(list, "", 0);
    for (;;) {
        char c;
        if (!dn_read(s, address, used, &c, 1)) break;
        if (!c) { result = dn_copy(list, text ? text : "", used); break; }
        if (used == capacity) {
            size_t next = capacity ? capacity * 2 : 64;
            char *grown;
            if (next < capacity || !(grown = (char *)realloc(text, next))) break;
            text = grown; capacity = next;
        }
        text[used++] = c;
    }
    free(text); return result;
}

/* A registry text, kept with the origin's records like the type or field
 * that holds it. */
static char *dn_text(sh_decl_native_schema *s, uintptr_t address, size_t offset)
{
    uintptr_t pointer;
    return dn_pointer(s, address, offset, &pointer) ? dn_string(s, &s->origin->records, pointer) : NULL;
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
    /* Enum pointer readers (+0x70) are indexed like +0x58 (VK 0x141a21260).
     * Without a matching table every enum pointer stays unknown. */
    if (!classes && dn_pointer(s, s->source.reflection, 0x70, &pointer_readers) &&
        dn_read(s, s->source.reflection, 0x78, &pointer_count, sizeof(pointer_count)) &&
        pointer_count == count && (pointer_readers || !count)) s->enum_pointer_readers = 1;
    else if (!classes) pointer_readers = 0;
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
        type = (dn_type *)dn_allocate(&s->origin->records, sizeof(*type));
        if (!type) return 0;
        type->name = name; type->address = address;
        if (!dn_pointer(s, readers, i * 16 + 8, &type->object_reader) ||
            (pointer_readers && !dn_pointer(s, pointer_readers, i * 16 + 8, &type->pointer_reader)) ||
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
        field = (dn_field *)dn_allocate(&s->origin->records, sizeof(*field));
        if (!field) return 0;
        field->name = name;
        field->type.name = dn_text(s, address, 0);
        field->type.ops = dn_text(s, address, 8);
        /* The member walker (VK 0x141a209b0) and writer (VK 0x141a22ff0) store
         * at the object plus +0x18; they consume +0x1c only as negative for a
         * bitfield written through its setter (+0x38). */
        if (!field->type.name || !field->type.ops ||
            !dn_read(s, address, 0x18, &field->offset, sizeof(field->offset)) ||
            !dn_read(s, address, 0x1c, &field->size, sizeof(field->size)) ||
            (field->size < 0 && !dn_pointer(s, address, 0x38, &field->setter))) return 0;
        *tail = field; tail = &field->next;
    }
    if (!dn_read(s, type->address, 0x10, &type->size, sizeof(type->size))) return 0;
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
    shape->element.ops = dn_copy(&s->owned, ops, (size_t)(suffix - ops));
    if (!shape->element.ops) return -1;
    shape->kind = SH_DECL_VALUE_COLLECTION; shape->count = count; shape->count_known = 1;
    return 1;
}

/* These internal spellings distinguish the script module's field namespace
 * from engine reflection. They never become resource or authored class names. */
#define DN_SCRIPT_PREFIX "@script/"
#define DN_SCRIPT_DECL "@script-decl"
static int dn_script_value(sh_decl_native_schema *s, const char *name, sh_decl_value_type *value)
{
    const sh_decl_script_type *script = sh_decl_script_schema_find(s->source.scripts, name);
    dn_type *native = dn_find(&s->classes, name);
    size_t length = strlen(name), prefix = sizeof(DN_SCRIPT_PREFIX) - 1;
    char *spelling;
    int derives;
    if (!script) return 0;
    if (!script->has_fields && native) {
        derives = sh_decl_native_schema_class_derives(s, name, "idDecl");
        if (derives < 0) return 0;
        if (derives) { *value = (sh_decl_value_type){native->name, DN_SCRIPT_DECL}; return 1; }
    }
    if (length > SIZE_MAX - prefix - 1 || !(spelling = dn_allocate(&s->owned, prefix + length + 1))) return 0;
    memcpy(spelling, DN_SCRIPT_PREFIX, prefix); memcpy(spelling + prefix, name, length + 1);
    *value = (sh_decl_value_type){spelling, ""}; return 1;
}
static int dn_script_describe(sh_decl_native_schema *s, const char *name, sh_decl_value_shape *shape)
{
    const sh_decl_script_type *script = sh_decl_script_schema_find(s->source.scripts, name);
    char *inner;
    int ok;
    if (!script) return 0;
    if (script->has_fields) { shape->kind = SH_DECL_VALUE_OBJECT; return 1; }
    inner = sh_decl_script_template(name, "ssList<");
    if (inner) {
        ok = *inner && dn_script_value(s, inner, &shape->element); free(inner);
        if (!ok) return 0;
        shape->kind = SH_DECL_VALUE_COLLECTION; shape->count_key = "num"; shape->item_key = "item";
        return 1;
    }
    /* After script structs/enums/list handling, the native dispatcher consumes
     * primitive values, strings and live entity handles, reads idDecl pointers,
     * or skips unknown types. Decl types were converted by dn_script_value. */
    shape->kind = SH_DECL_VALUE_IGNORE; return 1;
}

/* The exact preview type, still read through idStr's registered reader. A
 * different reader would be a different contract and stays unknown. */
static int dn_preview_model(const sh_decl_native_schema *s, const dn_type *type)
{ return type && type == s->preview_model && s->string_reader && type->object_reader == s->string_reader; }

/* The event decl argument reader (Vulkan 0x1409caa70 / OpenGL 0x1409ca230)
 * finds its declaration manager by type name, then the name with makeDefault.
 * Bind it as that class's typed pointer only when the class selects the same
 * manager and is read by the verified declaration pointer reader. */
static int dn_event_decl(sh_decl_native_schema *s, const char *family, sh_decl_value_shape *shape)
{
    const char *class_name = NULL, *selected = NULL;
    if (!s->registry || !family || !*family ||
        sh_decl_native_registry_type_class(s->registry, family, &class_name) != 1 ||
        sh_decl_native_registry_class_type(s->registry, class_name, &selected) != 1 ||
        !selected || _stricmp(selected, family) ||
        sh_decl_native_schema_reader(s, (sh_decl_value_type){class_name, "*"}) != SH_DECL_READER_DECL) return 0;
    shape->kind = SH_DECL_VALUE_REFERENCE; shape->element = (sh_decl_value_type){class_name, SH_DECL_EVENT_DECL_USE_OPS};
    return 1;
}

/* The event md6Anim argument reader (same functions) finds the name in the
 * animation resource list, then loads it with makeDefault. Bind it as
 * idMD6Anim's typed resource pointer only when that class is read by the
 * verified generic resource pointer reader, whose family table names "anim".
 * The element keeps the use's ops, so the reference reader can report that
 * this use, unlike a reflected idMD6Anim field, defaults an absent clip. */
static int dn_event_anim(sh_decl_native_schema *s, const char *class_name, sh_decl_value_shape *shape)
{
    if (!class_name || strcmp(class_name, SH_DECL_EVENT_ANIM_TYPE) ||
        sh_decl_native_schema_reader(s, (sh_decl_value_type){class_name, "*"}) != SH_DECL_READER_IMAGE) return 0;
    shape->kind = SH_DECL_VALUE_REFERENCE;
    shape->element = (sh_decl_value_type){class_name, SH_DECL_EVENT_ANIM_OPS};
    return 1;
}

/* Effects of the value readers dn_describe reads as IGNORE, by registered
 * reader identity (M77 ledger; bodies paired VK/GL). Reads use only the lexer,
 * the parse's arguments (constant for SetEntityEditState's per-layer calls,
 * VK 0x14088b5e0 / 0x1412cff00) and the named tables. Others stay unmodelled. */
static void dn_effects(const sh_decl_native_schema *s, uintptr_t reader, sh_decl_value_shape *shape)
{
    if (!reader) return;
    /* idStrId (VK 0x141a21150 / GL 0x141a13390): idStr's reader, then a lookup
     * in the static string-ID table (VK 0x141a2acd0), 0 when unknown or empty.
     * idAnimWebPath (VK 0x1409ca680 / GL 0x1409c9e40): idStr's reader, then
     * '.' and ':' become '/'. Other tokens clear the field and fail the parse. */
    if (reader == s->string_id_reader || reader == s->anim_path_reader) {
        shape->assigns = SH_DECL_ASSIGN_QUOTED; shape->effects = SH_DECL_EFFECTS_FIELD;
    /* idTypeInfoPtr (VK 0x1409d0510 / GL 0x1409cfcd0): as the idTypeInfo
     * pointer reader, storing NULL outside the expected class range. */
    } else if (reader == s->type_info_ptr_reader) {
        shape->assigns = SH_DECL_ASSIGN_TOKEN; shape->effects = SH_DECL_EFFECTS_FIELD;
    /* idTypesafeNumber/Time<float> (VK 0x1409d0c80, 0x1409d0f00 / GL 0x1409d0440,
     * 0x1409d06c0) and randomValue_t (VK 0x1409cda30 / GL 0x1409cd1f0): a number
     * assigns every member; a block assigns only the keys it names. */
    } else if (reader == s->float_wrapper_reader || reader == s->time_wrapper_reader ||
        reader == s->random_value_reader) {
        shape->assigns = SH_DECL_ASSIGN_NUMBER; shape->effects = SH_DECL_EFFECTS_FIELD;
    /* timeContainer_t (VK 0x1409d0120 / GL 0x1409cf8e0): an integer, or a block's
     * milliseconds, then seconds and ticks from the fixed tick rate. */
    } else if (reader == s->time_container_reader) {
        shape->assigns = SH_DECL_ASSIGN_INTEGER; shape->effects = SH_DECL_EFFECTS_FIELD;
    /* idColor (VK 0x141a20750 / GL 0x141a129a0): the channels a block names,
     * then a whole-value conversion chosen by the parse's arguments. */
    } else if (reader == s->color_reader) {
        shape->effects = SH_DECL_EFFECTS_FIELD;
    /* idAtomicString (VK 0x141a204f0 / GL 0x141a12730) and idAnimWebPath_Atomic
     * (VK 0x1409ca6c0 / GL 0x1409c9e80) intern in the global atomic-string table;
     * alias handles add pending registrations (VK 0x1409ca410); atomic alias
     * handles intern through the alias manager now (VK 0x1409ca550 / GL
     * 0x1409c9d10); managed pointers resolve world names and relink handles
     * (VK 0x1409cd7b0 / GL 0x1409ccf70). */
    } else if (reader == s->atomic_string_reader || reader == s->atomic_anim_path_reader ||
        reader == s->alias_handle_reader || reader == s->atomic_alias_handle_reader ||
        reader == s->managed_handle_reader) {
        shape->effects = SH_DECL_EFFECTS_GLOBAL;
    }
}

/* idEntityDefRef (no reflected fields, super idStr, the shared idStr reader)
 * holds a name its consumers resolve later. Two resolvers are traced: VK
 * 0x140c2aeb0 / GL 0x140c2aa60 for entitySettings_t.entityType (entityDef
 * FindByName with makeDefault 0, then the ancestry test; M122, R283), and VK
 * 0x140c45b20 / GL 0x140c45450 for idSyncAttack.syncMeleeEntityDefs (entityDef
 * find-or-load with makeDefault 0; R-295). The other fields' resolvers are not
 * traced; R285 withdrew VK 0x1404ea840, which reads spawnableEntities text.
 * Bind it as a by-name entityDef reference only when the live registry reads
 * the class with the idStr reader and idDeclEntityDef with the declaration
 * pointer reader; anything else keeps the plain-string shape. */
static int dn_by_name_decl(sh_decl_native_schema *s, const char *name, sh_decl_value_shape *shape)
{
    static const char *const by_name[][2] = {{"idEntityDefRef", "idDeclEntityDef"}};
    for (size_t i = 0; i < sizeof(by_name) / sizeof(by_name[0]); i++) {
        if (strcmp(name, by_name[i][0]) || sh_decl_native_schema_class_derives(s, name, "idStr") != 1 ||
            sh_decl_native_schema_reader(s, (sh_decl_value_type){by_name[i][1], "*"}) != SH_DECL_READER_DECL) continue;
        shape->kind = SH_DECL_VALUE_REFERENCE;
        shape->element = (sh_decl_value_type){by_name[i][1], SH_DECL_BY_NAME_DECL_OPS};
        return 1;
    }
    return 0;
}

static int dn_describe(void *context, sh_decl_value_type value, sh_decl_value_shape *shape)
{
    sh_decl_native_schema *s = (sh_decl_native_schema *)context;
    dn_type *type;
    if (value.ops && !strcmp(value.ops, SH_DECL_EVENT_DECL_OPS)) return dn_event_decl(s, value.name, shape);
    if (value.ops && !strcmp(value.ops, SH_DECL_EVENT_ANIM_OPS)) return dn_event_anim(s, value.name, shape);
    if (value.ops && !strcmp(value.ops, SH_DECL_EVENT_DECL_USE_OPS)) {
        if (sh_decl_native_schema_reader(s, value) != SH_DECL_READER_DECL) return 0;
        shape->kind = SH_DECL_VALUE_REFERENCE; shape->element = value; return 1;
    }
    if (value.ops && !strcmp(value.ops, SH_DECL_BY_NAME_DECL_OPS)) {
        if (sh_decl_native_schema_reader(s, value) != SH_DECL_READER_DECL) return 0;
        shape->kind = SH_DECL_VALUE_REFERENCE; shape->element = value;
        shape->assigns = SH_DECL_ASSIGN_QUOTED; shape->effects = SH_DECL_EFFECTS_FIELD; return 1;
    }
    type = dn_find(&s->classes, value.name);
    const char *ops = value.ops ? value.ops : "";
    int array = dn_array(s, value, shape);
    size_t i;
    if (!strcmp(ops, DN_SCRIPT_DECL)) {
        if (!type || sh_decl_native_schema_class_derives(s, value.name, "idDecl") != 1) return 0;
        shape->kind = SH_DECL_VALUE_REFERENCE; return 1;
    }
    if (!strncmp(value.name, DN_SCRIPT_PREFIX, sizeof(DN_SCRIPT_PREFIX) - 1))
        return !*ops && dn_script_describe(s, value.name + sizeof(DN_SCRIPT_PREFIX) - 1, shape);
    if (array) return array > 0;
    if (!strcmp(ops, "*") && type) {
        if (s->script_reader && type->pointer_reader == s->script_reader) {
            /* VK 0x1409ce070 / GL 0x1409cd830 creates every selected object
             * through the game's script manager (gameLocal +0x167c8) and
             * releases the earlier one; its field reader (VK 0x1409ce6a0 /
             * GL 0x1409cde60) also resolves entity names and declarations. */
            shape->kind = SH_DECL_VALUE_SCRIPT; shape->effects = SH_DECL_EFFECTS_GLOBAL;
        } else if (!type->pointer_reader) {
            /* The generic dispatch (VK 0x141a21260 / GL 0x141a134a0) stores NULL
             * for an unquoted NULL and otherwise skips the value (VK
             * 0x141a26af0), leaving the pointer unchanged. */
            shape->kind = SH_DECL_VALUE_IGNORE;
            shape->assigns = SH_DECL_ASSIGN_NULL; shape->effects = SH_DECL_EFFECTS_NONE;
        } else if ((s->voice_track_reader && type->pointer_reader == s->voice_track_reader) ||
            (s->collision_model_reader && type->pointer_reader == s->collision_model_reader)) {
            /* Find or create in a fixed resource list (sh_decl_native_schema_reader). */
            shape->kind = SH_DECL_VALUE_REFERENCE; shape->effects = SH_DECL_EFFECTS_GLOBAL;
        } else if (s->type_info_reader && type->pointer_reader == s->type_info_reader) {
            /* VK 0x1409d03e0 / GL 0x1409cfba0: the tokens up to ';', unquoted,
             * looked up in the static type registry (VK 0x1403dd9b0); a block
             * is skipped and stores NULL. Every form assigns. */
            shape->kind = SH_DECL_VALUE_IGNORE;
            shape->assigns = SH_DECL_ASSIGN_TOKEN; shape->effects = SH_DECL_EFFECTS_FIELD;
        }
        else for (i = 0; i < sizeof(s->references) / sizeof(s->references[0]); i++) if (s->references[i] && type->pointer_reader == s->references[i]) {
            /* Decl pointers find or create the decl (VK 0x1409ca890 -> 0x1417b36f0
             * with makeDefault) and list one decl type with the game (0x140300190);
             * resource pointers find or create list entries (0x1409cdf40,
             * 0x1409d0000 -> 0x141801380); render models resolve or intern names
             * and add pending registrations (0x1409cdd20); static models load
             * through the model manager (0x1409cfee0); idSWF frees the earlier
             * movie and constructs a new one (0x1409cf880). Paired on GL. */
            shape->kind = SH_DECL_VALUE_REFERENCE; shape->effects = SH_DECL_EFFECTS_GLOBAL; break;
        }
    } else if (!strcmp(ops, "*")) {
        /* Not a class by exact spelling (VK 0x141a1d500 / GL 0x141a0f740 match
         * case-sensitively, so `const T` is no class): the dispatch then tries
         * the enum index (VK 0x141a1d990 / GL 0x141a0fbd0) and its pointer
         * reader in table +0x70. Without one, NULL stores and anything else
         * is skipped, as for a class with no pointer reader. */
        dn_type *named = dn_find(&s->enums, value.name);
        if (named && (!s->enum_pointer_readers || named->pointer_reader)) return 0;
        shape->kind = SH_DECL_VALUE_IGNORE;
        shape->assigns = SH_DECL_ASSIGN_NULL; shape->effects = SH_DECL_EFFECTS_NONE;
    } else if (*ops) return 0;
    else if (type) {
        if ((s->movie_file_reader && type->object_reader == s->movie_file_reader) || dn_preview_model(s, type)) {
            shape->kind = SH_DECL_VALUE_REFERENCE;
            /* idSWFFile (VK 0x1409cf9f0 / GL 0x1409cf1b0) interns its string and
             * constructs and frees a temporary movie. */
            if (s->movie_file_reader && type->object_reader == s->movie_file_reader) shape->effects = SH_DECL_EFFECTS_GLOBAL;
        }
        else if (s->string_reader && type->object_reader == s->string_reader && dn_by_name_decl(s, value.name, shape))
            ;
        else if ((s->string_reader && type->object_reader == s->string_reader) ||
            (s->atomic_string_reader && type->object_reader == s->atomic_string_reader) ||
            (s->color_reader && type->object_reader == s->color_reader) ||
            (s->anim_path_reader && type->object_reader == s->anim_path_reader) ||
            (s->atomic_anim_path_reader && type->object_reader == s->atomic_anim_path_reader) ||
            (s->alias_handle_reader && type->object_reader == s->alias_handle_reader) ||
            (s->atomic_alias_handle_reader && type->object_reader == s->atomic_alias_handle_reader) ||
            (s->managed_handle_reader && type->object_reader == s->managed_handle_reader) ||
            (s->time_container_reader && type->object_reader == s->time_container_reader) ||
            (s->random_value_reader && type->object_reader == s->random_value_reader) ||
            (s->type_info_ptr_reader && type->object_reader == s->type_info_ptr_reader) || dn_primitive(value.name)) {
            shape->kind = SH_DECL_VALUE_IGNORE;
            shape->plain = !type->object_reader && dn_primitive(value.name);
            dn_effects(s, type->object_reader, shape);
        }
        else if ((s->float_wrapper_reader && type->object_reader == s->float_wrapper_reader) ||
            (s->time_wrapper_reader && type->object_reader == s->time_wrapper_reader)) {
            /* This registered reader consumes either a float or { value = float; }.
             * Verify the reflected element too; other templates remain unknown. */
            if (!dn_load(s, type) || !type->element.name || strcmp(type->element.name, "float") ||
                !type->element.ops || *type->element.ops) return 0;
            shape->kind = SH_DECL_VALUE_IGNORE;
            dn_effects(s, type->object_reader, shape);
        }
        else if ((s->bounded_int_reader && type->object_reader == s->bounded_int_reader) ||
            (s->bounded_float_reader && type->object_reader == s->bounded_float_reader)) {
            /* idBoundedIntBase / idBoundedFloatBase (VK 0x1409ca740, 0x1409ca700 /
             * GL 0x1409c9f00, 0x1409c9ec0) read one integer or float token (0
             * when it does not parse) and pass it to the field object's slot 0.
             * Each specialization's setter (VK 0x140926850, 0x140873770 / GL
             * 0x140925e00, 0x140872d50, the only bodies installed by reflected
             * owners) clamps that value and stores it at field+8 alone, never
             * reading the earlier value: a total, field-confined assignment. */
            shape->kind = SH_DECL_VALUE_IGNORE; shape->effects = SH_DECL_EFFECTS_FIELD;
            shape->assigns = type->object_reader == s->bounded_int_reader ? SH_DECL_ASSIGN_INTEGER : SH_DECL_ASSIGN_NUMBER;
        }
        else if (s->event_call_reader && type->object_reader == s->event_call_reader) {
            /* VK 0x1409cbaa0 / GL 0x1409cb260 finds the event, then its argument
             * reader (VK 0x1409caa70) finds decls with makeDefault, finds or loads
             * animations, interns aliases and resolves entity names. */
            shape->kind = SH_DECL_VALUE_EVENT_CALL; shape->effects = SH_DECL_EFFECTS_GLOBAL;
        }
        else if (s->polymorphic_reader && type->object_reader == s->polymorphic_reader) {
            if (!dn_load(s, type) || !type->element.name || !*type->element.name ||
                !type->element.ops || *type->element.ops || !dn_find(&s->classes, type->element.name)) return 0;
            /* This reader leaves the destination unchanged for NULL forms
             * (decl_polymorphic.h, sh_decl_polymorphic_unchanged). A selected
             * class is constructed through its type's factory (+0x28), the
             * earlier object destroyed (VK 0x1409c5fc0, 0x1409d2440 / GL
             * 0x1409c5810, 0x1409d1c00), and the object block read by the
             * type-level reader (VK 0x141a21260 / GL 0x141a134a0). */
            shape->kind = SH_DECL_VALUE_POLYMORPHIC; shape->element = type->element; shape->null_keeps = 1;
            shape->effects = SH_DECL_EFFECTS_GLOBAL;
        }
        else if (type->object_reader && (type->object_reader == s->list_reader || type->object_reader == s->static_list_reader)) {
            if (!dn_load(s, type) || !type->element.name || !*type->element.name) return 0;
            shape->kind = SH_DECL_VALUE_COLLECTION; shape->element = type->element;
            shape->item_key = "item"; shape->count_key = "num";
        } else if (!type->object_reader) shape->kind = SH_DECL_VALUE_OBJECT;
        /* idStr and its subclasses share one registered reader (Vulkan
         * 0x141a21140 -> 0x141a266d0 / OpenGL 0x141a13380 -> 0x141a18a90). Its
         * stub passes on only the lexer and the field; the body assigns a
         * string token, clears the field for any other token, then expects
         * ';'. It uses no container, registry, global or pending registration. */
        if (s->string_reader && type->object_reader == s->string_reader) {
            shape->assigns = SH_DECL_ASSIGN_QUOTED; shape->effects = SH_DECL_EFFECTS_FIELD;
        }
    } else {
        type = dn_find(&s->enums, value.name);
        if (type && !type->object_reader) { shape->kind = SH_DECL_VALUE_IGNORE; shape->plain = 1; }
    }
    return 1;
}

static int dn_field_type(void *context, sh_decl_value_type value, const char *key, sh_decl_value_type *out)
{
    sh_decl_native_schema *s = (sh_decl_native_schema *)context;
    const char *name = value.name;
    size_t depth;
    if (!strncmp(name, DN_SCRIPT_PREFIX, sizeof(DN_SCRIPT_PREFIX) - 1)) {
        const sh_decl_script_type *script = sh_decl_script_schema_find(s->source.scripts,
            name + sizeof(DN_SCRIPT_PREFIX) - 1);
        const sh_decl_script_field *field;
        if (!script) return -1;
        if (!script->has_fields) return 0;
        for (field = script->fields; field; field = field->next)
            if (!strcmp(field->name, key)) return dn_script_value(s, field->type, out) ? 1 : -1;
        return 0;
    }
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

/* The bytes the type reader stores for a value of name with modifier ops, as
 * the native size helper (VK 0x141a220a0 / GL 0x141a14460) computes them: it
 * removes every trailing "[n]", multiplying their counts, then takes 8 for a
 * remaining "*", the class record's +0x10 or 4 for an enum without one, and
 * no size otherwise. Its int arithmetic is unchecked, so a product beyond
 * INT_MAX stays unknown here, as do non-digit counts. -1 unknown. */
static long long dn_value_size(sh_decl_native_schema *s, const char *name, const char *ops)
{
    size_t length = strlen(ops);
    long long count = 1, size;
    while (length && ops[length - 1] == ']') {
        size_t open = length - 1;
        long long dimension = 0;
        while (open && ops[open - 1] != '[') open--;
        if (!open || open == length - 1) return -1;
        for (size_t i = open; i < length - 1; i++) {
            if (ops[i] < '0' || ops[i] > '9') return -1;
            dimension = dimension * 10 + (ops[i] - '0');
            if (dimension > INT_MAX) return -1;
        }
        count *= dimension;
        if (count > INT_MAX) return -1;
        length = open - 1;
    }
    if (length == 1 && ops[0] == '*') size = 8;
    else if (length) return -1;
    else {
        dn_type *type = dn_find(&s->classes, name);
        if (type) {
            if (!dn_load(s, type) || type->size <= 0) return -1;
            size = type->size;
        } else if (dn_find(&s->enums, name)) size = 4;
        else return -1;
    }
    return size * count > INT_MAX ? -1 : size * count;
}

/* ---- bitfield setter stores ----
 * Every bitfield setter of the pinned Vulkan and OpenGL images (722 each, 13
 * instruction forms) is a short straight-line function of the object (rcx)
 * and the value (edx): an optional constant, an AND clearing the field's bits
 * at [rcx + d], value arithmetic, an OR of the value's bits and RET; or a
 * byte store of the whole value (m84 census). This interpreter accepts only
 * the instructions those forms use and proves the store from the bits each
 * register can hold. Anything else stays unknown. */
typedef struct dn_reg { int defined; uint64_t bits; int constant; uint64_t value; } dn_reg;
static uint64_t dn_mask(int width) { return width >= 64 ? ~0ull : (1ull << width) - 1; }
static void dn_set(dn_reg *r, int width, uint64_t bits)
{
    uint64_t mask = dn_mask(width);
    if (width == 32) { r->defined = 64; r->bits = bits & mask; }
    else { r->bits = (r->bits & ~mask) | (bits & mask); if (r->defined < width) r->defined = width; }
    r->constant = 0;
}
static uint64_t dn_imm(const unsigned char *code, int bytes)
{
    uint64_t value = 0;
    for (int i = 0; i < bytes; i++) value |= (uint64_t)code[i] << (8 * i);
    return value;
}
int sh_decl_bitfield_setter_store(const unsigned char *code, size_t length, sh_decl_bitfield_store *store)
{
    dn_reg regs[3] = {{0, 0, 0, 0}, {0, 0, 0, 0}, {32, 0xffffffffull, 0, 0}}; /* rax, rcx (base), rdx (value) */
    int flags = 0, phase = 0, width = 0;
    long long offset = 0;
    uint64_t keep = 0;
    size_t at = 0;
    memset(store, 0, sizeof(*store));
    for (int steps = 0; steps < 16 && at < length; steps++) {
        int wide = 32, op, mod, reg, rm, operand;
        long long disp = 0;
        dn_reg *dst, *src;
        if (code[at] == 0x66) { wide = 16; if (++at >= length) return 0; }
        op = code[at++];
        if (op == 0xc3) {
            if (phase != 2) return 0;
            store->offset = offset; store->width = width / 8; store->bits = ~keep & dn_mask(width);
            return store->bits != 0;
        }
        if (phase == 2) return 0; /* the store is the last effect */
        if (op == 0xb8 && wide == 32) { /* mov eax, imm32 */
            if (at + 4 > length) return 0;
            regs[0] = (dn_reg){64, dn_imm(code + at, 4), 1, dn_imm(code + at, 4)};
            at += 4; continue;
        }
        if (op == 0x24 && wide == 32) { /* and al, imm8 */
            if (at >= length || regs[0].defined < 8) return 0;
            dn_set(&regs[0], 8, regs[0].bits & code[at++]); continue;
        }
        if (op == 0x0f) { /* setne al */
            if (at + 2 > length || code[at] != 0x95 || code[at + 1] != 0xc0 || !flags || wide != 32) return 0;
            at += 2; dn_set(&regs[0], 8, 1); continue;
        }
        if (at >= length) return 0;
        mod = code[at] >> 6; reg = code[at] >> 3 & 7; rm = code[at] & 7; at++;
        if (mod == 3) {
            int size = op == 0x80 || op == 0x02 || op == 0x1a ? 8 : wide;
            if (rm != 0 && rm != 2) return 0;
            dst = regs + rm;
            switch (op) {
            case 0x80: case 0x81: case 0x83: { /* and reg, imm */
                int bytes = op == 0x81 ? size / 8 : 1;
                uint64_t imm;
                if (reg != 4 || at + (size_t)bytes > length || dst->defined < size) return 0;
                imm = dn_imm(code + at, bytes);
                if (op == 0x83 && (imm & 0x80)) imm |= ~0xffull;
                at += (size_t)bytes;
                dn_set(dst, size, dst->bits & imm); continue;
            }
            case 0xc1: { /* shl reg, imm8 */
                int count;
                if (reg != 4 || at >= length || dst->defined < size) return 0;
                count = code[at++] & 31;
                dn_set(dst, size, count >= size ? 0 : dst->bits << count); continue;
            }
            case 0xf7: /* neg edx */
                if (reg != 3 || rm != 2 || size != 32 || dst->defined < 32) return 0;
                dn_set(dst, 32, ~0ull); flags = 1; continue;
            case 0x85: /* test edx, edx */
                if (reg != 2 || rm != 2 || size != 32 || dst->defined < 32) return 0;
                flags = 1; continue;
            case 0x1a: /* sbb al, al */
                if (reg != 0 || rm != 0 || wide != 32 || !flags) return 0;
                dn_set(dst, 8, 0xff); continue;
            case 0x02: case 0x03: /* add reg, reg: the same register doubles */
                if (reg != rm || dst->defined < size) return 0;
                dn_set(dst, size, dst->bits << 1); continue;
            default: return 0;
            }
        }
        /* Memory: only [rcx], [rcx + disp8] and [rcx + disp32]. */
        if (rm != 1) return 0;
        if (mod == 1) { if (at >= length) return 0; disp = (signed char)code[at++]; }
        else if (mod == 2) {
            if (at + 4 > length) return 0;
            disp = (int32_t)(uint32_t)dn_imm(code + at, 4);
            at += 4;
        }
        operand = op == 0x80 || op == 0x08 || op == 0x20 || op == 0x88 ? 8 : wide;
        if (width && (width != operand || offset != disp)) return 0;
        width = operand; offset = disp;
        src = reg == 0 || reg == 2 ? regs + reg : NULL;
        switch (op) {
        case 0x80: case 0x81: case 0x83: { /* and [m], imm: clears the field */
            int bytes = op == 0x81 ? width / 8 : 1;
            uint64_t imm;
            if (reg != 4 || phase != 0 || at + (size_t)bytes > length) return 0;
            imm = dn_imm(code + at, bytes);
            if (op == 0x83 && (imm & 0x80)) imm |= ~0xffull;
            at += (size_t)bytes;
            keep = imm & dn_mask(width); phase = 1; continue;
        }
        case 0x20: case 0x21: /* and [m], a constant register */
            if (!src || phase != 0 || !src->constant) return 0;
            keep = src->value & dn_mask(width); phase = 1; continue;
        case 0x08: case 0x09: /* or [m], reg: only the cleared bits */
            if (!src || phase != 1 || src->defined < width || (src->bits & dn_mask(width) & keep)) return 0;
            phase = 2; continue;
        case 0x88: case 0x89: /* mov [m], reg: the whole width */
            if (!src || phase != 0 || src->defined < width) return 0;
            keep = 0; phase = 2; continue;
        default: return 0;
        }
    }
    return 0;
}

/* The member walker resolves a name in the class, then each super, and passes
 * the object plus the variable's offset to the type reader (VK 0x141a21260 /
 * GL 0x141a134a0). That reader's result is the bytes it stores there: 8 for a
 * pointer (the pointee is separate storage); for a type without a modifier,
 * the struct reader's return (VK 0x141a209b0 / GL 0x141a12c00: the type
 * record's +0x10, or 4 for an enum); for an array, the array reader's (VK
 * 0x141a202d0 / GL 0x141a12510): it stores each element whose index is in
 * range at index * size(T), requires the element reader to return exactly
 * size(T), and returns count * size(T) (dn_value_size). A bitfield token goes
 * to the variable's setter with the object itself (the walker adds no
 * offset), so its storage is the setter's verified store or unknown. Script
 * module fields have no described footprint. */
static int dn_storage(void *context, sh_decl_value_type value, const char *key, sh_decl_member_storage *out)
{
    sh_decl_native_schema *s = (sh_decl_native_schema *)context;
    const char *name = value.name;
    memset(out, 0, sizeof(*out));
    if (!name || !strncmp(name, DN_SCRIPT_PREFIX, sizeof(DN_SCRIPT_PREFIX) - 1)) return -1;
    for (size_t depth = 0; *name && depth < s->classes.count; depth++) {
        dn_type *type = dn_find(&s->classes, name);
        if (!type || !dn_load(s, type)) return -1;
        for (const dn_field *field = type->fields; field; field = field->next) if (!strcmp(field->name, key)) {
            long long size;
            if (field->size < 0) {
                unsigned char code[32];
                sh_decl_bitfield_store store;
                int first = 0, last = 63;
                if (field->setter && dn_read(s, field->setter, 0, code, sizeof(code)) &&
                    sh_decl_bitfield_setter_store(code, sizeof(code), &store) && store.offset >= 0) {
                    while (!(store.bits >> first & 1)) first++;
                    while (!(store.bits >> last & 1)) last--;
                    out->offset = store.offset; out->extent = store.width;
                    out->bit_first = first; out->bit_count = last - first + 1;
                    /* The bitfield reader (VK 0x141a20600 / GL 0x141a12840)
                     * parses bool as a name token compared case-sensitively
                     * with true and false (VK 0x141a25f80 / GL 0x141a18340;
                     * another name only warns and passes an unset value), and
                     * int, unsigned int, char and unsigned char with the
                     * lexer's int parse (VK 0x141a0c0a0 / GL 0x1419fe2d0: an
                     * integer token, optionally after '-'; others are errors),
                     * then expects ';' and calls the setter. */
                    if (!strcmp(field->type.name, "bool")) out->assigns = SH_DECL_ASSIGN_BOOLEAN;
                    else if (!strcmp(field->type.name, "int") || !strcmp(field->type.name, "unsigned int") ||
                        !strcmp(field->type.name, "char") || !strcmp(field->type.name, "unsigned char"))
                        out->assigns = SH_DECL_ASSIGN_INTEGER;
                }
                return 2;
            }
            if (field->offset < 0 || (size = dn_value_size(s, field->type.name, field->type.ops)) <= 0) return -1;
            out->offset = field->offset; out->extent = size;
            return 1;
        }
        name = type->super;
    }
    return *name ? -1 : 0;
}

static int dn_reference(void *context, const char *path, sh_decl_value_type type, const char *name, size_t length)
{
    sh_decl_native_schema *s = (sh_decl_native_schema *)context;
    return s->observer.reference ? s->observer.reference(s->observer.context, path, type, name, length) : 0;
}
static int dn_reference_site(void *context, const char *path, sh_decl_value_type type,
    const char *name, size_t length, const sh_decl_reference_site *site)
{
    sh_decl_native_schema *s = context;
    return s->observer.reference_site ? s->observer.reference_site(s->observer.context,
        path, type, name, length, site) : 1;
}
static int dn_empty_lookup(void *context, sh_decl_value_type type)
{
    sh_decl_native_schema *s = context;
    /* A by-name lookup passes makeDefault 0, which returns null for "". */
    if (type.ops && !strcmp(type.ops, SH_DECL_BY_NAME_DECL_OPS)) return 0;
    return s->observer.empty_names && s->observer.empty_names(s->observer.context) &&
        sh_decl_native_schema_reader(s, type) == SH_DECL_READER_DECL;
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
    type = base.name ? dn_find(&s->classes, base.name) : NULL;
    if (s && type && base.ops && !strcmp(base.ops, "*") && s->script_reader &&
        type->pointer_reader == s->script_reader) {
        const sh_decl_script_type *script;
        if (!name || !length || length == SIZE_MAX || memchr(name, 0, length) ||
            !(terminated = dn_copy(&s->owned, name, length))) return 0;
        script = sh_decl_script_schema_find(s->source.scripts, terminated);
        if (!script || !script->constructible) return 0;
        /* The module may expose a constructible object with no serial fields. */
        return dn_script_value(s, terminated, selected);
    }
    if (!s || !base.name || (base.ops && *base.ops) || !name || !length || length == SIZE_MAX ||
        memchr(name, 0, length) || !(terminated = malloc(length + 1))) return 0;
    memcpy(terminated, name, length); terminated[length] = 0;
    type = dn_find(&s->classes, terminated); free(terminated);
    if (!type || sh_decl_native_schema_class_derives(s, type->name, base.name) != 1) return 0;
    *selected = (sh_decl_value_type){type->name, ""}; return 1;
}

/* Inside a save scope (sh_decl_native_schema_scope) the image's reflection
 * registry is indexed once per source: every later open over the same source
 * shares that index (sh_decl_native_schema_open_shared), as a library compile's
 * readers already do. The registry is the executable's static type data. */
#define DN_SCOPE_BASES 8
static struct { int depth; sh_decl_native_schema *bases[DN_SCOPE_BASES]; size_t count; } g_dn_scope;
static sh_decl_native_schema *dn_open_indexed(sh_decl_native_source source,
    sh_decl_dependency_observer observer, char *error, size_t error_capacity);
void sh_decl_native_schema_scope(int enter)
{
    if (enter) { g_dn_scope.depth++; return; }
    if (g_dn_scope.depth <= 0 || --g_dn_scope.depth) return;
    for (size_t i = 0; i < g_dn_scope.count; i++) sh_decl_native_schema_close(g_dn_scope.bases[i]);
    memset(&g_dn_scope, 0, sizeof(g_dn_scope));
}
sh_decl_native_schema *sh_decl_native_schema_open(sh_decl_native_source source,
    sh_decl_dependency_observer observer, char *error, size_t error_capacity)
{
    if (!g_dn_scope.depth) return dn_open_indexed(source, observer, error, error_capacity);
    for (size_t i = 0; i < g_dn_scope.count; i++) {
        const sh_decl_native_schema *base = g_dn_scope.bases[i];
        if (base->source.read == source.read && base->source.context == source.context &&
            base->source.reflection == source.reflection && base->source.scripts == source.scripts)
            return sh_decl_native_schema_open_shared(g_dn_scope.bases[i], source, observer, error, error_capacity);
    }
    sh_decl_native_schema *base = dn_open_indexed(source, (sh_decl_dependency_observer){0}, error, error_capacity);
    if (!base) return NULL;
    if (g_dn_scope.count == DN_SCOPE_BASES) {
        sh_decl_native_schema_close(base);
        return dn_open_indexed(source, observer, error, error_capacity);
    }
    g_dn_scope.bases[g_dn_scope.count++] = base;
    return sh_decl_native_schema_open_shared(base, source, observer, error, error_capacity);
}
static sh_decl_native_schema *dn_open_indexed(sh_decl_native_source source,
    sh_decl_dependency_observer observer, char *error, size_t error_capacity)
{
    sh_decl_native_schema *s = NULL;
    /* Order matches sh_decl_reference_reader after UNKNOWN. idSWF's pointer
     * reader constructs a movie from its string; empty means null. */
    const char *const resources[] = {"idDeclEntityDef", "idImage", "idRenderModel", "idStaticModel",
        "idStaticParticleModelData", "idSWF"};
    uintptr_t container;
    dn_type *type;
    size_t i;
    if (error && error_capacity) error[0] = 0;
    if (sizeof(uintptr_t) != 8 || !source.read || !source.reflection) goto failed;
    s = (sh_decl_native_schema *)calloc(1, sizeof(*s));
    if (!s) goto failed;
    s->source = source; s->observer = observer; s->origin = s; s->holders = 1;
    if (!dn_pointer(s, source.reflection, 0, &container) || !container ||
        !dn_registry(s, container, 1) || !dn_registry(s, container, 0)) goto failed;
    for (i = 0; i < sizeof(resources) / sizeof(resources[0]); i++) {
        type = dn_find(&s->classes, resources[i]);
        if (type) s->references[i] = type->pointer_reader;
    }
    type = dn_find(&s->classes, "idStr"); if (type) s->string_reader = type->object_reader;
    /* The atomic-string reader only interns text. Later consumers can assign
     * resource meaning to that text, but deserialization itself has no edge. */
    type = dn_find(&s->classes, "idAtomicString"); if (type) s->atomic_string_reader = type->object_reader;
    /* idStrId's registered reader (dn_effects). */
    type = dn_find(&s->classes, "idStrId"); if (type) s->string_id_reader = type->object_reader;
    /* Both native readers consume four numeric channels and optional color
     * space conversion. This registered reader creates no resource binding. */
    type = dn_find(&s->classes, "idColor"); if (type) s->color_reader = type->object_reader;
    /* These readers preserve animation-state paths (normalizing separators and
     * optionally interning path components); they do not load asset files.
     * Later animation consumers still need their own binding interpretation. */
    type = dn_find(&s->classes, "idAnimWebPath"); if (type) s->anim_path_reader = type->object_reader;
    type = dn_find(&s->classes, "idAnimWebPath_Atomic"); if (type) s->atomic_anim_path_reader = type->object_reader;
    /* Alias handles intern model-local animation names; their readers do not
     * resolve resource paths. The model/animation binding supplies the context.
     * Other handle families are not covered by these two registrations. */
    type = dn_find(&s->classes, "idHandle < unsigned short , invalidAliasHandle_t , INVALID_ALIAS_HANDLE >");
    if (type) s->alias_handle_reader = type->object_reader;
    type = dn_find(&s->classes, "idAtomicHandleT < aliasRefType_t >");
    if (type) s->atomic_alias_handle_reader = type->object_reader;
    /* These handles refer to entity instances in the current world or its
     * unresolved-name table. Their strings are not declaration/resource names.
     * Require the common registered reader across all native specializations. */
    s->managed_handle_reader = dn_family(&s->classes, "idManagedClassPtr < ");
    /* Native class identities refer to the executable's type registry, not
     * idDeclTypeInfo resources. Object-pointer wrappers below remain polymorphic. */
    type = dn_find(&s->classes, "idTypeInfo"); if (type) s->type_info_reader = type->pointer_reader;
    s->type_info_ptr_reader = dn_family(&s->classes, "idTypeInfoPtr < ");
    type = dn_find(&s->classes, "idTypesafeNumber < float , DegreesUnique_t >");
    if (type) s->float_wrapper_reader = type->object_reader;
    /* The game registers every float-time specialization with one numeric
     * reader. Require matching registrations and a reflected float element. */
    s->time_wrapper_reader = dn_family(&s->classes, "idTypesafeTime < float ,");
    /* Consumes a millisecond integer or a block's milliseconds field, then
     * derives seconds/ticks. Both registered renderer readers perform no
     * resource lookup, including their skipped unknown field branch. */
    type = dn_find(&s->classes, "timeContainer_t");
    if (type) s->time_container_reader = type->object_reader;
    /* Reads numeric min/max/granularity values (or one constant), skipping unknown
     * fields. Its registered reader has no resource dependencies. */
    type = dn_find(&s->classes, "randomValue_t");
    if (type) s->random_value_reader = type->object_reader;
    type = dn_find(&s->classes, "idSSObject");
    if (type) s->script_reader = type->pointer_reader;
    /* idSWFFile stores the same constructor string by value (and can precache
     * a temporary movie). Only its registered reader establishes the contract. */
    type = dn_find(&s->classes, "idSWFFile");
    if (type) s->movie_file_reader = type->object_reader;
    /* idEventCallInfo's registered reader (Vulkan 0x1409cbaa0 / OpenGL
     * 0x1409cb260): the event name, then counted typed arguments. */
    type = dn_find(&s->classes, "idEventCallInfo");
    if (type) s->event_call_reader = type->object_reader;
    /* Registered by their base names (VK 0x1409c79d0 / GL 0x1409c7190);
     * specializations share the base's registered reader. */
    type = dn_find(&s->classes, "idBoundedIntBase"); if (type) s->bounded_int_reader = type->object_reader;
    type = dn_find(&s->classes, "idBoundedFloatBase"); if (type) s->bounded_float_reader = type->object_reader;
    type = dn_find(&s->classes, "idVoiceTrack"); if (type) s->voice_track_reader = type->pointer_reader;
    type = dn_find(&s->classes, "idCollisionModel"); if (type) s->collision_model_reader = type->pointer_reader;
    /* SnapMap preview-model fields share idStr's reader, so only the declared
     * type identifies them. The editor's entity preview and interact models
     * (Vulkan 0x140d33770 / OpenGL 0x140d32e30) and attachments (0x140d31a20 /
     * 0x140d310e0) pass the text to its model dispatcher (0x1405df8a0 /
     * 0x1405df180). Consumers of the type's other fields were not traced. */
    s->preview_model = dn_find(&s->classes, "idStrRenderModel_Preview");
    s->list_reader = dn_family(&s->classes, "idList < ");
    s->static_list_reader = dn_family(&s->classes, "idStaticList < ");
    s->polymorphic_reader = dn_family(&s->classes, "idTypeInfoObjectPtr < ");
    s->schema.context = s; s->schema.describe = dn_describe; s->schema.field = dn_field_type; s->schema.storage = dn_storage;
    s->schema.reference = dn_reference; s->schema.gap = dn_gap;
    s->schema.reference_site = dn_reference_site;
    s->schema.dynamic_type = dn_dynamic_type;
    s->schema.empty_lookup = dn_empty_lookup;
    return s;
failed:
    if (error && error_capacity) snprintf(error, error_capacity, "native reflection metadata is unavailable or inconsistent");
    sh_decl_native_schema_close(s); return NULL;
}

/* The sharing schema copies base's registry view (index slots, reader
 * identities, preview type) and reads its origin's records; it keeps its own
 * observer, registry and answers. */
sh_decl_native_schema *sh_decl_native_schema_open_shared(sh_decl_native_schema *base,
    sh_decl_native_source source, sh_decl_dependency_observer observer, char *error, size_t error_capacity)
{
    sh_decl_native_schema *s;
    if (!base || base->source.read != source.read || base->source.context != source.context ||
        base->source.reflection != source.reflection || base->source.scripts != source.scripts)
        return sh_decl_native_schema_open(source, observer, error, error_capacity);
    if (error && error_capacity) error[0] = 0;
    s = (sh_decl_native_schema *)malloc(sizeof(*s));
    if (!s) {
        if (error && error_capacity) snprintf(error, error_capacity, "native reflection metadata is unavailable or inconsistent");
        return NULL;
    }
    *s = *base;
    s->observer = observer; s->schema.context = s;
    s->holders = 0; s->records = NULL; s->owned = NULL; s->registry = NULL;
    s->origin->holders++;
    return s;
}

const sh_decl_dependency_schema *sh_decl_native_schema_view(sh_decl_native_schema *s)
{ return s ? &s->schema : NULL; }

void sh_decl_native_schema_attach_registry(sh_decl_native_schema *s, sh_decl_native_registry *registry)
{ if (s) s->registry = registry; }

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

/* Whether a reflected layer of name's ancestry declares key, following the
 * superclass names only while the inventory has them. 1 declared, 0 not, -1
 * when a layer cannot be read or the ancestry cycles. */
static int dn_layer_declares(sh_decl_native_schema *s, const char *name, const char *key)
{
    for (size_t depth = 0; depth < s->classes.count; depth++) {
        dn_type *type;
        if (!name || !*name || !(type = dn_find(&s->classes, name))) return 0;
        if (!dn_load(s, type)) return -1;
        for (const dn_field *field = type->fields; field; field = field->next)
            if (!strcmp(field->name, key)) return 1;
        name = type->super;
    }
    return -1;
}

/* A member base_name does not reflect is read only by an entity whose class
 * derives from it (entityDef inheritance requires the child's class to derive
 * from its parent's) and reflects that member, through the same first-match
 * field lookup as dn_field_type. Every such class in the complete reflected
 * inventory is consulted. The live inventory also holds classes whose
 * superclass it lacks (idSaveGameProcessorDoomLoad's
 * idSaveGameProcessorLoadFiles, for one), so their derivation is
 * unknown; such a class is judged by its reflected layers, and only one that
 * declares the member leaves the type undecided. */
int sh_decl_native_schema_descendant_field(sh_decl_native_schema *s,
    const char *base_name, const char *key, sh_decl_value_type *out)
{
    sh_decl_value_type found = {0}, type;
    int status;
    if (!s || !base_name || !*base_name || !key || !*key || !out) return -1;
    *out = (sh_decl_value_type){0};
    if (!dn_find(&s->classes, base_name)) return -1;
    status = dn_field_type(s, (sh_decl_value_type){base_name, ""}, key, &type);
    if (status) return status < 0 ? -1 : 2;
    for (size_t i = 0; i < s->classes.capacity; i++) {
        const dn_type *candidate = s->classes.slots[i];
        int derives;
        if (!candidate || !strcmp(candidate->name, base_name)) continue;
        derives = sh_decl_native_schema_class_derives(s, candidate->name, base_name);
        if (derives < 0) {
            if (dn_layer_declares(s, candidate->name, key)) return -1;
            continue;
        }
        if (!derives) continue;
        status = dn_field_type(s, (sh_decl_value_type){candidate->name, ""}, key, &type);
        if (status < 0) return -1;
        if (!status) continue;
        if (!type.name || !type.ops) return -1;
        if (found.name && (strcmp(found.name, type.name) || strcmp(found.ops, type.ops))) return -1;
        found = type;
    }
    if (!found.name) return 0;
    *out = found; return 1;
}

int sh_decl_native_schema_class_known(sh_decl_native_schema *s, const char *class_name)
{
    if (!s || !class_name || !*class_name) return -1;
    return dn_find(&s->classes, class_name) != NULL;
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
    if (!s || !value.name || !value.ops) return SH_DECL_READER_UNKNOWN;
    if (!strcmp(value.ops, DN_SCRIPT_DECL))
        return sh_decl_native_schema_class_derives(s, value.name, "idDecl") == 1 ?
            SH_DECL_READER_DECL : SH_DECL_READER_UNKNOWN;
    if (!*value.ops) {
        type = dn_find(&s->classes, value.name);
        if (dn_preview_model(s, type)) return SH_DECL_READER_PREVIEW_MODEL;
        return type && s->movie_file_reader && type->object_reader == s->movie_file_reader ?
            SH_DECL_READER_MOVIE : SH_DECL_READER_UNKNOWN;
    }
    if (!strcmp(value.ops, SH_DECL_EVENT_ANIM_OPS)) {
        /* An event md6Anim use (dn_event_anim) reads with its class's pointer reader. */
        if (strcmp(value.name, SH_DECL_EVENT_ANIM_TYPE)) return SH_DECL_READER_UNKNOWN;
        return sh_decl_native_schema_reader(s, (sh_decl_value_type){value.name, "*"}) == SH_DECL_READER_IMAGE ?
            SH_DECL_READER_IMAGE : SH_DECL_READER_UNKNOWN;
    }
    if (!strcmp(value.ops, SH_DECL_EVENT_DECL_USE_OPS) || !strcmp(value.ops, SH_DECL_BY_NAME_DECL_OPS))
        /* An event decl use (dn_event_decl) or a by-name reference (dn_by_name_decl)
         * names a declaration of its class's manager, like its pointer reader. */
        return sh_decl_native_schema_reader(s, (sh_decl_value_type){value.name, "*"}) == SH_DECL_READER_DECL ?
            SH_DECL_READER_DECL : SH_DECL_READER_UNKNOWN;
    if (strcmp(value.ops, "*")) return SH_DECL_READER_UNKNOWN;
    type = dn_find(&s->classes, value.name);
    if (type && type->pointer_reader) for (i = 0; i < sizeof(s->references) / sizeof(s->references[0]); i++)
        if (s->references[i] == type->pointer_reader) return (sh_decl_reference_reader)(i + 1);
    /* VK 0x1409d1180 / GL 0x1409d0940 and VK 0x1409ca770 / GL 0x1409c9f30 use
     * the lists built as ("voicetrack", "idVoiceTrack") and ("cm",
     * "idCollisionModelLocal") (VK 0x1400f7050, 0x1402224c0 / GL 0x1400f7050,
     * 0x140221d40), whichever class the field declares. */
    if (type && s->voice_track_reader && type->pointer_reader == s->voice_track_reader) return SH_DECL_READER_VOICE_TRACK;
    if (type && s->collision_model_reader && type->pointer_reader == s->collision_model_reader)
        return SH_DECL_READER_COLLISION_MODEL;
    return SH_DECL_READER_UNKNOWN;
}

int sh_decl_native_schema_enum_constants(sh_decl_native_schema *s, const char *enum_name,
    int (*visit)(void *context, const char *name, uint32_t value), void *context)
{
    dn_type *type;
    uintptr_t members;
    if (!s || !enum_name || !visit) return -1;
    if (!(type = dn_find(&s->enums, enum_name))) return 0;
    if (!dn_pointer(s, type->address, 0x10, &members)) return -1;
    for (size_t i = 0; members && i < 4096; i++) {
        uintptr_t name_at;
        uint32_t value;
        char *name;
        if (!dn_pointer(s, members, i * 0x10, &name_at)) return -1;
        if (!name_at) return 1;
        if (!(name = dn_string(s, &s->owned, name_at)) || !dn_read(s, members, i * 0x10 + 8, &value, sizeof(value))) return -1;
        if (!*name) return 1;
        if (!visit(context, name, value)) return -1;
    }
    return members ? -1 : 1;
}

typedef struct dn_digest { int (*emit)(void *, const void *, size_t); void *context; int ok; } dn_digest;
static void dn_put(dn_digest *d, const void *bytes, size_t length)
{ if (d->ok && !d->emit(d->context, bytes, length)) d->ok = 0; }
static void dn_put_word(dn_digest *d, uint64_t value) { dn_put(d, &value, sizeof(value)); }
static void dn_put_text(dn_digest *d, const char *text)
{
    dn_put_word(d, text ? 1 : 0);
    if (text) dn_put(d, text, strlen(text) + 1);
}
static int dn_digest_constant(void *context, const char *name, uint32_t value)
{ dn_digest *d = context; dn_put_text(d, name); dn_put(d, &value, sizeof(value)); return d->ok; }
int sh_decl_native_schema_digest(sh_decl_native_schema *s,
    int (*emit)(void *context, const void *bytes, size_t length), void *context)
{
    dn_digest d = {emit, context, 1};
    if (!s || !emit) return 0;
    for (int enums = 0; enums < 2 && d.ok; enums++) {
        const dn_index *index = enums ? &s->enums : &s->classes;
        dn_put_word(&d, index->count);
        for (size_t i = 0; i < index->capacity && d.ok; i++) {
            dn_type *type = index->slots[i];
            if (!type) continue;
            dn_put_text(&d, type->name);
            dn_put_word(&d, type->object_reader); dn_put_word(&d, type->pointer_reader);
            if (enums) {
                int status = sh_decl_native_schema_enum_constants(s, type->name, dn_digest_constant, &d);
                dn_put_word(&d, (uint64_t)(int64_t)status);
                continue;
            }
            if (!dn_load(s, type)) { dn_put_word(&d, 0); continue; }
            size_t fields = 0;
            for (const dn_field *field = type->fields; field; field = field->next) fields++;
            dn_put_word(&d, 1); dn_put_text(&d, type->super); dn_put(&d, &type->size, sizeof(type->size));
            dn_put_text(&d, type->element.name); dn_put_text(&d, type->element.ops);
            dn_put_word(&d, fields);
            for (const dn_field *field = type->fields; field; field = field->next) {
                dn_put_text(&d, field->name); dn_put_text(&d, field->type.name); dn_put_text(&d, field->type.ops);
                dn_put(&d, &field->offset, sizeof(field->offset)); dn_put(&d, &field->size, sizeof(field->size));
                if (field->size < 0) {
                    /* The storage a bitfield setter's code proves (dn_storage). */
                    sh_decl_member_storage storage = {0};
                    dn_storage(s, (sh_decl_value_type){type->name, ""}, field->name, &storage);
                    dn_put_word(&d, (uint64_t)storage.offset); dn_put_word(&d, (uint64_t)storage.extent);
                    dn_put_word(&d, (uint64_t)storage.bit_first); dn_put_word(&d, (uint64_t)storage.bit_count);
                    dn_put_word(&d, (uint64_t)storage.assigns);
                }
            }
        }
    }
    return d.ok;
}
void sh_decl_native_schema_close(sh_decl_native_schema *s)
{
    sh_decl_native_schema *origin;
    if (!s) return;
    origin = s->origin;
    dn_free(&s->owned);
    if (s != origin) free(s);
    if (--origin->holders) return;
    dn_free(&origin->records);
    free(origin->classes.slots); free(origin->enums.slots); free(origin);
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
