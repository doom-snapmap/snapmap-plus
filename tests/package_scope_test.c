#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_scope.h"
#include "package_compiler.h"
#include "package_fixture.h"

static size_t component(const sh_package_sources *sources, const char *owner, const char *relative)
{
    for (size_t i = 0; i < sources->component_count; i++)
        if (!strcmp(sources->packages[sources->components[i].owner].name, owner) &&
            !strcmp(sources->components[i].relative, relative)) return i;
    assert(0); return SIZE_MAX;
}

static void expect(const sh_package_compilation *compiled, const char *path, const char *expected)
{
    const sh_compiled_resource *resource = sh_package_compilation_find(compiled, path);
    char error[256]; size_t length;
    unsigned char *body;
    if (!expected) { assert(!resource); return; }
    assert(resource);
    body = sh_package_compilation_read(compiled, resource, SIZE_MAX, &length, error, sizeof(error));
    assert(body && length == strlen(expected) && !memcmp(body, expected, length)); free(body);
}

static void provenance(const sh_package_sources *sources, size_t child)
{
    size_t indices[3] = {SIZE_MAX, SIZE_MAX, SIZE_MAX};
    const sh_package_component *entry = sources->components + child;
    const unsigned char *fingerprint = sources->fingerprints[entry->owner];
    char error[512];
    sh_package_sources *copies = sh_package_sources_join(sources, sources, error, sizeof(error));
    assert(copies);
    assert(sh_package_scope_find(sources, fingerprint, entry->relative, NULL, 0) == 1);
    assert(sh_package_scope_find(sources, fingerprint, entry->relative, indices, 3) == 1 && indices[0] == child);
    assert(sh_package_scope_find(copies, fingerprint, entry->relative, NULL, 0) == 2);
    indices[0] = indices[1] = SIZE_MAX;
    assert(sh_package_scope_find(copies, fingerprint, entry->relative, indices, 1) == SIZE_MAX &&
        indices[0] == SIZE_MAX && indices[1] == SIZE_MAX);
    assert(sh_package_scope_find(copies, fingerprint, entry->relative, indices, 3) == 2 &&
        indices[0] == child && indices[1] == child + sources->component_count);
    assert(!sh_package_scope_find(sources, fingerprint, "absent", NULL, 0));
    {
        unsigned char unknown[32]; memcpy(unknown, fingerprint, 32); unknown[0] ^= 1;
        assert(!sh_package_scope_find(sources, unknown, entry->relative, NULL, 0));
    }
    /* Folder names can change on installation; the source key cannot depend
     * on them or nonunique descriptor IDs. A changed source tree is distinct. */
    {
        char saved[sizeof(copies->packages[entry->owner].name)];
        memcpy(saved, copies->packages[entry->owner].name, sizeof(saved));
        strcpy(copies->packages[entry->owner].name, "renamed-on-install");
        assert(sh_package_scope_find(copies, fingerprint, entry->relative, NULL, 0) == 2);
        memcpy(copies->packages[entry->owner].name, saved, sizeof(saved));
    }
    copies->components[child].owner = copies->package_count;
    indices[0] = SIZE_MAX;
    assert(sh_package_scope_find(copies, fingerprint, entry->relative, indices, 3) == SIZE_MAX && indices[0] == SIZE_MAX);
    copies->components[child].owner = entry->owner;
    sh_package_sources_free(copies);
}

static int original(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = "{ edit = { health = 100; speed = 1; } }";
    (void)context; *body = NULL; *length = 0;
    if (strcmp(path, "generated/decls/entitydef/actor/test.decl")) return 0;
    *body = (unsigned char *)_strdup(text); *length = strlen(text);
    return *body ? 1 : -1;
}

static void original_tree_identity(const sh_package_sources *sources)
{
    char error[512];
    sh_package_sources *joined, *copies = sh_package_sources_join(sources, sources, error, sizeof(error));
    sh_package_sources empty = {0};
    sh_package_scope view = {0};
    size_t c = component(sources, "legacy-a", ""), owner = sources->components[c].owner;
    size_t peer = owner + sources->package_count, visible;
    unsigned char keys[2][32];
    assert(copies);
    copies->original_fingerprints = malloc(copies->package_count * sizeof(*copies->original_fingerprints));
    assert(copies->original_fingerprints);
    memcpy(copies->original_fingerprints, copies->fingerprints, copies->package_count * sizeof(*copies->fingerprints));
    copies->original_fingerprints[owner][0] ^= 0x40;
    copies->original_fingerprints[peer][0] ^= 0x80;
    memcpy(keys[0], copies->original_fingerprints[owner], 32);
    memcpy(keys[1], copies->original_fingerprints[peer], 32);
    /* Equal normalized bytes do not select somebody else's original bundle. */
    assert(sh_package_scope_find(copies, keys[0], "", NULL, 0) == 1);
    assert(sh_package_scope_find(copies, keys[1], "", NULL, 0) == 1);
    assert(sh_package_scope_find(copies, sources->fingerprints[owner], "", NULL, 0) == 0);
    assert(sh_package_scope_find_legacy(copies, sources->fingerprints[owner], "", NULL, 0) == 2);
    assert(sh_package_scope_composed_keys(copies, keys, 1, &view, error, sizeof(error)));
    visible = 0;
    for (size_t i = 0; i < view.sources.file_count; i++) if (view.sources.files[i].engine_path) {
        assert(view.sources.files[i].owner == owner); visible++;
    }
    assert(visible == 1); sh_package_scope_close(&view);
    /* Mixed historical/new keys retain each selected owner once. */
    memcpy(keys[1], sources->fingerprints[owner], 32);
    assert(!sh_package_scope_composed_keys(copies, keys, 2, &view, error, sizeof(error)));
    assert(sh_package_scope_composed_keys_legacy(copies, keys, 2, &view, error, sizeof(error)));
    visible = 0;
    for (size_t i = 0; i < view.sources.file_count; i++) if (view.sources.files[i].engine_path) visible++;
    assert(visible == 2); sh_package_scope_close(&view);
    joined = sh_package_sources_join(&empty, copies, error, sizeof(error)); assert(joined);
    assert(joined->original_fingerprints && !memcmp(sh_package_source_tree(joined, owner), keys[0], 32));
    memcpy(keys[1], sh_package_source_tree(joined, peer), 32);
    assert(sh_package_sources_remove(joined, owner));
    assert(!memcmp(sh_package_source_tree(joined, peer - 1), keys[1], 32));
    sh_package_sources_free(joined); sh_package_sources_free(copies);
}

static void composed(const sh_package_sources *sources)
{
    const char *path = "generated/decls/entitydef/actor/test.decl";
    sh_package_scope legacy = {0}, isolated = {0};
    unsigned char keys[3][32];
    sh_package_compilation *compiled;
    const sh_compiled_resource *resource;
    unsigned char *body;
    char error[512]; size_t length;
    size_t owners[] = {sources->components[component(sources, "legacy-a", "")].owner,
        sources->components[component(sources, "legacy-b", "")].owner};
    memcpy(keys[0], sources->fingerprints[owners[0]], 32);
    memcpy(keys[1], sources->fingerprints[owners[1]], 32);
    memcpy(keys[2], keys[0], 32);
    assert(sh_package_scope_composed_keys(sources, keys, 3, &legacy, error, sizeof(error)));
    assert(legacy.component == SIZE_MAX && legacy.sources.file_count == sources->file_count);
    compiled = sh_package_compile(&legacy.sources, original, NULL, error, sizeof(error));
    if (!compiled) fprintf(stderr, "%s\n", error);
    assert(compiled && compiled->resource_count == 1 && !compiled->policy.requirements.count);
    resource = sh_package_compilation_find(compiled, path); assert(resource);
    body = sh_package_compilation_read(compiled, resource, SIZE_MAX, &length, error, sizeof(error));
    assert(body && strstr((char *)body, "health = 10") && strstr((char *)body, "speed = 2") &&
        !strstr((char *)body, "12000")); free(body); sh_package_compilation_free(compiled);
    {
        sh_package_sources *copies = sh_package_sources_join(sources, sources, error, sizeof(error));
        size_t visible = 0;
        assert(copies && sh_package_scope_composed_keys(copies, keys, 3, &legacy, error, sizeof(error)));
        for (size_t i = 0; i < legacy.sources.file_count; i++)
            if (legacy.sources.files[i].engine_path) visible++;
        assert(visible == 4); /* All four intact contributors reach the composer. */
        sh_package_scope_close(&legacy); sh_package_sources_free(copies);
    }
    keys[0][0] ^= 1;
    assert(!sh_package_scope_composed_keys(sources, keys, 3, &legacy, error, sizeof(error)) &&
        !legacy.sources.files && !legacy.sources.components && strstr(error, "source tree"));
    keys[0][0] ^= 1;
    /* An isolated addition in the same inventory does not alter the historical
     * composition. Each compiler receives only that context's contributions. */
    assert(sh_package_scope_open(sources, component(sources, "new-variant", ""), &isolated, error, sizeof(error)));
    compiled = sh_package_compile(&isolated.sources, NULL, NULL, error, sizeof(error)); assert(compiled);
    expect(compiled, path, "{ edit = { health = 12000; } }");
    sh_package_compilation_free(compiled); sh_package_scope_close(&isolated);
    assert(sh_package_scope_composed(sources, NULL, 0, &legacy, error, sizeof(error)));
    compiled = sh_package_compile(&legacy.sources, NULL, NULL, error, sizeof(error));
    assert(compiled && !compiled->resource_count && !compiled->policy.requirements.count);
    sh_package_compilation_free(compiled);
    owners[0] = sources->package_count;
    assert(!sh_package_scope_composed(sources, owners, 1, &legacy, error, sizeof(error)) &&
        !legacy.sources.files && !legacy.sources.components);
    sh_package_scope_close(&legacy);
}

int main(void)
{
    char temporary[MAX_PATH], error[512];
    sh_package_sources *sources;
    sh_package_scope scope = {0};
    sh_package_compilation *compiled;
    size_t child;
    assert(GetTempPathA(sizeof(temporary), temporary));
    snprintf(root, sizeof(root), "%ssmp-scope-%lu-%llu", temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(root, NULL));
    create("overrides/rocks/package.json", "{\"id\":\"rocks\",\"name\":\"Rocks\"}");
    create("overrides/rocks/assets/shared.bin", "shared");
    create("overrides/rocks/assets/color.bin", "parent");
    create("overrides/rocks/red/package.json", "{\"id\":\"rocks\",\"name\":\"Red\"}");
    create("overrides/rocks/red/assets/color.bin", "red");
    create("overrides/rocks/reddish/package.json", "{\"id\":\"rocks\",\"name\":\"Green\"}");
    create("overrides/rocks/reddish/assets/color.bin", "green");
    create("overrides/rocks/reddish/assets/sibling.bin", "sibling");
    create("overrides/map-0123456789abcdef/rocks/package.json", "{\"id\":\"rocks\",\"name\":\"Delivered\",\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0}}}");
    create("overrides/map-0123456789abcdef/rocks/assets/color.bin", "blue");
    create("overrides/legacy-a/package.json", "{\"id\":\"same.id\",\"name\":\"Old A\"}");
    create("overrides/legacy-a/assets/generated/decls/entitydef/actor/test.decl", "{ edit = { health = 10; speed = 1; } }");
    create("overrides/legacy-b/package.json", "{\"id\":\"other.id\",\"name\":\"Old B\"}");
    create("overrides/legacy-b/assets/generated/decls/entitydef/actor/test.decl", "{ edit = { health = 100; speed = 2; } }");
    create("overrides/new-variant/package.json", "{\"id\":\"same.id\",\"name\":\"New\",\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0}}}");
    create("overrides/new-variant/assets/generated/decls/entitydef/actor/test.decl", "{ edit = { health = 12000; } }");
    sources = sh_package_sources_scan(root, error, sizeof(error));
    if (!sources) fprintf(stderr, "%s\n", error);
    assert(sources);
    child = component(sources, "rocks", "red");
    provenance(sources, child);
    provenance(sources, component(sources, "legacy-a", ""));
    original_tree_identity(sources);
    composed(sources);
    assert(sh_package_scope_open(sources, child, &scope, error, sizeof(error)));
    assert(scope.sources.file_count == sources->file_count && scope.sources.package_count == sources->package_count);
    compiled = sh_package_compile(&scope.sources, NULL, NULL, error, sizeof(error)); assert(compiled);
    expect(compiled, "color.bin", "red"); expect(compiled, "shared.bin", "shared"); expect(compiled, "sibling.bin", NULL);
    assert(!compiled->policy.requirements.count);
    sh_package_compilation_free(compiled); sh_package_scope_close(&scope);
    assert(sh_package_scope_open(sources, component(sources, "rocks", ""), &scope, error, sizeof(error)));
    compiled = sh_package_compile(&scope.sources, NULL, NULL, error, sizeof(error)); assert(compiled);
    expect(compiled, "color.bin", "parent"); expect(compiled, "shared.bin", "shared"); expect(compiled, "sibling.bin", NULL);
    sh_package_compilation_free(compiled); sh_package_scope_close(&scope);
    /* Equal metadata IDs cannot suppress a separately delivered scope's policy. */
    assert(sh_package_scope_open(sources, component(sources, "map-0123456789abcdef/rocks", ""), &scope, error, sizeof(error)));
    compiled = sh_package_compile(&scope.sources, NULL, NULL, error, sizeof(error)); assert(compiled);
    expect(compiled, "color.bin", "blue"); expect(compiled, "shared.bin", NULL);
    assert(compiled->policy.requirements.count == 1);
    sh_package_compilation_free(compiled); sh_package_scope_close(&scope);
    for (size_t i = 0; i < sources->file_count; i++)
        if (!sources->files[i].directory) assert(sh_package_source_verify(&sources->files[i]));
    assert(!sh_package_scope_open(sources, sources->component_count, &scope, error, sizeof(error)));
    assert(!scope.sources.files && !scope.sources.components);
    sh_package_sources_free(sources); cleanup();
    puts("package scope tests passed"); return failures != 0;
}
