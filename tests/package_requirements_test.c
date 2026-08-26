/* package_requirements_test.c -- strict policy parsing and load-state application. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string.h>

#include "package_requirements.h"

static int g_failed;
static int g_buffer_calls;
static char g_buffered[256];
static char g_last_log[512];

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

int main(void)
{
    char temp[MAX_PATH], root[MAX_PATH], overrides[MAX_PATH], package[MAX_PATH];
    char requirements[MAX_PATH], first[MAX_PATH], second[MAX_PATH], marker[MAX_PATH];
    volatile int load_state = 2;
    DWORD n = GetTempPathA(sizeof(temp), temp);
    CHECK(n > 0 && n < sizeof(temp));
    _snprintf_s(root, sizeof(root), _TRUNCATE, "%ssnapmap-plus-package-requirements-%lu",
                temp, GetCurrentProcessId());
    _snprintf_s(overrides, sizeof(overrides), _TRUNCATE, "%s\\overrides", root);
    _snprintf_s(package, sizeof(package), _TRUNCATE, "%s\\my-overrides", overrides);
    _snprintf_s(requirements, sizeof(requirements), _TRUNCATE, "%s\\requirements", package);
    _snprintf_s(first, sizeof(first), _TRUNCATE, "%s\\a.requirements", requirements);
    _snprintf_s(second, sizeof(second), _TRUNCATE, "%s\\z.requirements", requirements);
    CHECK(make_dir(root));
    CHECK(make_dir(overrides));
    CHECK(make_dir(package));
    /* Only the marker makes this a package; without it nothing enumerates it. */
    _snprintf_s(marker, sizeof(marker), _TRUNCATE, "%s\\package.json", package);
    CHECK(write_text(marker, "{}"));
    CHECK(make_dir(requirements));
    CHECK(write_text(first,
                     "# audited cut-content gates\n"
                     "cvar\tg_useResourceBlackList\t0\n"));
    CHECK(write_text(second,
                     "cvar\tg_useImageBlackList\t0\n"
                     "cvar\tg_useResourceBlackList\t0\n"));

    sh_package_requirements_test_reset();
    sh_package_requirements_test_set_load_state(&load_state);
    CHECK(sh_package_requirements_install(root, NULL, (void *)1,
                                          (void *)fake_buffer, 1) == 1);
    CHECK(sh_package_requirements_test_count() == 2);
    CHECK(g_buffer_calls == 0);
    sh_package_requirements_poll();
    CHECK(g_buffer_calls == 0);
    load_state = 3;
    sh_package_requirements_poll();
    CHECK(g_buffer_calls == 1);
    CHECK(strcmp(g_buffered,
                 "g_useImageBlackList 0\n"
                 "g_useResourceBlackList 0\n") == 0);
    sh_package_requirements_poll();
    CHECK(g_buffer_calls == 1);

    /* A package cannot smuggle an arbitrary command or non-audited cvar. */
    sh_package_requirements_test_reset();
    g_buffer_calls = 0;
    g_buffered[0] = '\0';
    CHECK(DeleteFileA(second));
    CHECK(write_text(first, "command\tquit\t1\n"));
    sh_package_requirements_test_set_load_state(&load_state);
    CHECK(sh_package_requirements_install(root, NULL, (void *)1,
                                          (void *)fake_buffer, 1) == 0);
    CHECK(sh_package_requirements_test_count() == 0);
    CHECK(g_buffer_calls == 0);
    CHECK(strstr(g_last_log, "REFUSED") != NULL);

    DeleteFileA(first);
    RemoveDirectoryA(requirements);
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
