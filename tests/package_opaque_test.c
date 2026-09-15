/* Original-relative binary replacements without whole-file materialization. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_compiler.h"
#include "package_fixture.h"

static const char *resource_path = "sound/soundbanks/pc/sample.bnk";
typedef struct original_fixture {
    sh_package_original_identity original;
    int answer, identity_calls, byte_calls, byte_scope, producer_calls;
    const char *bytes;
} original_fixture;

static int identity_reader(void *context, const char *path, sh_package_original_identity *out,
    char *error, size_t capacity)
{
    original_fixture *fixture = context;
    CHECK(!strcmp(path, resource_path)); fixture->identity_calls++;
    *out = fixture->original;
    if (fixture->answer == -1) snprintf(error, capacity, "original identity read failed");
    return fixture->answer;
}
static int byte_reader(void *context, const char *path, unsigned char **body, size_t *length)
{
    original_fixture *fixture = context;
    (void)path; fixture->byte_calls++; *body = NULL; *length = 0;
    if (fixture->byte_scope > 0) {
        *length = strlen(fixture->bytes); *body = (unsigned char *)_strdup(fixture->bytes);
        CHECK(*body);
    }
    return fixture->byte_scope;
}
static int producer(void *context, const char *path, sh_package_producer_reader read,
    void *read_context, unsigned char **body, size_t *length)
{
    original_fixture *fixture = context;
    (void)read; (void)read_context;
    CHECK(!strcmp(path, resource_path)); fixture->producer_calls++;
    *body = (unsigned char *)_strdup("product"); *length = 7; return *body ? 1 : -1;
}

static sh_package_sources *inventory(const char *a, const char *b, const char *c)
{
    const char *values[] = {a, b, c};
    char path[512], descriptor[128], error[512];
    sh_package_sources *sources;
    for (unsigned i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "overrides/p%u/package.json", i);
        snprintf(descriptor, sizeof(descriptor), "{\"id\":\"p%u\",\"name\":\"Package %u\"}", i, i);
        create(path, descriptor);
        snprintf(path, sizeof(path), "overrides/p%u/assets/%s", i, resource_path); create(path, values[i]);
    }
    sources = sh_package_sources_scan(root, error, sizeof(error));
    if (!sources) fprintf(stderr, "%s\n", error);
    CHECK(sources && sources->package_count == 3); return sources;
}

static void expect(const sh_package_sources *sources, sh_package_compile_environment *environment,
    int scope, uint64_t owners, const char *bytes)
{
    char error[1024]; size_t length = 0;
    sh_package_compilation *compiled = sh_package_compile_with(sources, environment, error, sizeof(error));
    const sh_compiled_resource *resource;
    unsigned char *body;
    if (!compiled) fprintf(stderr, "%s\n", error);
    CHECK(compiled); if (!compiled) return;
    resource = sh_package_compilation_find(compiled, resource_path);
    CHECK(resource && compiled->resource_count == 1);
    if (resource) {
        CHECK(resource->baseline_known == scope);
        CHECK(resource->owners.bits == owners && resource->gameplay_owners.bits == owners);
        CHECK(resource->source_count == 3 && !resource->body && !resource->composed);
        body = sh_package_compilation_read(compiled, resource, SIZE_MAX, &length, error, sizeof(error));
        CHECK(body && length == strlen(bytes) && !memcmp(body, bytes, length)); free(body);
    }
    sh_package_compilation_free(compiled);
}

static void refuse(const sh_package_sources *sources, sh_package_compile_environment *environment,
    const char *message)
{
    char error[1024];
    sh_package_compilation *compiled = sh_package_compile_with(sources, environment, error, sizeof(error));
    CHECK(!compiled && strstr(error, message)); sh_package_compilation_free(compiled);
}

int main(void)
{
    char temporary[MAX_PATH], error[512];
    original_fixture fixture = {0};
    sh_package_compile_environment environment = {0};
    sh_package_sources *sources;
    sh_package_builtin builtin = {"sound/soundbanks/pc/sample.bnk", (const unsigned char *)"product", 7};
    CHECK(GetTempPathA(sizeof(temporary), temporary));
    CHECK(GetTempFileNameA(temporary, "pop", 0, root)); CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    environment.baseline_identity = identity_reader; environment.identity_context = &fixture;
    environment.baseline = byte_reader; environment.baseline_context = &fixture;
    fixture.answer = 1; fixture.byte_scope = -1; fixture.original.scope = 1;
    CHECK(sh_package_bytes_identity("stock", 5, &fixture.original.file));

    /* Same change in separate bundles retains both owners; a copied stock
     * resource contributes no owner. An authoritative identity skips bytes. */
    sources = inventory("stock", "changed", "changed");
    expect(sources, &environment, 1, 6, "changed"); CHECK(!fixture.byte_calls);
    fixture.original.scope = 2; expect(sources, &environment, 2, 7, "changed");
    fixture.original.scope = 3; expect(sources, &environment, 3, 6, "changed");
    sh_package_sources_free(sources);
    sources = inventory("stock", "stock", "stock");
    fixture.original.scope = 1; expect(sources, &environment, 1, 0, "stock");
    fixture.original.scope = 2; expect(sources, &environment, 2, 7, "stock");
    fixture.original.scope = 3; expect(sources, &environment, 3, 0, "stock");
    fixture.original.scope = 0; expect(sources, &environment, 0, 7, "stock");

    /* The verified original may exceed 4 GiB. No original or contribution
     * payload buffer is required for this choice, regardless of byte reader. */
    fixture.original.scope = 1; fixture.original.file.length = (UINT64_C(1) << 32) + 17;
    expect(sources, &environment, 1, 7, "stock"); CHECK(!fixture.byte_calls);
    CHECK(sh_package_bytes_identity("stock", 5, &fixture.original.file));
    fixture.answer = 0; fixture.byte_scope = 1; fixture.bytes = "stock";
    expect(sources, &environment, 1, 0, "stock"); CHECK(fixture.byte_calls == 1);
    fixture.byte_scope = -1; refuse(sources, &environment, "original is unreadable");
    fixture.answer = -1; refuse(sources, &environment, "identity read failed");
    fixture.answer = 2; refuse(sources, &environment, "identity is unavailable or invalid");
    fixture.answer = 1; fixture.original.scope = 4;
    refuse(sources, &environment, "identity is unavailable or invalid");
    fixture.original.scope = -1; refuse(sources, &environment, "identity is unavailable or invalid");
    fixture.original.scope = 1;

    /* A changed duplicate must fail independently even though the other
     * package still has the bytes recorded by both inventory entries. */
    create("overrides/p2/assets/sound/soundbanks/pc/sample.bnk", "stale");
    refuse(sources, &environment, "source changed");
    sh_package_sources_free(sources);
    sources = inventory("stock", "one", "two");
    refuse(sources, &environment, "different opaque replacements");
    sh_package_sources_free(sources);
    sources = inventory("one", "two", "stock");
    refuse(sources, &environment, "different opaque replacements");

    /* Built-ins and producers participate in the same original-relative pass.
     * A verified absent game file still consults the product's original. */
    sh_package_sources_free(sources); sources = inventory("stock", "stock", "stock");
    environment.builtins = &builtin; environment.builtin_count = 1;
    {
        sh_package_compilation *compiled = sh_package_compile_with(sources, &environment, error, sizeof(error));
        const sh_compiled_resource *resource = sh_package_compilation_find(compiled, resource_path);
        CHECK(resource && resource->body && !strcmp((char *)resource->body, "product"));
        CHECK(resource && !resource->owners.bits && resource->baseline_known == 1);
        sh_package_compilation_free(compiled);
    }
    fixture.original.scope = 0; fixture.byte_scope = 0;
    expect(sources, &environment, 3, 7, "stock");
    sh_package_sources_free(sources); sources = inventory("product", "product", "product");
    expect(sources, &environment, 3, 0, "product");
    fixture.original.scope = 1; expect(sources, &environment, 1, 7, "product");
    sh_package_sources_free(sources); sources = inventory("different", "different", "different");
    refuse(sources, &environment, "built-in default");
    environment.builtins = NULL; environment.builtin_count = 0;
    environment.produce = producer; environment.producer_context = &fixture;
    refuse(sources, &environment, "generated resource");
    fixture.original.scope = 0;
    expect(sources, &environment, 3, 7, "different");
    fixture.byte_scope = -1; refuse(sources, &environment, "original is unreadable");
    environment.produce = NULL; fixture.byte_scope = 0;
    sh_package_sources_free(sources); sources = inventory("", "", "");
    fixture.original.scope = 1; CHECK(sh_package_bytes_identity(NULL, 0, &fixture.original.file));
    expect(sources, &environment, 1, 0, "");
    sh_package_sources_free(sources);

    /* Declaration readers must never be replaced by opaque identities. */
    create("declarations/package.json", "{\"id\":\"declarations\",\"name\":\"Declarations\"}");
    create("declarations/assets/generated/decls/entitydef/example.decl", "{ edit = { health = 10; } }");
    snprintf(temporary, sizeof(temporary), "%s/declarations", root);
    sources = sh_package_sources_scan_directory(temporary, error, sizeof(error)); CHECK(sources);
    fixture.answer = -1;
    {
        int calls = fixture.identity_calls;
        sh_package_compilation *compiled = sh_package_compile_with(sources, &environment, error, sizeof(error));
        CHECK(compiled && fixture.identity_calls == calls); sh_package_compilation_free(compiled);
    }
    sh_package_sources_free(sources); cleanup();
    if (failures) return 1;
    puts("opaque compiler identity and ownership checks passed"); return 0;
}
