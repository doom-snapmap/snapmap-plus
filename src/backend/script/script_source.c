/* script_source.c -- see script_source.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "script_source.h"
#include "../../common/text.h"
#ifndef SH_SCRIPT_PURE
#include "../modserve.h"
#include "../modmanifest.h"
#include "../modbundle.h"
#endif

/* A UTF-8 path as the wide long-path form Windows opens (\\?\ for a
 * drive path), malloc'd. */
static wchar_t *src_wide(const char *path)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    wchar_t *w, *out;
    if (n <= 0 || !(w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n))) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, w, n);
    if (w[0] && w[1] == L':') {
        size_t i;
        for (i = 0; w[i]; i++) if (w[i] == L'/') w[i] = L'\\';
        out = sh_wtextf(L"\\\\?\\%ls", w);
        free(w);
        return out;
    }
    return w;
}

typedef struct src_file {
    char *rel;                   /* "x.lua" or "lib/x.lua", malloc'd */
    char *text;                  /* the built bytes (NUL-terminated), NULL when they could not be read */
    size_t len;
} src_file;

typedef struct src_mod {
    char salt[SH_SCRIPT_SALT_CAP];
    char *name;                  /* malloc'd, any length */
    src_file *f;
    int n, cap;
} src_mod;

static src_mod *g_src;        /* grows with the map's scripted mods */
static int     g_nsrc, g_csrc;

static const src_mod *find_src(const char *salt)
{
    int i;
    if (!salt) return NULL;
    for (i = 0; i < g_nsrc; i++) if (!_stricmp(g_src[i].salt, salt)) return &g_src[i];
    return NULL;
}

static void mod_free(src_mod *m)
{
    int i;
    for (i = 0; i < m->n; i++) { free(m->f[i].text); free(m->f[i].rel); }
    free(m->f);
    free(m->name);
    memset(m, 0, sizeof *m);
}

void sh_script_source_clear(void)
{
    int i;
    for (i = 0; i < g_nsrc; i++) mod_free(&g_src[i]);
    g_nsrc = 0;
}

static src_mod *mod_add(const char *salt, const char *name)
{
    src_mod *m;
    if (!salt || !salt[0] || strlen(salt) >= SH_SCRIPT_SALT_CAP) return NULL;
    if (g_nsrc == g_csrc) {
        int cap = g_csrc ? g_csrc * 2 : 16;
        src_mod *grown = (src_mod *)realloc(g_src, (size_t)cap * sizeof *grown);
        if (!grown) return NULL;
        g_src = grown;
        g_csrc = cap;
    }
    m = &g_src[g_nsrc++];
    memset(m, 0, sizeof *m);
    strncpy_s(m->salt, sizeof m->salt, salt, _TRUNCATE);
    _strlwr_s(m->salt, sizeof m->salt);
    if (!(m->name = _strdup(name && name[0] ? name : salt))) { g_nsrc--; return NULL; }
    return m;
}

/* Keep one file (text NULL: listed, but its built bytes could not be read). */
static int file_add(src_mod *m, const char *rel, char *text, size_t len)
{
    src_file *f;
    if (m->n == m->cap) {
        int cap = m->cap ? m->cap * 2 : 16;
        src_file *grown = (src_file *)realloc(m->f, (size_t)cap * sizeof *grown);
        if (!grown) { free(text); return 0; }
        m->f = grown;
        m->cap = cap;
    }
    f = &m->f[m->n];
    f->rel = _strdup(rel);
    if (!f->rel) { free(text); return 0; }
    m->n++;
    f->text = text;
    f->len = text ? len : 0;
    return 1;
}

/* ------------------------------------------------------------ file source */

static int src_list(void *context, const char *salt, sh_script_file_fn fn, void *fn_context)
{
    const src_mod *m = find_src(salt);
    int i;
    (void)context;
    if (!m || !fn) return 0;
    for (i = 0; i < m->n; i++) fn(fn_context, m->f[i].rel);
    return m->n;
}

static int src_read(void *context, const char *salt, const char *relpath, char **text, size_t *len)
{
    const src_mod *m = find_src(salt);
    int i;
    (void)context;
    *text = NULL;
    *len = 0;
    if (!m || !relpath) return 0;
    for (i = 0; i < m->n; i++) {
        const src_file *f = &m->f[i];
        char *copy;
        if (_stricmp(f->rel, relpath)) continue;
        if (!f->text) return -1;
        copy = (char *)malloc(f->len + 1);
        if (!copy) return -1;
        memcpy(copy, f->text, f->len);
        copy[f->len] = 0;
        *text = copy;
        *len = f->len;
        return 1;
    }
    return 0;
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

/* Every mod with scripts: up to cap written, the whole count returned. */
static int fill(sh_script_mod_desc *out, int cap)
{
    int i, n = 0;
    for (i = 0; i < g_nsrc; i++) {
        if (!g_src[i].n) continue;
        if (out && n < cap) {
            out[n].salt = g_src[i].salt;
            out[n].name = g_src[i].name;
        }
        n++;
    }
    return n;
}

#ifdef SH_SCRIPT_PURE
/* ------------------------------------------------- the tests' stand-in */

static int read_whole(const char *path, char **text, size_t *len)
{
    wchar_t *w = src_wide(path);
    HANDLE h;
    LARGE_INTEGER size;
    DWORD got = 0;
    char *buf;
    if (!w) return 0;
    h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart >= 0xFFFFFFFFll) {   /* one ReadFile */
        CloseHandle(h);
        return 0;
    }
    buf = (char *)malloc((size_t)size.QuadPart + 1);
    if (!buf || (size.QuadPart && (!ReadFile(h, buf, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart))) {
        free(buf);
        CloseHandle(h);
        return 0;
    }
    CloseHandle(h);
    buf[size.QuadPart] = 0;
    *text = buf;
    *len = (size_t)size.QuadPart;
    return 1;
}

static void read_dir(src_mod *m, const char *dir, const char *prefix)
{
    char *pattern = sh_textf("%s\\*.lua", dir);
    wchar_t *w = pattern ? src_wide(pattern) : NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    free(pattern);
    if (!w) return;
    h = FindFirstFileW(w, &fd);
    free(w);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char name[1024], *rel, *path, *text = NULL;   /* a file name: 255 UTF-16 units, at most 3 bytes each */
        size_t len = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name, NULL, NULL)) continue;
        _strlwr_s(name, sizeof name);
        rel = sh_textf("%s%s", prefix, name);
        path = sh_textf("%s\\%s", dir, name);
        if (rel && path) {
            if (!read_whole(path, &text, &len)) text = NULL;
            file_add(m, rel, text, len);
        }
        free(rel);
        free(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int rel_cmp(const void *a, const void *b)
{
    return strcmp(((const src_file *)a)->rel, ((const src_file *)b)->rel);
}

int sh_script_source_set_folder(const char *salt, const char *name, const char *scripts_dir)
{
    src_mod *m;
    char *lib;
    int i;
    if (!scripts_dir) return 0;
    for (i = 0; i < g_nsrc; i++)
        if (!_stricmp(g_src[i].salt, salt)) { mod_free(&g_src[i]); g_src[i] = g_src[--g_nsrc]; break; }
    if (!(m = mod_add(salt, name))) return 0;
    read_dir(m, scripts_dir, "");
    if ((lib = sh_textf("%s\\lib", scripts_dir)) != NULL) read_dir(m, lib, "lib/");
    free(lib);
    qsort(m->f, (size_t)m->n, sizeof *m->f, rel_cmp);
    return 1;
}

/* Without the serving layer the open map's mods are the folders set directly. */
int sh_script_source_map_mods(sh_script_mod_desc *out, int cap)
{
    return fill(out, cap);
}
#else

/* ---------------------------------------------------------- the map's mods */

typedef struct { const char *hash; char salt[SH_SCRIPT_SALT_CAP]; } hash_salt;

static void salt_of_hash(void *context, const char *id, const char *hash, int admitted,
                         const char *cause, int files, int place)
{
    hash_salt *h = (hash_salt *)context;
    (void)cause; (void)files; (void)place;
    if (h->salt[0] || !admitted || !id || !hash || _stricmp(hash, h->hash)) return;
    strncpy_s(h->salt, sizeof h->salt, id, _TRUNCATE);
}

/* One served script: its bytes through the index, kept only when they are
 * the bytes its build recorded. */
static void keep_script(void *context, const char *relpath, const char *request, uint64_t size, const char *sha256,
                        const char *bundle_hash)
{
    src_mod *m = (src_mod *)context;
    unsigned char *bytes = NULL;
    size_t len = 0;
    char *text = NULL, actual[SH_MB_HASH_HEX + 1];
    (void)bundle_hash;
    if (sh_modserve_open(request, &bytes, &len) == 1) {
        sh_mb_sha256_hex(bytes, len, actual);
        if (len == size && sha256 && !_stricmp(actual, sha256) && (text = (char *)malloc(len + 1)) != NULL) {
            memcpy(text, bytes, len);
            text[len] = 0;
        }
        HeapFree(GetProcessHeap(), 0, bytes);
    }
    file_add(m, relpath, text, len);
}

/* Read the scripts of the map's mods into g_src. Returns the count of mods
 * with scripts, -1 when unreadable; *seed is derived from the builds. */
static int collect(uint64_t *seed)
{
    char (*hashes)[SH_MODSERVE_HASH_HEX + 1] = NULL;
    int n, i;
    uint64_t fnv = 1469598103934665603ull;
    sh_script_source_clear();
    if (seed) *seed = 0;
    n = sh_modserve_scope_hashes(NULL, 0);
    if (n <= 0) return 0;
    hashes = (char (*)[SH_MODSERVE_HASH_HEX + 1])calloc((size_t)n, sizeof *hashes);
    if (!hashes) return -1;
    n = sh_modserve_scope_hashes(hashes, n);
    for (i = 0; i < n; i++) {
        hash_salt hs;
        sh_modmanifest_row row;
        sh_modserve_bundle_ref ref;
        const char *seg;
        src_mod *m;
        int k;
        for (k = 0; hashes[i][k]; k++) fnv = (fnv ^ (unsigned char)hashes[i][k]) * 1099511628211ull;
        memset(&hs, 0, sizeof hs);
        hs.hash = hashes[i];
        sh_modserve_each_bundle(salt_of_hash, &hs);
        if (!hs.salt[0] || find_src(hs.salt)) continue;
        if (!sh_modserve_script_files(hs.salt, NULL, NULL)) continue;
        memset(&row, 0, sizeof row);
        strncpy_s(row.salt, sizeof row.salt, hs.salt, _TRUNCATE);
        strncpy_s(row.build, sizeof row.build, hashes[i], _TRUNCATE);
        memset(&ref, 0, sizeof ref);
        (void)sh_modserve_row_build(&row, &ref);
        seg = ref.folder && ref.folder[0] ? strrchr(ref.folder, '\\') : NULL;
        m = mod_add(hs.salt, ref.name && ref.name[0] ? ref.name : seg ? seg + 1 : ref.folder && ref.folder[0] ? ref.folder
                                                                                                        : "a mod");
        sh_modserve_bundle_ref_free(&ref);
        if (!m) continue;
        sh_modserve_script_files(m->salt, keep_script, m);
    }
    free(hashes);
    if (seed) *seed = fnv;
    return g_nsrc;
}

static int provider(void *context, sh_script_mod_desc *out, int cap, const sh_script_source **source, uint64_t *seed)
{
    (void)context;
    *source = &k_files;
    if (collect(seed) < 0) return -1;
    return fill(out, cap);
}

int sh_script_source_map_mods(sh_script_mod_desc *out, int cap)
{
    if (collect(NULL) < 0) return -1;
    return fill(out, cap);
}

void sh_script_source_install(void)
{
    sh_script_set_map_provider(provider, NULL);
}
#endif /* SH_SCRIPT_PURE */
