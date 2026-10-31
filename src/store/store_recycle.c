/* store_recycle.c -- see store_recycle.h. */
#include <windows.h>
#include <shellapi.h>
#include <stdlib.h>
#include <string.h>

#include "store_recycle.h"

/* The plain spelling of a long-form path: \\?\C:\x -> C:\x, \\?\UNC\s\x -> \\s\x. */
static wchar_t *rc_plain(const wchar_t *path)
{
    size_t n = wcslen(path);
    wchar_t *out = (wchar_t *)malloc(sizeof(wchar_t) * (n + 3));
    if (!out) return NULL;
    if (!wcsncmp(path, L"\\\\?\\UNC\\", 8)) {
        out[0] = L'\\';
        out[1] = L'\\';
        wcscpy_s(out + 2, n + 1, path + 8);
    } else if (!wcsncmp(path, L"\\\\?\\", 4)) {
        wcscpy_s(out, n + 3, path + 4);
    } else {
        wcscpy_s(out, n + 3, path);
    }
    return out;
}

static wchar_t *rc_join(const wchar_t *dir, const wchar_t *leaf)
{
    size_t a = wcslen(dir), b = wcslen(leaf);
    wchar_t *p = (wchar_t *)malloc(sizeof(wchar_t) * (a + b + 2));
    if (!p) return NULL;
    memcpy(p, dir, sizeof(wchar_t) * a);
    p[a] = L'\\';
    memcpy(p + a + 1, leaf, sizeof(wchar_t) * (b + 1));
    return p;
}

/* Remove a link itself (a reparse point), never what it points at. */
static int rc_unlink(const wchar_t *path, DWORD attributes)
{
    SetFileAttributesW(path, attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY);
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return RemoveDirectoryW(path) != 0;
    return DeleteFileW(path) != 0;
}

/* Remove every link inside a folder tree (links are not walked into). */
static int rc_unlink_inside(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = rc_join(dir, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    int ok = 1;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_NO_MORE_FILES;
    do {
        wchar_t *child;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        child = rc_join(dir, fd.cFileName);
        if (!child) { ok = 0; continue; }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { if (!rc_unlink(child, fd.dwFileAttributes)) ok = 0; }
        else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (!rc_unlink_inside(child)) ok = 0; }
        free(child);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

int sh_store_recycle(void *context, const wchar_t *path)
{
    sh_store_recycle_stats *st = (sh_store_recycle_stats *)context;
    wchar_t *plain, *from, drive[4];
    SHFILEOPSTRUCTW op;
    DWORD a;
    size_t n;
    int r, ok = 0;
    if (!path || !path[0]) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    a = GetFileAttributesW(path);
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD err = GetLastError();
        return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
    }
    if (a & FILE_ATTRIBUTE_REPARSE_POINT) return rc_unlink(path, a);
    plain = rc_plain(path);
    if (!plain) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    n = wcslen(plain);
    /* The Recycle Bin lives on fixed local disks; elsewhere the shell would
     * delete for good, so nothing is deleted. */
    if (n < 3 || plain[1] != L':' || plain[2] != L'\\') {
        free(plain);
        if (st) st->refused++;
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    drive[0] = plain[0]; drive[1] = L':'; drive[2] = L'\\'; drive[3] = 0;
    if (GetDriveTypeW(drive) != DRIVE_FIXED) {
        free(plain);
        if (st) st->refused++;
        SetLastError(ERROR_NOT_SUPPORTED);
        return 0;
    }
    if (n >= MAX_PATH) {
        free(plain);
        if (st) st->refused++;
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return 0;
    }
    if ((a & FILE_ATTRIBUTE_DIRECTORY) && !rc_unlink_inside(path)) {
        free(plain);
        if (st) st->refused++;
        return 0;
    }
    /* pFrom is a list: the path, then an empty one. */
    from = (wchar_t *)calloc(n + 2, sizeof(wchar_t));
    if (!from) { free(plain); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return 0; }
    memcpy(from, plain, sizeof(wchar_t) * n);
    memset(&op, 0, sizeof op);
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    r = SHFileOperationW(&op);
    if (r == 0 && !op.fAnyOperationsAborted && GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) ok = 1;
    else SetLastError(r ? (DWORD)r : ERROR_CANCELLED);
    if (st) { if (ok) st->recycled++; else st->refused++; }
    free(from);
    free(plain);
    return ok;
}
