/* XInput proxy exports keep controllers working while the backend loads in DOOM.
 * The backend ships as XINPUT1_4.dll, the name the supported DOOM build imports.
 * Calls resolve the System32 implementation lazily by absolute path, so a
 * forward can never reach this proxy itself.
 * Export through xinput1_4.def only; do not add __declspec(dllexport). DOOM imports
 * GetState and SetState by ordinals 2 and 3, so automatic ordinal assignment can
 * dispatch a controller poll to an incompatible function and corrupt memory. */
#include <windows.h>

#define ERR_DEV_NOT_CONNECTED 1167u   /* ERROR_DEVICE_NOT_CONNECTED -- benign "no controller" fallback */

#define X14 "C:\\Windows\\System32\\XInput1_4.dll"

static HMODULE real_module(void)
{
    HMODULE h = GetModuleHandleA(X14);
    return h ? h : LoadLibraryA(X14);
}

static FARPROC real_proc(const char *name)
{
    HMODULE h = real_module();
    return h ? GetProcAddress(h, name) : NULL;
}

DWORD WINAPI XInputGetState(DWORD i, void *s)
{
    static FARPROC p; if (!p) p = real_proc("XInputGetState");
    return p ? ((DWORD (WINAPI *)(DWORD, void *))p)(i, s) : ERR_DEV_NOT_CONNECTED;
}
DWORD WINAPI XInputSetState(DWORD i, void *v)
{
    static FARPROC p; if (!p) p = real_proc("XInputSetState");
    return p ? ((DWORD (WINAPI *)(DWORD, void *))p)(i, v) : ERR_DEV_NOT_CONNECTED;
}
DWORD WINAPI XInputGetCapabilities(DWORD i, DWORD f, void *c)
{
    static FARPROC p; if (!p) p = real_proc("XInputGetCapabilities");
    return p ? ((DWORD (WINAPI *)(DWORD, DWORD, void *))p)(i, f, c) : ERR_DEV_NOT_CONNECTED;
}
void WINAPI XInputEnable(int enable)
{
    static FARPROC p; if (!p) p = real_proc("XInputEnable");
    if (p) ((void (WINAPI *)(int))p)(enable);
}
DWORD WINAPI XInputGetBatteryInformation(DWORD i, BYTE t, void *b)
{
    static FARPROC p; if (!p) p = real_proc("XInputGetBatteryInformation");
    return p ? ((DWORD (WINAPI *)(DWORD, BYTE, void *))p)(i, t, b) : ERR_DEV_NOT_CONNECTED;
}
DWORD WINAPI XInputGetKeystroke(DWORD i, DWORD r, void *k)
{
    static FARPROC p; if (!p) p = real_proc("XInputGetKeystroke");
    return p ? ((DWORD (WINAPI *)(DWORD, DWORD, void *))p)(i, r, k) : ERR_DEV_NOT_CONNECTED;
}
DWORD WINAPI XInputGetAudioDeviceIds(DWORD i, WCHAR *render, UINT *render_count,
                                     WCHAR *capture, UINT *capture_count)
{
    static FARPROC p; if (!p) p = real_proc("XInputGetAudioDeviceIds");
    return p ? ((DWORD (WINAPI *)(DWORD, WCHAR *, UINT *, WCHAR *, UINT *))p)(
        i, render, render_count, capture, capture_count) : ERR_DEV_NOT_CONNECTED;
}

/* The system DLL's unnamed exports (100 XInputGetStateEx, 101 WaitForGuideButton,
 * 102 CancelGuideButtonWait, 103 PowerOffController, 104 GetBaseBusInformation,
 * 108 GetCapabilitiesEx, 109) take at most four integer or pointer arguments, which
 * the x64 convention passes in registers; forwarding four register-sized values
 * reaches each one with its own arguments intact. */
typedef UINT_PTR (WINAPI *ordinal_fn)(UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR);
static UINT_PTR forward_ordinal(WORD ordinal, FARPROC *cache,
                                UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d)
{
    if (!*cache) {
        HMODULE h = real_module();
        *cache = h ? GetProcAddress(h, MAKEINTRESOURCEA(ordinal)) : NULL;
    }
    return *cache ? ((ordinal_fn)*cache)(a, b, c, d) : ERR_DEV_NOT_CONNECTED;
}
#define SH_XINPUT_ORDINAL(n)                                                        \
    UINT_PTR WINAPI sh_xinput_ordinal_##n(UINT_PTR a, UINT_PTR b, UINT_PTR c, UINT_PTR d) \
    {                                                                               \
        static FARPROC p;                                                           \
        return forward_ordinal(n, &p, a, b, c, d);                                  \
    }
SH_XINPUT_ORDINAL(100)
SH_XINPUT_ORDINAL(101)
SH_XINPUT_ORDINAL(102)
SH_XINPUT_ORDINAL(103)
SH_XINPUT_ORDINAL(104)
SH_XINPUT_ORDINAL(108)
SH_XINPUT_ORDINAL(109)
