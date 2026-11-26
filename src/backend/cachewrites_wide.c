/* Save the engine's cache payload in the actual Windows Known Folder when its narrow path cannot name it. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "cachewrites.h"
#include "wide_path.h"

int sh_cachewrite_ansi(const wchar_t *path, char *out, size_t cap)
{
    UINT cp = GetACP();
    BOOL lossy = FALSE;
    int n;
    if (cp == CP_UTF8)
        n = WideCharToMultiByte(cp, WC_ERR_INVALID_CHARS, path, -1, out, (int)cap, NULL, NULL);
    else
        n = WideCharToMultiByte(cp, WC_NO_BEST_FIT_CHARS, path, -1, out, (int)cap, NULL, &lossy);
    if (n > 1 && !lossy) return 1;
    out[0] = 0;
    return 0;
}

/* A private sibling takes all the bytes before replacing the old cache file. */
static int write_atomic(const wchar_t *path, const void *data, size_t len)
{
    static volatile LONG serial;
    size_t cap = wcslen(path) + 40;
    wchar_t *tmp = malloc(cap * sizeof *tmp);
    HANDLE file = INVALID_HANDLE_VALUE;
    int ok = 0;
    if (!tmp) return 0;
    do {
        swprintf_s(tmp, cap, L"%ls.%lu.%lu.tmp", path, GetCurrentProcessId(), (DWORD)InterlockedIncrement(&serial));
        file = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    } while (file == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS);
    if (file != INVALID_HANDLE_VALUE) {
        size_t at = 0;
        while (at < len) {
            DWORD n = len - at > MAXDWORD ? MAXDWORD : (DWORD)(len - at), written = 0;
            if (!WriteFile(file, (const char *)data + at, n, &written, NULL) || written != n) break;
            at += written;
        }
        ok = CloseHandle(file) && at == len;
        if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
        if (!ok) DeleteFileW(tmp);
    }
    free(tmp);
    return ok;
}

int sh_cachewrite_wide(const wchar_t *saved, const char *rel, const void *data, size_t len)
{
    static const wchar_t base[] = L"id Software\\DOOM\\base";
    if (!sh_cachewrite_matches(rel)) return -1;
    int chars = MultiByteToWideChar(CP_ACP, 0, rel, -1, NULL, 0);
    if (!saved || !chars || (!data && len)) return 0;
    size_t cap = wcslen(saved) + _countof(base) + (size_t)chars + 12;
    wchar_t *path = calloc(cap, sizeof *path);
    if (!path) return 0;
    wcscpy_s(path, cap, saved);
    size_t start = sh_wpath_long(path, cap);
    size_t base_end = start ? sh_wpath_append(path, cap, base) : 0;
    int ok = 0;
    if (base_end && MultiByteToWideChar(CP_ACP, 0, rel, -1, path + base_end + 1, chars)) {
        path[base_end] = L'\\';
        for (wchar_t *q = path + start + 1; *q; q++) {
            if (*q != L'/' && *q != L'\\') continue;
            *q = 0;
            int made = sh_wpath_ensure_dir(path);
            *q = L'\\';
            if (!made) goto finished;
        }
        ok = write_atomic(path, data, len);
    }
finished:
    free(path);
    return ok;
}
