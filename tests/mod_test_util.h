/* mod_test_util.h -- temporary folders and file edits for the mod tests. */
#ifndef SNAPMAP_PLUS_MOD_TEST_UTIL_H
#define SNAPMAP_PLUS_MOD_TEST_UTIL_H

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int mt_temp_dir(const char *tag, char *out, size_t cap)
{
    char base[MAX_PATH];
    if (!GetTempPathA(sizeof base, base)) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%ssmp_%s_%lu_%lu", base, tag, GetCurrentProcessId(), GetTickCount());
    return CreateDirectoryA(out, NULL) != 0;
}

/* Remove a tree, clearing read-only attributes (store blobs are read-only). */
static void mt_remove_tree(const char *dir)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2], child[MAX_PATH * 2];
    HANDLE h;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            _snprintf_s(child, sizeof child, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
            SetFileAttributesA(child, FILE_ATTRIBUTE_NORMAL);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                mt_remove_tree(child);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryA(child);
            else
                DeleteFileA(child);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
}

/* Copy a tree; generated bundle files of the source are not copied. */
static int mt_copy_tree(const char *src, const char *dst)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2], s[MAX_PATH * 2], d[MAX_PATH * 2];
    HANDLE h;
    int ok = 1;
    if (!CreateDirectoryA(dst, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", src);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (!_stricmp(fd.cFileName, "bundle.json") || !_stricmp(fd.cFileName, "bundle.canonical.json") ||
            !_stricmp(fd.cFileName, ".state.json")) continue;
        _snprintf_s(s, sizeof s, _TRUNCATE, "%s\\%s", src, fd.cFileName);
        _snprintf_s(d, sizeof d, _TRUNCATE, "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = mt_copy_tree(s, d) && ok;
        else ok = CopyFileA(s, d, FALSE) && ok;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

static void mt_mkdirs_for(const char *file)
{
    char buf[MAX_PATH * 2];
    size_t i;
    strcpy_s(buf, sizeof buf, file);
    for (i = 3; buf[i]; i++)
        if (buf[i] == '\\') { buf[i] = 0; CreateDirectoryA(buf, NULL); buf[i] = '\\'; }
}

static void mt_put(const char *folder, const char *rel, const char *text, const char *mode)
{
    char path[MAX_PATH * 2];
    FILE *f = NULL;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", folder, rel);
    mt_mkdirs_for(path);
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    if (fopen_s(&f, path, mode) == 0 && f) { fwrite(text, 1, strlen(text), f); fclose(f); }
}

static void mt_write(const char *folder, const char *rel, const char *text) { mt_put(folder, rel, text, "wb"); }
static void mt_append(const char *folder, const char *rel, const char *text) { mt_put(folder, rel, text, "ab"); }

static int mt_has(const char *folder, const char *rel)
{
    char path[MAX_PATH * 2];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", folder, rel);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

static void mt_delete(const char *folder, const char *rel)
{
    char path[MAX_PATH * 2];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", folder, rel);
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    DeleteFileA(path);
}

static int mt_exists(const char *path) { return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES; }

/* Extract a ZIP of stored entries (the exports write method 0) into dir, as a
 * player's file manager would. Returns the entry count, -1 on a damaged file. */
static int mt_unzip_stored(const char *zip, const char *dir)
{
    FILE *f = NULL;
    unsigned char *z;
    long size;
    size_t i, at = (size_t)-1, cd, n, k;
    int count = 0;
    if (fopen_s(&f, zip, "rb") || !f) return -1;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    z = (unsigned char *)malloc((size_t)size + 1);
    if (!z || fread(z, 1, (size_t)size, f) != (size_t)size) { fclose(f); free(z); return -1; }
    fclose(f);
    for (i = (size_t)size >= 22 ? (size_t)size - 22 + 1 : 0; i-- > 0;)
        if (z[i] == 'P' && z[i + 1] == 'K' && z[i + 2] == 5 && z[i + 3] == 6) { at = i; break; }
    if (at == (size_t)-1) { free(z); return -1; }
    n = (size_t)(z[at + 10] | (z[at + 11] << 8));
    cd = (size_t)z[at + 16] | ((size_t)z[at + 17] << 8) | ((size_t)z[at + 18] << 16) | ((size_t)z[at + 19] << 24);
    for (k = 0; k < n; k++) {
        size_t nl = (size_t)(z[cd + 28] | (z[cd + 29] << 8)), xl = (size_t)(z[cd + 30] | (z[cd + 31] << 8));
        size_t cl = (size_t)(z[cd + 32] | (z[cd + 33] << 8));
        size_t usize = (size_t)z[cd + 24] | ((size_t)z[cd + 25] << 8) | ((size_t)z[cd + 26] << 16) | ((size_t)z[cd + 27] << 24);
        size_t lho = (size_t)z[cd + 42] | ((size_t)z[cd + 43] << 8) | ((size_t)z[cd + 44] << 16) | ((size_t)z[cd + 45] << 24);
        size_t data = lho + 30 + (size_t)(z[lho + 26] | (z[lho + 27] << 8)) + (size_t)(z[lho + 28] | (z[lho + 29] << 8));
        char name[MAX_PATH], path[MAX_PATH * 2];
        FILE *o = NULL;
        size_t c;
        if (nl >= sizeof name || z[cd + 10] != 0) { free(z); return -1; }
        memcpy(name, z + cd + 46, nl);
        name[nl] = 0;
        for (c = 0; c < nl; c++) if (name[c] == '/') name[c] = '\\';
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, name);
        mt_mkdirs_for(path);
        if (fopen_s(&o, path, "wb") || !o) { free(z); return -1; }
        fwrite(z + data, 1, usize, o);
        fclose(o);
        count++;
        cd += 46 + nl + xl + cl;
    }
    free(z);
    return count;
}

#endif
