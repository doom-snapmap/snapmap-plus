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
static const char *seq_root_path = "generated/decls/entitydef/seq_root.decl";
static const char *seq_child_path = "generated/decls/entitydef/seq_child.decl";
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
    if (!strcmp(path, seq_root_path)) value = "{ class = \"Base\"; editorVars { placeable = true; } edit = { health = 1; } }";
    if (!strcmp(path, seq_child_path)) value = "{ inherit = \"z_parent\"; edit = { health = 1; } }";
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
static void remove_authored(const char *package, const char *name)
{
    char path[4096];
    wchar_t *wide;
    snprintf(path, sizeof(path), "%s/overrides/%s/assets/generated/decls/entitydef/%s.decl", root, package, name);
    wide = sh_package_source_wide_path(path); CHECK(wide && DeleteFileW(wide)); free(wide);
}
static const size_t *owner_named(const sh_package_sources *sources, size_t index, const char *name)
{
    static const size_t found = 0;
    return sources && index < sources->package_count && !strcmp(sources->packages[index].name, name) ? &found : NULL;
}
static sh_package_compilation *compile(sh_package_compile_environment *environment,
    sh_package_sources **sources, char *error)
{
    *sources = sh_package_sources_scan(root, error, 2048); CHECK(*sources);
    return *sources ? sh_package_compile_with(*sources, environment, error, 2048) : NULL;
}
static size_t occurrences(const char *text, const char *needle)
{
    size_t count = 0;
    for (const char *at = text; (at = strstr(at, needle)) != NULL; at += strlen(needle)) count++;
    return count;
}
/* Compilation keeps both packages' exact authored copies as the sources. */
static void authored_copies(const sh_package_compilation *compiled, const sh_compiled_resource *resource,
    const char *first, const char *second)
{
    const char *texts[2] = {first, second};
    char error[512];
    CHECK(resource && resource->source_count == 2 && resource->owners.bits == 3);
    for (size_t i = 0; resource && i < resource->source_count && i < 2; i++) {
        size_t length = 0;
        unsigned char *bytes = sh_package_source_read(compiled->sources->files + resource->sources[i], SIZE_MAX,
            &length, error, sizeof(error));
        CHECK(bytes && length == strlen(texts[i]) && !memcmp(bytes, texts[i], length)); free(bytes);
    }
}
static const char *compiled_body(const sh_package_compilation *compiled, const char *path)
{
    const sh_compiled_resource *resource = compiled ? sh_package_compilation_find(compiled, path) : NULL;
    return resource && resource->body ? (const char *)resource->body : "";
}
/* M86: entityDef state text from several packages through the product
 * compiler. A definition with no parent is the first stateful layer: its first
 * block keeps repeats as an ordered sequence and its later statements are
 * inert. A child of a stateful parent updates: every statement named as the
 * inherited state applies in order, a repeat updating its first match.
 * editorVars members match in any case. */
static void test_entity_sequences(sh_package_compile_environment *environment)
{
    static const char *const root_a = "{ class = \"Base\"; editorVars { placeable = true; color = 1; } "
        "edit = { health = 40; health = 45; } edit = { shared = 5; } }";
    static const char *const root_b = "{ class = \"Base\"; editorVars { Placeable = true; size = 4; } "
        "edit = { health = 1; x = 3; } }";
    static const char *const child_a2 = "{ inherit = \"z_parent\"; edit = { health = 40; health = 45; } "
        "edit = { shared = 5; } }";
    static const char *const child_b2 = "{ inherit = \"z_parent\"; edit = { health = 1; x = 3; } }";
    static const char *const child_same = "{ inherit = \"z_parent\"; edit = { health = 45; } }";
    static const char *const unmodelled = "{ class = \"Base\"; editorVars { placeable = true; "
        "spawn = { a = 1; } spawn = { b = 2; } } edit = { health = 1; } }";
    sh_package_sources *sources;
    sh_package_compilation *compiled;
    const sh_compiled_resource *resource;
    const char *body, *at;
    char error[2048];
    authored("a", "seq_root", root_a); authored("b", "seq_root", root_b);
    authored("a", "seq_child", child_a2); authored("b", "seq_child", child_b2);
    compiled = compile(environment, &sources, error);
    if (!compiled) fprintf(stderr, "entity sequences: %s\n", error);
    CHECK(compiled);
    if (compiled) {
        /* Multi-change, first stateful layer: a's sequence stays in order, b's
         * addition joins it, a's later edit never reaches the state, and one
         * editorVars block holds both additions under the original spelling. */
        resource = sh_package_compilation_find(compiled, seq_root_path);
        authored_copies(compiled, resource, root_a, root_b);
        CHECK(resource && resource->composed);
        body = compiled_body(compiled, seq_root_path); at = strstr(body, "health = 40;");
        CHECK(at && strstr(at, "health = 45;") && strstr(body, "x = 3;") && !strstr(body, "shared = 5;"));
        CHECK(occurrences(body, "edit =") == 1 && occurrences(body, "editorVars") == 1);
        CHECK(strstr(body, "color = 1;") && strstr(body, "size = 4;") && occurrences(body, "placeable") == 1 &&
            !strstr(body, "Placeable"));
        if (!at || strstr(body, "shared = 5;")) fprintf(stderr, "entity sequences root: %s\n", body);
        /* Multi-change, update mode: the repeat leaves its last value and the
         * later edit applies. */
        resource = sh_package_compilation_find(compiled, seq_child_path);
        authored_copies(compiled, resource, child_a2, child_b2);
        body = compiled_body(compiled, seq_child_path);
        CHECK(strstr(body, "health = 45;") && !strstr(body, "health = 40;") && strstr(body, "shared = 5;") &&
            strstr(body, "x = 3;") && occurrences(body, "edit =") == 1);
        if (!strstr(body, "shared = 5;")) fprintf(stderr, "entity sequences child: %s\n", body);
    }
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    /* Equal variants: identical copies, and in update mode a repeat equal in
     * effect to a single assignment, are one change with both provenances. */
    authored("b", "seq_root", root_a); authored("b", "seq_child", child_same);
    compiled = compile(environment, &sources, error);
    if (!compiled) fprintf(stderr, "entity sequences equal: %s\n", error);
    CHECK(compiled);
    if (compiled) {
        resource = sh_package_compilation_find(compiled, seq_root_path);
        authored_copies(compiled, resource, root_a, root_a);
        body = compiled_body(compiled, seq_root_path); at = strstr(body, "health = 40;");
        CHECK(resource && !resource->composed && at && strstr(at, "health = 45;") && !strstr(body, "shared = 5;"));
        resource = sh_package_compilation_find(compiled, seq_child_path);
        authored_copies(compiled, resource, child_a2, child_same);
        body = compiled_body(compiled, seq_child_path);
        CHECK(occurrences(body, "health =") == 1 && strstr(body, "health = 45;") && strstr(body, "shared = 5;"));
    }
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    /* Different variants of one sequence are a peer conflict, never either. */
    authored("b", "seq_root", "{ class = \"Base\"; edit = { health = 40; health = 50; } }");
    compiled = compile(environment, &sources, error);
    CHECK(!compiled && strstr(error, "seq_root") && *environment->invalid_package == SIZE_MAX);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    /* One change the composer does not model is served exactly as written;
     * with a second change it stays apart, unmerged and unattributed. */
    authored("a", "seq_root", unmodelled); remove_authored("b", "seq_root");
    compiled = compile(environment, &sources, error);
    if (!compiled) fprintf(stderr, "entity sequences exact: %s\n", error);
    resource = compiled ? sh_package_compilation_find(compiled, seq_root_path) : NULL;
    CHECK(resource && resource->source_count == 1 && resource->body_length == strlen(unmodelled) &&
        !memcmp(resource->body, unmodelled, resource->body_length));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    authored("b", "seq_root", root_b);
    compiled = compile(environment, &sources, error);
    CHECK(!compiled && strstr(error, "seq_root") && *environment->invalid_package == SIZE_MAX);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    /* Leave equal copies, which compile, for the checks that follow. */
    authored("a", "seq_root", root_b);
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
    size_t invalid = SIZE_MAX;
    /* Defects that a package has on its own, with only the installed game and
     * product defaults, whatever other packages are installed. */
    static const struct { const char *body, *reason; } defects[] = {
        {"{ class = \"Deirved\"; edit = { health = 1; } }", "metadata is unavailable"},
        {"{ inherit = \"z_parnet\"; edit = { health = 1; } }", "parent declaration is absent"},
        {"{ inherit = \"z_parent\"; edit = { slots = { slots[7] = { x = 1; } } } }", "out-of-range"},
        {"{ edit = { health = 1; } }", "no class or inherited class"}
    };
    sh_package_sources *sources;
    sh_package_compilation *compiled;
    const sh_compiled_resource *child, *builtin;
    size_t i;
    for (i = 0; i < 2; i++) builtins[i].length = strlen((const char *)builtins[i].body);
    /* Local isolation must never blame one package for peer conflicts, class
     * or inheritance failures, or anything it did not author alone. */
    environment.invalid_package = &invalid;
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
    CHECK(!compiled && strstr(error, "extended") && strstr(error, "reader type") && invalid == SIZE_MAX);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    authored("b", "a_child", child_b);
    /* A parent's headers are merged from all original sources before child
     * state, even when parent sorts later and its final object does not exist. */
    authored("b", "z_parent", "{ class = \"Rewritten\"; edit = {} }");
    compiled = compile(&environment, &sources, error);
    CHECK(!compiled && strstr(error, "class") && strstr(error, "incompatible") && invalid == SIZE_MAX);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    authored("b", "z_parent", "{ class = \"Base\"; edit = {} }");
    authored("a", "z_parent", "{ inherit = \"a_child\"; class = \"Derived\"; edit = {} }");
    /* This cycle is complete inside package a, so a fails alone. */
    compiled = compile(&environment, &sources, error);
    CHECK(!compiled && strstr(error, "cycl") && owner_named(sources, invalid, "a"));
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
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    /* A typed contribution that cannot be parsed on its own belongs to its
     * package, even when another package supplies a valid edit. */
    create("overrides/c/package.json", "{\"id\":\"c\",\"name\":\"C\"}");
    authored("c", "a_child", "{ inherit = \"z_parent\"; edit = { health = !300; } }");
    compiled = compile(&environment, &sources, error);
    CHECK(!compiled && sources && invalid < sources->package_count && !strcmp(sources->packages[invalid].name, "c"));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    /* A class view contains the original and the contribution's own package,
     * so a parent that only another package declares is reported as missing.
     * That depends on a peer and is never attributed to either package. */
    authored("c", "a_child", child_b);
    create("overrides/d/package.json", "{\"id\":\"d\",\"name\":\"D\"}");
    authored("c", "c_parent", "{ class = \"Derived\"; edit = {} }");
    authored("d", "d_child", "{ inherit = \"c_parent\"; edit = { extended = 3; } }");
    compiled = compile(&environment, &sources, error);
    CHECK(!compiled && strstr(error, "d_child") && strstr(error, "parent declaration is absent") && invalid == SIZE_MAX);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    authored("d", "d_child", "{ inherit = \"z_parent\"; edit = { health = 3; } }");
    create("overrides/e/package.json", "{\"id\":\"e\",\"name\":\"E\"}");
    for (i = 0; i < sizeof(defects) / sizeof(defects[0]); i++) {
        authored("e", "e_new", defects[i].body);
        compiled = compile(&environment, &sources, error);
        CHECK(!compiled && owner_named(sources, invalid, "e") && strstr(error, "[e]") && strstr(error, defects[i].reason));
        if (compiled || !strstr(error, defects[i].reason)) fprintf(stderr, "defect %zu: %s\n", i, compiled ? "compiled" : error);
        sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    }
    /* A parent that disappears after the scan is a read failure, not a defect. */
    authored("e", "e_new", "{ inherit = \"e_parent\"; edit = { health = 1; } }");
    authored("e", "e_parent", "{ inherit = \"z_parent\"; edit = {} }");
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    remove_authored("e", "e_parent");
    compiled = sources ? sh_package_compile_with(sources, &environment, error, sizeof(error)) : NULL;
    CHECK(!compiled && strstr(error, "e_new") && invalid == SIZE_MAX);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    authored("e", "e_parent", "{ inherit = \"z_parent\"; edit = {} }");
    /* An unexpanded definition prepares its own state text alone, but Parse
     * still takes its class from the chain: a valid declaration, never a
     * classless defect of its package. */
    authored("e", "e_unexpanded", "{ inherit = \"z_parent\"; expandInheritance = false; poolCount = 3; "
        "edit = { health = 1; } }");
    compiled = compile(&environment, &sources, error);
    if (!compiled) fprintf(stderr, "unexpanded: %s\n", error);
    CHECK(compiled && sh_package_compilation_find(compiled, "generated/decls/entitydef/e_unexpanded.decl"));
    if (compiled) {
        const char *body = compiled_body(compiled, "generated/decls/entitydef/e_unexpanded.decl");
        const char *parent = body ? strstr(body, "inherit") : NULL, *mode = body ? strstr(body, "expandInheritance") : NULL;
        CHECK(parent && mode && parent < mode && strstr(body, "health = 1;"));
    }
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    /* Packages that are valid alone but disagree remain one peer conflict. */
    authored("e", "a_child", "{ inherit = \"z_parent\"; edit = { health = 300; shared = 0; "
        "slots = { slots[0] = { x = 1; } } } }");
    compiled = compile(&environment, &sources, error);
    CHECK(!compiled && strstr(error, "incompatible edits") && invalid == SIZE_MAX);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    /* A product default is always installed, so contradicting it fails alone. */
    authored("e", "a_child", "{ inherit = \"z_parent\"; edit = { health = 200; shared = 4; "
        "slots = { slots[0] = { x = 1; } } } }");
    compiled = compile(&environment, &sources, error);
    CHECK(!compiled && owner_named(sources, invalid, "e") && strstr(error, "built-in default"));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    authored("e", "a_child", child_b);
    compiled = compile(&environment, &sources, error);
    if (!compiled) fprintf(stderr, "%s\n", error);
    CHECK(compiled && invalid == SIZE_MAX);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    test_entity_sequences(&environment);
    sources = sh_package_sources_scan(root, error, sizeof(error));
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
