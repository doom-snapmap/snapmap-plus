/* Exercise the actual provider with complete compiled packages. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "overrides.h"
#include "package_runtime.h"
#include "resource_graph.h"
#include "package_fixture.h"
#include "../src/backend/overrides_baked.h"

/* Stub unrelated override services and enable the user layer for lookup tests. */
void backend_log(const char *message)
{
    (void)message;
}

static int user_layer_enabled = 1;
int sh_user_overrides_enabled_for_launch(void)
{
    return user_layer_enabled;
}




/* Baked navigation is a different subsystem with its own tests; the file shadow
 * only has to ask it first. */
#include "../src/backend/nav_bake.h"

/* Navigation baked from a map's marked volumes. No map is loaded in this test,
 * so the hook must fall through to the shadow exactly as it does in game. */
static const char *bake_test_source;
static unsigned source_invalidations;
static int source_update_active;
static const sh_package_compilation *source_update_before;
void sh_nav_bake_source_update_begin(void)
{
    CHECK(!source_update_active); source_update_active = 1;
    source_update_before = sh_package_runtime_acquire(); sh_package_runtime_release();
}
void sh_nav_bake_source_update_end(int committed)
{
    const sh_package_compilation *current = sh_package_runtime_acquire();
    CHECK(source_update_active);
    CHECK(committed ? current != source_update_before : current == source_update_before);
    sh_package_runtime_release(); source_update_active = 0;
    source_invalidations += committed != 0;
}
int sh_nav_bake_open(const char *name, sh_nav_bake_reader read_shipped,
                     unsigned char **out_bytes, size_t *out_len)
{
    if (out_bytes) *out_bytes = NULL;
    if (out_len) *out_len = 0;
    if (bake_test_source && name && !strcmp(name, "test/marked/marked.aas_monster48")) {
        *out_bytes = read_shipped(bake_test_source, out_len);
        return *out_bytes != NULL;
    }
    return 0;
}

int sh_navmesh_open(const char *name, unsigned char **out_bytes, size_t *out_len)
{
    (void)name;
    if (out_bytes) *out_bytes = NULL;
    if (out_len) *out_len = 0;
    return 0;
}


static int matches(const char *name, const char *body)
{
    char bytes[512] = {0};
    void *stream = sh_overrides_test_open(name, 0);
    int ok = stream && sh_overrides_test_stream_length(stream) == (long long)strlen(body) &&
        sh_overrides_test_stream_read(stream, bytes, sizeof bytes) == (long long)strlen(body) &&
        !memcmp(bytes, body, strlen(body));
    if (stream) sh_overrides_test_stream_close(stream);
    return ok;
}
static DWORD WINAPI reader(LPVOID context)
{
    volatile LONG *errors = context;
    for (int i = 0; i < 100; i++)
        if (!matches("generated/spirv/shock.vspv", "shader bytes")) InterlockedIncrement(errors);
    return 0;
}

static int original_decl(void *context, const char *path, unsigned char **body, size_t *length)
{
    *body = NULL; *length = 0;
    if (strcmp(path, "generated/decls/material/test/original.decl")) return 0;
    if (*(int *)context) return -1;
    *body = (unsigned char *)_strdup("{ value = 0; }");
    *length = strlen("{ value = 0; }");
    return *body ? 1 : -1;
}

typedef struct first_alias_activation {
    int fail, raise, recoveries;
    void *held;
} first_alias_activation;

static int first_alias_callback(void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    CHECK(changes);
    first_alias_activation *test = (first_alias_activation *)context;
    const char *alias = "decltree/entitydef/ai/cyber.decl";
    const char *text = "{ class = \"idAI\"; }";
    unsigned char *body = NULL;
    size_t length = 0;
    if (restoring) {
        const sh_package_compilation *compiled = sh_package_runtime_acquire();
        CHECK(!compiled); sh_package_runtime_release();
        test->recoveries++;
        CHECK(!sh_package_runtime_decl_alias_exists(alias));
        CHECK(sh_package_runtime_read_decl_alias(alias, &body, &length) == -1);
        CHECK(!body && !length);
        CHECK(!sh_overrides_internal_decl_published(alias));
        CHECK(!sh_overrides_test_open(alias, 0));
        return 1;
    }
    {
        sh_overrides_internal_decl_entry entry = {"entitydef", "ai/cyber", (const unsigned char *)text, strlen(text)};
        CHECK(sh_overrides_internal_decl_published_count() ?
            sh_overrides_test_internal_decl_table_merge(&entry, 1) :
            sh_overrides_test_internal_decl_table_install(&entry, 1));
    }
    CHECK(!sh_package_runtime_ready());
    CHECK(sh_package_runtime_decl_alias_exists(alias));
    CHECK(sh_overrides_internal_decl_published("decltree/entityDef/AI/CYBER.decl"));
    CHECK(matches(alias, text));
    test->held = sh_overrides_test_open(alias, 0); CHECK(test->held);
    if (test->raise) RaiseException(0xe04d504b, 0, 0, NULL);
    if (test->fail) {
        snprintf(error, capacity, "first alias activation refused");
        return 0;
    }
    return 1;
}

static void test_first_alias_activation(void)
{
    first_alias_activation test = {0};
    for (int mode = 0; mode < 3; mode++) {
        unsigned long result;
        char bytes[64] = {0};
        test.fail = mode != 2; test.raise = mode == 1;
        result = sh_overrides_rescan_packages_activated(first_alias_callback, &test);
        CHECK(result == (mode == 2 ? 2 : SH_OVERRIDES_RESCAN_FAILED));
        CHECK(test.recoveries == (mode == 0 ? 1 : 2));
        CHECK(sh_package_runtime_ready() == (mode == 2));
        CHECK(sh_overrides_internal_decl_published("decltree/entitydef/ai/cyber.decl") == (mode == 2));
        /* Already-open streams own stable bytes even after provider recovery. */
        if (test.held) {
            CHECK(sh_overrides_test_stream_read(test.held, bytes, sizeof(bytes)) == strlen("{ class = \"idAI\"; }"));
            CHECK(!strcmp(bytes, "{ class = \"idAI\"; }"));
            sh_overrides_test_stream_close(test.held); test.held = NULL;
        }
    }
    sh_overrides_test_internal_decl_table_reset();
}

typedef struct activation_test {
    const sh_package_compilation *previous;
    int calls, recoveries, fail, raise, recovery_fail, recovery_raise;
    int restoring;
} activation_test;

static DWORD WINAPI activation_reader(LPVOID context)
{
    activation_test *test = (activation_test *)context;
    const sh_package_compilation *compiled = sh_package_runtime_acquire();
    const char *expected = test->restoring ? "previous" : "candidate";
    char *policy;
    size_t length;
    int valid = compiled && (test->restoring ? compiled == test->previous : compiled != test->previous);
    sh_package_runtime_release();
    valid = valid && !sh_package_runtime_ready() && !source_update_active;
    valid = valid && matches("generated/image/demon.bimage", expected);
    valid = valid && matches("generated/decls/entitydef/ai/cyber.decl",
        test->restoring ? "{ class = \"idAI\"; health = 100; }" : "{ class = \"idAI\"; health = 200; }");
    policy = sh_package_runtime_policy("strings", &length);
    valid = valid && policy && strstr(policy, expected); free(policy);
    policy = sh_package_runtime_active_policy("strings", &length);
    valid = valid && policy && strstr(policy, expected); free(policy);
    return valid ? 0 : 1;
}

static int activation_callback(void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    CHECK(changes);
    activation_test *test = (activation_test *)context;
    HANDLE thread;
    DWORD result = 1;
    char map_error[256];
    test->restoring = restoring;
    if (restoring) test->recoveries++; else test->calls++;
    /* A reader on another thread must be able to use the provisional provider:
     * no native callback may run under its lock or derived-source guard. */
    thread = CreateThread(NULL, 0, activation_reader, test, 0, NULL);
    CHECK(thread);
    if (thread) {
        CHECK(WaitForSingleObject(thread, 30000) == WAIT_OBJECT_0);
        CHECK(GetExitCodeThread(thread, &result)); CHECK(!result); CloseHandle(thread);
    }
    CHECK(!sh_package_runtime_select_map(NULL, 0, NULL, map_error, sizeof(map_error)));
    CHECK(strstr(map_error, "activation is in progress"));
    if ((!restoring && test->raise) || (restoring && test->recovery_raise))
        RaiseException(0xe04d504b, 0, 0, NULL);
    if (restoring ? test->recovery_fail : test->fail) {
        snprintf(error, capacity, "fixture %s refused", restoring ? "recovery" : "activation");
        return 0;
    }
    return 1;
}

static void test_activation_rollback(void)
{
    const char *descriptor = "overrides/cyberdemon/package.json";
    const char *image_path = "overrides/cyberdemon/assets/generated/image/demon.bimage";
    const char *decl_path = "overrides/cyberdemon/assets/generated/decls/entitydef/ai/cyber.decl";
    sh_package_references refs = {0};
    activation_test test = {0};
    char error[2048], bytes[32] = {0};
    void *stream;
    unsigned invalidations;
    create(descriptor, "{\"id\":\"cyberdemon\",\"name\":\"Cyberdemon\",\"strings\":{\"en\":{\"#str_activation\":\"previous\"}}}");
    create(image_path, "previous");
    create(decl_path, "{ class = \"idAI\"; health = 100; }");
    CHECK(sh_overrides_rescan_packages() == 4);
    CHECK(sh_package_references_add(&refs, "", "generated/image/demon.bimage"));
    CHECK(sh_package_runtime_select_map("{}", 2, &refs, error, sizeof(error)));
    sh_package_references_free(&refs);
    test.previous = sh_package_runtime_acquire(); sh_package_runtime_release();
    stream = sh_overrides_test_open("generated/image/demon.bimage", 0); CHECK(stream);
    create(descriptor, "{\"id\":\"cyberdemon\",\"name\":\"Cyberdemon\",\"strings\":{\"en\":{\"#str_activation\":\"candidate\"}}}");
    create(image_path, "candidate");
    create(decl_path, "{ class = \"idAI\"; health = 200; }");
    /* An operational read failure must not expose the candidate or call
     * consumers. Unlike a content conflict, it cannot isolate an owner. */
    invalidations = source_invalidations;
    {
        char locked_path[4096];
        snprintf(locked_path, sizeof(locked_path), "%s/%s", root, image_path);
        HANDLE locked = CreateFileA(locked_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(locked != INVALID_HANDLE_VALUE);
        CHECK(sh_overrides_rescan_packages_activated(activation_callback, &test) == SH_OVERRIDES_RESCAN_FAILED);
        CHECK(!test.calls && !test.recoveries && source_invalidations == invalidations);
        CHECK(matches("generated/image/demon.bimage", "previous"));
        if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    }
    for (int mode = 0; mode < 4; mode++) {
        const sh_package_compilation *current;
        invalidations = source_invalidations;
        test.calls = test.recoveries = 0;
        test.fail = 1; test.raise = mode == 1;
        test.recovery_fail = mode == 2; test.recovery_raise = mode == 3;
        CHECK(sh_overrides_rescan_packages_activated(activation_callback, &test) == SH_OVERRIDES_RESCAN_FAILED);
        CHECK(test.calls == 1 && test.recoveries == 1);
        CHECK(!sh_package_runtime_ready() && !source_update_active);
        CHECK(source_invalidations == invalidations + 2);
        current = sh_package_runtime_acquire(); CHECK(current == test.previous);
        sh_package_runtime_release();
        CHECK(matches("generated/image/demon.bimage", "previous"));
        sh_package_runtime_error(error, sizeof(error));
        CHECK(mode == 1 ? strstr(error, "exception") != NULL : strstr(error, "fixture activation refused") != NULL);
        CHECK((strstr(error, "consumer recovery failed") != NULL) == (mode >= 2));
    }
    test.calls = test.recoveries = test.fail = test.raise = test.recovery_fail = test.recovery_raise = 0;
    invalidations = source_invalidations;
    CHECK(sh_overrides_rescan_packages_activated(activation_callback, &test) == 4);
    CHECK(test.calls == 1 && !test.recoveries && sh_package_runtime_ready());
    CHECK(source_invalidations == invalidations + 1 && !source_update_active);
    CHECK(matches("generated/image/demon.bimage", "candidate"));
    sh_package_runtime_error(error, sizeof(error)); CHECK(!error[0]);
    if (stream) {
        CHECK(sh_overrides_test_stream_read(stream, bytes, sizeof(bytes)) == strlen("previous"));
        CHECK(!strcmp(bytes, "previous")); sh_overrides_test_stream_close(stream);
    }
    CHECK(sh_package_runtime_select_map(NULL, 0, NULL, error, sizeof(error)));
    create(descriptor, "{\"id\":\"cyberdemon\",\"name\":\"Cyberdemon\"}");
    create(image_path, "updated image bytes");
    create(decl_path, "{ class = \"idAI\"; }");
    CHECK(sh_overrides_rescan_packages() == 4);
}

static void test_all_builtin_defaults(void)
{
    size_t i;
    char relative[4096], installed[4096], retired[4096];
    create("overrides/builtin-a/package.json", "{\"id\":\"builtin-a\",\"name\":\"Built-in A\"}");
    create("overrides/builtin-b/package.json", "{\"id\":\"builtin-b\",\"name\":\"Built-in B\"}");
    for (i = 0; i < sizeof(g_ov_baked_decls) / sizeof(g_ov_baked_decls[0]); i++) {
        const ov_baked_decl_t *value = &g_ov_baked_decls[i];
        /* Synthetic independent metadata additions exercise complete copies
         * of every real built-in identity against the product-only baseline. */
        char *body = (char *)malloc((size_t)value->len + 64);
        size_t end = value->len;
        CHECK(body); if (!body) continue;
        while (end && value->text[end - 1] != '}') end--;
        CHECK(end); if (!end) { free(body); continue; }
        memcpy(body, value->text, end - 1);
        strcpy(body + end - 1, " testMarkerA = true; }");
        snprintf(relative, sizeof(relative), "overrides/builtin-a/assets/%s", value->name);
        create(relative, body);
        strcpy(body + end - 1, " testMarkerB = true; }");
        snprintf(relative, sizeof(relative), "overrides/builtin-b/assets/%s", value->name);
        create(relative, body); free(body);
    }
    {
        size_t count = sh_overrides_rescan_packages();
        if (count != 4) {
            char error[2048]; sh_package_runtime_error(error, sizeof(error));
            fprintf(stderr, "built-in compilation: %s\n", error);
        }
        CHECK(count == 4);
    }
    {
        const sh_package_compilation *compiled = sh_package_runtime_acquire();
        for (i = 0; i < sizeof(g_ov_baked_decls) / sizeof(g_ov_baked_decls[0]); i++) {
            const sh_compiled_resource *resource = sh_package_compilation_find(compiled, g_ov_baked_decls[i].name);
            CHECK(resource && resource->baseline_known == 3 && resource->source_count == 2);
            CHECK(resource && strstr((char *)resource->body, "testMarkerA") && strstr((char *)resource->body, "testMarkerB"));
        }
        sh_package_runtime_release();
    }
    for (i = 0; i < 2; i++) {
        snprintf(installed, sizeof(installed), "%s/overrides/builtin-%c", root, (int)('a' + i));
        snprintf(retired, sizeof(retired), "%s/retired-builtin-%c", root, (int)('a' + i));
        CHECK(MoveFileA(installed, retired));
    }
    CHECK(sh_overrides_rescan_packages() == 2);
    {
        const sh_package_compilation *compiled = sh_package_runtime_acquire();
        for (i = 0; i < sizeof(g_ov_baked_decls) / sizeof(g_ov_baked_decls[0]); i++) {
            const ov_baked_decl_t *value = &g_ov_baked_decls[i];
            const sh_compiled_resource *resource = sh_package_compilation_find(compiled, value->name);
            CHECK(resource && resource->baseline_known == 3 && resource->restored_original);
            CHECK(resource && !sh_package_owners_count(&resource->owners) && !sh_package_owners_count(&resource->gameplay_owners) && !resource->source_count);
            CHECK(resource && resource->body_length == value->len && !memcmp(resource->body, value->text, value->len));
        }
        sh_package_runtime_release();
    }
    /* The fixture tracks authored paths for cleanup; restore that layout. */
    for (i = 0; i < 2; i++) {
        snprintf(installed, sizeof(installed), "%s/overrides/builtin-%c", root, (int)('a' + i));
        snprintf(retired, sizeof(retired), "%s/retired-builtin-%c", root, (int)('a' + i));
        CHECK(MoveFileA(retired, installed));
    }
}

static int transform_reference(void *context, const char *parent_type, const char *parent_name,
    const char *type, const char *name)
{
    int *found = context;
    (void)parent_type; (void)parent_name;
    if (!*type && !strcmp(name, "maps/modules/ind_dlc/ind_totally_blank_room_4x/lightprobes/light_probe_1_compressed.bimage"))
        *found = 1;
    return 1;
}

static void test_transform_sources(void)
{
    const char *input = "maps/modules/ind_dlc/ind_totally_blank_room_4x/lightprobes/light_probe_1_compressed.bimage";
    const char *derived = "maps/modules/smpgrid/v1/m/1536_1024_512/lightprobes/light_probe_1_compressed.bimage";
    char path[4096], cache[4096];
    unsigned char *held, *current;
    size_t held_length, length;
    const sh_package_compilation *compiled;
    const sh_compiled_resource *resource;
    snprintf(path, sizeof(path), "overrides/cyberdemon/assets/%s", input);
    create(path, "package source one");
    CHECK(sh_overrides_rescan_packages() == 2);
    CHECK(matches(derived, "package source one"));
    {
        int found = 0;
        /* Preview-like reads outside a native scope still retain the Grid
         * output's source path, so a later map can identify package owners. */
        CHECK(sh_resource_graph_walk("", derived, transform_reference, &found));
        CHECK(found);
        {
            sh_package_references references = {0};
            sh_package_owners owners = {0};
            CHECK(sh_package_references_add(&references, "", derived));
            compiled = sh_package_runtime_acquire();
            resource = sh_package_compilation_find(compiled, input);
            CHECK(sh_package_map_owners(compiled, NULL, "{}", 2, &references, &owners, NULL));
            CHECK(resource && sh_package_owners_count(&owners) == 1 &&
                owners.bits == resource->gameplay_owners.bits);
            sh_package_runtime_release();
            sh_package_references_free(&references); sh_package_owners_free(&owners);
        }
    }
    held = sh_overrides_read_engine_resource(derived, &held_length);
    CHECK(held && held_length == strlen("package source one") && !memcmp(held, "package source one", held_length));
    bake_test_source = input;
    CHECK(matches("test/marked/marked.aas_monster48", "package source one"));
    /* Marked-volume navigation can also consume the result of Grid generation. */
    bake_test_source = derived;
    CHECK(matches("test/marked/marked.aas_monster48", "package source one"));
    create(path, "package source two");
    CHECK(sh_overrides_rescan_packages() == 2);
    CHECK(matches(derived, "package source two"));
    CHECK(matches("test/marked/marked.aas_monster48", "package source two"));
    CHECK(held && !memcmp(held, "package source one", held_length));
    if (held) HeapFree(GetProcessHeap(), 0, held);
    /* Disabled package layers cannot enter the generated output indirectly. */
    user_layer_enabled = 0;
    current = sh_overrides_read_engine_resource(derived, &length);
    CHECK(!current && !length);
    if (current) HeapFree(GetProcessHeap(), 0, current);
    user_layer_enabled = 1;
    compiled = sh_package_runtime_acquire();
    resource = sh_package_compilation_find(compiled, input);
    CHECK(resource && resource->cache_path);
    cache[0] = 0;
    if (resource && resource->cache_path) strcpy_s(cache, sizeof(cache), resource->cache_path);
    sh_package_runtime_release();
    if (*cache) {
        CHECK(!DeleteFileA(cache));
        current = sh_overrides_read_engine_resource(derived, &length);
        CHECK(current && length == strlen("package source two") && !memcmp(current,"package source two",length));
        if (current) HeapFree(GetProcessHeap(), 0, current);
        CHECK(sh_overrides_rescan_packages() == 2);
        CHECK(matches(derived, "package source two"));
    }
    bake_test_source = NULL;
}

static int private_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    if (strcmp(path, "maps/modules/ind_dlc/ind_totally_blank_room_4x/lightprobes/light_probe_1_compressed.bimage")) return 0;
    *body = (unsigned char *)_strdup("installed probe"); *length = strlen("installed probe");
    return *body ? 1 : -1;
}

static void test_private_outputs(void)
{
    const char *input = "maps/modules/ind_dlc/ind_totally_blank_room_4x/lightprobes/light_probe_1_compressed.bimage";
    const char *output = "maps/modules/smpgrid/v1/m/1536_1024_512/lightprobes/light_probe_1_compressed.bimage";
    const char *alias = "generated/maps/modules/smpgrid/v1/m/1536_1024_512/lightprobes/light_probe_1_compressed.bimage";
    char stock[4096], direct[4096], duplicate[4096], from[4096], retired[4096];
    unsigned char *held;
    size_t held_length;
    const sh_package_compilation *compiled;
    const sh_compiled_resource *resource;
    snprintf(stock, sizeof(stock), "overrides/cyberdemon/assets/%s", input);
    snprintf(direct, sizeof(direct), "overrides/cyberdemon/assets/%s", output);
    snprintf(duplicate, sizeof(duplicate), "overrides/boss-demons/cyberdemon/assets/%s", alias);
    sh_package_runtime_test_baseline(private_original, NULL);
    create(stock, "installed probe");
    create(direct, "private probe"); create(duplicate, "private probe");
    CHECK(sh_overrides_rescan_packages() == 2);
    CHECK(matches(output, "private probe") && matches(alias, "private probe"));
    CHECK(matches("GENERATED\\MAPS\\MODULES\\SMPGRID\\V1\\M\\1536_1024_512\\LIGHTPROBES\\LIGHT_PROBE_1_COMPRESSED.BIMAGE", "private probe"));
    held = sh_overrides_read_engine_resource(alias, &held_length);
    CHECK(held && held_length == strlen("private probe") && !memcmp(held, "private probe", held_length));
    compiled = sh_package_runtime_acquire();
    resource = sh_package_compilation_find(compiled, alias);
    CHECK(resource && resource == sh_package_compilation_find(compiled, output));
    CHECK(resource && resource->generated && resource->baseline_known == 3 && resource->source_count == 2);
    CHECK(resource && resource->owners.bits == 3 && resource->gameplay_owners.bits == 3);
    CHECK(resource && resource->generated_input_count == 1 && !strcmp(resource->generated_inputs[0], input));
    {
        sh_package_references refs = {0};
        sh_package_owners owners = {0};
        CHECK(sh_package_references_add(&refs, "", alias));
        CHECK(sh_package_map_owners(compiled, NULL, "{}", 2, &refs, &owners, NULL));
        CHECK(owners.bits == 3);
        sh_package_owners_free(&owners); sh_package_references_free(&refs);
    }
    sh_package_runtime_release();
    {
        int found = 0;
        CHECK(sh_resource_graph_walk("", alias, transform_reference, &found)); CHECK(found);
    }
    /* Conflicting complete output bundles are excluded; their original
     * generated fallback remains available, and old open streams stay valid. */
    create(stock, "changed stock");
    {
        unsigned invalidations = source_invalidations;
        char *summary;
        CHECK(sh_overrides_rescan_packages() == 0);
        CHECK(source_invalidations == invalidations + 1 && sh_package_runtime_ready());
        summary = sh_package_runtime_summary();
        CHECK(summary && strstr(summary, "opaque replacements") && strstr(summary, "generated resource"));
        free(summary);
        CHECK(matches(alias, "installed probe"));
    }
    /* Remove both output files from assets without damaging fixture sources. */
    snprintf(from, sizeof(from), "%s/%s", root, direct);
    snprintf(retired, sizeof(retired), "%s/private-output-a", root); CHECK(MoveFileA(from, retired));
    snprintf(from, sizeof(from), "%s/%s", root, duplicate);
    snprintf(retired, sizeof(retired), "%s/private-output-b", root); CHECK(MoveFileA(from, retired));
    CHECK(sh_overrides_rescan_packages() == 2);
    CHECK(matches(alias, "changed stock"));
    CHECK(held && !memcmp(held, "private probe", held_length));
    if (held) HeapFree(GetProcessHeap(), 0, held);
    compiled = sh_package_runtime_acquire();
    resource = sh_package_compilation_find(compiled, output);
    CHECK(resource && resource->generated && !resource->source_count && sh_package_owners_count(&resource->owners) == 1);
    sh_package_runtime_release();
    create(stock, "installed probe");
    CHECK(sh_overrides_rescan_packages() == 2 && matches(output, "installed probe"));
    compiled = sh_package_runtime_acquire();
    resource = sh_package_compilation_find(compiled, output);
    CHECK(resource && !sh_package_owners_count(&resource->owners) && !sh_package_owners_count(&resource->gameplay_owners));
    sh_package_runtime_release();
    /* Restore exact fixture paths for bounded cleanup. */
    snprintf(from, sizeof(from), "%s/%s", root, direct);
    snprintf(retired, sizeof(retired), "%s/private-output-a", root); CHECK(MoveFileA(retired, from));
    snprintf(from, sizeof(from), "%s/%s", root, duplicate);
    snprintf(retired, sizeof(retired), "%s/private-output-b", root); CHECK(MoveFileA(retired, from));
    CHECK(sh_overrides_rescan_packages() == 2);
}

int main(void)
{
    char temp[MAX_PATH];
    GetTempPathA(sizeof temp, temp);
    CHECK(GetTempFileNameA(temp, "spk", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    create("overrides/cyberdemon/package.json", "{\"id\":\"cyberdemon\",\"name\":\"Cyberdemon\"}");
    create("overrides/cyberdemon/assets/generated/decls/entitydef/ai/cyber.decl", "{ class = \"idAI\"; }");
    create("overrides/cyberdemon/assets/generated/spirv/shock.vspv", "shader bytes");
    create("overrides/cyberdemon/assets/generated/image/demon.bimage", "image bytes");
    create("overrides/cyberdemon/assets/cooked/model/demon.bmodel", "model bytes");
    create("overrides/cyberdemon/assets/md6/demon.md6rig", "rig bytes");
    {
        char *large = malloc(200001);
        CHECK(large);
        if (large) { memset(large, 'x', 200000); large[200000] = 0;
            create("overrides/cyberdemon/assets/large.bin", large); free(large); }
    }
    create("overrides/cyberdemon/readme.txt", "authored auxiliary bytes");
    create("overrides/cyberdemon/decls/material/obsolete.decl", "{ }");
    create("overrides/generated/decls/entitydef/loose.decl", "{ }");
    create("overrides/boss-demons/package.json", "{\"id\":\"boss-demons\",\"name\":\"Boss demons\"}");
    create("overrides/boss-demons/cyberdemon/package.json", "{\"id\":\"cyberdemon\",\"name\":\"Cyberdemon\"}");
    create("overrides/boss-demons/cyberdemon/assets/generated/spirv/shock.vspv", "shader bytes");
    sh_package_runtime_test_empty_catalog();
    CHECK(sh_overrides_set_root(root));
    test_first_alias_activation();
    CHECK(sh_overrides_rescan_packages() == 2);
    CHECK(matches("generated/decls/entityDef/ai/cyber.decl", "{ class = \"idAI\"; }"));
    CHECK(matches("generated/spirv/shock.vspv", "shader bytes"));
    CHECK(matches("generated/image/demon.bimage", "image bytes"));
    CHECK(matches("cooked/model/demon.bmodel", "model bytes"));
    CHECK(matches("md6/demon.md6rig", "rig bytes"));
    {
        char bytes[32768];
        size_t read = 0;
        void *stream = sh_overrides_test_open("large.bin", 0);
        CHECK(stream && sh_overrides_test_stream_length(stream) == 200000);
        while (stream && read < 200000) {
            long long n = sh_overrides_test_stream_read(stream, bytes, sizeof bytes);
            CHECK(n > 0);
            if (n <= 0) break;
            for (long long i = 0; i < n; i++) CHECK(bytes[i] == 'x');
            read += (size_t)n;
        }
        CHECK(read == 200000);
        if (stream) sh_overrides_test_stream_close(stream);
    }
    CHECK(!sh_overrides_test_open("readme.txt", 0));
    CHECK(!sh_overrides_test_open("package.json", 0));
    CHECK(!sh_overrides_test_open("generated/decls/material/obsolete.decl", 0));
    CHECK(!sh_overrides_test_open("generated/decls/entitydef/loose.decl", 0));
    CHECK(!sh_overrides_test_open("generated/spirv/shock.vspv", 2));
    CHECK(!sh_overrides_test_open("../package.json", 0));
    {
        volatile LONG errors = 0;
        HANDLE threads[3];
        for (int i = 0; i < 3; i++) { threads[i] = CreateThread(NULL, 0, reader, (void *)&errors, 0, NULL); CHECK(threads[i]); }
        for (int i = 0; i < 12; i++) CHECK(sh_overrides_rescan_packages() == 2);
        for (int i = 0; i < 3; i++) if (threads[i]) {
            CHECK(WaitForSingleObject(threads[i], 30000) == WAIT_OBJECT_0); CloseHandle(threads[i]);
        }
        CHECK(!errors);
    }
    /* A stream already opened owns stable bytes while its source is edited. */
    {
        char bytes[64] = {0};
        void *stream = sh_overrides_test_open("generated/image/demon.bimage", 0);
        CHECK(stream);
        create("overrides/cyberdemon/assets/generated/image/demon.bimage", "updated image bytes");
        CHECK(matches("generated/image/demon.bimage", "image bytes"));
        CHECK(sh_overrides_rescan_packages() == 2);
        CHECK(matches("generated/image/demon.bimage", "updated image bytes"));
        if (stream) {
            CHECK(sh_overrides_test_stream_read(stream, bytes, sizeof bytes) == 11);
            CHECK(!strcmp(bytes, "image bytes")); sh_overrides_test_stream_close(stream);
        }
    }
    {
        FILE *file = NULL;
        uint64_t length = 0;
        char bytes[32] = {0};
        CHECK(sh_package_runtime_open_file("generated/image/demon.bimage", &file, &length) == 1);
        CHECK(file && length == strlen("updated image bytes"));
        if (file) {
            CHECK(!_fseeki64(file, -5, SEEK_END));
            CHECK(fread(bytes, 1, sizeof(bytes), file) == 5 && !strcmp(bytes, "bytes"));
            fclose(file);
        }
        CHECK(sh_package_runtime_open_file("missing.bin", &file, &length) == 0 && !file && !length);
        CHECK(sh_package_runtime_open_file("missing.bin", NULL, &length) == -1);
    }
    /* Published cache bytes cannot be corrupted. After fixture-only provider
     * retirement, an altered cache is rebuilt from verified authored source. */
    {
        const sh_package_compilation *compiled = sh_package_runtime_acquire();
        const sh_compiled_resource *resource = sh_package_compilation_find(compiled, "generated/image/demon.bimage");
        char *path = resource && resource->cache_path ? _strdup(resource->cache_path) : NULL;
        FILE *file = NULL;
        sh_package_runtime_release(); CHECK(path);
        if (path) {
            CHECK(fopen_s(&file, path, "wb") != 0 && !file);
            CHECK(matches("generated/image/demon.bimage", "updated image bytes"));
            sh_package_runtime_test_dispose();
            CHECK(!fopen_s(&file, path, "wb") && file);
            if (file) { fputs("broken cache", file); fclose(file); }
            CHECK(!sh_overrides_test_open("generated/image/demon.bimage", 0));
            CHECK(sh_overrides_rescan_packages() == 2);
            CHECK(matches("generated/image/demon.bimage", "updated image bytes"));
            free(path);
        }
    }
    /* Published native aliases follow the current compiler, including a
     * changed body and removal. They never retain an earlier package's text. */
    {
        const char *relative = "overrides/cyberdemon/assets/generated/decls/material/test/alias.decl";
        static const unsigned char stale[] = "{ value = 0; }";
        sh_overrides_internal_decl_entry entry = {"material", "test/alias", stale, sizeof(stale) - 1};
        char path[4096];
        void *held;
        create(relative, "{ value = 1; }");
        CHECK(sh_overrides_rescan_packages() == 2);
        CHECK(sh_overrides_test_internal_decl_table_install(&entry, 1));
        CHECK(matches("decltree/material/test/alias.decl", "{ value = 1; }"));
        CHECK(sh_overrides_internal_decl_published("decltree/material/test/alias.decl"));
        held = sh_overrides_test_open("decltree/material/test/alias.decl", 0); CHECK(held);
        create(relative, "{ value = 2; }");
        CHECK(sh_overrides_rescan_packages() == 2);
        CHECK(matches("decltree/material/test/alias.decl", "{ value = 2; }"));
        snprintf(path, sizeof(path), "%s/%s", root, relative); CHECK(DeleteFileA(path));
        CHECK(sh_overrides_rescan_packages() == 2);
        CHECK(!sh_overrides_test_open("decltree/material/test/alias.decl", 0));
        CHECK(!sh_overrides_internal_decl_published("decltree/material/test/alias.decl"));
        CHECK(!sh_package_runtime_decl_alias_exists("decltree/material/test/alias.decl"));
        if (held) {
            char old_bytes[32] = {0};
            CHECK(sh_overrides_test_stream_read(held, old_bytes, sizeof(old_bytes)) == strlen("{ value = 1; }"));
            CHECK(!strcmp(old_bytes, "{ value = 1; }")); sh_overrides_test_stream_close(held);
        }
        create(relative, "{ value = 2; }"); /* Restore the fixture for bounded cleanup. */
        CHECK(sh_overrides_rescan_packages() == 2);
        CHECK(sh_overrides_internal_decl_published("decltree/material/test/alias.decl"));
        CHECK(matches("decltree/material/test/alias.decl", "{ value = 2; }"));
    }
    create("overrides/boss-demons/cyberdemon/assets/generated/spirv/shock.vspv", "conflicting shader");
    CHECK(sh_overrides_rescan_packages() == 0);
    CHECK(sh_package_runtime_ready());
    /* No conflicting source wins; healthy built-ins remain. */
    {
        const sh_package_compilation *compiled = sh_package_runtime_acquire();
        const sh_compiled_resource *resource = sh_package_compilation_find(compiled, "generated/spirv/shock.vspv");
        CHECK(compiled && !compiled->sources->package_count && !resource);
        sh_package_runtime_release();
    }
    {
        const char *relative = "overrides/cyberdemon/assets/generated/decls/material/test/original.decl";
        const char *engine_path = "generated/decls/material/test/original.decl";
        char installed[4096], retired[4096];
        int unreadable = 0;
        /* Restore the conflicting fixture, then remove the entire inventory.
         * The new snapshot has no source files for original-only resources. */
        create("overrides/boss-demons/cyberdemon/assets/generated/spirv/shock.vspv", "shader bytes");
        sh_package_runtime_test_baseline(original_decl, &unreadable);
        create(relative, "{ value = 9; }");
        CHECK(sh_overrides_rescan_packages() == 2);
        snprintf(installed, sizeof(installed), "%s/overrides", root);
        snprintf(retired, sizeof(retired), "%s/retired", root);
        CHECK(MoveFileA(installed, retired));
        unreadable = 1;
        CHECK(sh_overrides_rescan_packages() == SH_OVERRIDES_RESCAN_FAILED);
        CHECK(!sh_package_runtime_ready());
        CHECK(matches(engine_path, "{ value = 9; }"));
        unreadable = 0;
        for (int i = 0; i < 2; i++) {
            unsigned char *body = NULL; size_t length = 0;
            const sh_package_compilation *compiled;
            const sh_compiled_resource *resource;
            CHECK(sh_overrides_rescan_packages() == 0);
            CHECK(sh_package_runtime_ready());
            CHECK(matches(engine_path, "{ value = 0; }"));
            CHECK(sh_package_runtime_read_decl_alias("decltree/material/test/original.decl", &body, &length) == 1);
            CHECK(body && !strcmp((char *)body, "{ value = 0; }")); free(body);
            compiled = sh_package_runtime_acquire();
            resource = sh_package_compilation_find(compiled, engine_path);
            CHECK(compiled && !compiled->sources->file_count && !compiled->sources->package_count);
            CHECK(resource && resource->restored_original && !sh_package_owners_count(&resource->owners) && !sh_package_owners_count(&resource->gameplay_owners));
            sh_package_runtime_release();
        }
        CHECK(MoveFileA(retired, installed));
        CHECK(sh_overrides_rescan_packages() == 2);
        CHECK(matches(engine_path, "{ value = 9; }"));
        sh_package_runtime_test_baseline(NULL, NULL);
    }
    test_transform_sources();
    test_private_outputs();
    test_all_builtin_defaults();
    test_activation_rollback();
    /* Failure after entering the final publication guard must release it and
     * preserve both the provider and the derived-resource revision. */
    {
        sh_package_references refs = {0};
        char error[2048];
        unsigned invalidations = source_invalidations;
        CHECK(sh_package_references_add(&refs, "", "generated/spirv/shock.vspv"));
        CHECK(sh_package_runtime_select_map("{}", 2, &refs, error, sizeof(error)));
        sh_package_references_free(&refs);
        CHECK(!sh_resource_graph_producer_inputs("fixture", "bad", (const char *const[]){NULL}, 1));
        CHECK(sh_overrides_rescan_packages() == SH_OVERRIDES_RESCAN_FAILED);
        CHECK(!source_update_active && source_invalidations == invalidations);
        CHECK(matches("generated/spirv/shock.vspv", "shader bytes"));
        sh_resource_graph_test_reset();
        CHECK(sh_package_runtime_select_map(NULL, 0, NULL, error, sizeof(error)));
        CHECK(sh_overrides_rescan_packages() == 4);
    }
    /* Losing the native schema refuses the prospective activation while
     * preserving the last complete provider snapshot. */
    {
        const sh_package_compilation *before = sh_package_runtime_acquire(), *after;
        unsigned invalidations = source_invalidations;
        sh_package_runtime_release();
        CHECK(before);
        sh_package_runtime_bind_native(0, 0, 0);
        CHECK(sh_package_runtime_native_status() == -1);
        CHECK(sh_overrides_rescan_packages() == SH_OVERRIDES_RESCAN_FAILED);
        CHECK(source_invalidations == invalidations);
        CHECK(!sh_package_runtime_ready());
        after = sh_package_runtime_acquire();
        CHECK(after == before);
        sh_package_runtime_release();
    }
    cleanup();
    printf("override_packages_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
int sh_navmesh_validate_aas(const unsigned char *p,size_t n,char *e,size_t cap)
{(void)p;(void)n;if(e&&cap)e[0]=0;return 0;}
