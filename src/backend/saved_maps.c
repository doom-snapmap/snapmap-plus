/* saved_maps.c -- see saved_maps.h. */
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/text.h"
#include "saved_maps.h"
#include "raw_deflate.h"

static SRWLOCK g_lock = SRWLOCK_INIT;
static char *g_root;   /* an override (tests, tools), malloc'd; NULL: the game's folder */

void sh_saved_maps_set_root(const char *path)
{
    AcquireSRWLockExclusive(&g_lock);
    free(g_root);
    g_root = path ? sh_text(path) : NULL;
    ReleaseSRWLockExclusive(&g_lock);
}

static wchar_t *sm_wide(const char *utf8)
{
    int n = utf8 ? MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0) : 0;
    wchar_t *w = n > 0 ? (wchar_t *)malloc((size_t)n * sizeof *w) : NULL;
    if (w && MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, n) != n) { free(w); w = NULL; }
    return w;
}

static char *sm_utf8(const wchar_t *wide)
{
    int n = wide ? WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL) : 0;
    char *s = n > 0 ? (char *)malloc((size_t)n) : NULL;
    if (s && WideCharToMultiByte(CP_UTF8, 0, wide, -1, s, n, NULL, NULL) != n) { free(s); s = NULL; }
    return s;
}

char *sh_saved_maps_root(void)
{
    PWSTR saved = NULL;
    char *utf8, *out = NULL;
    AcquireSRWLockShared(&g_lock);
    if (g_root) out = sh_text(g_root);
    ReleaseSRWLockShared(&g_lock);
    if (out) return out;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_SavedGames, 0, NULL, &saved)) || !saved) {
        if (saved) CoTaskMemFree(saved);
        return NULL;
    }
    utf8 = sm_utf8(saved);
    CoTaskMemFree(saved);
    if (!utf8) return NULL;
    out = sh_textf("%s\\id Software\\DOOM\\base\\savegame.user", utf8);
    free(utf8);
    return out;
}

/* A whole file (UTF-8 path), malloc'd; NULL when absent or unreadable. *absent 1 when it does not exist. */
static unsigned char *sm_read(const char *path, size_t *len, int *absent)
{
    wchar_t *w = sm_wide(path);
    HANDLE h;
    LARGE_INTEGER size;
    unsigned char *buf = NULL;
    DWORD got = 0;
    *len = 0;
    if (absent) *absent = 0;
    if (!w) return NULL;
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (absent) *absent = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
        return NULL;
    }
    if (GetFileSizeEx(h, &size) && size.QuadPart >= 0 && size.QuadPart < (LONGLONG)1 << 31 &&
        (buf = (unsigned char *)malloc((size_t)size.QuadPart + 1)) != NULL &&
        (size.QuadPart == 0 || (ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) && got == (DWORD)size.QuadPart))) {
        buf[got] = 0;
        *len = got;
    } else {
        free(buf);
        buf = NULL;
    }
    CloseHandle(h);
    return buf;
}

static char *sm_map_text(const char *path, size_t *len, int *absent)
{
    size_t n = 0, cap;
    unsigned char *raw = sm_read(path, &n, absent);
    char *out = NULL;
    *len = 0;
    if (!raw) return NULL;
    if (n < 4 + 2 + 4 || (raw[4] & 0x0F) != 8 || ((raw[4] << 8) | raw[5]) % 31 || (raw[5] & 0x20)) { free(raw); return NULL; }
    /* The buffer doubles until the whole text fits, of any size; DEFLATE expands at most 1032 to 1, so past that the
     * stream is malformed. */
    for (cap = n * 8 < 65536 ? 65536 : n * 8; cap / 2 <= n * 1032 + 65536 && cap < ((size_t)-1) / 2; cap *= 2) {
        size_t got;
        out = (char *)malloc(cap + 1);
        if (!out) break;
        got = sh_inflate_raw_upto(raw + 6, n - 6 - 4, (unsigned char *)out, cap);
        if (got && got < cap) { out[got] = 0; *len = got; break; }
        free(out);
        out = NULL;
    }
    free(raw);
    return out;
}

char *sh_saved_map_text(const char *path, size_t *len)
{
    size_t n = 0;
    char *t = sm_map_text(path, &n, NULL);
    if (len) *len = n;
    return t;
}

/* The itemId= line of a game.details text, malloc'd ("" for none). */
static char *sm_item_id(const char *details)
{
    const char *c;
    for (c = details; c && *c;) {
        const char *end = strchr(c, '\n');
        size_t len = end ? (size_t)(end - c) : strlen(c);
        if (len >= 7 && !strncmp(c, "itemId=", 7)) {
            size_t v = len - 7;
            while (v && (c[7 + v - 1] == '\r' || c[7 + v - 1] == ' ')) v--;
            return sh_textn(c + 7, v);
        }
        c = end ? end + 1 : NULL;
    }
    return sh_text("");
}

/* FindFirstFileW over a UTF-8 pattern. */
static HANDLE sm_find_first(const char *pattern, WIN32_FIND_DATAW *fd)
{
    wchar_t *w = sm_wide(pattern);
    HANDLE h = w ? FindFirstFileW(w, fd) : INVALID_HANDLE_VALUE;
    free(w);
    return h;
}

int sh_saved_maps_each(sh_saved_map_fn fn, void *context, size_t *unreadable)
{
    char *root, *pattern;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int count = 0;
    DWORD attr;
    wchar_t *wroot;
    if (unreadable) *unreadable = 0;
    if (!(root = sh_saved_maps_root())) return -1;
    wroot = sm_wide(root);
    attr = wroot ? GetFileAttributesW(wroot) : INVALID_FILE_ATTRIBUTES;
    if (attr == INVALID_FILE_ATTRIBUTES) {
        DWORD e = wroot ? GetLastError() : ERROR_OUTOFMEMORY;
        free(wroot);
        free(root);
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? 0 : -1;   /* no saves yet, or unknown */
    }
    free(wroot);
    pattern = sh_textf("%s\\*", root);
    h = pattern ? sm_find_first(pattern, &fd) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) { free(root); return -1; }
    do {
        char *profile, *sub;
        WIN32_FIND_DATAW sd;
        HANDLE hs;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if ((profile = sm_utf8(fd.cFileName)) == NULL) { count = -1; break; }
        sub = sh_textf("%s\\%s\\SNAPMAPS*", root, profile);
        hs = sub ? sm_find_first(sub, &sd) : INVALID_HANDLE_VALUE;
        free(sub);
        if (hs == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            free(profile);
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_NO_MORE_FILES) continue;   /* a profile with no maps */
            count = -1;
            break;
        }
        do {
            char *slot, *path, *text, *details_path, *details, *item;
            size_t len = 0, dl = 0;
            int absent = 0;
            if (!(sd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if ((slot = sm_utf8(sd.cFileName)) == NULL) { if (unreadable) (*unreadable)++; continue; }
            path = sh_textf("%s\\%s\\%s\\map.decl", root, profile, slot);
            text = path ? sm_map_text(path, &len, &absent) : NULL;
            free(path);
            if (!text) {
                if (!absent && unreadable) (*unreadable)++;
                free(slot);
                continue;
            }
            details_path = sh_textf("%s\\%s\\%s\\game.details", root, profile, slot);
            details = details_path ? (char *)sm_read(details_path, &dl, NULL) : NULL;
            free(details_path);
            item = sm_item_id(details);
            if (fn) fn(context, strlen(slot) > 8 ? slot + 8 : "", item ? item : "", text, len);
            free(item);
            free(details);
            free(text);
            free(slot);
            count++;
        } while (FindNextFileW(hs, &sd));
        FindClose(hs);
        free(profile);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    free(root);
    return count;
}

char *sh_saved_map_details(const char *local_id)
{
    char *root, *pattern, *text = NULL;
    const char *c;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (!local_id || !local_id[0] || strlen(local_id) > 40) return NULL;
    for (c = local_id; *c; c++) if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z'))) return NULL;
    if (!(root = sh_saved_maps_root())) return NULL;
    pattern = sh_textf("%s\\*", root);
    h = pattern ? sm_find_first(pattern, &fd) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) { free(root); return NULL; }
    do {
        char *profile, *path;
        size_t len = 0;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if ((profile = sm_utf8(fd.cFileName)) == NULL) continue;
        path = sh_textf("%s\\%s\\SNAPMAPS%s\\game.details", root, profile, local_id);
        free(profile);
        text = path ? (char *)sm_read(path, &len, NULL) : NULL;
        free(path);
    } while (!text && FindNextFileW(h, &fd));
    FindClose(h);
    free(root);
    return text;
}
