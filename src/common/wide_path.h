/* Wide file paths with no MAX_PATH limit.
 *
 * Every path is built as UTF-16 and handed to the W file APIs in its long form
 * ("\\?\C:\..." or "\\?\UNC\server\share\..."), so a game folder or user name
 * outside the ANSI code page, or deeper than 260 characters, works. Text that
 * leaves the process (logs, JSON records) takes the UTF-8 form, without the
 * long-form prefix. Buffers are the caller's; SH_WPATH_MAX wide characters
 * hold any path Windows accepts. Nothing here allocates except the known-folder
 * lookup, so the rest is usable from a fault handler. Header-only: callers link
 * shell32 and ole32 for sh_wpath_local_appdata. */
#ifndef SNAPMAP_PLUS_WIDE_PATH_H
#define SNAPMAP_PLUS_WIDE_PATH_H

#include <windows.h>
#include <shlobj.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SH_WPATH_MAX 32768
/* The same longest path in UTF-8: at most 3 bytes for each UTF-16 unit
 * (a surrogate pair is 2 units and 4 bytes). */
#define SH_U8PATH_MAX (32768 * 3)   /* SH_WPATH_MAX * 3; spelled alike in modlegacy.h */

static const wchar_t SH_WPATH_LONG_PREFIX_[] = L"\\\\?\\";
static const wchar_t SH_WPATH_UNC_PREFIX_[] = L"\\\\?\\UNC\\";

/* Rewrite an absolute path in place into its long form; returns its length,
 * or 0 when it is not absolute or does not fit. */
static __inline size_t sh_wpath_long(wchar_t *path, size_t cap)
{
    size_t len, add;
    const wchar_t *prefix;
    size_t skip = 0;
    if (!path || !cap) return 0;
    len = wcslen(path);
    if (!wcsncmp(path, SH_WPATH_LONG_PREFIX_, 4)) return len;                  /* already long */
    if (len >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) prefix = SH_WPATH_LONG_PREFIX_;
    else if (len >= 3 && path[0] == L'\\' && path[1] == L'\\') { prefix = SH_WPATH_UNC_PREFIX_; skip = 2; }
    else return 0;
    add = wcslen(prefix);
    if (len - skip + add + 1 > cap) return 0;
    memmove(path + add, path + skip, (len - skip + 1) * sizeof(wchar_t));
    memcpy(path, prefix, add * sizeof(wchar_t));
    for (size_t i = add; path[i]; i++) if (path[i] == L'/') path[i] = L'\\';
    return len - skip + add;
}

/* The directory holding module (NULL: the process executable), long form,
 * without a trailing separator. Returns its length, or 0. */
static __inline size_t sh_wpath_module_dir(void *module, wchar_t *out, size_t cap)
{
    DWORD n;
    wchar_t *slash;
    if (!out || cap < 2 || cap > 0xFFFFFFFFu) return 0;
    n = GetModuleFileNameW((HMODULE)module, out, (DWORD)cap);
    if (n == 0 || n >= cap) { out[0] = L'\0'; return 0; }
    slash = wcsrchr(out, L'\\');
    if (!slash) { out[0] = L'\0'; return 0; }
    *slash = L'\0';
    if (!sh_wpath_long(out, cap)) { out[0] = L'\0'; return 0; }
    return wcslen(out);
}

/* %LOCALAPPDATA% (the known folder), long form. Returns its length, or 0. */
static __inline size_t sh_wpath_local_appdata(wchar_t *out, size_t cap)
{
    PWSTR known = NULL;
    size_t len;
    if (!out || !cap) return 0;
    out[0] = L'\0';
#ifdef __cplusplus
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &known)) || !known) {
#else
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, 0, NULL, &known)) || !known) {
#endif
        if (known) CoTaskMemFree(known);
        return 0;
    }
    len = wcslen(known);
    if (len + 1 <= cap) memcpy(out, known, (len + 1) * sizeof(wchar_t));
    CoTaskMemFree(known);
    if (len + 1 > cap || !sh_wpath_long(out, cap)) { out[0] = L'\0'; return 0; }
    return wcslen(out);
}

/* Append "\leaf" to path. Returns the new length, or 0 (path unchanged) when
 * it does not fit. leaf may itself contain separators. */
static __inline size_t sh_wpath_append(wchar_t *path, size_t cap, const wchar_t *leaf)
{
    size_t len, add;
    if (!path || !leaf) return 0;
    len = wcslen(path);
    add = wcslen(leaf);
    if (len + 1 + add + 1 > cap) return 0;
    path[len] = L'\\';
    memcpy(path + len + 1, leaf, (add + 1) * sizeof(wchar_t));
    for (size_t i = len + 1; path[i]; i++) if (path[i] == L'/') path[i] = L'\\';
    return len + 1 + add;
}

/* Append "\leaf" where leaf is UTF-8. */
static __inline size_t sh_wpath_append_utf8(wchar_t *path, size_t cap, const char *leaf)
{
    size_t len;
    int n;
    if (!path || !leaf) return 0;
    len = wcslen(path);
    if (len + 2 > cap || cap - len - 1 > 0x7FFFFFFF) return 0;
    path[len] = L'\\';
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, leaf, -1, path + len + 1, (int)(cap - len - 1));
    if (n <= 0) { path[len] = L'\0'; return 0; }
    for (size_t i = len + 1; path[i]; i++) if (path[i] == L'/') path[i] = L'\\';
    return wcslen(path);
}

/* Create the directory unless it exists. Returns 1 when it exists afterwards. */
static __inline int sh_wpath_ensure_dir(const wchar_t *path)
{
    DWORD attributes;
    if (!path || !path[0]) return 0;
    if (CreateDirectoryW(path, NULL)) return 1;
    attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

/* The path as UTF-8, without any long-form prefix, for logs and records.
 * Returns the byte length, or 0 (out empty) when it does not fit. */
static __inline size_t sh_wpath_utf8(const wchar_t *path, char *out, size_t cap)
{
    const wchar_t *p = path;
    int n;
    if (!out || !cap) return 0;
    out[0] = '\0';
    if (!path) return 0;
    if (!wcsncmp(p, SH_WPATH_UNC_PREFIX_, 8)) {
        /* \\?\UNC\server\share -> \\server\share */
        if (cap < 3) return 0;
        out[0] = '\\'; out[1] = '\\';
        n = WideCharToMultiByte(CP_UTF8, 0, p + 8, -1, out + 2, (int)(cap > 0x7FFFFFFF ? 0x7FFFFFFF : cap) - 2,
                                NULL, NULL);
        if (n <= 0) { out[0] = '\0'; return 0; }
        return strlen(out);
    }
    if (!wcsncmp(p, SH_WPATH_LONG_PREFIX_, 4)) p += 4;
    n = WideCharToMultiByte(CP_UTF8, 0, p, -1, out, (int)(cap > 0x7FFFFFFF ? 0x7FFFFFFF : cap), NULL, NULL);
    if (n <= 0) { out[0] = '\0'; return 0; }
    return (size_t)n - 1;
}

#ifdef __cplusplus
}
#endif

#endif /* SNAPMAP_PLUS_WIDE_PATH_H */
