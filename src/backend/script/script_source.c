/* script_source.c -- see script_source.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "script_source.h"
#ifndef SH_SCRIPT_PURE
#include "../modserve.h"
#include "../modmanifest.h"
#include "../modstore.h"
#endif

#define SRC_PATH_MAX 1024

typedef struct src_mod {
    char salt[SH_SCRIPT_SALT_CAP];
    char name[SH_SCRIPT_MODNAME_CAP];
    char dir[SRC_PATH_MAX];          /* <store>\<folder>\assets\mods\<salt>\scripts */
} src_mod;

static src_mod g_src[SH_SCRIPT_MODS_MAX];
static int     g_nsrc;

static int utf8_to_wide(const char *s, wchar_t *out, int cap)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap) > 0;
}

static int wide_to_utf8(const wchar_t *s, char *out, int cap)
{
    return WideCharToMultiByte(CP_UTF8, 0, s, -1, out, cap, NULL, NULL) > 0;
}

static int is_dir(const char *path)
{
    wchar_t w[SRC_PATH_MAX];
    DWORD a;
    if (!utf8_to_wide(path, w, SRC_PATH_MAX)) return 0;
    a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static const src_mod *find_src(const char *salt)
{
    int i;
    if (!salt) return NULL;
    for (i = 0; i < g_nsrc; i++) if (!_stricmp(g_src[i].salt, salt)) return &g_src[i];
    return NULL;
}

/* ------------------------------------------------------------ file source */

/* Call fn for each regular file of dir\<sub> as "<prefix><name>". */
static int list_dir(const char *dir, const char *sub, const char *prefix, sh_script_file_fn fn, void *fn_context)
{
    char pattern[SRC_PATH_MAX];
    wchar_t w[SRC_PATH_MAX];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    int n = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s%s%s\\*", dir, sub[0] ? "\\" : "", sub);
    if (!utf8_to_wide(pattern, w, SRC_PATH_MAX)) return -1;
    h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND ? 0 : -1;
    do {
        char name[260], rel[300];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!wide_to_utf8(fd.cFileName, name, sizeof name)) continue;
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s%s", prefix, name);
        fn(fn_context, rel);
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static int src_list(void *context, const char *salt, sh_script_file_fn fn, void *fn_context)
{
    const src_mod *m = find_src(salt);
    int a, b;
    (void)context;
    if (!m || !fn) return 0;
    a = list_dir(m->dir, "", "", fn, fn_context);
    if (a < 0) return -1;
    {
        char lib[SRC_PATH_MAX];
        _snprintf_s(lib, sizeof lib, _TRUNCATE, "%s\\lib", m->dir);
        b = is_dir(lib) ? list_dir(m->dir, "lib", "lib/", fn, fn_context) : 0;
    }
    if (b < 0) return -1;
    return a + b;
}

/* A relpath the runtime asks for: "x.lua" or "lib/x.lua", nothing that
 * leaves the scripts folder. */
static int relpath_ok(const char *rel)
{
    size_t i, n = rel ? strlen(rel) : 0;
    if (!n || n >= 200 || rel[0] == '/' || rel[0] == '\\' || strstr(rel, "..") || strchr(rel, ':')) return 0;
    for (i = 0; i < n; i++) if ((unsigned char)rel[i] < 0x20) return 0;
    return 1;
}

static int src_read(void *context, const char *salt, const char *relpath, char **text, size_t *len)
{
    const src_mod *m = find_src(salt);
    char path[SRC_PATH_MAX];
    wchar_t w[SRC_PATH_MAX];
    HANDLE h;
    LARGE_INTEGER size;
    DWORD want, got = 0;
    char *buf;
    size_t i, at;
    (void)context;
    *text = NULL;
    *len = 0;
    if (!m || !relpath_ok(relpath)) return 0;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", m->dir, relpath);
    at = strlen(m->dir) + 1;
    for (i = at; path[i]; i++) if (path[i] == '/') path[i] = '\\';
    if (!utf8_to_wide(path, w, SRC_PATH_MAX)) return -1;
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? 0 : -1;
    }
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0) { CloseHandle(h); return -1; }
    /* One byte past the cap is enough for the runtime to refuse the file by name. */
    want = size.QuadPart > (LONGLONG)SH_SCRIPT_FILE_MAX ? (DWORD)SH_SCRIPT_FILE_MAX + 1 : (DWORD)size.QuadPart;
    buf = (char *)malloc((size_t)want + 1);
    if (!buf) { CloseHandle(h); return -1; }
    if (want && (!ReadFile(h, buf, want, &got, NULL) || got != want)) { free(buf); CloseHandle(h); return -1; }
    CloseHandle(h);
    buf[want] = 0;
    *text = buf;
    *len = want;
    return 1;
}

static void src_release(void *context, char *text)
{
    (void)context;
    free(text);
}

static const sh_script_source k_files = { NULL, src_list, src_read, src_release };

const sh_script_source *sh_script_source_files(void)
{
    return &k_files;
}

const char *sh_script_source_folder(const char *salt)
{
    const src_mod *m = find_src(salt);
    return m ? m->dir : "";
}

void sh_script_source_clear(void)
{
    g_nsrc = 0;
}

int sh_script_source_set_folder(const char *salt, const char *name, const char *scripts_dir)
{
    src_mod *m;
    int i;
    if (!salt || !salt[0] || strlen(salt) >= SH_SCRIPT_SALT_CAP || !scripts_dir || strlen(scripts_dir) >= SRC_PATH_MAX)
        return 0;
    for (i = 0; i < g_nsrc && _stricmp(g_src[i].salt, salt); i++) {}
    if (i == g_nsrc) {
        if (g_nsrc >= SH_SCRIPT_MODS_MAX) return 0;
        g_nsrc++;
    }
    m = &g_src[i];
    memset(m, 0, sizeof *m);
    strncpy_s(m->salt, sizeof m->salt, salt, _TRUNCATE);
    _strlwr_s(m->salt, sizeof m->salt);
    strncpy_s(m->name, sizeof m->name, name && name[0] ? name : salt, _TRUNCATE);
    strncpy_s(m->dir, sizeof m->dir, scripts_dir, _TRUNCATE);
    return 1;
}

static int fill(sh_script_mod_desc *out, int cap)
{
    int i;
    for (i = 0; i < g_nsrc && i < cap; i++) {
        out[i].salt = g_src[i].salt;
        out[i].name = g_src[i].name;
    }
    return i;
}

#ifdef SH_SCRIPT_PURE
/* Without the serving layer the open map's mods are the folders set directly. */
int sh_script_source_map_mods(sh_script_mod_desc *out, int cap)
{
    return fill(out, cap);
}
#else

/* ---------------------------------------------------------- the map's mods */

typedef struct { const char *hash; char salt[SH_SCRIPT_SALT_CAP]; } hash_salt;

static void salt_of_hash(void *context, const char *id, const char *version, const char *hash, int admitted,
                         const char *cause, int files, int place)
{
    hash_salt *h = (hash_salt *)context;
    (void)version; (void)cause; (void)files; (void)place;
    if (h->salt[0] || !admitted || !id || !hash || _stricmp(hash, h->hash)) return;
    strncpy_s(h->salt, sizeof h->salt, id, _TRUNCATE);
}

/* Find the folders: fills g_src. Returns the count, -1 when unreadable. */
static int collect(uint64_t *seed)
{
    char (*hashes)[SH_MODSERVE_HASH_HEX + 1] = NULL;
    char root[SRC_PATH_MAX];
    int n, i;
    uint64_t fnv = 1469598103934665603ull;
    g_nsrc = 0;
    if (seed) *seed = 0;
    if (!sh_ms_root(root, sizeof root)) return -1;
    n = sh_modserve_scope_hashes(NULL, 0);
    if (n <= 0) return 0;
    hashes = (char (*)[SH_MODSERVE_HASH_HEX + 1])calloc((size_t)n, sizeof *hashes);
    if (!hashes) return -1;
    n = sh_modserve_scope_hashes(hashes, n);
    for (i = 0; i < n && g_nsrc < SH_SCRIPT_MODS_MAX; i++) {
        hash_salt hs;
        sh_modmanifest_row row;
        sh_modserve_bundle_ref ref;
        src_mod *m;
        const char *seg;
        int k, dup = 0;
        for (k = 0; hashes[i][k]; k++) fnv = (fnv ^ (unsigned char)hashes[i][k]) * 1099511628211ull;
        memset(&hs, 0, sizeof hs);
        hs.hash = hashes[i];
        sh_modserve_each_bundle(salt_of_hash, &hs);
        if (!hs.salt[0]) continue;
        for (k = 0; k < g_nsrc; k++) if (!_stricmp(g_src[k].salt, hs.salt)) dup = 1;
        if (dup) continue;
        memset(&row, 0, sizeof row);
        strncpy_s(row.salt, sizeof row.salt, hs.salt, _TRUNCATE);
        strncpy_s(row.build, sizeof row.build, hashes[i], _TRUNCATE);
        if (!sh_modserve_row_build(&row, &ref) || !ref.folder[0]) continue;
        m = &g_src[g_nsrc];
        memset(m, 0, sizeof *m);
        strncpy_s(m->salt, sizeof m->salt, hs.salt, _TRUNCATE);
        _strlwr_s(m->salt, sizeof m->salt);
        _snprintf_s(m->dir, sizeof m->dir, _TRUNCATE, "%s\\%s\\assets\\mods\\%s\\scripts", root, ref.folder, m->salt);
        if (!is_dir(m->dir)) continue;
        seg = strrchr(ref.folder, '\\');
        strncpy_s(m->name, sizeof m->name, ref.name[0] ? ref.name : seg ? seg + 1 : ref.folder, _TRUNCATE);
        g_nsrc++;
    }
    free(hashes);
    if (seed) *seed = fnv;
    return g_nsrc;
}

static int provider(void *context, sh_script_mod_desc *out, int cap, const sh_script_source **source, uint64_t *seed)
{
    int n;
    (void)context;
    *source = &k_files;
    n = collect(seed);
    if (n < 0) return -1;
    return fill(out, cap);
}

int sh_script_source_map_mods(sh_script_mod_desc *out, int cap)
{
    int n = collect(NULL);
    if (n < 0) return -1;
    return fill(out, cap);
}

void sh_script_source_install(void)
{
    sh_script_set_map_provider(provider, NULL);
}
#endif /* SH_SCRIPT_PURE */
