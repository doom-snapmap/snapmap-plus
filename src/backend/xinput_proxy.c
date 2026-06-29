/* xinput_proxy.c -- the XInput export surface for the SnapHak BACKEND's XINPUT1_3.dll.
 *
 * Vanilla DOOM imports XINPUT1_3.dll for controller input (XInputGetState/SetState/GetCapabilities --
 * the 3 the OG SnapHak's own XINPUT1_3.dll implements). OUR backend takes that same app-dir slot (the
 * OG SnapHak loader lived in XINPUT1_3.dll too -- our clone occupies the same vector) so our DllMain
 * runs, and these thunks keep input working by forwarding each call to the real System32 XInput at
 * runtime (LoadLibrary by absolute path -> GetProcAddress). Runtime thunks (not .def forwarders)
 * because MSVC's .def `/EXPORT:name=dll.func` treats the dotted target as a local alias, not a
 * forwarder. (Reused verbatim from the fault-shield proxy template -- the backend is
 * a DISTINCT DLL, this is only the shared export-forwarding pattern.)
 *
 * XInputGetDSoundAudioDeviceGuids is XInput-1.3-only -> from XInput9_1_0; the rest from XInput1_4.
 *
 * EXPORT ORDINALS (critical -- do NOT re-add __declspec(dllexport)): DOOM imports XINPUT1_3.dll BY
 * ORDINAL (ord 2=XInputGetState, 3=XInputSetState -- the only two it pulls). The bodies below export via
 * xinput1_3.def (/DEF: in build.ps1), which pins both the names AND the ordinals to match the real
 * System32 XInput1_3.dll. With __declspec auto-numbering these landed ALPHABETICALLY (ord 2 =
 * XInputGetBatteryInformation), so DOOM's per-frame controller poll called the wrong function and the
 * real fn wrote a battery struct through a garbage pointer -> 0xC0000374 heap corruption on any machine
 * with a controller (keyboard/mouse never polls XInput, which is why it slipped through testing).
 */
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
