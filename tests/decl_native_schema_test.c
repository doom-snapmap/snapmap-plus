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
    size_t count = 52;
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
    fields = (unsigned char *)allocate(f, 28 * 0x48); pointer(record, 0x20, fields);
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
    enums = (unsigned char *)allocate(f, 2 * 0x18);
    enum_readers = (unsigned char *)allocate(f, 2 * 16);
    pointer(enums, 0, text(f, "Mode")); pointer(container, 0x10, enums);
    pointer(f->reflect, 0x58, enum_readers); number(f->reflect, 0x60, 2);
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
        json = "{\"target\":\"one\",\"object\":\"Entity\"}";
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

int main(void)
{
    script_metadata();
    fixture f;
    sh_decl_dependency_result result;
    sh_decl_native_schema *schema;
    setup(&f);
    declaration_roots(&f);
    polymorphic_types(&f);
    graph_dependencies(&f);
    source_binding();
    composition(&f);
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
