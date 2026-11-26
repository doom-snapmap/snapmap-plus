/* The optional bench argument selects an isolated product data directory for this process. */
#ifndef SH_PROFILE_DIR_H
#define SH_PROFILE_DIR_H
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <wchar.h>
#include "wide_path.h"

/* Caller releases the ordinary absolute path with CoTaskMemFree. */
static __inline wchar_t *sh_profile_resolve(int bench_only)
{
    const wchar_t option[] = L"-snapmap-plus-bench";
    wchar_t **args = NULL, *known = NULL, *result = NULL;
    const wchar_t *base = NULL;
    size_t length, size;
    int argc = 0, i;
    if (wcsstr(GetCommandLineW(), option)) {
        args = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (!args) return NULL;
        for (i = 1; i < argc; ++i) {
            if (wcscmp(args[i], option)) continue;
            if (base || ++i == argc) goto done;
            base = args[i];
            length = wcslen(base);
            if (length < 4 || !((base[0] >= L'A' && base[0] <= L'Z') || (base[0] >= L'a' && base[0] <= L'z')) ||
                base[1] != L':' || (base[2] != L'\\' && base[2] != L'/')) goto done;
        }
    }
    if (!base) {
        if (bench_only) goto done;
#ifdef __cplusplus
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &known)) || !known) goto done;
#else
        if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &known)) || !known) goto done;
#endif
        base = known;
    }
    length = wcslen(base);
    while (length > 3 && (base[length-1] == L'\\' || base[length-1] == L'/')) --length;
    size = length + (known ? sizeof(L"\\snapmap-plus")/sizeof(wchar_t) : 1);
    if (size > SH_WPATH_MAX) goto done;
    result = (wchar_t *)CoTaskMemAlloc(size * sizeof(wchar_t));
    if (result) {
        memcpy(result, base, length * sizeof(wchar_t));
        result[length] = 0;
        if (known) wcscat_s(result, size, L"\\snapmap-plus");
    }
done:
    if (known) CoTaskMemFree(known);
    if (args) LocalFree(args);
    return result;
}

static __inline wchar_t *sh_profile_dir(void)
{
    return sh_profile_resolve(0);
}

static __inline size_t sh_profile_long(wchar_t *out, size_t cap)
{
    wchar_t *dir = sh_profile_dir();
    if (!dir) return 0;
    size_t length = wcslen(dir);
    if (length < cap) memcpy(out, dir, (length + 1) * sizeof(wchar_t));
    CoTaskMemFree(dir);
    return length < cap ? sh_wpath_long(out, cap) : 0;
}
#endif
