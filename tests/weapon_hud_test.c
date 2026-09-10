/* weapon_hud_test.c -- package admission, composition and scoped native relay. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "weapon_hud.h"
#include "packages.h"
static int failures;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); failures++; } } while (0)
void backend_log(const char *s) { (void)s; }
static const char valid[] = "{\"schema\":\"snapmap-plus.weapon-hud.v1\",\"weapons\":{"
    "\"weapon/test/unlimited\":{\"ammo_display\":\"weapon\"}}}";
static int parse(const char *s, const char *owner) { return sh_weapon_hud_test_parse(s, strlen(s), owner); }
static BOOL WINAPI broken_next(HANDLE handle, LPWIN32_FIND_DATAA found)
{
    BOOL result = FindNextFileA(handle, found);
    if (!result) SetLastError(ERROR_ACCESS_DENIED);
    return result;
}
static void write_text(const char *path, const char *s)
{
    FILE *f = NULL;
    CHECK(fopen_s(&f, path, "wb") == 0 && f);
    if (f) { CHECK(fwrite(s, 1, strlen(s), f) == strlen(s)); fclose(f); }
}
int main(void)
{
    sh_weapon_hud_test_reset();
    CHECK(parse(valid, "first"));
    CHECK(parse(valid, "second"));
    CHECK(sh_weapon_hud_test_count() == 1);
    CHECK(sh_weapon_hud_select("weapon/test/unlimited", 1) == 0);
    CHECK(sh_weapon_hud_select("weapon/test/unlimited", 0) == 0);
    CHECK(sh_weapon_hud_select("weapon/test/other", 1) == 1);
    CHECK(sh_weapon_hud_select(NULL, 1) == 1);
    CHECK(!parse("{\"schema\":\"snapmap-plus.weapon-hud.v1\",\"weapons\":{"
        "\"weapon/test/unlimited\":{\"ammo_display\":\"engine\"}}}", "conflict"));
    CHECK(sh_weapon_hud_test_count() == 0);
    const char *bad[] = {
        "{}", "null", "[]",
        "{\"schema\":\"future\",\"weapons\":{}}",
        "{\"schema\":\"snapmap-plus.weapon-hud.v1\",\"weapons\":{},\"script\":\"x\"}",
        "{\"schema\":\"snapmap-plus.weapon-hud.v1\",\"weapons\":{\"a\":{\"ammo_display\":\"infinite\"}}}",
        "{\"schema\":\"snapmap-plus.weapon-hud.v1\",\"weapons\":{\"a\":{\"ammo_display\":false}}}",
        "{\"schema\":\"snapmap-plus.weapon-hud.v1\",\"weapons\":{\"a\":{\"ammo_display\":\"weapon\",\"damage\":4}}}",
        "{\"schema\":\"snapmap-plus.weapon-hud.v1\",\"weapons\":{\"../a\":{\"ammo_display\":\"weapon\"}}}",
        "{\"schema\":\"snapmap-plus.weapon-hud.v1\",\"weapons\":{\"a\\u0000b\":{\"ammo_display\":\"weapon\"}}}",
        "{\"schema\":\"snapmap-plus.weapon-hud.v1\",\"weapons\":{\"a\":{\"ammo_display\":\"weapon\",\"ammo_display\":\"engine\"}}}",
        "{\"schema\":\"snapmap-plus.weapon-hud.v1\",\"weapons\":{\"a\":{},\"a\":{}}}",
        "{\"schema\":\"snapmap-plus.weapon-hud.v1\",\"weapons\":{\"a\":{\"ammo_display\":\"weapon\\u0000\"}}}"
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        CHECK(parse(valid, "first"));
        CHECK(!parse(bad[i], "bad"));
        CHECK(sh_weapon_hud_test_count() == 0);
        CHECK(sh_weapon_hud_select("weapon/test/unlimited", 1) == 1);
    }
    char rows[40000], row[160];
    strcpy_s(rows, sizeof rows, "{\"schema\":\"snapmap-plus.weapon-hud.v1\",\"weapons\":{");
    for (int i = 0; i < 257; i++) {
        _snprintf_s(row, sizeof row, _TRUNCATE,
            "%s\"weapon/test/%d\":{\"ammo_display\":\"weapon\"}", i ? "," : "", i);
        strcat_s(rows, sizeof rows, row);
    }
    strcat_s(rows, sizeof rows, "}}");
    CHECK(!parse(rows, "too-many"));
    CHECK(sh_weapon_hud_test_count() == 0);

    char temp[MAX_PATH], root[MAX_PATH], over[MAX_PATH], pkg[MAX_PATH], hud[MAX_PATH];
    char marker[MAX_PATH], path[MAX_PATH];
    GetTempPathA(sizeof temp, temp);
    _snprintf_s(root, sizeof root, _TRUNCATE, "%sweapon-hud-test-%lu", temp, GetCurrentProcessId());
    _snprintf_s(over, sizeof over, _TRUNCATE, "%s\\overrides", root);
    _snprintf_s(pkg, sizeof pkg, _TRUNCATE, "%s\\example", over);
    _snprintf_s(hud, sizeof hud, _TRUNCATE, "%s\\hud", pkg);
    _snprintf_s(marker, sizeof marker, _TRUNCATE, "%s\\package.json", pkg);
    _snprintf_s(path, sizeof path, _TRUNCATE, "%s\\weapons.json", hud);
    CHECK(CreateDirectoryA(root, NULL)); CHECK(CreateDirectoryA(over, NULL));
    CHECK(CreateDirectoryA(pkg, NULL)); CHECK(CreateDirectoryA(hud, NULL));
    write_text(marker, "{}"); write_text(path, valid);
    CHECK(sh_weapon_hud_reload(root));
    CHECK(sh_weapon_hud_test_count() == 1);
    CHECK(sh_weapon_hud_test_relay(root));
    sh_packages_test_find_api api = {FindFirstFileA, broken_next, FindClose, GetFileAttributesA};
    sh_packages_test_set_api(&api);
    CHECK(!sh_weapon_hud_reload(root));
    CHECK(sh_weapon_hud_test_count() == 0);
    sh_packages_test_reset_api();
    write_text(path, "invalid");
    CHECK(!sh_weapon_hud_reload(root));
    CHECK(sh_weapon_hud_test_count() == 0);
    write_text(path, valid);
    CHECK(sh_weapon_hud_reload(root));
    CHECK(DeleteFileA(path));
    CHECK(sh_weapon_hud_reload(root));
    CHECK(sh_weapon_hud_test_count() == 0);
    /* Removing the package restores the engine fallback on recapture. */
    CHECK(DeleteFileA(marker));
    CHECK(RemoveDirectoryA(hud)); CHECK(RemoveDirectoryA(pkg));
    CHECK(RemoveDirectoryA(over)); CHECK(RemoveDirectoryA(root));
    printf("weapon_hud_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
