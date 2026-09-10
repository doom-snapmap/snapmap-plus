/* palette_refresh_contract_test.c -- source/signature wiring contract. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        failed++;                                                               \
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

static int contains_between(const char *begin, const char *end, const char *needle)
{
    const char *hit;
    if (!begin || !end || !needle || begin >= end) return 0;
    hit = strstr(begin, needle);
    return hit && hit < end;
}

int main(int argc, char **argv)
{
    char *refresh, *signatures, *build, *dllmain, *apply, *server;
    const char *install, *iface_install, *decl_install;
    const char *success_set, *refresh_call, *done_after_call, *missing_zero, *missing_end;
    const char *command, *palette_refusal, *palette_end, *usability_failure, *usability_end;
    const char *vtable_check, *builder_call;
    if (argc != 2) {
        fprintf(stderr, "usage: palette_refresh_contract_test <repo-root>\n");
        return 2;
    }
    refresh = read_file(argv[1], "src\\backend\\palette_refresh.c");
    signatures = read_file(argv[1], "src\\backend\\signatures.c");
    build = read_file(argv[1], "src\\backend\\build.ps1");
    dllmain = read_file(argv[1], "src\\backend\\dllmain.c");
    apply = read_file(argv[1], "src\\backend\\apply_engine.c");
    server = read_file(argv[1], "src\\backend\\decl_server.c");
    CHECK(refresh && signatures && build && dllmain && apply && server);
    if (!refresh || !signatures || !build || !dllmain || !apply || !server) goto done;

    CHECK(strstr(signatures, "\"SnapPaletteBuild\"") != NULL);
    CHECK(strstr(signatures, "48 8B C4 56 57 41 54 41 56 41 57 48 81 EC 70 07 00 00") != NULL);
    CHECK(strstr(signatures, "0x54AEE0u") != NULL);
    /* Require a clean unique signature result and consistent address/RVA pair;
     * pinned reference addresses must not gate other supported images. */
    CHECK(strstr(refresh, "builder->status != SIG_OK") != NULL);
    CHECK(strstr(refresh, "builder->addr != (uintptr_t)module_base + builder->rva") != NULL);
    CHECK(strstr(refresh, "builder->rva != PR_BUILDER_RVA") == NULL);
    CHECK(strstr(refresh, "PR_BUILDER_RVA          0x54AEE0u") != NULL);
    /* Locate the singleton through its runtime code anchor, not the pinned audit RVA. */
    CHECK(strstr(refresh, "PR_EDITOR_SINGLETON_RVA") != NULL);
    CHECK(strstr(refresh, "glb_resolve(g_module_base, \"editor_singleton\"") != NULL);
    CHECK(strstr(refresh, "editor != g_module_base + PR_EDITOR_SINGLETON_RVA") == NULL);
    /* The palette vtable is read out of the live editor object, so it is checked for plausibility
     * -- present, and a read-only location in the host image -- not against one build's address. */
    CHECK(strstr(refresh, "PR_PALETTE_VTABLE_RVA   0x20499A0u") != NULL);
    CHECK(strstr(refresh, "pr_address_in_readonly_section(g_module_base, palette_vtable)") != NULL);
    CHECK(strstr(refresh, "palette_vtable != (void *)(g_module_base + PR_PALETTE_VTABLE_RVA)") == NULL);
    CHECK(strstr(refresh, "PR_STATE_PENDING") != NULL);
    CHECK(strstr(refresh, "PR_STATE_APPLIED") != NULL);
    CHECK(strstr(refresh, "PR_STATE_REFUSED") != NULL);
    CHECK(strstr(refresh, "int sh_palette_refresh_after_decl_registration(void)") != NULL);
    CHECK(strstr(refresh, "palette-refresh ARMED") != NULL);
    CHECK(strstr(refresh, "palette-refresh FIRED") != NULL);
    CHECK(strstr(refresh, "palette-refresh SATISFIED") == NULL);
    CHECK(strstr(refresh, "init_byte") == NULL);
    CHECK(strstr(refresh, "PR_EDITOR_INIT_OFF") == NULL);
    CHECK(strstr(refresh, "palette-refresh REFUSED") != NULL);
    CHECK(strstr(refresh, "21088") == NULL);
    CHECK(strstr(refresh, "InterlockedCompareExchange(&g_state, PR_STATE_APPLIED, PR_STATE_PENDING)") != NULL);
    CHECK(strstr(refresh, "Sleep(") == NULL);
    /* Allow rebuilding after each registration pass, including from APPLIED,
     * while keeping REFUSED terminal. */
    CHECK(strstr(refresh, "static int pr_claim(void)") != NULL);
    CHECK(strstr(refresh, "PR_STATE_PENDING, PR_STATE_IDLE") != NULL);
    CHECK(strstr(refresh, "PR_STATE_PENDING, PR_STATE_APPLIED") != NULL);
    CHECK(strstr(refresh, "PR_STATE_PENDING, PR_STATE_REFUSED") == NULL);
    CHECK(strstr(refresh, "not found in palette") != NULL);
    CHECK(strstr(build, "\"palette_refresh.c\"") != NULL);

    /* The editor identity and palette vtable are checked before every native
     * builder call, for either editor initialization state. */
    vtable_check = strstr(refresh, "vtable_status = pr_read_ptr");
    builder_call = strstr(refresh, "g_builder((void *)(editor + PR_EDITOR_PALETTE_OFF), NULL);");
    CHECK(vtable_check && builder_call && vtable_check < builder_call);

    install = strstr(dllmain, "sh_palette_refresh_install(results, db, g_doom_base);");
    iface_install = strstr(dllmain, "sh_iface_engine_install(results, db, g_doom_base);");
    decl_install = strstr(dllmain, "sh_decl_server_install(results, db, g_doom_base, cmdsys);");
    CHECK(install != NULL);
    CHECK(iface_install && install && iface_install < install);
    CHECK(install && decl_install && install < decl_install);

    /* Palette refresh is synchronous at the successful registration command;
     * the ordinary engine tick must never poll or retry it. */
    CHECK(strstr(apply, "sh_palette_refresh_poll") == NULL);
    CHECK(strstr(apply, "palette_refresh") == NULL);
    command = strstr(server, "static void __cdecl ds_apply_command(void)");
    CHECK(command != NULL);
    missing_zero = command ? strstr(command, "if (missing == 0 && shadowed == 0)") : NULL;
    success_set = command ? strstr(command, "InterlockedExchange(&g_registration_succeeded, 1);") : NULL;
    refresh_call = strstr(server, "palette_ok = palette_refresh() ? 1 : 0;");
    done_after_call = success_set ? strstr(success_set, "InterlockedExchange(&g_state, DS_STATE_DONE);") : NULL;
    CHECK(missing_zero && success_set && missing_zero < success_set);
    CHECK(refresh_call && success_set && refresh_call < success_set);
    CHECK(success_set && done_after_call && success_set < done_after_call);
    CHECK(strstr(server, "sh_palette_refresh_after_decl_registration,") != NULL);
    /* A declined or faulting rebuild fails the usable pass. The command must
     * propagate that failure before it can publish registration success. */
    palette_refusal = refresh_call ? strstr(refresh_call, "if (palette_fault || !palette_ok)") : NULL;
    palette_end = palette_refusal ? strstr(palette_refusal, "return 1;") : NULL;
    CHECK(contains_between(palette_refusal, palette_end, "DS_PHASE_FAILURE_PALETTE"));
    CHECK(contains_between(palette_refusal, palette_end, "return 0;"));
    usability_failure = command ? strstr(command, "else if (usability_failed || refused ||") : NULL;
    usability_end = usability_failure ? strstr(usability_failure, "} else {") : NULL;
    CHECK(contains_between(command, usability_failure, "usability_failed = 1;"));
    CHECK(contains_between(usability_failure, usability_end,
                           "InterlockedExchange(&g_state, DS_STATE_FAILED);"));
    CHECK(!contains_between(usability_failure, usability_end, "g_registration_succeeded"));
    CHECK(usability_end && success_set && usability_end < success_set);
    CHECK(strstr(server, "the editor had not built one yet") == NULL);
    CHECK(strstr(server, "palette_skipped") == NULL);
    CHECK(strstr(server, "palette refresh failed after native registration; no retry") == NULL);
    CHECK(strstr(dllmain, "21088") == NULL);
    CHECK(strstr(server, "21088") == NULL);
    missing_end = missing_zero ? strstr(missing_zero, "ds_free_candidates();") : NULL;
    CHECK(missing_zero && missing_end);
    /* A truly empty pass needs no palette; shadow-only edits still reach it.
     * Refused candidates must not turn that empty path into success. */
    CHECK(contains_between(missing_zero, missing_end,
                           "InterlockedExchange(&g_registration_succeeded, refused == 0);"));
    CHECK(contains_between(missing_zero, missing_end,
                           "refused ? DS_STATE_FAILED : DS_STATE_DONE"));
    CHECK(!contains_between(missing_zero, missing_end, "sh_palette_refresh_after_decl_registration"));

done:
    free(refresh); free(signatures); free(build); free(dllmain); free(apply); free(server);
    if (failed) {
        fprintf(stderr, "%d palette-refresh contract test(s) failed\n", failed);
        return 1;
    }
    puts("palette refresh contract tests passed");
    return 0;
}
