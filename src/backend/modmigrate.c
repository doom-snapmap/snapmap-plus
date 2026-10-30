/* modmigrate.c -- see modmigrate.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#pragma comment(lib, "shell32.lib")   /* SHGetFolderPathA */

#include "modmigrate.h"
#include "modlegacy.h"
#include "modbundle.h"
#include "modstore.h"
#include "modserve.h"
#include "config_json.h"
#include "backend_log.h"

#define MG_NAME_MAX   520     /* an overrides-relative folder, '/'-separated, UTF-8 */
#define MG_CAUSE_MAX  512
/* The two retired package folder names; this module is the only reader. */
#define MG_OVERRIDES "overrides"         /* retired package folder, read once, never written */
#define MG_BACKUPS   "override-backups"  /* retired package folder, removed only when empty */

enum { MG_PACKAGE = 1, MG_DELIVERED, MG_ZIP, MG_FILE, MG_UNMARKED, MG_LINK };

typedef struct mg_item {
    char name[MG_NAME_MAX];            /* overrides-relative, '/' */
    int kind;
    int outcome;
    int deferred;                      /* left for the worker */
    int nested;                        /* the one package folder inside an unmarked folder */
    unsigned movies;                   /* .bswf movies left out */
    unsigned files;                    /* files found by the measuring walk */
    unsigned long long bytes;
    char reason[MG_CAUSE_MAX];         /* skipped or refused: why; built: notes */
    char id[SH_MB_ID_MAX + 1];
    char version[4 * SH_MB_VERSION_MAX + 1];
    char store[MG_NAME_MAX];           /* folder under mods\, '/' */
    char bundle[SH_MB_HASH_HEX + 1];
    char tree[SH_MB_HASH_HEX + 1];
    char duplicate_of[MG_NAME_MAX];    /* the first folder with the same id */
    unsigned bundle_files, left_out, warnings;
    unsigned long long ms;
    unsigned long long ms_read, ms_stage, ms_store;   /* walk and hash, write the staged tree, build and store */
} mg_item;

typedef struct mg_ctx {
    char root[MAX_PATH];               /* data root, ANSI */
    char overrides_u8[MAX_PATH * 4];   /* UTF-8 */
    wchar_t *overrides_w;              /* long-path form */
    wchar_t *mods_w;
    mg_item *items;
    size_t count, cap;
    unsigned entries;
    int again;
    int backups_removed;
    ULONGLONG started;
    unsigned long long boot_ms;
} mg_ctx;

static SRWLOCK g_mg_lock = SRWLOCK_INIT;
static char g_mg_root[MAX_PATH];
static volatile LONG g_mg_busy;
static HANDLE g_mg_thread;
static sh_modmigrate_stats g_mg_last;

static void mg_log(const char *fmt, ...)
{
    char line[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(line, sizeof line, _TRUNCATE, fmt, ap);
    va_end(ap);
    backend_log(line);
}

static void mg_cause(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(out, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

/* ---------------------------------------------------------------- paths ---- */

void sh_modmigrate_set_data_root(const char *root)
{
    AcquireSRWLockExclusive(&g_mg_lock);
    if (root) strncpy_s(g_mg_root, sizeof g_mg_root, root, _TRUNCATE);
    else g_mg_root[0] = 0;
    ReleaseSRWLockExclusive(&g_mg_lock);
}

static int mg_data_root(char *out, size_t cap)
{
    char base[MAX_PATH];
    AcquireSRWLockShared(&g_mg_lock);
    strncpy_s(out, cap, g_mg_root, _TRUNCATE);
    ReleaseSRWLockShared(&g_mg_lock);
    if (out[0]) return 1;
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, base))) return 0;
    _snprintf_s(out, cap, _TRUNCATE, "%s\\snapmap-plus", base);
    return 1;
}

/* ANSI path (plus an optional UTF-8 tail) to the long wide form. */
static wchar_t *mg_wide(const char *acp, const char *tail_u8)
{
    int na = MultiByteToWideChar(CP_ACP, 0, acp, -1, NULL, 0);
    int nt = tail_u8 ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, tail_u8, -1, NULL, 0) : 1;
    int prefix = (acp[0] && acp[1] == ':' && acp[2] == '\\') ? 4 : 0;
    wchar_t *w, *p;
    if (na <= 0 || nt <= 0) return NULL;
    w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(prefix + na + nt + 1));
    if (!w) return NULL;
    if (prefix) memcpy(w, L"\\\\?\\", 4 * sizeof(wchar_t));
    MultiByteToWideChar(CP_ACP, 0, acp, -1, w + prefix, na);
    if (tail_u8) {
        size_t at = (size_t)prefix + (size_t)na - 1;
        w[at++] = L'\\';
        MultiByteToWideChar(CP_UTF8, 0, tail_u8, -1, w + at, nt);
    }
    for (p = w + prefix; *p; p++) if (*p == L'/') *p = L'\\';
    return w;
}

static wchar_t *mg_wjoin(const wchar_t *dir, const wchar_t *leaf)
{
    size_t a = wcslen(dir), b = wcslen(leaf);
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (a + b + 2));
    if (!w) return NULL;
    memcpy(w, dir, a * sizeof(wchar_t));
    w[a] = L'\\';
    memcpy(w + a + 1, leaf, (b + 1) * sizeof(wchar_t));
    return w;
}

static DWORD mg_attr(const wchar_t *path) { return path ? GetFileAttributesW(path) : INVALID_FILE_ATTRIBUTES; }

static int mg_is_dir(const wchar_t *path)
{
    DWORD a = mg_attr(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

static int mg_is_file(const wchar_t *dir, const wchar_t *leaf)
{
    wchar_t *p = mg_wjoin(dir, leaf);
    DWORD a = mg_attr(p);
    free(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int mg_has_descriptor(const wchar_t *dir)
{
    return mg_is_file(dir, L"package.json") || mg_is_file(dir, L"mod.json");
}

/* Count files and bytes below `dir`, stopping past the caps. Links are
 * counted as found and stop nothing (the converter refuses them). */
static void mg_measure(const wchar_t *dir, unsigned *files, unsigned long long *bytes, unsigned depth)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = mg_wjoin(dir, L"*");
    HANDLE h;
    if (!pattern) return;
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (*files > SH_MODMIGRATE_MAX_FILES || *bytes > SH_MODMIGRATE_MAX_BYTES) break;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && depth < 64) {
                wchar_t *sub = mg_wjoin(dir, fd.cFileName);
                if (sub) { mg_measure(sub, files, bytes, depth + 1); free(sub); }
            }
        } else {
            (*files)++;
            *bytes += ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

/* Remove a staging tree this module created (never anything else: callers
 * pass only paths under <root>\mods-migrate.tmp). */
static void mg_rmtree(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = mg_wjoin(dir, L"*");
    HANDLE h;
    if (!pattern) return;
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *child;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            child = mg_wjoin(dir, fd.cFileName);
            if (!child) continue;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                mg_rmtree(child);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                RemoveDirectoryW(child);
            else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child);
            }
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

/* ---------------------------------------------------------------- items ---- */

static mg_item *mg_add(mg_ctx *c, const char *name, int kind)
{
    mg_item *it;
    if (c->count == c->cap) {
        size_t next = c->cap ? c->cap * 2 : 64;
        mg_item *grown = (mg_item *)realloc(c->items, next * sizeof *grown);
        if (!grown) return NULL;
        c->items = grown;
        c->cap = next;
    }
    it = &c->items[c->count++];
    memset(it, 0, sizeof *it);
    strncpy_s(it->name, sizeof it->name, name, _TRUNCATE);
    it->kind = kind;
    it->outcome = SH_MODMIGRATE_PENDING;
    return it;
}

static void mg_skip(mg_item *it, const char *fmt, ...)
{
    va_list ap;
    it->outcome = SH_MODMIGRATE_SKIPPED;
    va_start(ap, fmt);
    _vsnprintf_s(it->reason, sizeof it->reason, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static int mg_is_group(const char *name)
{
    size_t i;
    if (strlen(name) != 20 || strncmp(name, "map-", 4)) return 0;
    for (i = 4; i < 20; i++)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return 0;
    return 1;
}

static int mg_ends_with(const char *s, const char *suffix)
{
    size_t n = strlen(s), k = strlen(suffix);
    return n >= k && !_stricmp(s + n - k, suffix);
}

/* Descriptor folders one level below an unmarked folder, for the reason text. */
static unsigned mg_nested_packages(const wchar_t *dir, char *first, size_t cap)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = mg_wjoin(dir, L"*");
    HANDLE h;
    unsigned n = 0;
    first[0] = 0;
    if (!pattern) return 0;
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        wchar_t *sub;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        sub = mg_wjoin(dir, fd.cFileName);
        if (sub && mg_has_descriptor(sub)) {
            if (!n++) {
                char *u = sh_mb_utf8(fd.cFileName);
                if (u) { strncpy_s(first, cap, u, _TRUNCATE); free(u); }
            }
        }
        free(sub);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

static void mg_measure_item(mg_item *it, const wchar_t *path)
{
    mg_measure(path, &it->files, &it->bytes, 0);
    if (it->files > SH_MODMIGRATE_MAX_FILES || it->bytes > SH_MODMIGRATE_MAX_BYTES)
        mg_skip(it, "over the migration cap of %u files or %llu MiB (at least %u file(s), %llu MiB); install it "
                    "yourself in parts with sh_mods install", SH_MODMIGRATE_MAX_FILES, SH_MODMIGRATE_MAX_BYTES >> 20,
                it->files, it->bytes >> 20);
}

/* List overrides\ and classify every entry. */
static int mg_scan(mg_ctx *c)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = mg_wjoin(c->overrides_w, L"*");
    HANDLE h;
    if (!pattern) return 0;
    h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0);
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    do {
        char *name;
        wchar_t *path;
        mg_item *it;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        name = sh_mb_utf8(fd.cFileName);
        path = mg_wjoin(c->overrides_w, fd.cFileName);
        c->entries++;
        if (!name || !path) {
            free(name); free(path);
            it = mg_add(c, "?", MG_FILE);
            if (it) mg_skip(it, "a name that is not valid Unicode");
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            it = mg_add(c, name, MG_LINK);
            if (it) mg_skip(it, "a link or junction; links are not followed");
        } else if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            it = mg_add(c, name, mg_ends_with(name, ".zip") ? MG_ZIP : MG_FILE);
            if (it && it->kind == MG_ZIP)
                mg_skip(it, "a ZIP archive; unpack it into its own folder (package.json at the top) and run "
                            "sh_mods migrate --again");
            else if (it)
                mg_skip(it, "a loose file directly in the " MG_OVERRIDES " folder (the retired loose-file layer); put it in a mod "
                            "folder under its engine path");
        } else if (mg_is_group(name)) {
            /* beta.16-19 delivered packages: overrides\map-<16 hex>\<package>. */
            WIN32_FIND_DATAW gd;
            wchar_t *gp = mg_wjoin(path, L"*");
            HANDLE g = gp ? FindFirstFileExW(gp, FindExInfoBasic, &gd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
            unsigned children = 0;
            free(gp);
            if (g != INVALID_HANDLE_VALUE) {
                do {
                    char *leaf, rel[MG_NAME_MAX];
                    wchar_t *sub;
                    if (!wcscmp(gd.cFileName, L".") || !wcscmp(gd.cFileName, L"..")) continue;
                    leaf = sh_mb_utf8(gd.cFileName);
                    sub = mg_wjoin(path, gd.cFileName);
                    children++;
                    _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s/%s", name, leaf ? leaf : "?");
                    if (!leaf || !sub) it = mg_add(c, rel, MG_FILE);
                    else if (gd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                        it = mg_add(c, rel, MG_LINK);
                        if (it) mg_skip(it, "a link or junction; links are not followed");
                    } else if (!(gd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                        it = mg_add(c, rel, MG_FILE);
                        if (it) mg_skip(it, "a file beside the delivered packages, not a package");
                    } else if (!mg_has_descriptor(sub)) {
                        it = mg_add(c, rel, MG_UNMARKED);
                        if (it) mg_skip(it, "a delivered folder without package.json or mod.json");
                    } else {
                        it = mg_add(c, rel, MG_DELIVERED);
                        if (it) mg_measure_item(it, sub);
                    }
                    free(leaf);
                    free(sub);
                } while (FindNextFileW(g, &gd));
                FindClose(g);
            }
            if (!children) {
                it = mg_add(c, name, MG_UNMARKED);
                if (it) mg_skip(it, "an empty delivered-package folder");
            }
        } else if (mg_has_descriptor(path)) {
            it = mg_add(c, name, MG_PACKAGE);
            if (it) mg_measure_item(it, path);
        } else {
            char first[MAX_PATH];
            unsigned nested = mg_nested_packages(path, first, sizeof first);
            if (nested == 1) {
                /* beta.19 read an unmarked folder as a group: its one package
                 * folder is migrated under that package's own id. */
                char rel[MG_NAME_MAX];
                int wn = MultiByteToWideChar(CP_UTF8, 0, first, -1, NULL, 0);
                wchar_t *wfirst = wn > 0 ? (wchar_t *)malloc(sizeof(wchar_t) * (size_t)wn) : NULL;
                wchar_t *sub = NULL;
                if (wfirst) { MultiByteToWideChar(CP_UTF8, 0, first, -1, wfirst, wn); sub = mg_wjoin(path, wfirst); }
                free(wfirst);
                _snprintf_s(rel, sizeof rel, _TRUNCATE, "%s/%s", name, first);
                it = sub ? mg_add(c, rel, MG_PACKAGE) : NULL;
                if (it) {
                    it->nested = 1;
                    mg_measure_item(it, sub);
                }
                free(sub);
                free(name);
                free(path);
                continue;
            }
            it = mg_add(c, name, MG_UNMARKED);
            if (it && nested)
                mg_skip(it, "no package.json or mod.json at its top; it holds %u package folders one level down "
                            "(first: %s\\%s); move each directly under " MG_OVERRIDES " and run sh_mods migrate --again",
                        nested, name, first);
            else if (it)
                mg_skip(it, "no package.json or mod.json at its top, and none one level down");
        }
        free(name);
        free(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 1;
}

/* ------------------------------------------------------------- building ---- */

/* An id from a folder name, as era B named a package by its folder. */
static void mg_slug(const char *name, char *out, size_t cap)
{
    char tmp[SH_MB_ID_MAX + 1];
    size_t i, w = 0;
    const char *leaf = strrchr(name, '/');
    leaf = leaf ? leaf + 1 : name;
    for (i = 0; leaf[i] && w < SH_MB_ID_MAX; i++) {
        char ch = leaf[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '-')) ch = '-';
        if ((ch == '-' || ch == '.') && (!w || tmp[w - 1] == '-' || tmp[w - 1] == '.')) continue;
        tmp[w++] = ch;
    }
    while (w && (tmp[w - 1] == '-' || tmp[w - 1] == '.' || tmp[w - 1] == '_')) w--;
    tmp[w] = 0;
    if (!sh_modlegacy_normalize_id(tmp, out, cap)) strncpy_s(out, cap, "legacy", _TRUNCATE);
}

/* A JSON string member of a descriptor, decoded, or NULL. */
static char *mg_member_text(const sh_json_object *o, const char *key)
{
    const char *raw = sh_json_object_get(o, key);
    size_t n, got = 0;
    char *out;
    if (!raw || raw[0] != '"') return NULL;
    n = strlen(raw);
    out = (char *)malloc(n);
    if (!out) return NULL;
    if (!sh_json_decode_string(raw, n, out, n, &got) || strlen(out) != got) { free(out); return NULL; }
    return out;
}

/* The id and version the converter will give a package (lg_pick_id). */
static void mg_identity(const sh_modlegacy_zip *zip, const char *name, char *id, size_t idcap,
                        char *version, size_t vcap)
{
    const sh_modlegacy_zip_entry *d = sh_modlegacy_zip_find(zip, "package.json");
    sh_json_object o = {0};
    char *text = NULL, *idt = NULL, *vt = NULL;
    size_t n = 0;
    if (!d) d = sh_modlegacy_zip_find(zip, "mod.json");
    if (d) text = (char *)sh_modlegacy_zip_read(d, &n, NULL, 0);
    if (text) {
        const char *t = text;
        if (n >= 3 && !memcmp(t, "\xef\xbb\xbf", 3)) { t += 3; n -= 3; }
        while (n && (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n')) { t++; n--; }
        if (n && sh_json_parse_object(t, n, 24, &o)) {
            idt = mg_member_text(&o, "id");
            vt = mg_member_text(&o, "version");
        }
    }
    if (!(idt && sh_modlegacy_normalize_id(idt, id, idcap))) mg_slug(name, id, idcap);
    if (vt) {
        char *s = vt, *e;
        while (*s == ' ' || *s == '\t') s++;
        e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
        if (sh_mb_version_valid(s)) strncpy_s(version, vcap, s, _TRUNCATE);
        else strncpy_s(version, vcap, "0.0.0", _TRUNCATE);
    } else strncpy_s(version, vcap, "0.0.0", _TRUNCATE);
    free(idt);
    free(vt);
    free(text);
    sh_json_object_free(&o);
}

/* A version label for id that no other installed bundle uses: the wanted
 * one, unless a bundle of another source tree already holds id@wanted; then
 * the wanted label with the first 8 (then 16) hex of this tree. */
static void mg_pick_version(const mg_ctx *c, const char *id, const char *wanted, const char *tree, char *out, size_t cap)
{
    int round;
    for (round = 0; round < 3; round++) {
        WIN32_FIND_DATAW fd;
        wchar_t *pattern;
        char leaf[SH_MS_KEY_MAX + 8];
        HANDLE h;
        int taken = 0;
        if (round == 0) strncpy_s(out, cap, wanted, _TRUNCATE);
        else _snprintf_s(out, cap, _TRUNCATE, "%.*s-%.*s", (int)(SH_MB_VERSION_MAX - 17), wanted, round == 1 ? 8 : 16, tree);
        /* Only <id>@<version>+<build> folders can hold this id@version. */
        _snprintf_s(leaf, sizeof leaf, _TRUNCATE, "%s@%s+*", id, out);
        {
            int n = MultiByteToWideChar(CP_UTF8, 0, leaf, -1, NULL, 0);
            wchar_t *wleaf = n > 0 ? (wchar_t *)malloc(sizeof(wchar_t) * (size_t)n) : NULL;
            if (wleaf) MultiByteToWideChar(CP_UTF8, 0, leaf, -1, wleaf, n);
            pattern = wleaf ? mg_wjoin(c->mods_w, wleaf) : NULL;
            free(wleaf);
        }
        h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
        free(pattern);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                wchar_t *folder, *bj;
                HANDLE f;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || wcsstr(fd.cFileName, L".staging-") ||
                    wcsstr(fd.cFileName, L".old-")) continue;
                folder = mg_wjoin(c->mods_w, fd.cFileName);
                bj = folder ? mg_wjoin(folder, L"bundle.json") : NULL;
                free(folder);
                f = bj ? CreateFileW(bj, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) : INVALID_HANDLE_VALUE;
                free(bj);
                taken = 1;   /* a folder of id@version whose bundle.json does not name this tree */
                if (f != INVALID_HANDLE_VALUE) {
                    LARGE_INTEGER size;
                    char *text = NULL;
                    DWORD got = 0;
                    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20) &&
                        (text = (char *)malloc((size_t)size.QuadPart + 1)) != NULL &&
                        ReadFile(f, text, (DWORD)size.QuadPart, &got, NULL)) {
                        text[got] = 0;
                        if (strstr(text, tree)) taken = 0;
                    }
                    free(text);
                    CloseHandle(f);
                }
            } while (!taken && FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!taken) break;
    }
}

/* modlegacy's builder for a delivered package: the normal builder over the
 * installed bundles, without the game's resource reader (not up at boot). */
static int mg_build_legacy(const char *folder_acp, char *first_cause, size_t n)
{
    wchar_t wide[MAX_PATH * 2];
    char folder[MAX_PATH * 4], cause[SH_MB_CAUSE_MAX];
    sh_ms_list list;
    sh_mb_resolver r;
    sh_mb_bundle b;
    int ok = 0;
    if (first_cause && n) first_cause[0] = 0;
    if (!MultiByteToWideChar(CP_ACP, 0, folder_acp, -1, wide, (int)(sizeof wide / sizeof wide[0])) ||
        !WideCharToMultiByte(CP_UTF8, 0, wide, -1, folder, (int)sizeof folder, NULL, NULL)) {
        if (first_cause && n) strncpy_s(first_cause, n, "no usable folder path", _TRUNCATE);
        return 0;
    }
    memset(&list, 0, sizeof list);
    if (!sh_ms_enumerate(&list)) memset(&list, 0, sizeof list);
    r.installed = sh_ms_request_exists;
    r.installed_context = &list;
    r.stock = NULL;
    r.stock_context = NULL;
    if (!sh_mb_build(folder, &r, &b)) {
        if (first_cause && n) strncpy_s(first_cause, n, b.first_cause, _TRUNCATE);
    } else if (!sh_mb_write(&b, folder, cause, sizeof cause)) {
        if (first_cause && n) strncpy_s(first_cause, n, cause, _TRUNCATE);
    } else ok = 1;
    sh_mb_free(&b);
    sh_ms_list_free(&list);
    return ok;
}

static void mg_note_duplicate(mg_ctx *c, mg_item *it)
{
    size_t i;
    for (i = 0; c->items + i < it; i++) {
        const mg_item *o = &c->items[i];
        if ((o->outcome == SH_MODMIGRATE_BUILT || o->outcome == SH_MODMIGRATE_CONVERTED ||
             o->outcome == SH_MODMIGRATE_REUSED) && !strcmp(o->id, it->id)) {
            strncpy_s(it->duplicate_of, sizeof it->duplicate_of, o->name, _TRUNCATE);
            return;
        }
    }
    /* A later item may still name this id first in list order; the record
     * reports the pair from the second one either way. */
}

static void mg_display(const char *name, char *out, size_t cap)
{
    size_t i;
    strncpy_s(out, cap, name, _TRUNCATE);
    for (i = 0; out[i]; i++) if (out[i] == '/') out[i] = '\\';
}

static void mg_log_item(const mg_item *it)
{
    char shown[MG_NAME_MAX], store[MG_NAME_MAX], dup[2 * MG_NAME_MAX + 200] = "", left[MG_CAUSE_MAX + 64] = "";
    mg_display(it->name, shown, sizeof shown);
    mg_display(it->store, store, sizeof store);
    if (it->duplicate_of[0]) {
        char first[MG_NAME_MAX];
        mg_display(it->duplicate_of, first, sizeof first);
        _snprintf_s(dup, sizeof dup, _TRUNCATE, "; duplicate id %s (also " MG_OVERRIDES "\\%s), both kept", it->id, first);
    }
    if (it->left_out) _snprintf_s(left, sizeof left, _TRUNCATE, "; %u file(s) a bundle cannot carry left out (%u of them .bswf movies), first: %s",
                                  it->left_out, it->movies, it->reason);
    if (it->nested) {
        char parent[MG_NAME_MAX], *slash;
        size_t at = strlen(dup);
        mg_display(it->name, parent, sizeof parent);
        slash = strrchr(parent, '\\');
        if (slash) *slash = 0;
        _snprintf_s(dup + at, sizeof dup - at, _TRUNCATE, "; the one package folder inside " MG_OVERRIDES "\\%s, which has no descriptor of its own, migrated under its own id", parent);
    }
    switch (it->outcome) {
    case SH_MODMIGRATE_BUILT:
        mg_log("MODS: migration: " MG_OVERRIDES "\\%s: built mods\\%s, bundle %.16s, %u file(s), %llu KiB, %u warning(s)%s%s, %llu ms (read %llu, stage %llu, store %llu)",
               shown, store, it->bundle, it->bundle_files, it->bytes >> 10, it->warnings, left, dup, it->ms, it->ms_read, it->ms_stage, it->ms_store);
        break;
    case SH_MODMIGRATE_CONVERTED:
        mg_log("MODS: migration: " MG_OVERRIDES "\\%s: delivered package converted to mods\\%s, bundle %.16s, %u file(s), "
               "%llu KiB, key delivery.%.16s...%s%s, %llu ms (read %llu, convert %llu)",
               shown, store, it->bundle, it->bundle_files, it->bytes >> 10, it->tree, left, dup, it->ms, it->ms_read, it->ms_stage);
        break;
    case SH_MODMIGRATE_REUSED:
        mg_log("MODS: migration: " MG_OVERRIDES "\\%s: delivered package already converted as mods\\%s (key delivery.%.16s...), "
               "reused%s, %llu ms", shown, store, it->tree, dup, it->ms);
        break;
    case SH_MODMIGRATE_REFUSED:
        mg_log("MODS: migration: " MG_OVERRIDES "\\%s: refused -- %s (%llu ms)", shown, it->reason, it->ms);
        break;
    case SH_MODMIGRATE_SKIPPED:
        mg_log("MODS: migration: " MG_OVERRIDES "\\%s: skipped -- %s", shown, it->reason);
        break;
    default:
        mg_log("MODS: migration: " MG_OVERRIDES "\\%s: waiting for the background pass (%u file(s), %llu MiB)",
               shown, it->files, it->bytes >> 20);
        break;
    }
}

static void mg_do_item(mg_ctx *c, mg_item *it, size_t index)
{
    sh_modlegacy_zip zip;
    sh_modlegacy_verdict v;
    sh_modlegacy_origin origin;
    char folder_u8[MAX_PATH * 4], err[MG_CAUSE_MAX] = "", fallback[SH_MB_ID_MAX + 1];
    ULONGLONG t0 = GetTickCount64(), t1;
    size_t i;
    int ok;

    /* One folder is one store step: an install or download on another thread
     * waits for it (modstore_lock), and this folder waits for theirs. */
    modstore_lock();
    memset(&zip, 0, sizeof zip);
    memset(&origin, 0, sizeof origin);
    _snprintf_s(folder_u8, sizeof folder_u8, _TRUNCATE, "%s\\%s", c->overrides_u8, it->name);
    for (i = strlen(c->overrides_u8); folder_u8[i]; i++) if (folder_u8[i] == '/') folder_u8[i] = '\\';
    if (!sh_modlegacy_zip_from_folder(folder_u8, SH_MODMIGRATE_MAX_FILES, SH_MODMIGRATE_MAX_BYTES, &zip, NULL,
                                      err, sizeof err)) {
        it->outcome = SH_MODMIGRATE_REFUSED;
        mg_cause(it->reason, sizeof it->reason, "the folder cannot be read: %s", err);
        goto out;
    }
    {
        /* A tree whose carried part is already over the bundle caps is refused
         * before anything is hashed. */
        unsigned carried_files = 0;
        unsigned long long carried = sh_modlegacy_zip_carried_bytes(&zip, &carried_files);
        if (carried > SH_MB_MAX_TOTAL || carried_files > SH_MB_MAX_FILES) {
            it->outcome = SH_MODMIGRATE_REFUSED;
            mg_cause(it->reason, sizeof it->reason, "a bundle of it would hold at least %u file(s), %llu MiB, over the "
                     "bundle caps of %u files and %llu MiB; split it into smaller mods", carried_files, carried >> 20,
                     SH_MB_MAX_FILES, SH_MB_MAX_TOTAL >> 20);
            goto out;
        }
    }
    if (!sh_modlegacy_zip_tree(&zip, it->tree, err, sizeof err)) {
        it->outcome = SH_MODMIGRATE_REFUSED;
        mg_cause(it->reason, sizeof it->reason, "the folder cannot be fingerprinted: %s", err);
        goto out;
    }
    it->ms_read = GetTickCount64() - t0;
    mg_slug(it->name, fallback, sizeof fallback);
    origin.folder = it->name;
    origin.tree = it->tree;
    origin.fallback_id = fallback;

    if (it->kind == MG_DELIVERED) {
        origin.source = "delivered";
        origin.build = mg_build_legacy;
        t1 = GetTickCount64();
        ok = sh_modlegacy_convert_folder(&zip, it->tree, &origin, &v);
        it->ms_stage = GetTickCount64() - t1;
        if (!ok) {
            it->outcome = SH_MODMIGRATE_REFUSED;
            strncpy_s(it->reason, sizeof it->reason, v.reason[0] ? v.reason : "the conversion failed", _TRUNCATE);
            goto out;
        }
        it->outcome = v.files ? SH_MODMIGRATE_CONVERTED : SH_MODMIGRATE_REUSED;
        strncpy_s(it->id, sizeof it->id, v.id, _TRUNCATE);
        strncpy_s(it->version, sizeof it->version, v.version, _TRUNCATE);
        strncpy_s(it->bundle, sizeof it->bundle, v.bundle, _TRUNCATE);
        it->bundle_files = v.files;
        it->left_out = v.skipped;
        it->movies = v.movies;
        {
            const char *leaf = strrchr(v.folder, '\\');
            _snprintf_s(it->store, sizeof it->store, _TRUNCATE, "%s%s", strstr(v.folder, "\\legacy\\") ? "legacy/" : "",
                        leaf ? leaf + 1 : v.folder);
        }
    } else {
        char id[SH_MB_ID_MAX + 1], wanted[4 * SH_MB_VERSION_MAX + 1], staging[MAX_PATH], key[SH_MS_KEY_MAX + 1];
        char cause[SH_MB_CAUSE_MAX] = "";
        wchar_t *wstage, *wparent;
        sh_ms_install_stats st;
        sh_mb_bundle b;
        char *stage_u8;
        int installed;
        mg_identity(&zip, it->name, id, sizeof id, wanted, sizeof wanted);
        mg_pick_version(c, id, wanted, it->tree, it->version, sizeof it->version);
        origin.source = MG_OVERRIDES;
        origin.version = it->version;
        _snprintf_s(staging, sizeof staging, _TRUNCATE, "%s\\mods-migrate.tmp\\%zu-%lu", c->root, index,
                    GetCurrentProcessId());
        wparent = mg_wide(c->root, "mods-migrate.tmp");
        wstage = mg_wide(staging, NULL);
        if (!wparent || !wstage) { free(wparent); free(wstage); it->outcome = SH_MODMIGRATE_REFUSED;
            strncpy_s(it->reason, sizeof it->reason, "out of memory", _TRUNCATE); goto out; }
        CreateDirectoryW(wparent, NULL);
        if (mg_attr(wstage) != INVALID_FILE_ATTRIBUTES) mg_rmtree(wstage);
        t1 = GetTickCount64();
        ok = sh_modlegacy_stage_folder(&zip, &origin, staging, &v);
        it->ms_stage = GetTickCount64() - t1;
        if (!ok) {
            mg_rmtree(wstage);
            RemoveDirectoryW(wparent);
            free(wparent); free(wstage);
            it->outcome = SH_MODMIGRATE_REFUSED;
            strncpy_s(it->reason, sizeof it->reason, v.reason[0] ? v.reason : "the folder could not be staged", _TRUNCATE);
            goto out;
        }
        strncpy_s(it->id, sizeof it->id, v.id, _TRUNCATE);
        it->left_out = v.skipped;
        it->movies = v.movies;
        stage_u8 = sh_mb_utf8(wstage);
        memset(&b, 0, sizeof b);
        memset(&st, 0, sizeof st);
        t1 = GetTickCount64();
        installed = stage_u8 && sh_ms_install_from_folder(stage_u8, NULL, key, sizeof key, &st, &b, cause, sizeof cause);
        it->ms_store = GetTickCount64() - t1;
        free(stage_u8);
        mg_rmtree(wstage);
        RemoveDirectoryW(wparent);   /* only when nothing else is staged */
        free(wparent);
        free(wstage);
        if (!installed) {
            it->outcome = SH_MODMIGRATE_REFUSED;
            strncpy_s(it->reason, sizeof it->reason, cause[0] ? cause : "the store refused it", _TRUNCATE);
            sh_mb_free(&b);
            goto out;
        }
        it->outcome = SH_MODMIGRATE_BUILT;
        strncpy_s(it->store, sizeof it->store, key, _TRUNCATE);
        strncpy_s(it->bundle, sizeof it->bundle, b.hash, _TRUNCATE);
        it->bundle_files = (unsigned)b.file_count;
        it->warnings = b.warnings;
        sh_mb_free(&b);
    }
    if (it->left_out) strncpy_s(it->reason, sizeof it->reason, v.first_skipped, _TRUNCATE);
    mg_note_duplicate(c, it);
out:
    sh_modlegacy_zip_free(&zip);
    it->ms = GetTickCount64() - t0;
    mg_log_item(it);
    modstore_unlock();
}

/* ---------------------------------------------------------------- record ---- */

typedef struct mg_buf { char *p; size_t n, cap; int failed; } mg_buf;

static void mg_put(mg_buf *b, const char *s, size_t n)
{
    if (b->failed) return;
    if (b->n + n + 1 > b->cap) {
        size_t next = b->cap ? b->cap * 2 : 4096;
        char *grown;
        while (next < b->n + n + 1) next *= 2;
        grown = (char *)realloc(b->p, next);
        if (!grown) { b->failed = 1; return; }
        b->p = grown;
        b->cap = next;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void mg_puts(mg_buf *b, const char *s) { mg_put(b, s, strlen(s)); }

static void mg_putf(mg_buf *b, const char *fmt, ...)
{
    char tmp[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = _vsnprintf_s(tmp, sizeof tmp, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n > 0) mg_put(b, tmp, (size_t)n);
}

static void mg_putq(mg_buf *b, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    mg_put(b, "\"", 1);
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { char e[2] = { '\\', (char)ch }; mg_put(b, e, 2); }
        else if (ch < 0x20) { char e[6] = { '\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 15] }; mg_put(b, e, 6); }
        else mg_put(b, (const char *)&ch, 1);
    }
    mg_put(b, "\"", 1);
}

static const char *mg_outcome_name(int o)
{
    switch (o) {
    case SH_MODMIGRATE_BUILT: return "built";
    case SH_MODMIGRATE_CONVERTED: return "converted";
    case SH_MODMIGRATE_REUSED: return "reused";
    case SH_MODMIGRATE_SKIPPED: return "skipped";
    case SH_MODMIGRATE_REFUSED: return "refused";
    default: return "pending";
    }
}

static const char *mg_kind_name(int k)
{
    switch (k) {
    case MG_PACKAGE: return "package";
    case MG_DELIVERED: return "delivered";
    case MG_ZIP: return "zip";
    case MG_FILE: return "file";
    case MG_LINK: return "link";
    default: return "folder";
    }
}

static void mg_stats(const mg_ctx *c, sh_modmigrate_stats *s)
{
    size_t i, k;
    memset(s, 0, sizeof *s);
    s->entries = c->entries;
    s->ran = 1;
    s->backups_removed = c->backups_removed;
    s->boot_ms = c->boot_ms;
    s->ms = GetTickCount64() - c->started;
    for (i = 0; i < c->count; i++) {
        const mg_item *it = &c->items[i];
        switch (it->outcome) {
        case SH_MODMIGRATE_BUILT: s->built++; s->bytes += it->bytes; break;
        case SH_MODMIGRATE_CONVERTED: s->converted++; s->bytes += it->bytes; break;
        case SH_MODMIGRATE_REUSED: s->reused++; break;
        case SH_MODMIGRATE_SKIPPED: s->skipped++; break;
        case SH_MODMIGRATE_REFUSED: s->refused++; break;
        default: s->pending++; break;
        }
        if (it->duplicate_of[0]) s->duplicates++;
        (void)k;
    }
}

static int mg_write_record(mg_ctx *c, int complete)
{
    mg_buf b = {0};
    sh_modmigrate_stats s;
    size_t i;
    int first = 1, ok;
    wchar_t *path, *tmp;
    FILETIME ft;
    unsigned long long now;
    mg_stats(c, &s);
    GetSystemTimeAsFileTime(&ft);
    now = ((((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ull) / 10000000ull;
    mg_puts(&b, "{\n  \"format\": 1,\n  \"complete\": ");
    mg_puts(&b, complete ? "true" : "false");
    mg_putf(&b, ",\n  \"migratedAt\": %llu,\n  \"source\": ", now);
    mg_putq(&b, c->overrides_u8);
    mg_putf(&b, ",\n  \"entries\": %u,\n  \"built\": %u,\n  \"converted\": %u,\n  \"reused\": %u,\n  \"skipped\": %u,"
                "\n  \"refused\": %u,\n  \"duplicates\": %u,\n  \"pending\": %u,\n  \"bootMs\": %llu,\n  \"ms\": %llu,"
                "\n  \"overrideBackupsRemoved\": %s,\n  \"folders\": [",
            s.entries, s.built, s.converted, s.reused, s.skipped, s.refused, s.duplicates, s.pending, s.boot_ms, s.ms,
            c->backups_removed ? "true" : "false");
    for (i = 0; i < c->count; i++) {
        const mg_item *it = &c->items[i];
        mg_puts(&b, i ? ",\n    {" : "\n    {");
        mg_puts(&b, "\"folder\": "); mg_putq(&b, it->name);
        mg_puts(&b, ", \"kind\": "); mg_putq(&b, mg_kind_name(it->kind));
        mg_puts(&b, ", \"outcome\": "); mg_putq(&b, mg_outcome_name(it->outcome));
        if (it->files || it->bytes) mg_putf(&b, ", \"files\": %u, \"bytes\": %llu", it->files, it->bytes);
        if (it->id[0]) { mg_puts(&b, ", \"id\": "); mg_putq(&b, it->id); }
        if (it->version[0]) { mg_puts(&b, ", \"version\": "); mg_putq(&b, it->version); }
        if (it->store[0]) { mg_puts(&b, ", \"store\": "); mg_putq(&b, it->store); }
        if (it->bundle[0]) { mg_puts(&b, ", \"bundle\": "); mg_putq(&b, it->bundle); }
        if (it->tree[0]) { mg_puts(&b, ", \"tree\": "); mg_putq(&b, it->tree); }
        if (it->bundle_files) mg_putf(&b, ", \"bundleFiles\": %u", it->bundle_files);
        if (it->left_out) mg_putf(&b, ", \"leftOut\": %u, \"moviesLeftOut\": %u", it->left_out, it->movies);
        if (it->nested) mg_puts(&b, ", \"nested\": true");
        if (it->warnings) mg_putf(&b, ", \"warnings\": %u", it->warnings);
        if (it->duplicate_of[0]) { mg_puts(&b, ", \"duplicateOf\": "); mg_putq(&b, it->duplicate_of); }
        if (it->reason[0]) { mg_puts(&b, ", \"reason\": "); mg_putq(&b, it->reason); }
        if (it->ms) mg_putf(&b, ", \"ms\": %llu", it->ms);
        mg_puts(&b, "}");
    }
    mg_puts(&b, "\n  ],\n  \"trees\": {");
    for (i = 0; i < c->count; i++) {
        const mg_item *it = &c->items[i];
        size_t k;
        int seen = 0;
        if (!it->tree[0] || !it->store[0] || (it->outcome != SH_MODMIGRATE_BUILT &&
            it->outcome != SH_MODMIGRATE_CONVERTED && it->outcome != SH_MODMIGRATE_REUSED)) continue;
        for (k = 0; k < i; k++)
            if (!strcmp(c->items[k].tree, it->tree) && c->items[k].store[0] &&
                (c->items[k].outcome == SH_MODMIGRATE_BUILT || c->items[k].outcome == SH_MODMIGRATE_CONVERTED ||
                 c->items[k].outcome == SH_MODMIGRATE_REUSED)) seen = 1;
        if (seen) continue;   /* two identical trees: the first one answers */
        mg_puts(&b, first ? "\n    " : ",\n    ");
        first = 0;
        mg_putq(&b, it->tree);
        mg_puts(&b, ": ");
        mg_putq(&b, it->store);
    }
    mg_puts(&b, first ? "}\n}\n" : "\n  }\n}\n");
    if (b.failed) { free(b.p); return 0; }
    CreateDirectoryW(c->mods_w, NULL);
    path = mg_wjoin(c->mods_w, L".migrated.json");
    tmp = mg_wjoin(c->mods_w, L".migrated.json.tmp");
    ok = 0;
    if (path && tmp) {
        HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD put = 0;
        if (h != INVALID_HANDLE_VALUE) {
            ok = WriteFile(h, b.p, (DWORD)b.n, &put, NULL) && put == (DWORD)b.n && FlushFileBuffers(h);
            CloseHandle(h);
            ok = ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
            if (!ok) DeleteFileW(tmp);
        }
    }
    free(path);
    free(tmp);
    free(b.p);
    return ok;
}

/* 1 when the record exists and says the pass completed. */
static int mg_record_complete(const mg_ctx *c, int *exists)
{
    wchar_t *path = mg_wjoin(c->mods_w, L".migrated.json");
    HANDLE h;
    char buf[512];
    DWORD got = 0;
    int complete = 0;
    *exists = 0;
    if (!path) return 0;
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    free(path);
    if (h == INVALID_HANDLE_VALUE) return 0;
    *exists = 1;
    if (ReadFile(h, buf, sizeof buf - 1, &got, NULL)) {
        buf[got] = 0;
        complete = strstr(buf, "\"complete\": true") != NULL;
    }
    CloseHandle(h);
    return complete;
}

/* ----------------------------------------------------------------- pass ---- */

static void mg_ctx_free(mg_ctx *c)
{
    if (!c) return;
    free(c->items);
    free(c->overrides_w);
    free(c->mods_w);
    free(c);
}

/* Resolve the folders and decide whether a pass is due. Returns the context
 * (scanned) or NULL with *why: 0 nothing to do, -1 a failure. */
static mg_ctx *mg_prepare(int again, int *why)
{
    mg_ctx *c = (mg_ctx *)calloc(1, sizeof *c);
    char *u8;
    int exists = 0;
    *why = -1;
    if (!c) return NULL;
    c->again = again;
    c->started = GetTickCount64();
    if (!mg_data_root(c->root, sizeof c->root)) { mg_log("MODS: migration: the data folder cannot be resolved"); mg_ctx_free(c); return NULL; }
    c->overrides_w = mg_wide(c->root, MG_OVERRIDES);
    c->mods_w = mg_wide(c->root, "mods");
    if (!c->overrides_w || !c->mods_w) { mg_ctx_free(c); return NULL; }
    if (!mg_is_dir(c->overrides_w)) { *why = 0; mg_ctx_free(c); return NULL; }
    if (mg_record_complete(c, &exists) && !again) { *why = 0; mg_ctx_free(c); return NULL; }
    u8 = sh_mb_utf8(c->overrides_w);
    if (!u8) { mg_ctx_free(c); return NULL; }
    /* The plain spelling, for the record and the walk (both accept it). */
    strncpy_s(c->overrides_u8, sizeof c->overrides_u8, strncmp(u8, "\\\\?\\", 4) ? u8 : u8 + 4, _TRUNCATE);
    free(u8);
    if (exists && !again) mg_log("MODS: migration: the record says the last pass did not finish; running it again");
    {
        /* Staging left by a pass the process did not finish is ours to drop. */
        wchar_t *stale = mg_wide(c->root, "mods-migrate.tmp");
        if (stale && mg_is_dir(stale)) mg_rmtree(stale);
        free(stale);
    }
    if (!mg_scan(c)) { mg_log("MODS: migration: the " MG_OVERRIDES " folder cannot be listed (error %lu)", GetLastError()); mg_ctx_free(c); return NULL; }
    {
        /* An empty backups folder (beta.15's migration area) goes; a
         * non-empty one is the player's and stays. */
        wchar_t *backups = mg_wide(c->root, MG_BACKUPS);
        if (backups && mg_is_dir(backups)) {
            if (RemoveDirectoryW(backups)) {
                c->backups_removed = 1;
                mg_log("MODS: migration: removed the empty " MG_BACKUPS " folder");
            } else mg_log("MODS: migration: " MG_BACKUPS " is not empty; it is left in place");
        }
        free(backups);
    }
    *why = 1;
    return c;
}

static void mg_publish_stats(const mg_ctx *c)
{
    sh_modmigrate_stats s;
    mg_stats(c, &s);
    AcquireSRWLockExclusive(&g_mg_lock);
    g_mg_last = s;
    ReleaseSRWLockExclusive(&g_mg_lock);
}

static void mg_finish(mg_ctx *c)
{
    sh_modmigrate_stats s;
    int wrote = mg_write_record(c, 1);
    mg_stats(c, &s);
    mg_log("MODS: migration: %u entr%s in " MG_OVERRIDES ": %u built, %u delivered converted, %u delivered reused, %u skipped, "
           "%u refused, %u duplicate id(s); %llu MiB in %llu ms (%llu ms before the serving index); record %s; "
           "the " MG_OVERRIDES " folder is unchanged",
           s.entries, s.entries == 1 ? "y" : "ies", s.built, s.converted, s.reused, s.skipped, s.refused, s.duplicates,
           s.bytes >> 20, s.ms, s.boot_ms, wrote ? "mods\\.migrated.json" : "NOT written");
    mg_publish_stats(c);
}

static DWORD WINAPI mg_worker(void *param)
{
    mg_ctx *c = (mg_ctx *)param;
    size_t i;
    int any = 0;
    for (i = 0; i < c->count; i++)
        if (c->items[i].outcome == SH_MODMIGRATE_PENDING) {
            mg_do_item(c, &c->items[i], i);
            mg_publish_stats(c);
            any = 1;
        }
    mg_finish(c);
    if (any) {
        int n = sh_modserve_index_rescan();
        mg_log("MODS: migration: serving index rebuilt after the background pass (%d bundle(s) active); new editor rows "
               "need 'sh_mods palette rebuild' if the editor palette was already built", n);
    }
    mg_ctx_free(c);
    InterlockedExchange(&g_mg_busy, 0);
    return 0;
}

static int mg_spawn(mg_ctx *c)
{
    HANDLE t;
    if (g_mg_thread) { WaitForSingleObject(g_mg_thread, 0); CloseHandle(g_mg_thread); g_mg_thread = NULL; }
    t = CreateThread(NULL, 0, mg_worker, c, 0, NULL);
    if (!t) return 0;
    g_mg_thread = t;
    return 1;
}

int sh_modmigrate_boot(void)
{
    mg_ctx *c;
    int why;
    size_t i;
    unsigned long long budget = 0;
    unsigned files = 0;
    if (InterlockedCompareExchange(&g_mg_busy, 1, 0) != 0) return -1;
    c = mg_prepare(0, &why);
    if (!c) { InterlockedExchange(&g_mg_busy, 0); return why; }
    /* Small folders now, so the serving index built next already has them. */
    for (i = 0; i < c->count; i++) {
        mg_item *it = &c->items[i];
        if (it->outcome != SH_MODMIGRATE_PENDING) { mg_log_item(it); continue; }
        if (it->bytes <= SH_MODMIGRATE_BOOT_BYTES && it->files <= SH_MODMIGRATE_BOOT_FILES &&
            budget + it->bytes <= SH_MODMIGRATE_BOOT_TOTAL && files + it->files <= SH_MODMIGRATE_BOOT_TOTAL_FILES &&
            GetTickCount64() - c->started < SH_MODMIGRATE_BOOT_MS) {
            budget += it->bytes;
            files += it->files;
            mg_do_item(c, it, i);
        } else it->deferred = 1;
    }
    c->boot_ms = GetTickCount64() - c->started;
    mg_publish_stats(c);
    {
        unsigned deferred = 0;
        unsigned long long bytes = 0;
        for (i = 0; i < c->count; i++)
            if (c->items[i].outcome == SH_MODMIGRATE_PENDING) { deferred++; bytes += c->items[i].bytes; }
        if (!deferred) {
            mg_finish(c);
            mg_ctx_free(c);
            InterlockedExchange(&g_mg_busy, 0);
            return 1;
        }
        mg_write_record(c, 0);
        mg_log("MODS: migration: %llu ms at boot; %u more folder(s) (%llu MiB) continue on a background thread and "
               "the serving index is rebuilt when they are done", c->boot_ms, deferred, bytes >> 20);
        if (!mg_spawn(c)) {
            mg_log("MODS: migration: the background thread could not start; the larger folders are done now");
            for (i = 0; i < c->count; i++)
                if (c->items[i].outcome == SH_MODMIGRATE_PENDING) mg_do_item(c, &c->items[i], i);
            mg_finish(c);
            mg_ctx_free(c);
            InterlockedExchange(&g_mg_busy, 0);
        }
    }
    return 1;
}

int sh_modmigrate_run(int again, sh_modmigrate_stats *stats)
{
    mg_ctx *c;
    int why;
    size_t i;
    if (stats) memset(stats, 0, sizeof *stats);
    if (InterlockedCompareExchange(&g_mg_busy, 1, 0) != 0) return -1;
    c = mg_prepare(again, &why);
    if (!c) {
        InterlockedExchange(&g_mg_busy, 0);
        if (why == 0) mg_log("MODS: migration: nothing to do (no overrides folder, or the record says it is done; "
                             "'sh_mods migrate --again' repeats it)");
        return why;
    }
    for (i = 0; i < c->count; i++) {
        if (c->items[i].outcome == SH_MODMIGRATE_PENDING) mg_do_item(c, &c->items[i], i);
        else mg_log_item(&c->items[i]);
    }
    mg_finish(c);
    if (stats) mg_stats(c, stats);
    mg_ctx_free(c);
    InterlockedExchange(&g_mg_busy, 0);
    return 1;
}

int sh_modmigrate_start(int again)
{
    mg_ctx *c;
    int why;
    size_t i;
    if (InterlockedCompareExchange(&g_mg_busy, 1, 0) != 0) return -1;
    c = mg_prepare(again, &why);
    if (!c) {
        InterlockedExchange(&g_mg_busy, 0);
        if (why == 0) mg_log("MODS: migration: nothing to do (no overrides folder, or the record says it is done; "
                             "'sh_mods migrate --again' repeats it)");
        return why;
    }
    for (i = 0; i < c->count; i++) if (c->items[i].outcome != SH_MODMIGRATE_PENDING) mg_log_item(&c->items[i]);
    if (!mg_spawn(c)) {
        mg_ctx_free(c);
        InterlockedExchange(&g_mg_busy, 0);
        mg_log("MODS: migration: the background thread could not start");
        return -1;
    }
    return 1;
}

int sh_modmigrate_busy(void) { return InterlockedCompareExchange(&g_mg_busy, 0, 0) != 0; }

int sh_modmigrate_wait(unsigned ms)
{
    ULONGLONG until = GetTickCount64() + ms;
    while (sh_modmigrate_busy()) {
        if (GetTickCount64() >= until) return 0;
        Sleep(10);
    }
    return 1;
}

void sh_modmigrate_last(sh_modmigrate_stats *out)
{
    if (!out) return;
    AcquireSRWLockShared(&g_mg_lock);
    *out = g_mg_last;
    ReleaseSRWLockShared(&g_mg_lock);
}
