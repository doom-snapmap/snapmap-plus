/* Real source/compiler/delivery paths across multiple ownership words.
 * Native preparation is unavailable here; direct map roots remain sufficient. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_runtime.h"
#include "map_embed.h"
#include "package_archive.h"
#include "resource_graph_prepared.h"
#define PACKAGE_FIXTURE_ENTRIES 4096u
#include "package_fixture.h"

static sh_package_compilation *compiled;
static int acquired;
const sh_package_compilation *sh_package_runtime_acquire(void) { CHECK(!acquired); acquired = 1; return compiled; }
void sh_package_runtime_release(void) { CHECK(acquired); acquired = 0; }
const sh_resource_catalog *sh_package_runtime_catalog(void) { CHECK(acquired); return NULL; }
int sh_decl_server_registry_source(sh_decl_registry_source *source) { (void)source; CHECK(!acquired); return 0; }
void *sh_typeinfo_get_reflect(void) { CHECK(0); return NULL; }
sh_resource_graph_preparation *sh_resource_graph_prepare_open(sh_decl_registry_source source, uintptr_t reflect)
{ (void)source; (void)reflect; CHECK(0); return NULL; }
int sh_resource_graph_prepare_entity(sh_resource_graph_preparation *p, const char *name, sh_decl_dependency_result *result)
{ (void)p; (void)name; (void)result; CHECK(0); return 0; }
int sh_resource_graph_prepare_inline(sh_resource_graph_preparation *p, const char *class_name,
    const char *inherit, const char *json, size_t length,
    int (*visitor)(void *, const char *, const char *), void *context, sh_decl_dependency_result *result)
{ (void)p; (void)class_name; (void)inherit; (void)json; (void)length; (void)visitor; (void)context; (void)result; CHECK(0); return 0; }
void sh_resource_graph_prepare_close(sh_resource_graph_preparation *p) { (void)p; CHECK(0); }
void backend_log(const char *message) { (void)message; }

static int original(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    if (strcmp(path, "generated/decls/material/scale/composed.decl")) return 0;
    *body = (unsigned char *)_strdup("{ edit = {} }"); *length = strlen("{ edit = {} }");
    return *body ? 1 : -1;
}

int main(void)
{
    static const char all_map[] = "{\"inherit\":\"scale/shared\",\"targetType\":\"idDeclEntityDef\"}";
    static const char high_map[] = "{\"inherit\":\"scale/high\",\"targetType\":\"idDeclEntityDef\"}";
    sh_package_sources *sources = NULL;
    sh_package_owners owners = {0};
    sh_package_policy policy = {0};
    sh_mpkg_used *selected = NULL;
    char temp[MAX_PATH], path[512], body[512], error[2048];
    size_t i, count;
    CHECK(GetTempPathA(sizeof(temp), temp)); CHECK(GetTempFileNameA(temp, "psc", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    for (i = 0; i < 130; i++) {
        snprintf(path, sizeof(path), "overrides/p%03zu/package.json", i);
        /* Two independently authored roots intentionally share an ID. */
        snprintf(body, sizeof(body), "{\"id\":\"p%03zu\",\"name\":\"Package %zu\",\"strings\":{\"en\":{\"label_%zu\":\"Package %zu\"}}}", i == 1 ? 0 : i, i, i, i);
        create(path, body);
        snprintf(path, sizeof(path), "overrides/p%03zu/assets/generated/decls/%s/scale/shared.decl", i,
            i == 129 ? "snapeditorentitydef" : "entitydef");
        create(path, "{ edit = { health = 500; } }");
        if (i == 129) continue;
        snprintf(path, sizeof(path), "overrides/p%03zu/assets/generated/models/scale/shared.bmodel", i);
        create(path, "unchanged duplicate model");
        snprintf(path, sizeof(path), "overrides/p%03zu/assets/generated/decls/material/scale/composed.decl", i);
        snprintf(body, sizeof(body), "{ edit = { independent%zu = %zu; } }", i, i);
        create(path, body);
    }
    create("overrides/p128/assets/generated/decls/entitydef/scale/high.decl", "{ edit = { high = true; } }");
    create("overrides/p128/empty", NULL);
    create("overrides/p128/author-note.txt", "retain exact author data");
    create("overrides/p128/editor/package.json", "{\"id\":\"nested-tool\",\"name\":\"Nested tool\",\"strings\":{\"en\":{\"nested_label\":\"Nested tool\"}}}");
    sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(sources && sources->package_count == 130);
    if (!sources) goto done;
    compiled = sh_package_compile(sources, original, NULL, error, sizeof(error)); CHECK(compiled);
    if (!compiled) { fprintf(stderr, "%s\n", error); goto done; }
    {
        const sh_compiled_resource *r = sh_package_compilation_find(compiled, "generated/models/scale/shared.bmodel");
        CHECK(r && r->source_count == 129 && sh_package_owners_count(&r->owners) == 129 &&
            sh_package_owners_count(&r->gameplay_owners) == 129 && r->owners.more != r->gameplay_owners.more);
        r = sh_package_compilation_find(compiled, "generated/decls/material/scale/composed.decl");
        CHECK(r && r->composed && sh_package_owners_count(&r->owners) == 129);
        for (i = 0; r && i < 129; i++) {
            snprintf(body, sizeof(body), "independent%zu = %zu;", i, i);
            CHECK(strstr((char *)r->body, body));
        }
    }
    CHECK(sh_package_map_policy(compiled, NULL, high_map, sizeof(high_map) - 1, NULL,
        &policy, &owners, NULL, error, sizeof(error)));
    CHECK(sh_package_owners_count(&owners) == 1 && sh_package_owners_contains(&owners, 128) && !owners.bits);
    CHECK(strstr(sh_json_object_get(&policy.strings, "en"), "label_128") &&
        strstr(sh_json_object_get(&policy.strings, "en"), "nested_label") &&
        !strstr(sh_json_object_get(&policy.strings, "en"), "label_0"));
    sh_package_policy_free(&policy);
    count = sh_mpkg_used_packages(high_map, sizeof(high_map) - 1, root, &selected, error, sizeof(error));
    CHECK(!error[0]);
    sh_package_archive_delivery_id(sources->fingerprints[128], body);
    CHECK(count == 1 && selected && !strcmp(selected[0].id, body));
    if (count == 1) {
        size_t length;
        char id[SH_PACKAGE_ID_CAP];
        unsigned char fingerprint[32], *archive = sh_mpkg_pack_used(&selected[0], &length, error, sizeof(error));
        CHECK(archive);
        CHECK(archive && sh_package_archive_identity(archive, length, id, fingerprint, error, sizeof(error)) &&
            !strcmp(id, "p128") && !memcmp(fingerprint, sources->fingerprints[128], 32));
        if (archive) HeapFree(GetProcessHeap(), 0, archive);
    }
    count = sh_mpkg_used_packages(all_map, sizeof(all_map) - 1, root, &selected, error, sizeof(error));
    CHECK(count == 129);
    for (i = 0; count == 129 && i < count; i++) {
        sh_package_archive_delivery_id(sources->fingerprints[i], body); CHECK(!strcmp(selected[i].id, body));
    }
    CHECK(!strcmp(sources->components[0].descriptor.id, sources->components[1].descriptor.id));
    CHECK(strcmp(selected[0].id, selected[1].id));
    CHECK(sh_package_map_owners(compiled, NULL, all_map, sizeof(all_map) - 1, NULL, &owners, NULL));
    CHECK(sh_package_owners_count(&owners) == 129 && sh_package_owners_within(&owners, 129));
    CHECK(sh_package_owners_add(&owners, 4096));
    CHECK(!sh_package_compilation_policy(compiled, &owners, &policy, error, sizeof(error)) && error[0]);
    CHECK(sh_mpkg_used_packages("{", 1, root, &selected, error, sizeof(error)) == SIZE_MAX && !selected && !acquired);
    CHECK(strstr(error, "byte 1") && strstr(error, "JSON"));
    {
        sh_package_compilation *saved = compiled;
        compiled = NULL;
        CHECK(sh_mpkg_used_packages("{}", 2, root, &selected, error, sizeof(error)) == SIZE_MAX);
        CHECK(!selected && !acquired && strstr(error, "library is unavailable"));
        compiled = saved;
    }
    for (i = 0; i < sources->file_count; i++)
        if (!sources->files[i].directory) CHECK(sh_package_source_verify(&sources->files[i]));
    {
        sh_package_sources *conflicting_sources;
        sh_package_compilation *conflicting;
        sh_mpkg_used selected_before_edit;
        unsigned char *changed;
        size_t length;
        count = sh_mpkg_used_packages(high_map, sizeof(high_map) - 1, root, &selected, error, sizeof(error));
        CHECK(count == 1); selected_before_edit = selected[0];
        create("overrides/p128/assets/generated/decls/material/scale/composed.decl",
            "{ edit = { independent0 = 777; independent128 = 128; } }");
        changed = sh_mpkg_pack_used(&selected_before_edit, &length, error, sizeof(error));
        CHECK(!changed && !length && strstr(error, "changed after"));
        if (changed) HeapFree(GetProcessHeap(), 0, changed);
        conflicting_sources = sh_package_sources_scan(root, error, sizeof(error)); CHECK(conflicting_sources);
        conflicting = sh_package_compile(conflicting_sources, original, NULL, error, sizeof(error));
        CHECK(!conflicting && strstr(error, "p000") && strstr(error, "p128"));
        sh_package_compilation_free(conflicting); sh_package_sources_free(conflicting_sources);
    }
done:
    free(selected); sh_package_owners_free(&owners); sh_package_policy_free(&policy);
    sh_package_compilation_free(compiled); sh_package_sources_free(sources); cleanup();
    printf("package_scale_test: %d failures\n", failures); return failures ? 1 : 0;
}
