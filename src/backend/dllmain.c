/* XInput backend bootstrap. A worker resolves the host image and engine symbols,
 * checks patch primitives, installs feature hooks, starts the frontend, and arms
 * the fault shield compiled into this same DLL. */
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#include "native_class_registry.h"
#include "signatures.h"
#include "startup_bindings.h"
#include "hook.h"
#include "smoke.h"
#include "rawmap.h"
#include "editor_frame.h"
#include "map_package.h"
#include "map_payload.h"
#include "map_source.h"
#include "map_native.h"
#include "map_published.h"
#include "map_transition.h"
#include "editor_transition.h"
#include "palette_refresh.h"
#include "engine_dialog.h"
#include "../fault_shield/mapload_guards.h"
#include "../fault_shield/serialized_entities_guard.h"
#include "strids.h"
#include "overrides.h"
#include "resource_file_visibility.h"
#include "decl_source_file_reads.h"
#include "grid_room_native.h"
#include "module_ribbon_native.h"
#include "grid_room_editor.h"
#include "map_render.h"
#include "grid_room_snap.h"
#include "spawn_keys.h"
#include "snap_type_cache.h"
#include "native_decl_eval.h"
#include "key_inspector.h"
#include "package_requirements.h"
#include "weapon_hud.h"
#include "fsm_web.h"
#include "navmesh.h"
#include "nav_bake.h"
#include "nav_menu.h"
#include "nav_play.h"
#include "decl_server.h"
#include "commands.h"
#include "cvars.h"
#include "entity.h"
#include "typeinfo.h"
#include "megapreview.h"
#include "imgpreview.h"
#include "prefabpreview.h"
#include "soundpreview.h"
#include "audio_banks_native.h"
#include "package_sound_native.h"
#include "patch.h"
#include "algo.h"
#include "target_any.h"
#include "wiring_cleandirect.h"
#include "swf_textedit.h"
#include "ui_bridge.h"
#include "config.h"
#include "user_overrides.h"
#include "package_runtime.h"
#include "boot_phases.h"
#include "package_editor.h"
#include "package_role.h"
#include "resource_graph_native.h"
#include "resource_graph.h"
#include "resource_model_reads.h"
#include "resource_material_reads.h"
#include "resource_renderprog_reads.h"
#include "resource_md6_reads.h"
#include "resource_joint_reads.h"
#include "resource_presentation.h"
#include "resource_default_reads.h"
#include "decl_default_reads.h"
#include "decl_custom_default_reads.h"
#include "decl_particle_default_reads.h"
#include "resource_metadata_reads.h"
#include "image_exclusion.h"
#include "virtual_material_runtime.h"
#include "virtual_texture_provider.h"
#include "resource_resident.h"
#include "discrete_default.h"
#include "rig_default.h"
#include "audio_files_native.h"
#include "iface_engine.h"
#include "apply_engine.h"
#include "cvar_unlock.h"
#include "backend_log.h"
#include "host_image.h"
#include "engine_globals.h"
#include "../fault_shield/fault_shield.h"
#include "../fault_shield/fault_record.h"
#ifdef SH_DIAG
#include "../fault_shield/shield_diag.h"
#endif

static uint8_t *g_doom_base = NULL;
static size_t   g_doom_size = 0;

static void resolve_doom(void)
{
    /* Resolve the host image for either accepted executable name. */
    g_doom_base = (uint8_t *)sh_host_image_base();
    g_doom_size = sh_host_image_size();
}

static int resource_default_read(void *context, uintptr_t address, void *out, size_t size)
{
    (void)context;
    if (!address || !out || size > UINTPTR_MAX - address) return 0;
    __try { memcpy(out, (const void *)address, size); return 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
static void resource_default_prepare(sh_resource_default_read_receipt *receipt,
    sh_resource_default_read_input *input, void *resource, void *actual_default)
{
    memset(input, 0, sizeof(*input));
    input->image_base = (uintptr_t)sh_host_image_base();
    input->image_size = sh_host_image_size();
    input->renderer = sh_host_is_vulkan();
    input->pinned_image = input->renderer >= 0 && sh_host_is_pinned_renderer_build(input->renderer);
    input->resource = (uintptr_t)resource;
    input->default_callback = (uintptr_t)actual_default;
    input->read = resource_default_read;
    sh_resource_default_read_begin(receipt, input);
    if (receipt->reason == SH_DEFAULT_READ_UNSUPPORTED)
        sh_decl_default_read_begin(receipt, input);
    if (receipt->reason == SH_DEFAULT_READ_UNSUPPORTED)
        sh_decl_custom_default_read_begin(receipt, input);
    if (receipt->reason == SH_DEFAULT_READ_UNSUPPORTED)
        sh_decl_particle_default_read_begin(receipt, input);
}
static int resource_default_finish(sh_resource_default_read_receipt *receipt,
    const sh_resource_default_read_input *input, int returned, int in_order)
{
    int complete = sh_resource_default_read_finish(receipt, input, returned, in_order);
    if (!sh_resource_graph_native_ready() || !sh_resource_graph_recording_requests()) return 0;
    /* Only an unimplemented routine uses the static contract. A supported
     * cleanup whose old state could not be inspected remains incomplete. */
    if (receipt->reason == SH_DEFAULT_READ_UNSUPPORTED && returned && in_order) {
        if (sh_rig_default_observed((void *)input->resource,(void *)input->default_callback)) return 1;
        return sh_resource_graph_native_default_observed((void *)input->resource);
    }
    return complete;
}
static int resource_default_ready(const char *family)
{
    return sh_discrete_default_ready(family) && sh_rig_default_ready(family) &&
        sh_resource_model_reads_default_ready(family) &&
        (!family || _stricmp(family,"articulatedfigure") || sh_resource_joint_reads_ready());
}

/* SteamStub exposes the engine .text after DLL initialization. Poll resolution
 * until the database is available or the timeout permits a partial installation. */
#define PB0_POLL_INTERVAL_MS  75
#define PB0_POLL_TIMEOUT_MS   60000

/* Create runtime data directories for hand-deployed overlays and fresh profiles.
 * Existing directories are harmless; other creation failures are logged. */
static void ensure_user_dirs(void)
{
    static const char *subs[] = { "", "\\strings", "\\overrides", "\\prefabs" };
    char base[MAX_PATH], path[MAX_PATH];
    int i;
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) {
        backend_log("WARN: SHGetFolderPathA(CSIDL_LOCAL_APPDATA) failed -- snapmap-plus user dirs not created");
        return;
    }
    for (i = 0; i < (int)(sizeof subs / sizeof subs[0]); i++) {
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\snapmap-plus%s", base, subs[i]);
        if (!CreateDirectoryA(path, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            char line[MAX_PATH + 64];
            _snprintf_s(line, sizeof line, _TRUNCATE, "WARN: CreateDirectory %s failed (err %lu)",
                        path, GetLastError());
            backend_log(line);
        }
    }
    backend_log("snapmap-plus user-data dirs ensured (root + strings/overrides/prefabs)");
}

/* Out-of-process observers that inline-hook engine functions must not attach
 * while startup installs its detours: either side would steal the other's jump.
 * This per-process manual-reset event is set once startup has finished trying
 * (including an unsupported host), and stays open for later attaches. */
static HANDLE g_startup_hooks_done;

static void startup_hooks_open(void)
{
    wchar_t name[64];
    _snwprintf_s(name, 64, _TRUNCATE, L"Local\\SnapmapPlusStartupHooks.%lu", GetCurrentProcessId());
    g_startup_hooks_done = CreateEventW(NULL, TRUE, FALSE, name);
}

static void startup_hooks_done(void)
{
    sh_boot_phases_mark(SH_BOOT_HOOKS);
    if (g_startup_hooks_done) SetEvent(g_startup_hooks_done);
}

/* R-400: a boot whose first registration waits for the menu prepares its shader sources inside the promotion. */
static char g_boot_data_root[MAX_PATH];
static int boot_prepare_shaders(char *why, size_t capacity)
{ return sh_package_runtime_boot_prepare_shaders(g_boot_data_root, why, capacity); }

static DWORD WINAPI bootstrap_thread(LPVOID p)
{
    (void)p;
    /* Wait up to about 60 seconds for the supported host image. */
    for (int i = 0; i < 6000 && g_doom_base == NULL; i++) {
        resolve_doom();
        if (!g_doom_base) Sleep(10);
    }
    if (g_doom_base == NULL) {
        backend_log("FATAL: host process is not a supported DOOM 2016 build "
                    "(expected DOOMx64vk.exe or DOOMx64.exe)");
        startup_hooks_done();
        return 0;
    }

    char line[160];
    _snprintf_s(line, sizeof line, _TRUNCATE,
        "backend attached host=%s renderer=%s base=%p size=%zx",
        sh_host_image_name(),
        sh_host_is_vulkan() == 1 ? "vulkan" : (sh_host_is_vulkan() == 0 ? "opengl" : "unknown"),
        (void *)g_doom_base, g_doom_size);
    backend_log(line);


    ensure_user_dirs();
    sh_config_init(); /* nonfatal: the service retains defaults and status flags on failure */
    sh_user_overrides_capture_launch_state();
    /* User packages off for this launch: native map documents carry no package
     * ownership, and package-bearing ones are refused at capture. */
    sh_package_runtime_set_serving_disabled(!sh_user_overrides_enabled_for_launch());

    /* R-400: place the first package registration, and start the boot worker now, while the engine is still
     * decrypting and initializing: it needs no engine state. The marker file register-after-menu in the package
     * data root places the first registration after the menu (the runtime pass at the first SnapMap browser). */
    {
        char data_root[MAX_PATH], marker[MAX_PATH + 32];
        int enabled = sh_user_overrides_enabled_for_launch(), after_menu = 0;
        if (sh_overrides_get_root(data_root, sizeof(data_root))) {
            _snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\register-after-menu", data_root);
            after_menu = GetFileAttributesA(marker) != INVALID_FILE_ATTRIBUTES;
        } else data_root[0] = 0;
        sh_boot_placement placement = sh_boot_placement_decide(enabled, after_menu);
        sh_boot_phases_set_placement(placement);
        if (placement != SH_BOOT_PLACE_NONE && data_root[0]) sh_package_runtime_boot_prefetch_start(data_root);
        if (placement == SH_BOOT_PLACE_FIRST_BOUNDARY) {
            strcpy_s(g_boot_data_root, sizeof(g_boot_data_root), data_root);
            sh_decl_server_set_boot_preparation(boot_prepare_shaders);
        }
        char note[320];
        _snprintf_s(note, sizeof(note), _TRUNCATE, "MPKG: boot: the first package registration runs %s%s%s",
            sh_boot_placement_name(placement), placement != SH_BOOT_PLACE_NONE && data_root[0] ?
            "; the boot worker prefetches the warm record's installed reads" : "",
            placement == SH_BOOT_PLACE_FIRST_BOUNDARY ? "; its shader sources are prepared inside the boot promotion" : "");
        backend_log(note);
    }

    /* These scratch-only checks do not depend on engine symbol resolution. */
    sh_patch_selftest();
    sh_algo_selftest();

    /* Retry while startup code may still be encrypted; partial binding is allowed
     * after timeout and each feature must validate its own dependencies. */
    size_t total = sig_db_count();
    size_t db = total < SIG_RESULTS_MAX ? total : SIG_RESULTS_MAX;
    sig_result results[SIG_RESULTS_MAX];
    sh_startup_bindings bindings;
    sh_startup_bindings_init(&bindings, BACKEND_ENGINE_SIGNATURES, results, db);
    DWORD  t0 = GetTickCount();
    size_t last_ok = 0;
    for (;;) {
        int final_pass = total > SIG_RESULTS_MAX || GetTickCount() - t0 >= PB0_POLL_TIMEOUT_MS;
        last_ok = sh_startup_bindings_step(&bindings, g_doom_base,
            sh_resource_graph_native_signature, sh_resource_graph_native_install, final_pass);
        if (last_ok == total) break;
        if (final_pass) break;
        Sleep(PB0_POLL_INTERVAL_MS);
    }
    DWORD elapsed = GetTickCount() - t0;
    sh_boot_phases_mark(SH_BOOT_SIGNATURES);

    /* Reuse the verified pre-install snapshot: an early observer already owns
     * its detours, including on OpenGL where Vulkan RVA fallback is invalid. */
    sh_smoke_run(g_doom_base, results, db, elapsed);

    /* Resolve and log all data anchors here so unsupported dependencies are visible. */
    {
        size_t gtotal = glb_db_count();
        size_t gok    = glb_resolve_all(g_doom_base);
        char gline[128];
        _snprintf_s(gline, sizeof gline, _TRUNCATE,
            "engine globals: %zu/%zu resolved", gok, gtotal);
        backend_log(gline);
    }

    /* Bind features from the final resolution results, including partial results. */
    {
        /* Idempotent after early binding, or report unavailable dependencies
         * after a partial startup. Never infer that earlier loads were seen. */
        sh_resource_graph_native_install(results, db);
        /* Native image exclusion state for scopes that declare g_useImageBlackList 0 (R-309). */
        sh_image_exclusion_install(results, db);
        sh_virtual_material_runtime_install(results, db);
        sh_vt_provider_install(results, db, (void *const *)glb_resolve(g_doom_base, "decl_resource_head", NULL));
        sh_resource_model_reads_install(g_doom_base, sh_resource_graph_native_observe_resource,
            sh_resource_graph_native_observe_origin);
        sh_resource_material_reads_install(g_doom_base, sh_resource_graph_native_observe_resource);
        sh_resource_renderprog_reads_install(g_doom_base, sh_resource_graph_native_observe_resource);
        sh_resource_md6_reads_install(g_doom_base, sh_resource_graph_native_observe_resource);
        sh_resource_joint_reads_install(g_doom_base, sh_resource_graph_native_observe_resource);
        sh_resource_presentation_install(g_doom_base, sh_resource_graph_native_presentation_begin,
            sh_resource_graph_native_presentation_end);
        sh_resource_metadata_reads_bind(g_doom_base);

        /* Install rawmap load interception only on a clean prologue. The swap starts
         * disarmed; package admission also runs inside this detour. */
        void *deser = NULL;
        int   deser_clean = 0;
        for (size_t i = 0; i < db; i++) {
            if (results[i].name && strcmp(results[i].name, "DeserializeFromJson") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    deser = (void *)results[i].addr;
                deser_clean = (results[i].status == SIG_OK);   /* clean scan, not the hook-tolerant fallback */
                break;
            }
        }
        /* Capture installed source identities before load interception can run.
         * Later installs pass the gate after successful runtime registration. */
        {
            char mpkg_root[MAX_PATH];
            sh_mpkg_set_legacy_reader(sh_package_runtime_legacy_read, NULL);
            if (sh_overrides_get_root(mpkg_root, sizeof mpkg_root))
                sh_mpkg_boot_capture(mpkg_root);
            else
                backend_log("MPKG: boot capture SKIPPED -- no effective override root "
                            "(maps declaring packages will be refused, never crashed)");
        }
        sh_rawmap_swap_install(deser, deser_clean);
        /* Answer "is the open map a branch?" for a swapped map, so the engine's own Save asks for
         * a name rather than overwriting the map the rawmap opened over. */
        sh_rawmap_branch_install(g_doom_base);

        /* The editor-frame hook: a main-thread frame boundary, and the in-place map
         * reload it drives for the File menu's Load Rawmap. Installed beside the swap
         * it serves, and like it does not depend on the editor being up. */
        {
            void *ed_frame = NULL, *ed_loadmap = NULL, *ed_addtag = NULL, *ed_tojson = NULL;
            void *ed_readtext = NULL;
            int   ed_frame_clean = 0, ed_load_clean = 0, ed_read_clean = 0;
            for (size_t i = 0; i < db; i++) {
                if (results[i].name == NULL) continue;
                if (strcmp(results[i].name, "EditorFrame") == 0) {
                    if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                        ed_frame = (void *)results[i].addr;
                    ed_frame_clean = (results[i].status == SIG_OK);
                } else if (strcmp(results[i].name, "EditorLoadMap") == 0) {
                    if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                        ed_loadmap = (void *)results[i].addr;
                    ed_load_clean = results[i].status == SIG_OK;
                } else if (strcmp(results[i].name, "ReadLocalSavedMapText") == 0) {
                    if (results[i].status == SIG_OK) ed_readtext = (void *)results[i].addr;
                    ed_read_clean = results[i].status == SIG_OK;
                } else if (strcmp(results[i].name, "SnapMapAddBranchTag") == 0) {
                    if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                        ed_addtag = (void *)results[i].addr;
                } else if (strcmp(results[i].name, "SnapMapToJson") == 0) {
                    if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                        ed_tojson = (void *)results[i].addr;
                }
            }
            if (!sh_editor_transition_install(results, db, g_doom_base))
                backend_log("MPKG: editor saved-read activation boundary unavailable");
            sh_editor_frame_install(ed_frame, ed_frame_clean, ed_loadmap, g_doom_base);
            /* Save Rawmap serializes the open map, not the newest save on disk. The tag
             * function also derives the engine's idStr ctor/dtor. */
            sh_rawmap_set_live_serialize(ed_tojson, ed_addtag);
            sh_rawmap_preflight_install(ed_loadmap, ed_load_clean, ed_readtext, ed_read_clean);
        }

        /* Mirror saves to rawmap JSON. Require a clean serialization prologue before
         * installing this always-active shadow hook. */
        void *serialize = NULL;
        int   serialize_clean = 0;
        for (size_t i = 0; i < db; i++) {
            if (results[i].name && strcmp(results[i].name, "SerializeToJson") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    serialize = (void *)results[i].addr;
                serialize_clean = (results[i].status == SIG_OK);
                break;
            }
        }
        sh_rawmap_save_install(serialize, serialize_clean);
        for (size_t i = 0; i < db; i++) {
            if (results[i].name && !strcmp(results[i].name, "EditorSaveAndPlay")) {
                sh_rawmap_play_install((void *)results[i].addr, results[i].status == SIG_OK);
                break;
            }
        }
        sh_map_payload_install(results, db);
        if (!sh_resource_resident_bind(results, db, g_doom_base))
            backend_log("MPKG: native cached-resource refresh binding unavailable");
        /* Retired discrete animations get their engine default without a GPU readback (R-321). */
        sh_discrete_default_install(g_doom_base);
        sh_rig_default_install(g_doom_base,g_doom_size,sh_host_is_vulkan());
        sh_resource_resident_set_default_construction(sh_discrete_default_enter, sh_discrete_default_leave,
            resource_default_ready);
        sh_resource_resident_set_default_invoke(sh_rig_default_invoke);
        sh_resource_resident_set_default_origins(sh_resource_graph_native_default_origin_begin,
                                                sh_resource_graph_native_default_origin_finish);
        sh_resource_resident_set_instance_refresh(sh_resource_graph_native_refresh_instances);
        sh_resource_resident_set_instance_admit(sh_resource_graph_native_admit_instances);
        sh_resource_resident_set_instance_report(sh_resource_graph_native_admit_report);
        /* A retirement default's frame completes by the engine routine it ran (R-352). */
        sh_resource_resident_set_default_observation(sh_resource_graph_native_default_observed);
        sh_resource_resident_set_default_probe(resource_default_prepare, resource_default_finish);
        {
            const sh_map_transition_callbacks callbacks = {sh_rawmap_transition_pending,
                sh_rawmap_transition_activate, sh_rawmap_transition_commit, sh_rawmap_transition_finished};
            if (!sh_map_transition_install(results, db, g_doom_base, &callbacks))
                backend_log("MPKG: native package activation boundary unavailable");
        }
        if (!sh_map_source_install(results, db, g_doom_base) ||
            !sh_map_native_bind(results, db, g_doom_base) ||
            !sh_map_published_install(results, db, g_doom_base))
            backend_log("MPKG: published-map ownership and launch binding unavailable");

        sh_rawmap_embed_install(g_doom_base);


        /* The render-node guard remains disabled after the apply-path root fix.
         * The separate fault shield is installed normally below. */

        backend_log("rendernode-guard: DISABLED (redundant since the ae_apply_one root fix; "
                    "fault-shield itself is ACTIVE)");

        /* Guard stale event-wire lists and interactables whose subsystem is absent.
         * Their implementations define the engine-layout checks and recovery limits. */
        sh_evwire_guard_install(g_doom_base, results, db);
        sh_interactable_guard_install(g_doom_base, results, db);
        sh_serialized_entities_guard_install(results, db);

        void *get_decls = (void *)sig_addr_by_name(results, db, "GetDeclsOfType");


        /* Bind read-only compiler metadata now. Initial compilation runs on
         * the main thread at the pre-promotion boundary, after native reader
         * setup. Do not wait or scan packages before arming that hook. */
        /* The idClass registry construction reads (read only; no factory). */
        sh_native_class_bind_process(
            glb_resolve(g_doom_base, "class_registry_ready", NULL),
            glb_resolve(g_doom_base, "class_sorted_types", NULL),
            glb_resolve(g_doom_base, "class_sorted_count", NULL),
            glb_resolve(g_doom_base, "managed_class_first", NULL),
            glb_resolve(g_doom_base, "managed_class_last", NULL));
        /* The pool settings those construction routes read (cvar values only). */
        sh_native_pool_bind_process(glb_resolve(g_doom_base, "cvar_system_slot", NULL));
        sh_package_runtime_bind_native(
            glb_resolve(g_doom_base, "declmgr_accessor", NULL),
            glb_resolve(g_doom_base, "type_container", NULL),
            (uintptr_t)GetProcAddress((HMODULE)g_doom_base, "GetGameSystemInterface"));

        /* Inject custom #str_ entries before the first top-level language-table sort.
         * The detour needs a clean prologue; helper functions come from the same scan. */
        void *sort_body = NULL;
        int   sort_clean = 0;
        for (size_t i = 0; i < db; i++) {
            if (results[i].name && strcmp(results[i].name, "StridsSortBody") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    sort_body = (void *)results[i].addr;
                sort_clean = (results[i].status == SIG_OK);
                break;
            }
        }
        void *strids_lea   = (void *)sig_addr_by_name(results, db, "StridsTableLea");
        void *strids_ins   = (void *)sig_addr_by_name(results, db, "StridsInsert");
        void *strids_hash  = (void *)sig_addr_by_name(results, db, "StridsHash");
        void *idstr_ctor   = (void *)sig_addr_by_name(results, db, "IdStrAssign");
        sh_strids_bind_native_localization((const void *)glb_resolve(g_doom_base, "cvar_system_slot", NULL));
        {
            static const sh_strids_palette palette = {NULL, sh_package_runtime_palette};
            sh_strids_set_palette(&palette);
        }
        sh_strids_install(sort_body, sort_clean, strids_lea, strids_ins, strids_hash, idstr_ctor);

        /* Replace resource-provider open-by-name slot +0xF8 after validating the audited
         * provider ABI. User override layers use the immutable launch setting;
         * built-in defaults and engine resources remain available regardless. */
        /* Publish all three clean native idStr helpers before installing the provider. */
        void *res_ctor = NULL;
        int   ctor_clean = 0;
        void *read_string = NULL;
        void *compare = NULL;
        void *write_string = NULL;
        int   read_string_clean = 0;
        int   compare_clean = 0;
        int   write_string_clean = 0;
        for (size_t i = 0; i < db; i++) {
            if (results[i].name && strcmp(results[i].name, "ResProviderCtor") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    res_ctor = (void *)results[i].addr;
                ctor_clean = (results[i].status == SIG_OK);
            }
            if (results[i].name && strcmp(results[i].name, "IdFileReadString") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    read_string = (void *)results[i].addr;
                read_string_clean = (results[i].status == SIG_OK);
            }
            if (results[i].name && strcmp(results[i].name, "IdFileCompare") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    compare = (void *)results[i].addr;
                compare_clean = (results[i].status == SIG_OK);
            }
            if (results[i].name && strcmp(results[i].name, "IdFileWriteString") == 0) {
                if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                    write_string = (void *)results[i].addr;
                write_string_clean = (results[i].status == SIG_OK);
            }
        }
        int overrides_installed = sh_overrides_install(g_doom_base,
                                                       res_ctor, ctor_clean,
                                                       read_string, read_string_clean,
                                                       compare, compare_clean,
                                                       write_string, write_string_clean);

        /* Once a map is running, file and JSON resources read through the
         * resource manager's file open instead of the provider slot; serve the
         * shadow's claims there too. */
        if (overrides_installed && !sh_resource_file_visibility_install(results, db, g_doom_base))
            backend_log("resource-file visibility unavailable: running-state file resources read only the game's archives");
        if (overrides_installed && !sh_decl_source_file_reads_install(g_doom_base))
            backend_log("source-file observation unavailable: native input transitions retain their prior resources");

        /* Dimension-bearing module names use the same backend resource provider.
         * Construct native variants only when a map actually requests one. */
        int grid_installed = overrides_installed && sh_grid_native_install(results, db, g_doom_base);
        if (overrides_installed && !grid_installed)
            backend_log("GRID: native module variants unavailable on this build");
        if (grid_installed && !sh_grid_snap_install(results, db, g_doom_base))
            backend_log("GRID: exact doorway attachment unavailable on this build");
        /* Published package module variants join the module picker; the
         * private lookup above admits them for maps. */
        if (grid_installed && !sh_module_ribbon_install(results, db, g_doom_base))
            backend_log("MODULES: module picker publication unavailable on this build");

        /* Package AI registration keys are private at native dispatch before
         * the first package library compiles (package_keys.h). */
        sh_spawn_keys_install(results, db);

        /* Editor entities read their defaults from a type cache built once;
         * complete it for entity types published afterwards. */
        sh_snap_type_cache_install(results, db, g_doom_base);
        sh_native_decl_eval_install(results, db, g_doom_base);

        /* The AI filter's choices can name package keys, which the stock
         * bit-flag inspector cannot offer or write. */
        sh_key_inspector_install(results, db, g_doom_base);

        /* Navigation uses the resource shadow and map funnel; report its startup state. */
        sh_navmesh_install();

        /* Register cvars before commands. Commands additionally need cmdSystem;
         * each registration path handles missing dependencies independently. */
        void *cvar_reg = (void *)sig_addr_by_name(results, db, "CvarRegister");
        sh_cvars_install(cvar_reg, g_doom_base);

        void *add_cmd  = (void *)sig_addr_by_name(results, db, "AddCommand");
        void *printf_d = (void *)sig_addr_by_name(results, db, "Printf");
        void *cmdsys   = sh_resolve_cmdsys(results, db, g_doom_base);
        sh_commands_install(add_cmd, cmdsys, printf_d, get_decls, g_doom_base);

        /* Capture allowlisted package cvars, then verify them at native publication.
         * Do not alter these gates during startup declaration parsing. */
        {
            char override_root[MAX_PATH];
            void *buffer_cmd = (void *)sig_addr_by_name(results, db, "BufferCommandText");
            void *execute_cmds = (void *)sig_addr_by_name(results, db, "CmdExecuteBuffer");
            if (sh_overrides_get_root(override_root, sizeof(override_root))) {
                sh_package_requirements_install(override_root, g_doom_base, cmdsys, buffer_cmd,
                                                execute_cmds,
                                                sh_user_overrides_enabled_for_launch());
                sh_weapon_hud_install(override_root, g_doom_base, results, db,
                                      sh_user_overrides_enabled_for_launch());
                /* Package state-machine variants bind per layer configuration; the
                 * package runtime publishes each activation's substitutions. */
                sh_fsm_web_install(g_doom_base, results, db);
            } else
                backend_log("package-requirements REFUSED: effective override root unavailable");
        }

        /* Create the shared interface and launch the frontend after sh registration. */
        sh_ui_bridge_install();

        /* Bind apply dependencies before publishing their interface slots. */
        sh_apply_engine_install(results, db, g_doom_base, cmdsys);
        sh_package_editor_bind_native(); sh_palette_refresh_steer(sh_package_editor_row_steer);
        sh_palette_refresh_rows_retired(sh_package_editor_rows_retire);
        sh_package_runtime_lookup_rows_sink(sh_palette_refresh_internal_rows);
        sh_decl_server_rows_retired(sh_package_editor_rows_retire);
        sh_decl_server_image_state(sh_image_exclusion_query);
        if (!sh_package_editor_install(results, db, g_doom_base))
            backend_log("package editor ownership hooks could not be installed");
        sh_package_role_install(results, db, g_doom_base);
        if(grid_installed&&!sh_grid_editor_install(results,db,sh_grid_native_read,sh_grid_native_apply))
            backend_log("GRID: native dimension properties unavailable on this build");
        if(!sh_map_render_install(results,db,g_doom_base)||!sh_map_render_editor_install(results,db))
            backend_log("RENDER: per-map rendering controls unavailable on this build");

        /* Navigation geometry follows editor actions rather than a poll. */
        sh_apply_engine_install_edit_hook(results, db);

        /* Expose live entity reads so navigation sees marks edited since map load. */
        sh_nav_bake_set_live_editor(sh_apply_engine_entity_count,
                                    sh_apply_engine_entity_valid,
                                    sh_apply_engine_entity_json,
                                    sh_apply_engine_entity_transform, NULL);
        sh_nav_bake_set_snapshot(sh_apply_engine_nav_regions, NULL);
        sh_nav_menu_install(results, db);

        /* Read marks at entry to edit-to-build conversion, on the engine main thread.
         * Later bake callbacks can see entities with incomplete defsub state and
         * cannot safely clone them. Install only after the live read surface exists. */
        {
            void *snapbuild = NULL;
            int   snapbuild_clean = 0;
            for (size_t i = 0; i < db; i++) {
                if (results[i].name && strcmp(results[i].name, "SnapMapEditToSnapBuild") == 0) {
                    if (results[i].status == SIG_OK || results[i].status == SIG_OK_HOOKED)
                        snapbuild = (void *)results[i].addr;
                    snapbuild_clean = (results[i].status == SIG_OK);
                    break;
                }
            }
            sh_nav_play_install(snapbuild, snapbuild_clean);
            sh_nav_play_install_instances(results, db);
            sh_nav_play_install_volume_contents(results, db);
        }

        /* Bind engine slots after the interface and apply dependencies exist.
         * The editor singleton resolves through the engine-globals anchor. */
        sh_iface_engine_install(results, db, g_doom_base);

        /* Arm the native palette rebuild before declaration registration can queue it. */
        sh_palette_refresh_install(results, db, g_doom_base);

        /* Install native prompts before the decl server can raise a package dialog.
         * Failed binding leaves the OS message-box fallback available. */
        sh_engine_dialog_install(results, db, g_doom_base);

        sh_audio_banks_native_install(results, db);
        sh_audio_files_native_install(results, db, g_doom_base);
        sh_package_sound_native_install(results, db);

        /* Queue immutable new-declaration registration on the engine main thread.
         * The interface and palette callbacks must be ready before its command runs. */
        if (overrides_installed)
            sh_decl_server_install(results, db, g_doom_base, cmdsys);
        else
            backend_log("decl-server REFUSED: pinned resource-provider ABI was not installed");

        /* Bind dependencies for the already-registered entity commands. */
        sh_entity_install(results, db, g_doom_base, cmdsys);

        /* Bind reflection functions and the signed decl-manager accessor call site. */
        sh_typeinfo_install(results, db, g_doom_base);

        /* Decode virtual-texture previews from installed files through the signed
         * engine page decoder; no renderer or GPU calls are needed. */
        sh_megapreview_install(results, db, g_doom_base);
        /* Load installed-resource indexes for file-based image previews and catalogs. */
        sh_imgpreview_install();
        /* Start bounded prefab geometry previews using the installed-resource index. */
        sh_prefabpreview_install();
        /* Bind editor audio audition and stop functions together. */
        sh_soundpreview_install(results, db, g_doom_base, cmdsys);

        /* Cache math-override dependencies; cs_dontuse remains off until toggled. */
        sh_algo_install(g_doom_base);

        /* Bind registry enumeration for the visibility-toggle command. */
        sh_target_any_install(get_decls);

        /* Bare wire targets receive native references; real nodes retain stock wiring. */
        sh_wiring_cleandirect_install(g_doom_base);

        /* Add SWF clipboard shortcuts; paste requires its own assignment signature. */
        sh_swf_textedit_install(g_doom_base);
    }

    /* Arm the resident fault shield after startup resolution. */

    shield_install(g_doom_base, g_doom_size);

    {
        char line[128];
        _snprintf_s(line, sizeof(line), _TRUNCATE,
            "startup hooks: %d installed, %d owned", hook_installed_count(), hook_owned_count());
        backend_log(line);
    }
    startup_hooks_done();

    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        sh_boot_phases_mark(SH_BOOT_ATTACH);
        backend_set_logpath_from_module(hinst);   /* sh_backend.log under <DOOM>\snapmap-plus\logs\ */
        shield_set_logpath_from_module(hinst);     /* shield_faults.log under <DOOM>\snapmap-plus\logs\ (shield's own log) */
#ifdef SH_DIAG
        /* Diagnostic builds record faults and environment details without recovering. */
        shield_diag_install(hinst);
#endif
        /* Create the readiness event before any observer could look for it. */
        startup_hooks_open();
        /* Don't do engine work in DllMain (loader lock). Spin the bootstrap onto its own thread. */
        HANDLE h = CreateThread(NULL, 0, bootstrap_thread, NULL, 0, NULL);
        if (h) CloseHandle(h);
        else startup_hooks_done();   /* nothing will install; never leave observers waiting */
        /* The independent unlock worker can run before backend resolution completes. */
        sh_cvar_unlock_start();
    }
    else if (reason == DLL_PROCESS_DETACH) {
#ifdef SH_DIAG
        shield_diag_detach();   /* DIAGNOSTIC: record crash-vs-clean-exit in sh_diag.log */
#endif
        /* Process exit releases the preview worker and log handle.
         * Waiting or taking their locks here can deadlock the loader. */
    }
    return TRUE;
}
