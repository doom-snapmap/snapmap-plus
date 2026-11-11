/* Verify completed-file publication, nested-fault refusal and write failures,
 * in a crash folder whose path is outside the ANSI code page and longer than
 * MAX_PATH, and the product version a record carries. */
#include <assert.h>
#include <windows.h>
static BOOL WINAPI test_write_file(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
#define WriteFile test_write_file
#include "../src/fault_shield/crash_report.c"
#undef WriteFile
#include "../src/backend/config_json.h"

uint8_t *g_doom_base;
static int write_mode, emitted;
void shield_emit(const shield_fault *f) { (void)f; emitted++; }
int shield_last_engine_msg(char *out, size_t n) { if (n) out[0] = 0; return 0; }
void shield_capture_stack(const CONTEXT *ctx, char *out, size_t cap, int frames)
{ (void)ctx; (void)frames; if (cap) out[0] = 0; }
const char *sh_host_renderer_name(void) { return "vulkan"; }
static int game_calls;
int sh_host_game_identity(char *out, size_t cap)
{
    game_calls++;
    strcpy_s(out, cap, "DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)");
    return 1;
}

static BOOL WINAPI test_write_file(HANDLE h, LPCVOID data, DWORD size, LPDWORD wrote, LPOVERLAPPED overlapped)
{
    if (write_mode == 1) return WriteFile(h, data, size - 1, wrote, overlapped);
    if (write_mode == 2)
        crash_report_file("nested", 0, 0, 0, "", "", "", "");
    return WriteFile(h, data, size, wrote, overlapped);
}

static wchar_t g_pattern[SH_WPATH_MAX], g_file[SH_WPATH_MAX];

static int check_files(int clean)
{
    WIN32_FIND_DATAW fd;
    int count = 0;
    wcscpy_s(g_pattern, SH_WPATH_MAX, g_crash_dir);
    sh_wpath_append(g_pattern, SH_WPATH_MAX, L"*");
    HANDLE find = FindFirstFileW(g_pattern, &fd);
    assert(find != INVALID_HANDLE_VALUE);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        assert(wcsstr(fd.cFileName, L".tmp") == NULL);
        assert(wcsstr(fd.cFileName, L"pending-") == fd.cFileName);
        wcscpy_s(g_file, SH_WPATH_MAX, g_crash_dir);
        sh_wpath_append(g_file, SH_WPATH_MAX, fd.cFileName);
        HANDLE file = CreateFileW(g_file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        char buf[8192]; DWORD got = 0;
        assert(file != INVALID_HANDLE_VALUE);
        assert(ReadFile(file, buf, sizeof buf, &got, NULL)); CloseHandle(file);
        assert(got && sh_json_validate(buf, got, 8, NULL));
        count++;
        if (clean) assert(DeleteFileW(g_file));
    } while (FindNextFileW(find, &fd));
    assert(GetLastError() == ERROR_NO_MORE_FILES); FindClose(find);
    return count;
}

static wchar_t g_top[SH_WPATH_MAX];

int main(void)
{
    wchar_t leaf[96];
    DWORD n = GetTempPathW(SH_WPATH_MAX, g_top);
    assert(n);
    if (g_top[n - 1] == L'\\') g_top[n - 1] = 0;
    assert(sh_wpath_long(g_top, SH_WPATH_MAX));
    swprintf_s(leaf, 96, L"snapmap-crash-\x043e\x0442\x0447\x0451\x0442-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
    assert(sh_wpath_append(g_top, SH_WPATH_MAX, leaf) && sh_wpath_ensure_dir(g_top));
    wcscpy_s(g_crash_dir, SH_WPATH_MAX, g_top);
    while (wcslen(g_crash_dir) < 320) {
        assert(sh_wpath_append(g_crash_dir, SH_WPATH_MAX, L"deeper-than-max-path-allows"));
        assert(sh_wpath_ensure_dir(g_crash_dir));
    }
    assert(!strcmp(sh_product_version(), "dev"));
    write_mode = 1;
    crash_report_file("short-write", 0, 0, 0, "", "", "", "");
    assert(check_files(0) == 0 && emitted == 0 && !g_record_busy);
    write_mode = 2;
    crash_report_file("outer", 0, 0, 0, "", "", "", "");
    assert(check_files(0) == 1 && emitted == 1 && g_records_written == 2);
    write_mode = 0;
    for (int i = 0; i < 9; i++) crash_report_file("fatal", 0, 0, 0, "", "", "", "");
    assert(check_files(1) == 7 && emitted == 7);
    /* The game identity is taken at arm time only: writing records never asks
     * for it, and a record carries whatever was captured. */
    assert(game_calls == 0);
    sh_host_game_identity(g_game, sizeof g_game); game_calls = 0;
    g_records_written = 0;
    crash_report_file("fatal", 0, 0, 0, "", "", "", "");
    assert(game_calls == 0);
    {
        WIN32_FIND_DATAW fd; char buf[8192]; DWORD got = 0;
        wcscpy_s(g_pattern, SH_WPATH_MAX, g_crash_dir);
        sh_wpath_append(g_pattern, SH_WPATH_MAX, L"pending-*");
        HANDLE find = FindFirstFileW(g_pattern, &fd);
        assert(find != INVALID_HANDLE_VALUE); FindClose(find);
        wcscpy_s(g_file, SH_WPATH_MAX, g_crash_dir);
        sh_wpath_append(g_file, SH_WPATH_MAX, fd.cFileName);
        HANDLE file = CreateFileW(g_file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        assert(file != INVALID_HANDLE_VALUE);
        assert(ReadFile(file, buf, sizeof buf - 1, &got, NULL)); CloseHandle(file); buf[got] = 0;
        assert(strstr(buf, "\"game\":\"DOOM 2016 build 13954591 (20240321-104810-ginger-fuchsia, Vulkan, 3d8b4393)\""));
        assert(strstr(buf, "\"version\":\"dev\""));
        assert(DeleteFileW(g_file));
    }
    while (wcslen(g_crash_dir) > wcslen(g_top)) {
        assert(RemoveDirectoryW(g_crash_dir));
        *wcsrchr(g_crash_dir, L'\\') = 0;
    }
    assert(RemoveDirectoryW(g_top));
    puts("crash_report_test OK");
    return 0;
}
