/* The backend's startup creates exactly the user-data root and its prefabs
 * folder, through wide paths: under a base whose name is outside the ANSI code
 * page and whose length passes MAX_PATH, and again over an existing tree.
 * The settings service initializes over the same root. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "user_dirs.h"
#include "config.h"
#include "../src/common/wide_path.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_failed++; } } while (0)

void backend_log(const char *line) { (void)line; }

static int is_dir(const wchar_t *path)
{
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void remove_tree(const wchar_t *path)
{
    static wchar_t pattern[SH_WPATH_MAX];
    wchar_t *child = (wchar_t *)malloc(SH_WPATH_MAX * sizeof(wchar_t));
    WIN32_FIND_DATAW found;
    HANDLE search;
    if (!child) return;
    wcscpy_s(pattern, SH_WPATH_MAX, path);
    sh_wpath_append(pattern, SH_WPATH_MAX, L"*");
    search = FindFirstFileW(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
            wcscpy_s(child, SH_WPATH_MAX, path);
            sh_wpath_append(child, SH_WPATH_MAX, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else DeleteFileW(child);
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    free(child);
    RemoveDirectoryW(path);
}

static wchar_t g_base[SH_WPATH_MAX], g_top[SH_WPATH_MAX], g_path[SH_WPATH_MAX], g_pattern[SH_WPATH_MAX];

int main(void)
{
    WIN32_FIND_DATAW found;
    HANDLE search;
    char utf8[2048];
    int dirs = 0;
    DWORD n = GetTempPathW(SH_WPATH_MAX, g_top);
    CHECK(n > 0);
    if (g_top[n - 1] == L'\\') g_top[n - 1] = L'\0';
    CHECK(sh_wpath_long(g_top, SH_WPATH_MAX) > 0);
    {
        wchar_t leaf[64];
        /* Cyrillic and Greek: outside every Western ANSI code page. */
        swprintf_s(leaf, 64, L"snapmap-plus-\x0434\x0438\x0440-\x03b1-%lu", GetCurrentProcessId());
        CHECK(sh_wpath_append(g_top, SH_WPATH_MAX, leaf) > 0);
    }
    remove_tree(g_top);
    CHECK(sh_wpath_ensure_dir(g_top));
    wcscpy_s(g_base, SH_WPATH_MAX, g_top);
    while (wcslen(g_base) < 300) {
        CHECK(sh_wpath_append(g_base, SH_WPATH_MAX, L"a-folder-name-long-enough-to-pass-max-path") > 0);
        CHECK(sh_wpath_ensure_dir(g_base));
    }
    CHECK(wcslen(g_base) > MAX_PATH);

    CHECK(sh_user_dirs_ensure_under(g_base) == 2);
    sh_config_test_set_local_appdata(g_base);
    sh_config_init();
    CHECK(sh_user_dirs_ensure_under(g_base) == 2);   /* a second boot over the same tree */

    wcscpy_s(g_path, SH_WPATH_MAX, g_base);
    sh_wpath_append(g_path, SH_WPATH_MAX, L"snapmap-plus\\prefabs");
    CHECK(is_dir(g_path));
    wcscpy_s(g_pattern, SH_WPATH_MAX, g_base);
    sh_wpath_append(g_pattern, SH_WPATH_MAX, L"snapmap-plus\\*");
    search = FindFirstFileW(g_pattern, &found);
    CHECK(search != INVALID_HANDLE_VALUE);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(found.cFileName, L".") || !wcscmp(found.cFileName, L"..")) continue;
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            dirs++;
            if (_wcsicmp(found.cFileName, L"prefabs")) { fwprintf(stderr, L"unexpected folder %ls\n", found.cFileName); CHECK(0); }
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    CHECK(dirs == 1);

    /* The UTF-8 form drops the long-form prefix and keeps every character. */
    CHECK(sh_wpath_utf8(g_path, utf8, sizeof utf8) > 0);
    CHECK(strncmp(utf8, "\\\\?\\", 4) != 0 && strstr(utf8, "\xd0\xb4\xd0\xb8\xd1\x80") != NULL);
    CHECK(sh_wpath_utf8(g_path, utf8, 8) == 0 && utf8[0] == '\0');   /* never a cut path */

    sh_config_test_set_local_appdata(NULL);
    remove_tree(g_top);
    CHECK(!is_dir(g_top));
    if (g_failed) { fprintf(stderr, "startup_dirs_test: %d check(s) failed\n", g_failed); return 1; }
    printf("startup_dirs_test: root and prefabs created through a %zu-character non-ANSI path\n", wcslen(g_base));
    return 0;
}
