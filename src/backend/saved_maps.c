/* saved_maps.c -- see saved_maps.h. */
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../common/text.h"
#include "../common/wide_path.h"
#include "saved_maps.h"
#include "engine_cvar_read.h"
#include "raw_deflate.h"

#ifdef SH_SAVED_MAP_FIXTURE
static SRWLOCK g_lock = SRWLOCK_INIT;
static char *g_root;   /* an override (tests, tools), malloc'd; NULL: the game's folder */

void sh_saved_maps_set_root(const char *path)
{
    AcquireSRWLockExclusive(&g_lock);
    free(g_root);
    g_root = path ? sh_text(path) : NULL;
    ReleaseSRWLockExclusive(&g_lock);
}
#endif

static const void *g_cvar_slot;
static unsigned long long (*g_account)(void);
static int g_live;

void sh_saved_maps_bind(const void *cvar_slot, unsigned long long (*account)(void))
{
    g_cvar_slot = cvar_slot;
    g_account = account;
    g_live = 1;
}

static wchar_t *sm_wide(const char *utf8)
{
    int n = utf8 ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0) : 0;
    wchar_t *path = n > 0 ? malloc((size_t)n * sizeof *path) : NULL;
    if (!path) return NULL;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, path, n)) { free(path); return NULL; }
    DWORD need = GetFullPathNameW(path, 0, NULL, NULL);
    wchar_t *full = need && need <= SH_WPATH_MAX ? malloc((need + 8) * sizeof *full) : NULL;
    DWORD got = full ? GetFullPathNameW(path, need, full, NULL) : 0;
    free(path);
    if (!got || got >= need || !sh_wpath_long(full, need + 8)) { free(full); return NULL; }
    return full;
}

static char *sm_utf8(const wchar_t *wide)
{
    int n = wide ? WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL) : 0;
    char *s = n > 0 ? (char *)malloc((size_t)n) : NULL;
    if (s && WideCharToMultiByte(CP_UTF8, 0, wide, -1, s, n, NULL, NULL) != n) { free(s); s = NULL; }
    return s;
}

static char *sm_directory(const char *cvar_name, const KNOWNFOLDERID *known, const char *suffix)
{
    char *path = NULL;
    if (g_live) {
        const unsigned char *cvar = sh_engine_cvar_find(g_cvar_slot, SH_IDCVAR_NAME_FOR_LOOKUP, cvar_name);
        if (cvar) {
            __try { path = sh_text(*(const char *const *)(cvar + 0x10)); }
            __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
        }
        if (!path) return NULL;
    }
    if (!path || !*path) {
        free(path);
#ifdef SH_SAVED_MAP_FIXTURE
        if (known == &FOLDERID_SavedGames) {
            AcquireSRWLockShared(&g_lock);
            char *fixture = g_root ? sh_text(g_root) : NULL;
            ReleaseSRWLockShared(&g_lock);
            if (fixture) return fixture;
        }
#endif
        PWSTR folder = NULL;
        if (FAILED(SHGetKnownFolderPath(known, 0, NULL, &folder)) || !folder) {
            if (folder) CoTaskMemFree(folder);
            return NULL;
        }
        char *base = sm_utf8(folder);
        CoTaskMemFree(folder);
        char *root = base ? sh_textf("%s\\id Software\\DOOM\\base\\%s", base, suffix) : NULL;
        free(base);
        return root;
    }
    wchar_t *wide = sm_wide(path);
    DWORD attrs = wide ? GetFileAttributesW(wide) : INVALID_FILE_ATTRIBUTES;
    char *root = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)
               ? sh_textf("%s\\base\\%s", path, suffix) : NULL;
    free(wide);
    free(path);
    return root;
}

char *sh_saved_maps_root(void)
{
    return sm_directory("fs_savepath", &FOLDERID_SavedGames, "savegame.user");
}

char *sh_saved_maps_cloud_dir(int published)
{
    return sm_directory("fs_cachepath", &FOLDERID_LocalAppData,
                        published ? "doomsnapmaps\\published-maps" : "doomsnapmaps\\saved-maps");
}

/* A whole file (UTF-8 path), malloc'd; NULL when absent or unreadable. *absent 1 when it does not exist. */
static unsigned char *sm_read_wide(const wchar_t *w, size_t *len, int *absent)
{
    HANDLE h;
    LARGE_INTEGER size;
    unsigned char *buf = NULL;
    DWORD got = 0;
    *len = 0;
    if (absent) *absent = 0;
    if (!w) return NULL;
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, NULL);
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

static unsigned char *sm_read(const char *path, size_t *len, int *absent)
{
    wchar_t *wide = sm_wide(path);
    unsigned char *bytes = sm_read_wide(wide, len, absent);
    free(wide);
    return bytes;
}

static char *sm_map_text(const char *path, size_t *len, int *absent)
{
    size_t n = 0, cap;
    unsigned char *raw = sm_read(path, &n, absent);
    char *out = NULL;
    *len = 0;
    if (!raw) return NULL;
    if (n < 4 + 2 + 4 || (raw[4] & 0x0F) != 8 || ((raw[4] << 8) | raw[5]) % 31 || (raw[5] & 0x20)) { free(raw); return NULL; }
    /* The buffer doubles until the text fits; DEFLATE expands at most 1032 to 1, so past that the stream is malformed. */
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

static wchar_t *sm_reload_profile(const wchar_t *root, unsigned long long account)
{
    if (account) return sh_wtextf(L"%s\\%llu", root, account);
    wchar_t *pattern = sh_wtextf(L"%s\\*", root), *profile = NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h = pattern ? FindFirstFileW(pattern, &fd) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (profile) { free(profile); profile = NULL; break; }
        profile = sh_wtextf(L"%s\\%s", root, fd.cFileName);
        if (!profile) break;
    } while (FindNextFileW(h, &fd));
    if (profile && GetLastError() != ERROR_NO_MORE_FILES) { free(profile); profile = NULL; }
    FindClose(h);
    return profile;
}

wchar_t *sh_saved_map_profile(void)
{
    char *path = sh_saved_maps_root();
    wchar_t *root = sm_wide(path);
    free(path);
    if (!root) return NULL;
    wchar_t *profile = sm_reload_profile(root, g_account ? g_account() : 0);
    free(root);
    DWORD attrs = profile ? GetFileAttributesW(profile) : INVALID_FILE_ATTRIBUTES;
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) { free(profile); return NULL; }
    return profile;
}

static int sm_slot_file(const wchar_t *profile, const wchar_t *slot, const wchar_t *leaf)
{
    wchar_t *p = sh_wtextf(L"%s\\%s\\%s", profile, slot, leaf);
    DWORD attrs = p ? GetFileAttributesW(p) : INVALID_FILE_ATTRIBUTES;
    free(p);
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

int sh_saved_map_fallback(char *out, size_t cap)
{
    WIN32_FIND_DATAW fd;
    FILETIME latest = {0};
    if (!out || cap < 21) return 0;
    out[0] = 0;
    wchar_t *profile = sh_saved_map_profile();
    if (!profile) return 0;
    wchar_t *pattern = sh_wtextf(L"%s\\SNAPMAPS*", profile);
    HANDLE h = pattern ? FindFirstFileW(pattern, &fd) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const wchar_t *id = fd.cFileName + 8;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcslen(id) != 20 ||
                wcsspn(id, L"0123456789abcdefABCDEF") != 20) continue;
            if (out[0] && CompareFileTime(&fd.ftLastWriteTime, &latest) <= 0) continue;
            if (!sm_slot_file(profile, fd.cFileName, L"game.details") ||
                !sm_slot_file(profile, fd.cFileName, L"map.decl")) continue;
            for (int i = 0; i <= 20; ++i) out[i] = (char)id[i];
            latest = fd.ftLastWriteTime;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(profile);
    return out[0] != 0;
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
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) { free(root); return -1; }
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
        if (GetLastError() != ERROR_NO_MORE_FILES) count = -1;
        FindClose(hs);
        free(profile);
        if (count < 0) break;
    } while (FindNextFileW(h, &fd));
    if (GetLastError() != ERROR_NO_MORE_FILES) count = -1;
    FindClose(h);
    free(root);
    return count;
}

char *sh_saved_map_details(const char *local_id)
{
    const char *c;
    if (!local_id || !local_id[0] || strlen(local_id) > 40) return NULL;
    for (c = local_id; *c; c++) if (!((*c >= '0' && *c <= '9') || (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z'))) return NULL;
    wchar_t *profile = sh_saved_map_profile();
    wchar_t *path = profile ? sh_wtextf(L"%s\\SNAPMAPS%S\\game.details", profile, local_id) : NULL;
    size_t len = 0;
    char *text = (char *)sm_read_wide(path, &len, NULL);
    free(path);
    free(profile);
    return text;
}
