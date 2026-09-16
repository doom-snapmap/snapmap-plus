#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include "audio_files.h"
#include "package_sources.h"

C_ASSERT(offsetof(sh_audio_file_descriptor, sector) == 8);
C_ASSERT(offsetof(sh_audio_file_descriptor, block) == 12);
C_ASSERT(offsetof(sh_audio_file_descriptor, custom) == 16);
C_ASSERT(offsetof(sh_audio_file_descriptor, handle) == 24);
C_ASSERT(offsetof(sh_audio_file_descriptor, device) == 32);

/* A relative name that walks out of the resource namespace with a ".." segment
 * is not a name the compiled provider may answer. */
static int af_escapes(const wchar_t *name)
{
    for (const wchar_t *at = name; *at; at++) {
        if (at != name && at[-1] != L'/' && at[-1] != L'\\') continue;
        if (at[0] != L'.' || at[1] != L'.') continue;
        if (!at[2] || at[2] == L'/' || at[2] == L'\\') return 1;
    }
    return name[0] == L'.' && name[1] == L'.' && (!name[2] || name[2] == L'/' || name[2] == L'\\');
}

int sh_audio_file_path(const wchar_t *name, uint32_t id, int by_id,
    const void *flags, const wchar_t *bank_prefix, const wchar_t *media_prefix,
    const wchar_t *language, char **path)
{
    const wchar_t *prefix = L"", *locale = L"";
    const wchar_t root[] = L"sound/soundbanks/pc/";
    wchar_t number[32], *wide;
    char *utf8;
    size_t a, b, c, r = sizeof(root)/sizeof(root[0])-1, total;
    uint32_t company = 0, codec = 0;
    int length, external = 0;
    if (!path) return -1;
    *path = NULL;
    if (flags) {
        memcpy(&company, flags, 4);
        memcpy(&codec, (const unsigned char *)flags+4, 4);
        if (((const unsigned char *)flags)[24] && language) locale = language;
    }
    if (by_id) {
        if (!flags || company > 1) return 0;
        prefix = codec == 0 ? bank_prefix : media_prefix;
        swprintf_s(number, sizeof(number)/sizeof(number[0]),
            codec == 0 ? L"%u.bnk" : L"%u.wem", id);
        name = number;
    } else {
        if (!name || !name[0]) return 0;
        /* An external source names its own complete location: the native
         * location base writes no base path and no bank directory for it, and
         * still applies the language directory when the request is localized.
         * Reproduce exactly that, so a legitimate request under the resource
         * namespace is answered while an escaping one is left native. */
        external = flags && company == 0 && codec == 201;
        if (!external && flags && company == 0 && codec == 0) prefix = bank_prefix;
        if (external) r = 0;
    }
    if (!prefix) prefix = L"";
    /* Do not reinterpret absolute filenames as package-relative names. */
    if (name[0] == L'/' || name[0] == L'\\' || wcschr(name, L':')) return 0;
    if (af_escapes(name)) return 0;
    a = wcslen(prefix); b = wcslen(locale); c = wcslen(name);
    if (a > SIZE_MAX-r-2 || b > SIZE_MAX-r-a-2 || c > SIZE_MAX-r-a-b-2) return -1;
    total = r+a+b+(b != 0)+c+1;
    if (total > SIZE_MAX/sizeof(wchar_t)) return -1;
    wide = (wchar_t *)malloc(total*sizeof(wchar_t));
    if (!wide) return -1;
    memcpy(wide, root, r*sizeof(wchar_t));
    memcpy(wide+r, prefix, a*sizeof(wchar_t));
    memcpy(wide+r+a, locale, b*sizeof(wchar_t));
    if (b) wide[r+a+b] = L'/';
    memcpy(wide+r+a+b+(b != 0), name, (c+1)*sizeof(wchar_t));
    length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
    if (!length) { free(wide); return -1; }
    utf8 = (char *)malloc((size_t)length);
    if (!utf8) { free(wide); return -1; }
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, utf8, length, NULL, NULL)) {
        free(utf8); free(wide); return -1;
    }
    free(wide);
    *path = sh_package_engine_path(utf8);
    free(utf8);
    return *path ? 1 : 0;
}

int sh_audio_file_open(const char *path, uint32_t device,
    sh_audio_file_provider provider, void *context,
    unsigned char *synchronous, sh_audio_file_descriptor *out)
{
    FILE *stream = NULL;
    uint64_t length = 0;
    HANDLE handle = NULL;
    LARGE_INTEGER actual;
    intptr_t original;
    int result;
    sh_audio_file_descriptor candidate = {0};
    if (!path || !provider || !synchronous || !out || device == UINT32_MAX) return -1;
    result = provider(context, path, &stream, &length);
    if (result != 1) { if (stream) fclose(stream); return result == 0 ? 0 : -1; }
    if (!stream) return -1;
    original = _get_osfhandle(_fileno(stream));
    if (original != -1 && original != -2 && GetFileType((HANDLE)original) == FILE_TYPE_DISK &&
        GetFileSizeEx((HANDLE)original, &actual) && actual.QuadPart >= 0 &&
        (uint64_t)actual.QuadPart == length)
        DuplicateHandle(GetCurrentProcess(), (HANDLE)original, GetCurrentProcess(),
            &handle, 0, FALSE, DUPLICATE_SAME_ACCESS);
    fclose(stream);
    if (!handle || handle == INVALID_HANDLE_VALUE) return -1;
    candidate.length = length;
    candidate.handle = handle;
    candidate.device = device;
    *out = candidate;
    *synchronous = 1;
    return 1;
}
