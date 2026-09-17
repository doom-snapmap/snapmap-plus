/* rawmap_paths_test -- where a rawmap save actually goes, driven directly.
 *
 * Written because a claim about it was disputed and prose is not evidence. The disagreement was
 * about stickiness: after naming a destination ("Save Rawmap As", `sh_rawmaps save <path>`), does
 * that file become the place every later save goes? It did, and it should not have. These tests
 * drive the same functions the console and the File menu call, and check the bytes on disk.
 *
 * The rule under test, in one sentence: a named destination applies to ONE write and is spent by it;
 * the only durable way to move the destination is the "Use Rawmap as Save Path" toggle.
 *
 * No hooks, no engine, no editor -- the rules are decisions about two strings and one file write,
 * and they can be checked without a game running.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "../src/backend/rawmap.c"

/* ---- STUBS -------------------------------------------------------------------------------------
 * rawmap.c is one translation unit that also holds the hooks, the package embedder, the navmesh
 * embedder and the editor bridge. None of that is reachable from the destination rules -- those are
 * two strings and a file write -- so the collaborators are stubbed rather than linked. A stub that
 * gets called would be a bug in the test, so the ones with a return value return failure, which the
 * caller under test treats as "not available" and reports rather than ignores.
 */
#include "signatures.h"
#include "map_package.h"
#include "map_embed.h"
#include "navmesh.h"
#include "nav_bake.h"
#include "overrides.h"
#include "raw_deflate.h"
#include "editor_frame.h"
#include "cvars.h"
#include "hook.h"

const sig_entry BACKEND_ENGINE_SIGNATURES[] = { { NULL, NULL, 0u } };

void backend_log(const char *line) { (void)line; }
static int package_errors, package_gate_fault;
static int inspection_packages, inspection_strip_fail, inspection_parses, render_loaded_calls;
static int initial_read_parses, initial_read_mode;
static int play_prepare_fail, play_select_fail, play_hud_fail, play_selections;
static char play_selected_json[256];
static int pf_testing, pf_boundary, pf_packages, pf_gate = 1, pf_fail, pf_held;
static int pf_lobby, pf_transition, pf_deferred;
static void *pf_native_editor;
static int pf_reads, pf_loads, pf_parses, pf_ctors, pf_dtors, pf_closes, pf_plans, pf_frees;
static int pf_activations, pf_restorations, pf_active, pf_order_error;
static int pf_coverage, pf_ready, pf_gate_calls, pf_availability_calls;
static int pf_install_requests, pf_inventory_reads, pf_install_outcome, pf_cancellations, pf_commits, pf_aborts;
static sh_mpkg_install_completion pf_completion;
static void *pf_completion_context;
static char pf_parsed[256];
static char package_last_error[1024];
void sh_mpkg_report_error(const char *line)
{ if (line && *line) { package_errors++; snprintf(package_last_error, sizeof(package_last_error), "%s", line); } }
void sh_map_render_loaded(void *map) { (void)map; render_loaded_calls++; }
int sh_decl_server_map_boundary_safe(void) { return pf_testing && pf_boundary; }
int sh_decl_server_map_preparation_ready(void) { return pf_testing; }
int sh_map_transition_ready(void) { return pf_testing; }
int sh_map_transition_at_boundary(void) { return pf_transition; }
int sh_decl_server_map_browser_present(void) { return pf_boundary && !pf_lobby; }
int sh_decl_server_map_retirement_safe(void) { return pf_testing && pf_boundary && !pf_lobby; }
int sh_decl_server_map_editor_matches(const void *editor) { return editor == (void *)3 || (editor && editor == pf_native_editor); }
int sh_decl_server_activate_map(sh_package_map_plan *plan)
{
    if ((!pf_boundary && !pf_transition) || (plan && !pf_plans)) pf_order_error++;
    if (plan) pf_activations++; else pf_restorations++;
    if (plan && g_pending_map.state && !sh_rawmap_defers_install_commit()) pf_order_error++;
    if (pf_fail == (plan ? 3 : 4)) return 0;
    pf_active = plan != NULL; return 1;
}
sh_mpkg_context *sh_mpkg_context_open(const char *root, const char *json, size_t len, char *err, size_t cap)
{ (void)root; (void)json; (void)len; (void)err; (void)cap; return pf_testing && pf_fail != 1 ? (sh_mpkg_context *)1 : NULL; }
const char *sh_mpkg_context_root(const sh_mpkg_context *context) { (void)context; return "private-map"; }
size_t sh_mpkg_context_count(const sh_mpkg_context *context) { (void)context; return pf_packages; }
int sh_mpkg_context_close(sh_mpkg_context **context)
{
    if (!context || !*context) return 1;
    if (pf_active && !pf_deferred) pf_order_error++;
    if (pf_held) return 0;
    if (*context) pf_closes++;
    *context = NULL; return 1;
}
static int pf_queued_cleanup;
void sh_mpkg_context_retire(sh_mpkg_context **context)
{
    if (!sh_mpkg_context_close(context)) { pf_queued_cleanup++; *context = NULL; }
}
void sh_mpkg_context_collect(void)
{
    if (!pf_held) { pf_closes += pf_queued_cleanup; pf_queued_cleanup = 0; }
}
sh_package_map_plan *sh_package_runtime_prepare_map(const char *root, const char *map, char *err, size_t cap)
{ (void)root; (void)map; (void)err; (void)cap; pf_plans++; return pf_fail == 2 ? NULL : (sh_package_map_plan *)1; }
void sh_package_map_plan_free(sh_package_map_plan *plan) { if (plan) pf_frees++; }
const sh_package_compilation *sh_package_map_plan_compilation(const sh_package_map_plan *plan)
{ return (const sh_package_compilation *)plan; }
int sh_package_map_plan_prepare_inventory(sh_package_map_plan *plan, const char *root, char *error, size_t capacity)
{ (void)plan; (void)root; (void)error; (void)capacity; pf_inventory_reads++; return pf_fail != 9; }
int sh_mpkg_request_map_install(const char *json, size_t length,
    const sh_package_compilation *candidate, const sh_package_owners *owners,
    sh_mpkg_install_completion completion, void *context, char *error, size_t capacity)
{
    (void)json; (void)length; (void)candidate; (void)owners; (void)error; (void)capacity;
    pf_install_requests++;
    if (!pf_gate) return 0;
    pf_completion = completion; pf_completion_context = context;
    if (pf_install_outcome != 2) { pf_completion = NULL; completion(context, pf_install_outcome); }
    return 1;
}
void sh_mpkg_cancel_map_consent(void *context)
{
    if (pf_completion && context == pf_completion_context) {
        sh_mpkg_install_completion callback = pf_completion; pf_completion = NULL; callback(context, 0);
    }
}
int sh_mpkg_activation_cancel(void) { pf_cancellations++; return 1; }
int sh_mpkg_activation_commit(char *error, size_t capacity)
{
    if ((g_pending_map.state != 5 && g_pending_map.state != 2) || !g_preflight_busy || pf_transition ||
        (g_pending_map.state == 2 && !g_preflight_consumed)) pf_order_error++;
    pf_commits++;
    if (pf_fail == 9 || pf_fail == 10) { snprintf(error, capacity, "commit refused"); return 0; }
    return 1;
}
int sh_package_runtime_commit_map(sh_package_map_plan *plan,
    int (*commit)(char *error, size_t capacity), char *error, size_t capacity)
{
    if (plan != g_pending_map.plan || (plan && !pf_active)) pf_order_error++;
    return commit(error, capacity);
}
int sh_package_map_plan_resources(const sh_package_map_plan *plan, const char *json, size_t length,
    const sh_package_references *references, sh_package_references *out)
{ (void)plan; (void)json; (void)length; (void)references; (void)out; return 0; }
int sh_decl_server_registry_source(sh_decl_registry_source *source) { (void)source; return 0; }
void *sh_typeinfo_get_reflect(void) { return NULL; }
int sh_package_map_plan_source_resources(const sh_package_map_plan *plan,
    sh_decl_registry_source registry, uintptr_t reflection, const char *json, size_t length,
    sh_package_references *out, sh_package_source_graph_report *report, char *error, size_t capacity)
{ (void)plan; (void)registry; (void)reflection; (void)json; (void)length; (void)out; (void)report; (void)error; (void)capacity; return 0; }
int sh_package_references_add(void *context, const char *type, const char *name)
{ (void)context; (void)type; (void)name; return 0; }
int sh_package_map_plan_missing(const sh_package_map_plan *plan, const char *const *paths, size_t count,
    sh_package_missing *out, char *error, size_t capacity)
{ (void)plan; (void)paths; (void)count; (void)out; (void)error; (void)capacity; return 0; }
void sh_package_missing_free(sh_package_missing *missing) { (void)missing; }
int sh_package_map_plan_payload_missing(const sh_package_map_plan *plan,
    sh_package_missing *out, char *error, size_t capacity)
{
    static char *missing[] = {"missing.fixture"};
    if (!plan || (!pf_deferred && (!pf_boundary || pf_active || pf_loads))) pf_order_error++;
    pf_availability_calls++; out->checked = 73;
    if (pf_coverage == -2) RaiseException(0xe0422003u, 0, 0, NULL);
    if (pf_coverage < 0) { snprintf(error, capacity, "fixture unavailable source"); return 0; }
    if (!pf_coverage) { out->paths = missing; out->count = 1; }
    return 1;
}
int sh_mpkg_activation_ready(void) { return pf_ready; }
void sh_package_runtime_error(char *error, size_t capacity) { if (capacity) error[0] = 0; }
int sh_package_runtime_select_map(const char *json, size_t length,
    const sh_package_references *references, char *error, size_t capacity)
{
    (void)references;
    if (error && capacity) error[0] = 0;
    if (play_select_fail == 2) RaiseException(0xe0421002u, 0, 0, NULL);
    if (play_select_fail) return 0;
    if (json && length < sizeof(play_selected_json)) {
        memcpy(play_selected_json, json, length); play_selected_json[length] = 0;
        play_selections++;
    }
    return 1;
}
int sh_mpkg_prepare_map(const char *json, size_t length, sh_package_references *references,
                        char *error, size_t capacity)
{ (void)json; (void)length; memset(references, 0, sizeof(*references));
  if (error && capacity) snprintf(error, capacity, "%s", play_prepare_fail ? "fixture dependency failure" : "");
  return !play_prepare_fail; }
void sh_package_references_free(sh_package_references *references) { (void)references; }
int sh_weapon_hud_reload(const char *root) { (void)root; return !play_hud_fail; }

void *install_inline_hook(void *target, void *detour, size_t stolen)
{ (void)target; (void)detour; (void)stolen; return NULL; }

sig_status sig_resolve_one(const uint8_t *module_base, const sig_entry *sig, sig_result *out)
{ (void)module_base; (void)sig; (void)out; return SIG_NOT_FOUND; }

int sh_mpkg_gate(const char *json, size_t len)
{
    (void)json; (void)len;
    pf_gate_calls++;
    if (package_gate_fault) RaiseException(0xe0421001u, 0, 0, NULL);
    return pf_testing && pf_gate;
}
char *sh_mpkg_strip(const char *json, size_t len, size_t *out_len)
{
    char *copy;
    (void)json; (void)len; if (out_len) *out_len = 0;
    if (!inspection_packages || inspection_strip_fail) return NULL;
    copy = HeapAlloc(GetProcessHeap(), 0, 3);
    if (copy) { memcpy(copy, "{}", 3); if (out_len) *out_len = 2; }
    return copy;
}
size_t sh_mpkg_scan(const char *json, size_t length, sh_mpkg_decl *out, size_t capacity)
{ (void)json; (void)length; (void)out; (void)capacity; return inspection_packages ? SIZE_MAX : 0; }
char *sh_mpkg_embed(const char *json, size_t len, const char *pkg_id,
                    const unsigned char *payload, size_t payload_len,
                    size_t *out_len, char *err, size_t err_cap)
{ (void)json; (void)len; (void)pkg_id; (void)payload; (void)payload_len;
  if (out_len) *out_len = 0; if (err && err_cap) err[0] = '\0'; return NULL; }

unsigned char *sh_mpkg_pack_dir(const char *root, size_t *out_len, char *err, size_t err_cap)
{ (void)root; if (out_len) *out_len = 0; if (err && err_cap) err[0] = '\0'; return NULL; }

size_t sh_mpkg_used_packages(const char *json, size_t len, const char *data_root,
                             sh_mpkg_used **out, char *error, size_t capacity)
{ (void)json; (void)len; (void)data_root; if (out) *out = NULL;
  if (error && capacity) error[0] = 0; return 0; }

int sh_overrides_get_root(char *out, size_t cap)
{ if (out && cap) strcpy_s(out, cap, pf_testing ? "fixture-root" : ""); return pf_testing; }

void  sh_navmesh_build_from_map(const char *json, size_t len) { (void)json; (void)len; }
char *sh_navmesh_strip(const char *json, size_t len, size_t *out_len)
{ (void)json; (void)len; if (out_len) *out_len = 0; return NULL; }
char *sh_navmesh_embed_all(const char *json, size_t len, size_t *out_len)
{ (void)json; (void)len; if (out_len) *out_len = 0; return NULL; }
void  sh_nav_bake_set_map(const char *json, size_t len) { (void)json; (void)len; }

int sh_editor_frame_request_rawmap_save(char *out_msg, int msg_capacity)
{ if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity,
                                             "stub: no editor in this test", _TRUNCATE);
  return 0; }
int sh_editor_frame_request_rawmap_save_to(const char *destination, char *out_msg, int msg_capacity)
{ (void)destination; return sh_editor_frame_request_rawmap_save(out_msg, msg_capacity); }
int sh_editor_frame_can_rawmap_save(char *out_msg, int msg_capacity)
{ if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity,
                                             "stub: no editor in this test", _TRUNCATE);
  return 0; }
int sh_editor_frame_request_reload(char *out_msg, int msg_capacity)
{ if (out_msg && msg_capacity > 0) strncpy_s(out_msg, (size_t)msg_capacity,
                                             "stub: no editor in this test", _TRUNCATE);
  return 0; }
int sh_editor_frame_saved_map_dir(char *out, size_t cap, char *out_id, size_t id_cap)
{ if (out && cap) out[0] = '\0'; if (out_id && id_cap) out_id[0] = '\0'; return 0; }

size_t sh_inflate_raw_upto(const unsigned char *src, size_t src_len,
                           unsigned char *dst, size_t dst_cap)
{ (void)src; (void)src_len; (void)dst; (void)dst_cap; return 0; }

int sh_cvar_value_int(int index, int def) { (void)index; return def; }

/* ---- end stubs -------------------------------------------------------------------------------- */

static int g_failed;
static int g_checks;

#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static const char BODY[] = "{\"entities\":[]}";

static void eff_paths(char *load, int load_cap, char *save, int save_cap)
{
    if (load) load[0] = '\0';
    if (save) save[0] = '\0';
    sh_rawmap_get_paths(load, load_cap, save, save_cap);
}

static int save_is_default(void)
{
    char eff[MAX_PATH] = "", def[MAX_PATH] = "";
    sh_rawmap_get_paths(NULL, 0, eff, (int)sizeof eff);
    sh_rawmap_get_default_paths(NULL, 0, def, (int)sizeof def);
    return (eff[0] && _stricmp(eff, def) == 0) ? 1 : 0;
}

static int temp_path(const char *name, char *out, size_t cap)
{
    char dir[MAX_PATH];
    if (GetTempPathA((DWORD)sizeof dir, dir) == 0) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s%s", dir, name);
    return 1;
}

/* Setting the LOAD source validates the file, so it has to exist and parse as a rawmap. */
static int make_json(const char *name, char *out, size_t cap)
{
    HANDLE h;
    DWORD wrote = 0;
    if (!temp_path(name, out, cap)) return 0;
    h = CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    WriteFile(h, BODY, (DWORD)(sizeof BODY - 1), &wrote, NULL);
    CloseHandle(h);
    return 1;
}

static int file_exists(const char *p)
{
    DWORD a = GetFileAttributesA(p);
    return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) ? 1 : 0;
}

static void clean_state(void)
{
    sh_rawmap_swap_set_source(NULL);
    sh_rawmap_set_save_target(NULL);
}

/* --------------------------------------------------------------------------------------------- */

/* Save Rawmap As picks a file, and Save keeps writing there. */
static void a_chosen_file_stays_chosen(void)
{
    char picked[MAX_PATH], save[MAX_PATH];

    printf("\n-- a file chosen for saving is still the target after the write\n");

    clean_state();
    CHECK(save_is_default());
    CHECK(temp_path("rawmap_test_export.json", picked, sizeof picked));
    DeleteFileA(picked);

    CHECK(sh_rawmap_set_save_target(picked) == 1);
    eff_paths(NULL, 0, save, (int)sizeof save);
    printf("   chosen:           save to: %s\n", save);
    CHECK(_stricmp(save, picked) == 0);
    CHECK(!save_is_default());

    CHECK(sh_rawmap_test_write(BODY, sizeof BODY - 1) == (sizeof BODY - 1));
    CHECK(file_exists(picked));                       /* it went where it was aimed */

    eff_paths(NULL, 0, save, (int)sizeof save);
    printf("   after the write:  save to: %s\n", save);
    CHECK(_stricmp(save, picked) == 0);               /* THE POINT: still there */
    CHECK(!save_is_default());

    DeleteFileA(picked);
    clean_state();
}

/* A write that FAILED keeps its aim, so a retry still goes to the named file. */
static void a_failed_write_keeps_its_destination(void)
{
    char save[MAX_PATH];
    /* a path that cannot be created: a directory component that is not a directory */
    static const char bad[] = "\\\\?\\Z:\\no-such-volume\\nope.json";

    printf("\n-- a write that fails keeps the destination, so a retry is still aimed there\n");

    clean_state();
    CHECK(sh_rawmap_set_save_target(bad) == 1);
    CHECK(sh_rawmap_test_write(BODY, sizeof BODY - 1) == 0);   /* the write fails */

    eff_paths(NULL, 0, save, (int)sizeof save);
    printf("   after the failure: save to: %s\n", save);
    CHECK(_stricmp(save, bad) == 0);
    CHECK(!save_is_default());

    clean_state();
}

/* THE ARCHIVE RULE. Opening a rawmap must never aim saves at it. */
static void opening_a_rawmap_never_aims_saves_at_it(const char *archive)
{
    char save[MAX_PATH];

    printf("\n-- opening a rawmap leaves saves on the usual file\n");

    clean_state();
    CHECK(sh_rawmap_swap_set_source(archive) == 1);

    eff_paths(NULL, 0, save, (int)sizeof save);
    printf("   after opening:    save to: %s\n", save);
    CHECK(_stricmp(save, archive) != 0);              /* NOT the file that was opened */
    CHECK(save_is_default());

    clean_state();
}

/* Choosing the opened rawmap is allowed -- it just has to be asked for. */
static void choosing_the_opened_rawmap_works(const char *archive)
{
    char save[MAX_PATH];

    printf("\n-- choosing the opened rawmap aims saves at it\n");

    clean_state();
    CHECK(sh_rawmap_swap_set_source(archive) == 1);
    CHECK(sh_rawmap_set_save_target(archive) == 1);

    eff_paths(NULL, 0, save, (int)sizeof save);
    printf("   after choosing:   save to: %s\n", save);
    CHECK(_stricmp(save, archive) == 0);

    clean_state();
}

/* Opening a different map drops the target, so it cannot follow the person around. */
static void a_different_map_drops_the_target(void)
{
    char picked[MAX_PATH], save[MAX_PATH];

    printf("\n-- opening a different map puts saves back on the usual file\n");

    clean_state();
    CHECK(temp_path("rawmap_test_export3.json", picked, sizeof picked));
    CHECK(sh_rawmap_set_save_target(picked) == 1);
    CHECK(!save_is_default());

    sh_rawmap_clear_save_target_for_new_map();

    eff_paths(NULL, 0, save, (int)sizeof save);
    printf("   after the change: save to: %s\n", save);
    CHECK(save_is_default());

    clean_state();
}

/* A bare word is not a destination: it would land in DOOM's own install folder. */
static void a_bare_word_is_refused(void)
{
    printf("\n-- a name with no folder is refused\n");

    clean_state();
    CHECK(sh_rawmap_set_save_target("banana") == 0);
    CHECK(save_is_default());

    clean_state();
}

/* paths_are_default must answer both ways. It tested emptiness once, which is never true after the
 * installers materialize a default, so it was stuck on "not default" forever. */
static void are_default_answers_both_ways(const char *archive)
{
    char picked[MAX_PATH];

    printf("\n-- paths_are_default answers both ways\n");

    clean_state();
    CHECK(sh_rawmap_paths_are_default() == 1);

    CHECK(sh_rawmap_swap_set_source(archive) == 1);
    CHECK(sh_rawmap_paths_are_default() == 0);
    sh_rawmap_swap_set_source(NULL);
    CHECK(sh_rawmap_paths_are_default() == 1);

    CHECK(temp_path("rawmap_test_export4.json", picked, sizeof picked));
    CHECK(sh_rawmap_set_save_target(picked) == 1);
    CHECK(sh_rawmap_paths_are_default() == 0);   /* a chosen file counts as a moved save path */
    sh_rawmap_set_save_target(NULL);
    CHECK(sh_rawmap_paths_are_default() == 1);

    clean_state();
}

/* Picking the usual file is not a choice to keep saves off it, so nothing is held. */
static void picking_the_usual_file_holds_nothing(void)
{
    char dflt[MAX_PATH] = "", target[MAX_PATH] = "x";

    printf("\n-- picking the usual file leaves nothing in force\n");

    clean_state();
    sh_rawmap_get_default_paths(NULL, 0, dflt, (int)sizeof dflt);
    CHECK(dflt[0] != '\0');

    CHECK(sh_rawmap_set_save_target(dflt) == 1);
    sh_rawmap_get_save_target(target, (int)sizeof target);
    printf("   after picking it: target is %s\n", target[0] ? target : "(nothing)");
    CHECK(target[0] == '\0');            /* the tick stays clear */
    CHECK(save_is_default());
    CHECK(sh_rawmap_paths_are_default() == 1);

    clean_state();
}

/* A file chosen earlier is released when the usual file is picked. */
static void picking_the_usual_file_releases_a_chosen_one(void)
{
    char dflt[MAX_PATH] = "", picked[MAX_PATH], target[MAX_PATH] = "x";

    printf("\n-- picking the usual file releases a file chosen before it\n");

    clean_state();
    sh_rawmap_get_default_paths(NULL, 0, dflt, (int)sizeof dflt);
    CHECK(temp_path("rawmap_test_export5.json", picked, sizeof picked));
    CHECK(sh_rawmap_set_save_target(picked) == 1);
    CHECK(!save_is_default());

    CHECK(sh_rawmap_set_save_target(dflt) == 1);
    sh_rawmap_get_save_target(target, (int)sizeof target);
    CHECK(target[0] == '\0');
    CHECK(save_is_default());

    clean_state();
}

/* Write a file with exact bytes -- these tests are about what the LAST bytes say. */
static int write_file(const char *name, const char *bytes, char *out, size_t cap)
{
    HANDLE h;
    DWORD wrote = 0;
    if (!temp_path(name, out, cap)) return 0;
    h = CreateFileA(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    WriteFile(h, bytes, (DWORD)strlen(bytes), &wrote, NULL);
    CloseHandle(h);
    return 1;
}

/* WHAT COUNTS AS A RAWMAP, which is what `sh_rawmaps list` has to decide.
 *
 * It listed every *.json it found, and the folders involved are full of JSON that is not a map:
 * config.json, install.json, pinned.json, and a prefabs tree of *.snapmap.json. A prefab is the
 * awkward one -- it has a top-level "entities" array exactly like a map does, so the obvious marker
 * does not separate them. The engine's own top-level "~type" does. */
static void looks_like_rawmap_tells_them_apart(void)
{
    char p[MAX_PATH];

    printf("\n-- what counts as a rawmap (the `list` filter)\n");

    /* a rawmap: keys come out sorted, so "~type" is second to last */
    CHECK(write_file("rawmap_sniff_map.json",
                     "{\"entities\":[],\"version\":111,\"~type\":\"idSnapMap\",\"~version\":111}",
                     p, sizeof p));
    printf("   rawmap                        -> %d\n", sh_rawmap_looks_like_rawmap(p));
    CHECK(sh_rawmap_looks_like_rawmap(p) == 1);
    DeleteFileA(p);

    /* a PREFAB. Same "entities" array, different type. This is the case that made the first
     * filter wrong. */
    CHECK(write_file("rawmap_sniff_prefab.json",
                     "{\"cameraYaw\":125.5,\"entities\":[],\"grabAxis\":0,"
                     "\"~type\":\"idSnapEntityPrefab\",\"~version\":111}",
                     p, sizeof p));
    printf("   prefab                        -> %d\n", sh_rawmap_looks_like_rawmap(p));
    CHECK(sh_rawmap_looks_like_rawmap(p) == 0);
    DeleteFileA(p);

    /* a prefab holding MAP geometry, so the string idSnapMap appears -- as a PREFIX of
     * idSnapMapCapEntity. Searching without the closing quote would call this a map. */
    CHECK(write_file("rawmap_sniff_prefab_caps.json",
                     "{\"entities\":[{\"~type\":\"idSnapMapCapEntity\"}],"
                     "\"~type\":\"idSnapEntityPrefab\",\"~version\":111}",
                     p, sizeof p));
    printf("   prefab of map caps            -> %d\n", sh_rawmap_looks_like_rawmap(p));
    CHECK(sh_rawmap_looks_like_rawmap(p) == 0);
    DeleteFileA(p);

    /* our own settings files, which sit in the DEFAULT rawmap folder */
    CHECK(write_file("rawmap_sniff_config.json",
                     "{\"schema_version\":1,\"settings\":{\"theme\":\"dark\"}}", p, sizeof p));
    printf("   config.json                   -> %d\n", sh_rawmap_looks_like_rawmap(p));
    CHECK(sh_rawmap_looks_like_rawmap(p) == 0);
    DeleteFileA(p);

    /* not JSON at all */
    CHECK(write_file("rawmap_sniff_binary.json", "\x1f\x8b\x08 not json", p, sizeof p));
    printf("   not JSON                      -> %d\n", sh_rawmap_looks_like_rawmap(p));
    CHECK(sh_rawmap_looks_like_rawmap(p) == 0);
    DeleteFileA(p);

    /* a pretty-printed rawmap: sh_pretty_on re-lays these out, so the search cannot assume there
     * is no space after the colon */
    CHECK(write_file("rawmap_sniff_pretty.json",
                     "{\n  \"entities\": [],\n  \"version\": 111,\n"
                     "  \"~type\": \"idSnapMap\",\n  \"~version\": 111\n}\n",
                     p, sizeof p));
    printf("   pretty-printed rawmap         -> %d\n", sh_rawmap_looks_like_rawmap(p));
    CHECK(sh_rawmap_looks_like_rawmap(p) == 1);
    DeleteFileA(p);

    /* and the real thing on this machine, if it is there -- the fixtures above are only as good as
     * their resemblance to it, so check one that the engine actually wrote */
    {
        char real[MAX_PATH] = "";
        sh_rawmap_get_default_paths(NULL, 0, real, (int)sizeof real);
        if (real[0] && file_exists(real)) {
            printf("   the real %s -> %d\n", real, sh_rawmap_looks_like_rawmap(real));
            CHECK(sh_rawmap_looks_like_rawmap(real) == 1);
        } else {
            printf("   (no rawmap.json on this machine to check against)\n");
        }
    }
}

static void refused_menu_save_changes_nothing(void)
{
    char original[MAX_PATH], requested[MAX_PATH], actual[MAX_PATH], message[192];
    sh_rawmap_configure_fn configure = NULL;
    clean_state();
    CHECK(temp_path("rawmap_review_original.json", original, sizeof original));
    CHECK(temp_path("rawmap_review_requested.json", requested, sizeof requested));
    CHECK(sh_rawmap_set_save_target(original));
    sh_rawmap_get_slots(NULL, &configure, NULL);
    CHECK(configure(NULL, NULL, requested, -1, message, sizeof message) == 0);
    sh_rawmap_get_save_target(actual, sizeof actual);
    CHECK(strcmp(actual, original) == 0);
    CHECK(!sh_rawmap_save_oneshot_pending());
    CHECK(strstr(message, "no editor") != NULL);
    CHECK(configure(NULL, NULL, NULL, 5, message, sizeof message) == 0);
    CHECK(!sh_rawmap_save_oneshot_pending());
    clean_state();
}

static void missing_overwrite_protection_refuses_staging(const char *archive)
{
    sh_rawmap_configure_fn configure = NULL;
    char before[MAX_PATH], after[MAX_PATH], message[192];
    clean_state();
    sh_rawmap_set_branch_tag(1);
    sh_rawmap_get_paths(before, sizeof before, NULL, 0);
    sh_rawmap_get_slots(NULL, &configure, NULL);
    CHECK(!sh_rawmap_load_is_safe());
    CHECK(!configure(NULL, archive, NULL, 2, message, sizeof message));
    sh_rawmap_get_paths(after, sizeof after, NULL, 0);
    CHECK(strcmp(before, after) == 0);
    CHECK(!sh_rawmap_load_oneshot_pending());
    CHECK(strstr(message, "overwrite protection") != NULL);
    sh_rawmap_set_branch_tag(0);
    CHECK(sh_rawmap_load_is_safe());
    sh_rawmap_set_branch_tag(1);
}

static int live_serializations;
static void *fixture_idstr_ctor(void *self, const char *initial)
{ (void)initial; memset(self, 0, IDSTR_SIZE); return self; }
static void fixture_idstr_dtor(void *self) { (void)self; }
static unsigned char fixture_serialize(void *map, void *out, unsigned char compact)
{
    (void)map; (void)compact;
    ++live_serializations;
    *(int *)((char *)out + IDSTR_LEN_OFF) = sizeof BODY - 1;
    *(const char **)((char *)out + IDSTR_DATA_OFF) = BODY;
    return 1;
}
static unsigned char fixture_map_to_json(void *map, void *out, unsigned char compact)
{ return sh_ser_detour(map, out, compact); }

static int snapshot_visits, snapshot_accept;
static int fixture_visit(const void *map, const void *out, void *ctx)
{
    CHECK(map == (void *)1 && ctx == (void *)2);
    CHECK(*(const char *const *)((const char *)out + IDSTR_DATA_OFF) == BODY);
    CHECK(g_snapshot_depth == 1);
    snapshot_visits++;
    if (snapshot_accept < 0) RaiseException(0xe0000001, 0, 0, NULL);
    return snapshot_accept;
}

static void snapshots_visit_without_save_effects(void)
{
    unsigned char str[IDSTR_SIZE] = {0};
    unsigned long before = sh_rawmap_save_count();
    int caught = 0;
    clean_state();
    g_ser_orig = fixture_serialize;
    sh_rawmap_save_arm_once();
    snapshot_visits = 0; snapshot_accept = 1;
    CHECK(sh_rawmap_snapshot_inspect(fixture_map_to_json,(void *)1,str,fixture_visit,(void *)2));
    CHECK(snapshot_visits == 1 && sh_rawmap_save_count() == before);
    CHECK(sh_rawmap_save_oneshot_pending());
    CHECK(!g_snapshot_depth && !g_snapshot_visit && !g_snapshot_visit_ctx);
    CHECK(sh_rawmap_snapshot(fixture_map_to_json,(void *)1,str));
    CHECK(snapshot_visits == 1);
    snapshot_accept = 0;
    CHECK(!sh_rawmap_snapshot_inspect(fixture_map_to_json,(void *)1,str,fixture_visit,(void *)2));
    snapshot_accept = -1;
    __try { sh_rawmap_snapshot_inspect(fixture_map_to_json,(void *)1,str,fixture_visit,(void *)2); }
    __except(EXCEPTION_EXECUTE_HANDLER) { caught = 1; }
    CHECK(caught && !g_snapshot_depth && !g_snapshot_visit && !g_snapshot_visit_ctx);
    CHECK(sh_rawmap_save_count() == before && sh_rawmap_save_oneshot_pending());
    g_ser_orig = NULL;
    clean_state();
}

static void a_live_export_mirrors_once(void)
{
    char target[MAX_PATH], msg[192];
    unsigned long before = sh_rawmap_save_count();
    unsigned long long wrote = 0;
    clean_state();
    CHECK(temp_path("rawmap_review_live.json", target, sizeof target));
    CHECK(sh_rawmap_set_save_target(target));
    g_map_to_json = fixture_map_to_json;
    g_ser_orig = fixture_serialize;
    g_idstr_ctor = fixture_idstr_ctor;
    g_idstr_dtor = fixture_idstr_dtor;
    sh_rawmap_swap_arm(1);
    live_serializations = 0;
    CHECK(sh_rawmap_write_from_live((void *)1, target, msg, sizeof msg, &wrote));
    CHECK(live_serializations == 1 && wrote == sizeof BODY - 1);
    CHECK(sh_rawmap_save_count() == before + 1);
    CHECK(g_snapshot_depth == 0);
    sh_rawmap_swap_arm(0);
    g_map_to_json = NULL;
    g_ser_orig = NULL;
    g_idstr_ctor = NULL;
    g_idstr_dtor = NULL;
    DeleteFileA(target);
    clean_state();
}

static void an_existing_scratch_file_survives(void)
{
    char target[MAX_PATH], scratch[MAX_PATH], bytes[64] = {0};
    HANDLE h;
    DWORD got = 0;
    static const char saved[] = "previous scratch contents";
    CHECK(temp_path("rawmap_review_atomic.json", target, sizeof target));
    CHECK(write_file("rawmap_review_atomic.json.tmp", saved, scratch, sizeof scratch));
    CHECK(sh_rawmap_set_save_target(target));
    CHECK(sh_rawmap_test_write(BODY, sizeof BODY - 1) == sizeof BODY - 1);
    h = CreateFileA(scratch, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        CHECK(ReadFile(h, bytes, sizeof bytes - 1, &got, NULL));
        CloseHandle(h);
        CHECK(strcmp(bytes, saved) == 0);
    }
    /* A target locked against replacement must retain its original complete bytes. */
    h = CreateFileA(target, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        CHECK(sh_rawmap_test_write("replacement", 11) == 0);
        memset(bytes, 0, sizeof bytes);
        CHECK(ReadFile(h, bytes, sizeof bytes - 1, &got, NULL));
        CHECK(strcmp(bytes, BODY) == 0);
        CloseHandle(h);
    }
    DeleteFileA(scratch);
    DeleteFileA(target);
    clean_state();
}

static void large_sources_use_native_lengths(void)
{
    char path[MAX_PATH], message[256], block[65536];
    HANDLE file;
    DWORD wrote;
    size_t i, length = 0;
    char *body;
    CHECK(temp_path("rawmap_large_source.json", path, sizeof(path)));
    file = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    if (file == INVALID_HANDLE_VALUE) return;
    memset(block, ' ', sizeof(block));
    CHECK(WriteFile(file, "{", 1, &wrote, NULL) && wrote == 1);
    for (i = 0; i < 1040; i++) {
        if (!WriteFile(file, block, sizeof(block), &wrote, NULL) || wrote != sizeof(block)) {
            CHECK(0); CloseHandle(file); DeleteFileA(path); return;
        }
    }
    CHECK(WriteFile(file, "}", 1, &wrote, NULL) && wrote == 1);
    CloseHandle(file);
    CHECK(sh_rawmap_validate_source(path, message, sizeof(message)));
    CHECK(sh_rawmap_swap_set_source(path));
    body = read_source_file(&length);
    CHECK(body && length == (size_t)1040 * sizeof(block) + 2);
    if (body) {
        CHECK(body[0] == '{' && body[length - 1] == '}' && !body[length]);
        HeapFree(GetProcessHeap(), 0, body);
    }
    CHECK(sh_rawmap_swap_set_source(NULL));
    DeleteFileA(path); clean_state();
}

static int play_ctor_calls, play_dtor_calls, play_serializations, play_allocations;
static int play_native_fault, play_native_refuse;
static void *play_expected_editor, *play_expected_snapshot;
static void *play_ctor(void *self, const char *initial)
{
    ++play_ctor_calls;
    if (play_native_fault == 1) RaiseException(0xe0421003u, 0, 0, NULL);
    return fixture_idstr_ctor(self, initial);
}
static void play_dtor(void *self)
{
    (void)self; ++play_dtor_calls;
    if (play_native_fault == 3) RaiseException(0xe0421003u, 0, 0, NULL);
}
static unsigned char play_serialize(void *snapshot, void *out, unsigned char compact)
{
    const char *json = *(const char **)snapshot;
    ++play_serializations;
    CHECK(snapshot == play_expected_snapshot);
    CHECK(!compact && g_snapshot_depth == 1 && !g_snapshot_visit);
    if (play_native_fault == 2) RaiseException(0xe0421003u, 0, 0, NULL);
    if (play_native_refuse) return 0;
    *(int *)((char *)out + IDSTR_LEN_OFF) = (int)strlen(json);
    *(const char **)((char *)out + IDSTR_DATA_OFF) = json;
    return 1;
}
static void play_allocate(void *editor)
{
    CHECK(editor == play_expected_editor);
    CHECK(!strcmp(play_selected_json, *(const char **)play_expected_snapshot));
    CHECK(play_ctor_calls == play_dtor_calls && !g_snapshot_depth);
    ++play_allocations;
    ((unsigned char *)editor)[8] = 1;
}

static void play_uses_current_snapshot_without_saving(void)
{
    static const char package_map[] = "{\"entities\":[{\"inherit\":\"campaign/demon\"}]}";
    const char *current_map = package_map;
    unsigned char *editor = (unsigned char *)calloc(1, PLAY_MAP_OFF + sizeof(char *));
    unsigned long saves = sh_rawmap_save_count();
    int i, allocations, selections;
    CHECK(editor != NULL);
    if (!editor) return;
    clean_state(); sh_rawmap_save_arm_once(); sh_rawmap_load_arm_once();
    *(void **)(editor + PLAY_MAP_OFF) = &current_map;
    play_expected_snapshot = &current_map;
    play_expected_editor = editor;
    g_play_orig = play_allocate; g_ser_orig = play_serialize;
    g_map_to_json = play_serialize;
    g_idstr_ctor = play_ctor; g_idstr_dtor = play_dtor;
    sh_play_detour(editor);
    CHECK(play_allocations == 1 && editor[8] == 1);
    /* Mutate the native snapshot without a deserialize or save callback. */
    current_map = BODY;
    sh_play_detour(editor);
    CHECK(play_allocations == 2 && play_serializations == 2);
    CHECK(!strcmp(play_selected_json, BODY));
    CHECK(sh_rawmap_save_count() == saves);
    CHECK(sh_rawmap_save_oneshot_pending() && sh_rawmap_load_oneshot_pending());

    allocations = play_allocations;
    for (i = 0; i < 9; ++i) {
        editor[8] = 0; selections = play_selections;
        play_native_fault = i < 3 ? i + 1 : 0;
        play_native_refuse = i == 3;
        play_prepare_fail = i == 4;
        play_select_fail = i == 5 ? 1 : (i == 6 ? 2 : 0);
        play_hud_fail = i == 7;
        g_idstr_ctor = i == 8 ? NULL : play_ctor;
        sh_play_detour(editor);
        CHECK(play_allocations == allocations && editor[8] == 0);
        if (i == 4) CHECK(strstr(package_last_error, "fixture dependency failure"));
        if (i == 7) CHECK(strstr(package_last_error, "weapon HUD policy"));
        if (i != 7) CHECK(play_selections == selections);
        CHECK(!g_snapshot_depth && !g_snapshot_visit && !g_snapshot_visit_ctx);
        CHECK(sh_rawmap_save_count() == saves && sh_rawmap_save_oneshot_pending());
    }
    play_native_fault = play_native_refuse = play_prepare_fail = play_select_fail = play_hud_fail = 0;
    g_idstr_ctor = play_ctor;
    /* Retry after refusal, without restarting or reinstalling the hook. */
    play_ctor_calls = play_dtor_calls = 0;
    sh_play_detour(editor);
    CHECK(play_allocations == allocations + 1 && editor[8] == 1);
    g_play_orig = NULL; g_ser_orig = NULL; g_idstr_ctor = NULL; g_idstr_dtor = NULL;
    g_map_to_json = NULL;
    free(editor); play_expected_editor = NULL; clean_state();
}

static int play_publication_faults;
static void play_publication(void *target)
{
    (void)target;
    if (!g_play_orig || ((int (*)(void *))g_play_orig)(NULL) != 42) ++play_publication_faults;
}
static void play_hook_publishes_original_before_detour(void)
{
    unsigned char *code = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int before = hook_owned_count();
    CHECK(code != NULL); if (!code) return;
    memset(code, 0x90, PLAY_STOLEN);
    /* One RIP-relative LEA like the command prologue, ignored by this fixture. */
    code[12] = 0x48; code[13] = 0x8d; code[14] = 0x0d;
    memset(code + 15, 0, 4);
    code[PLAY_STOLEN] = 0xb8; code[PLAY_STOLEN + 1] = 42;
    memset(code + PLAY_STOLEN + 2, 0, 3); code[PLAY_STOLEN + 5] = 0xc3;
    FlushInstructionCache(GetCurrentProcess(), code, 64);
    sh_patch_test_observe_write(play_publication);
    sh_patch_test_faults(2 | 4 | 8, 0, 0);
    CHECK(!sh_rawmap_play_install(code, 1));
    CHECK(g_play_orig && !hook_is_installed((void *)g_play_orig));
    CHECK(hook_owned_count() == before + 1);
    sh_patch_test_faults(1, 0, 0);
    CHECK(!sh_rawmap_play_install(code, 1) && g_play_orig);
    sh_patch_test_faults(0, 0, 0);
    CHECK(sh_rawmap_play_install(code, 1) && hook_is_installed((void *)g_play_orig));
    CHECK(sh_rawmap_play_install(NULL, 0) && !play_publication_faults);
    CHECK(hook_unpatch((void *)g_play_orig)); g_play_orig = NULL;
    CHECK(hook_owned_count() == before);
    sh_patch_test_observe_write(NULL);
    VirtualFree(code, 0, MEM_RELEASE);
}

static void *pf_ctor(void *self, const char *initial)
{
    memset(self, 0, IDSTR_SIZE); pf_ctors++;
    if (initial && *initial) {
        size_t length = strlen(initial);
        char *body = HeapAlloc(GetProcessHeap(), 0, length + 1);
        if (body) memcpy(body, initial, length + 1);
        *(char **)((unsigned char *)self + 0x10) = body;
        *(int *)((unsigned char *)self + 8) = (int)length;
    }
    return self;
}
static void pf_dtor(void *self)
{
    void *body = *(void **)((unsigned char *)self + 0x10);
    if (body) HeapFree(GetProcessHeap(), 0, body);
    pf_dtors++;
    if (pf_fail == 6) RaiseException(0xe0422001, 0, 0, NULL);
}
static int pf_reader(const char *id, void *out, int flags, int slot)
{
    const char *json = "{\"from\":\"prepared\"}";
    size_t length = strlen(json);
    char *body;
    CHECK(!strcmp(id, "0123456789ABCDEF0123") && flags == 0 && slot == -1);
    CHECK(pf_boundary && !pf_loads); pf_reads++;
    if (pf_fail == 5) return -1;
    body = HeapAlloc(GetProcessHeap(), 0, length + 1); CHECK(body);
    if (!body) return -1;
    memcpy(body, json, length + 1);
    *(int *)((unsigned char *)out + 8) = (int)length;
    *(char **)((unsigned char *)out + 0x10) = body;
    return 0;
}
static int pf_parser(const char *json, void *map)
{
    CHECK(map == (void *)2 && (!pf_packages || pf_active));
    CHECK(!pf_packages || !pf_frees); pf_parses++;
    strcpy_s(pf_parsed, sizeof(pf_parsed), json);
    if (pf_fail == 7) RaiseException(0xe0422002, 0, 0, NULL);
    return pf_fail == 8 ? 0 : 1;
}
static int pf_loader(void *editor, const void *name)
{
    CHECK(!strcmp(*(const char *const *)((const unsigned char *)name + 0x10), "0123456789ABCDEF0123"));
    CHECK(editor == (void *)3); pf_loads++;
    pf_boundary = 0; /* Native initialization starts only here. */
    if (pf_fail == 12) return 0; /* Canceled before parsing the retained map. */
    return sh_deser_detour("{\"from\":\"second engine read\"}", (void *)2) ? 0 : 9;
}
static int pf_abort(void *editor)
{
    CHECK(editor == (void *)3 && !g_preflight_json && g_preflight_busy);
    pf_aborts++; pf_boundary = 1; return 1;
}
static void pf_native_set_active(void *editor, int active)
{
    unsigned char *native = editor;
    CHECK(editor == pf_native_editor && !active && *(int *)(native + 0x2366c) == 1);
    pf_aborts++;
    if (pf_fail) RaiseException(0xe0422084, 0, 0, NULL);
    native[9] = 0;
}
static void native_saved_cancellation_uses_owned_editor(void)
{
    unsigned char *editor = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 0x24000);
    void *vtable[10] = {0};
    CHECK(editor != NULL); if (!editor) return;
    pf_native_editor = editor; *(void ***)editor = vtable; vtable[9] = pf_native_set_active;
    g_pending_map.thread = GetCurrentThreadId(); pf_aborts = pf_fail = 0;
    editor[9] = 1;
    CHECK(!preflight_abort_saved_load(NULL));
    g_pending_map.thread++;
    CHECK(!preflight_abort_saved_load(editor) && editor[9] == 1 && !pf_aborts);
    g_pending_map.thread = GetCurrentThreadId();
    CHECK(preflight_abort_saved_load(editor) && !editor[9] && pf_aborts == 1);
    CHECK(preflight_abort_saved_load(editor) && pf_aborts == 1);
    editor[9] = 1; pf_fail = 1;
    CHECK(!preflight_abort_saved_load(editor) && editor[9] == 1 && pf_aborts == 2);
    pf_native_editor = NULL; pf_fail = 0; g_pending_map.thread = 0;
    HeapFree(GetProcessHeap(), 0, editor);
}
static void pf_reset(void)
{
    CHECK(!g_map_context && !g_preflight_json && !g_preflight_busy && !pf_active && !g_pending_map.state);
    pf_testing = pf_boundary = pf_gate = 1; pf_packages = pf_fail = pf_held = 0;
    pf_transition = pf_deferred = 0;
    pf_reads = pf_loads = pf_parses = pf_ctors = pf_dtors = pf_closes = pf_plans = pf_frees = 0;
    pf_activations = pf_restorations = pf_order_error = 0; pf_parsed[0] = 0;
    pf_coverage = pf_ready = 1; pf_gate_calls = pf_availability_calls = 0;
    pf_install_requests = pf_inventory_reads = pf_cancellations = pf_commits = pf_aborts = 0;
    pf_install_outcome = 2; pf_completion = NULL; pf_completion_context = NULL;
}
static void saved_map_preflight_retains_checked_bytes(void)
{
    unsigned char name[IDSTR_SIZE] = {0};
    char source[MAX_PATH];
    native_saved_cancellation_uses_owned_editor();
    *(const char **)(name + 0x10) = "0123456789ABCDEF0123";
    clean_state(); sh_rawmap_swap_arm(0); InterlockedExchange(&g_swap_oneshot, 0);
    /* Prevent any author-owned arm.flag beside the real default source from
     * influencing the synthetic preflight. No source file is created here. */
    CHECK(temp_path("rawmap_preflight_source.json", source, sizeof(source)));
    sh_rawmap_swap_set_source(source);
    g_preflight_orig = pf_loader; g_saved_map_text = pf_reader;
    g_preflight_abort = pf_abort;
    g_idstr_ctor = pf_ctor; g_idstr_dtor = pf_dtor; g_deser_orig = pf_parser;
    pf_reset(); CHECK(preflight_load((void *)3, name) == 0);
    CHECK(pf_reads == 1 && pf_loads == 1 && pf_parses == 1 && pf_ctors == 1 && pf_dtors == 1);
    CHECK(!strcmp(pf_parsed, "{\"from\":\"prepared\"}"));
    CHECK(!pf_plans && !pf_active && pf_closes == 1 && !pf_order_error);
    for (int failure = 1; failure <= 6; failure++) {
        pf_reset(); pf_packages = 1; pf_fail = failure;
        if (failure == 4) { pf_fail = 0; pf_gate = 0; pf_coverage = 0; }
        CHECK(preflight_load((void *)3, name) == 0);
        CHECK(!pf_loads && !pf_parses && !pf_active && !g_map_context);
        CHECK(pf_ctors == 1 && pf_dtors == 1 && !pf_order_error);
    }
    pf_reset(); pf_packages = 1;
    pf_gate = 0; /* Changed package contents would fail the old exact gate. */
    CHECK(preflight_load((void *)3, name) == 0);
    CHECK(!pf_gate_calls && pf_availability_calls == 1);
    CHECK(pf_active && g_map_context_active && g_map_context);
    CHECK(pf_plans == 1 && pf_frees == 1 && pf_activations == 1 && !pf_closes);
    sh_rawmap_map_context_poll(); CHECK(!pf_restorations && !pf_closes);
    pf_boundary = 1; pf_fail = 4; sh_rawmap_map_context_poll();
    CHECK(pf_active && g_map_context_active && !pf_closes);
    pf_fail = 0; pf_held = 1; sh_rawmap_map_context_poll();
    CHECK(!pf_active && !g_map_context_active && !g_map_context && !pf_closes && pf_queued_cleanup == 1);
    pf_held = 0; sh_mpkg_context_collect(); CHECK(!g_map_context && pf_closes == 1 && !pf_order_error);
    for (int failure = 0; failure < 4; failure++) {
        pf_reset(); pf_packages = 1; pf_gate = 1;
        if (failure == 0) { pf_coverage = 0; pf_gate = 0; }
        else if (failure == 1) pf_coverage = -1;
        else if (failure == 2) pf_coverage = -2;
        else pf_ready = 0;
        CHECK(preflight_load((void *)3, name) == 0);
        CHECK(!pf_active && !pf_loads && !pf_parses && !pf_activations && !g_map_context);
        CHECK(pf_availability_calls == 1 && !pf_gate_calls && pf_install_requests == (failure == 0));
        CHECK(pf_closes == 1 && pf_frees == 1 && !pf_order_error);
    }
    for (int failure = 7; failure <= 8; failure++) {
        pf_reset(); pf_packages = 1; pf_fail = failure;
        CHECK(preflight_load((void *)3, name) == (failure == 7 ? 1 : 9));
        CHECK(pf_active && g_map_context && !g_preflight_busy && !g_preflight_json && pf_frees == 1);
        pf_fail = 0; pf_boundary = 1; sh_rawmap_map_context_poll();
        CHECK(!pf_active && !g_map_context && pf_closes == 1 && !pf_order_error);
    }
    /* Retain the exact JSON, save id and candidate across consent. The native
     * load occurs once after activation, with no second disk/source read. */
    pf_reset(); pf_packages = 1; pf_coverage = 0;
    CHECK(preflight_load((void *)3, name) == 0);
    CHECK(g_pending_map.state == 1 && !pf_loads && !pf_frees && !pf_closes && pf_completion);
    sh_rawmap_pending_map_poll(); CHECK(!pf_inventory_reads && !pf_loads);
    pf_completion(pf_completion_context, 1); pf_completion = NULL;
    pf_boundary = 0; sh_rawmap_pending_map_poll(); CHECK(!pf_inventory_reads);
    pf_boundary = 1; sh_rawmap_pending_map_poll();
    CHECK(!g_pending_map.state && pf_loads == 1 && pf_parses == 1 && pf_reads == 1);
    CHECK(pf_inventory_reads == 1 && pf_activations == 1 && pf_frees == 1 && pf_active);
    CHECK(pf_commits == 1 && !sh_rawmap_defers_install_commit());
    CHECK(pf_ctors == 2 && pf_dtors == 2 && !strcmp(pf_parsed, "{\"from\":\"prepared\"}"));
    sh_rawmap_pending_map_poll(); CHECK(pf_loads == 1);
    pf_boundary = 1; sh_rawmap_map_context_poll(); CHECK(!pf_order_error && !g_map_context);
    /* Installation stays provisional through native parse failures, exceptions,
     * commit refusal and a zero-returning request that never loaded its map. */
    for (int scenario = 0; scenario < 4; scenario++) {
        pf_reset(); pf_packages = 1; pf_coverage = 0;
        CHECK(preflight_load((void *)3, name) == 0 && pf_completion);
        pf_completion(pf_completion_context, 1); pf_completion = NULL;
        pf_fail = scenario < 2 ? scenario + 7 : scenario == 2 ? 10 : 12;
        sh_rawmap_pending_map_poll();
        CHECK(!g_pending_map.state && !sh_rawmap_defers_install_commit());
        CHECK(pf_loads == 1 && pf_commits == (scenario == 2));
        CHECK(pf_aborts == 1 && pf_cancellations && !pf_active && !g_map_context);
        CHECK(pf_restorations == 1 && pf_closes == 1 && !pf_order_error);
    }
    /* Decline, publication failure, lost boundary, invalid caller and failed
     * preparation/activation all cancel without entering native map loading. */
    for (int scenario = 0; scenario < 7; scenario++) {
        pf_reset(); pf_packages = 1; pf_coverage = 0;
        CHECK(preflight_load((void *)3, name) == 0 && pf_completion);
        pf_completion(pf_completion_context, scenario < 2 ? -scenario : 1); pf_completion = NULL;
        if (scenario == 2) { pf_boundary = 0; g_pending_map.published_at = GetTickCount64() - 30001; }
        if (scenario == 3) g_pending_map.editor = (void *)4;
        if (scenario == 4) g_pending_map.thread++;
        if (scenario == 5) pf_fail = 9;
        if (scenario == 6) pf_fail = 3;
        sh_rawmap_pending_map_poll();
        CHECK(!g_pending_map.state && !pf_loads && !pf_parses && !pf_active);
        CHECK(pf_cancellations && pf_frees == 1 && pf_closes == 1 && !pf_order_error);
    }
    /* A new explicit load withdraws the old request. Consent for an abandoned
     * map can never install or load it afterward. */
    pf_reset(); pf_packages = 1; pf_coverage = 0;
    CHECK(preflight_load((void *)3, name) == 0 && pf_completion);
    pf_packages = 0; CHECK(preflight_load((void *)3, name) == 0);
    CHECK(!pf_completion && !g_pending_map.state && pf_loads == 1 && pf_closes == 2);
    pf_reset(); pf_testing = 0;
    g_preflight_orig = NULL; g_saved_map_text = NULL; g_deser_orig = NULL;
    g_preflight_abort = preflight_abort_saved_load;
    g_idstr_ctor = NULL; g_idstr_dtor = NULL; clean_state();
}

typedef struct pending_owner {
    int valid, entries, releases, validity_fault, entry_fault;
    int delayed;
} pending_owner;
static int request_valid(void *context)
{
    pending_owner *owner = context;
    CHECK(owner->releases == 0);
    if (owner->validity_fault) RaiseException(0xe0422010, 0, 0, NULL);
    return owner->valid;
}
static int request_enter(void *context, const char *json)
{
    pending_owner *owner = context;
    CHECK(!owner->releases && owner->valid && (!pf_packages || pf_active));
    owner->entries++; pf_loads++; if (!owner->delayed) pf_boundary = 0;
    if (owner->entry_fault) RaiseException(0xe0422011, 0, 0, NULL);
    CHECK(json == g_preflight_json);
    return sh_deser_detour("{\"from\":\"changed cache\"}", (void *)2) ? 0 : 9;
}
static void request_release(void *context)
{
    pending_owner *owner = context;
    CHECK(!owner->releases); owner->releases++;
}
static void owned_map_requests_survive_consent(void)
{
    pending_owner owner;
    sh_rawmap_request request = {&owner, request_valid, request_enter, request_release};
    char body[128], error[512], source[MAX_PATH];
    clean_state(); sh_rawmap_swap_arm(0); InterlockedExchange(&g_swap_oneshot, 0);
    CHECK(temp_path("rawmap_owned_source.json", source, sizeof(source)));
    sh_rawmap_swap_set_source(source); g_deser_orig = pf_parser;
    for (int missing = 0; missing < 2; missing++) {
        pf_reset(); pf_packages = 1; pf_coverage = !missing;
        memset(&owner, 0, sizeof(owner)); owner.valid = 1;
        strcpy_s(body, sizeof(body), "{\"from\":\"owned snapshot\"}");
        CHECK(sh_rawmap_preflight_request(body, strlen(body), &request, error, sizeof(error)));
        memset(body, 'x', strlen(body));
        CHECK(!owner.entries && !owner.releases && !pf_activations);
        if (missing) {
            sh_rawmap_pending_map_poll(); CHECK(!owner.entries && pf_completion);
            pf_completion(pf_completion_context, 1); pf_completion = NULL;
        }
        sh_rawmap_pending_map_poll();
        CHECK(owner.entries == 1 && owner.releases == 1 && !g_pending_map.state);
        CHECK(pf_inventory_reads == 1 && pf_activations == 1 && pf_parses == 1);
        CHECK(!strcmp(pf_parsed, "{\"from\":\"owned snapshot\"}"));
        CHECK(!pf_reads && !pf_ctors && !pf_dtors && pf_install_requests == missing);
        sh_rawmap_pending_map_poll(); CHECK(owner.entries == 1 && owner.releases == 1);
        pf_boundary = 1; sh_rawmap_map_context_poll(); CHECK(!pf_order_error && !g_map_context);
    }
    /* Native download completion can pump maintenance before its inspection
     * scope unwinds. Retained launches must wait, then consume their own source. */
    pf_reset(); pf_packages = 1;
    memset(&owner, 0, sizeof(owner)); owner.valid = 1;
    sh_rawmap_inspection_enter(); sh_rawmap_inspection_enter();
    CHECK(sh_rawmap_preflight_request("{\"from\":\"owned snapshot\"}", 25, &request, error, sizeof(error)));
    sh_rawmap_pending_map_poll();
    CHECK(!owner.entries && !owner.releases && !pf_activations && !pf_parses);
    sh_rawmap_inspection_leave(); sh_rawmap_pending_map_poll();
    CHECK(!owner.entries && !owner.releases && !pf_activations && !pf_parses);
    sh_rawmap_inspection_leave(); sh_rawmap_pending_map_poll();
    CHECK(owner.entries == 1 && owner.releases == 1 && pf_activations == 1 && pf_parses == 1);
    CHECK(!strcmp(pf_parsed, "{\"from\":\"owned snapshot\"}"));
    pf_boundary = 1; sh_rawmap_map_context_poll(); CHECK(!g_map_context && !pf_order_error);
    /* Cancellation while consent is pending or after publication, stale owner,
     * native validity fault and failed activation never enter the map. */
    for (int failure = 0; failure < 7; failure++) {
        pf_reset(); pf_packages = 1; pf_coverage = 0;
        memset(&owner, 0, sizeof(owner)); owner.valid = 1;
        CHECK(sh_rawmap_preflight_request("{}", 2, &request, error, sizeof(error)));
        if (failure > 0) {
            pf_completion(pf_completion_context, failure == 1 ? 0 : failure == 2 ? -1 : 1);
            pf_completion = NULL;
        }
        if (failure == 0 || failure == 3) owner.valid = 0;
        if (failure == 4) owner.validity_fault = 1;
        if (failure == 5) pf_fail = 3;
        if (failure == 6) pf_fail = 9;
        sh_rawmap_pending_map_poll();
        CHECK(!owner.entries && owner.releases == 1 && !g_pending_map.state);
        CHECK(!pf_completion && !pf_active && pf_cancellations && !pf_order_error);
    }
    /* A successful native request can queue its transition without leaving the
     * browser in this call. Repeated menu ticks must not retire the new map;
     * a lobby is an activation surface, but not a map-retirement surface. */
    pf_reset(); pf_packages = 1;
    memset(&owner, 0, sizeof(owner)); owner.valid = owner.delayed = 1;
    CHECK(sh_rawmap_preflight_request("{}", 2, &request, error, sizeof(error)));
    sh_rawmap_pending_map_poll(); CHECK(pf_active && g_map_context_await_departure);
    for (int tick = 0; tick < 20; tick++) sh_rawmap_map_context_poll();
    CHECK(pf_active && !pf_restorations && g_map_context);
    pf_lobby = 1; sh_rawmap_map_context_poll();
    CHECK(pf_active && !g_map_context_await_departure && !pf_restorations);
    pf_boundary = 0; sh_rawmap_map_context_poll(); CHECK(pf_active);
    pf_boundary = 1; pf_lobby = 0; sh_rawmap_map_context_poll();
    CHECK(!pf_active && !g_map_context && pf_restorations == 1);
    /* Refusal transfers nothing, including synchronous service refusal. */
    pf_reset(); pf_packages = 1; pf_coverage = 0;
    memset(&owner, 0, sizeof(owner)); owner.valid = 1;
    CHECK(sh_rawmap_preflight_request("{}", 2, &request, error, sizeof(error)));
    pf_held = 1; CHECK(sh_rawmap_cancel_pending_map());
    CHECK(!pf_completion && owner.releases == 1 && !owner.entries && pf_queued_cleanup == 1);
    pf_held = 0; sh_mpkg_context_collect(); CHECK(!pf_queued_cleanup && sh_rawmap_cancel_pending_map());
    CHECK(owner.releases == 1 && !g_pending_map.state && !pf_active);
    CHECK(sh_rawmap_cancel_pending_map() && owner.releases == 1);
    for (int failure = 0; failure < 5; failure++) {
        pf_reset(); pf_packages = 1;
        memset(&owner, 0, sizeof(owner)); owner.valid = 1;
        if (failure == 0) owner.valid = 0;
        if (failure == 1) owner.validity_fault = 1;
        if (failure == 2) pf_boundary = 0;
        if (failure == 3) { pf_gate = 0; pf_coverage = 0; }
        if (failure == 4) pf_ready = 0;
        CHECK(!sh_rawmap_preflight_request("{}", 2, &request, error, sizeof(error)));
        CHECK(!owner.entries && !owner.releases && !g_pending_map.state && !pf_active);
    }
    /* A native entry fault releases the owner once. Source storage stays held
     * until the active provider can retire. */
    pf_reset(); pf_packages = 1;
    memset(&owner, 0, sizeof(owner)); owner.valid = owner.entry_fault = 1;
    CHECK(sh_rawmap_preflight_request("{}", 2, &request, error, sizeof(error)));
    sh_rawmap_pending_map_poll(); CHECK(owner.entries == 1 && owner.releases == 1 && pf_active);
    pf_boundary = 1; sh_rawmap_map_context_poll(); CHECK(!g_map_context && !pf_order_error);
    /* Replacement releases only the abandoned owner. Its consent cannot carry
     * forward to a new vanilla request. */
    {
        pending_owner second = {1};
        sh_rawmap_request next = {&second, request_valid, request_enter, request_release};
        pf_reset(); pf_packages = 1; pf_coverage = 0;
        memset(&owner, 0, sizeof(owner)); owner.valid = 1;
        CHECK(sh_rawmap_preflight_request("{}", 2, &request, error, sizeof(error)));
        pf_packages = 0;
        CHECK(sh_rawmap_preflight_request("{}", 2, &next, error, sizeof(error)));
        CHECK(owner.releases == 1 && !owner.entries && !pf_completion);
        sh_rawmap_pending_map_poll();
        CHECK(second.entries == 1 && second.releases == 1 && !pf_active && !g_map_context);
    }
    pf_reset(); pf_testing = 0; g_deser_orig = NULL; clean_state();
}

typedef struct deferred_owner { int valid, waiting, selected, entered, released, synchronous, fail_select; } deferred_owner;
static int deferred_valid(void *context) { return ((deferred_owner *)context)->valid; }
static int deferred_waiting(void *context) { return ((deferred_owner *)context)->waiting; }
static int deferred_matches(void *context, const void *parameters)
{ (void)context; return parameters == (void *)0x77; }
static int deferred_select(void *context)
{
    deferred_owner *owner = context;
    CHECK(pf_transition && g_preflight_busy && !owner->released);
    owner->selected++; return !owner->fail_select;
}
static void deferred_release(void *context)
{
    deferred_owner *owner = context;
    CHECK(!g_preflight_busy && !pf_transition); owner->released++;
}
static int deferred_enter(void *context, const char *json)
{
    deferred_owner *owner = context;
    CHECK(!strcmp(json, "{}") && !pf_activations && !pf_restorations && !owner->released);
    owner->entered++;
    if (owner->synchronous) {
        CHECK(sh_rawmap_transition_pending(NULL, (void *)0x77) == 1);
        pf_transition = 1; CHECK(sh_rawmap_transition_activate()); pf_transition = 0;
        CHECK(sh_rawmap_transition_commit());
        sh_rawmap_transition_finished(1);
        CHECK(g_preflight_busy && !owner->released);
    }
    return 0;
}
static void deferred_requests_publish_at_native_boundary(void)
{
    deferred_owner owner;
    sh_rawmap_request request = {&owner, deferred_valid, deferred_enter, deferred_release};
    sh_rawmap_loading loading = {deferred_waiting, deferred_matches, deferred_select};
    char error[256];
    g_deser_orig = pf_parser;
    for (int synchronous = 0; synchronous < 2; synchronous++) {
        unsigned long generation = sh_rawmap_load_generation();
        pf_reset(); pf_deferred = 1; pf_boundary = 0; pf_packages = 1;
        /* Existing map remains live throughout compilation and lobby waiting. */
        g_map_context = (sh_mpkg_context *)2; g_map_context_active = pf_active = 1;
        memset(&owner, 0, sizeof(owner)); owner.valid = owner.waiting = 1;
        owner.synchronous = synchronous;
        CHECK(sh_rawmap_preflight_loading_request("{}", 2, &request, &loading, error, sizeof(error)));
        CHECK(sh_rawmap_defers_install_commit());
        CHECK(g_map_context == (sh_mpkg_context *)2 && !pf_restorations && !pf_activations);
        CHECK(sh_rawmap_load_generation() == generation && !owner.entered);
        sh_rawmap_pending_map_poll();
        CHECK(owner.entered == 1);
        if (!synchronous) {
            CHECK(!owner.released && !pf_activations && g_pending_map.state == 4);
            owner.valid = 0; /* Browser selection may close while lobby owns it. */
            g_pending_map.published_at = 1;
            for (int i = 0; i < 40; i++) sh_rawmap_pending_map_poll();
            CHECK(!owner.released && !pf_activations && sh_rawmap_load_generation() == generation);
            CHECK(!sh_rawmap_transition_pending(NULL, (void *)0x88));
            CHECK(sh_rawmap_transition_pending(NULL, (void *)0x77) == 1);
            CHECK(!sh_rawmap_cancel_pending_map());
            CHECK(!sh_rawmap_transition_activate());
            pf_transition = 1; CHECK(sh_rawmap_transition_activate()); pf_transition = 0;
            CHECK(!owner.released && owner.selected == 1 && g_map_context == (sh_mpkg_context *)1);
            CHECK(sh_rawmap_transition_commit());
            sh_rawmap_transition_finished(1);
        }
        CHECK(owner.released == 1 && !g_pending_map.state && !g_preflight_busy);
        CHECK(!sh_rawmap_defers_install_commit());
        CHECK(pf_activations == 1 && !pf_restorations && !pf_order_error);
        CHECK(sh_rawmap_load_generation() == generation + 1);
        pf_boundary = 1; sh_rawmap_map_context_poll();
        CHECK(!g_map_context && !pf_active);
    }
    /* Missing-resource consent is decided without replacing the old provider.
     * Decline/abandonment frees only pending sources. */
    for (int at = 0; at < 2; at++) {
        pf_reset(); pf_deferred = 1; pf_boundary = 0; pf_packages = 1; pf_coverage = 0;
        g_map_context = (sh_mpkg_context *)2; g_map_context_active = pf_active = 1;
        memset(&owner, 0, sizeof(owner)); owner.valid = owner.waiting = 1;
        CHECK(sh_rawmap_preflight_loading_request("{}", 2, &request, &loading, error, sizeof(error)));
        CHECK(pf_install_requests == 1 && !pf_activations);
        pf_completion(pf_completion_context, at); pf_completion = NULL;
        if (at) { sh_rawmap_pending_map_poll(); owner.waiting = 0; }
        sh_rawmap_pending_map_poll();
        CHECK(owner.released == 1 && g_map_context == (sh_mpkg_context *)2 && pf_active);
        CHECK(!pf_activations && !pf_restorations && !g_pending_map.state);
        pf_boundary = 1; sh_rawmap_map_context_poll(); CHECK(!g_map_context);
    }
    /* Refused activation leaves the previous provider owned until native
     * cancellation has released the world and reaches a retirement boundary. */
    pf_reset(); pf_deferred = 1; pf_boundary = 0; pf_packages = 1;
    g_map_context = (sh_mpkg_context *)2; g_map_context_active = pf_active = 1;
    memset(&owner, 0, sizeof(owner)); owner.valid = owner.waiting = 1;
    CHECK(sh_rawmap_preflight_loading_request("{}", 2, &request, &loading, error, sizeof(error)));
    sh_rawmap_pending_map_poll();
    CHECK(sh_rawmap_transition_pending(NULL, (void *)0x77) == 1);
    pf_fail = 3; pf_transition = 1; CHECK(!sh_rawmap_transition_activate()); pf_transition = 0;
    CHECK(g_map_context == (sh_mpkg_context *)2 && pf_active);
    sh_rawmap_transition_finished(0); CHECK(owner.released == 1 && !g_preflight_busy);
    pf_fail = 0; pf_boundary = 1; sh_rawmap_map_context_poll(); CHECK(!g_map_context);
    pf_reset(); pf_testing = 0; g_deser_orig = NULL;
}

static int inspection_parse(const char *json, void *map)
{
    CHECK(map == (void *)2 && !strcmp(json, "{}")); inspection_parses++; return 1;
}
static DWORD WINAPI inspection_other_thread(void *unused)
{ (void)unused; CHECK(g_inspection_depth == 0 && !g_initial_read); return 0; }
int sh_map_native_read_session(const char *json, void *map, char *error, size_t capacity)
{
    CHECK(map == (void *)2 && !strcmp(json, "{\"delivery fixture\":true}"));
    CHECK(g_initial_read && !g_initial_read->return_address && g_inspection_depth);
    initial_read_parses++;
    if (initial_read_mode == 2) RaiseException(0xe0427900, 0, 0, NULL);
    if (initial_read_mode == 1) { snprintf(error, capacity, "session refused"); return 0; }
    /* Simulate the native adapter's inner parse, including a coincident caller.
     * The admission is already spent, so only the full ordinary parser runs. */
    return inspection_decode("{}", map, (void *)0x790);
}
static void initial_published_reads(void)
{
    sh_rawmap_read_scope outer, nested;
    g_deser_orig = inspection_parse;
    inspection_parses = initial_read_parses = 0;
    sh_rawmap_read_enter(&outer, (void *)0x790);
    inspection_packages = 1;
    CHECK(inspection_decode("{\"delivery fixture\":true}", (void *)2, (void *)0x791));
    CHECK(!initial_read_parses && outer.return_address == (void *)0x790);
    sh_rawmap_read_enter(&nested, (void *)0x792);
    CHECK(inspection_decode("{\"delivery fixture\":true}", (void *)2, (void *)0x792));
    CHECK(initial_read_parses == 1 && !nested.return_address);
    sh_rawmap_read_leave(&nested);
    CHECK(g_initial_read == &outer && g_inspection_depth == 1);
    CHECK(inspection_decode("{\"delivery fixture\":true}", (void *)2, (void *)0x790));
    CHECK(initial_read_parses == 2 && !outer.return_address);
    CHECK(inspection_decode("{\"delivery fixture\":true}", (void *)2, (void *)0x790));
    CHECK(initial_read_parses == 2 && inspection_parses == 4);
    sh_rawmap_read_leave(&outer); CHECK(!g_initial_read && !g_inspection_depth);
    /* Vanilla's first read consumes admission without a session replacement. */
    sh_rawmap_read_enter(&outer, (void *)0x790); inspection_packages = 0;
    CHECK(inspection_decode("{}", (void *)2, (void *)0x790));
    inspection_packages = 1;
    CHECK(inspection_decode("{\"delivery fixture\":true}", (void *)2, (void *)0x790));
    CHECK(initial_read_parses == 2);
    sh_rawmap_read_leave(&outer);
    for (initial_read_mode = 1; initial_read_mode <= 2; initial_read_mode++) {
        int before = inspection_parses;
        sh_rawmap_read_enter(&outer, (void *)0x790);
        CHECK(!inspection_decode("{\"delivery fixture\":true}", (void *)2, (void *)0x790));
        CHECK(!outer.return_address && inspection_parses == before);
        sh_rawmap_read_leave(&outer); CHECK(!g_initial_read && !g_inspection_depth);
    }
    initial_read_mode = 0;
    sh_rawmap_read_enter(&outer, NULL);
    CHECK(inspection_decode("{\"delivery fixture\":true}", (void *)2, NULL));
    CHECK(initial_read_parses == 4);
    sh_rawmap_read_leave(&outer);
    inspection_packages = inspection_parses = 0; g_deser_orig = NULL;
}
static void inspection_preserves_active_map_state(void)
{
    unsigned long generation = sh_rawmap_load_generation();
    unsigned long swaps = sh_rawmap_swap_count();
    int selections = play_selections, rendered = render_loaded_calls;
    int installs = pf_install_requests;
    g_deser_orig = inspection_parse;
    sh_rawmap_load_arm_once();
    sh_rawmap_inspection_enter(); sh_rawmap_inspection_enter();
    {
        unsigned char text[IDSTR_SIZE] = {0};
        int serialized = live_serializations;
        g_ser_orig = fixture_serialize;
        CHECK(sh_ser_detour((void *)1, text, 1));
        CHECK(live_serializations == serialized + 1);
        g_ser_orig = NULL;
    }
    CHECK(sh_deser_detour("{}", (void *)2) == 1);
    inspection_packages = 1;
    CHECK(sh_deser_detour("{\"delivery fixture\":true}", (void *)2) == 1);
    inspection_strip_fail = 1;
    CHECK(sh_deser_detour("{\"delivery fixture\":true}", (void *)2) == 0);
    CHECK(inspection_parses == 2);
    sh_rawmap_inspection_leave();
    CHECK(g_inspection_depth == 1);
    {
        HANDLE thread = CreateThread(NULL, 0, inspection_other_thread, NULL, 0, NULL);
        CHECK(thread && WaitForSingleObject(thread, 10000) == WAIT_OBJECT_0); if (thread) CloseHandle(thread);
    }
    sh_rawmap_inspection_leave(); CHECK(!g_inspection_depth);
    CHECK(sh_rawmap_load_oneshot_pending() && sh_rawmap_load_generation() == generation);
    CHECK(sh_rawmap_swap_count() == swaps && play_selections == selections);
    CHECK(render_loaded_calls == rendered && pf_install_requests == installs);
    inspection_packages = inspection_strip_fail = 0;
    InterlockedExchange(&g_swap_oneshot, 0); g_deser_orig = NULL;
}

static int pf_publication_faults;
static void pf_publication(void *target)
{
    (void)target;
    if (!g_preflight_orig || !g_saved_map_text || g_preflight_orig(NULL, NULL) != 42) pf_publication_faults++;
}
static void preflight_hook_publishes_helpers_before_detour(void)
{
    unsigned char *code = VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    int before = hook_owned_count();
    CHECK(code); if (!code) return;
    memset(code, 0x90, 36); code[36] = 0xb8; code[37] = 42;
    memset(code + 38, 0, 3); code[41] = 0xc3;
    FlushInstructionCache(GetCurrentProcess(), code, 64);
    g_deser_orig = pf_parser; g_idstr_ctor = pf_ctor; g_idstr_dtor = pf_dtor;
    CHECK(!sh_rawmap_preflight_install(code, 0, pf_reader, 1));
    CHECK(!sh_rawmap_preflight_install(code, 1, pf_reader, 0));
    CHECK(hook_owned_count() == before);
    sh_patch_test_observe_write(pf_publication); sh_patch_test_faults(2 | 4 | 8, 0, 0);
    CHECK(!sh_rawmap_preflight_install(code, 1, pf_reader, 1));
    CHECK(g_preflight_orig && !hook_is_installed((void *)g_preflight_orig));
    CHECK(hook_owned_count() == before + 1);
    sh_patch_test_faults(1, 0, 0);
    CHECK(!sh_rawmap_preflight_install(code, 1, pf_reader, 1) && g_preflight_orig);
    sh_patch_test_faults(0, 0, 0);
    CHECK(sh_rawmap_preflight_install(code, 1, pf_reader, 1));
    CHECK(sh_rawmap_preflight_install(NULL, 0, NULL, 0) && !pf_publication_faults);
    CHECK(hook_unpatch((void *)g_preflight_orig));
    CHECK(hook_owned_count() == before);
    g_preflight_orig = NULL; g_saved_map_text = NULL; g_deser_orig = NULL;
    g_idstr_ctor = NULL; g_idstr_dtor = NULL;
    sh_patch_test_observe_write(NULL); VirtualFree(code, 0, MEM_RELEASE);
}

int main(void)
{
    char archive[MAX_PATH];

    printf("rawmap_paths_test\n");
    package_gate_fault = 1;
    CHECK(!mpkg_gate_guarded("{}"));
    CHECK(package_errors == 1);
    package_gate_fault = 0;

    if (!make_json("rawmap_test_archive.json", archive, sizeof archive)) {
        fprintf(stderr, "could not create the temp fixture\n");
        return 1;
    }

    a_chosen_file_stays_chosen();
    a_failed_write_keeps_its_destination();
    opening_a_rawmap_never_aims_saves_at_it(archive);
    choosing_the_opened_rawmap_works(archive);
    a_different_map_drops_the_target();
    a_bare_word_is_refused();
    picking_the_usual_file_holds_nothing();
    picking_the_usual_file_releases_a_chosen_one();
    are_default_answers_both_ways(archive);
    looks_like_rawmap_tells_them_apart();
    refused_menu_save_changes_nothing();
    missing_overwrite_protection_refuses_staging(archive);
    an_existing_scratch_file_survives();
    a_live_export_mirrors_once();
    snapshots_visit_without_save_effects();
    large_sources_use_native_lengths();
    play_uses_current_snapshot_without_saving();
    play_hook_publishes_original_before_detour();
    saved_map_preflight_retains_checked_bytes();
    owned_map_requests_survive_consent();
    deferred_requests_publish_at_native_boundary();
    inspection_preserves_active_map_state();
    initial_published_reads();
    preflight_hook_publishes_helpers_before_detour();

    DeleteFileA(archive);
    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
