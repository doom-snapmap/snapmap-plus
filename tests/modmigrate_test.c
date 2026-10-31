/* modmigrate_test -- the retired overrides\ tree moved into the mods\ store
 * (modmigrate.c) over a small synthetic tree: an era C pack with a requirement
 * the store does not support, a strings block, a composes block and a file
 * type bundles cannot carry; an era B pack without an id; two packs with one
 * id; a delivered map-<16 hex>\<package>; a stray ZIP; a folder without a
 * descriptor holding one package one level down (migrated) and one holding
 * two (skipped); a folder over the size cap and
 * one over the bundle cap; an empty override-backups folder. Checks the store, the record, the log lines,
 * that overrides\ is byte-for-byte unchanged, that a second pass does nothing
 * and --again repeats it identically, that a map carrying the same delivery
 * (or the same authored tree) is answered by the migrated bundle without a
 * conversion, and the in-game pass on the worker (sh_mods migrate): the
 * record completes and the serving index is rebuilt. The Mods tab's Migrate button
 * follows sh_modmigrate_pending: entries the record does not list, and
 * skipped or refused ones changed since it was written.
 *
 * Links the real builder, store and legacy converter; the serving index and
 * the product's builder entry are the seam stub (mod_seam_stub.c), whose
 * builder refuses, so any conversion a memo should have answered fails. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/modmigrate.h"
#include "../src/backend/modlegacy.h"
#include "../src/backend/modstore.h"
#include "../src/backend/modbundle.h"

/* A folder big enough that the worker is still busy while the test installs. */
#define MG_TEST_LARGE ((16ull << 20) + 4096)
#include "../src/backend/map_shards.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

static char g_log[262144];
static int g_echo;
void backend_log(const char *line)
{
    size_t at = strlen(g_log), n = line ? strlen(line) : 0;
    if (g_echo && line) printf("    %s\n", line);
    if (line && at + n + 2 < sizeof g_log) { memcpy(g_log + at, line, n); g_log[at + n] = '\n'; g_log[at + n + 1] = 0; }
}
extern int g_mod_stub_rescans;

/* ---- files ------------------------------------------------------------------------------------ */

static wchar_t *wide_of(const char *path)
{
    return sh_mb_wpath(path);
}

static void mkdirs_for(const char *file)
{
    char dir[MAX_PATH * 2];
    size_t i;
    strncpy_s(dir, sizeof dir, file, _TRUNCATE);
    for (i = 3; dir[i]; i++) {
        if (dir[i] == '\\' || dir[i] == '/') {
            char keep = dir[i];
            dir[i] = 0;
            CreateDirectoryA(dir, NULL);
            dir[i] = keep;
        }
    }
}

static void put_file(const char *root, const char *rel, const char *text)
{
    char path[MAX_PATH * 2];
    FILE *f = NULL;
    size_t i;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", root, rel);
    for (i = 0; path[i]; i++) if (path[i] == '/') path[i] = '\\';
    mkdirs_for(path);
    if (fopen_s(&f, path, "wb") || !f) { fprintf(stderr, "cannot write %s\n", path); return; }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

/* A file of `bytes` zeros, sparse so it costs no disk. */
static void put_sparse(const char *root, const char *rel, unsigned long long bytes)
{
    char path[MAX_PATH * 2];
    HANDLE h;
    DWORD got = 0;
    LARGE_INTEGER at;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", root, rel);
    mkdirs_for(path);
    h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DeviceIoControl(h, FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &got, NULL);
    at.QuadPart = (LONGLONG)bytes;
    SetFilePointerEx(h, at, NULL, FILE_BEGIN);
    SetEndOfFile(h);
    CloseHandle(h);
}

/* A real file of `bytes` pseudo-random bytes (so a bundle hashes something). */
static void put_bytes(const char *root, const char *rel, size_t bytes, unsigned seed)
{
    char path[MAX_PATH * 2];
    FILE *f = NULL;
    unsigned char *buf = (unsigned char *)malloc(bytes);
    size_t i;
    if (!buf) return;
    for (i = 0; i < bytes; i++) { seed = seed * 1103515245u + 12345u; buf[i] = (unsigned char)(seed >> 16); }
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", root, rel);
    for (i = 0; path[i]; i++) if (path[i] == '/') path[i] = '\\';
    mkdirs_for(path);
    if (!fopen_s(&f, path, "wb") && f) { fwrite(buf, 1, bytes, f); fclose(f); }
    free(buf);
}

static char *read_all(const char *path, size_t *len)
{
    wchar_t *w = wide_of(path);
    HANDLE h = w ? CreateFileW(w, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL) : INVALID_HANDLE_VALUE;
    LARGE_INTEGER size;
    char *p = NULL;
    DWORD got = 0;
    free(w);
    if (len) *len = 0;
    if (h == INVALID_HANDLE_VALUE) return NULL;
    if (GetFileSizeEx(h, &size) && size.QuadPart < (64 << 20)) {
        p = (char *)malloc((size_t)size.QuadPart + 1);
        if (p && ReadFile(h, p, (DWORD)size.QuadPart, &got, NULL)) { p[got] = 0; if (len) *len = got; }
        else { free(p); p = NULL; }
    }
    CloseHandle(h);
    return p;
}

static int exists(const char *path)
{
    wchar_t *w = wide_of(path);
    DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a != INVALID_FILE_ATTRIBUTES;
}

static void remove_tree_w(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    size_t n = wcslen(dir);
    wchar_t *pattern = (wchar_t *)malloc((n + 3) * sizeof(wchar_t));
    HANDLE h;
    if (!pattern) return;
    memcpy(pattern, dir, n * sizeof(wchar_t));
    memcpy(pattern + n, L"\\*", 3 * sizeof(wchar_t));
    h = FindFirstFileW(pattern, &fd);
    free(pattern);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            size_t k;
            wchar_t *child;
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            k = wcslen(fd.cFileName);
            child = (wchar_t *)malloc((n + k + 2) * sizeof(wchar_t));
            if (!child) continue;
            memcpy(child, dir, n * sizeof(wchar_t));
            child[n] = L'\\';
            memcpy(child + n + 1, fd.cFileName, (k + 1) * sizeof(wchar_t));
            SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                remove_tree_w(child);
            else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(child);
            else DeleteFileW(child);
            free(child);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static void remove_tree(const char *dir)
{
    wchar_t *w = wide_of(dir);
    if (w) { remove_tree_w(w); free(w); }
}

/* Every path under `dir` with its size, write time and (for files up to 4 MiB)
 * SHA-256, sorted: the before/after witness that nothing changed. */
typedef struct snap { char *text; size_t n, cap; } snap;
static void snap_put(snap *s, const char *t)
{
    size_t k = strlen(t);
    if (s->n + k + 1 > s->cap) { s->cap = (s->cap + k + 1) * 2; s->text = (char *)realloc(s->text, s->cap); }
    memcpy(s->text + s->n, t, k + 1);
    s->n += k;
}
static void snapshot_dir(const char *dir, const char *rel, snap *s)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2], child[MAX_PATH * 2], crel[MAX_PATH * 2], line[MAX_PATH * 3];
    HANDLE h;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        _snprintf_s(child, sizeof child, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        _snprintf_s(crel, sizeof crel, _TRUNCATE, "%s/%s", rel, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* No folder times: NTFS updates a folder's entry in its parent lazily. */
            _snprintf_s(line, sizeof line, _TRUNCATE, "d %s %08lx\n", crel, fd.dwFileAttributes);
            snap_put(s, line);
            snapshot_dir(child, crel, s);
        } else {
            char hex[SH_MB_HASH_HEX + 1] = "-";
            unsigned long long size = 0;
            if (fd.nFileSizeHigh == 0 && fd.nFileSizeLow < (4u << 20)) sh_mb_sha256_file(child, &size, hex);
            _snprintf_s(line, sizeof line, _TRUNCATE, "f %s %lu %08lx%08lx %08lx %s\n", crel, fd.nFileSizeLow,
                        fd.ftLastWriteTime.dwHighDateTime, fd.ftLastWriteTime.dwLowDateTime, fd.dwFileAttributes, hex);
            snap_put(s, line);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/* ---- the synthetic tree ------------------------------------------------------------------------ */

static const char ERA_C_DESC[] =
    "{\r\n  \"id\": \"Alex.Door\",\r\n  \"name\": \"Door pack\",\r\n"
    "  \"requirements\": {\"cvars\": {\"g_useResourceBlackList\": 0, \"sv_cheats\": \"1\"}},\r\n"
    "  \"strings\": {\"en\": {\"#str_door\": \"Door\"}},\r\n"
    "  \"composes\": {\"hands\": [\"alex/door\"]}\r\n}";
static const char ERA_B_DESC[] =
    "{\"schema\": \"snapmap-plus.override-package.v1\", \"name\": \"Old pack\", \"version\": \"1.0.0\", "
    "\"description\": \"an era B pack\"}";
static const char DECL_A[] = "{\r\n\tinherit = \"monster/imp\";\r\n}";
static const char DECL_B[] = "{\r\n\tinherit = \"monster/imp\";\r\n\tedit = {\r\n\t\thealth = 50;\r\n\t}\r\n}";

static void make_tree(const char *ov)
{
    put_file(ov, "pack-c/package.json", ERA_C_DESC);
    put_file(ov, "pack-c/assets/generated/decls/entitydef/alex/door.decl", DECL_A);
    put_file(ov, "pack-c/assets/cooked/model/models/door.materials.txt", "textures/door\n");
    put_file(ov, "Old Pack/package.json", ERA_B_DESC);
    put_file(ov, "Old Pack/decls/entitydef/old/thing.decl", DECL_A);
    put_file(ov, "Old Pack/readme.txt", "not an input");
    put_file(ov, "dupe-a/package.json", "{\"id\": \"local.dupe\", \"name\": \"Dupe\"}");
    put_file(ov, "dupe-a/assets/generated/decls/entitydef/dupe/one.decl", DECL_A);
    put_file(ov, "dupe-b/package.json", "{\"id\": \"local.dupe\", \"name\": \"Dupe boss\"}");
    put_file(ov, "dupe-b/assets/generated/decls/entitydef/dupe/one.decl", DECL_B);
    put_file(ov, "map-0123456789abcdef/doors/package.json", "{\"id\": \"doors\", \"name\": \"SP doors\"}");
    put_file(ov, "map-0123456789abcdef/doors/assets/generated/decls/entitydef/doors/d.decl", DECL_A);
    put_file(ov, "stray.zip", "PK\x05\x06 not really");
    put_file(ov, "nodesc/inner/package.json", "{\"id\": \"inner\", \"name\": \"Inner\"}");
    put_file(ov, "nodesc/inner/assets/generated/decls/entitydef/inner/i.decl", DECL_A);
    put_file(ov, "twodesc/one/package.json", "{\"id\": \"two.one\", \"name\": \"One\"}");
    put_file(ov, "twodesc/one/assets/generated/decls/entitydef/two/one.decl", DECL_A);
    put_file(ov, "twodesc/two/package.json", "{\"id\": \"two.two\", \"name\": \"Two\"}");
    put_file(ov, "twodesc/two/assets/generated/decls/entitydef/two/two.decl", DECL_A);
    put_file(ov, "huge/package.json", "{\"id\": \"huge\", \"name\": \"Huge\"}");
    put_sparse(ov, "huge\\assets\\generated\\image\\huge\\sky.bimage", (600ull << 20));
    put_file(ov, "big/package.json", "{\"id\": \"big\", \"name\": \"Big\"}");
    put_sparse(ov, "big\\assets\\generated\\image\\big\\sky.bimage", (300ull << 20));
}

/* A store folder's mod.json. */
static int bundle_of(const char *store, const char *folder, sh_mb_bundle *b)
{
    char path[MAX_PATH * 2];
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s\\mod.json", store, folder);
    memset(b, 0, sizeof *b);
    return sh_mb_load(path, b);
}

/* The migration record: the "migration" section of mods\snapmap-plus.json
 * (canonical JSON: sorted keys, no spaces). */
static char *record_of(const char *mods)
{
    char *text = NULL;
    if (sh_mb_record_get(mods, "migration", &text) != 1) { free(text); return NULL; }
    return text;
}

/* The record's entry for overrides\<folder> holds every needle. */
static int entry_has(const char *record, const char *folder, const char *a, const char *b)
{
    char key[MAX_PATH];
    const char *at, *start, *end;
    _snprintf_s(key, sizeof key, _TRUNCATE, "\"folder\":\"%s\"", folder);
    at = record ? strstr(record, key) : NULL;
    if (!at) return 0;
    for (start = at; start > record && *start != '{'; start--) {}
    end = strchr(at, '}');
    if (!end) return 0;
    {
        size_t n = (size_t)(end - start);
        char *one = (char *)malloc(n + 1);
        int ok;
        if (!one) return 0;
        memcpy(one, start, n);
        one[n] = 0;
        ok = (!a || strstr(one, a)) && (!b || strstr(one, b));
        free(one);
        return ok;
    }
}

static int has_file(const sh_mb_bundle *b, const char *request)
{
    size_t i;
    for (i = 0; i < b->file_count; i++) if (!strcmp(b->files[i].request, request)) return 1;
    return 0;
}

/* The tree fingerprint of a folder, as the converter computes it. */
static int tree_of(const char *folder, char tree[65])
{
    sh_modlegacy_zip zip;
    char err[256];
    int ok = sh_modlegacy_zip_from_folder(folder, 8192, 1ull << 30, &zip, tree, err, sizeof err);
    sh_modlegacy_zip_free(&zip);
    return ok;
}

/* A map with one smpkg carrier of a one-member stored ZIP under `key`. */
static char *carrier_map(const char *key, size_t *out_n)
{
    static const char BASE[] =
        "{\"entities\":[{\"entityDef\":{\"className\":\"idSnapEntity\",\"inherit\":\"doors/d\",\"name\":\"\"}}],"
        "\"variables\":{\"allocCount\":[0,0,0,0,1],\"string\":[{\"info\":{\"customIcon\":{\"targetType\":"
        "\"idDeclSnapCustomIcon\",\"value\":null,\"~type\":\"|pointer\"},\"name\":\"door_label\",\"~type\":"
        "\"snapVarInfo_t\"},\"initialValue\":\"Open\",\"~type\":\"snapVarString_t\"}]}}";
    static const unsigned char zip[] = {
        'P','K',3,4, 20,0, 0,0, 0,0, 0,0, 0,0, 0,0,0,0, 2,0,0,0, 2,0,0,0, 1,0, 0,0, 'a', '{','}',
        'P','K',1,2, 20,0, 20,0, 0,0, 0,0, 0,0, 0,0, 0,0,0,0, 2,0,0,0, 2,0,0,0, 1,0, 0,0, 0,0, 0,0, 0,0,
        0,0,0,0, 0,0,0,0, 'a',
        'P','K',5,6, 0,0, 0,0, 1,0, 1,0, 47,0,0,0, 33,0,0,0, 0,0 };
    char b64[512], header[256], digest[17], err[160];
    sh_shard_out part;
    sh_shard_b64_encode(zip, sizeof zip, b64);
    sh_shard_digest16(zip, sizeof zip, digest);
    _snprintf_s(header, sizeof header, _TRUNCATE, "smpkg.%s.0.1.%s", key, digest);
    part.header = header;
    part.chunk = b64;
    part.chunk_len = ((sizeof zip + 2) / 3) * 4;
    return sh_shard_insert(BASE, sizeof BASE - 1, &part, 1, out_n, err, sizeof err);
}

/* ---- tests ------------------------------------------------------------------------------------- */

static void configure(const char *root)
{
    char mods[MAX_PATH];
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", root);
    sh_modmigrate_set_data_root(root);
    sh_modlegacy_set_data_root(root);
    sh_ms_set_root(mods);
    sh_modlegacy_reset();
}

static void full_pass(const char *root)
{
    char ov[MAX_PATH], mods[MAX_PATH], path[MAX_PATH * 2], tree_c[65], tree_d[65], key[160];
    snap before = {0}, after = {0};
    sh_modmigrate_stats st;
    sh_mb_bundle b;
    char *record;
    sh_ms_list list;
    size_t i, n = 0;
    char dupe_b_key[SH_MS_KEY_MAX + 1] = "dupe-b";
    char *hash_c = NULL;

    printf("-- the synthetic overrides tree migrates into the store\n");
    _snprintf_s(ov, sizeof ov, _TRUNCATE, "%s\\overrides", root);
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", root);
    CreateDirectoryA(root, NULL);
    make_tree(ov);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\override-backups", root);
    CreateDirectoryA(path, NULL);
    configure(root);
    snapshot_dir(ov, "", &before);

    g_log[0] = 0;
    CHECK(sh_modmigrate_run(0, &st) == 1);
    printf("  %u entries: %u built, %u converted, %u reused, %u skipped, %u refused, %u duplicate(s), %llu ms\n",
           st.entries, st.built, st.converted, st.reused, st.skipped, st.refused, st.duplicates, st.ms);
    CHECK(st.entries == 10 && st.built == 5 && st.converted == 1 && st.reused == 0 && st.skipped == 3 &&
          st.refused == 1 && st.duplicates == 1 && st.pending == 0);
    CHECK(st.backups_removed == 1 && !exists(path));

    /* overrides\ is exactly as it was. */
    snapshot_dir(ov, "", &after);
    CHECK(before.text && after.text && !strcmp(before.text, after.text));
    if (before.text && after.text && strcmp(before.text, after.text))
        fprintf(stderr, "before:\n%s\nafter:\n%s\n", before.text, after.text);

    /* The era C pack: in mods\<its folder>, requirements filtered, strings and
     * composes kept, the sidecar left out, package.json now its mod.json. */
    CHECK(bundle_of(mods, "pack-c", &b) == 1);
    CHECK(!strcmp(b.origin, "migration") && !b.delivered);
    CHECK(b.legacy && b.requirements && !strcmp(b.requirements, "{\"cvars\":{\"g_useResourceBlackList\":\"0\"}}"));
    CHECK(b.extra && strstr(b.extra, "\"composes\":{\"hands\":[\"alex/door\"]}") && strstr(b.extra, "\"strings\"") &&
          strstr(b.extra, "\"source\":\"overrides\"") && strstr(b.extra, "\"folder\":\"pack-c\""));
    CHECK(b.file_count == 1 && has_file(&b, "generated/decls/entitydef/alex/door.decl"));
    hash_c = _strdup(b.hash);
    sh_mb_free(&b);
    /* The era B pack: named by its folder, its version kept, its layout migrated. */
    CHECK(bundle_of(mods, "Old Pack", &b) == 1 && !strcmp(b.id, "old-pack") && !strcmp(b.version, "1.0.0"));
    CHECK(b.file_count == 1 && has_file(&b, "generated/decls/entitydef/old/thing.decl") &&
          b.extra && strstr(b.extra, "\"schema\":\"snapmap-plus.override-package.v1\"") && strstr(b.extra, "\"layout\":\"B\""));
    sh_mb_free(&b);
    /* The duplicate pair: both built, each in its own folder, one id. */
    CHECK(bundle_of(mods, "dupe-a", &b) == 1 && !strcmp(b.id, "local.dupe"));
    sh_mb_free(&b);
    CHECK(bundle_of(mods, dupe_b_key, &b) == 1 && !strcmp(b.id, "local.dupe") && !strcmp(b.version, "0.0.0"));
    sh_mb_free(&b);
    memset(&list, 0, sizeof list);
    CHECK(sh_ms_enumerate(&list) == 1);
    /* The delivered package: an ordinary folder named by its id, keyed by its tree. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\map-0123456789abcdef\\doors", ov);
    CHECK(tree_of(path, tree_d) == 1);
    CHECK(bundle_of(mods, "doors", &b) == 1);
    CHECK(b.legacy && b.delivered && b.extra && strstr(b.extra, "\"source\":\"delivered\"") && strstr(b.extra, tree_d));
    CHECK(!strcmp(b.origin, "delivery"));
    sh_mb_free(&b);
    sh_ms_list_free(&list);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\legacy", mods);
    CHECK(!exists(path));
    /* Nothing was staged inside the store, and the staging area is gone. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods-migrate.tmp", root);
    CHECK(!exists(path));

    /* The record (a section of mods\snapmap-plus.json, no .migrated.json) and the log. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.migrated.json", mods);
    CHECK(!exists(path));
    record = record_of(mods);
    (void)n;
    /* Complete: only the folders not done keep a row; the trees stay. */
    CHECK(record && strstr(record, "\"complete\":true") && !strstr(record, "\"duplicateOf\"") &&
          !strstr(record, "\"outcome\":\"built\"") && !strstr(record, "\"entries\""));
    CHECK(entry_has(record, "stray.zip", "\"kind\":\"zip\"", "\"outcome\":\"skipped\""));
    CHECK(entry_has(record, "huge", "\"kind\":\"package\"", "\"outcome\":\"skipped\""));
    CHECK(record && strstr(record, "it holds 2 package folders one level down (first: twodesc\\\\"));
    CHECK(record && !strstr(record, "\"nodesc/inner\"") && strstr(record, "\":\"pack-c\""));   /* pack-c's tree */
    CHECK(entry_has(record, "big", "\"outcome\":\"refused\"", NULL));
    CHECK(bundle_of(mods, "inner", &b) == 1);
    sh_mb_free(&b);
    CHECK(strstr(g_log, "MODS: migration: overrides\\nodesc\\inner: built mods\\inner,") != NULL &&
          strstr(g_log, "the one package folder inside overrides\\nodesc, which has no descriptor of its own, "
                        "migrated under its own id") != NULL);
    CHECK(record && strstr(record, tree_d));
    CHECK(strstr(g_log, "MODS: migration: overrides\\stray.zip: skipped -- a ZIP archive") != NULL);
    CHECK(strstr(g_log, "MODS: migration: overrides\\huge: skipped -- over the migration cap") != NULL);
    CHECK(strstr(g_log, "MODS: migration: overrides\\big: refused -- a bundle of it would hold at least 1 file(s), 300 MiB, "
                        "over the bundle caps") != NULL);
    CHECK(strstr(g_log, "MODS: migration: overrides\\dupe-b: built mods\\dupe-b,") != NULL &&
          strstr(g_log, "duplicate id local.dupe (also overrides\\dupe-a), both kept") != NULL);
    CHECK(strstr(g_log, "MODS: migration: overrides\\map-0123456789abcdef\\doors: delivered package converted to "
                        "mods\\doors,") != NULL);
    CHECK(strstr(g_log, "MODS: migration: overrides\\pack-c: built mods\\pack-c,") != NULL &&
          strstr(g_log, "1 file(s) a bundle cannot carry left out (0 of them .bswf movies), first: assets/cooked/model/models/door.materials.txt") != NULL);
    CHECK(strstr(g_log, "MODS: migration: 10 entries in overrides: 5 built, 1 delivered converted") != NULL);
    free(record);

    /* A map carrying the same delivery is answered by the migrated folder: no
     * conversion (the stub builder would refuse one). */
    {
        size_t mn = 0, on = 0;
        char *map, *out;
        sh_modlegacy_verdict v[2];
        _snprintf_s(key, sizeof key, _TRUNCATE, "delivery.%s", tree_d);
        map = carrier_map(key, &mn);
        CHECK(map != NULL);
        out = map ? sh_modlegacy_process(map, mn, &on) : NULL;
        CHECK(out && sh_modlegacy_last_report(v, 2) == 1 && v[0].verdict == SH_MODLEGACY_CONVERT &&
              strstr(v[0].folder, "\\mods\\doors") && !strcmp(v[0].id, "doors"));
        if (out) HeapFree(GetProcessHeap(), 0, out);
        if (map) HeapFree(GetProcessHeap(), 0, map);
        /* ... and the same authored tree through the record's "trees". */
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\pack-c", ov);
        CHECK(tree_of(path, tree_c) == 1);
        _snprintf_s(key, sizeof key, _TRUNCATE, "delivery.%s", tree_c);
        map = carrier_map(key, &mn);
        out = map ? sh_modlegacy_process(map, mn, &on) : NULL;
        CHECK(out && sh_modlegacy_last_report(v, 2) == 1 && v[0].verdict == SH_MODLEGACY_CONVERT &&
              !strcmp(v[0].id, "alex.door") && hash_c && !strcmp(v[0].bundle, hash_c) && strstr(out, "\"smpmod1.0.1\""));
        if (out) HeapFree(GetProcessHeap(), 0, out);
        if (map) HeapFree(GetProcessHeap(), 0, map);
    }

    /* A second pass does nothing. */
    g_log[0] = 0;
    CHECK(sh_modmigrate_run(0, &st) == 0 && st.ran == 0);
    CHECK(strstr(g_log, "MODS: migration: nothing to do") != NULL);

    /* --again repeats it: the same bundles, same builds, the same folders. */
    g_log[0] = 0;
    CHECK(sh_modmigrate_run(1, &st) == 1);
    CHECK(st.built == 5 && st.reused == 1 && st.converted == 0 && st.skipped == 3 && st.refused == 1 && st.duplicates == 1);
    CHECK(bundle_of(mods, "pack-c", &b) == 1 && hash_c && !strcmp(b.hash, hash_c));
    sh_mb_free(&b);
    CHECK(bundle_of(mods, dupe_b_key, &b) == 1);
    sh_mb_free(&b);
    CHECK(strstr(g_log, "delivered package already converted as mods\\doors ") != NULL);
    {
        unsigned bundles = 0;
        memset(&list, 0, sizeof list);
        CHECK(sh_ms_enumerate(&list) == 1);
        for (i = 0; i < list.count; i++) if (list.items[i].loaded) bundles++;
        CHECK(bundles == 6);
        sh_ms_list_free(&list);
    }
    snapshot_dir(ov, "", &after);
    free(after.text);
    after.text = NULL; after.n = after.cap = 0;
    snapshot_dir(ov, "", &after);
    CHECK(!strcmp(before.text, after.text));
    free(hash_c);
    free(before.text);
    free(after.text);
}

static void kept_backups(const char *root)
{
    char ov[MAX_PATH], path[MAX_PATH * 2];
    sh_modmigrate_stats st;
    printf("-- a non-empty override-backups folder is left alone\n");
    CreateDirectoryA(root, NULL);
    _snprintf_s(ov, sizeof ov, _TRUNCATE, "%s\\overrides", root);
    put_file(ov, "tiny/package.json", "{\"id\": \"tiny\", \"name\": \"Tiny\"}");
    put_file(ov, "tiny/assets/generated/decls/entitydef/tiny/t.decl", DECL_A);
    put_file(root, "override-backups/migration-1/original/x.decl", DECL_A);
    configure(root);
    g_log[0] = 0;
    CHECK(sh_modmigrate_run(0, &st) == 1 && st.built == 1 && !st.backups_removed);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\override-backups\\migration-1\\original\\x.decl", root);
    CHECK(exists(path) && strstr(g_log, "override-backups is not empty; it is left in place") != NULL);
}

/* `sh_mods migrate` in the game: the pass on the worker, then the serving index rebuilt. */
static void worker_pass(const char *root)
{
    char ov[MAX_PATH], path[MAX_PATH * 2], *record;
    sh_modmigrate_stats st;
    int rescans;
    printf("-- sh_mods migrate: the pass on the worker, then the serving index\n");
    CreateDirectoryA(root, NULL);
    _snprintf_s(ov, sizeof ov, _TRUNCATE, "%s\\overrides", root);
    put_file(ov, "small/package.json", "{\"id\": \"small\", \"name\": \"Small\"}");
    put_file(ov, "small/assets/generated/decls/entitydef/small/s.decl", DECL_A);
    put_file(ov, "large/package.json", "{\"id\": \"large\", \"name\": \"Large\"}");
    put_bytes(ov, "large/assets/generated/image/large/sky.bimage", (size_t)MG_TEST_LARGE, 7);
    configure(root);
    g_log[0] = 0;
    rescans = g_mod_stub_rescans;
    CHECK(sh_modmigrate_start(0) == 1);
    CHECK(sh_modmigrate_wait(120000) == 1);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\small\\mod.json", root);
    CHECK(exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\large\\mod.json", root);
    CHECK(exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods", root);
    record = record_of(path);
    CHECK(record && strstr(record, "\"complete\":true") && strstr(record, "\"folders\":[]"));
    free(record);
    sh_modmigrate_last(&st);
    CHECK(st.built == 2 && st.pending == 0);
    CHECK(g_mod_stub_rescans == rescans + 1 && strstr(g_log, "serving index rebuilt after the background pass") != NULL);
    /* Done: the next pass without --again has nothing to do. */
    CHECK(sh_modmigrate_start(0) == 0);
}

static void incomplete_record(const char *root)
{
    char path[MAX_PATH * 2], ov[MAX_PATH];
    sh_modmigrate_stats st;
    printf("-- an incomplete record (the worker did not finish) runs the pass again\n");
    CreateDirectoryA(root, NULL);
    _snprintf_s(ov, sizeof ov, _TRUNCATE, "%s\\overrides", root);
    put_file(ov, "tiny/package.json", "{\"id\": \"tiny\", \"name\": \"Tiny\"}");
    put_file(ov, "tiny/assets/generated/decls/entitydef/tiny/t.decl", DECL_A);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods", root);
    CHECK(sh_mb_record_put(path, "migration", "{\"format\":1,\"complete\":false}"));
    configure(root);
    g_log[0] = 0;
    CHECK(sh_modmigrate_run(0, &st) == 1 && st.built == 1);
    CHECK(strstr(g_log, "the record says the last pass did not finish") != NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\tiny\\mod.json", root);
    CHECK(exists(path));
}

typedef struct mg_hold { DWORD ms; volatile LONG held; ULONGLONG released_at; } mg_hold;

static DWORD WINAPI mg_hold_thread(void *p)
{
    mg_hold *h = (mg_hold *)p;
    modstore_lock();
    InterlockedExchange(&h->held, 1);
    Sleep(h->ms);
    h->released_at = GetTickCount64();
    modstore_unlock();
    return 0;
}

/* What the Mods tab's Migrate button counts. */
static void pending_entries(const char *root)
{
    char ov[MAX_PATH], path[MAX_PATH * 2];
    sh_modmigrate_stats st;
    unsigned entries = 99;
    printf("-- pending: entries the record does not cover, and changed skipped ones\n");
    CreateDirectoryA(root, NULL);
    _snprintf_s(ov, sizeof ov, _TRUNCATE, "%s\\overrides", root);
    configure(root);
    CHECK(sh_modmigrate_pending(&entries) == 0 && entries == 0);   /* no overrides folder */
    put_file(ov, "tiny/package.json", "{\"id\": \"tiny\", \"name\": \"Tiny\"}");
    put_file(ov, "tiny/assets/generated/decls/entitydef/tiny/t.decl", DECL_A);
    put_file(ov, "packed.zip", "PK");
    CHECK(sh_modmigrate_pending(&entries) == 2 && entries == 2);   /* no record yet */
    CHECK(sh_modmigrate_run(0, &st) == 1 && st.built == 1 && st.skipped == 1);
    CHECK(sh_modmigrate_pending(&entries) == 0 && entries == 2);   /* built, and skipped unchanged */
    /* migratedAt has a resolution of one second, and a completed record lists
     * only the folders not done: an entry it does not list is pending when it
     * was created or written after the pass. */
    Sleep(1100);
    put_file(ov, "later/package.json", "{\"id\": \"later\", \"name\": \"Later\"}");
    put_file(ov, "later/assets/generated/decls/entitydef/later/t.decl", DECL_A);
    CHECK(sh_modmigrate_pending(&entries) == 1 && entries == 3);   /* a folder added after the pass */
    {
        /* The skipped ZIP written after the record: the player replaced it. */
        HANDLE h;
        FILETIME ft;
        ULARGE_INTEGER t;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\packed.zip", ov);
        h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        CHECK(h != INVALID_HANDLE_VALUE);
        GetSystemTimeAsFileTime(&ft);
        t.LowPart = ft.dwLowDateTime;
        t.HighPart = ft.dwHighDateTime;
        t.QuadPart += 120ull * 10000000ull;
        ft.dwLowDateTime = t.LowPart;
        ft.dwHighDateTime = t.HighPart;
        if (h != INVALID_HANDLE_VALUE) { SetFileTime(h, NULL, NULL, &ft); CloseHandle(h); }
    }
    CHECK(sh_modmigrate_pending(&entries) == 2);
    CHECK(sh_modmigrate_run(1, &st) == 1 && st.built == 2);
    CHECK(sh_modmigrate_pending(&entries) == 1);                   /* the ZIP is still newer than the record */
}

static void store_lock_contention(const char *root)
{
    char ov[MAX_PATH], path[MAX_PATH * 2], mod[MAX_PATH * 2], key[SH_MS_KEY_MAX + 1], cause[SH_MB_CAUSE_MAX];
    sh_modmigrate_stats st;
    sh_ms_install_stats ist;
    mg_hold h = { 300, 0, 0 };
    HANDLE t;
    ULONGLONG done;
    unsigned waits, installs = 0, failed = 0;
    printf("-- the store lock: the pass waits for a store step, an install during the pass waits for a folder\n");
    CreateDirectoryA(root, NULL);
    _snprintf_s(ov, sizeof ov, _TRUNCATE, "%s\\overrides", root);
    put_file(ov, "tiny/package.json", "{\"id\": \"tiny\", \"name\": \"Tiny\"}");
    put_file(ov, "tiny/assets/generated/decls/entitydef/tiny/t.decl", DECL_A);
    put_file(ov, "large/package.json", "{\"id\": \"large\", \"name\": \"Large\"}");
    put_bytes(ov, "large/assets/generated/image/large/sky.bimage", (size_t)MG_TEST_LARGE, 7);
    _snprintf_s(mod, sizeof mod, _TRUNCATE, "%s\\authoring\\test.lockmod", root);
    put_file(mod, "mod.json", "{\"id\": \"test.lockmod\", \"version\": \"1.0.0\", \"name\": \"Lock\", \"author\": \"tester\"}");
    put_file(mod, "assets/generated/image/mods/test.lockmod/a.bimage", "bytes for the lock test");
    configure(root);
    /* Another thread holds the lock (an install or a download): the pass waits, then runs. */
    waits = modstore_lock_waits();
    t = CreateThread(NULL, 0, mg_hold_thread, &h, 0, NULL);
    CHECK(t != NULL);
    while (t && !InterlockedCompareExchange(&h.held, 0, 0)) Sleep(1);
    CHECK(sh_modmigrate_run(0, &st) == 1 && st.built == 2 && st.refused == 0);
    done = GetTickCount64();
    if (t) { WaitForSingleObject(t, 60000); CloseHandle(t); }
    CHECK(done >= h.released_at && modstore_lock_waits() > waits);
    /* An install while the worker migrates: every one waits its turn and succeeds. */
    CHECK(sh_modmigrate_start(1) == 1);
    do {
        if (sh_ms_install_from_folder(mod, NULL, key, sizeof key, &ist, NULL, cause, sizeof cause)) installs++;
        else { failed++; fprintf(stderr, "  install during the pass: %s\n", cause); }
    } while (sh_modmigrate_busy() && installs < 50);
    CHECK(sh_modmigrate_wait(120000) == 1);
    CHECK(installs >= 1 && failed == 0);
    sh_modmigrate_last(&st);
    CHECK(st.built == 2 && st.refused == 0);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\large\\mod.json", root);
    CHECK(exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\test.lockmod\\mod.json", root);
    CHECK(exists(path));
}

int main(void)
{
    char base[MAX_PATH], root[MAX_PATH];
    printf("modmigrate_test\n");
    g_echo = getenv("MODMIGRATE_ECHO") != NULL;
    GetTempPathA(sizeof base, base);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%ssmp_modmigrate_%lu_a", base, GetCurrentProcessId());
    full_pass(root);
    remove_tree(root);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%ssmp_modmigrate_%lu_b", base, GetCurrentProcessId());
    kept_backups(root);
    remove_tree(root);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%ssmp_modmigrate_%lu_c", base, GetCurrentProcessId());
    worker_pass(root);
    remove_tree(root);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%ssmp_modmigrate_%lu_d", base, GetCurrentProcessId());
    incomplete_record(root);
    remove_tree(root);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%ssmp_modmigrate_%lu_e", base, GetCurrentProcessId());
    store_lock_contention(root);
    remove_tree(root);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%ssmp_modmigrate_%lu_f", base, GetCurrentProcessId());
    pending_entries(root);
    remove_tree(root);
    sh_modmigrate_set_data_root(NULL);
    sh_modlegacy_set_data_root(NULL);
    sh_ms_set_root(NULL);
    printf("\n%s -- %d checks, %d failed\n", g_failed ? "FAILED" : "ok", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
