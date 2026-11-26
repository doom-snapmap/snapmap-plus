/* cachewrites_test.c -- which of the engine's writes is a cache write, and where it goes (cachewrites_rule.c). */
#include <stdio.h>
#include <windows.h>
#include "wide_path.h"
#include <string.h>
#include "cachewrites.h"

static int g_checks, g_failed;
#define CHECK(x) do { g_checks++; if (!(x)) { g_failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

#define ROOT "C:\\Users\\p\\Saved Games\\id Software\\DOOM"

/* The rule's answer for rel under ROOT, and that a refusal leaves out untouched. */
static int goes_to(const char *rel, const char *want)
{
    char out[512];
    memset(out, 'x', sizeof out);
    out[sizeof out - 1] = '\0';
    if (!sh_cachewrite_redirect(rel, ROOT, out, sizeof out)) return want == NULL && out[0] == 'x' && out[1] == 'x';
    return want != NULL && strcmp(out, want) == 0;
}

static int file_bytes(const wchar_t *path, const char *expected)
{
    char data[24];
    DWORD read = 0;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int ok = ReadFile(h, data, sizeof data, &read, NULL) && read == strlen(expected) && !memcmp(data, expected, read);
    CloseHandle(h);
    return ok;
}

static int test_long_destination(const wchar_t *folder)
{
    static const char component[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    char dest[MAX_PATH], root[SH_U8PATH_MAX], rel[512] = "generated/cm";
    wchar_t target[1024], base[1024];
    if (!sh_cachewrite_ansi(folder, root, sizeof root)) return 1; /* A non-ANSI TEMP exercises the Unicode case below. */
    strcat_s(root, sizeof root, "\\id Software\\DOOM");
    for (int i = 0; i < 7; i++) {
        strcat_s(rel, sizeof rel, "/");
        strcat_s(rel, sizeof rel, component);
    }
    strcat_s(rel, sizeof rel, "/scaled.bcm");
    CHECK(!sh_cachewrite_redirect(rel, root, dest, sizeof dest));
    CHECK(sh_cachewrite_wide(folder, rel, "collision", 9) == 1);
    wcscpy_s(base, _countof(base), folder);
    CHECK(sh_wpath_long(base, _countof(base)));
    wcscpy_s(target, _countof(target), base);
    CHECK(sh_wpath_append(target, _countof(target), L"id Software\\DOOM\\base"));
    CHECK(sh_wpath_append_utf8(target, _countof(target), rel));
    printf("-- ACP-representable destination, %zu Windows path characters before the long-path prefix\n", wcslen(target) - 4);
    CHECK(wcslen(target) - 4 > MAX_PATH && file_bytes(target, "collision"));
    CHECK(DeleteFileW(target));
    while (wcslen(target) > wcslen(base)) {
        wchar_t *slash = wcsrchr(target, L'\\');
        if (!slash) return 0;
        *slash = 0;
        if (wcscmp(target, base)) CHECK(RemoveDirectoryW(target));
    }
    return sh_cachewrite_redirect("maps/ordinary.flight", root, dest, sizeof dest);
}

static int test_profile(void)
{
    wchar_t tmp[MAX_PATH], folder[MAX_PATH], saved[MAX_PATH + 64], clip[MAX_PATH + 150], flight[MAX_PATH + 150];
    char ansi[1024];
    if (!GetTempPathW(_countof(tmp), tmp) || !GetTempFileNameW(tmp, L"scw", 0, folder)) return 0;
    if (!DeleteFileW(folder) || !CreateDirectoryW(folder, NULL)) return 0;
    CHECK(test_long_destination(folder));
    swprintf_s(saved, _countof(saved), L"%ls\\\u65E5\u672C-\u0416-\U00020000", folder);
    if (!CreateDirectoryW(saved, NULL)) return 0;
    int narrow = sh_cachewrite_ansi(saved, ansi, sizeof ansi);
    printf("-- real Saved Games fixture on ACP %u, exact narrow path %d\n", GetACP(), narrow);
    if (GetACP() == 1252) CHECK(!narrow);
    if (GetACP() == CP_UTF8) CHECK(narrow);
    CHECK(sh_cachewrite_ansi(L"C:\\plain\\Saved Games", ansi, sizeof ansi) && !strcmp(ansi, "C:\\plain\\Saved Games"));
    CHECK(sh_cachewrite_wide(saved, "generated/cm/scaled.bcm", "first", 5) == 1);
    CHECK(sh_cachewrite_wide(saved, "maps/DynamicSnapMap.flight", "fly", 3) == 1);
    CHECK(sh_cachewrite_wide(saved, "maps/DynamicSnapMap.map", "ignored", 7) == -1);
    CHECK(sh_cachewrite_wide(saved, "generated/../escape", "ignored", 7) == -1);
    swprintf_s(clip, _countof(clip), L"%ls\\id Software\\DOOM\\base\\generated\\cm\\scaled.bcm", saved);
    swprintf_s(flight, _countof(flight), L"%ls\\id Software\\DOOM\\base\\maps\\DynamicSnapMap.flight", saved);
    CHECK(file_bytes(clip, "first") && file_bytes(flight, "fly"));
    HANDLE locked = CreateFileW(clip, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(locked != INVALID_HANDLE_VALUE);
    if (locked != INVALID_HANDLE_VALUE) {
        CHECK(sh_cachewrite_wide(saved, "generated/cm/scaled.bcm", "next", 4) == 0);
        CHECK(file_bytes(clip, "first"));
        CloseHandle(locked);
    }
    CHECK(sh_cachewrite_wide(saved, "generated/cm/scaled.bcm", "next", 4) == 1 && file_bytes(clip, "next"));
    CHECK(DeleteFileW(clip) && DeleteFileW(flight));
    const wchar_t *dirs[] = { L"id Software\\DOOM\\base\\generated\\cm", L"id Software\\DOOM\\base\\generated",
                               L"id Software\\DOOM\\base\\maps", L"id Software\\DOOM\\base", L"id Software\\DOOM", L"id Software" };
    for (size_t i = 0; i < _countof(dirs); i++) {
        swprintf_s(clip, _countof(clip), L"%ls\\%ls", saved, dirs[i]);
        CHECK(RemoveDirectoryW(clip));
    }
    return RemoveDirectoryW(saved) && RemoveDirectoryW(folder);
}

int main(void)
{
    char out[128];
    size_t need;
    printf("cachewrites_test\n");
    CHECK(test_profile());

    printf("-- the engine's cache writes\n");
    CHECK(goes_to("generated/cm/m1.bcm", ROOT "\\base\\generated\\cm\\m1.bcm"));
    CHECK(goes_to("generated/models/effects/particles/a.pmodel",
                  ROOT "\\base\\generated\\models\\effects\\particles\\a.pmodel"));
    CHECK(goes_to("generated/temp/maps/x.bcm.1A2B.tmp", ROOT "\\base\\generated\\temp\\maps\\x.bcm.1A2B.tmp"));
    CHECK(goes_to("maps/DynamicSnapMap.flight", ROOT "\\base\\maps\\DynamicSnapMap.flight"));
    CHECK(goes_to("maps/x.flight", ROOT "\\base\\maps\\x.flight"));
    CHECK(goes_to("generated/", ROOT "\\base\\generated\\"));

    printf("-- either slash, any case\n");
    CHECK(goes_to("generated\\cm\\m1.bcm", ROOT "\\base\\generated\\cm\\m1.bcm"));
    CHECK(goes_to("generated\\cm/m1.bcm", ROOT "\\base\\generated\\cm\\m1.bcm"));
    CHECK(goes_to("GENERATED/CM/M1.BCM", ROOT "\\base\\GENERATED\\CM\\M1.BCM"));
    CHECK(goes_to("Maps\\Sub\\X.FLIGHT", ROOT "\\base\\Maps\\Sub\\X.FLIGHT"));

    printf("-- every other write passes\n");
    CHECK(goes_to("generated", NULL));
    CHECK(goes_to("generatedx/a.bcm", NULL));
    CHECK(goes_to("maps/x.map", NULL));
    CHECK(goes_to("maps/x.flight.bak", NULL));
    CHECK(goes_to("mapsx/x.flight", NULL));
    CHECK(goes_to("x.flight", NULL));
    CHECK(goes_to("savegame.user/a", NULL));
    CHECK(goes_to("base/generated/cm/a.bcm", NULL));

    printf("-- absolute and parent paths are refused\n");
    CHECK(goes_to("C:/generated/a.bcm", NULL));
    CHECK(goes_to("d:\\generated\\a.bcm", NULL));
    CHECK(goes_to("/generated/a.bcm", NULL));
    CHECK(goes_to("\\generated\\a.bcm", NULL));
    CHECK(goes_to("generated/../a.bcm", NULL));
    CHECK(goes_to("generated\\..\\..\\a.bcm", NULL));
    CHECK(goes_to("maps/../x.flight", NULL));
    CHECK(goes_to("generated/..", NULL));
    CHECK(goes_to("generated/..a/b.bcm", ROOT "\\base\\generated\\..a\\b.bcm"));
    CHECK(goes_to("generated/a../b.bcm", ROOT "\\base\\generated\\a..\\b.bcm"));

    printf("-- NULL and empty\n");
    CHECK(!sh_cachewrite_redirect(NULL, ROOT, out, sizeof out));
    CHECK(!sh_cachewrite_redirect("", ROOT, out, sizeof out));
    CHECK(!sh_cachewrite_redirect("generated/a", NULL, out, sizeof out));
    CHECK(!sh_cachewrite_redirect("generated/a", "", out, sizeof out));
    CHECK(!sh_cachewrite_redirect("generated/a", ROOT, NULL, sizeof out));
    CHECK(!sh_cachewrite_redirect("generated/a", ROOT, out, 0));

    printf("-- a trailing slash on the root is not doubled\n");
    CHECK(sh_cachewrite_redirect("generated/a", "C:\\r\\", out, sizeof out) && !strcmp(out, "C:\\r\\base\\generated\\a"));
    CHECK(sh_cachewrite_redirect("generated/a", "C:\\r/", out, sizeof out) && !strcmp(out, "C:\\r\\base\\generated\\a"));
    CHECK(sh_cachewrite_redirect("generated/a", "C:\\r", out, sizeof out) && !strcmp(out, "C:\\r\\base\\generated\\a"));

    printf("-- the result must fit with its terminator\n");
    need = strlen("C:\\r\\base\\generated\\a") + 1;
    memset(out, 'x', sizeof out);
    CHECK(!sh_cachewrite_redirect("generated/a", "C:\\r", out, need - 1));
    CHECK(out[0] == 'x');
    CHECK(sh_cachewrite_redirect("generated/a", "C:\\r", out, need) && strlen(out) == need - 1);

    printf("cachewrites_test: %d checks, %d failed\n", g_checks, g_failed);
    printf(g_failed ? "cachewrites_test: FAIL\n" : "cachewrites_test: PASS\n");
    return g_failed ? 1 : 0;
}
