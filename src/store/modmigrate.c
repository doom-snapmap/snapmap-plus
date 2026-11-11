/* modmigrate.c -- see modmigrate.h. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modmigrate.h"
#include "modlegacy.h"
#include "modbundle.h"
#include "modstore.h"
#include "modstore_upgrade.h"
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
    int nested;                        /* the one package folder inside an unmarked folder */
    unsigned movies;                   /* .bswf movies left out */
    unsigned files;                    /* files found by the measuring walk */
    unsigned long long bytes;
    char reason[MG_CAUSE_MAX];         /* skipped or refused: why; built: notes */
    char id[SH_MB_ID_MAX + 1];
    char store[MG_NAME_MAX];           /* folder under mods\, '/' */
    char bundle[SH_MB_HASH_HEX + 1];
    char tree[SH_MB_HASH_HEX + 1];
    char duplicate_of[MG_NAME_MAX];    /* the first folder with the same id */
    unsigned bundle_files, left_out, warnings;
    unsigned pointers;                 /* of bundle_files, stock pointers to installed content */
    unsigned long long pointer_bytes;  /* their bytes, which the store did not receive */
    unsigned long long ms;
    unsigned long long ms_read, ms_stage, ms_store;   /* walk and hash, write the staged tree, build and store */
} mg_item;

typedef struct mg_ctx {
    char root[MAX_PATH * 4];           /* data root, UTF-8 */
    char overrides_u8[MAX_PATH * 4];   /* UTF-8 */
    wchar_t *overrides_w;              /* long-path form */
    wchar_t *mods_w;
    mg_item *items;
    size_t count, cap;
    unsigned entries;
    int again;
    int backups_removed;
    ULONGLONG started;
} mg_ctx;

static SRWLOCK g_mg_lock = SRWLOCK_INIT;
static char g_mg_root[MAX_PATH * 4];

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
    /* A delivered package converted here takes its salt from the store. */
    sh_modlegacy_set_salter(sh_ms_assign_salt);
    AcquireSRWLockShared(&g_mg_lock);
    strncpy_s(out, cap, g_mg_root, _TRUNCATE);
    ReleaseSRWLockShared(&g_mg_lock);
    if (out[0]) return 1;
    return sh_mb_user_root(out, cap);
}

/* A UTF-8 path (plus an optional UTF-8 tail) to the long wide form. */
static wchar_t *mg_wide(const char *acp, const char *tail_u8)
{
    int na = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, acp, -1, NULL, 0);
    int nt = tail_u8 ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, tail_u8, -1, NULL, 0) : 1;
    int prefix = (acp[0] && acp[1] == ':' && acp[2] == '\\') ? 4 : 0;
    wchar_t *w, *p;
    if (na <= 0 || nt <= 0) return NULL;
    w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(prefix + na + nt + 1));
    if (!w) return NULL;
    if (prefix) memcpy(w, L"\\\\?\\", 4 * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, acp, -1, w + prefix, na);
    if (tail_u8) {
        size_t at = (size_t)prefix + (size_t)na - 1;
        w[at++] = L'\\';
        MultiByteToWideChar(CP_UTF8, 0, tail_u8, -1, w + at, nt);
    }
    for (p = w + prefix; *p; p++) if (*p == L'/') *p = L'\\';
    return w;
}

/* The store folder as plain UTF-8 (no \\?\ prefix), for the product record. */
static int mg_mods_u8(const wchar_t *mods_w, char *out, size_t cap)
{
    char *u = mods_w ? sh_mb_utf8(mods_w) : NULL;
    if (!u) return 0;
    strncpy_s(out, cap, strncmp(u, "\\\\?\\", 4) ? u : u + 4, _TRUNCATE);
    free(u);
    return 1;
}

/* The record's text ("migration" section, compact canonical JSON), or NULL. */
static char *mg_record_text(const wchar_t *mods_w)
{
    char mods[MAX_PATH * 4], *text = NULL;
    if (!mg_mods_u8(mods_w, mods, sizeof mods)) return NULL;
    if (sh_mb_record_get(mods, SH_MODMIGRATE_RECORD, &text) != 1) { free(text); return NULL; }
    return text;
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
        mg_skip(it, "it holds at least %u file(s) and %llu MiB, more than the %u files or %llu MiB one folder can move; "
                    "split it into smaller mod folders and put each in the mods folder yourself",
                it->files, it->bytes >> 20, SH_MODMIGRATE_MAX_FILES, SH_MODMIGRATE_MAX_BYTES >> 20);
}

/* The retired loose-file layer (SnapHak and early releases served
 * overrides\<engine path> directly): a file directly in overrides\ that is
 * not a ZIP, and a folder that is not a delivered-package group, has no
 * descriptor and holds no package folder one level down (a tree of engine
 * paths, such as generated\decls\...). The loose step moves them, with the
 * strings document, into mods\my-overrides\. */
static int mg_loose_entry(const wchar_t *dir, const WIN32_FIND_DATAW *fd)
{
    char first[MAX_PATH], *name;
    wchar_t *path;
    int loose;
    if (fd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return 0;
    name = sh_mb_utf8(fd->cFileName);
    if (!name) return 0;
    if (!(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        loose = !mg_ends_with(name, ".zip");
        free(name);
        return loose;
    }
    path = mg_wjoin(dir, fd->cFileName);
    loose = path && !mg_is_group(name) && !mg_has_descriptor(path) && mg_nested_packages(path, first, sizeof first) == 0;
    free(path);
    free(name);
    return loose;
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
        c->entries++;
        if (mg_loose_entry(c->overrides_w, &fd)) continue;   /* the loose step's (sh_modmigrate_loose_run) */
        name = sh_mb_utf8(fd.cFileName);
        path = mg_wjoin(c->overrides_w, fd.cFileName);
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
            /* Every other loose file is the loose step's. */
            it = mg_add(c, name, MG_ZIP);
            if (it)
                mg_skip(it, "it is a ZIP archive; unpack it into a folder of its own with package.json at the top, then "
                            "close DOOM and run: snapmap-plus migrate");
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
            it = mg_add(c, name, MG_UNMARKED);   /* none one level down is the loose layer */
            if (it)
                mg_skip(it, "it has no package.json or mod.json at its top but holds %u package folders one level down "
                            "(the first is %s\\%s); move each directly into " MG_OVERRIDES ", then close DOOM and run: "
                            "snapmap-plus migrate", nested, name, first);
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

/* The id the converter will give a package (lg_pick_id). */
static void mg_identity(const sh_modlegacy_zip *zip, const char *name, char *id, size_t idcap)
{
    const sh_modlegacy_zip_entry *d = sh_modlegacy_zip_find(zip, "package.json");
    sh_json_object o = {0};
    char *text = NULL, *idt = NULL;
    size_t n = 0;
    if (!d) d = sh_modlegacy_zip_find(zip, "mod.json");
    if (d) text = (char *)sh_modlegacy_zip_read(d, &n, NULL, 0);
    if (text) {
        const char *t = text;
        if (n >= 3 && !memcmp(t, "\xef\xbb\xbf", 3)) { t += 3; n -= 3; }
        while (n && (*t == ' ' || *t == '\t' || *t == '\r' || *t == '\n')) { t++; n--; }
        if (n && sh_json_parse_object(t, n, 24, &o)) {
            idt = mg_member_text(&o, "id");
        }
    }
    if (!(idt && sh_modlegacy_normalize_id(idt, id, idcap))) mg_slug(name, id, idcap);
    free(idt);
    free(text);
    sh_json_object_free(&o);
}

/* Whether the migration record's trees give `tree` to the store folder
 * `name` (an earlier pass made it from this tree). */
static int mg_tree_folder_is(const char *mods, const char *tree, const char *name)
{
    char *text = NULL, want[MAX_PATH * 2], *esc = NULL;
    int mine = 0;
    size_t i, w = 0;
    if (!tree[0] || sh_mb_record_get(mods, "migration", &text) != 1 || !text) { free(text); return 0; }
    esc = (char *)malloc(strlen(name) * 2 + 1);
    if (esc) {
        for (i = 0; name[i]; i++) {
            if (name[i] == '"') esc[w++] = '\\';
            esc[w++] = name[i] == '\\' ? '/' : name[i];
        }
        esc[w] = 0;
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"%s\":\"%s\"", tree, esc);
        mine = strstr(text, want) != NULL;
    }
    free(esc);
    free(text);
    return mine;
}

/* The store folder for an authored package: mods\<its overrides folder
 * name>, the same folder again on a repeated pass (the migration record's
 * trees give this tree to it), else that name with the tree's first 8, then
 * 16 hex when another mod holds it. Two packages of one name never share a
 * folder: 0 when every candidate is another mod's. */
static int mg_target_name(const mg_ctx *c, const mg_item *it, char *out, size_t cap)
{
    const char *leaf = strrchr(it->name, '/');
    int round;
    leaf = leaf ? leaf + 1 : it->name;
    for (round = 0; round < 3; round++) {
        char folder[MAX_PATH * 4], mods[MAX_PATH * 4];
        wchar_t *w;
        DWORD a;
        if (round == 0) strncpy_s(out, cap, leaf, _TRUNCATE);
        else _snprintf_s(out, cap, _TRUNCATE, "%s-%.*s", leaf, round == 1 ? 8 : 16, it->tree);
        if (!mg_mods_u8(c->mods_w, mods, sizeof mods)) return 0;
        _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\%s", mods, out);
        w = sh_mb_wpath(folder);
        a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
        free(w);
        if (a == INVALID_FILE_ATTRIBUTES) return 1;
        if (mg_tree_folder_is(mods, it->tree, out)) return 1;
    }
    return 0;
}

/* What the last mg_build_legacy built: its stock pointers (the migration
 * runs one folder at a time under the store lock). */
static unsigned g_mg_pointers;
static unsigned long long g_mg_pointer_bytes;

/* Whether the game holds `request`, from the installed archives the stock
 * bridge reads from disk (the store tool runs without the engine): 1 yes, 0
 * no, -1 when no DOOM folder was given. A file outside the mod's namespace
 * must replace a game file, which only this can confirm. */
static int mg_stock(void *context, const char *request)
{
    char where[64];
    (void)context;
    if (!sh_mb_stock_available()) return -1;
    return sh_mb_stock_container(request, NULL, where, sizeof where) ? 1 : 0;
}

/* modlegacy's builder for a delivered package: the normal builder over the
 * installed bundles and the game's files read from the install (mg_stock).
 * Copies of installed content become stock pointers. A migrated package is
 * content the product received, so its replacements are kept
 * (sh_mb_keep_replacements_here). */
static int mg_build_legacy(const char *folder_utf8, char *first_cause, size_t n)
{
    char folder[MAX_PATH * 4], cause[SH_MB_CAUSE_MAX];
    sh_ms_list list;
    sh_mb_resolver r;
    sh_mb_bundle b;
    int ok = 0;
    if (first_cause && n) first_cause[0] = 0;
    if (!folder_utf8 || strlen(folder_utf8) >= sizeof folder || strncpy_s(folder, sizeof folder, folder_utf8, _TRUNCATE) != 0) {
        if (first_cause && n) strncpy_s(first_cause, n, "no usable folder path", _TRUNCATE);
        return 0;
    }
    memset(&list, 0, sizeof list);
    if (!sh_ms_enumerate(&list)) memset(&list, 0, sizeof list);
    r.installed = sh_ms_request_exists;
    r.installed_context = &list;
    r.stock = mg_stock;
    r.stock_context = NULL;
    sh_mb_keep_replacements_here(1);
    if (!sh_mb_build(folder, NULL, &r, &b)) {
        if (first_cause && n) strncpy_s(first_cause, n, b.first_cause, _TRUNCATE);
    } else if (!sh_mb_write(&b, folder, cause, sizeof cause)) {
        if (first_cause && n) strncpy_s(first_cause, n, cause, _TRUNCATE);
    } else {
        ok = 1;
        g_mg_pointers = b.stock_count;
        g_mg_pointer_bytes = b.stock_bytes;
    }
    sh_mb_keep_replacements_here(0);
    sh_mb_free(&b);
    sh_ms_list_free(&list);
    return ok;
}

/* Salt a staged folder from the package id its files are named by
 * (mods/<id>/), then install it at the root as `target` under the current
 * rules, keeping its replacements (content the product received). */
static int mg_install_staged(const char *stage_u8, const char *from, const char *target, char *key, size_t key_cap,
                             sh_ms_install_stats *st, sh_mb_bundle *b, char *cause, size_t cause_cap)
{
    char salt[SH_SALT_LEN + 1], root[MAX_PATH * 4], folder[MAX_PATH * 4];
    int ok, again = 0;
    /* Moved again into the folder an earlier pass made: it keeps its salt. */
    if (sh_ms_root(root, sizeof root)) {
        _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\%s", root, target);
        if (sh_mb_descriptor_state(folder) == SH_MB_DESC_BUILT) {
            wchar_t *w = sh_mb_wpath(folder);
            char *text = NULL;
            size_t n = 0;
            if (w) {
                char *u = sh_mb_utf8(w);
                char mj[MAX_PATH * 4];
                _snprintf_s(mj, sizeof mj, _TRUNCATE, "%s\\mod.json", u ? u : folder);
                text = (char *)sh_mb_read_file(mj, 16u << 20, &n);
                free(u);
            }
            free(w);
            if (text) {
                const char *at = strstr(text, "\"salt\"");
                at = at ? strchr(at + 6, '"') : NULL;
                if (at && sh_salt_valid(at + 1, SH_SALT_LEN) && at[1 + SH_SALT_LEN] == '"') {
                    memcpy(salt, at + 1, SH_SALT_LEN);
                    salt[SH_SALT_LEN] = 0;
                    again = 1;
                }
            }
            free(text);
        }
    }
    if (again ? sh_ms_assign_salt_as(stage_u8, from, salt, cause, cause_cap) < 0
              : sh_ms_assign_salt(stage_u8, from, salt, cause, cause_cap) < 0)
        return 0;
    sh_mb_keep_replacements_here(1);
    {
        /* The game's files decide what a file outside the mod's namespace replaces. */
        static const sh_mb_resolver game = { NULL, NULL, mg_stock, NULL };
        ok = sh_ms_install_as(stage_u8, target, &game, key, key_cap, st, b, cause, cause_cap);
    }
    sh_mb_keep_replacements_here(0);
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
    char ptr[128];
    /* Files byte-identical to installed game content became stock pointers:
     * the bundle names them and the store keeps (and uploads) nothing. */
    _snprintf_s(ptr, sizeof ptr, _TRUNCATE, ", %u of them unchanged game file(s) read from your game (%llu KiB)",
                it->pointers, it->pointer_bytes >> 10);
    mg_display(it->name, shown, sizeof shown);
    mg_display(it->store, store, sizeof store);
    if (it->duplicate_of[0]) {
        char first[MG_NAME_MAX];
        mg_display(it->duplicate_of, first, sizeof first);
        _snprintf_s(dup, sizeof dup, _TRUNCATE, "; duplicate id %s (also " MG_OVERRIDES "\\%s), both kept", it->id, first);
    }
    if (it->left_out) _snprintf_s(left, sizeof left, _TRUNCATE, "; %u file(s) a mod cannot carry left out (%u of them .bswf movies), first: %s",
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
        mg_log("MODS: migration: " MG_OVERRIDES "\\%s: moved into mods\\%s, %u file(s)%s, %llu KiB, %u warning(s)%s%s",
               shown, store, it->bundle_files, ptr, it->bytes >> 10, it->warnings, left, dup);
        break;
    case SH_MODMIGRATE_CONVERTED:
        mg_log("MODS: migration: " MG_OVERRIDES "\\%s: a package a map delivered, moved into mods\\%s, %u file(s)%s, "
               "%llu KiB%s%s", shown, store, it->bundle_files, ptr, it->bytes >> 10, left, dup);
        break;
    case SH_MODMIGRATE_REUSED:
        mg_log("MODS: migration: " MG_OVERRIDES "\\%s: a package a map delivered, already in mods\\%s%s", shown, store, dup);
        break;
    case SH_MODMIGRATE_REFUSED:
        mg_log("MODS: migration: " MG_OVERRIDES "\\%s: refused -- %s", shown, it->reason);
        break;
    case SH_MODMIGRATE_SKIPPED:
        mg_log("MODS: migration: " MG_OVERRIDES "\\%s: skipped -- %s", shown, it->reason);
        break;
    default:
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
            mg_cause(it->reason, sizeof it->reason, "it would hold at least %u file(s) and %llu MiB, more than one mod can "
                     "hold (%u files, %llu MiB); split it into smaller mods", carried_files, carried >> 20,
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
        g_mg_pointers = 0;
        g_mg_pointer_bytes = 0;
        ok = sh_modlegacy_convert_folder(&zip, it->tree, &origin, &v);
        it->pointers = g_mg_pointers;
        it->pointer_bytes = g_mg_pointer_bytes;
        it->ms_stage = GetTickCount64() - t1;
        if (!ok) {
            it->outcome = SH_MODMIGRATE_REFUSED;
            strncpy_s(it->reason, sizeof it->reason, v.reason[0] ? v.reason : "the conversion failed", _TRUNCATE);
            goto out;
        }
        it->outcome = v.files ? SH_MODMIGRATE_CONVERTED : SH_MODMIGRATE_REUSED;
        strncpy_s(it->id, sizeof it->id, v.id, _TRUNCATE);
        strncpy_s(it->bundle, sizeof it->bundle, v.build, _TRUNCATE);
        it->bundle_files = v.files;
        it->left_out = v.skipped;
        it->movies = v.movies;
        {
            const char *leaf = strrchr(v.folder, '\\');
            strncpy_s(it->store, sizeof it->store, leaf ? leaf + 1 : v.folder, _TRUNCATE);
        }
    } else {
        char id[SH_MB_ID_MAX + 1], staging[MAX_PATH * 4], key[SH_MS_NAME_MAX + 1], target[SH_MS_NAME_MAX + 1];
        char cause[SH_MB_CAUSE_MAX] = "";
        wchar_t *wstage, *wparent;
        sh_ms_install_stats st;
        sh_mb_bundle b;
        char *stage_u8;
        int installed;
        mg_identity(&zip, it->name, id, sizeof id);
        if (!mg_target_name(c, it, target, sizeof target)) {
            it->outcome = SH_MODMIGRATE_REFUSED;
            mg_cause(it->reason, sizeof it->reason, "every folder name it could take in the mods folder belongs to another mod "
                     "(the last is %s); rename that folder, then close DOOM and run: snapmap-plus migrate", target);
            goto out;
        }
        origin.source = MG_OVERRIDES;
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
        installed = stage_u8 && mg_install_staged(stage_u8, v.id[0] ? v.id : id, target, key, sizeof key, &st, &b, cause,
                                                  sizeof cause);
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
        it->pointers = st.stock_files;
        it->pointer_bytes = st.stock_bytes;
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
        s->pointers += it->pointers;
        s->pointer_bytes += it->pointer_bytes;
        (void)k;
    }
}

static int mg_write_record(mg_ctx *c, int complete)
{
    mg_buf b = {0};
    sh_modmigrate_stats s;
    size_t i;
    int first = 1, ok;
    char mods[MAX_PATH * 4];
    FILETIME ft;
    unsigned long long now;
    mg_stats(c, &s);
    GetSystemTimeAsFileTime(&ft);
    now = ((((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime) - 116444736000000000ull) / 10000000ull;
    /* A completed pass keeps what idempotence and the store tool's check need:
     * complete, migratedAt, source, the rows of folders not done (skipped or
     * refused) and the trees (which resolve maps published before Contract 5).
     * A pass in progress writes everything. */
    mg_puts(&b, complete ? "{\n  \"complete\": true" : "{\n  \"format\": 1,\n  \"complete\": false");
    mg_putf(&b, ",\n  \"migratedAt\": %llu,\n  \"source\": ", now);
    mg_putq(&b, c->overrides_u8);
    if (!complete)
        mg_putf(&b, ",\n  \"entries\": %u,\n  \"built\": %u,\n  \"converted\": %u,\n  \"reused\": %u,\n  \"skipped\": %u,"
                    "\n  \"refused\": %u,\n  \"duplicates\": %u,\n  \"pending\": %u,\n  \"ms\": %llu,"
                    "\n  \"overrideBackupsRemoved\": %s",
                s.entries, s.built, s.converted, s.reused, s.skipped, s.refused, s.duplicates, s.pending, s.ms,
                c->backups_removed ? "true" : "false");
    mg_puts(&b, ",\n  \"folders\": [");
    for (i = 0; i < c->count; i++) {
        const mg_item *it = &c->items[i];
        if (complete && (it->outcome == SH_MODMIGRATE_BUILT || it->outcome == SH_MODMIGRATE_CONVERTED ||
                         it->outcome == SH_MODMIGRATE_REUSED)) continue;
        mg_puts(&b, first ? "\n    {" : ",\n    {");
        first = 0;
        mg_puts(&b, "\"folder\": "); mg_putq(&b, it->name);
        mg_puts(&b, ", \"kind\": "); mg_putq(&b, mg_kind_name(it->kind));
        mg_puts(&b, ", \"outcome\": "); mg_putq(&b, mg_outcome_name(it->outcome));
        if (it->files || it->bytes) mg_putf(&b, ", \"files\": %u, \"bytes\": %llu", it->files, it->bytes);
        if (it->id[0]) { mg_puts(&b, ", \"id\": "); mg_putq(&b, it->id); }
        if (it->store[0]) { mg_puts(&b, ", \"store\": "); mg_putq(&b, it->store); }
        if (it->bundle[0]) { mg_puts(&b, ", \"bundle\": "); mg_putq(&b, it->bundle); }
        if (it->tree[0]) { mg_puts(&b, ", \"tree\": "); mg_putq(&b, it->tree); }
        if (it->bundle_files) mg_putf(&b, ", \"bundleFiles\": %u", it->bundle_files);
        if (it->pointers) mg_putf(&b, ", \"stockPointers\": %u, \"stockBytes\": %llu", it->pointers, it->pointer_bytes);
        if (it->left_out) mg_putf(&b, ", \"leftOut\": %u, \"moviesLeftOut\": %u", it->left_out, it->movies);
        if (it->nested) mg_puts(&b, ", \"nested\": true");
        if (it->warnings) mg_putf(&b, ", \"warnings\": %u", it->warnings);
        if (it->duplicate_of[0]) { mg_puts(&b, ", \"duplicateOf\": "); mg_putq(&b, it->duplicate_of); }
        if (it->reason[0]) { mg_puts(&b, ", \"reason\": "); mg_putq(&b, it->reason); }
        if (it->ms) mg_putf(&b, ", \"ms\": %llu", it->ms);
        mg_puts(&b, "}");
    }
    mg_puts(&b, first ? "],\n  \"trees\": {" : "\n  ],\n  \"trees\": {");
    first = 1;
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
    ok = mg_mods_u8(c->mods_w, mods, sizeof mods) && sh_mb_record_put(mods, SH_MODMIGRATE_RECORD, b.p);
    free(b.p);
    return ok;
}

/* 1 when the record exists and says the pass completed. */
static int mg_record_complete(const mg_ctx *c, int *exists)
{
    char *text = mg_record_text(c->mods_w);
    int complete;
    *exists = text != NULL;
    complete = text && strstr(text, "\"complete\":true") != NULL;
    free(text);
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


static void mg_finish(mg_ctx *c)
{
    sh_modmigrate_stats s;
    int wrote = mg_write_record(c, 1);
    mg_stats(c, &s);
    mg_log("MODS: migration: %u entr%s in " MG_OVERRIDES ": %u built, %u delivered converted, %u delivered reused, %u skipped, "
           "%u refused, %u duplicate id(s); %llu MiB in %llu ms; %u file(s) became "
           "stock pointers, %llu KiB not stored; record %s; the " MG_OVERRIDES " folder is unchanged",
           s.entries, s.entries == 1 ? "y" : "ies", s.built, s.converted, s.reused, s.skipped, s.refused, s.duplicates,
           s.bytes >> 20, s.ms, s.pointers, s.pointer_bytes >> 10, wrote ? "mods\\snapmap-plus.json (migration)" : "NOT written");
}

int sh_modmigrate_run(int again, sh_modmigrate_stats *stats)
{
    mg_ctx *c;
    int why;
    size_t i;
    if (stats) memset(stats, 0, sizeof *stats);
    c = mg_prepare(again, &why);
    if (!c) {
        if (why == 0) mg_log("MODS: migration: nothing to do (there is no overrides folder, or everything in it was moved "
                             "already)");
        return why;
    }
    for (i = 0; i < c->count; i++) {
        if (c->items[i].outcome == SH_MODMIGRATE_PENDING) mg_do_item(c, &c->items[i], i);
        else mg_log_item(&c->items[i]);
    }
    mg_finish(c);
    if (stats) mg_stats(c, stats);
    mg_ctx_free(c);
    return 1;
}

/* --------------------------------------------------------------- pending ---- */

static int mg_outcome_done(const char *o, size_t n)
{
    return (n == 5 && !strncmp(o, "built", 5)) || (n == 9 && !strncmp(o, "converted", 9)) ||
           (n == 6 && !strncmp(o, "reused", 6));
}

/* What the record says about overrides\<name>: 0 not listed, 1 every listed
 * folder of it built, converted or reused, 2 listed with another outcome
 * (skipped or refused), 3 listed as still pending. A name covers its own row
 * and the rows of folders inside it (map-<hex>\<package>, a nested package). */
static int mg_record_status(const char *text, const char *name)
{
    mg_buf exact = {0}, prefix = {0};
    const char *p;
    int status = 0;
    mg_puts(&exact, "\"folder\":");
    mg_putq(&exact, name);
    mg_puts(&prefix, "\"folder\":");
    mg_putq(&prefix, name);
    if (exact.failed || prefix.failed || !text) { free(exact.p); free(prefix.p); return 0; }
    prefix.p[--prefix.n] = 0;          /* drop the closing quote ... */
    mg_puts(&prefix, "/");             /* ... so "name/child" matches */
    /* The record is canonical (sorted keys, no spaces): in each folder entry
     * "outcome" follows "folder". */
    for (p = text; (p = strstr(p, "\"folder\":")) != NULL; p++) {
        const char *end = strchr(p, '}'), *o;
        if (strncmp(p, exact.p, exact.n) && strncmp(p, prefix.p, prefix.n)) continue;
        o = strstr(p, "\"outcome\":\"");
        if (!o || (end && o > end)) continue;
        o += 11;
        {
            const char *q = strchr(o, '"');
            size_t n = q ? (size_t)(q - o) : 0;
            int s = mg_outcome_done(o, n) ? 1 : (n == 7 && !strncmp(o, "pending", 7)) ? 3 : 2;
            if (s > status) status = s;
        }
    }
    free(exact.p);
    free(prefix.p);
    return status;
}

static unsigned long long mg_unix_time(const FILETIME *ft)
{
    unsigned long long t = ((unsigned long long)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    return t > 116444736000000000ull ? (t - 116444736000000000ull) / 10000000ull : 0;
}

/* The newest write or creation time of the entry and, for a folder, of its descriptor:
 * adding, removing or editing either marks a refused or skipped folder as
 * changed since the record. */
static unsigned long long mg_changed_at(const wchar_t *path, const WIN32_FIND_DATAW *fd)
{
    unsigned long long at = mg_unix_time(&fd->ftLastWriteTime), made = mg_unix_time(&fd->ftCreationTime);
    static const wchar_t *const desc[] = { L"package.json", L"mod.json" };
    size_t i;
    if (made > at) at = made;   /* a copied entry keeps its source's write time, not its creation time */
    if (!(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return at;
    for (i = 0; i < 2; i++) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        wchar_t *f = mg_wjoin(path, desc[i]);
        if (f && GetFileAttributesExW(f, GetFileExInfoStandard, &a)) {
            unsigned long long t = mg_unix_time(&a.ftLastWriteTime);
            if (t > at) at = t;
        }
        free(f);
    }
    return at;
}

int sh_modmigrate_pending(unsigned *entries)
{
    char root[MAX_PATH * 4];
    wchar_t *overrides = NULL, *record = NULL, *pattern = NULL;
    char *text = NULL;
    unsigned long long migrated_at = 0;
    unsigned pending = 0, seen = 0;
    int complete = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (entries) *entries = 0;
    if (!mg_data_root(root, sizeof root)) return -1;
    overrides = mg_wide(root, MG_OVERRIDES);
    record = mg_wide(root, "mods");
    if (!overrides || !record) { free(overrides); free(record); return -1; }
    if (!mg_is_dir(overrides)) { free(overrides); free(record); return 0; }
    text = mg_record_text(record);
    if (text) {
        const char *m = strstr(text, "\"migratedAt\":");
        if (m) migrated_at = _strtoui64(m + 13, NULL, 10);
        complete = strstr(text, "\"complete\":true") != NULL;
    }
    pattern = mg_wjoin(overrides, L"*");
    h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char *name;
            wchar_t *path;
            int status;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            seen++;
            if (mg_loose_entry(overrides, &fd)) continue;   /* the loose step's (sh_modmigrate_loose_scan) */
            name = sh_mb_utf8(fd.cFileName);
            path = mg_wjoin(overrides, fd.cFileName);
            status = name ? mg_record_status(text, name) : 0;
            /* A completed record lists only the folders not done: one it does
             * not list was done, unless it was added or changed since. */
            if (status == 0 && complete && path && mg_changed_at(path, &fd) <= migrated_at) status = 1;
            if (status == 0 || status == 3 || (status == 2 && path && mg_changed_at(path, &fd) > migrated_at)) pending++;
            free(name);
            free(path);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(pattern);
    free(text);
    free(overrides);
    free(record);
    if (entries) *entries = seen;
    return (int)pending;
}

/* ------------------------------------------------------------ loose step ---- */

#define MG_LOOSE_RECORD "loose"            /* the section of mods\snapmap-plus.json */
#define MG_LOOSE_FOLDER "my-overrides"     /* the mod the loose layer and the strings document become */
#define MG_STRINGS      "strings"          /* retired: the strings document's folder */
#define MG_STRINGS_DOC  "strids.json"

typedef struct mg_loose {
    char root[MAX_PATH * 4];
    char mods_u8[MAX_PATH * 4];
    char overrides_u8[MAX_PATH * 4];
    wchar_t *overrides_w, *mods_w, *strings_w, *doc_w;
    unsigned long long migrated_at;    /* the loose record's, 0 without one */
    int recorded;                      /* the record says the step completed */
    unsigned entries, due;             /* loose entries of overrides\, and those added or changed since */
    int doc;                           /* the document: 1 read, 0 absent, -1 not a strings document */
    int doc_due;                       /* read, and added or changed since the step */
    char doc_why[200];
    sh_json_object pairs;              /* its keys */
} mg_loose;

static void mg_loose_close(mg_loose *l)
{
    free(l->overrides_w);
    free(l->mods_w);
    free(l->strings_w);
    free(l->doc_w);
    sh_json_object_free(&l->pairs);
    memset(l, 0, sizeof *l);
}

/* The decoded text of a JSON string, or NULL. */
static char *mg_decode(const char *raw)
{
    size_t n, got = 0;
    char *out;
    if (!raw || raw[0] != '"') return NULL;
    n = strlen(raw);
    out = (char *)malloc(n + 1);
    if (!out) return NULL;
    if (!sh_json_decode_string(raw, n, out, n + 1, &got) || strlen(out) != got) { free(out); return NULL; }
    return out;
}

/* strings\strids.json: a flat object of "id": "text" (no #str_ prefix), UTF-8
 * with or without a BOM, as strids.c read it. 1 read, 0 absent, -1 not such a
 * document (the reason in `why`). */
static int mg_doc_read(const wchar_t *path, sh_json_object *pairs, char *why, size_t cap)
{
    char *u, *t;
    const char *p;
    size_t n = 0, i;
    if (mg_attr(path) == INVALID_FILE_ATTRIBUTES) return 0;
    u = sh_mb_utf8(path);
    t = u ? (char *)sh_mb_read_file(u, 16u << 20, &n) : NULL;
    free(u);
    if (!t) { mg_cause(why, cap, "it cannot be read (or is over 16 MiB)"); return -1; }
    p = t;
    if (n >= 3 && !memcmp(p, "\xef\xbb\xbf", 3)) { p += 3; n -= 3; }
    if (!sh_json_parse_object(p, n, 1, pairs)) {
        free(t);
        mg_cause(why, cap, "it is not a JSON object of \"id\": \"text\" pairs");
        return -1;
    }
    free(t);
    for (i = 0; i < pairs->count; i++) {
        char *text = mg_decode(pairs->members[i].value_json);
        int ok = text && pairs->members[i].key_length && strlen(pairs->members[i].key) == pairs->members[i].key_length;
        free(text);
        if (!ok) {
            mg_cause(why, cap, "its entry \"%.80s\" is not a text string with a name", pairs->members[i].key);
            sh_json_object_free(pairs);
            return -1;
        }
    }
    return 1;
}

/* Resolve the folders, read the loose record and the document, and count the
 * loose entries that are due. 0 when the data folder cannot be resolved. */
static int mg_loose_open(mg_loose *l)
{
    char *rec = NULL;
    memset(l, 0, sizeof *l);
    if (!mg_data_root(l->root, sizeof l->root)) return 0;
    l->overrides_w = mg_wide(l->root, MG_OVERRIDES);
    l->mods_w = mg_wide(l->root, "mods");
    l->strings_w = mg_wide(l->root, MG_STRINGS);
    l->doc_w = mg_wide(l->root, MG_STRINGS "\\" MG_STRINGS_DOC);
    if (!l->overrides_w || !l->mods_w || !l->strings_w || !l->doc_w || !mg_mods_u8(l->mods_w, l->mods_u8, sizeof l->mods_u8))
        return 0;
    {
        char *u = sh_mb_utf8(l->overrides_w);
        if (!u) return 0;
        strncpy_s(l->overrides_u8, sizeof l->overrides_u8, strncmp(u, "\\\\?\\", 4) ? u : u + 4, _TRUNCATE);
        free(u);
    }
    if (sh_mb_record_get(l->mods_u8, MG_LOOSE_RECORD, &rec) == 1 && rec) {
        const char *m = strstr(rec, "\"migratedAt\":");
        if (m) l->migrated_at = _strtoui64(m + 13, NULL, 10);
        l->recorded = strstr(rec, "\"complete\":true") != NULL;
    }
    free(rec);
    if (mg_is_dir(l->overrides_w)) {
        WIN32_FIND_DATAW fd;
        wchar_t *pattern = mg_wjoin(l->overrides_w, L"*");
        HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
        free(pattern);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                wchar_t *path;
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !mg_loose_entry(l->overrides_w, &fd)) continue;
                l->entries++;
                path = mg_wjoin(l->overrides_w, fd.cFileName);
                if (!l->recorded || !path || mg_changed_at(path, &fd) > l->migrated_at) l->due++;
                free(path);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    l->doc = mg_doc_read(l->doc_w, &l->pairs, l->doc_why, sizeof l->doc_why);
    if (l->doc == 1) {
        WIN32_FILE_ATTRIBUTE_DATA a;
        unsigned long long at = 0;
        if (GetFileAttributesExW(l->doc_w, GetFileExInfoStandard, &a)) {
            unsigned long long made = mg_unix_time(&a.ftCreationTime);
            at = mg_unix_time(&a.ftLastWriteTime);
            if (made > at) at = made;
        }
        l->doc_due = !l->recorded || at > l->migrated_at;
    }
    return 1;
}

/* Entries of overrides\ not in mods\: a package entry the record does not
 * count as done (built, converted or reused, or not listed by a completed
 * record and unchanged since), or a loose entry the loose step has not moved.
 * `first` receives the first one's name. */
static unsigned mg_overrides_left(const mg_loose *l, char *first, size_t cap)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern;
    HANDLE h;
    char *text = mg_record_text(l->mods_w);
    unsigned long long migrated_at = 0;
    unsigned left = 0;
    int complete = text && strstr(text, "\"complete\":true") != NULL;
    if (first && cap) first[0] = 0;
    if (text) {
        const char *m = strstr(text, "\"migratedAt\":");
        if (m) migrated_at = _strtoui64(m + 13, NULL, 10);
    }
    pattern = mg_wjoin(l->overrides_w, L"*");
    h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t *path;
            char *name;
            int done;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            path = mg_wjoin(l->overrides_w, fd.cFileName);
            name = sh_mb_utf8(fd.cFileName);
            if (!path || !name) done = 0;
            else if (mg_loose_entry(l->overrides_w, &fd))
                done = l->recorded && mg_changed_at(path, &fd) <= l->migrated_at;
            else {
                int status = complete ? mg_record_status(text, name) : 0;
                if (status == 0 && complete && mg_changed_at(path, &fd) <= migrated_at) status = 1;
                done = status == 1;
            }
            if (!done && !left++ && first && cap) strncpy_s(first, cap, name ? name : "?", _TRUNCATE);
            free(path);
            free(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    } else if (mg_is_dir(l->overrides_w)) left = 1;   /* it cannot be listed: keep it */
    free(text);
    return left;
}

int sh_modmigrate_loose_scan(sh_modmigrate_loose_check *out)
{
    mg_loose l;
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (!mg_loose_open(&l)) { mg_loose_close(&l); return -1; }
    out->loose_entries = l.entries;
    out->loose = l.due;
    out->strings_due = l.doc == 1 && l.doc_due;
    out->strings = out->strings_due ? (unsigned)l.pairs.count : 0;
    out->strings_bad = l.doc < 0;
    if (out->strings_bad) strncpy_s(out->why, sizeof out->why, l.doc_why, _TRUNCATE);
    if (mg_is_dir(l.overrides_w)) {
        out->left = mg_overrides_left(&l, out->first_left, sizeof out->first_left);
        out->overrides_done = !out->left;
    }
    out->strings_done = mg_is_dir(l.strings_w) && (l.doc == 0 || (l.doc == 1 && !l.doc_due));
    mg_loose_close(&l);
    return (out->loose || out->strings_due || out->strings_bad) + out->overrides_done + out->strings_done;
}

/* One entry of the loose layer's tree, read where it lies. Takes `name` and `source`. */
static int mg_zip_push(sh_modlegacy_zip *z, size_t *cap, char *name, wchar_t *source, unsigned long long size)
{
    sh_modlegacy_zip_entry *e;
    if (!name || !source) { free(name); free(source); return 0; }
    if (z->count == *cap) {
        size_t next = *cap ? *cap * 2 : 64;
        sh_modlegacy_zip_entry *grown = (sh_modlegacy_zip_entry *)realloc(z->entries, next * sizeof *grown);
        if (!grown) { free(name); free(source); return 0; }
        z->entries = grown;
        *cap = next;
    }
    e = &z->entries[z->count++];
    memset(e, 0, sizeof *e);
    e->name = name;
    e->source = source;
    e->size = e->packed = size;
    z->files++;
    z->expanded += size;
    return 1;
}

static char *mg_strcat3(const char *a, const char *b, const char *c)
{
    size_t n = strlen(a) + strlen(b) + strlen(c) + 1;
    char *s = (char *)malloc(n);
    if (s) _snprintf_s(s, n, _TRUNCATE, "%s%s%s", a, b, c);
    return s;
}

static int mg_entry_cmp(const void *a, const void *b)
{
    const sh_modlegacy_zip_entry *x = (const sh_modlegacy_zip_entry *)a, *y = (const sh_modlegacy_zip_entry *)b;
    int c = _stricmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

/* Every loose entry as assets/<its path below overrides\>. 1, or 0 with why. */
static int mg_loose_gather(const mg_loose *l, sh_modlegacy_zip *z, size_t *zcap, char *why, size_t cap)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = mg_wjoin(l->overrides_w, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    int ok = 1;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) { mg_cause(why, cap, "the " MG_OVERRIDES " folder cannot be listed"); return 0; }
    do {
        char *name;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !mg_loose_entry(l->overrides_w, &fd)) continue;
        name = sh_mb_utf8(fd.cFileName);
        if (!name) { mg_cause(why, cap, "a name in the " MG_OVERRIDES " folder is not valid Unicode"); ok = 0; break; }
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            unsigned long long size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            ok = mg_zip_push(z, zcap, mg_strcat3("assets/", name, ""), mg_wjoin(l->overrides_w, fd.cFileName), size);
            if (!ok) mg_cause(why, cap, "out of memory");
        } else {
            sh_modlegacy_zip sub;
            char folder[MAX_PATH * 4], err[MG_CAUSE_MAX] = "";
            unsigned files = 0;
            unsigned long long bytes = 0;
            wchar_t *path = mg_wjoin(l->overrides_w, fd.cFileName);
            size_t i;
            if (path) mg_measure(path, &files, &bytes, 0);
            free(path);
            if (!files) { free(name); continue; }   /* an empty tree carries nothing */
            _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\%s", l->overrides_u8, name);
            memset(&sub, 0, sizeof sub);
            if (!sh_modlegacy_zip_from_folder(folder, SH_MODMIGRATE_MAX_FILES, SH_MODMIGRATE_MAX_BYTES, &sub, NULL, err, sizeof err)) {
                mg_cause(why, cap, MG_OVERRIDES "\\%s cannot be read: %s", name, err);
                ok = 0;
            }
            for (i = 0; ok && i < sub.count; i++) {
                sh_modlegacy_zip_entry *e = &sub.entries[i];
                char prefix[MAX_PATH * 4];
                if (e->directory) continue;
                _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "assets/%s/", name);
                ok = mg_zip_push(z, zcap, mg_strcat3(prefix, e->name, ""), e->source, e->size);
                e->source = NULL;
                if (!ok) mg_cause(why, cap, "out of memory");
            }
            sh_modlegacy_zip_free(&sub);
        }
        free(name);
        if (ok && (z->files > SH_MODMIGRATE_MAX_FILES || z->expanded > SH_MODMIGRATE_MAX_BYTES)) {
            mg_cause(why, cap, "the loose files of the " MG_OVERRIDES " folder are over the migration cap of %u files or "
                     "%llu MiB; move some of them into mod folders of their own first", SH_MODMIGRATE_MAX_FILES,
                     SH_MODMIGRATE_MAX_BYTES >> 20);
            ok = 0;
        }
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* The same bytes? Sizes, then SHA-256. */
static int mg_same_file(const wchar_t *a, const wchar_t *b)
{
    char *ua = sh_mb_utf8(a), *ub = sh_mb_utf8(b), ha[SH_MB_HASH_HEX + 1], hb[SH_MB_HASH_HEX + 1];
    unsigned long long sa = 0, sb = 0;
    int same = ua && ub && sh_mb_sha256_file(ua, &sa, ha) && sh_mb_sha256_file(ub, &sb, hb) && sa == sb && !strcmp(ha, hb);
    free(ua);
    free(ub);
    return same;
}

/* Copy every file of the staged assets\ tree that mods\my-overrides\ lacks; a
 * file it has with other bytes is kept and named. */
static void mg_merge_files(const wchar_t *from, const wchar_t *to, const char *rel, sh_modmigrate_loose_stats *st)
{
    WIN32_FIND_DATAW fd;
    wchar_t *pattern = mg_wjoin(from, L"*");
    HANDLE h = pattern ? FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, 0) : INVALID_HANDLE_VALUE;
    free(pattern);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t *src, *dst;
        char *leaf, sub[MAX_PATH * 4];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        src = mg_wjoin(from, fd.cFileName);
        dst = mg_wjoin(to, fd.cFileName);
        leaf = sh_mb_utf8(fd.cFileName);
        _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s/%s", rel, leaf ? leaf : "?");
        if (!src || !dst || !leaf) st->files_failed++;
        else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) mg_merge_files(src, dst, sub, st);
        else if (mg_attr(dst) != INVALID_FILE_ATTRIBUTES) {
            if (mg_same_file(src, dst)) st->files_same++;
            else {
                st->files_kept++;
                mg_log("MODS: migration: kept my-overrides' %s; the copy in " MG_OVERRIDES "\\ differs and was not taken", sub);
            }
        } else if (sh_ms_copy_file(src, dst)) st->files_added++;
        else {
            st->files_failed++;
            mg_log("MODS: migration: %s could not be copied into mods\\" MG_LOOSE_FOLDER "\\ (error %lu); it is tried again "
                   "at the next run", sub, GetLastError());
        }
        free(src);
        free(dst);
        free(leaf);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static int mg_write_whole(const wchar_t *path, const char *text, size_t n)
{
    size_t len = wcslen(path);
    wchar_t *tmp = (wchar_t *)malloc(sizeof(wchar_t) * (len + 24));
    HANDLE h;
    DWORD wrote = 0;
    int ok;
    if (!tmp) return 0;
    _snwprintf_s(tmp, len + 24, _TRUNCATE, L"%s.tmp-%lu", path, GetCurrentProcessId());
    h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    ok = h != INVALID_HANDLE_VALUE && WriteFile(h, text, (DWORD)n, &wrote, NULL) && wrote == (DWORD)n && FlushFileBuffers(h);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    ok = ok && MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) DeleteFileW(tmp);
    free(tmp);
    return ok;
}

/* Add the document's keys that mods\my-overrides\mod.json lacks to its author
 * block's strings.en; a key it has (in any case) with other text is kept and
 * named. The file is rewritten only when a key is added. */
static int mg_merge_strings(const wchar_t *modjson, const sh_json_object *pairs, sh_modmigrate_loose_stats *st)
{
    sh_json_object root = {0}, strings = {0}, en = {0};
    char *u = sh_mb_utf8(modjson), *text, *s = NULL, *e = NULL, *out = NULL;
    const char *p, *raw;
    char prefix[SH_SALT_LEN + 2] = "";
    size_t n = 0, i, k, outn = 0;
    unsigned added = 0;
    int ok = 0;
    text = u ? (char *)sh_mb_read_file(u, 64u << 20, &n) : NULL;
    free(u);
    p = text;
    if (p && n >= 3 && !memcmp(p, "\xef\xbb\xbf", 3)) { p += 3; n -= 3; }
    if (!p || !sh_json_parse_object(p, n, 64, &root)) {
        mg_log("MODS: migration: mods\\" MG_LOOSE_FOLDER "\\mod.json cannot be read as a JSON object; the strings of "
               MG_STRINGS "\\" MG_STRINGS_DOC " were not added (fix mod.json and run this again)");
        goto done;
    }
    raw = sh_json_object_get(&root, "strings");
    if (raw && (raw[0] != '{' || !sh_json_parse_object(raw, strlen(raw), 63, &strings))) goto bad;
    raw = sh_json_object_get(&strings, "en");
    if (raw && (raw[0] != '{' || !sh_json_parse_object(raw, strlen(raw), 62, &en))) goto bad;
    {
        /* A salted folder's keys are "<salt>_<key>" (a folder still in an
         * earlier shape is prefixed when the store tool converts it). */
        sh_json_object block = {0};
        const char *sv;
        raw = sh_json_object_get(&root, SH_MB_BLOCK_KEY);
        if (raw && raw[0] == '{' && sh_json_parse_object(raw, strlen(raw), 61, &block) &&
            (sv = sh_json_object_get(&block, "salt")) != NULL && sv[0] == '"' && sh_salt_valid(sv + 1, strcspn(sv + 1, "\"")) &&
            sv[1 + SH_SALT_LEN] == '"')
            _snprintf_s(prefix, sizeof prefix, _TRUNCATE, "%.8s_", sv + 1);
        sh_json_object_free(&block);
    }
    for (i = 0; i < pairs->count; i++) {
        const sh_json_member *m0 = &pairs->members[i];
        sh_json_member pm = *m0;
        const sh_json_member *m = &pm;
        char keyed[SH_SALT_LEN + 2 + 512];
        if (prefix[0] && _strnicmp(m0->key, prefix, SH_SALT_LEN + 1)) {
            _snprintf_s(keyed, sizeof keyed, _TRUNCATE, "%s%.*s", prefix, (int)m0->key_length, m0->key);
            pm.key = keyed;
            pm.key_length = strlen(keyed);
        }
        for (k = 0; k < en.count && _stricmp(en.members[k].key, m->key); k++) {}
        if (k < en.count) {
            char *have = mg_decode(en.members[k].value_json), *want = mg_decode(m->value_json);
            if (have && want && !strcmp(have, want)) st->strings_same++;
            else {
                st->strings_kept++;
                /* The text as JSON, so a line break in it stays on the line. */
                mg_log("MODS: migration: kept my-overrides' %s; " MG_STRINGS "\\" MG_STRINGS_DOC " said %.160s", m->key,
                       m->value_json);
            }
            free(have);
            free(want);
        } else if (sh_json_object_set_n(&en, m->key, m->key_length, m->value_json, 1)) added++;
        else goto oom;
    }
    if (added) {
        if (!(e = sh_json_serialize_object(&en, 4, NULL)) || !sh_json_object_set(&strings, "en", e, 62)) goto oom;
        if (!(s = sh_json_serialize_object(&strings, 2, NULL)) || !sh_json_object_set(&root, "strings", s, 63)) goto oom;
        if (!(out = sh_json_serialize_object(&root, 0, &outn))) goto oom;
        if (!mg_write_whole(modjson, out, outn)) {
            mg_log("MODS: migration: mods\\" MG_LOOSE_FOLDER "\\mod.json could not be written (error %lu); the strings of "
                   MG_STRINGS "\\" MG_STRINGS_DOC " were not added (close the program holding it and run this again)",
                   GetLastError());
            goto done;
        }
    }
    st->strings_added += added;
    ok = 1;
    goto done;
bad:
    mg_log("MODS: migration: mods\\" MG_LOOSE_FOLDER "\\mod.json has a \"strings\" value that is not an object of languages; "
           "the strings of " MG_STRINGS "\\" MG_STRINGS_DOC " were not added (fix mod.json and run this again)");
    goto done;
oom:
    mg_log("MODS: migration: out of memory adding the strings of " MG_STRINGS "\\" MG_STRINGS_DOC);
done:
    free(text);
    free(s);
    free(e);
    free(out);
    sh_json_object_free(&en);
    sh_json_object_free(&strings);
    sh_json_object_free(&root);
    return ok;
}

/* The package.json the converter reads for the loose layer (and the mod.json
 * of a folder with no loose files): the mod's name and, when the document is
 * due, its keys as strings.en. The author's part only: the salting prefixes
 * the keys (`<salt>_<key>`) and the build writes the block. */
static char *mg_loose_descriptor(const mg_loose *l, int with_strings, int block, size_t *n)
{
    sh_json_object d = {0}, strings = {0};
    char *en = NULL, *s = NULL, *out = NULL;
    (void)block;
    if (sh_json_object_set(&d, "name", "\"My overrides\"", 1) &&
        (!with_strings ||
         ((en = sh_json_serialize_object(&l->pairs, 4, NULL)) != NULL && sh_json_object_set(&strings, "en", en, 2) &&
          (s = sh_json_serialize_object(&strings, 2, NULL)) != NULL && sh_json_object_set(&d, "strings", s, 3))))
        out = sh_json_serialize_object(&d, 0, n);
    free(en);
    free(s);
    sh_json_object_free(&strings);
    sh_json_object_free(&d);
    return out;
}

static void mg_loose_record(const mg_loose *l)
{
    char text[256];
    FILETIME ft;
    unsigned long long now;
    GetSystemTimeAsFileTime(&ft);
    now = mg_unix_time(&ft);
    _snprintf_s(text, sizeof text, _TRUNCATE, "{\"complete\": true, \"migratedAt\": %llu, \"folder\": \"" MG_LOOSE_FOLDER "\"}",
                now);
    CreateDirectoryW(l->mods_w, NULL);
    if (!sh_mb_record_put(l->mods_u8, MG_LOOSE_RECORD, text))
        mg_log("MODS: migration: the record (mods\\snapmap-plus.json, loose) could not be written; the next run looks again");
}

int sh_modmigrate_loose_run(sh_modmigrate_loose_stats *st)
{
    mg_loose l;
    sh_modmigrate_loose_stats local;
    sh_modlegacy_zip z;
    sh_modlegacy_verdict v;
    sh_modlegacy_origin origin;
    size_t zcap = 0, dn = 0;
    char why[MG_CAUSE_MAX] = "", tree[SH_MB_HASH_HEX + 1] = "", staging[MAX_PATH * 4], descpath[MAX_PATH * 4];
    char *desc = NULL, *stage_u8 = NULL;
    wchar_t *wparent = NULL, *wstage = NULL, *wdesc = NULL, *target = NULL;
    int with_strings, loose_files = 0, result = -1;
    if (!st) st = &local;
    memset(st, 0, sizeof *st);
    memset(&z, 0, sizeof z);
    if (!mg_loose_open(&l)) {
        mg_log("MODS: migration: the data folder cannot be resolved");
        mg_loose_close(&l);
        return -1;
    }
    st->strings_bad = l.doc < 0;
    if (l.doc < 0)
        mg_log("MODS: migration: " MG_STRINGS "\\" MG_STRINGS_DOC " left as it is: %s; fix it or remove it and run this again",
               l.doc_why);
    with_strings = l.doc == 1 && l.doc_due;
    if (!l.due && !with_strings) { mg_loose_close(&l); return 0; }
    modstore_lock();
    if (l.due && !mg_loose_gather(&l, &z, &zcap, why, sizeof why)) {
        mg_log("MODS: migration: the loose files of " MG_OVERRIDES "\\ were not moved: %s", why);
        goto out;
    }
    st->entries = l.due ? l.entries : 0;
    if (!z.count && (!with_strings || !l.pairs.count)) {
        /* Empty folders and an empty document carry nothing. */
        mg_loose_record(&l);
        result = 1;
        goto out;
    }
    /* The descriptor, read where it lies like every other entry. */
    _snprintf_s(staging, sizeof staging, _TRUNCATE, "%s\\mods-migrate.tmp\\loose-%lu", l.root, GetCurrentProcessId());
    _snprintf_s(descpath, sizeof descpath, _TRUNCATE, "%s\\mods-migrate.tmp\\loose-%lu.json", l.root, GetCurrentProcessId());
    wparent = mg_wide(l.root, "mods-migrate.tmp");
    wstage = mg_wide(staging, NULL);
    wdesc = mg_wide(descpath, NULL);
    target = mg_wide(l.mods_u8, MG_LOOSE_FOLDER);
    if (!wparent || !wstage || !wdesc || !target) { mg_log("MODS: migration: out of memory"); goto out; }
    CreateDirectoryW(wparent, NULL);
    if (mg_attr(wstage) != INVALID_FILE_ATTRIBUTES) mg_rmtree(wstage);
    loose_files = z.count != 0;
    if (loose_files) {
        /* The loose files are converted as a package is, with the descriptor read where it lies. */
        desc = mg_loose_descriptor(&l, with_strings, 0, &dn);
        if (!desc || !mg_write_whole(wdesc, desc, dn) ||
            !mg_zip_push(&z, &zcap, mg_strcat3("package.json", "", ""), _wcsdup(wdesc), dn)) {
            mg_log("MODS: migration: the loose step cannot write its work file in %s\\mods-migrate.tmp (error %lu)", l.root,
                   GetLastError());
            goto out;
        }
        qsort(z.entries, z.count, sizeof *z.entries, mg_entry_cmp);
        if (!sh_modlegacy_zip_tree(&z, tree, why, sizeof why)) {
            mg_log("MODS: migration: the loose files of " MG_OVERRIDES "\\ were not moved: %s", why);
            goto out;
        }
        memset(&origin, 0, sizeof origin);
        origin.source = MG_OVERRIDES;
        origin.folder = "";
        origin.tree = tree;
        origin.fallback_id = MG_LOOSE_FOLDER;
        if (!sh_modlegacy_stage_folder(&z, &origin, staging, &v)) {
            mg_log("MODS: migration: the loose files of " MG_OVERRIDES "\\ were not moved: %s",
                   v.reason[0] ? v.reason : "they could not be staged");
            goto out;
        }
        st->left_out = v.skipped;
        if (v.skipped)
            mg_log("MODS: migration: %u loose file(s) a mod cannot carry were left out (%u of them .bswf movies), first: %s; "
                   "they go with " MG_OVERRIDES "\\ to the Recycle Bin", v.skipped, v.movies, v.first_skipped);
    } else if (!mg_is_dir(target)) {
        /* The document alone: a mod folder of mod.json and an empty assets\. */
        wchar_t *modjson = mg_wjoin(wstage, L"mod.json");
        desc = mg_loose_descriptor(&l, with_strings, 1, &dn);
        wchar_t *assets = mg_wjoin(wstage, L"assets");
        if (!modjson || !assets || !desc || !CreateDirectoryW(wstage, NULL) || !CreateDirectoryW(assets, NULL) ||
            !mg_write_whole(modjson, desc, dn)) {
            mg_log("MODS: migration: the loose step cannot write its work folder in %s\\mods-migrate.tmp (error %lu)", l.root,
                   GetLastError());
            free(modjson);
            free(assets);
            goto out;
        }
        free(modjson);
        free(assets);
    }
    {
        wchar_t *assets = mg_wjoin(wstage, L"assets");
        if (mg_is_dir(target)) {
            /* The player's own my-overrides: add what it lacks, keep what it has. */
            wchar_t *have = mg_wjoin(target, L"assets"), *modjson = mg_wjoin(target, L"mod.json");
            if (loose_files && assets && have) mg_merge_files(assets, have, "assets", st);
            if (with_strings && modjson && !mg_merge_strings(modjson, &l.pairs, st)) st->files_failed++;
            free(have);
            free(modjson);
            mg_log("MODS: migration: %s%s%s added to mods\\" MG_LOOSE_FOLDER "\\: %u file(s) added, %u kept, %u already there; "
                   "%u string(s) added, %u kept, %u already there; it is built again at the next start or rescan",
                   loose_files ? "the loose files of " MG_OVERRIDES "\\" : "", loose_files && with_strings ? " and " : "",
                   with_strings ? MG_STRINGS "\\" MG_STRINGS_DOC : "", st->files_added, st->files_kept, st->files_same,
                   st->strings_added, st->strings_kept, st->strings_same);
        } else {
            char key[SH_MS_NAME_MAX + 1], cause[SH_MB_CAUSE_MAX] = "";
            sh_ms_install_stats ist;
            sh_mb_bundle b;
            memset(&b, 0, sizeof b);
            memset(&ist, 0, sizeof ist);
            stage_u8 = sh_mb_utf8(wstage);
            if (!stage_u8 || !mg_install_staged(stage_u8, MG_LOOSE_FOLDER, MG_LOOSE_FOLDER, key, sizeof key, &ist, &b, cause,
                                                sizeof cause)) {
                mg_log("MODS: migration: mods\\" MG_LOOSE_FOLDER "\\ was not made: %s", cause[0] ? cause : "the store refused it");
                st->files_failed++;
            } else {
                st->created = 1;
                st->files_added = (unsigned)b.file_count;
                st->pointers = ist.stock_files;
                st->strings_added = with_strings ? (unsigned)l.pairs.count : 0;
                mg_log("MODS: migration: %s%s%s moved into mods\\%s\\ (the mod My overrides): %u file(s), %u of them unchanged "
                       "game files read from your game, %u string(s)",
                       loose_files ? "the loose files of " MG_OVERRIDES "\\" : "", loose_files && with_strings ? " and " : "",
                       with_strings ? MG_STRINGS "\\" MG_STRINGS_DOC : "", key, st->files_added, st->pointers,
                       st->strings_added);
            }
            sh_mb_free(&b);
        }
        free(assets);
    }
    if (!st->files_failed) {
        mg_loose_record(&l);
        result = 1;
    }
out:
    sh_modlegacy_zip_free(&z);
    if (wstage && mg_attr(wstage) != INVALID_FILE_ATTRIBUTES) mg_rmtree(wstage);
    if (wdesc) DeleteFileW(wdesc);
    if (wparent) RemoveDirectoryW(wparent);   /* only when nothing else is staged */
    free(wparent);
    free(wstage);
    free(wdesc);
    free(target);
    free(desc);
    free(stage_u8);
    modstore_unlock();
    mg_loose_close(&l);
    return result;
}

int sh_modmigrate_retire(void)
{
    sh_modmigrate_loose_check c;
    int retired = 0, r;
    if (sh_modmigrate_loose_scan(&c) < 0) return -1;
    if (c.overrides_done) {
        r = sh_ms_retire_sibling(L"" MG_OVERRIDES);
        if (r == 1) {
            retired++;
            mg_log("MODS: migration: " MG_OVERRIDES "\\ %s: every entry of it is in mods\\", sh_ms_discard_verb());
        } else if (r == 0)
            mg_log("MODS: migration: " MG_OVERRIDES "\\ could not be retired (error %lu); it is tried again at the next run",
                   GetLastError());
    } else if (c.left)
        mg_log("MODS: migration: " MG_OVERRIDES "\\ kept: %u entr%s not moved into mods\\ (first: %s)", c.left,
               c.left == 1 ? "y is" : "ies are", c.first_left);
    if (c.strings_done) {
        r = sh_ms_retire_sibling(L"" MG_STRINGS);
        if (r == 1) {
            retired++;
            mg_log("MODS: migration: " MG_STRINGS "\\ %s: its strings are in mods\\" MG_LOOSE_FOLDER "\\", sh_ms_discard_verb());
        } else if (r == 0)
            mg_log("MODS: migration: " MG_STRINGS "\\ could not be retired (error %lu); it is tried again at the next run",
                   GetLastError());
    }
    return retired;
}
