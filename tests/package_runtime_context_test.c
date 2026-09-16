/* Real runtime provider transactions with independent library and map views.
 * Native activation is modeled here; renderer acceptance is separate. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "package_runtime.h"
#include "package_fixture.h"

static const char *health_path = "generated/decls/entitydef/ai/cyberdemon.decl";
static const char *descriptor = "{\"id\":\"installed-bosses\",\"name\":\"Installed Bosses\",\"strings\":{\"en\":{\"label\":\"local\"}}}";
static const char *original = "{ edit = { health = 25000; } }";
void backend_log(const char *message) { (void)message; }

static int baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    if (!strcmp(path, "stock.bin") || !strcmp(path, "campaign.bin")) {
        *length = 8; *body = (unsigned char *)_strdup("original");
        return !*body ? -1 : !strcmp(path, "stock.bin") ? 1 : 2;
    }
    if (strcmp(path, health_path)) return 0;
    *length = strlen(original); *body = (unsigned char *)_strdup(original);
    return *body ? 2 : -1;
}

static void expect_current(const char *path, const char *wanted)
{
    unsigned char *bytes = NULL; size_t length = 0;
    int result = sh_package_runtime_read(path, &bytes, &length);
    CHECK(wanted ? result == 1 && bytes && strstr((const char *)bytes, wanted) : result == 0);
    free(bytes);
}

static const sh_package_compilation *expect_library(const char *health)
{
    const sh_package_compilation *library = sh_package_runtime_library_acquire();
    const sh_compiled_resource *resource = sh_package_compilation_find(library, health_path);
    char error[512];
    CHECK(library && !library->map_overlay);
    CHECK(resource && resource->body && strstr((const char *)resource->body, health));
    CHECK(sh_package_compilation_probe(library, "map-only.bimage", error, sizeof(error)) == 0);
    sh_package_runtime_release(); return library;
}

typedef struct activation {
    const char *next_health, *previous_health, *library_health;
    const sh_package_compilation *previous, *library;
    int fail, calls, recoveries, map_active, previous_map_active, library_changed;
    const sh_package_change *expected;
    size_t expected_count;
} activation;

static int activate(void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    CHECK(changes);
    activation *state = context;
    if (state->expected) {
        CHECK(changes->count == state->expected_count);
        for (size_t i = 0; i < changes->count && i < state->expected_count; i++) {
            sh_package_change_kind wanted = state->expected[i].kind;
            if (restoring && wanted == SH_PACKAGE_RESOURCE_ADDED) wanted = SH_PACKAGE_RESOURCE_REMOVED;
            else if (restoring && wanted == SH_PACKAGE_RESOURCE_REMOVED) wanted = SH_PACKAGE_RESOURCE_ADDED;
            CHECK(!strcmp(changes->items[i].path, state->expected[i].path));
            CHECK(changes->items[i].kind == wanted);
        }
    }
    const sh_package_compilation *current = sh_package_runtime_acquire();
    CHECK(restoring ? current == state->previous : current != state->previous);
    sh_package_runtime_release();
    CHECK(!sh_package_runtime_ready());
    CHECK(!sh_package_runtime_admission_ready());
    CHECK(sh_package_runtime_has_map_provider() == (restoring ? state->previous_map_active : state->map_active));
    expect_current(health_path, restoring ? state->previous_health : state->next_health);
    /* A committed installation rescans the library in the same transaction, so
     * the activation pass sees a new library; recovery restores the old one. */
    if (state->library_changed && !restoring) CHECK(expect_library(state->library_health) != state->library);
    else CHECK(expect_library(state->library_health) == state->library);
    if (restoring) { state->recoveries++; return 1; }
    state->calls++;
    if (state->fail == 2) RaiseException(0xe04d504e, 0, 0, NULL);
    if (state->fail) { snprintf(error, capacity, "fixture map activation failure"); return 0; }
    return 1;
}

static int switch_map(const char *map, activation *state)
{
    state->previous = sh_package_runtime_acquire(); sh_package_runtime_release();
    state->library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    state->previous_map_active = sh_package_runtime_has_map_provider();
    return sh_package_runtime_activate_map(root, map, (sh_package_activation_guard){0}, activate, state);
}

static int activate_plan(sh_package_map_plan *plan, activation *state)
{
    state->previous = sh_package_runtime_acquire(); sh_package_runtime_release();
    state->library = sh_package_runtime_library_acquire(); sh_package_runtime_release();
    state->previous_map_active = sh_package_runtime_has_map_provider();
    return sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, activate, state);
}

static void test_prepared_map(const char *map_root)
{
    static const char *paths[] = {
        "generated/decls/material/prepared.decl", "generated/decls/fx/prepared.decl",
        "generated/decls/sound/prepared.decl", "generated/decls/aimodule/prepared.decl",
        "generated/rendermodels/prepared.bmodel", "generated/md6/prepared.md6mesh",
        "generated/md6/prepared.md6anim", "generated/images/prepared.bimage",
        "generated/spirv/prepared.vspv", "generated/glprogs/prepared.bprog",
        "sound/soundbanks/prepared.bnk", "sound/streams/prepared.wem",
        "future/engine-family/prepared.bytes"
    };
    const char *map_descriptor = "{\"id\":\"map-cyberdemon\",\"name\":\"Map Demon\"}";
    const char *local_bytes = "{ value = \"local\"; }", *map_bytes = "{ value = \"prepared\"; }";
    char path[4096], error[2048], previous_error[2048];
    sh_package_map_plan *plan, *cancelled;
    const sh_package_compilation *current, *candidate;
    activation state = {0};
    create("map-a/overrides/map/package.json", map_descriptor);
    create("map-a/overrides/map/assets/campaign.bin", "map campaign bytes");
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        snprintf(path, sizeof(path), "overrides/local/assets/%s", paths[i]); create(path, local_bytes);
        snprintf(path, sizeof(path), "map-a/overrides/map/assets/%s", paths[i]); create(path, map_bytes);
    }
    CHECK(sh_package_runtime_refresh(root)); CHECK(sh_package_runtime_ready());
    current = sh_package_runtime_acquire(); sh_package_runtime_release();
    sh_package_runtime_error(previous_error, sizeof(previous_error));
    CHECK(!sh_package_runtime_prepare_map(root, "", error, sizeof(error)));
    CHECK(!sh_package_runtime_prepare_map(NULL, map_root, NULL, 0));
    create("map-a/overrides/map/package.json", "malformed source");
    CHECK(!sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)));
    CHECK(error[0] && sh_package_runtime_ready());
    sh_package_runtime_error(error, sizeof(error)); CHECK(!strcmp(error, previous_error));
    const sh_package_compilation *after = sh_package_runtime_acquire(); CHECK(after == current); sh_package_runtime_release();
    create("map-a/overrides/map/package.json", map_descriptor);
    cancelled = sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)); CHECK(cancelled);
    CHECK(!sh_package_runtime_has_map_provider());
    sh_package_map_plan_free(cancelled); sh_package_map_plan_free(NULL);
    CHECK(sh_package_runtime_ready());
    plan = sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)); CHECK(plan);
    if (!plan) return;
    candidate = sh_package_map_plan_compilation(plan); CHECK(candidate && !candidate->map_overlay);
    {
        const char *requirements[] = {health_path, "stock.bin", "campaign.bin", "map-only.bimage"};
        sh_package_missing missing = {0};
        CHECK(sh_package_map_plan_missing(plan, requirements, 4, &missing, error, sizeof(error)));
        CHECK(missing.checked == 4 && missing.count == 2 && sh_package_owners_count(&missing.packages) == 1);
        CHECK(!strcmp(missing.paths[0], "campaign.bin") && !strcmp(missing.paths[1], "map-only.bimage"));
        CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)));
        CHECK(missing.count == 2 && !strcmp(missing.paths[0], "campaign.bin") &&
            !strcmp(missing.paths[1], "map-only.bimage"));
        CHECK(!sh_package_map_plan_payload_missing(NULL, &missing, error, sizeof(error)));
        CHECK(!missing.count && !missing.paths && !missing.checked);
        CHECK(!sh_package_map_plan_missing(NULL, requirements, 4, &missing, error, sizeof(error)));
        CHECK(!missing.count && !missing.paths); sh_package_missing_free(&missing);
    }
    CHECK(!sh_package_map_plan_compilation(NULL));
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        const sh_compiled_resource *resource = sh_package_compilation_find(candidate, paths[i]);
        unsigned char *bytes; size_t length = 0;
        CHECK(resource); bytes = sh_package_compilation_read(candidate, resource, SIZE_MAX, &length, error, sizeof(error));
        CHECK(bytes && length == strlen(map_bytes) && !memcmp(bytes, map_bytes, length)); free(bytes);
        expect_current(paths[i], "local");
        /* The prepared candidate owns declarations and cached opaque bytes.
         * Changing sources cannot alter the already inspected candidate. */
        snprintf(path, sizeof(path), "map-a/overrides/map/assets/%s", paths[i]); create(path, "changed after preparation");
    }
    create("map-a/overrides/map/package.json", "changed after preparation");
    /* Pending consent can outlive a local refresh. Activation must retain the
     * latest library, not restore whichever library existed at preparation. */
    create("overrides/local/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 16000; } }");
    CHECK(sh_package_runtime_refresh(root));
    state.next_health = "health = 10"; state.previous_health = "health = 16000";
    state.library_health = "health = 16000"; state.map_active = 1;
    for (int failure = 1; failure <= 2; failure++) {
        state.fail = failure; CHECK(!activate_plan(plan, &state));
        CHECK(state.calls == failure && state.recoveries == failure);
        CHECK(sh_package_map_plan_compilation(plan) == candidate);
        for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) expect_current(paths[i], "local");
    }
    state.fail = 0; CHECK(activate_plan(plan, &state)); CHECK(sh_package_runtime_ready());
    {
        const char *required[] = {"map-only.bimage"};
        sh_package_missing missing = {0};
        /* Active map bytes cannot certify their own local availability. */
        expect_current("map-only.bimage", "map-only bytes");
        CHECK(sh_package_map_plan_missing(plan, required, 1, &missing, error, sizeof(error)));
        CHECK(missing.count == 1 && sh_package_owners_count(&missing.packages) == 1);
        CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)));
        CHECK(missing.count == 2); /* Neither active map source counts as installed. */
        sh_package_missing_free(&missing);
    }
    sh_package_map_plan_free(plan); /* Runtime now owns the active map provider. */
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) expect_current(paths[i], "prepared");
    state.next_health = "health = 16000"; state.previous_health = "health = 10"; state.map_active = 0;
    state.fail = 1; CHECK(!activate_plan(NULL, &state)); CHECK(sh_package_runtime_has_map_provider());
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) expect_current(paths[i], "prepared");
    state.fail = 0; CHECK(activate_plan(NULL, &state)); CHECK(!sh_package_runtime_has_map_provider());
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) expect_current(paths[i], "local");
}

/* A whole-package installation commits its files into the shared package tree
 * while the temporary map provider is still the only thing supplying them.
 * Retiring that overlay against the pre-install library would report every
 * committed resource as removed and retire content the consumers are using. */
static void test_committed_install_joins_library(void)
{
    static const char *committed_mesh = "delivered.md6mesh";
    static const char *committed_decl = "generated/decls/entitydef/ai/delivered_demon.decl";
    static const char *identity = "{\"id\":\"delivered-demons\",\"name\":\"Delivered Demons\"}";
    static const char *decl_bytes = "{ edit = { health = 7; } }";
    const sh_package_change installed[] = {
        {"delivered.md6mesh", SH_PACKAGE_RESOURCE_ADDED},
        {"generated/decls/entitydef/ai/delivered_demon.decl", SH_PACKAGE_RESOURCE_ADDED}
    };
    char map_root[MAX_PATH], path[4096], error[2048];
    const sh_package_compilation *library;
    sh_package_map_plan *plan;
    activation state = {0};
    int calls;
    snprintf(map_root, sizeof(map_root), "%s/map-install", root);
    create("map-install/overrides/map/package.json", identity);
    snprintf(path, sizeof(path), "map-install/overrides/map/assets/%s", committed_mesh);
    create(path, "delivered mesh");
    snprintf(path, sizeof(path), "map-install/overrides/map/assets/%s", committed_decl);
    create(path, decl_bytes);
    CHECK(sh_package_runtime_refresh(root)); CHECK(sh_package_runtime_ready());
    expect_current(committed_mesh, NULL);
    plan = sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)); CHECK(plan);
    if (!plan) return;
    state.next_health = state.previous_health = state.library_health = "health = 16000";
    state.map_active = 1; state.expected = installed; state.expected_count = 2;
    CHECK(activate_plan(plan, &state)); CHECK(state.calls == 1);
    expect_current(committed_mesh, "delivered mesh");
    sh_package_map_plan_free(plan);
    create("overrides/delivered-demons/package.json", identity);
    snprintf(path, sizeof(path), "overrides/delivered-demons/assets/%s", committed_mesh);
    create(path, "delivered mesh");
    snprintf(path, sizeof(path), "overrides/delivered-demons/assets/%s", committed_decl);
    create(path, decl_bytes);
    sh_package_runtime_note_sources_changed();
    /* A library rescan that cannot compile retires nothing and keeps the overlay. */
    create("overrides/delivered-demons/package.json", "malformed after commit");
    state.map_active = 0; calls = state.calls;
    CHECK(!activate_plan(NULL, &state));
    CHECK(state.calls == calls && !state.recoveries);
    CHECK(sh_package_runtime_has_map_provider());
    expect_current(committed_mesh, "delivered mesh");
    create("overrides/delivered-demons/package.json", identity);
    /* Now the committed sources compile: the overlay retires with no change at
     * all, because the library already supplies exactly the same resources. */
    state.library_changed = 1; state.expected_count = 0;
    CHECK(activate_plan(NULL, &state)); CHECK(state.calls == calls + 1);
    CHECK(!sh_package_runtime_has_map_provider());
    expect_current(committed_mesh, "delivered mesh");
    expect_current(committed_decl, "health = 7");
    library = sh_package_runtime_library_acquire();
    CHECK(library && sh_package_compilation_find(library, committed_mesh));
    CHECK(sh_package_compilation_find(library, committed_decl));
    sh_package_runtime_release();
    /* The rescan is consumed. A later map transition keeps the same library. */
    plan = sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)); CHECK(plan);
    state.library_changed = 0; state.map_active = 1; state.expected_count = 0;
    CHECK(activate_plan(plan, &state));
    state.map_active = 0; CHECK(activate_plan(NULL, &state));
    CHECK(!sh_package_runtime_has_map_provider());
    expect_current(committed_mesh, "delivered mesh");
    sh_package_map_plan_free(plan);
}

static int inventory_commit_calls, inventory_commit_failure;
static int commit_inventory(char *error, size_t capacity)
{
    inventory_commit_calls++;
    if (inventory_commit_failure == 2) RaiseException(0xe0422083, 0, 0, NULL);
    if (inventory_commit_failure) { snprintf(error, capacity, "install commit refused"); return 0; }
    return 1;
}

static void test_installed_inventory_is_not_active_composition(void)
{
    char map_root[MAX_PATH], error[2048];
    sh_package_missing missing = {0};
    sh_package_map_plan *plan;
    activation state = {0};
    snprintf(map_root, sizeof(map_root), "%s/map-inventory", root);
    create("map-inventory/overrides/map/package.json", "{\"id\":\"installed-bosses\",\"name\":\"Authored variant\"}");
    create("map-inventory/overrides/map/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 10; } }");
    create("map-inventory/overrides/map/assets/boss.md6mesh", "authored mesh");
    plan = sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)); CHECK(plan);
    if (!plan) return;
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && missing.count == 1);
    /* A whole map-installed variant overlaps local gameplay values and shares
     * its id. Storing it must not force those contradictory values to compose. */
    create("overrides/delivered/bosses/package.json", "{\"id\":\"installed-bosses\",\"name\":\"Authored variant\"}");
    create("overrides/delivered/bosses/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 10; } }");
    create("overrides/delivered/bosses/assets/boss.md6mesh", "authored mesh");
    CHECK(sh_package_map_plan_prepare_inventory(plan, root, error, sizeof(error)));
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && missing.count == 1);
    CHECK(!sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)) && !inventory_commit_calls);
    state.next_health = "health = 10"; state.previous_health = state.library_health = "health = 16000";
    state.map_active = 1;
    for (int failure = 1; failure <= 2; failure++) {
        state.fail = failure; CHECK(!activate_plan(plan, &state));
        CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && missing.count == 1);
        expect_current(health_path, "health = 16000");
    }
    state.fail = 0; CHECK(activate_plan(plan, &state));
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && missing.count == 1);
    for (inventory_commit_failure = 1; inventory_commit_failure <= 2; inventory_commit_failure++) {
        CHECK(!sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)) && error[0]);
        CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && missing.count == 1);
        CHECK(sh_package_runtime_ready() && sh_package_runtime_admission_ready());
        expect_current(health_path, "health = 10");
    }
    inventory_commit_failure = 0;
    CHECK(!sh_package_runtime_commit_map(NULL, commit_inventory, error, sizeof(error)) && inventory_commit_calls == 2);
    CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)) && inventory_commit_calls == 3);
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && !missing.count);
    expect_current(health_path, "health = 10"); expect_library("health = 16000");
    CHECK(!sh_package_runtime_refresh(root));
    CHECK(!sh_package_runtime_ready() && sh_package_runtime_admission_ready());
    /* An unrelated rejected local composition does not invalidate the same
     * successfully activated map or require another installation. */
    CHECK(sh_package_runtime_commit_map(plan, commit_inventory, error, sizeof(error)));
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && !missing.count);
    expect_current(health_path, "health = 10"); expect_library("health = 16000");
    state.next_health = "health = 16000"; state.previous_health = "health = 10"; state.map_active = 0;
    CHECK(activate_plan(NULL, &state));
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && !missing.count);
    expect_current(health_path, "health = 16000"); expect_current("boss.md6mesh", NULL);
    /* No installed source changed, and cached active-map data alone never
     * certifies a source file that was subsequently removed or changed. */
    create("overrides/delivered/bosses/assets/boss.md6mesh", "changed externally");
    CHECK(!sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)));
    CHECK(!missing.count && strstr(error, "installed source changed"));
    sh_package_missing_free(&missing); sh_package_map_plan_free(plan);
}

static int startup_activation(void *context, int restoring,
    const sh_package_changes *changes, char *error, size_t capacity)
{
    CHECK(changes);
    int fail = *(int *)context;
    (void)error; (void)capacity;
    CHECK(!sh_package_runtime_admission_ready());
    if (restoring) return 1;
    return !fail;
}

static void test_conflicting_startup(void)
{
    char map_root[MAX_PATH], error[2048];
    sh_package_map_plan *plan;
    sh_package_missing missing = {0};
    const sh_package_compilation *library;
    char *summary;
    int fail = 1;
    create("overrides/a/package.json", "{\"id\":\"bosses\",\"name\":\"Original Bosses\"}");
    create("overrides/a/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 12000; } }");
    create("overrides/b/package.json", "{\"id\":\"bosses\",\"name\":\"Map Bosses\"}");
    create("overrides/b/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 10; } }");
    create("private/overrides/map/package.json", "{\"id\":\"bosses\",\"name\":\"Map Bosses\"}");
    create("private/overrides/map/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 10; } }");
    snprintf(map_root, sizeof(map_root), "%s/private", root);
    sh_package_runtime_test_baseline(baseline, NULL);
    CHECK(!sh_package_runtime_refresh_activated(root, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(!sh_package_runtime_admission_ready());
    library = sh_package_runtime_library_acquire(); CHECK(!library); sh_package_runtime_release();
    fail = 0;
    CHECK(!sh_package_runtime_refresh_activated(root, (sh_package_activation_guard){0}, startup_activation, &fail));
    CHECK(!sh_package_runtime_ready() && sh_package_runtime_admission_ready());
    library = sh_package_runtime_library_acquire();
    CHECK(library && !library->sources->package_count && !sh_package_compilation_find(library, health_path));
    sh_package_runtime_release();
    summary = sh_package_runtime_summary();
    CHECK(summary && strstr(summary, "Installed library: 2 packages") && strstr(summary, "Local authoring composition unavailable")); free(summary);
    plan = sh_package_runtime_prepare_map(root, map_root, error, sizeof(error)); CHECK(plan);
    CHECK(sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)) && !missing.count);
    CHECK(sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, startup_activation, &fail));
    expect_current(health_path, "health = 10"); CHECK(sh_package_runtime_admission_ready());
    CHECK(sh_package_runtime_activate_prepared_map(NULL, (sh_package_activation_guard){0}, startup_activation, &fail));
    expect_current(health_path, NULL); CHECK(sh_package_runtime_admission_ready());
    /* Reconciling the actual source conflict restores the normal complete local
     * composition, without a process reset or a hidden package priority. */
    create("overrides/b/assets/generated/decls/entitydef/ai/cyberdemon.decl", original);
    CHECK(sh_package_runtime_refresh(root)); expect_current(health_path, "health = 12000");
    summary = sh_package_runtime_summary(); CHECK(summary && !strstr(summary, "composition unavailable")); free(summary);
    sh_package_missing_free(&missing); sh_package_map_plan_free(plan);
}

static void run_conflicting_startup(void)
{
    char executable[MAX_PATH], command[MAX_PATH + 64];
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {0};
    DWORD result = 1;
    CHECK(GetModuleFileNameA(NULL, executable, sizeof(executable)));
    snprintf(command, sizeof(command), "\"%s\" --conflicting-startup", executable);
    CHECK(CreateProcessA(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process));
    if (!process.hProcess) return;
    CHECK(WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0);
    CHECK(GetExitCodeProcess(process.hProcess, &result) && !result);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
}

int main(int argc, char **argv)
{
    const sh_package_change map_a_changes[] = {
        {"generated/decls/entitydef/ai/cyberdemon.decl", SH_PACKAGE_RESOURCE_REPLACED},
        {"generated/spirv/fixture.vspv", SH_PACKAGE_RESOURCE_REPLACED},
        {"map-only.bimage", SH_PACKAGE_RESOURCE_ADDED}
    };
    const sh_package_change map_b_changes[] = {
        {"generated/decls/entitydef/ai/cyberdemon.decl", SH_PACKAGE_RESOURCE_REPLACED},
        {"generated/spirv/fixture.vspv", SH_PACKAGE_RESOURCE_REPLACED},
        {"map-only.bimage", SH_PACKAGE_RESOURCE_REMOVED}
    };
    char temporary[MAX_PATH], map_a[MAX_PATH], map_b[MAX_PATH], error[2048];
    activation state = {0};
    const sh_package_compilation *library;
    sh_package_references refs = {0};
    size_t length; char *policy;
    FILE *held_map_file = NULL; uint64_t held_length = 0;
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "pmr", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    if (argc == 2 && !strcmp(argv[1], "--conflicting-startup")) {
        test_conflicting_startup(); cleanup(); return failures ? 1 : 0;
    }
    run_conflicting_startup();
    snprintf(map_a, sizeof(map_a), "%s\\map-a", root); snprintf(map_b, sizeof(map_b), "%s\\map-b", root);
    create("overrides/local/package.json", descriptor);
    create("overrides/local/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 12000; } }");
    create("overrides/local/assets/generated/spirv/fixture.vspv", "local shader");
    create("map-a/overrides/map/package.json", "{\"id\":\"map-cyberdemon\",\"name\":\"Map Demon\",\"strings\":{\"en\":{\"label\":\"map A\"}}}");
    create("map-a/overrides/map/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 10; } }");
    create("map-a/overrides/map/assets/generated/spirv/fixture.vspv", "shader A");
    create("map-a/overrides/map/assets/map-only.bimage", "map-only bytes");
    create("map-b/overrides/map/package.json", "{\"id\":\"map-cyberdemon\",\"name\":\"Map Demon\",\"strings\":{\"en\":{\"label\":\"map B\"}}}");
    create("map-b/overrides/map/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 20; } }");
    create("map-b/overrides/map/assets/generated/spirv/fixture.vspv", "shader B");
    sh_package_runtime_test_baseline(baseline, NULL);
    CHECK(!sh_package_runtime_activate_map(root, map_a, (sh_package_activation_guard){0}, activate, &state));
    CHECK(!state.calls && !state.recoveries);
    CHECK(sh_package_runtime_refresh(root)); CHECK(!sh_package_runtime_has_map_provider());
    library = expect_library("health = 12000");
    {
        sh_package_missing missing = {0};
        sh_package_map_plan *plan = sh_package_runtime_prepare_map(root, map_b, error, sizeof(error));
        CHECK(plan && sh_package_map_plan_payload_missing(plan, &missing, error, sizeof(error)));
        /* Different package identity, health and shader bytes: every supplied
         * engine path is already installed, so no installation is needed. */
        CHECK(missing.checked == 2 && !missing.count && !sh_package_owners_count(&missing.packages));
        CHECK(!sh_package_runtime_has_map_provider() && sh_package_runtime_ready());
        expect_current(health_path, "health = 12000"); expect_current("generated/spirv/fixture.vspv", "local shader");
        sh_package_missing_free(&missing); sh_package_map_plan_free(plan);
    }
    CHECK(sh_package_references_add(&refs, "", health_path));
    CHECK(sh_package_runtime_select_map("{}", 2, &refs, error, sizeof(error))); sh_package_references_free(&refs);
    state.next_health = "health = 10"; state.previous_health = "health = 12000";
    state.library_health = "health = 12000"; state.map_active = 1;
    state.expected = map_a_changes; state.expected_count = 3;
    for (int failure = 1; failure <= 2; failure++) {
        state.fail = failure;
        CHECK(!switch_map(map_a, &state)); CHECK(state.calls == failure && state.recoveries == failure);
        CHECK(!sh_package_runtime_has_map_provider() && !sh_package_runtime_ready());
        expect_current(health_path, "health = 12000"); CHECK(expect_library("health = 12000") == library);
    }
    state.fail = 0; CHECK(switch_map(map_a, &state)); CHECK(sh_package_runtime_ready());
    expect_current("generated/spirv/fixture.vspv", "shader A"); expect_current("map-only.bimage", "map-only bytes");
    CHECK(sh_package_runtime_open_file("map-only.bimage", &held_map_file, &held_length) == 1);
    CHECK(held_map_file && held_length == strlen("map-only bytes"));
    policy = sh_package_runtime_active_policy("strings", &length); CHECK(policy && strstr(policy, "map A")); free(policy);
    /* A map switch reads only the new map. A broken local source cannot make
     * the runtime discard the already captured local restore provider. */
    create("overrides/local/package.json", "malformed external edit");
    state.next_health = "health = 20"; state.previous_health = "health = 10";
    state.expected = map_b_changes;
    state.fail = 1; CHECK(!switch_map(map_b, &state));
    expect_current("map-only.bimage", "map-only bytes");
    state.fail = 0;
    CHECK(switch_map(map_b, &state)); CHECK(expect_library("health = 12000") == library);
    expect_current("generated/spirv/fixture.vspv", "shader B"); expect_current("map-only.bimage", NULL);
    if (held_map_file) {
        char held_bytes[32] = {0};
        CHECK(fread(held_bytes, 1, sizeof(held_bytes), held_map_file) == strlen("map-only bytes"));
        CHECK(!strcmp(held_bytes, "map-only bytes")); fclose(held_map_file); held_map_file = NULL;
    }
    policy = sh_package_runtime_active_policy("strings", &length); CHECK(policy && strstr(policy, "map B")); free(policy);
    /* Re-entering an identical map still runs activation for policy and native
     * readiness, but supplies no false resource replacements. */
    state.previous_health = "health = 20"; state.expected_count = 0;
    CHECK(switch_map(map_b, &state));
    state.next_health = "health = 12000"; state.previous_health = "health = 20"; state.map_active = 0;
    state.expected_count = 2;
    state.fail = 1; CHECK(!switch_map(NULL, &state)); CHECK(sh_package_runtime_has_map_provider());
    expect_current("generated/spirv/fixture.vspv", "shader B");
    policy = sh_package_runtime_active_policy("strings", &length); CHECK(policy && strstr(policy, "map B")); free(policy);
    state.fail = 0; CHECK(switch_map(NULL, &state)); CHECK(!sh_package_runtime_has_map_provider());
    expect_current(health_path, "health = 12000"); expect_current("generated/spirv/fixture.vspv", "local shader");
    CHECK(expect_library("health = 12000") == library);
    {
        sh_json_object empty_policy = {0};
        policy = sh_package_runtime_active_policy("strings", &length);
        CHECK(policy && sh_json_parse_object(policy, length, 16, &empty_policy));
        CHECK(empty_policy.count == 0); sh_json_object_free(&empty_policy); free(policy);
    }
    create("overrides/local/package.json", descriptor);
    state.next_health = "health = 10"; state.previous_health = "health = 12000"; state.map_active = 1;
    state.expected = map_a_changes; state.expected_count = 3;
    CHECK(switch_map(map_a, &state));
    create("overrides/local/assets/generated/decls/entitydef/ai/cyberdemon.decl", "{ edit = { health = 15000; } }");
    CHECK(sh_package_runtime_refresh(root)); CHECK(sh_package_runtime_has_map_provider());
    expect_current(health_path, "health = 10"); library = expect_library("health = 15000");
    /* Failed local refresh also retains both exact prior providers. */
    create("overrides/local/package.json", "malformed external edit");
    const sh_package_compilation *previous = sh_package_runtime_acquire(); sh_package_runtime_release();
    CHECK(!sh_package_runtime_refresh(root));
    const sh_package_compilation *after = sh_package_runtime_acquire(); CHECK(after == previous); sh_package_runtime_release();
    CHECK(expect_library("health = 15000") == library); expect_current(health_path, "health = 10");
    state.next_health = "health = 15000"; state.previous_health = "health = 10";
    state.library_health = "health = 15000"; state.map_active = 0;
    state.expected = map_b_changes;
    CHECK(switch_map(NULL, &state)); CHECK(!sh_package_runtime_has_map_provider());
    expect_current(health_path, "health = 15000");
    create("overrides/local/package.json", descriptor);
    test_prepared_map(map_a);
    test_committed_install_joins_library();
    test_installed_inventory_is_not_active_composition();
    cleanup(); if (failures) return 1;
    puts("package runtime map context tests passed"); return 0;
}
