/* decl_server_test.c -- decl identity, ordering, validation, and walk tests. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "engine_globals.h"
#include "decl_server.h"
#include "boot_phases.h"
int sh_map_transition_at_boundary(void) { return 0; }
int sh_editor_transition_at_boundary(void) { return 0; }
static int g_occurrence_frame;
void *sh_editor_transition_capture(const void *editor)
{ return g_occurrence_frame && editor ? (void *)2 : NULL; }
int sh_editor_transition_frame_ready(void *editor, void *map)
{ return g_occurrence_frame && editor && map == (void *)2; }
void sh_nav_bake_source_update_begin(void) { }
void sh_nav_bake_source_update_end(int committed) { (void)committed; }
static int g_defer_install_commit;
/* Counts this predicate's calls down when positive; the call that reaches zero
 * returns g_defer_hook's answer instead, so a state change can begin between
 * the poll's check and the re-arm command's own. */
static int g_defer_countdown;
static int (*g_defer_hook)(void);
/* R-398: the native map boundary is available, so the idle retirement may wait for it (off by default). */
static int g_transition_ready;
int sh_map_transition_ready(void) { return g_transition_ready; }
/* Departures the idle retirement accounted for (sh_rawmap_log_retained_departure). */
static int g_retained_departures;
static char g_retained_route[64];
void sh_rawmap_log_retained_departure(const char *route)
{ g_retained_departures++; strcpy_s(g_retained_route, sizeof(g_retained_route), route ? route : ""); }
int sh_rawmap_launch_at_boundary(void) { return 0; }
int sh_rawmap_defers_install_commit(void)
{
    if (g_defer_countdown > 0 && --g_defer_countdown == 0 && g_defer_hook) return g_defer_hook();
    return g_defer_install_commit;
}
#include "package_runtime.h"
#include "resource_resident.h"
#include "resource_graph_native.h"
#include "package_native_assets.h"
#include "package_shader_receipt.h"
#include "native_resource_id.h"
static int g_resident_begin_calls, g_resident_drain_calls, g_resident_end_calls;
static int g_resident_fail_begin, g_resident_fail_drain;
static void *g_resident_only;
static int g_resident_filter;
static int g_shader_fixture_active, g_shader_scope_live, g_shader_resident_live;
static int g_shader_mode, g_shader_hold_end, g_shader_restores;
static int (*g_shader_admission)(void *);
static void *g_shader_admission_context;
static struct {
    unsigned char head[8], manager[0x30], items[8], object[0x90];
    char type[16], name[16];
} g_shader_native;
static const char g_shader_source[] = "{ Tex2D textures/fixture }";
static int shader_native_read(void *context, uintptr_t address, void *out, size_t length)
{
    uintptr_t start = (uintptr_t)&g_shader_native;
    (void)context;
    if (address < start || address - start > sizeof(g_shader_native) ||
        length > sizeof(g_shader_native) - (size_t)(address - start)) return 0;
    memcpy(out, (const void *)address, length); return 1;
}
static char g_shader_events[64];
static size_t g_shader_event_count;
static void shader_event(char event)
{
    if (g_shader_fixture_active && g_shader_event_count + 1 < sizeof(g_shader_events)) {
        g_shader_events[g_shader_event_count++] = event; g_shader_events[g_shader_event_count] = 0;
    }
}
/* Templates the stub pass selected (R-398 palette decision); none unless a test says so. */
static size_t g_resident_templates;
#ifndef SH_DECL_SERVER_REAL_RESIDENT
int sh_resource_resident_selected(const sh_resource_resident *pass, const void *object)
{ (void)pass; return !g_resident_filter || object == g_resident_only; }
int sh_resource_resident_describe(const sh_resource_resident *pass, const void *object, char *out, size_t capacity)
{
    int selected = sh_resource_resident_selected(pass, object);
    if (out && capacity) snprintf(out, capacity, "%s", selected ? "test:consumer (selected)" : "");
    return selected;
}
size_t sh_resource_resident_selected_count(const sh_resource_resident *pass)
{ (void)pass; return g_resident_filter && !g_resident_only ? 0 : 1; }
size_t sh_resource_resident_selected_type(const sh_resource_resident *pass, const char *type)
{ (void)pass; (void)type; return g_resident_templates; }
size_t sh_resource_resident_blocking_count(const sh_resource_resident *pass, char *first, size_t capacity)
{
    size_t count = sh_resource_resident_selected_count(pass);
    if (first && capacity) snprintf(first, capacity, "%s", count ? "test:consumer" : "");
    return count;
}
size_t sh_resource_resident_release_revived(const sh_resource_resident *pass, void **existing, size_t count)
{ (void)pass; (void)existing; return count; }
/* No recorded consumers here: recovery keeps refusing every selected object. */
int sh_resource_resident_take_prior(sh_resource_resident *pass, sh_resource_graph_impact *out)
{ (void)pass; if (out) { sh_resource_graph_impact_free(out); memset(out, 0, sizeof(*out)); } return 1; }
int sh_resource_resident_acquired(const sh_resource_resident *pass, const void *resource,
    const sh_resource_graph_impact *prior)
{ (void)pass; (void)resource; (void)prior; return 0; }
sh_resource_resident *sh_resource_resident_begin(const sh_package_changes *changes, int restoring, int initial, char *error, size_t capacity)
{
    (void)changes; (void)restoring; (void)initial; ++g_resident_begin_calls;
    if (g_resident_fail_begin) { g_resident_fail_begin = 0; snprintf(error, capacity, "resident capture failed"); return NULL; }
    shader_event('R'); g_shader_resident_live = 1;
    return (sh_resource_resident *)1;
}
int sh_resource_resident_owned(const sh_resource_resident *pass)
{ return pass == (sh_resource_resident *)1 && g_shader_resident_live; }
void sh_resource_resident_external(sh_resource_resident *pass, void *object) { (void)pass; (void)object; }
int sh_resource_resident_isolated_identity(const sh_resource_resident *pass, const char *type, const char *name)
{ (void)pass; (void)type; (void)name; return 0; }
void sh_resource_resident_touch(void *object) { (void)object; }
int sh_resource_resident_reconstruct(sh_resource_resident *pass, char *error, size_t capacity)
{ (void)pass; (void)error; (void)capacity; return 1; }
int sh_resource_resident_defaults(sh_resource_resident *pass, char *error, size_t capacity)
{ (void)pass; (void)error; (void)capacity; return 1; }
int sh_resource_resident_revives(const sh_resource_resident *pass, const void *resource,
    const char *type, const char *name)
{ (void)pass; (void)resource; (void)type; (void)name; return 0; }
#endif
/* Leaves reload after defaults and before any declaration is re-parsed. */
static int g_rt_load_total;
static int g_resident_leaf_calls, g_resident_leaf_loads_seen = -1;
#ifndef SH_DECL_SERVER_REAL_RESIDENT
int sh_resource_resident_drain_leaves(sh_resource_resident *pass, int (*leaf)(const char *family),
    char *error, size_t capacity)
{
    (void)pass; (void)error; (void)capacity; ++g_resident_leaf_calls;
    g_resident_leaf_loads_seen = g_rt_load_total;
    return leaf && leaf("skeleton") && !leaf("md6def");
}
int sh_resource_resident_drain(sh_resource_resident *pass, char *error, size_t capacity)
{
    (void)pass; ++g_resident_drain_calls;
    if (g_resident_fail_drain) { g_resident_fail_drain = 0; snprintf(error, capacity, "resident drain failed"); return 0; }
    return 1;
}
int sh_resource_resident_drain_additions(sh_resource_resident *pass, void *const *existing,
    size_t count, char *error, size_t capacity)
{ (void)existing; (void)count; return sh_resource_resident_drain(pass, error, capacity); }
int sh_resource_resident_end(sh_resource_resident *pass, int succeeded, char *error, size_t capacity)
{
    (void)pass; (void)error; (void)capacity; ++g_resident_end_calls;
    if (g_shader_fixture_active && error && capacity) *error = 0;
    shader_event('X'); g_shader_resident_live = 0; return succeeded;
}
#endif
#include "decl_server_path.h"
#include "decl_text.h"
#include "overrides.h"
#include "hook.h"

static int g_apply_palette_result = 1;
static int g_fail_next_palette;
static const char *g_linked_body = "{}";
static int g_apply_palette_calls;
static int g_user_enabled;
static int g_runtime_dependencies_ok = 1;
static int g_string_refreshes;
static int g_fail_next_string_refresh;
static int g_audio_refreshes, g_fail_next_audio_refresh;
#include "audio_banks_native.h"
int sh_audio_banks_native_activate(const uint32_t *events, size_t count,
    const sh_package_audio_bank *banks, size_t bank_count,
    const sh_package_audio_media *media, size_t media_count, char *error, size_t capacity)
{
    (void)events; (void)count; (void)banks; (void)bank_count; (void)media; (void)media_count; ++g_audio_refreshes;
    if (g_fail_next_audio_refresh) {
        g_fail_next_audio_refresh = 0;
        snprintf(error, capacity, "test audio activation failed"); return 0;
    }
    return 1;
}
sh_audio_bank_addition *sh_audio_banks_native_additions_begin(char *error, size_t capacity)
{ (void)error; (void)capacity; return (sh_audio_bank_addition *)1; }
int sh_audio_banks_native_activate_additive(sh_audio_bank_addition *addition,
    const uint32_t *events, size_t count, const sh_package_audio_bank *banks, size_t bank_count,
    const sh_package_audio_media *media, size_t media_count, char *error, size_t capacity)
{ (void)addition; return sh_audio_banks_native_activate(events, count, banks, bank_count, media, media_count, error, capacity); }
int sh_audio_banks_native_additions_end(sh_audio_bank_addition **addition, int accepted, char *error, size_t capacity)
{ (void)accepted; (void)error; (void)capacity; *addition = NULL; return 1; }
int sh_audio_banks_native_additions_ready(sh_audio_bank_addition *addition, char *error, size_t capacity)
{ (void)error; (void)capacity; return addition != NULL; }
void sh_audio_banks_native_additions_accept(sh_audio_bank_addition **addition) { *addition = NULL; }
/* Retained-program route: each call counts as one audio activation and
 * releases every owner reference it receives, as the native adapter does on
 * failure or duplicate delivery. */
int sh_audio_banks_native_programs_activate(const sh_audio_active_program *programs, size_t count,
    const sh_audio_shared_policy *baseline, void (*release)(void *), char *error, size_t capacity)
{
    (void)baseline;
    for (size_t i = 0; i < count; i++) if (release && programs[i].owner) release(programs[i].owner);
    return sh_audio_banks_native_activate(NULL, 0, NULL, 0, NULL, 0, error, capacity);
}
int sh_audio_banks_native_programs_add(sh_audio_bank_addition *addition,
    const sh_audio_active_program *programs, size_t count, void (*release)(void *), char *error, size_t capacity)
{
    (void)addition;
    for (size_t i = 0; i < count; i++) if (release && programs[i].owner) release(programs[i].owner);
    return sh_audio_banks_native_activate(NULL, 0, NULL, 0, NULL, 0, error, capacity);
}
int sh_audio_banks_native_programs_retire(char *error, size_t capacity) { (void)error; (void)capacity; return 0; }
size_t sh_audio_banks_native_programs_count(void) { return 0; }
#include "palette_refresh.h"
int sh_palette_entries_ready(void) { return 1; }
/* The additive palette's exact-name contract (palette_refresh.c
 * sh_palette_entries_prepare): the additive entries hold one declaration per
 * exact entityDef name, and a source naming a held name with another
 * declaration refuses the whole batch, which then holds nothing. */
typedef struct palette_stub_entry { char name[256]; const void *decl; const void *batch; } palette_stub_entry;
static palette_stub_entry g_palette_stub[64];
static size_t g_palette_stub_count;
static unsigned char g_palette_stub_tokens[16];
static int g_palette_stub_fail; /* a palette-stage fault after native publication */
static size_t g_palette_stub_token;
int sh_palette_entries_prepare(const sh_palette_entry_source *sources, size_t count,
    sh_palette_entries_admitted_fn admitted, void *context, sh_palette_entries **out, char *error, size_t capacity)
{
    size_t first = g_palette_stub_count;
    const void *batch;
    if (!admitted(context)) return 0;
    if (g_palette_stub_fail) { snprintf(error, capacity, "fixture palette refused"); return 0; }
    batch = g_palette_stub_tokens + g_palette_stub_token++ % sizeof(g_palette_stub_tokens);
    for (size_t i = 0; i < count; i++) {
        const char *name = sources[i].entity_def_name ? sources[i].entity_def_name : "";
        int held = 0;
        for (size_t e = 0; e < g_palette_stub_count && !held; e++) if (!strcmp(g_palette_stub[e].name, name)) {
            if (g_palette_stub[e].decl != sources[i].snap_editor_entity) {
                g_palette_stub_count = first;
                snprintf(error, capacity, "palette exact name already belongs to a different declaration"); return 0;
            }
            held = 1;
        }
        if (!held && g_palette_stub_count < sizeof(g_palette_stub) / sizeof(*g_palette_stub)) {
            palette_stub_entry *entry = g_palette_stub + g_palette_stub_count++;
            snprintf(entry->name, sizeof(entry->name), "%s", name);
            entry->decl = sources[i].snap_editor_entity; entry->batch = batch;
        }
    }
    *out = (sh_palette_entries *)batch; return 1;
}
int sh_palette_entries_publish(sh_palette_entries *batch, sh_palette_entries_admitted_fn admitted,
    void *context, char *error, size_t capacity)
{ (void)batch; (void)error; (void)capacity; return admitted(context); }
int sh_palette_entries_prepare_authoring(sh_palette_entries *batch, sh_palette_entries_admitted_fn admitted,
    void *context, char *error, size_t capacity)
{ (void)batch; (void)error; (void)capacity; return admitted(context); }
void sh_palette_entries_accept(sh_palette_entries *batch) { (void)batch; }
static int g_palette_end_result = 1, g_palette_end_calls;
int sh_palette_entries_end(sh_palette_entries **batch, sh_palette_entries_admitted_fn admitted,
    void *context, char *error, size_t capacity)
{
    (void)error; (void)capacity; if (!admitted(context)) return 0;
    ++g_palette_end_calls;
    if (g_palette_end_result != 1) return g_palette_end_result;
    for (size_t e = 0; e < g_palette_stub_count;)
        if (g_palette_stub[e].batch == *batch) g_palette_stub[e] = g_palette_stub[--g_palette_stub_count]; else e++;
    *batch = NULL; return 1;
}
static int g_scan_calls, g_scan_fail;
/* Whether the last pass was the installed-package watcher's (it may reuse an
 * unchanged library); -1 before any pass is observed. */
static int g_scan_watched = -1;
static int g_table_published;
static int g_table_merges;
static char g_fixture_root[MAX_PATH];
static unsigned char g_boundary_editor[16];
static unsigned char g_boundary_shell[0x20];
static unsigned char g_boundary_manager[0x918];
static unsigned char g_boundary_dialog_manager[0x910];
static unsigned char g_boundary_dialog_queue[2 * 0x1b0];
static unsigned char g_boundary_common[0xf577];
static void *g_boundary_shell_slot = g_boundary_shell;
static void *g_boundary_common_slot = g_boundary_common;
static DWORD g_boundary_thread;
/* g_defer_hook answers at the re-arm command's own deferral check: an
 * installation starts deferring there, or the browser stops being a safe
 * boundary (the poll found it safe a moment before). */
static int defer_begins(void) { return 1; }
static int boundary_closes(void) { g_boundary_thread = 0; return 0; }
static int g_boundary_unknown;
static void *g_boundary_resource_head;
static void heap_observe_native(int palette);
/* Whether the stub palette holds the rows its last rebuild produced (R-398): never, unless
 * a test says so, so every pass rebuilds as before. */
static int g_palette_current;
int sh_palette_refresh_current(void) { return g_palette_current; }
/* Stub palette notification while testing decl-server helpers. */
int sh_palette_refresh_after_decl_registration(void)
{
    heap_observe_native(1);
    g_apply_palette_calls++;
    if (g_fail_next_palette) {
        int failure = g_fail_next_palette; g_fail_next_palette = 0;
        if (failure == 2) RaiseException(0xe04d504c, 0, 0, NULL);
        return 0;
    }
    return g_apply_palette_result;
}

static int g_failed;
static int g_editor_live, g_editor_ready_refuse, g_editor_end_refuse, g_editor_after_commit_refuse;
static int g_editor_end_calls, g_editor_end_throw, g_editor_after_commit_throw;
static int g_editor_begins, g_editor_accepts, g_editor_rejects;
static int (*g_editor_admitted)(void *);
static void *g_editor_context;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

sh_resource_editor_state *sh_resource_graph_native_editor_state_begin(
    int (*admitted)(void *),void *context,char *error,size_t capacity)
{
    (void)error; (void)capacity; CHECK(!g_editor_live && admitted(context));
    g_editor_admitted=admitted; g_editor_context=context; g_editor_live=1; g_editor_begins++;
    return (sh_resource_editor_state *)2;
}
int sh_resource_graph_native_editor_state_ok(sh_resource_editor_state *state)
{ return state==(sh_resource_editor_state *)2 && g_editor_live && g_editor_admitted(g_editor_context); }
int sh_resource_graph_native_editor_state_ready(sh_resource_editor_state *state)
{
    if (g_editor_ready_refuse) { g_editor_ready_refuse--; return 0; }
    return sh_resource_graph_native_editor_state_ok(state);
}
int sh_resource_graph_native_editor_state_end(sh_resource_editor_state **owned,
    int accepted,int *committed,char *error,size_t capacity)
{
    (void)error; (void)capacity; *committed=0; g_editor_end_calls++;
    if (!*owned) { *committed=accepted; return 1; }
    CHECK(*owned==(sh_resource_editor_state *)2 && g_editor_live && g_editor_admitted(g_editor_context));
    if (g_editor_end_throw) {
        g_editor_end_throw=0; RaiseException(0xe0410012,0,0,NULL);
    }
    if (g_editor_end_refuse) { g_editor_end_refuse--; return 0; }
    if (accepted) g_editor_accepts++; else g_editor_rejects++;
    *committed=accepted; *owned=NULL; g_editor_live=0; g_editor_context=NULL; return 1;
}
/* Model the allocator scope stack independently of resource-level promotion. */
static struct {
    unsigned char prefix[0x44];
    int heaps[32];
    int depth;
} g_heap_local;
static int g_heap_push_noop, g_heap_pushes, g_heap_pops;
static int g_heap_check_native, g_heap_decl_calls, g_heap_palette_calls;
static int g_install_commit_calls, g_install_cancel_calls, g_install_commit_fail;
static const sh_package_compilation *g_install_expected_recovery;

static int g_startup_ready = 1, g_startup_recovery_ok = 1, g_startup_retries;
int sh_mpkg_startup_ready(void) { return g_startup_ready; }
void sh_mpkg_boot_capture(const char *root)
{
    CHECK(root && *root); g_startup_retries++;
    g_startup_ready = g_startup_recovery_ok;
}
int sh_mpkg_activation_commit(char *error, size_t capacity)
{
    g_install_commit_calls++;
    if (g_heap_check_native) CHECK(g_heap_local.depth == 2 && g_editor_live);
    if (g_install_commit_fail) {
        int failure = g_install_commit_fail;
        g_install_commit_fail = 0;
        if (failure == 2) RaiseException(0xe04d504d, 0, 0, NULL);
        snprintf(error, capacity, "fixture installation commit failure"); return 0;
    }
    if (g_editor_after_commit_refuse) { g_editor_after_commit_refuse=0; g_editor_end_refuse=1; }
    if (g_editor_after_commit_throw) { g_editor_after_commit_throw=0; g_editor_end_throw=1; }
    return 1;
}
int sh_mpkg_activation_cancel(void)
{
    const sh_package_compilation *compiled = sh_package_runtime_acquire();
    g_install_cancel_calls++;
    if (g_install_expected_recovery) CHECK(compiled == g_install_expected_recovery);
    sh_package_runtime_release(); return 1;
}

static void heap_fixture_reset(void)
{
    memset(&g_heap_local, 0, sizeof(g_heap_local));
    g_heap_local.depth = 1;
    g_heap_local.heaps[0] = 2;
    g_heap_push_noop = g_heap_pushes = g_heap_pops = 0;
    g_heap_check_native = g_heap_decl_calls = g_heap_palette_calls = 0;
}

static void *heap_get(void) { return &g_heap_local; }
static void heap_push(void *self, int heap)
{
    CHECK(self == &g_heap_local && heap == 0);
    g_heap_pushes++;
    if (g_heap_push_noop) return;
    CHECK(g_heap_local.depth >= 0 && g_heap_local.depth < 32);
    g_heap_local.heaps[g_heap_local.depth++] = heap;
}
static void heap_pop(void *self)
{
    CHECK(self == &g_heap_local && g_heap_local.depth > 0);
    g_heap_pops++;
    g_heap_local.depth--;
}

static void heap_observe_native(int palette)
{
    if (!g_heap_check_native) return;
    CHECK(g_heap_local.depth == 2 && g_heap_local.heaps[1] == 0);
    if (palette) g_heap_palette_calls++;
    else g_heap_decl_calls++;
}

/* The native image state probe sh_decl_server_image_state installs (R-309). */
static int g_test_image_state;
static int test_image_state(const char *name, const unsigned char *installed, size_t length)
{ (void)installed; (void)length; return name && !strcmp(name, "env/x") ? g_test_image_state : 0; }
static char g_last_log[512];
static char g_last_failure[512];
static int g_log_count;

/* Every line since the last reset, for checks that need more than the last line. */
static char g_log_all[16384];
static size_t g_log_all_used;
static void log_all_reset(void) { g_log_all[0] = 0; g_log_all_used = 0; }
void backend_log(const char *message)
{
    g_log_count++;
    /* Truncate: a long product log line must not fast-fail the fixture. */
    strncpy_s(g_last_log, sizeof(g_last_log), message ? message : "", _TRUNCATE);
    if (message && g_log_all_used < sizeof(g_log_all) - 1) {
        int n = _snprintf_s(g_log_all + g_log_all_used, sizeof(g_log_all) - g_log_all_used, _TRUNCATE, "%s\n", message);
        g_log_all_used = n < 0 ? sizeof(g_log_all) - 1 : g_log_all_used + (size_t)n;
    }
    if (message && strstr(message, "FAILED:")) strncpy_s(g_last_failure, sizeof(g_last_failure), message, _TRUNCATE);
}

/* Record optional post-palette operations without letting them fail registration. */
static int g_visibility_installs;
static int g_visibility_result = 1;

int sh_decl_visibility_install(const unsigned char *module_base,
                               const char *existing_probe_path,
                               const char *absent_probe_path)
{
    (void)module_base; (void)existing_probe_path; (void)absent_probe_path;
    g_visibility_installs++;
    return g_visibility_result;
}

/* Exercise hook ownership without modifying executable memory. */
int sh_decl_visibility_source_exists(const char *path) { (void)path; return -1; }
int sh_decl_visibility_native_exists(const char *path) { (void)path; return -1; }
static void *g_hook_owned;
static void (*g_hook_detour)(void);
static int g_hook_prepare_fail = 1;
static int g_hook_unpatch_fail;
static int g_hook_installed;
static int g_hook_invoke_on_commit;
static sh_patch_status g_hook_commit_result = B2_PATCH_FAIL_SEH;
static int g_hook_prepares, g_hook_commits, g_hook_unpatches;
static int g_hook_original_calls, g_requirements_calls;
static int g_linked_fixture;

void *hook_prepare(void *target, void *detour, size_t stolen)
{
    CHECK(target != NULL && detour != NULL && stolen >= 14);
    CHECK(g_hook_owned == NULL);
    g_hook_prepares++;
    if (g_hook_prepare_fail) return NULL;
    g_hook_owned = target;
    g_hook_detour = (void (*)(void))detour;
    return target;
}
sh_patch_status hook_commit(void *tramp)
{
    CHECK(tramp == g_hook_owned && tramp != NULL);
    g_hook_commits++;
    if (g_hook_invoke_on_commit) {
        int before = g_hook_original_calls;
        int refreshes;
        g_hook_detour();
        CHECK(g_hook_original_calls == before + 1);
        CHECK(sh_decl_server_registration_succeeded() == 0);
        refreshes = g_string_refreshes;
        CHECK(sh_decl_server_rearm() == 0 && g_string_refreshes == refreshes);
    }
    g_hook_installed = g_hook_commit_result == B2_PATCH_OK;
    return g_hook_commit_result;
}
int hook_is_installed(void *tramp)
{
    return tramp != NULL && tramp == g_hook_owned && g_hook_installed;
}
int hook_unpatch(void *tramp)
{
    CHECK(tramp == g_hook_owned && tramp != NULL);
    g_hook_unpatches++;
    g_hook_installed = 0;
    if (g_hook_unpatch_fail) return 0;
    g_hook_owned = NULL;
    g_hook_detour = NULL;
    return 1;
}

int sh_package_requirements_apply_now(void *execute_command_buffer)
{
    (void)execute_command_buffer;
    g_requirements_calls++;
    return 1;
}

int sh_overrides_get_root(char *out, size_t cap)
{
    if (out && cap) strcpy_s(out, cap, g_fixture_root);
    return g_fixture_root[0] != '\0';
}

int sh_overrides_internal_decl_table_can_install(void)
{
    return g_user_enabled;
}

int sh_overrides_internal_decl_published(const char *name)
{
    (void)name;
    return g_table_published;
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
    return g_user_enabled;
}

/* The provider is stubbed, but source compilation uses the real package format. */
static const char *runtime_dirs[] = {"overrides", "overrides/fixture", "overrides/fixture/assets",
    "overrides/fixture/assets/generated", "overrides/fixture/assets/generated/decls",
    "overrides/fixture/assets/generated/decls/material", "overrides/fixture/assets/generated/decls/material/rt"};
static int runtime_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    if (strcmp(path, "generated/decls/material/rt/alpha.decl")) return 0;
    *body = (unsigned char *)_strdup("{ original = 1; }");
    *length = strlen("{ original = 1; }");
    return *body ? 1 : -1;
}
/* Replace a file's contents, as an author's editor would. */
static void inventory_write(const char *path, const char *text)
{
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD written = 0;
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) return;
    CHECK(WriteFile(file, text, (DWORD)strlen(text), &written, NULL) && written == strlen(text));
    CloseHandle(file);
}
/* Write text unless the file already holds it: a preparation reads the
 * installed packages and never rewrites them, so the fixture keeps their
 * write times as production would. */
static void fixture_put(const char *path, const char *text)
{
    FILE *file = NULL;
    char current[4096];
    size_t length = strlen(text), read = 0;
    if (!fopen_s(&file, path, "rb") && file) {
        read = fread(current, 1, sizeof(current), file); fclose(file); file = NULL;
        if (read == length && !memcmp(current, text, length)) return;
    }
    inventory_write(path, text);
}
static void runtime_fixture_write(void)
{
    char path[MAX_PATH];
    for (size_t i = 0; i < sizeof(runtime_dirs)/sizeof(runtime_dirs[0]); i++) {
        snprintf(path, sizeof path, "%s/%s", g_fixture_root, runtime_dirs[i]);
        CHECK(CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    }
    snprintf(path, sizeof path, "%s/overrides/fixture/package.json", g_fixture_root);
    fixture_put(path, "{\"id\":\"fixture\",\"name\":\"Fixture\"}");
    snprintf(path, sizeof path, "%s/overrides/fixture/assets/generated/decls/material/rt/alpha.decl", g_fixture_root);
    if (g_linked_fixture) fixture_put(path, g_linked_body);
    else if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) CHECK(DeleteFileA(path));
}
/* The settle time the inventory tests use; production waits 2000 ms. */
#define INVENTORY_QUIET_MS 300u
static void inventory_poll_for(ULONGLONG ms)
{
    for (ULONGLONG start = GetTickCount64(); GetTickCount64() - start < ms;) { sh_decl_server_rearm_poll(); Sleep(5); }
}
/* Poll until a pass starts after scans, for at most 10 s (a pass runs inside
 * one poll). */
static void inventory_until_pass(int scans)
{
    for (ULONGLONG start = GetTickCount64(); g_scan_calls == scans && GetTickCount64() - start < 10000;) {
        sh_decl_server_rearm_poll(); Sleep(5);
    }
}
/* Poll until no pass has started for five settle times. */
static void inventory_settle(void)
{
    ULONGLONG start = GetTickCount64(), last = start;
    for (int before = g_scan_calls; GetTickCount64() - last < INVENTORY_QUIET_MS * 5 && GetTickCount64() - start < 30000;) {
        sh_decl_server_rearm_poll(); Sleep(5);
        if (g_scan_calls != before) { before = g_scan_calls; last = GetTickCount64(); }
    }
}
static int inventory_remove_tree(const char *path)
{
    char pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    _snprintf_s(pattern, sizeof(pattern), _TRUNCATE, "%s\\*", path);
    search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
            _snprintf_s(child, sizeof(child), _TRUNCATE, "%s\\%s", path, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!inventory_remove_tree(child)) { FindClose(search); return 0; } }
            else if (!DeleteFileA(child)) { FindClose(search); return 0; }
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    return RemoveDirectoryA(path) != 0;
}
static void runtime_fixture(void)
{
    runtime_fixture_write();
    CHECK(sh_package_runtime_refresh(g_fixture_root));
}
unsigned long sh_overrides_rescan_packages(void)
{
    g_scan_calls++;
    if (g_scan_fail) return SH_OVERRIDES_RESCAN_FAILED;
    runtime_fixture(); return 1;
}

/* When set, the author corrects the package while this pass reads it: the pass
 * still fails, and the file changes after its inventory was taken. */
static const char *g_scan_correct_path;
unsigned long sh_overrides_rescan_packages_activated(sh_package_activation_fn activate, void *context)
{
    g_scan_calls++;
    g_scan_watched = sh_package_runtime_test_watched_change();
    if (g_scan_correct_path) {
        inventory_write(g_scan_correct_path, "corrected during the pass");
        g_scan_correct_path = NULL; g_scan_fail = 0;
        return SH_OVERRIDES_RESCAN_FAILED;
    }
    if (g_scan_fail) return SH_OVERRIDES_RESCAN_FAILED;
    runtime_fixture_write();
    return sh_package_runtime_refresh_activated(g_fixture_root, (sh_package_activation_guard){0},
        activate, context) ? 1 : SH_OVERRIDES_RESCAN_FAILED;
}

int sh_overrides_activate_map(sh_package_map_plan *plan, sh_package_activation_fn activate, void *context)
{
    return sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){0}, activate, context);
}
int sh_overrides_activate_occurrences(const sh_package_occurrence_snapshot *map,
    sh_package_activation_fn activate, void *context)
{
    return sh_package_runtime_activate_occurrences(map, (sh_package_activation_guard){0}, activate, context);
}
sh_resource_shader_state *sh_resource_graph_native_shader_state_begin(
    sh_resource_shader_admission admitted, void *context, char *error, size_t capacity)
{
    (void)error; (void)capacity;
    CHECK(g_shader_fixture_active && !g_shader_scope_live && admitted(context));
    g_shader_scope_live = 1; g_shader_admission = admitted; g_shader_admission_context = context;
    shader_event('B'); return (sh_resource_shader_state *)1;
}
int sh_resource_graph_native_shader_state_restore(sh_resource_shader_state *state,
    char *error, size_t capacity)
{
    (void)error; (void)capacity;
    CHECK(state == (sh_resource_shader_state *)1 && g_shader_scope_live &&
        g_shader_admission(g_shader_admission_context));
    g_shader_restores++; shader_event('T'); return 1;
}
int sh_resource_graph_native_shader_state_end(sh_resource_shader_state *state,
    int accepted, char *error, size_t capacity)
{
    CHECK(state == (sh_resource_shader_state *)1 && g_shader_scope_live &&
        g_shader_admission(g_shader_admission_context));
    shader_event('E');
    if (g_shader_hold_end) { snprintf(error, capacity, "fixture restoration remains owed"); return -1; }
    g_shader_scope_live = 0; g_shader_admission = NULL; g_shader_admission_context = NULL;
    return accepted && g_shader_mode == 2 ? 0 : 1;
}
int sh_resource_graph_native_prepare_shader(void *resource, const char *family,
    const char *name, const void *source, size_t length,
    int (*admitted)(void *), void *context, char *error, size_t capacity)
{
    (void)error; (void)capacity;
    CHECK(g_shader_fixture_active && g_shader_scope_live && admitted(context));
    shader_event('P');
    if (g_shader_mode == 3) RaiseException(0xe0504453, 0, 0, NULL);
    if (g_shader_mode == 1 || g_shader_mode == 5) {
        snprintf(error, capacity, "fixture shader source failed"); return 0;
    }
    sh_package_shader_receipt_frame receipt;
    sh_package_shader_receipt_begin(&receipt, (uintptr_t)resource, family, name);
    sh_package_shader_receipt_retained(source, length);
    sh_package_shader_receipt_end(&receipt, 1, shader_native_read, NULL);
    return 1;
}
int sh_resource_graph_native_prepare_program(void *manager, const char *name,
    const void *source, size_t length, sh_resource_shader_source_reader original,
    void *original_context, int (*admitted)(void *), void *context,
    char *error, size_t capacity)
{
    (void)manager; (void)name; (void)source; (void)length; (void)error; (void)capacity;
    CHECK(original == sh_package_native_assets_shader_original_read && original_context);
    CHECK(admitted(context));
    /* Program cooking and its isolated receipt are exercised by the native
     * graph fixture. Fixed-slot tests must never enter this route. */
    CHECK(0); return 0;
}

int sh_strids_rearm(void)
{
    g_string_refreshes++;
    if (g_fail_next_string_refresh) { g_fail_next_string_refresh = 0; return 0; }
    return g_runtime_dependencies_ok;
}

int sh_weapon_hud_reload(const char *data_root)
{
    (void)data_root;
    return 1;
}

/* The re-arm retires the internal decl table so it can be re-published. Not linked here. */
void sh_overrides_internal_decl_table_reopen(void)
{
}

/* A fixture that serves native reads from the published table installs this. */
static int (*g_table_merge_hook)(const sh_overrides_internal_decl_entry *entries, size_t count);
int sh_overrides_internal_decl_table_merge(
    const sh_overrides_internal_decl_entry *entries, size_t count)
{
    g_table_merges++;
    return g_table_merge_hook ? g_table_merge_hook(entries, count) : 1;
}

/* The re-arm applies the package's cut-content gates before registering. Not linked here. */
int sh_package_requirements_rearm(const char *data_root, void *execute_command_buffer,
                                  int user_layer_enabled)
{
    (void)data_root; (void)user_layer_enabled;
    return g_runtime_dependencies_ok && sh_package_requirements_apply_now(execute_command_buffer);
}





uintptr_t sig_addr_by_name(const sig_result *results, size_t count, const char *name)
{
    (void)results;
    (void)count;
    (void)name;
    return 0;
}
/* Without an engine image, the globals resolver returns no list head and
 * ds_res_walk visits nothing. */
uintptr_t glb_resolve(const uint8_t *module_base, const char *name, glb_status *out_status)
{
    (void)module_base;
    (void)name;
    if (out_status) *out_status = GLB_ANCHOR_NOT_FOUND;
    if (g_boundary_unknown) return 0;
    if (!strcmp(name, "editor_singleton")) return (uintptr_t)g_boundary_editor;
    if (!strcmp(name, "shell_ptr_slot")) return (uintptr_t)&g_boundary_shell_slot;
    if (!strcmp(name, "validator_manager")) return (uintptr_t)&g_boundary_common_slot;
    if (!strcmp(name, "main_thread_id")) return (uintptr_t)&g_boundary_thread;
    if (!strcmp(name, "decl_resource_head")) return (uintptr_t)&g_boundary_resource_head;
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

static void *classify_find_decl(void *type_manager, const char *logical_name)
{
    (void)type_manager;
    (void)logical_name;
    g_classify_find_calls++;
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

    /* Use the real registry reader. An archive-only identity must stay absent
     * during classification, and an existing pending object must stay pending.
     * Neither case may invoke the native loader after the resident snapshot. */
    {
        unsigned char manager[0x30] = {0}, object[0x50] = {0};
        unsigned char before[sizeof(object)];
        void *entries[] = {object};
        classify_reset();
        sh_decl_server_test_set_reconstruct(NULL, NULL);
        g_classify_manager = manager;
        *(void ***)(manager + 0x20) = entries;
        *(const char **)(object + 8) = "custom/entity";
        object[0x2c] = 2;
        memcpy(before, object, sizeof(object));
        CHECK(sh_decl_server_test_classify_candidate("snapEditorEntityDef", "custom/entity",
            manager, classify_type_by_name, classify_source_find, NULL) ==
            SH_DECL_SERVER_TEST_CLASSIFY_MISSING);
        CHECK(*(int *)(manager + 0x28) == 0);
        *(int *)(manager + 0x28) = 1;
        CHECK(sh_decl_server_test_classify_candidate("snapEditorEntityDef", "custom/entity",
            manager, classify_type_by_name, classify_source_find, NULL) ==
            SH_DECL_SERVER_TEST_CLASSIFY_SHADOWED_LIVE);
        CHECK(!memcmp(before, object, sizeof(object)));
        CHECK(g_classify_find_calls == 0);
    }
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
    decl_memory[0x2c] = 0x00;
    *(void **)(decl_memory + 0x1c8) = decl_memory + 0x200;
    run_materialize_case(mixed, sizeof(mixed) / sizeof(mixed[0]), 1, 2);
    CHECK(g_materialize_type_calls == 2);
    CHECK(g_materialize_find_calls == 4);
    CHECK(g_materialize_make_default == 1);

    /* The native palette validator rejects an editor entity whose entityDef is
     * unresolved, so this service refuses it before the rebuild. */
    materialize_reset();
    g_materialize_decl = abstract_memory;
    abstract_memory[0x2c] = 0x00;
    *(void **)(abstract_memory + 0x1c8) = NULL;
    run_materialize_case(one, 1, 0, 0);

    /* Successful Read leaves the default/fallback bit clear. */
    materialize_reset();
    g_materialize_decl = decl_memory;
    decl_memory[0x2c] = 0x00;
    *(void **)(decl_memory + 0x1c8) = decl_memory + 0x200;
    run_materialize_case(one, 1, 1, 1);

    materialize_reset();
    g_materialize_decl = decl_memory;
    decl_memory[0x2c] = 0x01;
    run_materialize_case(one, 1, 0, 0);
    decl_memory[0x2c] = 0x00;

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
    decl_memory[0x2c] = 0x00;
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

    /* Active-editor admission uses the same scan/materialize ordering but
     * leaves whole-palette and resident refresh to its fresh-only owner. */
    pipeline_reset(); g_pipeline_decl = decl_memory;
    sh_decl_server_test_set_additive(1);
    sh_decl_server_test_set_resident((void *)1);
    int drains_before = g_resident_drain_calls;
    ok = sh_decl_server_test_scan_and_materialize_missing(
        items, sizeof(items) / sizeof(items[0]), (void *)(uintptr_t)0x98760000u,
        pipeline_type_by_name, pipeline_source_find, pipeline_find_decl,
        pipeline_palette_refresh, pipeline_ctor, pipeline_dtor, pipeline_register_file,
        &registered, &materialized, &failure_phase);
    sh_decl_server_test_set_additive(0); sh_decl_server_test_set_resident(NULL);
    CHECK(ok && registered == 2 && materialized == 2 && !failure_phase);
    CHECK(!g_pipeline_palette_calls && g_resident_drain_calls == drains_before);
    CHECK(g_pipeline_event_count == 12 && !memcmp(g_pipeline_events, "CSDCSDTFFTFF", 12));

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

    pipeline_reset();
    g_pipeline_decl = decl_memory;
    g_visibility_result = 0;
    ok = sh_decl_server_test_scan_and_materialize_missing(
        items, sizeof(items) / sizeof(items[0]),
        (void *)(uintptr_t)0x98760000u, pipeline_type_by_name, pipeline_source_find,
        pipeline_find_decl, pipeline_palette_refresh, pipeline_ctor,
        pipeline_dtor, pipeline_register_file, &registered, &materialized, &failure_phase);
    CHECK(ok == 0 && failure_phase == SH_DECL_SERVER_TEST_PHASE_VISIBILITY);
    CHECK(g_pipeline_palette_calls == 0);
    g_visibility_result = 1;
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
    decl_memory[0x2c] = 0x00;
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

/* The synthetic package exercises generic materialization order: non-editor
 * identities first. Existing source and live shadows must not be synthesized. */
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
    decl_memory[0x2c] = 0x00;
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

/* A private child perk named before its private parent existed was created on
 * demand and completed with its default. Reached in dependency order, after
 * the parent, it is rebuilt and parsed from its source; an identity that
 * parsed is left alone, and a child still ahead of its parent stays refused. */
static unsigned char *g_rd_parent, *g_rd_child;
static int g_rd_parent_made, g_rd_reconstructs, g_rd_child_parses;
static void *rd_type_by_name(void *registry, const char *short_name)
{ (void)registry; (void)short_name; return (void *)(uintptr_t)0x44000000u; }
static void *rd_source_find(void *type_manager, const char *logical_name)
{ (void)type_manager; (void)logical_name; return (void *)(uintptr_t)0x44100000u; }
static void *rd_find_decl(void *type_manager, const char *logical_name, unsigned char make_default)
{
    (void)type_manager;
    if (!strcmp(logical_name, "smp/parent")) {
        if (make_default) g_rd_parent_made = 1;
        return g_rd_parent_made ? g_rd_parent : NULL;
    }
    if (strcmp(logical_name, "smp/child")) return NULL;
    /* A pending lookup parses a rebuilt object (the child inherits the
     * parent); a defaulted object only drops the request, as observed. */
    if (g_rd_child[0x2c] & 0x02) {
        if (g_rd_child[0x2c] & 0x04) g_rd_child[0x2c] &= (unsigned char)~0x02;
        else { g_rd_child_parses++; g_rd_child[0x2c] = g_rd_parent_made ? 0x00 : 0x04; }
    }
    return g_rd_child;
}
static void rd_reconstruct(void *decl) { g_rd_reconstructs++; ((unsigned char *)decl)[0x2c] = 0x00; }

static void test_defaulted_identity_reparse(void)
{
    static const unsigned char parent_body[] = "{ edit = { maxNumUses = 4; } }";
    static const unsigned char child_body[] = "{ inherit = \"smp/parent\"; edit = { upgradeStat = \"STAT_INVALID\"; } }";
    sh_decl_server_test_materialize_item items[] = {
        { "perks", "smp/parent", "parent", SH_DECL_SERVER_TEST_MISSING, parent_body, sizeof(parent_body) - 1 },
        { "perks", "smp/child", "child", SH_DECL_SERVER_TEST_MISSING, child_body, sizeof(child_body) - 1 }
    }, reversed[2];
    int materialized = 0;
    g_rd_parent = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    g_rd_child = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    CHECK(g_rd_parent && g_rd_child);
    if (!g_rd_parent || !g_rd_child) return;
    sh_decl_server_test_set_reconstruct(rd_reconstruct, NULL);
    g_rd_parent_made = g_rd_reconstructs = g_rd_child_parses = 0;
    g_rd_child[0x2c] = 0x04;
    CHECK(sh_decl_server_test_materialize_missing_sedefs(items, 2, (void *)(uintptr_t)0x44200000u,
        rd_type_by_name, rd_source_find, rd_find_decl, &materialized) == 1);
    CHECK(materialized == 1 && g_rd_reconstructs == 1 && g_rd_child_parses == 1 && g_rd_child[0x2c] == 0x00);
    /* An existing identity that parsed is not rebuilt. */
    g_rd_parent_made = g_rd_reconstructs = g_rd_child_parses = 0; materialized = 0;
    CHECK(sh_decl_server_test_materialize_missing_sedefs(items, 2, (void *)(uintptr_t)0x44200000u,
        rd_type_by_name, rd_source_find, rd_find_decl, &materialized) == 1);
    CHECK(materialized == 1 && !g_rd_reconstructs && !g_rd_child_parses);
    /* Ahead of its parent the rebuilt child still completes with its default. */
    reversed[0] = items[1]; reversed[1] = items[0];
    g_rd_parent_made = g_rd_reconstructs = g_rd_child_parses = 0; materialized = 0;
    g_rd_child[0x2c] = 0x04;
    CHECK(sh_decl_server_test_materialize_missing_sedefs(reversed, 2, (void *)(uintptr_t)0x44200000u,
        rd_type_by_name, rd_source_find, rd_find_decl, &materialized) == 0);
    CHECK(g_rd_reconstructs == 1 && g_rd_child[0x2c] == 0x04);
    sh_decl_server_test_set_reconstruct(NULL, NULL);
    HeapFree(GetProcessHeap(), 0, g_rd_parent); HeapFree(GetProcessHeap(), 0, g_rd_child);
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
    valid_memory[0x2c] = 0x00;
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
    invalid_memory[0x2c] = 0x00;
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
    /* Shipped declaration names use these characters. */
    expect_path("material/models/weapons/bfg/bfg_3p_[3p_lp].decl",
                "material", "models/weapons/bfg/bfg_3p_[3p_lp]",
                "generated/decls/material/models/weapons/bfg/bfg_3p_[3p_lp].decl");
    expect_path("table/autogen/creature/archvile/shockwave_single/stage 2/c_0.decl",
                "table", "autogen/creature/archvile/shockwave_single/stage 2/c_0",
                "generated/decls/table/autogen/creature/archvile/shockwave_single/stage 2/c_0.decl");
    expect_path("unlockable/challenges/pvp/can't_fight_the_power_challenge.decl",
                "unlockable", "challenges/pvp/can't_fight_the_power_challenge",
                "generated/decls/unlockable/challenges/pvp/can't_fight_the_power_challenge.decl");
    expect_path("material/models/mapobjects/prop/hell/soulcube_[mesh_(2)].decl",
                "material", "models/mapobjects/prop/hell/soulcube_[mesh_(2)]",
                "generated/decls/material/models/mapobjects/prop/hell/soulcube_[mesh_(2)].decl");

    refuse_path("actor modifier/cacodemon.decl", "type");
    refuse_path("actormodifier/demon ;evil.decl", "character");
    refuse_path("actormodifier/demon;evil.decl", "character");
    refuse_path("actormodifier/demon{evil}.decl", "character");
    refuse_path("actormodifier/demon=evil.decl", "character");
    refuse_path("actormodifier/demon\"evil.decl", "character");
    refuse_path("actormodifier/demon,evil.decl", "character");
    refuse_path("actormodifier/demon /rune.decl", "space");
    refuse_path("actormodifier/demon .decl", "space");
    refuse_path("actormodifier/.decl", "name");
}

static void test_text_validation(void)
{
    static const unsigned char valid[] =
        "// opening brace in a comment: {\n"
        "{\n"
        "  inherit = \"player\\{ignored}\\\"\n"
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
    CHECK(sh_decl_server_test_body_is_single_block(valid, sizeof(valid) - 1) == 1);
    {
        const unsigned char child[] = "{ inherit = \"z\\parent\"; }";
        const unsigned char parent[] = "{}";
        sh_decl_reference_item items[] = {
            {"a/child", child, sizeof(child) - 1}, {"z/parent", parent, sizeof(parent) - 1}
        };
        size_t edges = 0, cycles = 0;
        CHECK(sh_decl_text_order_by_references(items, 2, &edges, &cycles));
        CHECK(edges == 1 && !cycles && !strcmp(items[0].name, "z/parent"));
    }
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

static void test_material_table_dependency_ordering(void)
{
    static const unsigned char material[] =
        "{ temp0 Z_BLINK[time] temp1 z_blink [time * 2] "
        "// comment_table[time]\n"
        "/* block_table[time] */ label \"string_table[time]\" "
        "temp2 prefix_table_suffix[time] }";
    static const unsigned char non_material[] = "{ z_blink[0] = 1; }";
    static const unsigned char table[] = "{ { 0, 1 } }";
    sh_decl_reference_item items[] = {
        { "a/material", material, sizeof(material) - 1, (void *)1, "material" },
        { "z_blink", table, sizeof(table) - 1, (void *)2, "table" },
        { "z_blink", table, sizeof(table) - 1, (void *)3, "entityDef" },
        { "comment_table", table, sizeof(table) - 1, (void *)4, "table" },
        { "block_table", table, sizeof(table) - 1, (void *)5, "table" },
        { "string_table", table, sizeof(table) - 1, (void *)6, "table" },
        { "table_suffix", table, sizeof(table) - 1, (void *)7, "table" },
        { "a/entity", non_material, sizeof(non_material) - 1, (void *)8, "entityDef" }
    };
    size_t edges = 0, cycles = 0, i, table_index = 99, material_index = 99;
    CHECK(sh_decl_text_order_by_references(items, sizeof(items) / sizeof(items[0]),
                                           &edges, &cycles) == 1);
    CHECK(edges == 1);
    CHECK(cycles == 0);
    for (i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
        if (items[i].value == (void *)1) material_index = i;
        if (items[i].value == (void *)2) table_index = i;
    }
    CHECK(table_index < material_index);
}

/* A boss (parent) lists its AI component, the component's groupEntityDefs
 * load its weapon entities, and those inherit the boss: one component of the
 * reference graph. Materializing the component loads the weapons, so their
 * parent must come first even when the component's name sorts earlier. */
static void test_scc_parent_before_loading_member(void)
{
    static const unsigned char boss[] =
        "{ edit = { components = { ptr[0] = { componentDecl = \"smp/22d0\"; } } } }";
    static const unsigned char guard[] = "{ edit = { groupEntityDefs = { item[0] = \"smp/47f2\"; } } }";
    static const unsigned char hammer[] = "{ inherit = \"smp/665b\"; }";
    sh_decl_reference_item items[] = {
        { "smp/22d0", guard, sizeof(guard) - 1, (void *)(uintptr_t)1 },
        { "smp/47f2", hammer, sizeof(hammer) - 1, (void *)(uintptr_t)2 },
        { "smp/665b", boss, sizeof(boss) - 1, (void *)(uintptr_t)3 }
    };
    size_t edges = 0, cycles = 0;
    CHECK(sh_decl_text_order_by_references(items, 3, &edges, &cycles) == 1);
    CHECK(edges == 3 && cycles == 3);
    CHECK(strcmp(items[0].name, "smp/665b") == 0);
    CHECK(strcmp(items[1].name, "smp/22d0") == 0);
    CHECK(strcmp(items[2].name, "smp/47f2") == 0);
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


/* Mark all empty reused placeholders before the first drain so parse-time
 * references can reload their targets. Re-parse changed live shadows in
 * place regardless of type or a prior pass.
 * The browser-safe pass must clear every pending mark. The lookup double
 * clears pending bit 0x02. Successful parsing leaves the fallback bit clear. */

#define RT_DECLS 4
static unsigned char *g_rt_objects[RT_DECLS];
static const char *g_rt_names[RT_DECLS];
static int g_rt_loads[RT_DECLS];
static int g_rt_first_load_saw_partner_marked = -1;
static int g_rt_load_total;
static int g_rt_null_on_load;
static int g_rt_no_drain;          /* lookup returns the object without draining the mark */
static int g_rt_direct_loads;      /* fake generic-load fallback invocations */
static int g_rt_capture_body, g_rt_body_count;
static char g_rt_bodies[4][128];
static int g_rt_reconstructs;
static int g_rt_reconstruct_fault;
static unsigned int g_rt_reconstruct_levels[RT_DECLS];
static struct {
    char name[SH_DECL_SERVER_NAME_CAP];
    unsigned char object[0x600];
    int loads;
} g_rt_builtins[5];
static size_t g_rt_builtin_count;
static unsigned char g_rt_builtin_node[0x30];
static void *g_rt_builtin_resources[5];
static void rt_reconstruct(void *decl);
static void *rt_peek(void *manager, const char *name, unsigned char make_default);

static int rt_builtin_index(const char *name, const void *object)
{
    size_t i;
    for (i = 0; i < g_rt_builtin_count; i++)
        if ((name && !strcmp(name, g_rt_builtins[i].name)) || object == g_rt_builtins[i].object) return (int)i;
    return -1;
}

static void rt_builtin_fixture(void)
{
    const sh_package_compilation *compiled = sh_package_runtime_acquire();
    size_t i, j;
    CHECK(compiled && compiled->resource_count == 5);
    g_rt_builtin_count = 0;
    memset(g_rt_builtins, 0, sizeof(g_rt_builtins));
    if (compiled) for (i = 0; i < compiled->resource_count && i < 5; i++) {
        const sh_compiled_resource *resource = &compiled->resources[i];
        CHECK(!sh_package_owners_count(&resource->owners) && !resource->source_count && resource->name);
        for (j = 0; j < i; j++) CHECK(strcmp(resource->name, g_rt_builtins[j].name));
        strcpy_s(g_rt_builtins[i].name, sizeof(g_rt_builtins[i].name), resource->name);
        *(unsigned int *)(g_rt_builtins[i].object + 0x28) = 4;
        *(void **)(g_rt_builtins[i].object + 0x1c8) = g_rt_builtins[i].object + 0x200;
        g_rt_builtin_resources[i] = g_rt_builtins[i].object;
        g_rt_builtin_count++;
    }
    memset(g_rt_builtin_node, 0, sizeof(g_rt_builtin_node));
    *(void **)(g_rt_builtin_node + 0x20) = g_rt_builtin_resources;
    *(int *)(g_rt_builtin_node + 0x28) = (int)g_rt_builtin_count;
    g_boundary_resource_head = g_rt_builtin_node;
    sh_package_runtime_release();
}

static void rt_reset(void)
{
    int i;
    for (i = 0; i < RT_DECLS; i++) {
        g_rt_objects[i] = NULL;
        g_rt_names[i] = NULL;
        g_rt_loads[i] = 0;
        g_rt_reconstruct_levels[i] = 0;
    }
    g_rt_first_load_saw_partner_marked = -1;
    g_rt_load_total = 0;
    g_rt_null_on_load = 0;
    g_rt_no_drain = 0;
    g_rt_direct_loads = 0;
    g_rt_reconstructs = 0;
    g_rt_reconstruct_fault = 0;
    sh_decl_server_test_set_reconstruct(rt_reconstruct, rt_peek);
}

static void rt_reconstruct(void *decl)
{
    int i;
    if (rt_builtin_index(NULL, decl) >= 0) {
        CHECK(*(unsigned int *)((unsigned char *)decl + 0x28) != 4u);
        *(unsigned int *)((unsigned char *)decl + 0x28) = 1;
        ((unsigned char *)decl)[0x2c] = 0;
        *(void **)((unsigned char *)decl + 0x18) = NULL;
        return;
    }
    for (i = 0; i < RT_DECLS; i++)
        if (g_rt_objects[i]) CHECK(!(g_rt_objects[i][0x2c] & 2));
    CHECK(*(unsigned int *)((unsigned char *)decl + 0x28) != 4u);
    if (g_rt_reconstructs < RT_DECLS)
        g_rt_reconstruct_levels[g_rt_reconstructs] =
            *(unsigned int *)((unsigned char *)decl + 0x28);
    /* The native constructor chooses a fresh lifetime; the caller must
     * restore permanent lifetime before any dependent parse can run. */
    *(unsigned int *)((unsigned char *)decl + 0x28) = 1u;
    if (g_rt_reconstruct_fault)
        RaiseException(0xe0011001u, 0, 0, NULL);
    ((unsigned char *)decl)[0x2c] = 0;
    *(void **)((unsigned char *)decl + 0x18) = NULL;
    g_rt_reconstructs++;
}

/* The generic-load fallback re-parses in place and clears the default state.
 * The caller must clear pending before invoking it. */
static void rt_generic_load(void *decl)
{
    CHECK(*(unsigned int *)((unsigned char *)decl + 0x28) == 4u);
    heap_observe_native(0);
    ((unsigned char *)decl)[0x2c] &= (unsigned char)~0x04;
    *(void **)((unsigned char *)decl + 0x18) = NULL;
    g_rt_direct_loads++;
}

static int rt_index_of(const char *name)
{
    int i;
    for (i = 0; i < RT_DECLS; i++)
        if (g_rt_names[i] && name && strcmp(g_rt_names[i], name) == 0) return i;
    return -1;
}

static void *rt_peek(void *manager, const char *name, unsigned char make_default)
{
    int index = rt_index_of(name);
    (void)manager; (void)make_default;
    if (index < 0) {
        index = rt_builtin_index(name, NULL);
        return index < 0 ? NULL : g_rt_builtins[index].object;
    }
    return index < 0 ? NULL : g_rt_objects[index];
}

static void *rt_find_decl(void *type_manager, const char *logical_name,
                          unsigned char make_default)
{
    heap_observe_native(0);
    int index = rt_index_of(logical_name);
    unsigned char *decl;
    (void)type_manager;
    (void)make_default;
    if (index < 0) {
        index = rt_builtin_index(logical_name, NULL);
        if (index < 0) return NULL;
        decl = g_rt_builtins[index].object;
        if (decl[0x2c] & 0x02) {
            decl[0x2c] &= (unsigned char)~0x06;
            *(void **)(decl + 0x18) = NULL;
            g_rt_builtins[index].loads++;
        }
        return decl;
    }
    decl = g_rt_objects[index];
    if (!decl) return NULL;
    if (decl[0x2c] & 0x02) {
        if (g_rt_null_on_load) return NULL;
        if (g_rt_no_drain) return decl;
        if (g_rt_capture_body && !strcmp(logical_name, "rt/alpha")) {
            unsigned char *body = NULL; size_t length = 0;
            CHECK(sh_package_runtime_read("generated/decls/material/rt/alpha.decl", &body, &length) == 1);
            CHECK(body && g_rt_body_count < 4);
            if (body && g_rt_body_count < 4)
                snprintf(g_rt_bodies[g_rt_body_count++], sizeof(g_rt_bodies[0]), "%s", (char *)body);
            free(body);
        }
        if (g_rt_load_total == 0) {
            /* At the first drain, every other empty placeholder must already be marked. */
            int partner = (index == 0) ? 1 : 0;
            g_rt_first_load_saw_partner_marked =
                (g_rt_objects[partner] && (g_rt_objects[partner][0x2c] & 0x02)) ? 1 : 0;
        }
        decl[0x2c] &= (unsigned char)~0x06;
        *(void **)(decl + 0x18) = NULL;
        g_rt_loads[index]++;
        g_rt_load_total++;
    }
    return decl;
}

static void *rt_type_by_name(void *registry, const char *short_name)
{
    (void)registry;
    (void)short_name;
    return (void *)(uintptr_t)0x52540000u;
}

static void *rt_source_find(void *type_manager, const char *logical_name)
{
    (void)type_manager;
    (void)logical_name;
    return NULL;
}

static void test_runtime_premark_reused_empties(void)
{
    static const unsigned char body[] = "{ }";
    sh_decl_server_test_materialize_item items[] = {
        { "material", "rt/alpha", "generated/decls/material/rt/alpha.decl",
          SH_DECL_SERVER_TEST_MISSING, body, sizeof(body) - 1, 0 },
        { "material", "rt/beta", "generated/decls/material/rt/beta.decl",
          SH_DECL_SERVER_TEST_MISSING, body, sizeof(body) - 1, 0 }
    };
    unsigned char *a;
    unsigned char *b;
    int materialized = -1;
    long marked = -1, left = -1, shadow = -1, faults = -1;

    a = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    b = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    CHECK(a != NULL && b != NULL);
    if (!a || !b) return;

    /* Failed source lookups leave native default resources, not successful
     * zero-state parses. Cover both a default and an explicit error record. */
    a[0x2c] = 0x04;
    *(void **)(b + 0x18) = (void *)(uintptr_t)1;

    rt_reset();
    g_rt_names[0] = "rt/alpha";
    g_rt_objects[0] = a;
    g_rt_names[1] = "rt/beta";
    g_rt_objects[1] = b;
    sh_decl_server_test_reset_runtime_state();
    sh_decl_server_test_set_runtime(1);
    CHECK(sh_decl_server_test_materialize_missing_sedefs(
              items, 2, (void *)(uintptr_t)0x98760000u, rt_type_by_name,
              rt_source_find, rt_find_decl, &materialized) == 1);
    sh_decl_server_test_set_runtime(0);

    /* Both placeholders reload once; all marks must precede the first drain. */
    CHECK(g_rt_loads[0] == 1);
    CHECK(g_rt_loads[1] == 1);
    CHECK(g_rt_reconstructs == 2);
    CHECK(g_rt_first_load_saw_partner_marked == 1);
    CHECK((a[0x2c] & 0x02) == 0);
    CHECK((b[0x2c] & 0x02) == 0);
    CHECK(materialized == 0);
    sh_decl_server_test_runtime_counters(&marked, &left, &shadow, &faults);
    CHECK(marked == 2);
    CHECK(left == 0);
    CHECK(shadow == 0);
    CHECK(faults == 0);
    CHECK(sh_decl_server_test_clear_stray_pending() == 0);
    HeapFree(GetProcessHeap(), 0, a);
    HeapFree(GetProcessHeap(), 0, b);
}

/* A live declaration naming a new identity (the stock player hands web naming
 * a package's private model) re-parses only after that identity exists: the
 * pass follows the snapshot's dependency order, not reloads first. */
static unsigned char g_order_model[0x300];
static int g_order_clock, g_order_model_made, g_order_web_loaded;
static void *rt_order_find(void *type_manager, const char *logical_name, unsigned char make_default)
{
    if (logical_name && !strcmp(logical_name, "rt/model")) {
        if (make_default && !g_order_model_made) g_order_model_made = ++g_order_clock;
        return g_order_model_made ? g_order_model : NULL;
    }
    if (logical_name && !strcmp(logical_name, "rt/web") && g_rt_objects[0] &&
        (g_rt_objects[0][0x2c] & 0x02) && !g_order_web_loaded)
        g_order_web_loaded = ++g_order_clock;
    return rt_find_decl(type_manager, logical_name, make_default);
}
static void test_runtime_reload_follows_new_dependencies(void)
{
    static const unsigned char body[] = "{ }";
    sh_decl_server_test_materialize_item items[] = {
        { "md6Def", "rt/model", "generated/decls/md6def/rt/model.decl",
          SH_DECL_SERVER_TEST_MISSING, body, sizeof(body) - 1, 0 },
        { "animWeb", "rt/web", "generated/decls/animweb/rt/web.decl",
          SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE }
    };
    unsigned char web[0x300] = {0};
    int materialized = -1;
    memset(g_order_model, 0, sizeof(g_order_model));
    g_order_clock = g_order_model_made = g_order_web_loaded = 0;
    rt_reset(); sh_decl_server_test_reset_runtime_state();
    g_rt_names[0] = "rt/web"; g_rt_objects[0] = web;
    *(unsigned int *)(web + 0x28) = 4u;
    sh_decl_server_test_set_runtime(1);
    CHECK(sh_decl_server_test_materialize_missing_sedefs(items, 2, (void *)(uintptr_t)0x98760000u,
        rt_type_by_name, rt_source_find, rt_order_find, &materialized) == 1);
    sh_decl_server_test_set_runtime(0);
    CHECK(materialized == 1 && g_rt_loads[0] == 1);
    CHECK(g_order_model_made && g_order_web_loaded && g_order_model_made < g_order_web_loaded);
    CHECK((web[0x2c] & 0x02) == 0 && sh_decl_server_test_clear_stray_pending() == 0);
    rt_reset(); sh_decl_server_test_reset_runtime_state();
}

static void test_runtime_preserves_unaffected_caches(void)
{
    unsigned char objects[2][0x800] = {0};
    sh_decl_server_test_materialize_item items[] = {
        {"snapEditorSettings", "settings", "generated/decls/snapeditorsettings/settings.decl",
         SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE},
        {"entityDef", "changed", "generated/decls/entitydef/changed.decl",
         SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE}
    };
    int materialized;
    rt_reset(); sh_decl_server_test_reset_runtime_state();
    for (int i = 0; i < 2; i++) {
        g_rt_names[i] = items[i].name; g_rt_objects[i] = objects[i];
        *(unsigned *)(objects[i] + 0x28) = 4;
    }
    *(void **)(objects[0] + 0x688) = objects[0];
    sh_decl_server_test_set_runtime(1);
    sh_decl_server_test_set_resident((void *)1);
    g_resident_filter = 1; g_resident_only = NULL;
    CHECK(sh_decl_server_test_materialize_missing_sedefs(items, 2, (void *)1,
        rt_type_by_name, rt_source_find, rt_find_decl, &materialized));
    CHECK(g_rt_reconstructs == 0 && g_rt_load_total == 0);
    CHECK(*(void **)(objects[0] + 0x688) == objects[0]);
    /* A changed dependency can select a consumer even if its own bytes did
     * not change. The declaration pass follows the captured resident set. */
    g_resident_only = objects[1];
    g_resident_leaf_calls = 0; g_resident_leaf_loads_seen = -1;
    CHECK(sh_decl_server_test_materialize_missing_sedefs(items, 2, (void *)1,
        rt_type_by_name, rt_source_find, rt_find_decl, &materialized));
    CHECK(g_rt_reconstructs == 1 && g_rt_loads[0] == 0 && g_rt_loads[1] == 1);
    CHECK(g_resident_leaf_calls == 1 && g_resident_leaf_loads_seen == 0);
    CHECK(*(void **)(objects[0] + 0x688) == objects[0]);
    sh_decl_server_test_set_resident(NULL); sh_decl_server_test_set_runtime(0);
    g_resident_filter = 0; g_resident_only = NULL;
    sh_decl_server_test_reset_runtime_state();
}

static void test_runtime_shadowed_refresh(void)
{
    sh_decl_server_test_materialize_item items[] = {
        { "entityDef", "rt/newly", "generated/decls/entitydef/rt/newly.decl",
          SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE },
        { "entityDef", "rt/served", "generated/decls/entitydef/rt/served.decl",
          SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE },
        { "entityDef", "rt/sourcekind", "generated/decls/entitydef/rt/sourcekind.decl",
          SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_SOURCE },
        { "sound", "rt/midgame", "generated/decls/sound/rt/midgame.decl",
          SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE }
    };
    unsigned char *newly;
    unsigned char *served;
    unsigned char *sourcekind;
    unsigned char *midgame;
    int materialized = -1;
    long marked = -1, left = -1, shadow = -1, faults = -1;

    newly = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    served = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    sourcekind = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    CHECK(newly != NULL && served != NULL && sourcekind != NULL);
    if (!newly || !served || !sourcekind) return;

    /* All three already hold loaded data -- the stale-content case. */
    newly[0x2c] = 0;
    served[0x2c] = 0;
    sourcekind[0x2c] = 0;

    rt_reset();
    g_rt_names[0] = "rt/newly";
    g_rt_objects[0] = newly;
    g_rt_names[1] = "rt/served";
    g_rt_objects[1] = served;
    g_rt_names[2] = "rt/sourcekind";
    g_rt_objects[2] = sourcekind;
    midgame = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    CHECK(midgame != NULL);
    if (!midgame) return;
    midgame[0x2c] = 0;
    *(unsigned int *)(newly + 0x28) = 4u;
    *(unsigned int *)(served + 0x28) = 1u;
    *(unsigned int *)(sourcekind + 0x28) = 2u;
    *(unsigned int *)(midgame + 0x28) = 4u;
    g_rt_names[3] = "rt/midgame";
    g_rt_objects[3] = midgame;
    sh_decl_server_test_reset_runtime_state();
    sh_decl_server_test_set_runtime(1);
    CHECK(sh_decl_server_test_materialize_missing_sedefs(
              items, 4, (void *)(uintptr_t)0x98760000u, rt_type_by_name,
              rt_source_find, rt_find_decl, &materialized) == 1);
    sh_decl_server_test_set_runtime(0);

    /* Refresh every live shadow, including prior-pass and source-classified
     * entries. All pending marks must drain in this pass. */
    CHECK(g_rt_loads[0] == 1);
    CHECK(g_rt_loads[1] == 1);
    CHECK(g_rt_loads[2] == 1);
    CHECK(g_rt_loads[3] == 1);
    CHECK(g_rt_reconstructs == 4);
    CHECK(g_rt_reconstruct_levels[0] == 2u);
    CHECK(g_rt_reconstruct_levels[1] == 1u);
    CHECK(g_rt_reconstruct_levels[2] == 2u);
    CHECK(g_rt_reconstruct_levels[3] == 2u);
    CHECK(*(unsigned int *)(newly + 0x28) == 4u);
    CHECK(*(unsigned int *)(served + 0x28) == 4u);
    CHECK(*(unsigned int *)(sourcekind + 0x28) == 4u);
    CHECK(*(unsigned int *)(midgame + 0x28) == 4u);
    CHECK((newly[0x2c] & 0x02) == 0);
    CHECK((newly[0x2c] & 0x04) == 0);
    CHECK((served[0x2c] & 0x02) == 0);
    CHECK((sourcekind[0x2c] & 0x02) == 0);
    CHECK((midgame[0x2c] & 0x02) == 0);
    CHECK((midgame[0x2c] & 0x04) == 0);
    sh_decl_server_test_runtime_counters(&marked, &left, &shadow, &faults);
    CHECK(marked == 0);
    CHECK(shadow == 4);
    CHECK(faults == 0);
    CHECK(g_rt_direct_loads == 0);
    CHECK(sh_decl_server_test_clear_stray_pending() == 0);
    HeapFree(GetProcessHeap(), 0, newly);
    HeapFree(GetProcessHeap(), 0, served);
    HeapFree(GetProcessHeap(), 0, sourcekind);
    HeapFree(GetProcessHeap(), 0, midgame);
}

static void test_runtime_reconstruction_fault_restores_lifetime(void)
{
    unsigned char decl[0x300] = {0};
    sh_decl_server_test_materialize_item item = {
        "material", "rt/throw", "generated/decls/material/rt/throw.decl",
        SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE
    };
    int materialized = -1;
    long faults = 0;
    rt_reset();
    g_rt_names[0] = "rt/throw";
    g_rt_objects[0] = decl;
    *(unsigned int *)(decl + 0x28) = 4u;
    g_rt_reconstruct_fault = 1;
    sh_decl_server_test_reset_runtime_state();
    sh_decl_server_test_set_runtime(1);
    CHECK(!sh_decl_server_test_materialize_missing_sedefs(&item, 1,
        (void *)1, rt_type_by_name, rt_source_find, rt_find_decl, &materialized));
    sh_decl_server_test_runtime_counters(NULL, NULL, NULL, &faults);
    CHECK(faults == 1 && g_rt_load_total == 0);
    CHECK(g_rt_reconstruct_levels[0] == 2u);
    CHECK(*(unsigned int *)(decl + 0x28) == 4u);
    CHECK((decl[0x2c] & 2) == 0);
    CHECK(sh_decl_server_test_clear_stray_pending() == 0);

    g_rt_reconstruct_fault = 0;
    sh_decl_server_test_reset_runtime_state();
    CHECK(sh_decl_server_test_materialize_missing_sedefs(&item, 1,
        (void *)1, rt_type_by_name, rt_source_find, rt_find_decl, &materialized));
    CHECK(g_rt_load_total == 1 && *(unsigned int *)(decl + 0x28) == 4u);
    CHECK(sh_decl_server_test_clear_stray_pending() == 0);
    sh_decl_server_test_set_runtime(0);
}

static void test_rejected_new_identity_does_not_strand_replacements(void)
{
    unsigned char a[0x300] = {0}, b[0x300] = {0};
    sh_decl_server_test_materialize_item items[] = {
        { "material", "rt/rejected", "generated/decls/material/rt/rejected.decl",
          SH_DECL_SERVER_TEST_MISSING, NULL, 0, 0 },
        { "aiFSMManager", "rt/a", "generated/decls/aifsmmanager/rt/a.decl",
          SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE },
        { "weapon", "rt/b", "generated/decls/weapon/rt/b.decl",
          SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE }
    };
    int materialized = -1;
    rt_reset();
    g_rt_names[0] = "rt/a"; g_rt_objects[0] = a;
    g_rt_names[1] = "rt/b"; g_rt_objects[1] = b;
    sh_decl_server_test_reset_runtime_state();
    sh_decl_server_test_set_runtime(1);
    CHECK(!sh_decl_server_test_materialize_missing_sedefs(items, 3, (void *)1,
        rt_type_by_name, rt_source_find, rt_find_decl, &materialized));
    CHECK(g_rt_reconstructs == 2);
    CHECK(g_rt_loads[0] == 1 && g_rt_loads[1] == 1);
    CHECK(a[0x2c] == 0 && b[0x2c] == 0);
    CHECK(sh_decl_server_test_clear_stray_pending() == 0);
    sh_decl_server_test_set_runtime(0);
    sh_decl_server_test_reset_runtime_state();
}

static void test_runtime_shadowed_drain_fallback(void)
{
    sh_decl_server_test_materialize_item items[] = {
        { "aiFSMManager", "rt/fsm", "generated/decls/aifsmmanager/rt/fsm.decl",
          SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE }
    };
    unsigned char *fsm;
    int materialized = -1;
    long marked = -1, left = -1, shadow = -1, faults = -1;

    fsm = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    CHECK(fsm != NULL);
    if (!fsm) return;
    fsm[0x2c] = 0x04;

    rt_reset();
    g_rt_names[0] = "rt/fsm";
    g_rt_objects[0] = fsm;
    /* If lookup leaves the mark pending, clear it and invoke the resolved
     * generic-load fallback directly. */
    g_rt_no_drain = 1;
    sh_decl_server_test_reset_runtime_state();
    sh_decl_server_test_set_generic_load(rt_generic_load);
    sh_decl_server_test_set_runtime(1);
    CHECK(sh_decl_server_test_materialize_missing_sedefs(
              items, 1, (void *)(uintptr_t)0x98760000u, rt_type_by_name,
              rt_source_find, rt_find_decl, &materialized) == 1);
    sh_decl_server_test_set_runtime(0);

    CHECK(g_rt_direct_loads == 1);
    CHECK((fsm[0x2c] & 0x02) == 0);
    CHECK((fsm[0x2c] & 0x04) == 0);
    sh_decl_server_test_runtime_counters(&marked, &left, &shadow, &faults);
    CHECK(shadow == 1);
    CHECK(faults == 0);
    CHECK(sh_decl_server_test_clear_stray_pending() == 0);
    sh_decl_server_test_set_generic_load(NULL);
    HeapFree(GetProcessHeap(), 0, fsm);
}

static void test_runtime_keeps_successful_missing_identity(void)
{
    unsigned char decl[0x300] = {0};
    static const unsigned char body[] = "{}";
    sh_decl_server_test_materialize_item item = {
        "material", "rt/valid", "generated/decls/material/rt/valid.decl",
        SH_DECL_SERVER_TEST_MISSING, body, 2, 0
    };
    int materialized = -1;
    rt_reset();
    g_rt_names[0] = "rt/valid"; g_rt_objects[0] = decl;
    sh_decl_server_test_reset_runtime_state();
    sh_decl_server_test_set_runtime(1);
    CHECK(sh_decl_server_test_materialize_missing_sedefs(&item, 1,
        (void *)1, rt_type_by_name, rt_source_find, rt_find_decl, &materialized));
    CHECK(g_rt_reconstructs == 0 && g_rt_load_total == 0 && materialized == 0);
    CHECK(decl[0x2c] == 0);
    sh_decl_server_test_set_runtime(0);
    CHECK(sh_decl_server_test_clear_stray_pending() == 0);
}

static void rt_fallback_load(void *decl)
{
    ((unsigned char *)decl)[0x2c] |= 4;
    *(void **)((unsigned char *)decl + 0x18) = (void *)1;
}

static void test_runtime_rejects_native_fallback(void)
{
    unsigned char decl[0x300] = {0};
    sh_decl_server_test_materialize_item item = {
        "material", "rt/error", "generated/decls/material/rt/error.decl",
        SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE
    };
    int materialized = -1;
    long faults = 0, shadow = 0;
    rt_reset();
    g_rt_names[0] = "rt/error"; g_rt_objects[0] = decl; g_rt_no_drain = 1;
    sh_decl_server_test_reset_runtime_state();
    sh_decl_server_test_set_generic_load(rt_fallback_load);
    sh_decl_server_test_set_runtime(1);
    CHECK(!sh_decl_server_test_materialize_missing_sedefs(&item, 1,
        (void *)1, rt_type_by_name, rt_source_find, rt_find_decl, &materialized));
    sh_decl_server_test_runtime_counters(NULL, NULL, &shadow, &faults);
    CHECK(shadow == 0 && faults == 1);
    CHECK((decl[0x2c] & 2) == 0 && (decl[0x2c] & 4) != 0);
    sh_decl_server_test_set_runtime(0);
    CHECK(sh_decl_server_test_clear_stray_pending() == 0);
    sh_decl_server_test_reset_runtime_state();
}

static void test_runtime_shadowed_drain_failed_is_swept(void)
{
    sh_decl_server_test_materialize_item items[] = {
        { "aiFSMManager", "rt/fsm2", "generated/decls/aifsmmanager/rt/fsm2.decl",
          SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE }
    };
    unsigned char *fsm;
    int materialized = -1;
    long marked = -1, left = -1, shadow = -1, faults = -1;

    fsm = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    CHECK(fsm != NULL);
    if (!fsm) return;
    fsm[0x2c] = 0x04;

    rt_reset();
    g_rt_names[0] = "rt/fsm2";
    g_rt_objects[0] = fsm;
    /* Without a lookup drain or fallback, count no refresh and disarm the
     * shadowed object's mark before map load. */
    g_rt_no_drain = 1;
    sh_decl_server_test_reset_runtime_state();
    sh_decl_server_test_set_runtime(1);
    CHECK(sh_decl_server_test_materialize_missing_sedefs(
              items, 1, (void *)(uintptr_t)0x98760000u, rt_type_by_name,
              rt_source_find, rt_find_decl, &materialized) == 0);
    sh_decl_server_test_set_runtime(0);

    CHECK(g_rt_direct_loads == 0);
    CHECK((fsm[0x2c] & 0x02) != 0);
    sh_decl_server_test_runtime_counters(&marked, &left, &shadow, &faults);
    CHECK(shadow == 0);
    CHECK(sh_decl_server_test_clear_stray_pending() == 1);
    CHECK((fsm[0x2c] & 0x02) == 0);
    HeapFree(GetProcessHeap(), 0, fsm);
}

static void test_runtime_import_keeps_native_fallback(void)
{
    unsigned char decl[0x300] = {0};
    sh_decl_server_test_materialize_item item = {
        "material", "rt/stock", "generated/decls/material/rt/stock.decl",
        SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE, 1
    };
    int materialized = -1;
    long faults = 0, shadow = 0;
    rt_reset();
    g_rt_names[0] = "rt/stock"; g_rt_objects[0] = decl; g_rt_no_drain = 1;
    sh_decl_server_test_reset_runtime_state();
    sh_decl_server_test_set_generic_load(rt_fallback_load);
    sh_decl_server_test_set_runtime(1);
    CHECK(sh_decl_server_test_materialize_missing_sedefs(&item, 1,
        (void *)1, rt_type_by_name, rt_source_find, rt_find_decl, &materialized));
    sh_decl_server_test_runtime_counters(NULL, NULL, &shadow, &faults);
    CHECK(shadow == 1 && faults == 0);
    CHECK((decl[0x2c] & 2) == 0 && (decl[0x2c] & 4) != 0);
    CHECK(sh_decl_server_test_clear_stray_pending() == 0);
    /* A known original never excuses an unfinished load. */
    sh_decl_server_test_set_generic_load(NULL);
    CHECK(!sh_decl_server_test_materialize_missing_sedefs(&item, 1,
        (void *)1, rt_type_by_name, rt_source_find, rt_find_decl, &materialized));
    CHECK(sh_decl_server_test_clear_stray_pending() == 1);
    sh_decl_server_test_set_runtime(0);
    sh_decl_server_test_reset_runtime_state();
}

/* Rebound installed originals (package_compiler.h rebound_original; R-321, r9's copy of
 * textures/effects/hud/magic_glass_broken_2). The engine's default is excused only for a material whose
 * rebinding renamed image names alone, and only while its installed original has not parsed without the
 * default in this process; the strict rule holds for every other case. */
static unsigned char *g_rebound_original_object;
static void *rebound_peek(void *manager, const char *name, unsigned char make_default)
{
    (void)manager; (void)make_default;
    return g_rebound_original_object && name && !strcmp(name, "textures/x") ? g_rebound_original_object : NULL;
}
typedef struct rebound_case {
    const char *type; int rebound; const char *original;
    int stock_state; /* the installed original's state byte; -1 when it is not registered */
    int accept; const char *basis;
} rebound_case;
static const rebound_case g_rebound_cases[] = {
    { "material", 1, "textures/x", -1, 1, "which this process has not parsed" },
    { "material", 1, "textures/x", 0x02, 1, "which this process has not parsed" },  /* registered, not loaded */
    { "material", 1, "textures/x", 0x04, 1, "as that original did in this process" },
    { "material", 1, "textures/x", 0x00, 0, NULL },  /* parsed without the default: the copy's own */
    { "material", 2, "textures/x", -1, 0, NULL },    /* a non-image name was rebound */
    { "entityDef", 1, "textures/x", -1, 0, NULL },   /* another declaration family */
    { "material", 1, NULL, -1, 0, NULL },            /* no installed original name */
    { "material", 0, "textures/x", -1, 0, NULL },    /* no certificate: authored or composed */
};
static void test_rebound_original_materialization(void)
{
    static const unsigned char body[] = "{ tex \"generated/image/smp/aa.bimage\" }";
    unsigned char *decl = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    unsigned char *stock = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    CHECK(decl && stock);
    if (!decl || !stock) return;
    for (size_t i = 0; i < sizeof(g_rebound_cases) / sizeof(g_rebound_cases[0]); i++) {
        const rebound_case *c = &g_rebound_cases[i];
        sh_decl_server_test_materialize_item item = {
            c->type, "smp/rebound", "generated/decls/material/smp/rebound.decl",
            SH_DECL_SERVER_TEST_MISSING, body, sizeof(body) - 1, 0, 0, 0, 0, c->rebound, c->original
        };
        materialize_reset();
        memset(decl, 0, DS_TEST_DECL_BYTES);
        decl[0x2c] = 0x04; *(void **)(decl + 0x18) = (void *)1; /* the engine's default */
        g_materialize_decl = decl;
        memset(stock, 0, DS_TEST_DECL_BYTES);
        g_rebound_original_object = c->stock_state < 0 ? NULL : stock;
        if (c->stock_state >= 0) {
            stock[0x2c] = (unsigned char)c->stock_state;
            if (c->stock_state & 0x04) *(void **)(stock + 0x18) = (void *)1;
        }
        sh_decl_server_test_set_reconstruct(NULL, rebound_peek);
        log_all_reset();
        /* A refused material is present and defaulted, so the pass isolates it and goes on
         * (test_defaulted_material_isolation); a refused entityDef stays terminal. */
        {
            int isolated = !c->accept && !strcmp(c->type, "material");
            run_materialize_case(&item, 1, c->accept || isolated, c->accept);
            CHECK(sh_decl_server_test_isolated_defaults() == isolated);
        }
        if (c->accept) {
            CHECK(strstr(g_log_all, "NATIVE-FALLBACK") && strstr(g_log_all, "rebound copy of installed material textures/x"));
            CHECK(strstr(g_log_all, c->basis) != NULL);
        } else {
            CHECK(!strstr(g_log_all, "NATIVE-FALLBACK"));
            CHECK(strstr(g_log_all, "REFUSED") != NULL);
        }
        if (c->stock_state == 0)
            CHECK(strstr(g_log_all, "its installed original parsed in this process without the engine's default") != NULL);
        if (!c->accept) {
            if (!strstr(g_log_all, "REFUSED")) fprintf(stderr, "rebound case %zu log:\n%s\n", i, g_log_all);
        }
    }
    sh_decl_server_test_set_reconstruct(NULL, NULL);
    g_rebound_original_object = NULL;
    HeapFree(GetProcessHeap(), 0, decl);
    HeapFree(GetProcessHeap(), 0, stock);
}
/* A material whose text names a render program neither the library nor the installed inventory
 * declares parses to the engine's default natively, whatever its certificate (the 2026-10-22 boot refused
 * material/smp/5dea5f3e..., the image-rebound copy of textures/effects/hud/magic_glass_broken_2, under the
 * strict rule). The predicted default is excused and logged with the program; other families and an
 * unfinished load keep the strict rule. */
static void test_native_default_materialization(void)
{
    static const unsigned char body[] = "{ stageProgram hud_magic_glass_off tex \"generated/image/smp/aa.bimage\" }";
    unsigned char *decl = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    CHECK(decl != NULL);
    if (!decl) return;
    sh_decl_server_test_set_reconstruct(NULL, NULL);
    for (int variant = 0; variant < 3; variant++) {
        sh_decl_server_test_materialize_item item = {
            variant == 1 ? "entityDef" : "material", "smp/glass", "generated/decls/material/smp/glass.decl",
            SH_DECL_SERVER_TEST_MISSING, body, sizeof(body) - 1, 0, 0, 0, 0, 0, NULL, "hud_magic_glass_off"
        };
        int accept = variant == 0;
        materialize_reset();
        memset(decl, 0, DS_TEST_DECL_BYTES);
        decl[0x2c] = variant == 2 ? 0x05 : 0x04; *(void **)(decl + 0x18) = (void *)1;
        g_materialize_decl = decl;
        log_all_reset();
        run_materialize_case(&item, 1, accept, accept);
        if (getenv("DS_TEST_VERBOSE")) fprintf(stderr, "native default variant %d:\n%s", variant, g_log_all);
        if (accept) {
            CHECK(strstr(g_log_all, "NATIVE-FALLBACK") && strstr(g_log_all, "renderProg 'hud_magic_glass_off'"));
            CHECK(!strstr(g_log_all, "REFUSED"));
        } else {
            CHECK(!strstr(g_log_all, "NATIVE-FALLBACK") && strstr(g_log_all, "REFUSED") != NULL);
        }
        CHECK(sh_decl_server_test_isolated_defaults() == 0);
    }
    HeapFree(GetProcessHeap(), 0, decl);
}

/* One present, defaulted material the pass cannot excuse is refused alone: the identities after it still
 * materialize, the pass completes, and the refusal is accounted (REFUSED with its certificate and original,
 * the isolated count). A null object or a defaulted declaration of another family stays terminal. */
static void test_defaulted_material_isolation(void)
{
    static const unsigned char body[] = "{ tex \"generated/image/smp/aa.bimage\" }";
    unsigned char *defaulted = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    unsigned char *clean = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, DS_TEST_DECL_BYTES);
    CHECK(defaulted && clean);
    if (!defaulted || !clean) return;
    defaulted[0x2c] = 0x04; *(void **)(defaulted + 0x18) = (void *)1;
    sh_decl_server_test_set_reconstruct(NULL, NULL);
    {
        sh_decl_server_test_materialize_item items[] = {
            { "material", "smp/broken", "generated/decls/material/smp/broken.decl",
              SH_DECL_SERVER_TEST_MISSING, body, sizeof(body) - 1, 0, 0, 0, 0, 0, NULL, NULL },
            { "material", "smp/fine", "generated/decls/material/smp/fine.decl",
              SH_DECL_SERVER_TEST_MISSING, body, sizeof(body) - 1, 0, 0, 0, 0, 0, NULL, NULL }
        };
        materialize_reset();
        g_materialize_decl = clean;
        g_materialize_null_entity_name = "smp/broken"; g_materialize_decl_null_entity = defaulted;
        log_all_reset();
        run_materialize_case(items, 2, 1, 1);
        if (getenv("DS_TEST_VERBOSE")) fprintf(stderr, "isolation case:\n%s", g_log_all);
        CHECK(sh_decl_server_test_isolated_defaults() == 1);
        CHECK(strstr(g_log_all, "REFUSED: 'generated/decls/material/smp/broken.decl -> material/smp/broken'") != NULL);
        CHECK(strstr(g_log_all, "did not complete source parsing (certificate 0, original '-'); isolated:") != NULL);
        CHECK(strstr(g_log_all, "MATERIALIZED: 'generated/decls/material/smp/fine.decl'") != NULL);
        CHECK(!strstr(g_log_all, "NATIVE-FALLBACK"));
    }
    {
        /* The certificate and original of a refused copy are named, so a live refusal can be read. */
        sh_decl_server_test_materialize_item item = {
            "material", "smp/copy", "generated/decls/material/smp/copy.decl",
            SH_DECL_SERVER_TEST_MISSING, body, sizeof(body) - 1, 0, 0, 0, 0, 2, "textures/x", NULL
        };
        materialize_reset();
        g_materialize_decl = defaulted;
        log_all_reset();
        run_materialize_case(&item, 1, 1, 0);
        if (getenv("DS_TEST_VERBOSE")) fprintf(stderr, "isolation case:\n%s", g_log_all);
        CHECK(strstr(g_log_all, "(certificate 2, original 'textures/x'); isolated:") != NULL);
        CHECK(sh_decl_server_test_isolated_defaults() == 1);
    }
    {
        /* A null object is the cut-content shape and stays terminal: nothing after it materializes. */
        sh_decl_server_test_materialize_item items[] = {
            { "material", "smp/broken", "generated/decls/material/smp/broken.decl",
              SH_DECL_SERVER_TEST_MISSING, body, sizeof(body) - 1, 0, 0, 0, 0, 0, NULL, NULL },
            { "material", "smp/fine", "generated/decls/material/smp/fine.decl",
              SH_DECL_SERVER_TEST_MISSING, body, sizeof(body) - 1, 0, 0, 0, 0, 0, NULL, NULL }
        };
        materialize_reset();
        g_materialize_decl = clean;
        g_materialize_null_entity_name = "smp/broken"; g_materialize_decl_null_entity = NULL;
        log_all_reset();
        run_materialize_case(items, 2, 0, 0);
        if (getenv("DS_TEST_VERBOSE")) fprintf(stderr, "isolation case:\n%s", g_log_all);
        CHECK(sh_decl_server_test_isolated_defaults() == 0);
        CHECK(strstr(g_log_all, "null decl") != NULL && !strstr(g_log_all, "isolated:"));
        CHECK(!strstr(g_log_all, "MATERIALIZED: 'generated/decls/material/smp/fine.decl'"));
    }
    {
        /* A defaulted declaration of another family is still terminal. */
        sh_decl_server_test_materialize_item item = {
            "entityDef", "smp/entity", "generated/decls/entitydef/smp/entity.decl",
            SH_DECL_SERVER_TEST_MISSING, body, sizeof(body) - 1, 0, 0, 0, 0, 0, NULL, NULL
        };
        materialize_reset();
        g_materialize_decl = defaulted;
        log_all_reset();
        run_materialize_case(&item, 1, 0, 0);
        if (getenv("DS_TEST_VERBOSE")) fprintf(stderr, "isolation case:\n%s", g_log_all);
        CHECK(sh_decl_server_test_isolated_defaults() == 0 && !strstr(g_log_all, "isolated:"));
    }
    HeapFree(GetProcessHeap(), 0, defaulted);
    HeapFree(GetProcessHeap(), 0, clean);
}
/* The runtime shadowed drain (activation and the recovery that restores a provider) applies the same rule:
 * a certified material's default is reloaded as accepted, and the contradicted, non-image and unfinished cases
 * are drain failures. */
static void test_runtime_rebound_original(void)
{
    for (int variant = 0; variant < 4; variant++) {
        unsigned char decl[0x300] = {0}, stock[0x300] = {0};
        sh_decl_server_test_materialize_item item = {
            "material", "rt/rebound", "generated/decls/material/rt/rebound.decl",
            SH_DECL_SERVER_TEST_SHADOWED, NULL, 0, SH_DECL_SERVER_TEST_SHADOW_LIVE, 0, 0, 0,
            variant == 2 ? 2 : 1, "rt/original"
        };
        int materialized = -1, accept = variant == 0;
        long faults = 0, shadow = 0;
        rt_reset();
        g_rt_names[0] = "rt/rebound"; g_rt_objects[0] = decl; g_rt_no_drain = 1;
        if (variant == 1) { g_rt_names[1] = "rt/original"; g_rt_objects[1] = stock; stock[0x2c] = 0; }
        sh_decl_server_test_reset_runtime_state();
        sh_decl_server_test_set_generic_load(variant == 3 ? NULL : rt_fallback_load);
        sh_decl_server_test_set_runtime(1);
        log_all_reset();
        CHECK(sh_decl_server_test_materialize_missing_sedefs(&item, 1,
            (void *)1, rt_type_by_name, rt_source_find, rt_find_decl, &materialized) == accept);
        sh_decl_server_test_runtime_counters(NULL, NULL, &shadow, &faults);
        if (accept) {
            CHECK(shadow == 1 && faults == 0);
            CHECK(strstr(g_log_all, "rebound copy of installed material rt/original") != NULL);
            CHECK(strstr(g_log_all, "RELOADED") != NULL && !strstr(g_log_all, "DRAIN-FAILED"));
            CHECK((decl[0x2c] & 2) == 0 && (decl[0x2c] & 4) != 0);
            CHECK(sh_decl_server_test_clear_stray_pending() == 0);
        } else {
            CHECK(!strstr(g_log_all, "NATIVE-FALLBACK"));
            if (variant != 3) CHECK(faults == 1 && strstr(g_log_all, "DRAIN-FAILED") != NULL);
            /* An unfinished load is never excused; its mark is swept. */
            if (variant == 3) CHECK(sh_decl_server_test_clear_stray_pending() == 1);
        }
        sh_decl_server_test_set_runtime(0);
        sh_decl_server_test_reset_runtime_state();
    }
}

static void test_runtime_stray_pending_cleared(void)
{
    static const unsigned char body[] = "{ }";
    sh_decl_server_test_materialize_item items[] = {
        { "material", "rt/stray", "generated/decls/material/rt/stray.decl",
          SH_DECL_SERVER_TEST_MISSING, body, sizeof(body) - 1, 0 }
    };
    unsigned char *stray;
    int materialized = -1;

    stray = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    CHECK(stray != NULL);
    if (!stray) return;
    stray[0x2c] = 0x04;

    rt_reset();
    g_rt_names[0] = "rt/stray";
    g_rt_objects[0] = stray;
    /* If materialization fails, disarm the unshadowed object's remaining mark. */
    g_rt_null_on_load = 1;
    sh_decl_server_test_reset_runtime_state();
    sh_decl_server_test_set_runtime(1);
    CHECK(sh_decl_server_test_materialize_missing_sedefs(
              items, 1, (void *)(uintptr_t)0x98760000u, rt_type_by_name,
              rt_source_find, rt_find_decl, &materialized) == 0);
    sh_decl_server_test_set_runtime(0);

    CHECK((stray[0x2c] & 0x02) != 0);
    CHECK(sh_decl_server_test_clear_stray_pending() == 1);
    CHECK((stray[0x2c] & 0x02) == 0);
    HeapFree(GetProcessHeap(), 0, stray);
}

static void test_runtime_off_leaves_placeholders_alone(void)
{
    static const unsigned char body[] = "{ }";
    sh_decl_server_test_materialize_item items[] = {
        { "material", "rt/alpha", "generated/decls/material/rt/alpha.decl",
          SH_DECL_SERVER_TEST_MISSING, body, sizeof(body) - 1, 0 }
    };
    unsigned char *a;
    int materialized = -1;
    long marked = -1, left = -1, shadow = -1, faults = -1;

    a = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    CHECK(a != NULL);
    if (!a) return;

    rt_reset();
    g_rt_names[0] = "rt/alpha";
    g_rt_objects[0] = a;
    sh_decl_server_test_reset_runtime_state();
    CHECK(sh_decl_server_test_materialize_missing_sedefs(
              items, 1, (void *)(uintptr_t)0x98760000u, rt_type_by_name,
              rt_source_find, rt_find_decl, &materialized) == 1);

    /* A boot pass neither marks nor re-parses: the placeholder is reused as-is. */
    CHECK(g_rt_loads[0] == 0);
    CHECK((a[0x2c] & 0x02) == 0);
    sh_decl_server_test_runtime_counters(&marked, &left, &shadow, &faults);
    CHECK(marked == 0);
    CHECK(shadow == 0);
    HeapFree(GetProcessHeap(), 0, a);
}

static int g_registered_commands;
static void lifecycle_add_command(void *cmdsys, const char *name, void *callback,
                                  const char *help, void *completion, unsigned int flags)
{
    (void)cmdsys; (void)name; (void)callback; (void)help; (void)completion; (void)flags;
    g_registered_commands++;
}

static void lifecycle_noop(void) {}
static void lifecycle_promote(void) { g_hook_original_calls++; }

static void test_boot_hook_recovery(const sig_result *results, size_t count,
                                    const unsigned char *base, void *cmdsys)
{
    int before, requirements, prepares;
    unsigned char resource_node[0x30] = {0}, resource[0x30] = {0};
    void *resources[] = {resource};
    sh_decl_server_test_reset_install();
    *(void **)(resource_node + 0x20) = resources;
    *(int *)(resource_node + 0x28) = 1;
    g_boundary_resource_head = resource_node;
    g_linked_fixture = 1;
    runtime_fixture();
    g_hook_prepares = g_hook_commits = g_hook_unpatches = 0;
    g_hook_original_calls = g_requirements_calls = 0;

    /* No prepared callback is owned, so a later install can start afresh. */
    g_hook_prepare_fail = 1;
    CHECK(sh_decl_server_install(results, count, base, cmdsys) == 0);
    CHECK(g_hook_prepares == 1 && g_hook_commits == 0 && g_hook_owned == NULL);
    CHECK(sh_decl_server_registration_succeeded() == 0);
    CHECK(sh_decl_server_rearm() == 0);

    /* Completed rollback also permits retry, without a test-state reset. */
    g_hook_prepare_fail = 0;
    g_hook_commit_result = B2_PATCH_FAIL_SEH;
    g_hook_unpatch_fail = 0;
    g_hook_invoke_on_commit = 1;
    CHECK(sh_decl_server_install(results, count, base, cmdsys) == 0);
    CHECK(g_hook_prepares == 2 && g_hook_commits == 1 && g_hook_unpatches == 1);
    CHECK(g_hook_owned == NULL && sh_decl_server_registration_succeeded() == 0);

    g_hook_commit_result = B2_PATCH_FAIL_ROLLBACK;
    g_hook_unpatch_fail = 1;
    g_hook_invoke_on_commit = 0;
    CHECK(sh_decl_server_install(results, count, base, cmdsys) == 0);
    CHECK(g_hook_prepares == 3 && g_hook_commits == 2 && g_hook_unpatches == 2);
    CHECK(g_hook_owned != NULL && !hook_is_installed(g_hook_owned));
    CHECK(sh_decl_server_registration_succeeded() == 0);
    before = g_hook_original_calls;
    requirements = g_requirements_calls;
    g_hook_detour();
    CHECK(g_hook_original_calls == before + 1);
    CHECK(g_requirements_calls == requirements);
    before = g_string_refreshes;
    CHECK(sh_decl_server_rearm() == 0);
    sh_decl_server_request_rearm();
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == before && sh_decl_server_registration_succeeded() == 0);
    prepares = g_hook_prepares;
    CHECK(sh_decl_server_install(results, count, base, cmdsys) == 0);
    CHECK(g_hook_prepares == prepares && g_hook_owned != NULL);

    /* Recovery releases the old record before preparing another. Commit calls
     * the detour immediately, proving its original callback is already visible. */
    g_hook_unpatch_fail = 0;
    g_hook_commit_result = B2_PATCH_OK;
    g_hook_invoke_on_commit = 1;
    before = g_hook_original_calls;
    CHECK(sh_decl_server_install(results, count, base, cmdsys) == 1);
    CHECK(g_hook_prepares == prepares + 1 && hook_is_installed(g_hook_owned));
    CHECK(g_hook_original_calls == before + 1);
    CHECK(g_requirements_calls == requirements + 1);
    before = g_string_refreshes;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == before + 1);
    if (!sh_decl_server_registration_succeeded())
        fprintf(stderr, "hook recovery refresh: %s\n", g_last_log);
    CHECK(sh_decl_server_registration_succeeded() == 1);
    prepares = g_hook_prepares;
    CHECK(sh_decl_server_install(results, count, base, cmdsys) == 1);
    CHECK(g_hook_prepares == prepares);
    /* An attempted removal also invalidates readiness while restoration waits. */
    g_hook_installed = 0;
    g_hook_unpatch_fail = 1;
    CHECK(sh_decl_server_install(results, count, base, cmdsys) == 0);
    CHECK(sh_decl_server_registration_succeeded() == 0 && sh_decl_server_rearm() == 0);
    CHECK(g_hook_prepares == prepares && g_hook_owned != NULL);
    g_hook_unpatch_fail = 0;
    CHECK(sh_decl_server_install(results, count, base, cmdsys) == 1);
    CHECK(g_hook_prepares == prepares + 1 && hook_is_installed(g_hook_owned));
    CHECK(sh_decl_server_registration_succeeded() == 1);
    sh_decl_server_test_reset_install();
    CHECK(g_hook_owned == NULL);
    g_boundary_resource_head = NULL;
    g_linked_fixture = 0;
    g_hook_prepare_fail = 1;
    g_hook_invoke_on_commit = 0;
}

/* R-400: a boot whose first registration waits for the menu. The promotion runs only the shader preparation,
 * in the boot scope where native shader preparation is admitted; nothing registers there; the first settled
 * browser tick registers the library. A preparation that cannot complete keeps the default placement: the
 * registration runs inside the promotion. */
static int g_boot_prep_calls, g_boot_prep_admitted, g_boot_prep_result;
static int boot_prepare_fixture(char *why, size_t capacity)
{
    g_boot_prep_calls++;
    g_boot_prep_admitted = sh_decl_server_shader_preparation_ready();
    if (!g_boot_prep_result) snprintf(why, capacity, "no shader preparation hint exists yet for these installed packages");
    return g_boot_prep_result;
}
static void test_deferred_first_registration(const sig_result *results, size_t count,
                                             const unsigned char *base, void *cmdsys)
{
    unsigned char resource_node[0x30] = {0}, resource[0x30] = {0};
    void *resources[] = {resource};
    for (int prepared = 1; prepared >= 0; prepared--) {
        sh_decl_server_test_reset_install();
        *(void **)(resource_node + 0x20) = resources;
        *(int *)(resource_node + 0x28) = 1;
        g_boundary_resource_head = resource_node;
        g_linked_fixture = 1;
        runtime_fixture();
        g_hook_prepare_fail = 0; g_hook_unpatch_fail = 0;
        g_hook_commit_result = B2_PATCH_OK;
        g_hook_invoke_on_commit = 0; /* the engine reaches the promotion later in idCommonLocal::Init */
        g_boot_prep_calls = g_boot_prep_admitted = 0; g_boot_prep_result = prepared;
        sh_boot_phases_set_placement(SH_BOOT_PLACE_FIRST_BOUNDARY);
        sh_decl_server_set_boot_preparation(boot_prepare_fixture);
        int requirements = g_requirements_calls, originals = g_hook_original_calls, strings = g_string_refreshes;
        CHECK(sh_decl_server_install(results, count, base, cmdsys) == 1);
        CHECK(g_hook_detour != NULL && g_requirements_calls == requirements);
        if (g_hook_detour) g_hook_detour();
        CHECK(g_hook_original_calls == originals + 1); /* the engine's own promotion always runs */
        if (g_boot_prep_calls != 1 || g_boot_prep_admitted != 1) fprintf(stderr, "boot preparation: %d call(s), admitted %d\n", g_boot_prep_calls, g_boot_prep_admitted);
        CHECK(g_boot_prep_calls == 1 && g_boot_prep_admitted == 1);
        if (prepared) {
            /* Menu first: nothing registered in the promotion, no half view, one pass at the browser. */
            CHECK(g_requirements_calls == requirements && g_string_refreshes == strings);
            CHECK(sh_decl_server_registration_succeeded() == 0);
            CHECK(sh_boot_phases_placement() == SH_BOOT_PLACE_FIRST_BOUNDARY);
            sh_decl_server_rearm_poll();
            if (!sh_decl_server_registration_succeeded()) fprintf(stderr, "deferred first registration: %s\n", g_last_log);
            CHECK(sh_decl_server_registration_succeeded() == 1);
            CHECK(g_requirements_calls == requirements + 1 && g_string_refreshes == strings + 1);
        } else {
            /* No hint: the registration runs inside the promotion exactly as a default boot. */
            CHECK(sh_boot_phases_placement() == SH_BOOT_PLACE_PROMOTION);
            CHECK(g_requirements_calls == requirements + 1);
            CHECK(sh_decl_server_registration_succeeded() == 1);
            sh_decl_server_rearm_poll();
            CHECK(g_string_refreshes == strings + 1); /* no second pass is waiting */
        }
    }
    sh_decl_server_set_boot_preparation(NULL);
    sh_boot_phases_set_placement(SH_BOOT_PLACE_PROMOTION);
    sh_decl_server_test_reset_install();
    g_boundary_resource_head = NULL;
    g_linked_fixture = 0;
    g_hook_prepare_fail = 1;
    g_hook_invoke_on_commit = 0;
}

static int builtin_loads(void)
{
    int loads = 0;
    for (size_t b = 0; b < g_rt_builtin_count; b++) loads += g_rt_builtins[b].loads;
    return loads;
}
static void test_empty_launch_and_runtime_results(void)
{
    static struct { unsigned char anchor[32]; void *registry; } image_anchor;
    static void *vtable[12];
    static void *registry;
    const unsigned char *base = (const unsigned char *)GetModuleHandleA(NULL);
    struct { const char *name; void *address; } bindings[] = {
        {"DeclRegistryAnchor", image_anchor.anchor},
        {"DeclTypeByName", rt_type_by_name}, {"DeclRegisterFile", pipeline_register_file},
        {"DeclFind", rt_find_decl}, {"DeclSourceFind", rt_source_find},
        {"IdStrCtor", pipeline_ctor}, {"IdStrDtor", pipeline_dtor},
        {"ResourceStaticPromote", lifecycle_promote}, {"CmdExecuteBuffer", lifecycle_noop},
        {"ResourceGenericLoad", rt_generic_load}, {"ResourceReconstruct", rt_reconstruct},
        {"ResourceListAdd", lifecycle_noop},
        {"AddCommand", lifecycle_add_command},
        {"MemLocalGet", heap_get}, {"MemLocalPushHeap", heap_push},
        {"MemLocalPopHeap", heap_pop}
    };
    sig_result results[sizeof(bindings)/sizeof(bindings[0])];
    char temp[MAX_PATH];
    int displacement = (int)((unsigned char *)&image_anchor.registry -
                              (image_anchor.anchor + 0x17));
    static const unsigned char body[] = "{}";
    sh_decl_server_test_materialize_item item = {
        "material", "rt/alpha", "generated/decls/material/rt/alpha.decl",
        SH_DECL_SERVER_TEST_SHADOWED, body, 2, SH_DECL_SERVER_TEST_SHADOW_LIVE
    };
    unsigned char *decl = (unsigned char *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x300);
    CHECK(decl != NULL);
    if (!decl) return;
    GetTempPathA(sizeof(temp), temp);
    _snprintf_s(g_fixture_root, sizeof(g_fixture_root), _TRUNCATE,
                "%ssnapmap-plus-empty-launch-%lu", temp, GetCurrentProcessId());
    CHECK(CreateDirectoryA(g_fixture_root, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    sh_package_runtime_test_empty_catalog();
    CHECK(sh_package_runtime_refresh(g_fixture_root));
    rt_builtin_fixture();
    vtable[7] = pipeline_register_file;
    vtable[11] = rt_type_by_name;
    registry = vtable;
    image_anchor.registry = &registry;
    memcpy(image_anchor.anchor + 0x10, "\x48\x8b\x0d", 3);
    memcpy(image_anchor.anchor + 0x13, &displacement, 4);
    for (size_t i = 0; i < sizeof(bindings)/sizeof(bindings[0]); i++) {
        results[i].name = bindings[i].name;
        results[i].status = SIG_OK;
        results[i].addr = (uintptr_t)bindings[i].address;
        results[i].rva = (uint32_t)(results[i].addr - (uintptr_t)base);
    }
    sh_decl_server_test_reset_install();
    heap_fixture_reset();
    g_user_enabled = 1;
    g_registered_commands = 0;
    g_last_failure[0] = 0;
    /* An empty authored inventory still compiles and publishes the product's
     * built-ins. Exercise the real boot path with already-loaded originals. */
    g_hook_prepare_fail = 0;
    g_hook_commit_result = B2_PATCH_OK;
    g_hook_invoke_on_commit = 0;
    g_scan_calls = 0;
    CHECK(sh_decl_server_install(results, sizeof(results)/sizeof(results[0]), base, &registry) == 1);
    CHECK(g_scan_calls == 0); /* Arming does not race native initialization. */
    g_hook_detour();
    CHECK(g_scan_calls == 1);
    CHECK(g_registered_commands == 2);
    if (!sh_decl_server_registration_succeeded()) fprintf(stderr, "built-in boot fixture: %s\n", g_last_failure);
    CHECK(sh_decl_server_registration_succeeded() == 1);
    {
        const sh_package_compilation *before = sh_package_runtime_acquire(), *after;
        int promotions = g_hook_original_calls, strings = g_string_refreshes;
        sh_package_runtime_release();
        sh_decl_server_test_reset_install();
        g_scan_fail = 1;
        CHECK(sh_decl_server_install(results, sizeof(results)/sizeof(results[0]), base, &registry) == 1);
        CHECK(g_scan_calls == 1);
        g_hook_detour();
        CHECK(g_scan_calls == 2 && g_hook_original_calls == promotions + 1);
        CHECK(!sh_decl_server_registration_succeeded() && g_string_refreshes == strings);
        after = sh_package_runtime_acquire(); CHECK(after == before); sh_package_runtime_release();
        /* Leave the failed boot pass bound. The normal browser rearm below
         * must recover in this same process without reinstalling the hook. */
        g_scan_fail = 0;
    }
    {
        const sh_package_compilation *before = sh_package_runtime_acquire(), *after;
        int scans = g_scan_calls, strings = g_string_refreshes, promotions = g_hook_original_calls;
        sh_package_runtime_release();
        sh_decl_server_test_reset_install();
        g_fail_next_string_refresh = 1;
        CHECK(sh_decl_server_install(results, sizeof(results)/sizeof(results[0]), base, &registry) == 1);
        g_hook_detour();
        after = sh_package_runtime_acquire(); CHECK(after == before); sh_package_runtime_release();
        CHECK(g_scan_calls == scans + 1 && g_string_refreshes == strings + 2);
        CHECK(g_hook_original_calls == promotions + 1);
        CHECK(!sh_package_runtime_ready() && !sh_decl_server_registration_succeeded());
    }
    {
        unsigned char node[0x30] = {0}, original[0x30] = {0}, fresh[0x30] = {0};
        void *entries[2] = {original, fresh};
        *(unsigned int *)(original + 0x28) = 2;
        *(unsigned int *)(fresh + 0x28) = 2;
        *(void **)(node + 0x20) = entries;
        *(int *)(node + 0x28) = 1;
        g_boundary_resource_head = node;
        CHECK(sh_decl_server_test_lifetime_watermark() == 1);
        *(int *)(node + 0x28) = 2;
        CHECK(sh_decl_server_test_promote_delta() == 1);
        CHECK(*(unsigned int *)(original + 0x28) == 2);
        CHECK(*(unsigned int *)(fresh + 0x28) == 4);
        *(void **)(node + 0x18) = (void *)1;
        CHECK(sh_decl_server_test_lifetime_watermark() == 0);
        *(void **)(node + 0x18) = node;
        CHECK(sh_decl_server_test_lifetime_watermark() == 0);
        *(void **)(node + 0x18) = NULL;
        *(int *)(node + 0x28) = 1;
        CHECK(sh_decl_server_test_lifetime_watermark() == 1);
        entries[1] = (void *)1;
        *(int *)(node + 0x28) = 2;
        CHECK(sh_decl_server_test_promote_delta() == 0);
        g_boundary_resource_head = g_rt_builtin_node;
    }
    *(void **)(g_boundary_shell + 8) = g_boundary_dialog_manager;
    *(void **)(g_boundary_dialog_manager + 0x900) = g_boundary_dialog_queue;
    *(int *)(g_boundary_dialog_manager + 0x908) = 0;
    *(void **)(g_boundary_shell + 0x18) = g_boundary_manager;
    *(int *)(g_boundary_manager + 8) = 0x3f;
    *(int *)(g_boundary_manager + 12) = 0x3f;
    *(int *)(g_boundary_manager + 0x910) = 1;
    *(int *)(g_boundary_manager + 0x914) = 1;
    /* DISMISS-A can leave the visible byte set after every descriptor is gone. */
    g_boundary_manager[0xa8] = 1;
    g_boundary_thread = GetCurrentThreadId();
    g_string_refreshes = 0;
    CHECK(sh_decl_server_rearm() == 1);
    CHECK(g_string_refreshes == 1);
    for (size_t i = 0; i < g_rt_builtin_count; i++) CHECK(g_rt_builtins[i].loads > 0);
    CHECK(g_boundary_manager[0xa8] == 1);
    {
        const int screens[] = {59, 63, 64, 65};
        for (size_t i = 0; i < sizeof(screens) / sizeof(screens[0]); i++) {
            *(int *)(g_boundary_manager + 8) = *(int *)(g_boundary_manager + 12) = screens[i];
            for (int state = -1; state <= 10; state++) {
                *(int *)(g_boundary_manager + 0x910) = *(int *)(g_boundary_manager + 0x914) = state;
                CHECK(sh_decl_server_map_boundary_safe() == (state >= 1 && state <= 4));
                CHECK(sh_decl_server_map_retirement_safe() == (screens[i] == 63 && state >= 1 && state <= 4));
            }
        }
        *(int *)(g_boundary_manager + 8) = *(int *)(g_boundary_manager + 12) = 63;
        *(int *)(g_boundary_manager + 0x910) = *(int *)(g_boundary_manager + 0x914) = 1;
        CHECK(sh_decl_server_map_browser_present());
        CHECK(sh_decl_server_map_preparation_ready());
        g_boundary_common[0xf576] = 1;
        CHECK(sh_decl_server_map_preparation_ready() && !sh_decl_server_map_boundary_safe());
        g_boundary_common[0xf576] = 0;
        g_boundary_editor[9] = 1;
        CHECK(sh_decl_server_map_preparation_ready() && !sh_decl_server_map_boundary_safe());
        g_boundary_editor[9] = 0;
        g_boundary_thread = GetCurrentThreadId() + 1;
        CHECK(!sh_decl_server_map_preparation_ready());
        g_boundary_thread = GetCurrentThreadId();
        *(int *)(g_boundary_dialog_manager + 0x908) = 1; g_boundary_dialog_queue[8] = 0;
        CHECK(sh_decl_server_map_browser_present() && !sh_decl_server_map_retirement_safe());
        *(int *)(g_boundary_dialog_manager + 0x908) = 0;
        g_boundary_common[0xf576] = 1; CHECK(!sh_decl_server_map_browser_present()); g_boundary_common[0xf576] = 0;
    }
    sh_decl_server_request_rearm();
    CHECK(sh_decl_server_registration_succeeded() == 0);
    /* Every unsafe state preserves the pending request without touching packages. */
    g_boundary_editor[9] = 1;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1 && sh_decl_server_registration_succeeded() == 0);
    g_boundary_editor[9] = 0;
    g_boundary_common[0xf576] = 1;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1);
    g_boundary_common[0xf576] = 2;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1);
    g_boundary_common[0xf576] = 0;
    *(int *)(g_boundary_manager + 0x910) = 10;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1);
    *(int *)(g_boundary_manager + 0x910) = 1;
    *(int *)(g_boundary_manager + 12) = 0x2e;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1);
    *(int *)(g_boundary_manager + 12) = 0x3f;
    /* Queue state must block even when the visible byte says no dialog. */
    g_boundary_manager[0xa8] = 0;
    *(int *)(g_boundary_dialog_manager + 0x908) = 2;
    g_boundary_dialog_queue[8] = 1;
    g_boundary_dialog_queue[0x1b0 + 8] = 0;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1);
    g_boundary_dialog_queue[0x1b0 + 8] = 2;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1);
    *(int *)(g_boundary_dialog_manager + 0x908) = -1;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1);
    *(int *)(g_boundary_dialog_manager + 0x908) = 65;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1);
    *(int *)(g_boundary_dialog_manager + 0x908) = 0;
    *(void **)(g_boundary_dialog_manager + 0x900) = NULL;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1);
    *(void **)(g_boundary_dialog_manager + 0x900) = (void *)1;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1);
    *(void **)(g_boundary_dialog_manager + 0x900) = g_boundary_dialog_queue;
    *(void **)(g_boundary_shell + 8) = NULL;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1);
    *(void **)(g_boundary_shell + 8) = (void *)1;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1);
    *(void **)(g_boundary_shell + 8) = g_boundary_dialog_manager;
    g_boundary_dialog_queue[0x1b0 + 8] = 1;
    *(int *)(g_boundary_dialog_manager + 0x908) = 2;
    g_boundary_manager[0xa8] = 1;
    g_boundary_thread = 0;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1);
    g_boundary_thread = GetCurrentThreadId();
    g_boundary_common_slot = NULL;
    sh_decl_server_rearm_poll();
    CHECK(g_string_refreshes == 1 && sh_decl_server_rearm() == 0);
    g_boundary_common_slot = g_boundary_common;
    sh_decl_server_rearm_poll();
    CHECK(sh_decl_server_registration_succeeded() == 1 && g_string_refreshes == 2);
    /* An installed-package request waits for the browser, as after a consent
     * install committed in the editor. A map-boundary activation completing
     * meanwhile finds the served view unchanged and keeps the registration
     * (R-381); the deferred pass still runs after. The watcher's pass over an
     * unchanged inventory keeps the registration too; a forced pass compiles,
     * registers and, changing nothing, logs its SHADOWED identities as a count. */
    {
        const LONG kinds[] = {0, 1};
        for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
            int refreshes = g_string_refreshes;
            long skipped = sh_package_runtime_activations_skipped();
            if (kinds[k]) sh_decl_server_request_rearm(); else sh_decl_server_test_request_watched();
            CHECK(!sh_decl_server_registration_succeeded());
            CHECK(sh_decl_server_activate_occurrences(NULL) == 1);
            CHECK(g_string_refreshes == refreshes && sh_package_runtime_activations_skipped() == skipped + 1 &&
                !sh_decl_server_registration_succeeded());
            log_all_reset();
            sh_decl_server_rearm_poll();
            CHECK(sh_decl_server_registration_succeeded() == 1);
            if (kinds[k]) {
                CHECK(g_string_refreshes == refreshes + 1 && !strstr(g_log_all, "served library is unchanged"));
                CHECK(strstr(g_log_all, "decl-server SHADOWED: ") != NULL && !strstr(g_log_all, "decl-server SHADOWED: '"));
                CHECK(strstr(g_log_all, "as at the last pass") != NULL);
            } else {
                CHECK(g_string_refreshes == refreshes && sh_package_runtime_activations_skipped() == skipped + 2);
                CHECK(strstr(g_log_all, "decl-server RE-ARM: the served library is unchanged") != NULL);
            }
        }
        /* A pending request never masks a failed activation. */
        sh_package_runtime_test_force_activation(1);
        g_runtime_dependencies_ok = 0;
        sh_decl_server_test_request_watched();
        CHECK(sh_decl_server_activate_occurrences(NULL) == 0);
        g_runtime_dependencies_ok = 1;
        sh_package_runtime_test_force_activation(0);
        sh_decl_server_rearm_poll();
        CHECK(sh_decl_server_registration_succeeded() == 1);
    }
    {
        /* A map provider activation that keeps failing (the departure poll asks
         * again) marks the first failure of the run and the run's end, and the
         * runtime counts its repeated diagnosis instead of logging it again. */
        int failures = 0, repeats = 0, recovered = 0;
        sh_package_runtime_test_force_activation(1);
        g_runtime_dependencies_ok = 0;
        log_all_reset();
        for (int attempt = 0; attempt < 3; attempt++) CHECK(sh_decl_server_activate_map(NULL) == 0);
        for (const char *at = g_log_all; (at = strstr(at, "map provider activation failed")); at++) failures++;
        CHECK(failures == 1);
        g_runtime_dependencies_ok = 1;
        sh_package_runtime_test_force_activation(0);
        log_all_reset();
        CHECK(sh_decl_server_activate_map(NULL) == 1);
        for (const char *at = g_log_all; (at = strstr(at, "repeated 2 more time(s)")); at++) repeats++;
        for (const char *at = g_log_all; (at = strstr(at, "succeeded after 3 failed attempt(s)")); at++) recovered++;
        CHECK(repeats == 1 && recovered == 1 && sh_decl_server_registration_succeeded() == 1);
    }
    {
        /* The idle retirement poll backs off after a failed retained activation (the 2026-10-22 boot
         * asked, and logged, once a second for the whole session): the wait doubles up to the cap, the
         * cap is announced once, and the schedule clears with the pending resources. */
        long attempts; int polls = 0, capped = 0;
        sh_decl_server_test_retirement_timing(20, 80);
        sh_package_runtime_test_force_activation(1);
        g_runtime_dependencies_ok = 0;
        sh_package_runtime_test_occurrences_pending(1);
        CHECK(sh_package_runtime_occurrences_pending() && sh_decl_server_map_retirement_safe());
        for (ULONGLONG start = GetTickCount64(); GetTickCount64() - start < 400;) {
            log_all_reset();
            sh_decl_server_rearm_poll();
            if (strstr(g_log_all, "retrying every 80 ms")) capped++;
            Sleep(5); polls++;
        }
        attempts = sh_decl_server_test_retirement_attempts();
        if (attempts < 3 || attempts > 8) fprintf(stderr, "retirement attempts %ld over %d polls\n", attempts, polls);
        CHECK(attempts >= 3 && attempts <= 8 && polls > attempts * 4);
        CHECK(capped == 1);
        sh_package_runtime_test_occurrences_pending(0);
        sh_decl_server_rearm_poll();
        CHECK(sh_decl_server_test_retirement_attempts() == attempts);
        g_runtime_dependencies_ok = 1;
        sh_package_runtime_test_force_activation(0);
        sh_decl_server_test_retirement_timing(1000, 60000);
        CHECK(sh_decl_server_rearm() == 1 && sh_decl_server_registration_succeeded());
    }
    {
        /* R-381: leases pending over an unchanged view (the 2026-10-22 boot: the
         * registration pass's own holds left them pending after every pass, and
         * the poll re-registered the library every 16 s for the session). The
         * boundary finds nothing to activate, keeps the registration, publishes
         * the leases and asks once. */
        long attempts, skipped = sh_package_runtime_activations_skipped();
        int refreshes = g_string_refreshes;
        sh_decl_server_test_retirement_timing(20, 80);
        sh_package_runtime_test_occurrences_pending(1);
        CHECK(sh_package_runtime_occurrences_pending() && sh_decl_server_map_retirement_safe());
        attempts = sh_decl_server_test_retirement_attempts();
        int departures = g_retained_departures;
        log_all_reset();
        for (ULONGLONG start = GetTickCount64(); GetTickCount64() - start < 300;) { sh_decl_server_rearm_poll(); Sleep(5); }
        CHECK(sh_decl_server_test_retirement_attempts() == attempts + 1);
        /* R-393: the successful retirement is accounted as a departure, once. */
        CHECK(g_retained_departures == departures + 1 && !strcmp(g_retained_route, "retired at the browser"));
        CHECK(!sh_package_runtime_occurrences_pending() && sh_package_runtime_activations_skipped() == skipped + 1);
        CHECK(g_string_refreshes == refreshes && sh_decl_server_registration_succeeded() == 1);
        CHECK(strstr(g_log_all, "the served provider is unchanged") != NULL);
        CHECK(!strstr(g_log_all, "RE-ARM:") && !strstr(g_log_all, "retained resource activation"));
        sh_decl_server_test_retirement_timing(1000, 60000);
    }
    {
        /* R-398: with the native map boundary available and nothing the browser shows at stake, released consumers'
         * retained rows wait for the next boundary: no retirement activation at the browser, one line. */
        long attempts;
        sh_decl_server_test_retirement_timing(20, 80);
        sh_package_runtime_test_occurrences_pending(1);
        g_transition_ready = 1;
        attempts = sh_decl_server_test_retirement_attempts();
        log_all_reset();
        for (ULONGLONG start = GetTickCount64(); GetTickCount64() - start < 200;) { sh_decl_server_rearm_poll(); Sleep(5); }
        CHECK(sh_decl_server_test_retirement_attempts() == attempts && sh_package_runtime_occurrences_pending());
        CHECK(strstr(g_log_all, "retained resource retirement deferred to the next map boundary") != NULL);
        {
            const char *first = strstr(g_log_all, "retirement deferred");
            CHECK(first && !strstr(first + 1, "retirement deferred"));
        }
        /* Without the boundary the browser retires them as before. */
        g_transition_ready = 0;
        for (ULONGLONG start = GetTickCount64(); GetTickCount64() - start < 300;) { sh_decl_server_rearm_poll(); Sleep(5); }
        CHECK(sh_decl_server_test_retirement_attempts() == attempts + 1 && !sh_package_runtime_occurrences_pending());
        sh_decl_server_test_retirement_timing(1000, 60000);
    }
    {
        /* R-398: each registration step runs only when its inputs changed, and
         * the registered state is the full pass's. The built-ins are editor
         * declarations: a pass that re-parses them rebuilds the palette. */
        int palettes, refreshes, loads;
        int before_loads;
        g_palette_current = 1;
        g_resident_filter = 1; g_resident_only = NULL; /* nothing selected: nothing changed */
        /* A full pass that changed no editor or entity template row keeps the palette. */
        loads = builtin_loads(); before_loads = loads; palettes = g_apply_palette_calls;
        log_all_reset();
        CHECK(sh_decl_server_rearm() == 1 && sh_decl_server_registration_succeeded());
        loads = builtin_loads();
        CHECK(loads == before_loads && g_apply_palette_calls == palettes);
        CHECK(strstr(g_log_all, "palette-refresh KEPT") != NULL && !strstr(g_log_all, "RE-ARM kept"));
        /* A reopen that changed nothing keeps the registration: the policy steps
         * run, nothing is re-parsed and the palette stands. */
        palettes = g_apply_palette_calls; refreshes = g_string_refreshes;
        sh_package_runtime_test_force_activation(1);
        log_all_reset();
        CHECK(sh_decl_server_activate_map(NULL) == 1 && sh_decl_server_registration_succeeded());
        loads = builtin_loads();
        CHECK(loads == before_loads && g_apply_palette_calls == palettes && g_string_refreshes == refreshes + 1);
        if (!strstr(g_log_all, "RE-ARM kept")) fprintf(stderr, "reopen: %s\n", g_log_all);
        CHECK(strstr(g_log_all, "RE-ARM kept") != NULL);
        /* Its registered state is the full pass's: a full pass over it re-parses
         * nothing, changes no row and keeps the same palette. */
        log_all_reset();
        CHECK(sh_decl_server_rearm() == 1 && sh_decl_server_registration_succeeded());
        loads = builtin_loads();
        CHECK(loads == before_loads && g_apply_palette_calls == palettes);
        CHECK(strstr(g_log_all, "palette-refresh KEPT") != NULL);
        /* A declaration not in its parsed state is repaired by a full pass, as before. */
        g_rt_builtins[0].object[0x2c] |= 0x04;
        log_all_reset();
        CHECK(sh_decl_server_activate_map(NULL) == 1 && sh_decl_server_registration_succeeded());
        CHECK(strstr(g_log_all, "registers in full: a declaration is not in its parsed state") != NULL);
        loads = builtin_loads();
        CHECK(loads > before_loads && !(g_rt_builtins[0].object[0x2c] & 0x04));
        CHECK(g_apply_palette_calls == palettes + 1); /* an editor declaration was re-parsed */
        before_loads = loads; palettes = g_apply_palette_calls;
        /* A pass whose resident refresh selects consumers (a changed or rebound
         * resource's) registers in full; re-parsed editor rows rebuild the palette. */
        g_resident_filter = 0;
        log_all_reset();
        CHECK(sh_decl_server_activate_map(NULL) == 1 && sh_decl_server_registration_succeeded());
        CHECK(strstr(g_log_all, "registers in full: the resident refresh selected consumers") != NULL);
        loads = builtin_loads();
        CHECK(loads > before_loads && g_apply_palette_calls == palettes + 1);
        g_resident_filter = 1; g_resident_only = NULL;
        /* Selected templates alone (the resident pass's consumers) rebuild it too. */
        loads = builtin_loads(); before_loads = loads; palettes = g_apply_palette_calls;
        g_resident_templates = 1;
        CHECK(sh_decl_server_rearm() == 1);
        CHECK(g_apply_palette_calls == palettes + 1);
        g_resident_templates = 0;
        /* A palette that is not the one built for these rows is rebuilt, and a
         * reopen over it registers in full. */
        palettes = g_apply_palette_calls;
        g_palette_current = 0;
        log_all_reset();
        CHECK(sh_decl_server_activate_map(NULL) == 1 && sh_decl_server_registration_succeeded());
        CHECK(g_apply_palette_calls == palettes + 1 && !strstr(g_log_all, "RE-ARM kept"));
        g_palette_current = 1;
        /* R-401: under register-once, a pass the runtime hands no change keeps the registration even when its
         * candidates are not the standing registration's inputs (a view that lost rows the engine keeps), and
         * records its rows as a full pass would; without the switch the same pass registers in full. */
        CHECK(sh_decl_server_rearm() == 1);
        sh_decl_server_test_perturb_registration();
        log_all_reset();
        CHECK(sh_decl_server_activate_map(NULL) == 1);
        CHECK(strstr(g_log_all, "registers in full: these inputs are not the registration standing") != NULL);
        loads = builtin_loads(); before_loads = loads; palettes = g_apply_palette_calls;
        sh_package_runtime_test_register_once(1);
        sh_decl_server_test_perturb_registration();
        log_all_reset();
        CHECK(sh_decl_server_activate_map(NULL) == 1 && sh_decl_server_registration_succeeded());
        if (!strstr(g_log_all, "RE-ARM kept (register-once)")) fprintf(stderr, "register-once keep: %s\n", g_log_all);
        CHECK(strstr(g_log_all, "RE-ARM kept (register-once)") != NULL);
        CHECK(builtin_loads() == before_loads && g_apply_palette_calls == palettes);
        /* Its registered state is the full pass's: a full pass re-parses nothing and keeps the palette. */
        log_all_reset();
        CHECK(sh_decl_server_rearm() == 1 && sh_decl_server_registration_succeeded());
        CHECK(builtin_loads() == before_loads && g_apply_palette_calls == palettes);
        sh_package_runtime_test_register_once(0);
        /* Additive admission or shader preparation forgets the standing registration. */
        CHECK(sh_decl_server_rearm() == 1);
        log_all_reset();
        CHECK(sh_decl_server_activate_map(NULL) == 1 && strstr(g_log_all, "RE-ARM kept"));
        sh_decl_server_test_forget_registration();
        log_all_reset();
        CHECK(sh_decl_server_activate_map(NULL) == 1);
        CHECK(strstr(g_log_all, "registers in full: these inputs are not the registration standing") != NULL);
        /* An explicit re-arm is always a full pass. */
        log_all_reset();
        CHECK(sh_decl_server_rearm() == 1 && !strstr(g_log_all, "RE-ARM kept"));
        sh_package_runtime_test_force_activation(0);
        g_resident_filter = 0; g_resident_only = NULL; g_palette_current = 0;
    }
    {
        const sh_package_compilation *before = sh_package_runtime_acquire(), *after;
        int scans = g_scan_calls, refreshes = g_string_refreshes, retries = g_startup_retries;
        sh_package_runtime_release();
        g_startup_ready = g_startup_recovery_ok = 0;
        CHECK(sh_decl_server_rearm() == 0 && !sh_decl_server_registration_succeeded());
        CHECK(g_startup_retries == retries + 1 && g_scan_calls == scans && g_string_refreshes == refreshes);
        after = sh_package_runtime_acquire(); CHECK(after == before); sh_package_runtime_release();
        g_startup_recovery_ok = 1;
        CHECK(sh_decl_server_rearm() == 1 && sh_decl_server_registration_succeeded());
        CHECK(g_startup_retries == retries + 2 && g_scan_calls == scans + 1 && g_string_refreshes == refreshes + 1);
    }
    g_runtime_dependencies_ok = 0;
    CHECK(sh_decl_server_rearm() == 0);
    CHECK(sh_decl_server_registration_succeeded() == 0);
    g_runtime_dependencies_ok = 1;
    CHECK(sh_decl_server_rearm() == 1);
    {
        const sh_package_compilation *before = sh_package_runtime_acquire(), *after;
        int scans = g_scan_calls, refreshes = g_string_refreshes;
        char error[2048];
        sh_package_runtime_release();
        g_fail_next_string_refresh = 1;
        CHECK(sh_decl_server_rearm() == 0);
        after = sh_package_runtime_acquire(); CHECK(after == before);
        sh_package_runtime_release();
        CHECK(g_scan_calls == scans + 1); /* Recovery must not read authored sources again. */
        CHECK(g_string_refreshes == refreshes + 2);
        CHECK(!sh_package_runtime_ready() && !sh_decl_server_registration_succeeded());
        sh_package_runtime_error(error, sizeof(error));
        CHECK(strstr(error, "activation") && !strstr(error, "recovery failed"));
        CHECK(sh_decl_server_rearm() == 1);
        CHECK(sh_package_runtime_ready() && sh_decl_server_registration_succeeded());
    }

    {
        /* R-309 in-place image recovery: after the resident drain a forward activation that recovers
         * an image fails while the native probe still reports an exclusion placeholder (or cannot
         * tell, or none is installed), and the previous provider is restored; a recovered image
         * passes; its return to the native decision is never refused. */
        static const char *const need[] = {"env/x"};
        const sh_package_compilation *before = sh_package_runtime_acquire(), *after;
        char error[2048];
        sh_package_runtime_release();
        sh_package_runtime_test_recovery_need(need, 1, 1);
        for (int state = 1; state >= -2; state -= 1) {
            if (state == 0) continue;
            g_test_image_state = state;
            sh_decl_server_image_state(state == -2 ? NULL : test_image_state);
            CHECK(sh_decl_server_rearm() == 0);
            after = sh_package_runtime_acquire(); CHECK(after == before); sh_package_runtime_release();
            sh_package_runtime_error(error, sizeof(error));
            if (!strstr(error, "declared image availability was not recovered: env/x"))
                fprintf(stderr, "recovery verification %d: %s\n", state, error);
            CHECK(strstr(error, "declared image availability was not recovered: env/x") &&
                strstr(error, state == 1 ? "still an exclusion placeholder" : "native state unknown"));
        }
        g_test_image_state = 0; sh_decl_server_image_state(test_image_state);
        CHECK(sh_decl_server_rearm() == 1 && sh_decl_server_registration_succeeded());
        /* The gate is no longer declared: the image returns, whatever the probe now reports. */
        sh_package_runtime_test_recovery_need(NULL, 0, 0);
        g_test_image_state = 1;
        CHECK(sh_decl_server_rearm() == 1 && sh_decl_server_registration_succeeded());
        sh_decl_server_image_state(NULL);
    }
    {
        const sh_package_compilation *before = sh_package_runtime_acquire(), *after;
        int audio = g_audio_refreshes, commits = g_install_commit_calls;
        int scans = g_scan_calls;
        sh_package_runtime_release();
        g_fail_next_audio_refresh = 1;
        CHECK(sh_decl_server_rearm() == 0);
        after = sh_package_runtime_acquire(); CHECK(after == before); sh_package_runtime_release();
        CHECK(g_audio_refreshes == audio + 2 && g_install_commit_calls == commits);
        CHECK(g_scan_calls == scans + 1);
        CHECK(!sh_package_runtime_ready() && !sh_decl_server_registration_succeeded());
        CHECK(sh_package_runtime_admission_ready());
        CHECK(sh_decl_server_rearm() == 1);
        CHECK(sh_package_runtime_ready() && sh_decl_server_registration_succeeded());
    }

    {
        /* Native inventory refusal and a late cached-resource failure both
         * restore the previous provider and skip installation commit. */
        const sh_package_compilation *before = sh_package_runtime_acquire(), *after;
        int commits = g_install_commit_calls;
        sh_package_runtime_release();
        for (int failure = 0; failure < 2; failure++) {
            int begins = g_resident_begin_calls, ends = g_resident_end_calls;
            if (failure) g_resident_fail_drain = 1; else g_resident_fail_begin = 1;
            CHECK(sh_decl_server_rearm() == 0);
            /* Failed Begin owns no resident handle. Only recovery ends one;
             * a failed drain still ends both admitted resident passes. */
            CHECK(g_resident_begin_calls == begins + 2 && g_resident_end_calls == ends + (failure ? 2 : 1));
            CHECK(g_install_commit_calls == commits && sh_package_runtime_admission_ready());
            after = sh_package_runtime_acquire(); CHECK(after == before); sh_package_runtime_release();
        }
        CHECK(sh_decl_server_rearm() == 1);
    }
    {
        /* A failed read-only storage preflight cannot reach disk commit. */
        const sh_package_compilation *before=sh_package_runtime_acquire(),*after;
        sh_package_runtime_release(); int commits=g_install_commit_calls,rejects=g_editor_rejects;
        g_editor_ready_refuse=1;
        CHECK(!sh_decl_server_rearm());
        after=sh_package_runtime_acquire(); CHECK(after==before); sh_package_runtime_release();
        CHECK(g_install_commit_calls==commits && g_editor_rejects==rejects+1 && !g_editor_live);
        CHECK(sh_decl_server_rearm());
        /* Simulate identity/admission refusal after the irreversible commit.
         * The candidate remains served; NEXT refresh closes accepted storage
         * before new work, without repeating the previous disk commit. */
        before=sh_package_runtime_acquire(); sh_package_runtime_release();
        commits=g_install_commit_calls; rejects=g_editor_rejects; int accepts=g_editor_accepts;
        int cancels=g_install_cancel_calls;
        g_editor_after_commit_refuse=1;
        CHECK(!sh_decl_server_rearm());
        after=sh_package_runtime_acquire(); CHECK(after!=before); sh_package_runtime_release();
        CHECK(g_install_commit_calls==commits+1 && g_editor_accepts==accepts &&
            g_editor_rejects==rejects && g_editor_live && !sh_package_runtime_admission_ready());
        CHECK(g_install_cancel_calls==cancels);
        CHECK(sh_decl_server_rearm());
        CHECK(g_install_commit_calls==commits+2 && g_editor_accepts==accepts+2 &&
            g_editor_rejects==rejects && !g_editor_live && g_install_cancel_calls==cancels);
    }
    {
        /* Installed packages changed outside an installation are prepared again
         * in this process at the browser, once per change: after a failed
         * preparation and in a served library that left an owner out, for a
         * change made after or during a pass or before the watch could open,
         * across the watched directory being replaced or removed, for an edit
         * in place that keeps the size and write time, and for a request the
         * server could not take at first. An unchanged inventory, served or
         * failed, is never prepared again, and a lost watch reopens, and an
         * unanswered request repeats, at a growing interval, not per frame.
         * Each wait outlasts the settle time several times before passes are
         * counted. */
        char overrides[MAX_PATH], marker[MAX_PATH], moved[MAX_PATH], left[MAX_PATH], path[MAX_PATH];
        const sh_package_compilation *served;
        size_t packages;
        long opens, requests;
        int scans;
        _snprintf_s(overrides, sizeof(overrides), _TRUNCATE, "%s\\overrides", g_fixture_root);
        _snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\overrides\\fixture\\corrected.txt", g_fixture_root);
        _snprintf_s(moved, sizeof(moved), _TRUNCATE, "%s\\overrides-moved", g_fixture_root);
        _snprintf_s(left, sizeof(left), _TRUNCATE, "%s\\overrides\\left", g_fixture_root);
        sh_decl_server_test_inventory_timing(INVENTORY_QUIET_MS, 20, 0, 20);
        /* Start with the watch open and nothing pending: earlier tests may
         * have closed it, and its reopening would count as one change. */
        sh_decl_server_test_inventory_drop();
        CHECK(sh_decl_server_rearm() == 1);
        inventory_settle();
        /* 1. The whole preparation failed; an unchanged inventory waits. */
        g_scan_fail = 1;
        CHECK(sh_decl_server_rearm() == 0);
        scans = g_scan_calls;
        inventory_settle();
        CHECK(g_scan_calls == scans && !sh_decl_server_registration_succeeded());
        /* Corrected afterwards: one pass at the browser serves it. */
        g_scan_fail = 0; inventory_write(marker, "corrected");
        inventory_settle();
        CHECK(g_scan_calls == scans + 1 && sh_decl_server_registration_succeeded() && sh_package_runtime_ready());
        /* 2. Corrected while the failing pass read the inventory. */
        g_scan_fail = 1; g_scan_correct_path = marker;
        CHECK(sh_decl_server_rearm() == 0);
        scans = g_scan_calls;
        inventory_settle();
        CHECK(g_scan_calls == scans + 1 && sh_decl_server_registration_succeeded());
        /* 3. A retry that fails again waits for the next change. */
        g_scan_fail = 1;
        CHECK(sh_decl_server_rearm() == 0);
        scans = g_scan_calls;
        inventory_write(marker, "still broken");
        inventory_settle();
        CHECK(g_scan_calls == scans + 1 && !sh_decl_server_registration_succeeded());
        /* 4. No watch when the pass began (its handle failed and reopening
         * still fails), and the correction comes before any watch exists: the
         * reopened watch counts as a change and the inventory is retried. */
        sh_decl_server_test_inventory_drop();
        sh_decl_server_test_inventory_fail_opens(2);
        CHECK(sh_decl_server_rearm() == 0);
        scans = g_scan_calls;
        g_scan_fail = 0; inventory_write(marker, "corrected before the watch");
        inventory_settle();
        CHECK(g_scan_calls == scans + 1 && sh_decl_server_registration_succeeded());
        /* 5. Replaced: the old handle watches the moved tree; the identity
         * check finds the new directory and its inventory is served. */
        g_scan_fail = 1;
        CHECK(sh_decl_server_rearm() == 0);
        scans = g_scan_calls;
        CHECK(MoveFileA(overrides, moved));
        CHECK(CreateDirectoryA(overrides, NULL));
        runtime_fixture_write();
        inventory_write(marker, "replaced");
        g_scan_fail = 0;
        inventory_settle();
        CHECK(g_scan_calls == scans + 1 && sh_decl_server_registration_succeeded());
        CHECK(inventory_remove_tree(moved));
        /* 6. Removed, then recreated: reopening backs off instead of trying
         * every frame, and the recreated inventory is served. */
        g_scan_fail = 1;
        CHECK(sh_decl_server_rearm() == 0);
        scans = g_scan_calls;
        CHECK(MoveFileA(overrides, moved));
        opens = sh_decl_server_test_inventory_opens();
        inventory_poll_for(1000);
        opens = sh_decl_server_test_inventory_opens() - opens;
        if (opens < 2 || opens > 6) fprintf(stderr, "removed watch: %ld reopen attempt(s) in 1 s\n", opens);
        CHECK(opens >= 2 && opens <= 6);
        g_scan_fail = 0;
        CHECK(MoveFileA(moved, overrides));
        inventory_write(marker, "recreated");
        inventory_until_pass(scans);
        inventory_settle();
        CHECK(g_scan_calls == scans + 1 && sh_decl_server_registration_succeeded());
        /* 7. Served with an owner left out (one of its declarations does not
         * read); correcting it serves it in this process. */
        CHECK(CreateDirectoryA(left, NULL));
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\package.json", left);
        inventory_write(path, "{\"id\":\"left\",\"name\":\"Left\"}");
        const char *levels[] = {"assets", "assets\\generated", "assets\\generated\\decls",
            "assets\\generated\\decls\\snapeditorentitydef"};
        for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", left, levels[i]);
            CHECK(CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
        }
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\assets\\left.bin", left);
        inventory_write(path, "independent resource");
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\assets\\generated\\decls\\snapeditorentitydef\\bad.decl", left);
        inventory_write(path, "{} }");
        scans = g_scan_calls;
        inventory_settle();
        CHECK(g_scan_calls == scans + 1 && sh_decl_server_registration_succeeded());
        {
            char *summary = sh_package_runtime_summary();
            if (!summary || !strstr(summary, "Skipped local package")) fprintf(stderr, "left out owner: %s\n", summary ? summary : "");
            CHECK(summary && strstr(summary, "Skipped local package"));
            free(summary);
        }
        served = sh_package_runtime_acquire(); packages = served ? served->sources->package_count : 0;
        sh_package_runtime_release();
        scans = g_scan_calls;
        CHECK(DeleteFileA(path));
        inventory_settle();
        CHECK(g_scan_calls == scans + 1 && sh_decl_server_registration_succeeded());
        served = sh_package_runtime_acquire();
        if (!served || served->sources->package_count != packages + 1) {
            char *summary = sh_package_runtime_summary();
            fprintf(stderr, "corrected owner: %zu package(s) served, %zu before: %s\n",
                served ? served->sources->package_count : 0, packages, summary ? summary : "");
            free(summary);
        }
        CHECK(served && served->sources->package_count == packages + 1);
        sh_package_runtime_release();
        /* 8. An edit in place that keeps the size and restores the write time
         * is a change: the notification decides, not the file's metadata, and
         * the pass serves the new bytes. */
        {
            WIN32_FILE_ATTRIBUTE_DATA after;
            unsigned char *body = NULL;
            size_t length = 0;
            FILETIME written;
            DWORD count = 0;
            HANDLE file;
            _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\assets\\left.bin", left);
            CHECK(sh_package_runtime_read("left.bin", &body, &length) == 1 && length == 20 &&
                !memcmp(body, "independent resource", 20));
            free(body); body = NULL;
            file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
            CHECK(file != INVALID_HANDLE_VALUE);
            scans = g_scan_calls;
            if (file != INVALID_HANDLE_VALUE) {
                CHECK(GetFileTime(file, NULL, NULL, &written));
                CHECK(WriteFile(file, "INDEPENDENT RESOURCE", 20, &count, NULL) && count == 20);
                CHECK(SetFileTime(file, NULL, NULL, &written) && FlushFileBuffers(file));
                CloseHandle(file);
                CHECK(GetFileAttributesExA(path, GetFileExInfoStandard, &after) && after.nFileSizeLow == 20 &&
                    !after.nFileSizeHigh && !CompareFileTime(&after.ftLastWriteTime, &written));
            }
            inventory_settle();
            CHECK(g_scan_calls == scans + 1 && sh_decl_server_registration_succeeded());
            CHECK(sh_package_runtime_read("left.bin", &body, &length) == 1 && length == 20 &&
                !memcmp(body, "INDEPENDENT RESOURCE", 20));
            free(body);
        }
        /* 9. A request the server cannot take yet (overrides unavailable for
         * this launch) is repeated at a growing interval, not lost and not per
         * frame, and the change is prepared once it can be. */
        g_user_enabled = 0;
        scans = g_scan_calls; requests = sh_decl_server_test_inventory_requests();
        inventory_write(marker, "while unavailable");
        inventory_poll_for(2500);
        requests = sh_decl_server_test_inventory_requests() - requests;
        if (requests < 3 || requests > 10) fprintf(stderr, "unanswered request: repeated %ld time(s) in 2.5 s\n", requests);
        CHECK(g_scan_calls == scans && requests >= 3 && requests <= 10);
        g_user_enabled = 1;
        inventory_until_pass(scans);
        inventory_settle();
        CHECK(g_scan_calls == scans + 1 && sh_decl_server_registration_succeeded());
        /* 10. The watcher's pass, and only it, lets the runtime reuse an
         * unchanged library; a forced request upgrades a pending watched one,
         * and a programmatic pass is forced. The note never outlives a pass. */
        scans = g_scan_calls; g_scan_watched = -1;
        inventory_write(marker, "watched pass");
        inventory_until_pass(scans);
        CHECK(g_scan_calls == scans + 1 && g_scan_watched == 1 && !sh_package_runtime_test_watched_change());
        inventory_settle();
        scans = g_scan_calls; g_scan_watched = -1;
        sh_decl_server_test_request_watched(); sh_decl_server_request_rearm();
        inventory_until_pass(scans);
        CHECK(g_scan_calls == scans + 1 && g_scan_watched == 0);
        scans = g_scan_calls; g_scan_watched = -1;
        sh_decl_server_test_request_watched();
        inventory_until_pass(scans);
        CHECK(g_scan_calls == scans + 1 && g_scan_watched == 1 && !sh_package_runtime_test_watched_change());
        scans = g_scan_calls; g_scan_watched = -1;
        CHECK(sh_decl_server_rearm() == 1 && g_scan_calls == scans + 1 && g_scan_watched == 0);
        inventory_settle();
        /* A watched pass whose scan fails before any refresh leaves no note. */
        scans = g_scan_calls; g_scan_watched = -1; g_scan_fail = 1;
        inventory_write(marker, "watched failure");
        inventory_until_pass(scans);
        CHECK(g_scan_calls == scans + 1 && g_scan_watched == 1 && !sh_package_runtime_test_watched_change());
        g_scan_fail = 0;
        CHECK(sh_decl_server_rearm() == 1);
        inventory_settle();
        /* 11. The product's own outputs beside the overrides tree (the package
         * cache and its warm, scope-reuse and eviction writes, the staging
         * folder, the rawmap shadow, prefabs, exports, the log) are not
         * watched: nothing the product writes there re-arms the server (R-381). */
        {
            const char *siblings[] = {"package-cache", "package-cache\\resources", "package-staging",
                "package-staging\\map-0123456789abcdef", "prefabs", "exports"};
            const char *files[] = {"package-cache\\warm-publication.txt", "package-cache\\resources\\0123.bin",
                "package-cache\\scope-reuse-a.txt", "package-staging\\map-0123456789abcdef\\pending",
                "rawmap.json", "prefabs\\probe.json", "exports\\probe.txt", "sh_backend.log"};
            int created[sizeof(siblings) / sizeof(siblings[0])] = {0};
            for (size_t i = 0; i < sizeof(siblings) / sizeof(siblings[0]); i++) {
                _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_fixture_root, siblings[i]);
                created[i] = CreateDirectoryA(path, NULL) != 0;
                CHECK(created[i] || GetLastError() == ERROR_ALREADY_EXISTS);
            }
            scans = g_scan_calls; requests = sh_decl_server_test_inventory_requests();
            for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
                _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_fixture_root, files[i]);
                inventory_write(path, "product output");
            }
            inventory_settle();
            for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
                _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_fixture_root, files[i]);
                inventory_write(path, "product output again");
                DeleteFileA(path);
            }
            inventory_settle();
            CHECK(g_scan_calls == scans && sh_decl_server_test_inventory_requests() == requests);
            CHECK(sh_decl_server_registration_succeeded());
            /* Only the exact directories this step created, deepest first. */
            for (size_t i = sizeof(siblings) / sizeof(siblings[0]); i; i--) if (created[i - 1]) {
                _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", g_fixture_root, siblings[i - 1]);
                CHECK(RemoveDirectoryA(path));
            }
        }
        /* 12. A report over an unchanged inventory (a file written with the bytes
         * it had) prepares once at the browser; the runtime keeps the served
         * library and the server keeps its registration: no compile, no pass. */
        {
            int refreshes = g_string_refreshes;
            long skipped = sh_package_runtime_activations_skipped();
            scans = g_scan_calls; g_scan_watched = -1;
            log_all_reset();
            inventory_write(marker, "watched failure");
            inventory_until_pass(scans);
            inventory_settle();
            CHECK(g_scan_calls == scans + 1 && g_scan_watched == 1);
            CHECK(g_string_refreshes == refreshes && sh_package_runtime_activations_skipped() == skipped + 1);
            CHECK(strstr(g_log_all, "nothing to compile") != NULL);
            CHECK(strstr(g_log_all, "decl-server RE-ARM: the served library is unchanged") != NULL);
            CHECK(sh_decl_server_registration_succeeded());
        }
        /* Restore the fixture inventory for the rest of the suite. */
        CHECK(inventory_remove_tree(left));
        CHECK(DeleteFileA(marker));
        inventory_settle();
        CHECK(sh_decl_server_rearm() == 1);
        sh_decl_server_test_inventory_timing(2000, 1000, 5000, 2000);
    }
    {
        int commits = g_install_commit_calls;
        g_defer_install_commit = 1;
        CHECK(sh_decl_server_rearm() == 0);
        sh_decl_server_rearm_poll();
        CHECK(g_install_commit_calls == commits);
        g_defer_install_commit = 0;
        sh_decl_server_rearm_poll();
        CHECK(g_install_commit_calls == commits + 1);
    }
    {
        /* A watched request waits out an installation's deferral at the poll
         * and is still the watcher's pass when it runs. */
        int scans = g_scan_calls;
        g_defer_install_commit = 1; g_scan_watched = -1;
        sh_decl_server_test_request_watched();
        sh_decl_server_rearm_poll();
        CHECK(g_scan_calls == scans);
        g_defer_install_commit = 0;
        sh_decl_server_rearm_poll();
        CHECK(g_scan_calls == scans + 1 && g_scan_watched == 1 && !sh_package_runtime_test_watched_change());
    }
    {
        /* Deferrals that begin after the poll took a watched request, at the
         * re-arm command's own checks, request it again as the watcher's: an
         * installation starting to defer, and the browser ceasing to be a
         * safe boundary. */
        int (*hooks[2])(void) = {defer_begins, boundary_closes};
        for (int i = 0; i < 2; i++) {
            int scans = g_scan_calls;
            g_scan_watched = -1;
            sh_decl_server_test_request_watched();
            g_defer_hook = hooks[i]; g_defer_countdown = 2;
            sh_decl_server_rearm_poll();
            CHECK(g_scan_calls == scans && !g_defer_countdown);
            g_defer_hook = NULL; g_boundary_thread = GetCurrentThreadId();
            sh_decl_server_rearm_poll();
            CHECK(g_scan_calls == scans + 1 && g_scan_watched == 1 && !sh_package_runtime_test_watched_change());
        }
    }
    for (int failure = 1; failure <= 2; failure++) {
        const sh_package_compilation *before = sh_package_runtime_acquire(), *after;
        int commits = g_install_commit_calls, cancels = g_install_cancel_calls;
        int scans = g_scan_calls, refreshes = g_string_refreshes;
        sh_package_runtime_release();
        g_install_expected_recovery = before; g_install_commit_fail = failure;
        CHECK(sh_decl_server_rearm() == 0);
        after = sh_package_runtime_acquire(); CHECK(after == before); sh_package_runtime_release();
        CHECK(g_install_commit_calls == commits + 1 && g_install_cancel_calls == cancels + 1);
        CHECK(g_scan_calls == scans + 1 && g_string_refreshes == refreshes + 2);
        CHECK(!sh_package_runtime_ready() && !sh_decl_server_registration_succeeded());
        g_install_expected_recovery = NULL;
        CHECK(sh_decl_server_rearm() == 1);
        CHECK(sh_package_runtime_ready() && sh_decl_server_registration_succeeded());
    }

    rt_reset();
    g_rt_names[0] = "rt/alpha";
    g_rt_objects[0] = decl;
    decl[0x2c] = 0x04;
    g_table_published = 1;
    g_table_merges = 0;
    g_apply_palette_calls = 0;
    CHECK(sh_decl_server_test_apply(&item, 1, 1) == 1);
    CHECK(g_apply_palette_calls == 1 && g_rt_loads[0] == 1 && g_table_merges == 1);
    /* Repeated same-identity edits must refresh again, including decltree bytes. */
    CHECK(sh_decl_server_test_apply(&item, 1, 1) == 1);
    CHECK(g_apply_palette_calls == 2 && g_rt_loads[0] == 2 && g_table_merges == 2);
    g_apply_palette_result = 0;
    CHECK(sh_decl_server_test_apply(&item, 1, 1) == 0);
    CHECK(sh_decl_server_registration_succeeded() == 0);
    g_apply_palette_result = 1;
    CHECK(sh_decl_server_test_apply(&item, 1, 1) == 1);
    {
        unsigned char resource_node[0x30] = {0};
        void *resources[] = {decl};
        int loads, palettes;
        *(void **)(resource_node + 0x20) = resources;
        *(int *)(resource_node + 0x28) = 1;
        g_boundary_resource_head = resource_node;
        g_linked_fixture = 1;
        heap_fixture_reset();
        g_heap_check_native = 1;
        CHECK(sh_decl_server_rearm() == 1);
        CHECK(g_heap_decl_calls > 0 && g_heap_palette_calls == 1);
        CHECK(g_heap_pushes == 1 && g_heap_pops == 1);
        CHECK(g_heap_local.depth == 1 && g_heap_local.heaps[0] == 2);

        /* An unconfirmed push must refuse before any native parse or palette
         * allocation; a later confirmed scope can retry the same pass. */
        loads = g_heap_decl_calls;
        palettes = g_heap_palette_calls;
        g_heap_push_noop = 1;
        CHECK(sh_decl_server_rearm() == 0);
        CHECK(sh_decl_server_registration_succeeded() == 0);
        CHECK(g_heap_decl_calls == loads && g_heap_palette_calls == palettes);
        CHECK(g_heap_local.depth == 1 && g_heap_local.heaps[0] == 2);
        CHECK(g_heap_pushes == 3 && g_heap_pops == 1); /* Candidate and recovery both refuse the push. */
        g_heap_push_noop = 0;
        CHECK(sh_decl_server_rearm() == 1);
        CHECK(g_heap_decl_calls > loads && g_heap_palette_calls == palettes + 1);
        CHECK(g_heap_pushes == 4 && g_heap_pops == 2);
        CHECK(g_heap_local.depth == 1 && g_heap_local.heaps[0] == 2);
        g_heap_check_native = 0;
        {
            const sh_package_compilation *before, *after;
            int scans;
            g_rt_capture_body = 1; g_rt_body_count = 0;
            g_linked_body = "{ value = 1; }";
            CHECK(sh_decl_server_rearm() == 1);
            CHECK(g_rt_body_count == 1 && strstr(g_rt_bodies[0], "value = 1"));
            before = sh_package_runtime_acquire(); sh_package_runtime_release();
            g_linked_body = "{ value = 2; }";
            for (int failure = 1; failure <= 2; failure++) {
                /* Fail after the new declaration was actually consumed. Both
                 * ordinary failure and a native exception must reload old bytes. */
                g_rt_body_count = 0; scans = g_scan_calls;
                g_fail_next_palette = failure;
                CHECK(sh_decl_server_rearm() == 0);
                CHECK(g_scan_calls == scans + 1);
                CHECK(g_rt_body_count == 2);
                CHECK(strstr(g_rt_bodies[0], "value = 2") && strstr(g_rt_bodies[1], "value = 1"));
                after = sh_package_runtime_acquire(); CHECK(after == before); sh_package_runtime_release();
                CHECK(!sh_package_runtime_ready() && !sh_decl_server_registration_succeeded());
                CHECK(!(decl[0x2c] & 0x06));
            }
            g_rt_body_count = 0;
            CHECK(sh_decl_server_rearm() == 1);
            CHECK(g_rt_body_count == 1 && strstr(g_rt_bodies[0], "value = 2"));
            CHECK(sh_package_runtime_ready());
            g_rt_capture_body = 0; g_linked_body = "{}";
        }
        sh_package_runtime_test_baseline(runtime_original, NULL);
        CHECK(sh_decl_server_rearm() == 1);
        loads = g_rt_loads[0]; palettes = g_apply_palette_calls;
        g_linked_fixture = 0;
        CHECK(sh_decl_server_rearm() == 1);
        CHECK(g_rt_loads[0] > loads && g_apply_palette_calls == palettes + 1);
        {
            /* R-398: a package adding, then withdrawing, a row that is no editor
             * declaration or entity template (this material) re-parses it and
             * keeps a palette holding the rows its last rebuild produced. */
            int kept_loads = g_rt_loads[0], kept_palettes = g_apply_palette_calls;
            g_palette_current = 1; g_resident_filter = 1; g_resident_only = decl;
            g_linked_fixture = 1;
            log_all_reset();
            CHECK(sh_decl_server_rearm() == 1);
            CHECK(g_rt_loads[0] > kept_loads && g_apply_palette_calls == kept_palettes);
            CHECK(strstr(g_log_all, "palette-refresh KEPT") != NULL);
            kept_loads = g_rt_loads[0];
            g_linked_fixture = 0;
            CHECK(sh_decl_server_rearm() == 1);
            CHECK(g_rt_loads[0] > kept_loads && g_apply_palette_calls == kept_palettes);
            g_palette_current = 0; g_resident_filter = 0; g_resident_only = NULL;
        }
        {
            unsigned char *restored = NULL; size_t length = 0;
            CHECK(sh_package_runtime_read("generated/decls/material/rt/alpha.decl", &restored, &length) == 1);
            CHECK(restored && !strcmp((char *)restored, "{ original = 1; }"));
            free(restored);
        }
        /* Keep the verified original reader until fixture teardown completes. */
        g_boundary_resource_head = NULL;
        g_linked_fixture = 0;
    }
    {
        /* End raises after successful disk commit, before writing committed.
         * Keep the new provider, disable readiness and retain the damaged
         * native context without cancellation or End/destructor replay. */
        const sh_package_compilation *before=sh_package_runtime_acquire(),*after;
        sh_package_runtime_release();
        int commits=g_install_commit_calls,cancels=g_install_cancel_calls;
        int ends=g_editor_end_calls,accepts=g_editor_accepts,rejects=g_editor_rejects;
        g_boundary_resource_head=g_rt_builtin_node;
        g_editor_after_commit_throw=1;
        CHECK(!sh_decl_server_rearm());
        after=sh_package_runtime_acquire(); CHECK(after!=before); sh_package_runtime_release();
        CHECK(g_install_commit_calls==commits+1 && g_install_cancel_calls==cancels &&
            g_editor_end_calls==ends+1 && g_editor_live &&
            g_editor_accepts==accepts && g_editor_rejects==rejects);
        CHECK(!sh_package_runtime_ready() && !sh_package_runtime_admission_ready());
        CHECK(!sh_decl_server_rearm());
        sh_decl_server_request_rearm(); sh_decl_server_rearm_poll();
        after=sh_package_runtime_acquire(); CHECK(after!=before); sh_package_runtime_release();
        CHECK(g_install_commit_calls==commits+1 && g_install_cancel_calls==cancels &&
            g_editor_end_calls==ends+1 && g_editor_accepts==accepts && g_editor_rejects==rejects);
        /* Fixture-owned sentinels and wrappers are discarded directly, after
         * asserting the production damage path cannot replay native cleanup. */
        sh_decl_server_test_reset_install();
        g_editor_live=g_shader_resident_live=0; g_editor_context=NULL;
        sh_package_runtime_test_dispose(); heap_fixture_reset();
    }
    test_boot_hook_recovery(results, sizeof(results)/sizeof(results[0]), base, &registry);
    test_deferred_first_registration(results, sizeof(results)/sizeof(results[0]), base, &registry);
    g_rt_builtin_count = 0;
    sh_decl_server_test_reset_install();
    g_user_enabled = 0;
    g_table_published = 0;
    {
        char path[MAX_PATH];
        g_linked_fixture = 0; runtime_fixture();
        sh_package_runtime_test_dispose(); /* Close delete-on-close bundle snapshots first. */
        snprintf(path, sizeof path, "%s/overrides/fixture/package.json", g_fixture_root); CHECK(DeleteFileA(path));
        for (size_t i = sizeof(runtime_dirs)/sizeof(runtime_dirs[0]); i; i--) {
            snprintf(path, sizeof path, "%s/%s", g_fixture_root, runtime_dirs[i - 1]); CHECK(RemoveDirectoryA(path));
        }
        /* The served owner of the inventory steps leaves its asset in the
         * content-addressed resource cache: remove those files and that exact
         * directory. */
        {
            char pattern[MAX_PATH], file[MAX_PATH]; WIN32_FIND_DATAA found; HANDLE search;
            snprintf(pattern, sizeof pattern, "%s/package-cache/resources/*", g_fixture_root);
            search = FindFirstFileA(pattern, &found);
            if (search != INVALID_HANDLE_VALUE) {
                do if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                    snprintf(file, sizeof file, "%s/package-cache/resources/%s", g_fixture_root, found.cFileName);
                    CHECK(DeleteFileA(file));
                } while (FindNextFileA(search, &found));
                FindClose(search);
            }
            snprintf(path, sizeof path, "%s/package-cache/resources", g_fixture_root);
            CHECK(RemoveDirectoryA(path) || GetLastError() == ERROR_PATH_NOT_FOUND || GetLastError() == ERROR_FILE_NOT_FOUND);
        }
        /* The bundle cache leaves these known empty children under this
         * fixture root. Remove only those exact directories, never recurse. */
        snprintf(path, sizeof path, "%s/package-cache/bundles", g_fixture_root);
        CHECK(RemoveDirectoryA(path) || GetLastError() == ERROR_PATH_NOT_FOUND);
        snprintf(path, sizeof path, "%s/package-cache", g_fixture_root);
        CHECK(RemoveDirectoryA(path) || GetLastError() == ERROR_PATH_NOT_FOUND);
    }
    CHECK(RemoveDirectoryA(g_fixture_root));
    g_fixture_root[0] = '\0';
    HeapFree(GetProcessHeap(), 0, decl);
}

static void test_large_snapshot(void)
{
    const char *dirs[] = {"assets", "assets/generated", "assets/generated/decls", "assets/generated/decls/material"};
    char temp[MAX_PATH], root[MAX_PATH], path[MAX_PATH], error[512];
    const char *descriptor = "{\"id\":\"tests.large\",\"name\":\"Large declarations\",\"strings\":{\"en\":{\"message\":\"";
    const size_t string_length = 2u * 1024u * 1024u;
    size_t descriptor_length = strlen(descriptor) + string_length + 4u;
    const size_t length = 17u * 1024u * 1024u;
    unsigned char *body = (unsigned char *)malloc(length + 1u);
    sh_package_sources *sources = NULL;
    sh_package_compilation *compiled = NULL;
    size_t i, count = 0, bytes = 0, made_dirs = 0, made_files = 0;
    int made_descriptor = 0;
    HANDLE file;
    DWORD written;
    CHECK(body); if (!body) return;
    CHECK(GetTempPathA(sizeof(temp), temp)); CHECK(GetTempFileNameA(temp, "dsb", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    snprintf(path, sizeof(path), "%s/package.json", root);
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE); if (file == INVALID_HANDLE_VALUE) goto done;
    made_descriptor = 1;
    memcpy(body, descriptor, strlen(descriptor));
    memset(body + strlen(descriptor), 'x', string_length);
    memcpy(body + strlen(descriptor) + string_length, "\"}}}", 4);
    CHECK(WriteFile(file, body, (DWORD)descriptor_length, &written, NULL) && written == descriptor_length);
    CloseHandle(file);
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, dirs[i]);
        if (!CreateDirectoryA(path, NULL)) { CHECK(0); goto done; }
        made_dirs++;
    }
    memset(body, ' ', length); memcpy(body, "{ /*", 4); memcpy(body + length - 4, "*/ }", 4); body[length] = 0;
    for (i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "%s/assets/generated/decls/material/large%zu.decl", root, i);
        file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(file != INVALID_HANDLE_VALUE); if (file == INVALID_HANDLE_VALUE) goto done;
        made_files++;
        CHECK(WriteFile(file, body, (DWORD)length, &written, NULL) && written == length);
        CloseHandle(file);
    }
    sources = sh_package_sources_scan_directory(root, error, sizeof(error)); CHECK(sources);
    if (!sources) goto done;
    compiled = sh_package_compile(sources, NULL, NULL, error, sizeof(error));
    if (!compiled) fprintf(stderr, "large declaration compilation: %s\n", error);
    CHECK(compiled); if (!compiled) goto done;
    CHECK(compiled->resource_count == 8);
    CHECK(compiled->policy.strings.count == 1);
    for (i = 0; i < compiled->resource_count; i++)
        CHECK(compiled->resources[i].body_length == length && !memcmp(compiled->resources[i].body, body, length));
    CHECK(sh_decl_server_test_capture(compiled, &count, &bytes));
    CHECK(count == 8 && bytes == length * 8u);
    printf("large native snapshot: %zu declarations, %zu exact text bytes\n", count, bytes);
done:
    sh_package_compilation_free(compiled); sh_package_sources_free(sources); free(body);
    for (i = 0; i < made_files; i++) {
        snprintf(path, sizeof(path), "%s/assets/generated/decls/material/large%zu.decl", root, i); CHECK(DeleteFileA(path));
    }
    while (made_dirs) { snprintf(path, sizeof(path), "%s/%s", root, dirs[--made_dirs]); CHECK(RemoveDirectoryA(path)); }
    if (made_descriptor) { snprintf(path, sizeof(path), "%s/package.json", root); CHECK(DeleteFileA(path)); }
    CHECK(RemoveDirectoryA(root));
}

/* Editor-row publication lineage: the linker's exact binding per row, fixed
 * identities, dependency variants, equal-resource sharing, product names never
 * native, positive native proof, bounded records and generation lifetime. */
typedef struct rl_decl { void *vtable; const char *name; } rl_decl;
static rl_decl rl_a = {0, "pkg/row_a"}, rl_v = {0, "pkg/row_v"}, rl_a2 = {0, "pkg/row_a"},
    rl_l = {0, "pkg/row_l"}, rl_d = {0, "pkg/row_d"}, rl_native = {0, "native/row"}, rl_stale = {0, "native/row"},
    rl_p = {0, "public/row"};
static rl_decl *rl_live[8]; static int rl_sources = 1, rl_retired;
static char rl_manager;
static void *rl_peek(void *manager, const char *name, unsigned char make_default)
{
    (void)manager; (void)make_default;
    for (int i = 0; i < 8; i++) if (rl_live[i] && !strcmp(rl_live[i]->name, name)) return rl_live[i];
    return NULL;
}
static void *rl_source(void *manager, const char *name) { (void)manager; (void)name; return rl_sources ? &rl_manager : NULL; }
/* Production mode's native resource index. */
static int rl_index = 1; static char rl_index_path[200];
static int rl_probe(const char *path) { snprintf(rl_index_path, sizeof(rl_index_path), "%s", path); return rl_index; }
static void rl_retire(void) { rl_retired++; }
static void rl_resource(sh_compiled_resource *resource, const char *name, const char *engine, const char *original, const char *body)
{
    memset(resource, 0, sizeof(*resource));
    resource->type = "snapEditorEntityDef"; resource->name = (char *)name; resource->engine_path = (char *)engine;
    resource->original_path = (char *)original; resource->body = (unsigned char *)body; resource->body_length = strlen(body);
    CHECK(sh_package_owners_add(&resource->owners, 0));
}
static void test_row_lineage(void)
{
    static unsigned char fingerprints[2][32], originals[2][32], tree_a[1][32], tree_b[1][32], tree_ad[2][32];
    static char longpath[1500];
    sh_package_sources sources = {0}; sh_package_compilation compiled = {0};
    sh_compiled_resource resources[3];
    sh_package_provenance bindings = {0}, supply = {0};
    char ia[80], iv[80], name[80];
    size_t ca, cb, cad, many[40], records;
    const char *authored = "decls/snapeditorentitydef/start.decl";
    memset(fingerprints[0], 0x11, 32); memset(fingerprints[1], 0x22, 32);
    memset(originals[0], 0xa1, 32); memset(originals[1], 0xb2, 32);
    memset(tree_a, 0xa1, sizeof(tree_a)); memset(tree_b, 0xb2, sizeof(tree_b));
    memset(tree_ad[0], 0xa1, 32); memset(tree_ad[1], 0xd4, 32);
    sources.package_count = 2; sources.fingerprints = fingerprints; sources.original_fingerprints = originals;
    /* Linker bindings: row_a is one resource equal in contexts A and B (shared
     * registration); row_v is the same authored path and tree linked in
     * another dependency context. */
    ca = sh_package_provenance_context_add(&bindings, SH_PACKAGE_CONTEXT_ISOLATED, (const unsigned char (*)[32])tree_a, 1, "pkg/a");
    cb = sh_package_provenance_context_add(&bindings, SH_PACKAGE_CONTEXT_ISOLATED, (const unsigned char (*)[32])tree_b, 1, "pkg/b");
    cad = sh_package_provenance_context_add(&bindings, SH_PACKAGE_CONTEXT_COMPOSED, (const unsigned char (*)[32])tree_ad, 2, NULL);
    CHECK(ca != SIZE_MAX && cb != SIZE_MAX && cad != SIZE_MAX);
    CHECK(sh_package_provenance_bind(&bindings, "snapeditorentitydef", authored, "pkg/row_a", &ca, 1) &&
          sh_package_provenance_bind(&bindings, "snapeditorentitydef", authored, "pkg/row_a", &cb, 1) &&
          sh_package_provenance_bind(&bindings, "snapeditorentitydef", authored, "pkg/row_v", &cad, 1));
    rl_resource(resources + 0, "pkg/row_a", "generated/decls/snapeditorentitydef/pkg/row_a.decl", authored, "{ a }");
    rl_resource(resources + 1, "pkg/row_v", "generated/decls/snapeditorentitydef/pkg/row_v.decl", authored, "{ v }");
    compiled.sources = &sources; compiled.resources = resources; compiled.resource_count = 2;
    sh_decl_server_test_rows_reset(); sh_decl_server_test_rows_bindings(&bindings); sh_decl_server_rows_retired(rl_retire);
    sh_decl_server_test_set_reconstruct(NULL, (sh_decl_server_test_find_decl_fn)rl_peek);
    sh_decl_server_test_rows_manager(&rl_manager, rl_source);
    memset(rl_live, 0, sizeof(rl_live)); rl_live[0] = &rl_a; rl_live[1] = &rl_v; rl_live[2] = &rl_native;
    /* Positive native proof only before and outside product publication. */
    CHECK(sh_decl_server_row_class(&rl_native, NULL, 0) == 0);
    CHECK(sh_decl_server_row_class(&rl_stale, NULL, 0) == -1);   /* not the live entry */
    rl_sources = 0; CHECK(sh_decl_server_row_class(&rl_native, NULL, 0) == -1); rl_sources = 1;
    /* Production mode has no source records for stock rows: the proof is the
     * native resource index for the row's own path, and only a positive one. */
    rl_sources = 0; sh_decl_server_test_rows_production(rl_probe);
    CHECK(sh_decl_server_row_class(&rl_native, NULL, 0) == 0 &&
          !strcmp(rl_index_path, "generated/decls/snapeditorentitydef/native/row.decl"));
    CHECK(sh_decl_server_row_class(&rl_stale, NULL, 0) == -1);
    rl_index = 0; CHECK(sh_decl_server_row_class(&rl_native, NULL, 0) == -1);
    rl_index = -1; rl_sources = 1; CHECK(sh_decl_server_row_class(&rl_native, NULL, 0) == -1);
    rl_index = 1; sh_decl_server_test_rows_production(NULL);
    rl_retired = 0; CHECK(sh_decl_server_test_rows_record(&compiled, 1) && rl_retired == 1);
    /* A published row is never native, whatever the index says. */
    sh_decl_server_test_rows_production(rl_probe);
    CHECK(sh_decl_server_row_class(&rl_a, NULL, 0) == 1 && sh_decl_server_row_class(&rl_v, NULL, 0) == 1);
    sh_decl_server_test_rows_production(NULL);
    /* Fixed-size exact identities; the dependency variant is distinct. */
    CHECK(sh_decl_server_row_class(&rl_a, ia, sizeof(ia)) == 1 && strlen(ia) == 64);
    CHECK(sh_decl_server_row_class(&rl_v, iv, sizeof(iv)) == 1 && strlen(iv) == 64 && strcmp(ia, iv));
    CHECK(sh_decl_server_row_class(&rl_a, name, 64) == -1);       /* never a truncated identity */
    CHECK(sh_decl_server_row_resolve(ia) == &rl_a && sh_decl_server_row_resolve(iv) == &rl_v);
    /* Equal-resource sharing: one binding carrying both linked contexts. */
    CHECK(sh_decl_server_row_supply(&rl_a, &supply) == 1 && supply.binding_count == 1 &&
          supply.bindings[0].context_count == 2 && !strcmp(supply.bindings[0].original, authored));
    sh_package_provenance_free(&supply); memset(&supply, 0, sizeof(supply));
    CHECK(sh_decl_server_row_supply(&rl_v, &supply) == 1 && supply.bindings[0].context_count == 1 &&
          supply.contexts[supply.bindings[0].contexts[0]].mode == SH_PACKAGE_CONTEXT_COMPOSED);
    sh_package_provenance_free(&supply); memset(&supply, 0, sizeof(supply));
    CHECK(sh_decl_server_row_supply(&rl_native, &supply) == 0 && !supply.binding_count);
    CHECK(sh_decl_server_row_class(&rl_native, NULL, 0) == 0);
    /* Only the wrong variant available: the identity never resolves to it. */
    rl_live[0] = NULL; CHECK(!sh_decl_server_row_resolve(ia) && sh_decl_server_row_resolve(iv) == &rl_v);
    /* A reused address carries only its new meaning (here a live native row). */
    rl_live[0] = &rl_a; rl_a.name = "pkg/row_x";
    CHECK(sh_decl_server_row_class(&rl_a, name, sizeof(name)) == 0 && sh_decl_server_row_supply(&rl_a, &supply) == 0);
    rl_a.name = "pkg/row_a";
    /* A reused name at a new unrecorded object: never native, never resolved. */
    rl_live[0] = &rl_a2;
    CHECK(sh_decl_server_row_class(&rl_a2, NULL, 0) == -1 && !sh_decl_server_row_resolve(ia));
    CHECK(sh_decl_server_test_rows_record(&compiled, 1));          /* republished at the new address */
    CHECK(sh_decl_server_row_class(&rl_a2, name, sizeof(name)) == 1 && !strcmp(name, ia));
    CHECK(sh_decl_server_row_resolve(ia) == &rl_a2 && sh_decl_server_row_class(&rl_a, NULL, 0) == -1);
    /* Repeated retirement and republication: one record per name. */
    records = sh_decl_server_test_rows_count(); CHECK(records == 2);
    for (int i = 0; i < 50; i++) CHECK(sh_decl_server_test_rows_record(&compiled, i & 1));
    CHECK(sh_decl_server_test_rows_count() == records && sh_decl_server_row_resolve(ia) == &rl_a2);
    /* A registration that fails over existing names: their meaning ends first. */
    rl_retired = 0; CHECK(sh_decl_server_test_rows_record(&compiled, 0) && rl_retired == 1);
    CHECK(sh_decl_server_row_class(&rl_a2, NULL, 0) == -1 && !sh_decl_server_row_resolve(ia) && !sh_decl_server_row_resolve(iv));
    CHECK(sh_decl_server_row_supply(&rl_a2, &supply) == 0);
    CHECK(sh_decl_server_test_rows_record(&compiled, 1) && sh_decl_server_row_resolve(ia) == &rl_a2);
    /* A valid row beyond the earlier capacity: long path, forty linked contexts. */
    memset(longpath, 'p', sizeof(longpath) - 1);
    for (int i = 0; i < 40; i++) {
        unsigned char tree[1][32]; char component[16];
        memset(tree, i + 1, sizeof(tree)); snprintf(component, sizeof(component), "pkg/m%d", i);
        many[i] = sh_package_provenance_context_add(&bindings, SH_PACKAGE_CONTEXT_ISOLATED, (const unsigned char (*)[32])tree, 1, component);
        CHECK(many[i] != SIZE_MAX);
    }
    CHECK(sh_package_provenance_bind(&bindings, "snapeditorentitydef", longpath, "pkg/row_l", many, 40));
    rl_resource(resources + 2, "pkg/row_l", "generated/decls/snapeditorentitydef/pkg/row_l.decl", longpath, "{ l }");
    compiled.resources = resources + 2; compiled.resource_count = 1; rl_live[3] = &rl_l;
    CHECK(sh_decl_server_test_rows_record(&compiled, 1));
    CHECK(sh_decl_server_row_class(&rl_l, name, sizeof(name)) == 1 && strlen(name) == 64 && sh_decl_server_row_resolve(name) == &rl_l);
    CHECK(sh_decl_server_row_supply(&rl_l, &supply) == 1 && supply.bindings[0].context_count == 40);
    sh_package_provenance_free(&supply); memset(&supply, 0, sizeof(supply));
    sh_package_owners_free(&resources[2].owners);
    /* Changed bytes served at an installed SnapMap original's own path occupy
     * that native slot (2), still an exact publication; the same scope under a
     * private name (the linker's original_path) is an alias, a publication (1);
     * a campaign original or an added path is only a publication (1); exact
     * original bytes are native (0). */
    CHECK(sh_package_provenance_bind(&bindings, "snapeditorentitydef", authored, "public/row", &ca, 1));
    rl_resource(resources + 2, "public/row", authored, NULL, "{ p }");
    resources[2].baseline_known = 1; rl_live[5] = &rl_p; CHECK(sh_decl_server_test_rows_record(&compiled, 1));
    CHECK(sh_decl_server_row_class(&rl_p, name, sizeof(name)) == 2 && strlen(name) == 64 && sh_decl_server_row_resolve(name) == &rl_p);
    CHECK(sh_decl_server_row_supply(&rl_p, &supply) == 1 && supply.binding_count == 1);
    sh_package_provenance_free(&supply); memset(&supply, 0, sizeof(supply));
    sh_package_owners_free(&resources[2].owners);
    rl_resource(resources + 2, "public/row", "generated/decls/snapeditorentitydef/public/row.decl", authored, "{ p }");
    resources[2].baseline_known = 1; CHECK(sh_decl_server_test_rows_record(&compiled, 1));
    CHECK(sh_decl_server_row_class(&rl_p, name, sizeof(name)) == 1 && strlen(name) == 64 && sh_decl_server_row_resolve(name) == &rl_p);
    resources[2].baseline_known = 2; CHECK(sh_decl_server_test_rows_record(&compiled, 1));
    CHECK(sh_decl_server_row_class(&rl_p, NULL, 0) == 1);
    resources[2].baseline_known = 0; CHECK(sh_decl_server_test_rows_record(&compiled, 1));
    CHECK(sh_decl_server_row_class(&rl_p, NULL, 0) == 1);
    resources[2].baseline_known = 1; resources[2].native_original = 1; CHECK(sh_decl_server_test_rows_record(&compiled, 1));
    CHECK(sh_decl_server_row_class(&rl_p, NULL, 0) == 0 && !sh_decl_server_row_resolve(name));
    sh_package_owners_free(&resources[2].owners);
    /* Two current publications of one exact identity: ambiguous. */
    CHECK(sh_package_provenance_bind(&bindings, "snapeditorentitydef", authored, "pkg/row_d", &ca, 1));
    rl_resource(resources + 2, "pkg/row_d", "generated/decls/snapeditorentitydef/pkg/row_d.decl", authored, "{ a }");
    rl_live[4] = &rl_d; compiled.resources = resources; compiled.resource_count = 3;
    CHECK(sh_decl_server_test_rows_record(&compiled, 1));
    CHECK(sh_decl_server_row_class(&rl_d, name, sizeof(name)) == 1 && !strcmp(name, ia) && !sh_decl_server_row_resolve(ia));
    /* A full compilation that no longer holds a name retires it, never
     * current, though the engine keeps its declaration allocated; an additive
     * pass retires nothing it does not compile. */
    compiled.resources = resources + 2; compiled.resource_count = 1; CHECK(sh_decl_server_test_rows_record(&compiled, 1));
    CHECK(sh_decl_server_row_resolve(ia) == &rl_d && sh_decl_server_row_class(&rl_a2, NULL, 0) == -1 &&
          !sh_decl_server_row_named(&rl_a2, ia) && !sh_decl_server_row_exact(&rl_a2, name, sizeof(name)));
    compiled.resources = resources; compiled.resource_count = 3;
    CHECK(sh_decl_server_test_rows_record(&compiled, 1) && !sh_decl_server_row_resolve(ia));
    sh_decl_server_test_set_additive(1); compiled.resources = resources + 2; compiled.resource_count = 1;
    CHECK(sh_decl_server_test_rows_record(&compiled, 1) && !sh_decl_server_row_resolve(ia) && sh_decl_server_row_class(&rl_a2, NULL, 0) == 1);
    sh_decl_server_test_set_additive(0);
    sh_package_owners_free(&resources[0].owners); sh_package_owners_free(&resources[1].owners);
    sh_package_owners_free(&resources[2].owners);
    sh_decl_server_rows_retired(NULL); sh_decl_server_test_rows_bindings(NULL);
    sh_decl_server_test_rows_manager(NULL, NULL); sh_decl_server_test_set_reconstruct(NULL, NULL);
    sh_decl_server_test_rows_reset(); sh_package_provenance_free(&bindings);
}
/* The original-slot pair as production emits it (VK s1, 2026-10-07: rows
 * player/start and smp/093d..., both scope 1). The installed editor package
 * restates a stock role row; the real library links that restatement in the
 * package's own scope under a private name and the linker records the
 * authored path as its original_path. The alias carries the original's scope
 * (1) by copy, so scope alone cannot say which row occupies the native slot:
 * only a resource served at its original's path does. The alias below is the
 * library's own output; the slot control is that same emitted resource served
 * at its authored path, as the shared publication serves player/start. */
static const char *slot_row = "generated/decls/snapeditorentitydef/player/start.decl";
static const char *slot_def = "generated/decls/entitydef/player/coop/player_start.decl";
static int slot_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, slot_row) ?
            "{\n\tedit = {\n\t\tentityDef = \"player/coop/player_start\";\n\t\tplacementFlags = \"SNAP_ENTITY_PLACE_FLOOR\";\n"
            "\t\tcollisionFlags = \"SNAP_ENTITY_ECF_SOLID\";\n\t\tinputs = {\n\t\t\tnum = 0;\n\t\t}\n\t}\n}\n" :
        !strcmp(path, slot_def) ? "{\n\tclass = \"Entity\";\n\tedit = {\n\t\thealth = 100;\n\t}\n}\n" : NULL;
    (void)context; *body = NULL; *length = 0;
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
/* Native registrations for the two typed families the pair reaches (the
 * runtime context test's collection fixture): the library's source readers
 * walk these, never a loaded declaration. */
typedef struct slot_region { void *address; size_t length; } slot_region;
typedef struct slot_fixture { slot_region regions[64]; size_t count; void *registry, *reflection; } slot_fixture;
static void *slot_alloc(slot_fixture *f, size_t length)
{
    void *p = calloc(1, length); CHECK(p && f->count < 64);
    if (p && f->count < 64) f->regions[f->count++] = (slot_region){p, length};
    return p;
}
static void slot_pointer(void *p, size_t at, const void *value)
{ uintptr_t address = (uintptr_t)value; memcpy((char *)p + at, &address, 8); }
static void slot_number(void *p, size_t at, int value) { memcpy((char *)p + at, &value, 4); }
static char *slot_string(slot_fixture *f, const char *text)
{ char *copy = slot_alloc(f, strlen(text) + 1); if (copy) strcpy(copy, text); return copy; }
static int slot_read(void *context, uintptr_t address, void *out, size_t length)
{
    slot_fixture *f = context;
    for (size_t i = 0; i < f->count; i++) {
        uintptr_t start = (uintptr_t)f->regions[i].address;
        if (address >= start && address - start <= f->regions[i].length &&
            length <= f->regions[i].length - (size_t)(address - start)) {
            memcpy(out, (void *)address, length); return 1;
        }
    }
    return 0;
}
static void slot_reflection(slot_fixture *f)
{
    const char *classes[] = {"idDeclEntityDef", "Entity", "idDeclTypeInfo", "int",
        "idDeclSnapEditorEntity", "idList < EntityRefs >", "idDeclTypeInfoGraph"};
    const char *types[] = {"entityDef", "snapEditorEntityDef"};
    const size_t indices[] = {0, 4};
    memset(f, 0, sizeof(*f));
    f->registry = slot_alloc(f, 0x18);
    void *managers = slot_alloc(f, 16);
    slot_pointer(f->registry, 8, managers); slot_number(f->registry, 0x10, 2);
    for (size_t i = 0; i < 2; i++) {
        void *manager = slot_alloc(f, 0x90); slot_pointer(managers, i * 8, manager);
        slot_pointer(manager, 8, slot_string(f, types[i]));
        slot_pointer(manager, 0x10, slot_string(f, classes[indices[i]]));
    }
    f->reflection = slot_alloc(f, 0xb0);
    void *container = slot_alloc(f, 0x30), *records = slot_alloc(f, 8 * 0x38);
    void *objects = slot_alloc(f, 8 * 16), *pointers = slot_alloc(f, 8 * 16);
    slot_pointer(f->reflection, 0, container); slot_pointer(container, 0x20, records);
    slot_pointer(f->reflection, 0x88, objects); slot_number(f->reflection, 0x90, 8);
    slot_pointer(f->reflection, 0xa0, pointers); slot_number(f->reflection, 0xa8, 8);
    for (size_t i = 0; i < 7; i++) {
        slot_pointer(records, i * 0x38, slot_string(f, classes[i]));
        slot_pointer(records, i * 0x38 + 8, slot_string(f, i == 4 || i == 6 ? "idDeclTypeInfo" : ""));
    }
    slot_pointer(pointers, 8, (void *)0x10); slot_pointer(pointers, 4 * 16 + 8, (void *)0x10);
    slot_pointer(objects, 5 * 16 + 8, (void *)0x80);
    slot_pointer(objects, 6 * 16 + 8, (void *)0x30);
    void *entity = slot_alloc(f, 3 * 0x48), *editor = slot_alloc(f, 3 * 0x48);
    void *element = slot_alloc(f, 0x48);
    slot_pointer(records, 0x38 + 0x20, entity); slot_pointer(records, 4 * 0x38 + 0x20, editor);
    slot_pointer(records, 5 * 0x38 + 0x18, element);
    void *rows[] = {entity, (char *)entity + 0x48, editor, element, (char *)editor + 0x48};
    const char *field_types[] = {"idDeclEntityDef", "int", "idList < EntityRefs >", "idDeclEntityDef", "idDeclEntityDef"};
    const char *operators[] = {"*", "", "", "*", "*"}, *names[] = {"target", "health", "inputs", "element", "entityDef"};
    for (size_t i = 0; i < 5; i++) {
        slot_pointer(rows[i], 0, slot_string(f, field_types[i]));
        slot_pointer(rows[i], 8, slot_string(f, operators[i]));
        slot_pointer(rows[i], 0x10, slot_string(f, names[i])); slot_number(rows[i], 0x1c, 8);
    }
}
static void slot_file(const char *root, const char *relative, const char *text)
{
    char path[MAX_PATH], *slash;
    snprintf(path, sizeof(path), "%s/%s", root, relative);
    for (slash = path + strlen(root) + 1; (slash = strchr(slash, '/')) != NULL; slash++) {
        *slash = 0; CreateDirectoryA(path, NULL); *slash = '/';
    }
    inventory_write(path, text);
}
static void test_row_slot_production(void)
{
    char root[MAX_PATH], temp[MAX_PATH];
    const sh_package_compilation *compiled = NULL;
    const sh_compiled_resource *alias = NULL;
    sh_compiled_resource pair[2];
    sh_package_compilation shaped = {0};
    rl_decl public_row = {0, "player/start"}, private_row = {0, NULL};
    char alias_identity[80] = "";
    size_t aliases = 0, at_path = 0;
    slot_fixture fixture;
    sh_decl_registry_source registry;
    GetTempPathA(sizeof(temp), temp);
    snprintf(root, sizeof(root), "%sdecl-server-slot-%lu", temp, GetCurrentProcessId());
    CreateDirectoryA(root, NULL);
    slot_file(root, "overrides/editor/package.json", "{\"id\":\"local.editor\",\"name\":\"editor\"}");
    slot_file(root, "overrides/editor/assets/generated/decls/snapeditorentitydef/player/start.decl",
        "{\n\tedit = {\n\t\tentityDef = \"player/coop/player_start\";\n\t\tplacementFlags = \"SNAP_ENTITY_PLACE_AIR_UNALIGNED\";\n"
        "\t\tinputs = {\n\t\t\tnum = 0;\n\t\t}\n\t}\n}\n");
    slot_reflection(&fixture);
    registry = (sh_decl_registry_source){&fixture, slot_read, (uintptr_t)fixture.registry, NULL};
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(slot_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)fixture.reflection);
    sh_package_runtime_test_installed_links(1);
    if (!sh_package_runtime_refresh(root)) {
        char error[1024]; sh_package_runtime_error(error, sizeof(error));
        fprintf(stderr, "slot pair library: %s\n", error); CHECK(0);
    }
    compiled = sh_package_runtime_acquire();
    for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (!r->type || _stricmp(r->type, "snapeditorentitydef")) continue;
        fprintf(stderr, "slot pair emitted: %s original=%s scope=%d native=%d restored=%d\n", r->engine_path,
            r->original_path ? r->original_path : "-", r->baseline_known, r->native_original, r->restored_original);
        if (!strcmp(r->engine_path, slot_row)) at_path++;
        else if (r->original_path && !strcmp(r->original_path, slot_row)) { alias = r; aliases++; }
    }
    /* The library's own alias: private name, linker-recorded original path,
     * the original's scope, not a native original. */
    CHECK(alias && aliases == 1 && !at_path);
    if (!alias) goto done;
    CHECK(alias->baseline_known == 1 && !alias->native_original && !alias->restored_original &&
          strcmp(alias->engine_path, alias->original_path));
    pair[0] = *alias; pair[1] = *alias;
    pair[1].engine_path = (char *)slot_row; pair[1].original_path = NULL; pair[1].name = "player/start";
    shaped.resources = pair; shaped.resource_count = 2;
    private_row.name = alias->name;
    rl_live[6] = &public_row; rl_live[7] = &private_row;
    sh_decl_server_test_rows_reset(); sh_decl_server_test_rows_bindings(sh_package_runtime_bindings());
    sh_decl_server_test_set_reconstruct(NULL, (sh_decl_server_test_find_decl_fn)rl_peek);
    sh_decl_server_test_rows_manager(&rl_manager, rl_source);
    CHECK(sh_decl_server_test_rows_record(&shaped, 1));
    /* Served at its original's path: the native slot. The alias: an ordinary
     * publication with its own portable identity. */
    {
        int served = sh_decl_server_row_class(&public_row, NULL, 0);
        int linked = sh_decl_server_row_class(&private_row, alias_identity, sizeof(alias_identity));
        fprintf(stderr, "slot pair classes: served-at-path %d, private alias %d (%s)\n", served, linked, alias_identity);
        CHECK(served == 2);
        CHECK(linked == 1 && strlen(alias_identity) == 64 && sh_decl_server_row_resolve(alias_identity) == &private_row);
    }
done:
    if (compiled) sh_package_runtime_release();
    rl_live[6] = rl_live[7] = NULL;
    sh_decl_server_test_rows_bindings(NULL); sh_decl_server_test_rows_manager(NULL, NULL);
    sh_decl_server_test_set_reconstruct(NULL, NULL); sh_decl_server_test_rows_reset();
    sh_package_runtime_test_installed_links(0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(runtime_original, NULL);
    for (size_t i = 0; i < fixture.count; i++) free(fixture.regions[i].address);
    CHECK(inventory_remove_tree(root));
}
/* Portable row identity (VK->GL s1, 2026-10-07: one authored cyberdemon row
 * linked its changed entityDef as smp/78864d54... on Vulkan and smp/9d2ff008...
 * on OpenGL, so OpenGL never resolved the Vulkan record). One authored row
 * compiled three ways: over another stock unit its changed entityDef reaches
 * (no rename: unchanged stock enters by native name), and with another changed
 * entityDef, which the linker renames and the row's linked bytes with it. The
 * authored row keeps one identity; the changed dependency is a different
 * supplier (its trees), never the same choice. */
static int g_port_stock, g_port_variant;
static int port_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text;
    if (strcmp(path, "generated/decls/entitydef/player/coop/base.decl")) return slot_baseline(context, path, body, length);
    text = g_port_stock ? "{\n\tclass = \"Entity\";\n\tedit = {\n\t\thealth = 2;\n\t}\n}\n" :
        "{\n\tclass = \"Entity\";\n\tedit = {\n\t\thealth = 1;\n\t}\n}\n";
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
static int port_compile(int stock, int variant, char identity[80], char entity[160], unsigned char trees[32])
{
    char root[MAX_PATH], temp[MAX_PATH];
    const sh_package_compilation *compiled = NULL;
    const sh_compiled_resource *row = NULL;
    sh_package_compilation shaped = {0};
    sh_package_provenance supply = {0};
    rl_decl private_row = {0, NULL};
    slot_fixture fixture;
    sh_decl_registry_source registry;
    int ok = 0, rows = 0;
    *identity = *entity = 0; memset(trees, 0, 32);
    g_port_stock = stock; g_port_variant = variant;
    GetTempPathA(sizeof(temp), temp);
    snprintf(root, sizeof(root), "%sdecl-server-port-%lu-%d%d", temp, GetCurrentProcessId(), stock, variant);
    CreateDirectoryA(root, NULL);
    slot_file(root, "overrides/editor/package.json", "{\"id\":\"local.editor\",\"name\":\"editor\"}");
    slot_file(root, "overrides/editor/assets/generated/decls/entitydef/player/coop/player_start.decl", variant ?
        "{\n\tclass = \"Entity\";\n\tedit = {\n\t\ttarget = \"player/coop/base\";\n\t\thealth = 151;\n\t}\n}\n" :
        "{\n\tclass = \"Entity\";\n\tedit = {\n\t\ttarget = \"player/coop/base\";\n\t\thealth = 150;\n\t}\n}\n");
    slot_file(root, "overrides/editor/assets/generated/decls/snapeditorentitydef/player/start.decl",
        "{\n\tedit = {\n\t\tentityDef = \"player/coop/player_start\";\n\t\tplacementFlags = \"SNAP_ENTITY_PLACE_AIR_UNALIGNED\";\n"
        "\t\tinputs = {\n\t\t\tnum = 0;\n\t\t}\n\t}\n}\n");
    slot_reflection(&fixture);
    registry = (sh_decl_registry_source){&fixture, slot_read, (uintptr_t)fixture.registry, NULL};
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(port_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)fixture.reflection);
    sh_package_runtime_test_installed_links(1);
    if (!sh_package_runtime_refresh(root)) {
        char error[1024]; sh_package_runtime_error(error, sizeof(error));
        fprintf(stderr, "portable row library: %s\n", error); goto done;
    }
    compiled = sh_package_runtime_acquire();
    for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (!r->type || !r->original_path) continue;
        if (!_stricmp(r->type, "snapeditorentitydef") && !strcmp(r->original_path, slot_row)) { row = r; rows++; }
        else if (!_stricmp(r->type, "entitydef") && !strcmp(r->original_path, slot_def) && r->name)
            snprintf(entity, 160, "%s", r->name);
    }
    if (!row || rows != 1 || !*entity) { fprintf(stderr, "portable row: emitted %d rows, entity '%s'\n", rows, entity); goto done; }
    shaped.sources = compiled->sources; shaped.resources = (sh_compiled_resource *)row; shaped.resource_count = 1;
    private_row.name = row->name; rl_live[7] = &private_row;
    sh_decl_server_test_rows_reset(); sh_decl_server_test_rows_bindings(sh_package_runtime_bindings());
    sh_decl_server_test_set_reconstruct(NULL, (sh_decl_server_test_find_decl_fn)rl_peek);
    sh_decl_server_test_rows_manager(&rl_manager, rl_source);
    if (sh_decl_server_test_rows_record(&shaped, 1) &&
        sh_decl_server_row_class(&private_row, identity, 80) == 1 &&
        sh_decl_server_row_supply(&private_row, &supply) == 1 && supply.binding_count == 1 &&
        supply.bindings[0].context_count == 1 && supply.contexts[supply.bindings[0].contexts[0]].tree_count == 1) {
        memcpy(trees, supply.contexts[supply.bindings[0].contexts[0]].trees[0], 32);
        ok = sh_decl_server_row_resolve(identity) == &private_row;
    }
    fprintf(stderr, "portable row stock=%d variant=%d: row %s entity %s identity %s\n",
        stock, variant, row->name, entity, identity);
done:
    sh_package_provenance_free(&supply);
    if (compiled) sh_package_runtime_release();
    rl_live[7] = NULL;
    sh_decl_server_test_rows_bindings(NULL); sh_decl_server_test_rows_manager(NULL, NULL);
    sh_decl_server_test_set_reconstruct(NULL, NULL); sh_decl_server_test_rows_reset();
    sh_package_runtime_test_installed_links(0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(runtime_original, NULL);
    for (size_t i = 0; i < fixture.count; i++) free(fixture.regions[i].address);
    CHECK(inventory_remove_tree(root));
    return ok;
}
/* A context of supply naming tree (an owner's selected supplier). */
static int supplied_by(const sh_package_provenance *supply, const unsigned char tree[32])
{
    for (size_t b = 0; b < supply->binding_count; b++)
        for (size_t c = 0; c < supply->bindings[b].context_count; c++) {
            const sh_package_provenance_context *context = supply->contexts + supply->bindings[b].contexts[c];
            for (size_t t = 0; t < context->tree_count; t++) if (!memcmp(context->trees[t], tree, 32)) return 1;
        }
    return 0;
}
/* One library, three packages publishing the same authored row. A and C are
 * byte-equal with equal dependencies; B changes only a dependency (the entity
 * its entityDef targets), so the linker keeps B's row as a separate variant.
 * The row's identity cannot tell them apart; only the supplier does. */
static void test_row_supplier_variants(void)
{
    static const char *const names[3] = {"a", "b", "c"};
    char root[MAX_PATH], temp[MAX_PATH], identity[2][80] = {{0}};
    const sh_package_compilation *compiled = NULL;
    sh_compiled_resource rows[2];
    sh_package_compilation shaped = {0};
    sh_package_provenance supply[2] = {{0}};
    rl_decl live[2] = {{0, NULL}, {0, NULL}};
    slot_fixture fixture;
    sh_decl_registry_source registry;
    size_t found = 0, shared, variant;
    g_port_stock = g_port_variant = 0;
    GetTempPathA(sizeof(temp), temp);
    snprintf(root, sizeof(root), "%sdecl-server-variants-%lu", temp, GetCurrentProcessId());
    CreateDirectoryA(root, NULL);
    for (int p = 0; p < 3; p++) {
        char path[MAX_PATH], text[256];
        snprintf(path, sizeof(path), "overrides/%s/package.json", names[p]);
        snprintf(text, sizeof(text), "{\"id\":\"local.%s\",\"name\":\"%s\"}", names[p], names[p]);
        slot_file(root, path, text);
        snprintf(path, sizeof(path), "overrides/%s/assets/%s", names[p], slot_def);
        slot_file(root, path, "{\n\tclass = \"Entity\";\n\tedit = {\n\t\ttarget = \"player/coop/base\";\n\t\thealth = 150;\n\t}\n}\n");
        if (p == 1) {
            snprintf(path, sizeof(path), "overrides/%s/assets/generated/decls/entitydef/player/coop/base.decl", names[p]);
            slot_file(root, path, "{\n\tclass = \"Entity\";\n\tedit = {\n\t\thealth = 7;\n\t}\n}\n");
        }
        snprintf(path, sizeof(path), "overrides/%s/assets/%s", names[p], slot_row);
        slot_file(root, path, "{\n\tedit = {\n\t\tentityDef = \"player/coop/player_start\";\n"
            "\t\tplacementFlags = \"SNAP_ENTITY_PLACE_AIR_UNALIGNED\";\n\t\tinputs = {\n\t\t\tnum = 0;\n\t\t}\n\t}\n}\n");
    }
    slot_reflection(&fixture);
    registry = (sh_decl_registry_source){&fixture, slot_read, (uintptr_t)fixture.registry, NULL};
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(port_baseline, NULL);
    sh_package_runtime_test_source_readers(registry, (uintptr_t)fixture.reflection);
    sh_package_runtime_test_installed_links(1);
    if (!sh_package_runtime_refresh(root)) {
        char error[1024]; sh_package_runtime_error(error, sizeof(error));
        fprintf(stderr, "row variants library: %s\n", error); CHECK(0); goto done;
    }
    compiled = sh_package_runtime_acquire();
    for (size_t i = 0; compiled && i < compiled->resource_count; i++) {
        const sh_compiled_resource *r = compiled->resources + i;
        if (r->type && r->original_path && !_stricmp(r->type, "snapeditorentitydef") &&
            !strcmp(r->original_path, slot_row) && found++ < 2) rows[found - 1] = *r;
    }
    CHECK(found == 2); if (found != 2) goto done;
    shaped.sources = compiled->sources; shaped.resources = rows; shaped.resource_count = 2;
    live[0].name = rows[0].name; live[1].name = rows[1].name; rl_live[6] = &live[0]; rl_live[7] = &live[1];
    sh_decl_server_test_rows_reset(); sh_decl_server_test_rows_bindings(sh_package_runtime_bindings());
    sh_decl_server_test_set_reconstruct(NULL, (sh_decl_server_test_find_decl_fn)rl_peek);
    sh_decl_server_test_rows_manager(&rl_manager, rl_source);
    CHECK(sh_decl_server_test_rows_record(&shaped, 2));
    for (int i = 0; i < 2; i++) {
        CHECK(sh_decl_server_row_class(&live[i], identity[i], sizeof(identity[i])) > 0);
        CHECK(sh_decl_server_row_supply(&live[i], &supply[i]) == 1 && supply[i].binding_count == 1);
    }
    fprintf(stderr, "row variants: %s %s / %s %s\n", rows[0].name, identity[0], rows[1].name, identity[1]);
    /* The linker kept two publications whose linked bodies differ; the authored
     * row is one identity, so no name alone tells them apart. */
    CHECK(strcmp(rows[0].name, rows[1].name) && strlen(identity[0]) == 64 && !strcmp(identity[0], identity[1]));
    CHECK(!sh_decl_server_row_resolve(identity[0]));
    shared = supply[0].binding_count == 1 && supply[0].bindings[0].context_count == 2 ? 0 : 1; variant = 1 - shared;
    CHECK(supply[shared].binding_count == 1 && supply[shared].bindings[0].context_count == 2);
    CHECK(supply[variant].binding_count == 1 && supply[variant].bindings[0].context_count == 1);
    if (supply[shared].binding_count == 1 && supply[shared].bindings[0].context_count == 2 &&
        supply[variant].binding_count == 1 && supply[variant].bindings[0].context_count == 1) {
        const sh_package_provenance_context *a = supply[shared].contexts + supply[shared].bindings[0].contexts[0];
        const sh_package_provenance_context *c = supply[shared].contexts + supply[shared].bindings[0].contexts[1];
        const sh_package_provenance_context *b = supply[variant].contexts + supply[variant].bindings[0].contexts[0];
        CHECK(a->tree_count == 1 && b->tree_count == 1 && c->tree_count == 1);
        /* Equal resources: one decl, two owners whose selected suppliers stay distinct. */
        CHECK(memcmp(a->trees[0], c->trees[0], 32));
        CHECK(supplied_by(&supply[shared], a->trees[0]) && supplied_by(&supply[shared], c->trees[0]));
        /* The exact supplier supplies its own variant and only it. */
        CHECK(supplied_by(&supply[variant], b->trees[0]) && !supplied_by(&supply[shared], b->trees[0]));
        CHECK(!supplied_by(&supply[variant], a->trees[0]) && !supplied_by(&supply[variant], c->trees[0]));
        /* Wrong variant only: with B's publication gone the identity resolves
         * uniquely to the A/C row, which B's selected supplier does not supply. */
        rl_live[6 + (int)variant] = NULL;
        CHECK(sh_decl_server_row_resolve(identity[0]) == &live[shared]);
        CHECK(!supplied_by(&supply[shared], b->trees[0]));
    }
done:
    for (int i = 0; i < 2; i++) sh_package_provenance_free(&supply[i]);
    if (compiled) sh_package_runtime_release();
    rl_live[6] = rl_live[7] = NULL;
    sh_decl_server_test_rows_bindings(NULL); sh_decl_server_test_rows_manager(NULL, NULL);
    sh_decl_server_test_set_reconstruct(NULL, NULL); sh_decl_server_test_rows_reset();
    sh_package_runtime_test_installed_links(0); sh_package_runtime_test_dispose();
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_baseline(runtime_original, NULL);
    for (size_t i = 0; i < fixture.count; i++) free(fixture.regions[i].address);
    CHECK(inventory_remove_tree(root));
}
static void test_row_portable_identity(void)
{
    char first[80], stock[80], variant[80], first_entity[160], stock_entity[160], variant_entity[160];
    unsigned char first_trees[32], stock_trees[32], variant_trees[32];
    CHECK(port_compile(0, 0, first, first_entity, first_trees));
    CHECK(port_compile(1, 0, stock, stock_entity, stock_trees));
    CHECK(port_compile(0, 1, variant, variant_entity, variant_trees));
    /* An unchanged stock unit's bytes rename nothing. */
    CHECK(strlen(first) == 64 && !strcmp(first, stock) && !strcmp(first_entity, stock_entity));
    CHECK(!memcmp(first_trees, stock_trees, 32));
    /* Premise: the linker renamed the changed entityDef the row names. */
    CHECK(*first_entity && *variant_entity && strcmp(first_entity, variant_entity));
    /* The authored row is the same bytes at the same original: one portable
     * identity, whatever its dependencies were privately named. */
    CHECK(strlen(variant) == 64 && !strcmp(first, variant));
    /* The changed dependency is another supplier, never the same choice: the
     * owner's selected context distinguishes it (package_editor). */
    CHECK(memcmp(first_trees, variant_trees, 32));
}
static void test_many_candidates(void)
{
    const size_t total = 4097;
    sh_package_compilation compiled = {0};
    sh_compiled_resource *resources = (sh_compiled_resource *)calloc(total, sizeof(*resources));
    char (*names)[32] = (char (*)[32])calloc(total, sizeof(*names));
    char (*paths)[128] = (char (*)[128])calloc(total, sizeof(*paths));
    size_t i, count = 0, bytes = 0;
    CHECK(resources && names && paths); if (!resources || !names || !paths) goto done;
    compiled.resources = resources; compiled.resource_count = total;
    for (i = 0; i < total; i++) {
        snprintf(names[i], sizeof(names[i]), "many_%zu", i);
        snprintf(paths[i], sizeof(paths[i]), "generated/decls/material/%s.decl", names[i]);
        resources[i].name = names[i]; resources[i].engine_path = paths[i]; resources[i].type = "material";
        resources[i].body = (unsigned char *)"{ }"; resources[i].body_length = 3;
    }
    CHECK(sh_decl_server_test_capture(&compiled, &count, &bytes));
    CHECK(count == total && bytes == total * 3u);
    resources[total - 1].body = (unsigned char *)"{ x";
    CHECK(!sh_decl_server_test_capture(&compiled, &count, &bytes)); CHECK(count == 0 && bytes == 0);
    compiled.resource_count = SIZE_MAX;
    CHECK(!sh_decl_server_test_capture(&compiled, &count, &bytes)); CHECK(count == 0 && bytes == 0);
    {
        /* An effect's reader lets end of file close its outermost brace, as
         * stock fists_1st does; other families and deeper gaps still refuse. */
        static const char open[] = "{\r\n\tedit = {\r\n\t\teditEvents = {\r\n\t\t\tnum = 0;\r\n\t}\r\n}";
        sh_compiled_resource effect = {0};
        sh_package_compilation one = {0}; one.resources = &effect; one.resource_count = 1;
        effect.name = "fx/weapons/fists/fists_1st"; effect.type = "fx";
        effect.engine_path = "generated/decls/fx/fx/weapons/fists/fists_1st.decl";
        effect.body = (unsigned char *)open; effect.body_length = sizeof(open) - 1;
        CHECK(sh_decl_server_test_capture(&one, &count, &bytes)); CHECK(count == 1 && bytes == sizeof(open) - 1);
        effect.type = "material";
        CHECK(!sh_decl_server_test_capture(&one, &count, &bytes));
        effect.type = "fx"; effect.body = (unsigned char *)"{ edit = { a = {"; effect.body_length = 16;
        CHECK(!sh_decl_server_test_capture(&one, &count, &bytes));
    }
done:
    free(resources); free(names); free(paths);
}

#define MANY_RUNTIME_DECLS 4097u
static unsigned char (*g_many_runtime)[0x300];
static size_t g_many_reconstructed, g_many_loaded;
static size_t g_many_undrained = SIZE_MAX;

static void *many_peek(void *manager, const char *name, unsigned char make_default)
{
    size_t index = SIZE_MAX;
    (void)manager; (void)make_default;
    if (sscanf_s(name, "many_%zu", &index) != 1 || index >= MANY_RUNTIME_DECLS) return NULL;
    return g_many_runtime[index];
}

static void many_reconstruct(void *decl)
{
    unsigned char *bytes = (unsigned char *)decl;
    CHECK(*(unsigned *)(bytes + 0x28) == 2u);
    CHECK(!(bytes[0x2c] & 2u));
    *(unsigned *)(bytes + 0x28) = 1u;
    bytes[0x2c] = 0;
    g_many_reconstructed++;
}

static void *many_find(void *manager, const char *name, unsigned char make_default)
{
    unsigned char *decl = (unsigned char *)many_peek(manager, name, make_default);
    size_t i;
    if (!decl) return NULL;
    if (decl[0x2c] & 2u) {
        CHECK(g_many_reconstructed == MANY_RUNTIME_DECLS);
        if (!g_many_loaded) for (i = 0; i < MANY_RUNTIME_DECLS; i++) {
            CHECK((g_many_runtime[i][0x2c] & 2u) != 0);
            CHECK(*(unsigned *)(g_many_runtime[i] + 0x28) == 4u);
        }
        if (g_many_undrained < MANY_RUNTIME_DECLS && decl == g_many_runtime[g_many_undrained]) return decl;
        decl[0x2c] = 0; g_many_loaded++;
    }
    return decl;
}

static void test_many_runtime_marks(void)
{
    sh_decl_server_test_materialize_item *items = (sh_decl_server_test_materialize_item *)calloc(MANY_RUNTIME_DECLS, sizeof(*items));
    char (*names)[32] = (char (*)[32])calloc(MANY_RUNTIME_DECLS, sizeof(*names));
    size_t i;
    int materialized = -1;
    long marked, left, shadow, faults;
    g_many_runtime = (unsigned char (*)[0x300])calloc(MANY_RUNTIME_DECLS, sizeof(*g_many_runtime));
    CHECK(items && names && g_many_runtime); if (!items || !names || !g_many_runtime) goto done;
    for (i = 0; i < MANY_RUNTIME_DECLS; i++) {
        snprintf(names[i], sizeof(names[i]), "many_%zu", i);
        items[i].type = "material"; items[i].name = names[i]; items[i].source = names[i];
        items[i].outcome = SH_DECL_SERVER_TEST_SHADOWED;
        items[i].shadow_kind = SH_DECL_SERVER_TEST_SHADOW_LIVE;
        *(unsigned *)(g_many_runtime[i] + 0x28) = 4u;
    }
    sh_decl_server_test_reset_runtime_state();
    sh_decl_server_test_set_reconstruct(many_reconstruct, many_peek);
    sh_decl_server_test_set_runtime(1);
    sh_decl_server_test_fail_mark_allocation(1);
    CHECK(!sh_decl_server_test_materialize_missing_sedefs(items, MANY_RUNTIME_DECLS,
        (void *)1, rt_type_by_name, rt_source_find, many_find, &materialized));
    CHECK(g_many_reconstructed == 0 && g_many_loaded == 0);
    for (i = 0; i < MANY_RUNTIME_DECLS; i++) {
        CHECK(g_many_runtime[i][0x2c] == 0);
        CHECK(*(unsigned *)(g_many_runtime[i] + 0x28) == 4u);
    }
    CHECK(sh_decl_server_test_clear_stray_pending() == 0);
    sh_decl_server_test_reset_runtime_state();
    CHECK(sh_decl_server_test_materialize_missing_sedefs(items, MANY_RUNTIME_DECLS,
        (void *)1, rt_type_by_name, rt_source_find, many_find, &materialized));
    CHECK(g_many_reconstructed == MANY_RUNTIME_DECLS && g_many_loaded == MANY_RUNTIME_DECLS);
    sh_decl_server_test_runtime_counters(&marked, &left, &shadow, &faults);
    CHECK(marked == 0 && left == 0 && shadow == MANY_RUNTIME_DECLS && faults == 0);
    CHECK(sh_decl_server_test_clear_stray_pending() == 0);
    sh_decl_server_test_reset_runtime_state();
    g_many_reconstructed = g_many_loaded = 0;
    g_many_undrained = MANY_RUNTIME_DECLS - 1u;
    CHECK(!sh_decl_server_test_materialize_missing_sedefs(items, MANY_RUNTIME_DECLS,
        (void *)1, rt_type_by_name, rt_source_find, many_find, &materialized));
    CHECK(g_many_reconstructed == MANY_RUNTIME_DECLS && g_many_loaded == MANY_RUNTIME_DECLS - 1u);
    CHECK(sh_decl_server_test_clear_stray_pending() == 1);
    CHECK(g_many_runtime[g_many_undrained][0x2c] == 0);
    sh_decl_server_test_set_runtime(0);
    sh_decl_server_test_reset_runtime_state();
    sh_decl_server_test_set_reconstruct(NULL, NULL);
done:
    free(items); free(names); free(g_many_runtime); g_many_runtime = NULL;
    g_many_undrained = SIZE_MAX;
}

static int shader_original(void *context, const char *path, unsigned char **body, size_t *length)
{
    (void)context; *body = NULL; *length = 0;
    if (strcmp(path, "generated/decls/renderparm/tex.decl")) return 0;
    *body = (unsigned char *)_strdup(g_shader_source); *length = strlen(g_shader_source); return *body ? 1 : -1;
}
static void *shader_find(void *manager, const char *name, unsigned char make_default)
{
    (void)make_default;
    CHECK(manager == g_shader_native.manager && !strcmp(name, "tex"));
    CHECK(g_shader_scope_live); return g_shader_native.object;
}
static void test_shader_preparation_lifetime(void)
{
    char error[512]; uint32_t id = 0;
    memset(&g_shader_native, 0, sizeof(g_shader_native));
    strcpy(g_shader_native.type, "renderParm"); strcpy(g_shader_native.name, "tex");
    *(void **)g_shader_native.head = g_shader_native.manager;
    *(void **)(g_shader_native.manager + 8) = g_shader_native.type;
    *(void **)(g_shader_native.manager + 0x20) = g_shader_native.items;
    *(int *)(g_shader_native.manager + 0x28) = 1;
    *(void **)g_shader_native.items = g_shader_native.object;
    *(void **)(g_shader_native.object + 8) = g_shader_native.name;
    *(void **)(g_shader_native.object + 0x10) = g_shader_native.manager;
    CHECK(sh_native_resource_id(g_shader_native.type, g_shader_native.name, &id));
    *(uint32_t *)(g_shader_native.object + 0x20) = id;
    *(uint32_t *)(g_shader_native.object + 0x68) = 0x410;
    *(uint32_t *)(g_shader_native.object + 0x6c) = 1;
    for (int mode = 0; mode < 6; mode++) {
        int kind = 0; uint32_t index = 0;
        sh_decl_server_test_reset_install();
        g_shader_fixture_active = 1; g_shader_mode = mode; g_shader_hold_end = mode >= 4;
        g_shader_restores = 0; g_shader_event_count = 0; g_shader_events[0] = 0;
        sh_package_shader_receipt_invalidate((uintptr_t)g_shader_native.object);
        sh_package_native_assets *assets = sh_package_native_assets_open("C:/shader-decl-server-fixture");
        CHECK(assets && sh_package_native_assets_configure_shader(assets, SH_PACKAGE_SHADER_OPENGL,
            (sh_native_resource_registry_source){NULL, shader_native_read, (uintptr_t)g_shader_native.head},
            shader_original, NULL, error, sizeof(error)));
        sh_package_library_native native = sh_package_native_assets_adapter(assets);
        const sh_package_source_shader *shader = native.source.shader;
        /* 2: the preparation request is retained for the batch below. */
        CHECK(shader->parameter(shader->context, "tex", (sh_decl_source){g_shader_source, strlen(g_shader_source)},
            &kind, &index, error, sizeof(error)) == 2);
        sh_package_shader_preparation *batch = sh_package_native_assets_shader_take(assets);
        CHECK(batch && sh_package_shader_preparation_count(batch) == 1);
        int result = sh_decl_server_test_prepare_shaders(batch, shader_find, error, sizeof(error));
        CHECK(result == (mode == 0 ? 1 : mode >= 4 ? -1 : 0));
        if (mode == 0) CHECK(!*error);
        if (mode == 1) CHECK(strstr(error, "fixture shader source failed") && strstr(error, "prior provider recovered"));
        if (mode == 2) CHECK(strstr(error, "native shader state cleanup failed"));
        if (mode == 3) CHECK(strstr(error, "native shader construction raised exception"));
        if (mode >= 4) CHECK(strstr(error, "fixture restoration remains owed"));
        if (mode == 5) CHECK(strstr(error, "fixture shader source failed") && strstr(error, "prior provider recovered"));
        CHECK(!strcmp(g_shader_events, mode == 5 ? "RBPTE" : mode == 4 ? "RBPE" : mode == 1 || mode == 3 ? "RBPTEX" : "RBPEX"));
        CHECK(g_shader_restores == ((mode == 1 || mode == 3 || mode == 5) ? 1 : 0));
        sh_package_shader_preparation_free(batch); sh_package_native_assets_close(assets);
        if (mode >= 4) {
            CHECK(g_shader_scope_live && g_shader_resident_live);
            /* Borrowed batch/stack is gone; the native state callback still
             * owns a valid context until final restoration and scope exit. */
            g_shader_hold_end = 0;
            CHECK(sh_decl_server_test_shader_recovery());
            CHECK(!strcmp(g_shader_events, mode == 5 ? "RBPTEEX" : "RBPEEX"));
        }
        CHECK(!g_shader_scope_live && !g_shader_resident_live);
        g_shader_fixture_active = 0;
        sh_package_shader_receipt_invalidate((uintptr_t)g_shader_native.object);
        sh_decl_server_test_reset_install();
    }
}

typedef struct occurrence_fixture { int mode, prepared, finalized, rolled_back; } occurrence_fixture;
static int occurrence_prepare(void *context, char *error, size_t capacity)
{
    occurrence_fixture *fixture = context;
    CHECK(g_shader_resident_live); fixture->prepared++;
    if (fixture->mode == 1) { snprintf(error, capacity, "fixture stage refusal"); return 0; }
    return 1;
}
static int occurrence_finalize(void *context, char *error, size_t capacity)
{
    occurrence_fixture *fixture = context;
    CHECK(!g_shader_resident_live); fixture->finalized++;
    if (fixture->mode >= 2) { snprintf(error, capacity, "fixture commit refusal"); return 0; }
    return 1;
}
static int occurrence_rollback(void *context, char *error, size_t capacity)
{
    occurrence_fixture *fixture = context;
    CHECK(g_shader_resident_live); fixture->rolled_back++;
    if (fixture->mode == 4 && fixture->rolled_back == 1) RaiseException(0xe0340002, 0, 0, NULL);
    if (fixture->mode == 3 && fixture->rolled_back == 1) {
        snprintf(error, capacity, "fixture retained teardown"); return 0;
    }
    return 1;
}
static void test_occurrence_admission(void)
{
    unsigned char manager[0x30] = {0}, resource[0x90] = {0};
    void *objects[] = {resource}; char error[1024];
    *(void ***)(manager + 0x20) = objects; *(int *)(manager + 0x28) = 1;
    {
        unsigned char decl[0x1d0] = {0}, entity[0x60] = {0};
        *(void **)(decl + 0x1c8) = entity;
        /* The materialized entityDef is a resource, not the editor body's
         * independently allocated defsub. Its +0x58 is not a name. */
        *(const char **)(entity + 8) = "palette/entity-name";
        CHECK(sh_decl_server_test_occurrence_source_name(decl) &&
            !strcmp(sh_decl_server_test_occurrence_source_name(decl), "palette/entity-name"));
        *(const char **)(entity + 8) = NULL;
        *(const char **)(entity + 0x58) = "unrelated-field";
        CHECK(!sh_decl_server_test_occurrence_source_name(decl));
    }
    for (int mode = 0; mode < 5; mode++) {
        sh_decl_server_test_reset_install();
        g_occurrence_frame = g_user_enabled = 1;
        g_boundary_thread = GetCurrentThreadId(); g_boundary_editor[9] = 1;
        g_boundary_resource_head = manager;
        g_resident_filter = 1; g_resident_only = NULL;
        sh_decl_server_test_occurrence_ready(g_boundary_editor);
        occurrence_fixture fixture = {mode};
        sh_decl_server_occurrence_hooks hooks = {&fixture, occurrence_prepare, occurrence_finalize, occurrence_rollback};
        int result = sh_decl_server_admit_occurrences(NULL, NULL, &hooks, error, sizeof(error));
        CHECK(result == (mode ? SH_PACKAGE_ADMISSION_FAILED : SH_PACKAGE_ADMISSION_READY));
        CHECK(fixture.prepared == 1 && fixture.finalized == (mode != 1));
        CHECK(fixture.rolled_back == (mode != 0));
        CHECK(!g_shader_resident_live);
        CHECK(sh_decl_server_occurrence_recovery_pending(&fixture) == (mode >= 3));
        if (mode >= 3) {
            sh_decl_server_occurrence_maintenance();
            CHECK(!sh_decl_server_occurrence_recovery_pending(&fixture) && fixture.rolled_back == 2);
            CHECK(sh_decl_server_registration_succeeded());
        }
        g_occurrence_frame = 0; g_boundary_editor[9] = 0;
        /* Retained successful batches leave with the browser; they never
         * invoke a caller hook after its completed request is released. */
        *(void **)(g_boundary_shell + 0x18) = g_boundary_manager;
        *(int *)(g_boundary_manager + 8) = *(int *)(g_boundary_manager + 12) = 63;
        *(int *)(g_boundary_manager + 0x910) = *(int *)(g_boundary_manager + 0x914) = 1;
        sh_decl_server_occurrence_maintenance();
    }
    /* A live accepted palette must not block the native rebuild that will
     * replace it. Only unsafe teardown failure blocks the rearm poll. */
    CHECK(sh_decl_server_test_palette_hold((void *)1));
    g_palette_end_result = 0; g_palette_end_calls = 0;
    CHECK(sh_decl_server_test_palette_maintenance() == 1);
    CHECK(g_palette_end_calls == 1 && sh_decl_server_test_palette_holds() == 1);
    g_palette_end_result = -1;
    CHECK(sh_decl_server_test_palette_maintenance() == 0);
    CHECK(g_palette_end_calls == 2 && sh_decl_server_test_palette_holds() == 1);
    g_palette_end_result = 1;
    CHECK(sh_decl_server_test_palette_maintenance() == 1);
    CHECK(g_palette_end_calls == 3 && sh_decl_server_test_palette_holds() == 0);
    g_resident_filter = 0; g_boundary_resource_head = NULL;
    sh_decl_server_test_reset_install();
}
/* Model the independently verified native cold/pending load distinction.
 * Two children can observe their parent's early fields through a pointer
 * cycle. Final inheritance must also receive its late fields; suppressing an
 * in-progress error alone would leave both children and a descendant wrong. */
static const char *const cycle_names[] = {
    "cycle/base", "cycle/edge", "cycle/child1", "cycle/child2", "cycle/grand"
};
static unsigned char cycle_objects[5][0x100];
static int cycle_present[5], cycle_reads[5], cycle_constructed, cycle_faults;
static int cycle_fail_create, cycle_fail_finish, cycle_first_read_constructed, cycle_late;
static void *cycle_vtable[4], *cycle_manager;
static int cycle_index(const char *name)
{ for (int i=0;i<5;i++) if (!strcmp(name,cycle_names[i])) return i; return -1; }
static void *cycle_type(void *registry,const char *name)
{ (void)registry;(void)name;return &cycle_manager; }
static void *cycle_peek(void *manager,const char *name,unsigned char make_default)
{ (void)manager;(void)make_default;int i=cycle_index(name);return i>=0 && cycle_present[i] ? cycle_objects[i] : NULL; }
static void *cycle_source(void *manager,const char *name)
{ (void)manager;return cycle_index(name)>=0 ? (void *)1 : NULL; }
static void *cycle_create(void *manager,const char *name)
{
    (void)manager;int i=cycle_index(name);CHECK(i>=0);
    if(i==cycle_fail_create)return NULL;
    unsigned char *o=cycle_objects[i];memset(o,0,0x100);
    *(const char **)(o+8)=cycle_names[i];cycle_constructed++;return o;
}
static void cycle_add(void *manager,void *object)
{
    unsigned char *o=object;int i=cycle_index(*(const char **)(o+8));CHECK(i>=0 && !cycle_present[i]);
    cycle_present[i]=1;*(void **)(o+0x10)=manager;*(unsigned *)(o+0x20)=(unsigned)i+1;
}
static void cycle_read(void *object);
static void *cycle_find(void *manager,const char *name,unsigned char make_default)
{
    int i=cycle_index(name);if(i<0)return NULL;
    unsigned char *o=cycle_objects[i];
    if(!cycle_present[i]) {
        if(!make_default || !cycle_create(manager,name))return NULL;
        cycle_add(manager,o);o[0x2c]|=1;cycle_read(o);o[0x2c]&=(unsigned char)~1;
    } else if(o[0x2c]&2) { o[0x2c]&=(unsigned char)~2;cycle_read(o); }
    return o;
}
static void cycle_read(void *object)
{
    unsigned char *o=object;int i=cycle_index(*(const char **)(o+8));
    if(cycle_first_read_constructed<0)cycle_first_read_constructed=cycle_constructed;
    cycle_reads[i]++;
    if(i==0) {
        *(int *)(o+0x50)=17;
        CHECK(cycle_find(&cycle_manager,cycle_names[1],1)!=NULL);
        *(int *)(o+0x54)=cycle_late;
    } else if(i==1) {
        CHECK(cycle_find(&cycle_manager,cycle_names[2],1)!=NULL);
        CHECK(cycle_find(&cycle_manager,cycle_names[3],1)!=NULL);
    } else {
        unsigned char *parent=cycle_find(&cycle_manager,cycle_names[i==4 ? 2 : 0],1);
        CHECK(parent!=NULL);
        if(parent[0x2c]&1) { cycle_faults++;o[0x2c]|=4;return; }
        memcpy(o+0x50,parent+0x50,8);
        if(cycle_fail_finish && i==2 && cycle_reads[i]>1)o[0x2c]|=4;
    }
}
static void cycle_reconstruct(void *object)
{
    unsigned char *o=object;
    CHECK(*(unsigned *)(o+0x28)!=4);memset(o+0x48,0,0x100-0x48);o[0x2c]=0;
}
static void test_native_cycle_construction(void)
{
    static const unsigned char base[]="{ edit = { pointer = \"cycle/edge\"; early = 17; late = 83; } }";
    static const unsigned char edge[]="{ a = \"cycle/child1\"; b = \"cycle/child2\"; }";
    static const unsigned char child[]="{ inherit = \"cycle/base\"; }";
    static const unsigned char grand[]="{ inherit = \"cycle/child1\"; }";
    const unsigned char *bodies[]={base,edge,child,child,grand};
    sh_decl_reference_item order[5]={0};
    for(int i=0;i<5;i++)order[i]=(sh_decl_reference_item){cycle_names[i],bodies[i],strlen((const char *)bodies[i]),(void *)(uintptr_t)i,"perks"};
    size_t edges,cycles;
    CHECK(sh_decl_text_order_by_references(order,5,&edges,&cycles));CHECK(cycles==4);
    sh_decl_server_test_materialize_item items[5]={0};
    for(int i=0;i<5;i++) {
        items[i]=(sh_decl_server_test_materialize_item){"perks",order[i].name,order[i].name,
            SH_DECL_SERVER_TEST_MISSING,order[i].text,order[i].text_length,0,0,order[i].reserve_cycle,order[i].inherit_parent};
        CHECK(order[i].reserve_cycle==(strcmp(order[i].name,"cycle/grand")!=0));
        CHECK(!order[i].inherit_parent || order[i].inherit_parent<=(size_t)i);
    }
    for(int mode=0;mode<3;mode++) {
        memset(cycle_objects,0,sizeof(cycle_objects));memset(cycle_present,0,sizeof(cycle_present));
        memset(cycle_reads,0,sizeof(cycle_reads));cycle_constructed=cycle_faults=0;cycle_first_read_constructed=-1;
        cycle_fail_create=mode==1 ? 2 : -1;cycle_fail_finish=mode==2;cycle_late=83;
        cycle_vtable[3]=cycle_create;cycle_manager=cycle_vtable;
        sh_decl_server_test_reset_runtime_state();sh_decl_server_test_set_runtime(0);
        sh_decl_server_test_set_resource_add(cycle_add);
        sh_decl_server_test_set_reconstruct(cycle_reconstruct,cycle_peek);
        sh_decl_server_test_set_generic_load(cycle_read);
        int grand_index=-1;
        if(mode==1) {
            /* Reservation failure precedes any reconstruction of old content. */
            cycle_add(&cycle_manager,cycle_create(&cycle_manager,cycle_names[4]));
            *(unsigned *)(cycle_objects[4]+0x28)=4;
            *(int *)(cycle_objects[4]+0x54)=301;
            for(int i=0;i<5;i++)if(!strcmp(items[i].name,cycle_names[4]))grand_index=i;
            CHECK(grand_index>=0);items[grand_index].outcome=SH_DECL_SERVER_TEST_SHADOWED;
            sh_decl_server_test_set_runtime(1);
        }
        int made=-1;
        int ok=sh_decl_server_test_materialize_missing_sedefs(items,5,(void *)1,
            cycle_type,cycle_source,cycle_find,&made);
        CHECK(ok==(mode==0));CHECK(cycle_faults==0);
        for(int i=0;i<5;i++)if(cycle_present[i])CHECK(!(cycle_objects[i][0x2c]&2));
        if(mode==1) {
            CHECK(cycle_first_read_constructed==-1);CHECK(cycle_objects[0][0x2c]&4);
            CHECK(*(int *)(cycle_objects[4]+0x54)==301 && cycle_objects[4][0x2c]==0);
            items[grand_index].outcome=SH_DECL_SERVER_TEST_MISSING;
            sh_decl_server_test_set_runtime(0);cycle_fail_create=-1;
            CHECK(sh_decl_server_test_materialize_missing_sedefs(items,5,(void *)1,
                cycle_type,cycle_source,cycle_find,&made));
            CHECK(cycle_constructed==5 && cycle_faults==0);
            for(int i=2;i<5;i++)CHECK(*(int *)(cycle_objects[i]+0x54)==83);
            continue;
        }
        CHECK(cycle_first_read_constructed==4);
        if(mode==2)continue;
        CHECK(made==5 && cycle_constructed==5);
        for(int i=2;i<5;i++) { CHECK(*(int *)(cycle_objects[i]+0x50)==17);CHECK(*(int *)(cycle_objects[i]+0x54)==83); }
        int reads=0;for(int i=0;i<5;i++)reads+=cycle_reads[i];
        CHECK(sh_decl_server_test_materialize_missing_sedefs(items,5,(void *)1,cycle_type,cycle_source,cycle_find,&made));
        CHECK(made==0 && cycle_constructed==5);
        for(int i=0;i<5;i++)reads-=cycle_reads[i];CHECK(reads==0);
        /* The same obligation applies to a cycle refreshed in place. */
        for(int i=0;i<5;i++)items[i].outcome=SH_DECL_SERVER_TEST_SHADOWED;
        cycle_late=97;sh_decl_server_test_set_runtime(1);
        CHECK(sh_decl_server_test_materialize_missing_sedefs(items,5,(void *)1,cycle_type,cycle_source,cycle_find,&made));
        CHECK(made==0 && cycle_constructed==5 && cycle_faults==0);
        for(int i=2;i<5;i++)CHECK(*(int *)(cycle_objects[i]+0x54)==97);
        CHECK(sh_decl_server_test_clear_stray_pending()==0);
        for(int i=0;i<5;i++)items[i].outcome=SH_DECL_SERVER_TEST_MISSING;
        sh_decl_server_test_set_runtime(0);
    }
    sh_decl_server_test_reset_runtime_state();sh_decl_server_test_set_reconstruct(NULL,NULL);
}

int main(void)
{
    test_native_idstr_boundary();
    test_source_first_classification();
    test_sedef_materialization();
    test_runtime_premark_reused_empties();
    test_runtime_shadowed_refresh();
    test_runtime_reload_follows_new_dependencies();
    test_runtime_preserves_unaffected_caches();
    test_runtime_reconstruction_fault_restores_lifetime();
    test_rejected_new_identity_does_not_strand_replacements();
    test_runtime_shadowed_drain_fallback();
    test_runtime_keeps_successful_missing_identity();
    test_runtime_rejects_native_fallback();
    test_runtime_shadowed_drain_failed_is_swept();
    test_runtime_import_keeps_native_fallback();
    test_rebound_original_materialization();
    test_runtime_rebound_original();
    test_native_default_materialization();
    test_defaulted_material_isolation();
    test_runtime_stray_pending_cleared();
    test_runtime_off_leaves_placeholders_alone();
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
    test_material_table_dependency_ordering();
    test_scc_dependency_ordering();
    test_scc_parent_before_loading_member();
    test_inheritance_cycle_refusal();
    test_complete_set_collision_ordering();
    test_walk_error_propagation();
    test_empty_launch_and_runtime_results();
    test_large_snapshot();
    test_many_candidates();
    test_row_lineage();
    test_row_slot_production();
    test_row_portable_identity();
    test_row_supplier_variants();
    test_many_runtime_marks();
    test_shader_preparation_lifetime();
    test_occurrence_admission();
    test_defaulted_identity_reparse();
    test_native_cycle_construction();
    if (g_failed) {
        fprintf(stderr, "%d decl-server test(s) failed\n", g_failed);
        return 1;
    }
    puts("decl server tests passed");
    return 0;
}
