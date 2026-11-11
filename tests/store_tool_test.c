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
#include "store_convert.h"
#include "store_deflate.h"
#include "store_saves.h"
#include "modmanifest.h"
#include "modstore.h"
#include "modbundle.h"
#include "modstock.h"
#include "mod_test_util.h"
#include "mod_stock_fixture.h"
#include "store_lock.h"
#include "modbundle_old.h"
#include "modbankinfo.h"

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
    /* The game's Wwise bank index: one event only the _sp bank (which SnapMap skips) files. */
    mt_write(doom, "base\\sound\\soundbanks\\pc\\soundbanksinfo.xml",
             "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<SoundBanksInfo Platform=\"Windows\">\n\t<SoundBanks>\n"
             "\t\t<SoundBank Id=\"332520835\" Language=\"SFX\">\n\t\t\t<ShortName>doom_weapon_mp</ShortName>\n"
             "\t\t\t<Path>doom_weapon_mp.bnk</Path>\n\t\t\t<IncludedEvents>\n"
             "\t\t\t\t<Event Id=\"11\" Name=\"Play_wpn_mp_plasma_flyby_lp\"/>\n"
             "\t\t\t</IncludedEvents>\n\t\t</SoundBank>\n"
             "\t\t<SoundBank Id=\"835849565\" Language=\"SFX\">\n\t\t\t<ShortName>doom_weapon_sp</ShortName>\n"
             "\t\t\t<Path>doom_weapon_sp.bnk</Path>\n\t\t\t<IncludedEvents>\n"
             "\t\t\t\t<Event Id=\"961946992\" Name=\"Play_wpn_sp_plasma_flyBy_LP\"/>\n"
             "\t\t\t</IncludedEvents>\n\t\t</SoundBank>\n\t</SoundBanks>\n</SoundBanksInfo>\n");
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
    CHECK(said("  mods\\: 1 folder(s) in the folder layout of an earlier release"));
    CHECK(said("  mods\\blobs\\: 1 file(s), 1 KiB, shared with the mod folders by hard links"));
    CHECK(said("  mods\\: old record file(s) claims.json"));
    CHECK(said("  removed-mods\\: 1 entry of the retired recycle bin"));
    CHECK(said("  overrides\\: 1 entry not yet moved into mods\\"));
    CHECK(said("5 kind(s) of work to do."));
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
    CHECK(said("  mods\\: 1 folder(s) of an earlier release are not converted yet, because building them needs the DOOM "
               "folder"));
    CHECK(said("Partly done: the store layout upgraded, removed-mods\\ retired"));
    CHECK(said("Some items are left; close DOOM and run the Snapmap+ installer again"));
    CHECK(said("(it needs to find your DOOM folder)"));
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
    CHECK(said("  overrides\\: 1 entry not yet moved into mods\\") && said("2 kind(s) of work to do."));
    /* The upgraded folder is beta.26's shape (a format-1 block): the conversion to the one shape is due. */
    CHECK(said("  mods\\: 1 mod folder(s) of an earlier release to convert to this version's shape"));
    CHECK(!said("folder layout of an earlier release") && !said("blobs") && !said("removed-mods"));

    /* With the game folder: overrides moved, the unchanged game file a pointer. */
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK);
    CHECK(said("  overrides\\: moving into mods\\, reading game files from "));
    CHECK(said("2 file(s), 1 of them unchanged game file(s) read from your game"));
    CHECK(said("Done: overrides\\ moved into mods\\ (1 built, 0 delivered converted, 0 reused, 0 skipped, 0 refused), "
               "1 retired folder(s) moved to the stand-in bin, 1 mod folder(s) converted (0 of them do not build yet), 0 "
               "left as they were"));
    CHECK(said("  mods\\alex.tiny: converted (it was alex.tiny; its salt is now "));
    {
        /* The converted folder: format 2, salted, its files under mods/<salt>/, built and verified. */
        sh_mb_bundle t1;
        char p2[MAX_PATH * 2];
        memset(&t1, 0, sizeof t1);
        _snprintf_s(p2, sizeof p2, _TRUNCATE, "%s\\mods\\alex.tiny", data);
        CHECK(sh_mb_load_folder(p2, &t1) && sh_salt_valid(t1.id, strlen(t1.id)) && t1.file_count == 2);
        _snprintf_s(p2, sizeof p2, _TRUNCATE, "%s\\mods\\alex.tiny\\assets\\generated\\decls\\entitydef\\mods\\%s\\a.decl", data,
                    t1.id);
        CHECK(mt_exists(p2));
        _snprintf_s(p2, sizeof p2, _TRUNCATE, "%s\\mods", data);
        CHECK(sh_mb_record_get(p2, "converted", &record) == 1 && record && strstr(record, "\"alex.tiny\":\"") &&
              strstr(record, t1.id) && strstr(record, "d199e9576a74b1e648f7b971bcb09f705ba1fd1a0ec0c6099b0050e65e305f45"));
        free(record);
        record = NULL;
        sh_mb_free(&t1);
    }
    CHECK(said("  migration: overrides\\ moved to the stand-in bin: every entry of it is in mods\\"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\tinypack\\mod.json", data);
    CHECK(mt_exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\tinypack\\assets\\generated\\image\\stock\\b.bimage", data);
    CHECK(mt_exists(path));   /* a pointer whose file the folder keeps: nothing is dropped at install */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods", data);
    CHECK(sh_mb_record_get(path, "migration", &record) == 1 && record && strstr(record, "\"complete\":true"));
    free(record);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" STOCK_REL, data);
    CHECK(!mt_exists(path) && in_bin("overrides"));   /* every entry is in mods\: the folder went to the bin */

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
    /* An entry added to overrides\ after the migration is work again: check names it and migrate moves it. */
    {
        char ov1[MAX_PATH * 2], ov2[MAX_PATH * 2];
        _snprintf_s(ov1, sizeof ov1, _TRUNCATE, "%s\\overrides\\tinypack", fixture);
        _snprintf_s(ov2, sizeof ov2, _TRUNCATE, "%s\\overrides\\tinypack2", data);
        Sleep(1100);   /* the record's migratedAt has one-second resolution; the new entry must be newer */
        CHECK(copy_all(ov1, ov2));
        mt_write(ov2, "package.json", "{\"id\": \"local.tinypack2\", \"name\": \"Tiny pack two\"}");
        {
            /* Its own names under its own id: the current rules refuse files under another mod's. */
            char a1[MAX_PATH * 2], a2[MAX_PATH * 2];
            _snprintf_s(a1, sizeof a1, _TRUNCATE, "%s\\assets\\generated\\decls\\entitydef\\mods\\local.tinypack", ov2);
            _snprintf_s(a2, sizeof a2, _TRUNCATE, "%s\\assets\\generated\\decls\\entitydef\\mods\\local.tinypack2", ov2);
            CHECK(MoveFileExA(a1, a2, 0));
        }
        CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK);
        CHECK(said("  overrides\\: 1 entry added or changed since it was migrated") && said("1 kind(s) of work to do."));
        CHECK(!said("not yet moved into mods"));
        CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK);
        /* The again pass rebuilds every entry there is (the first went to the bin with the folder). */
        CHECK(said("Done: overrides\\ moved into mods\\ (1 built, 0 delivered converted, 0 reused, 0 skipped, 0 refused)"));
        CHECK(!mt_exists(ov2));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\tinypack2\\mod.json", data);
        CHECK(mt_exists(path));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\tinypack\\mod.json", data);
        CHECK(mt_exists(path));
        CHECK(run("check", data, NULL, NULL) == SH_STORE_OK && said("Nothing to migrate."));
    }
    free(before);
    free(after);
    sh_modstock_reset();
}

#define MO_DECL "assets\\generated\\decls\\entitydef\\mods\\my-overrides\\x.decl"
#define MO_DECL2 "assets\\generated\\decls\\entitydef\\mods\\my-overrides\\y.decl"
/* The same files once the folder has its salt (mods/<salt>/). */
#define MO_DECL_AT "assets\\generated\\decls\\entitydef\\mods\\%s\\x.decl"
#define MO_DECL2_AT "assets\\generated\\decls\\entitydef\\mods\\%s\\y.decl"
#define DECL_X "{\n\tsize = \"7\";\n}\n"
#define DECL_MINE "{\n\tsize = \"8\";\n}\n"

static char *text_of(const char *dir, const char *rel);
/* `fmt` (one %s, the folder's salt) read from the salted folder. */
static char *salted_text(const char *folder, const char *fmt)
{
    char rel[MAX_PATH];
    sh_mb_bundle sb;
    char *t = NULL;
    memset(&sb, 0, sizeof sb);
    if (sh_mb_load_folder(folder, &sb) && sh_salt_valid(sb.id, strlen(sb.id))) {
        _snprintf_s(rel, sizeof rel, _TRUNCATE, fmt, sb.id);
        t = text_of(folder, rel);
    }
    sh_mb_free(&sb);
    return t;
}

static char *text_of(const char *dir, const char *rel)
{
    char path[MAX_PATH * 2];
    size_t n = 0;
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\%s", dir, rel);
    return (char *)sh_mb_read_file(path, 1 << 20, &n);
}

/* A store of an early release: no mods\, the loose-file layer in overrides\ (a tree of engine paths
 * and a file the store cannot carry directly in it) and strings\strids.json. */
static void make_loose_store(const char *fixture, const char *data)
{
    char from[MAX_PATH * 2], to[MAX_PATH * 2];
    mt_remove_tree(data);
    CreateDirectoryA(data, NULL);
    mt_write(data, "overrides\\generated\\decls\\entitydef\\mods\\my-overrides\\x.decl", DECL_X);
    mt_write(data, "overrides\\notes.txt", "my notes\n");
    _snprintf_s(from, sizeof from, _TRUNCATE, "%s\\" STOCK_REL, fixture);
    _snprintf_s(to, sizeof to, _TRUNCATE, "%s\\overrides\\generated\\image\\stock\\b.bimage", data);
    mt_mkdirs_for(to);
    CHECK(CopyFileA(from, to, FALSE));   /* a copy of installed content: a stock pointer, never stored */
    mt_write(data, "strings\\strids.json", "\xef\xbb\xbf{\r\n    \"sh_a\": \"Alpha\",\r\n    \"sh_b\": \"Caf\\u00e9\"\r\n}\r\n");
}

static void test_loose_layer(const char *fixture, const char *tmp)
{
    char data[MAX_PATH], doom[MAX_PATH], mods[MAX_PATH * 2], folder[MAX_PATH * 2], path[MAX_PATH * 2], *t, *record = NULL;
    sh_mb_bundle b;
    printf("-- the loose-file layer and strings\\strids.json become mods\\my-overrides; the folders go to the bin\n");
    _snprintf_s(data, sizeof data, _TRUNCATE, "%s\\loose", tmp);
    _snprintf_s(doom, sizeof doom, _TRUNCATE, "%s\\doom", tmp);
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", data);
    _snprintf_s(folder, sizeof folder, _TRUNCATE, "%s\\my-overrides", mods);
    make_doom(fixture, doom);

    /* 1. A fresh my-overrides. */
    make_loose_store(fixture, data);
    CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK);
    CHECK(said("  overrides\\: 2 loose file(s) or folder(s) of engine paths to move into mods\\my-overrides\\"));
    CHECK(said("  strings\\strids.json: 2 string(s) to move into mods\\my-overrides\\"));
    CHECK(!said("not yet moved into mods"));
    /* Without the game folder the loose files wait; nothing is half moved. */
    CHECK(run("migrate", data, NULL, NULL) == SH_STORE_PARTIAL);
    CHECK(said("its loose files are not moved yet: they need the DOOM folder") && said("(it needs to find your DOOM folder)"));
    CHECK(!mt_exists(folder));
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK);
    CHECK(said("migration: the loose files of overrides\\ and strings\\strids.json moved into mods\\my-overrides\\ (the mod "
               "My overrides)"));
    CHECK(said("1 of them unchanged game files read from your game, 2 string(s)"));
    CHECK(said("1 loose file(s) a mod cannot carry were left out") && said("notes.txt"));
    CHECK(said("migration: overrides\\ moved to the stand-in bin: every entry of it is in mods\\"));
    CHECK(said("migration: strings\\ moved to the stand-in bin: its strings are in mods\\my-overrides\\"));
    CHECK(said("Done: ") && said("string(s) moved into mods\\my-overrides, 2 retired folder(s) moved to the stand-in bin."));
    t = salted_text(folder, MO_DECL_AT);
    CHECK(t && !strcmp(t, DECL_X));
    free(t);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\assets\\generated\\image\\stock\\b.bimage", folder);
    CHECK(mt_exists(path));   /* a pointer whose file the folder keeps: nothing is dropped at install */
    memset(&b, 0, sizeof b);
    CHECK(sh_mb_load_folder(folder, &b));
    CHECK(sh_salt_valid(b.id, strlen(b.id)) && b.name && !strcmp(b.name, "My overrides"));
    {
        /* Its strings are named under its salt (DECISIONS 6). */
        char ka[64], kb[64];
        _snprintf_s(ka, sizeof ka, _TRUNCATE, "\"%s_sh_a\":\"Alpha\"", b.id);
        _snprintf_s(kb, sizeof kb, _TRUNCATE, "\"%s_sh_b\":\"Caf\xc3\xa9\"", b.id);
        CHECK(b.extra && strstr(b.extra, ka) && strstr(b.extra, kb));
    }
    sh_mb_free(&b);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\overrides", data);
    CHECK(!mt_exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\strings", data);
    CHECK(!mt_exists(path));
    CHECK(sh_mb_record_get(mods, "loose", &record) == 1 && record && strstr(record, "\"complete\":true"));
    free(record);
    record = NULL;
    /* A second run finds nothing. */
    CHECK(run("check", data, NULL, NULL) == SH_STORE_OK && said("Nothing to migrate."));
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK && said("Nothing to migrate."));

    /* 2. The player's own my-overrides: what it lacks is added, what it has is kept. */
    make_loose_store(fixture, data);
    /* (A binary at a game name is refused by a folder the player made, so the merge is shown with decls.) */
    mt_delete(data, "overrides\\generated\\image\\stock\\b.bimage");
    mt_write(data, "overrides\\generated\\decls\\entitydef\\mods\\my-overrides\\y.decl", DECL_X);
    mt_write(data, "strings\\strids.json", "{\"sh_a\": \"Other\", \"SH_MINE\": \"Mine\", \"sh_c\": \"Gamma\"}");
    /* A beta.26 folder of the player's own (an author id): the game no longer reads it, the tool converts it. */
    mt_write(folder, "mod.json", "{\n  \"id\": \"my-overrides\",\n  \"name\": \"Mine\",\n  \"note\": [1, 2],\n"
                                 "  \"strings\": {\"en\": {\"sh_a\": \"Mine A\", \"sh_mine\": \"Mine\"}}\n}\n");
    mt_write(folder, MO_DECL, DECL_MINE);
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK);
    CHECK(said("  mods\\my-overrides: converted (it was my-overrides; its salt is now "));
    CHECK(said("migration: kept my-overrides' assets/generated/decls/entitydef/mods/my-overrides/x.decl; the copy in "
               "overrides\\ differs and was not taken"));
    CHECK(said("migration: kept my-overrides' sh_a; strings\\strids.json said \"Other\""));
    CHECK(!said("kept my-overrides' SH_MINE"));   /* the same text in another case is already there */
    CHECK(said("added to mods\\my-overrides\\: 1 file(s) added, 1 kept, 0 already there; 1 string(s) added, 1 kept, "
               "1 already there"));
    t = salted_text(folder, MO_DECL_AT);
    CHECK(t && !strcmp(t, DECL_MINE));
    free(t);
    t = salted_text(folder, MO_DECL2_AT);
    CHECK(t && !strcmp(t, DECL_X));
    free(t);
    t = text_of(folder, "mod.json");
    CHECK(t && strstr(t, "\"name\": \"Mine\"") && strstr(t, "\"note\": [1, 2]") && strstr(t, "\"Mine A\"") &&
          strstr(t, "_sh_c\": \"Gamma\"") && !strstr(t, "Other") && strstr(t, "\"snapmap-plus\"") && !strstr(t, "\"id\""));
    free(t);
    /* Converted and built by the tool: the next start has nothing to build, and its strings serve from it. */
    sh_ms_set_root(mods);
    CHECK(sh_ms_sync_pending() == 0);
    memset(&b, 0, sizeof b);
    CHECK(sh_mb_load_folder(folder, &b) && b.extra && strstr(b.extra, "_sh_c\":\"Gamma\"") && b.file_count == 2);
    sh_mb_free(&b);
    CHECK(run("check", data, NULL, NULL) == SH_STORE_OK && said("Nothing to migrate."));

    /* 3. A ZIP the migration skips keeps overrides\; a malformed document keeps strings\. */
    mt_remove_tree(data);
    CreateDirectoryA(data, NULL);
    mt_write(data, "overrides\\generated\\decls\\entitydef\\mods\\my-overrides\\x.decl", DECL_X);
    mt_write(data, "overrides\\packed.zip", "PK");
    mt_write(data, "strings\\strids.json", "{\"sh_a\": {\"nested\": 1}}");
    CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK);
    CHECK(said("  strings\\strids.json: it is not a JSON object of \"id\": \"text\" pairs; fix it"));
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_PARTIAL);
    CHECK(said("overrides\\packed.zip: skipped -- it is a ZIP archive"));
    CHECK(said("migration: strings\\strids.json left as it is: it is not a JSON object"));
    CHECK(said("migration: the loose files of overrides\\ moved into mods\\my-overrides\\"));
    CHECK(said("migration: overrides\\ kept: 1 entry is not moved into mods\\ (first: packed.zip)"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\overrides\\packed.zip", data);
    CHECK(mt_exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\strings\\strids.json", data);
    CHECK(mt_exists(path));
    t = salted_text(folder, MO_DECL_AT);
    CHECK(t && !strcmp(t, DECL_X));
    free(t);
    /* The ZIP alone is no work for a second run; the document still is, until it is fixed. */
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_PARTIAL && !said("moved into mods\\my-overrides") &&
          !said("added to mods\\my-overrides") && said("Some items are left"));
    Sleep(1100);   /* the record's migratedAt has one-second resolution; the fixed document must be newer */
    mt_write(data, "strings\\strids.json", "{\"sh_a\": \"Fixed\"}");
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK);
    CHECK(said("strings\\strids.json added to mods\\my-overrides\\: 0 file(s) added, 0 kept, 0 already there; 1 string(s) "
               "added"));
    CHECK(said("migration: strings\\ moved to the stand-in bin"));
    CHECK(run("check", data, NULL, NULL) == SH_STORE_OK && said("Nothing to migrate."));
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
    CHECK(said("a.decl is a hard link that could not be replaced by a copy (the drive ") && said(" is full (writing ") &&
          said("a.decl); free some space on it and try again); the original was put back"));
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
    CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK && said("1 folder(s) in the folder layout of an earlier release"));

    /* 2. Retiring the set-aside original fails: the copy goes, the original returns. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" TINY "\\" A_DECL, data);
    g_fail_suffix = L".snapmap-plus-unlink";
    CHECK(run("migrate", data, NULL, NULL) == SH_STORE_PARTIAL);
    g_fail_suffix = NULL;
    CHECK(said("could not be replaced by a copy (Windows refused to write ") &&
          said("; close the program holding it or run the game once as the same user); the original was put back"));
    CHECK(mt_exists(path) && !mt_exists(aside) && links_of(path) == 2);

    /* 3. An interrupted run left the original aside and a partial copy: put back first, then converted. */
    CHECK(MoveFileExA(path, aside, 0));
    mt_write(data, TINY "\\" A_DECL, "{\n\tsi");
    CHECK(run("migrate", data, NULL, NULL) == SH_STORE_PARTIAL);
    CHECK(said("  store: a file set aside by an interrupted run was put back"));
    CHECK(said("Partly done: the store layout upgraded"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\alex.tiny\\" A_DECL, data);
    CHECK(mt_exists(path) && links_of(path) == 1);
    bytes = sh_mb_read_file(path, 4096, &len);
    CHECK(bytes && len == 17 && !memcmp(bytes, "{\n\tsize = \"1\";\n}\n", 17));
    free(bytes);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\mods\\alex.tiny\\" A_DECL ".snapmap-plus-unlink", data);
    CHECK(!mt_exists(path));
    /* The layout is done; the conversion to the one shape waits for the game folder. */
    CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK && said("1 mod folder(s) of an earlier release to convert"));
}

/* ------------------------------------------- beta.26 to the one shape ---- */

#define H1 "1111111111111111111111111111111111111111111111111111111111111111"
#define H2 "2222222222222222222222222222222222222222222222222222222222222222"
#define H3 "3333333333333333333333333333333333333333333333333333333333333333"
#define H4 "4444444444444444444444444444444444444444444444444444444444444444"
#define H5 "5555555555555555555555555555555555555555555555555555555555555555"
#define H6 "6666666666666666666666666666666666666666666666666666666666666666"
#define B26_BASE "assets\\generated\\decls\\entitydef\\mods\\alex.beta\\base.decl"
#define B26_ROW  "assets\\generated\\decls\\entitydef\\mods\\alex.beta\\row.decl"
#define B26_ACCOUNT "76561198000000001"
#define B26_ROWS(key, hash) "[{\"id\":\"" key "\",\"version\":\"1.0.0\",\"bundle\":\"" hash "\",\"size\":10,\"names\":[\"mods/" key "/row\"]}]"
#define B26_MAP(key, hash) "{\"entities\":[{\"entityDef\":{\"inherit\":\"mods/" key "/row\"}}],\"variables\":{\"allocCount\":" \
                           "[0,0,0,0,0],\"string\":[]},\"snapmap-plus\":{\"format\":1,\"mods\":" B26_ROWS(key, hash) "}}"

/* A beta.26 mod folder: an author id, plain strings, a format-1 block. */
static void b26_folder(const char *dir, const char *hash, long long built, const char *origin)
{
    char text[2048];
    _snprintf_s(text, sizeof text, _TRUNCATE,
                "{\n  \"id\": \"alex.beta\",\n  \"name\": \"Beta\",\n  \"version\": \"1.0.0\",\n"
                "  \"strings\": {\"en\": {\"beta_name\": \"Beta Imp\"}},\n\n"
                "  \"snapmap-plus\": {\"format\": 1, \"key\": \"alex.beta\", \"build\": 3, \"bundle\": \"%s\", \"built\": %lld,"
                " \"rules\": 7, \"validated\": true%s%s%s}\n}\n", hash, built, origin ? ", \"origin\": \"" : "",
                origin ? origin : "", origin ? "\"" : "");
    mt_write(dir, "mod.json", text);
    mt_write(dir, B26_BASE, "{\n\tsize = \"1\";\n}\n");
    mt_write(dir, B26_ROW, "{\n\tinherit = \"mods/alex.beta/base\";\n\tdisplayName = \"#str_beta_name\";\n}\n");
}

/* A saved map of My Maps: map.decl (a header and zlib) and its sidecar. */
static void b26_slot(const char *saves, const char *slot, const char *json)
{
    char dir[MAX_PATH * 2], path[MAX_PATH * 2];
    unsigned char *z, *raw, side[SH_STORE_SIDECAR_LEN], salt[8] = { 9, 8, 7, 6, 5, 4, 3, 2 };
    size_t zn = 0;
    unsigned long long id = _strtoui64(B26_ACCOUNT, NULL, 10) & ~(0xfffffull << 32);
    char user[32];
    FILE *f = NULL;
    _snprintf_s(user, sizeof user, _TRUNCATE, "%llu", id);   /* the game binds saves to the id with instance 0 */
    _snprintf_s(dir, sizeof dir, _TRUNCATE, "%s\\" B26_ACCOUNT "\\%s", saves, slot);
    z = sh_store_zlib((const unsigned char *)json, strlen(json), &zn);
    raw = (unsigned char *)malloc(zn + 4);
    CHECK(z && raw);
    if (!z || !raw) { free(z); free(raw); return; }
    memcpy(raw, "\x11\x22\x33\x44", 4);
    memcpy(raw + 4, z, zn);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\map.decl", dir);
    mt_mkdirs_for(path);
    if (fopen_s(&f, path, "wb") == 0 && f) { fwrite(raw, 1, zn + 4, f); fclose(f); }
    CHECK(sh_store_sidecar(raw, zn + 4, salt, user, "map.decl", side));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\map.decl.verify", dir);
    if (fopen_s(&f, path, "wb") == 0 && f) { fwrite(side, 1, sizeof side, f); fclose(f); }
    mt_write(dir, "game.details", "checksum=1\nitemId=\nmetadata={}\nmodified=1\n");
    free(z);
    free(raw);
}

/* The text of a saved map, and whether its sidecar verifies with the bound id. */
static char *b26_slot_text(const char *saves, const char *slot, int *signed_ok)
{
    char path[MAX_PATH * 2], user[32];
    unsigned char *raw, *side, got[SH_STORE_SIDECAR_LEN];
    size_t n = 0, sn = 0, tn = 0;
    char *text = NULL;
    _snprintf_s(user, sizeof user, _TRUNCATE, "%llu", _strtoui64(B26_ACCOUNT, NULL, 10) & ~(0xfffffull << 32));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" B26_ACCOUNT "\\%s\\map.decl", saves, slot);
    raw = sh_mb_read_file(path, 1 << 24, &n);
    strcat_s(path, sizeof path, ".verify");
    side = sh_mb_read_file(path, 64, &sn);
    *signed_ok = raw && side && sn == SH_STORE_SIDECAR_LEN && sh_store_sidecar(raw, n, side, user, "map.decl", got) &&
                 !memcmp(got, side, sizeof got);
    if (raw && n > 4) text = (char *)sh_store_unzlib(raw + 4, n - 4, 1 << 24, &tn);
    free(raw);
    free(side);
    return text;
}

static void test_beta26(const char *fixture, const char *tmp)
{
    char data[MAX_PATH], doom[MAX_PATH], mods[MAX_PATH * 2], saves[MAX_PATH * 2], path[MAX_PATH * 2], salt[16] = "";
    char *t, *record = NULL, *before, *after, *bin_before, *bin_after;
    wchar_t wsaves[MAX_PATH * 2];
    sh_mb_bundle b;
    int ok = 0;
    printf("-- beta.26: folders, second builds, .builds, the record, saved maps, a rawmap and a prefab, in the one shape\n");
    _snprintf_s(data, sizeof data, _TRUNCATE, "%s\\b26", tmp);
    _snprintf_s(doom, sizeof doom, _TRUNCATE, "%s\\doom", tmp);
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", data);
    _snprintf_s(saves, sizeof saves, _TRUNCATE, "%s\\savegame.user", tmp);
    mt_remove_tree(data);
    mt_remove_tree(saves);
    make_doom(fixture, doom);
    /* The player's folder, a second build the product made beside it, and a download. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.beta", mods);
    b26_folder(path, H1, 1790000000, NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.beta@0123456789abcdef", mods);
    b26_folder(path, H2, 1790000100, NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\downloads\\Old Map\\alex.beta", mods);
    b26_folder(path, H3, 1790000200, "download");
    /* A folder whose binary names its old key: it cannot be converted. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.bin", mods);
    mt_write(path, "mod.json", "{\n  \"id\": \"alex.bin\",\n  \"name\": \"Bin\",\n  \"snapmap-plus\": {\"format\": 1, \"key\": "
                               "\"alex.bin\", \"bundle\": \"" H4 "\"}\n}\n");
    {
        FILE *f = NULL;
        char bp[MAX_PATH * 2];
        _snprintf_s(bp, sizeof bp, _TRUNCATE, "%s\\assets\\generated\\image\\mods\\alex.bin\\x.bimage", path);
        mt_mkdirs_for(bp);
        if (fopen_s(&f, bp, "wb") == 0 && f) { fwrite("\x00\x01mods/alex.bin/tex\x00\x02", 1, 21, f); fclose(f); }
    }
    /* .builds\ and the retired record sections. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.builds\\alex.beta@4444444444444444", mods);
    b26_folder(path, H4, 1789999000, NULL);
    mt_write(mods, "snapmap-plus.json", "{\"format\":1,\"aliases\":{\"" H5 "\":\"" H1 "\"},\"snapshots\":{\"alex.beta\":"
                                        "\"alex.beta@4444444444444444\"},\"cache\":{\"" H2 "\":\"" H1 "\"},\"claims\":{}}");
    /* Maps: a saved map using the mod, one naming a key nothing provides, the rawmap and a prefab. */
    b26_slot(saves, "SNAPMAPS00000000000000000001", B26_MAP("alex.beta", H2));
    b26_slot(saves, "SNAPMAPS00000000000000000002", B26_MAP("nobody.else", H6));
    mt_write(data, "rawmap.json", B26_MAP("alex.beta", H1));
    mt_write(data, "prefabs\\p.json", "{\"entities\":[{\"entityDef\":{\"inherit\":\"mods/alex.beta/row\"}}],\"~type\":"
                                      "\"idSnapEntityPrefab\",\n\"snapmapPlus\":{\"format\":1,\"description\":\"\",\"tags\":[],"
                                      "\"mods\":" B26_ROWS("alex.beta", H1) "}}");
    MultiByteToWideChar(CP_UTF8, 0, saves, -1, wsaves, MAX_PATH * 2);
    sh_store_tool_set_saves(wsaves);

    CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK);
    CHECK(said("  mods\\: 2 mod folder(s) of an earlier release to convert"));
    CHECK(said("  mods\\: 2 extra copies of a mod that Snapmap+ made"));
    CHECK(said("  mods\\.builds\\: the retired last good builds"));
    CHECK(said("  mods\\snapmap-plus.json: retired section(s) cache, aliases, snapshots"));
    CHECK(said("  4 saved map(s), rawmap or prefab file(s) of an earlier release to convert"));
    bin_before = (char *)calloc(1, 1 << 16);
    bin_after = (char *)calloc(1, 1 << 16);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.bin", mods);
    if (bin_before) snapshot(path, "", bin_before, 1 << 16);

    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK);
    CHECK(said("  mods\\alex.beta: converted (it was alex.beta; its salt is now "));
    /* A folder the salting refuses is still written in the one shape, its build failed with the plain cause. */
    CHECK(said("  mods\\alex.bin: converted (it was alex.bin; its salt is now ") && said("but it does not build yet: "
               "assets/generated/image/mods/alex.bin/x.bimage names mods/alex.bin/ inside a binary file"));
    CHECK(said("  mods\\alex.beta@0123456789abcdef: an extra copy of alex.beta that Snapmap+ made, moved to the stand-in bin"));
    CHECK(said("  mods\\downloads\\Old Map\\alex.beta: an extra copy of alex.beta that Snapmap+ made"));
    CHECK(said("SNAPMAPS00000000000000000001\\map.decl: converted"));
    CHECK(said("SNAPMAPS00000000000000000002\\map.decl: left as it was: it uses nobody.else, which no installed mod folder "
               "provides"));
    CHECK(said("rawmap.json: converted") && said("p.json: converted"));
    CHECK(said("  mods\\.builds\\: the retired last good builds; moved to the stand-in bin"));
    CHECK(said("the retired section \"aliases\" removed") && said("the retired section \"snapshots\" removed"));

    /* The folder: format 2, its salt, its paths, its strings and their references. */
    memset(&b, 0, sizeof b);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.beta", mods);
    ok = sh_mb_load_folder(path, &b) && sh_salt_valid(b.id, strlen(b.id));
    CHECK(ok);
    if (ok) {
        char rel[MAX_PATH], want[128];
        strcpy_s(salt, sizeof salt, b.id);
        _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\decls\\entitydef\\mods\\%s\\row.decl", salt);
        t = text_of(path, rel);
        _snprintf_s(want, sizeof want, _TRUNCATE, "inherit = \"mods/%s/base\";\n\tdisplayName = \"#str_%s_beta_name\"", salt, salt);
        CHECK(t && strstr(t, want));
        free(t);
        t = text_of(path, "mod.json");
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"%s_beta_name\": \"Beta Imp\"", salt);
        CHECK(t && strstr(t, want) && !strstr(t, "\"id\"") && !strstr(t, "\"key\"") && strstr(t, "\"format\": 2"));
        free(t);
    }
    sh_mb_free(&b);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.beta@0123456789abcdef", mods);
    CHECK(!mt_exists(path) && in_bin("alex.beta@0123456789abcdef"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\downloads\\Old Map\\alex.beta", mods);
    CHECK(!mt_exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\.builds", mods);
    CHECK(!mt_exists(path) && in_bin(".builds"));
    /* The folder the salting refused: in the one shape with a salt, its build failed with the cause, its files unchanged. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.bin", mods);
    CHECK(!sh_mb_old_is_old(path));
    t = text_of(path, "mod.json");
    CHECK(t && strstr(t, "\"format\": 2") && strstr(t, "\"salt\"") && strstr(t, "\"cause\"") &&
          strstr(t, "inside a binary file") && !strstr(t, "\"id\"") && !strstr(t, "the folder is unchanged"));
    free(t);
    t = text_of(path, "assets\\generated\\image\\mods\\alex.bin\\x.bimage");
    CHECK(t != NULL);
    free(t);
    (void)bin_after;
    /* The record: old key and every old build hash to the salt; no retired section. */
    CHECK(sh_mb_record_get(mods, "converted", &record) == 1 && record);
    if (record && salt[0]) {
        char want[128];
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"alex.beta\":\"%s\"", salt);
        CHECK(strstr(record, want));
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"" H1 "\":\"%s\"", salt);
        CHECK(strstr(record, want));
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"" H2 "\":\"%s\"", salt);
        CHECK(strstr(record, want));
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"" H3 "\":\"%s\"", salt);
        CHECK(strstr(record, want));
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"" H4 "\":\"%s\"", salt);
        CHECK(strstr(record, want));
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"" H5 "\":\"%s\"", salt);
        CHECK(strstr(record, want));
        CHECK(!strstr(record, "\"refused\"") && !strstr(record, "\"pending\"") && strstr(record, "\"alex.bin\":\""));
    }
    free(record);
    record = NULL;
    CHECK(sh_mb_record_get(mods, "aliases", &record) == 0 && sh_mb_record_get(mods, "snapshots", &record) == 0 &&
          sh_mb_record_get(mods, "cache", &record) == 0);
    CHECK(sh_mb_record_get(mods, "claims", &record) == 1);   /* a section still in use stays */
    free(record);
    record = NULL;

    /* The saved map: its row and placed name by salt at the folder's build, re-signed, the original beside it. */
    {
        int signed_ok = 0;
        sh_modmanifest_list rows = {0};
        char want[128];
        t = b26_slot_text(saves, "SNAPMAPS00000000000000000001", &signed_ok);
        CHECK(t && signed_ok);
        if (t && salt[0]) {
            _snprintf_s(want, sizeof want, _TRUNCATE, "\"inherit\":\"mods/%s/row\"", salt);
            CHECK(strstr(t, want) && !strstr(t, "alex.beta"));
            CHECK(sh_modmanifest_read(t, strlen(t), &rows, NULL) == 1 && !strcmp(rows.rows[0].salt, salt) &&
                  rows.rows[0].build[0] && strcmp(rows.rows[0].build, H2) && !strcmp(rows.rows[0].name, "Beta"));
            sh_modmanifest_list_free(&rows);
        }
        free(t);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" B26_ACCOUNT "\\SNAPMAPS00000000000000000001\\map.decl" SH_CONV_BACKUP_SUFFIX,
                    saves);
        CHECK(mt_exists(path));
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" B26_ACCOUNT "\\SNAPMAPS00000000000000000001\\map.decl.verify"
                    SH_CONV_BACKUP_SUFFIX, saves);
        CHECK(mt_exists(path));
        /* The map naming a key nothing provides is untouched. */
        t = b26_slot_text(saves, "SNAPMAPS00000000000000000002", &signed_ok);
        CHECK(t && signed_ok && !strcmp(t, B26_MAP("nobody.else", H6)));
        free(t);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" B26_ACCOUNT "\\SNAPMAPS00000000000000000002\\map.decl" SH_CONV_BACKUP_SUFFIX,
                    saves);
        CHECK(!mt_exists(path));
    }
    /* The rawmap and the prefab, with their originals beside them. */
    t = text_of(data, "rawmap.json");
    CHECK(t && salt[0] && strstr(t, salt) && !strstr(t, "alex.beta") && strstr(t, "\"mod\":\""));
    free(t);
    t = text_of(data, "rawmap.json" SH_CONV_BACKUP_SUFFIX);
    CHECK(t && !strcmp(t, B26_MAP("alex.beta", H1)));
    free(t);
    t = text_of(data, "prefabs\\p.json");
    CHECK(t && salt[0] && strstr(t, salt) && !strstr(t, "alex.beta") && strstr(t, "\"mod\":\"") && strstr(t, "snapmapPlus"));
    free(t);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\prefabs\\p.json" SH_CONV_BACKUP_SUFFIX, data);
    CHECK(mt_exists(path));

    /* A second run does nothing: what was left is listed, not work. */
    CHECK(run("check", data, NULL, NULL) == SH_STORE_OK && said("Nothing to migrate."));
    CHECK(!said("folder(s) left as they were") && said("  1 map(s) left as they were by an earlier run"));
    before = (char *)calloc(1, 1 << 20);
    after = (char *)calloc(1, 1 << 20);
    if (before && after) {
        unsigned discards = g_discards;
        snapshot(data, "", before, 1 << 20);
        CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK && said("Nothing to migrate."));
        snapshot(data, "", after, 1 << 20);
        CHECK(!strcmp(before, after) && g_discards == discards);
    }
    /* An earlier-shape folder copied in after the conversion (DECISIONS 21): the next run converts it alone. */
    if (before && after) {
        before[0] = after[0] = 0;
        snapshot(mods, "", before, 1 << 20);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\late", mods);
        mt_write(path, "mod.json", "{\n  \"id\": \"alex.late\",\n  \"name\": \"Late\"\n}\n");
        mt_write(path, "assets\\generated\\decls\\entitydef\\mods\\alex.late\\l.decl", "{\n\tsize = \"3\";\n}\n");
        mt_write(path, "README.txt", "mods/alex.late/ is where my files are\n");
        CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK && said("1 mod folder(s) of an earlier release to convert"));
        CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK && said("  mods\\late: converted (it was alex.late;"));
        CHECK(!said("alex.beta") && !said("alex.bin"));
        t = text_of(path, "README.txt");   /* a file beside the mod is never read or rewritten (DECISIONS 16) */
        CHECK(t && !strcmp(t, "mods/alex.late/ is where my files are\n"));
        free(t);
        snapshot(mods, "", after, 1 << 20);
        {
            /* Every line of the earlier listing is still there unchanged, except the record. */
            char *line = before, *nl;
            for (; *line; line = nl + 1) {
                nl = strchr(line, '\n');
                if (!nl) break;
                *nl = 0;
                if (!strstr(line, "snapmap-plus.json")) CHECK(strstr(after, line) != NULL);
                *nl = '\n';
            }
        }
        CHECK(run("check", data, NULL, NULL) == SH_STORE_OK && said("Nothing to migrate."));
    }
    free(before);
    free(after);
    free(bin_before);
    free(bin_after);
    sh_store_tool_set_saves(L"");
    sh_modstock_reset();
}

static int t_stock(void *context, const char *request)
{
    (void)context;
    return sh_modstock_exists(request);
}

static char *record_section(const char *mods)
{
    char *r = NULL;
    if (sh_mb_record_get(mods, "converted", &r) != 1) { free(r); r = NULL; }
    return r;
}

/* DECISIONS 14, 15 and 16: a run stopped part way resumes and loses nothing; of
 * two folders of one old key the one named after it keeps the key's maps;
 * banks are named for the salt. */
static void test_interrupted(const char *fixture, const char *tmp)
{
    char data[MAX_PATH], doom[MAX_PATH], mods[MAX_PATH * 2], saves[MAX_PATH * 2], path[MAX_PATH * 2], p2[MAX_PATH * 2];
    char salt[16] = "", want[160], *rec, *t;
    wchar_t wsaves[MAX_PATH * 2];
    sh_mb_bundle b;
    int signed_ok = 0;
    printf("-- an interrupted run resumes and loses nothing; copies; sound banks\n");
    _snprintf_s(data, sizeof data, _TRUNCATE, "%s\\cut", tmp);
    _snprintf_s(doom, sizeof doom, _TRUNCATE, "%s\\doom", tmp);
    _snprintf_s(mods, sizeof mods, _TRUNCATE, "%s\\mods", data);
    _snprintf_s(saves, sizeof saves, _TRUNCATE, "%s\\savegame.cut", tmp);
    mt_remove_tree(data);
    mt_remove_tree(saves);
    make_doom(fixture, doom);
    MultiByteToWideChar(CP_UTF8, 0, saves, -1, wsaves, MAX_PATH * 2);
    sh_store_tool_set_saves(wsaves);

    /* 1. Stopped between the two renames of a folder: the folder as it was is beside a staged copy. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.beta.old-999999", mods);
    b26_folder(path, H1, 1790000000, NULL);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.beta.staging-999999", mods);
    mt_write(path, "mod.json", "{\"name\": \"half\"}");
    b26_slot(saves, "SNAPMAPS00000000000000000003", B26_MAP("alex.beta", H1));
    CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK && said("  mods\\: 2 item(s) an interrupted run left"));
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK);
    CHECK(said("alex.beta: put back as it was before an interrupted run") &&
          said("  mods\\alex.beta: converted (it was alex.beta; its salt is now "));
    CHECK(!mt_exists(path));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.beta", mods);
    memset(&b, 0, sizeof b);
    CHECK(sh_mb_load_folder(path, &b) && sh_salt_valid(b.id, strlen(b.id)));
    strcpy_s(salt, sizeof salt, b.id);
    sh_mb_free(&b);
    t = b26_slot_text(saves, "SNAPMAPS00000000000000000003", &signed_ok);
    CHECK(t && signed_ok && salt[0] && strstr(t, salt));
    free(t);

    /* 2. Stopped after the swap, before the folder's row moved from "pending": the next run commits it. */
    _snprintf_s(want, sizeof want, _TRUNCATE, "{\"pending\":{\"alex.beta\":\"%s alex.beta " H1 "\"}}", salt);
    CHECK(sh_mb_record_put(mods, "converted", want));
    CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK && said("1 item(s) an interrupted run left"));
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK);
    rec = record_section(mods);
    _snprintf_s(want, sizeof want, _TRUNCATE, "\"alex.beta\":\"%s\"", salt);
    CHECK(rec && strstr(rec, want) && !strstr(rec, "pending"));
    _snprintf_s(want, sizeof want, _TRUNCATE, "\"" H1 "\":\"%s\"", salt);
    CHECK(rec && strstr(rec, want));
    free(rec);

    /* 3. A saved map stopped between its map.decl and its signature: the next run puts the signature in place. */
    b26_slot(saves, "SNAPMAPS00000000000000000004", B26_MAP("alex.beta", H1));
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK && said("SNAPMAPS00000000000000000004\\map.decl: converted"));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" B26_ACCOUNT "\\SNAPMAPS00000000000000000004\\map.decl.verify", saves);
    _snprintf_s(p2, sizeof p2, _TRUNCATE, "%s.snapmap-plus-tmp", path);
    CHECK(MoveFileExA(path, p2, 0));
    strcat_s(path, sizeof path, SH_CONV_BACKUP_SUFFIX);
    {
        char sig[MAX_PATH * 2];
        _snprintf_s(sig, sizeof sig, _TRUNCATE, "%s\\" B26_ACCOUNT "\\SNAPMAPS00000000000000000004\\map.decl.verify", saves);
        CHECK(CopyFileA(path, sig, FALSE));   /* the old signature beside the new map: it does not verify */
    }
    t = b26_slot_text(saves, "SNAPMAPS00000000000000000004", &signed_ok);
    CHECK(t && !signed_ok);
    free(t);
    CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK);
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK && said("its signature, which an interrupted run had not put "
                                                                      "in place, is in place"));
    t = b26_slot_text(saves, "SNAPMAPS00000000000000000004", &signed_ok);
    CHECK(t && signed_ok && strstr(t, salt));
    free(t);
    CHECK(!mt_exists(p2));
    /* Stopped before map.decl was put in place: the temporaries go and the map converts. */
    b26_slot(saves, "SNAPMAPS00000000000000000005", B26_MAP("alex.beta", H1));
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\" B26_ACCOUNT "\\SNAPMAPS00000000000000000005", saves);
    mt_write(path, "map.decl.snapmap-plus-tmp", "half");
    mt_write(path, "map.decl.verify.snapmap-plus-tmp", "half");
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK && said("SNAPMAPS00000000000000000005\\map.decl: converted"));
    t = b26_slot_text(saves, "SNAPMAPS00000000000000000005", &signed_ok);
    CHECK(t && signed_ok && strstr(t, salt));
    free(t);
    _snprintf_s(p2, sizeof p2, _TRUNCATE, "%s\\map.decl.snapmap-plus-tmp", path);
    CHECK(!mt_exists(p2));
    CHECK(run("check", data, NULL, NULL) == SH_STORE_OK && said("Nothing to migrate."));

    /* 4. An Explorer copy whose name sorts first: the folder named after the key keeps the key's maps. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.zed", mods);
    mt_write(path, "mod.json", "{\n  \"id\": \"alex.zed\",\n  \"name\": \"Zed\"\n}\n");
    mt_write(path, "assets\\generated\\decls\\entitydef\\mods\\alex.zed\\z.decl", "{\n\tsize = \"4\";\n}\n");
    _snprintf_s(p2, sizeof p2, _TRUNCATE, "%s\\a copy of zed", mods);
    CHECK(copy_all(path, p2));
    /* 4b. A "scenes" block the game's builder refuses: converted, its build failed with the same cause. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.scenes", mods);
    mt_write(path, "mod.json", "{\n  \"id\": \"alex.scenes\",\n  \"name\": \"Scenes\",\n"
                               "  \"scenes\": {\"mods/alex.scenes/node\": {\"plays\": \"sideways\"}}\n}\n");
    mt_write(path, "assets\\generated\\decls\\entitydef\\mods\\alex.scenes\\node.decl", "{\n\tsize = \"4\";\n}\n");
    /* 5. A sound bank named for the old key. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.bank", mods);
    mt_write(path, "mod.json", "{\n  \"id\": \"alex.bank\",\n  \"name\": \"Bank\",\n  \"soundbank\": \"alex_bank_music\"\n}\n");
    mt_write(path, "assets\\generated\\decls\\sound\\mods\\alex.bank\\m.decl",
             "{\n\tbank = \"alex_bank_music\";\n\tother = \"alex_bank_musical\";\n}\n");
    /* A sound decl whose event only a stock bank SnapMap skips files: the build records that bank. */
    mt_write(path, "assets\\generated\\decls\\sound\\mods\\alex.bank\\wpn_sp_plasma_flyby_lp.decl",
             "{\n\tinherit = \"default\";\n}\n");
    mt_write(path, "README.txt", "alex_bank_music is my bank\n");
    {
        unsigned char bnk[24] = { 'B', 'K', 'H', 'D', 16, 0, 0, 0, 0x71, 0, 0, 0 };
        uint32_t id = sh_mb_fnv1_lower("alex_bank_music", 15);
        FILE *f = NULL;
        char bp[MAX_PATH * 2];
        memcpy(bnk + 12, &id, 4);
        _snprintf_s(bp, sizeof bp, _TRUNCATE, "%s\\assets\\sound\\soundbanks\\pc\\alex_bank_music.bnk", path);
        mt_mkdirs_for(bp);
        if (fopen_s(&f, bp, "wb") == 0 && f) { fwrite(bnk, 1, sizeof bnk, f); fclose(f); }
    }
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK);
    CHECK(said("  mods\\alex.scenes: converted (it was alex.scenes; its salt is now ") &&
          said("mod.json \"scenes\" is refused: "));
    rec = record_section(mods);
    {
        sh_mb_bundle z;
        memset(&z, 0, sizeof z);
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.zed", mods);
        CHECK(sh_mb_load_folder(path, &z) && sh_salt_valid(z.id, strlen(z.id)));
        _snprintf_s(want, sizeof want, _TRUNCATE, "\"alex.zed\":\"%s\"", z.id);
        CHECK(rec && strstr(rec, want));
        sh_mb_free(&z);
    }
    free(rec);
    {
        char bsalt[SH_SALT_LEN + 1] = "", bp[MAX_PATH * 2], stem[64];
        unsigned char *bytes;
        size_t n = 0;
        _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\alex.bank", mods);
        t = text_of(path, "mod.json");
        if (t && strstr(t, "\"salt\": \"")) memcpy(bsalt, strstr(t, "\"salt\": \"") + 9, SH_SALT_LEN);
        free(t);
        CHECK(sh_salt_valid(bsalt, strlen(bsalt)));
        _snprintf_s(stem, sizeof stem, _TRUNCATE, "%s_music", bsalt);
        _snprintf_s(bp, sizeof bp, _TRUNCATE, "%s\\assets\\sound\\soundbanks\\pc\\%s.bnk", path, stem);
        bytes = sh_mb_read_file(bp, 4096, &n);
        CHECK(bytes && n == 24);
        if (bytes && n == 24) {
            uint32_t id;
            memcpy(&id, bytes + 12, 4);
            CHECK(id == sh_mb_fnv1_lower(stem, strlen(stem)));
        }
        free(bytes);
        _snprintf_s(bp, sizeof bp, _TRUNCATE, "%s\\assets\\sound\\soundbanks\\pc\\alex_bank_music.bnk", path);
        CHECK(!mt_exists(bp));
        /* The converted folder's bank needs are the ones the game's own build of it records. */
        {
            sh_mb_bundle conv, game;
            sh_ms_list list;
            sh_mb_resolver r;
            memset(&conv, 0, sizeof conv);
            memset(&game, 0, sizeof game);
            memset(&list, 0, sizeof list);
            CHECK(sh_mb_load_folder(path, &conv));
            sh_ms_set_root(mods);
            CHECK(sh_ms_enumerate(&list));
            r.installed = sh_ms_request_exists;
            r.installed_context = &list;
            r.stock = t_stock;
            r.stock_context = NULL;
            sh_mb_set_bank_need(sh_mbi_bundle_need);
            /* (a copy without the README: the builder lane's change that ignores files beside mod.json and
             * assets\ is not on this branch) */
            _snprintf_s(p2, sizeof p2, _TRUNCATE, "%s\\gamecopy", tmp);
            mt_remove_tree(p2);
            CHECK(copy_all(path, p2));
            mt_delete(p2, "README.txt");
            CHECK(sh_mb_build(p2, NULL, &r, &game));
            if (game.errors) fprintf(stderr, "  game-side build: %s\n", game.first_cause);
            sh_mb_set_bank_need(NULL);
            sh_ms_list_free(&list);
            CHECK(conv.registries && strstr(conv.registries, "\"banks\":[\"doom_weapon_sp.bnk\"]"));
            CHECK(conv.registries && game.registries && !strcmp(conv.registries, game.registries));
            sh_mb_free(&conv);
            sh_mb_free(&game);
        }
        /* Every text of the mod naming the bank follows it; a longer word and a file beside the mod do not. */
        {
            char want[96], rel[MAX_PATH];
            _snprintf_s(rel, sizeof rel, _TRUNCATE, "assets\\generated\\decls\\sound\\mods\\%s\\m.decl", bsalt);
            t = text_of(path, rel);
            _snprintf_s(want, sizeof want, _TRUNCATE, "bank = \"%s\";", stem);
            CHECK(t && strstr(t, want) && strstr(t, "\"alex_bank_musical\"") && !strstr(t, "\"alex_bank_music\""));
            free(t);
            t = text_of(path, "mod.json");
            _snprintf_s(want, sizeof want, _TRUNCATE, "\"soundbank\": \"%s\"", stem);
            CHECK(t && strstr(t, want));
            free(t);
            t = text_of(path, "README.txt");
            CHECK(t && !strcmp(t, "alex_bank_music is my bank\n"));
            free(t);
        }
    }
    /* 6. A folder that cannot build (a new name outside its namespace, as the owner's cyberdemon boss): written in
     * the one shape, its build failed, the cause naming the file and where it must go under the real salt. */
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\cyber", mods);
    mt_write(path, "mod.json", "{\n  \"id\": \"alex.cyber\",\n  \"name\": \"Cyber\"\n}\n");
    mt_write(path, "assets\\generated\\decls\\animweb\\zion\\snapmap\\cyber_boss.decl", "{\n}\n");
    mt_write(path, "assets\\generated\\decls\\entitydef\\mods\\alex.cyber\\boss.decl", "{\n\tsize = \"5\";\n}\n");
    b26_slot(saves, "SNAPMAPS00000000000000000006", B26_MAP("alex.cyber", H6));
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK);
    CHECK(said("  mods\\cyber: converted (it was alex.cyber; its salt is now ") && said("but it does not build yet: "));
    CHECK(said("SNAPMAPS00000000000000000006\\map.decl: left as it was"));
    CHECK(!sh_mb_old_is_old(path));
    t = text_of(path, "mod.json");
    if (t && strstr(t, "\"salt\": \"")) {
        char csalt[SH_SALT_LEN + 1] = "", under[64];
        memcpy(csalt, strstr(t, "\"salt\": \"") + 9, SH_SALT_LEN);
        _snprintf_s(under, sizeof under, _TRUNCATE, "mods/%s/", csalt);
        CHECK(strstr(t, "\"cause\"") && strstr(t, "cyber_boss") && strstr(t, under) && !strstr(t, "\"build\""));
    } else CHECK(0);
    free(t);
    /* The map stays left while its folder fails, and converts at the next run once the author fixed the folder
     * and the game built it, without the map file changing (DECISIONS 21). */
    CHECK(run("check", data, NULL, NULL) == SH_STORE_OK && said("1 map(s) left as they were by an earlier run"));
    mt_delete(path, "assets\\generated\\decls\\animweb\\zion\\snapmap\\cyber_boss.decl");
    {
        char cause[256];
        CHECK(modbundle_build_folder(path, cause, sizeof cause));   /* what the game's next scan does */
    }
    CHECK(run("check", data, NULL, NULL) == SH_STORE_WORK && said("1 saved map(s), rawmap or prefab file(s)"));
    CHECK(run("migrate", data, "--doom", doom) == SH_STORE_OK && said("SNAPMAPS00000000000000000006\\map.decl: converted"));
    t = b26_slot_text(saves, "SNAPMAPS00000000000000000006", &signed_ok);
    CHECK(t && signed_ok && !strstr(t, "alex.cyber"));
    free(t);
    CHECK(run("check", data, NULL, NULL) == SH_STORE_OK && said("Nothing to migrate."));
    sh_store_tool_set_saves(L"");
    sh_modstock_reset();
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

/* The named lock of store_lock.h: one name per mods folder whatever its
 * spelling, and a holder in another thread keeps every other taker out. */
typedef struct lk_arg { const wchar_t *mods; HANDLE got; } lk_arg;
static DWORD WINAPI lk_take(void *p)
{
    lk_arg *a = (lk_arg *)p;
    a->got = sh_store_lock_take(a->mods, 50);
    sh_store_lock_give(a->got);
    return 0;
}

static void test_store_lock(const char *tmp)
{
    wchar_t mods[MAX_PATH], other[MAX_PATH], n1[64], n2[64], n3[64], n4[64];
    lk_arg a;
    HANDLE held, t;
    printf("-- the named lock on the mods folder\n");
    _snwprintf_s(mods, MAX_PATH, _TRUNCATE, L"%hs\\lock\\mods", tmp);
    _snwprintf_s(other, MAX_PATH, _TRUNCATE, L"%hs\\LOCK\\Mods\\", tmp);
    CHECK(sh_store_lock_name(mods, n1, 64) && sh_store_lock_name(other, n2, 64) && !wcscmp(n1, n2));
    CHECK(!wcsncmp(n1, L"Local\\snapmap-plus-store-", 25) && wcslen(n1) == 25 + 16);
    _snwprintf_s(other, MAX_PATH, _TRUNCATE, L"\\\\?\\%hs\\lock\\mods", tmp);
    CHECK(sh_store_lock_name(other, n3, 64) && !wcscmp(n1, n3));
    _snwprintf_s(other, MAX_PATH, _TRUNCATE, L"%hs\\lock\\mods2", tmp);
    CHECK(sh_store_lock_name(other, n4, 64) && wcscmp(n1, n4));
    /* The FNV-1a over "c:\a" in UTF-16LE: the contract's arithmetic, fixed. */
    CHECK(sh_store_lock_name(L"C:\\A", n4, 64) && !wcscmp(n4, L"Local\\snapmap-plus-store-f81f040d530f58f9"));
    held = sh_store_lock_take(mods, 1000);
    CHECK(held != NULL);
    a.mods = mods;
    a.got = (HANDLE)1;
    t = CreateThread(NULL, 0, lk_take, &a, 0, NULL);
    if (t) { WaitForSingleObject(t, 10000); CloseHandle(t); }
    CHECK(a.got == NULL);   /* held elsewhere: the other taker waits, then gives up */
    sh_store_lock_give(held);
    a.got = NULL;
    t = CreateThread(NULL, 0, lk_take, &a, 0, NULL);
    if (t) { WaitForSingleObject(t, 10000); CloseHandle(t); }
    CHECK(a.got != NULL);
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
    sh_store_tool_set_saves(L"");   /* never the player's own My Maps */

    test_check_and_migrate(fixture, tmp);
    test_loose_layer(fixture, tmp);
    test_link_failures(fixture, tmp);
    test_beta26(fixture, tmp);
    test_interrupted(fixture, tmp);
    test_errors(fixture, tmp);
    test_recycle_refusals();
    test_store_lock(tmp);

    if (g_failed) fprintf(stderr, "last output:\n%s", g_out);
    mt_remove_tree(tmp);
    if (g_failed) { fprintf(stderr, "store_tool_test: %d of %d checks failed\n", g_failed, g_checks); return 1; }
    printf("store_tool_test: check, migrate, the Recycle Bin route, the hard-link conversion and idempotence (%d checks)\n",
           g_checks);
    return 0;
}
