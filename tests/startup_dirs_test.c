/* R-021: the backend's startup path creates only the user-data root, strings
 * and prefabs -- never overrides\ or package-cache\, which belonged to the
 * retired package system.
 *
 *   startup_dirs_test <repository root>
 *
 * 1. Runs the startup folder creation (user_dirs.c) and the settings service
 *    initialization (config.c) against a temporary local-app-data root, then
 *    lists what exists under snapmap-plus\.
 * 2. Scans the backend, frontend, fault-shield and installer sources for any
 *    remaining mention of the retired folders, so a later change cannot bring
 *    their creation back unnoticed. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "user_dirs.h"
#include "config.h"

static int g_failed;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_failed++; } } while (0)

void backend_log(const char *line) { (void)line; }

static int is_dir(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void remove_tree(const char *path)
{
    char pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", path);
    search = FindFirstFileA(pattern, &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
            _snprintf_s(child, sizeof child, _TRUNCATE, "%s\\%s", path, found.cFileName);
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(child);
            else DeleteFileA(child);
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    RemoveDirectoryA(path);
}

static void startup_creates_only_the_kept_folders(void)
{
    char temp[MAX_PATH], base[MAX_PATH], root[MAX_PATH], path[MAX_PATH], pattern[MAX_PATH];
    wchar_t wide[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    int dirs = 0;
    GetTempPathA(sizeof temp, temp);
    _snprintf_s(base, sizeof base, _TRUNCATE, "%ssnapmap-plus-startup-%lu", temp, (unsigned long)GetCurrentProcessId());
    remove_tree(base);
    CHECK(CreateDirectoryA(base, NULL));
    _snprintf_s(root, sizeof root, _TRUNCATE, "%s\\snapmap-plus", base);

    /* The startup order of dllmain.c's bootstrap: folders, then settings. */
    CHECK(sh_user_dirs_ensure_under(base) == 3);
    CHECK(MultiByteToWideChar(CP_ACP, 0, base, -1, wide, MAX_PATH) > 0);
    sh_config_test_set_local_appdata(wide);
    sh_config_init();
    /* A second boot over an existing tree is the same. */
    CHECK(sh_user_dirs_ensure_under(base) == 3);

    CHECK(is_dir(root));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\strings", root); CHECK(is_dir(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\prefabs", root); CHECK(is_dir(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\overrides", root); CHECK(!is_dir(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\package-cache", root); CHECK(!is_dir(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\package-staging", root); CHECK(!is_dir(path));

    /* Nothing else appears as a folder. */
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", root);
    search = FindFirstFileA(pattern, &found);
    CHECK(search != INVALID_HANDLE_VALUE);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            dirs++;
            if (_stricmp(found.cFileName, "strings") && _stricmp(found.cFileName, "prefabs")) {
                fprintf(stderr, "unexpected folder created at startup: %s\n", found.cFileName);
                CHECK(0);
            }
        } while (FindNextFileA(search, &found));
        FindClose(search);
    }
    CHECK(dirs == 2);
    for (int i = 0; sh_user_dirs_subfolders[i]; i++) {
        CHECK(!strstr(sh_user_dirs_subfolders[i], "overrides"));
        CHECK(!strstr(sh_user_dirs_subfolders[i], "package"));
    }
    sh_config_test_set_local_appdata(NULL);
    remove_tree(base);
}

/* --- source scan ---------------------------------------------------------- */

static const char *const RETIRED[] = { "package-cache", "package-staging", "override-backups",
                                       "\\\\overrides", "\"overrides\"", "/overrides/", "overrides\\\\" };
static int g_files;

static char *read_text(const char *path)
{
    FILE *f = NULL;
    long n;
    char *buf;
    if (fopen_s(&f, path, "rb") || !f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = (char *)malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) buf[n] = 0;
    else { free(buf); buf = NULL; }
    fclose(f);
    return buf;
}

static int scanned_extension(const char *name)
{
    static const char *const ext[] = { ".c", ".h", ".cpp", ".go", ".ps1", ".js", ".html", ".def", ".inc" };
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (size_t i = 0; i < sizeof ext / sizeof ext[0]; i++) if (!_stricmp(dot, ext[i])) return 1;
    return 0;
}

static void scan_tree(const char *dir)
{
    char pattern[MAX_PATH], path[MAX_PATH];
    WIN32_FIND_DATAA found;
    HANDLE search;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    search = FindFirstFileA(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(found.cFileName, ".") || !strcmp(found.cFileName, "..")) continue;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, found.cFileName);
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* Test fixtures may describe old maps; the installer's test data and
             * vendored third-party trees are not product code. */
            if (!_stricmp(found.cFileName, "testdata") || !_stricmp(found.cFileName, "node_modules") ||
                !_stricmp(found.cFileName, "obj")) continue;
            scan_tree(path);
        } else if (scanned_extension(found.cFileName) && !strstr(found.cFileName, "_test.")) {
            char *text = read_text(path);
            g_files++;
            if (!text) { fprintf(stderr, "cannot read %s\n", path); CHECK(0); continue; }
            for (size_t i = 0; i < sizeof RETIRED / sizeof RETIRED[0]; i++)
                for (const char *hit = strstr(text, RETIRED[i]); hit; hit = strstr(hit + 1, RETIRED[i])) {
                    /* A line may name the retired folders only to keep them out
                     * (the installer's migration filter), and says so. */
                    const char *line = hit, *end = strchr(hit, '\n');
                    size_t n;
                    char *copy;
                    int allowed = 0;
                    while (line > text && line[-1] != '\n') line--;
                    if (!end) end = hit + strlen(hit);
                    n = (size_t)(end - line);
                    copy = (char *)malloc(n + 1);
                    if (copy) {
                        memcpy(copy, line, n); copy[n] = 0;
                        allowed = strstr(copy, "retired package folder") != NULL;
                        free(copy);
                    }
                    if (!allowed) {
                        fprintf(stderr, "%s mentions the retired folder %s\n", path, RETIRED[i]);
                        CHECK(0);
                    }
                }
            free(text);
        }
    } while (FindNextFileA(search, &found));
    FindClose(search);
}

int main(int argc, char **argv)
{
    static const char *const roots[] = { "src\\backend", "src\\ui", "src\\fault_shield", "src\\common", "installer" };
    startup_creates_only_the_kept_folders();
    if (argc < 2) { fprintf(stderr, "usage: startup_dirs_test <repository root>\n"); return 2; }
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        char dir[MAX_PATH];
        _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\%s", argv[1], roots[i]);
        CHECK(is_dir(dir));
        scan_tree(dir);
    }
    CHECK(g_files > 100);
    if (g_failed) { fprintf(stderr, "startup_dirs_test: %d check(s) failed\n", g_failed); return 1; }
    printf("startup_dirs_test: startup creates root, strings and prefabs only; %d source files name no retired folder\n", g_files);
    return 0;
}
