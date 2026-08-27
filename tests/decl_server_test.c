/* decl_server_test.c -- decl identity, ordering, validation, and walk tests. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "decl_server.h"
#include "decl_server_path.h"
#include "decl_text.h"
#include "overrides.h"

/* The production command notifies the palette one-shot after the final
 * native registration pass. This unit test exercises decl-server helpers
 * without linking that engine-facing service. */
int sh_palette_refresh_after_decl_registration(void)
{
    return 1;
}

static int g_failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

static char g_last_log[512];
static int g_log_count;

void backend_log(const char *message)
{
    g_log_count++;
    strcpy_s(g_last_log, sizeof(g_last_log), message ? message : "");
}

/* The decl server arms these after a successful palette rebuild. Neither may
 * affect its outcome, so the seam records the calls and always succeeds. */
static int g_visibility_installs;

int sh_decl_visibility_install(const unsigned char *module_base,
                               const char *existing_probe_path,
                               const char *absent_probe_path)
{
    (void)module_base; (void)existing_probe_path; (void)absent_probe_path;
    g_visibility_installs++;
    return 1;
}

/* Publication is triggered by a one-shot detour on the engine's whole-registry
 * resource promotion, and applies the package requirement cvars synchronously
 * just before it. Neither reaches an engine in this unit test, so both seams
 * record nothing and report the outcome that keeps sh_decl_server_install on
 * its refusal path. */
void *install_inline_hook(void *target, void *detour, size_t stolen)
{
    (void)target; (void)detour; (void)stolen;
    return NULL;
}

int sh_package_requirements_apply_now(void *execute_command_buffer)
{
    (void)execute_command_buffer;
    return 1;
}

int sh_overrides_get_root(char *out, size_t cap)
{
    if (out && cap) out[0] = '\0';
    return 0;
}

int sh_overrides_internal_decl_table_can_install(void)
{
    return 0;
}

int sh_overrides_internal_decl_table_install(
    const sh_overrides_internal_decl_entry *entries, size_t count)
{
    (void)entries;
    (void)count;
    return 0;
}

int sh_user_overrides_enabled_for_launch(void)
{
    return 0;
}

/* The runtime re-arm re-scans the override package list before publishing identities, so the
 * file shadow can serve the new package's bytes. This test links decl_server WITHOUT
 * overrides.c, matching the stubs above; the re-arm path is exercised live, not here. */
unsigned long sh_overrides_rescan_packages(void)
{
    return 0;
}

/* Same reason: the re-arm recaptures the resource-bridge manifests so a mid-session package's
 * linked game-owned entries resolve. Not linked here. */
int sh_resource_bridge_recapture(const char *data_root)
{
    (void)data_root;
    return 0;
}

/* The re-arm retires the internal decl table so it can be re-published. Not linked here. */
void sh_overrides_internal_decl_table_reopen(void)
{
}

int sh_overrides_internal_decl_table_merge(
    const sh_overrides_internal_decl_entry *entries, size_t count)
{
    (void)entries;
    (void)count;
    return 0;
}

/* The re-arm applies the package's cut-content gates before registering. Not linked here. */
int sh_package_requirements_rearm(const char *data_root, void *execute_command_buffer,
                                  int user_layer_enabled)
{
    (void)data_root; (void)execute_command_buffer; (void)user_layer_enabled;
    return 0;
}

int sh_resource_bridge_gate_ok(void)
{
    return 1;
}

size_t sh_resource_bridge_decl_count(void)
{
    return 0;
}

int sh_resource_bridge_decl_metadata(size_t index, const char **type,
                                     const char **name, const char **source)
{
    (void)index; (void)type; (void)name; (void)source;
    return 0;
}

int sh_resource_bridge_read_decl(size_t index, char **body, size_t *length,
                                 const char **reason)
{
    (void)index; (void)body; (void)length; (void)reason;
    return 0;
}

uintptr_t sig_addr_by_name(const sig_result *results, size_t count, const char *name)
{
    (void)results;
    (void)count;
    (void)name;
    return 0;
}

typedef struct find_step {
    int found;
    DWORD error;
    const char *name;
    DWORD attributes;
    uintptr_t handle;
} find_step;

static find_step g_first_steps[8];
static find_step g_next_steps[8];
static size_t g_first_count;
static size_t g_first_index;
static size_t g_next_count;
static size_t g_next_index;
static DWORD g_find_error;
static int g_close_count;

static void set_found_data(LPWIN32_FIND_DATAA found, const find_step *step)
{
    memset(found, 0, sizeof(*found));
    found->dwFileAttributes = step->attributes;
    strcpy_s(found->cFileName, sizeof(found->cFileName), step->name ? step->name : "");
}

static HANDLE WINAPI scripted_find_first(LPCSTR pattern, LPWIN32_FIND_DATAA found)
{
    const find_step *step;
    (void)pattern;
    if (g_first_index >= g_first_count) {
        g_find_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    step = &g_first_steps[g_first_index++];
    g_find_error = step->error;
    if (!step->found) return INVALID_HANDLE_VALUE;
    set_found_data(found, step);
    return (HANDLE)(uintptr_t)(step->handle ? step->handle : g_first_index);
}

static BOOL WINAPI scripted_find_next(HANDLE search, LPWIN32_FIND_DATAA found)
{
    const find_step *step;
    (void)search;
    if (g_next_index >= g_next_count) {
        g_find_error = ERROR_NO_MORE_FILES;
        return FALSE;
    }
    step = &g_next_steps[g_next_index++];
    g_find_error = step->error;
    if (!step->found) return FALSE;
    set_found_data(found, step);
    return TRUE;
}

static BOOL WINAPI scripted_find_close(HANDLE search)
{
    (void)search;
    g_close_count++;
    return TRUE;
}

static DWORD WINAPI scripted_get_last_error(void)
{
    return g_find_error;
}

static void reset_find_script(void)
{
    sh_decl_server_test_find_api api;
    memset(g_first_steps, 0, sizeof(g_first_steps));
    memset(g_next_steps, 0, sizeof(g_next_steps));
    g_first_count = 0;
    g_first_index = 0;
    g_next_count = 0;
    g_next_index = 0;
    g_find_error = ERROR_SUCCESS;
    g_close_count = 0;
    g_log_count = 0;
    g_last_log[0] = '\0';
    api.find_first = scripted_find_first;
    api.find_next = scripted_find_next;
    api.find_close = scripted_find_close;
    api.get_last_error = scripted_get_last_error;
    sh_decl_server_test_set_find_api(&api);
}

enum {
    BOUNDARY_CTOR = 1,
    BOUNDARY_SCAN = 2,
    BOUNDARY_DTOR = 3
};

static int g_boundary_order[4];
static int g_boundary_count;
static int g_boundary_ctor_throws;
static int g_boundary_scan_throws;
static int g_boundary_dtor_throws;
static int g_boundary_scan_calls;
static int g_boundary_dtor_calls;
static const char *g_boundary_ctor_source;
static const void *g_boundary_scan_source;
static void *g_boundary_registry;
static int g_boundary_scan_result;

static void boundary_reset(void)
{
    memset(g_boundary_order, 0, sizeof(g_boundary_order));
    g_boundary_count = 0;
    g_boundary_ctor_throws = 0;
    g_boundary_scan_throws = 0;
    g_boundary_dtor_throws = 0;
    g_boundary_scan_calls = 0;
    g_boundary_dtor_calls = 0;
    g_boundary_ctor_source = NULL;
    g_boundary_scan_source = NULL;
    g_boundary_registry = NULL;
    g_boundary_scan_result = 1;
}

static void *boundary_ctor(void *self, const char *source)
{
    g_boundary_order[g_boundary_count++] = BOUNDARY_CTOR;
    g_boundary_ctor_source = source;
    if (g_boundary_ctor_throws)
        RaiseException(0xE000D501u, EXCEPTION_NONCONTINUABLE, 0, NULL);
    memset(self, 0x5A, 0x30);
    return self;
}

static unsigned char boundary_scan(void *registry, const void *source,
                                   void *default_type_manager)
{
    (void)default_type_manager;
    g_boundary_order[g_boundary_count++] = BOUNDARY_SCAN;
    g_boundary_scan_calls++;
    g_boundary_registry = registry;
    g_boundary_scan_source = source;
    if (g_boundary_scan_throws)
        RaiseException(0xE000D502u, EXCEPTION_NONCONTINUABLE, 0, NULL);
    return (unsigned char)g_boundary_scan_result;
}

static void boundary_dtor(void *self)
{
    (void)self;
    g_boundary_order[g_boundary_count++] = BOUNDARY_DTOR;
    g_boundary_dtor_calls++;
    if (g_boundary_dtor_throws)
        RaiseException(0xE000D503u, EXCEPTION_NONCONTINUABLE, 0, NULL);
}

static void test_native_idstr_boundary(void)
{
    void *registry = (void *)(uintptr_t)0x12345678u;
    const char *source_name = "actormodifier/custom/first.decl";
    int status;

    boundary_reset();
    status = sh_decl_server_test_register_candidate(registry, source_name,
                                                    boundary_ctor, boundary_dtor,
                                                    boundary_scan);
    CHECK(status == 1);
    CHECK(g_boundary_count == 3);
    CHECK(g_boundary_order[0] == BOUNDARY_CTOR);
    CHECK(g_boundary_order[1] == BOUNDARY_SCAN);
    CHECK(g_boundary_order[2] == BOUNDARY_DTOR);
    CHECK(g_boundary_scan_calls == 1);
    CHECK(g_boundary_dtor_calls == 1);
    CHECK(g_boundary_ctor_source != NULL);
    CHECK(strcmp(g_boundary_ctor_source, source_name) == 0);
    CHECK(g_boundary_registry == registry);
    CHECK(g_boundary_scan_source != NULL);
    CHECK(g_boundary_scan_source != (const void *)source_name);

    boundary_reset();
    g_boundary_scan_result = 0;
    status = sh_decl_server_test_register_candidate(registry, source_name,
                                                    boundary_ctor, boundary_dtor,
                                                    boundary_scan);
    CHECK(status == -2);
    CHECK(g_boundary_scan_calls == 1);
    CHECK(g_boundary_dtor_calls == 1);
    CHECK(g_boundary_count == 3);

    boundary_reset();
    g_boundary_scan_throws = 1;
    status = sh_decl_server_test_register_candidate(registry, source_name,
                                                    boundary_ctor, boundary_dtor,
                                                    boundary_scan);
    CHECK(status == -1);
    CHECK(g_boundary_scan_calls == 1);
    CHECK(g_boundary_dtor_calls == 1);
    CHECK(g_boundary_count == 3);

    boundary_reset();
    g_boundary_ctor_throws = 1;
    status = sh_decl_server_test_register_candidate(registry, source_name,
                                                    boundary_ctor, boundary_dtor,
                                                    boundary_scan);
    CHECK(status == 0);
    CHECK(g_boundary_count == 1);
    CHECK(g_boundary_scan_calls == 0);
    CHECK(g_boundary_dtor_calls == 0);

    boundary_reset();
    g_boundary_dtor_throws = 1;
    status = sh_decl_server_test_register_candidate(registry, source_name,
                                                    boundary_ctor, boundary_dtor,
                                                    boundary_scan);
    CHECK(status == -3);
    CHECK(g_boundary_scan_calls == 1);
    CHECK(g_boundary_dtor_calls == 1);
    CHECK(g_boundary_count == 3);
}

static int g_classify_type_calls;
static int g_classify_source_calls;
static int g_classify_find_calls;
static int g_classify_type_null;
static int g_classify_source_throws;
static int g_classify_find_throws;
static void *g_classify_manager;
static void *g_classify_source_record;
static void *g_classify_live_decl;

static void classify_reset(void)
{
    g_classify_type_calls = 0;
    g_classify_source_calls = 0;
    g_classify_find_calls = 0;
    g_classify_type_null = 0;
    g_classify_source_throws = 0;
    g_classify_find_throws = 0;
    g_classify_manager = (void *)(uintptr_t)0x12340000u;
    g_classify_source_record = NULL;
    g_classify_live_decl = NULL;
}

static void *classify_type_by_name(void *registry, const char *short_name)
{
    (void)registry;
    (void)short_name;
    g_classify_type_calls++;
    return g_classify_type_null ? NULL : g_classify_manager;
}

static void *classify_source_find(void *type_manager, const char *logical_name)
{
    (void)type_manager;
    (void)logical_name;
    g_classify_source_calls++;
    if (g_classify_source_throws)
        RaiseException(0xE000D506u, EXCEPTION_NONCONTINUABLE, 0, NULL);
    return g_classify_source_record;
}

static void *classify_find_decl(void *type_manager, const char *logical_name,
                                unsigned char make_default)
{
    (void)type_manager;
    (void)logical_name;
    g_classify_find_calls++;
    CHECK(make_default == 0);
    if (g_classify_find_throws)
        RaiseException(0xE000D507u, EXCEPTION_NONCONTINUABLE, 0, NULL);
    return g_classify_live_decl;
}

static int classify_one(void)
{
    return sh_decl_server_test_classify_candidate(
        "snapEditorEntityDef", "custom/entity",
        (void *)(uintptr_t)0x98760000u, classify_type_by_name,
        classify_source_find, classify_find_decl);
}

static void test_source_first_classification(void)
{
    classify_reset();
    g_classify_source_record = (void *)(uintptr_t)0x11110000u;
    CHECK(classify_one() == SH_DECL_SERVER_TEST_CLASSIFY_SHADOWED_SOURCE);
    CHECK(g_classify_type_calls == 1);
    CHECK(g_classify_source_calls == 1);
    CHECK(g_classify_find_calls == 0);

    classify_reset();
    g_classify_live_decl = (void *)(uintptr_t)0x22220000u;
    CHECK(classify_one() == SH_DECL_SERVER_TEST_CLASSIFY_SHADOWED_LIVE);
    CHECK(g_classify_type_calls == 1);
    CHECK(g_classify_source_calls == 1);
    CHECK(g_classify_find_calls == 1);

    classify_reset();
    CHECK(classify_one() == SH_DECL_SERVER_TEST_CLASSIFY_MISSING);
    CHECK(g_classify_type_calls == 1);
    CHECK(g_classify_source_calls == 1);
    CHECK(g_classify_find_calls == 1);

    classify_reset();
    g_classify_source_throws = 1;
    CHECK(classify_one() == SH_DECL_SERVER_TEST_CLASSIFY_TERMINAL);
    CHECK(g_classify_type_calls == 1);
    CHECK(g_classify_source_calls == 1);
    CHECK(g_classify_find_calls == 0);

    classify_reset();
    g_classify_find_throws = 1;
    CHECK(classify_one() == SH_DECL_SERVER_TEST_CLASSIFY_TERMINAL);
    CHECK(g_classify_type_calls == 1);
    CHECK(g_classify_source_calls == 1);
    CHECK(g_classify_find_calls == 1);

    classify_reset();
    g_classify_type_null = 1;
    CHECK(classify_one() == SH_DECL_SERVER_TEST_CLASSIFY_REFUSED_TYPE);
    CHECK(g_classify_type_calls == 1);
    CHECK(g_classify_source_calls == 0);
    CHECK(g_classify_find_calls == 0);
}

static int g_materialize_type_calls;
static int g_materialize_find_calls;
static int g_materialize_make_default;
static int g_materialize_type_raises;
static int g_materialize_source_missing;
static int g_materialize_source_raises;
static int g_materialize_find_raises;
static void *g_materialize_manager;
static void *g_materialize_entity_manager;
static void *g_materialize_decl;
static void *g_materialize_decl_null_entity;
static const char *g_materialize_null_entity_name;

static void materialize_reset(void)
{
    g_materialize_type_calls = 0;
    g_materialize_find_calls = 0;
    g_materialize_make_default = -1;
    g_materialize_type_raises = 0;
    g_materialize_source_missing = 0;
    g_materialize_source_raises = 0;
    g_materialize_find_raises = 0;
    g_materialize_manager = (void *)(uintptr_t)0x12340000u;
    g_materialize_entity_manager = (void *)(uintptr_t)0x12350000u;
    g_materialize_decl = NULL;
    g_materialize_decl_null_entity = NULL;
    g_materialize_null_entity_name = NULL;
}

/* The native palette validator reads target arrays at +0x440..+0x460, so every
 * fake decl object in this suite is at least that large and zeroed. */
#define DS_TEST_DECL_BYTES 0x500

static void *materialize_type_by_name(void *registry, const char *short_name)
{
    (void)registry;
    g_materialize_type_calls++;
    if (g_materialize_type_raises)
        RaiseException(0xE000D504u, EXCEPTION_NONCONTINUABLE, 0, NULL);
    if (!short_name) return NULL;
    if (_stricmp(short_name, "entityDef") == 0)
        return g_materialize_entity_manager;
    return g_materialize_manager;
}

static void *materialize_source_find(void *type_manager, const char *logical_name)
{
    (void)type_manager;
    (void)logical_name;
    if (g_materialize_source_raises)
        RaiseException(0xE000D503u, EXCEPTION_NONCONTINUABLE, 0, NULL);
    return g_materialize_source_missing ? NULL : (void *)(uintptr_t)0x12360000u;
}

static void *materialize_find_decl(void *type_manager, const char *logical_name,
                                   unsigned char make_default)
{
    g_materialize_find_calls++;
    g_materialize_make_default = (int)make_default;
    if (g_materialize_find_raises)
        RaiseException(0xE000D505u, EXCEPTION_NONCONTINUABLE, 0, NULL);
    if (g_materialize_null_entity_name &&
        logical_name && strcmp(logical_name, g_materialize_null_entity_name) == 0)
        return g_materialize_decl_null_entity;
    if (!make_default && type_manager == g_materialize_entity_manager &&
        logical_name && strcmp(logical_name, "snapmaps/test/entity") == 0)
        return g_materialize_decl;
    if (!make_default && type_manager == g_materialize_manager &&
        logical_name && strcmp(logical_name, "snapmaps/test/base") == 0)
        return g_materialize_decl;
    if (!make_default) return NULL;
    return g_materialize_decl;
}

static int run_materialize_case(const sh_decl_server_test_materialize_item *items,
                                size_t count, int expected_ok,
                                int expected_materialized)
{
    int materialized = -1;
    int ok = sh_decl_server_test_materialize_missing_sedefs(
        items, count, (void *)(uintptr_t)0x98760000u,
        materialize_type_by_name, materialize_source_find,
        materialize_find_decl, &materialized);
    CHECK(ok == expected_ok);
    CHECK(materialized == expected_materialized);
    return ok;
}

static void test_sedef_materialization(void)
{
    static const unsigned char eligible_body[] =
        "{ edit = { entityDef = \"snapmaps/test/entity\"; } }";
    static const unsigned char inherited_body[] =
        "{ inherit = \"snapmaps/test/base\"; edit = { displayNameTag = \"Inherited\"; } }";
    static const unsigned char source_only_body[] =
        "{ edit = { displayNameTag = \"Source only\"; } }";
    unsigned char *decl_memory;
    unsigned char *abstract_memory;
    unsigned char *target_memory;
    sh_decl_server_test_materialize_item mixed[] = {
        { "SNAPEDITORENTITYDEF", "new/entity", "generated/decls/sedef/new/entity.decl",
          SH_DECL_SERVER_TEST_MISSING, eligible_body, sizeof(eligible_body) - 1 },
        { "material", "not-materialized", "generated/decls/material/not-materialized.decl",
          SH_DECL_SERVER_TEST_MISSING },
        { "snapEditorEntityDef", "already-there", "generated/decls/sedef/already-there.decl",
          SH_DECL_SERVER_TEST_SHADOWED }
    };
    sh_decl_server_test_materialize_item one[] = {
        { "snapEditorEntityDef", "new/entity", "generated/decls/sedef/new/entity.decl",
          SH_DECL_SERVER_TEST_MISSING, eligible_body, sizeof(eligible_body) - 1 }
    };
    sh_decl_server_test_materialize_item source_only[] = {
        { "snapEditorEntityDef", "source-only", "generated/decls/sedef/source-only.decl",
          SH_DECL_SERVER_TEST_MISSING, source_only_body, sizeof(source_only_body) - 1 }
    };

    decl_memory = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    CHECK(decl_memory != NULL);
    if (!decl_memory) return;
    abstract_memory = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    target_memory = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    CHECK(abstract_memory != NULL);
    CHECK(target_memory != NULL);
    if (!abstract_memory || !target_memory) {
        if (abstract_memory) HeapFree(GetProcessHeap(), 0, abstract_memory);
        if (target_memory) HeapFree(GetProcessHeap(), 0, target_memory);
        HeapFree(GetProcessHeap(), 0, decl_memory);
        return;
    }

    /* Every registered identity is materialized, non-editor types first, and
     * the one eligible editor entity last. */
    materialize_reset();
    g_materialize_decl = decl_memory;
    decl_memory[0x2c] = 0x04;
    *(void **)(decl_memory + 0x1c8) = decl_memory + 0x200;
    run_materialize_case(mixed, sizeof(mixed) / sizeof(mixed[0]), 1, 2);
    CHECK(g_materialize_type_calls == 2);
    CHECK(g_materialize_find_calls == 4);
    CHECK(g_materialize_make_default == 1);

    /* The native palette validator rejects an editor entity whose entityDef is
     * unresolved, so this service refuses it before the rebuild. */
    materialize_reset();
    g_materialize_decl = abstract_memory;
    abstract_memory[0x2c] = 0x04;
    *(void **)(abstract_memory + 0x1c8) = NULL;
    run_materialize_case(one, 1, 0, 0);

    /* The generic valid bit is not a native admission condition. An object the
     * engine never flagged 0x04 is still admitted when it satisfies the
     * validator contract; only the in-progress bit is disqualifying. */
    materialize_reset();
    g_materialize_decl = decl_memory;
    decl_memory[0x2c] = 0x00;
    *(void **)(decl_memory + 0x1c8) = decl_memory + 0x200;
    run_materialize_case(one, 1, 1, 1);

    materialize_reset();
    g_materialize_decl = decl_memory;
    decl_memory[0x2c] = 0x01;
    run_materialize_case(one, 1, 0, 0);
    decl_memory[0x2c] = 0x04;

    /* Resolved targets must carry the direction flags the native validator
     * requires: outputs 0x20, inputs 0x10. */
    {
        void *target_array[1];
        target_array[0] = target_memory;
        materialize_reset();
        g_materialize_decl = decl_memory;
        *(void **)(decl_memory + 0x440) = target_array;
        *(int *)(decl_memory + 0x448) = 1;
        target_memory[0x3cd] = 0x10;
        run_materialize_case(one, 1, 0, 0);
        target_memory[0x3cd] = 0x20;
        run_materialize_case(one, 1, 1, 1);
        *(void **)(decl_memory + 0x458) = target_array;
        *(int *)(decl_memory + 0x460) = 1;
        run_materialize_case(one, 1, 0, 0);
        target_memory[0x3cd] = 0x30;
        run_materialize_case(one, 1, 1, 1);
        target_array[0] = NULL;
        run_materialize_case(one, 1, 0, 0);
        *(int *)(decl_memory + 0x448) = 0;
        *(int *)(decl_memory + 0x460) = 0;
    }

    /* A source-only editor body is never materialized at all. */
    materialize_reset();
    g_materialize_decl = decl_memory;
    run_materialize_case(source_only, 1, 1, 0);
    CHECK(g_materialize_type_calls == 0);
    CHECK(g_materialize_find_calls == 0);
    CHECK(g_materialize_make_default == -1);

    materialize_reset();
    g_materialize_manager = NULL;
    g_materialize_decl = decl_memory;
    run_materialize_case(one, 1, 0, 0);
    CHECK(g_materialize_type_calls == 1);
    CHECK(g_materialize_find_calls == 0);

    materialize_reset();
    g_materialize_decl = decl_memory;
    g_materialize_type_raises = 1;
    run_materialize_case(one, 1, 0, 0);
    CHECK(g_materialize_type_calls == 1);
    CHECK(g_materialize_find_calls == 0);

    materialize_reset();
    g_materialize_decl = NULL;
    run_materialize_case(one, 1, 0, 0);
    CHECK(g_materialize_type_calls == 1);
    CHECK(g_materialize_find_calls == 2);

    materialize_reset();
    g_materialize_decl = decl_memory;
    g_materialize_find_raises = 1;
    run_materialize_case(one, 1, 0, 0);
    CHECK(g_materialize_find_calls == 1);
    CHECK(g_materialize_make_default == 0);

    {
        SYSTEM_INFO system_info;
        DWORD old_protect = 0;
        void *page;
        GetSystemInfo(&system_info);
        page = VirtualAlloc(NULL, system_info.dwPageSize,
                            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        CHECK(page != NULL);
        if (page) {
            CHECK(VirtualProtect(page, system_info.dwPageSize, PAGE_NOACCESS,
                                 &old_protect) != 0);
            materialize_reset();
            g_materialize_decl = page;
            run_materialize_case(one, 1, 0, 0);
            CHECK(VirtualProtect(page, system_info.dwPageSize, old_protect,
                                 &old_protect) != 0);
            CHECK(VirtualFree(page, 0, MEM_RELEASE) != 0);
        }
    }

    (void)inherited_body;
    HeapFree(GetProcessHeap(), 0, target_memory);
    HeapFree(GetProcessHeap(), 0, abstract_memory);
    HeapFree(GetProcessHeap(), 0, decl_memory);
}

static char g_pipeline_events[32];
static int g_pipeline_event_count;
static int g_pipeline_scan_count;
static int g_pipeline_scan_fail_on;
static int g_pipeline_find_fail;
static int g_pipeline_find_calls;
static int g_pipeline_palette_calls;
static int g_pipeline_palette_result;
static void *g_pipeline_manager;
static void *g_pipeline_entity_manager;
static void *g_pipeline_decl;

static void pipeline_reset(void)
{
    memset(g_pipeline_events, 0, sizeof(g_pipeline_events));
    g_pipeline_event_count = 0;
    g_pipeline_scan_count = 0;
    g_pipeline_scan_fail_on = 0;
    g_pipeline_find_fail = 0;
    g_pipeline_find_calls = 0;
    g_pipeline_palette_calls = 0;
    g_pipeline_palette_result = 1;
    g_pipeline_manager = (void *)(uintptr_t)0x22340000u;
    g_pipeline_entity_manager = (void *)(uintptr_t)0x22350000u;
    g_pipeline_decl = NULL;
}

static void pipeline_event(char event)
{
    if (g_pipeline_event_count < (int)sizeof(g_pipeline_events))
        g_pipeline_events[g_pipeline_event_count++] = event;
}

static void *pipeline_ctor(void *self, const char *source)
{
    (void)source;
    pipeline_event('C');
    memset(self, 0x5a, 0x30);
    return self;
}

static void pipeline_dtor(void *self)
{
    (void)self;
    pipeline_event('D');
}

static unsigned char pipeline_register_file(void *registry, const void *source,
                                            void *default_type_manager)
{
    (void)registry;
    (void)source;
    (void)default_type_manager;
    pipeline_event('S');
    g_pipeline_scan_count++;
    return g_pipeline_scan_fail_on == g_pipeline_scan_count ? 0 : 1;
}

static void *pipeline_type_by_name(void *registry, const char *short_name)
{
    (void)registry;
    pipeline_event('T');
    if (!short_name) return NULL;
    if (_stricmp(short_name, "entityDef") == 0)
        return g_pipeline_entity_manager;
    return g_pipeline_manager;
}

static void *pipeline_find_decl(void *type_manager, const char *logical_name,
                                unsigned char make_default)
{
    pipeline_event('F');
    g_pipeline_find_calls++;
    if (!make_default && type_manager == g_pipeline_entity_manager)
        return g_pipeline_decl;
    if (!make_default && type_manager == g_pipeline_manager && logical_name &&
        strcmp(logical_name, "snapmaps/test/base") == 0)
        return g_pipeline_decl;
    if (!make_default) return NULL;
    return g_pipeline_find_fail ? NULL : g_pipeline_decl;
}

static void *pipeline_source_find(void *type_manager, const char *logical_name)
{
    (void)type_manager;
    (void)logical_name;
    return (void *)(uintptr_t)0x22360000u;
}

static int pipeline_palette_refresh(void)
{
    pipeline_event('P');
    g_pipeline_palette_calls++;
    return g_pipeline_palette_result;
}

static void test_integrated_scan_materialize_pipeline(void)
{
    static const unsigned char eligible_body[] =
        "{ edit = { entityDef = \"snapmaps/test/entity\"; } }";
    unsigned char *decl_memory;
    int registered;
    int materialized;
    int failure_phase;
    int ok;
    sh_decl_server_test_materialize_item items[] = {
        { "snapEditorEntityDef", "entity-a", "generated/decls/snapeditorentitydef/entity-a.decl",
          SH_DECL_SERVER_TEST_MISSING, eligible_body, sizeof(eligible_body) - 1 },
        { "material", "entity-b", "generated/decls/material/entity-b.decl",
          SH_DECL_SERVER_TEST_MISSING },
        { "snapEditorEntityDef", "entity-shadowed", "generated/decls/snapeditorentitydef/entity-shadowed.decl",
          SH_DECL_SERVER_TEST_SHADOWED }
    };

    decl_memory = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    CHECK(decl_memory != NULL);
    if (!decl_memory) return;
    decl_memory[0x2c] = 0x04;
    *(void **)(decl_memory + 0x1c8) = decl_memory + 0x200;

    /* Every source is scanned before the first materialization, non-editor
     * identities are materialized before the editor entity, and exactly one
     * palette rebuild closes the pass. */
    pipeline_reset();
    g_pipeline_decl = decl_memory;
    ok = sh_decl_server_test_scan_and_materialize_missing(
        items, sizeof(items) / sizeof(items[0]),
        (void *)(uintptr_t)0x98760000u, pipeline_type_by_name,
        pipeline_source_find,
        pipeline_find_decl, pipeline_palette_refresh, pipeline_ctor,
        pipeline_dtor, pipeline_register_file, &registered, &materialized,
        &failure_phase);
    CHECK(ok == 1);
    CHECK(registered == 2);
    CHECK(materialized == 2);
    CHECK(failure_phase == SH_DECL_SERVER_TEST_PHASE_NONE);
    CHECK(g_pipeline_palette_calls == 1);
    CHECK(g_pipeline_event_count == 13);
    CHECK(memcmp(g_pipeline_events, "CSDCSDTFFTFFP", 13) == 0);
    CHECK(sh_decl_server_registration_succeeded() == 0);

    pipeline_reset();
    g_pipeline_decl = decl_memory;
    g_pipeline_scan_fail_on = 2;
    ok = sh_decl_server_test_scan_and_materialize_missing(
        items, sizeof(items) / sizeof(items[0]),
        (void *)(uintptr_t)0x98760000u, pipeline_type_by_name,
        pipeline_source_find,
        pipeline_find_decl, pipeline_palette_refresh, pipeline_ctor,
        pipeline_dtor, pipeline_register_file, &registered, &materialized,
        &failure_phase);
    CHECK(ok == 0);
    CHECK(registered == 1);
    CHECK(materialized == 0);
    CHECK(failure_phase == SH_DECL_SERVER_TEST_PHASE_SCAN);
    CHECK(g_pipeline_palette_calls == 0);
    CHECK(g_pipeline_event_count == 6);
    CHECK(memcmp(g_pipeline_events, "CSDCSD", 6) == 0);

    pipeline_reset();
    g_pipeline_decl = NULL;
    ok = sh_decl_server_test_scan_and_materialize_missing(
        items, sizeof(items) / sizeof(items[0]),
        (void *)(uintptr_t)0x98760000u, pipeline_type_by_name,
        pipeline_source_find,
        pipeline_find_decl, pipeline_palette_refresh, pipeline_ctor,
        pipeline_dtor, pipeline_register_file, &registered, &materialized,
        &failure_phase);
    CHECK(ok == 0);
    CHECK(registered == 2);
    CHECK(materialized == 0);
    CHECK(failure_phase == SH_DECL_SERVER_TEST_PHASE_MATERIALIZATION);
    CHECK(g_pipeline_palette_calls == 0);
    CHECK(g_pipeline_event_count == 9);
    CHECK(memcmp(g_pipeline_events, "CSDCSDTFF", 9) == 0);
    CHECK(sh_decl_server_registration_succeeded() == 0);

    pipeline_reset();
    g_pipeline_decl = decl_memory;
    g_pipeline_palette_result = 0;
    ok = sh_decl_server_test_scan_and_materialize_missing(
        items, sizeof(items) / sizeof(items[0]),
        (void *)(uintptr_t)0x98760000u, pipeline_type_by_name,
        pipeline_source_find,
        pipeline_find_decl, pipeline_palette_refresh, pipeline_ctor,
        pipeline_dtor, pipeline_register_file, &registered, &materialized,
        &failure_phase);
    CHECK(ok == 0);
    CHECK(registered == 2);
    CHECK(materialized == 2);
    CHECK(failure_phase == SH_DECL_SERVER_TEST_PHASE_PALETTE);
    CHECK(g_pipeline_palette_calls == 1);
    CHECK(g_pipeline_event_count == 13);
    CHECK(memcmp(g_pipeline_events, "CSDCSDTFFTFFP", 13) == 0);
    CHECK(sh_decl_server_registration_succeeded() == 0);

    HeapFree(GetProcessHeap(), 0, decl_memory);
}

static void test_source_only_pipeline_gate(void)
{
    static const unsigned char source_only_body[] =
        "{ edit = { displayNameTag = \"Source only\"; } }";
    static const unsigned char inherited_body[] =
        "{ inherit = \"snapmaps/test/base\"; edit = { displayNameTag = \"Inherited\"; } }";
    unsigned char *decl_memory;
    int registered;
    int materialized;
    int failure_phase;
    int ok;
    sh_decl_server_test_materialize_item items[] = {
        { "snapEditorEntityDef", "source-only", "generated/decls/snapeditorentitydef/source-only.decl",
          SH_DECL_SERVER_TEST_MISSING, source_only_body, sizeof(source_only_body) - 1 },
        { "snapEditorEntityDef", "inherited", "generated/decls/snapeditorentitydef/inherited.decl",
          SH_DECL_SERVER_TEST_MISSING, inherited_body, sizeof(inherited_body) - 1 }
    };

    decl_memory = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    CHECK(decl_memory != NULL);
    if (!decl_memory) return;
    decl_memory[0x2c] = 0x04;
    *(void **)(decl_memory + 0x1c8) = decl_memory + 0x200;

    /* A source-only body is still registered as a source, but it is never
     * materialized and never reaches the palette. */
    pipeline_reset();
    g_pipeline_decl = decl_memory;
    ok = sh_decl_server_test_scan_and_materialize_missing(
        items, sizeof(items) / sizeof(items[0]),
        (void *)(uintptr_t)0x98760000u, pipeline_type_by_name,
        pipeline_source_find,
        pipeline_find_decl, pipeline_palette_refresh, pipeline_ctor,
        pipeline_dtor, pipeline_register_file, &registered, &materialized,
        &failure_phase);
    CHECK(ok == 1);
    CHECK(registered == 2);
    CHECK(materialized == 1);
    CHECK(failure_phase == SH_DECL_SERVER_TEST_PHASE_NONE);
    CHECK(g_pipeline_find_calls == 2);
    CHECK(g_pipeline_palette_calls == 1);
    CHECK(g_pipeline_event_count == 10);
    CHECK(memcmp(g_pipeline_events, "CSDCSDTFFP", 10) == 0);

    HeapFree(GetProcessHeap(), 0, decl_memory);
}

static void *g_closure_sedef_manager;
static void *g_closure_entity_manager;
static void *g_closure_decl;
static const char *g_closure_invalid_name;
static void *g_closure_invalid_decl;
static char g_closure_make_names[8][SH_DECL_SERVER_NAME_CAP];
static int g_closure_make_count;

static void *closure_type_by_name(void *registry, const char *short_name)
{
    (void)registry;
    if (!short_name) return NULL;
    if (_stricmp(short_name, "snapEditorEntityDef") == 0)
        return g_closure_sedef_manager;
    if (_stricmp(short_name, "entityDef") == 0)
        return g_closure_entity_manager;
    return g_closure_entity_manager;
}

static void *closure_find_decl(void *type_manager, const char *logical_name,
                               unsigned char make_default)
{
    if (!type_manager || !logical_name) return NULL;
    if (g_closure_invalid_name &&
        strcmp(logical_name, g_closure_invalid_name) == 0)
        return make_default ? g_closure_invalid_decl : g_closure_invalid_decl;
    if (!make_default &&
        (strcmp(logical_name, "demons/demon_base") == 0 ||
         strcmp(logical_name, "ai/demon/cyberdemon_base") == 0))
        return g_closure_decl;
    if (!make_default) return NULL;
    if (g_closure_make_count < 8)
        strcpy_s(g_closure_make_names[g_closure_make_count],
                 sizeof(g_closure_make_names[0]), logical_name);
    g_closure_make_count++;
    return g_closure_decl;
}

static void *closure_source_find(void *type_manager, const char *logical_name)
{
    (void)type_manager;
    (void)logical_name;
    return (void *)(uintptr_t)0x33300000u;
}

/* The Cyberdemon-shaped package is the integration fixture for the generic
 * rule: every registered identity gets a live object in its own manager, in
 * registration order, non-editor types first. Identities the installation
 * already owns -- source or live shadows -- are never synthesized, which is the
 * exact regression that made an existing editor parent terminal. */
static void test_cyber_shaped_registration_order(void)
{
    static const unsigned char root_body[] =
        "{ inherit = \"demons/demon_base\"; edit = {"
        " entityDef = \"snapmaps/placeablesnapaiencounter/cyberdemon\";"
        " buildGameRefEntityDefs = { num = 1; item[0] = \"ai/demon/cyberdemon_hell\"; }"
        " } }";
    static const unsigned char cyber_body[] =
        "{ inherit = \"ai/demon/cyberdemon_base\"; }";
    static const unsigned char source_only_body[] =
        "{ edit = { displayNameTag = \"source only\"; } }";
    unsigned char *decl_memory;
    int materialized = -1;
    int i;
    sh_decl_server_test_materialize_item items[] = {
        { "snapEditorEntityDef", "demons/cyberdemon_enc", "root",
          SH_DECL_SERVER_TEST_MISSING, root_body, sizeof(root_body) - 1 },
        { "snapEditorEntityDef", "demons/demon_base", "base",
          SH_DECL_SERVER_TEST_SHADOWED, NULL, 0,
          SH_DECL_SERVER_TEST_SHADOW_LIVE },
        { "entityDef", "snapmaps/placeablesnapaiencounter/cyberdemon", "placeable",
          SH_DECL_SERVER_TEST_SHADOWED, NULL, 0,
          SH_DECL_SERVER_TEST_SHADOW_SOURCE },
        { "entityDef", "ai/demon/cyberdemon_hell", "cyber",
          SH_DECL_SERVER_TEST_MISSING, cyber_body, sizeof(cyber_body) - 1 },
        { "entityDef", "ai/demon/cyberdemon_base", "entity-base",
          SH_DECL_SERVER_TEST_SHADOWED, NULL, 0,
          SH_DECL_SERVER_TEST_SHADOW_LIVE },
        { "material", "unrelated", "unrelated",
          SH_DECL_SERVER_TEST_MISSING, (const unsigned char *)"{ value = 1; }", 14 },
        { "snapEditorEntityDef", "source-only", "source-only",
          SH_DECL_SERVER_TEST_MISSING, source_only_body, sizeof(source_only_body) - 1 }
    };

    decl_memory = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    CHECK(decl_memory != NULL);
    if (!decl_memory) return;
    decl_memory[0x2c] = 0x04;
    *(void **)(decl_memory + 0x1c8) = decl_memory + 0x200;
    g_closure_sedef_manager = (void *)(uintptr_t)0x33000000u;
    g_closure_entity_manager = (void *)(uintptr_t)0x33100000u;
    g_closure_decl = decl_memory;
    g_closure_invalid_name = NULL;
    g_closure_invalid_decl = NULL;
    memset(g_closure_make_names, 0, sizeof(g_closure_make_names));
    g_closure_make_count = 0;
    CHECK(sh_decl_server_test_materialize_missing_sedefs(
              items, sizeof(items) / sizeof(items[0]),
              (void *)(uintptr_t)0x33200000u, closure_type_by_name,
              closure_source_find,
              closure_find_decl, &materialized) == 1);
    CHECK(materialized == 3);
    CHECK(g_closure_make_count == 3);
    if (g_closure_make_count == 3) {
        CHECK(strcmp(g_closure_make_names[0], "ai/demon/cyberdemon_hell") == 0);
        CHECK(strcmp(g_closure_make_names[1], "unrelated") == 0);
        CHECK(strcmp(g_closure_make_names[2], "demons/cyberdemon_enc") == 0);
    }
    /* No shadowed identity and no source-only editor body may be synthesized. */
    for (i = 0; i < g_closure_make_count && i < 8; i++) {
        CHECK(strcmp(g_closure_make_names[i], "demons/demon_base") != 0);
        CHECK(strcmp(g_closure_make_names[i],
                     "snapmaps/placeablesnapaiencounter/cyberdemon") != 0);
        CHECK(strcmp(g_closure_make_names[i], "ai/demon/cyberdemon_base") != 0);
        CHECK(strcmp(g_closure_make_names[i], "source-only") != 0);
    }
    HeapFree(GetProcessHeap(), 0, decl_memory);
}

static void test_materialization_failure_guards(void)
{
    static const unsigned char root_body[] =
        "{ edit = { entityDef = \"entity/invalid-dependency\"; } }";
    static const unsigned char live_shadow_root_body[] =
        "{ edit = { entityDef = \"entity/live-shadow\"; } }";
    unsigned char *valid_memory;
    unsigned char *invalid_memory;
    int materialized = -1;
    int i;
    sh_decl_server_test_materialize_item invalid_items[] = {
        { "snapEditorEntityDef", "root/invalid-dependency", "root-invalid",
          SH_DECL_SERVER_TEST_MISSING, root_body, sizeof(root_body) - 1 },
        { "entityDef", "entity/invalid-dependency", "invalid-dependency",
          SH_DECL_SERVER_TEST_MISSING, (const unsigned char *)"{ }", 3 }
    };
    sh_decl_server_test_materialize_item live_shadow_items[] = {
        { "snapEditorEntityDef", "root/live-shadow", "root-live-shadow",
          SH_DECL_SERVER_TEST_MISSING, live_shadow_root_body,
          sizeof(live_shadow_root_body) - 1 },
        { "entityDef", "entity/live-shadow", "live-shadow",
          SH_DECL_SERVER_TEST_SHADOWED, NULL, 0,
          SH_DECL_SERVER_TEST_SHADOW_LIVE }
    };

    valid_memory = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    invalid_memory = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    CHECK(valid_memory != NULL);
    CHECK(invalid_memory != NULL);
    if (!valid_memory || !invalid_memory) {
        if (valid_memory) HeapFree(GetProcessHeap(), 0, valid_memory);
        if (invalid_memory) HeapFree(GetProcessHeap(), 0, invalid_memory);
        return;
    }
    valid_memory[0x2c] = 0x04;
    *(void **)(valid_memory + 0x1c8) = valid_memory + 0x200;

    /* An identity whose object is still mid-parse is terminal, and the editor
     * root behind it is never reached. */
    invalid_memory[0x2c] = 0x01;
    materialize_reset();
    g_closure_decl = invalid_memory;
    g_closure_invalid_name = NULL;
    g_closure_invalid_decl = NULL;
    memset(g_closure_make_names, 0, sizeof(g_closure_make_names));
    g_closure_make_count = 0;
    CHECK(sh_decl_server_test_materialize_missing_sedefs(
              invalid_items, sizeof(invalid_items) / sizeof(invalid_items[0]),
              (void *)(uintptr_t)0x33200000u, closure_type_by_name,
              closure_source_find, closure_find_decl, &materialized) == 0);
    CHECK(materialized == 0);
    CHECK(g_closure_make_count == 1);
    if (g_closure_make_count == 1)
        CHECK(strcmp(g_closure_make_names[0], "entity/invalid-dependency") == 0);

    /* A shadowed identity belongs to the installation. It is never synthesized,
     * and a root that still cannot satisfy the palette contract is terminal. */
    invalid_memory[0x2c] = 0x04;
    *(void **)(invalid_memory + 0x1c8) = NULL;
    materialize_reset();
    g_closure_decl = invalid_memory;
    memset(g_closure_make_names, 0, sizeof(g_closure_make_names));
    g_closure_make_count = 0;
    CHECK(sh_decl_server_test_materialize_missing_sedefs(
              live_shadow_items,
              sizeof(live_shadow_items) / sizeof(live_shadow_items[0]),
              (void *)(uintptr_t)0x33200000u, closure_type_by_name,
              closure_source_find, closure_find_decl, &materialized) == 0);
    CHECK(materialized == 0);
    for (i = 0; i < g_closure_make_count && i < 8; i++)
        CHECK(strcmp(g_closure_make_names[i], "entity/live-shadow") != 0);

    g_closure_decl = valid_memory;
    g_closure_invalid_name = NULL;
    g_closure_invalid_decl = NULL;
    HeapFree(GetProcessHeap(), 0, invalid_memory);
    HeapFree(GetProcessHeap(), 0, valid_memory);
}

static void expect_path(const char *relative, const char *want_type,
                        const char *want_name, const char *want_source)
{
    char type[SH_DECL_SERVER_TYPE_CAP];
    char name[SH_DECL_SERVER_NAME_CAP];
    char source[SH_DECL_SERVER_SOURCE_CAP];
    const char *reason = NULL;
    int ok = sh_decl_server_identity_from_relative(relative,
                                                    type, sizeof(type),
                                                    name, sizeof(name),
                                                    source, sizeof(source),
                                                    &reason);
    CHECK(ok == 1);
    if (!ok) {
        fprintf(stderr, "  path '%s' refused: %s\n", relative, reason ? reason : "?");
        return;
    }
    CHECK(strcmp(type, want_type) == 0);
    CHECK(strcmp(name, want_name) == 0);
    CHECK(strcmp(source, want_source) == 0);
}

static void refuse_path(const char *relative, const char *reason_fragment)
{
    char type[SH_DECL_SERVER_TYPE_CAP];
    char name[SH_DECL_SERVER_NAME_CAP];
    char source[SH_DECL_SERVER_SOURCE_CAP];
    const char *reason = NULL;
    int ok = sh_decl_server_identity_from_relative(relative,
                                                    type, sizeof(type),
                                                    name, sizeof(name),
                                                    source, sizeof(source),
                                                    &reason);
    CHECK(ok == 0);
    CHECK(reason != NULL);
    if (reason && reason_fragment) CHECK(strstr(reason, reason_fragment) != NULL);
}

static void test_identity_paths(void)
{
    expect_path("actormodifier\\actormodifier\\demon\\cacodemon.decl",
                "actormodifier", "actormodifier/demon/cacodemon",
                "generated/decls/actormodifier/actormodifier/demon/cacodemon.decl");
    expect_path("ActorModifier/actormodifier/demon/pinky.DECL",
                "ActorModifier", "actormodifier/demon/pinky",
                "generated/decls/ActorModifier/actormodifier/demon/pinky.DECL");
    expect_path("material/generated/my-material.v2.decl",
                "material", "generated/my-material.v2",
                "generated/decls/material/generated/my-material.v2.decl");

    refuse_path("cacodemon.decl", "type");
    refuse_path("actormodifier/cacodemon.json", "extension");
    refuse_path("/actormodifier/cacodemon.decl", "absolute");
    refuse_path("C:\\actormodifier\\cacodemon.decl", "absolute");
    refuse_path("actormodifier//cacodemon.decl", "empty");
    refuse_path("actormodifier/../cacodemon.decl", "traversal");
    refuse_path("actormodifier/./cacodemon.decl", "traversal");
    refuse_path("actor modifier/cacodemon.decl", "type");
    refuse_path("actormodifier/demon rune.decl", "character");
    refuse_path("actormodifier/demon;evil.decl", "character");
    refuse_path("actormodifier/demon{evil}.decl", "character");
    refuse_path("actormodifier/demon=evil.decl", "character");
    refuse_path("actormodifier/.decl", "name");
}

static void test_text_validation(void)
{
    static const unsigned char valid[] =
        "// opening brace in a comment: {\n"
        "{\n"
        "  inherit = \"player/{ignored}\\\"quoted\\\"\"\n"
        "  /* ignored close: } */\n"
        "}\n";
    static const unsigned char nested[] = "{ outer { inner } }";
    static const unsigned char bad_close[] = "{ } }";
    static const unsigned char bad_open[] = "{ { }";
    static const unsigned char bad_quote[] = "{ value = \"unterminated }";
    static const unsigned char bad_comment[] = "{ /* unterminated }";
    static const unsigned char no_brace[] = "value = 1";
    static const unsigned char embedded_nul[] = { '{', '}', 0, '{', '}' };

    CHECK(sh_decl_text_well_formed(valid, sizeof(valid) - 1) == 1);
    CHECK(sh_decl_text_well_formed(nested, sizeof(nested) - 1) == 1);
    CHECK(sh_decl_text_well_formed(bad_close, sizeof(bad_close) - 1) == 0);
    CHECK(sh_decl_text_well_formed(bad_open, sizeof(bad_open) - 1) == 0);
    CHECK(sh_decl_text_well_formed(bad_quote, sizeof(bad_quote) - 1) == 0);
    CHECK(sh_decl_text_well_formed(bad_comment, sizeof(bad_comment) - 1) == 0);
    CHECK(sh_decl_text_well_formed(no_brace, sizeof(no_brace) - 1) == 0);
    CHECK(sh_decl_text_well_formed(embedded_nul, sizeof(embedded_nul)) == 0);
    CHECK(sh_decl_text_well_formed(NULL, 0) == 0);
}

static void test_single_body_validation(void)
{
    static const unsigned char first[] = "{ value = 1; }";
    static const unsigned char second[] =
        "// before\n{ inherit = \"custom/first\"; } /* after */";
    static const unsigned char injected[] = "{} entitydef injected {}";
    static const unsigned char two_blocks[] = "{} {}";
    static const unsigned char outside_token[] = "token {}";
    size_t sum = 0;

    CHECK(sh_decl_server_test_checked_add(SIZE_MAX, 1, &sum) == 0);
    CHECK(sh_decl_server_test_checked_add(SIZE_MAX - 1, 1, &sum) == 1);
    CHECK(sum == SIZE_MAX);

    CHECK(sh_decl_server_test_body_is_single_block(first, sizeof(first) - 1) == 1);
    CHECK(sh_decl_server_test_body_is_single_block(second, sizeof(second) - 1) == 1);
    CHECK(sh_decl_server_test_body_is_single_block(injected, sizeof(injected) - 1) == 0);
    CHECK(sh_decl_server_test_body_is_single_block(two_blocks, sizeof(two_blocks) - 1) == 0);
    CHECK(sh_decl_server_test_body_is_single_block(outside_token,
                                                   sizeof(outside_token) - 1) == 0);
}

static void test_sedef_materialization_eligibility(void)
{
    static const unsigned char direct[] =
        "{ edit = { entityDef = \"snapmaps/test/direct\"; } }";
    static const unsigned char dotted[] =
        "{ EDIT . ENTITYDEF = \"snapmaps/test/dotted\"; }";
    static const unsigned char inherited[] =
        "{ InHeRiT = \"snapmaps/test/base\"; }";
    static const unsigned char nested_direct[] =
        "{ EDIT = { nested = { ignored = 1; } ENTITYDEF = \"snapmaps/test/nested\"; } }";
    static const unsigned char nested_decoy[] =
        "{ edit = { nested = { entityDef = \"snapmaps/test/decoy\"; } } }";
    static const unsigned char comment_decoy[] =
        "{ /* edit.entityDef = \"fake\"; */ // inherit = \"fake\"\n"
        "  edit = { displayNameTag = \"Only\"; } }";
    static const unsigned char quoted_decoy[] =
        "{ value = \"edit.entityDef = fake; inherit = fake\"; }";
    static const unsigned char unrelated_nested[] =
        "{ other = { entityDef = \"snapmaps/test/decoy\"; } }";

    CHECK(sh_decl_text_sedef_has_materializable_source(
              direct, sizeof(direct) - 1) == 1);
    CHECK(sh_decl_text_sedef_has_materializable_source(
              dotted, sizeof(dotted) - 1) == 1);
    CHECK(sh_decl_text_sedef_has_materializable_source(
              inherited, sizeof(inherited) - 1) == 1);
    CHECK(sh_decl_text_sedef_has_materializable_source(
              nested_direct, sizeof(nested_direct) - 1) == 1);
    CHECK(sh_decl_text_sedef_has_materializable_source(
              nested_decoy, sizeof(nested_decoy) - 1) == 0);
    CHECK(sh_decl_text_sedef_has_materializable_source(
              comment_decoy, sizeof(comment_decoy) - 1) == 0);
    CHECK(sh_decl_text_sedef_has_materializable_source(
              quoted_decoy, sizeof(quoted_decoy) - 1) == 0);
    CHECK(sh_decl_text_sedef_has_materializable_source(
              unrelated_nested, sizeof(unrelated_nested) - 1) == 0);
    CHECK(sh_decl_text_sedef_has_materializable_source(NULL, 0) == 0);
}

typedef struct dependency_capture {
    const char *types[8];
    char names[8][SH_DECL_SERVER_NAME_CAP];
    int count;
} dependency_capture;

static int capture_dependency(const char *type, const unsigned char *name,
                              size_t name_length, void *opaque)
{
    dependency_capture *capture = (dependency_capture *)opaque;
    if (!capture || capture->count >= 8 || !type || !name ||
        name_length >= sizeof(capture->names[0])) return 0;
    capture->types[capture->count] = type;
    memcpy(capture->names[capture->count], name, name_length);
    capture->names[capture->count][name_length] = '\0';
    capture->count++;
    return 1;
}

static void test_typed_materialization_dependencies(void)
{
    static const unsigned char cyber_shaped[] =
        "{ inherit = \"demons/demon_base\"; edit = {"
        " entityDef = \"snapmaps/placeablesnapaiencounter/cyberdemon\";"
        " buildGameRefEntityDefs = { num = 1; item[0] = \"ai/demon/cyberdemon_hell\"; }"
        " nested = { entityDef = \"ignored/nested\"; }; } }";
    static const unsigned char entity_body[] =
        "{ inherit = \"ai/demon/cyberdemon_base\";"
        " nested = { inherit = \"ignored/nested\"; } }";
    dependency_capture capture;

    memset(&capture, 0, sizeof(capture));
    CHECK(sh_decl_text_collect_sedef_dependencies(
              cyber_shaped, sizeof(cyber_shaped) - 1,
              capture_dependency, &capture) == 1);
    CHECK(capture.count == 3);
    if (capture.count == 3) {
        CHECK(strcmp(capture.types[0], "snapEditorEntityDef") == 0);
        CHECK(strcmp(capture.names[0], "demons/demon_base") == 0);
        CHECK(strcmp(capture.types[1], "entityDef") == 0);
        CHECK(strcmp(capture.names[1],
                     "snapmaps/placeablesnapaiencounter/cyberdemon") == 0);
        CHECK(strcmp(capture.types[2], "entityDef") == 0);
        CHECK(strcmp(capture.names[2], "ai/demon/cyberdemon_hell") == 0);
    }

    memset(&capture, 0, sizeof(capture));
    CHECK(sh_decl_text_collect_entitydef_dependencies(
              entity_body, sizeof(entity_body) - 1,
              capture_dependency, &capture) == 1);
    CHECK(capture.count == 1);
    if (capture.count == 1) {
        CHECK(strcmp(capture.types[0], "entityDef") == 0);
        CHECK(strcmp(capture.names[0], "ai/demon/cyberdemon_base") == 0);
    }
}

static void test_reference_dependency_ordering(void)
{
    static const unsigned char consumer[] =
        "{ declPrt = \"particle/base\"; sound = \"sound/fire\"; }";
    static const unsigned char particle[] = "{ value = 1; }";
    static const unsigned char sound[] = "{ value = 2; }";
    static const unsigned char cycle_a[] = "{ next = \"cycle/b\"; }";
    static const unsigned char cycle_b[] = "{ next = \"cycle/a\"; }";
    sh_decl_reference_item items[] = {
        { "fx/main", consumer, sizeof(consumer) - 1, (void *)(uintptr_t)1 },
        { "particle/base", particle, sizeof(particle) - 1, (void *)(uintptr_t)2 },
        { "sound/fire", sound, sizeof(sound) - 1, (void *)(uintptr_t)3 },
        { "cycle/a", cycle_a, sizeof(cycle_a) - 1, (void *)(uintptr_t)4 },
        { "cycle/b", cycle_b, sizeof(cycle_b) - 1, (void *)(uintptr_t)5 }
    };
    size_t edges = 0;
    size_t cycles = 0;

    CHECK(sh_decl_text_order_by_references(items, 5, &edges, &cycles) == 1);
    CHECK(edges == 4);
    CHECK(cycles == 2);
    CHECK(strcmp(items[0].name, "cycle/a") == 0);
    CHECK(strcmp(items[1].name, "cycle/b") == 0);
    CHECK(strcmp(items[2].name, "particle/base") == 0);
    CHECK(strcmp(items[3].name, "sound/fire") == 0);
    CHECK(strcmp(items[4].name, "fx/main") == 0);
}

static void test_scc_dependency_ordering(void)
{
    static const unsigned char weak_a[] = "{ next = \"weak/b\"; }";
    static const unsigned char weak_b[] = "{ next = \"weak/a\"; }";
    static const unsigned char ammo[] = "{ source = \"weak/a\"; }";
    static const unsigned char base[] = "{ inherit = \"ammo/rocket\"; }";
    static const unsigned char child[] = "{ inherit = \"ai/demon/base\"; }";
    static const unsigned char child_two[] = "{ inherit = \"ai/demon/base\"; }";
    sh_decl_reference_item items[] = {
        { "ai/demon/child-two", child_two, sizeof(child_two) - 1, (void *)(uintptr_t)1 },
        { "weak/b", weak_b, sizeof(weak_b) - 1, (void *)(uintptr_t)2 },
        { "ai/demon/base", base, sizeof(base) - 1, (void *)(uintptr_t)3 },
        { "ammo/rocket", ammo, sizeof(ammo) - 1, (void *)(uintptr_t)4 },
        { "weak/a", weak_a, sizeof(weak_a) - 1, (void *)(uintptr_t)5 },
        { "ai/demon/child", child, sizeof(child) - 1, (void *)(uintptr_t)6 }
    };
    sh_decl_reference_item reversed[sizeof(items) / sizeof(items[0])];
    size_t edges = 0;
    size_t reversed_edges = 0;
    size_t cycles = 0;
    size_t reversed_cycles = 0;
    size_t i;
    size_t base_index = 0;
    size_t child_index = 0;
    size_t child_two_index = 0;
    static const char *expected[] = {
        "weak/a", "weak/b", "ammo/rocket", "ai/demon/base",
        "ai/demon/child", "ai/demon/child-two"
    };

    for (i = 0; i < sizeof(items) / sizeof(items[0]); i++)
        reversed[i] = items[sizeof(items) / sizeof(items[0]) - i - 1];
    CHECK(sh_decl_text_order_by_references(items, sizeof(items) / sizeof(items[0]),
                                           &edges, &cycles) == 1);
    CHECK(sh_decl_text_order_by_references(reversed, sizeof(reversed) / sizeof(reversed[0]),
                                           &reversed_edges, &reversed_cycles) == 1);
    CHECK(edges == 6);
    CHECK(reversed_edges == edges);
    CHECK(cycles == 2);
    CHECK(reversed_cycles == cycles);
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        CHECK(strcmp(items[i].name, expected[i]) == 0);
        CHECK(strcmp(reversed[i].name, expected[i]) == 0);
    }
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        if (strcmp(items[i].name, "ai/demon/base") == 0) base_index = i;
        if (strcmp(items[i].name, "ai/demon/child") == 0) child_index = i;
        if (strcmp(items[i].name, "ai/demon/child-two") == 0) child_two_index = i;
    }
    CHECK(base_index < child_index);
    CHECK(base_index < child_two_index);
}

static void test_inheritance_cycle_refusal(void)
{
    static const unsigned char inherit_a[] = "{ inherit = \"inherit/b\"; }";
    static const unsigned char inherit_b[] = "{ inherit = \"inherit/a\"; }";
    sh_decl_reference_item items[] = {
        { "inherit/a", inherit_a, sizeof(inherit_a) - 1, (void *)(uintptr_t)1 },
        { "inherit/b", inherit_b, sizeof(inherit_b) - 1, (void *)(uintptr_t)2 }
    };
    size_t edges = 99;
    size_t cycles = 99;

    CHECK(sh_decl_text_order_by_references(items, sizeof(items) / sizeof(items[0]),
                                           &edges, &cycles) == 0);
    CHECK(strcmp(items[0].name, "inherit/a") == 0);
    CHECK(strcmp(items[1].name, "inherit/b") == 0);
}

static void test_complete_set_collision_ordering(void)
{
    enum { ITEM_COUNT = 516, UNIQUE_COUNT = 513 };
    static sh_decl_server_order_item items[ITEM_COUNT];
    static sh_decl_server_order_item reversed[ITEM_COUNT];
    static char types[ITEM_COUNT][32];
    static char names[ITEM_COUNT][64];
    static char sources[ITEM_COUNT][128];
    size_t i;
    size_t admitted;
    size_t reversed_admitted;
    size_t duplicate_count = 0;
    size_t overflow = 0;
    const char *last_admitted = NULL;

    memset(items, 0, sizeof(items));
    strcpy_s(types[0], sizeof(types[0]), "actormodifier");
    strcpy_s(names[0], sizeof(names[0]), "collision");
    strcpy_s(sources[0], sizeof(sources[0]), "generated/decls/a/collision.decl");
    for (i = 0; i < UNIQUE_COUNT; i++) {
        size_t index = i + 1;
        strcpy_s(types[index], sizeof(types[index]), "actormodifier");
        _snprintf_s(names[index], sizeof(names[index]), _TRUNCATE, "item/%04zu", i);
        _snprintf_s(sources[index], sizeof(sources[index]), _TRUNCATE,
                    "generated/decls/actormodifier/item/%04zu.decl", i);
    }
    strcpy_s(types[ITEM_COUNT - 2], sizeof(types[ITEM_COUNT - 2]), "ActorModifier");
    strcpy_s(names[ITEM_COUNT - 2], sizeof(names[ITEM_COUNT - 2]), "COLLISION");
    strcpy_s(sources[ITEM_COUNT - 2], sizeof(sources[ITEM_COUNT - 2]),
             "generated/decls/z/COLLISION.decl");
    strcpy_s(types[ITEM_COUNT - 1], sizeof(types[ITEM_COUNT - 1]), "ACTORMODIFIER");
    strcpy_s(names[ITEM_COUNT - 1], sizeof(names[ITEM_COUNT - 1]), "Collision");
    strcpy_s(sources[ITEM_COUNT - 1], sizeof(sources[ITEM_COUNT - 1]),
             "generated/decls/y/Collision.decl");

    for (i = 0; i < ITEM_COUNT; i++) {
        items[i].type = types[i];
        items[i].name = names[i];
        items[i].source = sources[i];
        items[i].value = (void *)(uintptr_t)i;
    }
    for (i = 0; i < ITEM_COUNT; i++) reversed[i] = items[ITEM_COUNT - 1 - i];
    admitted = sh_decl_server_order_and_admit(items, ITEM_COUNT, 512);
    reversed_admitted = sh_decl_server_order_and_admit(reversed, ITEM_COUNT, 512);
    CHECK(admitted == 512);
    CHECK(reversed_admitted == admitted);
    for (i = 0; i < ITEM_COUNT; i++) {
        CHECK(strcmp(items[i].type, reversed[i].type) == 0);
        CHECK(strcmp(items[i].name, reversed[i].name) == 0);
        CHECK(strcmp(items[i].source, reversed[i].source) == 0);
        CHECK(items[i].duplicate == reversed[i].duplicate);
        CHECK(items[i].admitted == reversed[i].admitted);
        if (items[i].duplicate) {
            duplicate_count++;
            CHECK(items[i].admitted == 0);
            continue;
        }
        if (items[i].admitted) {
            last_admitted = items[i].name;
        } else {
            overflow++;
        }
    }
    CHECK(duplicate_count == 3);
    CHECK(overflow == 1);
    CHECK(last_admitted != NULL);
    if (last_admitted) CHECK(strcmp(last_admitted, "item/0511") == 0);
}

static void test_walk_error_propagation(void)
{
    size_t retained = 99;

    CHECK(sh_decl_server_test_find_first_status(0, ERROR_FILE_NOT_FOUND) == 0);
    CHECK(sh_decl_server_test_find_first_status(0, ERROR_PATH_NOT_FOUND) == -1);
    CHECK(sh_decl_server_test_find_first_status(1, ERROR_ACCESS_DENIED) == 1);
    CHECK(sh_decl_server_test_find_next_status(0, ERROR_NO_MORE_FILES) == 0);
    CHECK(sh_decl_server_test_find_next_status(0, ERROR_ACCESS_DENIED) == -1);
    CHECK(sh_decl_server_test_find_next_status(1, ERROR_ACCESS_DENIED) == 1);
    CHECK(sh_decl_server_test_root_attributes_status(0, ERROR_FILE_NOT_FOUND) == 0);
    CHECK(sh_decl_server_test_root_attributes_status(0, ERROR_PATH_NOT_FOUND) == 0);
    CHECK(sh_decl_server_test_root_attributes_status(0, ERROR_ACCESS_DENIED) == -1);
    CHECK(sh_decl_server_test_root_attributes_status(1, ERROR_ACCESS_DENIED) == 1);

    reset_find_script();
    g_first_steps[0].found = 0;
    g_first_steps[0].error = ERROR_FILE_NOT_FOUND;
    g_first_count = 1;
    CHECK(sh_decl_server_test_walk("C:\\fake", "actormodifier", &retained) == 1);
    CHECK(retained == 0);
    CHECK(g_close_count == 0);

    reset_find_script();
    retained = 99;
    g_first_steps[0].found = 0;
    g_first_steps[0].error = ERROR_ACCESS_DENIED;
    g_first_count = 1;
    CHECK(sh_decl_server_test_walk("C:\\fake", "actormodifier", &retained) == 0);
    CHECK(retained == 0);
    CHECK(g_close_count == 0);
    CHECK(strstr(g_last_log, "FindFirstFileA failed with Win32 error 5") != NULL);

    reset_find_script();
    retained = 99;
    g_first_steps[0].found = 1;
    g_first_steps[0].name = "one.decl";
    g_first_steps[0].handle = 1;
    g_first_count = 1;
    g_next_steps[0].found = 0;
    g_next_steps[0].error = ERROR_NO_MORE_FILES;
    g_next_count = 1;
    CHECK(sh_decl_server_test_walk("C:\\fake", "actormodifier", &retained) == 1);
    CHECK(retained == 1);
    CHECK(g_close_count == 1);

    reset_find_script();
    retained = 99;
    g_first_steps[0].found = 1;
    g_first_steps[0].name = "one.decl";
    g_first_steps[0].handle = 1;
    g_first_count = 1;
    g_next_steps[0].found = 0;
    g_next_steps[0].error = ERROR_ACCESS_DENIED;
    g_next_count = 1;
    CHECK(sh_decl_server_test_walk("C:\\fake", "actormodifier", &retained) == 0);
    CHECK(retained == 0);
    CHECK(g_close_count == 1);
    CHECK(strstr(g_last_log, "FindNextFileA failed with Win32 error 5") != NULL);

    reset_find_script();
    retained = 99;
    g_first_steps[0].found = 1;
    g_first_steps[0].name = "actormodifier";
    g_first_steps[0].attributes = FILE_ATTRIBUTE_DIRECTORY;
    g_first_steps[0].handle = 1;
    g_first_steps[1].found = 0;
    g_first_steps[1].error = ERROR_ACCESS_DENIED;
    g_first_count = 2;
    CHECK(sh_decl_server_test_walk("C:\\fake", "", &retained) == 0);
    CHECK(retained == 0);
    CHECK(g_close_count == 1);
    CHECK(g_first_index == 2);
    CHECK(strstr(g_last_log, "FindFirstFileA failed with Win32 error 5") != NULL);

    sh_decl_server_test_reset_find_api();
}

int main(void)
{
    test_native_idstr_boundary();
    test_source_first_classification();
    test_sedef_materialization();
    test_integrated_scan_materialize_pipeline();
    test_source_only_pipeline_gate();
    test_cyber_shaped_registration_order();
    test_materialization_failure_guards();
    test_identity_paths();
    test_text_validation();
    test_single_body_validation();
    test_sedef_materialization_eligibility();
    test_typed_materialization_dependencies();
    test_reference_dependency_ordering();
    test_scc_dependency_ordering();
    test_inheritance_cycle_refusal();
    test_complete_set_collision_ordering();
    test_walk_error_propagation();
    if (g_failed) {
        fprintf(stderr, "%d decl-server test(s) failed\n", g_failed);
        return 1;
    }
    puts("decl server tests passed");
    return 0;
}
