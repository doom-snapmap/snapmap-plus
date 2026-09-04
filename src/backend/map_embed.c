/* map_embed.c -- see map_embed.h. Reading packages off the disk, and reading a
 * map to decide which ones to read.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "map_embed.h"
#include "packages.h"
#include "overrides.h"
#include "backend_log.h"

#define ME_MAX_PACKAGES   256u
#define ME_MAX_FILES      4096u
#define ME_MAX_FILE_BYTES (16u * 1024u * 1024u)

/* Our own sidecar, written by the installer to record which payload a folder came from. It is
 * metadata ABOUT the package, not part of it, and including it would make a repack of an
 * installed package produce a different digest than the payload that installed it. */
#define ME_SIDECAR "smpkg.digest"

static void me_err(char *err, size_t cap, const char *fmt, ...)
{
    va_list ap;
    if (!err || cap == 0) return;
    va_start(ap, fmt);
    _vsnprintf_s(err, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* ==================================================================== */
/* pack                                                                  */
/* ==================================================================== */

static unsigned me_crc32(const unsigned char *p, size_t n)
{
    static unsigned table[256];
    static int built;
    unsigned c = 0xFFFFFFFFu;
    size_t i;
    if (!built) {
        unsigned k, j;
        for (k = 0; k < 256; k++) {
            unsigned v = k;
            for (j = 0; j < 8; j++) v = (v & 1u) ? (0xEDB88320u ^ (v >> 1)) : (v >> 1);
            table[k] = v;
        }
        built = 1;
    }
    for (i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

typedef struct me_file {
    char   rel[MAX_PATH];    /* '/'-separated, relative to the package root */
    char   abs[MAX_PATH];
    size_t size;
} me_file;

typedef struct me_list {
    me_file *v;
    size_t   count, cap;
    size_t   total;
} me_list;

static int me_list_push(me_list *l, const char *abs, const char *rel, size_t size)
{
    if (l->count >= l->cap) return 0;
    strncpy_s(l->v[l->count].abs, MAX_PATH, abs, _TRUNCATE);
    strncpy_s(l->v[l->count].rel, MAX_PATH, rel, _TRUNCATE);
    l->v[l->count].size = size;
    l->count++;
    l->total += size;
    return 1;
}

/* Deterministic order, so packing the same folder twice yields the same bytes and therefore the
 * same digest. FindFirstFile's order is not specified and is not stable across machines. */
static int me_file_cmp(const void *a, const void *b)
{
    return _stricmp(((const me_file *)a)->rel, ((const me_file *)b)->rel);
}

/* Collect every file below `dir`. `prefix` is the '/'-separated path so far. Returns 0 if the
 * tree could not be read in full or did not fit -- a partial package must never be shipped. */
static int me_collect(const char *dir, const char *prefix, me_list *l, char *err, size_t err_cap)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int ok = 1;

    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        me_err(err, err_cap, "cannot read '%s'", dir);
        return 0;
    }
    do {
        char abs[MAX_PATH], rel[MAX_PATH];
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   /* never follow */
        _snprintf_s(abs, sizeof abs, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        if (prefix[0])
            _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s/%s", prefix, fd.cFileName);
        else
            _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s", fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!me_collect(abs, rel, l, err, err_cap)) { ok = 0; break; }
            continue;
        }
        if (!prefix[0] && _stricmp(fd.cFileName, ME_SIDECAR) == 0) continue;
        if (fd.nFileSizeHigh || fd.nFileSizeLow > ME_MAX_FILE_BYTES) {
            me_err(err, err_cap, "'%s' is too large to embed", rel);
            ok = 0;
            break;
        }
        if (!me_list_push(l, abs, rel, fd.nFileSizeLow)) {
            me_err(err, err_cap, "package has more than %u files", ME_MAX_FILES);
            ok = 0;
            break;
        }
        if (l->total > SH_MPKG_MAX_PAYLOAD) {
            me_err(err, err_cap, "package is over the %u-byte embed budget",
                   (unsigned)SH_MPKG_MAX_PAYLOAD);
            ok = 0;
            break;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

static int me_read_file(const char *path, unsigned char *buf, size_t size)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    size_t got = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    while (got < size) {
        DWORD chunk = (DWORD)((size - got) > 0x10000000 ? 0x10000000 : (size - got));
        DWORD rd = 0;
        if (!ReadFile(h, buf + got, chunk, &rd, NULL) || rd == 0) break;
        got += rd;
    }
    CloseHandle(h);
    return got == size;
}

static void me_put16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF); }
static void me_put32(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v & 0xFF);       p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF); p[3] = (unsigned char)((v >> 24) & 0xFF);
}

unsigned char *sh_mpkg_pack_dir(const char *root, size_t *out_len, char *err, size_t err_cap)
{
    me_list l;
    unsigned char *zip = NULL;
    unsigned *crcs = NULL, *offsets = NULL;
    size_t cap, w = 0, cd_start, i;
    char line[224];

    if (out_len) *out_len = 0;
    if (err && err_cap) err[0] = '\0';
    if (!root || !root[0]) { me_err(err, err_cap, "no package folder given"); return NULL; }

    l.v = (me_file *)HeapAlloc(GetProcessHeap(), 0, ME_MAX_FILES * sizeof(me_file));
    if (!l.v) { me_err(err, err_cap, "out of memory listing the package"); return NULL; }
    l.count = 0; l.cap = ME_MAX_FILES; l.total = 0;

    if (!me_collect(root, "", &l, err, err_cap)) goto fail_list;
    if (l.count == 0) { me_err(err, err_cap, "package folder is empty"); goto fail_list; }
    qsort(l.v, l.count, sizeof(me_file), me_file_cmp);

    crcs = (unsigned *)HeapAlloc(GetProcessHeap(), 0, l.count * sizeof(unsigned));
    offsets = (unsigned *)HeapAlloc(GetProcessHeap(), 0, l.count * sizeof(unsigned));
    if (!crcs || !offsets) { me_err(err, err_cap, "out of memory packing"); goto fail_side; }

    /* local headers (30 + name) + data, then central directory (46 + name), then EOCD (22) */
    cap = l.total + l.count * (30 + 46 + 2 * MAX_PATH) + 22;
    zip = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, cap);
    if (!zip) { me_err(err, err_cap, "out of memory packing"); goto fail_side; }

    for (i = 0; i < l.count; i++) {
        size_t nlen = strlen(l.v[i].rel);
        unsigned char *lh = zip + w;
        if (l.v[i].size && !me_read_file(l.v[i].abs, zip + w + 30 + nlen, l.v[i].size)) {
            me_err(err, err_cap, "cannot read '%s'", l.v[i].rel);
            goto fail_zip;
        }
        crcs[i] = l.v[i].size ? me_crc32(zip + w + 30 + nlen, l.v[i].size) : 0;
        offsets[i] = (unsigned)w;

        me_put32(lh + 0, 0x04034B50u);
        me_put16(lh + 4, 20);            /* version needed: 2.0 */
        me_put16(lh + 6, 0);             /* flags */
        me_put16(lh + 8, 0);             /* method 0 = stored */
        me_put16(lh + 10, 0);            /* mod time  -- fixed, so a repack is byte-identical */
        me_put16(lh + 12, 0x21);         /* mod date  -- 1980-01-01, the zip epoch */
        me_put32(lh + 14, crcs[i]);
        me_put32(lh + 18, (unsigned)l.v[i].size);
        me_put32(lh + 22, (unsigned)l.v[i].size);
        me_put16(lh + 26, (unsigned)nlen);
        me_put16(lh + 28, 0);            /* extra */
        memcpy(lh + 30, l.v[i].rel, nlen);
        w += 30 + nlen + l.v[i].size;
    }

    cd_start = w;
    for (i = 0; i < l.count; i++) {
        size_t nlen = strlen(l.v[i].rel);
        unsigned char *ch = zip + w;
        me_put32(ch + 0, 0x02014B50u);
        me_put16(ch + 4, 20);            /* version made by */
        me_put16(ch + 6, 20);            /* version needed */
        me_put16(ch + 8, 0);
        me_put16(ch + 10, 0);            /* stored */
        me_put16(ch + 12, 0);
        me_put16(ch + 14, 0x21);
        me_put32(ch + 16, crcs[i]);
        me_put32(ch + 20, (unsigned)l.v[i].size);
        me_put32(ch + 24, (unsigned)l.v[i].size);
        me_put16(ch + 28, (unsigned)nlen);
        me_put16(ch + 30, 0);            /* extra */
        me_put16(ch + 32, 0);            /* comment */
        me_put16(ch + 34, 0);            /* disk */
        me_put16(ch + 36, 0);            /* internal attrs */
        me_put32(ch + 38, 0);            /* external attrs */
        me_put32(ch + 42, offsets[i]);
        memcpy(ch + 46, l.v[i].rel, nlen);
        w += 46 + nlen;
    }

    me_put32(zip + w + 0, 0x06054B50u);
    me_put16(zip + w + 4, 0);
    me_put16(zip + w + 6, 0);
    me_put16(zip + w + 8, (unsigned)l.count);
    me_put16(zip + w + 10, (unsigned)l.count);
    me_put32(zip + w + 12, (unsigned)(w - cd_start));
    me_put32(zip + w + 16, (unsigned)cd_start);
    me_put16(zip + w + 20, 0);
    w += 22;

    _snprintf_s(line, sizeof line, _TRUNCATE,
                "MPKG: packed '%s' -- %u file(s), %zu bytes stored", root,
                (unsigned)l.count, w);
    backend_log(line);

    HeapFree(GetProcessHeap(), 0, offsets);
    HeapFree(GetProcessHeap(), 0, crcs);
    HeapFree(GetProcessHeap(), 0, l.v);
    if (out_len) *out_len = w;
    return zip;

fail_zip:
    HeapFree(GetProcessHeap(), 0, zip);
fail_side:
    if (offsets) HeapFree(GetProcessHeap(), 0, offsets);
    if (crcs) HeapFree(GetProcessHeap(), 0, crcs);
fail_list:
    HeapFree(GetProcessHeap(), 0, l.v);
    return NULL;
}

/* ==================================================================== */
/* used-package detection                                                */
/* ==================================================================== */

/* Case-insensitive search for `needle` in the first `n` bytes of `hay`. */
static const char *me_ifind(const char *hay, size_t n, const char *needle)
{
    size_t m = strlen(needle);
    size_t i;
    if (m == 0 || m > n) return NULL;
    for (i = 0; i + m <= n; i++) {
        if (_strnicmp(hay + i, needle, m) == 0) return hay + i;
    }
    return NULL;
}

/* Does the map name this decl identity?
 *
 * Two forms are accepted, because a map does not spell an identity the way the package's folder
 * layout does. Measured against a real map that uses a Cyberdemon package: the package holds
 * `decls/entitydef/ai/demon/cyberdemon_hell.decl`, and the map contains "cyberdemon_hell",
 * "aicomponent/cyberdemon_hell" and "threat/cyberdemon_hell" -- never the folder-shaped
 * `entitydef/ai/demon/cyberdemon_hell`. So:
 *
 *   - the logical name (everything after `decls/<type>/`) as a whole quoted string, and
 *   - the BASENAME, either as a whole quoted string or as the last segment of one.
 *
 * The basename form is the loose one, and that asymmetry is deliberate -- see the header. */
static int me_map_names(const char *json, size_t len, const char *logical)
{
    char pat[MAX_PATH + 4];
    const char *base = strrchr(logical, '/');
    base = base ? base + 1 : logical;

    _snprintf_s(pat, sizeof pat, _TRUNCATE, "\"%s\"", logical);
    if (me_ifind(json, len, pat)) return 1;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "\"%s\"", base);
    if (me_ifind(json, len, pat)) return 1;
    _snprintf_s(pat, sizeof pat, _TRUNCATE, "/%s\"", base);
    if (me_ifind(json, len, pat)) return 1;
    return 0;
}

/* Walk `<pkg>\decls` and ask the map about each identity the package ADDS, stopping at the
 * first hit. `key_prefix` accumulates the path below `decls\` so the identity can be looked up
 * in the decl server's published table, whose keys are `decltree/<path below decls>`. */
static int me_package_used(const char *decls_root, const char *prefix, const char *key_prefix,
                           const char *json, size_t len, unsigned depth)
{
    char pattern[MAX_PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int used = 0;

    if (depth > 16) return 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", decls_root);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        char child[MAX_PATH], rel[MAX_PATH];
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        _snprintf_s(child, sizeof child, _TRUNCATE, "%s\\%s", decls_root, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            char keyrel[MAX_PATH];
            /* depth 0 is the `<type>` folder, and the type is not part of what a map writes --
             * the logical name starts BELOW it. The PUBLISHED key keeps it. */
            if (depth == 0)
                _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s", "");
            else if (prefix[0])
                _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s/%s", prefix, fd.cFileName);
            else
                _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s", fd.cFileName);
            if (key_prefix[0])
                _snprintf_s(keyrel, sizeof keyrel, _TRUNCATE, "%s/%s", key_prefix, fd.cFileName);
            else
                _snprintf_s(keyrel, sizeof keyrel, _TRUNCATE, "%s", fd.cFileName);
            if (me_package_used(child, rel, keyrel, json, len, depth + 1)) { used = 1; break; }
            continue;
        }
        if (depth == 0) continue;              /* loose files directly under decls\ have no type */
        {
            size_t n = strlen(fd.cFileName);
            char stem[MAX_PATH], key[MAX_PATH];
            if (n <= 5 || _stricmp(fd.cFileName + n - 5, ".decl") != 0) continue;
            strncpy_s(stem, sizeof stem, fd.cFileName, n - 5);
            if (prefix[0])
                _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s/%s", prefix, stem);
            else
                _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s", stem);
            _snprintf_s(key, sizeof key, _TRUNCATE, "decltree/%s/%s", key_prefix, fd.cFileName);

            /* Only an identity the package ADDS is evidence. A shadowed override is content the
             * game already has, and a map naming it is not a map that needs this package. */
            if (!sh_overrides_internal_decl_published(key)) continue;
            if (me_map_names(json, len, rel)) { used = 1; break; }
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return used;
}

/* A shard header id is lowercase [a-z0-9_-] -- the grammar the consumer's parser accepts, and
 * the grammar the reference implementation enforces. A nested package (`group/package`) or one
 * with capitals cannot be named in a header at all, so it is reported rather than mangled. */
static int me_id_is_embeddable(const char *name)
{
    size_t i;
    if (!name || !name[0]) return 0;
    if (strlen(name) >= SH_MPKG_ID_CAP) return 0;
    for (i = 0; name[i]; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

size_t sh_mpkg_used_packages(const char *json, size_t len, const char *data_root,
                             sh_mpkg_used *out, size_t cap)
{
    sh_package *packages;
    size_t count = 0, i, kept = 0;

    if (!json || len == 0 || !data_root || !out || cap == 0) return 0;

    /* Nothing published means no package identity is live in this process, so nothing a map
     * saved right now could be using. Say so: silence here would look like a working feature
     * that simply found nothing. */
    if (sh_overrides_internal_decl_published_count() == 0) {
        backend_log("MPKG: no package identities are published in this process, so a saved map "
                    "carries nothing -- packages travel only when the decl server has "
                    "registered them");
        return 0;
    }

    packages = (sh_package *)HeapAlloc(GetProcessHeap(), 0,
                                       ME_MAX_PACKAGES * sizeof(sh_package));
    if (!packages) return 0;

    if (!sh_packages_enumerate(data_root, packages, ME_MAX_PACKAGES, &count)) {
        /* A partial enumeration would silently omit a package the map needs, which is the one
         * failure this feature exists to prevent. Embed nothing rather than embed some. */
        backend_log("MPKG: package enumeration was incomplete; embedding nothing this save");
        HeapFree(GetProcessHeap(), 0, packages);
        return 0;
    }

    for (i = 0; i < count && kept < cap; i++) {
        char decls[MAX_PATH];
        char line[MAX_PATH + 192];
        if (!sh_package_subdir(&packages[i], "decls", decls, sizeof decls)) continue;
        if (!me_package_used(decls, "", "", json, len, 0)) continue;
        if (!me_id_is_embeddable(packages[i].name)) {
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "MPKG: package '%s' is used by this map but its name cannot be a shard "
                        "header id (lowercase letters, digits, '_' and '-' only); it will NOT "
                        "travel with the map -- rename it to embed it",
                        packages[i].name);
            backend_log(line);
            continue;
        }
        strncpy_s(out[kept].id, SH_MPKG_ID_CAP, packages[i].name, _TRUNCATE);
        strncpy_s(out[kept].root, MAX_PATH, packages[i].root, _TRUNCATE);
        kept++;
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "MPKG: map uses package '%s' -- it will travel with the map",
                    packages[i].name);
        backend_log(line);
    }

    HeapFree(GetProcessHeap(), 0, packages);
    return kept;
}
