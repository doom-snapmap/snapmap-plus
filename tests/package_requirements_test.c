/* Package requirement ownership, native readback, rollback and retry. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "package_requirements.h"
#include "package_runtime.h"

static int g_failed;
static int g_buffer_calls;
static char g_buffered[256];
static char g_last_log[512];
static unsigned char g_cvars[2][0x80], g_cvar_system[0x20];
static void *g_cvar_rows[2], *g_cvar_slot;
static char g_pending[1024];
static int g_execute_calls, g_fault_modes[4], g_fault_count, g_fault_index;
static int g_buffer_fault, g_ignore_next_execute;
static volatile int g_load_state;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                             \
    }                                                                           \
} while (0)

void backend_log(const char *message)
{
    strncpy_s(g_last_log, sizeof(g_last_log), message ? message : "", _TRUNCATE);
}

static void fake_buffer(void *cmdsys, const char *text)
{
    CHECK(cmdsys == (void *)1);
    g_buffer_calls++;
    strncpy_s(g_buffered, sizeof(g_buffered), text ? text : "", _TRUNCATE);
    CHECK(strcat_s(g_pending, sizeof(g_pending), text ? text : "") == 0);
    if (g_buffer_fault) {
        g_buffer_fault = 0;
        RaiseException(0xe0012001u, 0, 0, NULL);
    }
}

static int *value(int index) { return (int *)(g_cvars[index] + 0x30); }

static void fake_execute(void *cmdsys)
{
    char pending[sizeof(g_pending)], *cursor;
    int writes = 0;
    int fault = g_fault_index < g_fault_count ? g_fault_modes[g_fault_index++] : 0;
    CHECK(cmdsys == (void *)1);
    g_execute_calls++;
    strcpy_s(pending, sizeof(pending), g_pending); g_pending[0] = 0;
    if (fault == 1) RaiseException(0xe0012002u, 0, 0, NULL);
    if (g_ignore_next_execute) { g_ignore_next_execute = 0; return; }
    for (cursor = pending; cursor && *cursor; cursor = strchr(cursor, '\n')) {
        char name[80]; int number;
        if (*cursor == '\n') cursor++;
        if (!*cursor) break;
        CHECK(sscanf_s(cursor, "%79s %d", name, (unsigned)sizeof(name), &number) == 2);
        if (!strcmp(name, "g_useImageBlackList")) *value(0) = number;
        else if (!strcmp(name, "g_useResourceBlackList")) *value(1) = number;
        else CHECK(0);
        if (++writes == 1 && fault == 2) RaiseException(0xe0012003u, 0, 0, NULL);
    }
}

static void reset_native(int image, int resource)
{
    sh_package_requirements_test_reset();
    memset(g_cvars, 0, sizeof(g_cvars));
    memset(g_cvar_system, 0, sizeof(g_cvar_system));
    g_cvar_rows[0] = g_cvars[0]; g_cvar_rows[1] = g_cvars[1];
    *(const char **)(g_cvars[0] + 0x40) = "g_useImageBlackList";
    *(const char **)(g_cvars[1] + 0x40) = "g_useResourceBlackList";
    *(void ***)(g_cvar_system + 8) = g_cvar_rows;
    *(unsigned int *)(g_cvar_system + 0x10) = 2;
    g_cvar_slot = g_cvar_system;
    *value(0) = image; *value(1) = resource;
    g_buffer_calls = g_execute_calls = 0;
    g_fault_count = g_fault_index = 0;
    g_buffer_fault = g_ignore_next_execute = 0;
    g_buffered[0] = g_pending[0] = 0;
    g_load_state = 2;
    sh_package_requirements_test_set_load_state(&g_load_state);
    sh_package_requirements_test_set_cvar_slot(&g_cvar_slot);
}

static void fail_execute(int first, int second)
{
    g_fault_modes[0] = first; g_fault_modes[1] = second;
    g_fault_count = second ? 2 : 1; g_fault_index = 0;
}

static int make_dir(const char *path)
{
    return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static int write_text(const char *path, const char *text)
{
    FILE *file = NULL;
    size_t length = strlen(text);
    if (fopen_s(&file, path, "wb") != 0 || !file) return 0;
    if (length && fwrite(text, 1, length, file) != length) {
        fclose(file);
        return 0;
    }
    fclose(file);
    return 1;
}

static void policy(const char *root, const char *marker, const char *cvars)
{
    char text[512];
    _snprintf_s(text, sizeof(text), _TRUNCATE,
                "{\"id\":\"example\",\"name\":\"Example\",\"requirements\":{\"cvars\":%s}}", cvars);
    CHECK(write_text(marker, text));
    CHECK(sh_package_runtime_refresh(root));
}

static int install(const char *root)
{
    return sh_package_requirements_install(root, NULL, (void *)1,
                                           (void *)fake_buffer, (void *)fake_execute, 1);
}

static void map_selection_retains_requirements(const char *root, const char *package, const char *marker)
{
    char *selected;
    size_t selected_length;
    char folders[4][MAX_PATH], resource[MAX_PATH], extra[MAX_PATH], extra_marker[MAX_PATH], error[512];
    const char *used = "{\"value\":\"used\",\"targetType\":\"idDeclEntityDef\"}";
    int before;
    snprintf(folders[0], MAX_PATH, "%s/assets", package);
    snprintf(folders[1], MAX_PATH, "%s/assets/generated", package);
    snprintf(folders[2], MAX_PATH, "%s/assets/generated/decls", package);
    snprintf(folders[3], MAX_PATH, "%s/assets/generated/decls/entitydef", package);
    for (int i = 0; i < 4; i++) CHECK(make_dir(folders[i]));
    snprintf(resource, sizeof(resource), "%s/used.decl", folders[3]);
    CHECK(write_text(resource, "{ edit = { health = 10; } }"));
    snprintf(extra, sizeof(extra), "%s/overrides/editor-tool", root);
    snprintf(extra_marker, sizeof(extra_marker), "%s/package.json", extra);
    CHECK(make_dir(extra));
    CHECK(write_text(extra_marker, "{\"id\":\"editor-tool\",\"name\":\"Editor tool\","
        "\"requirements\":{\"cvars\":{\"g_useResourceBlackList\":0}}}"));
    policy(root, marker, "{\"g_useImageBlackList\":0}");
    reset_native(1, 1);
    CHECK(install(root) && sh_package_requirements_apply_now(NULL));
    CHECK(*value(0) == 0 && *value(1) == 0);
    before = g_execute_calls;
    CHECK(sh_package_runtime_select_map(used, strlen(used), NULL, error, sizeof(error)));
    CHECK(g_execute_calls == before); /* Selecting/inspecting JSON cannot write cvars. */
    selected = sh_package_runtime_active_policy("requirements", &selected_length);
    CHECK(selected && selected_length && strstr(selected, "g_useImageBlackList"));
    CHECK(selected && !strstr(selected, "g_useResourceBlackList"));
    free(selected);
    /* A gameplay subset cannot close gates needed by resident editor resources. */
    g_load_state = 3;
    sh_package_requirements_poll();
    CHECK(sh_package_requirements_apply_now(NULL));
    CHECK(*value(0) == 0 && *value(1) == 0 && g_execute_calls == before);
    CHECK(sh_package_runtime_select_map(NULL, 0, NULL, error, sizeof(error)));
    selected = sh_package_runtime_active_policy("requirements", &selected_length);
    CHECK(selected && !strstr(selected, "g_useImageBlackList") &&
          !strstr(selected, "g_useResourceBlackList"));
    free(selected);
    /* A subsequent vanilla map has no package policy but still shares the
     * native resource registry. A refresh must retain installed requirements. */
    sh_package_requirements_poll();
    CHECK(sh_package_requirements_rearm(root, NULL, 1));
    CHECK(*value(0) == 0 && *value(1) == 0 && g_execute_calls == before);
    CHECK(sh_package_requirements_rearm(root, NULL, 0));
    CHECK(*value(0) == 1 && *value(1) == 1);
    CHECK(DeleteFileA(resource));
    for (int i = 3; i >= 0; i--) CHECK(RemoveDirectoryA(folders[i]));
    CHECK(DeleteFileA(extra_marker)); CHECK(RemoveDirectoryA(extra));
}

int main(void)
{
    char temp[MAX_PATH], root[MAX_PATH], overrides[MAX_PATH], package[MAX_PATH];
    char marker[MAX_PATH];
    const char *both = "{\"g_useResourceBlackList\":0,\"g_useImageBlackList\":0}";
    DWORD n = GetTempPathA(sizeof(temp), temp);
    CHECK(n > 0 && n < sizeof(temp));
    _snprintf_s(root, sizeof(root), _TRUNCATE, "%ssnapmap-plus-package-requirements-%lu",
                temp, GetCurrentProcessId());
    _snprintf_s(overrides, sizeof(overrides), _TRUNCATE, "%s\\overrides", root);
    _snprintf_s(package, sizeof(package), _TRUNCATE, "%s\\my-overrides", overrides);
    CHECK(make_dir(root));
    CHECK(make_dir(overrides));
    CHECK(make_dir(package));
    _snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\package.json", package);
    sh_package_runtime_test_empty_catalog();
    /* Bootstrap can bind commands before any compilation. Polling must not
     * apply an invented empty policy; the first successful compile is read
     * by the normal publication rearm. */
    reset_native(1, 1);
    CHECK(install(root));
    g_load_state = 3;
    sh_package_requirements_poll();
    CHECK(g_buffer_calls == 0 && g_execute_calls == 0);
    CHECK(!sh_package_requirements_apply_now(fake_execute));
    policy(root, marker, both);
    CHECK(sh_package_requirements_rearm(root, fake_execute, 1));
    CHECK(g_execute_calls == 1 && *value(0) == 0 && *value(1) == 0);
    reset_native(1, 1);
    CHECK(install(root));
    CHECK(sh_package_requirements_test_count() == 2);
    CHECK(g_buffer_calls == 0);
    sh_package_requirements_poll();
    CHECK(g_buffer_calls == 0);
    g_load_state = 3;
    sh_package_requirements_poll();
    CHECK(g_buffer_calls == 1);
    CHECK(strcmp(g_buffered,
                 "g_useImageBlackList 0\n"
                 "g_useResourceBlackList 0\n") == 0);
    CHECK(*value(0) == 0 && *value(1) == 0 && g_execute_calls == 1);
    sh_package_requirements_poll();
    CHECK(g_buffer_calls == 1);

    /* Repeated registration captures must not lose identical requests merely
     * because the preceding capture marked the same rows as admitted. */
    CHECK(sh_package_requirements_rearm(root, NULL, 1));
    CHECK(sh_package_requirements_test_count() == 2);
    CHECK(g_buffer_calls == 1); /* Unchanged values need no command/drain. */
    CHECK(strcmp(g_buffered, "g_useImageBlackList 0\ng_useResourceBlackList 0\n") == 0);

    /* Removing one requirement preserves the other's original baseline. */
    policy(root, marker, "{\"g_useResourceBlackList\":0}");
    CHECK(sh_package_requirements_rearm(root, NULL, 1));
    CHECK(*value(0) == 1 && *value(1) == 0);
    policy(root, marker, "{}");
    CHECK(sh_package_requirements_rearm(root, NULL, 1));
    CHECK(*value(0) == 1 && *value(1) == 1);
    CHECK(sh_package_requirements_test_count() == 0);

    /* A user's already-disabled gate must remain disabled after removal. */
    reset_native(0, 1); policy(root, marker, both);
    CHECK(install(root) && sh_package_requirements_apply_now(NULL));
    CHECK(*value(0) == 0 && *value(1) == 0);
    policy(root, marker, "{}");
    CHECK(sh_package_requirements_rearm(root, NULL, 1));
    CHECK(*value(0) == 0 && *value(1) == 1);

    /* A later external edit takes precedence when ownership is released. */
    reset_native(1, 1); policy(root, marker, both);
    CHECK(install(root) && sh_package_requirements_apply_now(NULL));
    *value(1) = 2;
    policy(root, marker, "{}");
    CHECK(sh_package_requirements_rearm(root, NULL, 1));
    CHECK(*value(0) == 1 && *value(1) == 2);

    /* A missing second native cvar refuses before changing the first. */
    reset_native(1, 1); policy(root, marker, both);
    g_cvar_rows[1] = NULL;
    CHECK(install(root) && !sh_package_requirements_apply_now(NULL));
    CHECK(*value(0) == 1 && *value(1) == 1 && g_buffer_calls == 0);
    g_cvar_rows[1] = g_cvars[1];
    CHECK(sh_package_requirements_rearm(root, NULL, 1));
    CHECK(*value(0) == 0 && *value(1) == 0);

    /* Partial native execution rolls the whole change back, then retries. */
    reset_native(1, 1); policy(root, marker, both); fail_execute(2, 0);
    CHECK(install(root) && !sh_package_requirements_apply_now(NULL));
    CHECK(*value(0) == 1 && *value(1) == 1 && g_execute_calls == 2);
    CHECK(sh_package_requirements_rearm(root, NULL, 1));
    CHECK(*value(0) == 0 && *value(1) == 0);
    policy(root, marker, "{}"); fail_execute(2, 0);
    CHECK(!sh_package_requirements_rearm(root, NULL, 1));
    CHECK(*value(0) == 0 && *value(1) == 0);
    CHECK(sh_package_requirements_rearm(root, NULL, 1));
    CHECK(*value(0) == 1 && *value(1) == 1);

    /* A failed rollback cannot become the baseline on an empty retry. */
    reset_native(1, 1); policy(root, marker, both); fail_execute(2, 1);
    CHECK(install(root) && !sh_package_requirements_apply_now(NULL));
    CHECK(*value(0) == 0 && *value(1) == 1);
    policy(root, marker, "{}");
    CHECK(sh_package_requirements_rearm(root, NULL, 1));
    CHECK(*value(0) == 1 && *value(1) == 1);

    /* Command success alone is insufficient: verify native values. */
    reset_native(1, 1); policy(root, marker, both); g_ignore_next_execute = 1;
    CHECK(install(root) && !sh_package_requirements_apply_now(NULL));
    CHECK(*value(0) == 1 && *value(1) == 1);
    CHECK(sh_package_requirements_rearm(root, NULL, 1));
    CHECK(*value(0) == 0 && *value(1) == 0);

    /* An enqueue that throws after appending must drain its rollback too. */
    reset_native(1, 1); policy(root, marker, both); g_buffer_fault = 1;
    CHECK(install(root) && !sh_package_requirements_apply_now(NULL));
    CHECK(*value(0) == 1 && *value(1) == 1 && !g_pending[0]);

    /* Empty launches keep dependencies, and disabling the layer releases them. */
    reset_native(1, 1); policy(root, marker, "{}");
    CHECK(install(root) && sh_package_requirements_apply_now(NULL));
    CHECK(g_buffer_calls == 0);
    policy(root, marker, both);
    CHECK(sh_package_requirements_rearm(root, NULL, 1));
    CHECK(*value(0) == 0 && *value(1) == 0);
    CHECK(sh_package_requirements_rearm(root, NULL, 0));
    CHECK(*value(0) == 1 && *value(1) == 1);

    /* Capture cannot run commands on the bootstrap worker, even at RUNNING. */
    reset_native(1, 1); g_load_state = 3;
    CHECK(install(root) && g_buffer_calls == 0);
    sh_package_requirements_poll();
    CHECK(*value(0) == 0 && *value(1) == 0);

    map_selection_retains_requirements(root, package, marker);
    policy(root, marker, both);

    /* A package cannot smuggle an arbitrary command or non-audited cvar. The
     * package is skipped as a whole: none of its policy, safe or not, applies. */
    reset_native(1, 1);
    CHECK(write_text(marker, "{\"id\":\"example\",\"name\":\"Example\","
        "\"requirements\":{\"cvars\":{\"quit\":1,\"g_useImageBlackList\":0}}}"));
    CHECK(sh_package_runtime_refresh(root));
    {
        char *summary = sh_package_runtime_summary();
        CHECK(summary && strstr(summary, "Skipped local package") && strstr(summary, "unsupported setting"));
        free(summary);
    }
    CHECK(install(root) && sh_package_requirements_apply_now(NULL));
    CHECK(sh_package_requirements_test_count() == 0);
    CHECK(!strstr(g_buffered, "quit"));
    CHECK(!strstr(g_buffered, "g_useImageBlackList"));

    DeleteFileA(marker);
    RemoveDirectoryA(package);
    RemoveDirectoryA(overrides);
    RemoveDirectoryA(root);
    if (g_failed) {
        fprintf(stderr, "package_requirements_test: %d failure(s)\n", g_failed);
        return 1;
    }
    puts("package_requirements_test: PASS");
    return 0;
}
