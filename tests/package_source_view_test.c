/* Source-bound custom readers must not observe unrelated package state. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_compiler.h"
#include "package_fixture.h"

static const char *root_path = "generated/decls/material/a.decl";
static const char *schema_path = "generated/decls/entitydef/z_schema.decl";
static const char *original = "{ edit = { x = 0; y = 0; z = 0; } }";
static const char *part_a = "{ edit = { x = 1; y = 0; z = 0; } }";
static const char *part_b = "{ edit = { x = 0; y = 2; z = 0; } }";
static const char *part_product = "{ edit = { x = 0; y = 0; z = 3; } }";
static int calls, corrupt;

static int baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = NULL;
    (void)context; *body = NULL; *length = 0;
    if (!strcmp(path, "unreadable.bin")) return -1;
    if (!strcmp(path, schema_path)) text = original;
    if (!strcmp(path, root_path)) text = "{}";
    if (!text) return 0;
    *body = (unsigned char *)_strdup(text); *length = strlen(text); return *body ? 1 : -1;
}
static char *canonical(const char *path)
{
    char *key = sh_package_engine_path(path);
    if (key && !strcmp(key, "aliases/schema")) { free(key); key = _strdup(schema_path); }
    return key;
}
static void text_is(sh_decl_source got, const char *expected)
{
    CHECK(got.text && got.length == strlen(expected));
    if (got.text) CHECK(!memcmp(got.text, expected, got.length < strlen(expected) ? got.length : strlen(expected)));
}
static void empty(const sh_package_resource_inputs *inputs)
{
    CHECK(!inputs->original.text && !inputs->original.length && !inputs->original_scope);
    CHECK(!inputs->contributions && !inputs->owners && !inputs->count);
}
static int custom(void *context, const char *type, sh_decl_source base,
    const sh_decl_source *sources, size_t count, char **body, size_t *length,
    char *error, size_t capacity, sh_decl_conflict *conflict, const sh_package_source_view *view)
{
    sh_package_resource_inputs inputs = {0};
    char detail[512];
    (void)context; (void)base; (void)conflict;
    if (strcmp(type, "material")) return 0;
    calls++; CHECK(view && view->read && count == 3);
    if (!view || !view->read) return -1;
    if (corrupt) {
        CHECK(view->read(view->context, SH_DECL_COMPOSITION_RESULT, 0, schema_path,
            &inputs, detail, sizeof(detail)) == -1);
        empty(&inputs); CHECK(detail[0]);
        snprintf(error, capacity, "%s", detail); return -1;
    }
    CHECK(view->read(view->context, SH_DECL_COMPOSITION_ORIGINAL, SIZE_MAX, "ALIASES\\SCHEMA",
        &inputs, detail, sizeof(detail)) == 1);
    text_is(inputs.original, original); CHECK(inputs.original_scope == 1 && !inputs.count);
    sh_package_resource_inputs_free(&inputs); empty(&inputs);
    for (size_t i = 0; i < count; i++) {
        int a = strstr(sources[i].text, "package_a") != NULL;
        int b = strstr(sources[i].text, "package_b") != NULL;
        CHECK(view->read(view->context, SH_DECL_COMPOSITION_CONTRIBUTION, i, "ALIASES\\SCHEMA",
            &inputs, detail, sizeof(detail)) == 1);
        text_is(inputs.original, original);
        CHECK(inputs.count == (a ? 2u : 1u));
        for (size_t j = 0; j < inputs.count; j++) {
            text_is(inputs.contributions[j], a ? part_a : b ? part_b : part_product);
            CHECK(inputs.owners[j] == (a ? 0u : b ? 1u : SIZE_MAX));
        }
        sh_package_resource_inputs_free(&inputs);
    }
    CHECK(view->read(view->context, SH_DECL_COMPOSITION_RESULT, SIZE_MAX, schema_path,
        &inputs, detail, sizeof(detail)) == 1);
    CHECK(inputs.count == 4); text_is(inputs.original, original);
    if (inputs.count == 4) {
        text_is(inputs.contributions[0], part_a); text_is(inputs.contributions[1], part_a);
        text_is(inputs.contributions[2], part_b); text_is(inputs.contributions[3], part_product);
        CHECK(inputs.owners[0] == 0 && inputs.owners[1] == 0 && inputs.owners[2] == 1 && inputs.owners[3] == SIZE_MAX);
    }
    sh_package_resource_inputs_free(&inputs);
    CHECK(view->read(view->context, SH_DECL_COMPOSITION_ORIGINAL, 0, "product-only.bin",
        &inputs, detail, sizeof(detail)) == 1);
    text_is(inputs.original, "product"); CHECK(inputs.original_scope == 3 && !inputs.count);
    sh_package_resource_inputs_free(&inputs);
    CHECK(view->read(view->context, SH_DECL_COMPOSITION_RESULT, 0, "missing.bin",
        &inputs, detail, sizeof(detail)) == 0); empty(&inputs);
    CHECK(view->read(view->context, SH_DECL_COMPOSITION_RESULT, 0, "unreadable.bin",
        &inputs, detail, sizeof(detail)) == -1); empty(&inputs); CHECK(detail[0]);
    CHECK(view->read(view->context, SH_DECL_COMPOSITION_CONTRIBUTION, count, schema_path,
        &inputs, detail, sizeof(detail)) == -1); empty(&inputs);
    CHECK(view->read(view->context, SH_DECL_COMPOSITION_RESULT, 0, "../escape",
        &inputs, detail, sizeof(detail)) == -1); empty(&inputs);
    CHECK(view->read(view->context, (sh_decl_composition_role)99, 0, schema_path,
        &inputs, detail, sizeof(detail)) == -1); empty(&inputs);
    CHECK(view->read(view->context, SH_DECL_COMPOSITION_RESULT, 0, NULL,
        &inputs, NULL, 0) == -1); empty(&inputs);
    *body = _strdup("{}"); *length = 2; return *body ? 1 : -1;
}
int main(void)
{
    char temporary[MAX_PATH], error[2048];
    sh_package_compile_environment environment = {0};
    sh_package_builtin builtins[] = {
        {root_path, (const unsigned char *)"{ product }", 11},
        {schema_path, (const unsigned char *)part_product, 0},
        {"product-only.bin", (const unsigned char *)"product", 7}
    };
    sh_package_sources *sources;
    sh_package_compilation *compiled;
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "psv", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    create("overrides/a/package.json", "{\"id\":\"a\",\"name\":\"A\"}");
    create("overrides/b/package.json", "{\"id\":\"b\",\"name\":\"B\"}");
    create("overrides/a/support/package.json", "{\"id\":\"support\",\"name\":\"Support\"}");
    create("overrides/a/assets/generated/decls/material/a.decl", "{ package_a }");
    create("overrides/b/assets/generated/decls/material/a.decl", "{ package_b }");
    create("overrides/a/assets/generated/decls/entitydef/z_schema.decl", part_a);
    create("overrides/a/support/assets/aliases/schema", part_a);
    create("overrides/b/assets/generated/decls/entitydef/z_schema.decl", part_b);
    /* An unrelated opaque resource must never enter the projected source set. */
    create("overrides/b/assets/z-unrelated.bin", "untouched");
    builtins[1].length = strlen(part_product);
    environment.baseline = baseline; environment.builtins = builtins; environment.builtin_count = 3;
    environment.canonical_path = canonical; environment.compose_custom = custom;
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources);
    if (sources) {
        compiled = sh_package_compile_with(sources, &environment, error, sizeof(error));
        if (!compiled) fprintf(stderr, "%s\n", error);
        CHECK(compiled && calls == 1);
        if (compiled) {
            const sh_compiled_resource *schema = sh_package_compilation_find(compiled, schema_path);
            CHECK(schema && schema->source_count == 3 && schema->owners.bits == 3);
            CHECK(schema && schema->body && strstr((const char *)schema->body, "x = 1"));
            CHECK(schema && schema->body && strstr((const char *)schema->body, "y = 2"));
            CHECK(schema && schema->body && strstr((const char *)schema->body, "z = 3"));
        }
        sh_package_compilation_free(compiled);
        create("overrides/a/support/assets/aliases/schema", "changed since discovery");
        corrupt = 1;
        compiled = sh_package_compile_with(sources, &environment, error, sizeof(error));
        CHECK(!compiled && calls == 2 && error[0]); sh_package_compilation_free(compiled);
        sh_package_sources_free(sources);
    }
    cleanup();
    printf("package source views: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
