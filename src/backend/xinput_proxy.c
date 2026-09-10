/* XInput proxy exports keep controllers working while the backend loads in DOOM.
 * Calls resolve system implementations lazily: XInput1_4 for common functions
 * and XInput9_1_0 for the audio-GUID export.
 * Export through xinput1_3.def only; do not add __declspec(dllexport). DOOM imports
 * GetState and SetState by ordinals 2 and 3, so automatic ordinal assignment can
 * dispatch a controller poll to an incompatible function and corrupt memory. */
#include <windows.h>

#define ERR_DEV_NOT_CONNECTED 1167u   /* ERROR_DEVICE_NOT_CONNECTED -- benign "no controller" fallback */

static FARPROC real_proc(const char *dll_abspath, const char *name)
{
    HMODULE h = GetModuleHandleA(dll_abspath);
    if (!h) h = LoadLibraryA(dll_abspath);
    return h ? GetProcAddress(h, name) : NULL;
}

#define X14 "C:\\Windows\\System32\\XInput1_4.dll"
#define X910 "C:\\Windows\\System32\\XInput9_1_0.dll"

DWORD WINAPI XInputGetState(DWORD i, void *s)
{
    static FARPROC p; if (!p) p = real_proc(X14, "XInputGetState");
    return p ? ((DWORD (WINAPI *)(DWORD, void *))p)(i, s) : ERR_DEV_NOT_CONNECTED;
}
DWORD WINAPI XInputSetState(DWORD i, void *v)
{
    static FARPROC p; if (!p) p = real_proc(X14, "XInputSetState");
    return p ? ((DWORD (WINAPI *)(DWORD, void *))p)(i, v) : ERR_DEV_NOT_CONNECTED;
}
DWORD WINAPI XInputGetCapabilities(DWORD i, DWORD f, void *c)
{
    static FARPROC p; if (!p) p = real_proc(X14, "XInputGetCapabilities");
    return p ? ((DWORD (WINAPI *)(DWORD, DWORD, void *))p)(i, f, c) : ERR_DEV_NOT_CONNECTED;
}
void WINAPI XInputEnable(int enable)
{
    static FARPROC p; if (!p) p = real_proc(X14, "XInputEnable");
    if (p) ((void (WINAPI *)(int))p)(enable);
}
DWORD WINAPI XInputGetBatteryInformation(DWORD i, BYTE t, void *b)
{
    static FARPROC p; if (!p) p = real_proc(X14, "XInputGetBatteryInformation");
    return p ? ((DWORD (WINAPI *)(DWORD, BYTE, void *))p)(i, t, b) : ERR_DEV_NOT_CONNECTED;
}
DWORD WINAPI XInputGetKeystroke(DWORD i, DWORD r, void *k)
{
    static FARPROC p; if (!p) p = real_proc(X14, "XInputGetKeystroke");
    return p ? ((DWORD (WINAPI *)(DWORD, DWORD, void *))p)(i, r, k) : ERR_DEV_NOT_CONNECTED;
}
DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD i, void *render, void *capture)
{
    static FARPROC p; if (!p) p = real_proc(X910, "XInputGetDSoundAudioDeviceGuids");
    return p ? ((DWORD (WINAPI *)(DWORD, void *, void *))p)(i, render, capture) : ERR_DEV_NOT_CONNECTED;
}
