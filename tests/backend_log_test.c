/* backend_log_test.c -- the session log lands beside the module, whatever its folder is called. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../src/backend/backend_log.h"

static int failures;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

int main(void)
{
    static wchar_t exe[32768], log[32768];
    char text[4096];
    DWORD n = GetModuleFileNameW(NULL, exe, 32768), got = 0;
    wchar_t *slash = wcsrchr(exe, L'\\');
    HANDLE h;
    CHECK(n > 0 && slash != NULL);
    if (!slash) return 1;
    slash[1] = L'\0';
    _snwprintf_s(log, 32768, _TRUNCATE, L"%lssnapmap-plus\\logs\\sh_backend.log", exe);
    DeleteFileW(log);

    backend_set_logpath_from_module(GetModuleHandleW(NULL));
    backend_log("backend log test: caf\xC3\xA9 line");
    backend_log_close();

    h = CreateFileW(log, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        CHECK(ReadFile(h, text, sizeof text - 1, &got, NULL));
        text[got] = '\0';
        CloseHandle(h);
        CHECK(strstr(text, "[snapmap+] ") != NULL);
        CHECK(strstr(text, "backend log test: caf\xC3\xA9 line\n") != NULL);
    }
    if (failures) { fprintf(stderr, "%d backend log check(s) failed\n", failures); return 1; }
    puts("backend log tests passed");
    return 0;
}
