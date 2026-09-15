/* Generated outputs compose from isolated original and effective input views. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_compiler.h"
#include "package_fixture.h"

static const char *target = "generated/decls/material/a_derived.decl";
static const char *stock = "generated/decls/material/z_stock.decl";
static const char *base = "{ edit = { x = 1; y = 2; } }";
static const char *edit_x = "{ edit = { x = 9; y = 2; } }";
static const char *edit_y = "{ edit = { x = 1; y = 8; } }";
static int mode, original_reads, effective_reads;
static int baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *value = NULL;
    (void)context; *body = NULL; *length = 0;
    if (!strcmp(path, stock)) value = base;
    if (!strcmp(path, "z_stock.bin")) value = "original";
    if (mode == 4 && !strcmp(path, stock)) return -1;
    if (!value) return 0;
    *body = (unsigned char *)_strdup(value); *length = strlen(value); return *body ? 1 : -1;
}
static char *canonical(const char *path)
{
    char *key = sh_package_engine_path(path);
    if (key && !strcmp(key, "aliases/derived.decl")) { free(key); key = _strdup(target); }
    return key;
}
static int produce(void *context, const char *path, sh_package_producer_reader read,
    void *reader_context, unsigned char **body, size_t *length)
{
    const char *input;
    int got;
    (void)context;
    if (!strcmp(path, target)) input = mode == 1 ? "middle.decl" : mode == 3 ? "missing.bin" : stock;
    else if (!strcmp(path, "a_derived.bin")) input = "z_stock.bin";
    else if (!strcmp(path, "middle.decl") && mode == 1) input = target;
    else return 0;
    if (mode == 5) return 0;
    got = read(reader_context, input, body, length);
    if (got > 0) {
        if (*length == strlen(base) && !memcmp(*body, base, *length)) original_reads++;
        if (*length == strlen(edit_y) && !memcmp(*body, edit_y, *length)) effective_reads++;
    }
    return got > 0 ? 1 : -1;
}
static void authored(const char *package, const char *path, const char *body)
{
    char relative[512];
    snprintf(relative, sizeof(relative), "overrides/%s/assets/%s", package, path);
    create(relative, body);
}
static int input_available(void *context, const char *path, char *error, size_t capacity)
{
    (void)error; (void)capacity;
    CHECK(!strcmp(path, stock)); /* Output paths are generated, not installed. */
    return *(int *)context;
}
static void generated_availability(const sh_package_compilation *compiled)
{
    const char *required[] = {target, "ALIASES\\DERIVED.DECL", stock};
    sh_package_missing missing = {0};
    char error[1024]; int available = 1;
    CHECK(sh_package_compilation_missing(compiled, required, 3, input_available, &available, &missing, error, sizeof(error)));
    CHECK(missing.checked == 2 && !missing.count && !sh_package_owners_count(&missing.packages));
    available = 0;
    CHECK(sh_package_compilation_missing(compiled, required, 3, input_available, &available, &missing, error, sizeof(error)));
    CHECK(missing.checked == 2 && missing.count == 1 && !strcmp(missing.paths[0], stock));
    CHECK(missing.packages.bits == 2); /* Deliver the input package intact. */
    CHECK(sh_package_compilation_payload_missing(compiled, input_available, &available, &missing, error, sizeof(error)));
    CHECK(missing.count == 1 && !strcmp(missing.paths[0], stock) && missing.packages.bits == 2);
    available = 1;
    CHECK(sh_package_compilation_payload_missing(compiled, input_available, &available, &missing, error, sizeof(error)));
    CHECK(!missing.count && !sh_package_owners_count(&missing.packages));
    sh_package_missing_free(&missing);
}
static sh_package_compilation *compile(sh_package_compile_environment *env,
    sh_package_sources **sources, char *error)
{
    *sources = sh_package_sources_scan(root, error, 2048); CHECK(*sources);
    return *sources ? sh_package_compile_with(*sources, env, error, 2048) : NULL;
}
static void remove_authored(sh_package_sources *sources, const char *path)
{
    size_t i, j;
    for (i = 0; i < sources->file_count; i++) {
        const sh_package_source_file *file = &sources->files[i];
        if (!file->engine_path || strcmp(file->engine_path, path)) continue;
        char absolute[4096], tracked[4096];
        int removed = 0;
        CHECK(DeleteFileA(file->absolute));
        CHECK(GetFullPathNameA(file->absolute, sizeof(absolute), absolute, NULL));
        for (j = 0; j < created_count; j++) {
            CHECK(GetFullPathNameA(created[j], sizeof(tracked), tracked, NULL));
            if (_stricmp(tracked, absolute)) continue;
            memmove(created + j, created + j + 1, (created_count - j - 1) * sizeof(*created));
            memmove(directories + j, directories + j + 1, (created_count - j - 1) * sizeof(*directories));
            created_count--; removed = 1; break;
        }
        CHECK(removed);
    }
}
int main(void)
{
    char temporary[MAX_PATH], error[2048];
    sh_package_compile_environment env = {0};
    sh_package_sources *sources = NULL, *next_sources = NULL;
    sh_package_compilation *compiled, *next;
    const sh_compiled_resource *resource;
    size_t i;
    env.baseline = baseline; env.produce = produce; env.canonical_path = canonical;
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "pcg", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    create("overrides/a/package.json", "{\"id\":\"a\",\"name\":\"Target package\"}");
    create("overrides/b/package.json", "{\"id\":\"b\",\"name\":\"Input package\"}");
    authored("a", target, edit_x); authored("b", stock, edit_y);
    compiled = compile(&env, &sources, error);
    if (!compiled) fprintf(stderr, "%s\n", error);
    CHECK(compiled && original_reads && effective_reads);
    if (compiled) generated_availability(compiled);
    resource = sh_package_compilation_find(compiled, target);
    CHECK(resource && resource->generated && resource->baseline_known == 3 && resource->composed);
    CHECK(resource && strstr((char *)resource->body, "x = 9;") && strstr((char *)resource->body, "y = 8;"));
    CHECK(resource && resource->source_count == 1 && resource->owners.bits == 3 && resource->gameplay_owners.bits == 3);
    CHECK(resource && resource->generated_input_count == 1 && !strcmp(resource->generated_inputs[0], stock));
    CHECK(resource == sh_package_compilation_find(compiled, "ALIASES\\DERIVED.DECL"));
    for (i = 0; sources && i < sources->file_count; i++) if (!sources->files[i].directory) CHECK(sh_package_source_verify(&sources->files[i]));
    /* A removed output must regenerate with today's inputs, not yesterday's
     * original, and must no longer claim the removed author's package. */
    env.previous = compiled;
    remove_authored(sources, target);
    next = compile(&env, &next_sources, error);
    if (!next) fprintf(stderr, "%s\n", error);
    resource = sh_package_compilation_find(next, target);
    CHECK(resource && resource->generated && !resource->source_count && resource->owners.bits == 2);
    CHECK(resource && !strcmp((char *)resource->body, edit_y));
    if (next) generated_availability(next);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    compiled = next; sources = next_sources; env.previous = compiled;
    remove_authored(sources, stock);
    next = compile(&env, &next_sources, error);
    resource = sh_package_compilation_find(next, target);
    CHECK(resource && resource->generated && !resource->owners.bits && !resource->gameplay_owners.bits);
    CHECK(resource && !strcmp((char *)resource->body, base));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    /* A removed producer is an error; no incomplete replacement snapshot. */
    env.previous = next; mode = 5;
    compiled = compile(&env, &sources, error);
    CHECK(!compiled && strstr(error, "lost its producer"));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    sh_package_compilation_free(next); sh_package_sources_free(next_sources); env.previous = NULL; mode = 0;
    /* Aliases group intact duplicates and use the same conflict identity. */
    authored("a", target, edit_x); authored("b", "aliases/derived.decl", edit_x);
    compiled = compile(&env, &sources, error);
    resource = sh_package_compilation_find(compiled, target);
    CHECK(compiled && compiled->duplicate_count == 1 && compiled->resource_count == 1);
    CHECK(resource && resource->source_count == 2 && resource->owners.bits == 3);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    authored("b", "aliases/derived.decl", "{ edit = { x = 7; y = 2; } }");
    compiled = compile(&env, &sources, error);
    CHECK(!compiled && strstr(error, "[a; b]") && strstr(error, "edit.x"));
    sh_package_compilation_free(compiled);
    remove_authored(sources, "aliases/derived.decl"); sh_package_sources_free(sources);
    /* Changed producer and direct output participate in one conflict. */
    authored("b", stock, "{ edit = { x = 7; y = 2; } }");
    compiled = compile(&env, &sources, error);
    CHECK(!compiled && strstr(error, "[a; generated resource]") && strstr(error, "generated inputs from: b"));
    sh_package_compilation_free(compiled);
    remove_authored(sources, stock); sh_package_sources_free(sources);
    /* A cycle cannot use an authored candidate to bypass its missing original. */
    mode = 1; compiled = compile(&env, &sources, error);
    CHECK(!compiled && strstr(error, "cyclic"));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    mode = 3; compiled = compile(&env, &sources, error);
    CHECK(!compiled && strstr(error, "producer failed"));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    mode = 4; compiled = compile(&env, &sources, error);
    CHECK(!compiled); sh_package_compilation_free(compiled); sh_package_sources_free(sources); mode = 0;
    /* One product identity cannot have both a static and dynamic producer. */
    {
        sh_package_builtin builtin = {target, (const unsigned char *)base, strlen(base)};
        env.builtins = &builtin; env.builtin_count = 1;
        compiled = compile(&env, &sources, error);
        CHECK(!compiled && strstr(error, "multiple product producers"));
        sh_package_compilation_free(compiled); sh_package_sources_free(sources);
        env.builtins = NULL; env.builtin_count = 0;
    }
    authored("a", "a_derived.bin", "private"); authored("b", "z_stock.bin", "stock edit");
    compiled = compile(&env, &sources, error);
    CHECK(!compiled && strstr(error, "opaque replacements") && strstr(error, "generated inputs from: b"));
    sh_package_compilation_free(compiled); sh_package_sources_free(sources);
    cleanup();
    if (failures) return 1;
    puts("package_producers_test: PASS"); return 0;
}
