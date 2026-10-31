/* store_tool_test -- snapmap-plus-store (src/store) against a store of an
 * earlier release: tests\fixtures\store-retired\data holds a built folder of
 * the retired layout (alex.tiny@1.0.0+1 with bundle.json, bundle.canonical.json
 * and .state.json), an old record file (claims.json), an entry of the retired
 * recycle bin (removed-mods\alex.gone) and an overrides package (tinypack, one
 * file of it byte-identical to an entry of a synthetic DOOM install). The test
 * adds blobs\ and makes one of the folder's files a hard link into it.
 *
 * Checks: check's list and exit codes; migrate without the game folder (the
 * layout upgrade and the bin sweep done, overrides left, exit 1); migrate with
 * it (overrides moved, a stock pointer made, exit 0); a second migrate finds
 * nothing and changes nothing; every retired file and folder goes through the
 * discard (here a stand-in Recycle Bin folder), none is deleted outright; the
 * hard-link conversion keeps the original through a failed copy, a failed
 * retire of the set-aside original and an interrupted run; the argument,
 * DOOM-running and --doom errors exit 2 and change nothing.
 *
 *   store_tool_test.exe <tests\fixtures\store-retired\data>
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "store_tool.h"
#include "store_recycle.h"
#include "modstore.h"
#include "modbundle.h"
#include "modstock.h"
#include "mod_test_util.h"
#include "mod_stock_fixture.h"

static int g_failed, g_checks;
#define CHECK(expr) do {                                                         \
    g_checks++;                                                                  \
    if (!(expr)) {                                                               \
        fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
        g_failed++;                                                              \
    }                                                                            \
} while (0)

/* ------------------------------------------------------------ output ---- */

static char g_out[262144];
static size_t g_out_n;
static void capture(void *ctx, int err, const char *line)
{
    size_t n = strlen(line);
    (void)ctx;
    if (g_out_n + n + 8 < sizeof g_out) {
        if (err) { memcpy(g_out + g_out_n, "[err] ", 6); g_out_n += 6; }
        memcpy(g_out + g_out_n, line, n);
        g_out_n += n;
        g_out[g_out_n++] = '\n';
        g_out[g_out_n] = 0;
    }
}
static int said(const char *needle) { return strstr(g_out, needle) != NULL; }

/* Run the tool on a command line of UTF-8 words. */
static int run(const char *a1, const char *a2, const char *a3, const char *a4)
{
    const char *words[5] = { "snapmap-plus-store", a1, a2, a3, a4 };
    wchar_t buf[5][MAX_PATH * 2], *argv[5];
    int argc = 0, i, r;
    for (i = 0; i < 5 && words[i]; i++) {
        MultiByteToWideChar(CP_UTF8, 0, words[i], -1, buf[i], MAX_PATH * 2);
        argv[argc++] = buf[i];
    }
    g_out_n = 0;
    g_out[0] = 0;
    r = sh_store_tool_run(argc, argv);
    if (getenv("STORE_TOOL_ECHO")) printf("---- exit %d\n%s", r, g_out);
    return r;
}

/* ----------------------------------------------- the stand-in bin ---- */

static char g_bin[MAX_PATH];
static unsigned g_discards;
static const wchar_t *g_fail_suffix;   /* a discard of a path ending so fails */

static int bin_discard(void *ctx, const wchar_t *path)
{
    wchar_t to[MAX_PATH * 2], bin[MAX_PATH];
    const wchar_t *leaf = wcsrchr(path, L'\\');
    size_t n = wcslen(path), k = g_fail_suffix ? wcslen(g_fail_suffix) : 0;
    (void)ctx;
    if (k && n >= k && !_wcsicmp(path + n - k, g_fail_suffix)) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    MultiByteToWideChar(CP_ACP, 0, g_bin, -1, bin, MAX_PATH);
    CreateDirectoryW(bin, NULL);
    _snwprintf_s(to, MAX_PATH * 2, _TRUNCATE, L"%s\\%03u-%s", bin, ++g_discards, leaf ? leaf + 1 : path);
    return MoveFileExW(path, to, 0) != 0;
}

static int in_bin(const char *leaf)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2];
    HANDLE h;
    int hit = 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*-%s", g_bin, leaf);
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) { hit = 1; FindClose(h); }
    return hit;
}

/* ------------------------------------------------------------ trees ---- */

static int copy_all(const char *src, const char *dst)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2], s[MAX_PATH * 2], d[MAX_PATH * 2];
    HANDLE h;
    int ok = 1;
    mt_mkdirs_for(dst);
    if (!CreateDirectoryA(dst, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", src);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        _snprintf_s(s, sizeof s, _TRUNCATE, "%s\\%s", src, fd.cFileName);
        _snprintf_s(d, sizeof d, _TRUNCATE, "%s\\%s", dst, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ok = copy_all(s, d) && ok;
        else ok = CopyFileA(s, d, FALSE) && ok;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return ok;
}

/* Every file and folder under dir with its size and write time. */
static void snapshot(const char *dir, const char *rel, char *out, size_t cap)
{
    WIN32_FIND_DATAA fd;
    char pattern[MAX_PATH * 2], sub[MAX_PATH * 2], subrel[MAX_PATH * 2];
    HANDLE h;
    _snprintf_s(pattern, sizeof pattern, _TRUNCATE, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        size_t n = strlen(out);
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        _snprintf_s(subrel, sizeof subrel, _TRUNCATE, "%s/%s", rel, fd.cFileName);
        /* A folder's own times in its parent's listing are updated lazily: its name only. */
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) _snprintf_s(out + n, cap - n, _TRUNCATE, "%s/\n", subrel);
        else _snprintf_s(out + n, cap - n, _TRUNCATE, "%s %lu %lu %lu\n", subrel, fd.nFileSizeLow,
                         fd.ftLastWriteTime.dwLowDateTime, fd.ftLastWriteTime.dwHighDateTime);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            _snprintf_s(sub, sizeof sub, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
            snapshot(sub, subrel, out, cap);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static DWORD links_of(const char *path)
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE h = CreateFileA(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    DWORD n = 0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    if (GetFileInformationByHandle(h, &info)) n = info.nNumberOfLinks;
    CloseHandle(h);
    return n;
}

#define TINY "mods\\alex.tiny@1.0.0+1"
#define A_DECL "assets\\generated\\decls\\entitydef\\mods\\alex.tiny\\a.decl"
#define A_BLOB "0e9e6000b10994ecc327e98e5bb149860c3e5823d420fef5935ef5c311bb9068"
#define STOCK_REL "overrides\\tinypack\\assets\\generated\\image\\stock\\b.bimage"

/* A copy of the fixture with blobs\ and a.decl hard-linked into it. */
static void make_store(const char *fixture, const char *data)
{
    char file[MAX_PATH * 2], blob[MAX_PATH * 2], blobs[MAX_PATH * 2];
    mt_remove_tree(data);
    CHECK(copy_all(fixture, data));
    _snprintf_s(file, sizeof file, _TRUNCATE, "%s\\" TINY "\\" A_DECL, data);
    _snprintf_s(blobs, sizeof blobs, _TRUNCATE, "%s\\mods\\blobs", data);
    _snprintf_s(blob, sizeof blob, _TRUNCATE, "%s\\" A_BLOB, blobs);
    CreateDirectoryA(blobs, NULL);
    CHECK(MoveFileExA(file, blob, 0));
    CHECK(CreateHardLinkA(file, blob, NULL));
    SetFileAttributesA(blob, FILE_ATTRIBUTE_READONLY);
    CHECK(links_of(file) == 2);
}

/* A DOOM folder whose archives hold the overrides pack's b.bimage. */
static void make_doom(const char *fixture, const char *doom)
{
    char path[MAX_PATH * 2];
    unsigned char *bytes;
    size_t len = 0;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" STOCK_REL, fixture);
    bytes = sh_mb_read_file(path, 1 << 20, &len);
    CHECK(bytes && len > 0);
    {
        entry e[1] = { { "generated/image/stock/b.bimage", bytes, len, 0, 0 } };
        write_container(doom, "gameresources", e, 1);
    }
    mt_write(doom, "DOOMx64vk.exe", "");
    free(bytes);
}

static int g_doom_up;
static int doom_running(void) { return g_doom_up; }

/* ------------------------------------------------------------- tests ---- */

static void test_check_and_migrate(const char *fixture, const char *tmp)
{
    char data[MAX_PATH], doom[MAX_PATH], path[MAX_PATH * 2], *before, *after, *record = NULL;
    unsigned discards;
    printf("-- check, migrate without and with the game folder, idempotence\n");
    _snprintf_s(data, sizeof data, _TRUNCATE, "%s\\data", tmp);
    _snprintf_s(doom, sizeof doom, _TRUNCATE, "%s\\doom", tmp);
    make_store(fixture, data);
    make_doom(fixture, doom);

    CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK);
    CHECK(said("Checking "));
    CHECK(said("  mods\\: 1 folder(s) in the retired layout"));
    CHECK(said("  mods\\blobs\\: 1 file(s), 1 KiB, shared with the mod folders by hard links"));
    CHECK(said("  mods\\: old record file(s) claims.json"));
    CHECK(said("  removed-mods\\: 1 entry of the retired recycle bin"));
    CHECK(said("  overrides\\: 1 entry not yet moved into mods\\"));
    CHECK(said("5 kind(s) of work found. Run: snapmap-plus-store migrate \""));
    /* check changes nothing */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" TINY "\\bundle.json", data);
    CHECK(mt_exists(path));

    /* Without the game folder: the layout and the bin, not overrides. */
    CHECK(run("migrate", data, NULL, NULL) == SH_STORE_PARTIAL);
    CHECK(said("Migrating "));
    CHECK(said("  store: " "alex.tiny@1.0.0+1: mod.json written for alex.tiny@1.0.0+1"));
    CHECK(said("1 hard link(s) replaced by copies"));
    CHECK(said("  store: alex.tiny@1.0.0+1 -> alex.tiny"));
    CHECK(said("  store: blobs\\ removed"));
    CHECK(said("  store: claims.json folded into snapmap-plus.json (claims) and removed"));
    CHECK(said("  removed-mods: alex.gone moved to the stand-in bin"));
    CHECK(said("  removed-mods: 1 folder(s) moved to the stand-in bin; the bin is retired"));
    CHECK(said("  overrides\\: not moved yet: it needs the DOOM folder"));
    CHECK(said("Partly done: the store layout upgraded, removed-mods\\ retired. 1 kind(s) of work are left; run again: "
               "snapmap-plus-store migrate \""));
    CHECK(said("--doom \"<DOOM folder>\""));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\alex.tiny\\mod.json", data);
    CHECK(mt_exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\alex.tiny\\" A_DECL, data);
    CHECK(links_of(path) == 1);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\alex.tiny\\bundle.json", data);
    CHECK(!mt_exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\blobs", data);
    CHECK(!mt_exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\removed-mods", data);
    CHECK(!mt_exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods", data);
    CHECK(sh_mb_record_get(path, "claims", &record) == 1 && record && strstr(record, "\"map-1\""));
    free(record);
    record = NULL;
    /* Everything retired is in the bin, nothing deleted outright. */
    CHECK(in_bin("bundle.json") && in_bin("bundle.canonical.json") && in_bin(".state.json") && in_bin("claims.json"));
    CHECK(in_bin("blobs") && in_bin("alex.gone") && in_bin("a.decl.snapmap-plus-unlink"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\overrides\\tinypack\\package.json", data);
    CHECK(mt_exists(path));

    CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK);
    CHECK(said("  overrides\\: 1 entry not yet moved into mods\\") && said("1 kind(s) of work found"));
    CHECK(!said("retired layout") && !said("blobs") && !said("removed-mods"));

    /* With the game folder: overrides moved, the unchanged game file a pointer. */
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK);
    CHECK(said("  overrides\\: moving into mods\\, reading game files from "));
    CHECK(said("2 file(s), 1 of them stock pointer(s) to installed content"));
    CHECK(said("Done: overrides\\ moved into mods\\ (1 built, 0 delivered converted, 0 reused, 0 skipped, 0 refused)."));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\tinypack\\mod.json", data);
    CHECK(mt_exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\tinypack\\assets\\generated\\image\\stock\\b.bimage", data);
    CHECK(!mt_exists(path));   /* a pointer: the store never receives installed content */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods", data);
    CHECK(sh_mb_record_get(path, "migration", &record) == 1 && record && strstr(record, "\"complete\":true"));
    free(record);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" STOCK_REL, data);
    CHECK(mt_exists(path));   /* overrides\ is never changed */

    CHECK(run("check", data, NULL, NULL) == SH_STORE_OK && said("Nothing to migrate."));

    /* Idempotent: a second migrate finds nothing and changes nothing. */
    before = (char *)calloc(1, 1 << 20);
    after = (char *)calloc(1, 1 << 20);
    CHECK(before && after);
    if (before && after) {
        snapshot(data, "", before, 1 << 20);
        discards = g_discards;
        CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK && said("Nothing to migrate."));
        CHECK(run("migrate", data, NULL, NULL) == SH_STORE_OK && said("Nothing to migrate."));
        snapshot(data, "", after, 1 << 20);
        CHECK(!strcmp(before, after));
        CHECK(g_discards == discards);
    }
    free(before);
    free(after);
    sh_modstock_reset();
}

static void test_link_failures(const char *fixture, const char *tmp)
{
    char data[MAX_PATH], path[MAX_PATH * 2], aside[MAX_PATH * 2], blob[MAX_PATH * 2];
    unsigned char *bytes;
    size_t len = 0;
    printf("-- the hard-link conversion keeps the original through every failure\n");
    _snprintf_s(data, sizeof data, _TRUNCATE, "%s\\links", tmp);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" TINY "\\" A_DECL, data);
    _snprintf_s(aside, sizeof aside, _TRUNCATE, "%s.snapmap-plus-unlink", path);
    _snprintf_s(blob, sizeof blob, _TRUNCATE, "%s\\mods\\blobs\\" A_BLOB, data);

    /* 1. The copy fails: the original is put back, still the hard link. */
    make_store(fixture, data);
    {
        char ov[MAX_PATH * 2];
        _snprintf_s(ov, sizeof ov, _TRUNCATE, "%s\\overrides", data);
        mt_remove_tree(ov);   /* the layout alone */
    }
    sh_ms_test_fail_unlink_copy = 1;
    CHECK(run("migrate", data, NULL, NULL) == SH_STORE_PARTIAL);
    CHECK(sh_ms_test_fail_unlink_copy == 0);
    CHECK(said("a.decl is a hard link that could not be replaced by a copy (error 112); the original was put back"));
    CHECK(said("  store: alex.tiny@1.0.0+1: left as it was"));
    CHECK(said("  store: blobs\\ kept: 1 folder(s) of the retired layout still need it"));
    CHECK(said("Partly done: the store layout partly upgraded (1 folder(s) left as they were)"));
    CHECK(mt_exists(path) && !mt_exists(aside) && links_of(path) == 2);
    bytes = sh_mb_read_file(path, 4096, &len);
    CHECK(bytes && len == 17 && !memcmp(bytes, "{\n\tsize = \"1\";\n}\n", 17));
    free(bytes);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" TINY "\\bundle.json", data);
    CHECK(mt_exists(path));
    CHECK(mt_exists(blob));
    CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK && said("1 folder(s) in the retired layout"));

    /* 2. Retiring the set-aside original fails: the copy goes, the original returns. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" TINY "\\" A_DECL, data);
    g_fail_suffix = L".snapmap-plus-unlink";
    CHECK(run("migrate", data, NULL, NULL) == SH_STORE_PARTIAL);
    g_fail_suffix = NULL;
    CHECK(said("could not be replaced by a copy (error 5); the original was put back"));
    CHECK(mt_exists(path) && !mt_exists(aside) && links_of(path) == 2);

    /* 3. An interrupted run left the original aside and a partial copy: put back first, then converted. */
    CHECK(MoveFileExA(path, aside, 0));
    mt_write(data, TINY "\\" A_DECL, "{\n\tsi");
    CHECK(run("migrate", data, NULL, NULL) == SH_STORE_OK);
    CHECK(said("  store: a file set aside by an interrupted run was put back"));
    CHECK(said("Done: the store layout upgraded"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\alex.tiny\\" A_DECL, data);
    CHECK(mt_exists(path) && links_of(path) == 1);
    bytes = sh_mb_read_file(path, 4096, &len);
    CHECK(bytes && len == 17 && !memcmp(bytes, "{\n\tsize = \"1\";\n}\n", 17));
    free(bytes);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\alex.tiny\\" A_DECL ".snapmap-plus-unlink", data);
    CHECK(!mt_exists(path));
    CHECK(run("check", data, NULL, NULL) == SH_STORE_OK);
}

static void test_errors(const char *fixture, const char *tmp)
{
    char data[MAX_PATH], path[MAX_PATH * 2], missing[MAX_PATH], *before, *after;
    printf("-- errors exit 2 and change nothing\n");
    _snprintf_s(data, sizeof data, _TRUNCATE, "%s\\errors", tmp);
    _snprintf_s(missing, sizeof missing, _TRUNCATE, "%s\\no-such-store", tmp);
    make_store(fixture, data);
    CHECK(run("frobnicate", data, NULL, NULL) == SH_STORE_ERROR && said("[err] error: unknown command \"frobnicate\""));
    CHECK(run("check", NULL, NULL, NULL) == SH_STORE_ERROR && said("[err] error: name the store folder"));
    CHECK(run("check", missing, NULL, NULL) == SH_STORE_ERROR && said("is not a folder"));
    CHECK(run("migrate", data, "--bogus", NULL) == SH_STORE_ERROR && said("[err] error: unknown option --bogus"));
    CHECK(run("migrate", data, "--doom", NULL) == SH_STORE_ERROR && said("[err] error: --doom needs the DOOM folder"));
    CHECK(run("help", NULL, NULL, NULL) == SH_STORE_OK && said("snapmap-plus-store check <store>"));
    before = (char *)calloc(1, 1 << 20);
    after = (char *)calloc(1, 1 << 20);
    if (before && after) {
        snapshot(data, "", before, 1 << 20);
        CHECK(run("migrate", data, "--doom", missing) == SH_STORE_ERROR && said("is not a DOOM 2016 folder"));
        g_doom_up = 1;
        CHECK(run("migrate", data, NULL, NULL) == SH_STORE_ERROR && said("[err] error: DOOM is running"));
        g_doom_up = 0;
        snapshot(data, "", after, 1 << 20);
        CHECK(!strcmp(before, after));
        if (strcmp(before, after)) fprintf(stderr, "before:\n%s\nafter:\n%s\n", before, after);
    }
    free(before);
    free(after);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" TINY "\\bundle.json", data);
    CHECK(mt_exists(path));
}

static void test_recycle_refusals(void)
{
    sh_store_recycle_stats st = { 0, 0 };
    printf("-- the Recycle Bin delete: an absent path counts as gone, an empty one is refused\n");
    CHECK(sh_store_recycle(&st, L"\\\\?\\C:\\smp-store-tool-test-absent-path-0123") == 1);
    CHECK(sh_store_recycle(&st, L"") == 0);
    CHECK(st.recycled == 0);
}

int main(int argc, char **argv)
{
    char fixture[MAX_PATH], tmp[MAX_PATH];
    if (!GetFullPathNameA(argc > 1 ? argv[1] : "fixtures\\store-retired\\data", sizeof fixture, fixture, NULL)) return 2;
    if (!mt_temp_dir("store_tool", tmp, sizeof tmp)) { fprintf(stderr, "no temp dir\n"); return 2; }
    _snprintf_s(g_bin, sizeof g_bin, _TRUNCATE, "%s\\bin", tmp);
    sh_store_tool_set_output(capture, NULL);
    sh_store_tool_set_discard(bin_discard, NULL, "moved to the stand-in bin");
    sh_store_tool_set_doom_running(doom_running);
    sh_store_tool_set_steam(0);

    test_check_and_migrate(fixture, tmp);
    test_link_failures(fixture, tmp);
    test_errors(fixture, tmp);
    test_recycle_refusals();

    if (g_failed) fprintf(stderr, "last output:\n%s", g_out);
    mt_remove_tree(tmp);
    if (g_failed) { fprintf(stderr, "store_tool_test: %d of %d checks failed\n", g_failed, g_checks); return 1; }
    printf("store_tool_test: check, migrate, the Recycle Bin route, the hard-link conversion and idempotence (%d checks)\n",
           g_checks);
    return 0;
}
