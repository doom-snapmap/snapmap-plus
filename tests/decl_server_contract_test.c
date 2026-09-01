/* decl_server_contract_test.c -- startup/signature/source-wiring contract. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

static char *read_file(const char *root, const char *relative)
{
    char path[1024];
    FILE *file = NULL;
    long length;
    char *bytes;
    if (_snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s", root, relative) < 0) return NULL;
    if (fopen_s(&file, path, "rb") != 0 || !file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    bytes = (char *)malloc((size_t)length + 1);
    if (!bytes) { fclose(file); return NULL; }
    if (length && fread(bytes, 1, (size_t)length, file) != (size_t)length) {
        free(bytes); fclose(file); return NULL;
    }
    fclose(file);
    bytes[length] = '\0';
    return bytes;
}

static int count_occurrences(const char *haystack, const char *needle)
{
    int n = 0;
    const char *at = haystack;
    size_t len = strlen(needle);
    while ((at = strstr(at, needle)) != NULL) { n++; at += len; }
    return n;
}

int main(int argc, char **argv)
{
    char *dllmain, *server, *signatures, *build, *overrides, *bridge, *requirements;
    const char *capture, *shadow, *commands, *package_requirements, *decl_server;
    const char *walk, *collisions, *admission;
    const char *classify, *source_lookup, *live_lookup, *table_install, *register_each, *scan, *scan_call;
    const char *materialize, *make_default, *state_byte;
    const char *phase_loop, *phase_scan, *phase_materialize, *phase_palette;
    const char *success_set;
    if (argc != 2) {
        fprintf(stderr, "usage: decl_server_contract_test <repo-root>\n");
        return 2;
    }
    dllmain = read_file(argv[1], "src\\backend\\dllmain.c");
    server = read_file(argv[1], "src\\backend\\decl_server.c");
    signatures = read_file(argv[1], "src\\backend\\signatures.c");
    build = read_file(argv[1], "src\\backend\\build.ps1");
    overrides = read_file(argv[1], "src\\backend\\overrides.c");
    bridge = read_file(argv[1], "src\\backend\\resource_bridge.c");
    requirements = read_file(argv[1], "src\\backend\\package_requirements.c");
    CHECK(dllmain && server && signatures && build && overrides && bridge && requirements);
    if (!dllmain || !server || !signatures || !build || !overrides || !bridge || !requirements) goto done;

    capture = strstr(dllmain, "sh_user_overrides_capture_launch_state();");
    shadow = strstr(dllmain, "sh_overrides_install(g_doom_base");
    commands = strstr(dllmain, "sh_commands_install(add_cmd");
    package_requirements = strstr(dllmain, "sh_package_requirements_install(override_root");
    decl_server = strstr(dllmain, "sh_decl_server_install(results, db");
    CHECK(capture && shadow && commands && package_requirements && decl_server);
    if (capture && shadow) CHECK(capture < shadow);
    if (shadow && commands) CHECK(shadow < commands);
    if (commands && package_requirements) CHECK(commands < package_requirements);
    if (package_requirements && decl_server) CHECK(package_requirements < decl_server);
    if (commands && decl_server) CHECK(commands < decl_server);

    /* Every override consumer discovers packages instead of hardcoding one
     * shared tree, so a package is a folder the user can drag in and delete. */
    CHECK(strstr(server, "sh_packages_enumerate(root, g_packages") != NULL);
    CHECK(strstr(server, "sh_package_subdir(&g_packages[package_index], \"decls\"") != NULL);
    CHECK(strstr(bridge, "sh_packages_enumerate(") != NULL);
    CHECK(strstr(bridge, "sh_package_subdir(&g_packages[i], \"resources\"") != NULL);
    CHECK(strstr(requirements, "sh_packages_enumerate(") != NULL);
    CHECK(strstr(requirements, "sh_package_subdir(&g_packages[package_index],") != NULL);
    /* Packages COMPOSE. An identical decl or manifest row shipped by two
     * packages collapses to one; only a real disagreement is refused, and the
     * refusal has to name who disagreed or it is not actionable. */
    CHECK(strstr(server, "ds_files_identical(other->absolute, absolute_path)") != NULL);
    CHECK(strstr(server, "decl-server COMPOSED:") != NULL);
    CHECK(strstr(server, "item->package") != NULL);
    CHECK(strstr(bridge, "rb_collapse_identical_rows") != NULL);
    CHECK(strstr(bridge, "is claimed by two ") != NULL);
    /* Manifest collection APPENDS per package. Restarting at index zero
     * silently dropped every manifest but the last package's. */
    CHECK(strstr(bridge, "size_t *inout_count") != NULL);
    CHECK(strstr(bridge, "*out_count = 0;") == NULL);
    /* 26 KB of package records must not sit on a capture's stack: it tripped
     * the /GS guard and fast-failed DOOM with 0xC0000409. */
    CHECK(strstr(server, "sh_package packages[SH_PACKAGES_MAX]") == NULL);
    CHECK(strstr(bridge, "sh_package packages[SH_PACKAGES_MAX]") == NULL);
    CHECK(strstr(requirements, "sh_package packages[SH_PACKAGES_MAX]") == NULL);
    CHECK(strstr(server, "snapmap_plus_decl_server_apply") != NULL);
    CHECK(strstr(server, "FILE_FLAG_OPEN_REPARSE_POINT") != NULL);
    CHECK(strstr(server, "DS_REGISTRY_REGISTER_FILE_SLOT 0x38u") != NULL);
    CHECK(strstr(server, "DS_REGISTRY_TYPE_SLOT          0x58u") != NULL);
    CHECK(strstr(server, "DS_IDSTR_SIZE          0x30u") != NULL);
    CHECK(strstr(server, "DS_DECL_ENTITYDEF_OFFSET 0x1c8u") != NULL);
    CHECK(strstr(server, "DS_PINNED_REGISTER_RVA") != NULL);
    CHECK(strstr(server, "ds_clean_at_pinned_rva") != NULL);
    CHECK(strstr(server, "anchor->status != SIG_OK") != NULL);
    CHECK(strstr(server, "AddFromText") == NULL);
    /* The banned dead-end was a raw DeclFind detour plus a process-wide object cache. That ban
     * stands, and is now enforced structurally rather than by keyword: there is exactly ONE engine
     * code patch and its target is the boot promotion, so the lookup path cannot be intercepted. */
    CHECK(strstr(server, "sh_install_detour") == NULL);
    CHECK(strstr(server, "install_inline_hook((void *)boot_promote,") != NULL);

    /* PUBLICATION IS TRIGGERED BY THE ENGINE'S OWN STATIC SNAPSHOT, NOT BY A LOAD-STATE POLL.
     * Every resource is born map-scoped (ctor 0x17FEAC0 writes level 1 or 2 at +0x28) and the
     * transition purge (0x1800E80) frees by a bitwise AND against mask 1/2. Level 4 escapes it, and
     * the only wholesale producer of level 4 is the engine's whole-registry promotion (0x1801830),
     * called once from idCommonLocal::Init at 0x17C6479. Shipped editor content is permanent purely
     * because it was alive when that pass ran. Publishing after it -- which the old RUNNING poll did,
     * by a measured 2.267s -- left new content map-scoped and the first playtest destroyed it. So
     * this service publishes from a one-shot detour ON that promotion, and the engine's own pass
     * then covers our content and its whole closure together. */
    CHECK(strstr(server, "install_inline_hook") != NULL);
    CHECK(strstr(server, "ds_boot_promotion_detour") != NULL);
    CHECK(strstr(server, "DS_PINNED_BOOT_PROMOTE_RVA 0x1801830u") != NULL);
    CHECK(strstr(server, "g_boot_promotion_original();") != NULL);
    /* Exactly one engine code patch, and it is that one. */
    CHECK(count_occurrences(server, "install_inline_hook(") == 1);
    /* The load-state trigger and its command-buffer delivery are gone, not merely bypassed. */
    CHECK(strstr(server, "sh_decl_server_poll") == NULL);
    CHECK(strstr(server, "DS_LOAD_STATE_RUNNING") == NULL);
    CHECK(strstr(server, "g_buffer_command") == NULL);
    /* REFUSE AND CONTINUE. A publication failure now happens during boot, so it must never be able
     * to stop one: the work is inside SEH and the engine's promotion is called unconditionally. */
    CHECK(strstr(server, "__except (EXCEPTION_EXECUTE_HANDLER) {" ) != NULL);
    /* The cut-content gates must be live BEFORE publication parses anything, and nothing drains the
     * command buffer between the detour and the promotion -- so they are applied synchronously. */
    CHECK(strstr(server, "sh_package_requirements_apply_now") != NULL);
    CHECK(strstr(server, "DS_PINNED_CMD_EXECUTE_RVA  0x1AA46B0u") != NULL);
    /* GENERALITY IS THE POINT. The promotion never inspects the objects it promotes, so publishing
     * before it needs no decl type, no resource class and no reference edge. A per-type table or a
     * hand-authored edge walk would only move the failure to the next edge nobody special-cased --
     * which is exactly how the md6Def-only build died. */
    CHECK(strstr(server, "DS_MD6DEF_MODEL_OFFSET") == NULL);
    CHECK(strstr(server, "\"md6Def\"") == NULL);
    CHECK(strstr(server, "\"animWeb\"") == NULL);
    /* And it must NEVER re-parse: FreeData 0xFF-fills joint buffers the render thread reads. */
    CHECK(strstr(server, "TouchDecl") == NULL);
    CHECK(strstr(server, "g_decl_touch") == NULL);
    /* The palette one-shot is no longer load-bearing, so its refusal must not be terminal: a
     * registration that fully succeeded may not be discarded to report the absence of a roster
     * nothing has consulted yet. (Measured live: it does NOT refuse -- the editor singleton and its
     * embedded palette are statically constructed, so the vtable identity it validates holds from
     * CRT init onward. The rebuild runs; it is simply redundant now.) */
    CHECK(strstr(server, "palette_failed") == NULL);
    CHECK(strstr(server, "palette_skipped") != NULL);

    /* THE ORDERING MUST BE PROVED, NOT ASSERTED. Every "before the engine boot promotion" string in
     * this service is its own prose and would read identically if the hook were on the wrong
     * function. So the detour reads a published identity's resource level back out of the engine
     * after the trampoline returns: level 4 is the field the purge (0x1800E80) tests, and only the
     * promotion we just called could have written it. */
    CHECK(strstr(server, "ds_report_promotion_outcome") != NULL);
    CHECK(strstr(server, "boot-promotion PROOF") != NULL);
    CHECK(strstr(server, "boot-promotion PROOF FAILED") != NULL);
    /* Read-only, and pinned as such: the level offset may appear exactly twice -- its #define and
     * the single ds_safe_read that measures it -- and the static value may only ever be COMPARED.
     * The engine writes that field; six earlier builds failed because this service tried to. */
    CHECK(count_occurrences(server, "DS_RESOURCE_LEVEL_OFFSET") == 2);
    CHECK(strstr(server, "ds_safe_read((const uint8_t *)decl + DS_RESOURCE_LEVEL_OFFSET") != NULL);
    CHECK(strstr(server, "level == DS_RESOURCE_LEVEL_STATIC") != NULL);
    CHECK(count_occurrences(server, "DS_RESOURCE_LEVEL_STATIC") == 2);
    /* Lookup-only, so the probe cannot fabricate the object it is measuring. */
    CHECK(strstr(server, "g_find_decl(type_manager, g_probe_name, 0)") != NULL);
    CHECK(strstr(server, "ds_log(\"REGISTERED\"") == NULL);
    CHECK(strstr(server, "ds_log(\"SHADOWED\"") != NULL);
    CHECK(strstr(server, "ds_log(\"REFUSED\"") != NULL);
    CHECK(strstr(server, "Sleep(") == NULL);

    walk = strstr(server, "!ds_walk(&discovery, g_packages[package_index].name, directory");
    collisions = strstr(server,
                        "sh_decl_server_order_and_admit(ordered, discovery.count, DS_MAX_CANDIDATES);");
    admission = strstr(server, "if (!ordered[i].admitted)");
    CHECK(walk && collisions && admission);
    if (walk && collisions) CHECK(walk < collisions);
    if (collisions && admission) CHECK(collisions < admission);
    CHECK(strstr(server, "DS_MAX_DISCOVERED") != NULL);
    CHECK(strstr(server, "ERROR_FILE_NOT_FOUND") != NULL);
    CHECK(strstr(server, "ERROR_PATH_NOT_FOUND") != NULL);
    CHECK(strstr(server, "ERROR_NO_MORE_FILES") != NULL);
    CHECK(strstr(server, "if (search == INVALID_HANDLE_VALUE) return;") == NULL);
    CHECK(strstr(server, "sh_decl_text_order_by_references") != NULL);
    CHECK(strstr(server, "quoted identity edge(s)") != NULL);
    CHECK(strstr(server, "ds_decl_body_is_single_block") != NULL);
    classify = strstr(server, "ds_classify_candidate");
    source_lookup = strstr(server, "source_record = source_find(type_manager, candidate->name);");
    live_lookup = strstr(server, "live_decl = find_decl(type_manager, candidate->name, 0);");
    table_install = strstr(server, "sh_overrides_internal_decl_table_install(entries,");
    register_each = strstr(server, "ds_register_candidate_source(registry, source_name,");
    scan = strstr(server, "register_file(registry, &idstr, NULL) ?");
    scan_call = strstr(server, "if (!ds_scan_and_materialize_missing(");
    phase_loop = strstr(server, "static int ds_scan_and_materialize_missing(");
    phase_scan = phase_loop ? strstr(phase_loop,
                                     "ds_register_candidate_source(registry, source_name,") : NULL;
    phase_materialize = phase_loop ? strstr(phase_loop,
        "ds_materialize_missing_sedefs(registry, type_by_name, source_find,") : NULL;
    phase_palette = phase_loop ? strstr(phase_loop, "palette_refresh()") : NULL;
    materialize = phase_materialize;
    make_default = strstr(server, "decl = context->find_decl(type_manager, candidate->name, 1);");
    state_byte = strstr(server, "DS_DECL_STATE_OFFSET");
    success_set = strstr(server, "InterlockedExchange(&g_registration_succeeded, 1);");
    CHECK(classify && source_lookup && live_lookup && table_install && register_each && scan && scan_call && materialize);
    CHECK(source_lookup < live_lookup);
    CHECK(strstr(server, "g_find_source") != NULL);
    CHECK(strstr(server, "source_find->status != SIG_OK") != NULL);
    CHECK(strstr(server, "DS_PINNED_SOURCE_FIND_RVA") != NULL);
    CHECK(strstr(server, "\"DeclSourceFind\", module_base, DS_PINNED_SOURCE_FIND_RVA") != NULL);
    CHECK(strstr(server, "existing = g_find_decl(type_manager, candidate->name, 0);") == NULL);
    CHECK(strstr(server, "if (!ds_publish_missing_table(missing))") != NULL);
    CHECK(strstr(server, "if (candidate->outcome != DS_CANDIDATE_MISSING) continue;") != NULL);
    CHECK(strstr(server, "ds_register_candidate_source") != NULL);
    CHECK(strstr(server, "ctor(&idstr, source_name)") != NULL);
    CHECK(strstr(server, "dtor(&idstr)") != NULL);
    CHECK(strstr(server, "_stricmp(candidate->type, \"snapEditorEntityDef\")") != NULL);
    CHECK(make_default && state_byte);
    CHECK(phase_loop && phase_scan && phase_materialize && phase_palette);
    CHECK(phase_scan && phase_materialize && phase_scan < phase_materialize);
    CHECK(phase_materialize && phase_palette && phase_materialize < phase_palette);
    CHECK(scan_call && success_set && scan_call < success_set);
    CHECK(strstr(server, "DS_PHASE_FAILURE_SCAN") != NULL);
    CHECK(strstr(server, "DS_PHASE_FAILURE_MATERIALIZATION") != NULL);
    CHECK(strstr(server, "DS_PHASE_FAILURE_PALETTE") != NULL);
    /* The palette phase is still DETECTED and still ordered last -- only its consequence changed.
     * Publication now runs inside idCommonLocal::Init, before the editor object exists, so the
     * one-shot rebuild necessarily refuses; that is the expected outcome, not a failure, and it may
     * not discard a registration that succeeded. The two genuinely terminal phases keep their
     * terminal handling. */
    CHECK(strstr(server, "palette_skipped = 1;") != NULL);
    CHECK(strstr(server, "native registration success was not published") == NULL);
    CHECK(strstr(server, "materialization was terminal; exact decltree table retained; no retry") != NULL);
    CHECK(strstr(server, "DS_DECL_IN_PROGRESS") != NULL);
    /* The native palette validator, not the generic valid bit, is the
     * admission contract for a new editor entity. */
    CHECK(strstr(server, "DS_DECL_VALID") == NULL);
    CHECK(strstr(server, "ds_sedef_palette_ready") != NULL);
    CHECK(strstr(server, "DS_DECL_ENTITYDEF_OFFSET") != NULL);
    CHECK(strstr(server, "DS_TARGET_OUTPUT_FLAG") != NULL);
    CHECK(strstr(server, "DS_TARGET_INPUT_FLAG") != NULL);
    CHECK(strstr(server, "DS_MAX_TARGETS") != NULL);
    CHECK(strstr(server, "entity_def") != NULL);
    CHECK(strstr(server, "NON-PALETTE") != NULL);
    CHECK(strstr(server, "DS_CANDIDATE_NON_PALETTE") != NULL);
    CHECK(strstr(server, "sh_decl_text_sedef_has_materializable_source") != NULL);
    CHECK(strstr(server, "source-only snapEditorEntityDef") != NULL);
    CHECK(strstr(server, "palette admission") != NULL);
    CHECK(strstr(server, "no palette refresh") != NULL);
    CHECK(strstr(server, "SH_OVERRIDES_INTERNAL_DECL_SOURCE") == NULL);
    CHECK(strstr(server, "no retry") != NULL);
    CHECK(strstr(server, "REGISTERED") != NULL);

    {
        char *visibility = read_file(argv[1], "src\\backend\\decl_visibility.c");
        CHECK(visibility != NULL);
        if (visibility) {
            /* Pinned location and slot of the runtime decl-resource existence
             * probe that DeclFind consults from map-load state 2 upward. */
            CHECK(strstr(visibility, "DV_MANAGER_PTR_RVA 0x5557090u") != NULL);
            CHECK(strstr(visibility, "DV_PROBE_SLOT      0x78u") != NULL);
            CHECK(strstr(visibility, "DV_PATH_PREFIX     \"generated/decls/\"") != NULL);
            /* The engine's own answer is always taken first and always wins. */
            CHECK(strstr(visibility, "original = orig(self, path, out1, out2, out3, out4, quiet);") != NULL);
            /* Arg 7 is the quiet flag; dropping it turns silent cache misses
             * into fatal engine errors. */
            CHECK(strstr(visibility, "unsigned char quiet)") != NULL);
            CHECK(strstr(visibility, "if (original) return original;") != NULL);
            /* Only identities this process published are ever corrected. */
            CHECK(strstr(visibility, "sh_overrides_internal_decl_published") != NULL);
            /* The slot is proven to hold the pinned method before it is patched,
             * and this is not a decl lookup detour or an object cache. */
            CHECK(strstr(visibility, "DV_PINNED_PROBE_RVA 0x1806100u") != NULL);
            CHECK(strstr(visibility, "method_rva != (unsigned long long)DV_PINNED_PROBE_RVA") != NULL);
            CHECK(strstr(visibility, "decl_find_fn") == NULL);
            CHECK(strstr(visibility, "make_default") == NULL);
            CHECK(strstr(visibility, "AddFromText") == NULL);
            free(visibility);
        }
    }

    CHECK(strstr(signatures, "\"DeclRegistryAnchor\"") != NULL);
    CHECK(strstr(signatures, "0x184E1D0u") != NULL);
    CHECK(strstr(signatures, "\"DeclRegisterFile\"") != NULL);
    CHECK(strstr(signatures, "0x17B7330u") != NULL);
    CHECK(strstr(signatures, "const idStr *source") != NULL);
    CHECK(strstr(signatures, "\"IdStrCtor\"") != NULL);
    CHECK(strstr(signatures, "0x19FCEF0u") != NULL);
    CHECK(strstr(signatures, "\"IdStrDtor\"") != NULL);
    CHECK(strstr(signatures, "0x19FD120u") != NULL);
    CHECK(strstr(signatures, "\"DeclTypeByName\"") != NULL);
    CHECK(strstr(signatures, "0x17B43B0u") != NULL);
    CHECK(strstr(signatures, "\"DeclAddFromText\"") == NULL);
    CHECK(strstr(signatures, "\"DeclFind\"") != NULL);
    CHECK(strstr(signatures, "0x17B36F0u") != NULL);
    CHECK(strstr(signatures, "\"DeclSourceFind\"") != NULL);
    CHECK(strstr(signatures, "0x17B34B0u") != NULL);
    CHECK(strstr(signatures, "\"IdFileReadString\"") != NULL);
    CHECK(strstr(signatures, "0x267390u") != NULL);
    CHECK(strstr(signatures, "\"IdFileCompare\"") != NULL);
    CHECK(strstr(signatures, "0x267290u") != NULL);
    CHECK(strstr(signatures, "\"IdFileWriteString\"") != NULL);
    CHECK(strstr(signatures, "0x268470u") != NULL);

    CHECK(strstr(build, "\"decl_text.c\"") != NULL);
    CHECK(strstr(build, "\"decl_server_path.c\"") != NULL);
    CHECK(strstr(build, "\"decl_server.c\"") != NULL);
    CHECK(strstr(build, "\"resource_bridge.c\"") != NULL);
    CHECK(strstr(build, "\"package_requirements.c\"") != NULL);
    CHECK(strstr(build, "\"raw_deflate.c\"") != NULL);
    CHECK(strstr(overrides, "sh_decl_text_well_formed") != NULL);
    CHECK(strstr(overrides, "int sh_overrides_get_root") != NULL);
    CHECK(strstr(overrides, "int sh_overrides_internal_decl_table_install") != NULL);
    CHECK(strstr(overrides, "published identity is authoritative") != NULL);
    CHECK(strstr(overrides, "decltree/") != NULL);
    CHECK(strstr(overrides, "sh_resource_bridge_open") != NULL);
    CHECK(strstr(overrides, "31-entry table") != NULL);
    CHECK(strstr(overrides, "OV_PINNED_PROVIDER_VTABLE_RVA") != NULL);
    CHECK(strstr(overrides, "ov_supported_build_abi") != NULL);
    CHECK(strstr(overrides, "OV_STREAM_HELPERS_FAILED") != NULL);
    CHECK(strstr(overrides, "ov_set_length") != NULL);
    CHECK(strstr(server, "sh_resource_bridge_gate_ok") != NULL);
    CHECK(strstr(server, "sh_resource_bridge_decl_count") != NULL);
    CHECK(strstr(bridge, "gameresources.pindex") != NULL);
    CHECK(strstr(bridge, "GENERIC_READ") != NULL);
    CHECK(strstr(bridge, "GENERIC_WRITE") == NULL);
    CHECK(strstr(bridge, "RB_STATE_INSTALLING") != NULL);
    CHECK(strstr(bridge, "InterlockedCompareExchange(&g_state, RB_STATE_INSTALLING, RB_STATE_NEW)") != NULL);
    CHECK(strstr(bridge, "if (!entry->size && entry->zsize) return 0") != NULL);
    CHECK(strstr(bridge, "RB_STATE_FAILED") != NULL);
    CHECK(strstr(requirements, "g_useResourceBlackList") != NULL);
    CHECK(strstr(requirements, "g_useImageBlackList") != NULL);
    CHECK(strstr(requirements, "PR_LOAD_STATE_RUNNING") != NULL);
    CHECK(strstr(requirements, "arbitrary") != NULL);

done:
    free(dllmain); free(server); free(signatures); free(build); free(overrides); free(bridge); free(requirements);
    if (g_failed) {
        fprintf(stderr, "%d decl-server contract test(s) failed\n", g_failed);
        return 1;
    }
    puts("decl server contract tests passed");
    return 0;
}
