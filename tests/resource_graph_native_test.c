#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "../src/backend/resource_graph_native.c"

static int failures, calls, throw_error;
void sh_resource_resident_touch(void *resource) { (void)resource; }
static void *expected_entity, *expected_reader;
static unsigned char expected_initialize;
static unsigned char entity[0x6d8], definition[0x168], manager[16], reader_data[0x1208];
static const char canonical_text[] = "edit = { model = body; }";
static unsigned int parse_result;
static void *expected_resource;
static unsigned int read_result;
static int read_delegates, read_noop, read_calls;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); failures++; } } while (0)

/* This suite executes the production callback against native-shaped memory.
 * Actual detour instructions are checked separately against both game images. */
void *hook_prepare(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; return NULL; }
sh_patch_status hook_commit(void *trampoline)
{ (void)trampoline; return B2_PATCH_OK; }
void backend_log(const char *message) { (void)message; }

static void put_pointer(unsigned char *buffer, size_t offset, const void *value)
{ memcpy(buffer + offset, &value, sizeof(value)); }
static void fake_state(void *tools, void *object, void *reader, unsigned char initialize)
{
    CHECK(tools == (void *)17 && object == expected_entity && reader == expected_reader);
    CHECK(initialize == expected_initialize);
    calls++;
    sh_resource_graph_file("deferred");
    if (throw_error) RaiseException(0xe0123456, 0, 0, NULL);
}
static unsigned int fake_parse(void *resource, unsigned char read_source, void *error_text)
{
    CHECK(resource == expected_resource && read_source == 7 && error_text == (void *)19);
    sh_resource_graph_file("deferred");
    if (throw_error) RaiseException(0xe0123456, 0, 0, NULL);
    return parse_result;
}
static unsigned int fake_read(void *resource, void *error_text)
{
    CHECK(resource == expected_resource && error_text == (void *)19);
    read_calls++;
    if (read_noop) return 0;
    sh_resource_graph_file("source");
    if (throw_error) RaiseException(0xe0123456, 0, 0, NULL);
    if (read_delegates) return rg_parse_hook(resource, 7, error_text);
    sh_resource_graph_file("deferred");
    return read_result;
}
static int visit(void *context, const char *pt, const char *pn, const char *type, const char *name)
{
    unsigned *bits = (unsigned *)context;
    (void)pt; (void)pn; (void)type;
    if (!strcmp(name, "source")) *bits |= 1;
    else if (!strcmp(name, "deferred")) *bits |= 2;
    return 1;
}
static void seed(const char *type, const char *name)
{
    sh_resource_graph_frame frame;
    sh_resource_graph_begin(&frame, type, name);
    sh_resource_graph_file("source");
    sh_resource_graph_end(&frame, 1);
}
static int walk(const char *type, const char *name, unsigned *bits)
{ *bits = 0; return sh_resource_graph_walk(type, name, visit, bits); }
static void invoke(void *object, void *reader, unsigned char initialize)
{
    expected_entity = object; expected_reader = reader; expected_initialize = initialize;
    rg_state_hook((void *)17, object, reader, initialize);
}
static void test_production_read(void)
{
    sh_resource_graph_frame outer;
    unsigned bits;
    int caught = 0;
    g_read = fake_read; g_parse = fake_parse;
    expected_resource = definition;
    put_pointer(manager, 8, "material");
    put_pointer(definition, 0x10, manager);
    definition[0x48] = 0;

    /* Production reads call the typed parser without the direct-source hook. */
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    CHECK(read_calls == 1 && walk("material", "example", &bits) && bits == 3);

    /* Development reads delegate after opening their file. Keep both edges,
     * including when GenericLoad already owns the outer source lifetime. */
    read_delegates = 1;
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    CHECK(walk("material", "example", &bits) && bits == 3);
    sh_resource_graph_begin(&outer, "material", "example");
    sh_resource_graph_file("source");
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    CHECK(sh_resource_graph_source_active("material", "example"));
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "example", &bits) && bits == 3);

    parse_result = 9;
    CHECK(rg_read_hook(definition, (void *)19) == 9);
    CHECK(!walk("material", "example", &bits));
    parse_result = 0; read_delegates = 0; read_result = 7;
    CHECK(rg_read_hook(definition, (void *)19) == 7);
    CHECK(!walk("material", "example", &bits));
    read_result = 0; throw_error = 1;
    __try { rg_read_hook(definition, (void *)19); }
    __except (GetExceptionCode() == 0xe0123456 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 1; }
    CHECK(caught && !sh_resource_graph_recording());
    CHECK(!walk("material", "example", &bits));
    throw_error = 0;

    /* A successful implicit no-op must neither invent nor clear dependencies. */
    read_noop = 1; definition[0x48] = 1;
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    CHECK(!walk("material", "example", &bits));
    seed("material", "example");
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    CHECK(walk("material", "example", &bits) && bits == 1);
    sh_resource_graph_begin(&outer, "material", "read-parent");
    sh_resource_graph_file("source");
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    CHECK(sh_resource_graph_source_active("material", "read-parent"));
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "read-parent", &bits) && bits == 1);

    /* Native temporary/invalid identities remain transparent call-throughs. */
    read_noop = 0; definition[0x48] = 0;
    put_pointer(definition, 0x10, NULL);
    sh_resource_graph_begin(&outer, "material", "read-parent");
    sh_resource_graph_file("source");
    CHECK(rg_read_hook(definition, (void *)19) == 0);
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "read-parent", &bits) && bits == 1);
    CHECK(walk("material", "example", &bits) && bits == 1);
    expected_resource = (void *)1; read_noop = 1;
    CHECK(rg_read_hook((void *)1, (void *)19) == 0);
    CHECK(!sh_resource_graph_recording());
    put_pointer(definition, 0x10, manager);
    sh_resource_graph_test_reset();
}
int main(void)
{
    sh_resource_graph_frame outer, pause;
    unsigned bits;
    int caught = 0;
    put_pointer(entity, 0x6d0, definition);
    put_pointer(definition, 8, "example");
    put_pointer(definition, 0x10, manager);
    put_pointer(manager, 8, "entityDef");
    put_pointer(definition, 0x140, canonical_text);
    put_pointer(reader_data, 0x1160, canonical_text);
    g_state = fake_state; g_ready = 1;
    seed("entitydef", "example");
    CHECK(!walk("entitydef", "example", &bits));
    invoke(entity, reader_data, 1);
    CHECK(calls == 1 && walk("entitydef", "example", &bits) && bits == 3);

    /* Same entity, different map text must not replace the canonical graph. */
    put_pointer(reader_data, 0x1160, "a per-map edit");
    sh_resource_graph_begin(&outer, "material", "surrounding-parser");
    sh_resource_graph_file("source");
    invoke(entity, reader_data, 1);
    CHECK(sh_resource_graph_recording());
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "surrounding-parser", &bits) && bits == 1);
    CHECK(walk("entitydef", "example", &bits) && bits == 3);

    seed("entitydef", "example");
    put_pointer(reader_data, 0x1160, canonical_text);
    invoke(entity, reader_data, 0); /* Partial-update mode is not initialization. */
    CHECK(!walk("entitydef", "example", &bits));
    invoke((void *)1, reader_data, 1); /* Failed identity read is a call-through. */
    invoke(entity, (void *)1, 1);
    CHECK(calls == 5 && !sh_resource_graph_recording());
    CHECK(!walk("entitydef", "example", &bits));

    reader_data[0x1200] = 1;
    invoke(entity, reader_data, 1);
    CHECK(!walk("entitydef", "example", &bits));
    reader_data[0x1200] = 0;
    throw_error = 1;
    __try { invoke(entity, reader_data, 1); }
    __except (GetExceptionCode() == 0xe0123456 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 1; }
    CHECK(caught && !sh_resource_graph_recording());
    CHECK(!walk("entitydef", "example", &bits));
    throw_error = 0;
    invoke(entity, reader_data, 1);
    CHECK(calls == 8 && walk("entitydef", "example", &bits) && bits == 3);
    seed("entitydef", "parent");
    sh_resource_graph_begin(&outer, "entitydef", "example");
    sh_resource_graph_reference("entitydef", "parent");
    sh_resource_graph_end(&outer, 1);
    definition[0x128] = 1;
    invoke(entity, reader_data, 1);
    CHECK(walk("entitydef", "example", &bits) && bits == 3);
    definition[0x128] = 0;
    invoke(entity, reader_data, 1);
    CHECK(!walk("entitydef", "example", &bits));
    sh_resource_graph_test_reset();

    g_parse = fake_parse; expected_resource = definition;
    put_pointer(manager, 8, "material");
    CHECK(rg_parse_hook(definition, 7, (void *)19) == 0);
    CHECK(walk("material", "example", &bits) && bits == 2);

    /* A generic load delegating to the direct parser has one source scope.
     * In particular, the file opened before delegation must not be lost. */
    sh_resource_graph_begin(&outer, "MATERIAL", "EXAMPLE");
    sh_resource_graph_file("source");
    CHECK(sh_resource_graph_source_active("material", "example"));
    CHECK(!sh_resource_graph_source_active("material", "different"));
    CHECK(rg_parse_hook(definition, 7, (void *)19) == 0);
    CHECK(sh_resource_graph_source_active("material", "example"));
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "example", &bits) && bits == 3);

    /* A direct failure cannot retain the previous successful source record. */
    parse_result = 5;
    CHECK(rg_parse_hook(definition, 7, (void *)19) == 5);
    CHECK(!walk("material", "example", &bits));
    parse_result = 0; throw_error = 1; caught = 0;
    __try { rg_parse_hook(definition, 7, (void *)19); }
    __except (GetExceptionCode() == 0xe0123456 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { caught = 1; }
    CHECK(caught && !sh_resource_graph_recording());
    CHECK(!walk("material", "example", &bits));
    throw_error = 0;

    /* Temporary decls lacking a manager must not add their files to a parent. */
    put_pointer(definition, 0x10, NULL);
    sh_resource_graph_begin(&outer, "material", "temporary-parent");
    sh_resource_graph_file("source");
    rg_parse_hook(definition, 7, (void *)19);
    CHECK(sh_resource_graph_source_active("material", "temporary-parent"));
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "temporary-parent", &bits) && bits == 1);
    put_pointer(definition, 0x10, manager);

    /* An instance barrier hides outer source scopes, but permits an
     * independently loaded canonical dependency to acquire its own scope. */
    sh_resource_graph_begin(&outer, "material", "barrier-parent");
    sh_resource_graph_file("source");
    sh_resource_graph_pause(&pause);
    CHECK(!sh_resource_graph_source_active("material", "barrier-parent"));
    rg_parse_hook(definition, 7, (void *)19);
    CHECK(!sh_resource_graph_recording());
    sh_resource_graph_end(&pause, 0);
    sh_resource_graph_end(&outer, 1);
    CHECK(walk("material", "barrier-parent", &bits) && bits == 1);
    CHECK(walk("material", "example", &bits) && bits == 2);

    put_pointer(manager, 8, "entityDef");
    seed("entitydef", "example");
    invoke(entity, reader_data, 1);
    CHECK(walk("entitydef", "example", &bits));
    rg_parse_hook(definition, 7, (void *)19);
    CHECK(!walk("entitydef", "example", &bits)); /* State must be consumed again. */
    invoke(entity, reader_data, 1);
    CHECK(walk("entitydef", "example", &bits) && bits == 2);
    sh_resource_graph_test_reset();
    test_production_read();
    if (failures) return 1;
    puts("canonical entity-state, direct source and production read capture checks passed"); return 0;
}
