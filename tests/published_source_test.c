/* The published-map source contract across its real boundaries (M130, root-published-source-contract.md): a
 * published launch through the production launch bridge (map_published.c), its native selection and cached-source
 * reader (map_native.c, map_source.c), the retained request, consent and deferred native transition (rawmap.c), the
 * private map context, installation and activation service (map_package.c), the document bind (package_document.c),
 * the map's prepared resource graph and the package compiler/runtime. Only the engine is substituted: published-map
 * objects and caches, native constructors and the local-heap stack, the virtual filesystem behind the cached source,
 * the engine's own JSON parser behind rawmap's DeserializeFromJson detour, native declaration registration, the native
 * state evaluator the R137 conversion consults, map/editor transition predicates, navigation, render settings, the HUD
 * and the modal dialog. The published documents are the historical writers' retained bodies
 * (map_package_historical_fixtures.h), byte for byte, with the native members every cached published map carries,
 * one same-identity variant written by the current writer, and the R137 load positive's convertible and mixed uses.
 * Launches cover offline, direct, hosting a lobby and a joining player's readiness for the host's map. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rawmap.c"
#include "map_native.c"
#include "map_source.c"
#include "map_published.c"
#include "package_runtime.h"
#include "package_editor.h"
#include "package_sources.h"
#include "package_archive.h"
#include "package_fixture.h"
#include "document_fixture.h"
#include "map_package_historical_fixtures.h"
#include "editor_frame.h"
#include "editor_transition.h"
#include "map_transition.h"
#include "nav_bake.h"
#include "navmesh.h"
#include "prefab_import.h"
#include "weapon_hud.h"
#include "cvars.h"
#include "engine_dialog.h"
#include "decl_tree.h"
#include "typeinfo.h"
#include "overrides.h"
#include "patch.h"

void backend_log(const char *message)
{ if (message && getenv("SH_PUBLISHED_TEST_LOG")) fprintf(stderr, "log: %s\n", message); }
int sh_user_overrides_enabled_for_launch(void) { return 1; }

/* ---- The engine. Nothing below is product code. ---- */
static DWORD main_thread;
static char data_root[MAX_PATH];
static admission_fixture registry_fixture;

/* No native code is bound or patched here: the product's statics take the engine's functions directly. */
const sig_entry BACKEND_ENGINE_SIGNATURES[] = { { NULL, NULL, 0u } };
sig_status sig_resolve_one(const uint8_t *module_base, const sig_entry *sig, sig_result *out)
{ (void)module_base; (void)sig; (void)out; return SIG_NOT_FOUND; }
uintptr_t glb_resolve(const uint8_t *module_base, const char *name, glb_status *out_status)
{ (void)module_base; (void)name; if (out_status) *out_status = GLB_UNKNOWN_NAME; return 0; }
void *hook_prepare(void *target, void *detour, size_t stolen) { (void)target; (void)detour; (void)stolen; return NULL; }
void *hook_prepare_relative_call(void *target, void *detour, size_t stolen, size_t call_offset)
{ (void)target; (void)detour; (void)stolen; (void)call_offset; return NULL; }
void *hook_prepare_rip_lea(void *target, void *detour, size_t stolen, size_t lea_offset)
{ (void)target; (void)detour; (void)stolen; (void)lea_offset; return NULL; }
int hook_target_is_redirect(const void *target) { (void)target; return 0; }
sh_patch_status hook_commit(void *trampoline) { (void)trampoline; return B2_PATCH_REFUSED_BADARG; }
int hook_unpatch(void *trampoline) { (void)trampoline; return 0; }
int hook_is_installed(void *trampoline) { return trampoline != NULL; }
sh_patch_status code_unpatch(sh_patch_handle *handle) { (void)handle; CHECK(0); return B2_PATCH_FAIL_NOTLIVE; }
sh_patch_status code_patch_sig(const sig_result *r, const uint8_t *expect, const uint8_t *new_bytes,
    size_t len, sh_patch_handle *out_handle)
{ (void)r; (void)expect; (void)new_bytes; (void)len; (void)out_handle; CHECK(0); return B2_PATCH_REFUSED_BADARG; }
int sh_cvar_value_int(int index, int def) { (void)index; return def; }

/* The local-heap stack (MemLocalGet/Push/Pop): heap ids at +0x44, depth at +0xc4. */
static struct { unsigned char prefix[0x44]; int heaps[32], depth; } engine_heap;
static void *heap_get(void) { return &engine_heap; }
static void heap_push(void *self, int heap)
{ CHECK(self == &engine_heap && !heap && GetCurrentThreadId() == main_thread); engine_heap.heaps[engine_heap.depth++] = 0; }
static void heap_pop(void *self) { CHECK(self == &engine_heap && engine_heap.depth > 1); engine_heap.depth--; }

/* idStr: length at +8, body at +0x10. */
static int engine_strings;
static void *string_ctor(void *out)
{
    memset(out, 0, MAP_STRING_SIZE); engine_strings++;
    *(char **)((char *)out + 0x10) = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 1);
    return out;
}
static void string_set(void *out, const char *value, size_t length)
{
    char *body = HeapAlloc(GetProcessHeap(), 0, length + 1);
    if (!body) { CHECK(0); return; }
    memcpy(body, value, length); body[length] = 0;
    HeapFree(GetProcessHeap(), 0, *(void **)((char *)out + 0x10));
    *(char **)((char *)out + 0x10) = body; *(int *)((char *)out + 8) = (int)length;
}
static void string_free(void *out)
{
    HeapFree(GetProcessHeap(), 0, *(void **)((char *)out + 0x10));
    *(void **)((char *)out + 0x10) = NULL; engine_strings--;
}
static const char *string_body(const void *string) { return *(char *const *)((const char *)string + 0x10); }

/* Published-map metadata: id, revision, title and the map name its level parameters carry. */
static const size_t metadata_strings[] = {0, 0x30, 0x60, 0x398};
static int metadata_copies, metadata_frees;
static void *metadata_copy(void *out, const void *in)
{
    CHECK(engine_heap.depth == 2);
    memset(out, 0, MAP_METADATA_SIZE);
    for (size_t i = 0; i < 4; i++) {
        const unsigned char *from = (const unsigned char *)in + metadata_strings[i];
        string_ctor((char *)out + metadata_strings[i]);
        string_set((char *)out + metadata_strings[i], string_body(from), (size_t)*(const int *)(from + 8));
    }
    metadata_copies++; return out;
}
static void metadata_free(void *out)
{
    for (size_t i = 0; i < 4; i++) string_free((char *)out + metadata_strings[i]);
    metadata_frees++;
}
/* The cached source of a published map, by its id. */
static void build_path(const void *metadata, void *out)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/published/%s_map.decl", root, string_body(metadata));
    string_set(out, path, strlen(path));
}
static int engine_snapshots;
static void *snapshot_ctor(void *out) { memset(out, 0, MAP_SNAPSHOT_SIZE); *(int *)out = 0x6f; engine_snapshots++; return out; }
static void snapshot_free(void *out) { CHECK(*(int *)out == 0x6f); engine_snapshots--; }
static unsigned char serialize_map(const void *map, void *out, unsigned char compact)
{
    static const char defaults[] = "{\"~type\":\"idSnapMap\",\"~version\":110}";
    CHECK(map && compact); string_set(out, defaults, sizeof(defaults) - 1); return 1;
}

/* The engine's own parser behind rawmap's detour: what the engine finally decodes. */
static char *engine_decoded;
static size_t engine_decoded_length;
static int engine_decodes, gameplay_decodes;
static int engine_parse(const char *json, void *out_map)
{
    size_t length = strlen(json);
    CHECK(out_map && *(int *)out_map == 0x6f && GetCurrentThreadId() == main_thread);
    engine_decodes++;
    if (strstr(json, "\"entities\"")) {
        gameplay_decodes++;
        free(engine_decoded); engine_decoded = malloc(length + 1);
        if (engine_decoded) { memcpy(engine_decoded, json, length + 1); engine_decoded_length = length; }
    }
    return 1;
}

/* The virtual filesystem (size, ReadFile into an owned native buffer with its terminator) over the real files. */
static void *fs_table[0x140 / 8], *memory_table[4];
static void **fs_object = fs_table, **memory_object = memory_table;
static void *files_slot = &fs_object, *memory_slot = &memory_object;
static int fs_reads;
static int fs_size(void *self, const char *path, int flags)
{
    WIN32_FILE_ATTRIBUTE_DATA data;
    CHECK(self == &fs_object && flags == 4);
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data) || data.nFileSizeHigh) return 0;
    return (int)data.nFileSizeLow;
}
static void fs_read(void *self, const char *path, map_source_buffer *buffer, int a, int b)
{
    HANDLE file;
    DWORD length, read = 0;
    CHECK(self == &fs_object && !a && b == 4);
    memset(buffer, 0, sizeof(*buffer));
    file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    length = GetFileSize(file, NULL);
    buffer->data = HeapAlloc(GetProcessHeap(), 0, (size_t)length + 1);
    if (buffer->data && ReadFile(file, buffer->data, length, &read, NULL) && read == length) {
        buffer->data[length] = 0; buffer->length = (int)length + 1; buffer->owned = 1; fs_reads++;
    }
    CloseHandle(file);
}
static void memory_release(void *self, void *data, int tag)
{ CHECK(self == &memory_object && tag == 0x10); HeapFree(GetProcessHeap(), 0, data); }

/* The published-map browser: manager, shell, system, cache, the lobby history (records of 0x38 bytes, each naming a
 * history object whose metadata is at +8), the session whose lobby parameters name the hosted map's id (+0x550) and
 * revision (+0x600), and the launch originals. */
static unsigned char manager[0xa40], shell[0x20], cache[0x24470], level[0x17100], engine_snapshot[0x10];
static unsigned char history_record[0x38], history_object[8 + MAP_METADATA_SIZE], host_parameters[0x680];
static void *system_table[8], *cache_table[64], *session_table[104], *lobby_table[88], *progress_table[8];
static void **system_object = system_table, **session_object = session_table, **lobby_object = lobby_table;
static void **progress_object = progress_table;
static void *shell_slot = shell, *session_slot = &session_object, *history_slot = history_record;
static int history_count, cache_sets, offline_launches, direct_launches, lobby_launches, ready_launches;
static int render_selections, progress_hidden;
static int ready_download;
static int native_pass;   /* a package-free map the bridge is handing to the engine's own launch */
static void *get_system(void) { return &system_object; }
static void *get_cache(void *self) { CHECK(self == &system_object); return cache; }
static void *get_lobby(void *self) { CHECK(self == &session_object); return &lobby_object; }
static void *get_parameters(void *self) { CHECK(self == &lobby_object); return host_parameters; }
static int browser = 1;
static void host_names(const void *metadata)
{
    const unsigned char *id = metadata, *revision = id ? id + 0x30 : NULL;
    string_set(host_parameters + 0x550, id ? string_body(id) : "", id ? (size_t)*(const int *)(id + 8) : 0);
    string_set(host_parameters + 0x600, id ? string_body(revision) : "", id ? (size_t)*(const int *)(revision + 8) : 0);
}
/* A native launch: the browser closes while the session's lobby names the launched map. */
static void engine_launched(const void *metadata) { host_names(metadata); browser = 0; }
static void set_cache(void *self, const void *metadata, void *snapshot)
{
    CHECK(self == cache && metadata && snapshot);
    for (size_t at = 0; at <= 0x30; at += 0x30)
        string_set(cache + 0x24408 + at, string_body((const unsigned char *)metadata + at),
            (size_t)*(const int *)((const unsigned char *)metadata + at + 8));
    cache_sets++;
}
static void engine_offline(void *self, const void *metadata, void *snapshot)
{ CHECK(self == manager && metadata && snapshot && *(int *)snapshot == 0x6f); offline_launches++; engine_launched(metadata); }
static void engine_direct(void *self, const void *metadata, void *snapshot)
{ CHECK(self == manager && metadata && snapshot && *(int *)snapshot == 0x6f); direct_launches++; engine_launched(metadata); }
static unsigned char engine_lobby(void *self, unsigned char option)
{ CHECK(self == manager && option == 1); lobby_launches++; engine_launched(history_object + 8); return 1; }
/* A joining player's readiness for the host's map: the bridge passes no download request to the original it
 * retained; a package-free map's pass-through keeps the player's own. */
static void engine_ready(void *self, void *download, const void *metadata, void *snapshot)
{
    CHECK(self == manager && download == (native_pass ? &ready_download : NULL) && metadata && snapshot &&
        *(int *)snapshot == 0x6f);
    ready_launches++; engine_launched(metadata);
}
static void hide_progress(void *self, int shown) { CHECK(self == &progress_object && !shown); progress_hidden++; }

/* Boundaries the engine reports. */
static int at_boundary = 1, transition_boundary, retirement_safe = 1;
int sh_decl_server_map_boundary_safe(void) { return at_boundary && GetCurrentThreadId() == main_thread; }
int sh_decl_server_map_preparation_ready(void) { return at_boundary && GetCurrentThreadId() == main_thread; }
int sh_decl_server_map_browser_present(void) { return browser; }
int sh_decl_server_map_retirement_safe(void) { return retirement_safe; }
int sh_decl_server_map_editor_matches(const void *editor) { (void)editor; return 0; }
int sh_map_transition_ready(void) { return 1; }
int sh_map_transition_at_boundary(void) { return transition_boundary; }
int sh_snapshot_templates_install(void) { return 0; }
int sh_decl_server_registration_succeeded(void) { return 1; }
void sh_decl_server_request_rearm(void) {}
int sh_decl_server_registry_source(sh_decl_registry_source *source)
{ *source = (sh_decl_registry_source){&registry_fixture, admission_read, (uintptr_t)registry_fixture.registry, NULL}; return 1; }
void *sh_typeinfo_get_reflect(void) { return registry_fixture.reflection; }

/* Activation. The product's sh_decl_server_activate_map checks the boundary and calls sh_overrides_activate_map, which
 * activates the prepared plan through the runtime under the navigation guard with decl_server's native registration;
 * that registration (and decl_server's own state bookkeeping) is the engine edge substituted here. */
static int registration_refusals, registrations, restoring_registrations, activation_requests;
/* R-396: retirements of a departure the exit deferred, at the next open or level (not a map's activation). */
static int departure_withdrawals;
static sh_package_map_plan *activation_plan;
void sh_nav_bake_source_update_begin(void) {}
void sh_nav_bake_source_update_end(int committed) { (void)committed; }
static int engine_register(void *context, int restoring, const sh_package_changes *changes, char *error, size_t capacity)
{
    (void)context; (void)changes;
    if (restoring) { restoring_registrations++; return 1; }
    if (registration_refusals) {
        registration_refusals--; snprintf(error, capacity, "the engine refused the declaration registration"); return 0;
    }
    registrations++; return 1;
}
int sh_decl_server_activate_map(sh_package_map_plan *plan)
{
    if (!sh_decl_server_map_boundary_safe()) return 0;
    if (!plan && g_map_context_departed) departure_withdrawals++;
    else { activation_requests++; activation_plan = plan; }
    return sh_package_runtime_activate_prepared_map(plan, (sh_package_activation_guard){
        sh_nav_bake_source_update_begin, sh_nav_bake_source_update_end}, engine_register, NULL);
}
/* Retained editor resources and editor maps are not this route. */
int sh_decl_server_activate_occurrences(const sh_package_occurrence_snapshot *map) { (void)map; CHECK(0); return 0; }
static sh_package_occurrence_snapshot *editor_world;
int sh_package_editor_ready(void) { return 1; }
int sh_package_editor_healthy(char *error, size_t capacity) { (void)error; (void)capacity; return 1; }
sh_package_occurrence_snapshot *sh_package_editor_world(void) { return editor_world; }
void sh_package_editor_restore_world(sh_package_occurrence_snapshot *world) { editor_world = world; }
void sh_package_editor_clear_world(void) { editor_world = NULL; }
sh_package_occurrence_snapshot *sh_package_editor_snapshot(unsigned kind, const void *flat, char *error, size_t capacity)
{ (void)kind; (void)flat; (void)error; (void)capacity; CHECK(0); return NULL; }
int sh_package_editor_attach(unsigned kind, const void *flat, const sh_package_occurrence_snapshot *document,
    char *error, size_t capacity)
{ (void)kind; (void)flat; (void)document; (void)error; (void)capacity; CHECK(0); return 0; }
void *sh_editor_transition_capture(const void *editor) { (void)editor; return NULL; }
int sh_editor_transition_frame_ready(void *editor, void *previous_map) { (void)editor; (void)previous_map; return 0; }
int sh_editor_transition_load(void *editor, void *previous_map, const void *name,
    sh_editor_transition_load_fn load, const sh_editor_transition_callbacks *callbacks, char *error, size_t capacity)
{ (void)editor; (void)previous_map; (void)name; (void)load; (void)callbacks; (void)error; (void)capacity; CHECK(0); return 1; }
int sh_editor_frame_request_rawmap_save_to(const char *destination, char *out_msg, int msg_capacity)
{ (void)destination; (void)out_msg; (void)msg_capacity; CHECK(0); return 0; }
int sh_editor_frame_request_reload(char *out_msg, int msg_capacity) { (void)out_msg; (void)msg_capacity; CHECK(0); return 0; }
int sh_prefab_import_pending(void) { return 0; }

/* Render settings, navigation, the HUD and the user data root. The fixtures carry no navigation envelopes. */
int sh_map_render_capture(void *snapshot, sh_map_render *settings)
{ CHECK(snapshot && settings); memset(settings, 0, sizeof(*settings)); return 1; }
void sh_map_render_loaded(void *map) { (void)map; }
void sh_map_render_select(const sh_map_render *settings) { CHECK(settings != NULL); render_selections++; }
void sh_nav_bake_set_map(const char *json, size_t len) { (void)json; (void)len; }
void sh_navmesh_build_from_map(const char *json, size_t len) { (void)json; (void)len; }
char *sh_navmesh_embed_all(const char *json, size_t len, size_t *out_len) { (void)json; (void)len; if (out_len) *out_len = 0; return NULL; }
char *sh_navmesh_strip(const char *json, size_t len, size_t *out_len) { (void)json; (void)len; if (out_len) *out_len = 0; return NULL; }
static int hud_reloads;
int sh_weapon_hud_reload(const char *root_path) { (void)root_path; hud_reloads++; return 1; }
int sh_overrides_get_root(char *out, size_t cap) { return out && cap && !strcpy_s(out, cap, data_root); }

/* The engine's modal dialog: one question on screen at a time. The player dismisses a notice (button set 1) when it
 * is next polled and answers a consent prompt (the Yes/No set 6) as the test decides; releasing a still-unanswered
 * question withdraws it. */
static int dialog_ticket, dialog_tickets, dialog_buttons, consent_answer = SH_ENGINE_DIALOG_PENDING;
static int notices, consents, withdrawn;
static char notice_text[256], consent_text[256];
int sh_engine_dialog_can_ask(void) { return !dialog_ticket; }
int sh_engine_dialog_queue_idle(const void *shell) { (void)shell; return 1; }
int sh_engine_dialog_ask(unsigned gdm_id, unsigned button_set, const char *text)
{
    if (getenv("SH_PUBLISHED_TEST_LOG")) fprintf(stderr, "dialog: %#x %u %s\n", gdm_id, button_set, text ? text : "(null)");
    CHECK(gdm_id == 0x29u && (button_set == 1u || button_set == 6u) && text && !dialog_ticket &&
        GetCurrentThreadId() == main_thread);
    if (!text || dialog_ticket) return 0;
    if (button_set == 6u) { consents++; snprintf(consent_text, sizeof(consent_text), "%s", text); }
    else { notices++; snprintf(notice_text, sizeof(notice_text), "%s", text); }
    dialog_buttons = (int)button_set;
    return dialog_ticket = ++dialog_tickets;
}
int sh_engine_dialog_poll(int ticket)
{
    if (!ticket || ticket != dialog_ticket) return SH_ENGINE_DIALOG_LOST;
    return dialog_buttons == 1 ? SH_ENGINE_DIALOG_ACCEPTED : consent_answer;
}
void sh_engine_dialog_release(int ticket)
{
    if (!ticket || ticket != dialog_ticket) return;
    if (dialog_buttons == 6 && consent_answer == SH_ENGINE_DIALOG_PENDING) withdrawn++;
    dialog_ticket = 0;
}

static void engine_setup(void)
{
    static const sh_process_heap_api heap = {heap_get, heap_push, heap_pop};
    main_thread = GetCurrentThreadId();
    engine_heap.depth = 1; engine_heap.heaps[0] = 2;
    g_map_native.heap = heap; g_map_native.snapshot = snapshot_ctor; g_map_native.string = string_ctor;
    g_map_native.metadata = metadata_copy; g_map_native.snapshot_free = snapshot_free;
    g_map_native.metadata_free = metadata_free; g_map_native.string_free = string_free; g_map_native.path = build_path;
    /* The resolved DeserializeFromJson entry runs rawmap's detour; its trampoline reaches the engine's parser. */
    g_map_native.parse = (native_parse_fn)sh_deser_detour; g_map_native.serialize = serialize_map;
    g_deser_orig = engine_parse;
    g_source_heap = heap; g_source_files = (void **)&files_slot; g_source_memory = (void **)&memory_slot;
    fs_table[0xc0 / 8] = (void *)fs_read; fs_table[0x130 / 8] = (void *)fs_size; memory_table[2] = (void *)memory_release;
    system_table[0x38 / 8] = (void *)get_cache; *(void **)cache = cache_table; cache_table[0x190 / 8] = (void *)set_cache;
    string_ctor(cache + 0x24408); string_ctor(cache + 0x24438); string_ctor(level + 0x20);
    string_ctor(host_parameters + 0x550); string_ctor(host_parameters + 0x600);
    session_table[0x338 / 8] = (void *)get_lobby; lobby_table[0x2b0 / 8] = (void *)get_parameters;
    progress_table[0x30 / 8] = (void *)hide_progress; *(void **)(manager + 0xa38) = &progress_object;
    *(void **)(shell + 0x18) = manager; *(int *)(manager + 8) = *(int *)(manager + 12) = 63; *(int *)(manager + 0x910) = 1;
    *(int *)engine_snapshot = 0x6f; *(void **)(history_record + 0x18) = history_object;
    g_pub_system = get_system; g_pub_cache_set = set_cache; g_pub_shell_slot = &shell_slot;
    g_pub_session_slot = &session_slot; g_pub_history_slot = &history_slot; g_pub_history_count = &history_count;
    g_pub_thread = &main_thread;
    g_pub_original[PUB_OFFLINE] = (void *)engine_offline; g_pub_original[PUB_DIRECT] = (void *)engine_direct;
    g_pub_original[PUB_LOBBY] = (void *)engine_lobby; g_pub_original[PUB_READY] = (void *)engine_ready;
    g_pub_ready = 1;
}

/* ---- Published documents ---- */
/* The native members every cached published map carries (native_map_document_fixture.h's retained document), added
 * before the closing brace of a retained body that lacks them: its four lobby slots, then its type and version. */
#define NATIVE_SLOTS ",\"snapSlotSettings\":[{\"race\":0,\"state\":0,\"team\":0,\"~type\":\"snapLobbySlotSetting_t\"}," \
    "{\"race\":0,\"state\":0,\"team\":0,\"~type\":\"snapLobbySlotSetting_t\"}," \
    "{\"race\":0,\"state\":0,\"team\":1,\"~type\":\"snapLobbySlotSetting_t\"}," \
    "{\"race\":0,\"state\":0,\"team\":1,\"~type\":\"snapLobbySlotSetting_t\"}]"
static const char untyped_members[] = NATIVE_SLOTS ",\"version\":111,\"~type\":\"idSnapMap\",\"~version\":111";
static const char typed_members[] = NATIVE_SLOTS ",\"~version\":111";
typedef struct published {
    unsigned char metadata[MAP_METADATA_SIZE];
    const char *name, *note, *carrier, *authored;
    const char *native_form;        /* what a use converted to native map data decodes as (R137), if any */
    size_t uses;                    /* its package uses, each recorded in per-use provenance */
    char *document;
    size_t length;
    const unsigned char *payload;   /* the exact archive its carrier embeds */
    size_t payload_length;
    unsigned char *packed;          /* that archive when this test packed it */
    char *expected;         /* what the real chain compiles for it, independently, at the current library state */
    size_t expected_length;
    char own[96];           /* the native name of its own authored override, found by a value only it authors */
} published;
static char *file_bytes(const char *path, size_t *length)
{
    HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD size, read = 0;
    char *bytes = NULL;
    *length = 0;
    if (file == INVALID_HANDLE_VALUE) return NULL;
    size = GetFileSize(file, NULL);
    if ((bytes = malloc((size_t)size + 1)) && ReadFile(file, bytes, size, &read, NULL) && read == size) {
        bytes[size] = 0; *length = size;
    } else { free(bytes); bytes = NULL; }
    CloseHandle(file);
    return bytes;
}
static int unchanged_on_disk(const published *p)
{
    char path[MAX_PATH], *bytes;
    size_t length;
    int same;
    snprintf(path, sizeof(path), "%s/published/%s_map.decl", root, string_body(p->metadata));
    bytes = file_bytes(path, &length);
    same = bytes && length == p->length && !memcmp(bytes, p->document, length);
    free(bytes); return same;
}
/* The real chain for one document, outside any launch: context, bind, then the carrier strip rawmap applies. */
static void expect(published *p)
{
    char error[2048] = "", *bound = heap_copy(p->document, p->length), *stripped = NULL;
    size_t bound_length = p->length;
    int failed = 0;
    sh_mpkg_context *context = sh_mpkg_context_open(data_root, p->document, p->length, error, sizeof(error));
    sh_package_map_plan *plan = NULL;
    free(p->expected); p->expected = NULL; p->expected_length = 0; p->own[0] = 0;
    /* R-398: over a deferred departure an open's preparation reads the departure view, staged without registering
     * it; the independent compile reads the same view. */
    char stage_error[256] = "";
    CHECK(sh_package_runtime_stage_departure(stage_error, sizeof(stage_error)));
    sh_package_runtime_preparation_scope(1);
    int bound_ok = context && bound && sh_package_document_bind(data_root, context, &bound, &bound_length, &plan, error,
        sizeof(error));
    sh_package_runtime_preparation_scope(0);
    if (!bound_ok) {
        fprintf(stderr, "independent compile: %s\n", error); CHECK(0);
    } else {
        const sh_compiled_resource *resource = p->authored ?
            authored_resource(sh_package_map_plan_compilation(plan), p->authored) : NULL;
        if (resource && resource->native_name) snprintf(p->own, sizeof(p->own), "%s", resource->native_name);
        stripped = prepare_map_buffer_mode(bound, 0, &failed);
        CHECK(!failed);
        if (!failed) {
            const char *chosen = stripped ? stripped : bound;
            p->expected_length = strlen(chosen);
            if ((p->expected = malloc(p->expected_length + 1)) != NULL) memcpy(p->expected, chosen, p->expected_length + 1);
        }
    }
    if (stripped) HeapFree(GetProcessHeap(), 0, stripped);
    if (bound) HeapFree(GetProcessHeap(), 0, bound);
    sh_package_map_plan_free(plan);
    CHECK(sh_mpkg_context_close(&context));
    if (getenv("SH_PUBLISHED_TEST_LOG")) fprintf(stderr, "expect %s: %zu -> %zu bytes, own '%s'\n", p->name, p->length,
        p->expected_length, p->own);
}
static void published_cache(published *p, const char *id, const char *name, const char *note, const char *carrier,
    const char *authored)
{
    char relative[MAX_PATH];
    p->name = name; p->note = note; p->carrier = carrier; p->authored = authored; p->uses = 1;
    for (size_t i = 0; i < 4; i++) string_ctor(p->metadata + metadata_strings[i]);
    string_set(p->metadata, id, strlen(id)); string_set(p->metadata + 0x30, "revision-1", 10);
    string_set(p->metadata + 0x60, "Published title", 15); string_set(p->metadata + 0x398, name, strlen(name));
    snprintf(relative, sizeof(relative), "published/%s_map.decl", id); create(relative, p->document);
    expect(p);
}
static void published_make(published *p, const char *id, const char *name, const char *body, size_t body_length,
    const char *members, const char *note, const char *carrier, const char *authored)
{
    size_t added = strlen(members);
    memset(p, 0, sizeof(*p));
    CHECK(body_length > 1 && body[0] == '{' && body[body_length - 1] == '}');
    p->length = body_length + added;
    p->document = malloc(p->length + 1);
    if (!p->document) { CHECK(0); return; }
    memcpy(p->document, body, body_length - 1); memcpy(p->document + body_length - 1, members, added);
    p->document[p->length - 1] = '}'; p->document[p->length] = 0;
    published_cache(p, id, name, note, carrier, authored);
}
/* A native document carrying the package authored in <root>/<folder>, packed and embedded by the current writer. */
static void embedded_make(published *p, const char *folder, const char *carrier, const char *body, size_t body_length,
    const char *id, const char *name, const char *note, const char *authored, unsigned char fingerprint[32])
{
    char path[MAX_PATH], error[1024] = "";
    sh_package_sources *sources;
    unsigned char *archive = NULL;
    size_t archive_length = 0, length = 0;
    char *embedded = NULL;
    memset(p, 0, sizeof(*p));
    snprintf(path, sizeof(path), "%s/%s", root, folder);
    sources = sh_package_sources_scan_directory(path, error, sizeof(error));
    CHECK(sources != NULL);
    if (sources) {
        memcpy(fingerprint, sources->fingerprints[0], 32);
        archive = sh_package_archive_pack(sources, 0, &archive_length, error, sizeof(error));
        sh_package_sources_free(sources);
    }
    CHECK(archive != NULL);
    if (archive) embedded = sh_mpkg_embed(body, body_length, carrier, archive, archive_length, &length, error, sizeof(error));
    p->packed = archive; p->payload = archive; p->payload_length = archive_length;
    CHECK(embedded != NULL);
    if (!embedded) { fprintf(stderr, "%s: %s\n", id, error); return; }
    p->document = malloc(length + 1);
    if (p->document) { memcpy(p->document, embedded, length); p->document[length] = 0; p->length = length; }
    HeapFree(GetProcessHeap(), 0, embedded);
    published_cache(p, id, name, note, carrier, authored);
}
/* A same-identity variant of the beta14/15 package, local.history-pack with other authored values, in a native
 * document that uses the same stock entity. */
static void variant_make(published *p, unsigned char fingerprint[32])
{
    static const char body[] = "{\"entities\":[{\"targetType\":\"idDeclEntityDef\",\"inherit\":\"history/root\"}],"
        "\"note\":\"variant/root\x97\",\"variables\":{\"allocCount\":[0,0,0,0,0,0,0,0,0,0],\"string\":[]}"
        NATIVE_SLOTS ",\"version\":111,\"~type\":\"idSnapMap\",\"~version\":111}";
    create("variant/local.history-pack/package.json", "{\r\n  \"id\": \"local.history-pack\",\r\n  \"name\": \"history-pack\"\r\n}\r\n");
    create("variant/local.history-pack/assets/generated/decls/entitydef/history/root.decl",
        "{\r\n\tclass = \"Entity\";\r\n\tedit = {\r\n\t\thealth = 20;\r\n\t\ttarget = \"history/added\";\r\n\t}\r\n}");
    create("variant/local.history-pack/assets/generated/decls/entitydef/history/added.decl",
        "{\r\n\tclass = \"Entity\";\r\n\tedit = {\r\n\t\thealth = 66;\r\n\t\ttarget = \"\";\r\n\t}\r\n}");
    embedded_make(p, "variant/local.history-pack", HIST_W3_ID, body, sizeof(body) - 1, "variant-map", "variant",
        "\"note\":\"variant/root\x97\"", "health = 20", fingerprint);
}
static void published_free(published *p)
{
    for (size_t i = 0; i < 4; i++) string_free(p->metadata + metadata_strings[i]);
    free(p->document); free(p->expected); free(p->packed); memset(p, 0, sizeof(*p));
}

/* The packages installed in the data root, all of them or by descriptor identity and complete tree. */
static int installed(const char *id, const unsigned char *fingerprint)
{
    char error[512] = "";
    int matches = 0;
    sh_package_sources *sources = sh_package_sources_scan(data_root, error, sizeof(error));
    CHECK(sources != NULL);
    if (!sources) return -1;
    if (!id) matches = (int)sources->package_count;
    else for (size_t i = 0; i < sources->component_count; i++) {
        const sh_package_component *c = &sources->components[i];
        if (c->relative[0] || strcmp(c->descriptor.id, id)) continue;
        if (!fingerprint || !memcmp(sources->fingerprints[c->owner], fingerprint, 32)) matches++;
    }
    sh_package_sources_free(sources); return matches;
}
/* Delete what the product installed under this fixture's own overrides: never a reparse point, never outside root. */
static void remove_tree(const char *path)
{
    char pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    CHECK(!strncmp(path, root, strlen(root)));
    if (strncmp(path, root, strlen(root))) return;
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
            snprintf(child, sizeof(child), "%s\\%s", path, found.cFileName);
            CHECK(!(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT));
            if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else CHECK(DeleteFileA(child));
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    CHECK(RemoveDirectoryA(path));
}
static void remove_installed(void)
{
    char pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    snprintf(pattern, sizeof(pattern), "%s\\overrides\\map-*", data_root);
    search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        snprintf(child, sizeof(child), "%s\\overrides\\%s", data_root, found.cFileName);
        CHECK((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT));
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            remove_tree(child);
    } while (FindNextFileA(search, &found));
    FindClose(search);
}

/* ---- Driving the route ---- */
static void show_notices(void);
static const sh_map_native_selection *pending_selection(void)
{
    const pub_request *request = g_pending_map.request.context;
    return request ? request->selection : NULL;
}
static int pending_clear(void)
{
    return !g_pending_map.state && !g_pending_map.json && !g_pending_map.plan && !g_pending_map.sources &&
        !g_pending_map.request.context && !g_preflight_busy && !g_pub_retired;
}
static int live_selections(void) { return metadata_copies - metadata_frees; }
static int launches_made, entries, loads;
static int lobby_early, lobbies_early;   /* a covered lobby is created at the press, not at entry */
static int early_entered;                /* early lobbies whose request then entered */
static int native_launches(void) { return offline_launches + direct_launches + lobby_launches + ready_launches; }
enum { BY_OFFLINE, BY_DIRECT, BY_LOBBY, BY_READY };
/* The player's launch: from the browser offline, direct, or hosting a lobby for the map the browser shows (the
 * engine records it as the newest lobby history entry); or, joining a host's lobby, the host's map becoming ready
 * once its download fills the browser cache. The native original does not run: the bridge holds a retained
 * request, ready (2) or waiting for installation consent (1). */
static int launch(published *p, int how)
{
    int launches = native_launches(), reads = fs_reads, hidden = progress_hidden, accepted = 1;
    browser = 1; host_names(NULL); launches_made++;
    if (how == BY_LOBBY) {
        memcpy(history_object + 8, p->metadata, MAP_METADATA_SIZE); history_count = 1;
        set_cache(cache, p->metadata, engine_snapshot);
        accepted = pub_lobby(manager, 1);
    } else if (how == BY_READY) {
        browser = 0; host_names(p->metadata); set_cache(cache, p->metadata, engine_snapshot);
        pub_ready(manager, &ready_download, p->metadata, engine_snapshot);
        /* The bridge releases the engine's download progress display before preparing. */
        accepted = progress_hidden == hidden + 1;
    } else if (how == BY_DIRECT) pub_direct(manager, p->metadata, engine_snapshot);
    else pub_offline(manager, p->metadata, engine_snapshot);
    /* The selection reads its cached source exactly once. A lobby whose map owes
     * no consent is the engine's own, created inside the press. */
    lobby_early = how == BY_LOBBY && g_pending_map.state == 2;
    if (lobby_early) lobbies_early++;
    return accepted && native_launches() == launches + lobby_early && fs_reads == reads + 1 ? g_pending_map.state : -1;
}
static int native_passes;
/* A package-free map: the bridge reads its cached source once to decide, then the engine's own launch runs at
 * once with the player's arguments. Nothing is retained, hidden, activated or reported, and the compiler is never
 * asked whether it is ready. */
static int native_launch(published *p, int how)
{
    int launches = native_launches(), reads = fs_reads, hidden = progress_hidden, before_notices = notices;
    int requests = activation_requests, ok = 1;
    browser = 1; host_names(NULL); launches_made++; native_passes++;
    native_pass = 1;
    if (how == BY_LOBBY) {
        memcpy(history_object + 8, p->metadata, MAP_METADATA_SIZE); history_count = 1;
        set_cache(cache, p->metadata, engine_snapshot);
        ok = pub_lobby(manager, 1) == 1;
    } else if (how == BY_READY) {
        browser = 0; host_names(p->metadata); set_cache(cache, p->metadata, engine_snapshot);
        pub_ready(manager, &ready_download, p->metadata, engine_snapshot);
    } else if (how == BY_DIRECT) pub_direct(manager, p->metadata, engine_snapshot);
    else pub_offline(manager, p->metadata, engine_snapshot);
    native_pass = 0;
    show_notices();
    return ok && native_launches() == launches + 1 && fs_reads == reads + 1 && progress_hidden == hidden &&
        pending_clear() && !live_selections() && activation_requests == requests && notices == before_notices &&
        !browser && unchanged_on_disk(p);
}
/* The engine's next tick raises the consent prompt naming the map's package; the player then answers it. */
static int consent_raised(const char *package)
{
    int before = consents;
    sh_mpkg_consent_poll();
    return consents == before + 1 && dialog_ticket && dialog_buttons == 6 &&
        strstr(consent_text, "Install it now? ") && strstr(consent_text, package);
}
static void consent_answered(int answer)
{ consent_answer = answer; sh_mpkg_consent_poll(); consent_answer = SH_ENGINE_DIALOG_PENDING; }
/* The engine's continuation for an accepted request: native entry, then a tick while the map loads with the browser
 * closed and the session's lobby naming the map; the transition into its level follows. */
static int enter_native(void)
{
    int launches = native_launches(), entered, expected = !lobby_early;
    sh_rawmap_pending_map_poll();
    entries++;
    entered = g_pending_map.state == 4 && native_launches() == launches + expected && !browser;
    if (lobby_early) early_entered++;
    lobby_early = 0;
    sh_rawmap_pending_map_poll();
    return entered && g_pending_map.state == 4;
}
/* The spawn info FinalizeMapChange receives: every SnapMap level is renamed DynamicSnapMap with flag
 * 0x2, and the session's lobby parameters name the published item the launch set at the press. */
static void level_starts(const published *p)
{
    string_set(level + 0x20, "DynamicSnapMap", 14);
    level[0x170f8] = 2;
    if (p) host_names(p->metadata);
}
static int transition(const published *p)
{
    int ok;
    level_starts(p);
    if (sh_rawmap_transition_pending(manager, level) != 1) return -1;
    transition_boundary = 1;
    ok = sh_rawmap_transition_activate();
    transition_boundary = 0;
    if (ok) ok = sh_rawmap_transition_commit();
    sh_rawmap_transition_finished(ok);
    return ok;
}
/* Two ticks: a queued notice is raised, then dismissed. */
static void show_notices(void) { sh_mpkg_consent_poll(); sh_mpkg_consent_poll(); }
/* The player leaves the published map for the browser: the session ends and the map's private context retires. */
static void depart(void)
{
    browser = 1; retirement_safe = 1; host_names(NULL);
    sh_rawmap_map_context_poll();
}
/* R-396: leaving a map with packages registers nothing. Its provider stays served, complete, and its context stays
 * held until the next boundary's activation withdraws it; a map without packages retires its context at once. */
static void departed(int packaged)
{
    if (packaged) CHECK(g_map_context && g_map_context_active && g_map_context_departed &&
        sh_package_runtime_departure_deferred() && sh_package_runtime_has_map_provider());
    else CHECK(!g_map_context && !g_map_context_active && !g_map_context_departed);
}
/* A level no request of ours loads after a deferred departure: the allocation boundary withdraws it in one
 * activation, after the unload, and the departed context retires. */
static void native_level_after_departure(void)
{
    int withdrawals = departure_withdrawals, ok;
    if (!g_map_context_departed) return;
    level_starts(NULL);
    CHECK(sh_rawmap_transition_pending(manager, level) == 1);
    transition_boundary = 1; ok = sh_rawmap_transition_activate(); transition_boundary = 0;
    CHECK(ok && sh_rawmap_transition_commit());
    sh_rawmap_transition_finished(ok);
    CHECK(departure_withdrawals == withdrawals + 1 && !g_map_context && !g_map_context_active &&
        !g_map_context_departed && !sh_package_runtime_departure_deferred() && !sh_package_runtime_has_map_provider());
}

static int decoded_is(const published *p)
{
    return engine_decoded && p->expected && engine_decoded_length == p->expected_length &&
        !memcmp(engine_decoded, p->expected, engine_decoded_length);
}
/* What the engine decoded: the map's own native note bytes, no delivery carrier or provenance variable; for a map
 * with a package, the native name of the override a use keeps and the native data a converted use became; a fully
 * converted map names no private resource. */
static void decoded_properties(const published *p)
{
    sh_package_provenance carried = {0};
    char error[512] = "";
    int present = 1;
    CHECK(engine_decoded && strstr(engine_decoded, p->note));
    CHECK(engine_decoded && !strstr(engine_decoded, "smpkg."));
    CHECK(engine_decoded && sh_map_provenance_read(engine_decoded, engine_decoded_length, &carried, &present,
        error, sizeof(error)) && !present);
    sh_package_provenance_free(&carried);
    if (p->native_form) CHECK(engine_decoded && strstr(engine_decoded, p->native_form));
    if (p->carrier) {
        if (p->authored) CHECK(p->own[0] && engine_decoded && strstr(engine_decoded, p->own));
        else CHECK(engine_decoded && !strstr(engine_decoded, "smp/"));
        CHECK(engine_decoded_length != p->length || memcmp(engine_decoded, p->document, p->length));
    } else CHECK(engine_decoded_length == p->length && !memcmp(engine_decoded, p->document, p->length));
}
/* The retained request holds the exact cached source, the map's own complete bundle and its bound document. */
static void retained_properties(const published *p)
{
    const sh_map_native_selection *selection = pending_selection();
    size_t length = 0;
    const char *source = selection ? sh_map_native_source(selection, &length) : NULL;
    CHECK(source && length == p->length && !memcmp(source, p->document, length));
    if (p->carrier) {
        size_t count = 0, archive_length = 0;
        sh_package_provenance bound = {0};
        char error[512] = "";
        int present = 0;
        const sh_package_bundle_binding *bundles = g_pending_map.sources ?
            sh_mpkg_context_bundles(g_pending_map.sources, &count) : NULL;
        sh_package_bundle *bundle = bundles && count == 1 ? sh_package_bundle_retain(bundles[0].original) : NULL;
        unsigned char *archive = bundle ? sh_package_bundle_read(bundle, &archive_length, error, sizeof(error)) : NULL;
        CHECK(g_pending_map.sources && sh_mpkg_context_count(g_pending_map.sources) == 1);
        CHECK(bundles && count == 1 && !strcmp(sh_mpkg_context_carrier(g_pending_map.sources, 0), p->carrier));
        /* Its retained delivery is the exact archive the map embeds. */
        CHECK(archive && archive_length == p->payload_length && !memcmp(archive, p->payload, archive_length));
        free(archive); sh_package_bundle_release(bundle);
        CHECK(g_pending_map.plan != NULL);
        /* Per-use provenance travels with the retained bound document until activation strips it, a use the R137
         * pass converted to native map data included. */
        CHECK(g_pending_map.json && sh_map_provenance_read(g_pending_map.json, strlen(g_pending_map.json), &bound,
            &present, error, sizeof(error)) && present && bound.use_count == p->uses);
        sh_package_provenance_free(&bound);
    } else CHECK(!g_pending_map.plan);
}

/* After a successful transition: one release, exactly the independently compiled document, no message; leaving the
 * map retires its context. */
static void loaded(published *p, sh_package_map_plan *plan, int requests, int before_notices)
{
    loads++;
    CHECK(pending_clear() && !live_selections());
    CHECK(activation_requests == requests + 1 && activation_plan == plan);
    CHECK(decoded_is(p)); decoded_properties(p);
    CHECK(!plan == !g_map_context_active);
    show_notices(); CHECK(notices == before_notices);
    depart();
    departed(plan != NULL);
    CHECK(unchanged_on_disk(p));
}
/* One complete launch: covered (no prompt) or with an accepted installation. */
static void complete_launch(published *p, int how, const char *install)
{
    int before_notices = notices, requests = activation_requests, before_consents = consents;
    sh_package_map_plan *plan;
    expect(p);
    CHECK(pending_clear() && !live_selections());
    CHECK(launch(p, how) == (install ? 1 : 2));
    retained_properties(p);
    if (install) {
        CHECK(consent_raised(install));
        consent_answered(SH_ENGINE_DIALOG_ACCEPTED);
        CHECK(g_pending_map.state == 2);
    }
    CHECK(consents == before_consents + !!install);
    plan = g_pending_map.plan;
    CHECK(enter_native());
    CHECK(transition(p) == 1);
    loaded(p, plan, requests, before_notices);
}
/* One native original ran per native entry and no prompt is left; every cached read was a launch's own; every engine
 * parse was a native entry's lobby session or a load's gameplay document; no engine scope or selection remains. */
static void accounted(const char *label)
{
    /* An early lobby whose request never entered is a native launch with no entry of ours; one that
     * entered decoded no preliminary session snapshot there. */
    CHECK(native_launches() == entries + native_passes + lobbies_early - early_entered && !dialog_ticket);
    CHECK(fs_reads == launches_made && gameplay_decodes == loads && engine_decodes == entries - early_entered + loads);
    CHECK(!engine_snapshots && engine_heap.depth == 1 && !live_selections());
    printf("%s: %d gameplay decodes, %d activations, %d registrations, %d restorations, %d prompts, %d notices, "
        "%d cached reads\n", label, gameplay_decodes, activation_requests, registrations, restoring_registrations,
        consents, notices, fs_reads);
}
/* The runtime and registry leave the data root and what the product installed is removed; retired contexts and
 * finished installations leave only their now-empty parents. Only the engine's own browser cache, level and session
 * strings remain. */
static void data_root_retired(void)
{
    char maps[MAX_PATH], staging[MAX_PATH];
    CHECK(engine_strings == 5);
    native_level_after_departure();
    sh_package_runtime_test_source_readers((sh_decl_registry_source){0}, 0);
    sh_package_runtime_test_dispose();
    for (size_t i = 0; i < registry_fixture.count; i++) free(registry_fixture.regions[i].address);
    memset(&registry_fixture, 0, sizeof(registry_fixture));
    remove_installed();
    sh_mpkg_shared_trees_release();
    snprintf(maps, sizeof(maps), "%s/package-cache/maps", data_root);
    CHECK(RemoveDirectoryA(maps));
    snprintf(staging, sizeof(staging), "%s/package-staging", data_root);
    if (GetFileAttributesA(staging) != INVALID_FILE_ATTRIBUTES) CHECK(RemoveDirectoryA(staging));
    cleanup_resources(data_root);
}

static void test_published_source_contract(void)
{
    char id[SH_PACKAGE_ID_CAP] = "", error[512] = "", w4_key[SH_PACKAGE_BUNDLE_KEY_CAP] = "delivery.";
    unsigned char w3_tree[32], variant_tree[32], complete_tree[32];
    published native, history, w3, w4, variant;
    create("published-data/overrides", NULL); create("published", NULL);
    snprintf(data_root, sizeof(data_root), "%s/published-data", root);
    admission_setup(&registry_fixture);
    sh_package_runtime_test_dispose();
    sh_package_runtime_test_baseline(historical_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data_root));
    sh_package_runtime_test_source_readers((sh_decl_registry_source){&registry_fixture, admission_read,
        (uintptr_t)registry_fixture.registry, NULL}, (uintptr_t)registry_fixture.reflection);
    sh_mpkg_boot_capture(data_root);
    CHECK(sh_mpkg_activation_ready());
    CHECK(sh_package_archive_identity(hist_w3_payload, hist_w3_payload_len, id, w3_tree, error, sizeof(error)) &&
        !strcmp(id, HIST_W3_ID));
    /* The beta16 writer carries the same archive under its complete-tree key. */
    CHECK(sh_package_archive_tree_identity(hist_w3_payload, hist_w3_payload_len, complete_tree, error, sizeof(error)));
    for (size_t i = 0; i < 32; i++) snprintf(w4_key + 9 + i * 2, 3, "%02x", complete_tree[i]);

    /* A native-only map, a beta13 legacy carrier, the beta14/15 descriptor-keyed and beta16 complete-tree carriers of
     * one archive, and a current-writer variant of that package identity with different authored values. */
    published_make(&native, "native-map", "native", hist_base_compact, hist_base_compact_len, typed_members,
        "\"entities\"", NULL, NULL);
    published_make(&history, "history-map", "history", hist_map_history, hist_map_history_len, untyped_members,
        "\"note\":\"history/root\x97\"", HIST_HISTORY_ID, "health = 10");
    history.payload = hist_history_payload; history.payload_length = hist_history_payload_len;
    published_make(&w3, "w3-map", "w3", hist_map_w3, hist_map_w3_len, untyped_members,
        "\"note\":\"history/root\xe2\x80\x94\"", HIST_W3_ID, "health = 10");
    w3.payload = hist_w3_payload; w3.payload_length = hist_w3_payload_len;
    published_make(&w4, "w4-map", "w4", hist_map_w4_reference, hist_map_w4_reference_len, untyped_members,
        "\"note\":\"history/root\xe2\x80\x94\"", w4_key, "health = 10");
    w4.payload = hist_w3_payload; w4.payload_length = hist_w3_payload_len;
    variant_make(&variant, variant_tree);
    CHECK(memcmp(variant_tree, w3_tree, 32));
    /* The variant compiles its own override under a name no other map's content produces. */
    CHECK(variant.own[0] && w3.own[0] && history.own[0] && strcmp(variant.own, w3.own) && strcmp(variant.own, history.own));
    CHECK(!installed(NULL, NULL));

    /* 1. A native-only map: no package, so the bridge hands every route to the engine's own launch after one read
     * of its cached source; nothing is retained, prepared, hidden or reported. */
    CHECK(native_launch(&native, BY_OFFLINE));
    CHECK(native_launch(&native, BY_DIRECT));
    CHECK(native_launch(&native, BY_LOBBY));
    CHECK(native_launch(&native, BY_READY));

    /* 2. A declined installation: one release, nothing activated or installed, no message. */
    {
        int requests = activation_requests, before = notices;
        CHECK(launch(&history, BY_DIRECT) == 1 && live_selections() == 1);
        retained_properties(&history);
        CHECK(consent_raised(HIST_HISTORY_ID));
        consent_answered(SH_ENGINE_DIALOG_DECLINED);
        sh_rawmap_pending_map_poll();
        CHECK(pending_clear() && !live_selections() && activation_requests == requests && !dialog_ticket);
        CHECK(!installed(NULL, NULL));
        show_notices(); CHECK(notices == before);
    }
    /* 3. A stale selection while the prompt is up (the browser shows another map): the prompt is withdrawn, one
     * release, nothing installed, no message. */
    {
        int requests = activation_requests, before = notices, removed = withdrawn;
        CHECK(launch(&w3, BY_OFFLINE) == 1 && consent_raised(HIST_W3_ID));
        set_cache(cache, native.metadata, engine_snapshot);
        sh_rawmap_pending_map_poll();
        CHECK(pending_clear() && !live_selections() && activation_requests == requests);
        CHECK(withdrawn == removed + 1 && !dialog_ticket && !installed(NULL, NULL));
        show_notices(); CHECK(notices == before);
    }
    /* 4. Another selection while the prompt is up: the new launch withdraws the prompt and releases the old request
     * before anything of its own; a package-free replacement then runs natively at once. */
    {
        int requests = activation_requests, before = notices, removed = withdrawn;
        CHECK(launch(&w3, BY_OFFLINE) == 1 && consent_raised(HIST_W3_ID));
        CHECK(native_launch(&native, BY_DIRECT));
        CHECK(withdrawn == removed + 1 && !dialog_ticket && activation_requests == requests);
        show_notices(); CHECK(notices == before);
        CHECK(!installed(NULL, NULL));
    }
    /* 5. An accepted installation whose map the engine then refuses to register: the prepared installation rolls back
     * with the previous provider restored, one release and one open notice; nothing stays installed. */
    {
        int before = notices, restored = restoring_registrations;
        expect(&w3);
        CHECK(launch(&w3, BY_OFFLINE) == 1 && consent_raised(HIST_W3_ID));
        consent_answered(SH_ENGINE_DIALOG_ACCEPTED);
        CHECK(g_pending_map.state == 2);
        CHECK(enter_native());
        registration_refusals = 1;
        CHECK(transition(&w3) == 0);
        CHECK(pending_clear()); CHECK(!live_selections()); CHECK(restoring_registrations > restored);
        /* R-398: an open over a deferred departure withdraws it in its own activation; refused, the departed map
         * stays served and its context stays held for the next boundary. */
        CHECK(!g_map_context || (g_map_context_departed && sh_package_runtime_departure_deferred()));
        CHECK(!installed(NULL, NULL));
        show_notices(); CHECK(notices == before + 1 && !strcmp(notice_text, "This map couldn't be opened."));
    }
    /* 6. The beta16 carrier of the same archive: asked again, since nothing stayed installed; accepted and loaded, the
     * installation commits the map's exact authored tree. */
    complete_launch(&w4, BY_OFFLINE, HIST_W3_ID);
    CHECK(installed(NULL, NULL) == 1 && installed(HIST_W3_ID, w3_tree) == 1);
    /* 6b. A published map that only references that now-installed delivery (D1 L2), hosted from its lobby: the
     * reference is honored silently, the engine's lobby is created inside the press, and the level transition decodes
     * the map's own compiled document with no consent and no notice. */
    {
        published l2;
        sh_mpkg_reference reference;
        char err[512] = "", relative[MAP_STRING_SIZE + 32];
        char *text;
        size_t n = 0;
        int before = notices, before_consents = consents, requests = activation_requests;
        sh_package_map_plan *plan;
        /* The same map as w3 -- it uses the package's content -- but carrying a reference to the delivery instead
         * of the delivery itself, as the current writer saves an oversized package. */
        published_make(&l2, "l2-map", "l2", hist_map_w3, hist_map_w3_len, untyped_members,
            "\"note\":\"history/root\xe2\x80\x94\"", NULL, NULL);
        CHECK(sh_mpkg_reference_describe(w4_key, hist_w3_payload, hist_w3_payload_len, &reference, err, sizeof err));
        text = sh_mpkg_strip(l2.document, l2.length, &n);
        CHECK(text != NULL);
        if (text) {
            char *referenced = sh_mpkg_reference_embed(text, n, &reference, &n, err, sizeof err);
            HeapFree(GetProcessHeap(), 0, text); text = referenced;
        }
        CHECK(text != NULL);
        if (text) {
            free(l2.document);
            l2.document = malloc(n + 1); CHECK(l2.document != NULL);
            if (l2.document) { memcpy(l2.document, text, n + 1); l2.length = n; }
            HeapFree(GetProcessHeap(), 0, text);
            snprintf(relative, sizeof(relative), "published/%s_map.decl", string_body(l2.metadata));
            create(relative, l2.document);
            expect(&l2);
            CHECK(sh_mpkg_reference_scan(l2.document, l2.length, NULL, 0) == 1);
            CHECK(launch(&l2, BY_LOBBY) == 2 && consents == before_consents && live_selections() == 1);
            /* The player's second press on the same tile, dispatched before the lobby screen showed:
             * nothing is prepared, read or launched again and the one request stays. */
            {
                int launches = native_launches(), reads = fs_reads;
                CHECK(pub_lobby(manager, 1) == 1);
                CHECK(native_launches() == launches && fs_reads == reads && g_pending_map.state == 2 &&
                    live_selections() == 1 && consents == before_consents);
            }
            plan = g_pending_map.plan;
            CHECK(enter_native() && transition(&l2) == 1);
            loads++;
            CHECK(pending_clear() && !live_selections());
            CHECK(activation_requests == requests + 1 && activation_plan == plan);
            CHECK(decoded_is(&l2) && engine_decoded && !strstr(engine_decoded, "smpkg.") && !strstr(engine_decoded, "smpref."));
            show_notices(); CHECK(notices == before);
            depart();
            departed(plan != NULL); CHECK(unchanged_on_disk(&l2));
            /* 6c. The engine starts the match before the request could enter (the compiler was not ready): the
             * boundary refuses the native build with one notice instead of building against the library. */
            {
                int before2 = notices, requests2 = activation_requests, launches;
                CHECK(launch(&l2, BY_LOBBY) == 2);
                launches = native_launches();
                at_boundary = 0;
                sh_rawmap_pending_map_poll(); CHECK(g_pending_map.state == 2);
                level_starts(&l2);
                CHECK(sh_rawmap_transition_pending(manager, level) == -1 && g_pending_map.state == 5);
                sh_rawmap_transition_finished(0);
                at_boundary = 1;
                CHECK(pending_clear() && !live_selections() && activation_requests == requests2);
                CHECK(native_launches() == launches && !sh_map_published_level_armed(level));
                show_notices();
                /* The player sees the open-problem notice; the diagnostic is in the log. */
                CHECK(notices == before2 + 1 && !strcmp(notice_text, "This map couldn't be opened."));
                /* 6d. The request is gone (canceled behind the lobby) but the lobby's level starts: refused once, then
                 * the level is native again. */
                CHECK(launch(&l2, BY_LOBBY) == 2);
                launches = native_launches();
                CHECK(sh_rawmap_cancel_pending_map() && pending_clear() && sh_map_published_level_armed(level));
                CHECK(sh_rawmap_transition_pending(manager, level) == -1);
                sh_rawmap_transition_finished(0);
                CHECK(!sh_map_published_level_armed(level));
                show_notices();
                CHECK(notices == before2 + 2 && !strcmp(notice_text, "This map couldn't be opened."));
                CHECK(native_launches() == launches && !live_selections());
                /* 6e. While the request waits in its lobby, a level that is positively another map loads
                 * natively: a campaign level, or a SnapMap level the session attributes to another item. A
                 * SnapMap level whose session item cannot be read is refused instead. */
                CHECK(launch(&l2, BY_LOBBY) == 2 && enter_native());
                launches = native_launches();
                string_set(level + 0x20, "game/argent_tower", 17); level[0x170f8] = 0; host_names(l2.metadata);
                CHECK(sh_rawmap_transition_pending(manager, level) == 0 && g_pending_map.state == 4);
                level_starts(&w3);
                CHECK(sh_rawmap_transition_pending(manager, level) == 0 && g_pending_map.state == 4);
                level_starts(NULL); host_names(NULL);
                CHECK(sh_rawmap_transition_pending(manager, level) == -1 && g_pending_map.state == 5);
                sh_rawmap_transition_finished(0);
                show_notices();
                CHECK(notices == before2 + 3 && !strcmp(notice_text, "This map couldn't be opened."));
                CHECK(pending_clear() && !live_selections() && native_launches() == launches);
            }
        }
        published_free(&l2);
    }
    /* 7. Installed from one carrier, the archive covers the other: the beta14/15 map launches with no prompt, from the
     * same exact source, to its own compiled document. */
    complete_launch(&w3, BY_DIRECT, NULL);
    /* 8. A different variant of the installed identity: covered by the installed resources, yet the engine decodes
     * this map's own authored values, never the installed variant's; the installed tree is untouched. */
    complete_launch(&variant, BY_OFFLINE, NULL);
    CHECK(engine_decoded && !strstr(engine_decoded, w3.own));
    CHECK(installed(NULL, NULL) == 1 && installed(HIST_W3_ID, w3_tree) == 1 && !installed(HIST_W3_ID, variant_tree));
    /* 9. The legacy carrier of the same content, now covered. */
    complete_launch(&history, BY_DIRECT, NULL);

    /* 10. Another selection after native entry: the old request releases at once; an engine transition into the
     * superseded map's level, before or after the new request's own native entry, is not this request's, activates
     * nothing and decodes nothing; the new map loads. */
    {
        int requests = activation_requests, decodes = gameplay_decodes, before = notices;
        sh_package_map_plan *plan;
        expect(&history); expect(&w3);
        CHECK(launch(&history, BY_OFFLINE) == 2 && enter_native());
        CHECK(launch(&w3, BY_OFFLINE) == 2 && live_selections() == 1);
        CHECK(transition(&history) == -1 && activation_requests == requests && gameplay_decodes == decodes);
        CHECK(g_pending_map.state == 2);
        retained_properties(&w3);
        plan = g_pending_map.plan;
        CHECK(enter_native());
        CHECK(transition(&history) == -1 && g_pending_map.state == 4 && activation_requests == requests &&
            gameplay_decodes == decodes);
        CHECK(transition(&w3) == 1);
        loaded(&w3, plan, requests, before);
    }
    /* 11. A stale selection after native entry (the browser cache now names another map) cancels silently: one
     * release, no activation; the same map then loads. */
    {
        int requests = activation_requests, before = notices;
        CHECK(launch(&history, BY_OFFLINE) == 2 && enter_native());
        set_cache(cache, w3.metadata, engine_snapshot);
        sh_rawmap_pending_map_poll();
        CHECK(pending_clear() && !live_selections() && activation_requests == requests);
        show_notices(); CHECK(notices == before);
        complete_launch(&history, BY_OFFLINE, NULL);
    }
    /* 12. A ready request canceled before native entry (a non-published load replaces it): one release, no message;
     * the same map then loads. */
    {
        int before = notices;
        CHECK(launch(&variant, BY_OFFLINE) == 2 && live_selections() == 1);
        CHECK(sh_rawmap_cancel_pending_map() && pending_clear() && !live_selections());
        show_notices(); CHECK(notices == before);
        complete_launch(&variant, BY_DIRECT, NULL);
    }
    /* 13. The engine refuses a covered map's registration: the previous provider returns, one release and one open
     * notice; a retry loads the map. */
    {
        int before = notices, restored = restoring_registrations;
        CHECK(launch(&history, BY_OFFLINE) == 2 && enter_native());
        registration_refusals = 1;
        CHECK(transition(&history) == 0);
        CHECK(pending_clear()); CHECK(!live_selections()); CHECK(restoring_registrations > restored);
        /* R-398: an open over a deferred departure withdraws it in its own activation; refused, the departed map
         * stays served and its context stays held for the next boundary. */
        CHECK(!g_map_context || (g_map_context_departed && sh_package_runtime_departure_deferred()));
        show_notices(); CHECK(notices == before + 1 && !strcmp(notice_text, "This map couldn't be opened."));
        complete_launch(&history, BY_OFFLINE, NULL);
    }
    /* 14. Hosting a lobby for the map the browser shows: the same retained source and compiled document. */
    complete_launch(&variant, BY_LOBBY, NULL);
    /* 15. The player leaves the hosted lobby for the browser before the level transition: one release, no activation,
     * no message. */
    {
        int requests = activation_requests, before = notices;
        CHECK(launch(&history, BY_LOBBY) == 2 && enter_native());
        browser = 1;
        sh_rawmap_pending_map_poll();
        CHECK(pending_clear() && !live_selections() && activation_requests == requests);
        show_notices(); CHECK(notices == before);
    }
    /* 16. The cached file changes while the request waits (another map's bytes arrive under this id): the launch keeps
     * the exact source it selected. */
    {
        char relative[MAX_PATH];
        int requests = activation_requests, before = notices;
        sh_package_map_plan *plan;
        snprintf(relative, sizeof(relative), "published/%s_map.decl", string_body(history.metadata));
        expect(&history);
        CHECK(launch(&history, BY_DIRECT) == 2);
        create(relative, w3.document);
        retained_properties(&history);
        plan = g_pending_map.plan;
        CHECK(enter_native() && transition(&history) == 1);
        create(relative, history.document);
        loaded(&history, plan, requests, before);
    }
    /* 17. Joining a host's lobby (shared multiplayer backend, no live session): the host's map becoming ready takes the
     * same retained request, and its native readiness receives no download request; the map loads. */
    complete_launch(&w3, BY_READY, NULL);
    /* 18. The host's lobby changes to another map before native entry: one release, no activation, no native
     * readiness, no message. */
    {
        int requests = activation_requests, before = notices, launches = native_launches();
        CHECK(launch(&history, BY_READY) == 2);
        host_names(variant.metadata);
        sh_rawmap_pending_map_poll();
        CHECK(pending_clear() && !live_selections() && activation_requests == requests && native_launches() == launches);
        show_notices(); CHECK(notices == before);
    }
    /* 19. The host's lobby ends after native entry, before the level transition: one release, no activation, no
     * message. */
    {
        int requests = activation_requests, before = notices;
        CHECK(launch(&variant, BY_READY) == 2 && enter_native());
        host_names(NULL);
        sh_rawmap_pending_map_poll();
        CHECK(pending_clear() && !live_selections() && activation_requests == requests);
        show_notices(); CHECK(notices == before);
    }

    CHECK(unchanged_on_disk(&native) && unchanged_on_disk(&history) && unchanged_on_disk(&w3) && unchanged_on_disk(&w4) &&
        unchanged_on_disk(&variant));
    CHECK(installed(NULL, NULL) == 1 && installed(HIST_W3_ID, w3_tree) == 1);
    accounted("published source contract");
    published_free(&native); published_free(&history); published_free(&w3); published_free(&w4); published_free(&variant);
    data_root_retired();
}

/* ---- An exactly convertible use through the published request (R137) ---- */
/* package_runtime_context_test's R137 load positive (test_native_inline_load): its map uses and its package, where
 * inline/tough states an int its stock palette base carries and inline/fraction a decimal that base cannot; with the
 * stock texts and shipped palette rows that conversion reads. Copied rather than shared: that test's baseline also
 * serves many other cases. */
#define INLINE_ENTITY(name) "{\"entityDef\":{\"className\":\"Entity\",\"inherit\":\"" name "\",\"name\":\"\"," \
    "\"state\":{\"edit\":{}},\"targetType\":\"idDeclEntityDef\",\"~type\":\"idDeclEntityDef\",\"~version\":26}}"
#define INLINE_VARIABLES "{\"variables\":{\"allocCount\":[0,0,0,0,0],\"string\":[]},\"entities\":["
static const char *const conversion_rows[] = {"inlinestock/row_base", "inlinestock/base_row"};
static int conversion_baseline(void *context, const char *path, unsigned char **body, size_t *length)
{
    const char *text = !strcmp(path, "generated/decls/entitydef/inlinestock/base.decl") ?
            "{ class = \"Entity\"; edit = { health = 1; target = \"\"; } }" :
        !strcmp(path, "generated/decls/entitydef/worldspawn.decl") ?
            "{ class = \"idWorldspawn\"; edit = { globalAIsettings = \"default\"; } }" :
        !strcmp(path, "generated/decls/snapeditorentitydef/inlinestock/row_base.decl") ?
            "{ edit = { previewImage = \"icons/stock\"; } }" :
        !strcmp(path, "generated/decls/snapeditorentitydef/inlinestock/base_row.decl") ?
            "{ inherit = \"inlinestock/row_base\"; edit = { entityDef = \"inlinestock/base\"; } }" : NULL;
    (void)context; *body = NULL; *length = 0;
    if (!text) return 0;
    *length = strlen(text); *body = (unsigned char *)_strdup(text); return *body ? 1 : -1;
}
/* The engine's native state evaluator (sh_native_decl_eval_compare), which the R137 conversion consults before it
 * replaces a package use with native map data. This stand-in evaluates the flat edit members this fixture states: a
 * definition's state is its stock base's edit block overlaid by each private layer down to its own text, and the
 * target and candidate states must agree member for member. Anything else is outside its coverage. */
typedef struct native_state { char keys[8][32], values[8][64]; size_t count; } native_state;
static int native_state_of(const char *text, const sh_native_eval_layer *layers, size_t layer_count, native_state *state,
    int depth)
{
    char error[160] = "", parent[160] = "", path[256];
    unsigned char *stock = NULL;
    size_t stock_length = 0;
    const char *next = NULL;
    sh_decl_node *tree;
    const sh_decl_node *inherit, *edit;
    int ok = 1;
    if (!text || depth > 8 || !(tree = sh_decl_tree_parse_ordered((sh_decl_source){text, strlen(text)}, error, sizeof(error))))
        return 0;
    inherit = sh_decl_tree_member(tree, "inherit"); edit = sh_decl_tree_member(tree, "edit");
    if (inherit && inherit->value) {
        const char *name = inherit->value + (inherit->value[0] == '"');
        snprintf(parent, sizeof(parent), "%.*s", (int)strcspn(name, "\""), name);
        for (size_t i = 0; i < layer_count && !next; i++) if (layers[i].name && !strcmp(layers[i].name, parent)) next = layers[i].text;
        snprintf(path, sizeof(path), "generated/decls/entitydef/%s.decl", parent);
        if (!next && conversion_baseline(NULL, path, &stock, &stock_length) == 1) next = (const char *)stock;
        ok = native_state_of(next, layers, layer_count, state, depth + 1);
    }
    for (const sh_decl_node *member = edit ? edit->children : NULL; ok && member; member = member->next) {
        size_t at = 0;
        if (member->compound || !member->key || !member->value || strlen(member->key) >= 32 || strlen(member->value) >= 64) {
            ok = 0; break;
        }
        while (at < state->count && strcmp(state->keys[at], member->key)) at++;
        if (at == state->count && (ok = state->count < 8) != 0) strcpy_s(state->keys[state->count++], 32, member->key);
        if (ok) strcpy_s(state->values[at], 64, member->value);
    }
    free(stock); sh_decl_tree_free(tree);
    return ok;
}
static int native_compares, native_disagrees;
static sh_native_eval_status engine_compare(const sh_native_eval_layer *layers, size_t layer_count, const char *target,
    const char *candidate, const sh_native_eval_reduction *reduction, char *detail, size_t capacity)
{
    native_state a = {0}, b = {0};
    native_compares++;
    if (native_disagrees) return SH_NATIVE_EVAL_UNEQUAL;
    if (reduction || !native_state_of(target, layers, layer_count, &a, 0) ||
        !native_state_of(candidate, layers, layer_count, &b, 0)) {
        snprintf(detail, capacity, "outside the stand-in's flat edit coverage"); return SH_NATIVE_EVAL_UNSUPPORTED;
    }
    if (a.count != b.count) return SH_NATIVE_EVAL_UNEQUAL;
    for (size_t i = 0; i < a.count; i++) {
        size_t j = 0;
        while (j < b.count && strcmp(a.keys[i], b.keys[j])) j++;
        if (j == b.count || strcmp(a.values[i], b.values[j])) return SH_NATIVE_EVAL_UNEQUAL;
    }
    return SH_NATIVE_EVAL_EQUAL;
}
static void test_published_conversion(void)
{
    static const char whole_body[] = INLINE_VARIABLES INLINE_ENTITY("inline/tough") "]" NATIVE_SLOTS
        ",\"version\":111,\"~type\":\"idSnapMap\",\"~version\":111}";
    static const char mixed_body[] = INLINE_VARIABLES INLINE_ENTITY("inline/tough") "," INLINE_ENTITY("inline/fraction")
        "]" NATIVE_SLOTS ",\"version\":111,\"~type\":\"idSnapMap\",\"~version\":111}";
    static const char converted[] = "\"inherit\":\"inlinestock/base\"";
    unsigned char tree[32], other[32];
    published whole, mixed;
    /* The map package service keeps the root it captured at startup: the same data root, now with the R137 stock
     * baseline and palette rows, and no installed package. */
    admission_setup(&registry_fixture);
    sh_package_runtime_test_dispose(); sh_package_runtime_test_baseline(conversion_baseline, NULL);
    CHECK(sh_package_runtime_refresh(data_root));
    sh_package_runtime_test_source_readers((sh_decl_registry_source){&registry_fixture, admission_read,
        (uintptr_t)registry_fixture.registry, NULL}, (uintptr_t)registry_fixture.reflection);
    sh_package_runtime_test_editor_rows(conversion_rows, sizeof(conversion_rows) / sizeof(*conversion_rows));
    sh_package_runtime_test_native_compare(engine_compare);
    CHECK(sh_mpkg_activation_ready() && !installed(NULL, NULL));
    create("conversion/tough/package.json", "{\"id\":\"tough-load\",\"name\":\"Tough load\"}");
    create("conversion/tough/assets/generated/decls/entitydef/inline/tough.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 40; } }");
    create("conversion/tough/assets/generated/decls/entitydef/inline/fraction.decl",
        "{ inherit = \"inlinestock/base\"; edit = { health = 2.5; } }");
    embedded_make(&whole, "conversion/tough", "tough-load", whole_body, sizeof(whole_body) - 1, "whole-map", "whole",
        "\"snapSlotSettings\"", NULL, tree);
    whole.native_form = converted; expect(&whole);
    embedded_make(&mixed, "conversion/tough", "tough-load", mixed_body, sizeof(mixed_body) - 1, "mixed-map", "mixed",
        "\"snapSlotSettings\"", "health = 2.5", other);
    mixed.native_form = converted; mixed.uses = 2; expect(&mixed);
    CHECK(!memcmp(tree, other, 32));

    /* 20. Every package use converts exactly: the map needs no installation and loads, without a prompt, as the
     * native map data its stock palette base carries, naming no private resource (the fully-native exception). */
    complete_launch(&whole, BY_OFFLINE, NULL);
    CHECK(native_compares && engine_decoded && strstr(engine_decoded, "\"state\":{\"edit\":{\"health\":40}}"));
    CHECK(!installed(NULL, NULL));
    /* 21. The same map when the native evaluator does not confirm equal state: no conversion, so its package is
     * missing and the prompt offers it; declined, one release, nothing installed, no message. */
    {
        int requests = activation_requests, before = notices;
        native_disagrees = 1;
        CHECK(launch(&whole, BY_OFFLINE) == 1 && consent_raised("tough-load"));
        consent_answered(SH_ENGINE_DIALOG_DECLINED);
        sh_rawmap_pending_map_poll();
        CHECK(pending_clear() && !live_selections() && activation_requests == requests && !installed(NULL, NULL));
        show_notices(); CHECK(notices == before);
        native_disagrees = 0;
    }
    /* 22. Mixed: the decimal use keeps its package, so the prompt offers the complete package; once installed, the
     * converted use loads as native data and the other as its own private override. */
    complete_launch(&mixed, BY_DIRECT, "tough-load");
    CHECK(engine_decoded && strstr(engine_decoded, "\"state\":{\"edit\":{\"health\":40}}"));
    CHECK(installed(NULL, NULL) == 1 && installed("tough-load", tree) == 1);

    CHECK(unchanged_on_disk(&whole) && unchanged_on_disk(&mixed));
    accounted("published conversion");
    published_free(&whole); published_free(&mixed);
    sh_package_runtime_test_native_compare(NULL); sh_package_runtime_test_editor_rows(NULL, 0);
    data_root_retired();
}

int main(void)
{
    char temporary[MAX_PATH];
    CHECK(GetTempPathA(sizeof(temporary), temporary)); CHECK(GetTempFileNameA(temporary, "pst", 0, root));
    CHECK(DeleteFileA(root)); CHECK(CreateDirectoryA(root, NULL));
    /* These flows prepare on the engine thread; the worker route is covered by rawmap_paths_test (R-398 perf16). */
    _putenv("SMP_ASYNC_OPEN=0");
    engine_setup();
    test_published_source_contract();
    test_published_conversion();
    free(engine_decoded);
    cleanup();
    if (failures) { fprintf(stderr, "%d check(s) FAILED\n", failures); return 1; }
    puts("published_source_test: all checks passed");
    return 0;
}
