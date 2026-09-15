/* Map providers override local runtime values without changing authored files. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_compiler.h"
#include "package_fixture.h"

static const char *health_path = "generated/decls/entitydef/ai/cyberdemon.decl";
static const char *editor_path = "generated/decls/snapeditorentitydef/volume/test.decl";
static const char *health_original = "{ edit = { health = 25000; speed = 1; } }";
static const char *editor_original = "{ edit = { label = 1; } }";
static const char *opaque_paths[] = {
    "cooked/model/fixture.bmodel", "cooked/anim/fixture.banim",
    "generated/basemodel/fixture.bmd6model", "generated/skeleton/fixture.bskel",
    "generated/image/fixture.bimage", "generated/spirv/fixture.spv",
    "generated/renderprogs/fixture.bin", "soundbanks/fixture.bnk"
};
static const char *declaration_paths[] = {
    "generated/decls/material/fixture.decl", "generated/decls/md6def/fixture.decl",
    "generated/decls/sound/fixture.decl", "generated/decls/renderprog/fixture.decl"
};

static int baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, health_path) ? health_original :
        !strcmp(path, editor_path) ? editor_original : NULL;
    (void)context; *body = NULL; *length = 0;
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text);
    return *body ? 1 : -1;
}

static sh_package_compilation *compile(const char *folder, sh_package_sources **sources)
{
    char path[MAX_PATH], error[2048];
    sh_package_builtin builtin = {editor_path, (const unsigned char *)editor_original, strlen(editor_original)};
    sh_package_compile_environment environment = {0};
    sh_package_compilation *compiled;
    environment.baseline = baseline; environment.builtins = &builtin; environment.builtin_count = 1;
    snprintf(path, sizeof(path), "%s\\%s", root, folder);
    *sources = sh_package_sources_scan(path, error, sizeof(error)); CHECK(*sources);
    if (!*sources) { fprintf(stderr, "%s\n", error); return NULL; }
    compiled = sh_package_compile_with(*sources, &environment, error, sizeof(error));
    if (!compiled) fprintf(stderr, "%s\n", error);
    return compiled;
}

static void expect(const sh_package_compilation *compiled, const char *path, const char *text)
{
    char error[512]; size_t length = 0;
    const sh_compiled_resource *resource = sh_package_compilation_find(compiled, path);
    unsigned char *body;
    CHECK(resource); if (!resource) return;
    body = sh_package_compilation_read(compiled, resource, SIZE_MAX, &length, error, sizeof(error));
    CHECK(body && strstr((const char *)body, text)); free(body);
}

static void expect_string(const sh_package_policy *policy, const char *key, const char *expected)
{
    sh_json_object locale = {0};
    const char *raw = sh_json_object_get(&policy->strings, "english"), *actual;
    CHECK(raw && sh_json_parse_object(raw, strlen(raw), 16, &locale));
    actual = sh_json_object_get(&locale, key);
    CHECK(expected ? actual && !strcmp(actual, expected) : !actual);
    sh_json_object_free(&locale);
}

typedef struct availability { const sh_package_compilation *library; size_t calls; const char *broken; } availability;
static int available(void *context, const char *path, char *error, size_t capacity)
{
    availability *state = context; state->calls++;
    if (state->broken && !strcmp(state->broken, path)) {
        snprintf(error, capacity, "fixture unreadable resource: %s", path); return -1;
    }
    return sh_package_compilation_probe(state->library, path, error, capacity);
}

static void resource_availability(const sh_package_compilation *map, const sh_package_compilation *local)
{
    const char *required[] = {health_path, "MODEL.BMODEL", "model.bmodel"};
    const char *missing_path[] = {"support.bin"};
    const char *bad[] = {"support.bin", "zz-not-delivered.bimage"};
    const char *invalid[] = {"support.bin", "../bad"};
    sh_package_missing missing = {0};
    availability state = {local, 0};
    char error[1024];
    /* Full payload coverage is a sufficient no-install proof. Unlike a
     * resolved gameplay closure it also reports unused supplied files. The
     * pure product editor default has no authored map source and is skipped. */
    CHECK(sh_package_compilation_payload_missing(map, available, &state, &missing, error, sizeof(error)));
    CHECK(missing.count == 2 && !strcmp(missing.paths[0], "hell-guard.bmodel") &&
        !strcmp(missing.paths[1], "support.bin") && sh_package_owners_count(&missing.packages) == 2);
    CHECK(missing.checked == map->resource_count - 1);
    state.broken = "model.bmodel";
    CHECK(!sh_package_compilation_payload_missing(map, available, &state, &missing, error, sizeof(error)));
    CHECK(strstr(error, "fixture unreadable") && !missing.count && !missing.paths);
    CHECK(!sh_package_compilation_payload_missing(NULL, available, &state, &missing, NULL, 0));
    CHECK(!missing.count && !missing.checked);
    state.broken = NULL; state.calls = 0;
    CHECK(sh_package_compilation_missing(map, required, 3, available, &state, &missing, error, sizeof(error)));
    CHECK(!missing.count && missing.checked == 2 && state.calls == 2);
    CHECK(!sh_package_owners_count(&missing.packages));
    /* An unused Hell Guard asset in this same delivery bundle is not a
     * requirement merely because the client lacks it. */
    CHECK(sh_package_compilation_probe(local, "hell-guard.bmodel", error, sizeof(error)) == 0);
    CHECK(sh_package_compilation_missing(map, opaque_paths, sizeof(opaque_paths) / sizeof(*opaque_paths),
        available, &state, &missing, error, sizeof(error)) && !missing.count);
    CHECK(sh_package_compilation_missing(map, declaration_paths, sizeof(declaration_paths) / sizeof(*declaration_paths),
        available, &state, &missing, error, sizeof(error)) && !missing.count);
    CHECK(sh_package_compilation_missing(map, missing_path, 1, available, &state, &missing, error, sizeof(error)));
    CHECK(missing.count == 1 && !strcmp(missing.paths[0], "support.bin") && sh_package_owners_count(&missing.packages) == 1);
    const sh_compiled_resource *support = sh_package_compilation_find(map, "support.bin"); CHECK(support);
    if (support) CHECK(sh_package_owners_contains(&missing.packages, map->sources->files[support->sources[0]].owner));
    /* No available prefix or unrelated installation offer survives failure. */
    CHECK(!sh_package_compilation_missing(map, bad, 2, available, &state, &missing, error, sizeof(error)));
    CHECK(strstr(error, "zz-not-delivered.bimage") && !missing.count && !missing.paths && !missing.checked);
    CHECK(!sh_package_owners_count(&missing.packages));
    CHECK(!sh_package_compilation_missing(map, invalid, 2, available, &state, &missing, error, sizeof(error)));
    state.broken = "model.bmodel";
    CHECK(!sh_package_compilation_missing(map, required, 3, available, &state, &missing, error, sizeof(error)));
    CHECK(strstr(error, "fixture unreadable") && !missing.count && !sh_package_owners_count(&missing.packages));
    state.broken = NULL; state.library = NULL;
    CHECK(sh_package_compilation_missing(map, required, 3, available, &state, &missing, error, sizeof(error)));
    CHECK(missing.count == 2 && sh_package_owners_count(&missing.packages) == 2); /* Both intact duplicates. */
    state.library = local;
    CHECK(sh_package_compilation_missing(map, (const char *const *)missing.paths, missing.count,
        available, &state, &missing, error, sizeof(error)));
    CHECK(missing.checked == 2 && !missing.count); /* Recheck the earlier owned result safely. */
    {
        size_t count = 131073;
        const char **repeated = malloc(count * sizeof(*repeated)); CHECK(repeated);
        if (repeated) {
            for (size_t i = 0; i < count; i++) repeated[i] = required[i % 3];
            state.calls = 0;
            CHECK(sh_package_compilation_missing(map, repeated, count, available, &state, &missing, error, sizeof(error)));
            CHECK(missing.checked == 2 && state.calls == 2 && !missing.count); free(repeated);
        }
    }
    CHECK(sh_package_compilation_missing(map, NULL, 0, available, &state, &missing, NULL, 0));
    CHECK(!missing.count && !missing.checked);
    sh_package_missing_free(&missing); sh_package_missing_free(&missing);
    {
        /* Internal provenance is not limited to one 64-bit owner word. */
        size_t file_index = 0;
        sh_package_source_file file = {0}; file.owner = 71;
        sh_package_sources sources = {0}; sources.files = &file; sources.file_count = 1; sources.package_count = 72;
        sh_compiled_resource resource = {0}; resource.engine_path = "support.bin";
        resource.sources = &file_index; resource.source_count = 1;
        sh_package_compilation many = {0}; many.sources = &sources; many.resources = &resource; many.resource_count = 1;
        CHECK(sh_package_compilation_missing(&many, missing_path, 1, available, &state, &missing, error, sizeof(error)));
        CHECK(missing.count == 1 && sh_package_owners_contains(&missing.packages, 71));
        CHECK(sh_package_owners_count(&missing.packages) == 1); sh_package_missing_free(&missing);
    }
}

int main(void)
{
    char temporary[MAX_PATH], error[2048], path[256], descriptor[160];
    sh_package_sources *local_sources = NULL, *map_sources = NULL;
    sh_package_compilation *local, *map, *view = NULL;
    const sh_compiled_resource *health;
    sh_package_owners selected = {0}; sh_package_policy policy = {0};
    size_t local_count, local_files, map_files, i;
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "pcm", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    create("library/overrides/cyber/package.json",
        "{\"id\":\"cyber\",\"name\":\"Local Cyberdemon\",\"strings\":{\"english\":{\"shared\":\"local\",\"localOnly\":\"kept\"}},"
        "\"hud\":{\"weapon\":{\"icon\":\"local\",\"scale\":2}}}");
    create("library/overrides/cyber/assets/generated/decls/entitydef/ai/cyberdemon.decl",
        "{ edit = { health = 12000; speed = 1; } }");
    create("library/overrides/cyber/assets/generated/decls/snapeditorentitydef/volume/test.decl",
        "{ edit = { label = 2; } }");
    create("library/overrides/cyber/assets/local-only.bin", "local-only resource");
    create("library/overrides/cyber/assets/model.bmodel", "local model");
    create("library/overrides/cyber/empty-author-folder", NULL);
    /* Crossing a provenance word boundary must not reinterpret map owners. */
    for (i = 0; i < 70; i++) {
        snprintf(path, sizeof(path), "library/overrides/extra-%02zu/package.json", i);
        snprintf(descriptor, sizeof(descriptor), "{\"id\":\"extra-%02zu\",\"name\":\"Extra %zu\"}", i, i);
        create(path, descriptor);
    }
    create("mapdata/overrides/cyber/package.json",
        "{\"id\":\"cyber\",\"name\":\"Map Cyberdemon\",\"strings\":{\"english\":{\"shared\":\"map\",\"mapOnly\":\"added\"}},"
        "\"hud\":{\"weapon\":{\"icon\":\"map\"}}}");
    create("mapdata/overrides/cyber/assets/generated/decls/entitydef/ai/cyberdemon.decl",
        "{ edit = { health = 10; speed = 1; } }");
    create("mapdata/overrides/cyber/assets/model.bmodel", "map model");
    create("mapdata/overrides/cyber/empty-map-folder", NULL);
    create("mapdata/overrides/cyber/support/package.json",
        "{\"id\":\"map-support\",\"name\":\"Map Support\",\"strings\":{\"english\":{\"nested\":\"carried\"}}}");
    create("mapdata/overrides/cyber/support/assets/support.bin", "nested component bytes");
    create("mapdata/overrides/cyber/support/empty-component-folder", NULL);
    create("mapdata/overrides/boss/package.json", "{\"id\":\"boss-demons\",\"name\":\"Boss Demons\"}");
    create("mapdata/overrides/boss/assets/generated/decls/entitydef/ai/cyberdemon.decl", health_original);
    create("mapdata/overrides/boss/assets/model.bmodel", "map model");
    create("mapdata/overrides/boss/assets/hell-guard.bmodel", "unused boss bytes");
    for (i = 0; i < sizeof(opaque_paths) / sizeof(*opaque_paths); i++) {
        snprintf(path, sizeof(path), "library/overrides/cyber/assets/%s", opaque_paths[i]); create(path, "local resource bytes");
        snprintf(path, sizeof(path), "mapdata/overrides/cyber/assets/%s", opaque_paths[i]); create(path, "map resource bytes");
    }
    for (i = 0; i < sizeof(declaration_paths) / sizeof(*declaration_paths); i++) {
        snprintf(path, sizeof(path), "library/overrides/cyber/assets/%s", declaration_paths[i]); create(path, "{ edit = { value = 12000; } }");
        snprintf(path, sizeof(path), "mapdata/overrides/cyber/assets/%s", declaration_paths[i]); create(path, "{ edit = { value = 10; } }");
    }
    local = compile("library", &local_sources); map = compile("mapdata", &map_sources);
    CHECK(local && map); if (!local || !map) goto done;
    resource_availability(map, local);
    local_count = local_sources->package_count; local_files = local_sources->file_count; map_files = map_sources->file_count;
    CHECK(local_count == 71);
    view = sh_package_compilation_overlay(local, map, error, sizeof(error));
    if (!view) fprintf(stderr, "%s\n", error);
    CHECK(view); if (!view) goto done;
    CHECK(view->sources->package_count == local_count + map_sources->package_count);
    CHECK(view->sources->file_count == local_files + map_files);
    CHECK(view->sources->component_count == local_sources->component_count + map_sources->component_count);
    CHECK(view->map_overlay && view->owns_sources && view->map_owner_begin == local_count);
    CHECK(view->duplicate_count == 1); /* Map's complete duplicated model remains represented. */
    expect(local, health_path, "health = 12000"); expect(map, health_path, "health = 10");
    expect(view, health_path, "health = 10");
    expect(view, editor_path, "label = 2"); /* Map's pure built-in cannot hide a local editor mod. */
    expect(view, "local-only.bin", "local-only resource"); expect(view, "model.bmodel", "map model");
    expect(view, "support.bin", "nested component bytes");
    expect_string(&view->policy, "shared", "\"map\"");
    expect_string(&view->policy, "localOnly", "\"kept\"");
    expect_string(&view->policy, "mapOnly", "\"added\"");
    expect_string(&view->policy, "nested", "\"carried\"");
    {
        sh_json_object weapon = {0};
        const char *raw = sh_json_object_get(&view->policy.hud, "weapon"), *icon, *scale;
        CHECK(raw && sh_json_parse_object(raw, strlen(raw), 16, &weapon));
        icon = sh_json_object_get(&weapon, "icon"); scale = sh_json_object_get(&weapon, "scale");
        CHECK(icon && !strcmp(icon, "\"map\"")); CHECK(scale && !strcmp(scale, "2"));
        sh_json_object_free(&weapon);
    }
    for (i = 0; i < sizeof(opaque_paths) / sizeof(*opaque_paths); i++) {
        expect(view, opaque_paths[i], "map resource bytes"); expect(local, opaque_paths[i], "local resource bytes");
        CHECK(sh_package_compilation_probe(local, opaque_paths[i], error, sizeof(error)) == 1);
    }
    for (i = 0; i < sizeof(declaration_paths) / sizeof(*declaration_paths); i++) {
        expect(view, declaration_paths[i], "value = 10"); expect(local, declaration_paths[i], "value = 12000");
        CHECK(sh_package_compilation_probe(local, declaration_paths[i], error, sizeof(error)) == 1);
    }
    CHECK(sh_package_compilation_probe(local, "generated/decls/EntityDef/ai/CYBERDEMON.decl", error, sizeof(error)) == 1);
    CHECK(sh_package_compilation_probe(local, "not-installed.bimage", error, sizeof(error)) == 0);
    CHECK(sh_package_compilation_probe(local, "../invalid.bimage", error, sizeof(error)) == -1);
    create("library/overrides/cyber/assets/local-only.bin", "changed after discovery by test");
    CHECK(sh_package_compilation_probe(local, "local-only.bin", error, sizeof(error)) == -1);
    create("library/overrides/cyber/assets/local-only.bin", "local-only resource");
    CHECK(sh_package_compilation_probe(local, "local-only.bin", error, sizeof(error)) == 1);
    {
        sh_compiled_resource *cached = (sh_compiled_resource *)sh_package_compilation_find(local, "model.bmodel");
        char cache[MAX_PATH];
        CHECK(cached);
        if (cached) {
            create("immutable/model.bin", "local model");
            snprintf(cache, sizeof(cache), "%s\\immutable\\model.bin", root);
            cached->cache_path = _strdup(cache); CHECK(cached->cache_path);
            create("library/overrides/cyber/assets/model.bmodel", "edited after cache capture by test");
            CHECK(sh_package_compilation_probe(local, "model.bmodel", error, sizeof(error)) == 1);
            create("immutable/model.bin", "corrupt cache bytes");
            CHECK(sh_package_compilation_probe(local, "model.bmodel", error, sizeof(error)) == -1);
            create("immutable/model.bin", "local model");
            CHECK(sh_package_compilation_probe(local, "model.bmodel", error, sizeof(error)) == 1);
            create("library/overrides/cyber/assets/model.bmodel", "local model");
        }
    }
    health = sh_package_compilation_find(view, health_path);
    CHECK(health && health->source_count == 2 && sh_package_owners_count(&health->gameplay_owners) == 1);
    if (health) {
        for (i = 0; i < local_count; i++) CHECK(!sh_package_owners_contains(&health->gameplay_owners, i));
        CHECK(sh_package_owners_union(&selected, &health->gameplay_owners));
        CHECK(sh_package_compilation_policy(view, &selected, &policy, error, sizeof(error)));
        expect_string(&policy, "shared", "\"map\"");
        expect_string(&policy, "localOnly", NULL);
        expect_string(&policy, "nested", "\"carried\"");
        sh_package_policy_free(&policy); sh_package_owners_free(&selected);
    }
    for (i = 0; i < view->sources->file_count; i++) if (!view->sources->files[i].directory)
        CHECK(sh_package_source_verify(&view->sources->files[i]));
    CHECK(!memcmp(view->sources->fingerprints, local_sources->fingerprints, local_count * 32));
    CHECK(!memcmp(view->sources->fingerprints + local_count, map_sources->fingerprints, map_sources->package_count * 32));
    CHECK(!sh_package_compilation_overlay(view, map, error, sizeof(error))); /* No accidental context stacking. */
    /* The result owns copies. Releasing inputs must not retire active resources. */
    sh_package_compilation_free(local); local = NULL; sh_package_sources_free(local_sources); local_sources = NULL;
    sh_package_compilation_free(map); map = NULL; sh_package_sources_free(map_sources); map_sources = NULL;
    expect(view, health_path, "health = 10"); expect(view, "model.bmodel", "map model");
    /* Recompiling the local library still finds its untouched authored values. */
    local = compile("library", &local_sources); CHECK(local); if (local) expect(local, health_path, "health = 12000");
    /* Contradictory edits by map peers still fail before creating a provider. */
    create("mapdata/overrides/boss/assets/generated/decls/entitydef/ai/cyberdemon.decl",
        "{ edit = { health = 20; speed = 1; } }");
    map = compile("mapdata", &map_sources); CHECK(!map);
    expect(local, health_path, "health = 12000"); expect(view, health_path, "health = 10");
    {
        sh_package_sources *alternate_sources = NULL;
        sh_package_compilation *alternate;
        create("alternate/overrides/boss/package.json", "{\"id\":\"another-boss-package\",\"name\":\"Another Boss Package\"}");
        create("alternate/overrides/boss/assets/generated/decls/entitydef/ai/cyberdemon.decl", health_original);
        create("alternate/overrides/boss/assets/model.bmodel", "another model");
        alternate = compile("alternate", &alternate_sources); CHECK(alternate);
        CHECK(sh_package_compilation_probe(alternate, health_path, error, sizeof(error)) == 1);
        CHECK(sh_package_compilation_probe(alternate, "model.bmodel", error, sizeof(error)) == 1);
        CHECK(sh_package_compilation_probe(alternate, "generated/image/fixture.bimage", error, sizeof(error)) == 0);
        sh_package_compilation_free(alternate); sh_package_sources_free(alternate_sources);
    }
done:
    sh_package_compilation_free(view); sh_package_compilation_free(local); sh_package_compilation_free(map);
    sh_package_sources_free(local_sources); sh_package_sources_free(map_sources);
    sh_package_policy_free(&policy); sh_package_owners_free(&selected);
    cleanup(); if (failures) return 1;
    puts("package map context tests passed"); return 0;
}
