/* Verify completed-file publication, nested-fault refusal and write failures. */
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

static BOOL WINAPI test_write_file(HANDLE h, LPCVOID data, DWORD size, LPDWORD wrote, LPOVERLAPPED overlapped)
{
    if (write_mode == 1) return WriteFile(h, data, size - 1, wrote, overlapped);
    if (write_mode == 2)
        crash_report_file("nested", 0, 0, 0, "", "", "", "");
    return WriteFile(h, data, size, wrote, overlapped);
}

static int check_files(int clean)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH], path[MAX_PATH];
    int count = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", g_crash_dir);
    HANDLE find = FindFirstFileA(pattern, &fd);
    assert(find != INVALID_HANDLE_VALUE);
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        assert(strstr(fd.cFileName, ".tmp") == NULL);
        assert(strstr(fd.cFileName, "pending-") == fd.cFileName);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", g_crash_dir, fd.cFileName);
        HANDLE file = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        char buf[8192]; DWORD got = 0;
        assert(file != INVALID_HANDLE_VALUE);
        assert(ReadFile(file, buf, sizeof buf, &got, NULL)); CloseHandle(file);
        assert(got && sh_json_validate(buf, got, 8, NULL));
        count++;
        if (clean) assert(DeleteFileA(path));
    } while (FindNextFileA(find, &fd));
    assert(GetLastError() == ERROR_NO_MORE_FILES); FindClose(find);
    return count;
}

int main(void)
{
    char temporary[MAX_PATH];
    assert(GetTempPathA(sizeof temporary, temporary));
    _snprintf_s(g_crash_dir, sizeof g_crash_dir, _TRUNCATE, "%ssnapmap-crash-test-%lu-%llu",
                temporary, GetCurrentProcessId(), GetTickCount64());
    assert(CreateDirectoryA(g_crash_dir, NULL));
    write_mode = 1;
    crash_report_file("short-write", 0, 0, 0, "", "", "", "");
    assert(check_files(0) == 0 && emitted == 0 && !g_record_busy);
    write_mode = 2;
    crash_report_file("outer", 0, 0, 0, "", "", "", "");
    assert(check_files(0) == 1 && emitted == 1 && g_records_written == 2);
    write_mode = 0;
    for (int i = 0; i < 9; i++) crash_report_file("fatal", 0, 0, 0, "", "", "", "");
    assert(check_files(1) == 7 && emitted == 7);
    assert(RemoveDirectoryA(g_crash_dir));
    puts("crash_report_test OK");
    return 0;
}
