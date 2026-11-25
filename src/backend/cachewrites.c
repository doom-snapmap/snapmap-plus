/* cachewrites.c -- the detour of the engine's atomic write (cachewrites.h). The root is the Saved Games folder plus
 * `\id Software\DOOM` (the default fs_savepath), resolved at install because the cvar system is not up yet; a cache
 * write gets `<root>\base\<rel>`, and any other write, or one whose folder cannot be made, passes through unchanged. */
#include <windows.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "signatures.h"
#include "cachewrites.h"
#include "backend_log.h"
#include "hook.h"
#include "patch.h"

#define WRITE_STOLEN   21      /* push rbp, rbx, rsi, rdi, r12, r13, r14, r15; lea rbp,[rsp-41A8h] */

typedef uint8_t (*atomic_write_fn)(const char *rel, int path_type, const void *buf, size_t len);

static atomic_write_fn g_write;
static char g_root[MAX_PATH];
static size_t g_root_len;
static volatile LONG g_redirected;
static volatile LONG g_dir_logged;

static void cw_log(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

/* Resolves the Saved Games folder plus `\id Software\DOOM` in the ANSI code page the engine's narrow paths use.
 * Returns 0 when the folder is unknown or its name has a character that code page cannot spell. */
static int resolve_root(void)
{
    static const GUID kSavedGames =
        { 0x4C5C32FFu, 0xBB9D, 0x43b0, { 0xB5, 0xB4, 0x2D, 0x72, 0xE5, 0x4E, 0xAA, 0xA4 } };
    static const char TAIL[] = "\\id Software\\DOOM";
    PWSTR wide = NULL;
    BOOL lossy = FALSE;
    int n;
    if (FAILED(SHGetKnownFolderPath(&kSavedGames, 0, NULL, &wide)) || !wide) {
        if (wide) CoTaskMemFree(wide);
        return 0;
    }
    n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide, -1, g_root, (int)sizeof g_root, NULL, &lossy);
    CoTaskMemFree(wide);
    if (n <= 1 || lossy) { g_root[0] = '\0'; return 0; }
    g_root_len = strlen(g_root);
    while (g_root_len && (g_root[g_root_len - 1] == '\\' || g_root[g_root_len - 1] == '/')) g_root[--g_root_len] = '\0';
    if (!g_root_len || g_root_len + sizeof TAIL > sizeof g_root) { g_root[0] = '\0'; g_root_len = 0; return 0; }
    memcpy(g_root + g_root_len, TAIL, sizeof TAIL);
    g_root_len += sizeof TAIL - 1;
    return 1;
}

/* Makes the folders of path below the root, the target's own name excluded. Returns 1 when they all exist. */
static int make_parents(char *path)
{
    size_t i;
    for (i = g_root_len + 1; path[i]; i++) {
        if (path[i] != '\\') continue;
        path[i] = '\0';
        if (!CreateDirectoryA(path, NULL)) {
            DWORD err = GetLastError();
            if (err == ERROR_ALREADY_EXISTS) { path[i] = '\\'; continue; }
            if (!InterlockedExchange(&g_dir_logged, 1))
                cw_log("CACHE: the folder %s could not be made (error %lu); cache writes it holds stay in the "
                       "game folder", path, err);
            path[i] = '\\';
            return 0;
        }
        path[i] = '\\';
    }
    return 1;
}

static uint8_t write_detour(const char *rel, int path_type, const void *buf, size_t len)
{
    char path[MAX_PATH];
    int moved = 0;
    __try {
        moved = sh_cachewrite_redirect(rel, g_root, path, sizeof path) && make_parents(path);
    } __except (EXCEPTION_EXECUTE_HANDLER) { moved = 0; }
    if (!moved) return g_write(rel, path_type, buf, len);
    InterlockedIncrement(&g_redirected);
    return g_write(path, path_type, buf, len);
}

int sh_cachewrites_install(const uint8_t *module_base)
{
    sig_result own;
    void *tramp;
    if (g_write) return 1;
    if (!resolve_root()) {
        cw_log("CACHE: the save path could not be found; the game's cache files are written in the game folder");
        return 0;
    }
    if (sig_resolve_one(module_base, &SH_CACHEWRITES_SIGNATURES[0], &own) != SIG_OK || !own.addr) {
        cw_log("CACHE: %s did not resolve on this executable; the game's cache files are written in the game "
               "folder", SH_CACHEWRITES_SIGNATURES[0].name);
        return 0;
    }
    tramp = hook_prepare((void *)own.addr, (void *)write_detour, WRITE_STOLEN);
    if (!tramp) {
        cw_log("CACHE: the atomic write could not be detoured (%s); the game's cache files are written in the game "
               "folder", hook_last_failure());
        return 0;
    }
    g_write = (atomic_write_fn)tramp;
    if (hook_commit(tramp) != B2_PATCH_OK) {
        if (hook_unpatch(tramp)) g_write = NULL;
        cw_log("CACHE: the atomic write's detour could not be committed; the game's cache files are written in the "
               "game folder");
        return 0;
    }
    cw_log("CACHE: cache writes go to %s\\base (generated\\, maps\\*.flight)", g_root);
    return 1;
}
