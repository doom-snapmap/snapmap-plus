/* Complete parent views and built-in inputs through the product compiler. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_compiler.h"
#include "package_fixture.h"

static const char *child_path = "generated/decls/entitydef/a_child.decl";
static const char *parent_path = "generated/decls/entitydef/z_parent.decl";
static const char *child_base = "{ inherit = \"z_parent\"; edit = { health = 100; shared = 0; "
    "slots = { slots[0] = { x = 1; } } } }";
static const char *child_a = "{ inherit = \"z_parent\"; edit = { health = 100; shared = 0; "
    "slots = { slots[0] = { x = 5; } } extended = 9; } }";
static const char *child_b = "{ inherit = \"z_parent\"; edit = { health = 200; shared = 0; "
    "slots = { slots[0] = { x = 1; } slots[2] = { x = 6; } } } }";
static const char *child_builtin = "{ inherit = \"z_parent\"; edit = { health = 100; shared = 3; "
    "slots = { slots[0] = { x = 1; } } } }";

static int baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *value = NULL;
    (void)context; *body = NULL; *length = 0;
    if (!strcmp(path, child_path)) value = child_base;
    if (!strcmp(path, parent_path)) value = "{ class = \"Base\"; edit = {} }";
    if (!value) return 0;
    *body = (unsigned char *)_strdup(value); *length = strlen(value); return *body ? 1 : -1;
}
static int known(const char *name)
{ return !strcmp(name, "Base") || !strcmp(name, "Derived") || !strcmp(name, "Rewritten"); }
static int derives(void *context, const char *child, const char *parent)
{
    (void)context;
    if (!known(child) || !known(parent)) return -1;
    return !strcmp(child, parent) || !strcmp(parent, "Base");
}
static int describe(void *context, sh_decl_value_type type, sh_decl_value_shape *shape)
{
    (void)context;
    if (type.ops && !strcmp(type.ops, "[4]")) {
        shape->kind = SH_DECL_VALUE_COLLECTION; shape->count = 4; shape->count_known = 1;
        shape->element = (sh_decl_value_type){type.name, ""};
    } else if (known(type.name) || !strcmp(type.name, "Slot")) shape->kind = SH_DECL_VALUE_OBJECT;
    else if (!strcmp(type.name, "int") || !strcmp(type.name, "float")) shape->kind = SH_DECL_VALUE_IGNORE;
    return 1;
}
static int field(void *context, sh_decl_value_type type, const char *key, sh_decl_value_type *out)
{
    (void)context;
    if (!strcmp(key, "slots")) *out = (sh_decl_value_type){"Slot", "[4]"};
    else if (!strcmp(key, "health")) *out = (sh_decl_value_type){!strcmp(type.name, "Rewritten") ? "float" : "int", ""};
    else if (!strcmp(key, "shared") || !strcmp(key, "x") ||
        (!strcmp(type.name, "Derived") && !strcmp(key, "extended"))) *out = (sh_decl_value_type){"int", ""};
    else return -1;
    return 1;
}
static void authored(const char *package, const char *name, const char *body)
{
    char path[512];
    snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/%s.decl", package, name);
    create(path, body);
}
static sh_package_compilation *compile(sh_package_compile_environment *environment,
    sh_package_sources **sources, char *error)
{
    *sources = sh_package_sources_scan(root, error, 2048); CHECK(*sources);
    return *sources ? sh_package_compile_with(*sources, environment, error, 2048) : NULL;
}
int main(void)
{
    char temporary[MAX_PATH], error[2048];
    sh_decl_dependency_schema types = {NULL, describe, field, NULL, NULL};
    sh_package_builtin builtins[] = {
        {"generated/decls/snapeditorentitydef/builtin_only.decl", (const unsigned char *)"{ edit = { x = 1; } }", 0},
        {"generated/decls/entitydef/a_child.decl", (const unsigned char *)child_builtin, 0}
    };
    sh_package_compile_environment environment = {baseline, NULL, builtins, 2, &types, NULL, derives};
    sh_package_sources *sources;
    sh_package_compilation *compiled;
    const sh_compiled_resource *child, *builtin;
    size_t i;
    for (i = 0; i < 2; i++) builtins[i].length = strlen((const char *)builtins[i].body);
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "pce", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    create("overrides/a/package.json", "{\"id\":\"a\",\"name\":\"A\"}");
    create("overrides/b/package.json", "{\"id\":\"b\",\"name\":\"B\"}");
    authored("a", "a_child", child_a); authored("b", "a_child", child_b);
    authored("a", "z_parent", "{ class = \"Derived\"; edit = {} }");
    compiled = compile(&environment, &sources, error);
    if (!compiled) fprintf(stderr, "%s\n", error);
    CHECK(compiled);
    if (compiled) {
        CHECK(compiled->resource_count == 3 && compiled->composed_count == 1);
        child = sh_package_compilation_find(compiled, child_path);
        CHECK(child && child->source_count == 2 && child->owners.bits == 3 && child->gameplay_owners.bits == 3);
        CHECK(child && strstr((char *)child->body, "health = 200;") && strstr((char *)child->body, "shared = 3;"));
        CHECK(child && strstr((char *)child->body, "extended = 9;") && strstr((char *)child->body, "x = 5;"));
        CHECK(child && strstr((char *)child->body, "slots[2]") && !strstr((char *)child->body, "num ="));
        builtin = sh_package_compilation_find(compiled, builtins[0].engine_path);
        CHECK(builtin && !builtin->source_count && !sh_package_owners_count(&builtin->owners) && !sh_package_owners_count(&builtin->gameplay_owners) && builtin->baseline_known == 3);
        for (i = 0; i < sources->file_count; i++) if (!sources->files[i].directory) CHECK(sh_package_source_verify(&sources->files[i]));
    }
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    /* A child authored with no corresponding parent edit cannot borrow a
     * different package's resulting class to legitimize its own unknown field. */
    authored("b", "a_child", child_a);
    compiled = compile(&environment, &sources, error);
    CHECK(!compiled && strstr(error, "extended") && strstr(error, "reader type"));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    authored("b", "a_child", child_b);
    /* A parent's headers are merged from all original sources before child
     * state, even when parent sorts later and its final object does not exist. */
    authored("b", "z_parent", "{ class = \"Rewritten\"; edit = {} }");
    compiled = compile(&environment, &sources, error);
    CHECK(!compiled && strstr(error, "class") && strstr(error, "incompatible"));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    authored("b", "z_parent", "{ class = \"Base\"; edit = {} }");
    authored("a", "z_parent", "{ inherit = \"a_child\"; class = \"Derived\"; edit = {} }");
    compiled = compile(&environment, &sources, error);
    CHECK(!compiled && strstr(error, "cycl"));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    /* A product-only parent is available to new package resources. */
    authored("a", "z_parent", "{ class = \"Derived\"; edit = {} }");
    authored("a", "new_child", "{ inherit = \"product_parent\"; edit = { health = 50; } }");
    builtins[0].engine_path = "generated/decls/entitydef/product_parent.decl";
    builtins[0].body = (const unsigned char *)"{ class = \"Base\"; edit = {} }";
    builtins[0].length = strlen((const char *)builtins[0].body);
    compiled = compile(&environment, &sources, error);
    if (!compiled) fprintf(stderr, "%s\n", error);
    CHECK(compiled && sh_package_compilation_find(compiled, "generated/decls/entitydef/new_child.decl"));
    sh_package_compilation_free(compiled);
    /* Invalid producer inventories fail before any prospective output exists. */
    builtins[1] = builtins[0];
    compiled = sh_package_compile_with(sources, &environment, error, sizeof(error));
    CHECK(!compiled && strstr(error, "built-in resource"));
    builtins[0].engine_path = NULL;
    compiled = sh_package_compile_with(sources, &environment, error, sizeof(error));
    CHECK(!compiled && strstr(error, "built-in resource"));
    sh_package_sources_free(sources); cleanup();
    if (failures) return 1;
    puts("package_environment_test: PASS"); return 0;
}
