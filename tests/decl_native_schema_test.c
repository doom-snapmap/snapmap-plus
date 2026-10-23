#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/backend/decl_native_schema.h"
#include "../src/backend/decl_compose.h"
#include "../src/backend/decl_graph_dependencies.h"
#include "../src/backend/decl_graph_compose.h"

typedef struct region { void *data; size_t size; } region;
typedef struct fixture {
    region regions[320];
    size_t region_count;
    unsigned char *reflect, *records, *objects, *pointers;
    uintptr_t unreadable;
    char references[4096];
    size_t gaps;
    const sh_decl_script_schema *scripts;
    uintptr_t script_tables[2];
    size_t script_lookups;
    int script_fail;
} fixture;

static void *allocate(fixture *f, size_t size)
{
    void *memory = calloc(1, size);
    assert(memory && f->region_count < sizeof(f->regions) / sizeof(f->regions[0]));
    f->regions[f->region_count++] = (region){memory, size}; return memory;
}
static char *text(fixture *f, const char *value)
{
    size_t size = strlen(value) + 1;
    char *copy = (char *)allocate(f, size); memcpy(copy, value, size); return copy;
}
static void pointer(void *memory, size_t offset, const void *value)
{ uintptr_t address = (uintptr_t)value; memcpy((char *)memory + offset, &address, sizeof(address)); }
static void number(void *memory, size_t offset, int32_t value)
{ memcpy((char *)memory + offset, &value, sizeof(value)); }
static int read_memory(void *context, uintptr_t address, void *out, size_t length)
{
    fixture *f = (fixture *)context;
    size_t i;
    if (f->unreadable == address) return 0;
    for (i = 0; i < f->region_count; i++) {
        uintptr_t start = (uintptr_t)f->regions[i].data;
        if (address >= start && address - start <= f->regions[i].size &&
            length <= f->regions[i].size - (size_t)(address - start)) {
            memcpy(out, (const void *)address, length); return 1;
        }
    }
    return 0;
}
static unsigned char *type(fixture *f, size_t index, const char *name, const char *super,
                           uintptr_t object_reader, uintptr_t pointer_reader)
{
    unsigned char *record = f->records + index * 0x38;
    pointer(record, 0, text(f, name)); pointer(record, 8, text(f, super));
    pointer(f->objects, index * 16 + 8, (const void *)object_reader);
    pointer(f->pointers, index * 16 + 8, (const void *)pointer_reader);
    return record;
}
static void field(fixture *f, unsigned char *fields, size_t index, const char *name,
                    const char *type_name, const char *ops, int32_t size)
{
    unsigned char *row = fields + index * 0x48;
    pointer(row, 0, text(f, type_name)); pointer(row, 8, text(f, ops));
    pointer(row, 0x10, text(f, name)); number(row, 0x1c, size);
}
static int reference(void *context, const char *path, sh_decl_value_type value, const char *name, size_t length)
{
    fixture *f = (fixture *)context;
    size_t used = strlen(f->references);
    int written = snprintf(f->references + used, sizeof(f->references) - used,
                           "%s|%s|%.*s\n", path, value.name, (int)length, name);
    assert(written > 0 && (size_t)written < sizeof(f->references) - used); return 1;
}
static void gap(void *context, const char *path, sh_decl_value_type value, const char *reason)
{ fixture *f = (fixture *)context; (void)value; assert(path && reason); f->gaps++; }

static void setup(fixture *f)
{
    unsigned char *container, *record, *fields, *element, *enums, *enum_readers;
    size_t count = 61;
    memset(f, 0, sizeof(*f));
    f->reflect = (unsigned char *)allocate(f, 0xc0);
    container = (unsigned char *)allocate(f, 0x30);
    f->records = (unsigned char *)allocate(f, (count + 1) * 0x38);
    f->objects = (unsigned char *)allocate(f, (count + 1) * 16);
    f->pointers = (unsigned char *)allocate(f, (count + 1) * 16);
    pointer(f->reflect, 0, container); pointer(container, 0x20, f->records);
    pointer(f->reflect, 0x88, f->objects); number(f->reflect, 0x90, (int32_t)count + 1);
    pointer(f->reflect, 0xa0, f->pointers); number(f->reflect, 0xa8, (int32_t)count + 1);
    type(f, 0, "idDeclEntityDef", "idDecl", 0, 0x10);
    type(f, 1, "idDeclInventory", "idDecl", 0, 0x10);
    type(f, 2, "idStr", "", 0x20, 0);
    type(f, 3, "idAtomicString", "", 0x30, 0);
    type(f, 4, "int", "", 0x40, 0);
    record = type(f, 5, "Base", "", 0, 0);
    fields = (unsigned char *)allocate(f, 2 * 0x48); pointer(record, 0x20, fields);
    field(f, fields, 0, "target", "idDeclEntityDef", "*", 8);
    record = type(f, 6, "Entity", "Base", 0, 0);
    fields = (unsigned char *)allocate(f, 29 * 0x48); pointer(record, 0x20, fields);
    field(f, fields, 0, "inventory", "idList < Ref , TAG >", "", 24);
    field(f, fields, 1, "name", "idStr", "", 24);
    field(f, fields, 2, "custom", "OpaqueAtom", "", 8);
    field(f, fields, 3, "array", "idDeclInventory", "*[2][3]", 48);
    field(f, fields, 4, "mode", "Mode", "", 4);
    field(f, fields, 5, "bit", "int", "", -1);
    field(f, fields, 6, "mystery", "CustomList < Ref >", "", 24);
    field(f, fields, 7, "angle", "idTypesafeNumber < float , DegreesUnique_t >", "", 4);
    field(f, fields, 8, "atomic", "idAtomicString", "", 8);
    field(f, fields, 9, "alias", "SharedAtom", "", 8);
    field(f, fields, 10, "variant", "idTypeInfoObjectPtr < Base >", "", 16);
    field(f, fields, 11, "color", "idColor", "", 16);
    field(f, fields, 12, "duration", "idTypesafeTime < float , secondUnique_t , 1 >", "", 4);
    field(f, fields, 13, "animState", "idAnimWebPath", "", 48);
    field(f, fields, 14, "atomicState", "idAnimWebPath_Atomic", "", 64);
    field(f, fields, 15, "nativeClass", "idTypeInfo", "*", 8);
    field(f, fields, 16, "classReference", "idTypeInfoPtr < Base >", "", 8);
    field(f, fields, 17, "animationAlias", "idHandle < unsigned short , invalidAliasHandle_t , INVALID_ALIAS_HANDLE >", "", 2);
    field(f, fields, 18, "atomicAlias", "idAtomicHandleT < aliasRefType_t >", "", 8);
    field(f, fields, 19, "deadline", "timeContainer_t", "", 12);
    field(f, fields, 20, "volume", "randomValue_t", "", 12);
    field(f, fields, 21, "randomAlias", "RandomAlias", "", 12);
    field(f, fields, 22, "script", "idSSObject", "*", 8);
    field(f, fields, 23, "bindParent", "idManagedClassPtr < idEntity >", "", 24);
    field(f, fields, 24, "targetActor", "idManagedClassPtr < idAI2 >", "", 24);
    field(f, fields, 25, "activateOnArrival", "idList < ManagedRefs >", "", 24);
    field(f, fields, 26, "staticData", "idStaticParticleModelData", "*", 8);
    field(f, fields, 27, "eventCall", "idEventCallInfo", "", 40);
    record = type(f, 7, "idList < Ref , TAG >", "", 0x50, 0);
    element = (unsigned char *)allocate(f, 0x48); pointer(record, 0x18, element);
    field(f, element, 0, "element", "idDeclInventory", "*", 8);
    type(f, 8, "CustomList < Ref >", "", 0x60, 0);
    type(f, 9, "idList < Other , TAG >", "", 0x50, 0);
    record = type(f, 10, "idTypesafeNumber < float , DegreesUnique_t >", "", 0x80, 0);
    element = (unsigned char *)allocate(f, 0x48); pointer(record, 0x18, element);
    field(f, element, 0, "element", "float", "", 4);
    type(f, 11, "idDeclTypeInfo", "", 0, 0);
    type(f, 12, "idDeclSnapEditorEntity", "idDeclTypeInfo", 0, 0);
    type(f, 13, "idMaterial", "Base", 0, 0);
    type(f, 14, "idDeclTypeInfoGraph", "idDeclTypeInfo", 0x90, 0x10);
    record = type(f, 15, "idDeclAttackGraph", "idDeclTypeInfoGraph", 0x90, 0x10);
    fields = (unsigned char *)allocate(f, 3 * 0x48); pointer(record, 0x20, fields);
    field(f, fields, 0, "target", "idDeclInventory", "*", 8);
    field(f, fields, 1, "nestedGraph", "idDeclAttackGraph", "", 24);
    type(f, 16, "idTypeInfoSubGraph", "Base", 0, 0);
    type(f, 17, "idTypeInfoGraphNode", "Base", 0, 0);
    type(f, 18, "idTypeInfoGraphLink", "Base", 0, 0);
    type(f, 19, "TestSubgraph", "idTypeInfoSubGraph", 0, 0);
    type(f, 20, "TestNode", "idTypeInfoGraphNode", 0, 0);
    type(f, 21, "TestLink", "idTypeInfoGraphLink", 0, 0);
    type(f, 22, "UnexpectedGraph", "idDeclTypeInfoGraph", 0x91, 0x10);
    type(f, 23, "OpaqueAtom", "", 0x31, 0);
    type(f, 24, "SharedAtom", "", 0x30, 0);
    record = type(f, 25, "idTypeInfoObjectPtr < Base >", "", 0xa0, 0);
    element = (unsigned char *)allocate(f, 0x48); pointer(record, 0x18, element);
    field(f, element, 0, "element", "Base", "", 16);
    record = type(f, 26, "idTypeInfoObjectPtr < idTypeInfoGraphNode >", "", 0xa0, 0);
    element = (unsigned char *)allocate(f, 0x48); pointer(record, 0x18, element);
    field(f, element, 0, "element", "idTypeInfoGraphNode", "", 16);
    type(f, 27, "CustomEntity", "Base", 0xb0, 0);
    type(f, 28, "idColor", "", 0xc0, 0);
    type(f, 29, "OtherColor", "", 0xc1, 0);
    record = type(f, 30, "idTypesafeTime < float , secondUnique_t , 1 >", "", 0xd0, 0);
    element = (unsigned char *)allocate(f, 0x48); pointer(record, 0x18, element);
    field(f, element, 0, "element", "float", "", 4);
    type(f, 31, "idAnimWebPath", "", 0xe0, 0);
    type(f, 32, "idAnimWebPath_Atomic", "", 0xe1, 0);
    type(f, 33, "idTypeInfo", "", 0, 0xe2);
    type(f, 34, "idTypeInfoPtr < Base >", "", 0xe3, 0);
    type(f, 35, "UnknownPath", "", 0xe4, 0);
    type(f, 36, "UnknownClassPointer", "", 0, 0xe4);
    type(f, 37, "idHandle < unsigned short , invalidAliasHandle_t , INVALID_ALIAS_HANDLE >", "", 0xf0, 0);
    type(f, 38, "idAtomicHandleT < aliasRefType_t >", "", 0xf1, 0);
    type(f, 39, "idAtomicHandleT < otherRefType_t >", "", 0xf2, 0);
    type(f, 40, "timeContainer_t", "", 0xf3, 0);
    type(f, 41, "OtherTimer", "", 0xf4, 0);
    type(f, 42, "idSSObject", "", 0, 0xf5);
    type(f, 43, "idDecl", "", 0, 0x10);
    type(f, 44, "randomValue_t", "", 0xf6, 0);
    type(f, 45, "RandomAlias", "", 0xf6, 0);
    type(f, 46, "idManagedClassPtr < idEntity >", "", 0x100, 0);
    type(f, 47, "idManagedClassPtr < idAI2 >", "", 0x100, 0);
    type(f, 48, "OtherManagedHandle", "", 0x101, 0);
    record = type(f, 49, "idList < ManagedRefs >", "", 0x50, 0);
    element = (unsigned char *)allocate(f, 0x48); pointer(record, 0x18, element);
    field(f, element, 0, "element", "idManagedClassPtr < idEntity >", "", 24);
    type(f, 50, "idStaticParticleModelData", "", 0, 0x110);
    type(f, 51, "OtherParticleData", "", 0, 0x111);
    type(f, 52, "idEventCallInfo", "", 0x120, 0);
    /* idMD6Anim shares the generic resource pointer reader registered by idImage. */
    type(f, 53, "idImage", "", 0, 0x130);
    type(f, 54, "idMD6Anim", "", 0, 0x130);
    type(f, 55, "idBoundedIntBase", "", 0x140, 0);
    type(f, 56, "idBoundedFloatBase", "", 0x141, 0);
    type(f, 57, "idBoundedFloat < lightProbeFalloff_t >", "", 0x141, 0);
    type(f, 58, "idVoiceTrack", "", 0, 0x150);
    type(f, 59, "idCollisionModel", "", 0, 0x151);
    type(f, 60, "SharedVoice", "", 0, 0x150);
    enums = (unsigned char *)allocate(f, 2 * 0x18);
    enum_readers = (unsigned char *)allocate(f, 2 * 16);
    pointer(enums, 0, text(f, "Mode")); pointer(container, 0x10, enums);
    pointer(f->reflect, 0x58, enum_readers); number(f->reflect, 0x60, 2);
    /* Enum pointer readers (+0x70), none registered. */
    pointer(f->reflect, 0x70, allocate(f, 2 * 16)); number(f->reflect, 0x78, 2);
}
static void cleanup(fixture *f)
{ while (f->region_count) free(f->regions[--f->region_count].data); }
static sh_decl_native_schema *open_schema(fixture *f)
{
    sh_decl_native_source source = {f, read_memory, (uintptr_t)f->reflect, f->scripts};
    sh_decl_dependency_observer observer = {f, reference, gap};
    return sh_decl_native_schema_open(source, observer, NULL, 0);
}
static int inspect(fixture *f, const char *value, sh_decl_dependency_result *result)
{
    sh_decl_native_schema *native = open_schema(f);
    sh_decl_source source = {value, strlen(value)};
    sh_decl_node *root = sh_decl_tree_parse(source, NULL, 0);
    sh_decl_value_type entity = {"Entity", ""};
    int complete;
    assert(native && root); f->references[0] = 0; f->gaps = 0;
    complete = sh_decl_entity_dependencies(root, entity, sh_decl_native_schema_view(native), result);
    sh_decl_tree_free(root); sh_decl_native_schema_close(native); return complete;
}

static void composition(fixture *f)
{
    sh_decl_native_source source = {f, read_memory, (uintptr_t)f->reflect};
    sh_decl_native_schema *native = sh_decl_native_schema_open(source,
        (sh_decl_dependency_observer){0}, NULL, 0);
    sh_decl_composition_schema schema = {0};
    const char *a = "{ edit = { array = { array[1] = \"a/ref\"; } } }";
    const char *b = "{ edit = { array = { array[5] = \"b/ref\"; } } }";
    sh_decl_source base = {"{}", 2}, inputs[] = {{a, strlen(a)}, {b, strlen(b)}};
    sh_decl_conflict conflict;
    char error[512], *output;
    size_t length;
    sh_decl_node *tree;
    sh_decl_dependency_result result;
    assert(native);
    schema.types = sh_decl_native_schema_view(native);
    schema.state_type = (sh_decl_value_type){"Entity", ""};
    output = sh_decl_compose_typed(base, inputs, 2, NULL, 0, &schema,
        &length, error, sizeof(error), &conflict);
    assert(output && strstr(output, "array[1] = \"a/ref\";") &&
        strstr(output, "array[5] = \"b/ref\";") && !strstr(output, "num ="));
    tree = sh_decl_tree_parse((sh_decl_source){output, length}, NULL, 0);
    assert(tree && !sh_decl_entity_dependencies(tree, schema.state_type, schema.types, &result));
    assert(result.aborted); /* Missing dependency observer cannot assert closure. */
    sh_decl_tree_free(tree); free(output);
    b = "{ edit = { array = { array[6] = \"outside/ref\"; } } }";
    inputs[1] = (sh_decl_source){b, strlen(b)};
    output = sh_decl_compose_typed(base, inputs, 2, NULL, 0, &schema,
        &length, error, sizeof(error), &conflict);
    assert(!output && strstr(error, "fixed native array") && conflict.first == 1);
    sh_decl_native_schema_close(native);
}

/* Schemas opened from one source share one registry and its loaded types:
 * the sharing open reads nothing, a type loaded through one is not read again
 * through another, each keeps its own observer and answers, and any of them
 * may close first. Another source, or no base, opens its own registry. */
static size_t g_shared_reads, g_other_reads;
static int shared_read(void *context, uintptr_t address, void *out, size_t length)
{ g_shared_reads++; return read_memory(context, address, out, length); }
static int other_read(void *context, uintptr_t address, void *out, size_t length)
{ g_other_reads++; return read_memory(context, address, out, length); }
typedef struct digest_bytes { unsigned char *data; size_t length; } digest_bytes;
static int digest_collect(void *context, const void *bytes, size_t length)
{
    digest_bytes *d = (digest_bytes *)context;
    unsigned char *grown = (unsigned char *)realloc(d->data, d->length + length + 1);
    if (!grown) return 0;
    memcpy(grown + d->length, bytes, length); d->data = grown; d->length += length; return 1;
}
static int shared_dependencies(sh_decl_native_schema *schema, const char *text, sh_decl_dependency_result *result)
{
    sh_decl_node *root = sh_decl_tree_parse((sh_decl_source){text, strlen(text)}, NULL, 0);
    int complete;
    assert(root);
    complete = sh_decl_entity_dependencies(root, (sh_decl_value_type){"Entity", ""},
        sh_decl_native_schema_view(schema), result);
    sh_decl_tree_free(root); return complete;
}
static void shared_core(fixture *f)
{
    static const char declaration[] = "{ edit = { target = \"base/ref\"; name = \"not/a/ref\"; mode = \"MODE_TEST\";"
        " bit = 1; inventory = { num = 1; item[0] = \"weapon/ref\"; } array = { array[5] = \"array/ref\"; }"
        " angle = 45; } }";
    sh_decl_native_source source = {f, shared_read, (uintptr_t)f->reflect, f->scripts};
    sh_decl_native_schema *base, *shared, *third, *own, *foreign;
    sh_decl_dependency_result result;
    sh_decl_value_type member;
    digest_bytes expected = {0}, actual = {0};
    fixture second, other;
    size_t reads;
    memset(&second, 0, sizeof(second));
    g_shared_reads = 0;
    base = sh_decl_native_schema_open(source, (sh_decl_dependency_observer){f, reference, gap}, NULL, 0);
    assert(base && g_shared_reads);
    reads = g_shared_reads;
    shared = sh_decl_native_schema_open_shared(base, source,
        (sh_decl_dependency_observer){&second, reference, gap}, NULL, 0);
    assert(shared && g_shared_reads == reads);
    assert(sh_decl_native_schema_view(shared)->field(shared, (sh_decl_value_type){"Entity", ""}, "name", &member) == 1 &&
        !strcmp(member.name, "idStr") && g_shared_reads > reads);
    reads = g_shared_reads;
    assert(sh_decl_native_schema_view(base)->field(base, (sh_decl_value_type){"Entity", ""}, "name", &member) == 1 &&
        !strcmp(member.name, "idStr") && g_shared_reads == reads);
    f->references[0] = 0;
    assert(shared_dependencies(shared, declaration, &result) && result.references == 3 && !result.gaps);
    assert(!f->references[0] && strstr(second.references, "edit.inventory.item[0]|idDeclInventory|weapon/ref"));
    sh_decl_native_schema_close(base);
    reads = g_shared_reads;
    third = sh_decl_native_schema_open_shared(shared, source, (sh_decl_dependency_observer){f, reference, gap}, NULL, 0);
    assert(third && g_shared_reads == reads);
    sh_decl_native_schema_close(shared);
    f->references[0] = 0;
    assert(shared_dependencies(third, declaration, &result) && result.references == 3 && !result.gaps);
    assert(strstr(f->references, "edit.target|idDeclEntityDef|base/ref") &&
        strstr(f->references, "edit.array.array[5]|idDeclInventory|array/ref"));
    own = sh_decl_native_schema_open(source, (sh_decl_dependency_observer){f, reference, gap}, NULL, 0);
    assert(own && sh_decl_native_schema_digest(own, digest_collect, &expected) &&
        sh_decl_native_schema_digest(third, digest_collect, &actual));
    assert(expected.length && expected.length == actual.length && !memcmp(expected.data, actual.data, expected.length));
    g_other_reads = 0;
    foreign = sh_decl_native_schema_open_shared(third, (sh_decl_native_source){f, other_read, (uintptr_t)f->reflect,
        f->scripts}, (sh_decl_dependency_observer){0}, NULL, 0);
    assert(foreign && g_other_reads);
    sh_decl_native_schema_close(foreign);
    reads = g_shared_reads;
    foreign = sh_decl_native_schema_open_shared(NULL, source, (sh_decl_dependency_observer){0}, NULL, 0);
    assert(foreign && g_shared_reads > reads);
    sh_decl_native_schema_close(foreign);
    setup(&other);
    reads = g_shared_reads;
    foreign = sh_decl_native_schema_open_shared(third, (sh_decl_native_source){&other, shared_read,
        (uintptr_t)other.reflect, other.scripts}, (sh_decl_dependency_observer){0}, NULL, 0);
    assert(foreign && g_shared_reads > reads && sh_decl_native_schema_class_derives(foreign, "Entity", "Base") == 1);
    sh_decl_native_schema_close(foreign); cleanup(&other);
    sh_decl_native_schema_close(own); sh_decl_native_schema_close(third);
    free(expected.data); free(actual.data);
}

static void source_binding(void)
{
    fixture f = {0};
    unsigned char *memory = (unsigned char *)allocate(&f, 0x400);
    unsigned char *accessor = memory, *manager = memory + 0x100, *container = memory + 0x300;
    sh_decl_native_source source = {&f, read_memory, 123};
    int32_t displacement;
    static const unsigned char tail[] = {0x48, 0x83, 0xc4, 0x30, 0x5b, 0xc3};
    memcpy(accessor + 0x47, "\x48\x8d\x1d", 3);
    displacement = (int32_t)(manager - (accessor + 0x4e));
    memcpy(accessor + 0x4a, &displacement, 4);
    memcpy(accessor + 0x74, "\x48\x8d\x05", 3);
    displacement = (int32_t)(manager - (accessor + 0x7b));
    memcpy(accessor + 0x77, &displacement, 4);
    memcpy(accessor + 0x7b, tail, sizeof(tail));
    assert(sh_decl_native_source_bind(&source, (uintptr_t)accessor, (uintptr_t)container) == 0);
    assert(!source.reflection);
    pointer(manager, 0x80, container);
    assert(sh_decl_native_source_bind(&source, (uintptr_t)accessor, (uintptr_t)container) == 1);
    assert(source.reflection == (uintptr_t)(manager + 0x80));
    {
        unsigned char *getter = memory + 0x200, *system = memory + 0x220;
        memcpy(getter, "\x48\x8d\x05", 3); getter[7] = 0xc3;
        displacement = (int32_t)(system - (getter + 7)); memcpy(getter + 3, &displacement, 4);
        /* Allocated reflection tables alone do not establish readiness. */
        assert(sh_decl_native_source_ready(&source, (uintptr_t)accessor, (uintptr_t)container, (uintptr_t)getter) == 0);
        assert(!source.reflection);
        pointer(system, 8, memory + 0x280); number(system, 0x10, 1); number(system, 0x14, 2);
        assert(sh_decl_native_source_ready(&source, (uintptr_t)accessor, (uintptr_t)container, (uintptr_t)getter) == 1);
        assert(source.reflection == (uintptr_t)(manager + 0x80));
        number(system, 0x10, 3);
        assert(sh_decl_native_source_ready(&source, (uintptr_t)accessor, (uintptr_t)container, (uintptr_t)getter) == -1);
        assert(!source.reflection);
        number(system, 0x10, -1);
        assert(sh_decl_native_source_ready(&source, (uintptr_t)accessor, (uintptr_t)container, (uintptr_t)getter) == -1);
        number(system, 0x10, 1); pointer(system, 8, NULL);
        assert(sh_decl_native_source_ready(&source, (uintptr_t)accessor, (uintptr_t)container, (uintptr_t)getter) == -1);
        pointer(system, 8, memory + 0x280); getter[7] = 0x90;
        assert(sh_decl_native_source_ready(&source, (uintptr_t)accessor, (uintptr_t)container, (uintptr_t)getter) == -1);
        getter[7] = 0xc3; f.unreadable = (uintptr_t)(system + 8);
        assert(sh_decl_native_source_ready(&source, (uintptr_t)accessor, (uintptr_t)container, (uintptr_t)getter) == -1);
        f.unreadable = 0;
    }
    assert(sh_decl_native_source_bind(&source, (uintptr_t)accessor, (uintptr_t)(container + 1)) == -1);
    assert(!source.reflection);
    accessor[0x77]++;
    assert(sh_decl_native_source_bind(&source, (uintptr_t)accessor, (uintptr_t)container) == -1);
    accessor[0x77]--;
    accessor[0x80] = 0x90;
    assert(sh_decl_native_source_bind(&source, (uintptr_t)accessor, (uintptr_t)container) == -1);
    accessor[0x80] = 0xc3;
    f.unreadable = (uintptr_t)(manager + 0x80);
    assert(sh_decl_native_source_bind(&source, (uintptr_t)accessor, (uintptr_t)container) == -1);
    f.unreadable = (uintptr_t)accessor;
    assert(sh_decl_native_source_bind(&source, (uintptr_t)accessor, (uintptr_t)container) == -1);
    assert(!source.reflection);
    cleanup(&f);
}

static void declaration_roots(fixture *f)
{
    sh_decl_native_schema *schema = open_schema(f);
    sh_decl_value_type state = {"stale", "*"};
    assert(schema);
    assert(sh_decl_native_schema_decl_type(schema, "snapeditorentitydef", &state) == 1);
    assert(!strcmp(state.name, "idDeclSnapEditorEntity") && !*state.ops);
    assert(sh_decl_native_schema_decl_type(schema, "material", &state) == 0 && !state.name);
    assert(sh_decl_native_schema_decl_type(schema, "entitydef", &state) == 0 && !state.name);
    assert(sh_decl_native_schema_decl_type(schema, "unregistered", &state) == 0 && !state.name);
    assert(sh_decl_native_schema_decl_type(schema, "aicomponent_cyberdemon", &state) == -1 && !state.name);
    assert(sh_decl_native_schema_decl_type(NULL, "snapeditorentitydef", &state) == -1 && !state.name);
    assert(sh_decl_native_schema_decl_type(schema, NULL, &state) == -1 && !state.name);
    assert(sh_decl_native_schema_decl_type(schema, "snapeditorentitydef", NULL) == -1);
    sh_decl_native_schema_close(schema);
    f->unreadable = (uintptr_t)f->records + 12 * 0x38 + 8;
    schema = open_schema(f); assert(schema);
    assert(sh_decl_native_schema_decl_type(schema, "snapeditorentitydef", &state) == -1 && !state.name);
    sh_decl_native_schema_close(schema); f->unreadable = 0;
}

static void polymorphic_types(fixture *f)
{
    sh_decl_native_schema *native = open_schema(f);
    const sh_decl_dependency_schema *schema = sh_decl_native_schema_view(native);
    sh_decl_value_shape shape = {0};
    sh_decl_value_type selected = {0}, wrapper = {"idTypeInfoObjectPtr < Base >", ""};
    sh_decl_dependency_result result;
    uintptr_t element;
    assert(native && schema->dynamic_type);
    assert(schema->describe(schema->context, wrapper, &shape));
    assert(shape.kind == SH_DECL_VALUE_POLYMORPHIC && !strcmp(shape.element.name, "Base") && !*shape.element.ops);
    assert(shape.null_keeps);
    assert(schema->dynamic_type(schema->context, shape.element, "Entity suffix", 6, &selected) == 1);
    assert(!strcmp(selected.name, "Entity") && !*selected.ops);
    assert(schema->dynamic_type(schema->context, shape.element, "Base", 4, &selected) == 1);
    assert(!schema->dynamic_type(schema->context, shape.element, "Mode", 4, &selected) && !selected.name);
    assert(!schema->dynamic_type(schema->context, shape.element, "idDeclInventory", 15, &selected) && !selected.name);
    assert(!schema->dynamic_type(schema->context, shape.element, "Missing", 7, &selected) && !selected.name);
    assert(!schema->dynamic_type(schema->context, shape.element, "Entity\0hidden", 13, &selected) && !selected.name);
    assert(!schema->dynamic_type(schema->context, (sh_decl_value_type){"Base", "*"}, "Entity", 6, &selected));
    sh_decl_native_schema_close(native);
    assert(inspect(f, "{ edit = { variant = { className = \"Entity\"; object = { target = \"poly/ref\"; } } } }", &result));
    assert(result.references == 1 && !result.gaps && strstr(f->references, "edit.variant.object.target|idDeclEntityDef|poly/ref"));
    assert(inspect(f, "{ edit = { variant = \"Entity\"; } }", &result) && !result.references && !result.gaps);
    assert(!inspect(f, "{ edit = { variant = { className = \"CustomEntity\"; object = { target = \"custom/ref\"; } } } }", &result));
    assert(result.gaps == 1 && !result.references && !result.aborted);
    assert(!inspect(f, "{ edit = { variant = \"idDeclInventory\"; target = \"known/ref\"; } }", &result));
    assert(result.gaps == 1 && result.references == 1 && !result.aborted);
    /* A family mismatch invalidates every instantiation, not only the changed row. */
    pointer(f->objects, 26 * 16 + 8, (const void *)0xa1);
    assert(!inspect(f, "{ edit = { variant = \"Entity\"; } }", &result) && result.gaps == 1);
    pointer(f->objects, 26 * 16 + 8, (const void *)0xa0);
    memcpy(&element, f->records + 25 * 0x38 + 0x18, sizeof(element));
    pointer((void *)element, 8, text(f, "*"));
    assert(!inspect(f, "{ edit = { variant = \"Entity\"; } }", &result) && result.gaps == 1);
    pointer((void *)element, 8, text(f, ""));
    f->unreadable = (uintptr_t)f->records + 25 * 0x38 + 0x18;
    assert(!inspect(f, "{ edit = { variant = \"Entity\"; } }", &result) && result.gaps == 1);
    f->unreadable = 0;
    assert(inspect(f, "{ edit = { variant = \"Entity\"; } }", &result) && !result.gaps);
}

static void graph_dependencies(fixture *f)
{
    const char *source = "{ inherit = \"graph/parent\"; edit = { object = { target = \"root/asset\"; } subGraphs = { "
        "subGraph = { object = { className = \"TestSubgraph\"; object = { name = \"main\"; target = \"sub/asset\"; } } "
        "nodes = { node = { object = { className = \"TestNode\"; object = { name = \"a\"; target = \"node/asset\"; } } } "
        "node = { object = { className = \"TestNode\"; object = { name = \"a\"; target = \"second/asset\"; } } } } "
        "links = { a = { link = { object = { className = \"TestLink\"; object = { name = \"same\"; target = \"link/asset\"; } } "
        "startNode = \"a\"; endNode = \"a\"; } link = { object = { className = \"TestLink\"; "
        "object = { name = \"same\"; target = \"other/asset\"; } } startNode = \"a\"; endNode = \"a\"; } } } } } } }";
    sh_decl_native_schema *schema = open_schema(f);
    sh_decl_dependency_result result;
    sh_decl_graph *graph = sh_decl_graph_open((sh_decl_source){source, strlen(source)}, NULL, 0);
    sh_decl_value_type type = {"idDeclAttackGraph", ""};
    assert(schema && graph);
    assert(sh_decl_native_schema_graph_type(schema, type) == 1);
    assert(sh_decl_native_schema_graph_type(schema, (sh_decl_value_type){"idDeclAttackGraph", "*"}) == 0);
    assert(sh_decl_native_schema_graph_type(schema, (sh_decl_value_type){"Entity", ""}) == 0);
    assert(sh_decl_native_schema_graph_type(schema, (sh_decl_value_type){"UnexpectedGraph", ""}) == -1);
    assert(sh_decl_native_schema_graph_type(schema, (sh_decl_value_type){"unknown", ""}) == -1);
    assert(sh_decl_native_schema_graph_type(NULL, type) == -1);
    f->references[0] = 0; f->gaps = 0;
    assert(sh_decl_graph_dependencies(graph, "attackgraph", schema, &result));
    assert(result.references == 7 && !result.gaps && !result.aborted);
    assert(strstr(f->references, "inherit|idDeclAttackGraph|graph/parent"));
    assert(strstr(f->references, "edit.object.target|idDeclInventory|root/asset"));
    assert(strstr(f->references, "edit.records[4].object.target|idDeclEntityDef|other/asset"));
    {
        char *a = _strdup(source), *b = _strdup(source), error[512];
        char *where = strstr(a, "node/asset"), *out;
        size_t length;
        sh_decl_source contributions[2];
        assert(where); memcpy(where, "node/other", 10);
        where = strstr(b, "other/asset"); assert(where); memcpy(where, "other/value", 11);
        contributions[0] = (sh_decl_source){a, strlen(a)};
        contributions[1] = (sh_decl_source){b, strlen(b)};
        out = sh_decl_graph_compose((sh_decl_source){source, strlen(source)}, contributions, 2,
            sh_decl_native_schema_view(schema), type, &length, error, sizeof(error), NULL);
        if (!out) fprintf(stderr, "native graph composition: %s\n", error);
        assert(out && strstr(out, "node/other") && strstr(out, "other/value"));
        assert(length == strlen(out));
        sh_decl_graph *composed = sh_decl_graph_open((sh_decl_source){out, length}, NULL, 0);
        assert(composed && sh_decl_graph_count(composed) == 5);
        assert(sh_decl_graph_dependencies(composed, "attackgraph", schema, &result));
        assert(result.references == 7 && !result.gaps);
        sh_decl_graph_close(composed); free(out); free(a); free(b);
    }
    assert(!sh_decl_graph_dependencies(graph, "snapeditorentitydef", schema, &result) && result.aborted);
    sh_decl_graph_close(graph);
    source = "{ edit = { object = { nestedGraph = { target = \"unproven/asset\"; } } subGraphs = { "
        "subGraph = { object = { className = \"TestSubgraph\"; object = { name = \"s\"; target = \"good/ref\"; } } "
        "nodes = { node = { object = { className = \"Entity\"; object = { name = \"n\"; target = \"wrong/class\"; } } } } } } } }";
    graph = sh_decl_graph_open((sh_decl_source){source, strlen(source)}, NULL, 0);
    f->references[0] = 0; f->gaps = 0; assert(graph);
    assert(!sh_decl_graph_dependencies(graph, "attackgraph", schema, &result));
    assert(result.gaps == 2 && f->gaps == 2 && result.references == 1 && !result.aborted);
    assert(strstr(f->references, "good/ref") && !strstr(f->references, "unproven") && !strstr(f->references, "wrong/class"));
    sh_decl_graph_close(graph);
    sh_decl_native_schema_close(schema);
}

static int script_lookup(void *context, const char *name, uintptr_t *table)
{
    fixture *f = context;
    f->script_lookups++;
    *table = 0;
    if (f->script_fail) return -1;
    if (!strcmp(name, "Entity")) *table = f->script_tables[0];
    else if (!strcmp(name, "Nested")) *table = f->script_tables[1];
    return *table ? 1 : 0;
}
static void script_field(fixture *f, uintptr_t table, size_t index,
    const char *name, const char *type_name)
{
    unsigned char *row = (unsigned char *)table + index * 40;
    pointer(row, 0, text(f, type_name)); pointer(row, 8, text(f, ""));
    pointer(row, 16, text(f, name));
}
static int inspect_script(fixture *f, const char *body, sh_decl_dependency_result *result)
{
    sh_decl_native_schema *native = open_schema(f);
    sh_decl_node *root = sh_decl_tree_parse_ordered((sh_decl_source){body, strlen(body)}, NULL, 0);
    int ok;
    assert(native && root);
    f->references[0] = 0; f->gaps = 0;
    ok = sh_decl_state_dependencies(root, (sh_decl_value_type){"idSSObject", "*"},
        sh_decl_native_schema_view(native), result);
    sh_decl_tree_free(root); sh_decl_native_schema_close(native); return ok;
}
static void script_metadata(void)
{
    fixture f;
    uintptr_t *names;
    sh_decl_script_source source;
    sh_decl_script_schema *scripts;
    sh_decl_dependency_result result;
    char error[256];
    size_t calls;
    setup(&f);
    names = allocate(&f, 3 * sizeof(*names));
    names[0] = (uintptr_t)text(&f, "Entity"); names[1] = (uintptr_t)text(&f, "Empty");
    /* The terminator needs only one readable pointer, like the native loop. */
    f.script_tables[0] = (uintptr_t)allocate(&f, 4 * 40 + 8);
    f.script_tables[1] = (uintptr_t)allocate(&f, 2 * 40 + 8);
    script_field(&f, f.script_tables[0], 0, "target", "idDeclEntityDef");
    script_field(&f, f.script_tables[0], 1, "label", "ssString");
    script_field(&f, f.script_tables[0], 2, "nested", "Nested");
    script_field(&f, f.script_tables[0], 3, "items", "ssList<idDeclInventory>");
    script_field(&f, f.script_tables[1], 0, "target", "idDeclEntityDef");
    script_field(&f, f.script_tables[1], 1, "recursive", "Nested");
    source = (sh_decl_script_source){&f, read_memory, (uintptr_t)names, script_lookup};
    scripts = sh_decl_script_schema_capture(source, error, sizeof(error));
    assert(scripts && !error[0]); f.scripts = scripts;
    calls = f.script_lookups; assert(calls == 7);
    assert(sh_decl_script_schema_find(scripts, "Entity")->constructible);
    assert(!sh_decl_script_schema_find(scripts, "Nested")->constructible);
    assert(!sh_decl_script_schema_find(scripts, "Empty")->has_fields);
    assert(!sh_decl_script_schema_find(scripts, "unknown"));
    /* Script dispatch uses generic idDecl resolution directly, even when the
     * same native class registers a different ordinary pointer reader. */
    pointer(f.pointers, 1 * 16 + 8, (const void *)0xf7);
    /* Change the backing module field name: the snapshot owns every string. */
    pointer((void *)f.script_tables[0], 16, text(&f, "changed"));
    assert(inspect_script(&f, "{ object = \"Entity\"; target = \"one\"; target = \"two\";"
        " label = \"unrelated/path\"; nested = { target = \"three\"; recursive = { target = \"four\"; } }"
        " items = { num = 1; item[0] = \"five\"; } obsolete = \"not/a/resource\"; }", &result));
    assert(result.references == 5 && !result.gaps && f.script_lookups == calls);
    assert(strstr(f.references, "edit.target|idDeclEntityDef|one") &&
        strstr(f.references, "edit.target|idDeclEntityDef|two") &&
        strstr(f.references, "edit.nested.recursive.target|idDeclEntityDef|four") &&
        strstr(f.references, "edit.items.item[0]|idDeclInventory|five"));
    {
        const char *json = "{\"object\":\"Entity\",\"target\":\"one\","
            "\"nested\":{\"target\":\"three\"},\"items\":[\"four\"],\"label\":\"unrelated/path\"}";
        sh_decl_native_schema *native = open_schema(&f);
        f.references[0] = 0;
        int inspected = sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"idSSObject", "*"},
            sh_decl_native_schema_view(native), &result);
        if (!inspected) fprintf(stderr, "script JSON: visited=%zu references=%zu gaps=%zu aborted=%d\n",
            result.visited, result.references, result.gaps, result.aborted);
        assert(inspected);
        assert(result.references == 3 && !result.gaps && strstr(f.references, "edit.items.[0]|idDeclInventory|four"));
        /* The engine writes the members in its own order (alphabetical in a
         * saved map), so the selector types the fields on either side of it. */
        json = "{\"target\":\"one\",\"object\":\"Entity\"}";
        f.references[0] = 0;
        assert(sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"idSSObject", "*"},
            sh_decl_native_schema_view(native), &result) && result.references == 1 && !result.gaps &&
            strstr(f.references, "edit.target|idDeclEntityDef|one"));
        json = "{\"target\":\"one\"}";
        assert(!sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"idSSObject", "*"},
            sh_decl_native_schema_view(native), &result) && result.gaps == 1);
        json = "{\"object\":null}";
        assert(sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"idSSObject", "*"},
            sh_decl_native_schema_view(native), &result) && !result.references);
        json = "{\"object\":\"NULL\"}";
        assert(sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"idSSObject", "*"},
            sh_decl_native_schema_view(native), &result) && !result.references);
        json = "{\"script\":\"NULL\"}";
        assert(!sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"Entity", ""},
            sh_decl_native_schema_view(native), &result) && result.gaps == 1);
        json = "{\"script\":null}";
        assert(sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"Entity", ""},
            sh_decl_native_schema_view(native), &result) && !result.references && !result.gaps);
        sh_decl_native_schema_close(native);
    }
    assert(inspect_script(&f, "{ object = \"Empty\"; ignored = \"anything\"; }", &result) && !result.references);
    assert(inspect_script(&f, "{}", &result) && !result.references);
    assert(inspect_script(&f, "{ object = NULL; }", &result) && !result.references);
    assert(inspect_script(&f, "{ object = \"NULL\"; }", &result) && !result.references);
    assert(!inspect(&f, "{ edit = { script = \"NULL\"; } }", &result) && result.gaps == 1);
    assert(inspect(&f, "{ edit = { script = NULL; } }", &result) && !result.references && !result.gaps);
    assert(!inspect_script(&f, "{ object = \"Nested\"; }", &result) && result.gaps == 1);
    assert(!inspect_script(&f, "{ target = \"one\"; object = \"Entity\"; }", &result) && result.gaps == 1);
    f.scripts = NULL;
    assert(!inspect_script(&f, "{ object = \"Entity\"; }", &result) && result.gaps == 1);
    sh_decl_script_schema_free(scripts);
    f.script_fail = 1;
    assert(!sh_decl_script_schema_capture(source, error, sizeof(error)) && error[0]);
    f.script_fail = 0; f.unreadable = f.script_tables[1];
    assert(!sh_decl_script_schema_capture(source, error, sizeof(error)) && error[0]);
    cleanup(&f);
}

/* Effect classes follow the registered reader, never the type name (M77
 * ledger): RandomAlias and SharedAtom share randomValue_t's and
 * idAtomicString's readers and SharedVoice idVoiceTrack's, while OtherTimer,
 * OpaqueAtom stay unmodelled; the bounded setters clamp into the field (M79).
 * A reader-less class pointer only stores NULL or is skipped (M78). */
static void expect_effects(sh_decl_native_schema *schema, const char *name, const char *ops,
    sh_decl_value_kind kind, int assigns, int effects)
{
    sh_decl_value_shape shape = {0};
    int described = sh_decl_native_schema_view(schema)->describe(schema, (sh_decl_value_type){name, ops}, &shape);
    if (!described || shape.kind != kind || shape.assigns != assigns || shape.effects != effects) {
        fprintf(stderr, "effects %s%s: described %d kind %d assigns %d effects %d\n", name, ops, described,
            (int)shape.kind, shape.assigns, shape.effects);
        assert(0);
    }
}
static void reader_effects(fixture *f)
{
    sh_decl_native_schema *schema = open_schema(f);
    const sh_decl_value_kind value = SH_DECL_VALUE_IGNORE;
    assert(schema);
    expect_effects(schema, "idStr", "", value, SH_DECL_ASSIGN_QUOTED, SH_DECL_EFFECTS_FIELD);
    expect_effects(schema, "idAnimWebPath", "", value, SH_DECL_ASSIGN_QUOTED, SH_DECL_EFFECTS_FIELD);
    expect_effects(schema, "idTypeInfo", "*", value, SH_DECL_ASSIGN_TOKEN, SH_DECL_EFFECTS_FIELD);
    expect_effects(schema, "idTypeInfoPtr < Base >", "", value, SH_DECL_ASSIGN_TOKEN, SH_DECL_EFFECTS_FIELD);
    expect_effects(schema, "idTypesafeNumber < float , DegreesUnique_t >", "", value, SH_DECL_ASSIGN_NUMBER,
        SH_DECL_EFFECTS_FIELD);
    expect_effects(schema, "idTypesafeTime < float , secondUnique_t , 1 >", "", value, SH_DECL_ASSIGN_NUMBER,
        SH_DECL_EFFECTS_FIELD);
    expect_effects(schema, "randomValue_t", "", value, SH_DECL_ASSIGN_NUMBER, SH_DECL_EFFECTS_FIELD);
    expect_effects(schema, "RandomAlias", "", value, SH_DECL_ASSIGN_NUMBER, SH_DECL_EFFECTS_FIELD);
    expect_effects(schema, "timeContainer_t", "", value, SH_DECL_ASSIGN_INTEGER, SH_DECL_EFFECTS_FIELD);
    expect_effects(schema, "idColor", "", value, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_FIELD);
    expect_effects(schema, "idAtomicString", "", value, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "SharedAtom", "", value, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "idAnimWebPath_Atomic", "", value, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "idHandle < unsigned short , invalidAliasHandle_t , INVALID_ALIAS_HANDLE >", "", value,
        SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "idAtomicHandleT < aliasRefType_t >", "", value, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "idManagedClassPtr < idAI2 >", "", value, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "idDeclEntityDef", "*", SH_DECL_VALUE_REFERENCE, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "idImage", "*", SH_DECL_VALUE_REFERENCE, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "idStaticParticleModelData", "*", SH_DECL_VALUE_REFERENCE, SH_DECL_ASSIGN_NONE,
        SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "idEventCallInfo", "", SH_DECL_VALUE_EVENT_CALL, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "int", "", value, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_UNMODELLED);
    expect_effects(schema, "OpaqueAtom", "", SH_DECL_VALUE_UNKNOWN, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_UNMODELLED);
    expect_effects(schema, "OtherTimer", "", SH_DECL_VALUE_UNKNOWN, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_UNMODELLED);
    expect_effects(schema, "Base", "*", value, SH_DECL_ASSIGN_NULL, SH_DECL_EFFECTS_NONE);
    expect_effects(schema, "idSSObject", "*", SH_DECL_VALUE_SCRIPT, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "idTypeInfoObjectPtr < Base >", "", SH_DECL_VALUE_POLYMORPHIC, SH_DECL_ASSIGN_NONE,
        SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "idBoundedIntBase", "", value, SH_DECL_ASSIGN_INTEGER, SH_DECL_EFFECTS_FIELD);
    expect_effects(schema, "idBoundedFloat < lightProbeFalloff_t >", "", value, SH_DECL_ASSIGN_NUMBER,
        SH_DECL_EFFECTS_FIELD);
    /* Exact class spelling: `const Base` is no class, and an enum without a
     * pointer reader is skipped like a reader-less class pointer (M79). */
    expect_effects(schema, "const Base", "*", value, SH_DECL_ASSIGN_NULL, SH_DECL_EFFECTS_NONE);
    expect_effects(schema, "Mode", "*", value, SH_DECL_ASSIGN_NULL, SH_DECL_EFFECTS_NONE);
    expect_effects(schema, "idVoiceTrack", "*", SH_DECL_VALUE_REFERENCE, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "SharedVoice", "*", SH_DECL_VALUE_REFERENCE, SH_DECL_ASSIGN_NONE, SH_DECL_EFFECTS_GLOBAL);
    expect_effects(schema, "idCollisionModel", "*", SH_DECL_VALUE_REFERENCE, SH_DECL_ASSIGN_NONE,
        SH_DECL_EFFECTS_GLOBAL);
    assert(sh_decl_native_schema_reader(schema, (sh_decl_value_type){"SharedVoice", "*"}) == SH_DECL_READER_VOICE_TRACK);
    assert(sh_decl_native_schema_reader(schema, (sh_decl_value_type){"idCollisionModel", "*"}) ==
        SH_DECL_READER_COLLISION_MODEL);
    assert(sh_decl_native_schema_reader(schema, (sh_decl_value_type){"Base", "*"}) == SH_DECL_READER_UNKNOWN);
    sh_decl_native_schema_close(schema);
}

/* ---- bitfield setter stores (M84): every instruction form of the census,
 * encoded here from its instructions, and code that is not a masked store. */
static int setter_store(const char *hex, sh_decl_bitfield_store *store)
{
    unsigned char code[32] = {0};
    size_t length = strlen(hex) / 2;
    assert(length <= sizeof(code));
    for (size_t i = 0; i < length; i++) {
        unsigned value; assert(sscanf(hex + 2 * i, "%2x", &value) == 1); code[i] = (unsigned char)value;
    }
    return sh_decl_bitfield_setter_store(code, sizeof(code), store);
}
static void expect_store(const char *hex, long long offset, int width, uint64_t bits)
{
    sh_decl_bitfield_store store;
    int ok = setter_store(hex, &store);
    if (!ok || store.offset != offset || store.width != width || store.bits != bits)
        fprintf(stderr, "setter %s: %d %lld %d %llx\n", hex, ok, store.offset, store.width, (unsigned long long)store.bits);
    assert(ok && store.offset == offset && store.width == width && store.bits == bits);
}
static void bitfield_setters(void)
{
    sh_decl_bitfield_store store;
    /* and byte [rcx+0x170], ~2; neg edx; sbb al, al; and al, 2; or byte [rcx+0x170], al; ret */
    expect_store("80a170010000fd" "f7da" "1ac0" "2402" "088170010000" "c3", 0x170, 1, 0x02);
    /* and byte [rcx+0x170], ~1; test edx, edx; setne al; or byte [rcx+0x170], al; ret */
    expect_store("80a170010000fe" "85d2" "0f95c0" "088170010000" "c3", 0x170, 1, 0x01);
    /* the same two at [rcx] */
    expect_store("8021fb" "f7da" "1ac0" "2404" "0801" "c3", 0, 1, 0x04);
    expect_store("8021fe" "85d2" "0f95c0" "0801" "c3", 0, 1, 0x01);
    /* and dword [rcx], ~4; and edx, 1; shl edx, 2; or dword [rcx], edx; ret */
    expect_store("8321fb" "83e201" "c1e202" "0911" "c3", 0, 4, 0x04);
    /* and dword [rcx], ~1; and edx, 1; or dword [rcx], edx; ret */
    expect_store("8321fe" "83e201" "0911" "c3", 0, 4, 0x01);
    /* and dword [rcx], ~2; and edx, 1; add edx, edx; or dword [rcx], edx; ret */
    expect_store("8321fd" "83e201" "03d2" "0911" "c3", 0, 4, 0x02);
    /* mov byte [rcx+0x90], dl; ret: the whole byte */
    expect_store("889190000000" "c3", 0x90, 1, 0xff);
    /* mov eax, 0xfff0; and dx, 0xf; and word [rcx+0x3a], ax; or word [rcx+0x3a], dx; ret */
    expect_store("b8f0ff0000" "6683e20f" "6621413a" "6609513a" "c3", 0x3a, 2, 0x000f);
    /* and word [rcx+0x3a], 0xf; shl dx, 4; or word [rcx+0x3a], dx; ret */
    expect_store("6683613a0f" "66c1e204" "6609513a" "c3", 0x3a, 2, 0xfff0);
    /* and byte [rcx+0x70], ~1; and dl, 1; or byte [rcx+0x70], dl; ret */
    expect_store("806170fe" "80e201" "085170" "c3", 0x70, 1, 0x01);
    /* and byte [rcx+0x70], ~2; and dl, 1; add dl, dl; or byte [rcx+0x70], dl; ret */
    expect_store("806170fd" "80e201" "02d2" "085170" "c3", 0x70, 1, 0x02);
    /* and dword [rcx], 0x3ff; shl edx, 10; or dword [rcx], edx; ret */
    expect_store("8121ff030000" "c1e20a" "0911" "c3", 0, 4, 0xfffffc00ull);
    /* Not a masked store: the OR reaches a kept bit (no and al), a different
     * byte, an unmasked shift, a second clear, a call, a garbage register,
     * no RET, a store after the OR, an RIP-relative operand. */
    assert(!setter_store("8021fd" "f7da" "1ac0" "0801" "c3", &store));
    assert(!setter_store("8021fe" "85d2" "0f95c0" "084101" "c3", &store));
    assert(!setter_store("8321fe" "c1e201" "0911" "c3", &store));
    assert(!setter_store("8021fe" "8021fd" "85d2" "0f95c0" "0801" "c3", &store));
    assert(!setter_store("8021fe" "e800000000" "0801" "c3", &store));
    assert(!setter_store("8021fe" "0801" "c3", &store));
    assert(!setter_store("8021fe" "85d2" "0f95c0" "0801", &store));
    assert(!setter_store("8021fe" "85d2" "0f95c0" "0801" "8811" "c3", &store));
    assert(!setter_store("8025000000fe" "85d2" "0f95c0" "0805000000" "c3", &store));
    assert(!setter_store("c3", &store));
}

/* ---- native string reads ----
 * The source copies exactly the bytes the schema requests and the process
 * adapter cannot bound an object, so strings are read up to their terminator
 * and never past it: exact-size allocations (one character, both sides of a
 * 64-byte request, a long name, an empty type suffix) and a name ending at a
 * page boundary before an inaccessible page. An unterminated or unreadable
 * name fails the open; no partial name is accepted. */
typedef struct string_fixture {
    region owned[32];
    size_t owned_count, past_end, faults;
    unsigned char *page; /* one readable page, then an inaccessible one */
    size_t page_size;
} string_fixture;
static void *string_allocate(string_fixture *f, size_t size)
{
    void *memory = calloc(1, size);
    assert(memory && f->owned_count < sizeof(f->owned) / sizeof(f->owned[0]));
    f->owned[f->owned_count++] = (region){memory, size}; return memory;
}
static char *string_text(string_fixture *f, const char *value)
{
    size_t size = strlen(value) + 1;
    char *copy = (char *)string_allocate(f, size); memcpy(copy, value, size); return copy;
}
/* A request that starts inside an owned allocation and runs past its end is
 * counted and refused, never copied. Others are copied under a guard, so the
 * inaccessible page faults as it would for the process adapter. */
static int string_read(void *context, uintptr_t address, void *out, size_t length)
{
    string_fixture *f = (string_fixture *)context;
    size_t i;
    for (i = 0; i < f->owned_count; i++) {
        uintptr_t start = (uintptr_t)f->owned[i].data;
        if (address >= start && address - start < f->owned[i].size) {
            if (length > f->owned[i].size - (size_t)(address - start)) { f->past_end++; return 0; }
            break;
        }
    }
    __try { memcpy(out, (const void *)address, length); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { f->faults++; return 0; }
}
static int string_constant(void *context, const char *name, uint32_t value)
{ size_t *visited = (size_t *)context; assert(!strcmp(name, "KIND_A") && value == 1); ++*visited; return 1; }
static int string_emit(void *context, const void *bytes, size_t length)
{ (void)bytes; *(size_t *)context += length; return 1; }
static sh_decl_native_schema *string_open(string_fixture *f, unsigned char *reflect)
{
    sh_decl_native_source source = {f, string_read, (uintptr_t)reflect, NULL};
    sh_decl_dependency_observer observer = {0};
    f->past_end = f->faults = 0;
    return sh_decl_native_schema_open(source, observer, NULL, 0);
}
static void string_reads(void)
{
    static const char tail[] = "PageEnd";
    char names[4][5001];
    const size_t lengths[4] = {62, 63, 64, 5000};
    unsigned char *reflect, *container, *records, *fields, *enums, *members, *at_end;
    SYSTEM_INFO system;
    DWORD previous;
    string_fixture f;
    sh_decl_native_schema *schema;
    size_t i, visited = 0, emitted = 0;
    memset(&f, 0, sizeof(f));
    GetSystemInfo(&system); f.page_size = system.dwPageSize;
    f.page = (unsigned char *)VirtualAlloc(NULL, 2 * f.page_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    assert(f.page && VirtualProtect(f.page + f.page_size, f.page_size, PAGE_NOACCESS, &previous));
    reflect = (unsigned char *)string_allocate(&f, 0xc0);
    container = (unsigned char *)string_allocate(&f, 0x30);
    records = (unsigned char *)string_allocate(&f, 7 * 0x38);
    pointer(reflect, 0, container); pointer(container, 0x20, records);
    pointer(reflect, 0x88, string_allocate(&f, 7 * 16)); number(reflect, 0x90, 7);
    pointer(reflect, 0xa0, string_allocate(&f, 7 * 16)); number(reflect, 0xa8, 7);
    /* "A" has no superclass (a null name pointer reads as empty). */
    pointer(records, 0, string_text(&f, "A"));
    for (i = 0; i < 4; i++) {
        memset(names[i], 'b' + (int)i, lengths[i]); names[i][lengths[i]] = 0;
        pointer(records + (i + 1) * 0x38, 0, string_text(&f, names[i]));
    }
    /* A field whose type suffix is the empty string (a one-byte allocation). */
    fields = (unsigned char *)string_allocate(&f, 2 * 0x48);
    pointer(fields, 0, string_text(&f, "int")); pointer(fields, 8, string_text(&f, ""));
    pointer(fields, 0x10, string_text(&f, "value")); number(fields, 0x1c, 4);
    pointer(records + 2 * 0x38, 0x20, fields);
    /* The last class name ends exactly at the inaccessible page. */
    at_end = f.page + f.page_size - sizeof(tail);
    memcpy(at_end, tail, sizeof(tail));
    pointer(records + 5 * 0x38, 0, at_end); pointer(records + 5 * 0x38, 8, string_text(&f, "A"));
    enums = (unsigned char *)string_allocate(&f, 2 * 0x18);
    members = (unsigned char *)string_allocate(&f, 2 * 0x10);
    pointer(enums, 0, string_text(&f, "Kind")); pointer(enums, 0x10, members);
    pointer(members, 0, string_text(&f, "KIND_A")); number(members, 8, 1);
    pointer(container, 0x10, enums);
    pointer(reflect, 0x58, string_allocate(&f, 2 * 16)); number(reflect, 0x60, 2);
    pointer(reflect, 0x70, string_allocate(&f, 2 * 16)); number(reflect, 0x78, 2);

    schema = string_open(&f, reflect);
    assert(schema);
    assert(sh_decl_native_schema_class_known(schema, "A") == 1 && sh_decl_native_schema_class_known(schema, tail) == 1);
    for (i = 0; i < 4; i++) assert(sh_decl_native_schema_class_known(schema, names[i]) == 1);
    assert(sh_decl_native_schema_class_derives(schema, tail, "A") == 1);
    assert(sh_decl_native_schema_enum_constants(schema, "Kind", string_constant, &visited) == 1 && visited == 1);
    assert(sh_decl_native_schema_digest(schema, string_emit, &emitted) && emitted > 5000);
    sh_decl_native_schema_close(schema);
    if (f.past_end || f.faults)
        fprintf(stderr, "string reads: %zu request(s) past an allocation's end, %zu fault(s)\n", f.past_end, f.faults);
    assert(!f.past_end && !f.faults);

    /* Bytes that end at the inaccessible page without a terminator, then a
     * name that starts there: each fails the open after one refused byte. */
    memcpy(f.page + f.page_size - 12, "Unterminated", 12);
    pointer(records, 0, f.page + f.page_size - 12);
    schema = string_open(&f, reflect);
    assert(!schema && f.faults == 1 && !f.past_end);
    pointer(records, 0, f.page + f.page_size);
    schema = string_open(&f, reflect);
    assert(!schema && f.faults == 1 && !f.past_end);

    while (f.owned_count) free(f.owned[--f.owned_count].data);
    VirtualFree(f.page, 0, MEM_RELEASE);
}

/* R-388: inside a save scope the reflection registry is indexed once per
 * source. Every open after the first reads nothing to open, and each keeps
 * its own observer and answers. Outside a scope, and for another source,
 * each open indexes again. The scope's base ends with it. */
static void scoped_opens(fixture *f)
{
    static const char declaration[] = "{ edit = { target = \"base/ref\"; name = \"not/a/ref\"; mode = \"MODE_TEST\";"
        " bit = 1; inventory = { num = 1; item[0] = \"weapon/ref\"; } array = { array[5] = \"array/ref\"; }"
        " angle = 45; } }";
    sh_decl_native_source source = {f, shared_read, (uintptr_t)f->reflect, f->scripts};
    sh_decl_dependency_result result;
    digest_bytes expected = {0}, actual = {0};
    size_t reads;
    sh_decl_native_schema *plain = sh_decl_native_schema_open(source, (sh_decl_dependency_observer){f, reference, gap}, NULL, 0);
    assert(plain && sh_decl_native_schema_digest(plain, digest_collect, &expected));
    sh_decl_native_schema_scope(1);
    g_shared_reads = 0;
    sh_decl_native_schema *first = sh_decl_native_schema_open(source, (sh_decl_dependency_observer){f, reference, gap}, NULL, 0);
    assert(first && g_shared_reads);
    reads = g_shared_reads;
    sh_decl_native_schema *again = sh_decl_native_schema_open(source, (sh_decl_dependency_observer){f, reference, gap}, NULL, 0);
    assert(again && g_shared_reads == reads);
    sh_decl_native_schema_close(first);
    f->references[0] = 0;
    assert(shared_dependencies(again, declaration, &result) && result.references == 3 && !result.gaps);
    assert(strstr(f->references, "edit.target|idDeclEntityDef|base/ref"));
    assert(sh_decl_native_schema_digest(again, digest_collect, &actual));
    assert(expected.length == actual.length && !memcmp(expected.data, actual.data, expected.length));
    g_other_reads = 0;
    sh_decl_native_schema *foreign = sh_decl_native_schema_open((sh_decl_native_source){f, other_read,
        (uintptr_t)f->reflect, f->scripts}, (sh_decl_dependency_observer){0}, NULL, 0);
    assert(foreign && g_other_reads);
    sh_decl_native_schema_close(foreign);
    sh_decl_native_schema_scope(0);
    reads = g_shared_reads;
    sh_decl_native_schema *after = sh_decl_native_schema_open(source, (sh_decl_dependency_observer){0}, NULL, 0);
    assert(after && g_shared_reads > reads);
    sh_decl_native_schema_close(after); sh_decl_native_schema_close(again); sh_decl_native_schema_close(plain);
    free(expected.data); free(actual.data);
}

int main(void)
{
    string_reads();
    script_metadata();
    bitfield_setters();
    fixture f;
    sh_decl_dependency_result result;
    sh_decl_native_schema *schema;
    setup(&f);
    declaration_roots(&f);
    polymorphic_types(&f);
    reader_effects(&f);
    graph_dependencies(&f);
    source_binding();
    composition(&f);
    shared_core(&f);
    scoped_opens(&f);
    schema = open_schema(&f); assert(schema);
    assert(sh_decl_native_schema_class_derives(schema, "Entity", "Base") == 1);
    assert(sh_decl_native_schema_class_derives(schema, "Entity", "Entity") == 1);
    assert(sh_decl_native_schema_class_derives(schema, "Base", "Entity") == 0);
    assert(sh_decl_native_schema_class_derives(schema, "Missing", "Missing") == -1);
    assert(sh_decl_native_schema_class_derives(schema, "entity", "Base") == -1);
    assert(sh_decl_native_schema_reader(schema, (sh_decl_value_type){"idDeclInventory", "*"}) == SH_DECL_READER_DECL);
    assert(sh_decl_native_schema_reader(schema, (sh_decl_value_type){"idDeclEntityDef", "*"}) == SH_DECL_READER_DECL);
    assert(sh_decl_native_schema_reader(schema, (sh_decl_value_type){"idDeclInventory", "*[2]"}) == SH_DECL_READER_UNKNOWN);
    assert(sh_decl_native_schema_reader(schema, (sh_decl_value_type){"idAtomicString", ""}) == SH_DECL_READER_UNKNOWN);
    assert(sh_decl_native_schema_reader(schema, (sh_decl_value_type){"idStaticParticleModelData", "*"}) == SH_DECL_READER_STATIC_PARTICLE);
    assert(sh_decl_native_schema_reader(schema, (sh_decl_value_type){"OtherParticleData", "*"}) == SH_DECL_READER_UNKNOWN);
    sh_decl_native_schema_close(schema);
    assert(inspect(&f, "{ edit = { staticData = \"models/particle.lwo\"; } }", &result));
    assert(result.references == 1 && !result.gaps && strstr(f.references, "edit.staticData|idStaticParticleModelData|models/particle.lwo"));
    assert(inspect(&f, "{ edit = { target = \"base/ref\"; name = \"not/a/ref\"; mode = \"MODE_TEST\"; bit = 1;"
                      " inventory = { num = 1; item[0] = \"weapon/ref\"; }"
                      " array = { array[5] = \"array/ref\"; } angle = 45; } }", &result));
    assert(result.references == 3 && !result.gaps && !result.aborted);
    assert(strstr(f.references, "edit.target|idDeclEntityDef|base/ref"));
    assert(strstr(f.references, "edit.inventory.item[0]|idDeclInventory|weapon/ref"));
    assert(strstr(f.references, "edit.array.array[5]|idDeclInventory|array/ref"));
    assert(!strstr(f.references, "not/a/ref"));
    assert(inspect(&f, "{ edit = { angle = { value = 30; } } }", &result) && !result.references);
    {
        sh_decl_native_schema *native = open_schema(&f);
        const char *json;
        /* Native entity state keys counted lists as declaration text does; the
         * writer spells an empty pointer NULL (a saved player inventory). */
        f.references[0] = 0;
        json = "{\"inventory\":{\"item[0]\":\"weapon/ref\",\"item[1]\":\"NULL\"},\"target\":\"NULL\"}";
        int keyed = sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"Entity", ""},
            sh_decl_native_schema_view(native), &result);
        if (!keyed || result.references != 1 || result.gaps) fprintf(stderr, "keyed list JSON: ok=%d refs=%zu gaps=%zu aborted=%d [%s]\n",
            keyed, result.references, result.gaps, result.aborted, f.references);
        assert(keyed && result.references == 1 && !result.gaps);
        assert(strstr(f.references, "edit.inventory.item[0]|idDeclInventory|weapon/ref") || strstr(f.references, "inventory.item[0]|idDeclInventory|weapon/ref"));
        json = "{\"inventory\":{\"num\":3,\"item[2]\":\"weapon/ref\"}}";
        assert(sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"Entity", ""},
            sh_decl_native_schema_view(native), &result) && result.references == 1 && !result.gaps);
        json = "{\"inventory\":{\"num\":1,\"item[2]\":\"weapon/ref\"}}";
        assert(!sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"Entity", ""},
            sh_decl_native_schema_view(native), &result) && result.gaps == 1 && !result.references);
        json = "{\"inventory\":{\"num\":\"2\",\"item[0]\":\"weapon/ref\"}}";
        assert(!sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"Entity", ""},
            sh_decl_native_schema_view(native), &result) && result.gaps == 1);
        json = "{\"inventory\":{\"slot[0]\":\"weapon/ref\"}}";
        assert(!sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"Entity", ""},
            sh_decl_native_schema_view(native), &result) && result.gaps == 1);
        /* idEventCallInfo's registered reader selects the event-call shape: the
         * authored no-argument timeline call is complete, while a decl argument
         * stays a gap until an attached registry proves its manager. */
        json = "{\"eventCall\":{\"\\neventHandle_t eventDef\":\"disable\",\"args\":{\"\\nnum\":0}}}";
        assert(sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"Entity", ""},
            sh_decl_native_schema_view(native), &result) && !result.gaps && !result.references);
        json = "{\"eventCall\":{\"eventDef\":\"startSoundShader\",\"args\":{\"num\":1,"
            "\"item[0]\":{\"decl\":{\"sound\":\"play_noise_hat\"}}}}}";
        assert(!sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"Entity", ""},
            sh_decl_native_schema_view(native), &result) && result.gaps == 1 && !result.references);
        /* An md6Anim argument binds through the attached registry only because
         * idMD6Anim is read by the generic resource pointer reader; the entity
         * and string arguments beside it stay plain values. */
        {
            sh_decl_value_shape anim = {0};
            assert(sh_decl_native_schema_view(native)->describe(native,
                (sh_decl_value_type){SH_DECL_EVENT_ANIM_TYPE, SH_DECL_EVENT_ANIM_OPS}, &anim) &&
                anim.kind == SH_DECL_VALUE_REFERENCE && !strcmp(anim.element.name, "idMD6Anim") &&
                !strcmp(anim.element.ops, SH_DECL_EVENT_ANIM_OPS));
            /* The element keeps the use (its reader defaults an absent clip);
             * it reads with the same pointer reader as a reflected field. */
            assert(sh_decl_native_schema_reader(native, anim.element) == SH_DECL_READER_IMAGE);
            assert(sh_decl_native_schema_reader(native, (sh_decl_value_type){"idMD6Anim", "*"}) == SH_DECL_READER_IMAGE);
            assert(sh_decl_native_schema_reader(native,
                (sh_decl_value_type){"idImage", SH_DECL_EVENT_ANIM_OPS}) == SH_DECL_READER_UNKNOWN);
            memset(&anim, 0, sizeof(anim));
            assert(!sh_decl_native_schema_view(native)->describe(native,
                (sh_decl_value_type){"idImage", SH_DECL_EVENT_ANIM_OPS}, &anim));
        }
        json = "{\"eventCall\":{\"eventDef\":\"playAnim\",\"args\":{\"num\":3,"
            "\"item[0]\":{\"entity\":\"0_room/snapmaps/func/snap_lift_63\"},"
            "\"item[1]\":{\"md6Anim\":\"models/snap/lift.md6\"},\"item[2]\":{\"string\":\"models/plain.md6\"}}}}";
        assert(sh_decl_json_state_dependencies(json, strlen(json), (sh_decl_value_type){"Entity", ""},
            sh_decl_native_schema_view(native), &result) && !result.gaps && result.references == 1);
        sh_decl_native_schema_close(native);
    }
    assert(inspect(&f, "{ edit = { duration = 1.25; } }", &result) && !result.references);
    assert(inspect(&f, "{ edit = { duration = { value = 2; } } }", &result) && !result.references);
    assert(inspect(&f, "{ edit = { deadline = 1000; } }", &result) && !result.references);
    assert(inspect(&f, "{ edit = { deadline = { milliseconds = 1000; } } }", &result) && !result.references);
    assert(inspect(&f, "{ edit = { deadline = { obsolete = \"not/a/resource\"; } } }", &result) && !result.references);
    assert(inspect(&f, "{ edit = { color = { r = 0.5; g = 1; b = 0; a = 1; } } }", &result) && !result.references);
    assert(inspect(&f, "{ edit = { animState = \"movement/run:start\"; atomicState = \"combat.attack\";"
                      " nativeClass = \"RuntimeClass\"; classReference = \"RuntimeClass\";"
                      " target = \"real/resource\"; } }", &result));
    assert(result.references == 1 && !result.gaps && strstr(f.references, "real/resource") &&
        !strstr(f.references, "RuntimeClass") && !strstr(f.references, "combat.attack"));
    assert(inspect(&f, "{ edit = { animationAlias = \"locomotion/run\"; atomicAlias = \"combat/fire\";"
                      " target = \"real/resource\"; } }", &result));
    assert(result.references == 1 && !result.gaps && strstr(f.references, "real/resource") &&
        !strstr(f.references, "locomotion/run") && !strstr(f.references, "combat/fire"));
    /* A different registered custom reader is still unknown, even when the
     * type name sounds like a color. The exact anchor follows native metadata. */
    schema = open_schema(&f); assert(schema);
    sh_decl_value_shape shape = {0};
    assert(sh_decl_native_schema_view(schema)->describe(schema,
        (sh_decl_value_type){"OtherColor", ""}, &shape) && shape.kind == SH_DECL_VALUE_UNKNOWN);
    shape = (sh_decl_value_shape){0};
    assert(sh_decl_native_schema_view(schema)->describe(schema,
        (sh_decl_value_type){"UnknownPath", ""}, &shape) && shape.kind == SH_DECL_VALUE_UNKNOWN);
    shape = (sh_decl_value_shape){0};
    assert(sh_decl_native_schema_view(schema)->describe(schema,
        (sh_decl_value_type){"UnknownClassPointer", "*"}, &shape) && shape.kind == SH_DECL_VALUE_UNKNOWN);
    shape = (sh_decl_value_shape){0};
    assert(sh_decl_native_schema_view(schema)->describe(schema,
        (sh_decl_value_type){"idAtomicHandleT < otherRefType_t >", ""}, &shape) && shape.kind == SH_DECL_VALUE_UNKNOWN);
    sh_decl_native_schema_close(schema);
    assert(inspect(&f, "{ edit = { atomic = \"not/a/typed/resource\"; alias = \"also/text\"; } }", &result));
    assert(!result.references && !result.gaps);
    assert(inspect(&f, "{ edit = { volume = { min = 0.2; max = 0.8; granularity = 0.1;"
        " unused = \"not/a/resource\"; } randomAlias = 0.5; target = \"real/resource\"; } }", &result));
    assert(result.references == 1 && !result.gaps && !strstr(f.references, "not/a/resource"));
    pointer(f.objects, 45 * 16 + 8, (const void *)0xf8);
    assert(!inspect(&f, "{ edit = { randomAlias = 0.5; } }", &result) && result.gaps == 1);
    pointer(f.objects, 45 * 16 + 8, (const void *)0xf6);
    assert(inspect(&f, "{ edit = { bindParent = \"looks/like/a/resource\"; targetActor = \"monster_001\"; "
        "activateOnArrival = { num = 2; item[0] = \"door_001\"; item[1] = \"\"; } "
        "target = \"real/resource\"; } }", &result));
    assert(result.references == 1 && !result.gaps && !strstr(f.references, "looks/like") &&
        !strstr(f.references, "monster_001") && !strstr(f.references, "door_001"));
    schema = open_schema(&f); assert(schema); shape = (sh_decl_value_shape){0};
    assert(sh_decl_native_schema_view(schema)->describe(schema,
        (sh_decl_value_type){"OtherManagedHandle", ""}, &shape) && shape.kind == SH_DECL_VALUE_UNKNOWN);
    sh_decl_native_schema_close(schema);
    pointer(f.objects, 47 * 16 + 8, (const void *)0x101);
    assert(!inspect(&f, "{ edit = { bindParent = \"instance\"; targetActor = \"instance\"; } }", &result) &&
        result.gaps == 2 && !result.references);
    pointer(f.objects, 47 * 16 + 8, (const void *)0x100);
    pointer(f.objects, 24 * 16 + 8, (const void *)0x31);
    assert(!inspect(&f, "{ edit = { alias = \"unknown/reader\"; } }", &result) && result.gaps == 1);
    pointer(f.objects, 24 * 16 + 8, (const void *)0x30);
    assert(!inspect(&f, "{ edit = { target = \"known/ref\"; custom = \"maybe/ref\";"
                       " mystery = { num = 1; item[0] = \"unknown/ref\"; }"
                       " array = { array[6] = \"outside/count\"; } } }", &result));
    assert(result.references == 1 && result.gaps == 3 && f.gaps == 3 && !result.aborted);
    pointer(f.objects, 9 * 16 + 8, (const void *)0x70);
    assert(!inspect(&f, "{ edit = { inventory = { num = 1; item[0] = \"unverified/ref\"; } } }", &result));
    assert(result.references == 0 && result.gaps == 1);
    pointer(f.objects, 9 * 16 + 8, (const void *)0x50);
    /* A missing field-table read cannot silently become an obsolete field. */
    f.unreadable = (uintptr_t)f.records + 6 * 0x38 + 0x20;
    schema = open_schema(&f); assert(schema);
    assert(sh_decl_native_schema_class_derives(schema, "Entity", "Base") == 1);
    sh_decl_native_schema_close(schema);
    assert(!inspect(&f, "{ edit = { target = \"unknown/ref\"; } }", &result));
    assert(result.gaps == 1 && !result.references);
    f.unreadable = 0;
    pointer(f.records + 5 * 0x38, 8, text(&f, "Entity"));
    schema = open_schema(&f); assert(schema);
    assert(sh_decl_native_schema_class_derives(schema, "Entity", "Base") == -1);
    sh_decl_native_schema_close(schema);
    assert(!inspect(&f, "{ edit = { nonexistent = 0; } }", &result) && result.gaps == 1);
    number(f.reflect, 0xa8, 0);
    schema = open_schema(&f); assert(!schema);
    number(f.reflect, 0xa8, 32);
    f.unreadable = (uintptr_t)f.reflect;
    schema = open_schema(&f); assert(!schema);
    cleanup(&f);
    puts("decl_native_schema_test: PASS"); return 0;
}
