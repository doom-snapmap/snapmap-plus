/* The proxy forwards to the system XInput1_4.dll, found through the system
 * directory rather than a fixed path: after a call, the loaded module is the
 * one in GetSystemDirectoryW, and calls return what the system returns. */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

DWORD WINAPI XInputGetState(DWORD i, void *s);
DWORD WINAPI XInputGetCapabilities(DWORD i, DWORD f, void *c);

int main(void)
{
    unsigned char state[16], caps[32];
    wchar_t system[MAX_PATH], loaded[MAX_PATH];
    HMODULE module;
    int failures = 0;
    DWORD r = XInputGetState(0, state);
    if (r != ERROR_SUCCESS && r != ERROR_DEVICE_NOT_CONNECTED) { printf("FAIL GetState returned %lu\n", r); failures++; }
    r = XInputGetCapabilities(5, 0, caps);   /* no such user index: the system says so */
    if (r != ERROR_BAD_ARGUMENTS && r != ERROR_DEVICE_NOT_CONNECTED) { printf("FAIL GetCapabilities returned %lu\n", r); failures++; }
    module = GetModuleHandleW(L"XInput1_4.dll");
    if (!module || !GetModuleFileNameW(module, loaded, MAX_PATH) || !GetSystemDirectoryW(system, MAX_PATH) ||
        _wcsnicmp(loaded, system, wcslen(system)) != 0) {
        printf("FAIL the system XInput1_4.dll was not loaded from the system directory\n");
        failures++;
    } else wprintf(L"loaded %ls\n", loaded);
    printf("xinput_proxy_test: %d failures\n", failures);
    return failures != 0;
}
