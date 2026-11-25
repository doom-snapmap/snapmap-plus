/* XInput proxy: the backend ships as XINPUT1_4.dll and forwards to the system DLL, loaded lazily with
 * LOAD_LIBRARY_SEARCH_SYSTEM32 so a forward never reaches this proxy. Export through xinput1_4.def
 * only (no __declspec(dllexport)): DOOM imports ordinals 2 and 3, and automatic numbering would corrupt memory. */
#include <windows.h>

#define ERR_DEV_NOT_CONNECTED 1167u   /* ERROR_DEVICE_NOT_CONNECTED -- benign "no controller" fallback */

/* The system DLL, loaded once by name from the system directory only. A
 * module handle is pointer-sized, so concurrent first calls at worst load it
 * twice, which only raises its reference count. */
static HMODULE real_module(void)
{
    static HMODULE module;
    if (!module) module = LoadLibraryExW(L"XInput1_4.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module;
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

/* The system DLL's unnamed exports (100-104, 108, 109) take at most four integer or pointer arguments,
 * passed in registers on x64, so forwarding four register-sized values preserves each call's arguments. */
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
