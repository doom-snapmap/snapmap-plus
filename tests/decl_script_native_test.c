#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "decl_script_native.h"

static void *active;
static int fault, leak, exchanges;
static const char *names[] = {"Fixture", NULL};
static uintptr_t fields[] = {(uintptr_t)"int", (uintptr_t)"", (uintptr_t)"value", 0, 0, 0};
static void *__fastcall exchange(void *object, void *context)
{
    void *prior = active;
    assert(object == names); exchanges++; active = context; return prior;
}
static uintptr_t __fastcall get_names(void *object) { assert(object == names); return (uintptr_t)names; }
static uintptr_t __fastcall get_fields(void *object, const char *name)
{
    void **methods = *(void ***)active;
    void *(__fastcall *allocate)(void *, size_t, int) = (void *)methods[0xd8 / 8];
    void (__fastcall *release)(void *, void *, int) = (void *)methods[0xe0 / 8];
    int (__fastcall *tag)(void *) = (void *)methods[0xe8 / 8];
    char *temporary;
    assert(object == names);
    temporary = allocate(active, strlen(name) + 1, tag(active)); assert(temporary);
    strcpy_s(temporary, strlen(name) + 1, name);
    if (fault) RaiseException(0xe0123456, 0, 0, NULL);
    if (!leak) release(active, temporary, tag(active));
    return !strcmp(name, "Fixture") ? (uintptr_t)fields : 0;
}
int main(int argc, char **argv)
{
    sh_decl_script_api api = {names, exchange, get_names, get_fields};
    sh_decl_script_schema *schema;
    char error[256]; int marker;
    active = &marker;
    schema = sh_decl_script_native_test_capture(api, error, sizeof(error));
    assert(schema && !error[0] && active == &marker && exchanges == 2);
    assert(sh_decl_script_schema_find(schema, "Fixture")->has_fields);
    sh_decl_script_schema_free(schema);
    fault = 1;
    schema = sh_decl_script_native_test_capture(api, error, sizeof(error));
    assert(!schema && error[0] && active == &marker && exchanges == 4);
    fault = 0; leak = 1;
    schema = sh_decl_script_native_test_capture(api, error, sizeof(error));
    assert(!schema && error[0] && active == &marker && exchanges == 6);
    leak = 0;
    schema = sh_decl_script_native_test_capture(api, error, sizeof(error));
    assert(schema && !error[0] && active == &marker && exchanges == 8);
    sh_decl_script_schema_free(schema);
    if (argc == 4 && !strcmp(argv[3], "bootstrap")) {
        assert(!GetModuleHandleW(L"superscriptx64.dll"));
        schema = sh_decl_script_native_capture_default(error, sizeof(error));
        if (!schema) fprintf(stderr, "bootstrap metadata: %s\n", error);
        assert(schema && !GetModuleHandleW(L"superscriptx64.dll"));
        assert(sh_decl_script_schema_find(schema, "horde_enemy_tracking")->constructible);
        sh_decl_script_schema_free(schema);
        puts("native script bootstrap: module reference released, copied metadata remains valid");
    }
    if (argc >= 2) {
        HMODULE module = LoadLibraryExA(argv[1], NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
        assert(module);
        schema = sh_decl_script_native_capture(module, error, sizeof(error));
        if (!schema) fprintf(stderr, "native metadata: %s\n", error);
        assert(schema);
        const sh_decl_script_type *type = sh_decl_script_schema_find(schema, "horde_enemy_tracking");
        assert(type && type->constructible);
        printf("native horde_enemy_tracking: fields=%d\n", type->has_fields);
        sh_decl_script_schema *again = sh_decl_script_native_capture_default(error, sizeof(error));
        assert(again && GetModuleHandleW(L"superscriptx64.dll") == module);
        sh_decl_script_schema_free(again);
        if (argc >= 3) {
            FILE *report = NULL;
            size_t types = 0, roots = 0, members = 0;
            assert(!fopen_s(&report, argv[2], "wb") && report);
            for (type = sh_decl_script_schema_first(schema); type; type = type->next) {
                const sh_decl_script_field *field;
                types++; roots += type->constructible;
                fprintf(report, "type\t%s\t%d\t%d\n", type->name, type->constructible, type->has_fields);
                for (field = type->fields; field; field = field->next) {
                    members++; fprintf(report, "field\t%s\t%s\t%s\t%s\n", type->name, field->name, field->type, field->ops);
                }
            }
            fclose(report);
            printf("native script snapshot: %zu types, %zu constructors, %zu fields\n", types, roots, members);
        }
        sh_decl_script_schema_free(schema); FreeLibrary(module);
    }
    puts("decl_script_native_test: PASS"); return 0;
}
